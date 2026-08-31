#pragma once

#include <AtlasCore/MorphisPreview.h>

#include <QHash>
#include <QObject>
#include <QString>

class QProcess;

namespace atlas {

// Owns the Atlas side of the Atlas -> Morphis preview bridge (see
// docs/morphis-preview-bridge.md): resolves morphis_studio.exe, writes preview
// request JSON beneath the Atlas cache root, launches it asynchronously via
// QProcess (never blocking the UI thread), and reports headless render results
// back through Qt signals keyed by requestId so stale results can be discarded.
class MorphisPreviewClient final : public QObject {
  Q_OBJECT
 public:
  explicit MorphisPreviewClient(QObject* parent = nullptr);

  void setCacheRoot(QString root);
  void setExecutablePath(QString path);
  [[nodiscard]] QString executablePath() const { return executablePath_; }
  [[nodiscard]] bool isConfigured() const;
  [[nodiscard]] static QString autoDetectExecutable();

  // Fills output paths and a stable cache-key requestId into `request`, then
  // either serves an existing cached render or launches Morphis headless.
  // Returns the requestId immediately; the outcome arrives via the signals below.
  QString requestHeadlessRender(MorphisPreviewRequest request);

  // Launches Morphis interactively (no --headless) so the user can tweak the
  // material live; fire-and-forget, does not wait for the process to exit.
  void openInteractive(MorphisPreviewRequest request);

 signals:
  void renderReady(QString requestId, QString imagePath, QString renderer, bool fromCache);
  void renderFailed(QString requestId, QString error);

 private:
  [[nodiscard]] QString cacheKeyFor(const MorphisPreviewRequest& request) const;
  void finishProcess(QProcess* process, const QString& requestId, const QString& resultPath);

  QString cacheRoot_;
  QString executablePath_;
};

}  // namespace atlas
