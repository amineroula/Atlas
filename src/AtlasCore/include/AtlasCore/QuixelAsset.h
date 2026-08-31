#pragma once

#include <AtlasCore/PbrMaterial.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace atlas {

enum class QuixelAssetKind { Model, Plant, Atlas, Surface, Brush, Unknown };

struct QuixelMesh {
  std::filesystem::path path;
  int lod{-1};
};

struct QuixelTexture {
  std::filesystem::path path;
  std::optional<PbrMapKind> kind;
  int lod{-1};
  int resolution{};
};

struct QuixelAsset {
  std::string id;
  std::string name;
  QuixelAssetKind kind{QuixelAssetKind::Unknown};
  std::filesystem::path directory;
  std::filesystem::path metadataPath;
  std::filesystem::path previewPath;
  std::vector<std::string> categories;
  std::vector<std::string> tags;
  std::vector<QuixelMesh> meshes;
  std::vector<QuixelTexture> textures;
};

// Groups paths already discovered by an Atlas scan into Quixel Bridge assets.
// The function only reads each asset's small JSON metadata file; source files
// remain untouched and no directory traversal is performed.
[[nodiscard]] std::vector<QuixelAsset> organizeQuixelAssets(
    const std::vector<std::filesystem::path>& scannedPaths,
    const std::filesystem::path& scanRoot);

[[nodiscard]] const char* quixelAssetKindName(QuixelAssetKind kind) noexcept;

}  // namespace atlas
