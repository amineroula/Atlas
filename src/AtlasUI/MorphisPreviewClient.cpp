#include "MorphisPreviewClient.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSettings>
#include <QTextStream>
#include <QTimer>

#include <chrono>

namespace atlas {
namespace {

// Filament may need to compile the production material on its first invocation.
// Keep the work asynchronous, but allow enough time for a cold renderer start.
constexpr int kHeadlessTimeoutMs = 120000;

QString toQString(const std::filesystem::path& path) {
  return QString::fromStdWString(path.wstring());
}

qint64 mtimeMs(const std::optional<std::filesystem::path>& path) {
  if (!path) return 0;
  std::error_code error;
  const auto time = std::filesystem::last_write_time(*path, error);
  if (error) return -1;
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::clock_cast<std::chrono::system_clock>(time).time_since_epoch())
      .count();
}

}  // namespace

MorphisPreviewClient::MorphisPreviewClient(QObject* parent) : QObject(parent) {
  QSettings settings;
  executablePath_ = settings.value("morphis/executablePath").toString();
  if (executablePath_.isEmpty() || !QFileInfo::exists(executablePath_)) {
    executablePath_ = autoDetectExecutable();
  }
}

void MorphisPreviewClient::setCacheRoot(QString root) { cacheRoot_ = std::move(root); }

void MorphisPreviewClient::setExecutablePath(QString path) {
  executablePath_ = std::move(path);
  QSettings settings;
  settings.setValue("morphis/executablePath", executablePath_);
}

bool MorphisPreviewClient::isConfigured() const {
  return !executablePath_.isEmpty() && QFileInfo::exists(executablePath_);
}

QString MorphisPreviewClient::autoDetectExecutable() {
  const auto appDir = QCoreApplication::applicationDirPath();
  const QStringList candidates{
      appDir + "/morphis_studio.exe",
      appDir + "/../Morphis/morphis_studio.exe",
      appDir + "/../morphis/morphis_studio.exe",
      appDir + "/../../Morphis/morphis_studio.exe",
      appDir + "/../../../../Morphis/build/morphis_studio.exe",
      appDir + "/../../../../Morphis/installed-local/bin/morphis_studio.exe",
      appDir + "/../../../../Morphis/installed-pbr/bin/morphis_studio.exe",
  };
  for (const auto& candidate : candidates) {
    const QFileInfo info(candidate);
    if (info.exists() && info.isFile()) return info.absoluteFilePath();
  }
  return {};
}

QString MorphisPreviewClient::cacheKeyFor(const MorphisPreviewRequest& request) const {
  QCryptographicHash hash(QCryptographicHash::Sha256);
  const auto addPath = [&](const std::optional<std::filesystem::path>& path) {
    if (!path) return;
    hash.addData(toQString(*path).toLower().toUtf8());
    hash.addData(QByteArray::number(mtimeMs(path)));
  };
  hash.addData("morphis.atlas-preview/1");
  addPath(request.maps.baseColor);
  addPath(request.maps.normal);
  addPath(request.maps.roughness);
  addPath(request.maps.glossiness);
  addPath(request.maps.metallic);
  addPath(request.maps.ambientOcclusion);
  addPath(request.maps.height);
  addPath(request.maps.opacity);
  addPath(request.maps.emissive);
  const auto& p = request.parameters;
  hash.addData(QByteArray::number(p.baseColor[0]) + QByteArray::number(p.baseColor[1]) +
              QByteArray::number(p.baseColor[2]) + QByteArray::number(p.roughness) +
              QByteArray::number(p.metallic) + QByteArray::number(p.specularIor) +
              QByteArray::number(p.specularLevel) + QByteArray::number(p.anisotropy) +
              QByteArray::number(p.anisotropyRotationDegrees) +
              QByteArray::number(p.coatWeight) + QByteArray::number(p.coatRoughness) +
              QByteArray::number(p.coatIor) + QByteArray::number(p.sheenWeight) +
              QByteArray::number(p.sheenRoughness) + QByteArray::number(p.sheenColor[0]) +
              QByteArray::number(p.sheenColor[1]) + QByteArray::number(p.sheenColor[2]) +
              QByteArray::number(p.normalStrength) +
              QByteArray::number(p.heightScale) + QByteArray::number(p.opacity) +
              QByteArray::number(p.emissiveStrength));
  hash.addData(QByteArray::number(p.uvScale[0]) + QByteArray::number(p.uvScale[1]) +
              QByteArray::number(p.uvOffset[0]) + QByteArray::number(p.uvOffset[1]) +
              QByteArray::number(p.uvRotationDegrees) +
              QString::fromStdString(p.normalConvention).toUtf8());
  addPath(request.model);
  addPath(request.environment);
  hash.addData(QString::fromUtf8(morphisPreviewGeometryName(request.geometry)).toUtf8());
  const auto& l = request.lights;
  hash.addData(QString::fromStdString(l.preset).toUtf8());
  hash.addData(QByteArray::number(l.keyIntensity) + QByteArray::number(l.fillIntensity) +
              QByteArray::number(l.rimIntensity) + QByteArray::number(l.exposure));
  const auto& c = request.camera;
  hash.addData(QByteArray::number(c.orbitDegrees) + QByteArray::number(c.elevationDegrees) +
              QByteArray::number(c.distance) + QByteArray::number(c.focalLengthMm));
  hash.addData(QByteArray::number(request.output.width) +
              QByteArray::number(request.output.height) +
              QByteArray::number(request.output.transparent ? 1 : 0));
  return QString::fromLatin1(hash.result().toHex());
}

QString MorphisPreviewClient::requestHeadlessRender(MorphisPreviewRequest request) {
  const auto key = cacheKeyFor(request);
  request.requestId = key.toStdString();
  const auto directory = cacheRoot_ + "/morphis-previews";
  QDir{}.mkpath(directory);
  const auto imagePath = directory + "/" + key + ".png";
  const auto resultPath = directory + "/" + key + ".result.json";
  const auto requestPath = directory + "/" + key + ".request.json";
  request.output.image = std::filesystem::path(imagePath.toStdWString());
  request.output.result = std::filesystem::path(resultPath.toStdWString());

  if (QFileInfo::exists(imagePath) && QFileInfo::exists(resultPath)) {
    QFile resultFile(resultPath);
    if (resultFile.open(QIODevice::ReadOnly)) {
      const auto document = QJsonDocument::fromJson(resultFile.readAll());
      const auto object = document.object();
      if (object.value("status").toString() == "success" &&
          object.value("requestId").toString() == key && QFileInfo::exists(imagePath)) {
        const auto renderer = object.value("renderer").toString("Morphis");
        QTimer::singleShot(0, this, [this, key, imagePath, renderer] {
          emit renderReady(key, imagePath, renderer, true);
        });
        return key;
      }
    }
  }

  if (!isConfigured()) {
    QTimer::singleShot(0, this, [this, key] {
      emit renderFailed(key, "Morphis is not configured (Settings > Choose Morphis executable)");
    });
    return key;
  }

  QFile requestFile(requestPath);
  if (!requestFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    QTimer::singleShot(0, this, [this, key] {
      emit renderFailed(key, "Could not write the preview request to the Atlas cache");
    });
    return key;
  }
  requestFile.write(QByteArray::fromStdString(serializeMorphisPreviewRequest(request)));
  requestFile.close();

  auto* process = new QProcess(this);
  process->setProgram(executablePath_);
  process->setArguments({"--atlas-preview", requestPath, "--headless", "--output", imagePath,
                         "--result", resultPath, "--render-backend", "auto"});
  auto* timeout = new QTimer(process);
  timeout->setSingleShot(true);
  connect(timeout, &QTimer::timeout, process, [process] {
    if (process->state() != QProcess::NotRunning) process->kill();
  });
  connect(process, &QProcess::finished, this, [this, process, key, resultPath, timeout](int, QProcess::ExitStatus) {
    timeout->stop();
    finishProcess(process, key, resultPath);
  });
  connect(process, &QProcess::errorOccurred, this, [this, process, key, resultPath, timeout](QProcess::ProcessError) {
    timeout->stop();
    finishProcess(process, key, resultPath);
  });
  process->start();
  timeout->start(kHeadlessTimeoutMs);
  return key;
}

void MorphisPreviewClient::finishProcess(QProcess* process, const QString& requestId,
                                         const QString& resultPath) {
  if (process->property("atlasResultHandled").toBool()) return;
  process->setProperty("atlasResultHandled", true);
  const auto stderrText = QString::fromUtf8(process->readAllStandardError());
  process->deleteLater();

  QFile resultFile(resultPath);
  if (resultFile.open(QIODevice::ReadOnly)) {
    const auto document = QJsonDocument::fromJson(resultFile.readAll());
    const auto object = document.object();
    const auto resultRequestId = object.value("requestId").toString();
    const auto imagePath = object.value("image").toString();
    if (object.value("status").toString() == "success" &&
        resultRequestId == requestId && QFileInfo::exists(imagePath)) {
      emit renderReady(requestId, imagePath, object.value("renderer").toString("Morphis"), false);
      return;
    }
    const auto warnings = object.value("warnings").toArray();
    QStringList warningTexts;
    for (const auto& warning : warnings) warningTexts << warning.toString();
    QString error = object.value("error").toString();
    if (error.isEmpty() && resultRequestId != requestId) {
      error = "Morphis returned a result for a different request";
    }
    if (error.isEmpty() && !imagePath.isEmpty() && !QFileInfo::exists(imagePath)) {
      error = "Morphis reported success but did not create the preview image";
    }
    if (error.isEmpty() && !warningTexts.isEmpty()) error = warningTexts.join("; ");
    if (error.isEmpty()) error = stderrText.isEmpty() ? "Morphis render failed" : stderrText;
    emit renderFailed(requestId, error);
    return;
  }
  emit renderFailed(requestId,
                    stderrText.isEmpty() ? "Morphis exited without writing a result" : stderrText);
}

void MorphisPreviewClient::openInteractive(MorphisPreviewRequest request) {
  if (!isConfigured()) {
    emit renderFailed(QString::fromStdString(request.requestId),
                      "Morphis is not configured (Settings > Choose Morphis executable)");
    return;
  }
  const auto key = cacheKeyFor(request);
  request.requestId = key.toStdString();
  const auto directory = cacheRoot_ + "/morphis-previews";
  QDir{}.mkpath(directory);
  const auto requestPath = directory + "/" + key + ".request.json";
  const auto imagePath = directory + "/" + key + ".png";
  const auto resultPath = directory + "/" + key + ".result.json";
  request.output.image = std::filesystem::path(imagePath.toStdWString());
  request.output.result = std::filesystem::path(resultPath.toStdWString());

  QFile requestFile(requestPath);
  if (!requestFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    emit renderFailed(key, "Could not write the preview request to the Atlas cache");
    return;
  }
  requestFile.write(QByteArray::fromStdString(serializeMorphisPreviewRequest(request)));
  requestFile.close();

  QProcess::startDetached(executablePath_, {"--atlas-preview", requestPath});
}

}  // namespace atlas
