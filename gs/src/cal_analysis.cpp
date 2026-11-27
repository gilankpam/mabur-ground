#include "cal_analysis.h"

#include <algorithm>
#include <cmath>

#include "mabur/profile.h"

namespace maburgs {
CapResult analyze_capacity(const CapCell& c, uint8_t mcs, uint8_t bw,
                           uint32_t dwell_ms, const CalThresholds& th) {
  CapResult out;

  // Best single card by delivered air bits, never the union: the two cards
  // do not independently bottleneck the same TX stream.
  int best = 0;
  if (c.air_bits[1] > c.air_bits[0]) best = 1;
  out.best_card = best;
  const size_t bi = static_cast<size_t>(best);

  // A peak RSSI reading above the threshold is enough to suspect RX
  // compression.
  if (c.have_rssi[bi] && static_cast<double>(c.rssi_dbm[bi]) > th.sat_rssi_dbm)
    out.flags |= kCalSaturated;

  // Rarely blocked means the air had slots free that the drone never filled
  // -- this dwell measured the drone's CPU/USB, not the air's capacity.
  if (static_cast<int>(c.blocked_pct) < th.tx_bound_blocked_pct)
    out.flags |= kCalTxBound;

  // Too few frames arrived to trust the count, independent of whether the
  // bits that did arrive look efficient.
  if (static_cast<double>(c.frames[bi]) <
      th.lossy_ratio * static_cast<double>(c.offered))
    out.flags |= kCalLossy;
  // Under half arrived: whatever the bits say, they measured the loss.
  if (static_cast<double>(c.frames[bi]) <
      th.lost_ratio * static_cast<double>(c.offered))
    out.flags |= kCalLost | kCalLossy;

  const double nominal_bps =
      mabur::rc::phy_rate_mbps({mabur::rc::PhyMode::HT, mcs, bw}) * 1e6;
  // Timed by the GS's own receive clock: the drone's TX queue keeps airing
  // after its blast returns, so the air was busy longer than the commanded
  // dwell. The bits after the first frame over first-to-last is the rate the
  // saturated air delivered; the first frame's bits were on the air before
  // its stamp, so they fall outside the span. Only the first MPDU is left
  // out, though the rest of the first A-MPDU (agg6: up to five more) aired
  // before the stamp too -- an over-read of about 0.1-0.2 % of a 2 s point,
  // well under the 0.01 rounding.
  const uint64_t span_ms =
      c.last_ms[bi] > c.first_ms[bi] ? c.last_ms[bi] - c.first_ms[bi] : 0;
  out.span_ms = static_cast<uint32_t>(std::min<uint64_t>(span_ms, UINT32_MAX));
  double eff = 0.0;
  if (nominal_bps > 0.0) {
    if (c.frames[bi] >= th.span_min_frames && span_ms >= th.span_min_ms) {
      const double bits = static_cast<double>(c.air_bits[bi] - c.first_bits[bi]);
      eff = bits / (static_cast<double>(span_ms) / 1000.0 * nominal_bps);
    } else {
      // Too few frames, or too short a span, to time: the commanded dwell
      // is the only clock left, and it reads high by the queue's drain.
      out.flags |= kCalShortSpan;
      if (dwell_ms > 0) {
        const double dwell_s = static_cast<double>(dwell_ms) / 1000.0;
        eff = static_cast<double>(c.air_bits[bi]) / (dwell_s * nominal_bps);
      }
    }
  }
  eff = std::round(eff * 100.0) / 100.0;
  out.eff = std::clamp(eff, 0.05, 1.0);

  return out;
}

}  // namespace maburgs
