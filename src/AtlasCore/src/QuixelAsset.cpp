#include <AtlasCore/QuixelAsset.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>

namespace atlas {
namespace {

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string readText(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return {};
  std::ostringstream contents;
  contents << stream.rdbuf();
  return contents.str();
}

std::string unescapeJsonString(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  bool escaped{};
  for (const char character : value) {
    if (!escaped) {
      if (character == '\\') escaped = true;
      else result.push_back(character);
      continue;
    }
    escaped = false;
    switch (character) {
      case 'n': result.push_back('\n'); break;
      case 'r': result.push_back('\r'); break;
      case 't': result.push_back('\t'); break;
      case 'b': result.push_back('\b'); break;
      case 'f': result.push_back('\f'); break;
      default: result.push_back(character); break;
    }
  }
  return result;
}

std::optional<std::string> jsonStringField(const std::string& json, const std::string& key) {
  const std::regex expression("\\\"" + key +
                              "\\\"\\s*:\\s*\\\"((?:\\\\.|[^\\\"])*)\\\"");
  std::smatch match;
  if (!std::regex_search(json, match, expression)) return std::nullopt;
  return unescapeJsonString(match[1].str());
}

std::vector<std::string> jsonStringArrayField(const std::string& json,
                                               const std::string& key) {
  const std::regex arrayExpression("\\\"" + key + "\\\"\\s*:\\s*\\[([^\\]]*)\\]");
  std::smatch arrayMatch;
  if (!std::regex_search(json, arrayMatch, arrayExpression)) return {};
  std::vector<std::string> values;
  const std::string body = arrayMatch[1].str();
  const std::regex stringExpression("\\\"((?:\\\\.|[^\\\"])*)\\\"");
  for (std::sregex_iterator iterator(body.begin(), body.end(), stringExpression), end;
       iterator != end; ++iterator) {
    values.push_back(unescapeJsonString((*iterator)[1].str()));
  }
  return values;
}

int suffixNumber(const std::string& filename, const std::regex& expression) {
  std::smatch match;
  if (!std::regex_search(filename, match, expression)) return -1;
  return std::stoi(match[1].str());
}

bool isInsideNamedDirectory(const std::filesystem::path& path, std::string_view name) {
  for (const auto& component : path) {
    if (lower(component.string()) == name) return true;
  }
  return false;
}

QuixelAssetKind categoryFromPath(const std::filesystem::path& directory,
                                 const std::filesystem::path& root) {
  std::error_code error;
  const auto relative = std::filesystem::relative(directory, root, error);
  if (error || relative.empty()) return QuixelAssetKind::Unknown;
  const auto category = lower((*relative.begin()).string());
  if (category == "3d") return QuixelAssetKind::Model;
  if (category == "3dplant") return QuixelAssetKind::Plant;
  if (category == "atlas") return QuixelAssetKind::Atlas;
  if (category == "surface") return QuixelAssetKind::Surface;
  if (category == "brush") return QuixelAssetKind::Brush;
  return QuixelAssetKind::Unknown;
}

}  // namespace

std::vector<QuixelAsset> organizeQuixelAssets(
    const std::vector<std::filesystem::path>& scannedPaths,
    const std::filesystem::path& scanRoot) {
  std::map<std::filesystem::path, QuixelAsset> assetsByDirectory;
  for (const auto& path : scannedPaths) {
    if (path.empty() || lower(path.extension().string()) != ".json") continue;
    const auto category = categoryFromPath(path.parent_path(), scanRoot);
    if (category == QuixelAssetKind::Unknown) continue;
    const auto json = readText(path);
    const auto id = jsonStringField(json, "id");
    const auto name = jsonStringField(json, "name");
    if (!id || !name) continue;  // Ignore support JSON such as Custom/assetsData.json.
    QuixelAsset asset;
    asset.id = *id;
    asset.name = *name;
    asset.kind = category;
    asset.directory = path.parent_path();
    asset.metadataPath = path;
    asset.categories = jsonStringArrayField(json, "categories");
    asset.tags = jsonStringArrayField(json, "tags");
    assetsByDirectory.try_emplace(asset.directory, std::move(asset));
  }

  const std::regex lodExpression("(?:^|[_-])lod([0-9]+)(?:[_.-]|$)",
                                 std::regex::icase);
  const std::regex resolutionExpression("(?:^|[_-])([1248])k(?:[_.-]|$)",
                                        std::regex::icase);
  for (const auto& path : scannedPaths) {
    if (path.empty() || isInsideNamedDirectory(path, "thumbs") ||
        isInsideNamedDirectory(path, "previews")) {
      continue;
    }
    auto directory = path.parent_path();
    auto owner = assetsByDirectory.end();
    while (!directory.empty()) {
      owner = assetsByDirectory.find(directory);
      if (owner != assetsByDirectory.end() || directory == scanRoot) break;
      const auto parent = directory.parent_path();
      if (parent == directory) break;
      directory = parent;
    }
    if (owner == assetsByDirectory.end()) continue;
    auto& asset = owner->second;
      const auto extension = lower(path.extension().string());
      const auto filename = lower(path.filename().string());
      if (extension == ".fbx" || extension == ".obj") {
        asset.meshes.push_back({path, suffixNumber(filename, lodExpression)});
        continue;
      }
      if (extension != ".jpg" && extension != ".jpeg" && extension != ".png" &&
          extension != ".tif" && extension != ".tiff" && extension != ".exr") {
        continue;
      }
      if (filename.find("preview") != std::string::npos ||
          filename.find("thumb") != std::string::npos) {
        if (asset.previewPath.empty() || extension == ".png") asset.previewPath = path;
        continue;
      }
      const auto identity = identifyPbrTexture(path);
      const int resolutionMarker = suffixNumber(filename, resolutionExpression);
      asset.textures.push_back({path, identity ? std::optional(identity->kind) : std::nullopt,
                                suffixNumber(filename, lodExpression),
                                resolutionMarker < 0 ? 0 : resolutionMarker * 1024});
  }

  std::vector<QuixelAsset> result;
  for (auto& [directory, asset] : assetsByDirectory) {
    std::sort(asset.meshes.begin(), asset.meshes.end(), [](const auto& left, const auto& right) {
      return left.lod < right.lod;
    });
    if (!asset.meshes.empty() || !asset.textures.empty()) result.push_back(std::move(asset));
  }
  std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
    return lower(left.name) < lower(right.name);
  });
  return result;
}

const char* quixelAssetKindName(QuixelAssetKind kind) noexcept {
  switch (kind) {
    case QuixelAssetKind::Model: return "3D Asset";
    case QuixelAssetKind::Plant: return "3D Plant";
    case QuixelAssetKind::Atlas: return "Atlas";
    case QuixelAssetKind::Surface: return "Surface";
    case QuixelAssetKind::Brush: return "Brush";
    case QuixelAssetKind::Unknown: return "Unknown";
  }
  return "Unknown";
}

}  // namespace atlas
