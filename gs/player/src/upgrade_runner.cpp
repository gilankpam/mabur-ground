#include "upgrade_runner.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

namespace maburplay {
namespace {

std::string slurp(const std::string& path) {
  std::ifstream f(path);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// The player holds the DRM device and sockets; the upgrade run must not.
void close_inherited_fds() {
#ifdef SYS_close_range
  if (::syscall(SYS_close_range, 3u, ~0u, 0u) == 0) return;
#endif
  long n = ::sysconf(_SC_OPEN_MAX);
  if (n < 0 || n > 65536) n = 65536;
  for (int fd = 3; fd < n; ++fd) ::close(fd);
}

std::vector<std::string> lines_of(const std::string& s) {
  std::vector<std::string> v;
  std::istringstream in(s);
  std::string l;
  while (std::getline(in, l)) v.push_back(l);
  return v;
}

// The text after "error <code> " on the tool's error line; code in *code.
bool error_line(const std::string& out, std::string* code, std::string* text) {
  for (const auto& l : lines_of(out)) {
    if (l.rfind("error ", 0) != 0) continue;
    std::string rest = l.substr(6);
    size_t sp = rest.find(' ');
    *code = rest.substr(0, sp);
    *text = sp == std::string::npos ? "" : rest.substr(sp + 1);
    return true;
  }
  return false;
}

}  // namespace

UpgradeRunner::UpgradeRunner(std::string tool, std::string dir)
    : tool_(std::move(tool)), dir_(std::move(dir)) {}

UpgradeRunner::~UpgradeRunner() { kill_child(&job_); }

bool UpgradeRunner::spawn(const std::vector<std::string>& args, Child* c) {
  int p[2];
  if (::pipe2(p, O_CLOEXEC) != 0) return false;
  std::vector<std::string> argv_s{tool_};
  argv_s.insert(argv_s.end(), args.begin(), args.end());
  std::vector<char*> argv;
  for (auto& a : argv_s) argv.push_back(a.data());
  argv.push_back(nullptr);
  // Built before the fork: setenv is not async-signal-safe in a threaded parent.
  std::vector<std::string> env_s;
  for (char** e = environ; e && *e; ++e) {
    if (std::strncmp(*e, "GS_UPGRADE_DIR=", 15) != 0) env_s.push_back(*e);
  }
  env_s.push_back("GS_UPGRADE_DIR=" + dir_);
  std::vector<char*> envp;
  for (auto& e : env_s) envp.push_back(e.data());
  envp.push_back(nullptr);
  pid_t pid = ::fork();
  if (pid < 0) {
    ::close(p[0]);
    ::close(p[1]);
    return false;
  }
  if (pid == 0) {
    ::dup2(p[1], 1);
    int nul = ::open("/dev/null", O_RDWR);
    if (nul >= 0) {
      ::dup2(nul, 0);
      ::dup2(nul, 2);
    }
    close_inherited_fds();
    ::execve(argv[0], argv.data(), envp.data());
    ::_exit(127);
  }
  ::close(p[1]);
  ::fcntl(p[0], F_SETFL, O_NONBLOCK);
  c->pid = pid;
  c->fd = p[0];
  c->out.clear();
  return true;
}

bool UpgradeRunner::reap(Child* c, int* status) {
  char b[512];
  for (;;) {
    ssize_t n = ::read(c->fd, b, sizeof(b));
    if (n > 0) {
      c->out.append(b, static_cast<size_t>(n));
      continue;
    }
    break;
  }
  pid_t w = ::waitpid(c->pid, status, WNOHANG);
  if (w == 0) return false;
  if (w < 0) *status = -1;  // already reaped elsewhere: exited, status unknown
  // Exited: whatever it wrote is in the pipe now.
  for (ssize_t n; (n = ::read(c->fd, b, sizeof(b))) > 0;) c->out.append(b, static_cast<size_t>(n));
  ::close(c->fd);
  c->fd = -1;
  c->pid = -1;
  return true;
}

void UpgradeRunner::kill_child(Child* c) {
  if (c->pid <= 0) return;
  ::kill(c->pid, SIGKILL);
  ::waitpid(c->pid, nullptr, 0);
  ::close(c->fd);
  c->pid = -1;
  c->fd = -1;
}

bool UpgradeRunner::run_short(const std::vector<std::string>& args, int timeout_ms,
                              std::string* out) {
  Child c;
  if (!spawn(args, &c)) return false;
  int st = 0;
  auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (!reap(&c, &st)) {
    if (std::chrono::steady_clock::now() > end) {
      kill_child(&c);
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  *out = c.out;
  return true;
}

bool UpgradeRunner::find() {
  if (busy()) return false;
  return spawn({"find"}, &job_);
}

bool UpgradeRunner::check(const std::string& channel) {
  if (busy()) return false;
  snapshot_ = channel == "snapshot";
  return spawn({"check", channel}, &job_);
}

std::optional<UpgradeRunner::FindResult> UpgradeRunner::poll_find() {
  int st = 0;
  if (!busy() || !reap(&job_, &st)) return std::nullopt;
  FindResult r;
  for (const auto& l : lines_of(job_.out)) {
    // "pkg <n> <path>"
    if (l.rfind("pkg ", 0) != 0) continue;
    size_t sp = l.find(' ', 4);
    if (sp == std::string::npos) continue;
    std::string n = l.substr(4, sp - 4);
    FoundPackage p;
    p.path = l.substr(sp + 1);
    std::string d = dir_ + "/pkg/" + n;
    p.ver = parse_version_file(slurp(d + "/VERSION"));
    // The file's "- " lines read exactly as a release body's do.
    p.changelog = changelog_lines("**Changes**\n" + slurp(d + "/CHANGELOG"));
    r.pkgs.push_back(std::move(p));
  }
  if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) r.error = "Could not look for packages";
  return r;
}

std::optional<UpgradeRunner::CheckResult> UpgradeRunner::poll_check(const std::string& board) {
  int st = 0;
  if (!busy() || !reap(&job_, &st)) return std::nullopt;
  CheckResult r;
  std::string code, text;
  if (error_line(job_.out, &code, &text)) {
    if (code == "clock") {
      r.error = "Clock not synced yet \xe2\x80\x94 try again in a minute";
    } else if (code == "network") {
      r.error = "No network \xe2\x80\x94 set wifi.toml to client mode";
    } else if (code == "notfound" && snapshot_) {
      r.error = "No snapshot right now \xe2\x80\x94 try again in a few minutes";
    } else {
      r.error = text.empty() ? "Check failed" : text;
    }
    return r;
  }
  if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
    r.error = "Check failed";
    return r;
  }
  std::string why;
  r.rel = parse_release_json(slurp(dir_ + "/release.json"), board, &why);
  if (!r.rel) r.error = why;
  return r;
}

bool UpgradeRunner::start(const UpgradeCmd& c, std::string* why) {
  std::vector<std::string> args{"start"};
  if (c.kind == UpgradeCmd::Kind::kStartFile) {
    args.insert(args.end(), {"file", c.arg});
  } else if (c.kind == UpgradeCmd::Kind::kStartOnline) {
    args.insert(args.end(), {"online", c.arg, c.version});
  } else {
    if (why) *why = "Nothing to start";
    return false;
  }
  std::string out;
  if (!run_short(args, 2000, &out)) {
    if (why) *why = "gs-upgrade did not answer";
    return false;
  }
  std::string code, text;
  if (error_line(out, &code, &text)) {
    if (why) *why = text.empty() ? "gs-upgrade refused to start" : text;
    return false;
  }
  if (out.rfind("started", 0) != 0) {
    if (why) *why = "gs-upgrade did not start";
    return false;
  }
  return true;
}

bool UpgradeRunner::cancel() {
  std::string out;
  return run_short({"cancel"}, 2000, &out);
}

std::optional<UpgradeStatus> UpgradeRunner::status() const {
  return parse_status(slurp(dir_ + "/status"));
}

bool UpgradeRunner::wants_ui_ready() const {
  auto s = status();
  // A failed stop leaves "error stop" behind while the GS stays up: no ask.
  return s && s->state == "running" && s->phase == "stop" &&
         ::access((dir_ + "/ui-ready").c_str(), F_OK) != 0;
}

bool UpgradeRunner::mark_ui_ready(std::string* why) {
  // Locked first: from here the player must not page-fault from the rootfs
  // that the write is about to replace.
  bool ok = ::mlockall(MCL_CURRENT | MCL_FUTURE) == 0;
  if (!ok && why) *why = std::string("mlockall: ") + std::strerror(errno);
  int fd = ::open((dir_ + "/ui-ready").c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) {
    if (why) *why = std::string("ui-ready: ") + std::strerror(errno);
    return false;
  }
  ::close(fd);
  return ok;
}

}  // namespace maburplay
