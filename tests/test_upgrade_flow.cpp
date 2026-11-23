// UpgradeFlow: the update pages' keys, commands and page contents.
#include "mtest.h"
#include "upgrade_flow.h"

using namespace maburplay;
using K = UpgradeKey;

namespace {
FoundPackage pkg(const std::string& path, const std::string& v, const std::string& board = "radxa_zero3",
                 std::vector<std::string> cl = {}) {
  return {path, PackageVersion{v, board, "v0.2.0"}, std::move(cl)};
}
UpgradeFlow flow() { return UpgradeFlow("v0.1.0", "radxa_zero3"); }
UpgradeFlow running(const std::string& phase) {
  UpgradeFlow f = flow();
  f.show_packages({pkg("/a.tgz", "v0.2.0")});
  f.key(K::kRight);
  f.key(K::kRight);
  UpgradeStatus st;
  st.state = "running";
  st.phase = phase;
  f.on_status(st);
  return f;
}
}  // namespace

TEST(no_packages) {
  auto f = flow();
  f.show_packages({});
  CHECK(!f.active());
  CHECK(f.toast() == "Put radxa_zero3.tar.gz on the SD card or DVR partition");
}

TEST(one_package_goes_straight_to_page) {
  auto f = flow();
  f.show_packages({pkg("/a.tgz", "v0.2.0", "radxa_zero3", {"one", "two"})});
  auto p = f.page();
  CHECK(f.active() && p.shown);
  CHECK(p.header == "v0.1.0 → v0.2.0");
  CHECK(p.lines.size() == 2);
}

TEST(package_without_version) {
  auto f = flow();
  f.show_packages({FoundPackage{"/a.tgz", std::nullopt, {}}});
  CHECK(f.page().header == "version unknown");
  CHECK(f.page().lines.empty());
  CHECK(f.key(K::kRight).kind == UpgradeCmd::Kind::kNone);
  CHECK(f.page().confirm == "Install a.tgz? The GS reboots. Keep the power on.");
  auto c = f.key(K::kRight);
  CHECK(c.kind == UpgradeCmd::Kind::kStartFile);
  CHECK(c.arg == "/a.tgz");
}

TEST(several_packages_list) {
  auto f = flow();
  f.show_packages({pkg("/a.tgz", "v0.2.0"), pkg("/b.tgz", "v0.3.0")});
  CHECK(f.page().cursor == 0 && f.page().lines.size() == 2);
  CHECK(f.page().lines[1] == "v0.3.0  b.tgz");  // file name only, not the path
  f.key(K::kDown);
  CHECK(f.page().cursor == 1);
  f.key(K::kRight);
  CHECK(f.page().header == "v0.1.0 → v0.3.0");
}

TEST(wrong_board_package) {
  auto f = flow();
  f.show_packages({pkg("/a.tgz", "v0.2.0", "orangepi_zero2w")});
  CHECK(f.page().error == "Built for orangepi_zero2w");
  f.key(K::kRight);
  CHECK(f.page().confirm.empty());
  CHECK(f.key(K::kRight).kind == UpgradeCmd::Kind::kNone);
}

TEST(older_and_snapshot_headers) {
  UpgradeFlow f("v0.2.0", "radxa_zero3");
  f.show_packages({pkg("/a.tgz", "v0.1.9")});
  CHECK(f.page().header == "v0.2.0 → v0.1.9 · older" && f.page().header_warn);
  f.show_packages({pkg("/a.tgz", "snapshot-abc")});
  CHECK(f.page().header_warn);
  f.show_packages({pkg("/a.tgz", "v0.2.0")});
  CHECK(f.page().header == "already on v0.2.0");
}

TEST(release_pairing) {
  auto f = flow();
  ReleaseInfo r;
  r.version = "v0.2.0";
  r.url = "https://x/y";
  r.drone_unpaired = true;
  f.show_release(r, "stable");
  CHECK(f.page().pair == "pairs with no drone release yet" && f.page().pair_warn);
  CHECK(f.page().title == "Update · Stable");
  r.drone_unpaired = false;
  r.drone = "v0.2.0";
  f.show_release(r, "snapshot");
  CHECK(f.page().pair == "pairs with drone v0.2.0" && !f.page().pair_warn);
  CHECK(f.page().title == "Update · Snapshot");
  f.key(K::kRight);
  CHECK(f.page().confirm == "Install v0.2.0? The GS reboots. Keep the power on.");
}

TEST(online_start_carries_url_and_version) {
  auto f = flow();
  ReleaseInfo r;
  r.version = "v0.2.0";
  r.url = "https://x/y";
  f.show_release(r, "stable");
  f.key(K::kRight);
  auto c = f.key(K::kRight);
  CHECK(c.kind == UpgradeCmd::Kind::kStartOnline && c.arg == "https://x/y" && c.version == "v0.2.0");
}

TEST(confirm_text) {
  auto f = flow();
  f.show_packages({pkg("/a.tgz", "v0.2.0")});
  f.key(K::kRight);
  CHECK(f.page().confirm == "Install v0.2.0? The GS reboots. Keep the power on.");
  f.key(K::kLeft);
  CHECK(f.page().confirm.empty() && f.active());
}

TEST(cancel_only_before_stop) {
  auto f = running("unpack");
  CHECK(f.key(K::kLeft).kind == UpgradeCmd::Kind::kCancel);
  auto w = running("write");
  const auto before = w.page();
  for (K k : {K::kUp, K::kDown, K::kLeft, K::kRight}) CHECK(w.key(k).kind == UpgradeCmd::Kind::kNone);
  CHECK(w.page() == before);
  CHECK(w.page().phase_text == "Writing");
}

TEST(progress_pct) {
  auto f = running("download");
  UpgradeStatus st;
  st.state = "running";
  st.phase = "download";
  st.done = 50;
  st.total = 200;
  f.on_status(st);
  CHECK(f.page().pct == 25);
  st.total = 0;
  f.on_status(st);
  CHECK(f.page().pct == -1);
}

TEST(write_error_text) {
  auto f = running("write");
  UpgradeStatus st;
  st.state = "error";
  st.phase = "write";
  st.msg = "dd failed";
  f.on_status(st);
  CHECK(f.page().error == "Write failed — reflash from SD card");
  CHECK(f.key(K::kLeft).kind == UpgradeCmd::Kind::kNone && !f.active());
  auto g = running("verify");
  st.phase = "verify";
  st.msg = "bad checksum";
  g.on_status(st);
  CHECK(g.page().error == "bad checksum");
  st.state = "cancelled";
  auto h = running("unpack");
  h.on_status(st);
  CHECK(h.active() && h.page().pct == -1);
  h.key(K::kRight);
  CHECK(!h.active());
}

TEST(running_only_while_the_flow_runs) {
  auto f = flow();
  f.show_packages({pkg("/a.tgz", "v0.2.0"), pkg("/b.tgz", "v0.3.0")});
  CHECK(!f.page().running);  // package list
  f.key(K::kRight);
  CHECK(!f.page().running);  // package page
  f.key(K::kRight);
  CHECK(!f.page().confirm.empty() && !f.page().running);
  f.key(K::kRight);
  CHECK(f.page().running);  // started, no status yet
  UpgradeStatus st;
  st.state = "running";
  st.phase = "unpack";
  f.on_status(st);
  CHECK(f.page().running);
  st.state = "cancelled";
  f.on_status(st);
  CHECK(f.page().phase_text == "Cancelled" && !f.page().running);
  auto e = running("verify");
  st.state = "error";
  st.phase = "verify";
  st.msg = "bad checksum";
  e.on_status(st);
  CHECK(e.page().error == "bad checksum" && !e.page().running);
}

TEST(scroll_clamps) {
  auto f = flow();
  std::vector<std::string> cl(20, "x");
  f.show_packages({pkg("/a.tgz", "v0.2.0", "radxa_zero3", cl)});
  for (int i = 0; i < 30; ++i) f.key(K::kDown);
  CHECK(f.page().first == 20 - UpgradeFlow::kBodyRows);
  for (int i = 0; i < 30; ++i) f.key(K::kUp);
  CHECK(f.page().first == 0);
}

TEST(generation_moves) {
  auto f = flow();
  f.show_packages({pkg("/a.tgz", "v0.2.0"), pkg("/b.tgz", "v0.3.0")});
  auto g = f.generation();
  f.key(K::kDown);
  CHECK(f.generation() > g);
  g = f.generation();
  f.key(K::kUp);
  f.key(K::kRight);
  CHECK(f.generation() > g);
  g = f.generation();
  f.key(K::kUp);  // nothing to scroll: page unchanged
  CHECK(f.generation() == g);
}

TEST(keys_on_inactive_flow_are_noops) {
  auto f = flow();
  for (int pass = 0; pass < 2; ++pass) {
    const auto g = f.generation();
    for (K k : {K::kUp, K::kDown, K::kLeft, K::kRight}) CHECK(f.key(k).kind == UpgradeCmd::Kind::kNone);
    CHECK(!f.active() && f.generation() == g);
    f.show_packages({});
  }
}

TEST(status_ignored_unless_running) {
  auto f = flow();
  f.show_packages({pkg("/a.tgz", "v0.2.0")});
  UpgradeStatus st;
  st.state = "error";
  st.msg = "boom";
  auto before = f.page();
  auto g = f.generation();
  f.on_status(st);
  CHECK(f.page() == before && f.generation() == g);
  f.key(K::kRight);
  before = f.page();
  g = f.generation();
  f.on_status(st);
  CHECK(f.page() == before && f.generation() == g);
}

TEST(stop_and_reboot_ignore_keys) {
  for (const char* ph : {"stop", "reboot"}) {
    auto f = running(ph);
    const auto before = f.page();
    const auto g = f.generation();
    for (K k : {K::kUp, K::kDown, K::kLeft, K::kRight}) CHECK(f.key(k).kind == UpgradeCmd::Kind::kNone);
    CHECK(f.page() == before && f.generation() == g);
  }
}

TEST(pad_events_map_to_page_keys) {
  using B = Button;
  using KK = KeyKind;
  auto k = [](B b, KK kind) { return upgrade_key(KeyEvent{b, kind}); };
  CHECK(k(B::kUp, KK::kTap) == K::kUp && k(B::kUp, KK::kRepeat) == K::kUp);
  CHECK(k(B::kDown, KK::kTap) == K::kDown && k(B::kDown, KK::kRepeat) == K::kDown);
  CHECK(k(B::kLeft, KK::kTap) == K::kLeft && !k(B::kLeft, KK::kHold));
  CHECK(k(B::kRight, KK::kTap) == K::kRight && k(B::kRight, KK::kHold) == K::kRight);
  CHECK(k(B::kSel, KK::kTap) == K::kRight && !k(B::kSel, KK::kHold));
}

MTEST_MAIN

namespace {
ReleaseInfo drone_rel() {
  ReleaseInfo r;
  r.version = "v0.4.2";
  r.url = "https://e.invalid/maburd";
  return r;
}
UpgradeFlow drone_running(const UpgradeStatus& st) {
  auto f = flow();
  f.show_drone_release(drone_rel(), "v0.4.0");
  f.key(K::kRight);
  f.key(K::kRight);
  f.on_status(st);
  return f;
}
UpgradeStatus ust(const char* state, const char* phase, uint64_t d = 0, uint64_t t = 0,
                  const char* msg = "") {
  UpgradeStatus s;
  s.state = state;
  s.phase = phase;
  s.done = d;
  s.total = t;
  s.msg = msg;
  return s;
}
}  // namespace

TEST(drone_release_page_header_from_drone_version) {
  auto f = flow();
  f.show_drone_release(drone_rel(), "v0.4.0");
  auto p = f.page();
  CHECK(p.title == "Update · Drone");
  CHECK(p.header == "v0.4.0 → v0.4.2");
}

TEST(drone_confirm_text_and_cmd) {
  auto f = flow();
  f.show_drone_release(drone_rel(), "v0.4.0");
  f.key(K::kRight);
  CHECK(f.page().confirm == "Install v0.4.2 on the drone? Keep the drone and the GS powered.");
  auto c = f.key(K::kRight);
  CHECK(c.kind == UpgradeCmd::Kind::kStartDrone);
  CHECK(c.arg == "https://e.invalid/maburd" && c.version == "v0.4.2");
}

TEST(drone_send_bar) {
  auto f = drone_running(ust("running", "send", 500, 1000));
  auto p = f.page();
  CHECK(p.pct == 50);
  CHECK(p.phase_text == "Sending to the drone");
}

TEST(drone_trial_seconds) {
  auto f = drone_running(ust("running", "trial", 31, 40));
  CHECK(f.page().phase_text == "Trial run \xc2\xb7 31 s");
}

TEST(drone_done_page) {
  auto f = drone_running(ust("done", "result", 0, 0, "Updated to v0.4.2"));
  auto p = f.page();
  CHECK(p.phase_text == "Updated to v0.4.2");
  CHECK(p.error.empty() && !p.running);
  CHECK(p.keys == "◀ close");
  f.key(K::kLeft);
  CHECK(!f.active());
}

TEST(drone_error_page) {
  auto f = drone_running(ust("error", "result", 0, 0, "Rolled back: the new build never linked"));
  CHECK(f.page().error == "Rolled back: the new build never linked");
}

TEST(cancel_allowed_in_send) {
  auto f = drone_running(ust("running", "send", 1, 1000));
  CHECK(f.key(K::kLeft).kind == UpgradeCmd::Kind::kCancel);
  auto g = drone_running(ust("running", "trial", 31, 40));
  CHECK(g.key(K::kLeft).kind == UpgradeCmd::Kind::kNone);
}

TEST(cancel_hidden_once_every_byte_is_sent) {
  auto f = drone_running(ust("running", "send", 999, 1000));
  CHECK(f.page().keys == "◀ cancel");
  f.on_status(ust("running", "send", 1000, 1000));
  CHECK(f.page().keys.empty());
  CHECK(f.key(K::kLeft).kind == UpgradeCmd::Kind::kNone);
}
