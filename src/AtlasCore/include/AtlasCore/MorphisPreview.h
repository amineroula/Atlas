#pragma once

#include <AtlasCore/PbrMaterial.h>

#include <filesystem>
#include <optional>
#include <string>

namespace atlas {

// Data model and JSON writer for the Atlas -> Morphis preview bridge (schema
// morphis.atlas-preview/1). See docs/morphis-preview-bridge.md. This module has no
// Qt or process-launching dependency so it can be unit-tested in isolation; the
// process client that invokes morphis_studio.exe lives in AtlasUI.

enum class MorphisPreviewGeometry { Sphere, Plane, Cube, Cylinder, Model };

struct MorphisPreviewMaps {
  std::optional<std::filesystem::path> baseColor;
  std::optional<std::filesystem::path> normal;
  std::optional<std::filesystem::path> roughness;
  std::optional<std::filesystem::path> glossiness;
  std::optional<std::filesystem::path> metallic;
  std::optional<std::filesystem::path> ambientOcclusion;
  std::optional<std::filesystem::path> height;
  std::optional<std::filesystem::path> opacity;
  std::optional<std::filesystem::path> emissive;
};

struct MorphisPreviewParameters {
  double baseColor[3]{1.0, 1.0, 1.0};
  double roughness{0.5};
  double metallic{0.0};
  double specularIor{1.5};
  double specularLevel{1.0};
  double anisotropy{0.0};
  double anisotropyRotationDegrees{0.0};
  double coatWeight{0.0};
  double coatRoughness{0.1};
  double coatIor{1.5};
  double sheenWeight{0.0};
  double sheenRoughness{0.3};
  double sheenColor[3]{1.0, 1.0, 1.0};
  double normalStrength{1.0};
  double heightScale{0.02};
  double opacity{1.0};
  double emissiveStrength{1.0};
  double uvScale[2]{1.0, 1.0};
  double uvOffset[2]{0.0, 0.0};
  double uvRotationDegrees{0.0};
  std::string normalConvention{"opengl"};
};

struct MorphisPreviewLights {
  std::string preset{"studio-three-point"};
  double keyIntensity{5.0};
  double fillIntensity{1.5};
  double rimIntensity{3.0};
  double exposure{0.0};
};

struct MorphisPreviewCamera {
  double orbitDegrees{25.0};
  double elevationDegrees{18.0};
  double distance{3.2};
  double focalLengthMm{55.0};
};

struct MorphisPreviewOutput {
  int width{1024};
  int height{1024};
  bool transparent{false};
  std::filesystem::path image;
  std::filesystem::path result;
};

struct MorphisPreviewRequest {
  std::string requestId;
  std::string materialName;
  PbrWorkflow workflow{PbrWorkflow::Incomplete};
  MorphisPreviewMaps maps;
  MorphisPreviewParameters parameters;
  MorphisPreviewGeometry geometry{MorphisPreviewGeometry::Sphere};
  std::optional<std::filesystem::path> model;
  std::optional<std::filesystem::path> environment;
  double background[3]{0.035, 0.04, 0.05};
  MorphisPreviewLights lights;
  MorphisPreviewCamera camera;
  MorphisPreviewOutput output;
};

// Returns the v1 JSON map key for a recognized PBR map role, or nullptr for roles
// the v1 schema has no slot for yet (Specular, IOR, Thickness, Translucency, Packed).
[[nodiscard]] const char* morphisPreviewMapKey(PbrMapKind kind) noexcept;
[[nodiscard]] const char* morphisPreviewWorkflowName(PbrWorkflow workflow) noexcept;
[[nodiscard]] const char* morphisPreviewGeometryName(MorphisPreviewGeometry geometry) noexcept;

[[nodiscard]] std::string serializeMorphisPreviewRequest(const MorphisPreviewRequest& request);

}  // namespace atlas
