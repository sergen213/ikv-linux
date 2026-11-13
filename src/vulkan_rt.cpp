#include "ikv/vulkan_rt.hpp"
#include "ikv/scene.hpp"
#include "ikv/renderer.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
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
        // scratch: kept only by structures built again or updated later (skins: update size; TLAS: build/update size).
        // build_scratch: the initial build's transient scratch, dropped once that build completed.
        Buffer vertices, indices, storage, scratch, build_scratch;
        VkDeviceSize build_scratch_size = 0;
        // Skins: the half2 UV bits of each triangle's three corners (triangle_record's .w), fixed for the BLAS's life,
        // so per-frame record rewrites only gather positions.
        std::vector<float> corner_uvs;
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
        std::vector<VkAccelerationStructureInstanceKHR> cpu_instances, previous_instances;
        std::uint32_t instance_count = 0;
        // TLAS and instance buffer are sized for this many instances, so placement-count changes (actor attach/detach,
        // entities entering/leaving) rebuild the same TLAS handle inside a frame instead of replacing it.
        std::uint32_t instance_capacity = 0;
        // Per scene instance, what the last full placement pass saw (placements) and wrote (sources): later refreshes
        // only compare these and rewrite the cpu_instances slots whose transform or entity placement moved.
        // `placed` is false after rebuild, retain, append or an aborted refresh: the next refresh re-places everything.
        struct Placement {
            std::uint32_t mesh = 0, material = 0, entity_id = 0;
            std::uint32_t slot = UINT32_MAX; // cpu_instances index; UINT32_MAX: not in the TLAS
            std::uint32_t entity_hint = 0;   // index into the last entities span holding entity_id
            std::uint8_t bits = 0;           // placement_bits(); placement_absent: entity-bound, entity not present
        };
        std::vector<Placement> placements;
        std::vector<Transform> sources;                // scene instance transform last placed
        std::vector<std::array<float, 4>> entity_at;  // entity x, ground_y, z, yaw last placed
        bool placed = false;
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
    // Device-local, host-visible, coherent memory in a large (resizable BAR) heap, or UINT32_MAX. Buffers the GPU reads
    // every frame (triangle records hit by rays, TLAS instances, skin BLAS inputs) live there instead of system memory;
    // the CPU only writes them, and growth/packing copies them on the GPU (copy_on_device).
    std::uint32_t rebar_type = UINT32_MAX;
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
    // referencing them; they are freed once the caller completed that frame (and with it every earlier one). Holders
    // without a handle carry the transient build scratch of builds recorded into that frame.
    std::array<std::vector<std::unique_ptr<Acceleration>>, 2> retired_slots;
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> build_infos;
    std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> build_ranges;
    std::vector<std::pair<VkBuffer, VkBufferCopy>> staged;
    std::vector<std::size_t> dirty_meshes;
    std::vector<std::uint32_t> dirty_slots;
    std::vector<Buffer> transient_scratch; // build scratch of static BLAS rebuilt by one refresh
    // (entity id, index) of the entities span of the refresh in progress, sorted; built on the first hint miss.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> entity_index;
    bool entity_indexed = false;
    // IKV_GPU_TIMING: per caller frame slot, timestamps before the staged copies, after them, after the BLAS builds and
    // after the TLAS build, read back once the caller completed that slot; averages of 30 recorded refreshes (plus the
    // CPU time of record_refresh) go to stderr.
    VkQueryPool timing = VK_NULL_HANDLE;
    double timestamp_ms = 0;
    std::array<bool, 2> timed{};
    std::uint32_t timing_base = UINT32_MAX; // query base of the refresh being recorded
    double timing_cpu = 0, timing_copy = 0, timing_blas = 0, timing_tlas = 0;
    unsigned timing_frames = 0, timing_gpu_frames = 0, timing_top_builds = 0;
    std::size_t timing_dirty = 0, timing_instances = 0;

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
        constexpr VkMemoryPropertyFlags rebar = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
            | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        constexpr VkMemoryPropertyFlags amd_uncached = VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD
            | VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD;
        for (std::uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
            const auto& type = memory_properties.memoryTypes[i];
            // A small (256 MiB) BAR window is left to the drivers and the raster renderer.
            if ((type.propertyFlags & rebar) == rebar && !(type.propertyFlags & amd_uncached)
                && memory_properties.memoryHeaps[type.heapIndex].size >= (VkDeviceSize{1} << 30)) {
                rebar_type = i;
                break;
            }
        }
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
        if (std::getenv("IKV_GPU_TIMING") && families[family].timestampValidBits) {
            VkQueryPoolCreateInfo query{};
            query.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            query.queryType = VK_QUERY_TYPE_TIMESTAMP;
            query.queryCount = 8;
            if (vkCreateQueryPool(device, &query, nullptr, &timing) != VK_SUCCESS) timing = VK_NULL_HANDLE;
            timestamp_ms = double(device_properties.properties.limits.timestampPeriod) * 1e-6;
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
        if (timing) vkDestroyQueryPool(device, timing, nullptr);
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
    // fallible: null instead of throwing when the heap is exhausted.
    std::shared_ptr<Block> allocate_block(std::uint32_t type, VkDeviceSize size, bool host_visible, bool fallible = false) {
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
        const auto result = vkAllocateMemory(device, &allocation, nullptr, &block->memory);
        if (fallible && (result == VK_ERROR_OUT_OF_DEVICE_MEMORY || result == VK_ERROR_OUT_OF_HOST_MEMORY)) return nullptr;
        checked(result, "Allocating RT memory");
        if (host_visible) {
            void* mapped = nullptr;
            checked(vkMapMemory(device, block->memory, 0, VK_WHOLE_SIZE, 0, &mapped), "Mapping RT memory");
            block->mapped = static_cast<std::byte*>(mapped);
        }
        return block;
    }
    // `pool`: bind into its current block of the chosen memory type (blocks grow 1 MiB..256 MiB); otherwise a dedicated
    // allocation. Host-visible buffers stay mapped for their lifetime. `device`: a host-visible buffer the GPU reads every
    // frame prefers rebar_type (the CPU must not read it back); once that heap is full, system memory as before.
    Buffer allocate(VkDeviceSize size, VkBufferUsageFlags usage, const void* source = nullptr, bool host_visible = false,
                    Pool* shared = nullptr, bool device_read = false) {
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
        const auto bind = [&](std::uint32_t memory_type, bool fallible) -> std::shared_ptr<Block> {
            if (!shared) return allocate_block(memory_type, requirements.size, host, fallible);
            const auto slot = memory_type * 2 + host;
            // 256: acceleration-structure storage and scratch addresses, whatever the reported buffer alignment.
            auto& block = shared->blocks[slot];
            auto offset = aligned(shared->used[slot], std::max<VkDeviceSize>(requirements.alignment, 256));
            if (!block || offset > block->size || requirements.size > block->size - offset) {
                const auto capacity = std::max(requirements.size, shared->next[slot] ? shared->next[slot] : VkDeviceSize{1} << 20);
                auto grown = allocate_block(memory_type, capacity, host, fallible);
                if (!grown) return nullptr;
                shared->next[slot] = std::min(capacity * 2, VkDeviceSize{256} << 20);
                block = std::move(grown);
                offset = 0;
            }
            shared->used[slot] = offset + requirements.size;
            result.offset = offset;
            return block;
        };
        if (host && device_read && rebar_type != UINT32_MAX && (requirements.memoryTypeBits >> rebar_type & 1u)) {
            result.block = bind(rebar_type, true);
            if (!result.block) rebar_type = UINT32_MAX;
        }
        if (!result.block) result.block = bind(type, false);
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
    // transient: the build's scratch comes from it (build_scratch, dropped by the caller once the build completed), and
    // only a structure updated later keeps a scratch, of the update size. Otherwise one scratch serves builds and updates.
    std::unique_ptr<Acceleration> prepare(VkAccelerationStructureBuildGeometryInfoKHR& info, std::uint32_t primitives,
                                          Pool* shared = nullptr, Pool* transient = nullptr) {
        auto acceleration = std::make_unique<Acceleration>(device, destroy);
        VkAccelerationStructureBuildSizesInfoKHR size_info{};
        size_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        sizes(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &primitives, &size_info);
        acceleration->storage = allocate(size_info.accelerationStructureSize,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, nullptr, false, shared);
        acceleration->build_scratch_size = size_info.buildScratchSize;
        if (transient) {
            acceleration->build_scratch = scratch(size_info.buildScratchSize, transient);
            if (info.flags & VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR)
                acceleration->scratch = scratch(size_info.updateScratchSize, shared);
        } else {
            acceleration->scratch = scratch(std::max(size_info.buildScratchSize, size_info.updateScratchSize), shared);
        }
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
        info.scratchData.deviceAddress = scratch_address(transient ? acceleration->build_scratch : acceleration->scratch);
        return acceleration;
    }
    Buffer scratch(VkDeviceSize bytes, Pool* shared = nullptr) {
        return allocate(std::max(bytes, VkDeviceSize{1}) + properties.minAccelerationStructureScratchOffsetAlignment - 1,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, nullptr, false, shared);
    }
    VkDeviceAddress scratch_address(const Buffer& buffer) const {
        return aligned(buffer.address, properties.minAccelerationStructureScratchOffsetAlignment);
    }
    // Retires the transient build scratch of `bottom` with the frame of `slot` (recorded builds) or frees it (completed).
    void drop_build_scratch(std::span<const std::unique_ptr<Acceleration>> bottom, VkCommandBuffer record, unsigned slot) {
        for (const auto& acceleration : bottom) {
            if (!acceleration || !acceleration->build_scratch.handle) continue;
            if (record) {
                auto holder = std::make_unique<Acceleration>(device, destroy);
                holder->build_scratch = std::move(acceleration->build_scratch);
                retired_slots[slot & 1u].push_back(std::move(holder));
            } else {
                acceleration->build_scratch = {};
            }
        }
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
    static constexpr VkDeviceSize skin_stride = 3 * sizeof(float);
    static void write_positions(float* destination, const Mesh& mesh) {
        for (const auto& vertex : mesh.vertices) {
            destination[0] = vertex.x;
            destination[1] = vertex.y;
            destination[2] = vertex.z;
            destination += 3;
        }
    }
    // Inputs of a BLAS about to be built. Static meshes keep their whole vertices (rebuilt only on rare revisions);
    // skins, refit and restaged every frame they move, get a position-only stream (12 of 84 bytes per vertex), their
    // fixed corner UVs, and device-read memory.
    void bottom_inputs(Acceleration& target, const Mesh& mesh, Pool* shared) {
        if (mesh.influences.empty()) {
            target.vertices = allocate(mesh.vertices.size() * sizeof(Vertex), input_usage, mesh.vertices.data(), true, shared);
            target.indices = allocate(mesh.indices.size() * sizeof(std::uint32_t), input_usage, mesh.indices.data(), true, shared);
            return;
        }
        target.vertices = allocate(mesh.vertices.size() * skin_stride, input_usage, nullptr, true, shared, true);
        write_positions(reinterpret_cast<float*>(target.vertices.mapped()), mesh);
        target.indices = allocate(mesh.indices.size() * sizeof(std::uint32_t), input_usage, mesh.indices.data(), true, shared, true);
        target.corner_uvs.resize(mesh.indices.size());
        for (std::size_t i = 0; i < mesh.indices.size(); ++i) target.corner_uvs[i] = half_uv(mesh.vertices[mesh.indices[i]]);
    }
    void bottom_input(BuildInput& input, const Mesh& mesh, const Acceleration& acceleration) {
        input.geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        input.geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR; // alpha-tested placements: FORCE_NO_OPAQUE (alpha_test)
        auto& triangles = input.geometry.geometry.triangles;
        triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        triangles.vertexStride = mesh.influences.empty() ? sizeof(Vertex) : skin_stride;
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
        input.info.scratchData.deviceAddress = scratch_address(acceleration.scratch);
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
    static constexpr std::uint8_t placement_proxy = 1, placement_sky = 2, placement_ordinary = 4, placement_absent = 8;
    static std::uint8_t placement_bits(const Instance& instance) {
        return static_cast<std::uint8_t>((instance.occlusion_proxy ? placement_proxy : 0) | (instance.sky ? placement_sky : 0)
            | (instance.source_ordinary_color_participates ? placement_ordinary : 0));
    }
    static bool same_key(const State::Placement& placement, const Instance& instance) {
        return instance.mesh == placement.mesh && instance.material == placement.material
            && instance.entity_id == placement.entity_id
            && placement_bits(instance) == (placement.bits & ~placement_absent);
    }
    // First entity with `id` (as a linear find), via the cached index `hint` or the sorted index of this refresh.
    const RenderEntity* find_entity(std::span<const RenderEntity> entities, std::uint32_t id, std::uint32_t& hint) {
        if (hint < entities.size() && entities[hint].id == id) return &entities[hint];
        if (!entity_indexed) {
            entity_index.clear();
            for (std::size_t i = 0; i < entities.size(); ++i)
                entity_index.emplace_back(entities[i].id, static_cast<std::uint32_t>(i));
            std::stable_sort(entity_index.begin(), entity_index.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
            entity_indexed = true;
        }
        const auto found = std::lower_bound(entity_index.begin(), entity_index.end(), id,
            [](const auto& entry, std::uint32_t value) { return entry.first < value; });
        if (found == entity_index.end() || found->first != id) return nullptr;
        hint = found->second;
        return &entities[hint];
    }
    static void write_transform(VkAccelerationStructureInstanceKHR& instance, const Transform& placed) {
        for (std::size_t row = 0; row < 3; ++row)
            for (std::size_t column = 0; column < 4; ++column)
                instance.transform.matrix[row][column] = placed.matrix[column*4 + row];
    }
    // The TLAS record of `mesh` placed by `placed`; rebuild() and refresh() share it.
    static VkAccelerationStructureInstanceKHR instance_record(const Scene& scene, const State& s, std::size_t mesh,
            std::uint32_t material, bool actor, const Transform& placed, std::vector<std::string>& slots, bool assign) {
        VkAccelerationStructureInstanceKHR instance{};
        write_transform(instance, placed);
        instance.instanceCustomIndex = s.mesh_offset[mesh] & 0xffffffu;
        // Mask 2: actors (entity-attached); 4: source ocean; 1: other static scenery. Faithful RT sun rays from
        // lightmapped (static) receivers test only actors, since the baked lightmap already holds the scenery's
        // shadows; enhanced faithful RT tests 1|2 (the ocean casts no shadow on the bed refracted through it).
        instance.mask = actor ? 0x02 : material < scene.materials.size() && scene.materials[material].source_ocean ? 0x04 : 0x01;
        // Source clockwise-front triangles are ray back faces by default; rt_lighting.glsl culls back faces.
        instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FLIP_FACING_BIT_KHR;
        alpha_test(instance, scene, material, slots, assign);
        instance.accelerationStructureReference = s.bottom[s.mesh_blas[mesh]]->address;
        return instance;
    }
    // Flags, mask, record offsets or BLAS changing at any slot: the TLAS is rebuilt, not updated (an update may only
    // move instances; Kabus 22 rebuilds on flag changes too).
    static bool same_layout(std::span<const VkAccelerationStructureInstanceKHR> before,
                            std::span<const VkAccelerationStructureInstanceKHR> after, std::size_t count) {
        if (before.size() < count || after.size() < count) return false;
        for (std::size_t i = 0; i < count; ++i) {
            const auto& a = before[i];
            const auto& b = after[i];
            if (a.mask != b.mask || a.flags != b.flags || a.instanceCustomIndex != b.instanceCustomIndex
                || a.instanceShaderBindingTableRecordOffset != b.instanceShaderBindingTableRecordOffset
                || a.accelerationStructureReference != b.accelerationStructureReference) return false;
        }
        return true;
    }
    // Every placement from scratch into cpu_instances, recording what each scene instance contributed.
    void place_all(const Scene& scene, std::span<const RenderEntity> entities) {
        auto& s = *state;
        auto& instances = s.cpu_instances;
        instances.clear();
        const bool preview = scene.instances.empty();
        const std::size_t count = instance_bound(scene);
        s.placements.resize(scene.instances.size());
        s.sources.resize(scene.instances.size());
        s.entity_at.resize(scene.instances.size());
        const Transform identity;
        for (std::size_t i = 0; i < count; ++i) {
            checkpoint(i);
            const std::size_t mesh = preview ? i : scene.instances[i].mesh;
            if (mesh >= s.mesh_blas.size()) throw std::runtime_error("Invalid retained RT mesh reference");
            const Transform& transform = preview ? identity : scene.instances[i].transform;
            validate_transform(transform);
            const bool color_draw = scene_source_color_draw(scene, static_cast<std::uint32_t>(mesh),
                preview ? UINT32_MAX : scene.instances[i].material,
                preview || scene.instances[i].source_ordinary_color_participates);
            State::Placement unplaced;
            State::Placement& placement = preview ? unplaced : s.placements[i];
            if (!preview) {
                const auto& in = scene.instances[i];
                placement = {in.mesh, in.material, in.entity_id, UINT32_MAX, placement.entity_hint, placement_bits(in)};
                if (in.occlusion_proxy || in.sky) continue;
            }
            if (!color_draw || s.mesh_blas[mesh] == std::numeric_limits<std::size_t>::max()) continue;
            Transform resolved;
            const Transform* placed = &transform;
            const bool actor = !preview && scene.instances[i].entity_id != 0;
            if (actor) {
                const auto* match = find_entity(entities, scene.instances[i].entity_id, placement.entity_hint);
                if (!match) {
                    placement.bits |= placement_absent;
                    continue;
                }
                resolved = entity_instance_transform(scene, scene.instances[i], match->x, match->ground_y, match->z, match->yaw);
                validate_transform(resolved);
                placed = &resolved;
                s.entity_at[i] = {match->x, match->ground_y, match->z, match->yaw};
            }
            const auto material = preview || scene.instances[i].material == UINT32_MAX
                ? scene.meshes[mesh].material : scene.instances[i].material;
            placement.slot = static_cast<std::uint32_t>(instances.size());
            if (!preview) s.sources[i] = transform;
            instances.push_back(instance_record(scene, s, mesh, material, actor, *placed, s.alpha_textures, false));
        }
    }
    // Rewrites the cpu_instances slots whose scene transform or entity placement changed since the last placement
    // (dirty_slots, ascending). False: some placement itself changed (mesh, material, flags, entity presence); the
    // caller places everything again.
    bool place_changed(const Scene& scene, std::span<const RenderEntity> entities) {
        auto& s = *state;
        for (std::size_t i = 0; i < scene.instances.size(); ++i) {
            const auto& in = scene.instances[i];
            auto& placement = s.placements[i];
            if (!same_key(placement, in)) return false;
            if (placement.slot == UINT32_MAX) {
                if ((placement.bits & placement_absent) && find_entity(entities, in.entity_id, placement.entity_hint))
                    return false;
                continue;
            }
            Transform resolved;
            const Transform* placed = &in.transform;
            if (in.entity_id) {
                const auto* match = find_entity(entities, in.entity_id, placement.entity_hint);
                if (!match) return false;
                const std::array<float, 4> at{match->x, match->ground_y, match->z, match->yaw};
                if (at == s.entity_at[i] && in.transform.matrix == s.sources[i].matrix) continue;
                validate_transform(in.transform);
                resolved = entity_instance_transform(scene, in, match->x, match->ground_y, match->z, match->yaw);
                validate_transform(resolved);
                placed = &resolved;
                s.entity_at[i] = at;
            } else {
                if (in.transform.matrix == s.sources[i].matrix) continue;
                validate_transform(in.transform);
            }
            s.sources[i] = in.transform;
            write_transform(s.cpu_instances[placement.slot], *placed);
            dirty_slots.push_back(placement.slot);
        }
        return true;
    }
    // record == VK_NULL_HANDLE: host writes and a waited submit; the caller completed every frame using this scene.
    // Otherwise the host inputs are staged in `slot`'s arena and the copies/builds are recorded into the caller's frame
    // command buffer ahead of its ray queries. Recording returns false, changing nothing on the GPU, when the placements
    // outgrow the TLAS capacity. `records` are appended triangle records written at `record_offset`.
    // A refresh that changes nothing (no moved placement, no new skin revision, nothing appended) records nothing.
    bool refresh(const Scene& scene, std::span<const RenderEntity> entities, std::span<const BuildInput> appended,
                 VkCommandBuffer record = VK_NULL_HANDLE, unsigned slot = 0, std::span<const Triangle> records = {},
                 std::size_t record_offset = 0) {
        auto& s = *state;
        const std::size_t count = instance_bound(scene);
        if (count > properties.maxInstanceCount || count > std::numeric_limits<std::uint32_t>::max())
            throw std::runtime_error("Scene exceeds RT instance limit");
        entity_indexed = false;
        dirty_slots.clear();
        const bool incremental = std::exchange(s.placed, false) && !s.rebuild_top && !scene.instances.empty()
            && s.placements.size() == scene.instances.size() && place_changed(scene, entities);
        std::uint32_t actual_count = s.instance_count;
        if (!incremental) {
            dirty_slots.clear();
            std::swap(s.previous_instances, s.cpu_instances);
            place_all(scene, entities);
            actual_count = static_cast<std::uint32_t>(s.cpu_instances.size());
            if (s.cpu_instances.empty()) s.cpu_instances.emplace_back();
        }
        auto& instances = s.cpu_instances;
        // Capacity follows the placement bound, not this frame's placed count, so record_refresh's bound check and this
        // growth agree: otherwise a grown bound that still fits would fail every in-frame update.
        const bool fits = std::max(count, instances.size()) <= s.instance_capacity;
        const bool same_count = fits && actual_count == s.instance_count && !s.rebuild_top
            && (incremental || same_layout(s.previous_instances, instances, actual_count));
        if (record && !fits) return false;
        dirty_meshes.clear();
        for (std::size_t m = 0; m < scene.meshes.size(); ++m)
            if (scene.meshes[m].revision != s.revisions[m] && s.mesh_blas[m] != std::numeric_limits<std::size_t>::max())
                dirty_meshes.push_back(m);
        if (incremental && dirty_slots.empty() && dirty_meshes.empty() && appended.empty() && records.empty() && same_count) {
            for (std::size_t m = 0; m < scene.meshes.size(); ++m) s.revisions[m] = scene.meshes[m].revision;
            s.placed = true;
            return true;
        }
        const VkDeviceSize instance_bytes = instances.size() * sizeof(instances[0]);
        const VkDeviceSize record_bytes = records.size() * sizeof(Triangle);
        std::byte* arena = nullptr;
        VkDeviceSize cursor = 0;
        staged.clear();
        if (record) {
            VkDeviceSize bytes = (incremental ? dirty_slots.size() * sizeof(instances[0]) : instance_bytes) + record_bytes;
            for (const auto m : dirty_meshes) {
                const auto& acceleration = *s.bottom[s.mesh_blas[m]];
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
            if (record) copy(put(s.triangles, record_offset * sizeof(Triangle), record_bytes), records.data(), record_bytes);
            else upload(s.triangles, records.data(), record_bytes, record_offset * sizeof(Triangle));
        }
        updates.clear();
        for (std::size_t i = 0; i < appended.size(); ++i) {
            checkpoint(i);
            updates.push_back(appended[i]);
        }
        transient_scratch.clear();
        for (std::size_t d = 0; d < dirty_meshes.size(); ++d) {
            checkpoint(d);
            const auto m = dirty_meshes[d];
            const auto& mesh = scene.meshes[m];
            const bool skin = !mesh.influences.empty();
            const auto color = validate_mesh(scene, m, !skin);
            auto& acceleration = *s.bottom[s.mesh_blas[m]];
            const VkDeviceSize triangle_bytes = mesh.indices.size() / 3 * sizeof(Triangle);
            std::byte* vertices = nullptr;
            void* mapped = nullptr;
            // Skin revisions change vertices only; immutable topology stays on GPU.
            if (record) {
                if (!skin)
                    copy(put(acceleration.indices, 0, acceleration.indices.size), mesh.indices.data(), acceleration.indices.size);
                vertices = put(acceleration.vertices, 0, acceleration.vertices.size);
                mapped = put(s.triangles, s.mesh_offset[m] * sizeof(Triangle), triangle_bytes);
            } else {
                if (!skin) upload(acceleration.indices, mesh.indices.data(), acceleration.indices.size);
                if (!acceleration.vertices.mapped() || !s.triangles.mapped())
                    throw std::runtime_error("RT upload exceeds retained host buffer");
                vertices = acceleration.vertices.mapped();
                mapped = s.triangles.mapped() + s.mesh_offset[m] * sizeof(Triangle);
            }
            auto* triangles = static_cast<Triangle*>(mapped);
            if (skin) {
                // The positions are the only per-revision change: UVs come from corner_uvs (fixed at the BLAS build).
                write_positions(reinterpret_cast<float*>(vertices), mesh);
                const float* uv = acceleration.corner_uvs.data();
                for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
                    const auto& a = mesh.vertices[mesh.indices[i]];
                    const auto& b = mesh.vertices[mesh.indices[i+1]];
                    const auto& c = mesh.vertices[mesh.indices[i+2]];
                    if (!std::isfinite(a.x + a.y + a.z + b.x + b.y + b.z + c.x + c.y + c.z))
                        throw std::runtime_error("Non-finite RT mesh vertex");
                    triangles[i/3] = {{a.x,a.y,a.z,uv[i]}, {b.x,b.y,b.z,uv[i+1]}, {c.x,c.y,c.z,uv[i+2]}, color};
                }
            } else {
                copy(vertices, mesh.vertices.data(), acceleration.vertices.size);
                for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
                    checkpoint(i / 3);
                    triangles[i/3] = triangle_record(mesh.vertices[mesh.indices[i]], mesh.vertices[mesh.indices[i+1]],
                        mesh.vertices[mesh.indices[i+2]], color);
                }
            }
            updates.emplace_back();
            bottom_input(updates.back(), mesh, acceleration);
            if (!skin && !acceleration.scratch.handle) {
                // A static BLAS built with a transient scratch: rebuilt with one that lives as long as this build.
                transient_scratch.push_back(scratch(acceleration.build_scratch_size));
                updates.back().info.scratchData.deviceAddress = scratch_address(transient_scratch.back());
            }
        }
        Buffer replacement_instances;
        std::uint32_t capacity = s.instance_capacity;
        if (record) {
            if (incremental) {
                // Only the moved slots, one copy per run of consecutive slots.
                for (std::size_t begin = 0; begin < dirty_slots.size();) {
                    std::size_t end = begin + 1;
                    while (end < dirty_slots.size() && dirty_slots[end] == dirty_slots[end - 1] + 1) ++end;
                    const VkDeviceSize bytes = (end - begin) * sizeof(instances[0]);
                    copy(put(s.instances, VkDeviceSize{dirty_slots[begin]} * sizeof(instances[0]), bytes),
                        &instances[dirty_slots[begin]], bytes);
                    begin = end;
                }
            } else {
                copy(put(s.instances, 0, instance_bytes), instances.data(), instance_bytes);
            }
        } else if (fits) {
            upload(s.instances, instances.data(), instance_bytes);
        } else {
            capacity = std::max<std::uint32_t>(instance_capacity(count), static_cast<std::uint32_t>(instances.size()));
            replacement_instances = allocate(VkDeviceSize{capacity} * sizeof(instances[0]), input_usage, nullptr, true,
                nullptr, true);
            upload(replacement_instances, instances.data(), instance_bytes);
        }
        updates.emplace_back();
        auto& top = updates.back();
        top.geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        top.geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        top.geometry.geometry.instances.data.deviceAddress = fits ? s.instances.address : replacement_instances.address;
        top.info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        top.info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
        // UPDATE while only transforms moved: a per-frame BUILD measured no faster (wave 27, 4.6k instances).
        top.info.mode = same_count ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        top.info.geometryCount = 1;
        top.info.pGeometries = &top.geometry;
        top.range.primitiveCount = actual_count;
        std::unique_ptr<Acceleration> replacement_top;
        if (fits) {
            // Sized for `instance_capacity` primitives: any smaller count rebuilds or updates this same handle.
            if (same_count) top.info.srcAccelerationStructure = s.top->handle;
            top.info.dstAccelerationStructure = s.top->handle;
            top.info.scratchData.deviceAddress = scratch_address(s.top->scratch);
        } else {
            replacement_top = prepare(top.info, capacity);
        }
        if (record && timing) {
            timing_dirty += dirty_meshes.size();
            timing_instances += actual_count;
            timing_top_builds += !same_count;
        }
        if (record) {
            record_staged(record, arenas[slot & 1u].handle, slot);
            for (auto& buffer : transient_scratch) {
                auto holder = std::make_unique<Acceleration>(device, destroy);
                holder->build_scratch = std::move(buffer);
                retired_slots[slot & 1u].push_back(std::move(holder));
            }
        } else {
            submit(updates);
        }
        transient_scratch.clear();
        if (!fits) {
            s.top = std::move(replacement_top);
            s.instances = std::move(replacement_instances);
            s.instance_capacity = capacity;
        }
        s.instance_count = actual_count;
        s.rebuild_top = false;
        for (std::size_t m = 0; m < scene.meshes.size(); ++m) s.revisions[m] = scene.meshes[m].revision;
        s.placed = !scene.instances.empty();
        return true;
    }
    void stamp(VkCommandBuffer frame, std::uint32_t index) {
        if (timing_base != UINT32_MAX)
            vkCmdWriteTimestamp(frame, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timing, timing_base + index);
    }
    void record_staged(VkCommandBuffer frame, VkBuffer source, unsigned slot) {
        timing_base = timing ? (slot & 1u) * 4 : UINT32_MAX;
        if (timing) {
            vkCmdResetQueryPool(frame, timing, timing_base, 4);
            timed[slot & 1u] = true;
        }
        // In-flight frames' ray queries/triangle reads and builds finish before inputs, records and structures change.
        VkMemoryBarrier before{};
        before.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        before.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_TRANSFER_WRITE_BIT;
        before.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR
            | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        vkCmdPipelineBarrier(frame, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR
            | VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &before, 0, nullptr, 0, nullptr);
        stamp(frame, 0);
        for (const auto& [destination, region] : staged) vkCmdCopyBuffer(frame, source, destination, 1, &region);
        VkMemoryBarrier copied{};
        copied.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        copied.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        copied.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        vkCmdPipelineBarrier(frame, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &copied, 0, nullptr, 0, nullptr);
        stamp(frame, 1);
        record_builds(frame, updates);
        timing_base = UINT32_MAX;
    }
    void submit(std::vector<BuildInput>& inputs) {
        execute([&](VkCommandBuffer target) {
            VkMemoryBarrier host{};
            host.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            host.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
            host.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(target, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                0, 1, &host, 0, nullptr, 0, nullptr);
            record_builds(target, inputs);
        });
    }
    // Copies between retained buffers on the GPU and waits (growth and packing of triangle records, whose device-read
    // memory the CPU must not read back). Same contract as submit(): no frame uses these buffers meanwhile.
    void copy_on_device(VkBuffer source, VkBuffer destination, std::span<const VkBufferCopy> regions) {
        if (regions.empty()) return;
        execute([&](VkCommandBuffer target) {
            vkCmdCopyBuffer(target, source, destination, static_cast<std::uint32_t>(regions.size()), regions.data());
        });
    }
    template<class Record> void execute(Record&& record_commands) {
        pulse();
        checked(vkResetCommandBuffer(command, 0), "Resetting RT build command");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        checked(vkBeginCommandBuffer(command, &begin), "Beginning RT build command");
        record_commands(command);
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
        stamp(target, 2);
        const auto* top_range = &inputs.back().range;
        build(target, 1, &inputs.back().info, &top_range);
        dependency.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(target, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &dependency, 0, nullptr, 0, nullptr);
        stamp(target, 3);
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
        Pool shared, transient;
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
            bottom_inputs(inputs, mesh, &shared);
            builds.emplace_back();
            auto& build_input = builds.back();
            bottom_input(build_input, mesh, inputs);
            build_input.info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            build_input.info.srcAccelerationStructure = VK_NULL_HANDLE;
            build_input.info.pGeometries = &build_input.geometry;
            auto acceleration = prepare(build_input.info, build_input.range.primitiveCount, &shared, &transient);
            acceleration->vertices = std::move(inputs.vertices);
            acceleration->indices = std::move(inputs.indices);
            acceleration->corner_uvs = std::move(inputs.corner_uvs);
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
            auto grown = allocate(capacity, triangle_usage, nullptr, true, nullptr, true);
            const VkBufferCopy prefix{0, 0, triangle_count * sizeof(Triangle)};
            if (prefix.size) copy_on_device(state->triangles.handle, grown.handle, {&prefix, 1});
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
            state->placed = false; // new BLAS may now be placed
            // New BLAS, their records, dirty existing skins and the TLAS share one submit or frame.
            refresh(scene, entities, builds, record, slot, records, triangle_count);
            drop_build_scratch(std::span(state->bottom).subspan(old_bottom), record, slot);
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
            packed = allocate(triangle_capacity(live), triangle_usage, nullptr, true, nullptr, true);
            std::vector<VkBufferCopy> regions;
            std::size_t cursor = 0;
            for (std::size_t m = 0; m < kept; ++m) {
                const auto old = remap[m];
                if (s.mesh_blas[old] == none) continue;
                const auto count = s.index_counts[old] / 3;
                if (count) regions.push_back({s.mesh_offset[old] * sizeof(Triangle), cursor * sizeof(Triangle), count * sizeof(Triangle)});
                mesh_offset[m] = static_cast<std::uint32_t>(cursor);
                cursor += count;
            }
            copy_on_device(s.triangles.handle, packed.handle, regions);
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
            // A placement unchanged since the last placement pass (same mesh, material and flags; BLAS mapping and
            // counts unchanged) cannot activate a dormant slot: only changed or new placements are checked again.
            const auto& s = *state;
            const bool cached = s.placed && !s.rebuild_top && s.placements.size() == scene.instances.size();
            for (std::size_t i = 0; i < scene.instances.size(); ++i) {
                checkpoint(i);
                const auto& instance = scene.instances[i];
                if (cached && same_key(s.placements[i], instance)) continue;
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
    void read_timing(unsigned slot) {
        if (!timing || !std::exchange(timed[slot & 1u], false)) return;
        std::array<std::uint64_t, 4> t{};
        if (vkGetQueryPoolResults(device, timing, (slot & 1u) * 4, 4, sizeof(t), t.data(), sizeof(t[0]),
                VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) return;
        timing_copy += double(t[1] - t[0]) * timestamp_ms;
        timing_blas += double(t[2] - t[1]) * timestamp_ms;
        timing_tlas += double(t[3] - t[2]) * timestamp_ms;
        ++timing_gpu_frames;
    }
    bool record_refresh(const Scene& scene, std::span<const RenderEntity> entities, VkCommandBuffer frame, unsigned slot) {
        const auto started = std::chrono::steady_clock::now();
        read_timing(slot);
        const bool recorded = record_refresh_untimed(scene, entities, frame, slot);
        if (timing && recorded) {
            timing_cpu += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            if (++timing_frames == 30) {
                const double gpu = std::max(1u, timing_gpu_frames);
                std::cerr << "Vulkan RT timing frames=30 cpuRefreshMs=" << timing_cpu / 30
                          << " gpuCopyMs=" << timing_copy / gpu << " gpuBlasMs=" << timing_blas / gpu
                          << " gpuTlasMs=" << timing_tlas / gpu << " gpuAsMs=" << (timing_copy + timing_blas + timing_tlas) / gpu
                          << " dirtyMeshes=" << double(timing_dirty) / 30 << " instances=" << timing_instances / 30
                          << " topBuilds=" << timing_top_builds << '\n';
                timing_cpu = timing_copy = timing_blas = timing_tlas = 0;
                timing_frames = timing_gpu_frames = timing_top_builds = 0;
                timing_dirty = timing_instances = 0;
            }
        }
        return recorded;
    }
    bool record_refresh_untimed(const Scene& scene, std::span<const RenderEntity> entities, VkCommandBuffer frame,
                                unsigned slot) {
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
        Pool shared, transient;
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
            Acceleration buffers(device, destroy);
            bottom_inputs(buffers, mesh, &shared);
            inputs.emplace_back();
            auto& input = inputs.back();
            bottom_input(input, mesh, buffers);
            input.info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            input.info.srcAccelerationStructure = VK_NULL_HANDLE;
            input.info.pGeometries = &input.geometry;
            auto acceleration = prepare(input.info, input.range.primitiveCount, &shared, &transient);
            acceleration->vertices = std::move(buffers.vertices);
            acceleration->indices = std::move(buffers.indices);
            acceleration->corner_uvs = std::move(buffers.corner_uvs);
            mesh_blas[m] = next->bottom.size();
            next->bottom.push_back(std::move(acceleration));
        }
        // A nonzero descriptor range for an empty scene; this record is never hit.
        if (triangle_data.empty()) triangle_data.emplace_back();
        next->triangles = allocate(triangle_capacity(triangle_data.size()), triangle_usage, nullptr, true, nullptr, true);
        upload(next->triangles, triangle_data.data(), triangle_data.size() * sizeof(Triangle));
        next->mesh_blas = std::move(mesh_blas);
        next->mesh_offset = std::move(mesh_offset);
        std::vector<VkAccelerationStructureInstanceKHR> instances;
        instances.reserve(instance_count);
        const Transform identity;
        entity_indexed = false;
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
            if (!color_draw || next->mesh_blas[mesh] == std::numeric_limits<std::size_t>::max()) continue;
            Transform resolved;
            const Transform* placed = &transform;
            const bool actor = !preview && scene.instances[i].entity_id != 0;
            if (actor) {
                std::uint32_t hint = UINT32_MAX;
                const auto* match = find_entity(entities, scene.instances[i].entity_id, hint);
                if (!match) continue;
                resolved = entity_instance_transform(scene, scene.instances[i],
                    match->x, match->ground_y, match->z, match->yaw);
                validate_transform(resolved);
                placed = &resolved;
            }
            instances.push_back(instance_record(scene, *next, mesh, preview || scene.instances[i].material == UINT32_MAX
                ? scene.meshes[mesh].material : scene.instances[i].material, actor, *placed, next->alpha_textures, true));
        }
        const std::uint32_t actual_instances = static_cast<std::uint32_t>(instances.size());
        if (instances.empty()) instances.emplace_back();
        const auto capacity = instance_capacity(instance_count);
        next->instances = allocate(VkDeviceSize{capacity} * sizeof(VkAccelerationStructureInstanceKHR), input_usage, nullptr,
            true, nullptr, true);
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
        drop_build_scratch(next->bottom, VK_NULL_HANDLE, 0);
        next->source = &scene;
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
