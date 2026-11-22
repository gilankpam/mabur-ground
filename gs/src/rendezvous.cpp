#include "rendezvous.h"
#include <random>

namespace maburgs {
namespace {
uint32_t random_nonce() {
  std::random_device rd;
  uint32_t n = 0;
  do { n = rd(); } while (n == 0);
  return n;
}
}  // namespace

VrxRendezvous::VrxRendezvous(VrxRzConfig cfg)
    : cfg_(cfg), nonce_(cfg.nonce ? cfg.nonce : random_nonce()), proposal_(cfg.op_channel) {}

void VrxRendezvous::feed_video(double now_ms) {
  last_video_ms_ = now_ms;
  if (now_ms - last_real_video_ms_ > kVideoGapMs) video_run_start_ms_ = now_ms;
  last_real_video_ms_ = now_ms;
  // Video proves the drone is there, not that we hold its session nonce.
  if (state_ == VrxState::BEACONING && vtx_nonce_) state_ = VrxState::SESSION;
}

VrxAction VrxRendezvous::tick(double now_ms) {
  if (state_ == VrxState::SESSION) {
    if (now_ms - last_video_ms_ > cfg_.link_lost_ms)
      state_ = VrxState::BEACONING;
    else
      return VrxAction::TxFeedback;
  }
  const int period =
      state_ == VrxState::VERSION_MISMATCH ? cfg_.keepalive_ms : cfg_.beacon_period_ms;
  if (now_ms - last_beacon_ms_ >= period) {
    last_beacon_ms_ = now_ms;
    return VrxAction::Beacon;
  }
  return VrxAction::Idle;
}

mabur::rc::Disc VrxRendezvous::beacon(double now_ms) {
  seq_ = static_cast<uint16_t>(seq_ + 1);
  sent_[seq_ % sent_.size()] = {seq_, now_ms};
  if (first_flagged_ms_) ++beacons_since_flagged_;
  mabur::rc::Disc d;
  d.vrx_nonce = nonce_;
  d.seq = seq_;
  return d;
}

bool VrxRendezvous::feed_disc_ack(const mabur::rc::DiscAck& ack, bool tag_ok, double now_ms,
                                  bool* adopted_new) {
  if (adopted_new) *adopted_new = false;
  if (ack.vrx_nonce != nonce_) return false;
  if (ack.flags & mabur::rc::kAckKeyMismatch) {
    if (!first_flagged_ms_) {
      first_flagged_ms_ = now_ms;
      beacons_since_flagged_ = 0;
    }
    // Both: the run is old enough AND enough of our DISCs went unanswered
    // by an unflagged ack (rendezvous.h). vtx_nonce_ is kept: KEY_MISMATCH
    // sends no RCFs.
    if (now_ms - *first_flagged_ms_ >= kKeyMismatchMs &&
        beacons_since_flagged_ >= kKeyMismatchBeacons) {
      state_ = VrxState::KEY_MISMATCH;
      drone_version_.reset();
    }
    return true;
  }
  // Unflagged: it must carry a verified tag and answer a recent DISC of
  // ours, or a captured ack could be replayed to roll the session back.
  const Sent& s = sent_[ack.seq % sent_.size()];
  if (!tag_ok || s.seq != ack.seq || now_ms - s.ms > kAckWindowMs) return false;
  first_flagged_ms_.reset();
  beacons_since_flagged_ = 0;
  if (ack.version != mabur::rc::RC_VERSION) {
    // vtx_nonce_ stays held but unused; tick() sends no RCFs here.
    state_ = VrxState::VERSION_MISMATCH;
    drone_version_ = ack.version;
    return true;
  }
  drone_version_.reset();
  if (!vtx_nonce_ || *vtx_nonce_ != ack.vtx_nonce) {
    vtx_nonce_ = ack.vtx_nonce;
    if (adopted_new) *adopted_new = true;
  }
  state_ = VrxState::SESSION;
  last_video_ms_ = now_ms;
  return true;
}
}  // namespace maburgs
