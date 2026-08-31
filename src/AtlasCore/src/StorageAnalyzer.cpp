#include <AtlasCore/StorageAnalyzer.h>
#include <AtlasCore/Asset.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
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

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
  return value;
}

void addAssetBytes(AssetBreakdown& breakdown, AssetKind kind, std::uintmax_t bytes) {
  switch (kind) {
    case AssetKind::Image: breakdown.images += bytes; break;
    case AssetKind::Video: breakdown.videos += bytes; break;
    case AssetKind::Audio: breakdown.audio += bytes; break;
    case AssetKind::Model: breakdown.models += bytes; break;
    case AssetKind::Archive: breakdown.archives += bytes; break;
    case AssetKind::Document: breakdown.documents += bytes; break;
    case AssetKind::Other: breakdown.other += bytes; break;
    case AssetKind::Directory: break;
  }
}

}  // namespace

StorageAnalysis StorageAnalyzer::analyze(const std::filesystem::path& root,
                                         std::stop_token token, std::size_t resultLimit,
                                         bool findDuplicates,
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
  std::unordered_map<std::string, FileTypeBreakdown> fileTypes;
  std::unordered_map<std::string, PbrMaterialSet> pbrCandidates;
  const auto options = std::filesystem::directory_options::skip_permission_denied;
  std::filesystem::recursive_directory_iterator iterator(root, options, error), end;
  while (iterator != end && !token.stop_requested()) {
    const auto path = iterator->path();
    if (iterator->is_directory(error)) {
      ++result.directories;
      std::filesystem::directory_iterator child(path, options, error);
      if (!error && child == std::filesystem::directory_iterator{}) {
        ++result.emptyFolderCount;
        if (result.emptyFolders.size() < resultLimit) result.emptyFolders.push_back(path);
      }
    } else if (iterator->is_regular_file(error)) {
      const auto size = iterator->file_size(error);
      if (!error) {
        ++result.files;
        result.bytes += size;
        const auto assetKind = classifyAsset(path);
        switch (assetKind) {
          case AssetKind::Image: ++result.assetTypes.images; break;
          case AssetKind::Video: ++result.assetTypes.videos; break;
          case AssetKind::Audio: ++result.assetTypes.audio; break;
          case AssetKind::Model: ++result.assetTypes.models; break;
          case AssetKind::Archive: ++result.assetTypes.archives; break;
          case AssetKind::Document: ++result.assetTypes.documents; break;
          case AssetKind::Other: ++result.assetTypes.other; break;
          case AssetKind::Directory: break;
        }
        addAssetBytes(result.assetBytes, assetKind, size);
        auto extension = lower(path.extension().string());
        if (extension.empty()) extension = "[no extension]";
        auto& type = fileTypes[extension];
        type.extension = extension;
        ++type.files;
        type.bytes += size;
        if (extension == ".fbx") result.fbxFiles.push_back({path, size});
        if (assetKind == AssetKind::Archive) result.archiveFiles.push_back({path, size});
        if (assetKind == AssetKind::Image) {
          if (const auto detected = identifyPbrTexture(path); detected &&
              !detected->canonicalMaterialKey.empty()) {
            const auto key = normalizedPath(path.parent_path()) + "|" +
                             detected->canonicalMaterialKey;
            auto& material = pbrCandidates[key];
            material.name = detected->materialName;
            material.directory = path.parent_path();
            material.textures.push_back({path, detected->kind});
          }
        }
        result.largestFiles.push_back({path, size});
        if (resultLimit != 0 && result.largestFiles.size() > resultLimit * 4) {
          std::sort(result.largestFiles.begin(), result.largestFiles.end(),
                    [](const PathSize& left, const PathSize& right) { return left.bytes > right.bytes; });
          result.largestFiles.resize(resultLimit);
        }
        if (findDuplicates) sizes[size].push_back(path);
        for (auto parent = path.parent_path(); !parent.empty(); parent = parent.parent_path()) {
          folderSizes[parent.generic_string()] += size;
          if (parent == root) break;
        }
      }
    }
    error.clear();
    if (progress && (result.files + result.directories) % 100 == 0) {
      progress(result.files, result.directories, path);
    }
    iterator.increment(error);
    error.clear();
  }
  result.cancelled = token.stop_requested();
  if (progress) progress(result.files, result.directories, {});
  const auto descending = [](const PathSize& left, const PathSize& right) { return left.bytes > right.bytes; };
  std::sort(result.largestFiles.begin(), result.largestFiles.end(), descending);
  if (result.largestFiles.size() > resultLimit) result.largestFiles.resize(resultLimit);
  for (const auto& [path, bytes] : folderSizes) result.largestFolders.push_back({path, bytes});
  std::sort(result.largestFolders.begin(), result.largestFolders.end(), descending);
  if (result.largestFolders.size() > resultLimit) result.largestFolders.resize(resultLimit);

  for (auto& [extension, type] : fileTypes) result.fileTypes.push_back(std::move(type));
  std::sort(result.fileTypes.begin(), result.fileTypes.end(), [](const auto& left, const auto& right) {
    if (left.bytes != right.bytes) return left.bytes > right.bytes;
    return left.extension < right.extension;
  });
  std::sort(result.fbxFiles.begin(), result.fbxFiles.end(), descending);
  std::sort(result.archiveFiles.begin(), result.archiveFiles.end(), descending);

  for (auto& [key, material] : pbrCandidates) {
    std::set<PbrMapKind> kinds;
    for (const auto& texture : material.textures) kinds.insert(texture.kind);
    if (kinds.size() < 2) continue;
    material.hasCoreMaps = classifyPbrWorkflow(kinds) != PbrWorkflow::Incomplete;
    std::sort(material.textures.begin(), material.textures.end(), [](const auto& left, const auto& right) {
      if (left.kind != right.kind) return left.kind < right.kind;
      return left.path < right.path;
    });
    result.pbrMaterials.push_back(std::move(material));
  }
  std::sort(result.pbrMaterials.begin(), result.pbrMaterials.end(), [](const auto& left, const auto& right) {
    if (left.hasCoreMaps != right.hasCoreMaps) return left.hasCoreMaps > right.hasCoreMaps;
    if (left.textures.size() != right.textures.size()) return left.textures.size() > right.textures.size();
    return left.name < right.name;
  });

  if (findDuplicates) for (const auto& [size, paths] : sizes) {
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
