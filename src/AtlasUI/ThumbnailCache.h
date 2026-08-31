#pragma once

#include <QImage>
#include <QString>

namespace atlas {

class ThumbnailCache {
 public:
  explicit ThumbnailCache(QString root);
  [[nodiscard]] QImage load(const QString& source, const QSize& size) const;
  void clear() const;

 private:
  [[nodiscard]] QString key(const QString& source, const QSize& size) const;
  QString root_;
};

}  // namespace atlas
