#pragma once

#include <AtlasCore/Asset.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <vector>

namespace atlas {

struct ScanSummary {
  std::uint64_t files{};
  std::uint64_t directories{};
  std::uint64_t bytes{};
  std::uint64_t inaccessible{};
  std::filesystem::path currentPath;
  bool cancelled{};
};

using AssetDiscovered = std::function<void(const AssetMetadata&)>;
using ScanProgress = std::function<void(const ScanSummary&)>;

struct ScanOptions {
  bool includeHidden{true};
  bool followDirectorySymlinks{false};
  std::optional<std::uint32_t> maximumDepth;
  std::uint64_t progressInterval{250};
};

struct DirectoryScanResult {
  std::vector<AssetMetadata> entries;
  std::uint64_t inaccessible{};
};

class Scanner {
 public:
  [[nodiscard]] ScanSummary scan(const std::filesystem::path& root,
                                 std::stop_token stopToken,
                                 const AssetDiscovered& discovered,
                                 const ScanOptions& options = {},
                                 const ScanProgress& progress = {}) const;
  [[nodiscard]] DirectoryScanResult scanDirectory(
      const std::filesystem::path& directory, std::stop_token stopToken = {},
      bool includeHidden = true) const;
};

}  // namespace atlas
