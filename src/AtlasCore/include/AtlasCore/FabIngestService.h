#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace atlas {

struct FabIngestResult {
  bool ok{false};
  std::string assetName;
  std::string vendorId;
  std::filesystem::path sourceDirectory;
  std::filesystem::path payloadCopy;
  std::vector<std::filesystem::path> files;
  std::string error;
};

// Provider-side ingestion boundary for Fab/Quixel exports.
// Atlas owns the normalized library; this service owns only provider ingestion.
class FabIngestService {
 public:
  explicit FabIngestService(std::filesystem::path libraryRoot);

  [[nodiscard]] FabIngestResult ingestJson(const std::string& payload) const;
  [[nodiscard]] const std::filesystem::path& libraryRoot() const noexcept;

 private:
  std::filesystem::path libraryRoot_;
};

}  // namespace atlas
