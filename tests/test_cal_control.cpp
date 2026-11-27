#include "mtest.h"
#include "cal_control.h"
#include "cal_picker.h"
#include "cal_session.h"
#include "ladder_controller.h"

#include <string>
#include <vector>

using namespace maburgs;
namespace cal = mabur::cal;

namespace {
const std::vector<cal::SweepSpec> kDefault20 = {{cal::Kind::Wall, 20},
                                                 {cal::Kind::Capacity, 20}};

// The link's channel, pinned: a wall sweep naming none measures this one.
struct PinnedPicker : CalPicker {
  Mode mode() const override { return Mode::Pinned; }
  uint8_t op() const override { return 36; }
  void begin_measure(uint64_t) override {}
  std::optional<uint8_t> measured_best() override { return op(); }
  bool order_relocate(uint8_t) override { return false; }
  std::optional<bool> relocate_settled() override { return std::nullopt; }
  void cancel() override {}
};
PinnedPicker g_picker;
}  // namespace

// --- default_sweeps -----------------------------------------------------

TEST(default_sweeps_is_two_for_a_20_mhz_gs) {
  LadderCfg ladder;  // every rung defaults to bw 20
  const auto sweeps = default_sweeps(/*rx_width=*/20, ladder);
  REQUIRE(sweeps.size() == 2);
  CHECK(sweeps[0].kind == cal::Kind::Wall);
  CHECK(sweeps[0].bw == 20);
  CHECK(sweeps[1].kind == cal::Kind::Capacity);
  CHECK(sweeps[1].bw == 20);
}

TEST(default_sweeps_ignores_a_40_rung_on_a_20_mhz_gs) {
  // A rung flying 40 MHz means nothing if the GS itself is pinned to 20 --
  // it can never hear that rung's sweep frames.
  LadderCfg ladder;
  ladder.ladder.push_back({4, 0.5, 0.5, 40});
  const auto sweeps = default_sweeps(/*rx_width=*/20, ladder);
  CHECK(sweeps.size() == 2);
}

TEST(four_for_a_40_gs_with_a_40_rung) {
  LadderCfg ladder;
  ladder.ladder.push_back({4, 0.5, 0.5, 40});
  const auto sweeps = default_sweeps(/*rx_width=*/40, ladder);
  CHECK(sweeps.size() == 4);
}

TEST(default_sweeps_is_two_for_a_40_gs_with_no_40_rung) {
  // The GS can fly 40, but nothing on its ladder ever does -- a 40 MHz
  // sweep would calibrate a width the ladder never picks.
  LadderCfg ladder;
  const auto sweeps = default_sweeps(/*rx_width=*/40, ladder);
  CHECK(sweeps.size() == 2);
}

// --- CalControl::apply: start ------------------------------------------

TEST(start_with_no_tokens_uses_the_defaults) {
  CalSession s(CalSessionCfg{}, nullptr, &g_picker);
  s.set_peer(true, true);
  std::string reply;
  CHECK(CalControl::apply("start", s, &reply, kDefault20, /*rx_width=*/20));
  CHECK(reply == "ok started power20,cap20");
  CHECK(s.state() == CalSession::State::AwaitAck);
  REQUIRE(s.sweeps().size() == 2);
}

TEST(start_with_tokens_builds_that_list) {
  CalSession s(CalSessionCfg{});
  s.set_peer(true, true);
  std::string reply;
  // Any order, plus a duplicate -- the session sorts (20 before 40, wall
  // before capacity within a width) and drops the repeat.
  CHECK(CalControl::apply("start cap40 power20 cap20 power40 power20", s, &reply,
                          kDefault20, /*rx_width=*/40));
  CHECK(reply == "ok started power20,cap20,power40,cap40");
  REQUIRE(s.sweeps().size() == 4);
}

TEST(start_power40_on_a_20_gs_is_refused_with_nothing_started) {
  CalSession s(CalSessionCfg{});
  s.set_peer(true, true);
  std::string reply;
  CHECK(!CalControl::apply("start power20 power40", s, &reply, kDefault20,
                           /*rx_width=*/20));
  CHECK(reply == "err refused: GS receiver width is 20 (load a 40 MHz ladder to calibrate 40 MHz)");
  CHECK(s.state() == CalSession::State::Idle);
}

TEST(channel_tokens) {
  for (const char* bad :
       {"power20@", "power20@0", "power20@abc", "power20@999", "power20@112x", "cap20@112",
        "power20@11x", "power20@+12"}) {
    CalSession s(CalSessionCfg{});
    s.set_peer(true, true);
    std::string reply;
    CHECK(!CalControl::apply(std::string("start ") + bad, s, &reply, kDefault20, 20));
    CHECK(reply == std::string("err bad token: ") + bad);
    CHECK(s.state() == CalSession::State::Idle);
  }
  CalSession s(CalSessionCfg{});
  s.set_peer(true, true);
  std::string reply;
  CHECK(CalControl::apply("start power20@112 cap20", s, &reply, kDefault20, 20));
  CHECK(reply == "ok started power20@112,cap20");
  REQUIRE(s.sweeps().size() == 2);
  CHECK(s.sweeps()[0].channel == 112);
  CHECK(s.sweeps()[1].channel == 0);
  // 177 is the top of the range, and a 40 MHz wall sweep takes a channel too.
  CalSession t(CalSessionCfg{});
  t.set_peer(true, true);
  CHECK(CalControl::apply("start power40@177", t, &reply, kDefault20, 40));
  CHECK(reply == "ok started power40@177");
}

// Two wall sweeps of one width on different channels both run; only an
// exact repeat is dropped.
TEST(two_channels_at_one_width_both_run) {
  CalSession s(CalSessionCfg{});
  s.set_peer(true, true);
  std::string reply;
  CHECK(CalControl::apply("start power20@144 power20@112 power20@112", s, &reply, kDefault20, 20));
  CHECK(reply == "ok started power20@112,power20@144");
}

TEST(bad_token_is_an_error) {
  CalSession s(CalSessionCfg{});
  s.set_peer(true, true);
  std::string reply;
  CHECK(!CalControl::apply("start power20 frobnicate", s, &reply, kDefault20,
                           /*rx_width=*/20));
  CHECK(reply == "err bad token: frobnicate");
  CHECK(s.state() == CalSession::State::Idle);
}

TEST(start_reports_why_it_refused) {
  CalSession s(CalSessionCfg{});
  s.set_peer(/*linked=*/false, /*cal_capable=*/true);
  std::string reply;
  CHECK(!CalControl::apply("start", s, &reply, kDefault20, /*rx_width=*/20));
  CHECK(reply.find("err") == 0);
  CHECK(reply.find("link") != std::string::npos);
}

// --- status / render_status ---------------------------------------------

TEST(status_never_changes_state) {
  CalSession s(CalSessionCfg{});
  s.set_peer(true, true);
  std::string reply;
  CHECK(!CalControl::apply("status", s, &reply, kDefault20, /*rx_width=*/20));
  CHECK(reply.find("idle") != std::string::npos);
  CHECK(s.state() == CalSession::State::Idle);
}

TEST(status_renders_the_snapshot) {
  CalSession s(CalSessionCfg{}, nullptr, &g_picker);
  s.set_peer(true, true);
  std::string reply;
  REQUIRE(CalControl::apply("start", s, &reply, kDefault20, /*rx_width=*/20));
  reply.clear();
  CHECK(!CalControl::apply("status", s, &reply, kDefault20, /*rx_width=*/20));
  CHECK(reply.find("ok state=") == 0);
  CHECK(reply.find("sweep=1/2") != std::string::npos);
  CHECK(reply.find("kind=wall") != std::string::npos);
  CHECK(reply.find("bw=20") != std::string::npos);
  CHECK(reply.find("cell=") != std::string::npos);
}

TEST(render_status_shows_capacity_kind_during_a_capacity_sweep) {
  CalSnapshot snap;
  snap.state = CalSession::State::Sweep;
  snap.sweep_i = 2;
  snap.sweep_n = 4;
  snap.cur = {cal::Kind::Capacity, 40};
  snap.cell_i = 3;
  snap.cell_n = 8;
  CHECK(CalControl::render_status(snap) ==
        "state=sweep sweep=3/4 kind=capacity bw=40 cell=3/8");
}

TEST(render_status_carries_the_failure_reason_when_failed) {
  CalSnapshot snap;
  snap.state = CalSession::State::Failed;
  snap.sweep_i = 0;
  snap.sweep_n = 1;
  snap.cur = {cal::Kind::Wall, 20};
  snap.cell_i = 0;
  snap.cell_n = 0;
  snap.fail_reason = "calibration ack timeout";
  const std::string out = CalControl::render_status(snap);
  CHECK(out.find("state=failed") == 0);
  CHECK(out.find("fail_reason=calibration ack timeout") != std::string::npos);
}

// Revert (no mismatch field): a run silently dropping every frame of a
// sweep for a flags mismatch looks like loss on the status line.
TEST(render_status_shows_stamp_mismatches_only_when_there_are_some) {
  CalSnapshot snap;
  snap.state = CalSession::State::Sweep;
  snap.sweep_i = 0;
  snap.sweep_n = 1;
  snap.cur = {cal::Kind::Wall, 20};
  snap.cell_i = 3;
  snap.cell_n = 20;
  CHECK(CalControl::render_status(snap).find("mismatch") == std::string::npos);
  snap.stamp_mismatch = 12;
  CHECK(CalControl::render_status(snap) ==
        "state=sweep sweep=1/1 kind=wall bw=20 cell=3/20 mismatch=12");
  snap.state = CalSession::State::Failed;
  snap.fail_reason = "calibration ack timeout";
  CHECK(CalControl::render_status(snap) ==
        "state=failed sweep=1/1 kind=wall bw=20 cell=3/20 mismatch=12 "
        "fail_reason=calibration ack timeout");
}

// --- abort / loopback (unchanged behaviour) ------------------------------

TEST(abort_stops_a_running_session) {
  CalSession s(CalSessionCfg{}, nullptr, &g_picker);
  s.set_peer(true, true);
  std::string reply;
  REQUIRE(CalControl::apply("start", s, &reply, kDefault20, /*rx_width=*/20));
  CHECK(CalControl::apply("abort", s, &reply, kDefault20, /*rx_width=*/20));
  CHECK(s.state() == CalSession::State::Idle);
}

TEST(abort_while_idle_is_a_no_op_and_is_not_logged) {
  // The stderr line poll() emits on a true return is the post-mortem record
  // that a calibration happened. An operator's idle sanity-check abort must
  // not write one.
  CalSession s(CalSessionCfg{});
  s.set_peer(true, true);
  std::string reply;
  CHECK(!CalControl::apply("abort", s, &reply, kDefault20, /*rx_width=*/20));
  CHECK(s.state() == CalSession::State::Idle);
}

TEST(unknown_and_empty_commands_are_errors_not_crashes) {
  CalSession s(CalSessionCfg{});
  s.set_peer(true, true);
  std::string reply;
  CHECK(!CalControl::apply("", s, &reply, kDefault20, /*rx_width=*/20));
  CHECK(reply.find("err") == 0);
  CHECK(!CalControl::apply("frobnicate", s, &reply, kDefault20, /*rx_width=*/20));
  CHECK(reply.find("err") == 0);
}

TEST(binds_loopback_only) {
  // The GS is reachable on its LAN address; this port must never be reachable from the
  // drone or a bench laptop.
  CalControl c;
  REQUIRE(c.open(18400));
  CHECK(c.ok());
  CHECK(std::string(c.bound_address()) == "127.0.0.1");
}

MTEST_MAIN
