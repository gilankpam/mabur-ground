#include "update_client.h"

#include <algorithm>

namespace maburgs {
namespace {
using Phase = UpdateProgress::Phase;
enum : uint8_t { kCreate = 6, kWrite = 7, kAck = 128, kNak = 129 };
constexpr char kPath[] = "maburd";
constexpr uint8_t kSysid = 255, kCompid = 190;

std::vector<uint8_t> pack(const mabur::ota::OtaMsg& m) {
  return mabur::ota::encode(m, kSysid, kCompid, mabur::mav::kChanGsLocal);
}
}  // namespace

UpdateClient::UpdateClient(std::vector<uint8_t> image, mabur::Sha256 sha)
    : UpdateClient(std::move(image), sha, Cfg{}) {}

UpdateClient::UpdateClient(std::vector<uint8_t> image, mabur::Sha256 sha, Cfg cfg)
    : image_(std::move(image)), sha_(sha), cfg_(cfg) {
  p_.total = image_.size();
}

void UpdateClient::finish(std::string result) {
  p_.phase = Phase::kDone;
  p_.result = std::move(result);
}

std::vector<std::vector<uint8_t>> UpdateClient::tick(uint64_t now) {
  std::vector<std::vector<uint8_t>> pk;
  if (!started_) {  // the first tick starts every phase's clock
    started_ = true;
    phase_ms_ = last_ms_ = now;
    if (p_.phase == Phase::kOpen) last_ms_ = now - cfg_.retry_ms;
  }
  switch (p_.phase) {
    case Phase::kOpen:
      if (now - phase_ms_ >= cfg_.stall_ms) return finish("error no drone"), pk;
      if (now - last_ms_ >= cfg_.retry_ms) {
        last_ms_ = now;
        mabur::ota::Ftp f;
        f.seq = create_seq_ = next_seq_++;
        f.opcode = kCreate;
        f.data.assign(kPath, kPath + sizeof(kPath) - 1);
        f.size = uint8_t(f.data.size());
        pk.push_back(pack(f));
      }
      break;
    case Phase::kSend: {
      if (now - last_ms_ >= cfg_.stall_ms) return finish("error lost drone"), pk;
      auto write = [&](uint16_t seq, uint32_t off) {
        mabur::ota::Ftp f;
        f.seq = seq;
        f.opcode = kWrite;
        f.offset = off;
        size_t n = std::min(mabur::ota::kFtpDataMax, image_.size() - off);
        f.data.assign(image_.begin() + off, image_.begin() + off + n);
        f.size = uint8_t(n);
        pk.push_back(pack(f));
      };
      for (auto& [seq, w] : out_)
        if (now - w.last_ms >= cfg_.retry_ms) {
          w.last_ms = now;
          write(seq, w.offset);
        }
      while (int(out_.size()) < cfg_.window && next_offset_ < image_.size()) {
        uint16_t seq = next_seq_++;
        out_[seq] = {next_offset_, now};
        write(seq, next_offset_);
        next_offset_ += uint32_t(std::min(mabur::ota::kFtpDataMax, image_.size() - next_offset_));
      }
      break;
    }
    case Phase::kStart:
    case Phase::kTrial:
      if (p_.phase == Phase::kStart && !have_reply_ && now - phase_ms_ >= cfg_.final_ms)
        return finish("error lost drone"), pk;
      if (p_.phase == Phase::kTrial && now >= deadline_ms_)
        return finish("error lost drone"), pk;
      if (now - last_ms_ >= (p_.phase == Phase::kStart ? cfg_.retry_ms : cfg_.poll_ms)) {
        last_ms_ = now;
        mabur::ota::UpdateCmd c;
        c.cmd = p_.phase == Phase::kStart ? mabur::ota::Cmd::kStart : mabur::ota::Cmd::kStatus;
        c.sha256 = sha_;
        pk.push_back(pack(c));
      }
      break;
    case Phase::kDone:
      break;
  }
  return pk;
}

void UpdateClient::on_packet(const uint8_t* bytes, size_t n, uint64_t now) {
  for (const auto& m : mabur::ota::decode_all(bytes, n)) {
    if (done()) return;
    if (const auto* f = std::get_if<mabur::ota::Ftp>(&m)) on_ftp(*f, now);
    else if (const auto* s = std::get_if<mabur::ota::Status>(&m)) on_status(*s, now);
  }
}

void UpdateClient::on_ftp(const mabur::ota::Ftp& f, uint64_t now) {
  if (f.opcode != kAck && f.opcode != kNak) return;
  const uint16_t seq = uint16_t(f.seq - 1);
  const bool nak = f.opcode == kNak;
  const int code = nak && !f.data.empty() ? f.data[0] : 0;
  auto fail = [&] {
    finish(code == 1 && p_.phase == Phase::kOpen ? "error drone busy"
                                                  : "error ftp nak " + std::to_string(code));
  };
  if (p_.phase == Phase::kOpen && f.req_opcode == kCreate && seq == create_seq_) {
    if (nak) return fail();
    p_.phase = Phase::kSend;
    last_ms_ = now;
    if (image_.empty()) p_.phase = Phase::kStart, phase_ms_ = now, last_ms_ = now - cfg_.retry_ms;
  } else if (p_.phase == Phase::kSend && f.req_opcode == kWrite) {
    auto it = out_.find(seq);
    if (it == out_.end()) return;
    if (nak) return fail();
    p_.sent += std::min(mabur::ota::kFtpDataMax, image_.size() - it->second.offset);
    out_.erase(it);
    last_ms_ = now;
    if (p_.sent >= image_.size()) {
      p_.phase = Phase::kStart;
      phase_ms_ = now;
      last_ms_ = now - cfg_.retry_ms;  // START goes out on the next tick
    }
  }
}

void UpdateClient::on_status(const mabur::ota::Status& s, uint64_t now) {
  using St = mabur::ota::State;
  if (p_.phase == Phase::kStart) {
    if (s.state == St::kRefused) return finish(std::string("refused ") + mabur::ota::reason_name(s.reason));
    p_.phase = Phase::kTrial;
    have_reply_ = true;
    first_reply_ms_ = now;
    deadline_ms_ = now + cfg_.final_ms;
    last_ms_ = now;
    // The old binary's "trial 40" says it is about to exit, not that a trial
    // runs: the seconds come from the trial build's own replies.
    if (s.state == St::kTrial) {
      p_.installing = true;
      return;
    }
  } else if (p_.phase != Phase::kTrial) {
    return;
  }
  const std::string why = mabur::ota::reason_name(s.reason);
  switch (s.state) {
    case St::kTrial:
      p_.installing = false;
      p_.trial_s = s.trial_s;
      // The window plus 60 s, counted from the drone's own report: its install
      // before the trial does not eat the margin.
      deadline_ms_ = std::max(deadline_ms_, now + uint64_t(s.trial_s) * 1000 + 60000);
      break;
    case St::kCommitted: finish("committed"); break;
    case St::kRolledBack: finish("rolled_back " + why); break;
    case St::kRefused: finish("refused " + why); break;
    default: break;  // idle / receiving: the drone is between restarts
  }
}

}  // namespace maburgs
