#pragma once
// RC frames as a peer at another RC_VERSION would send them: the version
// byte rewritten, the key-only tag recomputed where the frame has one (the
// version is inside the tagged bytes), the CRC recomputed.
#include <cstdint>
#include <vector>

#include "mabur/crc16.h"
#include "mabur/link_key.h"
#include "mabur/siphash.h"

namespace rc_test {

inline void fix_crc(std::vector<uint8_t>& b) {
  const uint16_t crc = mabur::crc16_ccitt(b.data(), b.size() - 2);
  b[b.size() - 2] = static_cast<uint8_t>(crc);
  b[b.size() - 1] = static_cast<uint8_t>(crc >> 8);
}

// A packed DISC_ACK (26 bytes, tag at 16) at version `ver`, tagged under `key`.
inline std::vector<uint8_t> ack_at_version(std::vector<uint8_t> b, uint8_t ver,
                                           const mabur::LinkKey& key) {
  b[2] = ver;
  std::vector<uint8_t> in(b.begin(), b.begin() + 16);
  in.insert(in.end(), 12, 0);   // ctx {0, 0, 0}
  const uint64_t tag = mabur::siphash24(key, in.data(), in.size());
  for (int i = 0; i < 8; ++i) b[16 + i] = static_cast<uint8_t>(tag >> (8 * i));
  fix_crc(b);
  return b;
}

// An untagged frame (Telem) at version `ver`.
inline std::vector<uint8_t> untagged_at_version(std::vector<uint8_t> b, uint8_t ver) {
  b[2] = ver;
  fix_crc(b);
  return b;
}

}  // namespace rc_test
