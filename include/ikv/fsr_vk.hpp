#pragma once
#include "ikv/renderer.hpp"
#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace ikv {
// AMD FidelityFX Super Resolution 3.1.4 upscaler (FidelityFX SDK v1.1.4 Vulkan backend, third_party/FidelityFX-SDK).
// Inputs: render-resolution colour/depth/reactive, unjittered UV-space motion (previousUV = UV + motion), non-inverted
// finite depth. The upscaler records compute work into the caller's command buffer; it never submits or waits.
class Fsr3Upscaler {
public:
    // Device support (reason in *why when false) and the core features FSR's shaders need at device creation.
    static bool supported(VkPhysicalDevice, const char** why);
    static void enable_features(VkPhysicalDevice, VkPhysicalDeviceFeatures&);
    // Frame generation (optical flow + frame interpolation) on top of supported(): subgroup arithmetic and storage
    // writes to the display-format interpolation source/output.
    static bool frame_generation_supported(VkPhysicalDevice, VkFormat display, const char** why);
    static VkExtent2D render_extent(VkExtent2D display, FsrQuality);
    // generation_format != VK_FORMAT_UNDEFINED also creates the FSR 3.1.4 optical-flow and frame-interpolation contexts
    // (display size, that colour format), which reuse this upscaler's dilated depth/motion and previous depth.
    Fsr3Upscaler(VkPhysicalDevice, VkDevice, VkExtent2D render, VkExtent2D display, bool hdr,
                 VkFormat generation_format = VK_FORMAT_UNDEFINED);
    ~Fsr3Upscaler();
    Fsr3Upscaler(const Fsr3Upscaler&) = delete;
    Fsr3Upscaler& operator=(const Fsr3Upscaler&) = delete;
    // FSR's Halton(2,3) sub-pixel offset in render pixels ([-.5,.5]) for a frame index.
    std::array<float, 2> jitter(std::uint64_t frame) const;
    struct Image { VkImage image; VkFormat format; VkExtent2D extent; VkImageUsageFlags usage; };
    // All inputs are in SHADER_READ_ONLY_OPTIMAL (depth too) and the output as well; they are returned in that layout.
    struct Dispatch {
        VkCommandBuffer command;
        Image color, depth, motion, reactive, output;
        std::array<float, 2> jitter;
        float frame_ms, sharpness, near_plane, far_plane, fov_y;
        bool reset;
    };
    void dispatch(const Dispatch&);
    // After dispatch() in the same command buffer: writes the frame halfway between the previous and this source
    // (both display size, generation_format, HUD-less, SHADER_READ_ONLY_OPTIMAL) into output (GENERAL, storage +
    // sampled), recording the copy of source the next call interpolates from. reset: no valid previous source/history.
    struct Interpolation {
        VkCommandBuffer command;
        Image source, output;
        float frame_ms, near_plane, far_plane, fov_y;
        bool reset;
        std::uint64_t frame; // +1 per call; a gap resets interpolation
    };
    void interpolate(const Interpolation&);
    bool generates() const;
    VkExtent2D render() const { return render_; }
private:
    struct Context;
    std::unique_ptr<Context> context_;
    std::vector<unsigned char> scratch_, generation_scratch_;
    VkExtent2D render_{}, display_{};
};
}
