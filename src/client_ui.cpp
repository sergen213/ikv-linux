#include "ikv/client_ui.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H
#include <fontconfig/fontconfig.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace ikv {
namespace {
constexpr int max_extent = 8192;

std::uint32_t next_codepoint(std::string_view input, std::size_t& position) {
    const auto lead = static_cast<unsigned char>(input[position++]);
    if (lead < 0x80) return lead;
    unsigned remaining;
    std::uint32_t value, minimum;
    if (lead >= 0xc2 && lead <= 0xdf) {
        remaining = 1; value = lead & 0x1f; minimum = 0x80;
    } else if (lead >= 0xe0 && lead <= 0xef) {
        remaining = 2; value = lead & 0x0f; minimum = 0x800;
    } else if (lead >= 0xf0 && lead <= 0xf4) {
        remaining = 3; value = lead & 0x07; minimum = 0x10000;
    } else {
        return 0xfffd;
    }
    for (unsigned i = 0; i < remaining; ++i) {
        if (position == input.size()) return 0xfffd;
        const auto continuation = static_cast<unsigned char>(input[position]);
        if ((continuation & 0xc0) != 0x80) return 0xfffd;
        ++position;
        value = (value << 6) | (continuation & 0x3f);
    }
    if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return 0xfffd;
    return value;
}

void blend(std::uint8_t* target, const std::array<std::uint8_t, 4>& color, unsigned coverage = 255) {
    const unsigned source_alpha = (color[3] * coverage + 127) / 255;
    if (!source_alpha) return;
    if (source_alpha == 255) {
        std::copy(color.begin(), color.end(), target);
        return;
    }
    // Exact shortcut of the general formula below over a fully transparent target: (c*a*255 + a*255/2) / (a*255) == c.
    if (!target[3]) {
        std::copy_n(color.begin(), 3, target);
        target[3] = static_cast<std::uint8_t>(source_alpha);
        return;
    }
    // Overlay stores straight alpha, including when glyphs overlap translucent panels.
    const unsigned retained_alpha = target[3] * (255 - source_alpha);
    const unsigned alpha = source_alpha * 255 + retained_alpha;
    for (unsigned channel = 0; channel < 3; ++channel)
        target[channel] = static_cast<std::uint8_t>(
            (color[channel] * source_alpha * 255 + target[channel] * retained_alpha + alpha / 2) / alpha);
    target[3] = static_cast<std::uint8_t>((alpha + 127) / 255);
}
} // namespace

struct ClientUI::Impl {
    struct Glyph {
        FT_Face face = nullptr;
        FT_UInt index = 0;
        int width = 0, height = 0, left = 0, top = 0, advance = 0;
        std::vector<std::uint8_t> alpha;
    };
    FT_Library library = nullptr;
    FT_Face face = nullptr, original_fallback = nullptr, native_notdef = nullptr, bold = nullptr; // bold: sibling verdanab.ttf
    bool bold_active = false;
    FcConfig* font_config = nullptr;
    FcFontSet* native_fonts = nullptr;
    std::vector<FT_Face> native_faces;
    std::vector<bool> native_attempted;
    int pixel_size = 0;
    bool missing_reported = false;
    int ascender = 0, line_height = 0;
    Overlay overlay;
    std::unordered_map<std::uint64_t, Glyph> glyphs; // key: pixel size << 33 | bold << 32 | codepoint
    // Sampled colours of recent image() calls, keyed by image storage/size and every draw parameter. HUD repaints redraw
    // the same sprites each frame; a hit skips the bilinear sampling and only blends. begin() drops entries the previous
    // paint did not use.
    using SpriteKey = std::array<std::uint32_t, 21>;
    struct SpriteHash {
        std::size_t operator()(const SpriteKey& key) const noexcept {
            std::uint64_t h = 1469598103934665603ull;
            for (const auto word : key) h = (h ^ word) * 1099511628211ull;
            return static_cast<std::size_t>(h);
        }
    };
    struct Sprite { std::vector<std::array<std::uint8_t, 4>> colors; bool used = true; };
    std::unordered_map<SpriteKey, Sprite, SpriteHash> sprites;

    ~Impl() {
        if (face) FT_Done_Face(face);
        if (original_fallback) FT_Done_Face(original_fallback);
        if (bold) FT_Done_Face(bold);
        for (auto native : native_faces) if (native) FT_Done_Face(native);
        if (native_fonts) FcFontSetDestroy(native_fonts);
        if (font_config) FcConfigDestroy(font_config);
        if (library) FT_Done_FreeType(library);
    }

    FT_Face load_face(const std::filesystem::path& path, int index = 0) {
        FT_Face loaded = nullptr;
        if (FT_New_Face(library, path.string().c_str(), index, &loaded)) return nullptr;
        if (!FT_IS_SCALABLE(loaded) || FT_Select_Charmap(loaded, FT_ENCODING_UNICODE) ||
            FT_Set_Pixel_Sizes(loaded, 0, static_cast<FT_UInt>(pixel_size))) {
            FT_Done_Face(loaded);
            return nullptr;
        }
        return loaded;
    }

    FT_Face native_face(int index) {
        if (!native_attempted[index]) {
            native_attempted[index] = true;
            FcChar8* path = nullptr;
            int face_index = 0;
            if (FcPatternGetString(native_fonts->fonts[index], FC_FILE, 0, &path) == FcResultMatch) {
                FcPatternGetInteger(native_fonts->fonts[index], FC_INDEX, 0, &face_index);
                native_faces[index] = load_face(reinterpret_cast<const char*>(path), face_index);
            }
        }
        return native_faces[index];
    }

    void select_glyph(std::uint32_t codepoint, Glyph& result) {
        for (auto original : {bold_active ? bold : nullptr, face, original_fallback}) {
            if (original && (result.index = FT_Get_Char_Index(original, codepoint))) {
                result.face = original;
                return;
            }
        }
        for (int i = 0; i < native_fonts->nfont; ++i) {
            FcCharSet* charset = nullptr;
            if (FcPatternGetCharSet(native_fonts->fonts[i], FC_CHARSET, 0, &charset) != FcResultMatch ||
                !FcCharSetHasChar(charset, codepoint)) continue;
            auto native = native_face(i);
            if (native && (result.index = FT_Get_Char_Index(native, codepoint))) {
                result.face = native;
                return;
            }
        }
        result.face = native_notdef;
        result.index = 0; // The native font's real .notdef, never a synthetic replacement.
        if (!missing_reported) {
            missing_reported = true;
            std::fputs("UI font coverage exhausted; using native .notdef (text omitted).\n", stderr);
        }
    }

    const Glyph& glyph(std::uint32_t codepoint) {
        const auto key = static_cast<std::uint64_t>(pixel_size) << 33 | static_cast<std::uint64_t>(bold_active) << 32 | codepoint;
        if (const auto found = glyphs.find(key); found != glyphs.end()) return found->second;
        Glyph result;
        select_glyph(codepoint, result);
        if (FT_Load_Glyph(result.face, result.index, FT_LOAD_DEFAULT | FT_LOAD_NO_BITMAP) ||
            FT_Render_Glyph(result.face->glyph, FT_RENDER_MODE_NORMAL))
            throw std::runtime_error("Cannot rasterize UI font glyph");
        const auto& bitmap = result.face->glyph->bitmap;
        if (bitmap.width > max_extent || bitmap.rows > max_extent ||
            (bitmap.width && bitmap.rows && bitmap.pixel_mode != FT_PIXEL_MODE_GRAY))
            throw std::runtime_error("Unsupported UI font glyph bitmap");
        result.width = static_cast<int>(bitmap.width);
        result.height = static_cast<int>(bitmap.rows);
        result.left = result.face->glyph->bitmap_left;
        result.top = result.face->glyph->bitmap_top;
        result.advance = static_cast<int>((result.face->glyph->advance.x + 32) / 64);
        result.alpha.resize(static_cast<std::size_t>(result.width) * result.height);
        for (int row = 0; row < result.height && result.width > 0; ++row) {
            const auto* source = bitmap.buffer + static_cast<std::ptrdiff_t>(row) * bitmap.pitch;
            auto* destination = result.alpha.data() + static_cast<std::size_t>(row) * result.width;
            if (bitmap.num_grays == 256) {
                std::copy_n(source, result.width, destination);
            } else {
                if (bitmap.num_grays < 2) throw std::runtime_error("Invalid UI font grayscale bitmap");
                for (int column = 0; column < result.width; ++column)
                    destination[column] = static_cast<std::uint8_t>(source[column] * 255u / (bitmap.num_grays - 1));
            }
        }
        return glyphs.emplace(key, std::move(result)).first->second;
    }
    void metrics() {
        ascender = static_cast<int>((face->size->metrics.ascender + 63) / 64);
        line_height = std::max(1, static_cast<int>((face->size->metrics.height + 63) / 64));
    }

    int kerning(const Glyph* previous, const Glyph& current) const {
        if (!previous || previous->face != current.face || !previous->index ||
            !current.index || !FT_HAS_KERNING(current.face)) return 0;
        FT_Vector delta{};
        if (FT_Get_Kerning(current.face, previous->index, current.index, FT_KERNING_DEFAULT, &delta))
            throw std::runtime_error("Cannot read UI font kerning");
        return static_cast<int>(delta.x / 64);
    }
};

ClientUI::ClientUI(const std::filesystem::path& font, int pixelSize) : impl_(std::make_unique<Impl>()) {
    if (pixelSize <= 0 || pixelSize > max_extent) throw std::invalid_argument("UI font pixel size outside 1..8192");
    if (font.empty() || !std::filesystem::is_regular_file(font))
        throw std::runtime_error("Original UI font unavailable: " + font.string());
    if (FT_Init_FreeType(&impl_->library)) throw std::runtime_error("Cannot initialize FreeType");
    impl_->pixel_size = pixelSize;
    impl_->face = impl_->load_face(font);
    if (!impl_->face) throw std::runtime_error("Cannot load original Unicode outline UI font: " + font.string());
    const auto fallback = font.parent_path() / "verdana.ttf";
    if (fallback != font && std::filesystem::is_regular_file(fallback)) {
        impl_->original_fallback = impl_->load_face(fallback);
        if (!impl_->original_fallback)
            throw std::runtime_error("Original Unicode UI fallback unavailable: " + fallback.string());
    }
    if (const auto bold = font.parent_path() / "verdanab.ttf"; std::filesystem::is_regular_file(bold)) impl_->bold = impl_->load_face(bold);
    impl_->font_config = FcInitLoadConfigAndFonts();
    if (!impl_->font_config) throw std::runtime_error("Cannot initialize native Fontconfig fallback");
    const std::unique_ptr<FcPattern, decltype(&FcPatternDestroy)> pattern(FcPatternCreate(), FcPatternDestroy);
    if (!pattern || !FcPatternAddString(pattern.get(), FC_FAMILY, reinterpret_cast<const FcChar8*>("sans-serif")) ||
        !FcPatternAddBool(pattern.get(), FC_SCALABLE, FcTrue) ||
        !FcConfigSubstitute(impl_->font_config, pattern.get(), FcMatchPattern))
        throw std::runtime_error("Cannot configure native UI font fallback");
    FcDefaultSubstitute(pattern.get());
    FcResult match_result{};
    impl_->native_fonts = FcFontSort(impl_->font_config, pattern.get(), FcFalse, nullptr, &match_result);
    if (!impl_->native_fonts) throw std::runtime_error("Cannot discover native UI font fallback");
    impl_->native_faces.resize(impl_->native_fonts->nfont, nullptr);
    impl_->native_attempted.resize(impl_->native_fonts->nfont, false);
    for (int i = 0; i < impl_->native_fonts->nfont; ++i) {
        const auto native = impl_->native_face(i);
        if (native && !FT_Load_Glyph(native, 0, FT_LOAD_DEFAULT | FT_LOAD_NO_BITMAP) &&
            !FT_Render_Glyph(native->glyph, FT_RENDER_MODE_NORMAL) &&
            native->glyph->bitmap.width > 0 && native->glyph->bitmap.rows > 0 &&
            native->glyph->bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
            impl_->native_notdef = native;
            break;
        }
    }
    if (!impl_->native_notdef) throw std::runtime_error("No native Unicode outline font with a drawable .notdef");
    impl_->metrics();
}

void ClientUI::set_pixel_size(int pixelSize) {
    if (pixelSize == impl_->pixel_size) return;
    if (pixelSize <= 0 || pixelSize > max_extent) throw std::invalid_argument("UI font pixel size outside 1..8192");
    impl_->pixel_size = pixelSize;
    std::vector<FT_Face> loaded = impl_->native_faces;
    loaded.push_back(impl_->face); loaded.push_back(impl_->original_fallback); loaded.push_back(impl_->bold);
    for (const auto face : loaded)
        if (face && FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(pixelSize))) throw std::runtime_error("Cannot resize UI font");
    impl_->metrics();
}
int ClientUI::pixel_size() const { return impl_->pixel_size; }
void ClientUI::set_bold(bool bold) { impl_->bold_active = bold; }

ClientUI::~ClientUI() = default;

void ClientUI::begin(int width, int height) {
    if (width < 0 || height < 0 || width > max_extent || height > max_extent)
        throw std::invalid_argument("UI overlay dimensions outside 0..8192");
    auto& output = impl_->overlay;
    const auto bytes = static_cast<std::size_t>(width) * height * 4;
    const auto retained = std::min(output.rgba.size(), bytes);
    output.rgba.resize(bytes);
    std::fill_n(output.rgba.begin(), retained, 0);
    output.width = width;
    output.height = height;
    std::erase_if(impl_->sprites, [](const auto& entry) { return !entry.second.used; });
    for (auto& entry : impl_->sprites) entry.second.used = false;
}

void ClientUI::rect(int x, int y, int w, int h, std::array<std::uint8_t, 4> color) {
    if (w <= 0 || h <= 0 || !color[3]) return;
    auto& output = impl_->overlay;
    const auto left = std::max<std::int64_t>(0, x), top = std::max<std::int64_t>(0, y);
    const auto right = std::min<std::int64_t>(output.width, static_cast<std::int64_t>(x) + w);
    const auto bottom = std::min<std::int64_t>(output.height, static_cast<std::int64_t>(y) + h);
    for (auto row = top; row < bottom; ++row)
        for (auto column = left; column < right; ++column)
            blend(output.rgba.data() + (static_cast<std::size_t>(row) * output.width + column) * 4, color);
}

void ClientUI::image(const assets::DdsImage& image, std::array<float, 4> destination,
                     const std::array<std::array<float, 2>, 4>& uv,
                     std::array<std::uint8_t, 4> tint) {
    // Validate borrowed immutable storage once at the draw boundary, not inside the pixel loop.
    if (image.mips.empty()) throw std::invalid_argument("UI image has no base mip");
    const auto& mip = image.mips.front();
    const auto pixels = static_cast<std::uint64_t>(mip.width) * mip.height;
    if (!mip.width || !mip.height || mip.width != image.width || mip.height != image.height ||
        pixels > assets::max_entry_bytes / 4 || mip.size != pixels * 4 ||
        mip.offset > image.rgba.size() || mip.size > image.rgba.size() - mip.offset)
        throw std::invalid_argument("UI image has invalid base mip storage");
    for (const auto value : destination)
        if (!std::isfinite(value)) throw std::invalid_argument("UI image destination is nonfinite");
    for (const auto& corner : uv)
        for (const auto value : corner)
            if (!std::isfinite(value)) throw std::invalid_argument("UI image UV is nonfinite");
    if (destination[2] <= 0 || destination[3] <= 0 || !tint[3]) return;

    auto& output = impl_->overlay;
    const double x = destination[0], y = destination[1];
    const double width = destination[2], height = destination[3];
    // Pixel-center coverage; clamp before converting even enormous finite float coordinates.
    const auto edge = [](double value, int extent) {
        return static_cast<int>(std::clamp(std::ceil(value - 0.5), 0.0, static_cast<double>(extent)));
    };
    const int left = edge(x, output.width), top = edge(y, output.height);
    const int right = edge(x + width, output.width), bottom = edge(y + height, output.height);
    if (left >= right || top >= bottom) return;
    const auto* source = image.rgba.data() + mip.offset;
    Impl::SpriteKey key{};
    const auto address = reinterpret_cast<std::uintptr_t>(source);
    key[0] = static_cast<std::uint32_t>(address); key[1] = static_cast<std::uint32_t>(static_cast<std::uint64_t>(address) >> 32);
    key[2] = mip.width; key[3] = mip.height;
    key[4] = static_cast<std::uint32_t>(tint[0] | tint[1] << 8 | tint[2] << 16) | static_cast<std::uint32_t>(tint[3]) << 24;
    for (unsigned i = 0; i < 4; ++i) key[5 + i] = std::bit_cast<std::uint32_t>(destination[i]);
    for (unsigned i = 0; i < 8; ++i) key[9 + i] = std::bit_cast<std::uint32_t>(uv[i / 2][i % 2]);
    key[17] = static_cast<std::uint32_t>(left); key[18] = static_cast<std::uint32_t>(top);
    key[19] = static_cast<std::uint32_t>(right); key[20] = static_cast<std::uint32_t>(bottom);
    auto [found, inserted] = impl_->sprites.try_emplace(key);
    auto& sprite = found->second;
    sprite.used = true;
    const auto span = static_cast<std::size_t>(right - left);
    if (inserted) {
        sprite.colors.resize(span * static_cast<std::size_t>(bottom - top));
        const double inverse_width = 1 / width, inverse_height = 1 / height;
        auto* sampled = sprite.colors.data();
        for (int row = top; row < bottom; ++row) {
            const double sy = std::clamp((row + 0.5 - y) * inverse_height, 0.0, 1.0);
            for (int column = left; column < right; ++column) {
                const double sx = std::clamp((column + 0.5 - x) * inverse_width, 0.0, 1.0);
                std::array<double, 2> coordinate{};
                // Original 0048cea0: (UL,UR,LR), (UL,LR,LL), not a bilinear UV quad.
                for (unsigned axis = 0; axis < 2; ++axis)
                    coordinate[axis] = sx >= sy
                        ? (1 - sx) * uv[0][axis] + (sx - sy) * uv[1][axis] + sy * uv[3][axis]
                        : (1 - sy) * uv[0][axis] + sx * uv[3][axis] + (sy - sx) * uv[2][axis];
                const double tx = std::clamp(std::clamp(coordinate[0], 0.0, 1.0) * mip.width - 0.5,
                                             0.0, static_cast<double>(mip.width - 1));
                const double ty = std::clamp(std::clamp(coordinate[1], 0.0, 1.0) * mip.height - 0.5,
                                             0.0, static_cast<double>(mip.height - 1));
                const auto x0 = static_cast<std::uint32_t>(tx), y0 = static_cast<std::uint32_t>(ty);
                const auto x1 = std::min(x0 + 1, mip.width - 1), y1 = std::min(y0 + 1, mip.height - 1);
                const double fx = tx - x0, fy = ty - y0;
                const auto* ul = source + (static_cast<std::size_t>(y0) * mip.width + x0) * 4;
                const auto* ur = source + (static_cast<std::size_t>(y0) * mip.width + x1) * 4;
                const auto* ll = source + (static_cast<std::size_t>(y1) * mip.width + x0) * 4;
                const auto* lr = source + (static_cast<std::size_t>(y1) * mip.width + x1) * 4;
                auto& color = *sampled++;
                for (unsigned channel = 0; channel < 4; ++channel) {
                    const double sample = (ul[channel] * (1 - fx) + ur[channel] * fx) * (1 - fy) +
                                          (ll[channel] * (1 - fx) + lr[channel] * fx) * fy;
                    color[channel] = static_cast<std::uint8_t>(
                        std::clamp(sample * tint[channel] / 255 + 0.5, 0.0, 255.0));
                }
            }
        }
    }
    const auto* sampled = sprite.colors.data();
    for (int row = top; row < bottom; ++row) {
        auto* target = output.rgba.data() + (static_cast<std::size_t>(row) * output.width + left) * 4;
        for (std::size_t column = 0; column < span; ++column, target += 4) blend(target, *sampled++);
    }
}

void ClientUI::text(int x, int y, std::string_view utf8, std::array<std::uint8_t, 4> color) {
    auto& output = impl_->overlay;
    std::int64_t pen = x, baseline = static_cast<std::int64_t>(y) + impl_->ascender;
    const Impl::Glyph* previous = nullptr;
    for (std::size_t position = 0; position < utf8.size();) {
        const auto codepoint = next_codepoint(utf8, position);
        if (codepoint == '\n') {
            pen = x;
            baseline += impl_->line_height;
            previous = nullptr;
            continue;
        }
        const auto& glyph = impl_->glyph(codepoint);
        pen += impl_->kerning(previous, glyph);
        const auto left = pen + glyph.left, top = baseline - glyph.top;
        const auto first_x = std::max<std::int64_t>(0, -left), first_y = std::max<std::int64_t>(0, -top);
        const auto last_x = std::min<std::int64_t>(glyph.width, output.width - left);
        const auto last_y = std::min<std::int64_t>(glyph.height, output.height - top);
        for (auto row = first_y; row < last_y; ++row)
            for (auto column = first_x; column < last_x; ++column)
                blend(output.rgba.data() + (static_cast<std::size_t>(top + row) * output.width + left + column) * 4,
                      color, glyph.alpha[static_cast<std::size_t>(row) * glyph.width + column]);
        pen += glyph.advance;
        previous = &glyph;
    }
}

int ClientUI::text_width(std::string_view utf8) {
    std::int64_t width = 0, maximum = 0;
    const Impl::Glyph* previous = nullptr;
    for (std::size_t position = 0; position < utf8.size();) {
        const auto codepoint = next_codepoint(utf8, position);
        if (codepoint == '\n') {
            maximum = std::max(maximum, width);
            width = 0;
            previous = nullptr;
            continue;
        }
        const auto& glyph = impl_->glyph(codepoint);
        width += impl_->kerning(previous, glyph) + glyph.advance;
        previous = &glyph;
    }
    return static_cast<int>(std::clamp<std::int64_t>(std::max(maximum, width), 0, std::numeric_limits<int>::max()));
}

int ClientUI::line_height() const { return impl_->line_height; }

const Overlay& ClientUI::overlay() const { return impl_->overlay; }
} // namespace ikv
