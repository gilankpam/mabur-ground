#pragma once
// Builds the GS's calibration sweep commands (wall, capacity) and predicts
// how long the drone will be busy with one. Pure: no state, no I/O.
#include <cstdint>
#include <string>

#include "mabur/cal_proto.h"

namespace maburgs {

constexpr uint16_t kCapDwellMs = 2000, kCapSettleMs = 300;

// The drone's own deadline on one wall measurement (shared with the drone):
// it reports by then or gives the measurement up, so the GS's listen window
// is sized to it.
using mabur::cal::kWallDeadlineMs;

// The drone measures the TX walls of `channel` at `bw` itself; nothing
// airs for the GS to tally.
mabur::cal::SweepCmd make_wall_sweep(uint32_t nonce, uint8_t seq, uint8_t bw, uint8_t channel);

// One capacity dwell per rate named by mcs_mask (all eight by default), at
// the production A-MPDU mode. The drone's capacity engine reads
// SweepCmd::settle_ms as its per-point settle.
mabur::cal::SweepCmd make_capacity_sweep(uint32_t nonce, uint8_t seq, uint8_t bw);

// How long the drone is busy with a SweepCmd: the GS's listen window opens
// at its end. Wall: kWallDeadlineMs. Capacity: one dwell per rate in
// mcs_mask, each settle_ms + dwell_ms.
uint32_t sweep_duration_ms(const mabur::cal::SweepCmd& c);

// The operator's token for one sweep: power20, power40, cap20, cap40, and
// power20@<ch> / power40@<ch> for a wall sweep naming its channel.
std::string cal_sweep_token(const mabur::cal::SweepSpec& spec);

}  // namespace maburgs
