#include "arm_record.h"

namespace maburplay {

ArmAction ArmRecord::step(bool enabled, int stop_delay_s, ArmState s, bool rec_on,
                          uint64_t now_ms) {
  if (!enabled) {
    last_ = ArmState::Unknown;
    stop_pending_ = false;
    return ArmAction::None;
  }
  if (s == ArmState::Unknown) return ArmAction::None;
  const ArmState was = last_;
  last_ = s;
  if (s == ArmState::Armed) {
    if (was == ArmState::Armed) return ArmAction::None;
    stop_pending_ = false;
    return rec_on ? ArmAction::None : ArmAction::Start;
  }
  // Disarmed.
  if (was == ArmState::Armed) {
    stop_pending_ = true;
    stop_at_ms_ = now_ms + static_cast<uint64_t>(stop_delay_s > 0 ? stop_delay_s : 0) * 1000;
  }
  if (!stop_pending_ || now_ms < stop_at_ms_) return ArmAction::None;
  stop_pending_ = false;
  return rec_on ? ArmAction::Stop : ArmAction::None;
}

}  // namespace maburplay
