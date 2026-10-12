#include "ikv/vulkan_rt.hpp"
#include "ikv/scene.hpp"
#include "ikv/renderer.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ikv {
namespace {
void checked(VkResult result, const char* operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " failed (VkResult " + std::to_string(result) + ")");
}
bool extension(const std::vector<VkExtensionProperties>& extensions, const char* name) {
    return std::any_of(extensions.begin(), extensions.end(), [name](const auto& e) {
        return std::strcmp(e.extensionName, name) == 0;
    });
}
struct Triangle { std::array<float, 4> a, b, c, color; };
static_assert(sizeof(Triangle) == 64);
// Triangle records carry the vertex UVs as half2 bits in a/b/c.w for rt_lighting.glsl's source alpha test; instances
// whose material is alpha-tested (source cutoff, scene.cpp) are FORCE_NO_OPAQUE with the rtAlphaTextures slot + 1 in
// their shader binding table offset (unused by ray queries otherwise).
float half_uv(const Vertex& vertex) {
    const auto half = [](float x) {
        return std::uint32_t(std::bit_cast<std::uint16_t>(static_cast<_Float16>(std::isfinite(x) ? std::clamp(x, -65504.f, 65504.f) : 0.f)));
    };
    return std::bit_cast<float>(half(vertex.u) | half(vertex.v) << 16);
}
Triangle triangle_record(const Vertex& a, const Vertex& b, const Vertex& c, const std::array<float, 4>& color) {
    return {{a.x,a.y,a.z,half_uv(a)}, {b.x,b.y,b.z,half_uv(b)}, {c.x,c.y,c.z,half_uv(c)}, color};
}
// Marks an alpha-tested placement: slot + 1 of its material's diffuse in `slots` (appended when `assign` and room
// remains); no slot leaves it opaque.
void alpha_test(VkAccelerationStructureInstanceKHR& instance, const Scene& scene, std::uint32_t material,
                std::vector<std::string>& slots, bool assign) {
    if (material >= scene.materials.size()) return;
    const auto& m = scene.materials[material];
    if (!(m.alpha_cutoff > 0) || m.diffuse.empty()) return;
    auto found = std::find(slots.begin(), slots.end(), m.diffuse);
    if (found == slots.end()) {
        // ponytail: textures past rt_alpha_texture_slots (zones use <= 22) and those first met by an in-frame actor
        // append stay opaque quads until the next rebuild.
        if (!assign || slots.size() >= rt_alpha_texture_slots) return;
        found = slots.insert(slots.end(), m.diffuse);
    }
    instance.instanceShaderBindingTableRecordOffset = (static_cast<std::uint32_t>(found - slots.begin()) + 1) & 0xffffffu;
    instance.flags |= VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR;
}
constexpr std::uint8_t mesh_referenced = 1;
constexpr std::uint8_t mesh_color_draw = 2;
// Inputs, instances and triangle records are also copy destinations of record_refresh's staged per-frame updates.
constexpr VkBufferUsageFlags input_usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
    | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
constexpr VkBufferUsageFlags triangle_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
VkDeviceSize aligned(VkDeviceSize value, VkDeviceSize alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}
void validate_transform(const Transform& transform) {
    const auto& m = transform.matrix;
    for (float value : m)
        if (!std::isfinite(value)) throw std::runtime_error("Non-finite RT instance transform");
    const double determinant = double(m[0]) * (double(m[5])*m[10] - double(m[9])*m[6])
        - double(m[4]) * (double(m[1])*m[10] - double(m[9])*m[2])
        + double(m[8]) * (double(m[1])*m[6] - double(m[5])*m[2]);
    if (determinant == 0 || m[3] != 0 || m[7] != 0 || m[11] != 0 || m[15] != 1)
        throw std::runtime_error("RT requires invertible affine instance transforms");
}
} // namespace

VulkanRtFeatures query_rt_features(VkPhysicalDevice physical) {
    std::vector<VkExtensionProperties> extensions;
    VkResult result;
    do {
        std::uint32_t count = 0;
        checked(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr), "Enumerating RT extensions");
        extensions.resize(count);
        result = vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, extensions.data());
        extensions.resize(count);
    } while (result == VK_INCOMPLETE);
    checked(result, "Enumerating RT extensions");
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical, &properties);
    if (properties.apiVersion < VK_API_VERSION_1_2) return {};
    VkPhysicalDeviceRayQueryFeaturesKHR ray{};
    ray.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration{};
    acceleration.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    VkPhysicalDeviceBufferDeviceAddressFeatures address{};
    address.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
    address.pNext = &acceleration;
    acceleration.pNext = &ray;
    VkPhysicalDeviceDescriptorIndexingFeatures indexing{};
    indexing.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
    ray.pNext = &indexing;
    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &address;
    vkGetPhysicalDeviceFeatures2(physical, &features);
    const bool dependencies = extension(extensions, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    return {dependencies && extension(extensions, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) && acceleration.accelerationStructure,
            extension(extensions, VK_KHR_RAY_QUERY_EXTENSION_NAME) && ray.rayQuery,
            address.bufferDeviceAddress != VK_FALSE, indexing.shaderSampledImageArrayNonUniformIndexing != VK_FALSE
                && std::min(properties.limits.maxPerStageDescriptorSamplers, properties.limits.maxPerStageDescriptorSampledImages)
                       >= rt_alpha_texture_slots + 16};
}

struct VulkanRtScene::Impl {
    // Device memory shared by every buffer bound into it; freed (implicitly unmapped) with the last such buffer.
    struct Block {
        VkDevice device = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
        std::byte* mapped = nullptr; // persistent mapping of host-visible blocks
        Block() = default;
        Block(const Block&) = delete;
        Block& operator=(const Block&) = delete;
        ~Block() { if (memory) vkFreeMemory(device, memory, nullptr); }
    };
    struct Buffer {
        Buffer& operator=(Buffer&& other) noexcept {
            if (this == &other) return *this;
            if (handle) vkDestroyBuffer(device, handle, nullptr);
            device = other.device;
            handle = std::exchange(other.handle, VK_NULL_HANDLE);
            block = std::move(other.block);
            offset = other.offset;
            size = other.size;
            address = other.address;
            return *this;
        }
        VkDevice device = VK_NULL_HANDLE;
        VkBuffer handle = VK_NULL_HANDLE;
        std::shared_ptr<Block> block;
        VkDeviceSize offset = 0;
        VkDeviceSize size = 0;
        VkDeviceAddress address = 0;
        std::byte* mapped() const noexcept { return block && block->mapped ? block->mapped + offset : nullptr; }
        Buffer() = default;
        Buffer(const Buffer&) = delete;
        Buffer& operator=(const Buffer&) = delete;
        Buffer(Buffer&& other) noexcept
            : device(other.device), handle(std::exchange(other.handle, VK_NULL_HANDLE)), block(std::move(other.block)),
              offset(other.offset), size(other.size), address(other.address) {}
        ~Buffer() { if (handle) vkDestroyBuffer(device, handle, nullptr); }
    };
    // One rebuild/append sub-allocates its many per-mesh buffers from a few geometrically growing blocks per memory
    // type: one vkAllocateMemory per buffer made ~18k allocations ~5.1 s of the ~8.2 s Eminönü RT load.
    struct Pool {
        std::array<std::shared_ptr<Block>, 2 * VK_MAX_MEMORY_TYPES> blocks; // [type*2 + host_visible]
        std::array<VkDeviceSize, 2 * VK_MAX_MEMORY_TYPES> used{}, next{};
    };
    struct Acceleration {
        VkDevice device;
        PFN_vkDestroyAccelerationStructureKHR destroy;
        VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
        VkDeviceAddress address = 0;
        Buffer vertices, indices, storage, scratch;
        Acceleration(VkDevice d, PFN_vkDestroyAccelerationStructureKHR fn) : device(d), destroy(fn) {}
        ~Acceleration() { if (handle) destroy(device, handle, nullptr); }
    };
    struct BuildInput {
        VkAccelerationStructureGeometryKHR geometry{};
        VkAccelerationStructureBuildGeometryInfoKHR info{};
        VkAccelerationStructureBuildRangeInfoKHR range{};
        BuildInput() {
            geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
            info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        }
    };
    struct State {
        // Reverse member destruction: TLAS before referenced BLAS, then buffers.
        Buffer triangles, instances;
        std::vector<std::unique_ptr<Acceleration>> bottom;
        std::unique_ptr<Acceleration> top;
        const Scene* source = nullptr;
        std::vector<std::size_t> mesh_blas, vertex_counts, index_counts;
        std::vector<std::uint32_t> mesh_offset;
        std::vector<std::uint64_t> revisions, keys;
        std::vector<std::string> alpha_textures; // rtAlphaTextures slots (alpha_test)
        bool rebuild_top = false; // Retired/moved BLAS: the next TLAS build may not update in place.
        std::vector<VkAccelerationStructureInstanceKHR> cpu_instances;
        std::uint32_t instance_count = 0;
        // TLAS and instance buffer are sized for this many instances, so placement-count changes (actor attach/detach,
        // entities entering/leaving) rebuild the same TLAS handle inside a frame instead of replacing it.
        std::uint32_t instance_capacity = 0;
    };
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool pending = false;
    std::function<void()> progress;
    VkPhysicalDeviceMemoryProperties memory_properties{};
    VkPhysicalDeviceAccelerationStructurePropertiesKHR properties{};
    VkDeviceSize max_triangle_bytes = 0;
    PFN_vkCreateAccelerationStructureKHR create;
    PFN_vkDestroyAccelerationStructureKHR destroy;
    PFN_vkGetAccelerationStructureBuildSizesKHR sizes;
    PFN_vkCmdBuildAccelerationStructuresKHR build;
    PFN_vkGetAccelerationStructureDeviceAddressKHR as_address;
    PFN_vkGetBufferDeviceAddress buffer_address;
    std::unique_ptr<State> state;
    std::vector<BuildInput> updates;
    // record_refresh host inputs: one persistently mapped arena per caller frame slot, rewritten only after the caller
    // completed that slot's previous frame.
    std::array<Buffer, 2> arenas;
    // BLAS of departed actors, dropped while recording a frame of that slot. Its TLAS rebuild in the same frame stops
    // referencing them; they are freed once the caller completed that frame (and with it every earlier one).
    std::array<std::vector<std::unique_ptr<Acceleration>>, 2> retired_slots;
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> build_infos;
    std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> build_ranges;
    std::vector<std::pair<VkBuffer, VkBufferCopy>> staged;

    template<class T> T procedure(const char* name) {
        auto pointer = reinterpret_cast<T>(vkGetDeviceProcAddr(device, name));
        if (!pointer) throw std::runtime_error(std::string("Missing enabled Vulkan RT procedure: ") + name);
        return pointer;
    }
    Impl(VkPhysicalDevice p, VkDevice d, VkQueue q, std::uint32_t family, std::function<void()> hook)
        : physical(p), device(d), queue(q), progress(std::move(hook)) {
        if (!physical || !device || !queue || !query_rt_features(physical).supported())
            throw std::runtime_error("Vulkan hardware ray query features are unavailable");
        create = procedure<PFN_vkCreateAccelerationStructureKHR>("vkCreateAccelerationStructureKHR");
        destroy = procedure<PFN_vkDestroyAccelerationStructureKHR>("vkDestroyAccelerationStructureKHR");
        sizes = procedure<PFN_vkGetAccelerationStructureBuildSizesKHR>("vkGetAccelerationStructureBuildSizesKHR");
        build = procedure<PFN_vkCmdBuildAccelerationStructuresKHR>("vkCmdBuildAccelerationStructuresKHR");
        as_address = procedure<PFN_vkGetAccelerationStructureDeviceAddressKHR>("vkGetAccelerationStructureDeviceAddressKHR");
        buffer_address = procedure<PFN_vkGetBufferDeviceAddress>("vkGetBufferDeviceAddress");
        std::uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
        if (family >= count || !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT))
            throw std::runtime_error("RT builds require a compute-capable queue family");
        vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
        VkFormatProperties vertex_format{};
        vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R32G32B32_SFLOAT, &vertex_format);
        if (!(vertex_format.bufferFeatures & VK_FORMAT_FEATURE_ACCELERATION_STRUCTURE_VERTEX_BUFFER_BIT_KHR))
            throw std::runtime_error("RT requires acceleration-structure float3 vertex format support");
        properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
        VkPhysicalDeviceProperties2 device_properties{};
        device_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        device_properties.pNext = &properties;
        vkGetPhysicalDeviceProperties2(physical, &device_properties);
        max_triangle_bytes = device_properties.properties.limits.maxStorageBufferRange;
        VkCommandPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        info.queueFamilyIndex = family;
        checked(vkCreateCommandPool(device, &info, nullptr, &pool), "Creating RT command pool");
        VkCommandBufferAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocation.commandPool = pool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        const auto result = vkAllocateCommandBuffers(device, &allocation, &command);
        if (result != VK_SUCCESS) {
            vkDestroyCommandPool(device, pool, nullptr);
            pool = VK_NULL_HANDLE;
            checked(result, "Allocating RT build command");
        }
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        const auto fence_result = vkCreateFence(device, &fence_info, nullptr, &fence);
        if (fence_result != VK_SUCCESS) {
            vkDestroyCommandPool(device, pool, nullptr);
            pool = VK_NULL_HANDLE;
            checked(fence_result, "Creating RT build fence");
        }
    }
    void pulse() const {
        if (progress) progress();
    }
    void checkpoint(std::size_t index) const {
        if ((index & 1023u) == 0) pulse();
    }
    void drain() noexcept {
        if (!pending) return;
        // No hook during unwind: submitted builds can reference caller locals.
        // Device loss also ends the wait and permits resource destruction.
        for (;;) {
            const auto result = vkWaitForFences(device, 1, &fence, VK_TRUE,
                std::numeric_limits<std::uint64_t>::max());
            if (result == VK_SUCCESS || result == VK_ERROR_DEVICE_LOST) {
                pending = false;
                return;
            }
            // A timeout or allocation error does not retire submitted work.
        }
    }
    ~Impl() noexcept {
        drain();
        state.reset();
        if (fence) vkDestroyFence(device, fence, nullptr);
        if (pool) vkDestroyCommandPool(device, pool, nullptr);
    }
    void copy(void* destination, const void* source, std::size_t bytes) const {
        constexpr std::size_t chunk_bytes = 1024 * 1024;
        for (std::size_t offset = 0; offset < bytes;) {
            if (offset) pulse(); // callers pulse per item; only multi-MiB copies pump in between
            const auto count = std::min(chunk_bytes, bytes - offset);
            std::memcpy(static_cast<char*>(destination) + offset,
                static_cast<const char*>(source) + offset, count);
            offset += count;
        }
    }
    std::shared_ptr<Block> allocate_block(std::uint32_t type, VkDeviceSize size, bool host_visible) {
        auto block = std::make_shared<Block>();
        block->device = device;
        block->size = size;
        // Every RT buffer sharing a block may need a device address (bufferDeviceAddress is a required feature).
        VkMemoryAllocateFlagsInfo address_flags{};
        address_flags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
        address_flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.pNext = &address_flags;
        allocation.allocationSize = size;
        allocation.memoryTypeIndex = type;
        checked(vkAllocateMemory(device, &allocation, nullptr, &block->memory), "Allocating RT memory");
        if (host_visible) {
            void* mapped = nullptr;
            checked(vkMapMemory(device, block->memory, 0, VK_WHOLE_SIZE, 0, &mapped), "Mapping RT memory");
            block->mapped = static_cast<std::byte*>(mapped);
        }
        return block;
    }
    // `pool`: bind into its current block of the chosen memory type (blocks grow 1 MiB..256 MiB); otherwise a dedicated
    // allocation. Host-visible buffers stay mapped for their lifetime.
    Buffer allocate(VkDeviceSize size, VkBufferUsageFlags usage, const void* source = nullptr, bool host_visible = false,
                    Pool* shared = nullptr) {
        Buffer result;
        result.device = device;
        result.size = size;
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = size;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        checked(vkCreateBuffer(device, &info, nullptr, &result.handle), "Creating RT buffer");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, result.handle, &requirements);
        const bool host = source || host_visible;
        const VkMemoryPropertyFlags flags = host
            ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
            : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        std::uint32_t type = memory_properties.memoryTypeCount;
        for (std::uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i)
            if ((requirements.memoryTypeBits & (1u << i)) && (memory_properties.memoryTypes[i].propertyFlags & flags) == flags) {
                type = i;
                break;
            }
        if (type == memory_properties.memoryTypeCount) throw std::runtime_error("No suitable Vulkan RT memory type");
        if (shared) {
            const auto slot = type * 2 + host;
            // 256: acceleration-structure storage and scratch addresses, whatever the reported buffer alignment.
            auto& block = shared->blocks[slot];
            auto offset = aligned(shared->used[slot], std::max<VkDeviceSize>(requirements.alignment, 256));
            if (!block || offset > block->size || requirements.size > block->size - offset) {
                const auto capacity = std::max(requirements.size, shared->next[slot] ? shared->next[slot] : VkDeviceSize{1} << 20);
                shared->next[slot] = std::min(capacity * 2, VkDeviceSize{256} << 20);
                block = allocate_block(type, capacity, host);
                offset = 0;
            }
            shared->used[slot] = offset + requirements.size;
            result.block = block;
            result.offset = offset;
        } else {
            result.block = allocate_block(type, requirements.size, host);
        }
        checked(vkBindBufferMemory(device, result.handle, result.block->memory, result.offset), "Binding RT buffer memory");
        if (source) copy(result.mapped(), source, static_cast<std::size_t>(size));
        if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
            VkBufferDeviceAddressInfo address_info{};
            address_info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
            address_info.buffer = result.handle;
            result.address = buffer_address(device, &address_info);
            if (!result.address) throw std::runtime_error("RT buffer device address unavailable; enable bufferDeviceAddress");
        }
        return result;
    }
    // shared_scratch: the caller places the build scratch (stream()'s arena) and receives its size; the BLAS then owns
    // none until refresh() needs one for a rebuild.
    std::unique_ptr<Acceleration> prepare(VkAccelerationStructureBuildGeometryInfoKHR& info, std::uint32_t primitives,
                                          Pool* shared = nullptr, VkDeviceSize* shared_scratch = nullptr) {
        auto acceleration = std::make_unique<Acceleration>(device, destroy);
        VkAccelerationStructureBuildSizesInfoKHR size_info{};
        size_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        sizes(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &primitives, &size_info);
        acceleration->storage = allocate(size_info.accelerationStructureSize,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, nullptr, false, shared);
        const VkDeviceSize alignment = properties.minAccelerationStructureScratchOffsetAlignment;
        if (shared_scratch) *shared_scratch = aligned(std::max(size_info.buildScratchSize, VkDeviceSize{1}), alignment);
        else acceleration->scratch = allocate(std::max({size_info.buildScratchSize, size_info.updateScratchSize, VkDeviceSize{1}}) + alignment - 1,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, nullptr, false, shared);
        VkAccelerationStructureCreateInfoKHR creation{};
        creation.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        creation.buffer = acceleration->storage.handle;
        creation.size = size_info.accelerationStructureSize;
        creation.type = info.type;
        checked(create(device, &creation, nullptr, &acceleration->handle), "Creating acceleration structure");
        VkAccelerationStructureDeviceAddressInfoKHR address_info{};
        address_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
        address_info.accelerationStructure = acceleration->handle;
        acceleration->address = as_address(device, &address_info);
        if (!acceleration->address) throw std::runtime_error("Acceleration structure device address unavailable");
        info.dstAccelerationStructure = acceleration->handle;
        info.scratchData.deviceAddress = aligned(acceleration->scratch.address, alignment);
        return acceleration;
    }
    void upload(const Buffer& buffer, const void* source, VkDeviceSize bytes, VkDeviceSize offset = 0) {
        if (offset > buffer.size || bytes > buffer.size - offset || !buffer.mapped())
            throw std::runtime_error("RT upload exceeds retained host buffer");
        copy(buffer.mapped() + offset, source, static_cast<std::size_t>(bytes));
    }
    // Upper bound of TLAS instances a scene can place (one per placement, or per mesh in a placement-free preview).
    static std::size_t instance_bound(const Scene& scene) {
        return scene.instances.empty() ? scene.meshes.size() : scene.instances.size();
    }
    std::uint32_t instance_capacity(std::size_t bound) const {
        return static_cast<std::uint32_t>(std::min<std::size_t>(properties.maxInstanceCount, bound + bound / 2 + 64));
    }
    void bottom_input(BuildInput& input, const Mesh& mesh, const Acceleration& acceleration) {
        input.geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        input.geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR; // alpha-tested placements: FORCE_NO_OPAQUE (alpha_test)
        auto& triangles = input.geometry.geometry.triangles;
        triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        triangles.vertexStride = sizeof(Vertex);
        triangles.maxVertex = static_cast<std::uint32_t>(mesh.vertices.size() - 1);
        triangles.indexType = VK_INDEX_TYPE_UINT32;
        triangles.vertexData.deviceAddress = acceleration.vertices.address;
        triangles.indexData.deviceAddress = acceleration.indices.address;
        input.info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        input.info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        if (!mesh.influences.empty()) input.info.flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
        input.info.mode = mesh.influences.empty() ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR
                                               : VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
        input.info.geometryCount = 1;
        input.info.dstAccelerationStructure = acceleration.handle;
        if (!mesh.influences.empty()) input.info.srcAccelerationStructure = acceleration.handle;
        input.info.scratchData.deviceAddress = aligned(acceleration.scratch.address, properties.minAccelerationStructureScratchOffsetAlignment);
        input.range.primitiveCount = static_cast<std::uint32_t>(mesh.indices.size() / 3);
    }
    // topology=false: a skin refresh. Its immutable indices were range-checked against the same vertex count when the
    // BLAS was built; the caller checks the vertices its triangle records read.
    std::array<float, 4> validate_mesh(const Scene& scene, std::size_t m, bool topology = true) const {
        const auto& mesh = scene.meshes[m];
        const auto primitives = mesh.indices.size() / 3;
        if (mesh.indices.size() % 3 || (!mesh.indices.empty() && mesh.vertices.empty())
            || mesh.vertices.size() > std::numeric_limits<std::uint32_t>::max()
            || primitives > properties.maxPrimitiveCount
            || primitives > std::numeric_limits<std::uint32_t>::max()
            || primitives > max_triangle_bytes / sizeof(Triangle))
            throw std::runtime_error("Invalid or oversized RT triangle mesh");
        if (!topology && (m >= state->vertex_counts.size() || mesh.vertices.size() != state->vertex_counts[m]
                          || mesh.indices.size() != state->index_counts[m]))
            throw std::runtime_error("RT skin topology changed without rebuild");
        for (std::size_t i = 0; topology && i < mesh.vertices.size(); ++i) {
            checkpoint(i);
            const auto& vertex = mesh.vertices[i];
            if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y) || !std::isfinite(vertex.z))
                throw std::runtime_error("Non-finite RT mesh vertex");
        }
        for (std::size_t i = 0; topology && i < mesh.indices.size(); ++i) {
            checkpoint(i);
            if (mesh.indices[i] >= mesh.vertices.size())
                throw std::runtime_error("RT triangle index outside vertex buffer");
        }
        const auto color = mesh.material < scene.materials.size() ? scene.materials[mesh.material].color
            : std::array<float, 4>{1,1,1,1};
        for (float value : color)
            if (!std::isfinite(value)) throw std::runtime_error("Non-finite RT material color");
        return color;
    }
    // record == VK_NULL_HANDLE: host writes and a waited submit; the caller completed every frame using this scene.
    // Otherwise the host inputs are staged in `slot`'s arena and the copies/builds are recorded into the caller's frame
    // command buffer ahead of its ray queries. Recording returns false, changing nothing, when the placements outgrow
    // the TLAS capacity. `records` are appended triangle records written at `record_offset`.
    bool refresh(const Scene& scene, std::span<const RenderEntity> entities, std::span<const BuildInput> appended,
                 VkCommandBuffer record = VK_NULL_HANDLE, unsigned slot = 0, std::span<const Triangle> records = {},
                 std::size_t record_offset = 0) {
        auto& instances = state->cpu_instances;
        instances.clear();
        const std::size_t count = instance_bound(scene);
        if (count > properties.maxInstanceCount || count > std::numeric_limits<std::uint32_t>::max())
            throw std::runtime_error("Scene exceeds RT instance limit");
        const Transform identity;
        for (std::size_t i = 0; i < count; ++i) {
            checkpoint(i);
            const bool preview = scene.instances.empty();
            const std::size_t mesh = preview ? i : scene.instances[i].mesh;
            if (mesh >= state->mesh_blas.size()) throw std::runtime_error("Invalid retained RT mesh reference");
            const Transform& transform = preview ? identity : scene.instances[i].transform;
            validate_transform(transform);
            const bool color_draw = scene_source_color_draw(scene, static_cast<std::uint32_t>(mesh),
                preview ? UINT32_MAX : scene.instances[i].material,
                preview || scene.instances[i].source_ordinary_color_participates);
            if (!preview && (scene.instances[i].occlusion_proxy || scene.instances[i].sky)) continue;
            if (!color_draw || state->mesh_blas[mesh] == std::numeric_limits<std::size_t>::max()) continue;
            Transform resolved;
            const Transform* placed = &transform;
            if (!preview && scene.instances[i].entity_id != 0) {
                const auto entity_id = scene.instances[i].entity_id;
                const auto match = std::find_if(entities.begin(), entities.end(),
                    [entity_id](const RenderEntity& entity) { return entity.id == entity_id; });
                if (match == entities.end()) continue;
                resolved = entity_instance_transform(scene, scene.instances[i],
                    match->x, match->ground_y, match->z, match->yaw);
                validate_transform(resolved);
                placed = &resolved;
            }
            VkAccelerationStructureInstanceKHR instance{};
            for (std::size_t row = 0; row < 3; ++row)
                for (std::size_t column = 0; column < 4; ++column)
                    instance.transform.matrix[row][column] = placed->matrix[column*4 + row];
            instance.instanceCustomIndex = state->mesh_offset[mesh] & 0xffffffu;
            // Mask 2: actors (entity-attached); 1: static scenery. Faithful RT sun rays from lightmapped (static)
            // receivers test only actors, since the baked lightmap already holds the scenery's shadows.
            instance.mask = !preview && scene.instances[i].entity_id != 0 ? 0x02 : 0x01;
            // Source clockwise-front triangles are ray back faces by default; rt_lighting.glsl culls back faces.
            instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FLIP_FACING_BIT_KHR;
            alpha_test(instance, scene, preview || scene.instances[i].material == UINT32_MAX
                ? scene.meshes[mesh].material : scene.instances[i].material, state->alpha_textures, false);
            instance.accelerationStructureReference = state->bottom[state->mesh_blas[mesh]]->address;
            instances.push_back(instance);
        }
        const auto actual_count = static_cast<std::uint32_t>(instances.size());
        if (instances.empty()) instances.emplace_back();
        // Capacity follows the placement bound, not this frame's placed count, so record_refresh's bound check and this
        // growth agree: otherwise a grown bound that still fits would fail every in-frame update.
        const bool fits = std::max(count, instances.size()) <= state->instance_capacity;
        const bool same_count = fits && actual_count == state->instance_count && !state->rebuild_top;
        if (record && !fits) return false;
        const auto dirty = [&](std::size_t m) {
            return scene.meshes[m].revision != state->revisions[m]
                && state->mesh_blas[m] != std::numeric_limits<std::size_t>::max();
        };
        const VkDeviceSize instance_bytes = instances.size() * sizeof(instances[0]);
        const VkDeviceSize record_bytes = records.size() * sizeof(Triangle);
        std::byte* arena = nullptr;
        VkDeviceSize cursor = 0;
        staged.clear();
        if (record) {
            VkDeviceSize bytes = instance_bytes + record_bytes;
            for (std::size_t m = 0; m < scene.meshes.size(); ++m) {
                checkpoint(m);
                if (!dirty(m)) continue;
                const auto& acceleration = *state->bottom[state->mesh_blas[m]];
                bytes += (scene.meshes[m].influences.empty() ? acceleration.indices.size : 0) + acceleration.vertices.size
                    + scene.meshes[m].indices.size() / 3 * sizeof(Triangle);
            }
            auto& stage = arenas[slot & 1u];
            if (stage.size < bytes) {
                stage = {};
                stage = allocate(bytes + bytes / 2, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, nullptr, true);
            }
            arena = stage.mapped();
        }
        const auto put = [&](const Buffer& destination, VkDeviceSize offset, VkDeviceSize bytes) {
            staged.push_back({destination.handle, {cursor, offset, bytes}});
            cursor += bytes;
            return arena + cursor - bytes;
        };
        // Appended records land past every record a live instance references; recorded copies are additionally
        // ordered after in-flight frames, which may still read dead records being overwritten here.
        if (!records.empty()) {
            if (record) copy(put(state->triangles, record_offset * sizeof(Triangle), record_bytes), records.data(), record_bytes);
            else upload(state->triangles, records.data(), record_bytes, record_offset * sizeof(Triangle));
        }
        updates.clear();
        for (std::size_t i = 0; i < appended.size(); ++i) {
            checkpoint(i);
            updates.push_back(appended[i]);
        }
        for (std::size_t m = 0; m < scene.meshes.size(); ++m) {
            checkpoint(m);
            if (!dirty(m)) continue;
            const auto& mesh = scene.meshes[m];
            const bool skin = !mesh.influences.empty();
            const auto color = validate_mesh(scene, m, !skin);
            auto& acceleration = *state->bottom[state->mesh_blas[m]];
            const VkDeviceSize triangle_bytes = mesh.indices.size() / 3 * sizeof(Triangle);
            void* mapped = nullptr;
            // Skin revisions change vertices only; immutable topology stays on GPU.
            if (record) {
                if (mesh.influences.empty())
                    copy(put(acceleration.indices, 0, acceleration.indices.size), mesh.indices.data(), acceleration.indices.size);
                copy(put(acceleration.vertices, 0, acceleration.vertices.size), mesh.vertices.data(), acceleration.vertices.size);
                mapped = put(state->triangles, state->mesh_offset[m] * sizeof(Triangle), triangle_bytes);
            } else {
                if (mesh.influences.empty())
                    upload(acceleration.indices, mesh.indices.data(), acceleration.indices.size);
                upload(acceleration.vertices, mesh.vertices.data(), acceleration.vertices.size);
                mapped = state->triangles.mapped() + state->mesh_offset[m] * sizeof(Triangle);
            }
            auto* triangles = static_cast<Triangle*>(mapped);
            for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
                checkpoint(i / 3);
                const auto& a = mesh.vertices[mesh.indices[i]];
                const auto& b = mesh.vertices[mesh.indices[i+1]];
                const auto& c = mesh.vertices[mesh.indices[i+2]];
                if (skin && !std::isfinite(a.x + a.y + a.z + b.x + b.y + b.z + c.x + c.y + c.z))
                    throw std::runtime_error("Non-finite RT mesh vertex");
                triangles[i/3] = triangle_record(a, b, c, color);
            }
            updates.emplace_back();
            bottom_input(updates.back(), mesh, acceleration);
            if (!acceleration.scratch.handle) { // stream() BLAS built from the shared arena
                auto& info = updates.back().info;
                info.pGeometries = &updates.back().geometry;
                VkAccelerationStructureBuildSizesInfoKHR size_info{};
                size_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
                sizes(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &updates.back().range.primitiveCount, &size_info);
                const VkDeviceSize alignment = properties.minAccelerationStructureScratchOffsetAlignment;
                acceleration.scratch = allocate(std::max({size_info.buildScratchSize, size_info.updateScratchSize, VkDeviceSize{1}}) + alignment - 1,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
                info.scratchData.deviceAddress = aligned(acceleration.scratch.address, alignment);
            }
        }
        Buffer replacement_instances;
        std::uint32_t capacity = state->instance_capacity;
        if (record) {
            copy(put(state->instances, 0, instance_bytes), instances.data(), instance_bytes);
        } else if (fits) {
            upload(state->instances, instances.data(), instance_bytes);
        } else {
            capacity = std::max<std::uint32_t>(instance_capacity(count), static_cast<std::uint32_t>(instances.size()));
            replacement_instances = allocate(VkDeviceSize{capacity} * sizeof(instances[0]), input_usage, nullptr, true);
            upload(replacement_instances, instances.data(), instance_bytes);
        }
        updates.emplace_back();
        auto& top = updates.back();
        top.geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        top.geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        top.geometry.geometry.instances.data.deviceAddress = fits ? state->instances.address : replacement_instances.address;
        top.info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        top.info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
        top.info.mode = same_count ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        top.info.geometryCount = 1;
        top.info.pGeometries = &top.geometry;
        top.range.primitiveCount = actual_count;
        std::unique_ptr<Acceleration> replacement_top;
        if (fits) {
            // Sized for `instance_capacity` primitives: any smaller count rebuilds or updates this same handle.
            if (same_count) top.info.srcAccelerationStructure = state->top->handle;
            top.info.dstAccelerationStructure = state->top->handle;
            top.info.scratchData.deviceAddress = aligned(state->top->scratch.address, properties.minAccelerationStructureScratchOffsetAlignment);
        } else {
            replacement_top = prepare(top.info, capacity);
        }
        if (record) record_staged(record, arenas[slot & 1u].handle);
        else submit(updates);
        if (!fits) {
            state->top = std::move(replacement_top);
            state->instances = std::move(replacement_instances);
            state->instance_capacity = capacity;
        }
        state->instance_count = actual_count;
        state->rebuild_top = false;
        for (std::size_t m = 0; m < scene.meshes.size(); ++m) {
            checkpoint(m);
            state->revisions[m] = scene.meshes[m].revision;
        }
        return true;
    }
    void record_staged(VkCommandBuffer frame, VkBuffer source) {
        // In-flight frames' ray queries/triangle reads and builds finish before inputs, records and structures change.
        VkMemoryBarrier before{};
        before.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        before.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_TRANSFER_WRITE_BIT;
        before.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR
            | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        vkCmdPipelineBarrier(frame, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR
            | VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &before, 0, nullptr, 0, nullptr);
        for (const auto& [destination, region] : staged) vkCmdCopyBuffer(frame, source, destination, 1, &region);
        VkMemoryBarrier copied{};
        copied.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        copied.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        copied.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        vkCmdPipelineBarrier(frame, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &copied, 0, nullptr, 0, nullptr);
        record_builds(frame, updates);
    }
    void submit(std::vector<BuildInput>& inputs) {
        pulse();
        checked(vkResetCommandBuffer(command, 0), "Resetting RT build command");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        checked(vkBeginCommandBuffer(command, &begin), "Beginning RT build command");
        VkMemoryBarrier host{};
        host.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        host.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0, 1, &host, 0, nullptr, 0, nullptr);
        record_builds(command, inputs);
        checked(vkEndCommandBuffer(command), "Ending RT build command");
        VkSubmitInfo submission{};
        submission.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submission.commandBufferCount = 1;
        submission.pCommandBuffers = &command;
        checked(vkResetFences(device, 1, &fence), "Resetting RT build fence");
        checked(vkQueueSubmit(queue, 1, &submission, fence), "Submitting RT build");
        pending = true;
        try {
            for (;;) {
                pulse();
                const auto result = vkWaitForFences(device, 1, &fence, VK_TRUE, 50'000'000);
                if (result == VK_TIMEOUT) continue;
                checked(result, "Waiting for RT build");
                pending = false;
                break;
            }
        } catch (...) {
            drain();
            throw;
        }
    }
    void record_builds(VkCommandBuffer target, std::vector<BuildInput>& inputs) {
        // Every BLAS in one call (each has its own scratch): thousands of single-structure calls serialized the zone build.
        build_infos.clear();
        build_ranges.clear();
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            checkpoint(i);
            inputs[i].info.pGeometries = &inputs[i].geometry;
            if (i + 1 == inputs.size()) break;
            build_infos.push_back(inputs[i].info);
            build_ranges.push_back(&inputs[i].range);
        }
        if (!build_infos.empty())
            build(target, static_cast<std::uint32_t>(build_infos.size()), build_infos.data(), build_ranges.data());
        VkMemoryBarrier dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        dependency.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        dependency.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        vkCmdPipelineBarrier(target, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &dependency, 0, nullptr, 0, nullptr);
        const auto* top_range = &inputs.back().range;
        build(target, 1, &inputs.back().info, &top_range);
        dependency.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(target, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &dependency, 0, nullptr, 0, nullptr);
    }
    // Marks the meshes past `old_meshes` the scene references (and color-draws); returns their color-drawn triangles.
    std::size_t plan_append(const Scene& scene, std::size_t old_meshes, std::vector<std::uint8_t>& used) const {
        used.assign(scene.meshes.size() - old_meshes, scene.instances.empty() ? mesh_referenced : 0);
        if (scene.instances.empty()) {
            for (std::size_t m = old_meshes; m < scene.meshes.size(); ++m) {
                checkpoint(m);
                if (scene_source_color_draw(scene, static_cast<std::uint32_t>(m))) used[m - old_meshes] |= mesh_color_draw;
            }
        }
        for (std::size_t i = 0; i < scene.instances.size(); ++i) {
            checkpoint(i);
            const auto& instance = scene.instances[i];
            if (instance.mesh >= scene.meshes.size()) throw std::runtime_error("Invalid appended RT mesh reference");
            validate_transform(instance.transform);
            const bool color_draw = scene_source_color_draw(scene, instance.mesh, instance.material,
                instance.source_ordinary_color_participates);
            if (instance.occlusion_proxy || instance.sky) continue;
            if (instance.mesh >= old_meshes)
                used[instance.mesh - old_meshes] |= mesh_referenced | (color_draw ? mesh_color_draw : 0);
        }
        std::size_t drawable = 0;
        for (std::size_t m = old_meshes; m < scene.meshes.size(); ++m)
            if (used[m - old_meshes] & mesh_color_draw) drawable += scene.meshes[m].indices.size() / 3;
        return drawable;
    }
    // End of the triangle records any retained mesh references.
    std::size_t live_end() const {
        std::size_t end = 0;
        for (std::size_t m = 0; m < state->mesh_blas.size(); ++m) {
            checkpoint(m);
            if (state->mesh_blas[m] != std::numeric_limits<std::size_t>::max())
                end = std::max(end, static_cast<std::size_t>(state->mesh_offset[m]) + state->index_counts[m] / 3);
        }
        return end;
    }
    // record: as refresh(); the caller (retain) checked that the records fit the current triangle buffer.
    void append(const Scene& scene, std::span<const RenderEntity> entities, VkCommandBuffer record = VK_NULL_HANDLE,
                unsigned slot = 0) {
        const std::size_t old_meshes = state->mesh_blas.size();
        const std::size_t old_bottom = state->bottom.size();
        const std::size_t triangle_count = live_end();
        std::vector<std::uint8_t> used;
        const std::size_t drawable_triangles = plan_append(scene, old_meshes, used);
        std::size_t appended_triangles = 0;
        for (std::size_t m = old_meshes; m < scene.meshes.size(); ++m) {
            checkpoint(m);
            if (!used[m - old_meshes]) continue;
            const auto count = scene.meshes[m].indices.size() / 3;
            if (count > max_triangle_bytes / sizeof(Triangle)
                || triangle_count + appended_triangles > max_triangle_bytes / sizeof(Triangle) - count)
                throw std::runtime_error("Appended scene exceeds RT triangle buffer limit");
            appended_triangles += count;
        }
        std::vector<Triangle> records;
        records.reserve(drawable_triangles);
        std::vector<BuildInput> builds;
        builds.reserve(used.size());
        std::vector<std::unique_ptr<Acceleration>> bottom;
        bottom.reserve(used.size());
        std::vector<std::size_t> mapping(used.size(), std::numeric_limits<std::size_t>::max());
        std::vector<std::uint32_t> offsets(used.size());
        Pool shared;
        for (std::size_t m = old_meshes; m < scene.meshes.size(); ++m) {
            pulse();
            const auto& mesh = scene.meshes[m];
            if (!used[m - old_meshes]) continue;
            const auto color = validate_mesh(scene, m);
            if (!(used[m - old_meshes] & mesh_color_draw) || mesh.indices.empty()) continue;
            if (triangle_count + records.size() > 0xffffffu)
                throw std::runtime_error("Appended RT triangle offset exceeds instance custom index limit");
            offsets[m - old_meshes] = static_cast<std::uint32_t>(triangle_count + records.size());
            for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
                checkpoint(i / 3);
                const auto& a = mesh.vertices[mesh.indices[i]];
                const auto& b = mesh.vertices[mesh.indices[i+1]];
                const auto& c = mesh.vertices[mesh.indices[i+2]];
                records.push_back(triangle_record(a, b, c, color));
            }
            Acceleration inputs(device, destroy);
            inputs.vertices = allocate(mesh.vertices.size() * sizeof(Vertex), input_usage, mesh.vertices.data(), true, &shared);
            inputs.indices = allocate(mesh.indices.size() * sizeof(std::uint32_t), input_usage, mesh.indices.data(), true, &shared);
            builds.emplace_back();
            auto& build_input = builds.back();
            bottom_input(build_input, mesh, inputs);
            build_input.info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            build_input.info.srcAccelerationStructure = VK_NULL_HANDLE;
            build_input.info.pGeometries = &build_input.geometry;
            auto acceleration = prepare(build_input.info, build_input.range.primitiveCount, &shared);
            acceleration->vertices = std::move(inputs.vertices);
            acceleration->indices = std::move(inputs.indices);
            mapping[m - old_meshes] = old_bottom + bottom.size();
            bottom.push_back(std::move(acceleration));
        }
        // Capacity growth alone copies the static prefix. Existing triangle
        // offsets and BLAS/input/scratch allocations remain stable on append.
        const VkDeviceSize required = std::max<std::size_t>(triangle_count + records.size(), 1) * sizeof(Triangle);
        Buffer previous_triangles;
        if (required > state->triangles.size) {
            if (record) throw std::logic_error("Recorded RT append outgrew its checked triangle capacity");
            const VkDeviceSize capacity = std::min(max_triangle_bytes,
                std::max(required, state->triangles.size * 2));
            auto grown = allocate(capacity, triangle_usage, nullptr, true);
            copy(grown.mapped(), state->triangles.mapped(), triangle_count * sizeof(Triangle));
            previous_triangles = std::move(state->triangles);
            state->triangles = std::move(grown);
        }
        try {
            const auto reserve = [](auto& values, std::size_t required_count) {
                if (required_count > values.capacity())
                    values.reserve(std::max(required_count, values.capacity() * 2));
            };
            reserve(state->mesh_blas, scene.meshes.size());
            reserve(state->mesh_offset, scene.meshes.size());
            reserve(state->vertex_counts, scene.meshes.size());
            reserve(state->index_counts, scene.meshes.size());
            reserve(state->revisions, scene.meshes.size());
            reserve(state->keys, scene.meshes.size());
            reserve(state->bottom, old_bottom + bottom.size());
            reserve(updates, scene.meshes.size() + 1);
            for (std::size_t m = old_meshes; m < scene.meshes.size(); ++m) {
                checkpoint(m);
                state->mesh_blas.push_back(mapping[m - old_meshes]);
                state->mesh_offset.push_back(offsets[m - old_meshes]);
                state->vertex_counts.push_back(scene.meshes[m].vertices.size());
                state->index_counts.push_back(scene.meshes[m].indices.size());
                state->revisions.push_back(scene.meshes[m].revision);
                state->keys.push_back(scene.meshes[m].key);
            }
            for (auto& acceleration : bottom) state->bottom.push_back(std::move(acceleration));
            // New BLAS, their records, dirty existing skins and the TLAS share one submit or frame.
            refresh(scene, entities, builds, record, slot, records, triangle_count);
        } catch (...) {
            state->mesh_blas.resize(old_meshes);
            state->mesh_offset.resize(old_meshes);
            state->vertex_counts.resize(old_meshes);
            state->index_counts.resize(old_meshes);
            state->revisions.resize(old_meshes);
            state->keys.resize(old_meshes);
            state->bottom.resize(old_bottom);
            if (previous_triangles.handle) state->triangles = std::move(previous_triangles);
            throw;
        }
    }
    // Twice the live records, as append growth would double anyway: actor churn appends without copying the zone prefix.
    VkDeviceSize triangle_capacity(std::size_t records) const {
        return std::min(max_triangle_bytes, static_cast<VkDeviceSize>(std::max<std::size_t>(records, 1)) * sizeof(Triangle) * 2);
    }
    // Actor attach/detach mirror: keeps retained meshes' BLAS and triangle records (moved by key) and moves dropped BLAS
    // into `retired`; the caller frees them after the TLAS that referenced them is rebuilt. False: rebuild everything.
    // Dropped triangle records stay dead in place until they outnumber live ones; then live records are packed.
    // record: also false, with the state untouched, when that packing or appending past the triangle buffer's
    // capacity would replace the buffer in-flight frames bind.
    bool retain(const Scene& scene, std::vector<std::unique_ptr<Acceleration>>& retired, bool record = false) {
        constexpr auto none = std::numeric_limits<std::size_t>::max();
        auto& s = *state;
        const auto remap = scene_mesh_remap(s.keys, scene);
        std::size_t kept = 0;
        while (kept < remap.size() && remap[kept] != none) ++kept;
        for (std::size_t m = kept; m < remap.size(); ++m)
            if (remap[m] != none) return false; // New meshes only ever follow retained ones; anything else is not an append.
        bool identity = kept == s.mesh_blas.size();
        for (std::size_t m = 0; identity && m < kept; ++m) identity = remap[m] == m;
        const bool grows = kept < scene.meshes.size();
        if (identity && !(record && grows)) return true;
        std::size_t live = 0, end = 0;
        for (std::size_t m = 0; m < kept; ++m) {
            checkpoint(m);
            const auto old = remap[m];
            if (s.mesh_blas[old] == none) continue;
            live += s.index_counts[old] / 3;
            end = std::max(end, static_cast<std::size_t>(s.mesh_offset[old]) + s.index_counts[old] / 3);
        }
        const bool pack = !identity && end - live > live;
        if (record) {
            std::vector<std::uint8_t> used;
            const auto appended = grows ? plan_append(scene, kept, used) : 0;
            if (pack || end + appended > s.triangles.size / sizeof(Triangle)) return false;
        }
        if (identity) return true;
        std::vector<std::size_t> mesh_blas(kept, none), vertex_counts(kept), index_counts(kept);
        std::vector<std::uint32_t> mesh_offset(kept);
        std::vector<std::uint64_t> revisions(kept), keys(kept);
        for (std::size_t m = 0; m < kept; ++m) mesh_offset[m] = s.mesh_offset[remap[m]];
        Buffer packed;
        if (pack) {
            packed = allocate(triangle_capacity(live), triangle_usage, nullptr, true);
            std::size_t cursor = 0;
            for (std::size_t m = 0; m < kept; ++m) {
                const auto old = remap[m];
                if (s.mesh_blas[old] == none) continue;
                const auto count = s.index_counts[old] / 3;
                copy(packed.mapped() + cursor * sizeof(Triangle), s.triangles.mapped() + s.mesh_offset[old] * sizeof(Triangle),
                    count * sizeof(Triangle));
                mesh_offset[m] = static_cast<std::uint32_t>(cursor);
                cursor += count;
            }
        }
        std::vector<std::unique_ptr<Acceleration>> bottom;
        bottom.reserve(s.bottom.size());
        retired.reserve(s.bottom.size());
        // Allocation done: the moves below cannot throw, so state never holds a half-permuted mapping.
        for (std::size_t m = 0; m < kept; ++m) {
            const auto old = remap[m];
            vertex_counts[m] = s.vertex_counts[old];
            index_counts[m] = s.index_counts[old];
            revisions[m] = s.revisions[old];
            keys[m] = s.keys[old];
            if (s.mesh_blas[old] == none) continue;
            mesh_blas[m] = bottom.size();
            bottom.push_back(std::move(s.bottom[s.mesh_blas[old]]));
        }
        if (packed.handle) s.triangles = std::move(packed);
        for (auto& acceleration : s.bottom)
            if (acceleration) retired.push_back(std::move(acceleration));
        s.bottom = std::move(bottom);
        s.mesh_blas = std::move(mesh_blas);
        s.vertex_counts = std::move(vertex_counts);
        s.index_counts = std::move(index_counts);
        s.mesh_offset = std::move(mesh_offset);
        s.revisions = std::move(revisions);
        s.keys = std::move(keys);
        s.rebuild_top = true;
        return true;
    }
    // Retained meshes keep their vertex/index counts and no placement activates a dormant (unbuilt) slot.
    bool same_topology(const Scene& scene) const {
        bool same = true;
        for (std::size_t m = 0; m < state->mesh_blas.size(); ++m) {
            checkpoint(m);
            same &= state->vertex_counts[m] == scene.meshes[m].vertices.size()
                && state->index_counts[m] == scene.meshes[m].indices.size();
        }
        if (!same) return false;
        const auto check_dormant = [&](std::uint32_t mesh, std::uint32_t material, bool ordinary_participates = true) {
            const bool color_draw = scene_source_color_draw(scene, mesh, material, ordinary_participates);
            if (mesh < state->mesh_blas.size()
                && state->mesh_blas[mesh] == std::numeric_limits<std::size_t>::max()) {
                validate_mesh(scene, mesh);
                // A new placement/override can activate an old unbuilt slot without mesh-vector growth.
                if (color_draw && !scene.meshes[mesh].indices.empty()) same = false;
            }
        };
        if (scene.instances.empty()) {
            for (std::size_t m = 0; m < state->mesh_blas.size(); ++m) {
                checkpoint(m);
                check_dormant(static_cast<std::uint32_t>(m), UINT32_MAX);
            }
        } else {
            for (std::size_t i = 0; i < scene.instances.size(); ++i) {
                checkpoint(i);
                const auto& instance = scene.instances[i];
                validate_transform(instance.transform);
                if (instance.occlusion_proxy || instance.sky) continue;
                check_dormant(instance.mesh, instance.material, instance.source_ordinary_color_participates);
            }
        }
        return same;
    }
    void release(std::vector<std::unique_ptr<Acceleration>>& retired) {
        for (auto& acceleration : retired) {
            pulse();
            acceleration.reset();
        }
        retired.clear();
    }
    bool record_refresh(const Scene& scene, std::span<const RenderEntity> entities, VkCommandBuffer frame, unsigned slot) {
        pulse();
        // The caller completed this slot's previous frame and with it every earlier one: no frame still traces the
        // BLAS dropped while recording it.
        auto& retired = retired_slots[slot & 1u];
        release(retired);
        if (!frame || !state || state->source != &scene || instance_bound(scene) > state->instance_capacity) return false;
        // Attach/detach in place: the departed BLAS stay alive in `retired` while earlier frames may trace them; on a
        // false return after retain() the caller waits for every frame and rebuild() starts over.
        if (!retain(scene, retired, true) || !same_topology(scene)) return false;
        if (state->mesh_blas.size() < scene.meshes.size()) append(scene, entities, frame, slot);
        else if (!refresh(scene, entities, {}, frame, slot)) return false;
        return true;
    }
    // NONCANONICAL wave-16 background build: see VulkanRtScene::stream. Only the unkeyed zone prefix is streamed (actor
    // attach/detach never moves it); keyed actor meshes are appended in-frame by the first record_refresh.
    struct Stream {
        std::unique_ptr<State> next;
        std::vector<std::uint8_t> used;
        std::size_t prefix = 0, cursor = 0, records = 0;
        Pool pool;
    };
    std::unique_ptr<Stream> streaming;
    Buffer scratch_arena;                 // shared build scratch of the static BLAS of one stream() chunk
    std::array<Buffer, 2> retired_arenas; // outgrown arena, freed once the caller completed that slot's next frame
    bool stream(const Scene& scene, VkCommandBuffer frame, unsigned slot, VkDeviceSize budget) {
        pulse();
        retired_arenas[slot & 1u] = {};
        if (state) throw std::logic_error("RT stream requires a fresh scene");
        constexpr auto none = std::numeric_limits<std::size_t>::max();
        if (!streaming) {
            auto s = std::make_unique<Stream>();
            while (s->prefix < scene.meshes.size() && !scene.meshes[s->prefix].key) ++s->prefix;
            s->used.assign(s->prefix, scene.instances.empty() ? mesh_referenced : 0);
            if (instance_bound(scene) > properties.maxInstanceCount || instance_bound(scene) > std::numeric_limits<std::uint32_t>::max())
                throw std::runtime_error("Scene exceeds RT instance limit");
            if (scene.instances.empty())
                for (std::size_t m = 0; m < s->prefix; ++m)
                    if (scene_source_color_draw(scene, static_cast<std::uint32_t>(m))) s->used[m] |= mesh_color_draw;
            for (const auto& instance : scene.instances) {
                if (instance.mesh >= scene.meshes.size()) throw std::runtime_error("Invalid RT scene mesh reference");
                const bool color_draw = scene_source_color_draw(scene, instance.mesh, instance.material,
                    instance.source_ordinary_color_participates);
                if (instance.occlusion_proxy || instance.sky || instance.mesh >= s->prefix) continue;
                s->used[instance.mesh] |= mesh_referenced | (color_draw ? mesh_color_draw : 0);
            }
            std::size_t drawable = 0;
            for (std::size_t m = 0; m < s->prefix; ++m)
                if (s->used[m] & mesh_color_draw) drawable += scene.meshes[m].indices.size() / 3;
            if (drawable > max_triangle_bytes / sizeof(Triangle) || drawable > 0xffffffu)
                throw std::runtime_error("Scene exceeds RT triangle buffer or instance custom index limits");
            s->next = std::make_unique<State>();
            s->next->mesh_blas.assign(s->prefix, none);
            s->next->mesh_offset.assign(s->prefix, 0);
            s->next->triangles = allocate(triangle_capacity(drawable), triangle_usage, nullptr, true);
            streaming = std::move(s);
        }
        auto& s = *streaming;
        auto& next = *s.next;
        std::vector<BuildInput> builds;
        std::vector<VkDeviceSize> scratch; // per build; 0 = owns its scratch (skins update in place)
        VkDeviceSize bytes = 0;
        while (s.cursor < s.prefix && bytes < budget) {
            const auto m = s.cursor++;
            if (!s.used[m]) continue;
            const auto& mesh = scene.meshes[m];
            const auto color = validate_mesh(scene, m);
            if (!(s.used[m] & mesh_color_draw) || mesh.indices.empty()) continue;
            next.mesh_offset[m] = static_cast<std::uint32_t>(s.records);
            auto* records = reinterpret_cast<Triangle*>(next.triangles.mapped()) + s.records;
            for (std::size_t i = 0; i < mesh.indices.size(); i += 3)
                records[i/3] = triangle_record(mesh.vertices[mesh.indices[i]], mesh.vertices[mesh.indices[i+1]],
                    mesh.vertices[mesh.indices[i+2]], color);
            s.records += mesh.indices.size() / 3;
            Acceleration inputs(device, destroy);
            inputs.vertices = allocate(mesh.vertices.size() * sizeof(Vertex), input_usage, mesh.vertices.data(), true, &s.pool);
            inputs.indices = allocate(mesh.indices.size() * sizeof(std::uint32_t), input_usage, mesh.indices.data(), true, &s.pool);
            builds.emplace_back();
            auto& input = builds.back();
            bottom_input(input, mesh, inputs);
            input.info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            input.info.srcAccelerationStructure = VK_NULL_HANDLE;
            input.info.pGeometries = &input.geometry;
            auto& size = scratch.emplace_back(0);
            auto acceleration = prepare(input.info, input.range.primitiveCount, &s.pool, mesh.influences.empty() ? &size : nullptr);
            acceleration->vertices = std::move(inputs.vertices);
            acceleration->indices = std::move(inputs.indices);
            next.mesh_blas[m] = next.bottom.size();
            next.bottom.push_back(std::move(acceleration));
            bytes += mesh.vertices.size() * sizeof(Vertex) + mesh.indices.size() * (sizeof(std::uint32_t) + sizeof(Triangle) / 3);
        }
        VkDeviceSize total = 0;
        for (auto size : scratch) total += size;
        const VkDeviceSize alignment = properties.minAccelerationStructureScratchOffsetAlignment;
        if (total && total + alignment > scratch_arena.size) {
            const auto grown = std::max(total, scratch_arena.size * 2) + alignment;
            retired_arenas[slot & 1u] = std::move(scratch_arena); // earlier frames' chunk builds may still use it
            scratch_arena = allocate(grown, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
        }
        VkDeviceAddress cursor = aligned(scratch_arena.address, alignment);
        build_infos.clear();
        build_ranges.clear();
        for (std::size_t i = 0; i < builds.size(); ++i) {
            if (scratch[i]) {builds[i].info.scratchData.deviceAddress = cursor; cursor += scratch[i];}
            builds[i].info.pGeometries = &builds[i].geometry;
            build_infos.push_back(builds[i].info);
            build_ranges.push_back(&builds[i].range);
        }
        VkMemoryBarrier dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        // The previous chunk's builds (same arena) finish before this chunk reuses its scratch.
        dependency.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        dependency.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        vkCmdPipelineBarrier(frame, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &dependency, 0, nullptr, 0, nullptr);
        if (!build_infos.empty())
            build(frame, static_cast<std::uint32_t>(build_infos.size()), build_infos.data(), build_ranges.data());
        if (s.cursor < s.prefix) return false;
        // Last chunk: the TLAS over the zone placements (actors are placed by the first record_refresh).
        auto& instances = next.cpu_instances;
        for (const auto& placement : scene.instances) {
            if (placement.occlusion_proxy || placement.sky || placement.entity_id || placement.mesh >= s.prefix
                || next.mesh_blas[placement.mesh] == none
                || !scene_source_color_draw(scene, placement.mesh, placement.material, placement.source_ordinary_color_participates))
                continue;
            validate_transform(placement.transform);
            VkAccelerationStructureInstanceKHR instance{};
            for (std::size_t row = 0; row < 3; ++row)
                for (std::size_t column = 0; column < 4; ++column)
                    instance.transform.matrix[row][column] = placement.transform.matrix[column*4 + row];
            instance.instanceCustomIndex = next.mesh_offset[placement.mesh] & 0xffffffu;
            instance.mask = 0x01; // see the refresh path
            instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FLIP_FACING_BIT_KHR;
            alpha_test(instance, scene, placement.material == UINT32_MAX ? scene.meshes[placement.mesh].material : placement.material,
                next.alpha_textures, true);
            instance.accelerationStructureReference = next.bottom[next.mesh_blas[placement.mesh]]->address;
            instances.push_back(instance);
        }
        if (scene.instances.empty())
            for (std::size_t m = 0; m < s.prefix; ++m) {
                if (next.mesh_blas[m] == none) continue;
                VkAccelerationStructureInstanceKHR instance{};
                instance.transform.matrix[0][0] = instance.transform.matrix[1][1] = instance.transform.matrix[2][2] = 1;
                instance.instanceCustomIndex = next.mesh_offset[m] & 0xffffffu;
                instance.mask = 0x01;
                instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FLIP_FACING_BIT_KHR;
                alpha_test(instance, scene, scene.meshes[m].material, next.alpha_textures, true);
                instance.accelerationStructureReference = next.bottom[next.mesh_blas[m]]->address;
                instances.push_back(instance);
            }
        next.instance_count = static_cast<std::uint32_t>(instances.size());
        if (instances.empty()) instances.emplace_back();
        next.instance_capacity = instance_capacity(instance_bound(scene));
        next.instances = allocate(VkDeviceSize{next.instance_capacity} * sizeof(VkAccelerationStructureInstanceKHR), input_usage, nullptr, true);
        upload(next.instances, instances.data(), instances.size() * sizeof(VkAccelerationStructureInstanceKHR));
        BuildInput top;
        top.geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        top.geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        top.geometry.geometry.instances.data.deviceAddress = next.instances.address;
        top.info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        top.info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
        top.info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        top.info.geometryCount = 1;
        top.info.pGeometries = &top.geometry;
        top.range.primitiveCount = next.instance_count;
        next.top = prepare(top.info, next.instance_capacity);
        top.info.pGeometries = &top.geometry;
        dependency.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR; // every chunk's BLAS, then the TLAS
        vkCmdPipelineBarrier(frame, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &dependency, 0, nullptr, 0, nullptr);
        const auto* top_range = &top.range;
        build(frame, 1, &top.info, &top_range);
        dependency.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(frame, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &dependency, 0, nullptr, 0, nullptr);
        next.source = &scene;
        for (std::size_t m = 0; m < s.prefix; ++m) {
            next.vertex_counts.push_back(scene.meshes[m].vertices.size());
            next.index_counts.push_back(scene.meshes[m].indices.size());
            next.revisions.push_back(scene.meshes[m].revision);
            next.keys.push_back(0);
        }
        updates.reserve(scene.meshes.size() + 1);
        state = std::move(s.next);
        streaming.reset();
        return true;
    }
    void rebuild(const Scene& scene, std::span<const RenderEntity> entities) {
        pulse();
        std::vector<std::unique_ptr<Acceleration>> retired;
        if (state && state->source == &scene && retain(scene, retired)) {
            if (same_topology(scene)) {
                if (state->mesh_blas.size() < scene.meshes.size()) append(scene, entities);
                else refresh(scene, entities, {});
                // The rebuilt TLAS no longer references the departed actors' BLAS.
                release(retired);
                for (auto& slot : retired_slots) release(slot);
                return;
            }
        }
        auto next = std::make_unique<State>();
        const std::size_t instance_count = scene.instances.empty() ? scene.meshes.size() : scene.instances.size();
        if (instance_count > properties.maxInstanceCount || instance_count > std::numeric_limits<std::uint32_t>::max())
            throw std::runtime_error("Scene exceeds RT instance limit");
        std::vector<std::uint8_t> used(scene.meshes.size(), scene.instances.empty() ? mesh_referenced : 0);
        // Replacement RT visibility excludes no-color subsets; this is not original shadow parity proof.
        if (scene.instances.empty()) {
            for (std::size_t m = 0; m < scene.meshes.size(); ++m) {
                checkpoint(m);
                if (scene_source_color_draw(scene, static_cast<std::uint32_t>(m))) used[m] |= mesh_color_draw;
            }
        }
        for (std::size_t i = 0; i < scene.instances.size(); ++i) {
            checkpoint(i);
            const auto& instance = scene.instances[i];
            if (instance.mesh >= used.size()) throw std::runtime_error("Invalid RT scene mesh reference");
            validate_transform(instance.transform);
            const bool color_draw = scene_source_color_draw(scene, instance.mesh, instance.material,
                instance.source_ordinary_color_participates);
            if (instance.occlusion_proxy || instance.sky) continue;
            used[instance.mesh] |= mesh_referenced | (color_draw ? mesh_color_draw : 0);
        }
        std::vector<BuildInput> inputs;
        inputs.reserve(scene.meshes.size() + 1);
        std::vector<std::size_t> mesh_blas(scene.meshes.size(), std::numeric_limits<std::size_t>::max());
        std::vector<std::uint32_t> mesh_offset(scene.meshes.size());
        std::vector<Triangle> triangle_data;
        std::size_t triangle_count = 0;
        std::size_t drawable_triangles = 0;
        for (std::size_t m = 0; m < scene.meshes.size(); ++m) {
            checkpoint(m);
            if (!used[m]) continue;
            const std::size_t count = scene.meshes[m].indices.size() / 3;
            if (count > max_triangle_bytes / sizeof(Triangle)
                || triangle_count > max_triangle_bytes / sizeof(Triangle) - count)
                throw std::runtime_error("Scene exceeds RT triangle buffer limit");
            triangle_count += count;
            if (used[m] & mesh_color_draw) drawable_triangles += count;
        }
        triangle_data.reserve(std::max<std::size_t>(drawable_triangles, 1));
        Pool shared;
        for (std::size_t m = 0; m < scene.meshes.size(); ++m) {
            pulse();
            if (!used[m]) continue;
            const auto& mesh = scene.meshes[m];
            const auto color = validate_mesh(scene, m);
            if (!(used[m] & mesh_color_draw) || mesh.indices.empty()) continue;
            const std::size_t primitives = mesh.indices.size() / 3;
            if (triangle_data.size() > 0xffffffu || triangle_data.size() > max_triangle_bytes / sizeof(Triangle) - primitives)
                throw std::runtime_error("Scene exceeds RT triangle buffer or instance custom index limits");
            mesh_offset[m] = static_cast<std::uint32_t>(triangle_data.size());
            for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
                checkpoint(i / 3);
                const auto& a = mesh.vertices[mesh.indices[i]];
                const auto& b = mesh.vertices[mesh.indices[i+1]];
                const auto& c = mesh.vertices[mesh.indices[i+2]];
                triangle_data.push_back(triangle_record(a, b, c, color));
            }
            inputs.emplace_back();
            auto& input = inputs.back();
            input.geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            input.geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR; // alpha-tested placements: FORCE_NO_OPAQUE (alpha_test)
            input.geometry.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
            auto& triangles = input.geometry.geometry.triangles;
            triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
            triangles.vertexStride = sizeof(Vertex);
            triangles.maxVertex = static_cast<std::uint32_t>(mesh.vertices.size() - 1);
            triangles.indexType = VK_INDEX_TYPE_UINT32;
            input.info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            input.info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
            if (!mesh.influences.empty()) input.info.flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
            input.info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            input.info.geometryCount = 1;
            input.info.pGeometries = &input.geometry;
            input.range.primitiveCount = static_cast<std::uint32_t>(primitives);
            auto vertex_buffer = allocate(mesh.vertices.size() * sizeof(Vertex), input_usage, mesh.vertices.data(), true, &shared);
            auto index_buffer = allocate(mesh.indices.size() * sizeof(std::uint32_t), input_usage, mesh.indices.data(), true, &shared);
            triangles.vertexData.deviceAddress = vertex_buffer.address;
            triangles.indexData.deviceAddress = index_buffer.address;
            auto acceleration = prepare(input.info, input.range.primitiveCount, &shared);
            acceleration->vertices = std::move(vertex_buffer);
            acceleration->indices = std::move(index_buffer);
            mesh_blas[m] = next->bottom.size();
            next->bottom.push_back(std::move(acceleration));
        }
        // A nonzero descriptor range for an empty scene; this record is never hit.
        if (triangle_data.empty()) triangle_data.emplace_back();
        next->triangles = allocate(triangle_capacity(triangle_data.size()), triangle_usage, nullptr, true);
        upload(next->triangles, triangle_data.data(), triangle_data.size() * sizeof(Triangle));
        std::vector<VkAccelerationStructureInstanceKHR> instances;
        instances.reserve(instance_count);
        const Transform identity;
        for (std::size_t i = 0; i < instance_count; ++i) {
            checkpoint(i);
            // Standalone mesh preview uses identity only when no placements exist.
            const bool preview = scene.instances.empty();
            const std::size_t mesh = preview ? i : scene.instances[i].mesh;
            const Transform& transform = preview ? identity : scene.instances[i].transform;
            validate_transform(transform);
            const bool color_draw = scene_source_color_draw(scene, static_cast<std::uint32_t>(mesh),
                preview ? UINT32_MAX : scene.instances[i].material,
                preview || scene.instances[i].source_ordinary_color_participates);
            if (!preview && (scene.instances[i].occlusion_proxy || scene.instances[i].sky)) continue;
            if (!color_draw || mesh_blas[mesh] == std::numeric_limits<std::size_t>::max()) continue;
            Transform resolved;
            const Transform* placed = &transform;
            if (!preview && scene.instances[i].entity_id != 0) {
                const auto entity_id = scene.instances[i].entity_id;
                const auto match = std::find_if(entities.begin(), entities.end(),
                    [entity_id](const RenderEntity& entity) { return entity.id == entity_id; });
                if (match == entities.end()) continue;
                resolved = entity_instance_transform(scene, scene.instances[i],
                    match->x, match->ground_y, match->z, match->yaw);
                validate_transform(resolved);
                placed = &resolved;
            }
            VkAccelerationStructureInstanceKHR instance{};
            for (std::size_t row = 0; row < 3; ++row)
                for (std::size_t column = 0; column < 4; ++column)
                    instance.transform.matrix[row][column] = placed->matrix[column*4 + row];
            instance.instanceCustomIndex = mesh_offset[mesh] & 0xffffffu;
            instance.mask = !preview && scene.instances[i].entity_id != 0 ? 0x02 : 0x01; // see the refresh path
            instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FLIP_FACING_BIT_KHR;
            alpha_test(instance, scene, preview || scene.instances[i].material == UINT32_MAX
                ? scene.meshes[mesh].material : scene.instances[i].material, next->alpha_textures, true);
            instance.accelerationStructureReference = next->bottom[mesh_blas[mesh]]->address;
            instances.push_back(instance);
        }
        const std::uint32_t actual_instances = static_cast<std::uint32_t>(instances.size());
        if (instances.empty()) instances.emplace_back();
        const auto capacity = instance_capacity(instance_count);
        next->instances = allocate(VkDeviceSize{capacity} * sizeof(VkAccelerationStructureInstanceKHR), input_usage, nullptr, true);
        upload(next->instances, instances.data(), instances.size() * sizeof(VkAccelerationStructureInstanceKHR));
        inputs.emplace_back();
        auto& top = inputs.back();
        top.geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        top.geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        top.geometry.geometry.instances.data.deviceAddress = next->instances.address;
        top.info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        top.info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
        top.info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        top.info.geometryCount = 1;
        top.info.pGeometries = &top.geometry;
        top.range.primitiveCount = actual_instances;
        next->top = prepare(top.info, capacity);
        next->instance_capacity = capacity;
        submit(inputs);
        next->source = &scene;
        next->mesh_blas = std::move(mesh_blas);
        next->mesh_offset = std::move(mesh_offset);
        next->instance_count = actual_instances;
        next->cpu_instances = std::move(instances);
        next->vertex_counts.reserve(scene.meshes.size());
        next->index_counts.reserve(scene.meshes.size());
        next->revisions.reserve(scene.meshes.size());
        next->keys.reserve(scene.meshes.size());
        for (std::size_t m = 0; m < scene.meshes.size(); ++m) {
            checkpoint(m);
            const auto& mesh = scene.meshes[m];
            next->vertex_counts.push_back(mesh.vertices.size());
            next->index_counts.push_back(mesh.indices.size());
            next->revisions.push_back(mesh.revision);
            next->keys.push_back(mesh.key);
        }
        updates.reserve(scene.meshes.size() + 1);
        // Install before pulsed retirement so throwing progress leaves the completed state owned.
        auto previous = std::move(state);
        state = std::move(next);
        if (previous) {
            pulse();
            previous->top.reset();
            for (auto& acceleration : previous->bottom) {
                pulse();
                acceleration.reset();
            }
            pulse();
            previous->instances = {};
            pulse();
            previous->triangles = {};
            pulse();
        }
        release(retired);
        for (auto& slot : retired_slots) release(slot);
    }
};

VulkanRtScene::VulkanRtScene(VkPhysicalDevice physical, VkDevice device, VkQueue queue, std::uint32_t family,
    std::function<void()> progress)
    : impl_(std::make_unique<Impl>(physical, device, queue, family, std::move(progress))) {}
VulkanRtScene::~VulkanRtScene() noexcept = default;
void VulkanRtScene::rebuild(const Scene& scene, std::span<const RenderEntity> entities) { impl_->rebuild(scene, entities); }
bool VulkanRtScene::stream(const Scene& scene, VkCommandBuffer frame, unsigned slot, VkDeviceSize budget) {
    return impl_->stream(scene, frame, slot, budget);
}
bool VulkanRtScene::record_refresh(const Scene& scene, std::span<const RenderEntity> entities, VkCommandBuffer frame,
    unsigned slot) {
    return impl_->record_refresh(scene, entities, frame, slot);
}
VkAccelerationStructureKHR VulkanRtScene::acceleration_structure() const noexcept {
    return impl_->state ? impl_->state->top->handle : VK_NULL_HANDLE;
}
VkBuffer VulkanRtScene::triangles() const noexcept {
    return impl_->state ? impl_->state->triangles.handle : VK_NULL_HANDLE;
}
VkDeviceSize VulkanRtScene::triangle_bytes() const noexcept {
    return impl_->state ? impl_->state->triangles.size : 0;
}
std::span<const std::string> VulkanRtScene::alpha_textures() const noexcept {
    return impl_->state ? std::span<const std::string>(impl_->state->alpha_textures) : std::span<const std::string>();
}
} // namespace ikv
