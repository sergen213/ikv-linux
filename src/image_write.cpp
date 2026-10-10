#include "ikv/renderer.hpp"

#include <turbojpeg.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

namespace ikv {
namespace {
void put16(std::string& out, unsigned value) { out += char(value & 0xff); out += char(value >> 8 & 0xff); }
void put32(std::string& out, std::uint32_t value) { put16(out, value & 0xffff); put16(out, value >> 16); }
}

// Original screenshot 00468010 saves the back buffer with D3DXSaveSurfaceToFile as JPG, TGA (Ctrl) or BMP (Shift).
void write_rgb_image(const std::filesystem::path& path, int width, int height, std::span<const std::uint8_t> rgb) {
    const auto row = static_cast<std::size_t>(width) * 3;
    if (width <= 0 || height <= 0 || rgb.size() != row * static_cast<std::size_t>(height)) throw std::invalid_argument("screenshot pixel buffer invalid");
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    std::string out;
    if (extension == ".jpg" || extension == ".jpeg") {
        const std::unique_ptr<void, decltype(&tj3Destroy)> encoder(tj3Init(TJINIT_COMPRESS), &tj3Destroy);
        unsigned char* jpeg = nullptr; std::size_t size = 0;
        if (!encoder || tj3Set(encoder.get(), TJPARAM_QUALITY, 90) || tj3Set(encoder.get(), TJPARAM_SUBSAMP, TJSAMP_420) ||
            tj3Compress8(encoder.get(), rgb.data(), width, 0, height, TJPF_RGB, &jpeg, &size)) { tj3Free(jpeg); throw std::runtime_error("screenshot JPEG encode failed"); }
        out.assign(reinterpret_cast<const char*>(jpeg), size); tj3Free(jpeg);
    } else if (extension == ".tga" || extension == ".bmp") {
        const bool tga = extension == ".tga";
        const auto stride = tga ? row : (row + 3) & ~std::size_t{3};
        if (tga) { // uncompressed true-colour, bottom-up BGR
            out.append("\0\0\2\0\0\0\0\0\0\0\0\0", 12); put16(out, unsigned(width)); put16(out, unsigned(height)); out += char(24); out += char(0);
        } else { // BITMAPFILEHEADER + BITMAPINFOHEADER, bottom-up BGR rows padded to 4 bytes
            out += "BM"; put32(out, std::uint32_t(54 + stride * std::size_t(height))); put32(out, 0); put32(out, 54);
            put32(out, 40); put32(out, std::uint32_t(width)); put32(out, std::uint32_t(height)); put16(out, 1); put16(out, 24);
            put32(out, 0); put32(out, std::uint32_t(stride * std::size_t(height))); put32(out, 2835); put32(out, 2835); put32(out, 0); put32(out, 0);
        }
        out.reserve(out.size() + stride * std::size_t(height));
        for (int y = height - 1; y >= 0; --y) {
            const auto* p = rgb.data() + std::size_t(y) * row;
            for (int x = 0; x < width; ++x, p += 3) { out += char(p[2]); out += char(p[1]); out += char(p[0]); }
            out.append(stride - row, '\0');
        }
    } else {
        out = "P6\n" + std::to_string(width) + ' ' + std::to_string(height) + "\n255\n";
        out.append(reinterpret_cast<const char*>(rgb.data()), rgb.size());
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) throw std::runtime_error("cannot open screenshot output");
    file.write(out.data(), static_cast<std::streamsize>(out.size()));
    file.close();
    if (!file) throw std::runtime_error("screenshot write failed");
}
} // namespace ikv
