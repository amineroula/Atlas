#include "AtlasLibraryManifest.h"

#include <AtlasCore/MorphisPreview.h>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

#include <map>

namespace atlas {
namespace {

QString pathString(const std::filesystem::path& path) {
  return QDir::fromNativeSeparators(QString::fromStdWString(path.wstring()));
}

QString stableMaterialUri(const CatalogMaterial& material,
                          const std::vector<QuixelAsset>& quixelAssets) {
  const auto directory = pathString(material.directory).toLower();
  for (const auto& asset : quixelAssets) {
    if (pathString(asset.directory).toLower() == directory && !asset.id.empty()) {
      return "atlas://quixel/" + QString::fromStdString(asset.id) + "/material/default";
    }
  }
  const auto identity = directory + "|" + QString::fromStdString(material.name).toLower();
  const auto digest = QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256)
                          .toHex().left(32);
  return "atlas://material/" + QString::fromLatin1(digest);
}

int texturePreference(const CatalogMaterialTexture& texture) {
  const auto filename = pathString(texture.path.filename());
  static const QRegularExpression lodExpression("(?:^|[_-])LOD([0-9]+)(?:[_.-]|$)",
                                                 QRegularExpression::CaseInsensitiveOption);
  const auto match = lodExpression.match(filename);
  // A map with no LOD suffix is the authored master. When Quixel supplies only
  // LOD-specific normals, LOD0 is the highest-detail source.
  return match.hasMatch() ? 1000 - match.captured(1).toInt() : 2000;
}

std::vector<const CatalogMaterialTexture*> selectedMaterialTextures(
    const CatalogMaterial& material) {
  std::map<PbrMapKind, const CatalogMaterialTexture*> selected;
  for (const auto& texture : material.textures) {
    const auto current = selected.find(texture.kind);
    if (current == selected.end() || texturePreference(texture) > texturePreference(*current->second)) {
      selected[texture.kind] = &texture;
    }
  }
  std::vector<const CatalogMaterialTexture*> result;
  result.reserve(selected.size());
  for (const auto& [kind, texture] : selected) result.push_back(texture);
  return result;
}

QString materialPreview(const std::vector<const CatalogMaterialTexture*>& textures) {
  for (const auto* texture : textures) {
    if (texture->kind == PbrMapKind::BaseColor) return pathString(texture->path);
  }
  return textures.empty() ? QString{} : pathString(textures.front()->path);
}

QJsonObject materialJson(const CatalogMaterial& material,
                         const std::vector<QuixelAsset>& quixelAssets) {
  const auto selectedTextures = selectedMaterialTextures(material);
  QJsonArray maps;
  for (const auto* texture : selectedTextures) {
    maps.append(QJsonObject{{"role", QString::fromUtf8(morphisPreviewMapKey(texture->kind)
                                                           ? morphisPreviewMapKey(texture->kind)
                                                           : pbrMapKindName(texture->kind))},
                            {"path", pathString(texture->path)}});
  }
  QJsonArray models;
  for (const auto& model : material.models) models.append(pathString(model.path));
  return QJsonObject{{"id", stableMaterialUri(material, quixelAssets)},
                     {"name", QString::fromStdString(material.name)},
                     {"directory", pathString(material.directory)},
                     {"workflow", QString::fromUtf8(pbrWorkflowName(material.workflow))},
                     {"preview", materialPreview(selectedTextures)},
                     {"maps", maps},
                     {"models", models}};
}

QJsonObject quixelJson(const QuixelAsset& asset) {
  QJsonArray meshes;
  for (const auto& mesh : asset.meshes) {
    meshes.append(QJsonObject{{"path", pathString(mesh.path)}, {"lod", mesh.lod}});
  }
  QJsonArray textures;
  for (const auto& texture : asset.textures) {
    QJsonObject value{{"path", pathString(texture.path)},
                      {"lod", texture.lod},
                      {"resolution", texture.resolution}};
    if (texture.kind) value.insert("role", QString::fromUtf8(pbrMapKindName(*texture.kind)));
    textures.append(value);
  }
  QJsonArray tags;
  for (const auto& tag : asset.tags) tags.append(QString::fromStdString(tag));
  return QJsonObject{{"id", "atlas://quixel/" + QString::fromStdString(asset.id)},
                     {"vendorId", QString::fromStdString(asset.id)},
                     {"name", QString::fromStdString(asset.name)},
                     {"type", QString::fromUtf8(quixelAssetKindName(asset.kind))},
                     {"directory", pathString(asset.directory)},
                     {"metadata", pathString(asset.metadataPath)},
                     {"preview", pathString(asset.previewPath)},
                     {"tags", tags},
                     {"meshes", meshes},
                     {"textures", textures}};
}

}  // namespace

QString atlasLibraryManifestPath() {
  return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
      .filePath("Morphis/Atlas/library-v1.json");
}

bool writeAtlasLibraryManifest(const std::vector<CatalogMaterial>& materials,
                               const std::vector<QuixelAsset>& quixelAssets,
                               QString* error) {
  QJsonArray materialValues;
  for (const auto& material : materials) materialValues.append(materialJson(material, quixelAssets));
  QJsonArray assetValues;
  for (const auto& asset : quixelAssets) assetValues.append(quixelJson(asset));
  QJsonObject root{{"schema", "morphis.atlas-library/1"},
                   {"generatedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                   {"materials", materialValues},
                   {"assets", assetValues}};
  const auto path = atlasLibraryManifestPath();
  if (!QDir{}.mkpath(QFileInfo(path).absolutePath())) {
    if (error) *error = "Could not create the Atlas bridge directory";
    return false;
  }
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly)) {
    if (error) *error = file.errorString();
    return false;
  }
  if (file.write(QJsonDocument(root).toJson(QJsonDocument::Compact)) < 0 || !file.commit()) {
    if (error) *error = file.errorString();
    return false;
  }
  return true;
}

}  // namespace atlas
