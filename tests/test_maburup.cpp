// maburup, the binary, against a UDP responder that plays the drone with the
// real OtaServer (fake_ota_drone.h) in real time.
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "fake_ota_drone.h"
#include "mtest.h"

using namespace fake_ota;

namespace {

uint64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// Binds 127.0.0.1:0; answers each datagram's sender, one reply per datagram.
struct Responder {
  FakeDrone drone;
  int fd, port = 0;
  std::atomic<bool> stop{false};
  std::atomic<int> datagrams{0};
  std::thread th;
  explicit Responder(std::string outcome)
      : drone(std::move(outcome), 3, 0, {"v0.9.0", "board-x", "8812eu"}) {
    fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
    socklen_t l = sizeof a;
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &l);
    port = ntohs(a.sin_port);
    th = std::thread([this] {
      while (!stop) {
        pollfd pf{fd, POLLIN, 0};
        if (::poll(&pf, 1, 20) <= 0) continue;
        uint8_t buf[2048];
        sockaddr_in from{};
        socklen_t fl = sizeof from;
        ssize_t n = ::recvfrom(fd, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl);
        if (n <= 0) continue;
        ++datagrams;
        for (const auto& m : mabur::ota::decode_all(buf, size_t(n)))
          for (const auto& r : drone.handle(m, now_ms())) {
            auto w = mabur::ota::encode(r, 1, 191, mabur::mav::kChanGsRx);
            ::sendto(fd, w.data(), w.size(), 0, reinterpret_cast<sockaddr*>(&from), fl);
          }
      }
    });
  }
  ~Responder() {
    stop = true;
    th.join();
    ::close(fd);
  }
};

struct Out { std::string text; int status; };

Out run(const std::string& args) {
  std::string cmd = std::string(MABURUP_BIN) + " " + args + " 2>&1";
  FILE* p = ::popen(cmd.c_str(), "r");
  Out o{"", -1};
  char b[256];
  while (p && fgets(b, sizeof b, p)) o.text += b;
  int st = p ? ::pclose(p) : -1;
  o.status = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
  return o;
}

std::string last_line(std::string s) {
  while (!s.empty() && s.back() == '\n') s.pop_back();
  auto i = s.rfind('\n');
  return i == std::string::npos ? s : s.substr(i + 1);
}

std::string image_file(std::string& hex) {
  std::string img(5000, 'x');
  for (size_t i = 0; i < img.size(); ++i) img[i] = char(i * 13 + 1);
  char t[] = "/tmp/maburup-img-XXXXXX";
  int fd = mkstemp(t);
  ::close(fd);
  spit(t, img);
  hex = mabur::sha256_hex(*mabur::sha256_file(t));
  return t;
}
}  // namespace

TEST(ident_prints_line) {
  Responder r("committed");
  auto o = run("--port " + std::to_string(r.port) + " ident");
  CHECK(o.text == "version=v0.9.0 board=board-x chip=8812eu\n");
  CHECK(o.status == 0);
}

TEST(install_commits) {
  Responder r("committed");
  std::string hex, f = image_file(hex);
  auto o = run("--port " + std::to_string(r.port) + " install " + f + " " + hex);
  CHECK(o.text.find("send ") != std::string::npos);
  CHECK(last_line(o.text) == "committed");
  CHECK(o.status == 0);
  ::unlink(f.c_str());
}

TEST(install_bad_sha_sends_nothing) {
  Responder r("committed");
  std::string hex, f = image_file(hex);
  auto o = run("--port " + std::to_string(r.port) + " install " + f + " " + std::string(64, '0'));
  CHECK(o.text == "error bad hash\n");
  CHECK(o.status == 1);
  CHECK(r.datagrams == 0);
  ::unlink(f.c_str());
}

TEST(ident_no_drone_exits_2) {
  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);  // bound, never read: nothing answers
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
  socklen_t l = sizeof a;
  ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &l);
  auto o = run("--port " + std::to_string(ntohs(a.sin_port)) + " ident");
  ::close(fd);
  CHECK(o.text == "error no drone\n");
  CHECK(o.status == 2);
}

MTEST_MAIN
