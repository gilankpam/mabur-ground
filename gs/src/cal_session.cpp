#include "cal_session.h"

#include <algorithm>
#include <bitset>
#include <cmath>
#include <string>
#include <tuple>
#include <variant>

#include "cal_log.h"
#include "cal_picker.h"
#include "cal_plan.h"

namespace maburgs {
namespace cal = mabur::cal;

namespace {

// How often an unanswered SweepCmd or ApplyCmd is offered again. The uplink
// loses a large share of frames, so one send is not enough; both are
// idempotent on the drone by (nonce, seq), so a repeat costs nothing there.
constexpr uint32_t kResendMs = 200;

int median(std::vector<int> v) {
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

const char* ack_word(cal::AckResult r) {
  switch (r) {
    case cal::AckResult::Accepted: return "accepted";
    case cal::AckResult::AnchorUnreadable: return "anchor_unreadable";
    case cal::AckResult::Busy: return "busy";
    case cal::AckResult::Unsupported: return "unsupported";
    case cal::AckResult::BadChannel: return "channel not in the drone's set";
    case cal::AckResult::BadWidth: return "width not flown by the drone";
  }
  return "unknown";
}

const char* wall_word(cal::WallResult r) {
  switch (r) {
    case cal::WallResult::Ok: return "ok";
    case cal::WallResult::Unparked: return "unparked";
    case cal::WallResult::TooFewPoints: return "too_few_points";
    case cal::WallResult::FloorTooHigh: return "floor_too_high";
    case cal::WallResult::NotSaturated: return "not_saturated";
    case cal::WallResult::Deadline: return "deadline";
    case cal::WallResult::DeviceError: return "device_error";
    case cal::WallResult::Aborted: return "aborted";
  }
  return "unknown";
}

// Wall before capacity within a width, so a capacity sweep flies at the
// walls its own width just stored.
int kind_order(cal::Kind k) { return k == cal::Kind::Wall ? 0 : 1; }

}  // namespace

bool CalSession::start(uint32_t nonce, std::vector<cal::SweepSpec> sweeps, uint64_t now_ms,
                       std::string* err) {
  now_ms = advance(now_ms);
  // Picking is a run too: a second start would fight the first one's pick.
  if (running() || state_ == State::Picking) {
    if (err) *err = "a calibration session is already running";
    return false;
  }
  if (!linked_) {
    if (err) *err = "refused: link is down";
    return false;
  }
  if (!cal_capable_) {
    if (err) *err = "refused: waiting for drone telemetry";
    return false;
  }
  if (!*cal_capable_) {
    if (err) *err = "refused: peer does not advertise CAP_CALIBRATE";
    return false;
  }
  if (sweeps.empty()) {
    if (err) *err = "refused: no sweeps";
    return false;
  }
  for (const auto& s : sweeps) {
    if (s.kind != cal::Kind::Wall && s.kind != cal::Kind::Capacity) {
      if (err) *err = "refused: a sweep is wall or capacity";
      return false;
    }
    if (s.bw != 20 && s.bw != 40) {
      if (err) *err = "refused: a sweep's width is 20 or 40";
      return false;
    }
  }
  auto key = [](const cal::SweepSpec& s) {
    return std::make_tuple(s.bw, kind_order(s.kind), s.channel);
  };
  std::sort(sweeps.begin(), sweeps.end(),
            [&](const cal::SweepSpec& a, const cal::SweepSpec& b) { return key(a) < key(b); });
  sweeps.erase(std::unique(sweeps.begin(), sweeps.end(),
                           [&](const cal::SweepSpec& a, const cal::SweepSpec& b) {
                             return key(a) == key(b);
                           }),
               sweeps.end());

  nonce_ = nonce;
  sweeps_ = std::move(sweeps);
  sweep_i_ = 0;
  fail_reason_.clear();
  run_logged_ = false;
  reached_drone_ = false;
  stamp_mismatch_ = 0;
  cell_i_ = 0;
  ack_flags_ = 0;
  done_.reset();
  width_ = {};
  walls_.clear();
  skipped_.clear();
  reloc_ordered_ = false;
  // pending_abort_ is deliberately kept: copies of the previous run's
  // Abort that have not gone out yet still must.

  channel_ = picker_ ? picker_->op() : 0;
  if (picker_ == nullptr || picker_->mode() != CalPicker::Mode::Auto) {
    pick_ = (picker_ && picker_->mode() == CalPicker::Mode::OneCard) ? "onecard" : "pinned";
    pick_note_ = pick_;
    begin_sweep(0, now_ms);
    return true;
  }
  pick_ = "auto";
  pick_note_ = "measuring";
  pick_start_ms_ = now_ms;
  state_ = State::Picking;
  picker_->begin_measure(now_ms);
  return true;
}

void CalSession::on_mav(const cal::CalMsg& m, uint64_t now_ms) {
  now_ms = advance(now_ms);
  step(now_ms);
  if (const auto* a = std::get_if<cal::SweepAck>(&m)) {
    if (state_ != State::AwaitAck || a->nonce != nonce_ || a->seq != cmd_.seq) return;
    if (a->result == cal::AckResult::Unsupported && cmd_.kind == cal::Kind::Wall) {
      // A chip that cannot measure itself: the rest of the run still
      // calibrates what it can.
      log_run(a->flags);
      const std::string tok = cal_sweep_token(sweeps_[sweep_i_]);
      skipped_.push_back(tok);
      if (log_) log_->skipped(cmd_.seq, tok, ack_word(a->result));
      next_sweep(now_ms);
      return;
    }
    if (a->result != cal::AckResult::Accepted) {
      fail(std::string("sweep refused by the drone: ") + ack_word(a->result));
      return;
    }
    on_accepted(a->flags, a->start_in_ms, now_ms);
  } else if (const auto* d = std::get_if<cal::SweepDone>(&m)) {
    if (state_ != State::Sweep && state_ != State::AwaitDone) return;
    if (d->nonce != nonce_ || d->seq != cmd_.seq) return;
    done_ = *d;
    // A capacity sweep's tail may still be airing; step() closes it once
    // the tail has gone quiet. A wall report closes its sweep at once.
    if (tail_quiet(now_ms)) finish_sweep(now_ms);
  } else if (const auto* ap = std::get_if<cal::Applied>(&m)) {
    // Matched on the sweep's seq as well as the width: a run may apply
    // twice at one width, and the drone keeps answering repeats of the
    // first from its cache.
    if (state_ != State::Apply || ap->nonce != nonce_ || ap->seq != cmd_.seq ||
        ap->bw != cmd_.bw)
      return;
    on_applied(*ap, now_ms);
  }
}

std::optional<cal::CalMsg> CalSession::due_mav(uint64_t now_ms) {
  now_ms = advance(now_ms);
  // Capacity frames handled since the last tick: the sweep was not quiet up
  // to this tick. The tally's own clock is not enough -- it is built from
  // the frames' receive stamps, and a core-loop pass that runs long puts
  // its tick far past them while the rest of the drain still waits in the
  // receive queue. due_mav is the caller's once-per-pass tick, after the
  // pass has handled what it drained; on_mav and on_cal_frame carry
  // receive stamps and never mark the sweep quiet.
  if (cap_fresh_) {
    last_cap_seen_ms_ = std::max(last_cap_seen_ms_, now_ms);
    cap_fresh_ = false;
  }
  step(now_ms);
  if (pending_abort_ && now_ms >= next_abort_ms_) {
    const cal::Abort a = *pending_abort_;
    if (--aborts_left_ == 0) pending_abort_.reset();
    next_abort_ms_ = now_ms + kResendMs;
    return cal::CalMsg{a};
  }
  if (state_ != State::AwaitAck && state_ != State::Apply) return std::nullopt;
  if (sent_once_ && now_ms - last_sent_ms_ < kResendMs) return std::nullopt;
  sent_once_ = true;
  last_sent_ms_ = now_ms;
  if (state_ == State::AwaitAck) {
    reached_drone_ = true;
    return cal::CalMsg{cmd_};
  }
  return cal::CalMsg{apply_};
}

void CalSession::on_cal_frame(int card, const cal::CalFrameInfo& f, int rssi_dbm, bool crc_ok,
                              size_t air_bytes, uint64_t now_ms) {
  // The frame's own receive stamp, before advance() folds it into the
  // session clock: a capacity point is timed by when its frames arrived,
  // not by when the core loop got round to them.
  const uint64_t rx_ms = now_ms;
  now_ms = advance(now_ms);
  step(now_ms);
  if (card < 0 || card > 1 || f.rate > 7) return;
  if (state_ == State::AwaitAck) {
    // The ack rides a lossy uplink; a frame of the commanded sweep proves
    // what it would have -- the drone accepted this sweep and is airing it
    // -- and its stamp carries the flags the ack would have announced.
    // Without this the GS would keep sending SweepCmd repeats into the
    // live sweep and drop every frame of it. Only a CRC-clean frame: a
    // corrupted one still parses with a flipped LDPC/STBC bit, and the
    // flags it would adopt filter every frame of the sweep.
    if (f.kind != cmd_.kind || f.bw != cmd_.bw) {
      ++stamp_mismatch_;
      return;
    }
    if (!crc_ok) return;
    on_accepted(f.flags, 0, now_ms);
  }
  if (state_ == State::Sweep || state_ == State::AwaitDone) {
    if (f.kind != cmd_.kind || f.bw != cmd_.bw || f.flags != ack_flags_) {
      ++stamp_mismatch_;
      return;
    }
    tally_capacity(card, f, rssi_dbm, crc_ok, air_bytes, rx_ms);
  }
}

bool CalSession::radio_silent(uint64_t now_ms) const {
  now_ms = std::max(now_ms, now_);
  if (state_ == State::Sweep || state_ == State::AwaitDone) return now_ms < window_end_ms_;
  return false;
}

bool CalSession::running() const { return is_running(state_); }

bool CalSession::is_running(State st) {
  switch (st) {
    case State::AwaitAck:
    case State::Sweep:
    case State::AwaitDone:
    case State::Analyze:
    case State::Apply:
      return true;
    default:
      return false;
  }
}

void CalSession::abort(const char* why) {
  if (running() || state_ == State::Picking) {
    if (picker_) picker_->cancel();
    // Only a run that offered the drone a SweepCmd can have opened one
    // there; the drone closes it (restoring its power table) at once
    // rather than at its own await timeout.
    if (reached_drone_) queue_abort();
    fail_reason_ = why ? why : "aborted";
    if (log_) log_->flush();
  }
  state_ = State::Idle;
}

void CalSession::set_peer(bool linked, std::optional<bool> cal_capable, mabur::rc::TagCtx ctx) {
  linked_ = linked;
  cal_capable_ = cal_capable;
  peer_ctx_ = ctx;
}

CalSnapshot CalSession::snapshot() const {
  CalSnapshot s;
  s.state = state_;
  s.sweep_i = sweep_i_;
  s.sweep_n = sweeps_.size();
  s.cell_i = cell_i_;
  if (!sweeps_.empty()) {
    s.cur = sweeps_[sweep_i_];
    if (cmd_.kind == cal::Kind::Capacity) {
      s.cell_n = static_cast<uint32_t>(std::bitset<8>(cmd_.mcs_mask).count());
    } else {
      s.cell_n = kWallDeadlineMs / 1000;
      s.cell_i = (state_ == State::Sweep || state_ == State::AwaitDone)
                     ? static_cast<uint32_t>((now_ - accepted_ms_) / 1000)
                     : 0;
    }
  }
  s.channel = channel_;
  s.pick_note = pick_note_;
  s.flags = ack_flags_;
  s.stamp_mismatch = stamp_mismatch_;
  s.width = width_;
  s.walls = walls_;
  s.skipped = skipped_;
  s.fail_reason = fail_reason_;
  return s;
}

// --- private ---------------------------------------------------------

void CalSession::step(uint64_t now_ms) {
  switch (state_) {
    case State::Picking:
      step_picking(now_ms);
      break;
    case State::AwaitAck:
      if (now_ms - phase_start_ms_ >= cfg_.ack_timeout_ms) fail("calibration ack timeout");
      break;
    case State::Sweep:
    case State::AwaitDone:
      // The drone's SweepDone normally ends this early (once a capacity
      // sweep's tail has gone quiet); its loss costs at most phase_slack_ms
      // of a capacity sweep, and fails a wall sweep at the window's close.
      if (done_ && tail_quiet(now_ms)) {
        finish_sweep(now_ms);
        break;
      }
      if (state_ == State::Sweep) {
        if (now_ms < sweep_end_ms_) break;
        state_ = State::AwaitDone;
      }
      if (now_ms >= window_end_ms_) finish_sweep(now_ms);
      break;
    case State::Apply:
      if (now_ms - phase_start_ms_ >= cfg_.apply_timeout_ms) fail("calibration apply timeout");
      break;
    default:
      break;
  }
}

void CalSession::step_picking(uint64_t now_ms) {
  const std::string op = std::to_string(picker_->op());
  if (now_ms - pick_start_ms_ >= cfg_.pick_timeout_ms) {
    picker_->cancel();
    pick_note_ = "stayed on " + op +
                 (reloc_ordered_ ? ", relocate unconfirmed" : ", measure timed out");
    finish_pick("stayed", now_ms);
    return;
  }
  if (!reloc_ordered_) {
    const auto best = picker_->measured_best();
    if (!best) return;
    if (*best == picker_->op()) {
      pick_note_ = "auto: " + op + " is the best member";
      finish_pick("auto", now_ms);
      return;
    }
    if (!picker_->order_relocate(*best)) {
      picker_->cancel();
      pick_note_ = "stayed on " + op + ", relocate refused";
      finish_pick("stayed", now_ms);
      return;
    }
    reloc_ordered_ = true;
    reloc_to_ = *best;
    pick_note_ = "relocating " + op + " -> " + std::to_string(*best);
    return;
  }
  const auto settled = picker_->relocate_settled();
  if (!settled) return;
  if (*settled) {
    pick_note_ = "auto: moved to " + std::to_string(reloc_to_);
    finish_pick("auto", now_ms);
    channel_ = reloc_to_;
    return;
  }
  picker_->cancel();
  pick_note_ = "stayed on " + op + ", relocate unconfirmed";
  finish_pick("stayed", now_ms);
}

void CalSession::finish_pick(const char* pick, uint64_t now_ms) {
  pick_ = pick;
  channel_ = picker_->op();
  begin_sweep(0, now_ms);
}

void CalSession::begin_sweep(size_t i, uint64_t now_ms) {
  sweep_i_ = i;
  const cal::SweepSpec& sp = sweeps_[i];
  const uint8_t seq = static_cast<uint8_t>(i);
  if (sp.kind == cal::Kind::Wall) {
    // A wall sweep naming no channel measures the one the link is on; the
    // pair never moves for a named one.
    uint8_t ch = sp.channel;
    if (ch == 0) ch = picker_ ? picker_->op() : channel_;
    if (ch == 0) {
      fail("no link channel known");
      return;
    }
    cmd_ = make_wall_sweep(nonce_, seq, sp.bw, ch);
  } else {
    cmd_ = make_capacity_sweep(nonce_, seq, sp.bw);
  }
  // Frozen when the run first commands the drone: the drone latches the
  // pair its first SweepCmd arrived under and checks every later cal
  // message of the run against it, while the GS keeps beaconing between
  // sweeps and may adopt a fresh pair meanwhile.
  if (i == 0) tag_ctx_ = peer_ctx_;

  cap_ = {};
  cap_rssi_ = {};
  last_cap_seen_ms_ = 0;
  cap_fresh_ = false;
  done_.reset();
  cell_i_ = 0;
  phase_start_ms_ = now_ms;
  sent_once_ = false;
  state_ = State::AwaitAck;
}

void CalSession::log_run(uint8_t flags) {
  if (picker_) channel_ = picker_->op();
  if (run_logged_) return;
  // The GS has no LDPC setting of its own: the drone's stamp flags are the
  // truth about what this run flies.
  if (log_) log_->run(nonce_, cfg_.margin_db, channel_, pick_, (flags & cal::kFlagLdpc) != 0);
  run_logged_ = true;
}

void CalSession::on_accepted(uint8_t flags, uint16_t start_in_ms, uint64_t now_ms) {
  ack_flags_ = flags;
  log_run(flags);
  if (log_)
    log_->sweep(cmd_.seq, cmd_.kind, cmd_.bw,
                cmd_.kind == cal::Kind::Capacity ? cmd_.dwell_ms : cmd_.channel);
  accepted_ms_ = now_ms;
  sweep_end_ms_ = now_ms + start_in_ms + sweep_duration_ms(cmd_);
  window_end_ms_ = sweep_end_ms_ + cfg_.phase_slack_ms;
  state_ = State::Sweep;
}

void CalSession::tally_capacity(int card, const cal::CalFrameInfo& f, int rssi_dbm, bool crc_ok,
                                size_t air_bytes, uint64_t rx_ms) {
  if (!(cmd_.mcs_mask & (1u << f.rate))) return;
  cell_i_ = static_cast<uint32_t>(
                std::bitset<8>(cmd_.mcs_mask & ((1u << f.rate) - 1u)).count()) +
            1;
  // Any frame of the sweep, clean or not, says the air is still busy: the
  // sweep is not quiet until the next due_mav tick, and quiet from there.
  last_cap_seen_ms_ = std::max(last_cap_seen_ms_, now_);
  cap_fresh_ = true;
  if (!crc_ok) return;  // delivered air bits count clean frames only
  CapCell& c = cap_[f.rate];
  const size_t k = static_cast<size_t>(card);
  const uint64_t bits = static_cast<uint64_t>(air_bytes) * 8u;
  // One card's stamps are monotonic (one receive thread stamps them in
  // arrival order); min/max keeps the span right if a caller interleaves.
  if (c.frames[k] == 0 || rx_ms < c.first_ms[k]) {
    c.first_ms[k] = rx_ms;
    c.first_bits[k] = bits;
  }
  if (c.frames[k] == 0 || rx_ms > c.last_ms[k]) c.last_ms[k] = rx_ms;
  ++c.frames[k];
  c.air_bits[k] += bits;
  cap_rssi_[f.rate][k].push_back(rssi_dbm);
}

bool CalSession::tail_quiet(uint64_t now_ms) const {
  if (cmd_.kind != cal::Kind::Capacity) return true;
  return !cap_fresh_ && now_ms >= last_cap_seen_ms_ + cfg_.tail_quiet_ms;
}

void CalSession::finish_sweep(uint64_t now_ms) {
  if (cmd_.kind == cal::Kind::Wall) {
    finish_wall(now_ms);
    return;
  }
  state_ = State::Analyze;
  if (analyze_capacity_sweep())
    begin_apply(now_ms);
  else
    next_sweep(now_ms);
}

void CalSession::finish_wall(uint64_t now_ms) {
  // The drone measures, stores and flies the walls itself: there is
  // nothing to analyse or apply, only the report to record.
  if (!done_) {
    fail("no wall report from the drone");
    return;
  }
  if (done_->wall_result != cal::WallResult::Ok) {
    fail(std::string("wall measurement failed: ") + wall_word(done_->wall_result));
    return;
  }
  WallReport w;
  w.channel = done_->channel;
  w.bw = done_->bw;
  for (size_t r = 0; r < 8; ++r) w.walls[r] = done_->walls[r];
  w.floor_evm_x10 = done_->floor_evm_x10;
  w.anchor = done_->anchor;
  walls_.push_back(w);
  if (log_) log_->wall(w);
  next_sweep(now_ms);
}

bool CalSession::analyze_capacity_sweep() {
  WidthResult& w = width_result();
  apply_ = cal::ApplyCmd{};
  apply_.nonce = nonce_;
  apply_.seq = cmd_.seq;
  apply_.bw = cmd_.bw;
  if (!done_) {
    // Without the SweepDone neither offered nor blocked_pct is known, so a
    // tx_bound point cannot be told apart from a real one -- and a
    // tx_bound point must never be written. Nothing is.
    w.eff_unknown = true;
    w.have_eff = false;
    w.cap = {};
    w.eff_x1000 = {};
    return false;
  }
  bool any = false;
  for (size_t r = 0; r < 8; ++r) {
    if (!(cmd_.mcs_mask & (1u << r))) continue;
    CapCell c = cap_[r];
    c.offered = done_->offered[r];
    c.blocked_pct = done_->blocked_pct[r];
    for (size_t k = 0; k < 2; ++k) {
      if (!cap_rssi_[r][k].empty()) {
        c.rssi_dbm[k] = median(cap_rssi_[r][k]);
        c.have_rssi[k] = true;
      }
    }
    const CapResult res =
        analyze_capacity(c, static_cast<uint8_t>(r), cmd_.bw, cmd_.dwell_ms, cfg_.th);
    w.cap[r] = res;
    // A tx_bound point measured the drone's CPU/USB, and a lost one the
    // loss, not the air: 0 leaves the config's value alone.
    const uint16_t e = (res.flags & (kCalTxBound | kCalLost))
                           ? 0
                           : static_cast<uint16_t>(std::lround(res.eff * 1000.0));
    apply_.eff_x1000[r] = e;
    if (e) any = true;
    if (log_) log_->cap(static_cast<uint8_t>(r), c, res);
  }
  w.eff_x1000 = apply_.eff_x1000;
  w.have_eff = true;
  w.eff_unknown = false;
  return any;
}

void CalSession::begin_apply(uint64_t now_ms) {
  phase_start_ms_ = now_ms;
  sent_once_ = false;
  state_ = State::Apply;
}

void CalSession::on_applied(const cal::Applied& a, uint64_t now_ms) {
  WidthResult& w = width_result();
  if (log_) log_->applied(a.bw, a.ok, a.reason);
  w.applied = a.ok;
  w.apply_reason = a.reason;
  if (!a.ok) {
    fail("apply refused by the drone: " + a.reason);
    return;
  }
  next_sweep(now_ms);
}

void CalSession::next_sweep(uint64_t now_ms) {
  if (sweep_i_ + 1 < sweeps_.size()) {
    begin_sweep(sweep_i_ + 1, now_ms);
    return;
  }
  state_ = State::Done;
  // Closes the drone's run now -- restoring its operating power and video
  // -- instead of after its await window; the drone's close keeps the
  // config the applies wrote.
  queue_abort();
  // maburcal reads cal.log the moment it sees a terminal state; the
  // writer's own 1 Hz flush would race it.
  if (log_) log_->flush();
}

void CalSession::fail(const std::string& why) {
  if (state_ == State::Picking && picker_) picker_->cancel();
  // A run the drone may have opened is closed there at once, restoring
  // its power table, rather than at its own await timeout.
  if (reached_drone_) queue_abort();
  fail_reason_ = why;
  state_ = State::Failed;
  if (log_) log_->flush();
}

void CalSession::queue_abort() {
  pending_abort_ = cal::Abort{nonce_};
  aborts_left_ = kAbortCopies;
  next_abort_ms_ = 0;  // the first copy goes out at the next due_mav()
}

WidthResult& CalSession::width_result() { return width_[cmd_.bw == 40 ? 1 : 0]; }

}  // namespace maburgs
