#include "mtest.h"
#include "cal_plan.h"
#include "mabur/cal_proto.h"

using namespace maburgs;
namespace cal = mabur::cal;

TEST(wall_sweep_fields) {
  const auto c = make_wall_sweep(7, 2, 40, 112);
  CHECK(c.kind == cal::Kind::Wall && c.bw == 40 && c.channel == 112 && c.nonce == 7 && c.seq == 2);
  CHECK(sweep_duration_ms(c) == kWallDeadlineMs);
}

TEST(capacity_sweep_is_eight_two_second_points) {
  const auto c = make_capacity_sweep(5, 1, 40);
  CHECK(c.nonce == 5);
  CHECK(c.seq == 1);
  CHECK(c.kind == cal::Kind::Capacity);
  CHECK(c.bw == 40);
  CHECK(c.settle_ms == kCapSettleMs);
  CHECK(c.dwell_ms == kCapDwellMs);
  CHECK(c.mcs_mask == 0xFF);
  // 8 points * (300 ms settle + 2000 ms dwell) = 18400 ms.
  CHECK(sweep_duration_ms(c) == 18400);
}

TEST(sweep_tokens) {
  CHECK(cal_sweep_token({cal::Kind::Wall, 40}) == "power40");
  CHECK(cal_sweep_token({cal::Kind::Wall, 20, 112}) == "power20@112");
  CHECK(cal_sweep_token({cal::Kind::Capacity, 20}) == "cap20");
}

MTEST_MAIN
