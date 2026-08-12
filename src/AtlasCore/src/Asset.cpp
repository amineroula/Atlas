#include <AtlasCore/Asset.h>

#include <algorithm>
#include <array>
#include <cctype>

namespace atlas {

std::string normalizedPath(const std::filesystem::path& path) {
  std::error_code error;
  auto absolute = std::filesystem::absolute(path, error);
  if (error) absolute = path;
  auto value = absolute.lexically_normal().generic_string();
#ifdef _WIN32
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
#endif
  return value;
}

AssetKind classifyAsset(const std::filesystem::path& path, bool isDirectory) {
  if (isDirectory) return AssetKind::Directory;
  auto extension = path.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
  const auto in = [&extension](const auto& values) {
    return std::find(values.begin(), values.end(), extension) != values.end();
  };
  static constexpr std::array images{".bmp", ".exr", ".gif", ".hdr", ".heic", ".jpeg", ".jpg", ".png", ".psd", ".tga", ".tif", ".tiff", ".webp"};
  static constexpr std::array videos{".avi", ".m4v", ".mkv", ".mov", ".mp4", ".mpeg", ".mpg", ".webm"};
  static constexpr std::array audio{".aac", ".flac", ".m4a", ".mp3", ".ogg", ".wav", ".wma"};
  static constexpr std::array models{".3ds", ".abc", ".blend", ".fbx", ".gltf", ".glb", ".obj", ".ply", ".stl", ".usd", ".usda", ".usdc"};
  static constexpr std::array archives{".7z", ".gz", ".rar", ".tar", ".zip"};
  static constexpr std::array documents{".csv", ".doc", ".docx", ".json", ".md", ".pdf", ".rtf", ".txt", ".xml"};
  if (in(images)) return AssetKind::Image;
  if (in(videos)) return AssetKind::Video;
  if (in(audio)) return AssetKind::Audio;
  if (in(models)) return AssetKind::Model;
  if (in(archives)) return AssetKind::Archive;
  if (in(documents)) return AssetKind::Document;
  return AssetKind::Other;
}

std::string_view assetKindName(AssetKind kind) noexcept {
  switch (kind) {
    case AssetKind::Directory: return "Folder";
    case AssetKind::Image: return "Image";
    case AssetKind::Video: return "Video";
    case AssetKind::Audio: return "Audio";
    case AssetKind::Model: return "3D Model";
    case AssetKind::Archive: return "Archive";
    case AssetKind::Document: return "Document";
    case AssetKind::Other: return "Other";
  }
  return "Other";
}

}  // namespace atlas
