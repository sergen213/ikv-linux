#include "ikv/client_ui.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H
#ifndef _WIN32
#include <fontconfig/fontconfig.h>
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
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
#ifdef _WIN32
    std::vector<std::filesystem::path> native_files; // NONCANONICAL: %WINDIR%\Fonts list in place of Fontconfig
#else
    FcConfig* font_config = nullptr;
    FcFontSet* native_fonts = nullptr;
#endif
    std::vector<FT_Face> native_faces;
    std::vector<bool> native_attempted;
    int pixel_size = 0;
    bool missing_reported = false;
    int ascender = 0, line_height = 0;
    Overlay overlay;
    // World layer (see begin_world): transparent outside world_ink. world_shown are the boxes composited into overlay and
    // under the HUD pixels they cover there. Boxes are left, top, right, bottom.
    using Box = std::array<int, 4>;
    Overlay world;
    bool world_active = false, fresh = true; // fresh: overlay is the HUD alone (after begin)
    std::vector<Box> world_ink, world_shown;
    std::vector<std::vector<std::uint8_t>> under;
    std::vector<std::array<int, 4>> changed;
    Overlay& target() { return world_active ? world : overlay; }
    void ink(std::int64_t left, std::int64_t top, std::int64_t right, std::int64_t bottom) {
        if (!world_active || left >= right || top >= bottom) return;
        const Box box{int(left), int(top), int(right), int(bottom)};
        if (!world_ink.empty() && overlap(world_ink.back(), box)) unite(world_ink.back(), box); // a label's shadow and text
        else world_ink.push_back(box);
    }
    static bool overlap(const Box& a, const Box& b) { return a[0] < b[2] && b[0] < a[2] && a[1] < b[3] && b[1] < a[3]; }
    static void unite(Box& a, const Box& b) { a = {std::min(a[0], b[0]), std::min(a[1], b[1]), std::max(a[2], b[2]), std::max(a[3], b[3])}; }
    // ponytail: quadratic merge until no two boxes overlap; fine for the few dozen name tags a view holds.
    static void merge(std::vector<Box>& boxes) {
        for (bool merged = true; merged;) {
            merged = false;
            for (std::size_t i = 0; i < boxes.size(); ++i)
                for (std::size_t j = i + 1; j < boxes.size();)
                    if (overlap(boxes[i], boxes[j])) { unite(boxes[i], boxes[j]); boxes.erase(boxes.begin() + std::ptrdiff_t(j)); merged = true; }
                    else ++j;
        }
    }
    void clear_world_ink() {
        for (const auto& box : world_ink)
            for (int row = box[1]; row < box[3]; ++row)
                std::fill_n(world.rgba.begin() + (std::ptrdiff_t(row) * world.width + box[0]) * 4, std::size_t(box[2] - box[0]) * 4, 0);
        world_ink.clear();
    }
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
#ifndef _WIN32
        if (native_fonts) FcFontSetDestroy(native_fonts);
        if (font_config) FcConfigDestroy(font_config);
#endif
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
#ifdef _WIN32
            native_faces[index] = load_face(native_files[index]);
#else
            FcChar8* path = nullptr;
            int face_index = 0;
            if (FcPatternGetString(native_fonts->fonts[index], FC_FILE, 0, &path) == FcResultMatch) {
                FcPatternGetInteger(native_fonts->fonts[index], FC_INDEX, 0, &face_index);
                native_faces[index] = load_face(reinterpret_cast<const char*>(path), face_index);
            }
#endif
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
        for (int i = 0; i < static_cast<int>(native_faces.size()); ++i) {
#ifndef _WIN32
            FcCharSet* charset = nullptr;
            if (FcPatternGetCharSet(native_fonts->fonts[i], FC_CHARSET, 0, &charset) != FcResultMatch ||
                !FcCharSetHasChar(charset, codepoint)) continue;
#endif
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
#ifdef _WIN32
    // Common Unicode sans/symbol/CJK faces stand in for Fontconfig's sorted sans-serif set; the original font comes last
    // so a drawable .notdef exists even in a bare (Wine) Fonts directory.
    const wchar_t* windows = _wgetenv(L"WINDIR");
    const auto fonts = std::filesystem::path(windows && *windows ? windows : L"C:\\Windows") / "Fonts";
    for (const char* name : {"segoeui.ttf", "arial.ttf", "tahoma.ttf", "seguisym.ttf", "msyh.ttc", "malgun.ttf", "msgothic.ttc", "nirmala.ttf"})
        if (std::filesystem::is_regular_file(fonts / name)) impl_->native_files.push_back(fonts / name);
    impl_->native_files.push_back(font);
    const auto native_count = impl_->native_files.size();
#else
    impl_->font_config = FcInitLoadConfigAndFonts();
    if (!impl_->font_config) throw std::runtime_error("Cannot initialize native Fontconfig fallback");
    const std::unique_ptr<FcPattern, decltype(&FcPatternDestroy)> pattern(FcPatternCreate(), FcPatternDestroy);
    if (!pattern || !FcPatternAddString(pattern.get(), FC_FAMILY, reinterpret_cast<const FcChar8*>("sans-serif")) ||
        !FcPatternAddBool(pattern.get(), FC_SCALABLE, FcTrue) ||
        !FcConfigSubstitute(impl_->font_config, pattern.get(), FcMatchPattern))
        throw std::runtime_error("Cannot configure native UI font fallback");
#if FC_VERSION >= 21700  // FcDefaultSubstitute would load (and keep) a second, global current config since 2.17
    FcConfigSetDefaultSubstitute(impl_->font_config, pattern.get());
#else
    FcDefaultSubstitute(pattern.get());
#endif
    FcResult match_result{};
    impl_->native_fonts = FcFontSort(impl_->font_config, pattern.get(), FcFalse, nullptr, &match_result);
    if (!impl_->native_fonts) throw std::runtime_error("Cannot discover native UI font fallback");
    const auto native_count = static_cast<std::size_t>(impl_->native_fonts->nfont);
#endif
    impl_->native_faces.resize(native_count, nullptr);
    impl_->native_attempted.resize(native_count, false);
    for (int i = 0; i < static_cast<int>(native_count); ++i) {
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
    auto& world = impl_->world;
    if (world.width != width || world.height != height) {
        world.width = width; world.height = height; world.rgba.assign(bytes, 0); impl_->world_ink.clear();
    } else impl_->clear_world_ink();
    impl_->world_active = false; impl_->fresh = true; impl_->world_shown.clear(); impl_->under.clear();
}

void ClientUI::rect(int x, int y, int w, int h, std::array<std::uint8_t, 4> color) {
    if (w <= 0 || h <= 0 || !color[3]) return;
    auto& output = impl_->target();
    const auto left = std::max<std::int64_t>(0, x), top = std::max<std::int64_t>(0, y);
    const auto right = std::min<std::int64_t>(output.width, static_cast<std::int64_t>(x) + w);
    const auto bottom = std::min<std::int64_t>(output.height, static_cast<std::int64_t>(y) + h);
    for (auto row = top; row < bottom; ++row)
        for (auto column = left; column < right; ++column)
            blend(output.rgba.data() + (static_cast<std::size_t>(row) * output.width + column) * 4, color);
    impl_->ink(left, top, right, bottom);
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

    auto& output = impl_->target();
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
        if (uv[0][1] == uv[1][1] && uv[2][1] == uv[3][1] && uv[0][0] == uv[2][0] && uv[1][0] == uv[3][0]) {
            // An axis-aligned UV rectangle maps x and y independently: both source triangles reduce to the same affine map,
            // so the texel taps are per column and per row and each source row is filtered horizontally once. The
            // bilinear terms keep the general path's order; only the coordinate rounding of the triangle form can differ.
            // (Large first paints, e.g. a 9-sliced Yardım window, took ~38 ms through the per-pixel path.)
            struct Tap { std::uint32_t first, second; double weight; };
            const auto tap = [](double s, double a, double b, std::uint32_t extent) {
                const double t = std::clamp(std::clamp((1 - s) * a + s * b, 0.0, 1.0) * extent - 0.5, 0.0, static_cast<double>(extent - 1));
                const auto first = static_cast<std::uint32_t>(t);
                return Tap{first, std::min(first + 1, extent - 1), t - first};
            };
            std::vector<Tap> columns(span);
            for (std::size_t i = 0; i < span; ++i)
                columns[i] = tap(std::clamp((left + static_cast<int>(i) + 0.5 - x) * inverse_width, 0.0, 1.0), uv[0][0], uv[1][0], mip.width);
            std::vector<double> upper(span * 4), lower(span * 4);
            std::uint32_t upper_row = UINT32_MAX, lower_row = UINT32_MAX;
            const auto filter = [&](std::uint32_t row, std::vector<double>& out) {
                const auto* line = source + static_cast<std::size_t>(row) * mip.width * 4;
                for (std::size_t i = 0; i < span; ++i)
                    for (unsigned channel = 0; channel < 4; ++channel)
                        out[i * 4 + channel] = line[columns[i].first * 4 + channel] * (1 - columns[i].weight) +
                                               line[columns[i].second * 4 + channel] * columns[i].weight;
            };
            for (int row = top; row < bottom; ++row) {
                const auto v = tap(std::clamp((row + 0.5 - y) * inverse_height, 0.0, 1.0), uv[0][1], uv[2][1], mip.height);
                if (v.first != upper_row) {
                    if (v.first == lower_row) { std::swap(upper, lower); std::swap(upper_row, lower_row); }
                    else { filter(v.first, upper); upper_row = v.first; }
                }
                if (v.second != lower_row) { filter(v.second, lower); lower_row = v.second; }
                for (std::size_t i = 0; i < span; ++i) {
                    auto& color = *sampled++;
                    for (unsigned channel = 0; channel < 4; ++channel)
                        color[channel] = static_cast<std::uint8_t>(std::clamp(
                            (upper[i * 4 + channel] * (1 - v.weight) + lower[i * 4 + channel] * v.weight) * tint[channel] / 255 + 0.5, 0.0, 255.0));
                }
            }
        } else
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
    impl_->ink(left, top, right, bottom);
}

void ClientUI::text(int x, int y, std::string_view utf8, std::array<std::uint8_t, 4> color) {
    auto& output = impl_->target();
    std::int64_t ink_left = INT64_MAX, ink_top = INT64_MAX, ink_right = INT64_MIN, ink_bottom = INT64_MIN;
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
        if (first_x < last_x && first_y < last_y) {
            ink_left = std::min(ink_left, left + first_x); ink_top = std::min(ink_top, top + first_y);
            ink_right = std::max(ink_right, left + last_x); ink_bottom = std::max(ink_bottom, top + last_y);
        }
        pen += glyph.advance;
        previous = &glyph;
    }
    impl_->ink(ink_left, ink_top, ink_right, ink_bottom);
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

void ClientUI::begin_world() { impl_->clear_world_ink(); impl_->world_active = true; }
void ClientUI::end_world() { impl_->world_active = false; }

std::span<const std::array<int, 4>> ClientUI::compose() {
    auto& m = *impl_;
    auto& output = m.overlay;
    std::vector<Impl::Box> boxes;
    if (!m.fresh) { // put the HUD back where the previous world boxes were composited
        for (std::size_t i = 0; i < m.world_shown.size(); ++i) {
            const auto& box = m.world_shown[i];
            const auto row_bytes = std::size_t(box[2] - box[0]) * 4;
            for (int row = box[1]; row < box[3]; ++row)
                std::copy_n(m.under[i].begin() + std::ptrdiff_t(std::size_t(row - box[1]) * row_bytes), row_bytes,
                            output.rgba.begin() + (std::ptrdiff_t(row) * output.width + box[0]) * 4);
        }
        boxes = m.world_shown;
    }
    Impl::merge(m.world_ink);
    m.under.resize(m.world_ink.size());
    for (std::size_t i = 0; i < m.world_ink.size(); ++i) { // overlay = HUD over world inside each (disjoint) box
        const auto& box = m.world_ink[i];
        const auto row_bytes = std::size_t(box[2] - box[0]) * 4;
        auto& under = m.under[i];
        under.resize(row_bytes * std::size_t(box[3] - box[1]));
        for (int row = box[1]; row < box[3]; ++row) {
            const auto offset = (std::ptrdiff_t(row) * output.width + box[0]) * 4;
            auto* hud = under.data() + std::size_t(row - box[1]) * row_bytes;
            std::copy_n(output.rgba.begin() + offset, row_bytes, hud);
            auto* target = output.rgba.data() + offset;
            std::copy_n(m.world.rgba.begin() + offset, row_bytes, target);
            for (std::size_t column = 0; column < row_bytes; column += 4) blend(target + column, {hud[column], hud[column + 1], hud[column + 2], hud[column + 3]});
        }
    }
    m.changed.clear();
    if (m.fresh) m.changed.push_back({0, 0, output.width, output.height});
    else {
        boxes.insert(boxes.end(), m.world_ink.begin(), m.world_ink.end());
        Impl::merge(boxes);
        for (const auto& box : boxes) m.changed.push_back({box[0], box[1], box[2] - box[0], box[3] - box[1]});
    }
    m.world_shown = m.world_ink;
    m.clear_world_ink();
    m.fresh = false;
    return m.changed;
}
} // namespace ikv
