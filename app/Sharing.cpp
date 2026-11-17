// Sharing helpers (see Sharing.h).
#include "Sharing.h"

#include <QFileInfo>
#include <QLocale>
#include <QMimeData>
#include <QSettings>
#include <QUrl>

#include <algorithm>

#include "vyrn/package.h"
#include "vyrn/project.h"

namespace sharing {

namespace {

constexpr int kPreviewSide = 256;   // previews are enlarged to about this many pixels on their longest side
const QString kAuthorKey = QStringLiteral("share/author");

}  // namespace

QString qs(const std::string& text) { return QString::fromStdString(text); }

FileKind kindOf(const QString& path) {
  const QString suffix = QStringLiteral(".") + QFileInfo(path).suffix().toLower();
  if (suffix == QLatin1String(vyrn::kProjectExt)) return FileKind::Project;
  if (suffix == QLatin1String(vyrn::kBrainExt)) return FileKind::Brain;
  return FileKind::None;
}

QString packageIn(const QMimeData* data) {
  if (!data || !data->hasUrls()) return {};
  for (const QUrl& url : data->urls()) {
    if (!url.isLocalFile()) continue;
    const QString file = url.toLocalFile();
    if (kindOf(file) != FileKind::None && QFileInfo(file).isFile()) return file;
  }
  return {};
}

QString sizeText(quint64 bytes) {   // "412 KB": the words people know (1 KB = 1024 bytes)
  if (bytes < 1024) return bytes == 1 ? QStringLiteral("1 byte") : QStringLiteral("%1 bytes").arg(bytes);
  const double kb = double(bytes) / 1024.0;
  if (kb < 1000) return QStringLiteral("%1 KB").arg(QLocale().toString(kb, 'f', 0));
  const double mb = kb / 1024.0;
  if (mb < 1000) return QStringLiteral("%1 MB").arg(QLocale().toString(mb, 'f', mb < 10 ? 1 : 0));
  return QStringLiteral("%1 GB").arg(QLocale().toString(mb / 1024.0, 'f', 1));
}

QString modeText(const std::string& mode) {
  if (mode == "realtime") return QStringLiteral("Real game: this AI presses keys in the window you choose");
  if (mode == "sim") return QStringLiteral("Simulation");
  return {};
}

std::vector<uint8_t> previewPng(const QImage& picture) {
  if (picture.isNull() || picture.width() <= 0 || picture.height() <= 0) return {};
  const QImage rgb = picture.convertToFormat(QImage::Format_RGB888);
  const int w = rgb.width(), h = rgb.height();
  std::vector<uint8_t> bytes(size_t(w) * size_t(h) * 3);
  const size_t row = size_t(w) * 3;
  for (int y = 0; y < h; ++y) std::copy_n(rgb.constScanLine(y), row, bytes.data() + size_t(y) * row);
  const int scale = std::clamp(kPreviewSide / std::max(w, h), 1, 32);
  try {
    return vyrn::png_encode(w, h, bytes, scale);
  } catch (const std::exception&) {
    return {};
  }
}

QImage imageFromPng(const std::vector<uint8_t>& png) {
  if (png.empty()) return {};
  QImage image;
  image.loadFromData(png.data(), int(png.size()), "PNG");
  return image;
}

QString rememberedAuthor() { return QSettings().value(kAuthorKey).toString(); }

void rememberAuthor(const QString& author) { QSettings().setValue(kAuthorKey, author.trimmed()); }

}  // namespace sharing
