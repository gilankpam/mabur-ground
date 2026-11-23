#pragma once
// UpdateClient -- the ground side of a drone update: write the image to the
// drone over MAVLink FTP (CreateFile, then a window of WriteFile requests),
// send START with the image's sha256, then poll STATUS through the drone's
// restart and trial until it reports a final state. Pure: packets in, packets
// out, time passed in; the caller owns the transport and the clock.
//
// Every request is retried until answered: a write unacked for retry_ms is
// resent with the same seq (the drone's write is idempotent), START is resent
// until the first status reply and never after (a START after a commit would
// reinstall). A silent drone ends the run: no create ACK within stall_ms is
// "no drone", no write progress for stall_ms or no final state within
// final_ms of the first START reply (or 60 s past the trial window the drone
// last reported, if later) is "lost drone".
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "mabur/ota_mav.h"
#include "mabur/sha256.h"

namespace maburgs {

struct UpdateProgress {
  enum class Phase { kOpen, kSend, kStart, kTrial, kDone };
  Phase phase = Phase::kOpen;
  uint64_t sent = 0, total = 0;  // bytes acked / image size
  int trial_s = 0;               // last trial_s the drone reported
  // kDone only: "committed" | "rolled_back <r>" | "refused <r>" | "error <text>"
  std::string result;
};

class UpdateClient {
 public:
  struct Cfg {
    int window = 32;
    uint64_t retry_ms = 1000, stall_ms = 15000, poll_ms = 1000, final_ms = 100000;
  };
  UpdateClient(std::vector<uint8_t> image, mabur::Sha256 sha);
  UpdateClient(std::vector<uint8_t> image, mabur::Sha256 sha, Cfg cfg);

  // Packets due now, as wire bytes.
  std::vector<std::vector<uint8_t>> tick(uint64_t now_ms);
  // Whole packets of any kind; anything but the update lane is ignored.
  void on_packet(const uint8_t* bytes, size_t n, uint64_t now_ms);

  const UpdateProgress& progress() const { return p_; }
  bool done() const { return p_.phase == UpdateProgress::Phase::kDone; }

 private:
  struct Pending {
    uint32_t offset;
    uint64_t last_ms;
  };
  void finish(std::string result);
  void on_ftp(const mabur::ota::Ftp& f, uint64_t now_ms);
  void on_status(const mabur::ota::Status& s, uint64_t now_ms);

  std::vector<uint8_t> image_;
  mabur::Sha256 sha_;
  Cfg cfg_;
  UpdateProgress p_;
  uint16_t next_seq_ = 1, create_seq_ = 0;
  uint32_t next_offset_ = 0;
  std::map<uint16_t, Pending> out_;
  bool started_ = false;   // first packet of the current phase sent
  uint64_t phase_ms_ = 0;  // when the current phase's clock began
  uint64_t last_ms_ = 0;   // last send (create, START) or last write progress
  uint64_t first_reply_ms_ = 0, deadline_ms_ = 0;
  bool have_reply_ = false;
};

}  // namespace maburgs
