#pragma once

#include <AtlasCore/PbrMaterial.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
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

struct AssetBreakdown {
  std::uint64_t images{};
  std::uint64_t videos{};
  std::uint64_t audio{};
  std::uint64_t models{};
  std::uint64_t archives{};
  std::uint64_t documents{};
  std::uint64_t other{};
};

struct FileTypeBreakdown {
  std::string extension;
  std::uint64_t files{};
  std::uintmax_t bytes{};
};

struct PbrTexture {
  std::filesystem::path path;
  PbrMapKind kind{};
};

struct PbrMaterialSet {
  std::string name;
  std::filesystem::path directory;
  std::vector<PbrTexture> textures;
  bool hasCoreMaps{};
};

struct StorageAnalysis {
  std::uintmax_t capacity{};
  std::uintmax_t available{};
  std::uint64_t files{};
  std::uint64_t directories{};
  std::uintmax_t bytes{};
  AssetBreakdown assetTypes;
  AssetBreakdown assetBytes;
  std::vector<FileTypeBreakdown> fileTypes;
  std::vector<PbrMaterialSet> pbrMaterials;
  std::vector<PathSize> fbxFiles;
  std::vector<PathSize> archiveFiles;
  std::vector<PathSize> largestFiles;
  std::vector<PathSize> largestFolders;
  std::vector<DuplicateGroup> duplicates;
  std::vector<std::filesystem::path> emptyFolders;
  std::uint64_t emptyFolderCount{};
  bool cancelled{};
};

using AnalysisProgress =
    std::function<void(std::uint64_t, std::uint64_t, const std::filesystem::path&)>;

class StorageAnalyzer {
 public:
  [[nodiscard]] StorageAnalysis analyze(const std::filesystem::path& root,
                                        std::stop_token token = {},
                                        std::size_t resultLimit = 25,
                                        bool findDuplicates = true,
                                        const AnalysisProgress& progress = {}) const;
};

}  // namespace atlas
