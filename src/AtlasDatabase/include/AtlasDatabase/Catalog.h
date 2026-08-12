#pragma once

#include <AtlasCore/Asset.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace atlas {

using AssetId = std::int64_t;

struct CatalogAsset {
  AssetId id{};
  AssetMetadata metadata;
};

struct AssetQuery {
  std::string text;
  std::optional<std::string> extension;
  std::optional<std::uintmax_t> minimumSize;
  std::optional<std::uintmax_t> maximumSize;
  std::optional<std::chrono::system_clock::time_point> modifiedAfter;
  std::optional<std::filesystem::path> beneath;
  std::size_t limit{500};
  std::size_t offset{};
};

struct CatalogStatistics {
  std::uint64_t files{};
  std::uint64_t directories{};
  std::uint64_t bytes{};
};

class Catalog {
 public:
  explicit Catalog(const std::filesystem::path& databasePath);
  ~Catalog();
  Catalog(Catalog&&) noexcept;
  Catalog& operator=(Catalog&&) noexcept;
  Catalog(const Catalog&) = delete;
  Catalog& operator=(const Catalog&) = delete;

  AssetId upsert(const AssetMetadata& metadata);
  std::vector<AssetId> upsertBatch(const std::vector<AssetMetadata>& metadata);
  [[nodiscard]] std::optional<CatalogAsset> findByPath(
      const std::filesystem::path& path) const;
  [[nodiscard]] std::uint64_t assetCount() const;
  [[nodiscard]] std::vector<CatalogAsset> search(const AssetQuery& query) const;
  [[nodiscard]] std::vector<CatalogAsset> largestFiles(std::size_t limit = 100) const;
  [[nodiscard]] CatalogStatistics statistics() const;
  std::uint64_t removeMissingUnder(const std::filesystem::path& root,
                                   const std::vector<std::filesystem::path>& observedPaths);

  void addBookmark(const std::filesystem::path& path);
  void removeBookmark(const std::filesystem::path& path);
  [[nodiscard]] std::vector<std::filesystem::path> bookmarks() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace atlas
