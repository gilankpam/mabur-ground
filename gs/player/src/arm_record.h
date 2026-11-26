// Record on arm: turns the drone's arm state (sideport drone.armed) into
// recording starts and stops for the DVR. Pure: one step() per player loop
// iteration, no clock or I/O of its own, so the rules are host-tested.
//
// Edges drive it, not levels: an arm starts a recording if none runs, a
// disarm starts the stop delay, and in between the record button is free
// -- a recording stopped by hand while armed stays stopped until the next
// arm. "Unknown" (no telemetry, a quiet sideport, an FC that stopped
// answering) never acts: a link drop mid-flight must not stop a recording.
#pragma once

#include <cstdint>

namespace maburplay {

enum class ArmState : uint8_t { Unknown, Disarmed, Armed };
enum class ArmAction : uint8_t { None, Start, Stop };

class ArmRecord {
 public:
  // enabled = dvr.record_on_arm; stop_delay_s = dvr.disarm_stop_s (0 = stop
  // on the disarm itself); rec_on = a recording runs right now.
  //
  // Disabled forgets the last known state, so turning the feature on treats
  // the next known state as the first one -- armed then starts a recording,
  // which is also what a GS that boots mid-flight gets. The first known
  // state being Disarmed is no edge. A stop is due only on a known Disarmed
  // sample at or past the deadline.
  ArmAction step(bool enabled, int stop_delay_s, ArmState s, bool rec_on, uint64_t now_ms);

  // The record button was pressed, either way: a pending stop is dropped,
  // so a recording started by hand just after a disarm is not cut short.
  void on_manual() { stop_pending_ = false; }

  bool stop_pending() const { return stop_pending_; }

 private:
  ArmState last_ = ArmState::Unknown;  // last KNOWN state; Unknown = none yet
  bool stop_pending_ = false;
  uint64_t stop_at_ms_ = 0;
};

}  // namespace maburplay
