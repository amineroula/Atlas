#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace atlas {

enum class AssetKind {
  Unknown,
  Model3D,
  Material,
  Plant,
  Hdri,
  Decal
};

enum class AssetAvailability {
  Cloud,
  Downloading,
  Offline,
  Missing,
  UpdateAvailable
};

struct AssetSource {
  std::string provider;      // quixel, kitbash, local, scan, ...
  std::string vendorId;      // provider-specific stable id
  std::string sourceUri;     // provider/catalog URI when known
};

struct CatalogAsset {
  std::string atlasId;       // provider-independent Atlas identity
  std::string name;
  AssetKind kind{AssetKind::Unknown};
  std::string category;      // Buildings, Animals, Hard Surface, ...
  std::vector<std::string> tags;
  AssetSource source;
  AssetAvailability availability{AssetAvailability::Offline};
  std::filesystem::path localDirectory;
  std::filesystem::path preview;
  std::vector<std::filesystem::path> meshes;
  std::vector<std::filesystem::path> textures;
};

// Atlas is organized by asset meaning, not by vendor. Providers feed this
// normalized model; the UI can then filter the same catalog by category,
// provider, availability, tags, or asset kind.
class AssetCatalogModel {
 public:
  void upsert(CatalogAsset asset);
  [[nodiscard]] const std::vector<CatalogAsset>& assets() const noexcept;
  [[nodiscard]] std::vector<CatalogAsset> byCategory(const std::string& category) const;
  [[nodiscard]] std::vector<CatalogAsset> byProvider(const std::string& provider) const;
  [[nodiscard]] std::vector<CatalogAsset> byAvailability(AssetAvailability availability) const;

 private:
  std::vector<CatalogAsset> assets_;
};

}  // namespace atlas
