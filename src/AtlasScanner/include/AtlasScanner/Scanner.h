#pragma once

#include <AtlasCore/Asset.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>

namespace atlas {

struct ScanSummary {
  std::uint64_t files{};
  std::uint64_t directories{};
  std::uint64_t bytes{};
  std::uint64_t inaccessible{};
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

class Scanner {
 public:
  [[nodiscard]] ScanSummary scan(const std::filesystem::path& root,
                                 std::stop_token stopToken,
                                 const AssetDiscovered& discovered,
                                 const ScanOptions& options = {},
                                 const ScanProgress& progress = {}) const;
};

}  // namespace atlas
