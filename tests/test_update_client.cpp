// UpdateClient against a real drone-side OtaServer and the restart loop.
#include <set>

#include "fake_ota_drone.h"
#include "mtest.h"
#include "update_client.h"

using namespace fake_ota;
using maburgs::UpdateClient;
using maburgs::UpdateProgress;
using mabur::ota::Cmd;
using mabur::ota::Ftp;
using mabur::ota::OtaMsg;
using mabur::ota::Status;
using mabur::ota::UpdateCmd;

namespace {
std::vector<uint8_t> image(size_t n, uint8_t seed) {
  std::vector<uint8_t> v(n);
  for (size_t i = 0; i < n; ++i) v[i] = uint8_t(seed + i * 7 + (i >> 8));
  return v;
}

mabur::Sha256 sha_of(const std::vector<uint8_t>& v) {
  mabur::Sha256Ctx c;
  c.update(v.data(), v.size());
  return c.final();
}

bool is_start(const OtaMsg& m) {
  const auto* u = std::get_if<UpdateCmd>(&m);
  return u && u->cmd == Cmd::kStart;
}

std::string str(const std::vector<uint8_t>& v) { return std::string(v.begin(), v.end()); }
}  // namespace

TEST(installs_to_committed) {
  FakeDrone d("committed");
  auto img = image(20011, 1);
  UpdateClient c(img, sha_of(img));
  Link l;
  l.run(c, d, 200000);
  CHECK(c.done());
  CHECK(c.progress().result == "committed");
  CHECK(d.staged() == str(img));
  CHECK(c.progress().sent == c.progress().total);
  CHECK(c.progress().total == img.size());
}

TEST(lossy_link_still_commits) {
  FakeDrone d("committed", 3, 500);
  auto img = image(20011, 2);
  UpdateClient c(img, sha_of(img));
  Link l;
  l.drop_up = [](uint64_t, const OtaMsg&, size_t i) { return i % 5 == 0; };
  l.drop_down = [](uint64_t, const OtaMsg&, size_t i) { return i % 5 == 0; };
  l.run(c, d, 300000);
  CHECK(c.progress().result == "committed");
  CHECK(d.staged() == str(img));
}

TEST(window_never_exceeds_cfg) {
  FakeDrone d("committed");
  auto img = image(30000, 3);
  UpdateClient c(img, sha_of(img));
  Link l;
  std::set<uint16_t> out;
  size_t peak = 0;
  l.on_up = [&](const OtaMsg& m) {
    const auto* f = std::get_if<Ftp>(&m);
    if (f && f->opcode == 7) out.insert(f->seq);
    peak = std::max(peak, out.size());
  };
  l.on_down = [&](const OtaMsg& m) {
    if (const auto* f = std::get_if<Ftp>(&m)) out.erase(uint16_t(f->seq - 1));
  };
  l.run(c, d, 200000);
  CHECK(c.progress().result == "committed");
  CHECK(peak > 1);
  CHECK(peak <= 32);
}

TEST(start_resent_until_first_reply_then_never) {
  FakeDrone d("committed");
  auto img = image(1000, 4);
  UpdateClient c(img, sha_of(img));
  Link l;
  size_t starts = 0, statuses = 0;
  l.on_up = [&](const OtaMsg& m) { if (is_start(m)) ++starts; };
  l.drop_down = [&](uint64_t, const OtaMsg& m, size_t) {
    if (!std::holds_alternative<Status>(m)) return false;
    return ++statuses <= 2;
  };
  l.run(c, d, 200000);
  CHECK(c.progress().result == "committed");
  CHECK(starts == 3);
  CHECK(d.starts_seen == 3);
}

TEST(bad_hash_refused) {
  FakeDrone d("committed");
  auto img = image(1000, 5);
  UpdateClient c(img, sha_of(image(1000, 6)));
  Link l;
  l.run(c, d, 200000);
  CHECK(c.progress().result == "refused bad_hash");
}

TEST(rollback_reported) {
  FakeDrone d("rolled_back no_link");
  auto img = image(1000, 7);
  UpdateClient c(img, sha_of(img));
  Link l;
  l.run(c, d, 200000);
  CHECK(c.progress().result == "rolled_back no_link");
}

TEST(silent_drone_is_no_drone) {
  FakeDrone d("committed");
  d.mute = true;
  auto img = image(1000, 8);
  UpdateClient c(img, sha_of(img));
  Link l;
  l.run(c, d, 200000);
  CHECK(c.progress().result == "error no drone");
  CHECK(l.now >= 15000 && l.now < 16000);
}

TEST(lost_during_trial) {
  FakeDrone d("committed", 1000000);  // never reaches a result
  auto img = image(1000, 9);
  UpdateClient c(img, sha_of(img));
  Link l;
  size_t t0 = 0;
  l.on_step = [&](uint64_t now) {
    if (c.progress().phase == UpdateProgress::Phase::kTrial) {
      if (!d.mute) t0 = now;
      d.mute = true;
    }
  };
  l.run(c, d, 300000);
  CHECK(c.progress().result == "error lost drone");
  CHECK(l.now - t0 >= 99000 && l.now - t0 <= 101000);
}

TEST(create_busy) {
  FakeDrone d("committed");
  spit(d.dir + "/go", "x\n");
  auto img = image(1000, 10);
  UpdateClient c(img, sha_of(img));
  Link l;
  l.run(c, d, 200000);
  CHECK(c.progress().result == "error drone busy");
}

TEST(blackout_mid_transfer_recovers) {
  FakeDrone d("committed");
  auto img = image(30000, 11);
  UpdateClient c(img, sha_of(img));
  Link l;
  uint64_t from = 0;
  auto black = [&](uint64_t now, const OtaMsg&, size_t) {
    if (!from && c.progress().sent * 2 >= c.progress().total) from = now;
    return from && now >= from && now < from + 3000;
  };
  l.drop_up = black;
  l.drop_down = black;
  l.run(c, d, 300000);
  CHECK(from != 0);
  CHECK(c.progress().result == "committed");
  CHECK(d.staged() == str(img));
}

TEST(second_update_same_power_on) {
  FakeDrone d("committed");
  auto a = image(5000, 12), b = image(7777, 13);
  {
    UpdateClient c(a, sha_of(a));
    Link l;
    l.run(c, d, 200000);
    CHECK(c.progress().result == "committed");
  }
  UpdateClient c(b, sha_of(b));
  Link l;
  l.run(c, d, 200000);
  CHECK(c.progress().result == "committed");
  CHECK(d.staged() == str(b));
}

MTEST_MAIN
