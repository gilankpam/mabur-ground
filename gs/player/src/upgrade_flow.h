// The goggles menu's update pages as a pure state machine: button events and
// on-device status in, start/cancel commands and a drawable page out. No I/O,
// clock or threads, so the layer that draws it and the runner that executes
// the commands stay separate.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "upgrade_info.h"

namespace maburplay {

enum class UpgradeKey { kUp, kDown, kLeft, kRight };

struct FoundPackage {
  std::string path;
  std::optional<PackageVersion> ver;  // nullopt: no readable VERSION file
  std::vector<std::string> changelog;
};

struct UpgradeCmd {
  enum class Kind { kNone, kStartFile, kStartOnline, kCancel } kind = Kind::kNone;
  std::string arg, version;  // file: arg = path; online: arg = url, version = release
};

struct UpgradePage {
  bool shown = false;
  std::string title;
  std::string header;
  bool header_warn = false;
  std::string pair;
  bool pair_warn = false;
  std::vector<std::string> lines;  // body: changelog or package list
  int first = 0;                   // first visible body line
  int cursor = -1;                 // package list only
  std::string confirm;             // non-empty: confirm prompt over the page
  int pct = -1;                    // -1: no bar
  bool running = false;             // an install under way: the GS must stay powered
  std::string phase_text, error, keys;
  bool operator==(const UpgradePage&) const = default;
};

class UpgradeFlow {
 public:
  static constexpr int kBodyRows = 8;
  UpgradeFlow(std::string current_version, std::string board);

  bool active() const { return view_ != View::kNone; }
  // Moves on every page change, so the drawer knows when to repaint.
  uint64_t generation() const { return gen_; }
  // Zero packages leaves the flow inactive and sets toast(); one goes straight
  // to its page; several show a list.
  void show_packages(std::vector<FoundPackage> pkgs);
  void show_release(const ReleaseInfo& rel, const std::string& channel);
  // The last refusal text for the menu to toast, "" if none.
  std::string toast() const { return toast_; }
  UpgradeCmd key(UpgradeKey k);
  void on_status(const UpgradeStatus& st);
  UpgradePage page() const;

 private:
  enum class View { kNone, kList, kInfo, kRunning, kEnded };
  struct Item {  // one installable thing: a file package or an online release
    std::string title, header, pair, error, version, arg;
    bool header_warn = false, pair_warn = false, online = false;
    std::vector<std::string> changelog;
  };
  Item from_package(const FoundPackage& p) const;
  void open_item(int i);
  UpgradeCmd key_impl(UpgradeKey k);
  void status_impl(const UpgradeStatus& st);

  std::string cur_, board_, toast_;
  View view_ = View::kNone;
  std::vector<Item> items_;
  int sel_ = 0;     // list cursor / open item
  int scroll_ = 0;  // body scroll of the open page
  bool from_list_ = false, confirm_ = false;
  UpgradeStatus st_;
  bool have_status_ = false;
  uint64_t gen_ = 0;
};

}  // namespace maburplay
