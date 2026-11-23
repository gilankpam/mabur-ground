// maburup -- update the drone over the maburgs MAVLink endpoint.
//   maburup [--port N] ident
//   maburup [--port N] install <file> <sha256-hex>
// Each packet is one UDP datagram to 127.0.0.1:<port>; replies come back on
// the same socket. Output is one line per event, flushed as it happens.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "drone_ident.h"
#include "mabur/sha256.h"
#include "update_client.h"

namespace {

uint64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

struct Sock {
  int fd = -1;
  explicit Sock(int port) {
    fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(uint16_t(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fd >= 0 && ::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
      ::close(fd);
      fd = -1;
    }
  }
  ~Sock() { if (fd >= 0) ::close(fd); }
  void send(const std::vector<uint8_t>& p) { (void)::send(fd, p.data(), p.size(), 0); }
  // Waits up to 10 ms; each datagram goes to `fn(bytes, n)`.
  template <class F>
  void poll10(F fn) {
    pollfd pf{fd, POLLIN, 0};
    if (::poll(&pf, 1, 10) <= 0) return;
    uint8_t buf[2048];
    for (;;) {
      ssize_t n = ::recv(fd, buf, sizeof buf, MSG_DONTWAIT);
      if (n <= 0) return;
      fn(buf, size_t(n));
    }
  }
};

int ident(int port) {
  Sock s(port);
  if (s.fd < 0) { std::puts("error no socket"); return 1; }
  maburgs::DroneIdent id;
  for (;;) {
    if (auto req = id.tick(1, now_ms())) s.send(*req);
    s.poll10([&](const uint8_t* b, size_t n) { id.on_packet(b, n); });
    if (id.ident()) {
      std::printf("version=%s board=%s chip=%s\n", id.ident()->version.c_str(),
                  id.ident()->board.c_str(), id.ident()->chip.c_str());
      return 0;
    }
    if (id.gave_up()) { std::puts("error no drone"); return 2; }
  }
}

int install(int port, const char* path, const char* hex) {
  std::ifstream f(path, std::ios::binary);
  std::vector<uint8_t> img((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (!f.is_open() || f.bad() || img.empty()) { std::puts("error bad file"); return 1; }
  mabur::Sha256Ctx c;
  c.update(img.data(), img.size());
  auto want = mabur::sha256_from_hex(hex);
  mabur::Sha256 have = c.final();
  if (!want || *want != have) { std::puts("error bad hash"); return 1; }

  Sock s(port);
  if (s.fd < 0) { std::puts("error no socket"); return 1; }
  maburgs::UpdateClient cl(std::move(img), have);
  uint64_t last_print = 0;
  int last_trial = -1;
  bool printed_total = false;
  for (;;) {
    const uint64_t t = now_ms();
    for (const auto& p : cl.tick(t)) s.send(p);
    s.poll10([&](const uint8_t* b, size_t n) { cl.on_packet(b, n, now_ms()); });
    const auto& pr = cl.progress();
    const bool full = pr.total && pr.sent == pr.total;
    if (pr.total && ((full && !printed_total) || (!full && t - last_print >= 250 && pr.phase == maburgs::UpdateProgress::Phase::kSend))) {
      std::printf("send %llu %llu\n", (unsigned long long)pr.sent, (unsigned long long)pr.total);
      last_print = t;
      printed_total = printed_total || full;
    }
    if (pr.trial_s != last_trial && pr.trial_s) {
      last_trial = pr.trial_s;
      std::printf("trial %d\n", pr.trial_s);
    }
    std::fflush(stdout);
    if (cl.done()) {
      std::printf("%s\n", pr.result.c_str());
      return pr.result == "committed" ? 0 : 1;
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  int port = 14550, i = 1;
  if (i + 1 < argc && !std::strcmp(argv[i], "--port")) { port = std::atoi(argv[i + 1]); i += 2; }
  if (i < argc && !std::strcmp(argv[i], "ident") && i + 1 == argc) return ident(port);
  if (i < argc && !std::strcmp(argv[i], "install") && i + 3 == argc) return install(port, argv[i + 1], argv[i + 2]);
  std::fputs("usage: maburup [--port N] ident | install <file> <sha256-hex>\n", stderr);
  return 64;
}
