#pragma once

#include <QByteArray>
#include <QString>
#include <cstdint>
#include <map>

namespace ikv::launcher_skin {
// RT_BITMAP resources of an untrusted PE image, keyed by numeric resource id (first
// language). Each value is a complete BMP file (BITMAPFILEHEADER prepended) for QImage.
// Throws std::runtime_error on any malformed or out-of-bounds structure.
std::map<std::uint32_t, QByteArray> bitmaps(const QByteArray& pe);
// Directory holding the original launcher.exe for a selected game source
// (the source itself, its app/ child, or the parent when base/ was selected); empty if none.
QString originalAppDirectory(const QString& source);
} // namespace ikv::launcher_skin
