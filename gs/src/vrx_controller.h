#pragma once

#include <array>
#include <optional>
#include <vector>

#include "mabur/link_key.h"
#include "mabur/rc_proto.h"

#include "ladder_controller.h"
#include "op_point.h"
#include "rendezvous.h"

namespace maburgs {

struct VrxCfg {
  // Link key: tags every DISC/RCF this controller builds (link pairing).
  // config.link.key on the GS.
  mabur::LinkKey key = mabur::kDefaultLinkKey;
  uint8_t op_channel = 149;
  int feedback_ms = 100;
  int beacon_keepalive_ms = 1000;
  // Measured-loss ladder controller config (see LinkCfg::ladder_cfg,
  // config.h). Consulted every tick unless pinned.
  LadderCfg ladder;
  // Static-link pin: mcs >= 0 bypasses the adaptive controller (see
  // LinkCfg::static_mcs). overhead pair used only when pinned (from
  // LinkCfg::static_overhead_base/enh).
  int pin_mcs = -1;
  int pin_bw = 20;  // link.static_bw
  double pin_overhead_base = 0.25;
  double pin_overhead_enh = 0.25;
  // link.probe.pin_mcs: static-pin mode only -- probe a fixed MCS while
  // pinned (bench validation).
  int probe_pin_mcs = -1;
  // Rendezvous vrx_nonce: 0 = random per process (a restarted GS is a new
  // session, rendezvous.h). Only a replay pins one, so its sent frames (DISC
  // nonce, RCF tags) are reproducible -- the native/WASM parity trace.
  uint32_t rz_nonce = 0;
};

// The index of the rung with this {mcs, bw} in `ladder`, else 0: where a
// re-entry lands when the operating point it keeps is not on the ladder.
int rung_index(const LadderCfg& ladder, int mcs, int bw);

class VrxController {
 public:
  explicit VrxController(VrxCfg cfg);
  // A video body arrived: feeds the rendezvous video-silence timer only. The
  // RSSI/SNR/seq the old ScoreWindow consumed here went nowhere but the RCF
  // score/ack_seq fields, both deleted from the wire in RC_VERSION 3.
  void on_video(double now_ms);
  void on_rc_frame(const uint8_t* buf, size_t len, double now_ms);
  struct Out {
    std::vector<uint8_t> frame;
    bool is_disc;
  };
  // health: this window's measured loss (LadderController::LinkHealth, see
  // ladder_controller.h). Ignored entirely in pin mode. The ladder's own
  // internal checks (video_starved forces the failsafe rung; sample_valid
  // gates everything else) replace the old SNR-survivor-bias special case
  // here — see ladder_controller.cpp update().
  std::optional<Out> step(double now_ms, const LinkHealth& health);
  const OpPoint& cur_op() const;
  // The ladder controller itself, for the sideport link.ctl block and
  // the "ctl: rung a->b" transition line in main.cpp. Exists even in pin
  // mode (constructed unconditionally) but is never ticked/updated there.
  const LadderController& ctl() const { return ctrl_; }
  VrxState link_state() const;
  // VrxRendezvous::video_flowing: link video is flowing, not just a session.
  bool video_flowing(double now_ms) const { return rz_.video_flowing(now_ms); }
  // Low 16 bits of rcf_seq32(): the wire seq of the last built RCF.
  uint16_t rcf_seq() const;
  // The RCF tag counter: tag ctx = {vrx_nonce, vtx_nonce, seq32}. Restarts at
  // 0 on each NEW vtx_nonce adoption so the drone's fresh per-session seq
  // tracker and our ctx agree from the first RCF.
  uint32_t rcf_seq32() const { return seq32_; }
  // The probe byte the last built RCF carried (kNoProbeProfile when none).
  uint8_t probe_profile() const { return last_cmd_probe_profile_; }
  // chip_caps from the latest Telem of the current session; 0 until one
  // arrives (and again after a new session is adopted). Read for
  // CAP_CALIBRATE and CAP_MAVLINK, only once peer_caps_known(): 0 before
  // the first Telem is "not heard yet", not "no caps".
  uint16_t peer_caps() const { return peer_caps_; }
  // Whether any verified DiscAck has ever been accepted (the web GS state
  // reports it). A key_mismatch-flagged ack does not count.
  bool peer_acked() const { return peer_acked_; }
  // The drone's radio width from the latest Telem; 0 = no Telem yet.
  uint8_t peer_width() const { return peer_width_; }
  // A Telem has arrived since the last session adoption: caps/width are real
  // (0 caps before this is "not heard yet", not "none").
  bool peer_caps_known() const { return peer_caps_known_; }
  // A verified ack came from a drone at another RC_VERSION: no session.
  bool version_mismatch() const { return rz_.state() == VrxState::VERSION_MISMATCH; }
  std::optional<uint8_t> drone_rc_version() const { return rz_.drone_rc_version(); }
  // In-flight channel hop. set_hop() is carried in
  // every RCF built from now on (build_rcf() stamps r.hop_ch/r.hop_epoch);
  // restore_rung()/blank_store() forward straight to the ladder controller
  // -- restore_rung() additionally refreshes cur_op_ immediately (the same
  // way step() does after a rung change) so an RCF built in the SAME tick
  // already carries the restored profile.
  void set_hop(uint8_t hop_ch, uint8_t hop_epoch) {
    hop_ch_ = hop_ch;
    hop_epoch_ = hop_epoch;
  }
  void restore_rung(int rung, double now_ms);
  void blank_store(double until_ms) { ctrl_.blank_store(until_ms); }
  // Static-link pin, live (config.link.static_*): mcs < 0 is adaptive
  // (the ladder drives cur_op()), mcs >= 0 bypasses it with a fixed op.
  // Takes effect on the next step(), exactly as the boot-time cfg does. A
  // change from pinned back to adaptive re-enters the ladder on the rung
  // matching the pinned {mcs, bw}, else rung 0 (an Unpin ctl event, the
  // feedback stamp restarted), and cur_op() reads the ladder's op at once
  // rather than the stale pinned value.
  void set_pin(int mcs, int bw, double overhead_base, double overhead_enh, double now_ms);
  int pin_mcs() const { return cfg_.pin_mcs; }
  // Rebuild the ladder controller inside the session (config.link.ladder,
  // live): the old op rung's {mcs, bw} is looked up in the new ladder and
  // restored to that index when present, else rung 0. restore() always
  // runs (even landing on rung 0) so the re-key settle window opens for
  // every op-point change this causes, not only the ones that move rung().
  // The new controller keeps the old one's last feedback time: a swap is
  // no feedback gap for the blind-side timeout to act on.
  void replace_ladder(const LadderCfg& ladder, double now_ms);
  const LadderCfg& ladder() const { return cfg_.ladder; }
  uint8_t hop_ch() const { return hop_ch_; }
  uint8_t hop_epoch() const { return hop_epoch_; }

  // Rendezvous nonce for test construction of acceptable DiscAcks.
  uint32_t rz_nonce() const { return rz_.nonce(); }
  void set_proposal(uint8_t ch) { rz_.set_proposal(ch); }
  // Record a DISC as sent at now_ms without emitting it and return its seq:
  // lets a caller with no drone to answer (replay, tests) build an ack that
  // passes the rendezvous seq window.
  uint16_t stamp_disc(double now_ms) { return rz_.stamp(now_ms).seq; }
  // Hold the SESSION keep-alive DISC while a hop order is in flight: a
  // keep-alive takes that tick's RCF slot, and every RCF of the hop carries
  // the order. RCFs are unaffected; the keep-alive is due at once when the
  // hold lifts. BEACONING's DISCs ignore it.
  void set_keepalive_hold(bool hold) { keepalive_hold_ = hold; }
  // VTX recorder wish for every RCF (RecControl::wire()).
  void set_rec_wish(uint8_t w) { rec_wish_ = w; }
  // GS-requested IDR epoch for every RCF: maburgs sets it from its
  // RefreshRequester each tick, the web GS from its own request counter.
  void set_idr_epoch(uint8_t e) { idr_epoch_ = e; }
  // Phase lock: maburplay's sensor-rate offset (via RecControl), carried in
  // every RCF. 0 = no steering.
  void set_snr_off(int16_t mfps) { snr_off_ = mfps; }
  uint8_t proposal() const { return rz_.proposal(); }
  // Last accepted ack's agreed_channel (0 before any accept). Set BEFORE
  // peer_caps_ so a caller reading both on one tick sees a consistent pair.
  uint8_t agreed_channel() const { return agreed_channel_; }
  // Channel-move edge (link pairing): the drone
  // retunes only after our first RCF verifies, so the GS follows it to the
  // agreed channel when a Telem says LINKED (the drone emits one on the old
  // channel as it promotes the session) or, if that frame is lost, after
  // kMoveAfterRcfs RCFs sent under this session. Armed on each NEW
  // vtx_nonce adoption; true once per arming, cleared by read.
  static constexpr int kMoveAfterRcfs = 5;
  bool take_move_edge() { const bool e = move_edge_; move_edge_ = false; return e; }
  // Test seam: arm the move edge as a LINKED Telem under a fresh vtx_nonce would.
  void test_set_move_edge() { move_edge_ = true; }
  // A parsed Telem (our version: only those parse): records its caps and
  // width, reports the drone alive to the rendezvous (an other-version ack
  // is ignored while it is), and fires the move edge on state LINKED
  // (2 == RcAgent::State::LINKED).
  void note_telem(const mabur::rc::Telem& t, double now_ms);
  // The drone answers our DISCs with key_mismatch: our key differs from its.
  // No RCF/cal goes out; beacons continue.
  bool key_mismatch() const { return rz_.state() == VrxState::KEY_MISMATCH; }
  // Tag ctx for cal frames: {vrx, vtx, 0}; vtx 0 when no session is held.
  mabur::rc::TagCtx session_ctx() const {
    return mabur::rc::TagCtx{rz_.nonce(), rz_.vtx_nonce().value_or(0), 0};
  }

 private:
  mabur::rc::Rcf build_rcf();
  void note_cmd(const mabur::rc::Rcf& r);
  // cur_op_ = OpPoint derived from ctrl_.op() -- the one place step() and
  // restore_rung() both refresh the cached operating point from the ladder.
  void sync_op_();

  VrxCfg cfg_;
  LadderController ctrl_;
  VrxRendezvous rz_;
  double last_fb_ms_ = -1e18;
  double last_keepalive_ms_ = -1e18;
  uint32_t seq32_ = 0;
  OpPoint cur_op_;
  uint16_t peer_caps_ = 0;
  uint8_t peer_width_ = 0;
  bool peer_caps_known_ = false;
  bool peer_acked_ = false;
  bool keepalive_hold_ = false;
  uint8_t last_cmd_probe_profile_ = mabur::rc::kNoProbeProfile;
  uint8_t agreed_channel_ = 0;
  bool move_armed_ = false;
  bool move_edge_ = false;
  int rcfs_since_adopt_ = 0;
  uint8_t hop_ch_ = 0;
  uint8_t hop_epoch_ = 0;
  uint8_t rec_wish_ = 0;
  uint8_t idr_epoch_ = 0;
  int16_t snr_off_ = 0;
};

}  // namespace maburgs
