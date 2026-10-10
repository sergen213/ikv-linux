// PE RT_BITMAP walker: synthetic image, truncation/corruption robustness, and the
// user's original launcher.exe when present (argv[1]; skipped if absent).
#include "ikv/launcher_skin.hpp"

#include <QFile>
#include <QImage>
#include <cstdio>
#include <stdexcept>

namespace {
void put(QByteArray& b, int at, std::uint32_t value, int size = 4) {
    for (int i = 0; i < size; ++i) b[at + i] = static_cast<char>((value >> (8 * i)) & 0xff);
}
void check(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }

QByteArray syntheticPe() {
    QByteArray pe(0x200 + 0xa0, '\0');
    put(pe, 0, 0x5a4d, 2); put(pe, 0x3c, 0x40);
    put(pe, 0x40, 0x4550); put(pe, 0x46, 1, 2); put(pe, 0x54, 224, 2); // 1 section, PE32 optional header
    const int optional = 0x58, rsrc = 0x200, size = 0xa0;
    put(pe, optional, 0x10b, 2); put(pe, optional + 92, 16);
    put(pe, optional + 96 + 16, 0x1000); put(pe, optional + 96 + 20, size);
    const int section = optional + 224;
    put(pe, section + 8, size); put(pe, section + 12, 0x1000); put(pe, section + 16, size); put(pe, section + 20, rsrc);
    put(pe, rsrc + 14, 1, 2); put(pe, rsrc + 16, 2); put(pe, rsrc + 20, 0x80000018);        // RT_BITMAP
    put(pe, rsrc + 0x18 + 14, 1, 2); put(pe, rsrc + 0x28, 139); put(pe, rsrc + 0x2c, 0x80000030); // id 139
    put(pe, rsrc + 0x30 + 14, 1, 2); put(pe, rsrc + 0x40, 1055); put(pe, rsrc + 0x44, 0x48);      // language
    put(pe, rsrc + 0x48, 0x1058); put(pe, rsrc + 0x4c, 40 + 16);                                   // data entry
    const int dib = rsrc + 0x58;
    put(pe, dib, 40); put(pe, dib + 4, 2); put(pe, dib + 8, 2); put(pe, dib + 12, 1, 2); put(pe, dib + 14, 24, 2);
    for (int row = 0; row < 2; ++row) { put(pe, dib + 40 + row * 8, 0xff007f, 3); put(pe, dib + 43 + row * 8, 0x00ff00, 3); } // BGR
    return pe;
}
} // namespace

int main(int argc, char** argv) {
    try {
        const QByteArray pe = syntheticPe();
        const auto found = ikv::launcher_skin::bitmaps(pe);
        check(found.size() == 1 && found.count(139), "synthetic bitmap not found");
        const QImage image = QImage::fromData(found.at(139), "BMP");
        check(image.size() == QSize(2, 2) && image.pixel(0, 0) == qRgb(255, 0, 127) && image.pixel(1, 1) == qRgb(0, 255, 0),
              "synthetic bitmap decoded wrongly");
        int rejected = 0;
        for (qsizetype n = 0; n < pe.size(); ++n) {
            try { ikv::launcher_skin::bitmaps(pe.left(n)); } catch (const std::runtime_error&) { ++rejected; }
        }
        check(rejected == pe.size(), "a truncated image was accepted");
        for (qsizetype at = 0; at < pe.size(); ++at) { // every single-byte corruption stays bounded
            QByteArray corrupt = pe; corrupt[at] = static_cast<char>(~corrupt[at]);
            try { ikv::launcher_skin::bitmaps(corrupt); } catch (const std::runtime_error&) {}
        }
        QFile original(argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString());
        if (!original.open(QIODevice::ReadOnly)) { std::puts("launcher-skin: synthetic OK; original launcher.exe absent, skipped"); return 0; }
        const auto skin = ikv::launcher_skin::bitmaps(original.readAll());
        const auto size = [&](std::uint32_t id) { return skin.count(id) ? QImage::fromData(skin.at(id), "BMP").size() : QSize(); };
        check(size(139) == QSize(640, 705) && size(141) == QSize(136, 49) && size(144) == QSize(70, 24) &&
              size(146) == QSize(116, 27) && size(140) == QSize(26, 26) && size(143) == QSize(26, 26), "original skin bitmaps");
        std::printf("launcher-skin: synthetic OK; original %zu bitmaps OK\n", skin.size());
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "launcher-skin: %s\n", e.what());
        return 1;
    }
}
