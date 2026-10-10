#include "ikv/client_ui.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H
#include <fontconfig/fontconfig.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
constexpr int canvas_width = 2048, canvas_height = 512;
constexpr int origin_x = 24, origin_y = 32;
constexpr std::array<std::uint8_t, 4> ink{191, 227, 149, 173};

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

struct Library {
    FT_Library value = nullptr;
    Library() { require(FT_Init_FreeType(&value) == 0, "FreeType initialization failed"); }
    ~Library() { FT_Done_FreeType(value); }
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;
};

struct Face {
    FT_Face value = nullptr;
    std::filesystem::path path;
    long index;
    Face(FT_Library library, std::filesystem::path filename, long face_index, int size)
        : path(std::move(filename)), index(face_index) {
        require(FT_New_Face(library, path.string().c_str(), index, &value) == 0,
                "Cannot open independent FreeType face: " + path.string());
        if (!FT_IS_SCALABLE(value) || FT_Select_Charmap(value, FT_ENCODING_UNICODE) ||
            FT_Set_Pixel_Sizes(value, 0, static_cast<FT_UInt>(size))) {
            FT_Done_Face(value);
            value = nullptr;
            throw std::runtime_error("Cannot configure independent Unicode face: " + path.string());
        }
    }
    ~Face() { if (value) FT_Done_Face(value); }
    Face(const Face&) = delete;
    Face& operator=(const Face&) = delete;
};

std::string utf8(std::u32string_view scalars) {
    std::string result;
    for (const auto scalar : scalars) {
        const auto c = static_cast<std::uint32_t>(scalar);
        require(c <= 0x10ffff && !(c >= 0xd800 && c <= 0xdfff), "Invalid diagnostic scalar");
        if (c < 0x80) result.push_back(static_cast<char>(c));
        else if (c < 0x800) {
            result.push_back(static_cast<char>(0xc0 | (c >> 6)));
            result.push_back(static_cast<char>(0x80 | (c & 0x3f)));
        } else if (c < 0x10000) {
            result.push_back(static_cast<char>(0xe0 | (c >> 12)));
            result.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3f)));
            result.push_back(static_cast<char>(0x80 | (c & 0x3f)));
        } else {
            result.push_back(static_cast<char>(0xf0 | (c >> 18)));
            result.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3f)));
            result.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3f)));
            result.push_back(static_cast<char>(0x80 | (c & 0x3f)));
        }
    }
    return result;
}

void metadata(Face& face) {
    std::cout << "font=" << face.path << " face_index=" << face.index
              << " family=" << (face.value->family_name ? face.value->family_name : "(unnamed)")
              << " total_glyphs=" << face.value->num_glyphs << '\n';
    const auto selected = face.value->charmap;
    for (int i = 0; i < face.value->num_charmaps; ++i) {
        const auto map = face.value->charmaps[i];
        std::cout << "  charmap=" << i << " encoding=0x" << std::hex
                  << static_cast<unsigned long>(map->encoding) << std::dec
                  << " platform_id=" << map->platform_id << " encoding_id=" << map->encoding_id;
        if (map->encoding == FT_ENCODING_UNICODE) {
            require(FT_Set_Charmap(face.value, map) == 0, "Cannot select reported Unicode charmap");
            std::size_t count = 0;
            std::vector<bool> mapped_glyphs(static_cast<std::size_t>(face.value->num_glyphs));
            FT_UInt glyph = 0;
            for (auto c = FT_Get_First_Char(face.value, &glyph); glyph;
                 c = FT_Get_Next_Char(face.value, c, &glyph)) {
                if (c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) continue;
                ++count;
                require(glyph < mapped_glyphs.size(), "Charmap glyph index outside face");
                mapped_glyphs[glyph] = true;
            }
            std::cout << " Unicode_mapped_scalars=" << count << " Unicode_unique_glyphs="
                      << std::count(mapped_glyphs.begin(), mapped_glyphs.end(), true);
        }
        if (map == selected) std::cout << " selected";
        std::cout << '\n';
    }
    require(FT_Set_Charmap(face.value, selected) == 0, "Cannot restore selected charmap");
    constexpr std::array<char32_t, 15> probes{
        U'İ', U'ı', U'Ğ', U'ğ', U'Ş', U'ş', U'Ý', U'A', U'V', U'a', U'z', U' ', U'.', U'!', U'?'};
    for (const auto scalar : probes) {
        const auto glyph = FT_Get_Char_Index(face.value, scalar);
        std::array<char, 256> name{};
        const bool named = FT_HAS_GLYPH_NAMES(face.value) &&
                           FT_Get_Glyph_Name(face.value, glyph, name.data(), name.size()) == 0;
        std::cout << "  U+" << std::hex << std::uppercase << std::setw(4) << std::setfill('0')
                  << static_cast<std::uint32_t>(scalar) << std::dec << std::nouppercase << std::setfill(' ')
                  << " glyph=" << glyph << " FT_name=" << (named ? name.data() : "(unavailable)")
                  << (glyph ? " covered" : " absent (.notdef index)") << '\n';
    }
}

struct Selection {
    Face* face;
    FT_UInt glyph;
};

// The oracle owns separate FreeType faces; it never reads ClientUI's private state.
struct Reference {
    Library library;
    Face primary;
    std::unique_ptr<Face> sibling_verdana;
    Face* verdana = nullptr;
    std::unique_ptr<FcConfig, decltype(&FcConfigDestroy)> config{nullptr, FcConfigDestroy};
    std::unique_ptr<FcFontSet, decltype(&FcFontSetDestroy)> fonts{nullptr, FcFontSetDestroy};
    std::vector<std::unique_ptr<Face>> native;
    Face* native_notdef = nullptr;
    int ascender, line_height;

    Reference(const std::filesystem::path& font, int size)
        : primary(library.value, font, 0, size),
          ascender(static_cast<int>((primary.value->size->metrics.ascender + 63) / 64)),
          line_height(std::max(1, static_cast<int>((primary.value->size->metrics.height + 63) / 64))) {
        const auto sibling = font.parent_path() / "verdana.ttf";
        if (std::filesystem::is_regular_file(sibling)) {
            if (std::filesystem::equivalent(font, sibling)) verdana = &primary;
            else {
                sibling_verdana = std::make_unique<Face>(library.value, sibling, 0, size);
                verdana = sibling_verdana.get();
            }
        }
        config.reset(FcInitLoadConfigAndFonts());
        require(config != nullptr, "Fontconfig initialization failed");
        std::unique_ptr<FcPattern, decltype(&FcPatternDestroy)> pattern(FcPatternCreate(), FcPatternDestroy);
        require(pattern != nullptr, "Fontconfig pattern allocation failed");
        require(FcPatternAddString(pattern.get(), FC_FAMILY, reinterpret_cast<const FcChar8*>("sans-serif")) &&
                FcPatternAddBool(pattern.get(), FC_SCALABLE, FcTrue), "Fontconfig family/scalable setup failed");
        require(FcConfigSubstitute(config.get(), pattern.get(), FcMatchPattern), "Fontconfig substitution failed");
        FcDefaultSubstitute(pattern.get());
        FcResult result{};
        fonts.reset(FcFontSort(config.get(), pattern.get(), FcFalse, nullptr, &result));
        require(fonts != nullptr && fonts->nfont > 0, "No native Fontconfig faces");
        native.resize(static_cast<std::size_t>(fonts->nfont));
        std::size_t usable = 0;
        for (int i = 0; i < fonts->nfont; ++i) {
            FcChar8* path = nullptr;
            int index = 0;
            if (FcPatternGetString(fonts->fonts[i], FC_FILE, 0, &path) != FcResultMatch) continue;
            FcPatternGetInteger(fonts->fonts[i], FC_INDEX, 0, &index);
            try {
                native[i] = std::make_unique<Face>(
                    library.value, reinterpret_cast<const char*>(path), index, size);
            } catch (const std::runtime_error&) {
                // Fontconfig can enumerate unusable files; only actual FT faces are candidates.
                continue;
            }
            ++usable;
            const auto face = native[i]->value;
            if (!native_notdef && !FT_Load_Glyph(face, 0, FT_LOAD_DEFAULT | FT_LOAD_NO_BITMAP) &&
                !FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) &&
                face->glyph->bitmap.width > 0 && face->glyph->bitmap.rows > 0 &&
                face->glyph->bitmap.pixel_mode == FT_PIXEL_MODE_GRAY)
                native_notdef = native[i].get();
        }
        require(native_notdef != nullptr, "No drawable native Unicode outline .notdef");
        std::cout << "pixel_size=" << size << " primary_ascender=" << ascender
                  << " primary_line_height=" << line_height << " usable_native_faces=" << usable << '\n';
    }

    Selection select(char32_t scalar) {
        const auto primary_glyph = FT_Get_Char_Index(primary.value, scalar);
        if (primary_glyph) return {&primary, primary_glyph};
        if (verdana && verdana != &primary)
            if (const auto glyph = FT_Get_Char_Index(verdana->value, scalar)) return {verdana, glyph};
        for (int i = 0; i < fonts->nfont; ++i) {
            FcCharSet* charset = nullptr;
            if (!native[i] || FcPatternGetCharSet(fonts->fonts[i], FC_CHARSET, 0, &charset) != FcResultMatch ||
                !FcCharSetHasChar(charset, scalar)) continue;
            if (const auto glyph = FT_Get_Char_Index(native[i]->value, scalar)) return {native[i].get(), glyph};
        }
        return {native_notdef, 0};
    }

    int kerning(Selection previous, Selection current) const {
        if (previous.face != current.face || !previous.glyph || !current.glyph ||
            !FT_HAS_KERNING(current.face->value)) return 0;
        FT_Vector delta{};
        require(FT_Get_Kerning(current.face->value, previous.glyph, current.glyph,
                              FT_KERNING_DEFAULT, &delta) == 0, "Reference kerning failed");
        return static_cast<int>(delta.x / 64);
    }

    void load(Selection selected) const {
        require(FT_Load_Glyph(selected.face->value, selected.glyph, FT_LOAD_DEFAULT | FT_LOAD_NO_BITMAP) == 0,
                "Reference glyph load failed");
        require(FT_Render_Glyph(selected.face->value->glyph, FT_RENDER_MODE_NORMAL) == 0,
                "Reference glyph render failed");
    }

    int width(std::u32string_view scalars) {
        int pen = 0, maximum = 0;
        Selection previous{nullptr, 0};
        for (const auto scalar : scalars) {
            if (scalar == U'\n') { maximum = std::max(maximum, pen); pen = 0; previous = {nullptr, 0}; continue; }
            const auto current = select(scalar);
            pen += kerning(previous, current);
            load(current);
            pen += static_cast<int>((current.face->value->glyph->advance.x + 32) / 64);
            previous = current;
        }
        return std::max({0, pen, maximum});
    }

    ikv::Overlay raster(std::u32string_view scalars, int x = origin_x, int y = origin_y) {
        ikv::Overlay result{canvas_width, canvas_height,
                            std::vector<std::uint8_t>(canvas_width * canvas_height * 4)};
        int pen = x, baseline = y + ascender;
        Selection previous{nullptr, 0};
        for (const auto scalar : scalars) {
            if (scalar == U'\n') { pen = x; baseline += line_height; previous = {nullptr, 0}; continue; }
            const auto current = select(scalar);
            pen += kerning(previous, current);
            load(current);
            const auto slot = current.face->value->glyph;
            const auto& bitmap = slot->bitmap;
            require(!bitmap.width || !bitmap.rows || bitmap.pixel_mode == FT_PIXEL_MODE_GRAY,
                    "Reference glyph has unsupported bitmap format");
            const int left = pen + slot->bitmap_left, top = baseline - slot->bitmap_top;
            for (unsigned row = 0; row < bitmap.rows; ++row) {
                const auto* source = bitmap.buffer + static_cast<std::ptrdiff_t>(row) * bitmap.pitch;
                for (unsigned column = 0; column < bitmap.width; ++column) {
                    const int px = left + static_cast<int>(column), py = top + static_cast<int>(row);
                    require(px >= 0 && px < canvas_width && py >= 0 && py < canvas_height,
                            "Diagnostic canvas clips reference glyph");
                    require(bitmap.num_grays >= 2, "Invalid reference gray levels");
                    const unsigned coverage = bitmap.num_grays == 256 ? source[column]
                        : source[column] * 255u / (bitmap.num_grays - 1);
                    const unsigned alpha = (ink[3] * coverage + 127) / 255;
                    if (!alpha) continue;
                    auto* target = result.rgba.data() + (static_cast<std::size_t>(py) * canvas_width + px) * 4;
                    const unsigned retained = target[3] * (255 - alpha);
                    const unsigned combined = alpha * 255 + retained;
                    for (unsigned channel = 0; channel < 3; ++channel)
                        target[channel] = static_cast<std::uint8_t>(
                            (ink[channel] * alpha * 255 + target[channel] * retained + combined / 2) / combined);
                    target[3] = static_cast<std::uint8_t>((combined + 127) / 255);
                }
            }
            pen += static_cast<int>((slot->advance.x + 32) / 64);
            previous = current;
        }
        return result;
    }
};

void equal_pixels(const ikv::Overlay& actual, const ikv::Overlay& expected, std::string_view label) {
    require(actual.width == expected.width && actual.height == expected.height, "Raster dimensions differ");
    const auto mismatch = std::mismatch(actual.rgba.begin(), actual.rgba.end(), expected.rgba.begin(), expected.rgba.end());
    if (mismatch.first != actual.rgba.end())
        throw std::runtime_error(std::string(label) + ": RGBA mismatch at byte " +
                                 std::to_string(mismatch.first - actual.rgba.begin()));
}

void check_run(ikv::ClientUI& ui, Reference& reference, std::u32string_view scalars, std::string_view label) {
    const auto text = utf8(scalars);
    const int actual_width = ui.text_width(text), expected_width = reference.width(scalars);
    require(actual_width == expected_width, std::string(label) + ": measured advance differs from independent FT");
    ui.begin(canvas_width, canvas_height);
    ui.text(origin_x, origin_y, text, ink);
    equal_pixels(ui.overlay(), reference.raster(scalars), label);
    std::size_t pixels = 0;
    for (std::size_t i = 3; i < ui.overlay().rgba.size(); i += 4) pixels += ui.overlay().rgba[i] != 0;
    require(scalars.empty() || pixels > 0, std::string(label) + ": raster is empty");
    std::cout << "PASS " << label << " width=" << actual_width << " nonzero_alpha_pixels=" << pixels << '\n';
}

void write_sample(const ikv::Overlay& overlay, const std::filesystem::path& path, std::string_view label) {
    std::ofstream output(path, std::ios::binary);
    require(output.is_open(), "Cannot open raster sample: " + path.string());
    output << "P6\n# " << label << "\n" << overlay.width << ' ' << overlay.height << "\n255\n";
    constexpr std::array<unsigned, 3> background{24, 28, 36};
    std::array<char, canvas_width * 3> row{};
    for (int y = 0; y < overlay.height; ++y) {
        for (int x = 0; x < overlay.width; ++x) {
            const auto* pixel = overlay.rgba.data() + (static_cast<std::size_t>(y) * overlay.width + x) * 4;
            for (std::size_t channel = 0; channel < background.size(); ++channel)
                row[static_cast<std::size_t>(x) * 3 + channel] = static_cast<char>(
                    (pixel[channel] * pixel[3] + background[channel] * (255 - pixel[3]) + 127) / 255);
        }
        output.write(row.data(), static_cast<std::streamsize>(overlay.width) * 3);
    }
    output.close();
    require(!output.fail(), "Cannot write raster sample: " + path.string());
    std::cout << "raster_sample label=\"" << label << "\" path=" << path << '\n';
}

void check_image_api(ikv::ClientUI& ui) {
    // Synthetic decoded storage: conventional bilinear/clamp edge checks, not original raster-parity evidence.
    // UVs use texel-center normalization; destination samples are CPU pixel centers x+0.5,y+0.5.
    ikv::assets::DdsImage image{2, 2,
        {255, 0, 255, 255, // Sentinel before the nonzero base-mip offset.
         0, 0, 0, 255, 160, 0, 80, 255,
         0, 160, 80, 255, 160, 160, 160, 255},
        {{2, 2, 4, 16}}};
    const std::array<std::array<float, 2>, 4> uv{{
        {0.25f, 0.25f}, {0.75f, 0.25f}, {0.25f, 0.75f}, {0.5f, 0.5f}}};
    const std::array<float, 4> box{0, 0, 1, 1};
    ui.begin(4, 4);
    ui.image(image, {-1, -1, 4, 4}, uv);
    // Non-affine LR distinguishes (UL,UR,LR)/(UL,LR,LL) from a bilinear UV quad.
    // Clipping must retain the original -1,-1 destination, not remap the visible 3x3.
    equal_pixels(ui.overlay(), ikv::Overlay{4, 4, {
        30,30,30,255, 70,30,50,255, 110,30,70,255, 0,0,0,0,
        30,70,50,255, 50,50,50,255, 90,50,70,255, 0,0,0,0,
        30,110,70,255, 50,90,70,255, 70,70,70,255, 0,0,0,0,
        0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0}},
        "image source triangles, bilinear texel centers, clipping and base-mip offset");
    auto constant_uv = uv;
    constant_uv.fill({-1, 2});
    ui.begin(1, 1);
    ui.image(image, box, constant_uv);
    equal_pixels(ui.overlay(), ikv::Overlay{1, 1, {0,160,80,255}}, "image UV clamp");
    for (std::size_t alpha = 7; alpha < image.rgba.size(); alpha += 4) image.rgba[alpha] = 128;
    constant_uv.fill({0.75f, 0.25f});
    ui.begin(1, 1);
    ui.image(image, box, constant_uv, {128,255,64,128});
    equal_pixels(ui.overlay(), ikv::Overlay{1, 1, {80,0,20,64}}, "image straight RGB/alpha tint");
    ui.begin(1, 1);
    ui.rect(0, 0, 1, 1, {20,40,60,128});
    ui.image(image, box, constant_uv, {128,255,64,128});
    equal_pixels(ui.overlay(), ikv::Overlay{1, 1, {44,24,44,160}}, "image straight-alpha blend");
    const auto unchanged = ui.overlay();
    const auto rejected = [&](const ikv::assets::DdsImage& value, std::array<float, 4> destination,
                              const std::array<std::array<float, 2>, 4>& coordinates) {
        bool invalid = false;
        try { ui.image(value, destination, coordinates); }
        catch (const std::invalid_argument&) { invalid = true; }
        require(invalid, "Image API accepted invalid mip storage or nonfinite destination/UV");
    };
    auto bad = image;
    for (const auto mip : std::array<ikv::assets::DdsMip, 7>{{
             {0,2,4,16}, {1,2,4,8}, {2,1,4,8}, {2,2,4,15},
             {2,2,image.rgba.size() + 1,16}, {2,2,5,16},
             {2,2,std::numeric_limits<std::size_t>::max(),16}}}) {
        bad.mips[0] = mip;
        rejected(bad, box, uv);
    }
    bad.mips.clear();
    rejected(bad, box, uv);
    bad = image;
    bad.width = 1;
    rejected(bad, box, uv);
    for (const float value : {std::numeric_limits<float>::quiet_NaN(),
                              std::numeric_limits<float>::infinity(),
                              -std::numeric_limits<float>::infinity()}) {
        for (std::size_t axis = 0; axis < box.size(); ++axis) {
            auto destination = box;
            destination[axis] = value;
            rejected(image, destination, uv);
        }
        for (std::size_t corner = 0; corner < uv.size(); ++corner)
            for (std::size_t axis = 0; axis < 2; ++axis) {
                auto coordinates = uv;
                coordinates[corner][axis] = value;
                rejected(image, box, coordinates);
            }
    }
    for (const float size : {0.0f, -1.0f}) {
        ui.image(image, {0,0,size,1}, uv);
        ui.image(image, {0,0,1,size}, uv);
    }
    ui.image(image, box, uv, {255,255,255,0});
    equal_pixels(ui.overlay(), unchanged, "image invalid inputs and empty sizes leave overlay unchanged");
    std::cout << "PASS synthetic decoded image API edges (not original raster-parity evidence)\n";
}

void exercise(const std::filesystem::path& font, int size, bool human, bool print_metadata,
              const std::filesystem::path& raster_directory) {
    std::cout << "role=" << (human ? "human text" : "explicit icon resource API")
              << " font=" << font << " pixel_size=" << size << '\n';
    Reference reference(font, size);
    ikv::ClientUI ui(font, size);
    if (print_metadata) {
        metadata(reference.primary);
        if (reference.verdana && reference.verdana != &reference.primary) metadata(*reference.verdana);
    }
    const std::u32string ordinary = U"AV To Wa Hello, world! ordinary lowercase: abcdefghijklmnopqrstuvwxyz 0123456789 .,:;!? () [] +-=/";
    const std::u32string human_sample =
        U"İstanbul / İstanbul'da görev\n"
        U"ABCÇDEFGĞHIİJKLMNOÖPRSŞTUÜVYZ\n"
        U"abcçdefgğhıijklmnoöprsştuüvyz\n"
        U"Can: 125/150 Mana: 80/100 Bölge: İstanbul Şöhret: 42";
    std::u32string sample;
    if (human) {
        sample = human_sample;
        for (const auto scalar : sample) {
            if (scalar == U'\n') continue;
            require(FT_Get_Char_Index(reference.primary.value, scalar) != 0,
                    "Human primary lacks ordinary Turkish sample cmap coverage");
            require(reference.select(scalar).face == &reference.primary,
                    "Human sample did not select its primary face");
        }
        for (const auto scalar : ordinary)
            require(FT_Get_Char_Index(reference.primary.value, scalar) != 0 &&
                    reference.select(scalar).face == &reference.primary,
                    "Human primary lacks ordinary lowercase/punctuation coverage");
        check_run(ui, reference, sample, "human primary Turkish alphabet and mixed-case HUD");
    } else {
        // These Unicode slots select resource glyphs; their shapes need not be human letters.
        for (const auto scalar : std::u32string_view(U"ABCDEFGHIJKLMNOPQRSTUVWXYZ ["))
            if (FT_Get_Char_Index(reference.primary.value, scalar)) sample.push_back(scalar);
        require(sample.size() > 20, "Insufficient icon primary coverage for preservation run");
        for (const auto scalar : sample)
            require(reference.select(scalar).face == &reference.primary, "Icon primary glyph was displaced");
        check_run(ui, reference, sample, "explicit icon-resource primary glyph preservation");
    }
    if (!raster_directory.empty())
        write_sample(ui.overlay(), raster_directory /
                     ((human ? std::string("human-") : std::string("explicit-icon-")) +
                      std::to_string(size) + "px.ppm"),
                     human ? "ClientUI human Turkish text" : "ClientUI explicit icon-resource Unicode slots");
    check_run(ui, reference, ordinary, human ? "human ordinary lowercase and punctuation"
                                           : "icon API ordinary input with actual per-scalar fallback");

    const auto dotted_i = reference.select(U'İ');
    const bool primary_dotted_i = FT_Get_Char_Index(reference.primary.value, U'İ') != 0;
    if (reference.verdana)
        require(FT_Get_Char_Index(reference.verdana->value, U'İ') != 0, "Sibling Verdana lacks U+0130");
    require(!human || primary_dotted_i, "Human primary lacks U+0130");
    if (primary_dotted_i || reference.verdana)
        require(dotted_i.face == (primary_dotted_i ? &reference.primary : reference.verdana),
                "U+0130 oracle did not select the first covering face");
    std::cout << "U+0130 expected_face=" << dotted_i.face->path
              << " glyph=" << dotted_i.glyph << " baseline=" << origin_y + reference.ascender << '\n';
    check_run(ui, reference, U"İ", primary_dotted_i ? "primary U+0130 bitmap on primary baseline"
        : reference.verdana ? "icon API sibling Verdana U+0130 on primary baseline"
                            : "icon API native U+0130 fallback on primary baseline");
    check_run(ui, reference,
              U"ABCÇDEFGĞHIİJKLMNOÖPRSŞTUÜVYZ abcçdefgğhıijklmnoöprsştuüvyz",
              human ? "human full Turkish uppercase/lowercase alphabet" : "icon API Turkish per-scalar selection");
    check_run(ui, reference, U"İıĞğŞşÝ", "Turkish scalar probes without transliteration");
    check_run(ui, reference, U"AVİToİWaİAV", "actual same-face kerning and face transitions");
    for (const auto pair : {U"Aİ", U"İA", U"Vİ", U"İV"}) {
        const auto scalars = std::u32string_view(pair);
        if (reference.select(scalars[0]).face == reference.select(scalars[1]).face) continue;
        const int separate = ui.text_width(utf8(scalars.substr(0, 1))) +
                             ui.text_width(utf8(scalars.substr(1, 1)));
        require(ui.text_width(utf8(scalars)) == separate, "Kerning leaked across an actual face transition");
        std::cout << "PASS cross-face zero kerning width=" << separate << '\n';
    }

    const std::u32string extra = U"Ω Ж € → 漢 😀";
    check_run(ui, reference, extra, "additional Unicode");
    char32_t native_scalar = 0;
    for (const auto scalar : std::u32string_view(U"ΩЖ→漢😀�")) {
        const auto selected = reference.select(scalar);
        if (selected.glyph && selected.face != &reference.primary && selected.face != reference.verdana) {
            native_scalar = scalar;
            std::cout << "native_coverage U+" << std::hex << static_cast<std::uint32_t>(scalar) << std::dec
                      << " expected_face=" << selected.face->path << " glyph=" << selected.glyph << '\n';
            break;
        }
    }
    require(native_scalar != 0, "No native-only coverage found among diagnostic probes");
    check_run(ui, reference, std::u32string(1, native_scalar), "actual native fallback bitmap");

    const auto missing = reference.select(U'\U0010ffff');
    require(FT_Get_Char_Index(reference.primary.value, U'\U0010ffff') == 0 &&
            (!reference.verdana || FT_Get_Char_Index(reference.verdana->value, U'\U0010ffff') == 0),
            "Original face actually covers U+10FFFF");
    for (const auto& face : reference.native)
        require(!face || FT_Get_Char_Index(face->value, U'\U0010ffff') == 0,
                "Native face actually covers U+10FFFF");
    require(missing.glyph == 0 && missing.face == reference.native_notdef,
            "U+10FFFF is not missing everywhere or does not select native .notdef");
    std::cout << "U+10FFFF expected_native_notdef_face=" << missing.face->path << " glyph=0\n";
    check_run(ui, reference, U"\U0010ffff", "missing-everywhere actual native .notdef");

    // Explicit expectations, not a second copy of the production UTF-8 parser.
    const std::array<std::pair<std::string, std::u32string>, 10> malformed{{
        {std::string("\x80", 1), U"�"},
        {std::string("\xc0\xaf", 2), U"��"},
        {std::string("\xe0\x80\xaf", 3), U"�"},
        {std::string("\xed\xa0\x80", 3), U"�"},
        {std::string("\xf4\x90\x80\x80", 4), U"�"},
        {std::string("\xf5\x80", 2), U"��"},
        {std::string("\xe2\x82", 2), U"�"},
        {std::string("\xe2") + "A", U"�A"},
        {std::string("\xe2\x82") + "A", U"�A"},
        {std::string("A\xff\nB", 4), U"A�\nB"}
    }};
    for (std::size_t i = 0; i < malformed.size(); ++i) {
        const auto& [bytes, replacement] = malformed[i];
        check_run(ui, reference, replacement, "expected U+FFFD replacement");
        const auto expected = ui.overlay();
        require(ui.text_width(bytes) == ui.text_width(utf8(replacement)), "Malformed UTF-8 replacement width differs");
        ui.begin(canvas_width, canvas_height);
        ui.text(origin_x, origin_y, bytes, ink);
        equal_pixels(ui.overlay(), expected, "Malformed UTF-8 replacement raster");
        std::cout << "PASS malformed_case=" << i << " width=" << ui.text_width(bytes) << '\n';
    }

    const std::u32string first = U"AVİ abc!", second = U"İıĞğŞş";
    const auto multiline = first + U"\n" + second + U"\n";
    check_run(ui, reference, multiline, "multiline raster and kerning reset");
    require(ui.text_width(utf8(multiline)) == std::max(ui.text_width(utf8(first)), ui.text_width(utf8(second))),
            "Multiline width is not maximum line advance");
    require(ui.text_width("") == 0 && ui.text_width("\n\n") == 0, "Empty line width is not zero");

    // A face transition prevents boundary kerning: the marker must begin at the measured advance.
    const auto prefix = first + U" ";
    const auto suffix = std::u32string(U"\U0010ffff");
    const int advance = ui.text_width(utf8(prefix));
    check_run(ui, reference, prefix + suffix, "measured-advance marker reference");
    const auto joined = ui.overlay();
    ui.begin(canvas_width, canvas_height);
    ui.text(origin_x, origin_y, utf8(prefix), ink);
    ui.text(origin_x + advance, origin_y, utf8(suffix), ink);
    equal_pixels(ui.overlay(), joined, "Measured-advance split raster alignment");
    const int dx = 37, dy = 19;
    ui.begin(canvas_width, canvas_height);
    ui.text(origin_x + dx, origin_y + dy, utf8(prefix + suffix), ink);
    equal_pixels(ui.overlay(), reference.raster(prefix + suffix, origin_x + dx, origin_y + dy),
                 "Translated raster alignment");
    std::cout << "PASS multiline max-width and translated measured-advance alignment advance=" << advance
              << " dx=" << dx << " dy=" << dy << '\n';
    if (human && size == 16) check_image_api(ui);
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc <= 4, "Usage: client_font_diagnostic [human-font-path] [icon-font-path] [raster-output-directory]");
        const std::filesystem::path human_font = argc >= 2 ? argv[1] : "original/app/base/fonts/verdana.ttf";
        const std::filesystem::path icon_font = argc >= 3 ? std::filesystem::path(argv[2])
                                                        : human_font.parent_path() / "sobee.ttf";
        const std::filesystem::path raster_directory = argc >= 4 ? argv[3] : "";
        if (!raster_directory.empty()) std::filesystem::create_directories(raster_directory);
        std::cout << "Human text uses the configured human face; the explicit icon-resource API preserves resource glyphs.\n"
                     "Cmap coverage, FT glyph names and raster equality alone do not prove human semantic readability.\n";
        for (const int size : {16, 32}) {
            exercise(human_font, size, true, size == 16, raster_directory);
            exercise(icon_font, size, false, size == 16, raster_directory);
        }
        std::cout << "PASS all human/icon independent font metadata, glyph-selection, raster and advance assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL client_font_diagnostic: " << error.what() << '\n';
        return 1;
    }
}
