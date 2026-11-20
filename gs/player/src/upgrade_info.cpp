#include "upgrade_info.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>

#include "json.hpp"

namespace maburplay {

namespace {

// Calls fn(key, value) for each key=value line.
template <class F>
void each_kv(const std::string& text, F fn) {
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    auto eq = line.find('=');
    if (eq != std::string::npos) fn(line.substr(0, eq), line.substr(eq + 1));
  }
}

}  // namespace

std::optional<PackageVersion> parse_version_file(const std::string& text) {
  PackageVersion p;
  bool have = false;
  each_kv(text, [&](const std::string& k, const std::string& v) {
    if (k == "version") {
      p.version = v;
      have = true;
    } else if (k == "board") {
      p.board = v;
    } else if (k == "drone") {
      p.drone = v;
    }
  });
  if (!have) return std::nullopt;
  return p;
}

std::optional<UpgradeStatus> parse_status(const std::string& text) {
  UpgradeStatus s;
  bool have = false;
  each_kv(text, [&](const std::string& k, const std::string& v) {
    if (k == "state") {
      s.state = v;
      have = true;
    } else if (k == "phase") {
      s.phase = v;
    } else if (k == "msg") {
      s.msg = v;
    } else if (k == "done") {
      s.done = std::strtoull(v.c_str(), nullptr, 10);
    } else if (k == "total") {
      s.total = std::strtoull(v.c_str(), nullptr, 10);
    }
  });
  if (!have) return std::nullopt;
  return s;
}

std::vector<std::string> changelog_lines(const std::string& body) {
  std::vector<std::string> out;
  std::istringstream in(body);
  std::string line;
  bool in_block = false;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!in_block) {
      in_block = line == "**Changes**";
    } else if (line.empty()) {
      break;
    } else if (line.rfind("- ", 0) == 0) {
      out.push_back(line.substr(2));
    }
  }
  return out;
}

std::optional<ReleaseInfo> parse_release_json(const std::string& json, const std::string& board,
                                              std::string* why) {
  auto fail = [&](const std::string& m) -> std::optional<ReleaseInfo> {
    if (why) *why = m;
    return std::nullopt;
  };
  nlohmann::json j = nlohmann::json::parse(json, nullptr, false);
  if (!j.is_object()) return fail("bad release json");

  const std::string prefix = "mabur-ground-";
  const std::string suffix = "-" + board + ".tar.gz";
  ReleaseInfo r;
  bool found = false;
  const auto assets = j.find("assets");
  if (assets != j.end() && assets->is_array()) {
    for (const auto& a : *assets) {
      const std::string name = a.value("name", "");
      if (name.size() <= prefix.size() + suffix.size() || name.rfind(prefix, 0) != 0 ||
          name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
        continue;
      }
      r.version = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
      r.url = a.value("browser_download_url", "");
      found = true;
      break;
    }
  }
  if (!found || r.url.empty()) return fail("no package for " + board);

  const std::string body = j.value("body", "");
  const std::string pairs = "**Pairs with drone ";
  auto p = body.find(pairs);
  if (p != std::string::npos) {
    p += pairs.size();
    auto e = body.find("**", p);
    if (e != std::string::npos) r.drone = body.substr(p, e - p);
  }
  r.drone_unpaired = body.find("**Pin contract matches no drone release yet**") != std::string::npos;
  r.changelog = changelog_lines(body);
  return r;
}

VersionRel version_rel(const std::string& cur, const std::string& next) {
  if (cur == next) return VersionRel::kSame;
  int a[3], b[3];
  char tail;
  if (std::sscanf(cur.c_str(), "v%d.%d.%d%c", &a[0], &a[1], &a[2], &tail) != 3 ||
      std::sscanf(next.c_str(), "v%d.%d.%d%c", &b[0], &b[1], &b[2], &tail) != 3) {
    return VersionRel::kDifferent;
  }
  for (int i = 0; i < 3; ++i) {
    if (b[i] != a[i]) return b[i] > a[i] ? VersionRel::kNewer : VersionRel::kOlder;
  }
  return VersionRel::kSame;
}

std::string os_release_value(const std::string& text, const std::string& key) {
  std::string out;
  each_kv(text, [&](const std::string& k, const std::string& v) {
    if (k != key) return;
    out = v;
    if (out.size() >= 2 && out.front() == '"' && out.back() == '"') {
      out = out.substr(1, out.size() - 2);
    }
  });
  return out;
}

}  // namespace maburplay
