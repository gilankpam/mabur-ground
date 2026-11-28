#pragma once
// The calibration session brain on the GS. A run is an ordered list of
// sweeps the GS composed (wall and/or capacity, at 20 and/or 40 MHz); per
// sweep the session commands the drone, waits for the ack and keeps the GS
// radio silent through the sweep's window. A wall sweep is measured by the
// drone itself and ends with the walls it reports; a capacity sweep's
// stamped frames are tallied, analysed when the drone says it is done (or
// the window closes) and the efficiency sent back for the drone to apply.
//
// It talks to the drone only through plain structs (mabur::cal::CalMsg):
// the caller encodes due_mav()'s output onto the MAVLink tunnel's cal lane
// and decodes the drone's replies into on_mav(). It is also the sole place
// that decides whether the GS may transmit while a run is in progress
// (radio_silent()). No maburgs-only include, so the web GS can link it.
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "mabur/cal_proto.h"
#include "mabur/cal_wire.h"
#include "mabur/rc_proto.h"

#include "cal_analysis.h"

namespace maburgs {

// Forward-declared: the session only holds pointers to these (optional
// sinks/collaborators, not owned).
class CalLog;
class CalPicker;

struct CalSessionCfg {
  CalThresholds th;
  // How long a SweepCmd is repeated without an ack before the run fails --
  // or, for a wall sweep, before the GS goes silent for its window anyway.
  uint32_t ack_timeout_ms = 3000;
  // How long past a sweep's planned end the GS keeps listening (and stays
  // silent) for late frames and the drone's SweepDone.
  uint32_t phase_slack_ms = 1000;
  // After a capacity sweep's SweepDone, how long (in session time, with no
  // frame of the sweep handled at all) its frames must have been quiet
  // before the sweep closes. The drone sends SweepDone when its last
  // blast returns, ahead of the frames its TX queue is still airing; a
  // saturated stream never gaps this long, the end of the drain does.
  // Bounded by the listen window like everything else in AwaitDone.
  uint32_t tail_quiet_ms = 50;
  // How long an ApplyCmd is repeated without an Applied before the run
  // fails.
  uint32_t apply_timeout_ms = 5000;
  // Bound on the channel pick (measure round + relocate confirm).
  uint32_t pick_timeout_ms = 8000;
  // The GS's copy of the wall margin, for cal.log's R line only: the drone
  // applies its own radio.wall_margin_db.
  double margin_db = 1.0;
};

// What one width's capacity sweep produced so far in this run.
struct WidthResult {
  bool have_eff = false;    // a capacity sweep was analysed with its SweepDone
  // A capacity sweep ran but its SweepDone never arrived: offered and
  // blocked_pct are unknown, so no point can be told apart from tx_bound
  // and nothing is written.
  bool eff_unknown = false;
  bool applied = false;      // the latest apply at this width was adopted
  std::string apply_reason;  // the drone's reason on the latest apply
  std::array<CapResult, 8> cap{};
  std::array<uint16_t, 8> eff_x1000{};  // what was sent; 0 = leave
};

// One finished wall sweep, as the drone reported it: walls relative to
// `anchor`, the chip's TXAGC reference on that channel; floor EVM in
// 0.1 dB.
struct WallReport {
  uint8_t channel = 0, bw = 0;
  std::array<int, 8> walls{};
  int floor_evm_x10 = 0;
  int anchor = 0;
};

struct CalSnapshot;

class CalSession {
 public:
  // How many copies of each Abort due_mav() offers.
  static constexpr int kAbortCopies = 3;

  // Picking runs the channel pick before anything reaches the drone; it is
  // outside running(). Analyze is transient (entered and left within one
  // call) and only ever visible as a state name in passing.
  enum class State { Idle, Picking, AwaitAck, Sweep, AwaitDone, Analyze, Apply, Done, Failed };

  // `log` and `picker` are optional and not owned; both must outlive the
  // session. A null picker runs without a channel pick.
  explicit CalSession(CalSessionCfg cfg, CalLog* log = nullptr, CalPicker* picker = nullptr)
      : cfg_(cfg), log_(log), picker_(picker) {}

  // Starts a run over `sweeps`. The list is sorted by (width, kind,
  // channel) -- 20 before 40, wall before capacity, so a capacity sweep
  // measures at the walls its own width just stored -- and deduplicated on
  // the same key, so two wall sweeps of one width on different channels
  // both run. Refuses (false, with `*err`) an empty list, an entry that is
  // neither wall nor capacity or not 20/40, a run already in progress
  // (Picking included), a link that is down, a linked peer whose caps are
  // not known yet, or a peer without CAP_CALIBRATE. A wall sweep naming no
  // channel measures the link's; a run that reaches one with no link
  // channel known fails there.
  bool start(uint32_t nonce, std::vector<mabur::cal::SweepSpec> sweeps, uint64_t now_ms,
             std::string* err);

  // The drone's SweepAck, SweepDone and Applied. Anything naming another
  // nonce, another sweep or arriving in the wrong state is ignored. A wall
  // sweep refused as Unsupported (a chip that cannot measure itself) is
  // skipped and the run goes on; any other refusal fails the run. A
  // SweepDone closes a wall sweep at once -- skipping it when the result is
  // Unparked (a channel the drone cannot measure), failing the run on any
  // other result but Ok, and standing in for an ack that never arrived -- and
  // a capacity sweep once its frames have been quiet for tail_quiet_ms.
  void on_mav(const mabur::cal::CalMsg& m, uint64_t now_ms);

  // The caller's once-per-pass tick, called after the pass has handled the
  // frames it drained; the capacity tail's quiet is measured from it. The
  // message to send right now, or nullopt: a SweepCmd (repeated every
  // 200 ms until acked), an ApplyCmd (repeated every 200 ms until
  // Applied), or the Abort that closes the drone's run after Done, a
  // failure or abort() -- kAbortCopies copies 200 ms apart, since the
  // uplink is lossy and the drone ignores a nonce it has already closed.
  // The caller sends it as is, never gated on
  // radio_silent(): the session only offers something when the air is the
  // GS's to use. Advances time-driven transitions first.
  std::optional<mabur::cal::CalMsg> due_mav(uint64_t now_ms);

  // A received calibration (capacity) frame. `air_bytes` is the frame's
  // size on the air (dot11 header + body + FCS), summed into the delivered
  // air bits. A frame counts only while a capacity sweep runs, at its
  // width and with the flags the drone's ack announced; anything else is
  // counted in the snapshot's stamp_mismatch and never tallied. Before the
  // ack, the first CRC-clean frame of the commanded sweep stands in for it
  // and its flags become the run's. `now_ms` is the frame's own
  // receive stamp (the RX thread's, not the core loop's tick): a capacity
  // point is timed by when its frames arrived.
  void on_cal_frame(int card, const mabur::cal::CalFrameInfo& f, int rssi_dbm, bool crc_ok,
                    size_t air_bytes, uint64_t now_ms);

  // True whenever a GS transmit would corrupt the measurement: from the
  // ack (or the frame that stood in for it) until the sweep ends or its
  // listen window closes. The drone itself stays silent through a wall
  // measurement, so the GS hears nothing until its report; a wall sweep
  // whose ack never came goes silent at the ack timeout, for the window the
  // last SweepCmd copy would have opened.
  bool radio_silent(uint64_t now_ms) const;

  // AwaitAck through Apply: a run owns the air. Picking is not included
  // -- the channel machinery must keep running while the pick measures.
  bool running() const;
  // running()'s set of states, for a caller holding only a snapshot.
  static bool is_running(State st);

  // Abandons the run from any state: the Aborts go out through due_mav()
  // (if the run already reached the drone), a pick in progress is
  // cancelled, the air reopens and the session goes Idle. The snapshot
  // keeps `why` as its fail_reason.
  void abort(const char* why);

  // Told every tick: link state, whether the peer advertised
  // CAP_CALIBRATE (nullopt: no Telem since the session opened, caps not
  // known yet), and the pair's tag context.
  void set_peer(bool linked, std::optional<bool> cal_capable, mabur::rc::TagCtx ctx = {});

  // The tag context every cal message of the current run is packed under:
  // set_peer()'s ctx as of the moment the run first commands the drone
  // (start() without a pick, the end of the pick otherwise), frozen for
  // the run -- the drone latches the pair its first SweepCmd arrived
  // under and verifies the rest of the run against it.
  const mabur::rc::TagCtx& tag_ctx() const { return tag_ctx_; }

  double margin_db() const { return cfg_.margin_db; }
  State state() const { return state_; }
  CalSnapshot snapshot() const;

  // The run's sweep list after sorting and dedup.
  const std::vector<mabur::cal::SweepSpec>& sweeps() const { return sweeps_; }

 private:
  // The session's clock: the latest stamp any entry point has seen. The
  // receive path stamps frames and replies on its own thread, so a stamp
  // can be older than a tick already seen; every entry point advances this
  // first and works from it, so elapsed-time checks never underflow.
  uint64_t advance(uint64_t now_ms) {
    if (now_ms > now_) now_ = now_ms;
    return now_;
  }
  void step(uint64_t now_ms);
  void step_picking(uint64_t now_ms);
  void finish_pick(const char* pick, uint64_t now_ms);
  void begin_sweep(size_t i, uint64_t now_ms);
  // cal.log's R line, once per run; nullopt flags = no ack heard yet.
  void log_run(std::optional<uint8_t> flags);
  void log_accepted();  // R (once) and the sweep's S line
  // nullopt flags: an implicit ack that carried none (a wall report).
  void on_accepted(std::optional<uint8_t> flags, uint16_t start_in_ms, uint64_t now_ms);
  void finish_sweep(uint64_t now_ms);
  void finish_wall(uint64_t now_ms);
  void skip_sweep(const char* why, uint64_t now_ms);  // records it, goes on to the next sweep
  bool analyze_capacity_sweep();   // true: an apply is due
  void begin_apply(uint64_t now_ms);
  void on_applied(const mabur::cal::Applied& a, uint64_t now_ms);
  void next_sweep(uint64_t now_ms);
  void fail(const std::string& why);
  void queue_abort();
  void tally_capacity(int card, const mabur::cal::CalFrameInfo& f, int rssi_dbm, bool crc_ok,
                      size_t air_bytes, uint64_t rx_ms);
  // True once no capacity frame has reached the session for tail_quiet_ms
  // of session time (a wall sweep has no tail: always true).
  bool tail_quiet(uint64_t now_ms) const;
  WidthResult& width_result();

  CalSessionCfg cfg_;
  CalLog* log_ = nullptr;
  CalPicker* picker_ = nullptr;

  State state_ = State::Idle;
  uint32_t nonce_ = 0;
  bool linked_ = false;
  std::optional<bool> cal_capable_;
  mabur::rc::TagCtx peer_ctx_{};
  mabur::rc::TagCtx tag_ctx_{};
  std::string fail_reason_;

  std::vector<mabur::cal::SweepSpec> sweeps_;
  size_t sweep_i_ = 0;
  mabur::cal::SweepCmd cmd_{};  // the running sweep's command

  // Picking.
  uint64_t now_ = 0;  // see advance()
  uint64_t pick_start_ms_ = 0;
  bool reloc_ordered_ = false;
  uint8_t reloc_to_ = 0;
  uint8_t channel_ = 0;
  std::string pick_note_;
  const char* pick_ = "pinned";  // cal.log R's pick token
  bool run_logged_ = false;      // R written (at the first accepted ack)

  // AwaitAck / Apply repeats, Abort.
  uint64_t phase_start_ms_ = 0;  // entered AwaitAck / Apply
  uint64_t last_sent_ms_ = 0;
  bool sent_once_ = false;
  bool reached_drone_ = false;  // at least one SweepCmd was offered
  // The Abort still owed to the drone, and how many more copies of it.
  std::optional<mabur::cal::Abort> pending_abort_;
  int aborts_left_ = 0;
  uint64_t next_abort_ms_ = 0;

  // Sweep / AwaitDone windows.
  uint8_t ack_flags_ = 0;
  bool flags_known_ = false;    // some ack of this run announced ack_flags_
  // A wall sweep whose ack never came: the GS went silent for the window
  // anyway, and the drone's report is the only proof it ran.
  bool unacked_ = false;
  uint64_t accepted_ms_ = 0;    // the running sweep's ack
  uint64_t sweep_end_ms_ = 0;   // planned end: AwaitDone opens here
  uint64_t window_end_ms_ = 0;  // AwaitDone closes here
  std::optional<mabur::cal::SweepDone> done_;
  uint32_t stamp_mismatch_ = 0;
  uint32_t cell_i_ = 0;

  // Capacity tally per rate; RSSI samples per card, median at analysis.
  std::array<CapCell, 8> cap_;
  std::array<std::array<std::vector<int>, 2>, 8> cap_rssi_;
  // Session time a capacity frame was last handled (tail_quiet): the clock
  // at its tally, raised to the next due_mav tick (cap_fresh_ until then).
  uint64_t last_cap_seen_ms_ = 0;
  bool cap_fresh_ = false;
  mabur::cal::ApplyCmd apply_{};

  std::array<WidthResult, 2> width_{};
  std::vector<WallReport> walls_;
  std::vector<std::string> skipped_;
  std::vector<std::string> skip_reason_;
};

struct CalSnapshot {
  CalSession::State state = CalSession::State::Idle;
  size_t sweep_i = 0, sweep_n = 0;
  mabur::cal::SweepSpec cur{mabur::cal::Kind::Wall, 20};
  // Progress through the running sweep. Capacity: the rate of the latest
  // tallied frame, 1-based, out of the sweep's rates. Wall: whole seconds
  // since the ack, out of the drone's deadline.
  uint32_t cell_i = 0, cell_n = 0;
  uint8_t channel = 0;
  std::string pick_note;
  uint8_t flags = 0;  // the stamp flags the drone's ack announced
  uint32_t stamp_mismatch = 0;  // frames dropped for a kind/width/flags mismatch
  std::array<WidthResult, 2> width;  // [0] = 20 MHz, [1] = 40 MHz
  std::vector<WallReport> walls;     // one per finished wall sweep, in run order
  std::vector<std::string> skipped;  // tokens of sweeps the drone could not run
  std::vector<std::string> skip_reason;  // why, one per skipped token: "unsupported", "unparked"
  std::string fail_reason;
};

}  // namespace maburgs
