// Record on arm: the edge tracker that turns the drone's arm state into
// recording starts and stops. No player, no clock -- every case drives
// step() by hand.
#include "mtest.h"
#include "arm_record.h"

using namespace maburplay;
using A = ArmAction;
using S = ArmState;

TEST(disabled_never_acts) {
  ArmRecord r;
  CHECK(r.step(false, 5, S::Armed, false, 0) == A::None);
  CHECK(r.step(false, 5, S::Disarmed, true, 100) == A::None);
  CHECK(r.step(false, 5, S::Armed, false, 200) == A::None);
}

TEST(first_known_armed_starts) {
  ArmRecord r;
  CHECK(r.step(true, 5, S::Unknown, false, 0) == A::None);
  CHECK(r.step(true, 5, S::Armed, false, 100) == A::Start);
  CHECK(r.step(true, 5, S::Armed, true, 200) == A::None);
}

TEST(first_known_disarmed_does_nothing) {
  ArmRecord r;
  CHECK(r.step(true, 5, S::Disarmed, false, 0) == A::None);
  CHECK(!r.stop_pending());
  CHECK(r.step(true, 5, S::Disarmed, false, 60'000) == A::None);
}

TEST(disarm_stops_after_delay) {
  ArmRecord r;
  CHECK(r.step(true, 5, S::Armed, false, 0) == A::Start);
  CHECK(r.step(true, 5, S::Disarmed, true, 10'000) == A::None);
  CHECK(r.stop_pending());
  CHECK(r.step(true, 5, S::Disarmed, true, 14'999) == A::None);
  CHECK(r.step(true, 5, S::Disarmed, true, 15'000) == A::Stop);
  CHECK(!r.stop_pending());
  CHECK(r.step(true, 5, S::Disarmed, false, 16'000) == A::None);
}

TEST(zero_delay_stops_on_the_edge) {
  ArmRecord r;
  CHECK(r.step(true, 0, S::Armed, false, 0) == A::Start);
  CHECK(r.step(true, 0, S::Disarmed, true, 1'000) == A::Stop);
}

TEST(rearm_within_delay_keeps_one_recording) {
  ArmRecord r;
  CHECK(r.step(true, 5, S::Armed, false, 0) == A::Start);
  CHECK(r.step(true, 5, S::Disarmed, true, 10'000) == A::None);
  CHECK(r.step(true, 5, S::Armed, true, 12'000) == A::None);
  CHECK(!r.stop_pending());
  CHECK(r.step(true, 5, S::Armed, true, 20'000) == A::None);
}

TEST(unknown_changes_nothing) {
  ArmRecord r;
  CHECK(r.step(true, 5, S::Armed, false, 0) == A::Start);
  for (uint64_t t = 0; t <= 600'000; t += 1'000)
    CHECK(r.step(true, 5, S::Unknown, true, t) == A::None);
  CHECK(r.step(true, 5, S::Armed, true, 601'000) == A::None);
  // A disarm edge, then silence past the deadline: still nothing until a
  // known Disarmed sample is in hand again.
  CHECK(r.step(true, 5, S::Disarmed, true, 700'000) == A::None);
  CHECK(r.step(true, 5, S::Unknown, true, 710'000) == A::None);
  CHECK(r.step(true, 5, S::Disarmed, true, 711'000) == A::Stop);
}

TEST(manual_stop_while_armed_is_respected) {
  ArmRecord r;
  CHECK(r.step(true, 5, S::Armed, false, 0) == A::Start);
  for (uint64_t t = 1'000; t <= 60'000; t += 1'000)
    CHECK(r.step(true, 5, S::Armed, false, t) == A::None);
}

TEST(manual_press_cancels_pending_stop) {
  ArmRecord r;
  CHECK(r.step(true, 5, S::Armed, false, 0) == A::Start);
  CHECK(r.step(true, 5, S::Disarmed, true, 10'000) == A::None);
  r.on_manual();
  CHECK(!r.stop_pending());
  CHECK(r.step(true, 5, S::Disarmed, true, 20'000) == A::None);
}

TEST(enabling_while_armed_starts) {
  ArmRecord r;
  CHECK(r.step(false, 5, S::Armed, false, 0) == A::None);
  CHECK(r.step(true, 5, S::Armed, false, 100) == A::Start);
}

TEST(disabling_drops_a_pending_stop) {
  ArmRecord r;
  CHECK(r.step(true, 5, S::Armed, false, 0) == A::Start);
  CHECK(r.step(true, 5, S::Disarmed, true, 1'000) == A::None);
  CHECK(r.step(false, 5, S::Disarmed, true, 2'000) == A::None);
  CHECK(!r.stop_pending());
  CHECK(r.step(true, 5, S::Disarmed, true, 10'000) == A::None);
}

TEST(no_start_when_already_recording) {
  ArmRecord r;
  CHECK(r.step(true, 5, S::Armed, true, 0) == A::None);
}

MTEST_MAIN
