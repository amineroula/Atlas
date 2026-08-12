#include "ThumbnailCache.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>

namespace atlas {

ThumbnailCache::ThumbnailCache(QString root) : root_(std::move(root)) {
  QDir{}.mkpath(root_);
}

QString ThumbnailCache::key(const QString& source, const QSize& size) const {
  const QFileInfo info(source);
  const auto fingerprint = source.toUtf8() + '|' + QByteArray::number(info.size()) + '|' +
      QByteArray::number(info.lastModified().toMSecsSinceEpoch()) + '|' +
      QByteArray::number(size.width()) + 'x' + QByteArray::number(size.height());
  return root_ + '/' + QCryptographicHash::hash(fingerprint, QCryptographicHash::Sha256).toHex() + ".png";
}

QImage ThumbnailCache::load(const QString& source, const QSize& size) const {
  const auto cachePath = key(source, size);
  QImage cached(cachePath);
  if (!cached.isNull()) return cached;
  QImageReader reader(source);
  reader.setAutoTransform(true);
  const auto original = reader.size();
  if (original.isValid()) reader.setScaledSize(original.scaled(size, Qt::KeepAspectRatio));
  const auto image = reader.read();
  if (image.isNull()) return {};
  QDir{}.mkpath(root_);
  image.save(cachePath, "PNG");
  return image;
}

void ThumbnailCache::clear() const {
  QDir directory(root_);
  for (const auto& name : directory.entryList({"*.png"}, QDir::Files)) directory.remove(name);
}

}  // namespace atlas
