#pragma once
// A drone's update path for tests: a real mabur::OtaServer on a temp dir, plus
// the restart loop that takes over when the server asks to exit (read `go`,
// check the staged file's hash, start a trial server, write `result`), and a
// simulated-clock link that carries a UpdateClient's packets both ways.
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
#include <utility>
#include <sstream>
#include <string>
#include <vector>

#include "mabur/ota_mav.h"
#include "mabur/sha256.h"
#include "ota.h"

namespace fake_ota {

inline std::string slurp(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

inline void spit(const std::string& p, const std::string& s) {
  std::ofstream(p, std::ios::binary | std::ios::trunc) << s;
}

class FakeDrone {
 public:
  // `outcome` is the result file's text once the trial has been polled
  // `polls` times; the loop is silent for `gap_ms` after the exit.
  explicit FakeDrone(std::string outcome, int polls = 3, uint64_t gap_ms = 0)
      : outcome_(std::move(outcome)), polls_(polls), gap_ms_(gap_ms) {
    char t[] = "/tmp/fake-ota-XXXXXX";
    dir = mkdtemp(t);
    server_ = std::make_unique<mabur::OtaServer>(env(false, 0));
  }
  ~FakeDrone() {
    server_.reset();
    for (const char* f : {"maburd", "go", "result"}) ::unlink((dir + "/" + f).c_str());
    ::rmdir(dir.c_str());
  }

  std::string dir;
  bool mute = false;  // the drone hears and says nothing
  int starts_seen = 0;

  std::string staged() const { return slurp(dir + "/maburd"); }

  // Answers one decoded message; the replies go back to the GS.
  std::vector<mabur::ota::OtaMsg> handle(const mabur::ota::OtaMsg& m, uint64_t now) {
    if (mute || now < down_until_) return {};
    if (const auto* u = std::get_if<mabur::ota::UpdateCmd>(&m)) {
      if (u->cmd == mabur::ota::Cmd::kStart) ++starts_seen;
      if (u->cmd == mabur::ota::Cmd::kStatus && trial_ && ++polled_ >= polls_)
        spit(dir + "/result", outcome_ + "\n");
    }
    auto out = server_->on_msg(m, now);
    if (server_->exit_requested()) restart(now);
    return out;
  }

 private:
  mabur::OtaEnv env(bool trial, uint64_t start) const {
    mabur::OtaEnv e;
    e.dir = dir;
    e.ident = {"v", "b", "c"};
    e.trial = trial;
    e.start_ms = start;
    return e;
  }

  // What S96mabur's step does for a go file.
  void restart(uint64_t now) {
    std::string want = slurp(dir + "/go");
    ::unlink((dir + "/go").c_str());
    ::unlink((dir + "/result").c_str());
    while (!want.empty() && want.back() == '\n') want.pop_back();
    auto have = mabur::sha256_file(dir + "/maburd");
    bool ok = have && mabur::sha256_hex(*have) == want;
    if (!ok) spit(dir + "/result", "refused bad_hash\n");
    trial_ = ok;
    polled_ = 0;
    down_until_ = now + gap_ms_;
    server_ = std::make_unique<mabur::OtaServer>(env(ok, now + gap_ms_));
  }

  std::string outcome_;
  int polls_;
  uint64_t gap_ms_, down_until_ = 0;
  int polled_ = 0;
  bool trial_ = false;
  std::unique_ptr<mabur::OtaServer> server_;
};

// Drop predicates see the time, the message and the packet's index within
// its direction (from 1).
using Drop = std::function<bool(uint64_t now, const mabur::ota::OtaMsg&, size_t idx)>;
using Watch = std::function<void(const mabur::ota::OtaMsg&)>;

struct Link {
  Drop drop_up, drop_down;
  Watch on_up;    // every uplink message, before the drop decision
  Watch on_down;  // every downlink message that is delivered
  std::function<void(uint64_t now)> on_step;
  uint64_t now = 0, step_ms = 10;
  size_t n_up = 0, n_down = 0;

  // Steps the clock until the client is done or `max_ms` more have passed.
  template <class Client>
  void run(Client& c, FakeDrone& d, uint64_t max_ms) {
    const uint64_t end = now + max_ms;
    while (!c.done() && now < end) {
      if (on_step) on_step(now);
      std::vector<std::pair<mabur::ota::OtaMsg, std::vector<uint8_t>>> down;  // delivered after the step's uplink
      for (const auto& pkt : c.tick(now)) {
        for (const auto& m : mabur::ota::decode_all(pkt.data(), pkt.size())) {
          ++n_up;
          if (on_up) on_up(m);
          if (drop_up && drop_up(now, m, n_up)) continue;
          for (const auto& r : d.handle(m, now)) {
            auto wire = mabur::ota::encode(r, 1, 191, mabur::mav::kChanGsRx);
            ++n_down;
            if (drop_down && drop_down(now, r, n_down)) continue;
            down.push_back({r, std::move(wire)});
          }
        }
      }
      for (const auto& [m, w] : down) {
        if (on_down) on_down(m);
        c.on_packet(w.data(), w.size(), now);
      }
      now += step_ms;
    }
  }
};

}  // namespace fake_ota
