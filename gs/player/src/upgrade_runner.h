// Drives the on-device gs-upgrade tool for the menu: find/check run as
// non-blocking children, start/cancel are short bounded calls, and the status
// and ui-ready files are plain file I/O. Nothing here blocks the main loop.
#pragma once

#include <sys/types.h>

#include <optional>
#include <string>
#include <vector>

#include "upgrade_flow.h"
#include "upgrade_info.h"

namespace maburplay {

class UpgradeRunner {
 public:
  UpgradeRunner(std::string tool = "/usr/sbin/gs-upgrade", std::string dir = "/tmp/gs-upgrade");
  ~UpgradeRunner();
  UpgradeRunner(const UpgradeRunner&) = delete;
  UpgradeRunner& operator=(const UpgradeRunner&) = delete;

  bool find();                             // false: a find/check is already in flight
  bool check(const std::string& channel);  // "stable" | "snapshot"
  // Asks the drone check: the drone version and board come from the link.
  bool check_drone(const std::string& version, const std::string& board);
  bool busy() const { return job_.pid > 0; }
  // The newer version `gs-upgrade notify` left in <dir>/available, or "".
  std::string available() const;

  struct FindResult {
    std::vector<FoundPackage> pkgs;
    std::string error;
  };
  struct CheckResult {
    std::optional<ReleaseInfo> rel;
    std::string error;
  };
  // Each returns once, when its child has exited; nullopt until then.
  std::optional<FindResult> poll_find();
  std::optional<CheckResult> poll_check(const std::string& board);

  std::optional<CheckResult> poll_check_drone(const std::string& board);

  bool start(const UpgradeCmd& c, std::string* why);
  bool cancel();
  std::optional<UpgradeStatus> status() const;
  bool wants_ui_ready() const;  // state=running phase=stop and no ui-ready file yet
  // Locks memory, then touches ui-ready even if the lock failed (false, *why).
  bool mark_ui_ready(std::string* why);
  // False only when gs-upgrade's lock names a pid that is gone: a run killed
  // before it wrote a final status. No lock or no pid yet reads as alive, as
  // a run drops its lock just before its final status.
  bool run_alive() const;

 private:
  struct Child {
    pid_t pid = -1;
    int fd = -1;  // read end of the child's stdout, non-blocking
    std::string out;
  };
  bool spawn(const std::vector<std::string>& args, Child* c);
  // True once the child has exited and its output is drained; *status is its wait status.
  bool reap(Child* c, int* status);
  void kill_child(Child* c);
  // Runs one short verb, waiting at most timeout_ms; false on spawn failure or timeout.
  bool run_short(const std::vector<std::string>& args, int timeout_ms, std::string* out);

  std::string tool_, dir_;
  Child job_;  // the one find/check in flight
  bool snapshot_ = false;  // the check in flight asked for the snapshot
};

}  // namespace maburplay
