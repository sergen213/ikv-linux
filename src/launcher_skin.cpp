#include "ikv/launcher_skin.hpp"

#include <QDir>
#include <QFileInfo>
#include <stdexcept>
#include <string>

namespace ikv::launcher_skin {
namespace {
[[noreturn]] void bad(const char* what) { throw std::runtime_error(std::string("Malformed launcher.exe: ") + what); }

// Bounds-checked little-endian reads; offsets are 64-bit so sums of 32-bit fields cannot wrap.
struct Reader {
    const QByteArray& bytes;
    void need(std::uint64_t at, std::uint64_t size) const {
        const auto total = static_cast<std::uint64_t>(bytes.size());
        if (at > total || size > total - at) bad("structure outside file");
    }
    std::uint32_t byte(std::uint64_t at) const { return static_cast<unsigned char>(bytes[static_cast<qsizetype>(at)]); }
    std::uint32_t u16(std::uint64_t at) const { need(at, 2); return byte(at) | byte(at + 1) << 8; }
    std::uint32_t u32(std::uint64_t at) const { need(at, 4); return u16(at) | u16(at + 2) << 16; }
};

QByteArray bmpFile(const QByteArray& dib) {
    const Reader r{dib};
    const std::uint32_t header = r.u32(0);
    if (header < 40 || header > static_cast<std::uint64_t>(dib.size())) bad("bitmap header");
    const std::uint32_t bits = r.u16(14), compression = r.u32(16), used = r.u32(32);
    if (bits != 1 && bits != 4 && bits != 8 && bits != 16 && bits != 24 && bits != 32) bad("bitmap depth");
    const std::uint64_t colors = used ? used : bits <= 8 ? 1u << bits : 0;
    const std::uint64_t masks = header == 40 && compression == 3 ? 12 : 0;
    const std::uint64_t pixels = header + masks + colors * 4;
    if (pixels > static_cast<std::uint64_t>(dib.size())) bad("bitmap palette");
    QByteArray file(14, '\0');
    const auto put = [&file](int at, std::uint64_t value) {
        for (int i = 0; i < 4; ++i) file[at + i] = static_cast<char>((value >> (8 * i)) & 0xff);
    };
    file[0] = 'B'; file[1] = 'M';
    put(2, 14 + static_cast<std::uint64_t>(dib.size())); put(10, 14 + pixels);
    return file + dib;
}
} // namespace

std::map<std::uint32_t, QByteArray> bitmaps(const QByteArray& pe) {
    const Reader r{pe};
    if (r.u16(0) != 0x5a4d) bad("DOS signature");
    const std::uint64_t nt = r.u32(0x3c);
    if (r.u32(nt) != 0x4550) bad("PE signature");
    const std::uint64_t sections = r.u16(nt + 6), optionalSize = r.u16(nt + 20), optional = nt + 24;
    const std::uint32_t magic = r.u16(optional);
    if (magic != 0x10b && magic != 0x20b) bad("optional header");
    const std::uint64_t directories = optional + (magic == 0x10b ? 96 : 112);
    if (r.u32(directories - 4) < 3) return {};
    if (directories + 24 > optional + optionalSize) bad("data directories");
    const std::uint32_t resourceRva = r.u32(directories + 16), resourceSize = r.u32(directories + 20);
    if (resourceRva == 0) return {};
    const std::uint64_t table = optional + optionalSize;
    r.need(table, sections * 40);
    // File offset of [rva, rva+size) when wholly inside one section's raw data.
    const auto offset = [&](std::uint64_t rva, std::uint64_t size) -> std::uint64_t {
        for (std::uint64_t s = table; s < table + sections * 40; s += 40) {
            const std::uint64_t va = r.u32(s + 12), rawSize = r.u32(s + 16), raw = r.u32(s + 20);
            if (rva < va || rva - va >= rawSize) continue;
            if (size > rawSize - (rva - va)) bad("resource crosses section");
            r.need(raw + rva - va, size);
            return raw + rva - va;
        }
        bad("resource outside sections");
    };
    const std::uint64_t base = offset(resourceRva, resourceSize);
    // Calls each(name, target) for every entry of the directory at section offset dir.
    const auto entries = [&](std::uint64_t dir, auto each) {
        if (dir + 16 > resourceSize) bad("resource directory");
        const std::uint64_t count = r.u16(base + dir + 12) + r.u16(base + dir + 14);
        if (dir + 16 + count * 8 > resourceSize) bad("resource entries");
        for (std::uint64_t e = dir + 16; e < dir + 16 + count * 8; e += 8) each(r.u32(base + e), r.u32(base + e + 4));
    };
    constexpr std::uint32_t subdirectory = 0x80000000u;
    std::map<std::uint32_t, QByteArray> result;
    entries(0, [&](std::uint32_t type, std::uint32_t types) {
        if (type != 2 || !(types & subdirectory)) return; // RT_BITMAP only
        entries(types & ~subdirectory, [&](std::uint32_t id, std::uint32_t languages) {
            if ((id & subdirectory) || !(languages & subdirectory) || result.count(id)) return;
            bool found = false;
            entries(languages & ~subdirectory, [&](std::uint32_t, std::uint32_t data) {
                if (found || (data & subdirectory)) return;
                if (static_cast<std::uint64_t>(data) + 16 > resourceSize) bad("resource data entry");
                const std::uint32_t rva = r.u32(base + data), size = r.u32(base + data + 4);
                const auto at = offset(rva, size);
                result.emplace(id, bmpFile(pe.mid(static_cast<qsizetype>(at), static_cast<qsizetype>(size))));
                found = true;
            });
        });
    });
    return result;
}

QString originalAppDirectory(const QString& source) {
    if (source.isEmpty()) return {};
    const QDir dir(source);
    QStringList candidates{dir.absolutePath(), dir.absoluteFilePath("app")};
    if (dir.dirName() == "base") candidates << QFileInfo(dir.absolutePath()).absolutePath();
    for (const auto& candidate : candidates)
        if (QFileInfo(candidate + "/launcher.exe").isFile()) return candidate;
    return {};
}
} // namespace ikv::launcher_skin
