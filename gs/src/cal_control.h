#pragma once
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <random>
#include <string>
#include <vector>

#include "cal_session.h"
#include "cal_wiring.h"
#include "ladder_controller.h"

namespace maburgs {

// Wall then capacity for every width the GS flies: 20 always, plus 40 iff the GS's
// receiver width (ChannelCore::width_mhz(), derived from the ladder or a
// static pin) is 40 AND some rung on its ladder actually uses it -- a GS
// that can fly 40 but whose ladder never picks it would be calibrating a
// width nothing lands on.
inline std::vector<mabur::cal::SweepSpec> default_sweeps(uint8_t rx_width,
                                                          const LadderCfg& ladder) {
  std::vector<mabur::cal::SweepSpec> out = {{mabur::cal::Kind::Wall, 20},
                                             {mabur::cal::Kind::Capacity, 20}};
  const bool flies_40 =
      rx_width == 40 &&
      std::any_of(ladder.ladder.begin(), ladder.ladder.end(),
                  [](const Rung& r) { return r.bw == 40; });
  if (flies_40) {
    out.push_back({mabur::cal::Kind::Wall, 40});
    out.push_back({mabur::cal::Kind::Capacity, 40});
  }
  return out;
}

// Loopback-only UDP command listener that lets an operator ssh'd into the
// GS box start, inspect, or abort a TX-power calibration run.
//
// Binds 127.0.0.1 ONLY. The GS is reachable on its LAN address, and a calibration
// sweep silences the GS's own transmit path for whole phases at a time
// (CalSession::radio_silent) -- something the drone or a bench laptop must
// never be able to trigger by accident or on purpose. The loopback bind is
// what makes this a deliberate trigger: you have to be logged into the GS
// itself to reach port 8400.
//
// Unlike loss_control.h's LossControl, this is compiled into every prod
// build -- calibration is a real operator workflow, not a bench-only
// scaffold -- so there is no MABUR_LOSS_SIM-style CMake guard here.
//
// Non-blocking; poll() is called from the core loop next to the other
// per-tick polls, so command handling runs on the thread that owns the
// CalSession. No locking.
class CalControl {
 public:
  ~CalControl() { if (fd_ >= 0) close(fd_); }
  CalControl() = default;
  CalControl(const CalControl&) = delete;
  CalControl& operator=(const CalControl&) = delete;

  bool ok() const { return fd_ >= 0; }

  static constexpr const char* bound_address() { return "127.0.0.1"; }

  bool open(int port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, bound_address(), &addr.sin_addr) != 1) return false;
    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (fd < 0) return false;
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      close(fd);
      return false;
    }
    fd_ = fd;
    return true;
  }

  // Drains every pending datagram, applies it, and replies to the sender.
  // `defaults` is the sweep list a bare `start` runs (default_sweeps(),
  // computed by the caller from the live config); `rx_width` is the GS's
  // own receiver width (ChannelCore::width_mhz(), derived from the ladder
  // or a static pin -- there is no radio.width config key on the GS any
  // more), which bounds what a `start power40`/`cap40` token may ask for.
  void poll(CalSession& s, const std::vector<mabur::cal::SweepSpec>& defaults,
            uint8_t rx_width) {
    if (fd_ < 0) return;
    char buf[256];
    for (;;) {
      sockaddr_in from{};
      socklen_t flen = sizeof(from);
      const ssize_t n = recvfrom(fd_, buf, sizeof(buf) - 1, 0,
                                 reinterpret_cast<sockaddr*>(&from), &flen);
      if (n < 0) return;    // EAGAIN/error when drained -- a genuine 0-length
                            // datagram must still be answered, not treated
                            // as end-of-drain (that would leave any
                            // datagrams still queued behind it unanswered
                            // until the next tick).
      buf[n] = '\0';
      std::string reply;
      const bool changed = apply(buf, s, &reply, defaults, rx_width);
      sendto(fd_, reply.data(), reply.size(), 0,
             reinterpret_cast<sockaddr*>(&from), flen);
      if (changed) {
        // Loud, on every accepted start/abort: /tmp/maburgs.log is the
        // post-mortem surface, and "why did the link go quiet for 30s"
        // must be answerable from the log without asking the operator.
        std::fprintf(stderr, "maburgs: CAL %s -> %s\n", buf, reply.c_str());
        std::fflush(stderr);
      }
    }
  }

  // Parses one command and mutates `s`. Returns true iff state changed.
  // `*reply` always gets the datagram to send back. Static and I/O-free so
  // the command language is unit-testable without a socket.
  //
  // Command language:
  //   start [tok...]        begin a session (refuses if not linked / no
  //                         CAP_CALIBRATE / already running). With no
  //                         tokens, runs `defaults`; otherwise each token
  //                         is power20|power40|cap20|cap40, or
  //                         power20@<ch>|power40@<ch> for a wall sweep of
  //                         channel <ch> (1..177; the pair stays where it
  //                         is), any order, duplicates dropped (the
  //                         session itself sorts and dedups).
  //   status                one-line progress, never mutates
  //   abort                 abandon the running session
  //
  // There is no `margin=<db>`: the drone applies its own
  // radio.wall_margin_db, the operator's safety choice hand-set in
  // /etc/mabur.toml, and a calibration never changes it. A stray
  // `margin=2.0` is refused as any other unrecognized token.
  static bool apply(const std::string& line, CalSession& s, std::string* reply,
                    const std::vector<mabur::cal::SweepSpec>& defaults,
                    uint8_t rx_width) {
    const std::vector<std::string> tok = split(line);
    if (tok.empty()) { *reply = "err empty"; return false; }

    if (tok[0] == "status") {
      *reply = "ok " + render_status(s.snapshot());
      return false;
    }

    if (tok[0] == "abort") {
      // abort() is idempotent -- Idle stays Idle -- but the caller (poll())
      // logs a `true` return as "a calibration happened" for /tmp/maburgs.log
      // to preserve as post-mortem evidence. An operator's idle sanity-check
      // abort is not an event and must not write that line.
      const bool was_running = s.state() != CalSession::State::Idle &&
                               s.state() != CalSession::State::Done &&
                               s.state() != CalSession::State::Failed;
      s.abort("operator abort");
      if (!was_running) {
        *reply = "ok nothing running";
        return false;
      }
      *reply = "ok aborted";
      return true;
    }

    if (tok[0] == "start") {
      std::vector<mabur::cal::SweepSpec> sweeps;
      if (tok.size() == 1) {
        sweeps = defaults;
      } else {
        for (size_t i = 1; i < tok.size(); ++i) {
          mabur::cal::SweepSpec spec;
          if (!parse_token(tok[i], &spec)) {
            *reply = "err bad token: " + tok[i];
            return false;
          }
          if (spec.bw == 40 && rx_width != 40) {
            *reply = "err refused: GS receiver width is " +
                     std::to_string(rx_width) +
                     " (load a 40 MHz ladder to calibrate 40 MHz)";
            return false;
          }
          sweeps.push_back(spec);
        }
      }

      // A fresh nonce per start means a stale ack from a session the
      // operator already aborted can never be mistaken for this one's --
      // random_device rather than rand() to match how the rest of the GS
      // mints session/nonce identifiers (see the stats-sideport session id
      // in main.cpp).
      static std::random_device rd;
      const uint32_t nonce = rd();
      std::string err;
      if (!s.start(nonce, sweeps, now_ms(), &err)) {
        *reply = "err " + err;
        return false;
      }
      *reply = "ok started " + sweeps_token_list(s.sweeps());
      return true;
    }

    *reply = "err want start | status | abort";
    return false;
  }

  // "state=sweep sweep=2/4 kind=capacity bw=40 cell=3/8", plus
  // mismatch=<n> when frames were dropped for a kind/width/flags mismatch
  // (CalSnapshot::stamp_mismatch, omitted at 0), and fail_reason=<why>
  // appended last (free text to the end of the line) when the state is
  // failed. sweep=k/n is 1-based (k = the running sweep's position, n =
  // the run's total); kind is "wall" or "capacity", and a wall sweep's
  // cell=i/n counts seconds since the ack against the drone's deadline.
  static std::string render_status(const CalSnapshot& snap) {
    const char* kind = cal_kind_name(snap);
    const size_t sweep_k = snap.sweep_n ? snap.sweep_i + 1 : 0;
    std::string out = std::string("state=") + cal_state_name(snap.state) +
                      " sweep=" + std::to_string(sweep_k) + "/" +
                      std::to_string(snap.sweep_n) + " kind=" + kind +
                      " bw=" + std::to_string(snap.cur.bw) +
                      " cell=" + std::to_string(snap.cell_i) + "/" +
                      std::to_string(snap.cell_n);
    if (snap.stamp_mismatch) out += " mismatch=" + std::to_string(snap.stamp_mismatch);
    if (snap.state == CalSession::State::Failed && !snap.fail_reason.empty()) {
      out += " fail_reason=" + snap.fail_reason;
    }
    return out;
  }

 private:
  // power20 | power40 | cap20 | cap40 | power20@<ch> | power40@<ch> ->
  // the sweep it names, <ch> 1..177 in decimal digits only. False (spec
  // untouched) for anything else.
  static bool parse_token(const std::string& tok, mabur::cal::SweepSpec* spec) {
    using mabur::cal::Kind;
    if (tok == "cap20") { *spec = {Kind::Capacity, 20}; return true; }
    if (tok == "cap40") { *spec = {Kind::Capacity, 40}; return true; }
    const std::string head = tok.substr(0, tok.find('@'));
    if (head != "power20" && head != "power40") return false;
    const uint8_t bw = head == "power20" ? 20 : 40;
    if (head.size() == tok.size()) { *spec = {Kind::Wall, bw}; return true; }
    const std::string ch = tok.substr(head.size() + 1);
    if (ch.empty() || ch.size() > 3 ||
        !std::all_of(ch.begin(), ch.end(), [](unsigned char c) { return std::isdigit(c); }))
      return false;
    const int n = std::stoi(ch);
    if (n < 1 || n > 177) return false;
    *spec = {Kind::Wall, bw, static_cast<uint8_t>(n)};
    return true;
  }

  // Comma-separated, in the order the session will actually run them --
  // the reply to `start` names the run that was really composed, not the
  // tokens the operator happened to type.
  static std::string sweeps_token_list(const std::vector<mabur::cal::SweepSpec>& sweeps) {
    std::string out;
    for (size_t i = 0; i < sweeps.size(); ++i) {
      if (i) out += ",";
      out += cal_sweep_token(sweeps[i]);
    }
    return out;
  }

  static uint64_t now_ms() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000 +
           static_cast<uint64_t>(ts.tv_nsec) / 1000000;
  }

  static std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
      while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
      const size_t start = i;
      while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
      if (i > start) out.push_back(s.substr(start, i - start));
    }
    return out;
  }

  int fd_ = -1;
};

}  // namespace maburgs
