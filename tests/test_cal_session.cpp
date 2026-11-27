#include "mtest.h"
#include "cal_session.h"
#include "cal_wiring.h"
#include "cal_analysis.h"
#include "cal_log.h"
#include "cal_picker.h"
#include "cal_plan.h"

#include "mabur/cal_proto.h"
#include "mabur/cal_wire.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <vector>

using namespace maburgs;
namespace cal = mabur::cal;

namespace {

using cal::Kind;
using State = CalSession::State;

constexpr uint8_t kFlags = cal::kFlagLdpc | cal::kFlagStbc;

struct FakePicker : CalPicker {
  Mode m = Mode::Auto;
  uint8_t op_ch = 36;
  std::optional<uint8_t> best;
  bool accept_reloc = true;
  std::optional<bool> settled;
  int measures = 0, relocs = 0, cancels = 0;
  uint8_t reloc_to = 0;
  Mode mode() const override { return m; }
  uint8_t op() const override { return op_ch; }
  void begin_measure(uint64_t) override { ++measures; }
  std::optional<uint8_t> measured_best() override { return best; }
  bool order_relocate(uint8_t ch) override {
    ++relocs;
    reloc_to = ch;
    return accept_reloc;
  }
  std::optional<bool> relocate_settled() override { return settled; }
  void cancel() override { ++cancels; }
};

// A pinned link on `ch`: no pick runs, and a wall sweep naming no channel
// measures this one.
FakePicker pinned(uint8_t ch) {
  FakePicker p;
  p.m = CalPicker::Mode::Pinned;
  p.op_ch = ch;
  return p;
}

std::string fresh_dir(const char* name) {
  const std::string d = std::string(MABUR_TEST_SCRATCH_DIR) + "/" + name;
  if (std::system(("rm -rf " + d).c_str()) != 0) { /* best-effort */ }
  ::mkdir(d.c_str(), 0755);
  return d;
}

std::vector<std::string> cal_log_lines(const std::string& dir) {
  std::ifstream f(dir + "/cal.log");
  std::vector<std::string> out;
  for (std::string l; std::getline(f, l);) out.push_back(l);
  return out;
}

// Every frame goes through the real stamp codec, as maburgs' RX path does.
// Only capacity frames exist on the air.
cal::CalFrameInfo stamp(Kind k, uint8_t rate, int idx, uint8_t bw, uint8_t flags,
                        uint32_t seq = 0) {
  const auto p = cal::build_cal_payload(k, rate, static_cast<int8_t>(idx), bw, flags, seq, 1405);
  cal::CalFrameInfo f;
  REQUIRE(cal::parse_cal_payload(p.data(), p.size(), &f));
  return f;
}

template <class T>
std::optional<T> as(const std::optional<cal::CalMsg>& m) {
  if (!m) return std::nullopt;
  if (const T* p = std::get_if<T>(&*m)) return *p;
  return std::nullopt;
}

void ack(CalSession& s, uint32_t nonce, uint8_t seq, uint64_t now, uint8_t flags = kFlags,
         cal::AckResult r = cal::AckResult::Accepted) {
  s.on_mav(cal::SweepAck{nonce, seq, r, /*start_in_ms=*/0, flags}, now);
}

// The drone's report of a finished wall measurement.
cal::SweepDone wall_done(uint32_t nonce, uint8_t seq, uint8_t ch, uint8_t bw,
                         cal::WallResult r = cal::WallResult::Ok) {
  cal::SweepDone d;
  d.nonce = nonce;
  d.seq = seq;
  d.wall_result = r;
  d.channel = ch;
  d.bw = bw;
  d.walls = {63, 63, 63, 63, 44, 26, 24, 20};
  d.floor_evm_x10 = -306;
  d.anchor = 39;
  return d;
}

// Drives one wall sweep from its SweepCmd to the drone's report.
cal::SweepCmd run_wall(CalSession& s, uint32_t nonce, uint8_t seq, uint64_t t) {
  const auto cmd = as<cal::SweepCmd>(s.due_mav(t));
  REQUIRE(cmd.has_value());
  REQUIRE(cmd->kind == Kind::Wall && cmd->seq == seq);
  ack(s, nonce, seq, t + 10);
  s.on_mav(wall_done(nonce, seq, cmd->channel, cmd->bw), t + 3000);
  return *cmd;
}

// Drives one capacity sweep from its SweepCmd to its ApplyCmd: 900 of 1000
// frames heard on mcs0, nothing else offered.
cal::ApplyCmd run_capacity(CalSession& s, uint32_t nonce, uint8_t seq, uint64_t t) {
  const auto cmd = as<cal::SweepCmd>(s.due_mav(t));
  REQUIRE(cmd.has_value());
  REQUIRE(cmd->kind == Kind::Capacity && cmd->seq == seq);
  ack(s, nonce, seq, t + 10, cal::kFlagStbc);
  for (int k = 0; k < 900; ++k)
    s.on_cal_frame(0, stamp(Kind::Capacity, 0, 0, cmd->bw, cal::kFlagStbc, k), -60, true, 1500,
                   t + 100);
  s.due_mav(t + 100);  // the core loop's tick after the pass that handled them
  cal::SweepDone d;
  d.nonce = nonce;
  d.seq = seq;
  d.offered = {1000, 0, 0, 0, 0, 0, 0, 0};
  d.blocked_pct = {80, 0, 0, 0, 0, 0, 0, 0};
  s.on_mav(d, t + 200);
  const auto a = as<cal::ApplyCmd>(s.due_mav(t + 210));
  REQUIRE(a.has_value());
  REQUIRE(a->seq == seq);
  return *a;
}

}  // namespace

TEST(start_sorts_and_dedups_the_sweep_list) {
  FakePicker p = pinned(144);
  CalSession s(CalSessionCfg{}, nullptr, &p);
  s.set_peer(true, true);
  std::string err;
  REQUIRE(s.start(1,
                  {{Kind::Capacity, 40}, {Kind::Wall, 20, 112}, {Kind::Capacity, 20},
                   {Kind::Wall, 40}, {Kind::Wall, 20}, {Kind::Capacity, 40},
                   {Kind::Wall, 20, 112}},
                  0, &err));
  const auto& v = s.sweeps();
  REQUIRE(v.size() == 5);
  CHECK(v[0].kind == Kind::Wall && v[0].bw == 20 && v[0].channel == 0);
  CHECK(v[1].kind == Kind::Wall && v[1].bw == 20 && v[1].channel == 112);
  CHECK(v[2].kind == Kind::Capacity && v[2].bw == 20);
  CHECK(v[3].kind == Kind::Wall && v[3].bw == 40);
  CHECK(v[4].kind == Kind::Capacity && v[4].bw == 40);
  CHECK(s.snapshot().sweep_n == 5);
  CHECK(s.snapshot().sweep_i == 0);

  CalSession t(CalSessionCfg{});
  t.set_peer(true, true);
  CHECK(!t.start(2, {}, 0, &err));
  CHECK(!t.start(2, {{static_cast<Kind>(2), 20}}, 0, &err));
  CHECK(err == "refused: a sweep is wall or capacity");
  CHECK(!t.start(2, {{Kind::Wall, 30, 36}}, 0, &err));
  CHECK(t.state() == State::Idle);
}

TEST(start_refuses_without_capability_or_link) {
  CalSession s(CalSessionCfg{});
  std::string err;
  s.set_peer(/*linked=*/false, /*cal_capable=*/true);
  CHECK(!s.start(1, {{Kind::Wall, 20, 36}}, 0, &err));
  CHECK(err.find("link") != std::string::npos);

  s.set_peer(/*linked=*/true, /*cal_capable=*/false);
  CHECK(!s.start(1, {{Kind::Wall, 20, 36}}, 0, &err));
  CHECK(err.find("CAP_CALIBRATE") != std::string::npos);

  // Linked, but no Telem yet: the caps are unknown, not missing.
  s.set_peer(/*linked=*/true, /*cal_capable=*/std::nullopt);
  CHECK(!s.start(1, {{Kind::Wall, 20, 36}}, 0, &err));
  CHECK(err.find("waiting for drone telemetry") != std::string::npos);
  CHECK(err.find("CAP_CALIBRATE") == std::string::npos);

  s.set_peer(true, true);
  REQUIRE(s.start(1, {{Kind::Wall, 20, 36}}, 0, &err));
  CHECK(!s.start(2, {{Kind::Wall, 20, 36}}, 10, &err));
  CHECK(err.find("running") != std::string::npos);

  // A pick in progress is a run too, though not yet running().
  FakePicker p;
  CalSession q(CalSessionCfg{}, nullptr, &p);
  q.set_peer(true, true);
  REQUIRE(q.start(3, {{Kind::Wall, 20}}, 0, &err));
  REQUIRE(q.state() == State::Picking);
  CHECK(!q.start(4, {{Kind::Wall, 20}}, 10, &err));
}

TEST(picking_relocates_to_the_best_member_then_awaits_ack) {
  FakePicker p;
  p.op_ch = 36;
  CalSession s(CalSessionCfg{}, nullptr, &p);
  s.set_peer(true, true);
  std::string err;
  REQUIRE(s.start(5, {{Kind::Wall, 20}}, 0, &err));
  CHECK(s.state() == State::Picking);
  CHECK(p.measures == 1);
  CHECK(!s.running());
  CHECK(!s.radio_silent(0));
  CHECK(!s.due_mav(100).has_value());  // round still measuring

  p.best = 44;
  CHECK(!s.due_mav(200).has_value());
  CHECK(p.relocs == 1 && p.reloc_to == 44);
  CHECK(s.state() == State::Picking);
  CHECK(!s.due_mav(400).has_value());  // relocate pending
  CHECK(p.relocs == 1);                // ordered once

  p.settled = true;
  p.op_ch = 44;
  const auto cmd = as<cal::SweepCmd>(s.due_mav(600));
  REQUIRE(cmd.has_value());
  CHECK(cmd->channel == 44);  // the wall sweep measures the channel the pick moved to
  CHECK(s.state() == State::AwaitAck);
  CHECK(s.running());
  const auto snap = s.snapshot();
  CHECK(snap.channel == 44);
  CHECK(snap.pick_note.find("44") != std::string::npos);
  CHECK(p.cancels == 0);

  // Already on the best member: no relocate at all.
  FakePicker q;
  q.op_ch = 36;
  q.best = 36;
  CalSession t(CalSessionCfg{}, nullptr, &q);
  t.set_peer(true, true);
  REQUIRE(t.start(6, {{Kind::Wall, 20}}, 0, &err));
  CHECK(as<cal::SweepCmd>(t.due_mav(10)).has_value());
  CHECK(q.relocs == 0);
  CHECK(t.snapshot().channel == 36);
}

TEST(pinned_picker_skips_picking_with_a_note) {
  std::string err;
  FakePicker p = pinned(149);
  CalSession s(CalSessionCfg{}, nullptr, &p);
  s.set_peer(true, true);
  REQUIRE(s.start(7, {{Kind::Wall, 20}}, 0, &err));
  CHECK(s.state() == State::AwaitAck);
  CHECK(p.measures == 0);
  CHECK(s.snapshot().pick_note == "pinned");
  CHECK(s.snapshot().channel == 149);
  CHECK(as<cal::SweepCmd>(s.due_mav(0)).has_value());

  FakePicker q;
  q.m = CalPicker::Mode::OneCard;
  CalSession t(CalSessionCfg{}, nullptr, &q);
  t.set_peer(true, true);
  REQUIRE(t.start(8, {{Kind::Wall, 20}}, 0, &err));
  CHECK(t.state() == State::AwaitAck);
  CHECK(q.measures == 0);
  CHECK(t.snapshot().pick_note == "onecard");

  // No picker at all: same as pinned -- the run does not choose.
  CalSession u(CalSessionCfg{});
  u.set_peer(true, true);
  REQUIRE(u.start(9, {{Kind::Wall, 20, 149}}, 0, &err));
  CHECK(u.state() == State::AwaitAck);
  CHECK(u.snapshot().pick_note == "pinned");
}

TEST(unconfirmed_relocate_stays_on_op_and_proceeds) {
  std::string err;
  FakePicker p;
  p.op_ch = 36;
  p.best = 44;
  CalSession s(CalSessionCfg{}, nullptr, &p);
  s.set_peer(true, true);
  REQUIRE(s.start(10, {{Kind::Wall, 20}}, 0, &err));
  CHECK(!s.due_mav(10).has_value());
  REQUIRE(p.relocs == 1);
  p.settled = false;
  const auto cmd = as<cal::SweepCmd>(s.due_mav(20));
  REQUIRE(cmd.has_value());
  CHECK(cmd->channel == 36);
  CHECK(s.state() == State::AwaitAck);
  CHECK(s.snapshot().channel == 36);
  CHECK(s.snapshot().pick_note == "stayed on 36, relocate unconfirmed");
  CHECK(p.cancels == 1);

  // A refused order stays the same way.
  FakePicker q;
  q.op_ch = 36;
  q.best = 44;
  q.accept_reloc = false;
  CalSession t(CalSessionCfg{}, nullptr, &q);
  t.set_peer(true, true);
  REQUIRE(t.start(11, {{Kind::Wall, 20}}, 0, &err));
  CHECK(as<cal::SweepCmd>(t.due_mav(10)).has_value());
  CHECK(t.snapshot().channel == 36);
  CHECK(t.snapshot().pick_note.find("stayed on 36") == 0);
  CHECK(q.cancels == 1);
}

// --- wall sweeps -----------------------------------------------------------

TEST(a_wall_sweep_sends_the_link_channel_and_records_the_report) {
  FakePicker p = pinned(144);
  CalSession s(CalSessionCfg{}, nullptr, &p);
  s.set_peer(true, true);
  std::string err;
  REQUIRE(s.start(50, {{Kind::Wall, 20}}, 0, &err));
  int applies = 0;
  const auto cmd = as<cal::SweepCmd>(s.due_mav(0));
  REQUIRE(cmd.has_value());
  CHECK(cmd->kind == Kind::Wall && cmd->bw == 20 && cmd->channel == 144);
  ack(s, 50, 0, 10);
  REQUIRE(s.state() == State::Sweep);
  CHECK(s.radio_silent(20));
  s.on_mav(wall_done(50, 0, 144, 20), 4000);
  CHECK(s.state() == State::Done);
  for (uint64_t t = 4000; t < 30000; t += 50)
    if (as<cal::ApplyCmd>(s.due_mav(t))) ++applies;
  CHECK(applies == 0);  // the drone stores its own walls: nothing to apply
  const auto snap = s.snapshot();
  REQUIRE(snap.walls.size() == 1);
  CHECK(snap.walls[0].channel == 144);
  CHECK(snap.walls[0].bw == 20);
  CHECK(snap.walls[0].walls[4] == 44);
  CHECK(snap.walls[0].walls[7] == 20);
  CHECK(snap.walls[0].floor_evm_x10 == -306);
  CHECK(snap.walls[0].anchor == 39);
  CHECK(snap.skipped.empty());
}

TEST(a_named_channel_is_sent_as_named) {
  FakePicker p = pinned(144);
  CalSession s(CalSessionCfg{}, nullptr, &p);
  s.set_peer(true, true);
  std::string err;
  REQUIRE(s.start(51, {{Kind::Wall, 20, 112}, {Kind::Wall, 40, 36}}, 0, &err));
  const auto c0 = run_wall(s, 51, 0, 0);
  CHECK(c0.channel == 112 && c0.bw == 20);
  const auto c1 = run_wall(s, 51, 1, 3100);
  CHECK(c1.channel == 36 && c1.bw == 40);
  CHECK(s.state() == State::Done);
  CHECK(p.relocs == 0);  // the pair never moves for a named channel
  const auto snap = s.snapshot();
  REQUIRE(snap.walls.size() == 2);
  CHECK(snap.walls[0].channel == 112 && snap.walls[1].channel == 36);
}

TEST(the_report_ends_the_sweep_at_once) {
  FakePicker p = pinned(144);
  CalSession s(CalSessionCfg{}, nullptr, &p);
  s.set_peer(true, true);
  std::string err;
  REQUIRE(s.start(52, {{Kind::Wall, 20}, {Kind::Capacity, 20}}, 0, &err));
  REQUIRE(as<cal::SweepCmd>(s.due_mav(0)).has_value());
  ack(s, 52, 0, 10);
  CHECK(!s.due_mav(2000).has_value());  // silent while the drone measures
  s.on_mav(wall_done(52, 0, 144, 20), 3010);
  CHECK(!s.radio_silent(3010));
  const auto next = as<cal::SweepCmd>(s.due_mav(3010));
  REQUIRE(next.has_value());
  CHECK(next->kind == Kind::Capacity && next->seq == 1);
}

TEST(an_unsupported_drone_skips_walls_and_runs_capacity) {
  const std::string dir = fresh_dir("cal_session_skip");
  CalLog log(dir);
  log.header();
  FakePicker p = pinned(36);
  CalSession s(CalSessionCfg{}, &log, &p);
  s.set_peer(true, true);
  std::string err;
  REQUIRE(s.start(53, {{Kind::Wall, 20}, {Kind::Capacity, 20}}, 0, &err));
  REQUIRE(as<cal::SweepCmd>(s.due_mav(0)).has_value());
  ack(s, 53, 0, 10, kFlags, cal::AckResult::Unsupported);
  CHECK(s.state() == State::AwaitAck);
  CHECK(!s.radio_silent(10));
  const auto snap = s.snapshot();
  CHECK((snap.skipped == std::vector<std::string>{"power20"}));
  CHECK(snap.walls.empty());
  run_capacity(s, 53, 1, 20);
  s.on_mav(cal::Applied{53, 1, 20, true, ""}, 300);
  CHECK(s.state() == State::Done);
  log.flush();
  const auto lines = cal_log_lines(dir);
  REQUIRE(lines.size() >= 3);
  CHECK(lines[1] == "R 53 1.00 36 pinned 1");
  CHECK(lines[2] == "X 0 power20 unsupported");
}

TEST(other_refusals_fail_the_run) {
  const std::pair<cal::AckResult, const char*> cases[] = {
      {cal::AckResult::AnchorUnreadable, "anchor_unreadable"},
      {cal::AckResult::Busy, "busy"},
      {cal::AckResult::BadChannel, "channel not in the drone's set"},
      {cal::AckResult::BadWidth, "width not flown by the drone"}};
  for (const auto& [res, word] : cases) {
    FakePicker p = pinned(36);
    CalSession s(CalSessionCfg{}, nullptr, &p);
    s.set_peer(true, true);
    std::string err;
    REQUIRE(s.start(54, {{Kind::Wall, 20}, {Kind::Capacity, 20}}, 0, &err));
    REQUIRE(s.due_mav(0).has_value());
    ack(s, 54, 0, 10, kFlags, res);
    CHECK(s.state() == State::Failed);
    CHECK(s.snapshot().fail_reason == std::string("sweep refused by the drone: ") + word);
    CHECK(!s.radio_silent(10));
  }
  // Unsupported skips only a wall sweep: a capacity sweep refused so fails.
  CalSession s(CalSessionCfg{});
  s.set_peer(true, true);
  std::string err;
  REQUIRE(s.start(55, {{Kind::Capacity, 20}}, 0, &err));
  REQUIRE(s.due_mav(0).has_value());
  ack(s, 55, 0, 10, kFlags, cal::AckResult::Unsupported);
  CHECK(s.state() == State::Failed);
  CHECK(s.snapshot().fail_reason == "sweep refused by the drone: unsupported");
}

TEST(an_invalid_measurement_fails_the_run) {
  const std::pair<cal::WallResult, const char*> cases[] = {
      {cal::WallResult::Unparked, "unparked"},
      {cal::WallResult::TooFewPoints, "too_few_points"},
      {cal::WallResult::FloorTooHigh, "floor_too_high"},
      {cal::WallResult::NotSaturated, "not_saturated"},
      {cal::WallResult::Deadline, "deadline"},
      {cal::WallResult::DeviceError, "device_error"},
      {cal::WallResult::Aborted, "aborted"}};
  for (const auto& [res, word] : cases) {
    FakePicker p = pinned(36);
    CalSession s(CalSessionCfg{}, nullptr, &p);
    s.set_peer(true, true);
    std::string err;
    REQUIRE(s.start(56, {{Kind::Wall, 20}, {Kind::Capacity, 20}}, 0, &err));
    REQUIRE(s.due_mav(0).has_value());
    ack(s, 56, 0, 10);
    s.on_mav(wall_done(56, 0, 36, 20, res), 5000);
    CHECK(s.state() == State::Failed);
    CHECK(s.snapshot().fail_reason == std::string("wall measurement failed: ") + word);
    CHECK(s.snapshot().walls.empty());
    CHECK(!s.radio_silent(5000));
    CHECK(as<cal::Abort>(s.due_mav(5000)).has_value());  // the drone's run is closed
  }
}

TEST(no_report_fails_the_run_when_the_window_closes) {
  CalSessionCfg cfg;
  FakePicker p = pinned(36);
  CalSession s(cfg, nullptr, &p);
  s.set_peer(true, true);
  std::string err;
  REQUIRE(s.start(57, {{Kind::Wall, 20}, {Kind::Capacity, 20}}, 0, &err));
  REQUIRE(s.due_mav(0).has_value());
  constexpr uint16_t kStartIn = 200;
  s.on_mav(cal::SweepAck{57, 0, cal::AckResult::Accepted, kStartIn, kFlags}, 100);
  const uint64_t close = 100 + kStartIn + kWallDeadlineMs + cfg.phase_slack_ms;
  // The drone is silent the whole time it measures; so is the GS.
  for (uint64_t t = 200; t < close; t += 100) {
    CHECK(!s.due_mav(t).has_value());
    CHECK(s.radio_silent(t));
  }
  CHECK(s.state() == State::AwaitDone);
  s.due_mav(close + 1);
  CHECK(s.state() == State::Failed);
  CHECK(s.snapshot().fail_reason == "no wall report from the drone");
  CHECK(!s.radio_silent(close + 1));
}

TEST(a_late_wall_report_is_ignored_by_the_next_sweep) {
  FakePicker p = pinned(36);
  CalSession s(CalSessionCfg{}, nullptr, &p);
  s.set_peer(true, true);
  std::string err;
  REQUIRE(s.start(58, {{Kind::Wall, 20}, {Kind::Capacity, 20}}, 0, &err));
  run_wall(s, 58, 0, 0);
  const auto cmd = as<cal::SweepCmd>(s.due_mav(3010));
  REQUIRE(cmd.has_value() && cmd->seq == 1);
  // A repeat of the wall report while the capacity command waits for its ack.
  s.on_mav(wall_done(58, 0, 36, 20), 3020);
  CHECK(s.state() == State::AwaitAck);
  ack(s, 58, 1, 3030, cal::kFlagStbc);
  REQUIRE(s.state() == State::Sweep);
  // ...and while the capacity sweep runs: no state change, no second report.
  s.on_mav(wall_done(58, 0, 36, 20), 3040);
  s.on_mav(wall_done(58, 0, 36, 20, cal::WallResult::Deadline), 3050);
  CHECK(s.state() == State::Sweep);
  CHECK(s.radio_silent(3050));
  CHECK(s.snapshot().walls.size() == 1);
  CHECK(s.snapshot().fail_reason.empty());
}

// --- a wall sweep whose ack is lost ----------------------------------------

TEST(a_wall_report_stands_in_for_a_lost_ack) {
  const std::string dir = fresh_dir("cal_session_implicit");
  CalLog log(dir);
  log.header();
  FakePicker p = pinned(36);
  CalSession s(CalSessionCfg{}, &log, &p);
  s.set_peer(true, true);
  std::string err;
  REQUIRE(s.start(61, {{Kind::Wall, 20}, {Kind::Capacity, 20}}, 0, &err));
  REQUIRE(as<cal::SweepCmd>(s.due_mav(0)).has_value());
  REQUIRE(s.state() == State::AwaitAck);
  s.on_mav(wall_done(61, 0, 36, 20), 1500);  // no ack ever arrived
  CHECK(s.snapshot().walls.size() == 1);
  CHECK(s.state() == State::AwaitAck);  // the capacity sweep's
  const auto next = as<cal::SweepCmd>(s.due_mav(1500));
  REQUIRE(next.has_value());
  CHECK(next->kind == Kind::Capacity && next->seq == 1);
  // Another sweep's or run's report is no ack.
  CalSession t(CalSessionCfg{}, nullptr, &p);
  t.set_peer(true, true);
  REQUIRE(t.start(62, {{Kind::Wall, 20}}, 0, &err));
  REQUIRE(t.due_mav(0).has_value());
  t.on_mav(wall_done(62, 1, 36, 20), 100);
  t.on_mav(wall_done(99, 0, 36, 20), 100);
  CHECK(t.state() == State::AwaitAck);
  // No ack announced the stamp flags: the R line's ldpc is unknown.
  log.flush();
  const auto lines = cal_log_lines(dir);
  REQUIRE(lines.size() >= 4);
  CHECK(lines[1] == "R 61 1.00 36 pinned -");
  CHECK(lines[2] == "S 0 wall 20 36");
  CHECK(lines[3] == "W 20 36 39 -306 63 63 63 63 44 26 24 20");
}

TEST(a_lost_ack_goes_silent_and_takes_a_late_report) {
  const std::string dir = fresh_dir("cal_session_lost_ack");
  CalLog log(dir);
  log.header();
  CalSessionCfg cfg;
  FakePicker p = pinned(36);
  CalSession s(cfg, &log, &p);
  s.set_peer(true, true);
  std::string err;
  REQUIRE(s.start(63, {{Kind::Wall, 20}}, 0, &err));
  uint64_t last_cmd = 0;
  for (uint64_t t = 0; t < cfg.ack_timeout_ms; t += 50)
    if (as<cal::SweepCmd>(s.due_mav(t))) last_cmd = t;
  REQUIRE(last_cmd == 2800);
  CHECK(!s.radio_silent(cfg.ack_timeout_ms - 1));
  // The ack timeout does not fail a wall sweep: the drone may be measuring.
  CHECK(!s.due_mav(cfg.ack_timeout_ms).has_value());
  CHECK(s.state() == State::Sweep);
  CHECK(s.radio_silent(cfg.ack_timeout_ms));
  for (uint64_t t = cfg.ack_timeout_ms; t < 6000; t += 100) {
    CHECK(!s.due_mav(t).has_value());  // no SweepCmd repeats into the measurement
    CHECK(s.radio_silent(t));
  }
  s.on_mav(wall_done(63, 0, 36, 20), 6000);
  CHECK(s.state() == State::Done);
  CHECK(s.snapshot().walls.size() == 1);
  CHECK(!s.radio_silent(6000));
  // The late report is recorded as an accepted sweep's.
  const auto lines = cal_log_lines(dir);
  REQUIRE(lines.size() == 4);
  CHECK(lines[1] == "R 63 1.00 36 pinned -");
  CHECK(lines[2] == "S 0 wall 20 36");
  CHECK(lines[3].rfind("W 20 36 ", 0) == 0);
}

// The report can land after the measurement window's nominal end, with the
// session already in AwaitDone: it still logs the sweep it implies.
// Reverting the AwaitDone arm in on_mav leaves the S row missing.
TEST(a_late_report_in_await_done_still_logs_the_sweep) {
  const std::string dir = fresh_dir("cal_session_late_done");
  CalLog log(dir);
  log.header();
  CalSessionCfg cfg;
  FakePicker p = pinned(36);
  CalSession s(cfg, &log, &p);
  s.set_peer(true, true);
  std::string err;
  REQUIRE(s.start(65, {{Kind::Wall, 20}}, 0, &err));
  for (uint64_t t = 0; t < cfg.ack_timeout_ms; t += 50) s.due_mav(t);
  const uint64_t past = 2800 + 200 + kWallDeadlineMs + 10;
  for (uint64_t t = cfg.ack_timeout_ms; t < past; t += 100) s.due_mav(t);
  REQUIRE(s.state() == State::AwaitDone);
  s.on_mav(wall_done(65, 0, 36, 20), past);
  CHECK(s.state() == State::Done);
  const auto lines = cal_log_lines(dir);
  REQUIRE(lines.size() == 4);
  CHECK(lines[2] == "S 0 wall 20 36");
  CHECK(lines[3].rfind("W 20 36 ", 0) == 0);
}

// A capacity frame is no ack for a wall sweep, and not a mismatch of one
// either: nothing of a wall sweep airs to be tallied.
// Reverting the capacity-only guard in on_cal_frame counts it as a mismatch.
TEST(a_capacity_frame_is_ignored_while_a_wall_sweep_awaits_its_ack) {
  FakePicker p = pinned(36);
  CalSession s(CalSessionCfg{}, nullptr, &p);
  s.set_peer(true, true);
  std::string err;
  REQUIRE(s.start(66, {{Kind::Wall, 20}}, 0, &err));
  REQUIRE(s.due_mav(0).has_value());
  s.on_cal_frame(0, stamp(Kind::Capacity, 0, 0, 20, kFlags), -50, true, 100, 100);
  CHECK(s.state() == State::AwaitAck);
  CHECK(s.snapshot().stamp_mismatch == 0);
}

TEST(a_lost_ack_and_no_report_fail_when_the_window_closes) {
  CalSessionCfg cfg;
  FakePicker p = pinned(36);
  CalSession s(cfg, nullptr, &p);
  s.set_peer(true, true);
  std::string err;
  REQUIRE(s.start(64, {{Kind::Wall, 20}, {Kind::Capacity, 20}}, 0, &err));
  for (uint64_t t = 0; t < cfg.ack_timeout_ms; t += 50) s.due_mav(t);
  // The last SweepCmd copy went out at 2800; the drone may have accepted
  // only that one, starting 200 ms later.
  const uint64_t close = 2800 + 200 + kWallDeadlineMs + cfg.phase_slack_ms;
  for (uint64_t t = cfg.ack_timeout_ms; t < close; t += 100) {
    CHECK(!s.due_mav(t).has_value());
    CHECK(s.radio_silent(t));
  }
  CHECK(s.state() == State::AwaitDone);
  s.due_mav(close);
  CHECK(s.state() == State::Failed);
  CHECK(s.snapshot().fail_reason == "no ack and no wall report from the drone");
  CHECK(!s.radio_silent(close));
}

TEST(no_link_channel_fails_before_sending) {
  CalSession s(CalSessionCfg{});
  s.set_peer(true, true);
  std::string err;
  REQUIRE(s.start(59, {{Kind::Wall, 20}}, 0, &err));
  CHECK(s.state() == State::Failed);
  CHECK(s.snapshot().fail_reason == "no link channel known");
  CHECK(!s.due_mav(0).has_value());  // nothing reached the drone: no Abort either

  // A link channel of 0 from the picker is no channel either.
  FakePicker p = pinned(0);
  CalSession t(CalSessionCfg{}, nullptr, &p);
  t.set_peer(true, true);
  REQUIRE(t.start(60, {{Kind::Wall, 20}}, 0, &err));
  CHECK(t.state() == State::Failed);
}

// --- the window and the frames ---------------------------------------------

TEST(sweep_command_repeats_until_acked_then_silence_opens) {
  CalSessionCfg cfg;
  CalSession s(cfg);
  s.set_peer(true, true);
  std::string err;
  REQUIRE(s.start(42, {{Kind::Capacity, 20}}, 0, &err));
  REQUIRE(as<cal::SweepCmd>(s.due_mav(0)).has_value());
  CHECK(!s.due_mav(100).has_value());
  CHECK(as<cal::SweepCmd>(s.due_mav(200)).has_value());
  CHECK(!s.radio_silent(250));

  ack(s, 42, /*seq=*/1, 280);  // another sweep's ack: ignored
  CHECK(s.state() == State::AwaitAck);
  ack(s, 41, /*seq=*/0, 290);  // another run's ack: ignored
  CHECK(s.state() == State::AwaitAck);

  s.on_mav(cal::SweepAck{42, 0, cal::AckResult::Accepted, /*start_in_ms=*/200, kFlags}, 300);
  CHECK(s.state() == State::Sweep);
  CHECK(!s.due_mav(400).has_value());
  CHECK(s.radio_silent(400));

  const uint64_t end = 300 + 200 + sweep_duration_ms(make_capacity_sweep(42, 0, 20));
  CHECK(!s.due_mav(end - 1).has_value());
  CHECK(s.state() == State::Sweep);
  CHECK(!s.due_mav(end).has_value());
  CHECK(s.state() == State::AwaitDone);
  CHECK(s.radio_silent(end + cfg.phase_slack_ms - 1));
  CHECK(!s.radio_silent(end + cfg.phase_slack_ms));
  s.due_mav(end + cfg.phase_slack_ms);
  CHECK(s.state() != State::AwaitDone);
  CHECK(!s.radio_silent(end + cfg.phase_slack_ms));
}

TEST(a_frame_with_the_wrong_width_or_flags_is_not_tallied) {
  CalSession s(CalSessionCfg{});
  s.set_peer(true, true);
  std::string err;
  REQUIRE(s.start(12, {{Kind::Capacity, 20}}, 0, &err));
  REQUIRE(s.due_mav(0).has_value());
  ack(s, 12, 0, 10, cal::kFlagStbc);
  s.on_cal_frame(0, stamp(Kind::Capacity, 3, 0, 40, cal::kFlagStbc), -60, true, 1500, 20);
  s.on_cal_frame(0, stamp(Kind::Capacity, 3, 0, 20, kFlags), -60, true, 1500, 20);
  CHECK(s.snapshot().cell_i == 0);
  CHECK(s.snapshot().stamp_mismatch == 2);
  s.on_cal_frame(0, stamp(Kind::Capacity, 3, 0, 20, cal::kFlagStbc), -60, true, 1500, 20);
  CHECK(s.snapshot().cell_i == 4);
  CHECK(s.snapshot().stamp_mismatch == 2);

  // Nothing airs during a wall sweep: a capacity frame then is a mismatch.
  CalSession w(CalSessionCfg{});
  w.set_peer(true, true);
  REQUIRE(w.start(13, {{Kind::Wall, 20, 36}}, 0, &err));
  REQUIRE(w.due_mav(0).has_value());
  ack(w, 13, 0, 10, cal::kFlagStbc);
  w.on_cal_frame(0, stamp(Kind::Capacity, 3, 0, 20, cal::kFlagStbc), -60, true, 1500, 20);
  CHECK(w.snapshot().stamp_mismatch == 1);
  CHECK(w.state() == State::Sweep);
}

TEST(capacity_sweep_yields_eff_and_zero_for_tx_bound) {
  CalSessionCfg cfg;
  CalSession s(cfg);
  s.set_peer(true, true);
  std::string err;
  REQUIRE(s.start(18, {{Kind::Capacity, 20}}, 0, &err));
  const auto cmd = as<cal::SweepCmd>(s.due_mav(0));
  REQUIRE(cmd.has_value());
  CHECK(cmd->kind == Kind::Capacity && cmd->dwell_ms == kCapDwellMs);
  ack(s, 18, 0, 10, cal::kFlagStbc);
  constexpr size_t kAir = 1500;
  for (uint8_t r = 0; r < 2; ++r)
    for (int k = 0; k < 900; ++k)
      s.on_cal_frame(0, stamp(Kind::Capacity, r, 0, 20, cal::kFlagStbc, k), -60, true, kAir,
                     100);
  s.due_mav(100);  // the core loop's tick after the pass that handled them
  cal::SweepDone d;
  d.nonce = 18;
  d.seq = 0;
  d.offered = {1000, 1000, 0, 0, 0, 0, 0, 0};
  d.blocked_pct = {80, 10, 0, 0, 0, 0, 0, 0};
  s.on_mav(d, 200);
  const auto a = as<cal::ApplyCmd>(s.due_mav(210));
  REQUIRE(a.has_value());
  CHECK(a->bw == 20);

  CapCell c0;
  c0.offered = 1000;
  c0.blocked_pct = 80;
  c0.frames = {900, 0};
  c0.air_bits = {900ull * kAir * 8, 0};
  c0.rssi_dbm = {-60, 0};
  c0.have_rssi = {true, false};
  const CapResult r0 = analyze_capacity(c0, 0, 20, kCapDwellMs, cfg.th);
  REQUIRE(!(r0.flags & kCalTxBound));
  CHECK(a->eff_x1000[0] == static_cast<uint16_t>(std::lround(r0.eff * 1000.0)));
  CHECK(a->eff_x1000[0] > 0);
  CHECK(a->eff_x1000[1] == 0);  // blocked 10 %: tx_bound, never written
  for (size_t r = 2; r < 8; ++r) CHECK(a->eff_x1000[r] == 0);
  const auto w = s.snapshot().width[0];
  CHECK(w.have_eff);
  CHECK(w.cap[1].flags & kCalTxBound);

  s.on_mav(cal::Applied{18, 0, 20, true, ""}, 300);
  CHECK(s.state() == State::Done);
  CHECK(!s.radio_silent(300));
}

// mcs0/HT20 is 6.5 Mbps nominal. 1212 frames of 1435 air bytes heard over
// 2300 ms (the drone's 2 s blast plus its TX queue's drain) is
// 1211 * 11480 / (2.3 s * 6.5e6) = 0.93; over the commanded 2 s dwell the
// same frames read 1.07, clamped to 1.00.
constexpr size_t kCapAir = 1435;
constexpr int kCapFrames = 1212;
constexpr uint64_t kCapSpanMs = 2300;

// The receive stamp of frame k of kCapFrames, spread evenly over the span
// from `t0`.
uint64_t cap_rx_ms(uint64_t t0, int k) {
  return t0 + kCapSpanMs * static_cast<uint64_t>(k) / static_cast<uint64_t>(kCapFrames - 1);
}

cal::SweepDone cap_done(uint32_t nonce, uint8_t seq, uint32_t offered) {
  cal::SweepDone d;
  d.nonce = nonce;
  d.seq = seq;
  d.offered = {offered, 0, 0, 0, 0, 0, 0, 0};
  d.blocked_pct = {90, 0, 0, 0, 0, 0, 0, 0};
  return d;
}

TEST(capacity_eff_is_timed_by_the_frames_receive_stamps) {
  // The core loop drains the receive queue in bursts, so the session's
  // clock (its latest tick) can run ahead of the frames it is handed. The
  // span is the frames' own receive stamps: here the loop stalls 300 ms
  // before the last ten frames, and timing them by the tick would read
  // 2600 ms, an eff of 0.82.
  const std::string dir = fresh_dir("cal_session_rx_stamp");
  CalLog log(dir);
  log.header();
  CalSession s(CalSessionCfg{}, &log);
  s.set_peer(true, true);
  std::string err;
  REQUIRE(s.start(40, {{Kind::Capacity, 20}}, 0, &err));
  REQUIRE(as<cal::SweepCmd>(s.due_mav(0)).has_value());
  ack(s, 40, 0, 10, cal::kFlagStbc);
  for (int k = 0; k < kCapFrames; ++k) {
    const uint64_t rx = cap_rx_ms(100, k);
    s.due_mav(k == kCapFrames - 10 ? rx + 300 : rx + 2);  // the core loop's tick
    s.on_cal_frame(0, stamp(Kind::Capacity, 0, 0, 20, cal::kFlagStbc, k), -60, true, kCapAir,
                   rx);
  }
  s.due_mav(2400);  // the core loop's tick after the pass that handled them
  s.on_mav(cap_done(40, 0, kCapFrames), 2800);
  const auto a = as<cal::ApplyCmd>(s.due_mav(2810));
  REQUIRE(a.has_value());
  CHECK(a->eff_x1000[0] == 930);
  log.flush();
  std::string p_row;
  for (const auto& l : cal_log_lines(dir))
    if (l.rfind("P 0 ", 0) == 0) p_row = l;
  CHECK(p_row == "P 0 1212 1212 13913760 930 90 0 2300");
}

TEST(a_capacity_sweep_listens_through_the_drones_tx_queue_drain) {
  // The drone's SweepDone goes out when its last blast returns, ahead of
  // the frames its TX queue still holds: the GS keeps tallying until the
  // stream has been quiet for tail_quiet_ms. Leaving at the SweepDone would
  // drop the tail from the point's count (reading it as loss) and count
  // what arrived after the GS moved on as stamp mismatches.
  const std::string dir = fresh_dir("cal_session_drain");
  CalLog log(dir);
  log.header();
  CalSessionCfg cfg;
  CalSession s(cfg, &log);
  s.set_peer(true, true);
  std::string err;
  REQUIRE(s.start(41, {{Kind::Capacity, 20}, {Kind::Wall, 40, 36}}, 0, &err));
  REQUIRE(as<cal::SweepCmd>(s.due_mav(0)).has_value());
  ack(s, 41, 0, 10, cal::kFlagStbc);
  bool done_sent = false;
  for (int k = 0; k < kCapFrames; ++k) {
    const uint64_t rx = cap_rx_ms(100, k);
    if (!done_sent && rx >= 2100) {  // the blast returned at 2100; 300 ms still queued
      s.on_mav(cap_done(41, 0, kCapFrames), rx);
      done_sent = true;
    }
    CHECK(!as<cal::ApplyCmd>(s.due_mav(rx)).has_value());
    CHECK(s.radio_silent(rx));
    s.on_cal_frame(0, stamp(Kind::Capacity, 0, 0, 20, cal::kFlagStbc, k), -60, true, kCapAir,
                   rx);
  }
  REQUIRE(done_sent);
  // The tick after the pass that handled the last frame starts the quiet.
  const uint64_t tick = cap_rx_ms(100, kCapFrames - 1) + 1;
  CHECK(!as<cal::ApplyCmd>(s.due_mav(tick)).has_value());
  CHECK(!as<cal::ApplyCmd>(s.due_mav(tick + cfg.tail_quiet_ms - 1)).has_value());
  const auto a = as<cal::ApplyCmd>(s.due_mav(tick + cfg.tail_quiet_ms));
  REQUIRE(a.has_value());
  CHECK(a->eff_x1000[0] == 930);
  log.flush();
  std::string p_row;
  for (const auto& l : cal_log_lines(dir))
    if (l.rfind("P 0 ", 0) == 0) p_row = l;
  CHECK(p_row == "P 0 1212 1212 13913760 930 90 0 2300");
  CHECK(s.snapshot().stamp_mismatch == 0);
}

TEST(a_stalled_core_loop_cannot_fake_a_quiet_tail) {
  // Quiet is measured in session time on the processing side: one core-loop
  // pass running long puts the clock 80 ms past the last frame it handled
  // while the drain's last frames still wait in the receive queue (with
  // older stamps). That is no sweep frame having reached the session for
  // 0 ms, not 80: the sweep stays open until 50 ms of session time pass
  // with nothing tallied.
  const std::string dir = fresh_dir("cal_session_stall");
  CalLog log(dir);
  log.header();
  CalSessionCfg cfg;
  CalSession s(cfg, &log);
  s.set_peer(true, true);
  std::string err;
  REQUIRE(s.start(43, {{Kind::Capacity, 20}}, 0, &err));
  REQUIRE(as<cal::SweepCmd>(s.due_mav(0)).has_value());
  ack(s, 43, 0, 10, cal::kFlagStbc);
  constexpr int kSplit = 1180;
  for (int k = 0; k < kSplit; ++k) {
    const uint64_t rx = cap_rx_ms(100, k);
    s.due_mav(rx);
    s.on_cal_frame(0, stamp(Kind::Capacity, 0, 0, 20, cal::kFlagStbc, k), -60, true, kCapAir,
                   rx);
  }
  const uint64_t seen = cap_rx_ms(100, kSplit - 1);
  s.on_mav(cap_done(43, 0, kCapFrames), seen);
  // The long pass ends: its tick is 80 ms past the last frame it handled.
  CHECK(!as<cal::ApplyCmd>(s.due_mav(seen + 80)).has_value());
  // The next pass handles the queued tail, all stamped before that tick.
  for (int k = kSplit; k < kCapFrames; ++k) {
    const uint64_t rx = cap_rx_ms(100, k);
    REQUIRE(rx < seen + 80);
    s.on_cal_frame(0, stamp(Kind::Capacity, 0, 0, 20, cal::kFlagStbc, k), -60, true, kCapAir,
                   rx);
  }
  const uint64_t tick = seen + 85;
  CHECK(!as<cal::ApplyCmd>(s.due_mav(tick)).has_value());
  CHECK(!as<cal::ApplyCmd>(s.due_mav(tick + cfg.tail_quiet_ms - 1)).has_value());
  const auto a = as<cal::ApplyCmd>(s.due_mav(tick + cfg.tail_quiet_ms));
  REQUIRE(a.has_value());
  CHECK(a->eff_x1000[0] == 930);
  log.flush();
  std::string p_row;
  for (const auto& l : cal_log_lines(dir))
    if (l.rfind("P 0 ", 0) == 0) p_row = l;
  CHECK(p_row == "P 0 1212 1212 13913760 930 90 0 2300");
  CHECK(s.snapshot().stamp_mismatch == 0);
}

TEST(a_capacity_point_that_lost_over_half_its_frames_is_not_written) {
  // Under half of what was offered arrived: the dwell measured the loss,
  // not the air, and is kept out of the config like a tx_bound point. From
  // half up it is lossy and still written.
  CalSession s(CalSessionCfg{});
  s.set_peer(true, true);
  std::string err;
  REQUIRE(s.start(19, {{Kind::Capacity, 20}}, 0, &err));
  REQUIRE(as<cal::SweepCmd>(s.due_mav(0)).has_value());
  ack(s, 19, 0, 10, cal::kFlagStbc);
  constexpr size_t kAir = 1500;
  const int got[3] = {10, 499, 600};  // of 1000 offered each
  for (uint8_t r = 0; r < 3; ++r)
    for (int k = 0; k < got[r]; ++k)
      s.on_cal_frame(0, stamp(Kind::Capacity, r, 0, 20, cal::kFlagStbc, k), -60, true, kAir,
                     100);
  s.due_mav(100);  // the core loop's tick after the pass that handled them
  cal::SweepDone d;
  d.nonce = 19;
  d.seq = 0;
  d.offered = {1000, 1000, 1000, 0, 0, 0, 0, 0};
  d.blocked_pct = {80, 80, 80, 0, 0, 0, 0, 0};
  s.on_mav(d, 200);
  const auto a = as<cal::ApplyCmd>(s.due_mav(210));
  REQUIRE(a.has_value());
  CHECK(a->eff_x1000[0] == 0);
  CHECK(a->eff_x1000[1] == 0);
  CHECK(a->eff_x1000[2] > 0);
  const auto w = s.snapshot().width[0];
  CHECK(w.eff_x1000[0] == 0 && w.eff_x1000[1] == 0 && w.eff_x1000[2] > 0);
  CHECK((w.cap[0].flags & kCalLost) && (w.cap[0].flags & kCalLossy));
  CHECK((w.cap[1].flags & kCalLost) && (w.cap[1].flags & kCalLossy));
  CHECK(!(w.cap[2].flags & kCalLost) && (w.cap[2].flags & kCalLossy));
}

TEST(a_capacity_sweep_that_lost_every_point_writes_nothing) {
  CalSession s(CalSessionCfg{});
  s.set_peer(true, true);
  std::string err;
  REQUIRE(s.start(20, {{Kind::Capacity, 20}}, 0, &err));
  REQUIRE(as<cal::SweepCmd>(s.due_mav(0)).has_value());
  ack(s, 20, 0, 10, cal::kFlagStbc);
  for (int k = 0; k < 5; ++k)
    s.on_cal_frame(0, stamp(Kind::Capacity, 0, 0, 20, cal::kFlagStbc, k), -60, true, 1500, 100);
  s.due_mav(100);  // the core loop's tick after the pass that handled them
  cal::SweepDone d;
  d.nonce = 20;
  d.seq = 0;
  d.offered = {1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000};
  d.blocked_pct = {80, 80, 80, 80, 80, 80, 80, 80};
  s.on_mav(d, 200);
  for (size_t r = 0; r < 8; ++r) CHECK(s.snapshot().width[0].eff_x1000[r] == 0);
}

TEST(capacity_without_done_writes_no_eff) {
  CalSessionCfg cfg;
  CalSession s(cfg);
  s.set_peer(true, true);
  std::string err;
  REQUIRE(s.start(19, {{Kind::Capacity, 20}}, 0, &err));
  REQUIRE(s.due_mav(0).has_value());
  ack(s, 19, 0, 10, cal::kFlagStbc);
  for (int k = 0; k < 900; ++k)
    s.on_cal_frame(0, stamp(Kind::Capacity, 0, 0, 20, cal::kFlagStbc, k), -60, true, 1500, 100);
  s.due_mav(100);  // the core loop's tick after the pass that handled them
  const uint64_t close =
      10 + sweep_duration_ms(make_capacity_sweep(19, 0, 20)) + cfg.phase_slack_ms;
  for (uint64_t t = 200; t <= close + 2000; t += 100)
    CHECK(!as<cal::ApplyCmd>(s.due_mav(t)).has_value());
  CHECK(s.state() == State::Done);
  const auto w = s.snapshot().width[0];
  CHECK(w.eff_unknown);
  CHECK(!w.have_eff);
  for (size_t r = 0; r < 8; ++r) CHECK(w.eff_x1000[r] == 0);
}

TEST(refused_apply_fails_with_the_drones_reason) {
  CalSession s(CalSessionCfg{});
  s.set_peer(true, true);
  std::string err;
  REQUIRE(s.start(17, {{Kind::Capacity, 20}, {Kind::Capacity, 40}}, 0, &err));
  run_capacity(s, 17, 0, 0);
  s.on_mav(cal::Applied{17, 0, 20, false, "config line not found"}, 300);
  CHECK(s.state() == State::Failed);
  const auto snap = s.snapshot();
  CHECK(snap.fail_reason.find("config line not found") != std::string::npos);
  CHECK(!snap.width[0].applied);
  CHECK(snap.width[0].apply_reason == "config line not found");
  CHECK(!s.radio_silent(300));
}

TEST(a_receive_stamp_older_than_the_last_tick_fires_no_timeout) {
  // on_mav and on_cal_frame run with receive-thread stamps, which can be
  // older than the core loop's tick the session has already seen. Time in
  // the session never runs backwards, so neither the ack nor the apply
  // timeout reads an old stamp as a huge elapsed time.
  CalSession s(CalSessionCfg{});
  s.set_peer(true, true);
  std::string err;
  REQUIRE(s.start(21, {{Kind::Capacity, 20}}, 1000, &err));
  REQUIRE(as<cal::SweepCmd>(s.due_mav(1005)).has_value());
  s.on_mav(cal::SweepDone{}, 999);  // not this state's; only the stamp matters
  CHECK(s.state() == State::AwaitAck);
  s.on_cal_frame(0, stamp(Kind::Capacity, 0, 0, 40, kFlags), -60, true, 1500, 998);
  CHECK(s.state() == State::AwaitAck);

  run_capacity(s, 21, 0, 1990);
  REQUIRE(s.state() == State::Apply);
  s.on_mav(cal::SweepAck{}, 2001);  // older than the apply began
  CHECK(s.state() == State::Apply);
}

TEST(done_sends_three_aborts_so_the_drone_resumes_at_once) {
  // The drone otherwise sits in its await window (video off) until it
  // times out. Three copies 200 ms apart, since the uplink is lossy and the
  // drone ignores a nonce it already closed.
  CalSession s(CalSessionCfg{});
  s.set_peer(true, true);
  std::string err;
  REQUIRE(s.start(35, {{Kind::Wall, 20, 36}}, 0, &err));
  run_wall(s, 35, 0, 0);
  std::vector<uint64_t> at;
  for (uint64_t t = 3000; t < 5000; t += 50) {
    const auto m = s.due_mav(t);
    if (const auto a = as<cal::Abort>(m)) {
      at.push_back(t);
      CHECK(a->nonce == 35);
    }
    CHECK(!as<cal::SweepCmd>(m).has_value());
  }
  CHECK(s.state() == State::Done);
  REQUIRE(at.size() == 3);
  CHECK(at[1] - at[0] == 200);
  CHECK(at[2] - at[1] == 200);
}

TEST(a_repeated_done_after_analyze_is_ignored) {
  // The drone repeats SweepDone every 200 ms until the GS's next command
  // reaches it, so repeats keep landing after the sweep was analysed: in
  // Apply and in the next sweep's AwaitAck. None may move the session or
  // change what it sends.
  CalSession s(CalSessionCfg{});
  s.set_peer(true, true);
  std::string err;
  REQUIRE(s.start(37, {{Kind::Capacity, 20}, {Kind::Capacity, 40}}, 0, &err));
  const auto a = run_capacity(s, 37, 0, 0);
  REQUIRE(s.state() == State::Apply);
  cal::SweepDone d;
  d.nonce = 37;
  d.seq = 0;
  s.on_mav(d, 220);
  CHECK(s.state() == State::Apply);
  const auto again = as<cal::ApplyCmd>(s.due_mav(420));
  REQUIRE(again.has_value());
  CHECK(again->seq == a.seq && again->eff_x1000 == a.eff_x1000);

  s.on_mav(cal::Applied{37, 0, 20, true, ""}, 430);
  const auto cmd = as<cal::SweepCmd>(s.due_mav(440));
  REQUIRE(cmd.has_value());
  CHECK(cmd->seq == 1);
  s.on_mav(d, 450);
  CHECK(s.state() == State::AwaitAck);
  CHECK(s.snapshot().width[0].have_eff);
}

TEST(a_stale_applied_for_another_seq_does_not_satisfy_the_apply) {
  CalSession s(CalSessionCfg{});
  s.set_peer(true, true);
  std::string err;
  REQUIRE(s.start(36, {{Kind::Wall, 40, 36}, {Kind::Capacity, 40}}, 0, &err));
  run_wall(s, 36, 0, 0);
  const auto a = run_capacity(s, 36, 1, 3100);
  CHECK(a.seq == 1 && a.bw == 40);
  // The drone's cached answer to another sweep of the run (same nonce,
  // same width) must not count as this apply's.
  s.on_mav(cal::Applied{36, 0, 40, true, ""}, 3320);
  CHECK(s.state() == State::Apply);
  CHECK(as<cal::ApplyCmd>(s.due_mav(3520)).has_value());
  s.on_mav(cal::Applied{36, 1, 40, true, ""}, 3530);
  CHECK(s.state() == State::Done);
}

TEST(a_crc_bad_frame_is_not_an_implicit_ack) {
  // maburgs keeps corrupted frames, and a flipped LDPC/STBC bit still
  // parses: such a frame must not set the run's flags.
  CalSession s(CalSessionCfg{});
  s.set_peer(true, true);
  std::string err;
  REQUIRE(s.start(37, {{Kind::Capacity, 20}}, 0, &err));
  REQUIRE(s.due_mav(0).has_value());
  s.on_cal_frame(0, stamp(Kind::Capacity, 0, 0, 20, cal::kFlagLdpc), -60, /*crc_ok=*/false, 1500,
                 50);
  CHECK(s.state() == State::AwaitAck);
  CHECK(as<cal::SweepCmd>(s.due_mav(200)).has_value());  // still commanding
  s.on_cal_frame(0, stamp(Kind::Capacity, 0, 0, 20, kFlags), -60, true, 1500, 250);
  CHECK(s.state() == State::Sweep);
  CHECK(s.snapshot().flags == kFlags);
}

TEST(ack_timeout_fails_without_an_apply) {
  CalSessionCfg cfg;
  CalSession s(cfg);
  s.set_peer(true, true);
  std::string err;
  REQUIRE(s.start(22, {{Kind::Capacity, 20}}, 0, &err));
  int applies = 0, aborts = 0;
  for (uint64_t t = 0; t <= cfg.ack_timeout_ms + 1000; t += 50) {
    const auto m = s.due_mav(t);
    if (as<cal::ApplyCmd>(m)) ++applies;
    if (as<cal::Abort>(m)) ++aborts;
  }
  CHECK(s.state() == State::Failed);
  CHECK(s.snapshot().fail_reason.find("ack") != std::string::npos);
  CHECK(applies == 0);
  CHECK(aborts == 3);  // the drone may have heard a SweepCmd: close its run
  CHECK(!s.radio_silent(cfg.ack_timeout_ms + 1000));
}

TEST(apply_timeout_fails) {
  CalSessionCfg cfg;
  CalSession s(cfg);
  s.set_peer(true, true);
  std::string err;
  REQUIRE(s.start(24, {{Kind::Capacity, 20}}, 0, &err));
  run_capacity(s, 24, 0, 0);  // Apply from the SweepDone at t = 200
  int applies = 1;
  for (uint64_t t = 250; t < 200 + cfg.apply_timeout_ms; t += 50)
    if (as<cal::ApplyCmd>(s.due_mav(t))) ++applies;
  CHECK(s.state() == State::Apply);
  CHECK(applies >= 20);  // every 200 ms
  s.due_mav(200 + cfg.apply_timeout_ms);
  CHECK(s.state() == State::Failed);
  CHECK(s.snapshot().fail_reason.find("apply") != std::string::npos);
}

TEST(abort_emits_three_aborts_and_reopens_the_air) {
  CalSession s(CalSessionCfg{});
  s.set_peer(true, true);
  std::string err;
  REQUIRE(s.start(25, {{Kind::Wall, 20, 36}}, 0, &err));
  REQUIRE(s.due_mav(0).has_value());
  ack(s, 25, 0, 10);
  REQUIRE(s.radio_silent(100));
  s.abort("operator abort");
  CHECK(!s.radio_silent(100));
  CHECK(s.state() == State::Idle);
  CHECK(!s.running());
  const auto m = as<cal::Abort>(s.due_mav(110));
  REQUIRE(m.has_value());
  CHECK(m->nonce == 25);
  CHECK(!s.due_mav(120).has_value());
  CHECK(!s.due_mav(309).has_value());
  CHECK(as<cal::Abort>(s.due_mav(310)).has_value());
  CHECK(!s.due_mav(400).has_value());
  const auto last = as<cal::Abort>(s.due_mav(510));
  REQUIRE(last.has_value());
  CHECK(last->nonce == 25);
  CHECK(!s.due_mav(710).has_value());
  CHECK(!s.due_mav(1000).has_value());
  CHECK(s.snapshot().fail_reason == "operator abort");
  // A late report or ack of the aborted run changes nothing.
  ack(s, 25, 0, 130);
  s.on_mav(wall_done(25, 0, 36, 20), 140);
  CHECK(s.state() == State::Idle);
  CHECK(s.snapshot().walls.empty());
}

TEST(picker_is_cancelled_on_pick_timeout_and_abort) {
  CalSessionCfg cfg;
  std::string err;
  FakePicker p;
  p.op_ch = 36;
  CalSession s(cfg, nullptr, &p);
  s.set_peer(true, true);
  REQUIRE(s.start(26, {{Kind::Wall, 20}}, 0, &err));
  CHECK(!s.due_mav(cfg.pick_timeout_ms - 1).has_value());
  CHECK(p.cancels == 0);
  CHECK(as<cal::SweepCmd>(s.due_mav(cfg.pick_timeout_ms)).has_value());
  CHECK(p.cancels == 1);
  CHECK(s.snapshot().channel == 36);
  CHECK(s.snapshot().pick_note.find("stayed on 36") == 0);

  FakePicker q;
  CalSession t(cfg, nullptr, &q);
  t.set_peer(true, true);
  REQUIRE(t.start(27, {{Kind::Wall, 20}}, 0, &err));
  t.abort("operator abort");
  CHECK(q.cancels == 1);
  CHECK(t.state() == State::Idle);
  CHECK(!t.due_mav(10).has_value());  // nothing reached the drone: no Abort
}

TEST(implicit_ack_from_the_first_matching_frame) {
  CalSession s(CalSessionCfg{});
  s.set_peer(true, true);
  std::string err;
  REQUIRE(s.start(28, {{Kind::Capacity, 20}}, 0, &err));
  REQUIRE(s.due_mav(0).has_value());
  // A 40 MHz frame does not stand in for the ack.
  s.on_cal_frame(0, stamp(Kind::Capacity, 0, 0, 40, cal::kFlagStbc), -60, true, 1500, 60);
  CHECK(s.state() == State::AwaitAck);
  s.on_cal_frame(0, stamp(Kind::Capacity, 0, 0, 20, cal::kFlagStbc), -60, true, 1500, 100);
  CHECK(s.state() == State::Sweep);
  CHECK(s.radio_silent(100));
  CHECK(s.snapshot().flags == cal::kFlagStbc);  // adopted from the frame
  CHECK(s.snapshot().cell_i == 1);
  CHECK(!s.due_mav(300).has_value());  // no more SweepCmd repeats
  // The adopted flags now filter like an ack's.
  const uint32_t mismatched = s.snapshot().stamp_mismatch;
  s.on_cal_frame(0, stamp(Kind::Capacity, 0, 0, 20, kFlags), -60, true, 1500, 110);
  CHECK(s.snapshot().stamp_mismatch == mismatched + 1);

  // A wall sweep airs nothing: a capacity frame never stands in for its ack.
  CalSession w(CalSessionCfg{});
  w.set_peer(true, true);
  REQUIRE(w.start(29, {{Kind::Wall, 20, 36}}, 0, &err));
  REQUIRE(w.due_mav(0).has_value());
  w.on_cal_frame(0, stamp(Kind::Capacity, 0, 0, 20, cal::kFlagStbc), -60, true, 1500, 100);
  CHECK(w.state() == State::AwaitAck);
}

TEST(snapshot_tracks_sweep_and_cell_progress) {
  CalSession s(CalSessionCfg{});
  s.set_peer(true, true);
  std::string err;
  REQUIRE(s.start(30, {{Kind::Capacity, 20}, {Kind::Wall, 20, 36}}, 0, &err));
  REQUIRE(s.due_mav(0).has_value());
  ack(s, 30, 0, 10);
  auto snap = s.snapshot();
  CHECK(snap.state == State::Sweep);
  CHECK(snap.sweep_i == 0 && snap.sweep_n == 2);
  CHECK(snap.cur.kind == Kind::Wall && snap.cur.bw == 20);
  // A wall sweep counts seconds since its ack, against the drone's deadline.
  CHECK(snap.cell_n == kWallDeadlineMs / 1000);
  CHECK(snap.cell_i == 0);
  CHECK(snap.flags == kFlags);
  s.due_mav(5009);
  CHECK(s.snapshot().cell_i == 4);
  s.due_mav(5010);
  CHECK(s.snapshot().cell_i == 5);

  s.on_mav(wall_done(30, 0, 36, 20), 6000);
  const auto cmd = as<cal::SweepCmd>(s.due_mav(6010));
  REQUIRE(cmd.has_value());
  CHECK(cmd->kind == Kind::Capacity && cmd->seq == 1);
  ack(s, 30, 1, 6020);
  snap = s.snapshot();
  CHECK(snap.sweep_i == 1);
  CHECK(snap.cur.kind == Kind::Capacity);
  CHECK(snap.cell_n == 8);
  s.on_cal_frame(0, stamp(Kind::Capacity, 3, 0, 20, kFlags), -60, true, 1500, 6030);
  CHECK(s.snapshot().cell_i == 4);
}

TEST(tag_context_is_frozen_at_start) {
  CalSession s(CalSessionCfg{});
  s.set_peer(true, true, mabur::rc::TagCtx{11, 22, 0});
  std::string err;
  REQUIRE(s.start(31, {{Kind::Wall, 20, 36}}, 0, &err));
  s.set_peer(true, true, mabur::rc::TagCtx{11, 33, 0});   // re-paired mid-run
  s.set_peer(false, true, mabur::rc::TagCtx{11, 33, 0});  // and lost again
  CHECK(s.tag_ctx().vrx_nonce == 11);
  CHECK(s.tag_ctx().vtx_nonce == 22);
  s.abort("test");
  s.set_peer(true, true, mabur::rc::TagCtx{11, 44, 0});
  REQUIRE(s.start(32, {{Kind::Wall, 20, 36}}, 10, &err));
  CHECK(s.tag_ctx().vtx_nonce == 44);

  // With a pick, nothing reaches the drone until the pick ends, so the
  // context is the one the link holds when the first SweepCmd is due.
  FakePicker p;
  p.best = 36;
  CalSession t(CalSessionCfg{}, nullptr, &p);
  t.set_peer(true, true, mabur::rc::TagCtx{1, 2, 0});
  REQUIRE(t.start(33, {{Kind::Wall, 20}}, 0, &err));
  t.set_peer(true, true, mabur::rc::TagCtx{1, 3, 0});
  REQUIRE(as<cal::SweepCmd>(t.due_mav(10)).has_value());
  CHECK(t.tag_ctx().vtx_nonce == 3);
  t.set_peer(true, true, mabur::rc::TagCtx{1, 4, 0});
  CHECK(t.tag_ctx().vtx_nonce == 3);
}

TEST(cal_log_gets_run_sweep_wall_cap_and_applied_rows) {
  const std::string dir = fresh_dir("cal_session_log");
  CalLog log(dir);
  REQUIRE(log.ok());
  log.header();
  FakePicker p = pinned(36);
  CalSession s(CalSessionCfg{}, &log, &p);
  s.set_peer(true, true);
  std::string err;
  REQUIRE(s.start(77, {{Kind::Capacity, 20}, {Kind::Wall, 20}}, 0, &err));
  run_wall(s, 77, 0, 0);
  run_capacity(s, 77, 1, 3100);
  s.on_mav(cal::Applied{77, 1, 20, true, ""}, 3320);
  REQUIRE(s.state() == State::Done);

  // On disk the moment the run reaches Done, without closing the log.
  const auto lines = cal_log_lines(dir);
  std::vector<std::string> seq;
  for (const auto& l : lines) {
    const std::string tag = l.substr(0, l.find(' '));
    if (seq.empty() || seq.back() != tag) seq.push_back(tag);
  }
  const std::vector<std::string> want = {"callog", "R", "S", "W", "S", "P", "A"};
  CHECK(seq == want);
  REQUIRE(lines.size() == 6 + 8);  // a P row per rate the capacity sweep commanded
  CHECK(lines[0] == "callog 6");
  CHECK(lines[1] == "R 77 1.00 36 pinned 1");
  CHECK(lines[2] == "S 0 wall 20 36");
  CHECK(lines[3] == "W 20 36 39 -306 63 63 63 63 44 26 24 20");
  CHECK(lines[4] == "S 1 capacity 20 2000");
  CHECK(lines[5].rfind("P 0 1000 900 ", 0) == 0);
  CHECK(lines.back() == "A 20 1 ");
}

TEST(null_log_sink_is_inert) {
  CalSession s(CalSessionCfg{});
  s.set_peer(true, true);
  std::string err;
  REQUIRE(s.start(34, {{Kind::Wall, 20, 36}}, 0, &err));
  run_wall(s, 34, 0, 0);
  CHECK(s.state() == State::Done);
}

MTEST_MAIN
