// upgrade_info: the parsers for the package VERSION file, the gs-upgrade
// status file and the GitHub release response.
#include <fstream>
#include <sstream>
#include <string>

#include "mtest.h"
#include "upgrade_info.h"

using namespace maburplay;

namespace {
std::string slurp(const std::string& path) {
  std::ifstream f(path);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
std::string fixture() { return slurp(std::string(MABUR_FIXTURE_DIR) + "/release_latest.json"); }
}  // namespace

TEST(version_file_parses_and_allows_empty_drone) {
  auto v = parse_version_file("version=v0.2.0\nboard=radxa_zero3\ndrone=\n");
  REQUIRE(v);
  CHECK(v->version == "v0.2.0");
  CHECK(v->board == "radxa_zero3");
  CHECK(v->drone == "");
  CHECK(!parse_version_file("garbage\nnothing here\n"));
}

TEST(status_round_trips_and_defaults_done) {
  auto s = parse_status("state=running\nphase=download\ndone=10\ntotal=200\nmsg=hello world\n");
  REQUIRE(s);
  CHECK(s->state == "running");
  CHECK(s->phase == "download");
  CHECK(s->done == 10);
  CHECK(s->total == 200);
  CHECK(s->msg == "hello world");
  auto t = parse_status("state=error\n");
  REQUIRE(t);
  CHECK(t->done == 0);
  CHECK(!parse_status("phase=write\n"));
}

TEST(release_json_picks_the_board_asset) {
  std::string why;
  auto r = parse_release_json(fixture(), "radxa_zero3", &why);
  REQUIRE(r);
  CHECK(r->version == "v0.2.0");
  CHECK(r->url == "https://example.invalid/dl/mabur-ground-v0.2.0-radxa_zero3.tar.gz");
  CHECK(r->drone == "v0.2.0");
  CHECK(!r->drone_unpaired);
  CHECK(r->changelog.size() == 2);
  auto o = parse_release_json(fixture(), "orangepi_zero2w", &why);
  REQUIRE(o);
  CHECK(o->url.find("orangepi_zero2w") != std::string::npos);
}

TEST(release_json_without_board_asset_says_why) {
  std::string why;
  CHECK(!parse_release_json(fixture(), "pi5", &why));
  CHECK(why == "no package for pi5");
  CHECK(!parse_release_json("not json", "pi5", &why));
  CHECK(!why.empty());
}

TEST(release_json_unpaired_and_snapshot_version) {
  std::string j = R"({"body":"**Pin contract matches no drone release yet** (x).\n",
    "assets":[{"name":"mabur-ground-snapshot-1a2b3c4d-radxa_zero3.tar.gz",
               "browser_download_url":"u"}]})";
  std::string why;
  auto r = parse_release_json(j, "radxa_zero3", &why);
  REQUIRE(r);
  CHECK(r->version == "snapshot-1a2b3c4d");
  CHECK(r->drone_unpaired);
  CHECK(r->drone == "");
}

TEST(changelog_lines_reads_the_changes_block) {
  auto l = changelog_lines("x\n\n**Changes**\n- one\n- two\n\n| t |\n");
  REQUIRE(l.size() == 2);
  CHECK(l[0] == "one");
  CHECK(l[1] == "two");
  CHECK(changelog_lines("no block\n- stray\n").empty());
}

TEST(version_rel_is_numeric) {
  CHECK(version_rel("v0.1.0", "v0.2.0") == VersionRel::kNewer);
  CHECK(version_rel("v0.2.0", "v0.1.9") == VersionRel::kOlder);
  CHECK(version_rel("v0.2.0", "v0.2.0") == VersionRel::kSame);
  CHECK(version_rel("v0.10.0", "v0.9.0") == VersionRel::kOlder);
  CHECK(version_rel("snapshot-1a2b3c4d", "v0.2.0") == VersionRel::kDifferent);
  CHECK(version_rel("snapshot-aa", "snapshot-aa") == VersionRel::kSame);
}

TEST(os_release_value_unquotes) {
  CHECK(os_release_value("VERSION=\"v0.1.0\"\n", "VERSION") == "v0.1.0");
  CHECK(os_release_value("A=b\nVERSION=plain\n", "VERSION") == "plain");
  CHECK(os_release_value("A=b\n", "VERSION") == "");
}

MTEST_MAIN
