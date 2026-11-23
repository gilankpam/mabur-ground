#include "upgrade_flow.h"

#include <algorithm>

namespace maburplay {

namespace {
std::string phase_name(const std::string& p) {
  if (p == "download") return "Downloading";
  if (p == "unpack") return "Unpacking";
  if (p == "verify") return "Checking";
  if (p == "stop") return "Stopping services";
  if (p == "write") return "Writing";
  if (p == "reboot") return "Rebooting";
  if (p == "send") return "Sending to the drone";
  if (p == "install") return "Installing on the drone";
  if (p == "trial") return "Trial run";
  return "Starting";
}
// The tool honours a cancel only before it touches services, and not once the
// last byte is on its way to the drone.
bool cancellable(const UpgradeStatus& st) {
  const std::string& p = st.phase;
  if (p == "send") return !(st.total > 0 && st.done == st.total);
  return p.empty() || p == "download" || p == "unpack" || p == "verify";
}
// Header text for installing `next` over `cur`; warns when it is a downgrade or
// a snapshot.
void version_header(const std::string& cur, const std::string& next, std::string* header,
                    bool* warn) {
  *warn = false;
  switch (version_rel(cur, next)) {
    case VersionRel::kSame: *header = "already on " + next; break;
    case VersionRel::kNewer: *header = cur + " → " + next; break;
    case VersionRel::kOlder:
      *header = cur + " → " + next + " · older";
      *warn = true;
      break;
    case VersionRel::kDifferent:
      *header = cur + " → " + next + " · snapshot";
      *warn = true;
      break;
  }
}

std::string file_name(const std::string& path) { return path.substr(path.find_last_of('/') + 1); }
}  // namespace

UpgradeFlow::UpgradeFlow(std::string current_version, std::string board)
    : cur_(std::move(current_version)), board_(std::move(board)) {}

UpgradeFlow::Item UpgradeFlow::from_package(const FoundPackage& p) const {
  Item it;
  it.title = "Update from file";
  it.arg = p.path;
  it.changelog = p.changelog;
  if (!p.ver) {
    it.header = "version unknown";
    return it;
  }
  it.version = p.ver->version;
  if (!p.ver->board.empty() && p.ver->board != board_) it.error = "Built for " + p.ver->board;
  it.pair = p.ver->drone.empty() ? "pairs with no drone release yet"
                                 : "pairs with drone " + p.ver->drone;
  it.pair_warn = p.ver->drone.empty();
  version_header(cur_, it.version, &it.header, &it.header_warn);
  return it;
}

void UpgradeFlow::show_packages(std::vector<FoundPackage> pkgs) {
  toast_.clear();
  items_.clear();
  confirm_ = false;
  have_status_ = false;
  ++gen_;
  if (pkgs.empty()) {
    view_ = View::kNone;
    toast_ = "Put " + board_ + ".tar.gz on the SD card or DVR partition";
    return;
  }
  for (const auto& p : pkgs) items_.push_back(from_package(p));
  sel_ = 0;
  if (items_.size() == 1) {
    from_list_ = false;
    open_item(0);
  } else {
    from_list_ = true;
    view_ = View::kList;
  }
}

void UpgradeFlow::show_release(const ReleaseInfo& rel, const std::string& channel) {
  toast_.clear();
  Item it;
  it.online = true;
  it.title = channel == "snapshot" ? "Update · Snapshot" : "Update · Stable";
  it.version = rel.version;
  it.arg = rel.url;
  it.changelog = rel.changelog;
  if (rel.drone_unpaired) {
    it.pair = "pairs with no drone release yet";
    it.pair_warn = true;
  } else if (!rel.drone.empty()) {
    it.pair = "pairs with drone " + rel.drone;
  }
  version_header(cur_, rel.version, &it.header, &it.header_warn);
  items_ = {it};
  from_list_ = false;
  confirm_ = false;
  have_status_ = false;
  ++gen_;
  open_item(0);
}

void UpgradeFlow::show_drone_release(const ReleaseInfo& rel, const std::string& drone_version) {
  toast_.clear();
  Item it;
  it.online = it.drone = true;
  it.title = "Update · Drone";
  it.version = rel.version;
  it.arg = rel.url;
  it.changelog = rel.changelog;
  version_header(drone_version, rel.version, &it.header, &it.header_warn);
  items_ = {it};
  from_list_ = false;
  confirm_ = false;
  have_status_ = false;
  ++gen_;
  open_item(0);
}

void UpgradeFlow::open_item(int i) {
  sel_ = i;
  scroll_ = 0;
  confirm_ = false;
  view_ = View::kInfo;
}

std::optional<UpgradeKey> upgrade_key(const KeyEvent& e) {
  switch (e.button) {
    case Button::kUp:
    case Button::kDown:
      if (e.kind == KeyKind::kHold) break;
      return e.button == Button::kUp ? UpgradeKey::kUp : UpgradeKey::kDown;
    case Button::kLeft:
      if (e.kind == KeyKind::kTap) return UpgradeKey::kLeft;
      break;
    case Button::kRight:
      if (e.kind != KeyKind::kRepeat) return UpgradeKey::kRight;
      break;
    case Button::kSel:
      if (e.kind == KeyKind::kTap) return UpgradeKey::kRight;
      break;
    case Button::kCount: break;
  }
  return std::nullopt;
}

UpgradeCmd UpgradeFlow::key(UpgradeKey k) {
  const UpgradePage before = page();
  UpgradeCmd c = key_impl(k);
  if (!(page() == before)) ++gen_;
  return c;
}

UpgradeCmd UpgradeFlow::key_impl(UpgradeKey k) {
  UpgradeCmd cmd;
  if (view_ == View::kNone) return cmd;
  const int n = view_ == View::kList ? static_cast<int>(items_.size())
                                     : static_cast<int>(items_[sel_].changelog.size());
  switch (view_) {
    case View::kNone: break;
    case View::kList:
      if (k == UpgradeKey::kUp) sel_ = std::max(0, sel_ - 1);
      if (k == UpgradeKey::kDown) sel_ = std::min(n - 1, sel_ + 1);
      if (k == UpgradeKey::kRight) open_item(sel_);
      if (k == UpgradeKey::kLeft) view_ = View::kNone;
      break;
    case View::kInfo: {
      const Item& it = items_[sel_];
      if (confirm_) {
        if (k == UpgradeKey::kLeft) confirm_ = false;
        if (k == UpgradeKey::kRight) {
          cmd.kind = it.drone    ? UpgradeCmd::Kind::kStartDrone
                     : it.online ? UpgradeCmd::Kind::kStartOnline
                                 : UpgradeCmd::Kind::kStartFile;
          cmd.arg = it.arg;
          cmd.version = it.version;
          view_ = View::kRunning;
          confirm_ = false;
          have_status_ = false;
          st_ = {};
        }
        break;
      }
      const int max_first = std::max(0, n - kBodyRows);
      if (k == UpgradeKey::kUp) scroll_ = std::max(0, scroll_ - 1);
      if (k == UpgradeKey::kDown) scroll_ = std::min(max_first, scroll_ + 1);
      if (k == UpgradeKey::kRight && it.error.empty()) confirm_ = true;
      if (k == UpgradeKey::kLeft) view_ = from_list_ ? View::kList : View::kNone;
      break;
    }
    case View::kRunning:
      if (k == UpgradeKey::kLeft && cancellable(st_)) cmd.kind = UpgradeCmd::Kind::kCancel;
      break;
    case View::kEnded:
      if (k == UpgradeKey::kLeft || k == UpgradeKey::kRight) view_ = View::kNone;
      break;
  }
  return cmd;
}

void UpgradeFlow::on_status(const UpgradeStatus& st) {
  const UpgradePage before = page();
  status_impl(st);
  if (!(page() == before)) ++gen_;
}

void UpgradeFlow::status_impl(const UpgradeStatus& st) {
  if (view_ != View::kRunning) return;
  st_ = st;
  have_status_ = true;
  if (st.state == "error" || st.state == "cancelled" || st.state == "done") view_ = View::kEnded;
}

UpgradePage UpgradeFlow::page() const {
  UpgradePage p;
  if (view_ == View::kNone) return p;
  p.shown = true;
  if (view_ == View::kList) {
    p.title = "Update from file";
    for (const auto& it : items_) p.lines.push_back(it.version.empty() ? file_name(it.arg)
                                                               : it.version + "  " + file_name(it.arg));
    p.cursor = sel_;
    p.first = std::max(0, sel_ - kBodyRows + 1);
    p.keys = "▲▼ choose  ▶ open  ◀ close";
    return p;
  }
  const Item& it = items_[sel_];
  p.title = it.title;
  p.header = it.header;
  p.header_warn = it.header_warn;
  p.pair = it.pair;
  p.pair_warn = it.pair_warn;
  p.lines = it.changelog;
  p.first = scroll_;
  p.error = it.error;
  if (view_ == View::kInfo) {
    if (confirm_) {
      // A package with no VERSION is named by its file.
      const std::string name =
          it.version.empty() ? file_name(it.arg) : it.version;
      p.confirm = it.drone ? "Install " + name + " on the drone? Keep the drone and the GS powered."
                           : "Install " + name + "? The GS reboots. Keep the power on.";
      p.keys = "▶ install  ◀ back";
    } else {
      p.keys = it.error.empty() ? "▲▼ scroll  ▶ install  ◀ back" : "▲▼ scroll  ◀ back";
    }
    return p;
  }
  p.first = 0;
  p.lines.clear();
  if (view_ == View::kRunning) {
    p.running = true;
    p.phase_text = phase_name(st_.phase);
    // done is the seconds run of the window; the text shows what is left.
    if (st_.phase == "trial" && st_.total >= st_.done)
      p.phase_text += " · " + std::to_string(st_.total - st_.done) + " s";
    if (have_status_ && st_.total > 0) p.pct = static_cast<int>(st_.done * 100 / st_.total);
    p.keys = cancellable(st_) ? "◀ cancel" : "";
    return p;
  }
  if (st_.state == "done") {
    p.phase_text = st_.msg;
  } else if (st_.state == "cancelled") {
    p.phase_text = "Cancelled";
  } else if (st_.phase == "write") {
    p.error = "Write failed — reflash from SD card";
  } else {
    p.error = st_.msg;
  }
  p.keys = "◀ close";
  return p;
}

}  // namespace maburplay
