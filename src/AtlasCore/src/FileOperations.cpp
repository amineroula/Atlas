#include <AtlasCore/FileOperations.h>

#include <stdexcept>
#include <system_error>

namespace atlas {
namespace {

std::filesystem::path uniquePath(const std::filesystem::path& requested) {
  if (!std::filesystem::exists(requested)) return requested;
  const auto parent = requested.parent_path();
  const auto stem = requested.stem().string();
  const auto extension = requested.extension().string();
  for (std::uint64_t number = 2;; ++number) {
    auto candidate = parent / (stem + " (" + std::to_string(number) + ")" + extension);
    if (!std::filesystem::exists(candidate)) return candidate;
  }
}

void copyOne(const std::filesystem::path& source, const std::filesystem::path& destination,
             bool overwrite) {
  const auto options = std::filesystem::copy_options::recursive |
                       (overwrite ? std::filesystem::copy_options::overwrite_existing
                                  : std::filesystem::copy_options::none);
  std::filesystem::copy(source, destination, options);
}

}  // namespace

FileOperationResult FileOperations::transfer(
    FileOperationKind kind, const std::vector<std::filesystem::path>& sources,
    const std::filesystem::path& destinationDirectory, ConflictPolicy policy,
    std::stop_token stopToken, const FileProgressCallback& progress) {
  if (policy == ConflictPolicy::Ask) {
    throw std::invalid_argument("ConflictPolicy::Ask must be resolved by the caller");
  }
  if (!std::filesystem::is_directory(destinationDirectory)) {
    throw std::invalid_argument("destination is not a directory");
  }
  FileOperationResult result;
  for (std::size_t index = 0; index < sources.size(); ++index) {
    if (stopToken.stop_requested()) {
      result.cancelled = true;
      break;
    }
    const auto& source = sources[index];
    auto destination = destinationDirectory / source.filename();
    if (progress) progress({index, sources.size(), source, destination});
    try {
      if (!std::filesystem::exists(source)) throw std::runtime_error("source does not exist");
      if (std::filesystem::exists(destination)) {
        if (policy == ConflictPolicy::Skip) {
          ++result.skipped;
          continue;
        }
        if (policy == ConflictPolicy::Rename) destination = uniquePath(destination);
        if (policy == ConflictPolicy::Overwrite) std::filesystem::remove_all(destination);
      }
      if (kind == FileOperationKind::Copy) {
        copyOne(source, destination, policy == ConflictPolicy::Overwrite);
      } else {
        std::error_code error;
        std::filesystem::rename(source, destination, error);
        if (error) {
          copyOne(source, destination, policy == ConflictPolicy::Overwrite);
          std::filesystem::remove_all(source);
        }
      }
      ++result.succeeded;
    } catch (...) {
      result.failed.push_back(source);
    }
  }
  if (progress) progress({result.succeeded + result.skipped, sources.size(), {}, {}});
  return result;
}

std::filesystem::path FileOperations::duplicate(const std::filesystem::path& source) {
  if (!std::filesystem::exists(source)) throw std::invalid_argument("source does not exist");
  const auto destination = uniquePath(source.parent_path() /
      (source.stem().string() + " copy" + source.extension().string()));
  copyOne(source, destination, false);
  return destination;
}

void FileOperations::rename(const std::filesystem::path& source,
                            const std::filesystem::path& newPath) {
  if (std::filesystem::exists(newPath)) throw std::invalid_argument("destination already exists");
  std::filesystem::rename(source, newPath);
}

}  // namespace atlas
