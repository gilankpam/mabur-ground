// Parsers for what the GS upgrade flow reads: the package VERSION file, the
// gs-upgrade status file and GitHub's release API response.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace maburplay {

struct PackageVersion {
  std::string version, board, drone;
};
// nullopt: no version= line.
std::optional<PackageVersion> parse_version_file(const std::string& text);

struct UpgradeStatus {
  std::string state, phase, msg;
  uint64_t done = 0, total = 0;
};
// nullopt: no state= line.
std::optional<UpgradeStatus> parse_status(const std::string& text);

struct ReleaseInfo {
  std::string version, url, drone;
  bool drone_unpaired = false;
  std::vector<std::string> changelog;
};
// nullopt with *why set when the JSON is bad or has no asset for `board`.
std::optional<ReleaseInfo> parse_release_json(const std::string& json, const std::string& board,
                                              std::string* why);

// The "- " lines under "**Changes**", up to the next blank line, markers stripped.
std::vector<std::string> changelog_lines(const std::string& body);

enum class VersionRel { kSame, kNewer, kOlder, kDifferent };
// Numeric compare of vX.Y.Z; anything else is kSame when equal, else kDifferent.
VersionRel version_rel(const std::string& cur, const std::string& next);

// Value of KEY in os-release style text, with surrounding double quotes removed.
std::string os_release_value(const std::string& text, const std::string& key);

}  // namespace maburplay
