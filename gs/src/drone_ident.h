#pragma once
// Asks the drone who it is (IDENT_REQ) when a session's pair appears and
// keeps the answer. It works in update-only sessions too: the identity is
// what says which build to offer.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "mabur/ota_mav.h"

namespace maburgs {

class DroneIdent {
 public:
  static constexpr int kTries = 5;
  static constexpr uint64_t kRetryMs = 1000;

  // vtx_nonce: the current pair's drone nonce, 0 = no session. A change
  // clears the answer and starts asking again; 0 clears and asks nothing.
  // Returns one IDENT_REQ packet's wire bytes when one is due.
  std::optional<std::vector<uint8_t>> tick(uint32_t vtx_nonce, uint64_t now_ms);
  // Whole packets; anything but MABUR_IDENT is ignored.
  void on_packet(const uint8_t* bytes, size_t n);
  const std::optional<mabur::ota::Ident>& ident() const { return ident_; }
  // kTries sent under this nonce and the last one's kRetryMs window ran out
  // with no answer; set by tick(), cleared by a new nonce or an answer.
  bool gave_up() const { return gave_up_ && !ident_; }

 private:
  uint32_t nonce_ = 0;
  int sent_ = 0;
  bool gave_up_ = false;
  uint64_t last_ms_ = 0;
  std::optional<mabur::ota::Ident> ident_;
};

}  // namespace maburgs
