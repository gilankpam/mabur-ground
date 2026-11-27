#include "cal_plan.h"

#include <bitset>

namespace maburgs {

mabur::cal::SweepCmd make_wall_sweep(uint32_t nonce, uint8_t seq, uint8_t bw, uint8_t channel) {
  mabur::cal::SweepCmd c;
  c.nonce = nonce;
  c.seq = seq;
  c.kind = mabur::cal::Kind::Wall;
  c.bw = bw;
  c.channel = channel;
  return c;
}

mabur::cal::SweepCmd make_capacity_sweep(uint32_t nonce, uint8_t seq, uint8_t bw) {
  mabur::cal::SweepCmd c;
  c.nonce = nonce;
  c.seq = seq;
  c.kind = mabur::cal::Kind::Capacity;
  c.bw = bw;
  c.settle_ms = kCapSettleMs;
  c.dwell_ms = kCapDwellMs;
  return c;
}

uint32_t sweep_duration_ms(const mabur::cal::SweepCmd& c) {
  if (c.kind == mabur::cal::Kind::Wall) return kWallDeadlineMs;
  const uint32_t rows = std::bitset<8>(c.mcs_mask).count();
  return rows * (static_cast<uint32_t>(c.settle_ms) + c.dwell_ms);
}

std::string cal_sweep_token(const mabur::cal::SweepSpec& spec) {
  const bool wall = spec.kind == mabur::cal::Kind::Wall;
  std::string t = std::string(wall ? "power" : "cap") + std::to_string(spec.bw);
  if (wall && spec.channel != 0) t += "@" + std::to_string(spec.channel);
  return t;
}

}  // namespace maburgs
