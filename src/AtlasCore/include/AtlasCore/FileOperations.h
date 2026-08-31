#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <vector>

namespace atlas {

enum class ConflictPolicy { Ask, Skip, Overwrite, Rename };
enum class FileOperationKind { Copy, Move };

struct FileOperationProgress {
  std::uint64_t completed{};
  std::uint64_t total{};
  std::filesystem::path source;
  std::filesystem::path destination;
};

struct FileOperationResult {
  std::uint64_t succeeded{};
  std::uint64_t skipped{};
  std::vector<std::filesystem::path> failed;
  bool cancelled{};
};

using FileProgressCallback = std::function<void(const FileOperationProgress&)>;

class FileOperations {
 public:
  [[nodiscard]] static FileOperationResult transfer(
      FileOperationKind kind, const std::vector<std::filesystem::path>& sources,
      const std::filesystem::path& destinationDirectory, ConflictPolicy policy,
      std::stop_token stopToken = {}, const FileProgressCallback& progress = {});
  [[nodiscard]] static std::filesystem::path duplicate(const std::filesystem::path& source);
  static void rename(const std::filesystem::path& source, const std::filesystem::path& newPath);
};

}  // namespace atlas
