#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace atlas {

enum class AssetKind { Directory, Image, Video, Audio, Model, Archive, Document, Other };

struct AssetMetadata {
  std::filesystem::path path;
  std::uintmax_t sizeBytes{};
  std::chrono::system_clock::time_point modifiedAt{};
  bool isDirectory{};
  bool isSymlink{};
};

[[nodiscard]] std::string normalizedPath(const std::filesystem::path& path);
[[nodiscard]] AssetKind classifyAsset(const std::filesystem::path& path, bool isDirectory = false);
[[nodiscard]] std::string_view assetKindName(AssetKind kind) noexcept;

}  // namespace atlas
