// In-process loopback of a whole drone update across RC versions: the GS's
// VrxController, MavTunnel, DroneIdent and UpdateClient against the drone's
// RcAgent, MavComponent and OtaServer, with a radio in between that makes
// each end a peer of another RC_VERSION (every DISC, DISC_ACK, T_MAV and
// T_MAV_UP rewritten to RC_VERSION - 1, its tag recomputed). This is the
// only place both ends of an update-only session run against each other.
//
// The harness is deliberately a TRANSCRIPTION of the two call sites, not an
// idealized protocol:
//
//   GS (gs/src/main.cpp): vrx.step() -> the radio; the rc sink hands
//       is_mav_frame(frozen_type) bodies to mav_tunnel.on_rc_frame and the
//       rest to vrx.on_rc_frame, behind the real Aggregator; the MAVLink
//       block per tick: mav_ctx = session_ctx() if linked, update_ctx() if
//       version_mismatch(), else {}; set_session(mav_ctx, version_mismatch);
//       drone_ident.tick(mav_ctx.vtx_nonce) -> push_outbound; the ground
//       app's packets (UpdateClient, as maburup over the endpoint) ->
//       push_outbound; flush(). The deliver lambda feeds drone_ident and the
//       ground app.
//   Drone (drone/src/main.cpp): the RX callback's gate (our version, or a
//       frozen DISC / T_MAV of any version) -> T_MAV to the MAV queue, the
//       rest to the agent thread's on_rc_frame; agent.tick every 100 ms;
//       the MAV thread's pass: check_mav verdicts (kOk / kOkUpdate set
//       reply_to_update; kUpdateDropped is the agent's count) ->
//       comp.on_payload (which routes the update lane to OtaServer::on_msg);
//       poll_results; the 1 Hz HEARTBEAT only with a published session; the
//       reply pair update_ctx() after a kOkUpdate, else uplink_ctx(false);
//       unpaced with only the update pair open; one take_outbound per pass
//       -> pack_mav(T_MAV_UP) under the shared counter; exit once the
//       replies are gone or after the grace.
//   Restart: what S96mabur does for a go file (fake_ota_drone.h), then a
//       new process: a new RcAgent (a new vtx_nonce), a new MavComponent on
//       the trial OtaServer, the T_MAV_UP counter back at 0.
//
// Not transcribed: the GS's RCF slotter (a T_MAV frame goes straight to the
// radio here) and the calibration lane (test_cal_e2e.cpp).
//
// If a fix has to change one of those call sites, it has to change here
// too -- which is the point.
#include <cstdio>
#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "aggregator.h"
#include "drone_ident.h"
#include "fake_ota_drone.h"
#include "mabur/link_key.h"
#include "mabur/mav.h"
#include "mabur/node.h"
#include "mabur/rc_proto.h"
#include "mabur/uep_encoder.h"
#include "mav_component.h"
#include "mav_tunnel.h"
#include "mtest.h"
#include "rc_agent.h"
#include "rc_at_version.h"
#include "update_client.h"
#include "vrx_controller.h"

using namespace mabur;

namespace {

constexpr uint8_t kOld = rc::RC_VERSION - 1;
constexpr uint64_t kOtaExitGraceMs = 1000;   // drone main's grace
constexpr uint64_t kBootGapMs = 1500;        // process gone between exit and the trial

struct LoopActuator : Actuator {
  std::vector<std::vector<uint8_t>> controls;
  void apply_op(const AppliedOp&) override {}
  void send_control(const std::vector<uint8_t>& b) override { controls.push_back(b); }
  bool set_bitrate_kbps(int) override { return true; }
  bool set_roi_qp(int) override { return true; }
  bool set_fps(int) override { return true; }
  void request_idr() override {}
  void retune(uint8_t, const char*) override {}
  bool set_record(bool) override { return true; }
};

Config drone_cfg() {
  Config cfg;
  cfg.link.key = kDefaultLinkKey;
  cfg.link.failsafe_ms = 1000;
  cfg.link.rendezvous_ms = 30000;
  cfg.link.tick_ms = 100;
  cfg.link.move_confirm_ms = 2000;
  cfg.radio.channels = {136, 149, 161};
  return cfg;
}

maburgs::VrxCfg gs_cfg() {
  maburgs::VrxCfg v;
  v.key = kDefaultLinkKey;
  v.op_channel = 136;
  v.ladder = maburgs::LadderCfg{{{0, 0.5, 0.25}, {1, 0.5, 0.25}, {2, 0.5, 0.25}}};
  return v;
}

std::vector<uint8_t> image(size_t n, uint8_t seed) {
  std::vector<uint8_t> v(n);
  for (size_t i = 0; i < n; ++i) v[i] = uint8_t(seed + i * 7 + (i >> 8));
  return v;
}

Sha256 sha_of(const std::vector<uint8_t>& v) {
  Sha256Ctx c;
  c.update(v.data(), v.size());
  return c.final();
}

std::vector<uint8_t> heartbeat() {
  mavlink_message_t msg;
  mavlink_msg_heartbeat_pack(255, 190, &msg, MAV_TYPE_GCS, MAV_AUTOPILOT_INVALID, 0, 0,
                             MAV_STATE_ACTIVE);
  uint8_t buf[MAVLINK_MAX_PACKET_LEN];
  const uint16_t n = mavlink_msg_to_send_buffer(buf, &msg);
  return std::vector<uint8_t>(buf, buf + n);
}

ApplyPort no_applier() {
  ApplyPort p;
  p.submit = [](size_t, const mavparam::Value&, std::string* why) {
    if (why) *why = "no applier in this test";
    return false;
  };
  p.take_result = [](ConfigApplyResult*) { return false; };
  p.busy = [] { return false; };
  return p;
}

// One maburd process: what dies on exit and is built again for the trial.
struct DroneProc {
  LoopActuator act;
  RcAgent agent;
  MavComponent comp;
  std::deque<std::vector<uint8_t>> rc_queue, mav_queue;
  uint32_t mav_up_counter = 0;
  uint64_t last_hb = 0, last_up = 0, next_tick = 0;
  bool reply_to_update = false;
  std::optional<uint64_t> exit_seen_ms;
  DroneProc(const Config& cfg, OtaServer& ota)
      : agent(cfg, act), comp("/nonexistent/maburd.toml", no_applier(), nullptr, &ota) {}
};

struct Rig {
  Config cfg = drone_cfg();
  fake_ota::FakeDrone fake;
  std::unique_ptr<DroneProc> drone;
  uint64_t down_until = 0;   // the drone is between processes until then
  uint64_t update_dropped = 0, restarts = 0;

  maburgs::VrxController vrx{gs_cfg()};
  std::array<UepLayerCfg, 2> layers = [] {
    std::array<UepLayerCfg, 2> l{};
    for (auto& lc : l) lc = UepLayerCfg{SwConfig{64, 128, 1.0}, 4};
    return l;
  }();
  maburgs::Aggregator agg{layers, 512, 1};
  rc::TagCtx mav_ctx{};
  maburgs::DroneIdent drone_ident;
  maburgs::UpdateClient* client = nullptr;   // the ground app, when one runs
  maburgs::MavTunnel tunnel{
      kDefaultLinkKey,
      [this](std::vector<uint8_t> f) { to_drone(rc_test::mav_at_version(f, kOld, kDefaultLinkKey, mav_ctx)); },
      [this](const uint8_t* b, size_t n) {
        drone_ident.on_packet(b, n);
        if (client) client->on_packet(b, n, now);
      }};
  uint16_t mac_seq = 0;
  uint64_t now = 0;
  int version_mismatch_ticks = 0;

  explicit Rig(std::string outcome) : fake(std::move(outcome)) {
    drone = std::make_unique<DroneProc>(cfg, fake.server());
    agg.set_rc_sink([this](uint8_t, const std::vector<uint8_t>& f, uint64_t us) {
      if (maburgs::is_mav_frame(rc::frozen_type(f.data(), f.size()))) {
        tunnel.on_rc_frame(f.data(), f.size());
        return;
      }
      vrx.on_rc_frame(f.data(), f.size(), static_cast<double>(us) / 1000.0);
    });
  }

  uint64_t total_update_dropped() const { return update_dropped + drone->agent.update_dropped(); }

  // The radio, GS -> drone, then the drone's RX callback gate.
  void to_drone(const std::vector<uint8_t>& body) {
    if (now < down_until) return;
    const int rc_type = rc::frame_type(body.data(), body.size());
    const int frozen = rc_type >= 0 ? rc_type : rc::frozen_type(body.data(), body.size());
    if (rc_type >= 0 || frozen == rc::T_DISC || frozen == rc::T_MAV) {
      if (rc_type == rc::T_NACK) return;
      (frozen == rc::T_MAV ? drone->mav_queue : drone->rc_queue).push_back(body);
    }
  }

  // The radio, drone -> GS, into the GS's receive path.
  void to_gs(const std::vector<uint8_t>& body) {
    node::RxBody m;
    m.mono_us = now * 1000;
    m.mac_seq = mac_seq++;
    m.crc_ok = true;
    m.body = body;
    agg.on_rx_body(m);
  }

  void gs_step(const std::vector<std::vector<uint8_t>>& app_packets) {
    const double t = static_cast<double>(now);
    if (auto out = vrx.step(t, maburgs::LinkHealth{true, 0.0, 0.0, false}))
      to_drone(out->is_disc ? rc_test::disc_at_version(out->frame, kOld, kDefaultLinkKey)
                            : rc_test::untagged_at_version(out->frame, kOld));
    const bool mav_linked = vrx.link_state() == maburgs::VrxState::SESSION;
    const bool update_only = vrx.version_mismatch();
    if (update_only) ++version_mismatch_ticks;
    mav_ctx = mav_linked ? vrx.session_ctx() : update_only ? vrx.update_ctx() : rc::TagCtx{};
    tunnel.set_session(mav_ctx, update_only);
    if (auto p = drone_ident.tick(mav_ctx.vtx_nonce, now)) tunnel.push_outbound(p->data(), p->size());
    for (const auto& p : app_packets) tunnel.push_outbound(p.data(), p.size());
    tunnel.flush();
  }

  // The drone's agent thread and one MAV thread pass.
  void drone_step() {
    if (now < down_until) return;
    DroneProc& d = *drone;
    while (!d.rc_queue.empty()) {
      d.agent.on_rc_frame(d.rc_queue.front().data(), d.rc_queue.front().size(), now);
      d.rc_queue.pop_front();
    }
    if (now >= d.next_tick) {
      d.agent.tick(now, RadioHealth{});
      d.next_tick = now + 100;
    }
    for (auto& c : d.act.controls)
      to_gs(rc::frozen_type(c.data(), c.size()) == rc::T_DISC_ACK
                ? rc_test::ack_at_version(c, kOld, kDefaultLinkKey)
                : rc_test::untagged_at_version(c, kOld));
    d.act.controls.clear();

    while (!d.mav_queue.empty()) {
      const std::vector<uint8_t> body = std::move(d.mav_queue.front());
      d.mav_queue.pop_front();
      rc::Mav m;
      switch (d.agent.check_mav(body.data(), body.size(), &m, /*cal_run_open=*/false)) {
        case RcAgent::MavCheck::kOk:
          d.reply_to_update = false;
          d.comp.on_payload(m.payload.data(), m.payload.size(), now);
          break;
        case RcAgent::MavCheck::kOkUpdate:
          d.reply_to_update = true;
          for (const auto& msg : ota::decode_all(m.payload.data(), m.payload.size()))
            fake.observe(msg);
          d.comp.on_payload(m.payload.data(), m.payload.size(), now);
          break;
        default:
          break;
      }
    }
    d.comp.poll_results();
    const bool linked = d.agent.has_published_session();
    if (linked && now - d.last_hb >= 1000) {
      d.comp.queue_heartbeat();
      d.last_hb = now;
    }
    std::optional<rc::TagCtx> ctx = d.reply_to_update ? d.agent.update_ctx() : std::nullopt;
    if (!ctx) ctx = d.agent.uplink_ctx(false);
    const uint64_t pace_ms = linked ? kMavUplinkMinMs : 0;
    if ((linked || d.agent.update_session_open()) && ctx && d.comp.outbound_packets() > 0 &&
        now - d.last_up >= pace_ms) {
      rc::Mav m;
      m.type = rc::T_MAV_UP;
      m.counter = d.mav_up_counter++;
      d.comp.take_outbound(&m.payload);
      ctx->seq32 = m.counter;
      to_gs(rc_test::mav_at_version(rc::pack_mav(m, kDefaultLinkKey, *ctx), kOld, kDefaultLinkKey, *ctx));
      d.last_up = now;
    }
    if (d.comp.exit_requested()) {
      if (!d.exit_seen_ms) d.exit_seen_ms = now;
      if (d.comp.outbound_packets() == 0 || now - *d.exit_seen_ms >= kOtaExitGraceMs) {
        update_dropped += d.agent.update_dropped();
        drone.reset();
        down_until = now + kBootGapMs;
      }
    }
  }

  // The restart loop brings the next process up once the gap has passed.
  void maybe_boot() {
    if (drone || now < down_until) return;
    fake.restart(now);
    drone = std::make_unique<DroneProc>(cfg, fake.server());
    ++restarts;
  }

  void step(const std::vector<std::vector<uint8_t>>& app_packets = {}) {
    maybe_boot();
    gs_step(app_packets);
    if (drone) drone_step();
    now += 10;
  }

  // Runs `c` until it is done or `max_ms` have passed; `on_step` may add
  // packets of its own (another ground app on the same endpoint).
  void run(maburgs::UpdateClient& c, uint64_t max_ms,
           const std::function<void(std::vector<std::vector<uint8_t>>&)>& on_step = nullptr) {
    client = &c;
    const uint64_t end = now + max_ms;
    while (!c.done() && now < end) {
      auto pkts = c.tick(now);
      if (on_step) on_step(pkts);
      step(pkts);
    }
    client = nullptr;
  }

  // Up to VERSION_MISMATCH with the drone's ident in hand.
  void reach_ident(uint64_t max_ms) {
    const uint64_t end = now + max_ms;
    while (now < end && !drone_ident.ident()) step();
  }
};

}  // namespace

TEST(ident_across_versions) {
  Rig r("committed");
  r.reach_ident(2000);
  CHECK(r.vrx.version_mismatch());
  REQUIRE(r.vrx.drone_rc_version().has_value());
  CHECK(*r.vrx.drone_rc_version() == kOld);
  REQUIRE(r.drone_ident.ident().has_value());
  CHECK(r.drone_ident.ident()->version == "v");
  CHECK(r.drone_ident.ident()->board == "b");
  CHECK(r.drone_ident.ident()->chip == "c");
  CHECK(r.drone->agent.state() != RcAgent::State::LINKED);
  std::fprintf(stderr, "ident after %llu ms\n", static_cast<unsigned long long>(r.now));
}

TEST(install_across_versions_commits) {
  Rig r("committed");
  r.reach_ident(2000);
  REQUIRE(r.drone_ident.ident().has_value());
  const uint32_t vtx_before = r.vrx.update_ctx().vtx_nonce;
  REQUIRE(vtx_before != 0);
  auto img = image(20011, 1);
  maburgs::UpdateClient c(img, sha_of(img));
  r.run(c, 200000);
  CHECK(c.done());
  CHECK(c.progress().result == "committed");
  CHECK(r.fake.staged() == std::string(img.begin(), img.end()));
  CHECK(r.restarts == 1);
  CHECK(r.vrx.version_mismatch());
  CHECK(r.vrx.update_ctx().vtx_nonce != 0);
  CHECK(r.vrx.update_ctx().vtx_nonce != vtx_before);
  CHECK(r.total_update_dropped() == 0);
  CHECK(r.tunnel.stats().drop_auth == 0);
  std::fprintf(stderr, "committed at %llu ms\n", static_cast<unsigned long long>(r.now));
}

TEST(bad_hash_refused_across_versions) {
  Rig r("committed");
  r.reach_ident(2000);
  auto img = image(1000, 5);
  maburgs::UpdateClient c(img, sha_of(image(1000, 6)));
  r.run(c, 200000);
  CHECK(c.progress().result == "refused bad_hash");
  CHECK(r.total_update_dropped() == 0);
}

TEST(rollback_across_versions_reported) {
  Rig r("rolled_back exited");
  r.reach_ident(2000);
  auto img = image(1000, 7);
  maburgs::UpdateClient c(img, sha_of(img));
  r.run(c, 200000);
  CHECK(c.progress().result == "rolled_back exited");
  CHECK(r.total_update_dropped() == 0);
}

TEST(heartbeat_from_ground_app_does_not_break_transfer) {
  Rig r("committed");
  r.reach_ident(2000);
  auto img = image(20011, 3);
  maburgs::UpdateClient c(img, sha_of(img));
  const auto hb = heartbeat();
  uint64_t last_hb = 0;
  bool first = true;
  r.run(c, 200000, [&](std::vector<std::vector<uint8_t>>& pkts) {
    if (first || r.now - last_hb >= 1000) {
      pkts.push_back(hb);
      last_hb = r.now;
      first = false;
    }
  });
  CHECK(c.progress().result == "committed");
  CHECK(r.tunnel.stats().drop_not_update > 0);
  CHECK(r.total_update_dropped() == 0);
}

MTEST_MAIN
