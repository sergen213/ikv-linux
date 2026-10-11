#pragma once

#include "ikv/assets.hpp"
#include "ikv/renderer.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>

namespace ikv {
class ClientUI {
public:
    // Original Unicode glyphs first; sibling verdana.ttf, then Fontconfig native coverage.
    explicit ClientUI(const std::filesystem::path& font, int pixelSize = 16);
    ~ClientUI();
    ClientUI(const ClientUI&) = delete;
    ClientUI& operator=(const ClientUI&) = delete;

    void begin(int width, int height);
    void rect(int x, int y, int w, int h, std::array<std::uint8_t, 4> color);
    // Borrow a cached immutable DDS; sample its original base mip without a row flip.
    // Destination is x,y,w,h; normalized UV corners are UL,UR,LL,LR.
    // Source triangles are (UL,UR,LR) and (UL,LR,LL); CPU pixel centers are x+0.5,y+0.5.
    // Bilinear/clamp sampling and straight RGBA tint; clipping preserves the unclipped mapping.
    void image(const assets::DdsImage& image, std::array<float, 4> destination,
               const std::array<std::array<float, 2>, 4>& uv,
               std::array<std::uint8_t, 4> tint = {255, 255, 255, 255});
    // Top-left uses the primary font's line metrics; all faces share that baseline. No wrapping.
    void text(int x, int y, std::string_view utf8,
              std::array<std::uint8_t, 4> color = {255, 255, 255, 255});
    int text_width(std::string_view utf8);
    int line_height() const;
    // Switch the active text size (glyphs stay cached per size); line_height follows.
    void set_pixel_size(int pixelSize);
    int pixel_size() const;
    // Draw following text with the original bold face (sibling verdanab.ttf) where it covers the glyph.
    void set_bold(bool bold);
    const Overlay& overlay() const;
    // World-anchored layer beneath the HUD (name labels, floaters). Draws between begin_world() and end_world() go to it
    // instead of the HUD; each begin_world() starts it empty. compose() puts the HUD over it in overlay() and returns the
    // non-overlapping x,y,w,h rectangles changed since the previous compose (the whole overlay after begin()), so a frame
    // where only world text moved repaints and uploads just its old and new boxes.
    void begin_world();
    void end_world();
    std::span<const std::array<int, 4>> compose();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ikv
