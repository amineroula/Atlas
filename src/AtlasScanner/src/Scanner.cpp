#include <AtlasScanner/Scanner.h>

#include <chrono>
#include <stdexcept>
#include <system_error>

namespace atlas {
namespace {

std::chrono::system_clock::time_point toSystemTime(std::filesystem::file_time_type value) {
  return std::chrono::time_point_cast<std::chrono::system_clock::duration>(
      value - std::filesystem::file_time_type::clock::now() +
      std::chrono::system_clock::now());
}

}  // namespace

ScanSummary Scanner::scan(const std::filesystem::path& root, std::stop_token stopToken,
                          const AssetDiscovered& discovered, const ScanOptions& scanOptions,
                          const ScanProgress& progress) const {
  if (!discovered) throw std::invalid_argument("scan requires a discovery callback");
  std::error_code error;
  if (!std::filesystem::exists(root, error) || error) {
    throw std::invalid_argument("scan root does not exist: " + root.string());
  }

  ScanSummary summary;
  auto options = std::filesystem::directory_options::skip_permission_denied;
  if (scanOptions.followDirectorySymlinks) {
    options |= std::filesystem::directory_options::follow_directory_symlink;
  }
  std::filesystem::recursive_directory_iterator iterator(root, options, error), end;
  if (error) throw std::filesystem::filesystem_error("open scan root", root, error);

  while (iterator != end) {
    if (stopToken.stop_requested()) {
      summary.cancelled = true;
      break;
    }
    const auto& entry = *iterator;
    if (scanOptions.maximumDepth && iterator.depth() >= static_cast<int>(*scanOptions.maximumDepth)) {
      iterator.disable_recursion_pending();
    }
#ifdef _WIN32
    if (!scanOptions.includeHidden) {
      const auto filename = entry.path().filename().wstring();
      if (!filename.empty() && filename.front() == L'.') {
        if (entry.is_directory(error)) iterator.disable_recursion_pending();
        iterator.increment(error);
        error.clear();
        continue;
      }
    }
#else
    if (!scanOptions.includeHidden && entry.path().filename().string().starts_with('.')) {
      if (entry.is_directory(error)) iterator.disable_recursion_pending();
      iterator.increment(error);
      error.clear();
      continue;
    }
#endif
    AssetMetadata metadata{.path = entry.path()};
    metadata.isSymlink = entry.is_symlink(error);
    error.clear();
    metadata.isDirectory = entry.is_directory(error);
    if (error) {
      ++summary.inaccessible;
      error.clear();
    } else {
      if (metadata.isDirectory) {
        ++summary.directories;
      } else {
        metadata.sizeBytes = entry.file_size(error);
        if (error) {
          ++summary.inaccessible;
          metadata.sizeBytes = 0;
          error.clear();
        } else {
          ++summary.files;
          summary.bytes += metadata.sizeBytes;
        }
      }
      const auto modified = entry.last_write_time(error);
      if (!error) metadata.modifiedAt = toSystemTime(modified);
      error.clear();
      discovered(metadata);
      const auto processed = summary.files + summary.directories;
      if (progress && scanOptions.progressInterval != 0 &&
          processed % scanOptions.progressInterval == 0) progress(summary);
    }
    iterator.increment(error);
    if (error) {
      ++summary.inaccessible;
      error.clear();
    }
  }
  if (progress) progress(summary);
  return summary;
}

}  // namespace atlas
