#include <AtlasCore/FabIngestService.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QString>

#include <initializer_list>
#include <set>
#include <utility>

namespace atlas {
namespace {
QString toQString(const std::filesystem::path& path) { return QDir::fromNativeSeparators(QString::fromStdWString(path.wstring())); }
std::filesystem::path toPath(const QString& path) { return std::filesystem::path(QDir::toNativeSeparators(path).toStdWString()); }
bool looksLikeAssetFile(const QString& value) {
  static const std::set<QString> extensions = {".abc", ".exr", ".fbx", ".glb", ".gltf", ".hdr", ".jpeg", ".jpg", ".json", ".obj", ".png", ".tif", ".tiff", ".usd", ".usda", ".usdc", ".usdz"};
  const auto suffix = QFileInfo(value).suffix();
  return !suffix.isEmpty() && extensions.contains("." + suffix.toLower());
}
void collectPaths(const QJsonValue& value, std::set<QString>* paths) {
  if (value.isString()) { const auto candidate = value.toString(); if (looksLikeAssetFile(candidate) && QFileInfo::exists(candidate)) paths->insert(QDir::cleanPath(QFileInfo(candidate).absoluteFilePath())); return; }
  if (value.isArray()) { for (const auto& child : value.toArray()) collectPaths(child, paths); return; }
  if (value.isObject()) { const auto object = value.toObject(); for (auto it = object.begin(); it != object.end(); ++it) collectPaths(it.value(), paths); }
}
QString firstString(const QJsonObject& object, std::initializer_list<const char*> keys) { for (const auto* key : keys) { const auto value = object.value(QString::fromUtf8(key)); if (value.isString() && !value.toString().trimmed().isEmpty()) return value.toString().trimmed(); } return {}; }
QString safeName(QString value) { value = value.trimmed(); if (value.isEmpty()) value = "fab-asset"; for (auto& ch : value) if (!ch.isLetterOrNumber() && ch != '-' && ch != '_') ch = '-'; while (value.contains("--")) value.replace("--", "-"); return value.left(96); }
QString stableFallbackId(const QByteArray& payload) { return QString::fromLatin1(QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex().left(24)); }
}  // namespace

FabIngestService::FabIngestService(std::filesystem::path libraryRoot) : libraryRoot_(std::move(libraryRoot)) {}
const std::filesystem::path& FabIngestService::libraryRoot() const noexcept { return libraryRoot_; }
FabIngestResult FabIngestService::ingestJson(const std::string& payload) const {
  FabIngestResult result; QJsonParseError parseError; const QByteArray bytes(payload.data(), static_cast<qsizetype>(payload.size())); const auto document = QJsonDocument::fromJson(bytes, &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) { result.error = ("Invalid Fab export JSON: " + parseError.errorString()).toStdString(); return result; }
  const auto object = document.object(); auto assetName = firstString(object, {"assetName", "name", "title"}); auto vendorId = firstString(object, {"assetId", "vendorId", "id", "uid"}); if (vendorId.isEmpty()) vendorId = stableFallbackId(bytes); if (assetName.isEmpty()) assetName = "Fab asset " + vendorId.left(8);
  std::set<QString> discovered; collectPaths(object, &discovered); if (discovered.empty()) { result.error = "Fab payload contains no exported files that exist on disk"; return result; }
  const auto providerRoot = QDir(toQString(libraryRoot_)).filePath("providers/fab"); const auto assetRoot = QDir(providerRoot).filePath(safeName(vendorId)); const auto metadataRoot = QDir(assetRoot).filePath("metadata"); if (!QDir{}.mkpath(metadataRoot)) { result.error = "Could not create Atlas Fab metadata directory"; return result; }
  const auto metadataPath = QDir(metadataRoot).filePath("fab-export.json"); QSaveFile metadata(metadataPath); if (!metadata.open(QIODevice::WriteOnly) || metadata.write(document.toJson(QJsonDocument::Indented)) < 0 || !metadata.commit()) { result.error = ("Could not persist Fab metadata: " + metadata.errorString()).toStdString(); return result; }
  result.ok = true; result.assetName = assetName.toStdString(); result.vendorId = vendorId.toStdString(); result.payloadCopy = toPath(metadataPath); result.sourceDirectory = toPath(QFileInfo(*discovered.begin()).absolutePath()); result.files.reserve(discovered.size()); for (const auto& path : discovered) result.files.push_back(toPath(path)); return result;
}
}  // namespace atlas
