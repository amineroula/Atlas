#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <vector>

namespace atlas {

struct PathSize {
  std::filesystem::path path;
  std::uintmax_t bytes{};
};

struct DuplicateGroup {
  std::uintmax_t bytesPerFile{};
  std::vector<std::filesystem::path> paths;
};

struct StorageAnalysis {
  std::uintmax_t capacity{};
  std::uintmax_t available{};
  std::uint64_t files{};
  std::uint64_t directories{};
  std::uintmax_t bytes{};
  std::vector<PathSize> largestFiles;
  std::vector<PathSize> largestFolders;
  std::vector<DuplicateGroup> duplicates;
  std::vector<std::filesystem::path> emptyFolders;
  bool cancelled{};
};

using AnalysisProgress = std::function<void(std::uint64_t, const std::filesystem::path&)>;

class StorageAnalyzer {
 public:
  [[nodiscard]] StorageAnalysis analyze(const std::filesystem::path& root,
                                        std::stop_token token = {},
                                        std::size_t resultLimit = 25,
                                        const AnalysisProgress& progress = {}) const;
};

}  // namespace atlas
