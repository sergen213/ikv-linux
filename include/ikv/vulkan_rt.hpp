#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace ikv {
struct Scene;
struct RenderEntity;

struct VulkanRtFeatures {
    bool acceleration_structure = false;
    bool ray_query = false;
    bool buffer_device_address = false;
    bool sampled_image_nonuniform = false; // alpha-tested ray hits index rtAlphaTextures per lane
    bool supported() const noexcept {
        return acceleration_structure && ray_query && buffer_device_address && sampled_image_nonuniform;
    }
};
VulkanRtFeatures query_rt_features(VkPhysicalDevice physical_device);
// Size of the RT set's binding 2 sampler array (rt_lighting.glsl rtAlphaTextures).
inline constexpr std::uint32_t rt_alpha_texture_slots = 64;

// Requires Vulkan 1.2+, enabled bufferDeviceAddress/accelerationStructure/rayQuery/
// shaderSampledImageArrayNonUniformIndexing, and KHR_acceleration_structure, KHR_ray_query,
// KHR_deferred_host_operations.
// The supplied queue must support compute and share the ray-query renderer's
// queue family. The caller owns external queue/device synchronization: complete
// every renderer frame using these resources before rebuild, and drain device
// work before destruction. RT waits only for its own builds. Destroy before
// VkDevice; rebind ALL RT descriptors after each rebuild. Progress runs on the
// calling thread, may throw, and must not reenter the renderer or submit queue work.
class VulkanRtScene {
public:
    VulkanRtScene(VkPhysicalDevice, VkDevice, VkQueue, std::uint32_t queue_family,
        std::function<void()> progress = {});
    ~VulkanRtScene() noexcept;
    VulkanRtScene(const VulkanRtScene&) = delete;
    VulkanRtScene& operator=(const VulkanRtScene&) = delete;
    void rebuild(const Scene&, std::span<const RenderEntity> entities = {});
    // In-frame update of the scene last passed to rebuild() (same Scene object): skin/transform changes and actor
    // attach/detach (retained meshes kept by key, new meshes appended). Records staged copies, new-BLAS builds and an
    // in-place BLAS/TLAS update or TLAS rebuild into the caller's frame command buffer ahead of its ray queries, with
    // barriers ordering them after earlier frames' uses. The caller completes the previous frame of `slot` (0/1)
    // first; BLAS dropped while recording are freed by the next call for that slot or by rebuild(). The TLAS handle
    // and triangle buffer/range stay unchanged. False: the change needs rebuild() (packing, triangle or TLAS capacity
    // growth, new zone); BLAS already dropped stay alive until rebuild() returns.
    bool record_refresh(const Scene&, std::span<const RenderEntity>, VkCommandBuffer frame, unsigned slot);
    VkAccelerationStructureKHR acceleration_structure() const noexcept;
    VkBuffer triangles() const noexcept;
    VkDeviceSize triangle_bytes() const noexcept;
    // Diffuse texture names of the alpha-tested materials, by rtAlphaTextures slot (set by rebuild()).
    std::span<const std::string> alpha_textures() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ikv
