#include "mtest.h"
#include "cal_analysis.h"

#include <vector>

using namespace maburgs;

// mcs4/HT20, long GI: phy_rate_mbps gives 39 Mbps. The drone's TX queue
// keeps airing after its 2 s blast returns, so the GS heard this point for
// 2.3 s: 5705 frames of 1435 air bytes, the first at 1000 ms and the last
// at 3300 ms. The air was saturated for that whole span, and the 5704
// frames after the first one are what it carried in it:
// 5704 * 11480 / (2.3 * 39e6) = 0.7300... -- not the 0.84 the commanded
// dwell would read.
TEST(efficiency_is_delivered_over_the_receive_span) {
  constexpr uint64_t kBits = 1435 * 8;
  CapCell c;
  c.offered = 5705;
  c.blocked_pct = 80;  // well clear of tx_bound
  c.frames = {5705, 0};
  c.air_bits = {5705 * kBits, 0};
  c.first_bits = {kBits, 0};
  c.first_ms = {1000, 0};
  c.last_ms = {3300, 0};
  c.rssi_dbm = {-70, 0};
  c.have_rssi = {true, false};
  const auto r = analyze_capacity(c, /*mcs=*/4, /*bw=*/20, /*dwell_ms=*/2000,
                                   CalThresholds{});
  CHECK(r.eff == 0.73);
  CHECK(r.span_ms == 2300);
  CHECK((r.flags & kCalShortSpan) == 0);
  CHECK(r.best_card == 0);
}

// The span is the best card's own: card 1 heard more bits, so its first
// and last frame time the point even though card 0's span is longer.
TEST(the_span_is_the_best_cards) {
  constexpr uint64_t kBits = 1435 * 8;
  CapCell c;
  c.offered = 5705;
  c.blocked_pct = 80;
  c.frames = {3000, 5705};
  c.air_bits = {3000 * kBits, 5705 * kBits};
  c.first_bits = {kBits, kBits};
  c.first_ms = {500, 1000};
  c.last_ms = {4000, 3300};
  const auto r = analyze_capacity(c, 4, 20, 2000, CalThresholds{});
  CHECK(r.best_card == 1);
  CHECK(r.span_ms == 2300);
  CHECK(r.eff == 0.73);
}

// Too few frames, or too short a span, to time the delivery: eff falls back
// to the commanded dwell and the point says so. mcs4/20 at 39 Mbps:
// 5000 frames * 11480 bits / (2 s * 39e6) = 0.7358 -> 0.74.
TEST(a_short_span_falls_back_to_the_commanded_dwell_and_says_so) {
  constexpr uint64_t kBits = 1435 * 8;
  CapCell c;
  c.offered = 5000;
  c.blocked_pct = 80;
  c.frames = {5000, 0};
  c.air_bits = {5000 * kBits, 0};
  c.first_bits = {kBits, 0};
  c.first_ms = {1000, 0};
  c.last_ms = {1499, 0};  // 499 ms: under the 500 ms floor
  auto r = analyze_capacity(c, 4, 20, 2000, CalThresholds{});
  CHECK((r.flags & kCalShortSpan) != 0);
  CHECK(r.eff == 0.74);
  CHECK(r.span_ms == 499);

  c.last_ms = {1500, 0};  // 500 ms: long enough to time
  r = analyze_capacity(c, 4, 20, 2000, CalThresholds{});
  CHECK((r.flags & kCalShortSpan) == 0);
  CHECK(r.eff == 1.0);  // 4999 * 11480 / (0.5 s * 39e6) clamps at 1.0

  // mcs0/HT20 (6.5 Mbps), a 500 ms span. 19 frames are too few to time:
  // 19 * 11480 / (2 s * 6.5e6) = 0.017, floored at 0.05.
  c.frames = {19, 0};
  c.air_bits = {19 * kBits, 0};
  r = analyze_capacity(c, 0, 20, 2000, CalThresholds{});
  CHECK((r.flags & kCalShortSpan) != 0);
  CHECK(r.eff == 0.05);
  // 20 are enough, and the span is what times them: 19 * 11480 /
  // (0.5 s * 6.5e6) = 0.067 -> 0.07, where the dwell would read 0.05.
  c.frames = {20, 0};
  c.air_bits = {20 * kBits, 0};
  r = analyze_capacity(c, 0, 20, 2000, CalThresholds{});
  CHECK((r.flags & kCalShortSpan) == 0);
  CHECK(r.span_ms == 500);
  CHECK(r.eff == 0.07);
}

TEST(best_card_is_the_one_with_more_bits) {
  CapCell c;
  c.offered = 1000;
  c.blocked_pct = 80;
  c.frames = {400, 900};
  c.air_bits = {10000000, 56800000};
  c.rssi_dbm = {-70, -70};
  c.have_rssi = {true, true};
  const auto r = analyze_capacity(c, 4, 20, 2000, CalThresholds{});
  CHECK(r.best_card == 1);
}

TEST(tx_bound_when_the_sender_was_rarely_blocked) {
  CapCell c;
  c.offered = 1000;
  c.blocked_pct = 30;  // under the default 50 % threshold
  c.frames = {980, 0};
  c.air_bits = {56800000, 0};
  c.rssi_dbm = {-70, 0};
  c.have_rssi = {true, false};
  const auto r = analyze_capacity(c, 4, 20, 2000, CalThresholds{});
  CHECK((r.flags & kCalTxBound) != 0);
}

TEST(lossy_when_fewer_than_nine_tenths_arrive) {
  CapCell c;
  c.offered = 1000;
  c.blocked_pct = 80;
  c.frames = {800, 0};  // 80 % < 90 % of offered
  c.air_bits = {46000000, 0};
  c.rssi_dbm = {-70, 0};
  c.have_rssi = {true, false};
  const auto r = analyze_capacity(c, 4, 20, 2000, CalThresholds{});
  CHECK((r.flags & kCalLossy) != 0);
}

TEST(zero_frames_floors_efficiency_and_flags_lossy) {
  CapCell c;
  c.offered = 1000;
  c.blocked_pct = 80;
  c.frames = {0, 0};
  c.air_bits = {0, 0};
  c.rssi_dbm = {-70, 0};
  c.have_rssi = {true, false};
  const auto r = analyze_capacity(c, 4, 20, 2000, CalThresholds{});
  CHECK(r.eff == 0.05);
  CHECK((r.flags & kCalLossy) != 0);
  CHECK((r.flags & kCalLost) != 0);
}

TEST(lost_when_under_half_arrive_and_lossy_alone_from_half_up) {
  // Under half of what was offered measured the loss, not the air: the
  // point is lost (and lossy). From half up to nine tenths it is lossy and
  // still a measurement.
  CapCell c;
  c.offered = 1000;
  c.blocked_pct = 80;
  c.rssi_dbm = {-70, 0};
  c.have_rssi = {true, false};
  c.frames = {499, 0};
  c.air_bits = {499ull * 1435 * 8, 0};
  auto r = analyze_capacity(c, 4, 20, 2000, CalThresholds{});
  CHECK((r.flags & kCalLossy) != 0);
  CHECK((r.flags & kCalLost) != 0);
  c.frames = {500, 0};
  c.air_bits = {500ull * 1435 * 8, 0};
  r = analyze_capacity(c, 4, 20, 2000, CalThresholds{});
  CHECK((r.flags & kCalLossy) != 0);
  CHECK((r.flags & kCalLost) == 0);
  c.frames = {950, 0};
  c.air_bits = {950ull * 1435 * 8, 0};
  r = analyze_capacity(c, 4, 20, 2000, CalThresholds{});
  CHECK((r.flags & (kCalLossy | kCalLost)) == 0);
}

// Bits 0-2, 4 and 5 belonged to the GS's old wall pass and are retired:
// cal.log files on the DVR still carry them.
TEST(retired_flag_bits_are_not_reused) {
  CHECK(kCalSaturated == 8);
  CHECK(kCalTxBound == 64);
  CHECK(kCalLossy == 128);
  CHECK(kCalLost == 256);
  CHECK(kCalShortSpan == 512);
}

MTEST_MAIN
