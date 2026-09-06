#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace atlas {

enum class AssetKind { Unknown, Model3D, Material, Animation, Vdb, Plant, Hdri, Decal, Image, Audio };
enum class AssetAvailability { Cloud, Downloading, Offline, Missing, UpdateAvailable };

struct AssetSource { std::string provider; std::string vendorId; std::string sourceUri; };
struct AssetPreviewSet {
  std::filesystem::path hero;
  std::filesystem::path front;
  std::filesystem::path back;
  std::filesystem::path left;
  std::filesystem::path right;
  std::filesystem::path top;
  std::filesystem::path perspective;
};

struct CatalogAsset {
  std::string atlasId;
  std::string name;
  AssetKind kind{AssetKind::Unknown};
  std::string category;
  std::vector<std::string> tags;
  AssetSource source;
  AssetAvailability availability{AssetAvailability::Offline};
  std::filesystem::path localDirectory;
  AssetPreviewSet previews;
  std::vector<std::filesystem::path> meshes;
  std::vector<std::filesystem::path> textures;
  std::vector<std::filesystem::path> dependencies;
  int rating{0}; // 0 = unrated, 1..5 = user rating
};

struct LibraryFolder {
  std::string id;
  std::string name;
  std::string parentId;
  std::vector<std::string> assetIds;
};

class AssetCatalogModel {
 public:
  void upsert(CatalogAsset asset);
  void upsertFolder(LibraryFolder folder);
  bool addAssetToFolder(const std::string& assetId, const std::string& folderId);
  bool removeAssetFromFolder(const std::string& assetId, const std::string& folderId);
  bool setRating(const std::string& assetId, int rating);
  [[nodiscard]] const std::vector<CatalogAsset>& assets() const noexcept;
  [[nodiscard]] const std::vector<LibraryFolder>& folders() const noexcept;
  [[nodiscard]] std::vector<CatalogAsset> byKind(AssetKind kind) const;
  [[nodiscard]] std::vector<CatalogAsset> byCategory(const std::string& category) const;
  [[nodiscard]] std::vector<CatalogAsset> byProvider(const std::string& provider) const;
  [[nodiscard]] std::vector<CatalogAsset> byAvailability(AssetAvailability availability) const;

 private:
  std::vector<CatalogAsset> assets_;
  std::vector<LibraryFolder> folders_;
};

}  // namespace atlas
