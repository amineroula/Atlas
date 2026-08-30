#include <AtlasCore/MorphisPreview.h>

#include <cstdio>
#include <sstream>

namespace atlas {
namespace {

std::string pathToUtf8(const std::filesystem::path& path) {
  const auto generic = path.generic_u8string();
  return std::string(generic.begin(), generic.end());
}

std::string jsonEscape(const std::string& value) {
  std::string result;
  result.reserve(value.size() + 8);
  for (const auto character : value) {
    switch (character) {
      case '"': result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\b': result += "\\b"; break;
      case '\f': result += "\\f"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default:
        if (static_cast<unsigned char>(character) < 0x20) {
          char buffer[8];
          std::snprintf(buffer, sizeof buffer, "\\u%04x", character);
          result += buffer;
        } else {
          result += character;
        }
    }
  }
  return result;
}

std::string jsonString(const std::string& value) { return "\"" + jsonEscape(value) + "\""; }
std::string jsonString(const std::filesystem::path& value) { return jsonString(pathToUtf8(value)); }

std::string jsonNumber(double value) {
  char buffer[64];
  std::snprintf(buffer, sizeof buffer, "%g", value);
  return buffer;
}

std::string jsonNumber(int value) { return std::to_string(value); }
std::string jsonBool(bool value) { return value ? "true" : "false"; }

void appendField(std::ostringstream& out, bool& first, const std::string& key,
                  const std::string& value) {
  if (!first) out << ',';
  first = false;
  out << jsonString(key) << ':' << value;
}

std::string mapsJson(const MorphisPreviewMaps& maps) {
  std::ostringstream out;
  out << '{';
  bool first = true;
  const auto add = [&](const char* key, const std::optional<std::filesystem::path>& value) {
    if (!value) return;
    appendField(out, first, key, jsonString(*value));
  };
  add("baseColor", maps.baseColor);
  add("normal", maps.normal);
  add("roughness", maps.roughness);
  add("glossiness", maps.glossiness);
  add("metallic", maps.metallic);
  add("ambientOcclusion", maps.ambientOcclusion);
  add("height", maps.height);
  add("opacity", maps.opacity);
  add("emissive", maps.emissive);
  out << '}';
  return out.str();
}

std::string parametersJson(const MorphisPreviewParameters& parameters) {
  std::ostringstream out;
  out << '{';
  bool first = true;
  appendField(out, first, "baseColor",
              "[" + jsonNumber(parameters.baseColor[0]) + "," +
                  jsonNumber(parameters.baseColor[1]) + "," +
                  jsonNumber(parameters.baseColor[2]) + "]");
  appendField(out, first, "roughness", jsonNumber(parameters.roughness));
  appendField(out, first, "metallic", jsonNumber(parameters.metallic));
  appendField(out, first, "specularIor", jsonNumber(parameters.specularIor));
  appendField(out, first, "specularLevel", jsonNumber(parameters.specularLevel));
  appendField(out, first, "anisotropy", jsonNumber(parameters.anisotropy));
  appendField(out, first, "anisotropyRotationDegrees",
              jsonNumber(parameters.anisotropyRotationDegrees));
  appendField(out, first, "coatWeight", jsonNumber(parameters.coatWeight));
  appendField(out, first, "coatRoughness", jsonNumber(parameters.coatRoughness));
  appendField(out, first, "coatIor", jsonNumber(parameters.coatIor));
  appendField(out, first, "sheenWeight", jsonNumber(parameters.sheenWeight));
  appendField(out, first, "sheenRoughness", jsonNumber(parameters.sheenRoughness));
  appendField(out, first, "sheenColor",
              "[" + jsonNumber(parameters.sheenColor[0]) + "," +
                  jsonNumber(parameters.sheenColor[1]) + "," +
                  jsonNumber(parameters.sheenColor[2]) + "]");
  appendField(out, first, "normalStrength", jsonNumber(parameters.normalStrength));
  appendField(out, first, "heightScale", jsonNumber(parameters.heightScale));
  appendField(out, first, "opacity", jsonNumber(parameters.opacity));
  appendField(out, first, "emissiveStrength", jsonNumber(parameters.emissiveStrength));
  appendField(out, first, "uvScale",
              "[" + jsonNumber(parameters.uvScale[0]) + "," + jsonNumber(parameters.uvScale[1]) + "]");
  appendField(out, first, "uvOffset",
              "[" + jsonNumber(parameters.uvOffset[0]) + "," + jsonNumber(parameters.uvOffset[1]) + "]");
  appendField(out, first, "uvRotationDegrees", jsonNumber(parameters.uvRotationDegrees));
  appendField(out, first, "normalConvention", jsonString(parameters.normalConvention));
  out << '}';
  return out.str();
}

std::string lightsJson(const MorphisPreviewLights& lights) {
  std::ostringstream out;
  out << '{';
  bool first = true;
  appendField(out, first, "preset", jsonString(lights.preset));
  appendField(out, first, "keyIntensity", jsonNumber(lights.keyIntensity));
  appendField(out, first, "fillIntensity", jsonNumber(lights.fillIntensity));
  appendField(out, first, "rimIntensity", jsonNumber(lights.rimIntensity));
  appendField(out, first, "exposure", jsonNumber(lights.exposure));
  out << '}';
  return out.str();
}

std::string cameraJson(const MorphisPreviewCamera& camera) {
  std::ostringstream out;
  out << '{';
  bool first = true;
  appendField(out, first, "orbitDegrees", jsonNumber(camera.orbitDegrees));
  appendField(out, first, "elevationDegrees", jsonNumber(camera.elevationDegrees));
  appendField(out, first, "distance", jsonNumber(camera.distance));
  appendField(out, first, "focalLengthMm", jsonNumber(camera.focalLengthMm));
  out << '}';
  return out.str();
}

std::string outputJson(const MorphisPreviewOutput& output) {
  std::ostringstream out;
  out << '{';
  bool first = true;
  appendField(out, first, "width", jsonNumber(output.width));
  appendField(out, first, "height", jsonNumber(output.height));
  appendField(out, first, "transparent", jsonBool(output.transparent));
  appendField(out, first, "image", jsonString(output.image));
  appendField(out, first, "result", jsonString(output.result));
  out << '}';
  return out.str();
}

}  // namespace

const char* morphisPreviewMapKey(PbrMapKind kind) noexcept {
  switch (kind) {
    case PbrMapKind::BaseColor: return "baseColor";
    case PbrMapKind::Normal: return "normal";
    case PbrMapKind::Roughness: return "roughness";
    case PbrMapKind::Glossiness: return "glossiness";
    case PbrMapKind::Metallic: return "metallic";
    case PbrMapKind::AmbientOcclusion: return "ambientOcclusion";
    case PbrMapKind::Height: return "height";
    case PbrMapKind::Opacity: return "opacity";
    case PbrMapKind::Emissive: return "emissive";
    case PbrMapKind::Specular:
    case PbrMapKind::IndexOfRefraction:
    case PbrMapKind::Thickness:
    case PbrMapKind::Translucency:
    case PbrMapKind::Packed:
      return nullptr;
  }
  return nullptr;
}

const char* morphisPreviewWorkflowName(PbrWorkflow workflow) noexcept {
  switch (workflow) {
    case PbrWorkflow::MetalRoughness: return "metal-roughness";
    case PbrWorkflow::SpecularGlossiness: return "specular-glossiness";
    case PbrWorkflow::Foliage: return "foliage";
    case PbrWorkflow::Incomplete: return "incomplete";
  }
  return "incomplete";
}

const char* morphisPreviewGeometryName(MorphisPreviewGeometry geometry) noexcept {
  switch (geometry) {
    case MorphisPreviewGeometry::Sphere: return "sphere";
    case MorphisPreviewGeometry::Plane: return "plane";
    case MorphisPreviewGeometry::Cube: return "cube";
    case MorphisPreviewGeometry::Cylinder: return "cylinder";
    case MorphisPreviewGeometry::Model: return "model";
  }
  return "sphere";
}

std::string serializeMorphisPreviewRequest(const MorphisPreviewRequest& request) {
  std::ostringstream out;
  out << '{';
  bool first = true;
  appendField(out, first, "schema", jsonString(std::string("morphis.atlas-preview/1")));
  appendField(out, first, "requestId", jsonString(request.requestId));

  std::ostringstream material;
  material << '{';
  bool materialFirst = true;
  appendField(material, materialFirst, "name", jsonString(request.materialName));
  appendField(material, materialFirst, "workflow",
              jsonString(std::string(morphisPreviewWorkflowName(request.workflow))));
  appendField(material, materialFirst, "maps", mapsJson(request.maps));
  appendField(material, materialFirst, "parameters", parametersJson(request.parameters));
  material << '}';
  appendField(out, first, "material", material.str());

  std::ostringstream preview;
  preview << '{';
  bool previewFirst = true;
  appendField(preview, previewFirst, "geometry",
              jsonString(std::string(morphisPreviewGeometryName(request.geometry))));
  appendField(preview, previewFirst, "model",
              request.model ? jsonString(*request.model) : std::string("null"));
  appendField(preview, previewFirst, "environment",
              request.environment ? jsonString(*request.environment) : std::string("null"));
  appendField(preview, previewFirst, "background",
              "[" + jsonNumber(request.background[0]) + "," + jsonNumber(request.background[1]) +
                  "," + jsonNumber(request.background[2]) + "]");
  appendField(preview, previewFirst, "lights", lightsJson(request.lights));
  appendField(preview, previewFirst, "camera", cameraJson(request.camera));
  preview << '}';
  appendField(out, first, "preview", preview.str());

  appendField(out, first, "output", outputJson(request.output));
  out << '}';
  return out.str();
}

}  // namespace atlas
