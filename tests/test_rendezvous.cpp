#include "mtest.h"
#include "rendezvous.h"
using namespace maburgs;
using mabur::rc::DiscAck;
using mabur::rc::kAckKeyMismatch;

static VrxRzConfig cfg(uint32_t nonce = 0) { return VrxRzConfig{1000, 20, 149, nonce}; }
static DiscAck ack_for(const VrxRendezvous& rz, uint32_t vtx, uint8_t flags = 0) {
  DiscAck a; a.vrx_nonce = rz.nonce(); a.vtx_nonce = vtx; a.flags = flags; return a;
}

// An unflagged ack for a DISC sent at `now`: the seq echo the window needs.
static DiscAck ok(VrxRendezvous& rz, uint32_t vtx, double now) {
  DiscAck a = ack_for(rz, vtx);
  a.version = mabur::rc::RC_VERSION;
  a.seq = rz.beacon(now).seq;
  return a;
}

TEST(starts_beaconing_and_paces) {
  VrxRendezvous rz(cfg());
  CHECK(rz.state() == VrxState::BEACONING);
  CHECK(rz.tick(0) == VrxAction::Beacon);
  CHECK(rz.tick(10) == VrxAction::Idle);
  CHECK(rz.tick(21) == VrxAction::Beacon);
}

TEST(nonce_is_random_per_instance_unless_pinned) {
  VrxRendezvous a(cfg()), b(cfg());
  CHECK(a.nonce() != b.nonce());
  CHECK(a.nonce() != 0);
  VrxRendezvous p(cfg(0xCAFEF00D));
  CHECK(p.nonce() == 0xCAFEF00D);
  CHECK(p.beacon(0).vrx_nonce == 0xCAFEF00D);
}

TEST(video_alone_never_enters_session) {
  VrxRendezvous rz(cfg());
  rz.feed_video(100);
  CHECK(rz.state() == VrxState::BEACONING);
  CHECK(!rz.vtx_nonce().has_value());
}

TEST(unflagged_ack_adopts_vtx_nonce_and_enters_session) {
  VrxRendezvous rz(cfg());
  bool adopted = false;
  DiscAck wrong; wrong.vrx_nonce = rz.nonce() ^ 1; wrong.vtx_nonce = 5;
  CHECK(!rz.feed_disc_ack(wrong, true, 100, &adopted));
  CHECK(!adopted);
  CHECK(rz.feed_disc_ack(ok(rz, 0xBEEF0001, 100), true, 100, &adopted));
  CHECK(adopted);
  CHECK(rz.state() == VrxState::SESSION);
  CHECK(rz.vtx_nonce() == std::optional<uint32_t>(0xBEEF0001));
  CHECK(rz.tick(150) == VrxAction::TxFeedback);
  // Retry of the same ack: not a new adoption.
  CHECK(rz.feed_disc_ack(ok(rz, 0xBEEF0001, 200), true, 200, &adopted));
  CHECK(!adopted);
  // A restarted drone issues a new nonce: adopted, seq must reset (caller).
  CHECK(rz.feed_disc_ack(ok(rz, 0xBEEF0002, 300), true, 300, &adopted));
  CHECK(adopted);
  CHECK(rz.vtx_nonce() == std::optional<uint32_t>(0xBEEF0002));
}

TEST(link_loss_keeps_vtx_nonce_and_video_resumes_session) {
  VrxRendezvous rz(cfg());
  bool adopted = false;
  rz.feed_disc_ack(ok(rz, 7, 100), true, 100, &adopted);
  CHECK(rz.tick(1200) == VrxAction::Beacon);        // lost at 100 + 1000
  CHECK(rz.state() == VrxState::BEACONING);
  CHECK(rz.vtx_nonce().has_value());
  rz.feed_video(1300);
  CHECK(rz.state() == VrxState::SESSION);
}

// Stranger rule: KEY_MISMATCH needs BOTH a flagged-only run >= kKeyMismatchMs
// old AND >= kKeyMismatchBeacons DISCs sent since its first flagged ack. In
// BEACONING (20 ms DISCs) the time dominates (~1 s); in SESSION (1 s
// keep-alives) the beacon count does (~3 s), so one or two lost acks from
// our own drone beside a stranger do not trip it.
TEST(flagged_acks_need_a_second_and_three_beacons_unflagged_leaves) {
  VrxRendezvous rz(cfg());
  bool adopted = false;
  CHECK(rz.feed_disc_ack(ack_for(rz, 0, kAckKeyMismatch), false, 0, &adopted));
  CHECK(!adopted);
  CHECK(rz.state() == VrxState::BEACONING);
  rz.beacon(0);
  rz.feed_disc_ack(ack_for(rz, 0, kAckKeyMismatch), false, 500, &adopted);
  CHECK(rz.state() == VrxState::BEACONING);
  rz.beacon(0);
  rz.feed_disc_ack(ack_for(rz, 0, kAckKeyMismatch), false, 1001, &adopted);
  CHECK(rz.state() == VrxState::BEACONING);           // >= 1 s, only 2 beacons
  rz.beacon(0);
  rz.feed_disc_ack(ack_for(rz, 0, kAckKeyMismatch), false, 1002, &adopted);
  CHECK(rz.state() == VrxState::KEY_MISMATCH);        // 3 beacons and >= 1 s
  CHECK(!rz.vtx_nonce().has_value());
  CHECK(rz.tick(1021) == VrxAction::Beacon);          // keeps beaconing
  CHECK(rz.tick(1030) == VrxAction::Idle);
  rz.feed_disc_ack(ok(rz, 9, 1100), true, 1100, &adopted);   // corrected key on one end
  CHECK(adopted);
  CHECK(rz.state() == VrxState::SESSION);
}

TEST(flagged_run_needs_the_full_second_however_many_beacons) {
  VrxRendezvous rz(cfg());
  bool adopted = false;
  rz.feed_disc_ack(ack_for(rz, 0, kAckKeyMismatch), false, 0, &adopted);
  for (int i = 0; i < 49; ++i) rz.beacon(0);                 // 20 ms beacons
  rz.feed_disc_ack(ack_for(rz, 0, kAckKeyMismatch), false, 999, &adopted);
  CHECK(rz.state() == VrxState::BEACONING);
  rz.feed_disc_ack(ack_for(rz, 0, kAckKeyMismatch), false, 1000, &adopted);
  CHECK(rz.state() == VrxState::KEY_MISMATCH);
}

TEST(unflagged_ack_resets_both_the_time_and_the_beacon_count) {
  VrxRendezvous rz(cfg());
  bool adopted = false;
  rz.feed_disc_ack(ack_for(rz, 0, kAckKeyMismatch), false, 0, &adopted);
  for (int i = 0; i < 5; ++i) rz.beacon(0);
  rz.feed_disc_ack(ok(rz, 42, 500), true, 500, &adopted);          // ours: run over
  CHECK(rz.state() == VrxState::SESSION);
  rz.feed_disc_ack(ack_for(rz, 0, kAckKeyMismatch), false, 600, &adopted);   // new run
  rz.feed_disc_ack(ack_for(rz, 0, kAckKeyMismatch), false, 1700, &adopted);  // 0 beacons since
  CHECK(rz.state() == VrxState::SESSION);
  for (int i = 0; i < 3; ++i) rz.beacon(0);
  rz.feed_disc_ack(ack_for(rz, 0, kAckKeyMismatch), false, 1701, &adopted);
  CHECK(rz.state() == VrxState::KEY_MISMATCH);
}

// A stranger's flagged acks plus three lost acks from our own (still
// LINKED) drone put us in KEY_MISMATCH while we hold its vtx_nonce. When our drone's
// next ack carries the SAME nonce it is not a new session: reporting it as
// adopted would make the controller zero seq32, and the drone (seq32 must be
// strictly greater) would reject every RCF until its failsafe. Revert = clear
// vtx_nonce_ on entering KEY_MISMATCH: adopted reads true here.
TEST(key_mismatch_keeps_the_held_vtx_nonce_and_same_nonce_is_not_new) {
  VrxRendezvous rz(cfg());
  bool adopted = false;
  rz.feed_disc_ack(ok(rz, 0xBEEF0001, 0), true, 0, &adopted);
  CHECK(adopted);
  rz.feed_disc_ack(ack_for(rz, 0, kAckKeyMismatch), false, 100, &adopted);
  for (int i = 0; i < 3; ++i) rz.beacon(0);
  rz.feed_disc_ack(ack_for(rz, 0, kAckKeyMismatch), false, 1100, &adopted);
  CHECK(rz.state() == VrxState::KEY_MISMATCH);
  CHECK(rz.vtx_nonce() == std::optional<uint32_t>(0xBEEF0001));
  CHECK(rz.tick(1110) != VrxAction::TxFeedback);      // held nonce sends nothing
  rz.feed_video(1120);
  CHECK(rz.state() == VrxState::KEY_MISMATCH);         // video does not leave it
  CHECK(rz.feed_disc_ack(ok(rz, 0xBEEF0001, 1200), true, 1200, &adopted));
  CHECK(!adopted);
  CHECK(rz.state() == VrxState::SESSION);
  CHECK(rz.vtx_nonce() == std::optional<uint32_t>(0xBEEF0001));
}

TEST(stranger_drone_flagged_acks_never_trigger_while_ours_answers) {
  VrxRendezvous rz(cfg());
  bool adopted = false;
  for (double t = 0; t < 5000; t += 20) {
    rz.feed_disc_ack(ack_for(rz, 0, kAckKeyMismatch), false, t, &adopted);      // stranger
    rz.feed_disc_ack(ok(rz, 42, t + 1), true, t + 1, &adopted);                   // ours
    CHECK(rz.state() == VrxState::SESSION);
  }
}

// A DISC_ACK opens SESSION and refreshes the silence timer, but it is
// not video: video_flowing() reads feed_video() only, and only a run of
// it with no gap above kVideoGapMs for kVideoRunMs.
TEST(video_flowing_needs_a_run_of_video_not_an_ack) {
  VrxRendezvous rz(cfg());
  bool adopted = false;
  rz.feed_disc_ack(ok(rz, 42, 1000), true, 1000, &adopted);
  CHECK(rz.state() == VrxState::SESSION);
  CHECK(!rz.video_flowing(1000));
  for (double t = 1000; t < 2500; t += 660) {          // ~1.5 fps trickle
    rz.feed_video(t);
    CHECK(!rz.video_flowing(t));
  }
  double t = 3000;
  for (; t < 3000 + VrxRendezvous::kVideoRunMs; t += 33) {   // 30 fps
    rz.feed_video(t);
    CHECK(!rz.video_flowing(t));
  }
  rz.feed_video(t);
  CHECK(rz.video_flowing(t));
  CHECK(rz.video_flowing(t + VrxRendezvous::kVideoGapMs));
  CHECK(!rz.video_flowing(t + VrxRendezvous::kVideoGapMs + 1));   // video stopped
  rz.feed_video(t + 500);                                          // a new run starts over
  CHECK(!rz.video_flowing(t + 500));
}

TEST(unverified_unflagged_ack_is_not_adopted) {
  VrxRendezvous rz(cfg());
  bool adopted = true;
  CHECK(!rz.feed_disc_ack(ok(rz, 5, 0), false, 10, &adopted));
  CHECK(!adopted);
  CHECK(rz.state() == VrxState::BEACONING);
  CHECK(!rz.vtx_nonce().has_value());
}

TEST(ack_outside_the_2s_seq_window_is_not_adopted) {
  VrxRendezvous rz(cfg());
  bool adopted = false;
  const DiscAck a = ok(rz, 5, 0);                       // seq 1 at t=0
  CHECK(!rz.feed_disc_ack(a, true, 2001, &adopted));
  CHECK(rz.state() == VrxState::BEACONING);
  CHECK(rz.feed_disc_ack(a, true, 1999, &adopted));
  CHECK(adopted);
  DiscAck unsent = ack_for(rz, 6); unsent.version = mabur::rc::RC_VERSION; unsent.seq = 900;
  CHECK(!rz.feed_disc_ack(unsent, true, 1999, &adopted));   // never sent
}

TEST(replayed_old_ack_does_not_roll_back_the_nonce) {
  VrxRendezvous rz(cfg());
  bool adopted = false;
  const DiscAck a = ok(rz, 0xA, 0);
  CHECK(rz.feed_disc_ack(a, true, 10, &adopted));
  CHECK(rz.feed_disc_ack(ok(rz, 0xB, 3000), true, 3010, &adopted));
  CHECK(rz.vtx_nonce() == std::optional<uint32_t>(0xB));
  CHECK(!rz.feed_disc_ack(a, true, 3020, &adopted));
  CHECK(rz.vtx_nonce() == std::optional<uint32_t>(0xB));
}

TEST(verified_other_version_ack_enters_version_mismatch) {
  VrxRendezvous rz(cfg());
  bool adopted = false;
  DiscAck a = ok(rz, 5, 0);
  a.version = 18;
  CHECK(rz.feed_disc_ack(a, true, 10, &adopted));
  CHECK(!adopted);
  CHECK(rz.state() == VrxState::VERSION_MISMATCH);
  CHECK(rz.drone_rc_version() == std::optional<uint8_t>(18));
  CHECK(!rz.vtx_nonce().has_value());
  for (double t = 20; t < 2500; t += 20) CHECK(rz.tick(t) != VrxAction::TxFeedback);
  VrxRendezvous r2(cfg());
  r2.tick(0);
  a = ok(r2, 5, 0); a.version = 18;
  r2.feed_disc_ack(a, true, 10, &adopted);
  CHECK(r2.tick(500) == VrxAction::Idle);               // 1000 ms cadence
  CHECK(r2.tick(1000) == VrxAction::Beacon);
  CHECK(r2.tick(1999) == VrxAction::Idle);
  CHECK(r2.tick(2000) == VrxAction::Beacon);
  for (double t = 2010; t < 2400; t += 33) r2.feed_video(t);
  CHECK(r2.state() == VrxState::VERSION_MISMATCH);      // video never opens SESSION
}

TEST(same_version_verified_ack_leaves_version_mismatch) {
  VrxRendezvous rz(cfg());
  bool adopted = false;
  DiscAck a = ok(rz, 5, 0); a.version = 18;
  rz.feed_disc_ack(a, true, 10, &adopted);
  CHECK(rz.feed_disc_ack(ok(rz, 7, 1000), true, 1010, &adopted));
  CHECK(adopted);
  CHECK(rz.state() == VrxState::SESSION);
  CHECK(!rz.drone_rc_version().has_value());
}

TEST(key_mismatch_outranks_version_mismatch) {
  VrxRendezvous rz(cfg());
  bool adopted = false;
  for (int i = 0; i < 3; ++i) rz.beacon(0);
  DiscAck f = ack_for(rz, 0, kAckKeyMismatch); f.version = 18;
  rz.feed_disc_ack(f, false, 0, &adopted);
  for (int i = 0; i < 3; ++i) rz.beacon(500);
  rz.feed_disc_ack(f, false, 1000, &adopted);
  CHECK(rz.state() == VrxState::KEY_MISMATCH);
  CHECK(!rz.drone_rc_version().has_value());
}

TEST(version_mismatch_then_key_mismatch_clears_the_drone_version) {
  VrxRendezvous rz(cfg());
  bool adopted = false;
  DiscAck a = ok(rz, 5, 0); a.version = 18;
  rz.feed_disc_ack(a, true, 10, &adopted);
  REQUIRE(rz.state() == VrxState::VERSION_MISMATCH);
  DiscAck f = ack_for(rz, 0, kAckKeyMismatch); f.version = 18;
  rz.feed_disc_ack(f, false, 100, &adopted);
  for (int i = 0; i < 3; ++i) rz.beacon(500);
  rz.feed_disc_ack(f, false, 1200, &adopted);
  CHECK(rz.state() == VrxState::KEY_MISMATCH);
  CHECK(!rz.drone_rc_version().has_value());
}

// The mismatched drone goes away: with no verified other-version ack for
// three keep-alive periods the GS falls back to BEACONING (20 ms DISCs),
// so the channel search can resume. Acks at the keep-alive rate hold it.
TEST(version_mismatch_falls_back_to_beaconing_without_acks) {
  VrxRendezvous rz(cfg());
  DiscAck a = ok(rz, 5, 0); a.version = 18;
  rz.feed_disc_ack(a, true, 10, nullptr);
  REQUIRE(rz.state() == VrxState::VERSION_MISMATCH);
  for (double t = 1000; t <= 5000; t += 1000) {
    rz.tick(t);
    DiscAck b = ok(rz, 5, t); b.version = 18;
    rz.feed_disc_ack(b, true, t + 10, nullptr);
    CHECK(rz.state() == VrxState::VERSION_MISMATCH);
  }
  rz.tick(8009);                                   // 2999 ms after the last ack
  CHECK(rz.state() == VrxState::VERSION_MISMATCH);
  rz.tick(8010);                                   // 3 keep-alives without one
  CHECK(rz.state() == VrxState::BEACONING);
  CHECK(!rz.drone_rc_version().has_value());
  CHECK(rz.tick(8030) == VrxAction::Beacon);        // fast cadence again
}

// A verified other-version ack is acted on only when our own drone has gone
// quiet: no same-version verified ack and no same-version Telem
// (note_peer_alive) in the last kPeerAliveMs. A same-key spare drone at
// another version must not take down a live session.
TEST(other_version_ack_is_ignored_while_the_same_version_drone_is_alive) {
  VrxRendezvous rz(cfg());
  bool adopted = false;
  REQUIRE(rz.feed_disc_ack(ok(rz, 7, 0), true, 10, &adopted));
  REQUIRE(rz.state() == VrxState::SESSION);
  DiscAck a = ok(rz, 9, 500); a.version = 18;
  CHECK(!rz.feed_disc_ack(a, true, 510, &adopted));    // 500 ms after our ack
  CHECK(rz.state() == VrxState::SESSION);
  CHECK(rz.ignored_version_acks() == 1);
  rz.note_peer_alive(1900);                            // a same-version Telem
  a = ok(rz, 9, 3800); a.version = 18;
  CHECK(!rz.feed_disc_ack(a, true, 3810, &adopted));   // 1910 ms after the Telem
  CHECK(rz.state() == VrxState::SESSION);
  CHECK(rz.ignored_version_acks() == 2);
  a = ok(rz, 9, 3900); a.version = 18;
  CHECK(rz.feed_disc_ack(a, true, 3910, &adopted));    // 2010 ms: our drone is gone
  CHECK(rz.state() == VrxState::VERSION_MISMATCH);
  CHECK(rz.ignored_version_acks() == 2);
}

MTEST_MAIN
