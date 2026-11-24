#include "drone_ident.h"

namespace maburgs {

std::optional<std::vector<uint8_t>> DroneIdent::tick(uint32_t vtx_nonce, uint64_t now_ms) {
  if (vtx_nonce != nonce_) {
    nonce_ = vtx_nonce;
    sent_ = 0;
    gave_up_ = false;
    ident_.reset();
  }
  if (nonce_ == 0 || ident_) return std::nullopt;
  if (sent_ >= kTries) {
    if (now_ms - last_ms_ >= kRetryMs) gave_up_ = true;
    return std::nullopt;
  }
  if (sent_ > 0 && now_ms - last_ms_ < kRetryMs) return std::nullopt;
  ++sent_;
  last_ms_ = now_ms;
  return mabur::ota::encode(mabur::ota::IdentReq{}, 255, 190, mabur::mav::kChanGsLocal);
}

void DroneIdent::on_packet(const uint8_t* bytes, size_t n) {
  if (nonce_ == 0) return;
  for (const auto& m : mabur::ota::decode_all(bytes, n))
    if (const auto* id = std::get_if<mabur::ota::Ident>(&m)) ident_ = *id;
}

}  // namespace maburgs
