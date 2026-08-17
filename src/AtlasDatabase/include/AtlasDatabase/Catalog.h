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
using ScanSessionId = std::int64_t;

enum class ScanSessionState { Pending, Running, Paused, Completed, Failed };

struct ScanSession {
  ScanSessionId id{};
  std::string name;
  std::filesystem::path root;
  bool driveScan{};
  bool snapshot{};
  std::optional<ScanSessionId> sourceSessionId;
  std::uint32_t dataVersion{1};
  std::uint32_t organizationVersion{};
  ScanSessionState state{ScanSessionState::Pending};
  std::uint64_t files{};
  std::uint64_t directories{};
  std::uint64_t bytes{};
  std::uint64_t inaccessible{};
  std::uint64_t pendingDirectories{};
  std::chrono::system_clock::time_point startedAt{};
  std::chrono::system_clock::time_point updatedAt{};
  std::string error;
};

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

  ScanSessionId createScanSession(const std::filesystem::path& root, bool driveScan);
  ScanSessionId saveScanSnapshot(ScanSessionId sourceId, std::string name);
  [[nodiscard]] std::optional<ScanSession> scanSession(ScanSessionId id) const;
  [[nodiscard]] std::vector<ScanSession> scanSessions() const;
  void setScanSessionState(ScanSessionId id, ScanSessionState state,
                           std::string error = {});
  void setScanOrganizationVersion(ScanSessionId id, std::uint32_t version);
  [[nodiscard]] std::optional<std::filesystem::path> nextScanDirectory(
      ScanSessionId id) const;
  void recordScannedDirectory(ScanSessionId id, const std::filesystem::path& directory,
                              const std::vector<AssetMetadata>& entries,
                              std::uint64_t inaccessible = 0);
  [[nodiscard]] std::vector<CatalogAsset> scanAssets(ScanSessionId id,
                                                     std::size_t limit = 10000) const;
  [[nodiscard]] std::vector<CatalogAsset> scanPbrTextureAssets(
      ScanSessionId id, std::size_t limit = 100000) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace atlas
