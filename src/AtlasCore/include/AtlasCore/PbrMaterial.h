#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

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

struct MaterialTextureInput {
  std::int64_t assetId{};
  std::filesystem::path path;
};

struct MaterialModelInput {
  std::int64_t assetId{};
  std::filesystem::path path;
};

struct OrganizedTexture {
  std::int64_t assetId{};
  std::filesystem::path path;
  PbrMapKind kind{};
};

struct OrganizedModel {
  std::int64_t assetId{};
  std::filesystem::path path;
};

struct OrganizedMaterial {
  std::string name;
  std::filesystem::path directory;
  PbrWorkflow workflow{};
  std::vector<OrganizedTexture> textures;
  std::vector<OrganizedModel> models;
};

// Groups texture assets into PBR material sets (folder + normalized name identity,
// filtered to sets with 2+ distinct map roles) and links 3D model assets found in the
// same directory: by normalized filename-token overlap with the material's name, or,
// when a directory holds exactly one material and exactly one model, by proximity.
[[nodiscard]] std::vector<OrganizedMaterial> organizeMaterials(
    const std::vector<MaterialTextureInput>& textures,
    const std::vector<MaterialModelInput>& models);

}  // namespace atlas
