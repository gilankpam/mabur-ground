// UpgradeRunner against a fake gs-upgrade: the process and file side of the
// upgrade flow.
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

#include "mtest.h"
#include "upgrade_runner.h"

using namespace maburplay;

namespace {
std::string fake() { return std::string(MABUR_FIXTURE_DIR) + "/fake-gs-upgrade.sh"; }

std::string fresh_dir() {
  static int n = 0;
  std::string d = std::string(MABUR_TEST_SCRATCH_DIR) + "/upgrade-runner-" +
                  std::to_string(::getpid()) + "-" + std::to_string(n++);
  std::string cmd = "rm -rf " + d + " && mkdir -p " + d;
  REQUIRE(std::system(cmd.c_str()) == 0);
  return d;
}

void put(const std::string& path, const std::string& text) {
  std::ofstream(path) << text;
}

void clear_env() {
  for (const char* v : {"FAKE_SLEEP", "FAKE_FIND_N", "FAKE_CHECK_ERR", "FAKE_RELEASE", "FAKE_START_ERR"})
    ::unsetenv(v);
}

template <class F>
auto wait_for(F poll) {
  auto r = poll();
  for (int i = 0; i < 500 && !r; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    r = poll();
  }
  return r;
}
}  // namespace

TEST(find_reads_version_and_changelog_per_package) {
  clear_env();
  ::setenv("FAKE_FIND_N", "2", 1);
  UpgradeRunner r(fake(), fresh_dir());
  REQUIRE(r.find());
  auto res = wait_for([&] { return r.poll_find(); });
  REQUIRE(res);
  REQUIRE(res->pkgs.size() == 2);
  CHECK(res->pkgs[0].path == "/media/dvr/pkg1.tar.gz");
  REQUIRE(res->pkgs[1].ver);
  CHECK(res->pkgs[1].ver->version == "v0.2.0");
  REQUIRE(res->pkgs[0].changelog.size() == 2);
  CHECK(res->pkgs[0].changelog[0] == "change one of 1");
  CHECK(!r.busy());
}

TEST(find_is_busy_until_the_child_exits) {
  clear_env();
  ::setenv("FAKE_SLEEP", "0.3", 1);
  UpgradeRunner r(fake(), fresh_dir());
  REQUIRE(r.find());
  CHECK(r.busy());
  CHECK(!r.find());
  CHECK(!r.poll_find());
  auto res = wait_for([&] { return r.poll_find(); });
  CHECK(res);
  CHECK(!r.busy());
}

TEST(check_parses_the_release) {
  clear_env();
  std::string rel = std::string(MABUR_FIXTURE_DIR) + "/release_latest.json";
  ::setenv("FAKE_RELEASE", rel.c_str(), 1);
  UpgradeRunner r(fake(), fresh_dir());
  REQUIRE(r.check("stable"));
  auto res = wait_for([&] { return r.poll_check("radxa_zero3"); });
  REQUIRE(res);
  REQUIRE(res->rel);
  CHECK(res->rel->version == "v0.2.0");
  CHECK(res->error.empty());
}

TEST(check_error_codes_map_to_fixed_texts) {
  clear_env();
  struct Row { const char* chan; const char* err; const char* text; } rows[] = {
      {"stable", "clock x", "Clock not synced yet \xe2\x80\x94 try again in a minute"},
      {"stable", "network x", "No network \xe2\x80\x94 set wifi.toml to client mode"},
      {"snapshot", "notfound x", "No snapshot right now \xe2\x80\x94 try again in a few minutes"},
      {"stable", "notfound No release found", "No release found"},
      {"stable", "http HTTP 500", "HTTP 500"},
  };
  for (auto& row : rows) {
    ::setenv("FAKE_CHECK_ERR", row.err, 1);
    UpgradeRunner r(fake(), fresh_dir());
    REQUIRE(r.check(row.chan));
    auto res = wait_for([&] { return r.poll_check("radxa_zero3"); });
    REQUIRE(res);
    CHECK(!res->rel);
    CHECK(res->error == row.text);
  }
}

TEST(start_reports_started_or_the_busy_text) {
  clear_env();
  UpgradeRunner r(fake(), fresh_dir());
  std::string why;
  UpgradeCmd c;
  c.kind = UpgradeCmd::Kind::kStartOnline;
  c.arg = "https://example.invalid/x.tar.gz";
  c.version = "v0.2.0";
  CHECK(r.start(c, &why));
  auto st = r.status();
  REQUIRE(st);
  CHECK(st->state == "running");
  ::setenv("FAKE_START_ERR", "busy An update is already running", 1);
  CHECK(!r.start(c, &why));
  CHECK(why == "An update is already running");
}

TEST(child_sees_the_runners_dir_in_its_environment) {
  clear_env();
  ::setenv("GS_UPGRADE_DIR", "/wrong", 1);
  ::setenv("FAKE_START_ERR", "ECHO_DIR", 1);
  std::string d = fresh_dir();
  UpgradeRunner r(fake(), d);
  UpgradeCmd c;
  c.kind = UpgradeCmd::Kind::kStartFile;
  c.arg = "/x";
  std::string why;
  CHECK(!r.start(c, &why));
  CHECK(why == d);
  ::unsetenv("GS_UPGRADE_DIR");
}

TEST(cancel_runs_the_verb) {
  clear_env();
  std::string d = fresh_dir();
  UpgradeRunner r(fake(), d);
  CHECK(r.cancel());
  CHECK(::access((d + "/cancel").c_str(), F_OK) == 0);
}

TEST(ui_ready_is_wanted_at_stop_until_marked) {
  clear_env();
  std::string d = fresh_dir();
  UpgradeRunner r(fake(), d);
  CHECK(!r.status());
  CHECK(!r.wants_ui_ready());
  put(d + "/status", "state=running\nphase=write\ndone=0\ntotal=0\nmsg=\n");
  CHECK(!r.wants_ui_ready());
  put(d + "/status", "state=running\nphase=stop\ndone=0\ntotal=0\nmsg=\n");
  CHECK(r.wants_ui_ready());
  std::string why;
  r.mark_ui_ready(&why);  // mlockall may be refused without privilege
  CHECK(::access((d + "/ui-ready").c_str(), F_OK) == 0);
  CHECK(!r.wants_ui_ready());
}

MTEST_MAIN
