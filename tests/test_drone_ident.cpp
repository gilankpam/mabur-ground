#include <vector>

#include "drone_ident.h"
#include "mabur/ota_mav.h"
#include "mtest.h"

using maburgs::DroneIdent;
namespace ota = mabur::ota;

static std::vector<uint8_t> ident_pkt() {
  return ota::encode(ota::Ident{"v0.9.0", "board-x", "8812eu"}, 1, 1, mabur::mav::kChanGsLocal);
}

TEST(asks_at_once_then_every_second_five_times) {
  DroneIdent d;
  CHECK(d.tick(7, 0).has_value());
  CHECK(!d.tick(7, 500).has_value());
  for (uint64_t t : {1000, 2000, 3000, 4000}) CHECK(d.tick(7, t).has_value());
  CHECK(!d.gave_up());
  CHECK(!d.tick(7, 5000).has_value());
  CHECK(d.gave_up());
}

TEST(answer_stops_asking) {
  DroneIdent d;
  CHECK(d.tick(7, 0).has_value());
  auto p = ident_pkt();
  d.on_packet(p.data(), p.size());
  REQUIRE(d.ident().has_value());
  CHECK(d.ident()->version == "v0.9.0");
  CHECK(d.ident()->board == "board-x");
  CHECK(d.ident()->chip == "8812eu");
  CHECK(!d.tick(7, 1000).has_value());
  CHECK(!d.gave_up());
}

TEST(new_nonce_clears_and_asks_again) {
  DroneIdent d;
  d.tick(7, 0);
  auto p = ident_pkt();
  d.on_packet(p.data(), p.size());
  CHECK(d.tick(8, 100).has_value());
  CHECK(!d.ident().has_value());
}

TEST(zero_nonce_clears_and_is_silent) {
  DroneIdent d;
  d.tick(7, 0);
  auto p = ident_pkt();
  d.on_packet(p.data(), p.size());
  CHECK(!d.tick(0, 100).has_value());
  CHECK(!d.ident().has_value());
  CHECK(!d.gave_up());
}

TEST(ident_while_no_session_ignored) {
  DroneIdent d;
  auto p = ident_pkt();
  d.on_packet(p.data(), p.size());
  CHECK(!d.ident().has_value());
}

MTEST_MAIN
