#pragma once
// Capacity analysis for the calibration kit. Pure: no I/O, no state, no
// clock -- one capacity dwell's tally in, one efficiency out.
#include <array>
#include <cstdint>

namespace maburgs {

// Bits 0, 1, 2, 4 and 5 (no_dip, undetermined, narrow, card_disagree,
// drift: the GS's old wall pass) are retired and never reused, since
// cal.log files on the DVR still carry them.
enum CalFlag : uint32_t {
  kCalSaturated = 1u << 3,      // RX front-end compression suspected
  kCalTxBound = 1u << 6,        // sender rarely blocked: measures CPU/USB, not air
  kCalLossy = 1u << 7,          // too few frames arrived to trust the count
  kCalLost = 1u << 8,           // under half arrived: measured the loss, not written
  kCalShortSpan = 1u << 9,      // too few frames or too short a span to time: eff over the dwell
};

struct CalThresholds {
  double sat_rssi_dbm = -45.0;    // above this, suspect RX saturation
  int tx_bound_blocked_pct = 50;  // blocked_pct below this => kCalTxBound
  double lossy_ratio = 0.9;       // best-card frames below this * offered => kCalLossy
  double lost_ratio = 0.5;        // ...and below this * offered => kCalLost too
  // A capacity point is timed over its own receive span only when the best
  // card heard at least this many frames over at least this long;
  // otherwise its eff falls back to the commanded dwell (kCalShortSpan).
  uint32_t span_min_frames = 20;
  uint32_t span_min_ms = 500;
};

// One capacity dwell's tally at a single (mcs, bw) operating point, flown at
// the production A-MPDU mode.
// `offered` is the frame count the drone reports having handed its TX path
// in the dwell. `blocked_pct` is the share of the dwell the sender spent
// blocked waiting for a TX opportunity -- the air being the bottleneck, not
// the drone's CPU/USB.
//
// Per card: the CRC-clean frames heard, their air bits, and the receive
// times (ms, the GS's own clock) of the first and last of them, with the
// first one's bits -- what times the delivery (analyze_capacity).
struct CapCell {
  uint32_t offered = 0;
  uint8_t blocked_pct = 0;
  std::array<uint32_t, 2> frames{};
  std::array<uint64_t, 2> air_bits{};
  std::array<uint64_t, 2> first_bits{};
  std::array<uint64_t, 2> first_ms{};
  std::array<uint64_t, 2> last_ms{};
  std::array<int, 2> rssi_dbm{};
  std::array<bool, 2> have_rssi{};
};

struct CapResult {
  double eff = 0.0;    // delivered air bits / nominal PHY bits over the timed span
  int best_card = -1;
  uint32_t span_ms = 0;  // the best card's first-to-last receive span
  uint32_t flags = 0;
};

// Delivered air efficiency for one capacity dwell at (mcs, bw), timed by
// the GS's own receive clock: on the best card, the bits that arrived after
// its first frame over the span from that first frame to the last,
//
//   eff = (air_bits - first_bits) / ((last_ms - first_ms) / 1000 * nominal)
//
// -- n frames span n - 1 inter-arrival gaps, so the first frame's bits (on
// the air before its receive stamp) are left out. The drone's blast
// enqueues for the commanded dwell, but its TX queue keeps airing after the
// blast returns, so the air was busy for longer than the dwell: dividing by
// the dwell reads high. Over the receive span the air was saturated
// throughout, and delivered over nominal is the truthful number. When the
// best card heard fewer than span_min_frames frames or over less than
// span_min_ms, the span is too short to trust and eff divides by the
// commanded dwell instead (kCalShortSpan).
//
// Rounded to 0.01 and clamped to [0.05, 1.0] -- never zero, which on the
// wire means "leave the config's entry". Flags a dwell that measured the
// drone's CPU/USB rather than the air (kCalTxBound: the sender was rarely
// blocked, so more air was available than the drone could feed it), one
// whose frame count arrived too thin to trust (kCalLossy), and one where
// under half of what was offered arrived (kCalLost, with kCalLossy): that
// dwell measured the loss, not the air, and like a tx_bound point is not
// written.
CapResult analyze_capacity(const CapCell& c, uint8_t mcs, uint8_t bw,
                           uint32_t dwell_ms, const CalThresholds& th);

}  // namespace maburgs
