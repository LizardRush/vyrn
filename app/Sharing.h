// Small helpers for sharing (.vrl / .vrlp files, docs/dev/package.md) used by the share and open dialogs, the
// launcher and the project window. The package functions themselves are in vyrn/package.h (std-only).
#pragma once

#include <QImage>
#include <QString>

#include <cstdint>
#include <string>
#include <vector>

class QMimeData;

namespace sharing {

enum class FileKind { None, Brain, Project };   // Brain: .vrl, Project: .vrlp

FileKind kindOf(const QString& path);           // by the extension (any case)
QString packageIn(const QMimeData* data);       // the first local .vrl/.vrlp file in a drag, else ""

QString sizeText(quint64 bytes);                // "412 KB", "3.4 MB", "836 bytes"
QString modeText(const std::string& mode);      // "Simulation" | "Real game: this AI presses keys in the window you choose"

// The picture of "what the AI sees" as a PNG for a package preview, enlarged with nearest-neighbour so a 10×10
// picture is still visible (vyrn::png_encode). Empty when there's no picture.
std::vector<uint8_t> previewPng(const QImage& picture);
QImage imageFromPng(const std::vector<uint8_t>& png);   // null when empty or not a picture

// The author name people put on what they share: remembered in the app settings, never taken from the system.
QString rememberedAuthor();
void rememberAuthor(const QString& author);

QString qs(const std::string& text);            // UTF-8 std::string -> QString

}  // namespace sharing
