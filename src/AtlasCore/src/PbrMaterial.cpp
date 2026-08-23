#include <AtlasCore/PbrMaterial.h>
#include <AtlasCore/Asset.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace atlas {
namespace {

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
  return value;
}

std::vector<std::string> tokensFromString(std::string value) {
  value = lower(std::move(value));
  for (auto& character : value) {
    if (!std::isalnum(static_cast<unsigned char>(character))) character = ' ';
  }
  std::vector<std::string> tokens;
  std::string token;
  for (const auto character : value) {
    if (character == ' ') {
      if (!token.empty()) tokens.push_back(std::move(token));
      token.clear();
    } else {
      token.push_back(character);
    }
  }
  if (!token.empty()) tokens.push_back(std::move(token));
  return tokens;
}

std::vector<std::string> tokensFor(const std::filesystem::path& path) {
  return tokensFromString(path.stem().string());
}

std::string withoutTrailingDigits(std::string value) {
  while (!value.empty() && std::isdigit(static_cast<unsigned char>(value.back()))) {
    value.pop_back();
  }
  return value;
}

std::string trailingDigits(const std::string& value) {
  const auto start = std::find_if(value.rbegin(), value.rend(),
                                  [](unsigned char character) {
                                    return !std::isdigit(character);
                                  }).base();
  return std::string(start, value.end());
}

std::optional<PbrMapKind> roleForToken(const std::string& original) {
  const auto token = withoutTrailingDigits(original);
  if (token == "basecolor" || token == "albedo" || token == "diffuse" ||
      token == "diff" || token == "color" || token == "colour") return PbrMapKind::BaseColor;
  if (token == "normal" || token == "norma" || token == "normalgl" ||
      token == "normaldx" || token == "nrm" || token == "nor") return PbrMapKind::Normal;
  if (token == "roughness" || token == "rough") return PbrMapKind::Roughness;
  if (token == "glossiness" || token == "gloss" || token == "smoothness") {
    return PbrMapKind::Glossiness;
  }
  if (token == "metallic" || token == "metalness" || token == "metal") {
    return PbrMapKind::Metallic;
  }
  if (token == "ao" || token == "ambientocclusion" || token == "occlusion") {
    return PbrMapKind::AmbientOcclusion;
  }
  if (token == "height" || token == "bump" || token == "displacement" || token == "disp") {
    return PbrMapKind::Height;
  }
  if (token == "opacity" || token == "alpha" || token == "mask" ||
      token == "fraction" || token == "fractionall") return PbrMapKind::Opacity;
  if (token == "translucency" || token == "translussency" || token == "translucent" ||
      token == "transmissive" || token == "transmission" || token == "transfert" ||
      token == "transparency" || token == "transparent") return PbrMapKind::Translucency;
  if (token == "emissive" || token == "emission" || token == "emit") {
    return PbrMapKind::Emissive;
  }
  if (token == "specular" || token == "spec" || token == "reflection") {
    return PbrMapKind::Specular;
  }
  if (token == "ior") return PbrMapKind::IndexOfRefraction;
  if (token == "thickness") return PbrMapKind::Thickness;
  if (token == "orm" || token == "rma" || token == "arm" || token == "mra") {
    return PbrMapKind::Packed;
  }
  return std::nullopt;
}

bool genericIdentityToken(const std::string& token) {
  static const std::unordered_set<std::string> generic{
      "material", "defaultmaterial", "texture", "textures", "tex", "map", "maps",
      "udim", "gl", "dx", "directx", "opengl", "back", "front", "test", "n"};
  if (generic.contains(token)) return true;
  if (token == "1k" || token == "2k" || token == "4k" || token == "8k" ||
      token == "16k" || token == "24k") return true;
  if (token.size() > 1 && token.back() == 'k' &&
      std::all_of(token.begin(), token.end() - 1,
                  [](unsigned char value) { return std::isdigit(value); })) return true;
  return false;
}

std::string join(const std::vector<std::string>& tokens, std::string_view separator) {
  std::string result;
  for (const auto& token : tokens) {
    if (!result.empty()) result += separator;
    result += token;
  }
  return result;
}

}  // namespace

std::optional<PbrTextureIdentity> identifyPbrTexture(const std::filesystem::path& path) {
  const auto tokens = tokensFor(path);
  std::optional<std::size_t> roleIndex;
  std::optional<PbrMapKind> kind;
  for (std::size_t index = 0; index < tokens.size(); ++index) {
    if (tokens[index] == "base" && index + 1 < tokens.size() && tokens[index + 1] == "color") {
      roleIndex = index;
      kind = PbrMapKind::BaseColor;
      break;
    }
    if (tokens[index] == "ambient" && index + 1 < tokens.size() &&
        tokens[index + 1] == "occlusion") {
      roleIndex = index;
      kind = PbrMapKind::AmbientOcclusion;
      break;
    }
    if (const auto detected = roleForToken(tokens[index])) {
      roleIndex = index;
      kind = detected;
      break;
    }
  }
  if (!roleIndex || !kind) return std::nullopt;

  std::vector<std::string> identityTokens;
  for (std::size_t index = 0; index < *roleIndex; ++index) {
    if (!genericIdentityToken(tokens[index])) identityTokens.push_back(tokens[index]);
  }
  const auto variant = trailingDigits(tokens[*roleIndex]);
  if (identityTokens.empty()) return PbrTextureIdentity{*kind, {}, {}, variant};
  return PbrTextureIdentity{
      *kind, join(identityTokens, "_"), join(identityTokens, "_"), variant};
}

PbrWorkflow classifyPbrWorkflow(const std::set<PbrMapKind>& maps) noexcept {
  const bool color = maps.contains(PbrMapKind::BaseColor);
  const bool normal = maps.contains(PbrMapKind::Normal);
  if (color && normal &&
      (maps.contains(PbrMapKind::Roughness) || maps.contains(PbrMapKind::Packed))) {
    return PbrWorkflow::MetalRoughness;
  }
  if (color && normal &&
      (maps.contains(PbrMapKind::Glossiness) || maps.contains(PbrMapKind::Specular))) {
    return PbrWorkflow::SpecularGlossiness;
  }
  if (color && normal &&
      (maps.contains(PbrMapKind::Opacity) || maps.contains(PbrMapKind::Translucency))) {
    return PbrWorkflow::Foliage;
  }
  return PbrWorkflow::Incomplete;
}

const char* pbrMapKindName(PbrMapKind kind) noexcept {
  switch (kind) {
    case PbrMapKind::BaseColor: return "Base Color / Diffuse";
    case PbrMapKind::Normal: return "Normal";
    case PbrMapKind::Roughness: return "Roughness";
    case PbrMapKind::Glossiness: return "Glossiness";
    case PbrMapKind::Metallic: return "Metallic";
    case PbrMapKind::AmbientOcclusion: return "Ambient Occlusion";
    case PbrMapKind::Height: return "Height / Displacement";
    case PbrMapKind::Opacity: return "Opacity / Mask";
    case PbrMapKind::Translucency: return "Translucency / Transmission";
    case PbrMapKind::Emissive: return "Emissive";
    case PbrMapKind::Specular: return "Specular / Reflection";
    case PbrMapKind::IndexOfRefraction: return "Index of Refraction";
    case PbrMapKind::Thickness: return "Thickness";
    case PbrMapKind::Packed: return "Packed ORM/RMA";
  }
  return "Unknown";
}

const char* pbrWorkflowName(PbrWorkflow workflow) noexcept {
  switch (workflow) {
    case PbrWorkflow::Incomplete: return "Candidate / incomplete";
    case PbrWorkflow::MetalRoughness: return "Complete - Metal/Roughness";
    case PbrWorkflow::SpecularGlossiness: return "Complete - Specular/Glossiness";
    case PbrWorkflow::Foliage: return "Complete - Foliage/Opacity";
  }
  return "Candidate / incomplete";
}

namespace {

bool tokensOverlap(const std::vector<std::string>& left, const std::vector<std::string>& right) {
  for (const auto& token : left) {
    if (token.size() < 3) continue;
    for (const auto& other : right) {
      if (token == other) return true;
    }
  }
  return false;
}

}  // namespace

std::vector<OrganizedMaterial> organizeMaterials(
    const std::vector<MaterialTextureInput>& textures,
    const std::vector<MaterialModelInput>& models) {
  struct Identified {
    const MaterialTextureInput* texture;
    PbrTextureIdentity identity;
  };
  std::vector<Identified> identified;
  identified.reserve(textures.size());
  for (const auto& texture : textures) {
    if (auto identity = identifyPbrTexture(texture.path)) {
      identified.push_back({&texture, std::move(*identity)});
    }
  }

  // First pass: only split a material into named variants (e.g. "leaf1" vs "leaf5")
  // when at least two distinct map roles corroborate that variant under this material.
  std::map<std::string, std::map<std::string, std::set<std::string>>> variantEvidence;
  for (const auto& entry : identified) {
    if (entry.identity.canonicalMaterialKey.empty() || entry.identity.mapVariant.empty()) continue;
    const auto baseKey = normalizedPath(entry.texture->path.parent_path()) + "|" +
                         entry.identity.canonicalMaterialKey;
    variantEvidence[baseKey][entry.identity.mapVariant].insert(
        pbrMapKindName(entry.identity.kind));
  }

  struct Group {
    std::string name;
    std::filesystem::path directory;
    std::vector<OrganizedTexture> textures;
    std::set<PbrMapKind> kinds;
  };
  std::vector<std::string> order;
  std::unordered_map<std::string, Group> groups;
  for (const auto& entry : identified) {
    const auto directory = entry.texture->path.parent_path();
    const bool folderIdentity = entry.identity.canonicalMaterialKey.empty();
    auto name = folderIdentity ? directory.filename().string() : entry.identity.materialName;
    const auto baseKey = normalizedPath(directory) + "|" +
                         (folderIdentity ? "@dedicated-folder" : entry.identity.canonicalMaterialKey);
    const auto& variant = entry.identity.mapVariant;
    const bool provenVariant =
        !variant.empty() && variantEvidence[baseKey][variant].size() >= 2;
    const auto key = provenVariant ? baseKey + "|variant:" + variant : baseKey;
    if (provenVariant) name += " - variant " + variant;

    auto [iterator, inserted] = groups.try_emplace(key);
    if (inserted) {
      iterator->second.name = name;
      iterator->second.directory = directory;
      order.push_back(key);
    }
    iterator->second.textures.push_back(
        {entry.texture->assetId, entry.texture->path, entry.identity.kind});
    iterator->second.kinds.insert(entry.identity.kind);
  }

  std::vector<OrganizedMaterial> materials;
  std::unordered_map<std::string, std::vector<std::size_t>> materialsByDirectory;
  for (const auto& key : order) {
    auto& group = groups.at(key);
    if (group.kinds.size() < 2) continue;
    OrganizedMaterial material;
    material.name = group.name;
    material.directory = group.directory;
    material.workflow = classifyPbrWorkflow(group.kinds);
    material.textures = std::move(group.textures);
    std::sort(material.textures.begin(), material.textures.end(),
              [](const auto& left, const auto& right) {
                if (left.kind != right.kind) return left.kind < right.kind;
                return left.path < right.path;
              });
    materialsByDirectory[normalizedPath(group.directory)].push_back(materials.size());
    materials.push_back(std::move(material));
  }

  std::unordered_map<std::string, std::vector<const MaterialModelInput*>> modelsByDirectory;
  for (const auto& model : models) {
    modelsByDirectory[normalizedPath(model.path.parent_path())].push_back(&model);
  }

  for (auto& [directory, modelPointers] : modelsByDirectory) {
    const auto materialIndicesIt = materialsByDirectory.find(directory);
    if (materialIndicesIt == materialsByDirectory.end()) continue;
    const auto& materialIndices = materialIndicesIt->second;

    if (materialIndices.size() == 1 && modelPointers.size() == 1) {
      materials[materialIndices.front()].models.push_back(
          {modelPointers.front()->assetId, modelPointers.front()->path});
      continue;
    }
    for (const auto* model : modelPointers) {
      const auto modelTokens = tokensFor(model->path);
      for (const auto index : materialIndices) {
        const auto materialTokens = tokensFromString(materials[index].name);
        if (tokensOverlap(modelTokens, materialTokens)) {
          materials[index].models.push_back({model->assetId, model->path});
        }
      }
    }
  }

  return materials;
}

}  // namespace atlas
