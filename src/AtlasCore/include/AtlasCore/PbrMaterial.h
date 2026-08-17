#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace atlas {

inline constexpr std::uint32_t PbrOrganizerVersion = 2;

enum class PbrMapKind {
  BaseColor,
  Normal,
  Roughness,
  Glossiness,
  Metallic,
  AmbientOcclusion,
  Height,
  Opacity,
  Translucency,
  Emissive,
  Specular,
  IndexOfRefraction,
  Thickness,
  Packed
};

enum class PbrWorkflow {
  Incomplete,
  MetalRoughness,
  SpecularGlossiness,
  Foliage
};

struct PbrTextureIdentity {
  PbrMapKind kind{};
  std::string materialName;
  std::string canonicalMaterialKey;
  std::string mapVariant;
};

[[nodiscard]] std::optional<PbrTextureIdentity> identifyPbrTexture(
    const std::filesystem::path& path);
[[nodiscard]] PbrWorkflow classifyPbrWorkflow(const std::set<PbrMapKind>& maps) noexcept;
[[nodiscard]] const char* pbrMapKindName(PbrMapKind kind) noexcept;
[[nodiscard]] const char* pbrWorkflowName(PbrWorkflow workflow) noexcept;

}  // namespace atlas
