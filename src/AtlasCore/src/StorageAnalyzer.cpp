#include <AtlasCore/StorageAnalyzer.h>

#include <algorithm>
#include <fstream>
#include <unordered_map>

namespace atlas {
namespace {

std::uint64_t hashFile(const std::filesystem::path& path, std::stop_token token) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return 0;
  std::uint64_t hash = 14695981039346656037ull;
  char buffer[64 * 1024];
  while (stream && !token.stop_requested()) {
    stream.read(buffer, sizeof buffer);
    const auto count = stream.gcount();
    for (std::streamsize index = 0; index < count; ++index) {
      hash ^= static_cast<unsigned char>(buffer[index]);
      hash *= 1099511628211ull;
    }
  }
  return hash;
}

bool filesEqual(const std::filesystem::path& left, const std::filesystem::path& right,
                std::stop_token token) {
  std::ifstream leftStream(left, std::ios::binary);
  std::ifstream rightStream(right, std::ios::binary);
  if (!leftStream || !rightStream) return false;
  char leftBuffer[64 * 1024];
  char rightBuffer[64 * 1024];
  while (!token.stop_requested()) {
    leftStream.read(leftBuffer, sizeof leftBuffer);
    rightStream.read(rightBuffer, sizeof rightBuffer);
    const auto leftCount = leftStream.gcount();
    if (leftCount != rightStream.gcount()) return false;
    if (!std::equal(leftBuffer, leftBuffer + leftCount, rightBuffer)) return false;
    if (leftCount == 0) return true;
  }
  return false;
}

}  // namespace

StorageAnalysis StorageAnalyzer::analyze(const std::filesystem::path& root,
                                         std::stop_token token, std::size_t resultLimit,
                                         const AnalysisProgress& progress) const {
  StorageAnalysis result;
  std::error_code error;
  const auto space = std::filesystem::space(root, error);
  if (!error) {
    result.capacity = space.capacity;
    result.available = space.available;
  }
  std::unordered_map<std::string, std::uintmax_t> folderSizes;
  std::unordered_map<std::uintmax_t, std::vector<std::filesystem::path>> sizes;
  const auto options = std::filesystem::directory_options::skip_permission_denied;
  std::filesystem::recursive_directory_iterator iterator(root, options, error), end;
  while (iterator != end && !token.stop_requested()) {
    const auto path = iterator->path();
    if (iterator->is_directory(error)) {
      ++result.directories;
      std::filesystem::directory_iterator child(path, options, error);
      if (!error && child == std::filesystem::directory_iterator{}) result.emptyFolders.push_back(path);
    } else if (iterator->is_regular_file(error)) {
      const auto size = iterator->file_size(error);
      if (!error) {
        ++result.files;
        result.bytes += size;
        result.largestFiles.push_back({path, size});
        sizes[size].push_back(path);
        for (auto parent = path.parent_path(); !parent.empty(); parent = parent.parent_path()) {
          folderSizes[parent.generic_string()] += size;
          if (parent == root) break;
        }
      }
    }
    error.clear();
    if (progress && (result.files + result.directories) % 250 == 0) progress(result.files, path);
    iterator.increment(error);
    error.clear();
  }
  result.cancelled = token.stop_requested();
  const auto descending = [](const PathSize& left, const PathSize& right) { return left.bytes > right.bytes; };
  std::sort(result.largestFiles.begin(), result.largestFiles.end(), descending);
  if (result.largestFiles.size() > resultLimit) result.largestFiles.resize(resultLimit);
  for (const auto& [path, bytes] : folderSizes) result.largestFolders.push_back({path, bytes});
  std::sort(result.largestFolders.begin(), result.largestFolders.end(), descending);
  if (result.largestFolders.size() > resultLimit) result.largestFolders.resize(resultLimit);

  for (const auto& [size, paths] : sizes) {
    if (paths.size() < 2 || size == 0 || token.stop_requested()) continue;
    std::unordered_map<std::uint64_t, std::vector<std::filesystem::path>> hashes;
    for (const auto& path : paths) hashes[hashFile(path, token)].push_back(path);
    for (const auto& [hash, matches] : hashes) {
      if (matches.size() < 2 || hash == 0) continue;
      std::vector<std::vector<std::filesystem::path>> exactGroups;
      for (const auto& path : matches) {
        auto group = std::find_if(exactGroups.begin(), exactGroups.end(), [&](const auto& candidate) {
          return filesEqual(candidate.front(), path, token);
        });
        if (group == exactGroups.end()) exactGroups.push_back({path});
        else group->push_back(path);
      }
      for (auto& group : exactGroups) if (group.size() > 1) result.duplicates.push_back({size, std::move(group)});
    }
  }
  std::sort(result.duplicates.begin(), result.duplicates.end(), [](const auto& left, const auto& right) {
    return left.bytesPerFile * left.paths.size() > right.bytesPerFile * right.paths.size();
  });
  if (result.duplicates.size() > resultLimit) result.duplicates.resize(resultLimit);
  return result;
}

}  // namespace atlas
