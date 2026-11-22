// MavTunnel: MAVLink packets over T_MAV control frames, GS side.
#include <cstring>
#include <vector>

#include "mtest.h"
#include "mabur/link_key.h"
#include "mabur/mav.h"
#include "mabur/rc_proto.h"
#include "mav_tunnel.h"

using namespace maburgs;
using namespace mabur;
using namespace mabur::rc;

static LinkKey test_key() { return *parse_key_hex("3f9a1c77e04b5d2290ab6ef1c8d34e5a"); }

using Bytes = std::vector<uint8_t>;

static Bytes to_bytes(const mavlink_message_t& msg) {
  uint8_t buf[MAVLINK_MAX_PACKET_LEN];
  const uint16_t n = mavlink_msg_to_send_buffer(buf, &msg);
  return Bytes(buf, buf + n);
}

static Bytes hb_bytes(uint8_t sysid, uint8_t compid) {
  mavlink_message_t msg;
  mavlink_msg_heartbeat_pack(sysid, compid, &msg, MAV_TYPE_GCS, MAV_AUTOPILOT_INVALID, 0, 0,
                             MAV_STATE_ACTIVE);
  return to_bytes(msg);
}

// A PARAM_EXT_VALUE whose every payload byte is significant, so the packer
// truncates nothing: 149 B payload + 12 B framing = 161 B.
static Bytes param_ext_value_bytes(uint16_t index) {
  char id[16] = "ANY_PARAM_ID";   // the packer copies all 16 id bytes
  char value[128];
  std::memset(value, 'v', sizeof(value));
  mavlink_message_t msg;
  mavlink_msg_param_ext_value_pack(1, 191, &msg, id, value,
                                   MAV_PARAM_EXT_TYPE_INT32, 13, index);
  return to_bytes(msg);
}

// Raw packets built by hand: forms the library's pack helpers never emit.
static uint16_t mav_crc(const Bytes& b, size_t from, uint8_t crc_extra) {
  uint16_t crc;
  crc_init(&crc);
  for (size_t i = from; i < b.size(); ++i) crc_accumulate(b[i], &crc);
  crc_accumulate(crc_extra, &crc);
  return crc;
}
// MAVLink 2 heartbeat with its trailing zero byte (mavlink_version 0) left
// in: a valid packet the sender chose not to truncate.
static Bytes untrimmed_v2_heartbeat() {
  Bytes b = {0xFD, 9, 0, 0, 7, 255, 190, 0, 0, 0,
             1, 2, 3, 4, MAV_TYPE_GCS, MAV_AUTOPILOT_INVALID, 0, MAV_STATE_ACTIVE, 0};
  const uint16_t crc = mav_crc(b, 1, MAVLINK_MSG_ID_HEARTBEAT_CRC);
  b.push_back(crc & 0xFF);
  b.push_back(crc >> 8);
  return b;
}
static Bytes v1_heartbeat() {
  Bytes b = {0xFE, 9, 7, 255, 190, 0,
             0, 0, 0, 0, MAV_TYPE_GCS, MAV_AUTOPILOT_INVALID, 0, MAV_STATE_ACTIVE, 3};
  const uint16_t crc = mav_crc(b, 1, MAVLINK_MSG_ID_HEARTBEAT_CRC);
  b.push_back(crc & 0xFF);
  b.push_back(crc >> 8);
  return b;
}
// msgid 0xFFFFFF: not in the common dialect, so no CRC_EXTRA to check against.
static Bytes unknown_msgid_packet() {
  return Bytes{0xFD, 1, 0, 0, 7, 1, 191, 0xFF, 0xFF, 0xFF, 0x42, 0x12, 0x34};
}

static Bytes cat(std::initializer_list<Bytes> parts) {
  Bytes out;
  for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
  return out;
}

struct Rig {
  std::vector<Bytes> sent;
  std::vector<Bytes> delivered;
  MavTunnel t{test_key(), [this](Bytes f) { sent.push_back(std::move(f)); },
              [this](const uint8_t* p, size_t n) { delivered.emplace_back(p, p + n); }};
};

// The drone's frame: T_MAV_UP tagged under `pair` with seq32 = its counter.
static const TagCtx kPair{11, 22, 0};
static std::vector<uint8_t> pack_up(mabur::rc::Mav m, TagCtx pair = kPair) {
  m.type = mabur::rc::T_MAV_UP;
  pair.seq32 = m.counter;
  return mabur::rc::pack_mav(m, test_key(), pair);
}

TEST(outbound_dropped_until_linked) {
  std::vector<Bytes> sent;
  MavTunnel t(test_key(), [&](auto f) { sent.push_back(std::move(f)); }, [](auto, auto) {});
  auto hb = hb_bytes(255, 190);
  t.push_outbound(hb.data(), hb.size());
  CHECK(t.flush() == 0 && t.stats().drop_unlinked == 1);
  CHECK(sent.empty());
  t.set_session(TagCtx{11, 22, 0});
  t.push_outbound(hb.data(), hb.size());
  CHECK(t.flush() == 1 && sent.size() == 1);
  auto p = parse_mav(sent[0].data(), sent[0].size());
  REQUIRE(p.has_value());
  CHECK(p->counter == 1 && p->payload == hb);
  CHECK(verify_control(sent[0].data(), sent[0].size(), test_key(), TagCtx{11, 22, 1}));
  CHECK(t.stats().drop_unlinked == 1);
  CHECK(t.stats().tx_frames == 1 && t.stats().tx_packets == 1);
}

TEST(packets_queued_before_the_link_drops_are_dropped_at_flush) {
  Rig r;
  r.t.set_session(kPair);
  auto hb = hb_bytes(255, 190);
  r.t.push_outbound(hb.data(), hb.size());
  r.t.push_outbound(hb.data(), hb.size());
  r.t.set_session(TagCtx{0, 0, 0});
  CHECK(r.t.flush() == 0);
  CHECK(r.sent.empty());
  CHECK(r.t.stats().drop_unlinked == 2);
  r.t.set_session(TagCtx{11, 22, 0});
  CHECK(r.t.flush() == 0);   // nothing left over from before the drop
}

TEST(packets_batch_into_frames_of_at_most_600_bytes) {
  Rig r;
  r.t.set_session(TagCtx{11, 22, 0});
  std::vector<Bytes> pkts;
  for (uint16_t i = 0; i < 5; ++i) {
    pkts.push_back(param_ext_value_bytes(i));
    REQUIRE(pkts.back().size() == 161);
    r.t.push_outbound(pkts.back().data(), pkts.back().size());
  }
  CHECK(r.t.flush() == 2);
  REQUIRE(r.sent.size() == 2);
  auto f0 = parse_mav(r.sent[0].data(), r.sent[0].size());
  auto f1 = parse_mav(r.sent[1].data(), r.sent[1].size());
  REQUIRE(f0.has_value() && f1.has_value());
  CHECK(f0->counter == 1 && f1->counter == 2);
  CHECK(f0->payload.size() == 483);
  CHECK(f0->payload == cat({pkts[0], pkts[1], pkts[2]}));
  CHECK(f1->payload == cat({pkts[3], pkts[4]}));
  CHECK(verify_control(r.sent[0].data(), r.sent[0].size(), test_key(), TagCtx{11, 22, 1}));
  CHECK(verify_control(r.sent[1].data(), r.sent[1].size(), test_key(), TagCtx{11, 22, 2}));
  CHECK(r.t.stats().tx_frames == 2);
  CHECK(r.t.stats().tx_packets == 5);
  CHECK(r.t.counter() == 2);
  CHECK(r.t.flush() == 0);   // the queue is empty after a flush
}

TEST(split_delivery_survives_a_byte_at_a_time_feed) {
  Rig r;
  r.t.set_session(kPair);
  auto hb = hb_bytes(255, 190);
  for (size_t i = 0; i < hb.size(); ++i) {
    r.t.push_outbound(&hb[i], 1);
    if (i + 1 < hb.size()) CHECK(r.t.flush() == 0);
  }
  CHECK(r.t.flush() == 1);
  REQUIRE(r.sent.size() == 1);
  auto p = parse_mav(r.sent[0].data(), r.sent[0].size());
  REQUIRE(p.has_value());
  CHECK(p->payload == hb);
  CHECK(r.t.stats().tx_packets == 1);
  CHECK(r.t.stats().drop_bad_packet == 0);
}

TEST(outbound_packets_pass_through_byte_exact) {
  // A non-truncated sender's packet must not be re-serialised: the library's
  // serialiser would trim the zero tail and keep the old CRC, corrupting it.
  Rig r;
  r.t.set_session(TagCtx{11, 22, 0});
  auto raw = untrimmed_v2_heartbeat();
  r.t.push_outbound(raw.data(), raw.size());
  CHECK(r.t.flush() == 1);
  REQUIRE(r.sent.size() == 1);
  auto p = parse_mav(r.sent[0].data(), r.sent[0].size());
  REQUIRE(p.has_value());
  CHECK(p->payload == raw);
}

TEST(outbound_mavlink1_is_dropped) {
  Rig r;
  r.t.set_session(TagCtx{11, 22, 0});
  auto v1 = v1_heartbeat();
  r.t.push_outbound(v1.data(), v1.size());
  CHECK(r.t.flush() == 0);
  CHECK(r.t.stats().drop_bad_packet == 1);
}

TEST(counter_restarts_on_a_new_vtx_nonce) {
  Rig r;
  auto hb = hb_bytes(255, 190);
  auto send_one = [&]() -> uint32_t {
    r.t.push_outbound(hb.data(), hb.size());
    REQUIRE(r.t.flush() == 1);
    auto p = parse_mav(r.sent.back().data(), r.sent.back().size());
    REQUIRE(p.has_value());
    return p->counter;
  };
  r.t.set_session(TagCtx{11, 22, 0});
  CHECK(send_one() == 1);
  CHECK(send_one() == 2);
  r.t.set_session(TagCtx{11, 33, 0});
  CHECK(send_one() == 1);
  CHECK(verify_control(r.sent.back().data(), r.sent.back().size(), test_key(),
                       TagCtx{11, 33, 1}));
  r.t.set_session(TagCtx{11, 33, 0});
  CHECK(send_one() == 2);
  CHECK(r.t.counter() == 2);
}

TEST(inbound_frame_splits_into_packets_and_drops_a_truncated_tail) {
  Rig r;
  r.t.set_session(kPair);
  auto hb = hb_bytes(1, 191);
  Mav m;
  m.counter = 5;
  m.payload = cat({hb, hb, Bytes(hb.begin(), hb.begin() + 10)});
  auto f = pack_up(m);
  r.t.on_rc_frame(f.data(), f.size());
  REQUIRE(r.delivered.size() == 2);
  CHECK(r.delivered[0] == hb && r.delivered[1] == hb);
  CHECK(r.t.stats().rx_frames == 1);
  CHECK(r.t.stats().rx_packets == 2);
  CHECK(r.t.stats().drop_bad_packet == 1);
  // The truncated tail does not bleed into the next frame.
  Mav m2;
  m2.counter = 6;
  m2.payload = hb;
  auto f2 = pack_up(m2);
  r.t.on_rc_frame(f2.data(), f2.size());
  REQUIRE(r.delivered.size() == 3);
  CHECK(r.delivered[2] == hb);
  CHECK(r.t.stats().rx_packets == 3);
  CHECK(r.t.stats().drop_bad_packet == 1);
}

TEST(mavlink1_and_unknown_crc_extra_are_dropped) {
  Rig r;
  r.t.set_session(kPair);
  Mav m;
  m.counter = 1;
  m.payload = cat({v1_heartbeat(), unknown_msgid_packet()});
  auto f = pack_up(m);
  r.t.on_rc_frame(f.data(), f.size());
  CHECK(r.delivered.empty());
  CHECK(r.t.stats().rx_frames == 1);
  CHECK(r.t.stats().rx_packets == 0);
  CHECK(r.t.stats().drop_bad_packet == 2);
}

// The rc sink's routing in main.cpp: the drone's replies are T_MAV_UP, and a
// sink that sent only T_MAV to the tunnel dropped every one of them.
TEST(the_rc_sink_routes_t_mav_up_and_t_mav_to_the_tunnel) {
  CHECK(maburgs::is_mav_frame(mabur::rc::T_MAV_UP));
  CHECK(maburgs::is_mav_frame(mabur::rc::T_MAV));
  CHECK(!maburgs::is_mav_frame(mabur::rc::T_TELEM));
  CHECK(!maburgs::is_mav_frame(-1));
}

TEST(inbound_tagged_or_corrupt_frame_delivers_nothing) {
  Rig r;
  r.t.set_session(kPair);
  auto hb = hb_bytes(1, 191);
  Mav m;
  m.counter = 1;
  m.payload = hb;
  // A tagged T_MAV is the GS's own uplink heard by its other card: counted
  // apart, never a reject.
  auto tagged = pack_mav(m, test_key(), TagCtx{1, 2, 1});
  r.t.on_rc_frame(tagged.data(), tagged.size());
  CHECK(r.delivered.empty());
  CHECK(r.t.stats().rx_frames == 0);
  CHECK(r.t.stats().rx_uplink_heard == 1);
  CHECK(r.t.stats().drop_bad_packet == 0);
  // A tagged frame still carrying the 4-byte FCS is heard the same way.
  Bytes with_fcs = cat({tagged, Bytes{1, 2, 3, 4}});
  r.t.on_rc_frame(with_fcs.data(), with_fcs.size());
  CHECK(r.t.stats().rx_uplink_heard == 2);
  // A corrupt frame is a reject.
  auto bad = pack_up(m);
  bad.back() ^= 0xFF;
  r.t.on_rc_frame(bad.data(), bad.size());
  CHECK(r.delivered.empty());
  CHECK(r.t.stats().rx_uplink_heard == 2);
  CHECK(r.t.stats().drop_bad_packet == 1);
  // A tagged T_MAV is never delivered, whatever pair it verifies under.
  auto own = pack_mav(m, test_key(), TagCtx{11, 22, 1});
  r.t.on_rc_frame(own.data(), own.size());
  CHECK(r.delivered.empty());
  CHECK(r.t.stats().rx_uplink_heard == 3);
}

TEST(untagged_frame_with_fcs_is_not_taken_for_the_gs_own_uplink) {
  Rig r;
  r.t.set_session(kPair);
  Mav m;
  m.counter = 3;
  m.payload = hb_bytes(1, 191);
  Bytes f = cat({pack_up(m), Bytes{0xAA, 0xBB, 0xCC, 0xDD}});
  r.t.on_rc_frame(f.data(), f.size());
  REQUIRE(r.delivered.size() == 1);
  CHECK(r.t.stats().rx_uplink_heard == 0);
}

TEST(second_card_copy_of_a_drone_frame_is_dropped) {
  Rig r;
  r.t.set_session(kPair);
  auto hb = hb_bytes(1, 191);
  auto frame = [&](uint32_t counter) {
    Mav m;
    m.counter = counter;
    m.payload = hb;
    return pack_up(m);
  };
  auto f7 = frame(7);
  r.t.on_rc_frame(f7.data(), f7.size());
  r.t.on_rc_frame(f7.data(), f7.size());   // the other card's copy
  CHECK(r.delivered.size() == 1);
  CHECK(r.t.stats().rx_dup == 1);
  CHECK(r.t.stats().rx_frames == 1);
  CHECK(r.t.stats().drop_bad_packet == 0);
  // N and N+1 both deliver.
  auto f8 = frame(8);
  r.t.on_rc_frame(f8.data(), f8.size());
  CHECK(r.delivered.size() == 2);
  CHECK(r.t.stats().rx_dup == 1);
  // A restarted drone (new vtx nonce) counts from 0 again.
  r.t.set_session(TagCtx{11, 33, 0});
  auto f0 = pack_up([&] { Mav z; z.counter = 0; z.payload = hb; return z; }(), TagCtx{11, 33, 0});
  r.t.on_rc_frame(f0.data(), f0.size());
  CHECK(r.delivered.size() == 3);
  r.t.on_rc_frame(f0.data(), f0.size());
  CHECK(r.delivered.size() == 3);
  CHECK(r.t.stats().rx_dup == 2);
  CHECK(r.t.stats().rx_frames == 3);
}

TEST(first_drone_frame_after_a_new_vtx_nonce_delivers_even_with_the_old_counter) {
  Rig r;
  r.t.set_session(TagCtx{11, 22, 0});
  Mav m;
  m.counter = 0;
  m.payload = hb_bytes(1, 191);
  auto f = pack_up(m);
  r.t.on_rc_frame(f.data(), f.size());
  r.t.set_session(TagCtx{11, 33, 0});
  auto g = pack_up(m, TagCtx{11, 33, 0});
  r.t.on_rc_frame(g.data(), g.size());
  CHECK(r.delivered.size() == 2);
  CHECK(r.t.stats().rx_dup == 0);
}

static Mav up_mav(uint32_t counter) {
  Mav m;
  m.counter = counter;
  m.payload = hb_bytes(1, 191);
  return m;
}

TEST(uplink_verifies_under_live_session) {
  Rig r;
  r.t.set_session(kPair);
  auto f = pack_up(up_mav(4));
  r.t.on_rc_frame(f.data(), f.size());
  CHECK(r.delivered.size() == 1);
  CHECK(r.t.stats().drop_auth == 0);
}

// The live session lapses during every calibration sweep; the drone's cal
// replies are tagged under the run's pair and must still verify.
TEST(uplink_verifies_under_cal_run_pair_after_the_live_session_is_gone) {
  Rig r;
  const TagCtx run{51, 52, 0};
  r.t.push_cal(hb_bytes(1, 191), run);
  r.t.flush();
  r.t.set_session(TagCtx{});   // live session gone
  auto f = pack_up(up_mav(1), run);
  r.t.on_rc_frame(f.data(), f.size());
  CHECK(r.delivered.size() == 1);
  CHECK(r.t.stats().drop_auth == 0);
  // A pair that is neither live nor the cal run's still fails.
  auto other = pack_up(up_mav(2), TagCtx{61, 62, 0});
  r.t.on_rc_frame(other.data(), other.size());
  CHECK(r.delivered.size() == 1);
  CHECK(r.t.stats().drop_auth == 1);
}

TEST(forged_uplink_is_dropped_as_auth) {
  Rig r;
  r.t.set_session(kPair);
  auto wrong_key = pack_mav(
      [] { Mav m = up_mav(1); m.type = T_MAV_UP; return m; }(),
      *parse_key_hex("00112233445566778899aabbccddeeff"), TagCtx{11, 22, 1});
  r.t.on_rc_frame(wrong_key.data(), wrong_key.size());
  Mav m2 = up_mav(2);
  m2.type = T_MAV_UP;
  auto wrong_ctr = pack_mav(m2, test_key(), TagCtx{11, 22, 3});   // tag over another counter
  r.t.on_rc_frame(wrong_ctr.data(), wrong_ctr.size());
  CHECK(r.delivered.empty());
  CHECK(r.t.stats().drop_auth == 2);
  CHECK(r.t.stats().rx_frames == 0);
}

TEST(repeat_counter_is_rx_dup) {
  Rig r;
  r.t.set_session(kPair);
  auto f = pack_up(up_mav(9));
  r.t.on_rc_frame(f.data(), f.size());
  r.t.on_rc_frame(f.data(), f.size());
  CHECK(r.delivered.size() == 1);
  CHECK(r.t.stats().rx_dup == 1);
}

// Two drone threads share one counter and can air out of order: the window
// takes each counter once, rejects what it has seen or what fell off it.
TEST(uplink_window_takes_out_of_order_once_and_drops_old) {
  Rig r;
  r.t.set_session(kPair);
  auto take = [&](uint32_t c) {
    auto f = pack_up(up_mav(c));
    r.t.on_rc_frame(f.data(), f.size());
  };
  take(100);
  take(101);
  take(99);    // late but inside the window
  CHECK(r.delivered.size() == 3);
  take(99);    // seen
  take(101);   // seen
  CHECK(r.delivered.size() == 3);
  CHECK(r.t.stats().rx_dup == 2);
  take(164);   // H = 164: 101 is exactly 63 back and still taken
  take(100);   // already taken, in window
  take(37);    // 127 back: too old
  CHECK(r.delivered.size() == 4);
  CHECK(r.t.stats().rx_dup == 4);
  take(101);   // in window (63 back) but seen
  take(102);   // in window, never seen: delivers
  CHECK(r.delivered.size() == 5);
  take(300);   // a jump past the window slides it right over everything
  take(250);   // inside the new window, never seen
  take(164);   // 136 back: too old now
  CHECK(r.delivered.size() == 7);
}

TEST(uplink_window_resets_on_a_new_pair) {
  Rig r;
  r.t.set_session(kPair);
  auto f = pack_up(up_mav(500));
  r.t.on_rc_frame(f.data(), f.size());
  r.t.set_session(TagCtx{11, 33, 0});
  auto g = pack_up(up_mav(3), TagCtx{11, 33, 0});   // far below the old high mark
  r.t.on_rc_frame(g.data(), g.size());
  CHECK(r.delivered.size() == 2);
  CHECK(r.t.stats().rx_dup == 0);
}

// Cal lane: frames of only cal packets, tagged under the run's frozen ctx,
// counted apart from the live lane and sent whether or not the GS has a
// live session at all.

TEST(cal_lane_sends_while_unlinked) {
  Rig r;   // no set_session: the live lane is unlinked throughout
  auto cal_pkt = hb_bytes(1, 191);   // stand-in packet bytes; the lane never parses them
  r.t.push_cal(cal_pkt, TagCtx{11, 22, 0});
  CHECK(r.t.flush() == 1);
  REQUIRE(r.sent.size() == 1);
  auto p = parse_mav(r.sent[0].data(), r.sent[0].size());
  REQUIRE(p.has_value());
  CHECK(p->counter == 1 && p->payload == cal_pkt);
  CHECK(verify_control(r.sent[0].data(), r.sent[0].size(), test_key(), TagCtx{11, 22, 1}));
  CHECK(r.t.cal_counter() == 1);
  CHECK(r.t.stats().tx_cal_frames == 1);
}

TEST(cal_lane_counter_restarts_per_run_ctx) {
  Rig r;
  auto cal_pkt = hb_bytes(1, 191);
  r.t.push_cal(cal_pkt, TagCtx{11, 22, 0});
  CHECK(r.t.flush() == 1);
  CHECK(r.t.cal_counter() == 1);
  r.t.push_cal(cal_pkt, TagCtx{11, 22, 0});
  CHECK(r.t.flush() == 1);
  CHECK(r.t.cal_counter() == 2);
  // A new run's pair (vtx_nonce changes) restarts the counter at 1.
  r.t.push_cal(cal_pkt, TagCtx{11, 33, 0});
  CHECK(r.t.flush() == 1);
  CHECK(r.t.cal_counter() == 1);
  auto p = parse_mav(r.sent.back().data(), r.sent.back().size());
  REQUIRE(p.has_value());
  CHECK(verify_control(r.sent.back().data(), r.sent.back().size(), test_key(),
                       TagCtx{11, 33, 1}));
}

TEST(cal_lane_counter_continues_for_a_second_run_on_the_same_pair) {
  Rig r;
  auto cal_pkt = hb_bytes(1, 191);
  r.t.push_cal(cal_pkt, TagCtx{11, 22, 0});
  CHECK(r.t.flush() == 1);
  CHECK(r.t.cal_counter() == 1);
  r.t.push_cal(cal_pkt, TagCtx{11, 22, 0});
  CHECK(r.t.flush() == 1);
  CHECK(r.t.cal_counter() == 2);
  // A second run on the same pair (vtx_nonce unchanged) does not restart --
  // the drone would refuse a restart as a replay.
  r.t.push_cal(cal_pkt, TagCtx{11, 22, 0});
  CHECK(r.t.flush() == 1);
  CHECK(r.t.cal_counter() == 3);
  auto p = parse_mav(r.sent.back().data(), r.sent.back().size());
  REQUIRE(p.has_value());
  CHECK(p->counter == 3);
  CHECK(verify_control(r.sent.back().data(), r.sent.back().size(), test_key(),
                       TagCtx{11, 22, 3}));
}

TEST(cal_frames_go_out_before_live_frames) {
  Rig r;
  r.t.set_session(kPair);
  auto hb = hb_bytes(255, 190);
  r.t.push_outbound(hb.data(), hb.size());   // live
  auto cal_pkt = hb_bytes(1, 191);
  r.t.push_cal(cal_pkt, TagCtx{44, 55, 0});   // cal, a different pair entirely
  CHECK(r.t.flush() == 2);
  REQUIRE(r.sent.size() == 2);
  auto f0 = parse_mav(r.sent[0].data(), r.sent[0].size());
  auto f1 = parse_mav(r.sent[1].data(), r.sent[1].size());
  REQUIRE(f0.has_value() && f1.has_value());
  CHECK(f0->payload == cal_pkt);
  CHECK(f1->payload == hb);
  CHECK(verify_control(r.sent[0].data(), r.sent[0].size(), test_key(), TagCtx{44, 55, 1}));
  CHECK(verify_control(r.sent[1].data(), r.sent[1].size(), test_key(), TagCtx{11, 22, 1}));
}

TEST(cal_frame_carries_only_its_packet) {
  Rig r;
  r.t.set_session(kPair);
  auto hb = hb_bytes(255, 190);
  r.t.push_outbound(hb.data(), hb.size());   // live
  auto cal_pkt = hb_bytes(1, 191);
  r.t.push_cal(cal_pkt, TagCtx{11, 22, 0});   // same pair as the live session
  CHECK(r.t.flush() == 2);
  REQUIRE(r.sent.size() == 2);
  auto f0 = parse_mav(r.sent[0].data(), r.sent[0].size());
  auto f1 = parse_mav(r.sent[1].data(), r.sent[1].size());
  REQUIRE(f0.has_value() && f1.has_value());
  CHECK(f0->payload == cal_pkt);
  CHECK(f1->payload == hb);
  CHECK(r.t.stats().tx_cal_frames == 1);
  CHECK(r.t.stats().tx_frames == 1);
  CHECK(r.t.stats().tx_packets == 1);   // the live lane's own packet count, cal excluded
}

TEST(cal_lane_drops_an_oversized_packet) {
  Rig r;
  Bytes oversized(kMavMaxPayload + 1, 0xAB);
  r.t.push_cal(oversized, TagCtx{11, 22, 0});
  CHECK(r.t.flush() == 0);
  CHECK(r.sent.empty());
  CHECK(r.t.stats().drop_cal_oversize == 1);
  CHECK(r.t.stats().tx_cal_frames == 0);
  CHECK(r.t.cal_counter() == 0);
  // A later, properly sized cal packet still gets counter 1: the dropped
  // packet never consumed one.
  auto cal_pkt = hb_bytes(1, 191);
  r.t.push_cal(cal_pkt, TagCtx{11, 22, 0});
  CHECK(r.t.flush() == 1);
  CHECK(r.t.cal_counter() == 1);
}

// A tunnel built with a cal sender hands cal frames to it and live frames
// to the live sender: the GS sends the two through different gates (a
// calibration message never waits on the RCF slotter or the radio-silence
// gate, a live one always does).
TEST(cal_frames_go_to_the_cal_sender_when_one_is_given) {
  std::vector<Bytes> live, calf;
  MavTunnel t(test_key(), [&](Bytes f) { live.push_back(std::move(f)); },
              [](const uint8_t*, size_t) {},
              [&](Bytes f) { calf.push_back(std::move(f)); });
  t.set_session(TagCtx{11, 22, 0});
  auto hb = hb_bytes(255, 190);
  t.push_outbound(hb.data(), hb.size());
  auto cal_pkt = hb_bytes(1, 191);
  t.push_cal(cal_pkt, TagCtx{11, 22, 0});
  CHECK(t.flush() == 2);
  REQUIRE(calf.size() == 1);
  REQUIRE(live.size() == 1);
  auto fc = parse_mav(calf[0].data(), calf[0].size());
  auto fl = parse_mav(live[0].data(), live[0].size());
  REQUIRE(fc.has_value() && fl.has_value());
  CHECK(fc->payload == cal_pkt);
  CHECK(fl->payload == hb);
}

MTEST_MAIN
