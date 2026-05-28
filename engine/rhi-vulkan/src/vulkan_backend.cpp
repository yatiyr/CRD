#include "log_channel.hpp"

#include <crd/containers/array.hpp>
#include <crd/log/log.hpp>
#include <crd/memory/allocators/offset_allocator.hpp> // ADR-0085 S6: GPU suballocation kernel
#include <crd/memory/construct.hpp>
#include <crd/rhi/gpu_residency.hpp> // ADR-0085 S7: defrag + residency policy seams
#include <crd/rhi/vulkan_backend.hpp>

#include <mutex>
#include <utility>

// In GLFW 3.4, GLFW_INCLUDE_VULKAN does NOT suppress GL/gl.h; only GLFW_INCLUDE_NONE does.
// Include Vulkan first so GLFW sees VK_VERSION_1_0 and declares glfwCreateWindowSurface.
#include <vulkan/vulkan.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cstring>
#include <memory>

namespace crd::rhi
{
namespace
{
[[nodiscard]] bool vk_ok(VkResult result, const char* what) noexcept
{
    if (result == VK_SUCCESS)
    {
        return true;
    }
    CRD_LOG_ERROR(detail::g_log_rhi_vulkan, "{} failed with VkResult={}", what, static_cast<int>(result));
    return false;
}

[[nodiscard]] VkFormat to_vk_format(Format format) noexcept
{
    switch (format)
    {
        case Format::R8G8B8A8Unorm:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case Format::B8G8R8A8Unorm:
            return VK_FORMAT_B8G8R8A8_UNORM;
        case Format::R32G32Sfloat:
            return VK_FORMAT_R32G32_SFLOAT;
        case Format::R32G32B32Sfloat:
            return VK_FORMAT_R32G32B32_SFLOAT;
        case Format::R32G32B32A32Sfloat:
            return VK_FORMAT_R32G32B32A32_SFLOAT;
        case Format::R32Uint:
            return VK_FORMAT_R32_UINT;
        case Format::R32Sfloat:
            return VK_FORMAT_R32_SFLOAT;
        case Format::D24UnormS8Uint:
            return VK_FORMAT_D24_UNORM_S8_UINT;
        case Format::D32Sfloat:
            return VK_FORMAT_D32_SFLOAT;
        case Format::Undefined:
        default:
            return VK_FORMAT_UNDEFINED;
    }
}

[[nodiscard]] VkPrimitiveTopology to_vk_topology(PrimitiveTopology topology) noexcept
{
    switch (topology)
    {
        case PrimitiveTopology::TriangleList:
        default:
            return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

[[nodiscard]] VkVertexInputRate to_vk_input_rate(VertexInputRate rate) noexcept
{
    switch (rate)
    {
        case VertexInputRate::Instance:
            return VK_VERTEX_INPUT_RATE_INSTANCE;
        case VertexInputRate::Vertex:
        default:
            return VK_VERTEX_INPUT_RATE_VERTEX;
    }
}

[[nodiscard]] VkBufferUsageFlags to_vk_buffer_usage(crd::u32 usage_bits) noexcept
{
    VkBufferUsageFlags flags = 0;
    if (has_flag(usage_bits, BufferUsage::TransferSrc))
        flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (has_flag(usage_bits, BufferUsage::TransferDst))
        flags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (has_flag(usage_bits, BufferUsage::Vertex))
        flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (has_flag(usage_bits, BufferUsage::Index))
        flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (has_flag(usage_bits, BufferUsage::Uniform))
        flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    // Phase 3.1.7.6 v0b — `BufferUsage::Storage` was declared in the
    // enum at the descriptor system's introduction but never wired
    // through here; surfaced when v0b's first-light compute dispatch
    // needed an actual SSBO. v0a's "create_buffer with Storage works"
    // test passed because Vulkan accepts a buffer with TRANSFER flags
    // alone — but using it as an SSBO at bind would have failed
    // validation. Fixed alongside `Indirect`.
    if (has_flag(usage_bits, BufferUsage::Storage))
        flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (has_flag(usage_bits, BufferUsage::Indirect))
        flags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    return flags;
}

[[nodiscard]] VkMemoryPropertyFlags to_vk_memory_properties(MemoryUsage usage) noexcept
{
    switch (usage)
    {
        case MemoryUsage::CpuToGpu:
            return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        case MemoryUsage::GpuToCpu:
            return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        case MemoryUsage::GpuOnly:
        default:
            return VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    }
}

[[nodiscard]] crd::u32 find_memory_type(VkPhysicalDevice physical_device, crd::u32 type_bits,
                                        VkMemoryPropertyFlags required)
{
    VkPhysicalDeviceMemoryProperties memory_properties{};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
    for (crd::u32 i = 0; i < memory_properties.memoryTypeCount; ++i)
    {
        if ((type_bits & (1U << i)) != 0U && (memory_properties.memoryTypes[i].propertyFlags & required) == required)
        {
            return i;
        }
    }
    return UINT32_MAX;
}

struct VkAccessInfo
{
    VkPipelineStageFlags stage;
    VkAccessFlags access;
    VkImageLayout layout;
};

[[nodiscard]] VkAccessInfo to_vk_access_info(ImageAccess access) noexcept
{
    switch (access)
    {
        case ImageAccess::ColorWrite:
            return {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        case ImageAccess::DepthWrite:
            return {VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        case ImageAccess::DepthRead:
            return {VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        case ImageAccess::ShaderRead:
            return {VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        case ImageAccess::TransferSrc:
            return {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
        case ImageAccess::TransferDst:
            return {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL};
        case ImageAccess::Present:
            return {VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
        // Phase 3.1.7.6 v0c — compute-stage image access (sampled / storage).
        // GENERAL layout for read-write / write paths since storage images
        // require it; ShaderRead-only stays in SHADER_READ_ONLY_OPTIMAL.
        case ImageAccess::ComputeShaderRead:
            return {VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        case ImageAccess::ComputeShaderWrite:
            return {VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_GENERAL};
        case ImageAccess::ComputeShaderReadWrite:
            return {VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_GENERAL};
        case ImageAccess::Undefined:
        default:
            return {VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED};
    }
}

// Phase 3.1.7.6 v0c (ADR-0080 D8) — typed BufferAccess → (stage, access)
// pair. Each variant maps to ONE specific stage + access bit pair so the
// pipeline barrier picks the tightest valid mask (no over-barrier).
struct VkBufferAccessInfo
{
    VkPipelineStageFlags stage;
    VkAccessFlags access;
};

[[nodiscard]] VkBufferAccessInfo to_vk_buffer_access_info(BufferAccess access) noexcept
{
    switch (access)
    {
        case BufferAccess::ComputeShaderRead:
            return {VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT};
        case BufferAccess::ComputeShaderWrite:
            return {VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT};
        case BufferAccess::ComputeShaderReadWrite:
            return {VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
        case BufferAccess::VertexShaderRead:
            return {VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT};
        case BufferAccess::FragmentShaderRead:
            return {VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT};
        case BufferAccess::VertexAttributeRead:
            return {VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT};
        case BufferAccess::IndexRead:
            return {VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, VK_ACCESS_INDEX_READ_BIT};
        case BufferAccess::UniformRead:
            // Uniform buffers can be read from any shader stage. Cover both
            // common stages; consumers needing tighter scope use the
            // VertexShaderRead / FragmentShaderRead / ComputeShaderRead variants.
            return {VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_ACCESS_UNIFORM_READ_BIT};
        case BufferAccess::IndirectRead:
            return {VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, VK_ACCESS_INDIRECT_COMMAND_READ_BIT};
        case BufferAccess::TransferSrc:
            return {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT};
        case BufferAccess::TransferDst:
            return {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT};
        case BufferAccess::HostRead:
            return {VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT};
        case BufferAccess::HostWrite:
            return {VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_WRITE_BIT};
        case BufferAccess::None:
        default:
            return {VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0};
    }
}

[[nodiscard]] VkAttachmentLoadOp to_vk_load_op(LoadOp op) noexcept
{
    switch (op)
    {
        case LoadOp::Load:
            return VK_ATTACHMENT_LOAD_OP_LOAD;
        case LoadOp::Clear:
            return VK_ATTACHMENT_LOAD_OP_CLEAR;
        case LoadOp::DontCare:
        default:
            return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
}

[[nodiscard]] VkAttachmentStoreOp to_vk_store_op(StoreOp op) noexcept
{
    switch (op)
    {
        case StoreOp::Store:
            return VK_ATTACHMENT_STORE_OP_STORE;
        case StoreOp::DontCare:
        default:
            return VK_ATTACHMENT_STORE_OP_DONT_CARE;
    }
}

[[nodiscard]] VkDescriptorType to_vk_descriptor_type(DescriptorType type) noexcept
{
    switch (type)
    {
        case DescriptorType::StorageBuffer:
            return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        case DescriptorType::CombinedImageSampler:
            return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        case DescriptorType::SampledImage:
            return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        case DescriptorType::Sampler:
            return VK_DESCRIPTOR_TYPE_SAMPLER;
        case DescriptorType::StorageImage:
            return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        case DescriptorType::UniformBuffer:
        default:
            return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    }
}

[[nodiscard]] VkShaderStageFlags to_vk_shader_stage_flags(ShaderStage stages) noexcept
{
    VkShaderStageFlags flags = 0;
    if (has_stage(stages, ShaderStage::Vertex))
        flags |= VK_SHADER_STAGE_VERTEX_BIT;
    if (has_stage(stages, ShaderStage::Fragment))
        flags |= VK_SHADER_STAGE_FRAGMENT_BIT;
    if (has_stage(stages, ShaderStage::Compute))
        flags |= VK_SHADER_STAGE_COMPUTE_BIT;
    return flags;
}

[[nodiscard]] bool has_extension(crd::containers::ConstSpan<VkExtensionProperties> exts, const char* name) noexcept
{
    for (const auto& ext : exts)
    {
        if (std::strcmp(ext.extensionName, name) == 0)
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool has_layer(crd::containers::ConstSpan<VkLayerProperties> layers, const char* name) noexcept
{
    for (const auto& layer : layers)
    {
        if (std::strcmp(layer.layerName, name) == 0)
        {
            return true;
        }
    }
    return false;
}

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT /*type*/,
                                              const VkDebugUtilsMessengerCallbackDataEXT* callback_data,
                                              void* /*user_data*/)
{
    const char* message =
        callback_data != nullptr && callback_data->pMessage != nullptr ? callback_data->pMessage : "(null)";
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0)
    {
        CRD_LOG_ERROR(detail::g_log_rhi_vulkan, "{}", message);
    }
    else
    {
        CRD_LOG_WARN(detail::g_log_rhi_vulkan, "{}", message);
    }
    return VK_FALSE;
}

struct FrameSync
{
    VkSemaphore image_available = VK_NULL_HANDLE;
    VkFence in_flight = VK_NULL_HANDLE;
};

struct ImageSync
{
    VkSemaphore render_finished = VK_NULL_HANDLE;
};

// ADR-0085 S6: a sub-block descriptor. `memory`/`offset` is what the resource binds
// to; the rest is free bookkeeping. A zeroed allocation (memory == VK_NULL_HANDLE,
// e.g. a swapchain image) is a no-op to free.
struct VulkanAllocation
{
    VkDeviceMemory memory            = VK_NULL_HANDLE; // owning block (or dedicated) memory
    VkDeviceSize   offset            = 0;              // suballocation offset within `memory`
    VkDeviceSize   size_bytes        = 0;              // requested size
    crd::u32       memory_type_index = 0;
    void*          mapped            = nullptr; // host-visible: persistent block map + offset; else null
    crd::u32       block_index       = 0xFFFFFFFFU; // index into VulkanAllocator::m_blocks; UINT32_MAX => dedicated/none
    crd::memory::OffsetAllocator::Allocation suballoc{}; // for returning the offset to the block's allocator
    bool                                     dedicated = false;
};

class VulkanBuffer; // registered with the allocator for defrag relocation (ADR-0085 S7)
class VulkanImage;

// Pending image relocation captured during the defrag command-recording pass; the
// swap is committed after the single submit+wait.
struct ImageRelocation
{
    VulkanImage*     img            = nullptr;
    VkImage          new_vk         = VK_NULL_HANDLE;
    VkImageView      new_view       = VK_NULL_HANDLE;
    VulkanAllocation new_alloc{};
    VkImage          old_vk         = VK_NULL_HANDLE;
    VkImageView      old_view       = VK_NULL_HANDLE;
    VulkanAllocation old_alloc{};
};

// O(n) swap-remove of a non-owning pointer from a registry array.
template <typename T> void swap_remove(crd::containers::Array<T>& arr, T value) noexcept
{
    for (crd::u32 i = 0; i < static_cast<crd::u32>(arr.size()); ++i)
    {
        if (arr[i] == value)
        {
            arr[i] = arr[arr.size() - 1];
            arr.pop_back();
            return;
        }
    }
}

// VulkanAllocator — VkDeviceMemory SUBALLOCATOR (ADR-0085 S6, supersedes the v1e
// one-allocation-per-resource helper).
//
// Carves large VkDeviceMemory blocks (per memory-type, sized min(256 MiB, heap/8))
// into sub-blocks via crd::memory::OffsetAllocator (O(1), external metadata — the
// reason it can manage device-local VRAM). Allocations >= 16 MiB get a dedicated
// VkDeviceMemory. Pools are keyed by (memoryTypeIndex, linear) so buffers and
// optimal-tiling images never share a block — sidestepping bufferImageGranularity.
// Host-visible blocks are persistently mapped; map() returns (block map + offset).
// Non-coherent host memory rounds suballocation alignment up to nonCoherentAtomSize
// so flush/invalidate ranges stay legal.
//
// Thread-safe: a mutex guards the block pools + per-block OffsetAllocator (the
// resource-create/destroy path, not a per-frame hot path).
//
// LIFETIME: all Buffer/Image instances MUST be destroyed before the owning Device
// (standard Vulkan). Pooled blocks are freed by destroy_all() just before
// vkDestroyDevice; dedicated allocations are freed in the resource's own dtor — so a
// resource outliving the device would vkFreeMemory on a dead device.
class VulkanAllocator final : public IGpuResidencyContext
{
public:
    VulkanAllocator(VkPhysicalDevice physical_device, VkDevice device)
        : m_physical_device(physical_device), m_device(device), m_blocks(crd::memory::default_allocator())
    {
        vkGetPhysicalDeviceMemoryProperties(physical_device, &m_mem_props);
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(physical_device, &props);
        m_non_coherent_atom = props.limits.nonCoherentAtomSize == 0 ? 1 : props.limits.nonCoherentAtomSize;
    }

    ~VulkanAllocator() override { destroy_all(); }

    VulkanAllocator(const VulkanAllocator&)            = delete;
    VulkanAllocator& operator=(const VulkanAllocator&) = delete;

    // Free every block's VkDeviceMemory. MUST be called while the VkDevice is still
    // alive (the owning VulkanDevice calls this before vkDestroyDevice). Idempotent.
    void destroy_all() noexcept
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        for (Block* b : m_blocks)
        {
            destroy_block(b);
        }
        m_blocks.clear();
    }

    [[nodiscard]] bool allocate_buffer(const BufferDesc& desc, VkBuffer& out_buffer, VulkanAllocation& out_allocation)
    {
        VkBufferCreateInfo create_info{};
        create_info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        create_info.pNext       = nullptr;
        create_info.size        = desc.size_bytes;
        create_info.usage       = to_vk_buffer_usage(desc.usage);
        create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (!vk_ok(vkCreateBuffer(m_device, &create_info, nullptr, &out_buffer), "vkCreateBuffer"))
        {
            return false;
        }

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(m_device, out_buffer, &requirements);
        // Buffers are always linear.
        if (!sub_allocate(requirements, to_vk_memory_properties(desc.memory_usage), true, out_allocation,
                          "buffer"))
        {
            vkDestroyBuffer(m_device, out_buffer, nullptr);
            out_buffer = VK_NULL_HANDLE;
            return false;
        }

        if (!vk_ok(vkBindBufferMemory(m_device, out_buffer, out_allocation.memory, out_allocation.offset),
                   "vkBindBufferMemory"))
        {
            free(out_allocation);
            out_allocation = {};
            vkDestroyBuffer(m_device, out_buffer, nullptr);
            out_buffer = VK_NULL_HANDLE;
            return false;
        }

        return true;
    }

    [[nodiscard]] bool allocate_image(const ImageDesc& desc, VkImage& out_image, VulkanAllocation& out_allocation)
    {
        VkImageUsageFlags usage = 0;
        if (has_flag(desc.usage, ImageUsage::TransferSrc))
            usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (has_flag(desc.usage, ImageUsage::TransferDst))
            usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (has_flag(desc.usage, ImageUsage::Sampled))
            usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
        if (has_flag(desc.usage, ImageUsage::ColorAttachment))
            usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (has_flag(desc.usage, ImageUsage::DepthStencilAttachment))
            usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

        VkImageCreateInfo create_info{};
        create_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        create_info.imageType     = VK_IMAGE_TYPE_2D;
        create_info.format        = to_vk_format(desc.format);
        create_info.extent        = {desc.extent.width, desc.extent.height, 1};
        create_info.mipLevels     = desc.mip_levels;
        create_info.arrayLayers   = desc.array_layers;
        create_info.samples       = VK_SAMPLE_COUNT_1_BIT;
        create_info.tiling        = VK_IMAGE_TILING_OPTIMAL;
        create_info.usage         = usage;
        create_info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (!vk_ok(vkCreateImage(m_device, &create_info, nullptr, &out_image), "vkCreateImage"))
        {
            return false;
        }

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(m_device, out_image, &requirements);
        // Optimal-tiling images are non-linear -> their own pools (granularity-safe).
        if (!sub_allocate(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false, out_allocation, "image"))
        {
            vkDestroyImage(m_device, out_image, nullptr);
            out_image = VK_NULL_HANDLE;
            return false;
        }

        if (!vk_ok(vkBindImageMemory(m_device, out_image, out_allocation.memory, out_allocation.offset),
                   "vkBindImageMemory"))
        {
            free(out_allocation);
            out_allocation = {};
            vkDestroyImage(m_device, out_image, nullptr);
            out_image = VK_NULL_HANDLE;
            return false;
        }

        return true;
    }

    // Return a suballocation to its block (or free a dedicated allocation). Idempotent
    // on a zeroed allocation (swapchain images carry no allocation).
    void free(const VulkanAllocation& allocation) noexcept
    {
        if (allocation.memory == VK_NULL_HANDLE)
        {
            return;
        }
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (is_device_local(allocation.memory_type_index))
        {
            m_device_local_used.fetch_sub(allocation.size_bytes, std::memory_order_relaxed);
        }
        if (allocation.dedicated)
        {
            vkFreeMemory(m_device, allocation.memory, nullptr); // implicitly unmaps
            return;
        }
        Block* b = m_blocks[allocation.block_index];
        b->oa.free(allocation.suballoc);
        --b->live_count;
    }

    // Back-compat shim for create_image's error path.
    void destroy_allocation(VulkanAllocation& allocation) noexcept
    {
        free(allocation);
        allocation = {};
    }

    // Diagnostic: number of VkDeviceMemory blocks (incl. dedicated are not counted —
    // they aren't pooled). A small count across many small allocations proves
    // suballocation is happening.
    [[nodiscard]] crd::u32 block_count() const noexcept { return static_cast<crd::u32>(m_blocks.size()); }

    // ---- Live-resource registry (ADR-0085 S7: enables defrag relocation) -------
    void register_buffer(VulkanBuffer* b)
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_live_buffers.push_back(b);
    }
    void unregister_buffer(VulkanBuffer* b) noexcept
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        swap_remove(m_live_buffers, b);
    }
    void register_image(VulkanImage* i)
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_live_images.push_back(i);
    }
    void unregister_image(VulkanImage* i) noexcept
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        swap_remove(m_live_images, i);
    }

    // ADR-0085 S7 defrag pass — idle-gated, one submit. Relocates the buffers/images
    // the policy selects to compacted locations (recreate + transfer-copy + swap the
    // resource's internal handle), then fires on_relocated callbacks OUTSIDE the lock.
    // Defined out-of-line (needs complete VulkanBuffer/VulkanImage). MUST be called
    // single-threaded with no other allocation/use in flight (idle-gated contract).
    void defragment(IDefragPolicy& policy, VkQueue queue, VkCommandPool pool);

    // ---- ADR-0085 S7 residency ------------------------------------------------
    // The transfer queue + pool used by relocation copies (set by the owning device
    // once its command pool exists). The residency policy + soft device-local budget
    // drive the auto-pressure loop in sub_allocate (mirrors the S5 StreamingAllocator).
    void set_transfer_context(VkQueue queue, VkCommandPool pool) noexcept
    {
        m_queue = queue;
        m_pool  = pool;
    }
    void set_residency(IResidencyPolicy* policy, crd::u64 device_local_budget) noexcept
    {
        m_residency_policy    = policy != nullptr ? policy : &m_null_residency;
        m_device_local_budget = device_local_budget;
    }

    // IGpuResidencyContext (the policy acts on the allocator through this).
    [[nodiscard]] crd::u64 device_local_used() const noexcept override
    {
        return m_device_local_used.load(std::memory_order_relaxed);
    }
    [[nodiscard]] crd::u64 device_local_budget() const noexcept override { return m_device_local_budget; }
    [[nodiscard]] crd::u32 resident_buffer_count() const noexcept override;
    [[nodiscard]] Buffer*  resident_buffer_at(crd::u32 index) noexcept override;
    [[nodiscard]] crd::u64 evict_to_host(Buffer& buffer) override;

    // Re-promote an evicted buffer host-visible -> device-local (consumer "on access").
    crd::u64 make_resident(Buffer& buffer);

private:
    struct Block
    {
        VkDeviceMemory               memory = VK_NULL_HANDLE;
        VkDeviceSize                 size   = 0;
        crd::u32                     memory_type_index = 0;
        bool                         linear            = false;
        void*                        mapped            = nullptr;
        crd::u32                     live_count        = 0;
        crd::memory::OffsetAllocator oa; // by value: Block is heap-allocated and never moved

        Block(VkDeviceMemory mem, VkDeviceSize sz, crd::u32 mti, bool lin, void* map_ptr, crd::u32 cap)
            : memory(mem), size(sz), memory_type_index(mti), linear(lin), mapped(map_ptr),
              oa(cap, 4096U, crd::memory::default_allocator(), "GpuBlock")
        {
        }
    };

    [[nodiscard]] bool host_visible(crd::u32 mti) const noexcept
    {
        return (m_mem_props.memoryTypes[mti].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
    }
    [[nodiscard]] bool host_coherent(crd::u32 mti) const noexcept
    {
        return (m_mem_props.memoryTypes[mti].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    }
    [[nodiscard]] bool is_device_local(crd::u32 mti) const noexcept
    {
        return (m_mem_props.memoryTypes[mti].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
    }

    // Recreate `b` in a pool backed by `target_usage` memory, copy its contents over,
    // swap the internal handle (idle-gated single-shot copy). Returns the relocated
    // size in bytes (0 on failure). Shared by evict_to_host + make_resident.
    crd::u64 relocate_buffer_pool(VulkanBuffer& b, MemoryUsage target_usage);

    [[nodiscard]] VkDeviceSize block_size_for(crd::u32 mti) const noexcept
    {
        const crd::u32     heap_index = m_mem_props.memoryTypes[mti].heapIndex;
        const VkDeviceSize heap_size  = m_mem_props.memoryHeaps[heap_index].size;
        VkDeviceSize       block      = std::min<VkDeviceSize>(kMaxBlockSize, heap_size / 8);
        if (block < kMinBlockSize)
        {
            block = kMinBlockSize;
        }
        return block;
    }

    [[nodiscard]] bool sub_allocate(const VkMemoryRequirements& reqs, VkMemoryPropertyFlags required, bool linear,
                                    VulkanAllocation& out, const char* what)
    {
        const crd::u32 mti = find_memory_type(m_physical_device, reqs.memoryTypeBits, required);
        if (mti == UINT32_MAX)
        {
            CRD_LOG_ERROR(detail::g_log_rhi_vulkan, "No compatible Vulkan memory type for {}", what);
            return false;
        }

        VkDeviceSize alignment = reqs.alignment == 0 ? 1 : reqs.alignment;
        // Non-coherent host memory: flush/invalidate ranges must be nonCoherentAtomSize
        // aligned, so the sub-block must start (and the next must too) on that boundary.
        if (host_visible(mti) && !host_coherent(mti))
        {
            alignment = std::max<VkDeviceSize>(alignment, m_non_coherent_atom);
        }

        // S7 residency pressure (mirrors the S5 StreamingAllocator): a device-local
        // allocation over the soft budget asks the policy to evict device-local
        // resources to host, OUTSIDE the lock, until it fits or the policy can shed no
        // more (then we proceed — the budget is soft; physical VRAM is the hard limit).
        // Note: the policy's evict path re-enters sub_allocate via allocate_buffer(host)
        // to create the host-visible destination. That recursion TERMINATES because the
        // host allocation is not device-local — this gate is skipped on the inner call.
        const bool dev_local = is_device_local(mti);
        if (dev_local && m_device_local_budget != 0)
        {
            for (int attempt = 0; attempt <= 16; ++attempt)
            {
                if (m_device_local_used.load(std::memory_order_relaxed) + reqs.size <= m_device_local_budget)
                {
                    break;
                }
                if (m_residency_policy->evict(*this, reqs.size) == 0)
                {
                    break;
                }
            }
        }

        const std::lock_guard<std::mutex> lock(m_mutex);

        // Large allocations get their own VkDeviceMemory (no suballocation).
        if (reqs.size >= kDedicatedThreshold)
        {
            VkDeviceMemory mem = allocate_device_memory(reqs.size, mti, what);
            if (mem == VK_NULL_HANDLE)
            {
                return false;
            }
            void* mapped = nullptr;
            if (host_visible(mti))
            {
                (void)vkMapMemory(m_device, mem, 0, VK_WHOLE_SIZE, 0, &mapped);
            }
            out                   = {};
            out.memory            = mem;
            out.offset            = 0;
            out.size_bytes        = reqs.size;
            out.memory_type_index = mti;
            out.mapped            = mapped;
            out.dedicated         = true;
            if (dev_local)
            {
                m_device_local_used.fetch_add(reqs.size, std::memory_order_relaxed);
            }
            return true;
        }

        // Suballocate within an existing (or new) block of the right pool.
        const auto size32 = static_cast<crd::u32>(reqs.size);
        const auto algn32 = static_cast<crd::u32>(alignment);
        for (crd::u32 i = 0; i < static_cast<crd::u32>(m_blocks.size()); ++i)
        {
            Block* b = m_blocks[i];
            if (b->memory_type_index != mti || b->linear != linear)
            {
                continue;
            }
            const crd::memory::OffsetAllocator::Allocation a = b->oa.allocate(size32, algn32);
            if (a.valid())
            {
                fill_suballocation(out, *b, i, a, reqs.size, mti);
                return true;
            }
        }

        // No block fit -> grow one big enough for at least this request.
        Block* nb = create_block(mti, linear, reqs.size);
        if (nb == nullptr)
        {
            return false;
        }
        const crd::memory::OffsetAllocator::Allocation a = nb->oa.allocate(size32, algn32);
        if (!a.valid())
        {
            return false; // a fresh block should always fit a sub-dedicated-threshold request
        }
        fill_suballocation(out, *nb, static_cast<crd::u32>(m_blocks.size()) - 1U, a, reqs.size, mti);
        return true;
    }

    void fill_suballocation(VulkanAllocation& out, Block& b, crd::u32 block_index,
                            crd::memory::OffsetAllocator::Allocation a, VkDeviceSize size, crd::u32 mti) noexcept
    {
        out                   = {};
        out.memory            = b.memory;
        out.offset            = a.offset;
        out.size_bytes        = size;
        out.memory_type_index = mti;
        out.block_index       = block_index;
        out.suballoc          = a;
        out.dedicated         = false;
        out.mapped            = b.mapped != nullptr ? static_cast<crd::u8*>(b.mapped) + a.offset : nullptr;
        ++b.live_count;
        if (is_device_local(mti))
        {
            m_device_local_used.fetch_add(size, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] Block* create_block(crd::u32 mti, bool linear, VkDeviceSize min_size) noexcept
    {
        VkDeviceSize block_size = block_size_for(mti);
        if (block_size < min_size)
        {
            block_size = min_size;
        }
        VkDeviceMemory mem = allocate_device_memory(block_size, mti, "block");
        if (mem == VK_NULL_HANDLE)
        {
            return nullptr;
        }
        void* mapped = nullptr;
        if (host_visible(mti))
        {
            (void)vkMapMemory(m_device, mem, 0, VK_WHOLE_SIZE, 0, &mapped);
        }
        crd::memory::IAllocator* alloc = crd::memory::default_allocator();
        void*                    raw   = alloc->allocate(sizeof(Block), alignof(Block));
        Block*                   b     = new (raw) Block(mem, block_size, mti, linear, mapped,
                                                         static_cast<crd::u32>(block_size));
        m_blocks.push_back(b);
        return b;
    }

    void destroy_block(Block* b) noexcept
    {
        if (b == nullptr)
        {
            return;
        }
        if (b->memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(m_device, b->memory, nullptr); // implicitly unmaps a persistent map
        }
        crd::memory::IAllocator* alloc = crd::memory::default_allocator();
        b->~Block();
        alloc->deallocate(b);
    }

    [[nodiscard]] VkDeviceMemory allocate_device_memory(VkDeviceSize size, crd::u32 memory_type_index,
                                                        const char* what) noexcept
    {
        VkMemoryAllocateInfo alloc_info{};
        alloc_info.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc_info.allocationSize  = size;
        alloc_info.memoryTypeIndex = memory_type_index;

        VkDeviceMemory memory = VK_NULL_HANDLE;
        if (!vk_ok(vkAllocateMemory(m_device, &alloc_info, nullptr, &memory), what))
        {
            return VK_NULL_HANDLE;
        }
        return memory;
    }

    // Allocations >= this get their own VkDeviceMemory. A future render/compute
    // profile Config will expose this as a tunable knob (consumer-pulled).
    // ADR-0085 S7 image defrag (defined out-of-line; need complete VulkanImage):
    // record_* recreates image+view at a compacted location and records the
    // subresource copy + layout transitions; commit_* swaps the handles + frees old.
    void record_image_relocation(VkCommandBuffer cmd, VulkanImage& img, crd::containers::Array<ImageRelocation>& out);
    void commit_image_relocation(ImageRelocation& r) noexcept;

    static constexpr VkDeviceSize kDedicatedThreshold = VkDeviceSize{16} << 20; // 16 MiB
    static constexpr VkDeviceSize kMaxBlockSize       = VkDeviceSize{256} << 20; // 256 MiB
    static constexpr VkDeviceSize kMinBlockSize       = VkDeviceSize{16} << 20;  // 16 MiB floor

    VkPhysicalDevice                 m_physical_device = VK_NULL_HANDLE;
    VkDevice                         m_device          = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties m_mem_props{};
    VkDeviceSize                     m_non_coherent_atom = 1;
    mutable std::mutex               m_mutex;
    crd::containers::Array<Block*>   m_blocks;
    crd::containers::Array<VulkanBuffer*> m_live_buffers{crd::memory::default_allocator()};
    crd::containers::Array<VulkanImage*>  m_live_images{crd::memory::default_allocator()};

    // ---- S7 residency state ---------------------------------------------------
    VkQueue               m_queue = VK_NULL_HANDLE; // transfer/graphics queue for relocation copies
    VkCommandPool         m_pool  = VK_NULL_HANDLE;
    NullResidencyPolicy   m_null_residency;
    IResidencyPolicy*     m_residency_policy = &m_null_residency;
    std::atomic<crd::u64> m_device_local_used{0};      // bytes in DEVICE_LOCAL memory types
    crd::u64              m_device_local_budget = 0;   // soft ceiling; 0 == unlimited
};

class VulkanImage final : public Image
{
public:
    VulkanImage(VkDevice device, ImageDesc desc, VkImage image, VkImageView image_view,
                VulkanAllocation allocation = {}, bool owns_image = false, VulkanAllocator* allocator = nullptr)
        : m_device(device), m_desc(std::move(desc)), m_image(image), m_image_view(image_view), m_allocation(allocation),
          m_owns_image(owns_image), m_allocator(allocator)
    {
        if (m_allocator != nullptr)
        {
            m_allocator->register_image(this);
        }
    }

    ~VulkanImage() noexcept override
    {
        if (m_allocator != nullptr)
        {
            m_allocator->unregister_image(this);
        }
        if (m_image_view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(m_device, m_image_view, nullptr);
            m_image_view = VK_NULL_HANDLE;
        }
        if (m_owns_image && m_image != VK_NULL_HANDLE)
        {
            vkDestroyImage(m_device, m_image, nullptr);
            m_image = VK_NULL_HANDLE;
        }
        // Suballocated images return their sub-block to the allocator; swapchain
        // images (m_allocator == nullptr) own no memory.
        if (m_allocator != nullptr)
        {
            m_allocator->free(m_allocation);
            m_allocation = {};
        }
    }

    [[nodiscard]] const ImageDesc& desc() const noexcept override { return m_desc; }
    [[nodiscard]] VkImage handle() const noexcept { return m_image; }
    [[nodiscard]] VkImageView image_view() const noexcept { return m_image_view; }
    [[nodiscard]] VkImageLayout layout() const noexcept { return m_layout; }
    void set_layout(VkImageLayout layout) noexcept { m_layout = layout; }
    [[nodiscard]] crd::u32 generation() const noexcept { return m_generation; }

private:
    VkDevice m_device = VK_NULL_HANDLE;
    ImageDesc m_desc{};
    VkImage m_image = VK_NULL_HANDLE;
    VkImageView m_image_view = VK_NULL_HANDLE;
    VkImageLayout m_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VulkanAllocation m_allocation{};
    bool m_owns_image = false;
    VulkanAllocator* m_allocator = nullptr; // non-null => suballocated; free via it on destroy
    crd::u32 m_generation = 0;              // bumped on defrag relocation (ADR-0085 S7)

    friend class VulkanAllocator; // defrag swaps m_image/m_image_view/m_allocation + bumps m_generation
};

class VulkanBuffer final : public Buffer
{
public:
    VulkanBuffer(VkDevice device, BufferDesc desc, VkBuffer buffer, VulkanAllocation allocation,
                 VulkanAllocator* allocator)
        : m_device(device), m_desc(desc), m_buffer(buffer), m_allocation(allocation), m_allocator(allocator)
    {
        if (m_allocator != nullptr)
        {
            m_allocator->register_buffer(this);
        }
    }

    ~VulkanBuffer() noexcept override
    {
        if (m_allocator != nullptr)
        {
            m_allocator->unregister_buffer(this);
        }
        if (m_buffer != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(m_device, m_buffer, nullptr);
            m_buffer = VK_NULL_HANDLE;
        }
        if (m_allocator != nullptr)
        {
            m_allocator->free(m_allocation);
            m_allocation = {};
        }
    }

    [[nodiscard]] crd::u32 generation() const noexcept { return m_generation; }

    [[nodiscard]] const BufferDesc& desc() const noexcept override { return m_desc; }

    // Host-visible blocks are persistently mapped; the allocation's `mapped` is the
    // block map + this buffer's offset. GpuOnly allocations carry mapped == nullptr.
    [[nodiscard]] void* map() noexcept override { return m_allocation.mapped; }

    // No-op: the underlying block stays mapped for its lifetime (persistent mapping).
    void unmap() noexcept override {}

    [[nodiscard]] VkBuffer handle() const noexcept { return m_buffer; }

private:
    VkDevice m_device = VK_NULL_HANDLE;
    BufferDesc m_desc{};
    VkBuffer m_buffer = VK_NULL_HANDLE;
    VulkanAllocation m_allocation{};
    VulkanAllocator* m_allocator = nullptr;
    crd::u32 m_generation = 0; // bumped on defrag relocation (ADR-0085 S7)

    friend class VulkanAllocator; // defrag swaps m_buffer/m_allocation + bumps m_generation
};

// One-shot transfer: record into a primary command buffer, submit, wait idle, free.
// Idle-gated relocation uses this (one submit per defrag pass — not per resource).
template <typename RecordFn>
void submit_one_shot(VkDevice device, VkQueue queue, VkCommandPool pool, RecordFn&& record) noexcept
{
    VkCommandBufferAllocateInfo ai{};
    ai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool        = pool;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd   = VK_NULL_HANDLE;
    if (!vk_ok(vkAllocateCommandBuffers(device, &ai, &cmd), "vkAllocateCommandBuffers(relocate)"))
    {
        return;
    }
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    std::forward<RecordFn>(record)(cmd);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{};
    si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &cmd;
    static_cast<void>(vk_ok(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit(relocate)"));
    vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(device, pool, 1, &cmd);
}

[[nodiscard]] VkImageAspectFlags image_aspect_for(Format format) noexcept
{
    if (format == Format::D24UnormS8Uint)
    {
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    if (format == Format::D32Sfloat)
    {
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    }
    return VK_IMAGE_ASPECT_COLOR_BIT;
}

// Recreate an image view matching create_image's logic (aspect + view type).
[[nodiscard]] VkImageView make_image_view(VkDevice device, VkImage image, const ImageDesc& desc) noexcept
{
    VkImageViewCreateInfo vi{};
    vi.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image            = image;
    vi.viewType         = desc.array_layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    vi.format           = to_vk_format(desc.format);
    vi.subresourceRange = {image_aspect_for(desc.format), 0, desc.mip_levels, 0, desc.array_layers};
    VkImageView view    = VK_NULL_HANDLE;
    static_cast<void>(vk_ok(vkCreateImageView(device, &vi, nullptr, &view), "vkCreateImageView(relocate)"));
    return view;
}

void image_layout_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                          VkImageAspectFlags aspect, crd::u32 mips, crd::u32 layers, VkAccessFlags src_access,
                          VkAccessFlags dst_access, VkPipelineStageFlags src_stage,
                          VkPipelineStageFlags dst_stage) noexcept
{
    VkImageMemoryBarrier b{};
    b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout           = from;
    b.newLayout           = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image               = image;
    b.subresourceRange    = {aspect, 0, mips, 0, layers};
    b.srcAccessMask       = src_access;
    b.dstAccessMask       = dst_access;
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

void VulkanAllocator::defragment(IDefragPolicy& policy, VkQueue queue, VkCommandPool pool)
{
    vkDeviceWaitIdle(m_device);

    // Snapshot the registries under the lock; operate on the snapshot afterward
    // (allocate_*/free re-lock internally — defrag is single-threaded by contract).
    crd::containers::Array<VulkanBuffer*> buffers(crd::memory::default_allocator());
    crd::containers::Array<VulkanImage*>  images(crd::memory::default_allocator());
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        for (VulkanBuffer* b : m_live_buffers)
        {
            buffers.push_back(b);
        }
        for (VulkanImage* i : m_live_images)
        {
            images.push_back(i);
        }
    }

    struct BufReloc
    {
        VulkanBuffer*    b;
        VkBuffer         new_vk;
        VulkanAllocation new_alloc;
        VkBuffer         old_vk;
        VulkanAllocation old_alloc;
    };
    crd::containers::Array<BufReloc> buf_relocs(crd::memory::default_allocator());
    crd::containers::Array<ImageRelocation> img_relocs(crd::memory::default_allocator());

    submit_one_shot(m_device, queue, pool, [&](VkCommandBuffer cmd) {
        for (VulkanBuffer* b : buffers)
        {
            if (!policy.should_defrag(static_cast<const Buffer&>(*b)))
            {
                continue;
            }
            VkBuffer         new_vk = VK_NULL_HANDLE;
            VulkanAllocation new_alloc;
            if (!allocate_buffer(b->m_desc, new_vk, new_alloc))
            {
                continue; // no room to relocate — leave it where it is
            }
            VkBufferCopy region{};
            region.size = b->m_desc.size_bytes;
            vkCmdCopyBuffer(cmd, b->m_buffer, new_vk, 1, &region);
            buf_relocs.push_back(BufReloc{b, new_vk, new_alloc, b->m_buffer, b->m_allocation});
        }
        for (VulkanImage* im : images)
        {
            if (!policy.should_defrag(static_cast<const Image&>(*im)))
            {
                continue;
            }
            record_image_relocation(cmd, *im, img_relocs);
        }
    });

    // submit_one_shot already waited idle -> safe to swap handles + free old.
    for (BufReloc& r : buf_relocs)
    {
        r.b->m_buffer     = r.new_vk;
        r.b->m_allocation = r.new_alloc;
        ++r.b->m_generation;
        vkDestroyBuffer(m_device, r.old_vk, nullptr);
        free(r.old_alloc);
    }
    for (ImageRelocation& r : img_relocs)
    {
        commit_image_relocation(r);
    }

    // Fire callbacks OUTSIDE the lock (consumers re-bind descriptors here).
    for (BufReloc& r : buf_relocs)
    {
        policy.on_relocated(static_cast<const Buffer&>(*r.b), r.b->m_generation);
    }
    for (ImageRelocation& r : img_relocs)
    {
        policy.on_relocated(static_cast<const Image&>(*r.img), r.img->m_generation);
    }
}

void VulkanAllocator::record_image_relocation(VkCommandBuffer cmd, VulkanImage& img,
                                              crd::containers::Array<ImageRelocation>& out)
{
    // UNDEFINED == no content worth preserving (and copying from UNDEFINED is invalid).
    if (img.m_layout == VK_IMAGE_LAYOUT_UNDEFINED)
    {
        return;
    }
    const ImageDesc& desc    = img.m_desc;
    VkImage          new_img = VK_NULL_HANDLE;
    VulkanAllocation new_alloc;
    if (!allocate_image(desc, new_img, new_alloc))
    {
        return; // no room to relocate
    }
    const VkImageView new_view = make_image_view(m_device, new_img, desc);
    if (new_view == VK_NULL_HANDLE)
    {
        vkDestroyImage(m_device, new_img, nullptr);
        free(new_alloc);
        return;
    }

    const VkImageAspectFlags aspect     = image_aspect_for(desc.format);
    const VkImageLayout      src_layout = img.m_layout;

    image_layout_barrier(cmd, img.m_image, src_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, aspect, desc.mip_levels,
                         desc.array_layers, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    image_layout_barrier(cmd, new_img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, aspect,
                         desc.mip_levels, desc.array_layers, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    // Copy every mip level (all array layers per level).
    constexpr crd::u32 max_mips = 16;
    VkImageCopy        copies[max_mips];
    const crd::u32     mips = desc.mip_levels < max_mips ? desc.mip_levels : max_mips;
    for (crd::u32 m = 0; m < mips; ++m)
    {
        VkImageCopy c{};
        c.srcSubresource    = {aspect, m, 0, desc.array_layers};
        c.dstSubresource    = {aspect, m, 0, desc.array_layers};
        const crd::u32 w    = desc.extent.width >> m;
        const crd::u32 h    = desc.extent.height >> m;
        c.extent            = {w == 0 ? 1U : w, h == 0 ? 1U : h, 1U};
        copies[m]           = c;
    }
    vkCmdCopyImage(cmd, img.m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, new_img,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, mips, copies);

    // Restore the new image to the layout the consumer expects (preserves the
    // m_layout invariant — consumers see no layout change).
    image_layout_barrier(cmd, new_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, src_layout, aspect, desc.mip_levels,
                         desc.array_layers, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

    out.push_back(ImageRelocation{&img, new_img, new_view, new_alloc, img.m_image, img.m_image_view, img.m_allocation});
}

void VulkanAllocator::commit_image_relocation(ImageRelocation& r) noexcept
{
    r.img->m_image      = r.new_vk;
    r.img->m_image_view = r.new_view;
    r.img->m_allocation = r.new_alloc;
    // m_layout is unchanged (the new image was restored to it during recording).
    ++r.img->m_generation;
    vkDestroyImageView(m_device, r.old_view, nullptr);
    vkDestroyImage(m_device, r.old_vk, nullptr);
    free(r.old_alloc);
}

// ---- ADR-0085 S7 residency (out-of-line; need complete VulkanBuffer) -----------

crd::u32 VulkanAllocator::resident_buffer_count() const noexcept
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    crd::u32                          count = 0;
    for (const VulkanBuffer* b : m_live_buffers)
    {
        if (is_device_local(b->m_allocation.memory_type_index))
        {
            ++count;
        }
    }
    return count;
}

Buffer* VulkanAllocator::resident_buffer_at(crd::u32 index) noexcept
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    crd::u32                          seen = 0;
    for (VulkanBuffer* b : m_live_buffers)
    {
        if (is_device_local(b->m_allocation.memory_type_index))
        {
            if (seen == index)
            {
                return static_cast<Buffer*>(b);
            }
            ++seen;
        }
    }
    return nullptr;
}

crd::u64 VulkanAllocator::relocate_buffer_pool(VulkanBuffer& b, MemoryUsage target_usage)
{
    if (m_queue == VK_NULL_HANDLE || m_pool == VK_NULL_HANDLE)
    {
        return 0; // transfer context not configured
    }
    BufferDesc new_desc   = b.m_desc;
    new_desc.memory_usage = target_usage;

    VkBuffer         new_vk = VK_NULL_HANDLE;
    VulkanAllocation new_alloc;
    if (!allocate_buffer(new_desc, new_vk, new_alloc))
    {
        return 0; // no room in the target pool
    }

    vkDeviceWaitIdle(m_device);
    submit_one_shot(m_device, m_queue, m_pool, [&](VkCommandBuffer cmd) {
        VkBufferCopy region{};
        region.size = b.m_desc.size_bytes;
        vkCmdCopyBuffer(cmd, b.m_buffer, new_vk, 1, &region);
    });

    const crd::u64   moved     = b.m_allocation.size_bytes;
    VkBuffer         old_vk    = b.m_buffer;
    VulkanAllocation old_alloc = b.m_allocation;
    b.m_buffer                 = new_vk;
    b.m_allocation             = new_alloc;
    b.m_desc                   = new_desc; // residency state lives in the desc's memory_usage
    ++b.m_generation;
    vkDestroyBuffer(m_device, old_vk, nullptr);
    free(old_alloc); // device_local_used bookkeeping rides on allocate_buffer + free
    return moved;
}

crd::u64 VulkanAllocator::evict_to_host(Buffer& buffer)
{
    auto& vb = static_cast<VulkanBuffer&>(buffer);
    if (!is_device_local(vb.m_allocation.memory_type_index))
    {
        return 0; // already host-resident
    }
    return relocate_buffer_pool(vb, MemoryUsage::CpuToGpu); // host-visible|coherent
}

crd::u64 VulkanAllocator::make_resident(Buffer& buffer)
{
    auto& vb = static_cast<VulkanBuffer&>(buffer);
    if (is_device_local(vb.m_allocation.memory_type_index))
    {
        return 0; // already device-resident
    }
    return relocate_buffer_pool(vb, MemoryUsage::GpuOnly);
}

class VulkanShaderModule final : public ShaderModule
{
public:
    VulkanShaderModule(VkDevice device, ShaderModuleDesc desc, VkShaderModule module)
        : m_device(device), m_stage(desc.stage), m_entry_point(desc.entry_point), m_module(module)
    {
    }

    ~VulkanShaderModule() noexcept override
    {
        if (m_module != VK_NULL_HANDLE)
        {
            vkDestroyShaderModule(m_device, m_module, nullptr);
            m_module = VK_NULL_HANDLE;
        }
    }

    [[nodiscard]] ShaderStage stage() const noexcept override { return m_stage; }
    [[nodiscard]] crd::containers::StringView entry_point() const noexcept override { return m_entry_point; }
    [[nodiscard]] VkShaderModule handle() const noexcept { return m_module; }

private:
    VkDevice m_device = VK_NULL_HANDLE;
    ShaderStage m_stage = ShaderStage::Vertex;
    crd::containers::String m_entry_point{};
    VkShaderModule m_module = VK_NULL_HANDLE;
};

class VulkanDescriptorSetLayout final : public DescriptorSetLayout
{
public:
    VulkanDescriptorSetLayout(VkDevice device, const DescriptorSetLayoutDesc& desc, VkDescriptorSetLayout layout)
        : m_device(device), m_layout(layout)
    {
        // Deep-copy the binding data into owned storage. `desc.bindings` is a
        // ConstSpan (non-owning view); the canonical caller pattern at
        // forward_render_path.cpp:32-35 builds the desc with a span pointing
        // to a stack-local `DescriptorBinding`, then lets the local fall out
        // of scope before the layout is queried. Without this copy, every
        // later `desc().bindings` access would read freed stack memory —
        // ASan surfaced exactly this in the win-asan sandbox-smoke on
        // 2026-05-11 (vulkan_backend.cpp:711, push_back from
        // VulkanDescriptorAllocator::allocate).
        m_bindings_owned = crd::containers::Array<DescriptorBinding>(desc.bindings.size());
        for (const auto& b : desc.bindings)
        {
            m_bindings_owned.push_back(b);
        }
        m_desc.bindings = crd::containers::make_span(m_bindings_owned.data(),
                                                     m_bindings_owned.size());
    }

    ~VulkanDescriptorSetLayout() noexcept override
    {
        if (m_layout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(m_device, m_layout, nullptr);
            m_layout = VK_NULL_HANDLE;
        }
    }

    [[nodiscard]] const DescriptorSetLayoutDesc& desc() const noexcept override { return m_desc; }
    [[nodiscard]] VkDescriptorSetLayout handle() const noexcept { return m_layout; }

private:
    VkDevice m_device = VK_NULL_HANDLE;
    crd::containers::Array<DescriptorBinding> m_bindings_owned{};
    DescriptorSetLayoutDesc m_desc{};
    VkDescriptorSetLayout m_layout = VK_NULL_HANDLE;
};

class VulkanPipelineLayout final : public PipelineLayout
{
public:
    VulkanPipelineLayout(VkDevice device, const PipelineLayoutDesc& desc, VkPipelineLayout layout)
        : m_device(device), m_layout(layout)
    {
        // Deep-copy both spans into owned storage — same lifetime hazard as
        // VulkanDescriptorSetLayout (callers pass stack-locals via spans,
        // expecting the layout object to own its desc). Fix companion to
        // the 2026-05-11 ASan finding in VulkanDescriptorSetLayout.
        m_set_layouts_owned = crd::containers::Array<const DescriptorSetLayout*>(desc.set_layouts.size());
        for (const auto* sl : desc.set_layouts)
        {
            m_set_layouts_owned.push_back(sl);
        }
        m_push_ranges_owned = crd::containers::Array<PushConstantRange>(desc.push_constant_ranges.size());
        for (const auto& r : desc.push_constant_ranges)
        {
            m_push_ranges_owned.push_back(r);
        }
        m_desc.set_layouts = crd::containers::make_span(m_set_layouts_owned.data(),
                                                        m_set_layouts_owned.size());
        m_desc.push_constant_ranges = crd::containers::make_span(m_push_ranges_owned.data(),
                                                                 m_push_ranges_owned.size());
    }

    ~VulkanPipelineLayout() noexcept override
    {
        if (m_layout != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(m_device, m_layout, nullptr);
            m_layout = VK_NULL_HANDLE;
        }
    }

    [[nodiscard]] const PipelineLayoutDesc& desc() const noexcept override { return m_desc; }
    [[nodiscard]] VkPipelineLayout handle() const noexcept { return m_layout; }

private:
    VkDevice m_device = VK_NULL_HANDLE;
    crd::containers::Array<const DescriptorSetLayout*> m_set_layouts_owned{};
    crd::containers::Array<PushConstantRange> m_push_ranges_owned{};
    PipelineLayoutDesc m_desc{};
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
};

class VulkanDescriptorSet final : public DescriptorSet
{
public:
    VulkanDescriptorSet(VkDevice device, VkDescriptorSet set, crd::containers::Array<DescriptorBinding> bindings)
        : m_device(device), m_set(set), m_bindings(std::move(bindings))
    {
    }

    // The VkDescriptorSet is owned by its pool and not freed individually —
    // the pool is reset en-masse in VulkanDescriptorAllocator::begin_frame().
    ~VulkanDescriptorSet() noexcept override = default;

    void update_buffer(crd::u32 binding_slot, Buffer& buffer, crd::u64 offset_bytes, crd::u64 size_bytes) override
    {
        auto* vk_buffer = dynamic_cast<VulkanBuffer*>(&buffer);
        CRD_ASSERT(vk_buffer != nullptr);

        VkDescriptorType vk_type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        for (const auto& b : m_bindings)
        {
            if (b.binding == binding_slot)
            {
                vk_type = to_vk_descriptor_type(b.type);
                break;
            }
        }

        VkDescriptorBufferInfo buffer_info{};
        buffer_info.buffer = vk_buffer->handle();
        buffer_info.offset = offset_bytes;
        buffer_info.range = (size_bytes == 0) ? VK_WHOLE_SIZE : size_bytes;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = m_set;
        write.dstBinding = binding_slot;
        write.dstArrayElement = 0;
        write.descriptorCount = 1;
        write.descriptorType = vk_type;
        write.pBufferInfo = &buffer_info;

        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
    }

    [[nodiscard]] VkDescriptorSet handle() const noexcept { return m_set; }

private:
    VkDevice m_device = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;
    crd::containers::Array<DescriptorBinding> m_bindings{};
};

// Ring-buffer descriptor allocator.
//
// Internally: `frames_in_flight` VkDescriptorPools. begin_frame(i) resets
// pool[i % N], making all sets from that pool invalid.  allocate() writes into
// the current frame's pool. No individual vkFreeDescriptorSets is ever called.
class VulkanDescriptorAllocator final : public DescriptorAllocator
{
public:
    VulkanDescriptorAllocator(VkDevice device, const DescriptorAllocatorDesc& desc) : m_device(device)
    {
        m_pools.resize(desc.frames_in_flight);
        for (auto& pool : m_pools)
        {
            pool = create_pool(desc);
            CRD_ASSERT(pool != VK_NULL_HANDLE);
        }
        m_frames_in_flight = desc.frames_in_flight;
        m_alloc_desc = desc;
    }

    ~VulkanDescriptorAllocator() noexcept override
    {
        for (auto& pool : m_pools)
        {
            if (pool != VK_NULL_HANDLE)
            {
                vkDestroyDescriptorPool(m_device, pool, nullptr);
                pool = VK_NULL_HANDLE;
            }
        }
    }

    void begin_frame(crd::u32 frame_index) override
    {
        m_current_pool_index = frame_index % m_frames_in_flight;
        vkResetDescriptorPool(m_device, m_pools[m_current_pool_index], 0);
    }

    [[nodiscard]] std::unique_ptr<DescriptorSet> allocate(const DescriptorSetLayout& layout) override
    {
        const auto* vk_layout = dynamic_cast<const VulkanDescriptorSetLayout*>(&layout);
        CRD_ASSERT(vk_layout != nullptr);

        VkDescriptorSetLayout raw_layout = vk_layout->handle();
        VkDescriptorSetAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = m_pools[m_current_pool_index];
        alloc_info.descriptorSetCount = 1;
        alloc_info.pSetLayouts = &raw_layout;

        VkDescriptorSet raw_set = VK_NULL_HANDLE;
        if (!vk_ok(vkAllocateDescriptorSets(m_device, &alloc_info, &raw_set), "vkAllocateDescriptorSets"))
        {
            CRD_LOG_ERROR(detail::g_log_rhi_vulkan, "DescriptorAllocator pool exhausted — increase max_sets_per_frame");
            return nullptr;
        }

        // Copy layout bindings into the set for type-aware update_buffer().
        crd::containers::Array<DescriptorBinding> bindings;
        for (const auto& b : layout.desc().bindings)
        {
            bindings.push_back(b);
        }

        return std::make_unique<VulkanDescriptorSet>(m_device, raw_set, std::move(bindings));
    }

private:
    [[nodiscard]] VkDescriptorPool create_pool(const DescriptorAllocatorDesc& desc) const
    {
        VkDescriptorPoolSize pool_sizes[] = {
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, desc.max_uniform_buffers_per_frame},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, desc.max_storage_buffers_per_frame},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, desc.max_combined_image_samplers_per_frame},
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, desc.max_sampled_images_per_frame},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, desc.max_storage_images_per_frame},
            {VK_DESCRIPTOR_TYPE_SAMPLER, desc.max_samplers_per_frame},
        };

        VkDescriptorPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.maxSets = desc.max_sets_per_frame;
        pool_info.poolSizeCount = static_cast<crd::u32>(std::size(pool_sizes));
        pool_info.pPoolSizes = pool_sizes;

        VkDescriptorPool pool = VK_NULL_HANDLE;
        if (!vk_ok(vkCreateDescriptorPool(m_device, &pool_info, nullptr, &pool), "vkCreateDescriptorPool"))
        {
            return VK_NULL_HANDLE;
        }
        return pool;
    }

    VkDevice m_device = VK_NULL_HANDLE;
    crd::containers::Array<VkDescriptorPool> m_pools{};
    DescriptorAllocatorDesc m_alloc_desc{};
    crd::u32 m_frames_in_flight = 2;
    crd::u32 m_current_pool_index = 0;
};

class VulkanPipeline final : public Pipeline
{
public:
    // `owned_layout` is non-null only when create_graphics_pipeline synthesised an empty
    // layout for a desc.pipeline_layout == nullptr call. User-provided layouts are NOT owned.
    VulkanPipeline(VkDevice device, GraphicsPipelineDesc desc, VkPipeline pipeline,
                   VkPipelineLayout owned_layout = VK_NULL_HANDLE)
        : m_device(device), m_desc(desc), m_pipeline(pipeline), m_owned_layout(owned_layout)
    {
    }

    ~VulkanPipeline() noexcept override
    {
        if (m_pipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(m_device, m_pipeline, nullptr);
            m_pipeline = VK_NULL_HANDLE;
        }
        if (m_owned_layout != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(m_device, m_owned_layout, nullptr);
            m_owned_layout = VK_NULL_HANDLE;
        }
    }

    [[nodiscard]] const GraphicsPipelineDesc& desc() const noexcept override { return m_desc; }
    [[nodiscard]] VkPipeline handle() const noexcept { return m_pipeline; }

private:
    VkDevice m_device = VK_NULL_HANDLE;
    GraphicsPipelineDesc m_desc{};
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_owned_layout = VK_NULL_HANDLE; // only set when synthesised internally
};

// Phase 3.1.7.6 v0a (ADR-0080) — compute pipeline.
//
// Mirrors `VulkanPipeline` (graphics) but skips all graphics-only state
// (vertex input, viewport, raster, blend). Single compute-stage shader
// module + pipeline layout — that's the whole surface.
//
// `owned_layout` is non-null only when create_compute_pipeline synthesised
// an empty layout for a desc.pipeline_layout == nullptr call. User-provided
// layouts are NOT owned (matches VulkanPipeline pattern).
class VulkanComputePipeline final : public ComputePipeline
{
public:
    VulkanComputePipeline(VkDevice device, ComputePipelineDesc desc, VkPipeline pipeline,
                          VkPipelineLayout owned_layout = VK_NULL_HANDLE)
        : m_device(device), m_desc(desc), m_pipeline(pipeline), m_owned_layout(owned_layout)
    {
    }

    ~VulkanComputePipeline() noexcept override
    {
        if (m_pipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(m_device, m_pipeline, nullptr);
            m_pipeline = VK_NULL_HANDLE;
        }
        if (m_owned_layout != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(m_device, m_owned_layout, nullptr);
            m_owned_layout = VK_NULL_HANDLE;
        }
    }

    [[nodiscard]] const ComputePipelineDesc& desc() const noexcept override { return m_desc; }
    [[nodiscard]] VkPipeline handle() const noexcept { return m_pipeline; }

private:
    VkDevice m_device = VK_NULL_HANDLE;
    ComputePipelineDesc m_desc{};
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_owned_layout = VK_NULL_HANDLE; // only set when synthesised internally
};

class VulkanSwapchain;

class VulkanCommandBuffer final : public CommandBuffer
{
public:
    VulkanCommandBuffer(VkDevice device, VkCommandPool command_pool, VkCommandBuffer command_buffer,
                        bool /*sync2_enabled*/)
        : m_device(device), m_command_pool(command_pool), m_command_buffer(command_buffer)
    {
    }

    ~VulkanCommandBuffer() noexcept override
    {
        if (m_command_buffer != VK_NULL_HANDLE)
        {
            vkFreeCommandBuffers(m_device, m_command_pool, 1, &m_command_buffer);
            m_command_buffer = VK_NULL_HANDLE;
        }
    }

    void begin() override
    {
        CRD_VERIFY(vkResetCommandBuffer(m_command_buffer, 0) == VK_SUCCESS);
        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        CRD_VERIFY(vkBeginCommandBuffer(m_command_buffer, &begin_info) == VK_SUCCESS);
    }

    void end() override { CRD_VERIFY(vkEndCommandBuffer(m_command_buffer) == VK_SUCCESS); }

    void reset() override { CRD_VERIFY(vkResetCommandBuffer(m_command_buffer, 0) == VK_SUCCESS); }

    void begin_rendering(const RenderingInfo& info) override
    {
        // Color attachment is optional (null = depth-only pass).
        auto* color_image = dynamic_cast<VulkanImage*>(info.color_attachment.image);
        const bool has_color = color_image != nullptr;

        VkRenderingAttachmentInfo color_attachment_info{};
        color_attachment_info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        if (has_color)
        {
            color_attachment_info.imageView = color_image->image_view();
            color_attachment_info.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color_attachment_info.loadOp = to_vk_load_op(info.color_attachment.load_op);
            color_attachment_info.storeOp = to_vk_store_op(info.color_attachment.store_op);
            color_attachment_info.clearValue.color = {
                {info.color_attachment.clear_color.r, info.color_attachment.clear_color.g,
                 info.color_attachment.clear_color.b, info.color_attachment.clear_color.a}};
        }

        VkRenderingAttachmentInfo depth_attachment_info{};
        depth_attachment_info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        const bool has_depth = info.depth_attachment != nullptr && info.depth_attachment->image != nullptr;
        if (has_depth)
        {
            auto* depth_image = dynamic_cast<VulkanImage*>(info.depth_attachment->image);
            CRD_ASSERT(depth_image != nullptr);
            depth_attachment_info.imageView = depth_image->image_view();
            depth_attachment_info.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depth_attachment_info.loadOp = to_vk_load_op(info.depth_attachment->load_op);
            depth_attachment_info.storeOp = to_vk_store_op(info.depth_attachment->store_op);
            depth_attachment_info.clearValue.depthStencil = {info.depth_attachment->clear_depth_stencil.depth,
                                                             info.depth_attachment->clear_depth_stencil.stencil};
        }

        VkRenderingInfo rendering_info{};
        rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering_info.renderArea = {{0, 0}, {info.extent.width, info.extent.height}};
        rendering_info.layerCount = 1;
        rendering_info.colorAttachmentCount = has_color ? 1U : 0U;
        rendering_info.pColorAttachments = has_color ? &color_attachment_info : nullptr;
        rendering_info.pDepthAttachment = has_depth ? &depth_attachment_info : nullptr;
        vkCmdBeginRendering(m_command_buffer, &rendering_info);
    }

    void end_rendering() override { vkCmdEndRendering(m_command_buffer); }

    void bind_pipeline(Pipeline& pipeline) override
    {
        auto* vk_pipeline = dynamic_cast<VulkanPipeline*>(&pipeline);
        CRD_ASSERT(vk_pipeline != nullptr);
        vkCmdBindPipeline(m_command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_pipeline->handle());
    }

    void bind_vertex_buffer(Buffer& buffer, crd::u64 offset_bytes) override
    {
        auto* vk_buffer = dynamic_cast<VulkanBuffer*>(&buffer);
        CRD_ASSERT(vk_buffer != nullptr);
        const VkBuffer handle = vk_buffer->handle();
        const VkDeviceSize offset = offset_bytes;
        vkCmdBindVertexBuffers(m_command_buffer, 0, 1, &handle, &offset);
    }

    void bind_index_buffer(Buffer& buffer, crd::u64 offset_bytes, rhi::IndexType type) override
    {
        auto* vk_buffer = dynamic_cast<VulkanBuffer*>(&buffer);
        CRD_ASSERT(vk_buffer != nullptr);
        const VkIndexType vk_type = (type == rhi::IndexType::Uint16) ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
        vkCmdBindIndexBuffer(m_command_buffer, vk_buffer->handle(), offset_bytes, vk_type);
    }

    void draw(crd::u32 vertex_count, crd::u32 first_vertex) override
    {
        vkCmdDraw(m_command_buffer, vertex_count, 1, first_vertex, 0);
    }

    void draw_indexed(crd::u32 index_count, crd::u32 first_index, crd::i32 vertex_offset) override
    {
        vkCmdDrawIndexed(m_command_buffer, index_count, 1, first_index, vertex_offset, 0);
    }

    void draw_instanced(crd::u32 vertex_count, crd::u32 instance_count,
                        crd::u32 first_vertex, crd::u32 first_instance) override
    {
        vkCmdDraw(m_command_buffer, vertex_count, instance_count, first_vertex, first_instance);
    }

    void copy_buffer(Buffer& src, Buffer& dst,
                     crd::u64 src_offset, crd::u64 dst_offset, crd::u64 size_bytes) override
    {
        auto* vk_src = dynamic_cast<VulkanBuffer*>(&src);
        auto* vk_dst = dynamic_cast<VulkanBuffer*>(&dst);
        CRD_ASSERT(vk_src != nullptr && vk_dst != nullptr);

        VkBufferCopy region{};
        region.srcOffset = src_offset;
        region.dstOffset = dst_offset;
        region.size      = size_bytes;
        vkCmdCopyBuffer(m_command_buffer, vk_src->handle(), vk_dst->handle(), 1, &region);
    }

    void copy_buffer_to_image(Buffer& src, Image& dst,
                              crd::containers::ConstSpan<BufferImageCopy> regions) override
    {
        auto* vk_src = dynamic_cast<VulkanBuffer*>(&src);
        auto* vk_dst = dynamic_cast<VulkanImage*>(&dst);
        CRD_ASSERT(vk_src != nullptr && vk_dst != nullptr);

        crd::containers::Array<VkBufferImageCopy> vk_regions;
        vk_regions.resize(regions.size());
        for (crd::usize i = 0; i < regions.size(); ++i)
        {
            const auto& r = regions[i];
            auto& vk_r    = vk_regions[i];
            vk_r.bufferOffset      = r.buffer_offset;
            vk_r.bufferRowLength   = 0;
            vk_r.bufferImageHeight = 0;
            vk_r.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
            vk_r.imageSubresource.mipLevel       = r.mip_level;
            vk_r.imageSubresource.baseArrayLayer = 0;
            vk_r.imageSubresource.layerCount     = 1;
            vk_r.imageOffset = {0, 0, 0};
            vk_r.imageExtent = {r.extent.width, r.extent.height, 1U};
        }
        vkCmdCopyBufferToImage(m_command_buffer, vk_src->handle(), vk_dst->handle(),
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<crd::u32>(vk_regions.size()), vk_regions.data());
    }

    void blit_image(Image& src, Image& dst, Extent2D src_extent, Extent2D dst_extent) noexcept override
    {
        auto* vk_src = dynamic_cast<VulkanImage*>(&src);
        auto* vk_dst = dynamic_cast<VulkanImage*>(&dst);
        CRD_ASSERT(vk_src != nullptr && vk_dst != nullptr);

        VkImageBlit region{};
        region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.srcSubresource.mipLevel = 0;
        region.srcSubresource.baseArrayLayer = 0;
        region.srcSubresource.layerCount = 1;
        region.srcOffsets[0] = {0, 0, 0};
        region.srcOffsets[1] = {static_cast<crd::i32>(src_extent.width), static_cast<crd::i32>(src_extent.height), 1};
        region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.dstSubresource.mipLevel = 0;
        region.dstSubresource.baseArrayLayer = 0;
        region.dstSubresource.layerCount = 1;
        region.dstOffsets[0] = {0, 0, 0};
        region.dstOffsets[1] = {static_cast<crd::i32>(dst_extent.width), static_cast<crd::i32>(dst_extent.height), 1};

        vkCmdBlitImage(m_command_buffer, vk_src->handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk_dst->handle(),
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);
    }

    void push_constants(PipelineLayout& layout, ShaderStage stages, crd::u32 offset, crd::u32 size,
                        const void* data) override
    {
        auto* vk_layout = dynamic_cast<VulkanPipelineLayout*>(&layout);
        CRD_ASSERT(vk_layout != nullptr);
        vkCmdPushConstants(m_command_buffer, vk_layout->handle(), to_vk_shader_stage_flags(stages), offset, size, data);
    }

    void bind_descriptor_sets(PipelineLayout& layout, crd::u32 first_set,
                              crd::containers::ConstSpan<DescriptorSet*> sets) override
    {
        auto* vk_layout = dynamic_cast<VulkanPipelineLayout*>(&layout);
        CRD_ASSERT(vk_layout != nullptr);

        crd::containers::Array<VkDescriptorSet> raw_sets;
        raw_sets.resize(sets.size());
        for (crd::usize i = 0; i < sets.size(); ++i)
        {
            auto* vk_set = dynamic_cast<VulkanDescriptorSet*>(sets[i]);
            CRD_ASSERT(vk_set != nullptr);
            raw_sets[i] = vk_set->handle();
        }

        vkCmdBindDescriptorSets(m_command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_layout->handle(), first_set,
                                static_cast<crd::u32>(raw_sets.size()), raw_sets.data(), 0, nullptr);
    }

    // ---------------------------------------------------------------
    // Phase 3.1.7.6 v0b (ADR-0080) — compute dispatch surface.
    // ---------------------------------------------------------------

    void bind_compute_pipeline(ComputePipeline& pipeline) override
    {
        auto* vk_pipeline = dynamic_cast<VulkanComputePipeline*>(&pipeline);
        CRD_ASSERT(vk_pipeline != nullptr);
        vkCmdBindPipeline(m_command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, vk_pipeline->handle());
    }

    void bind_compute_descriptor_sets(PipelineLayout& layout, crd::u32 first_set,
                                      crd::containers::ConstSpan<DescriptorSet*> sets) override
    {
        auto* vk_layout = dynamic_cast<VulkanPipelineLayout*>(&layout);
        CRD_ASSERT(vk_layout != nullptr);

        crd::containers::Array<VkDescriptorSet> raw_sets;
        raw_sets.resize(sets.size());
        for (crd::usize i = 0; i < sets.size(); ++i)
        {
            auto* vk_set = dynamic_cast<VulkanDescriptorSet*>(sets[i]);
            CRD_ASSERT(vk_set != nullptr);
            raw_sets[i] = vk_set->handle();
        }

        vkCmdBindDescriptorSets(m_command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, vk_layout->handle(),
                                first_set, static_cast<crd::u32>(raw_sets.size()), raw_sets.data(), 0, nullptr);
    }

    void dispatch(crd::u32 group_count_x, crd::u32 group_count_y, crd::u32 group_count_z) override
    {
        vkCmdDispatch(m_command_buffer, group_count_x, group_count_y, group_count_z);
    }

    void dispatch_indirect(Buffer& buffer, crd::u64 offset_bytes) override
    {
        auto* vk_buffer = dynamic_cast<VulkanBuffer*>(&buffer);
        CRD_ASSERT(vk_buffer != nullptr);
        vkCmdDispatchIndirect(m_command_buffer, vk_buffer->handle(), offset_bytes);
    }

    void set_viewport(Extent2D extent) noexcept override
    {
        VkViewport viewport{};
        viewport.x        = 0.0F;
        viewport.y        = 0.0F;
        viewport.width    = static_cast<float>(extent.width);
        viewport.height   = static_cast<float>(extent.height);
        viewport.minDepth = 0.0F;
        viewport.maxDepth = 1.0F;
        vkCmdSetViewport(m_command_buffer, 0, 1, &viewport);
    }

    void set_scissor(Rect2D rect) noexcept override
    {
        VkRect2D vk_rect{{rect.x, rect.y}, {rect.width, rect.height}};
        vkCmdSetScissor(m_command_buffer, 0, 1, &vk_rect);
    }

    void transition_image(Image& image, ImageAccess from, ImageAccess to) noexcept override
    {
        if (from == to)
            return;
        auto* vk_image = dynamic_cast<VulkanImage*>(&image);
        CRD_ASSERT(vk_image != nullptr);

        const auto src = to_vk_access_info(from);
        const auto dst = to_vk_access_info(to);

        const VkImageAspectFlags aspect = (to == ImageAccess::DepthWrite || to == ImageAccess::DepthRead ||
                                           from == ImageAccess::DepthWrite || from == ImageAccess::DepthRead)
                                              ? VK_IMAGE_ASPECT_DEPTH_BIT
                                              : VK_IMAGE_ASPECT_COLOR_BIT;

        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = src.access;
        barrier.dstAccessMask = dst.access;
        barrier.oldLayout = src.layout;
        barrier.newLayout = dst.layout;
        barrier.image = vk_image->handle();
        barrier.subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};

        vkCmdPipelineBarrier(m_command_buffer, src.stage, dst.stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        vk_image->set_layout(dst.layout);
    }

    // Phase 3.1.7.6 v0c (ADR-0080 D8) — single-buffer pipeline barrier.
    // No-op when from == to (validation layer would also flag a same-state
    // barrier as redundant; cheap early-out). Same-queue path; queue
    // ownership transfer ships at v0d.
    void buffer_barrier(Buffer& buffer, BufferAccess from, BufferAccess to) noexcept override
    {
        if (from == to)
            return;
        auto* vk_buffer = dynamic_cast<VulkanBuffer*>(&buffer);
        CRD_ASSERT(vk_buffer != nullptr);

        const auto src = to_vk_buffer_access_info(from);
        const auto dst = to_vk_buffer_access_info(to);

        VkBufferMemoryBarrier barrier{};
        barrier.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barrier.srcAccessMask       = src.access;
        barrier.dstAccessMask       = dst.access;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer              = vk_buffer->handle();
        barrier.offset              = 0;
        barrier.size                = VK_WHOLE_SIZE;

        vkCmdPipelineBarrier(m_command_buffer, src.stage, dst.stage, 0, 0, nullptr, 1, &barrier, 0, nullptr);
    }

    [[nodiscard]] VkCommandBuffer handle() const noexcept { return m_command_buffer; }

private:
    VkDevice m_device = VK_NULL_HANDLE;
    VkCommandPool m_command_pool = VK_NULL_HANDLE;
    VkCommandBuffer m_command_buffer = VK_NULL_HANDLE;
};

class VulkanSwapchain final : public Swapchain
{
public:
    VulkanSwapchain(VkDevice device, VkPhysicalDevice physical_device, VkSurfaceKHR surface,
                    VkSurfaceFormatKHR chosen_format, VkPresentModeKHR chosen_present_mode,
                    VkSwapchainKHR swapchain, SwapchainDesc desc, crd::u32 frames_in_flight,
                    crd::containers::Array<std::unique_ptr<VulkanImage>> images)
        : m_device(device), m_physical_device(physical_device), m_surface(surface),
          m_chosen_format(chosen_format), m_chosen_present_mode(chosen_present_mode),
          m_swapchain(swapchain), m_desc(std::move(desc)), m_frames_in_flight(frames_in_flight),
          m_images(std::move(images))
    {
        if (!create_frame_sync_objects())
        {
            CRD_LOG_ERROR(detail::g_log_rhi_vulkan, "Fatal: swapchain frame sync object creation failed");
        }
    }

    ~VulkanSwapchain() noexcept override
    {
        for (auto& frame : m_frames)
        {
            if (frame.image_available != VK_NULL_HANDLE)
            {
                vkDestroySemaphore(m_device, frame.image_available, nullptr);
            }
            if (frame.in_flight != VK_NULL_HANDLE)
            {
                vkDestroyFence(m_device, frame.in_flight, nullptr);
            }
        }

        for (auto& image_sync : m_image_sync_array)
        {
            if (image_sync.render_finished != VK_NULL_HANDLE)
            {
                vkDestroySemaphore(m_device, image_sync.render_finished, nullptr);
            }
        }
        if (m_swapchain != VK_NULL_HANDLE)
        {
            vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
            m_swapchain = VK_NULL_HANDLE;
        }
    }

    [[nodiscard]] const SwapchainDesc& desc() const noexcept override { return m_desc; }

    [[nodiscard]] bool acquire_next_image() override
    {
        FrameSync& frame = current_frame_sync();
        if (!vk_ok(vkWaitForFences(m_device, 1, &frame.in_flight, VK_TRUE, UINT64_MAX), "vkWaitForFences"))
        {
            return false;
        }

        const VkResult result = vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX, frame.image_available,
                                                      VK_NULL_HANDLE, &m_current_image_index);
        if (result == VK_ERROR_OUT_OF_DATE_KHR)
        {
            // Swapchain is stale — leave the fence signaled so the next acquire can wait correctly.
            return false;
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        {
            CRD_LOG_ERROR(detail::g_log_rhi_vulkan, "vkAcquireNextImageKHR failed with VkResult={}",
                          static_cast<int>(result));
            return false;
        }

        // Reset fence only after a successful acquire — prevents a hang if acquire fails next frame.
        if (!vk_ok(vkResetFences(m_device, 1, &frame.in_flight), "vkResetFences"))
        {
            return false;
        }
        m_image_acquired = true;
        return true;
    }

    [[nodiscard]] crd::u32 current_image_index() const noexcept override { return m_current_image_index; }
    [[nodiscard]] Image& current_image() noexcept override { return *m_images[m_current_image_index]; }
    [[nodiscard]] VkSwapchainKHR handle() const noexcept { return m_swapchain; }
    [[nodiscard]] crd::u32 image_count() const noexcept { return static_cast<crd::u32>(m_images.size()); }

    [[nodiscard]] FrameSync& current_frame_sync() noexcept { return m_frames[m_frame_index]; }
    [[nodiscard]] ImageSync& current_image_sync() noexcept { return m_image_sync_array[m_current_image_index]; }
    [[nodiscard]] crd::u32 frame_index() const noexcept { return m_frame_index; }
    void advance_frame() noexcept { m_frame_index = (m_frame_index + 1U) % m_frames_in_flight; }
    [[nodiscard]] bool image_acquired() const noexcept { return m_image_acquired; }
    void clear_image_acquired() noexcept { m_image_acquired = false; }

    void resize(Extent2D new_extent) noexcept override
    {
        if (new_extent.width == 0 || new_extent.height == 0)
            return;

        // Destroy old image views (VkImages are owned by the swapchain driver).
        m_images.clear();

        // Build new swapchain; pass old handle so the driver can recycle resources.
        VkSurfaceCapabilitiesKHR caps{};
        if (!vk_ok(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physical_device, m_surface, &caps),
                   "vkGetPhysicalDeviceSurfaceCapabilitiesKHR(resize)"))
        {
            return;
        }

        // Use the driver's reported currentExtent when it's well-defined (not UINT32_MAX).
        // On Windows the driver always sets currentExtent = the actual surface size, and
        // minImageExtent == maxImageExtent == currentExtent, so the window-event extent is
        // just a hint — using it directly causes VUID-VkSwapchainCreateInfoKHR-pNext-07781.
        VkExtent2D chosen_extent;
        if (caps.currentExtent.width != UINT32_MAX)
        {
            chosen_extent = caps.currentExtent;
        }
        else
        {
            chosen_extent.width  = std::clamp(new_extent.width,  caps.minImageExtent.width,  caps.maxImageExtent.width);
            chosen_extent.height = std::clamp(new_extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
        }

        VkSwapchainCreateInfoKHR create_info{};
        create_info.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        create_info.surface          = m_surface;
        create_info.minImageCount    = static_cast<crd::u32>(m_image_sync_array.size());
        create_info.imageFormat      = m_chosen_format.format;
        create_info.imageColorSpace  = m_chosen_format.colorSpace;
        create_info.imageExtent      = chosen_extent;
        create_info.imageArrayLayers = 1;
        create_info.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        create_info.preTransform     = caps.currentTransform;
        create_info.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        create_info.presentMode      = m_chosen_present_mode;
        create_info.clipped          = VK_TRUE;
        create_info.oldSwapchain     = m_swapchain;

        VkSwapchainKHR new_swapchain = VK_NULL_HANDLE;
        if (!vk_ok(vkCreateSwapchainKHR(m_device, &create_info, nullptr, &new_swapchain),
                   "vkCreateSwapchainKHR(resize)"))
        {
            return;
        }

        // Destroy old swapchain after creating the new one (oldSwapchain reference is now transferred).
        vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
        m_swapchain = new_swapchain;

        // Acquire new image handles and wrap them.
        crd::u32 image_count = 0;
        vkGetSwapchainImagesKHR(m_device, m_swapchain, &image_count, nullptr);
        crd::containers::Array<VkImage> images;
        images.resize(image_count);
        vkGetSwapchainImagesKHR(m_device, m_swapchain, &image_count, images.data());

        for (const VkImage image : images)
        {
            VkImageViewCreateInfo view_info{};
            view_info.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view_info.image            = image;
            view_info.viewType         = VK_IMAGE_VIEW_TYPE_2D;
            view_info.format           = m_chosen_format.format;
            view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

            VkImageView view = VK_NULL_HANDLE;
            if (!vk_ok(vkCreateImageView(m_device, &view_info, nullptr, &view), "vkCreateImageView(resize)"))
                return;

            m_images.push_back(
                std::make_unique<VulkanImage>(m_device,
                                              ImageDesc{{chosen_extent.width, chosen_extent.height},
                                                        Format::B8G8R8A8Unorm,
                                                        enum_bits(ImageUsage::ColorAttachment), 1, 1},
                                              image, view));
        }

        // Rebuild image sync semaphores if image count changed.
        if (image_count != static_cast<crd::u32>(m_image_sync_array.size()))
        {
            for (auto& s : m_image_sync_array)
            {
                if (s.render_finished != VK_NULL_HANDLE)
                    vkDestroySemaphore(m_device, s.render_finished, nullptr);
            }
            m_image_sync_array.clear();
            m_image_sync_array.resize(image_count);
            for (auto& s : m_image_sync_array)
            {
                VkSemaphoreCreateInfo semaphore_info{};
                semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
                static_cast<void>(vk_ok(vkCreateSemaphore(m_device, &semaphore_info, nullptr, &s.render_finished),
                                        "vkCreateSemaphore(resize)"));
            }
        }

        m_desc.extent = {chosen_extent.width, chosen_extent.height};
        m_current_image_index = 0;
    }

private:
    [[nodiscard]] bool create_frame_sync_objects()
    {
        m_frames.resize(m_frames_in_flight);
        for (auto& frame : m_frames)
        {
            VkSemaphoreCreateInfo semaphore_info{};
            semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            VkFenceCreateInfo fence_info{};
            fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            if (!vk_ok(vkCreateSemaphore(m_device, &semaphore_info, nullptr, &frame.image_available),
                       "vkCreateSemaphore(image_available)"))
            {
                return false;
            }
            if (!vk_ok(vkCreateFence(m_device, &fence_info, nullptr, &frame.in_flight), "vkCreateFence"))
            {
                return false;
            }
        }

        m_image_sync_array.resize(m_images.size());
        for (auto& image_sync : m_image_sync_array)
        {
            VkSemaphoreCreateInfo semaphore_info{};
            semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            if (!vk_ok(vkCreateSemaphore(m_device, &semaphore_info, nullptr, &image_sync.render_finished),
                       "vkCreateSemaphore(render_finished)"))
            {
                return false;
            }
        }
        return true;
    }

    VkDevice m_device = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical_device = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkSurfaceFormatKHR m_chosen_format{};
    VkPresentModeKHR m_chosen_present_mode = VK_PRESENT_MODE_FIFO_KHR;
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    SwapchainDesc m_desc{};
    crd::u32 m_frames_in_flight = 2;
    crd::containers::Array<FrameSync> m_frames{};
    crd::containers::Array<ImageSync> m_image_sync_array{};
    crd::containers::Array<std::unique_ptr<VulkanImage>> m_images{};
    crd::u32 m_current_image_index = 0;
    crd::u32 m_frame_index = 0;
    bool m_image_acquired = false;
};

// Phase 3.0 v1o1 — VulkanFence wraps a VkFence + the owning device handle
// (needed for vkResetFences / vkDestroyFence). Created in the unsignalled
// state via vkCreateFence with no flags; destroyed by the unique_ptr.
class VulkanFence final : public Fence
{
public:
    VulkanFence(VkDevice device, VkFence handle) noexcept
        : m_device(device), m_handle(handle)
    {
    }

    ~VulkanFence() override
    {
        if (m_handle != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE)
        {
            vkDestroyFence(m_device, m_handle, nullptr);
        }
    }

    [[nodiscard]] bool is_signaled() const noexcept override
    {
        if (m_handle == VK_NULL_HANDLE)
        {
            return false;
        }
        const VkResult status = vkGetFenceStatus(m_device, m_handle);
        // VK_SUCCESS → signaled; VK_NOT_READY → not yet; other → error (treat as not signaled).
        return status == VK_SUCCESS;
    }

    void wait() override
    {
        if (m_handle == VK_NULL_HANDLE)
        {
            return;
        }
        // UINT64_MAX = wait indefinitely.
        static_cast<void>(vk_ok(
            vkWaitForFences(m_device, 1, &m_handle, VK_TRUE, UINT64_MAX),
            "vkWaitForFences"));
    }

    void reset() override
    {
        if (m_handle == VK_NULL_HANDLE)
        {
            return;
        }
        static_cast<void>(vk_ok(vkResetFences(m_device, 1, &m_handle), "vkResetFences"));
    }

    [[nodiscard]] VkFence handle() const noexcept { return m_handle; }

private:
    VkDevice m_device = VK_NULL_HANDLE;
    VkFence  m_handle = VK_NULL_HANDLE;
};

// Phase 3.1.7.6 v0d (ADR-0080 D10) — binary semaphore for cross-queue
// GPU-GPU sync. Reset implicit on wait per Vulkan binary semantics.
class VulkanSemaphore final : public Semaphore
{
public:
    VulkanSemaphore(VkDevice device, VkSemaphore handle) noexcept
        : m_device(device), m_handle(handle)
    {
    }

    ~VulkanSemaphore() override
    {
        if (m_handle != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE)
        {
            vkDestroySemaphore(m_device, m_handle, nullptr);
        }
    }

    [[nodiscard]] VkSemaphore handle() const noexcept { return m_handle; }

private:
    VkDevice    m_device = VK_NULL_HANDLE;
    VkSemaphore m_handle = VK_NULL_HANDLE;
};

[[nodiscard]] VkPipelineStageFlags to_vk_pipeline_stage(PipelineStage stage) noexcept
{
    switch (stage)
    {
        case PipelineStage::ComputeShader:    return VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        case PipelineStage::VertexInput:      return VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
        case PipelineStage::VertexShader:     return VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
        case PipelineStage::FragmentShader:   return VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        case PipelineStage::ColorAttachment:  return VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        case PipelineStage::Transfer:         return VK_PIPELINE_STAGE_TRANSFER_BIT;
        case PipelineStage::BottomOfPipe:     return VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        case PipelineStage::Top:
        default:                              return VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    }
}

class VulkanQueue final : public Queue
{
public:
    VulkanQueue(VkQueue queue, bool /*sync2_enabled*/) : m_queue(queue) {}

    [[nodiscard]] bool submit(CommandBuffer& command_buffer, Swapchain& swapchain) override
    {
        auto* vk_command_buffer = dynamic_cast<VulkanCommandBuffer*>(&command_buffer);
        auto* vk_swapchain = dynamic_cast<VulkanSwapchain*>(&swapchain);
        if (vk_command_buffer == nullptr || vk_swapchain == nullptr)
        {
            CRD_LOG_ERROR(detail::g_log_rhi_vulkan, "Queue::submit received incompatible backend objects");
            return false;
        }
        if (!vk_swapchain->image_acquired())
        {
            CRD_LOG_ERROR(detail::g_log_rhi_vulkan, "Queue::submit called before acquire_next_image");
            return false;
        }

        FrameSync& frame = vk_swapchain->current_frame_sync();
        ImageSync& image_sync = vk_swapchain->current_image_sync();
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkCommandBuffer handle = vk_command_buffer->handle();
        VkSubmitInfo submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.waitSemaphoreCount = 1;
        submit_info.pWaitSemaphores = &frame.image_available;
        submit_info.pWaitDstStageMask = &wait_stage;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &handle;
        submit_info.signalSemaphoreCount = 1;
        submit_info.pSignalSemaphores = &image_sync.render_finished;
        return vk_ok(vkQueueSubmit(m_queue, 1, &submit_info, frame.in_flight), "vkQueueSubmit");
    }

    void submit_and_wait(CommandBuffer& command_buffer) override
    {
        auto* vk_cmd = dynamic_cast<VulkanCommandBuffer*>(&command_buffer);
        CRD_ASSERT(vk_cmd != nullptr);

        VkCommandBuffer handle = vk_cmd->handle();
        VkSubmitInfo submit_info{};
        submit_info.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers    = &handle;
        static_cast<void>(vk_ok(vkQueueSubmit(m_queue, 1, &submit_info, VK_NULL_HANDLE), "vkQueueSubmit(headless)"));
        vkQueueWaitIdle(m_queue);
    }

    void submit(CommandBuffer& command_buffer, Fence& fence) override
    {
        // Phase 3.0 v1o1 — non-blocking submit (ADR-0061 §"Layer 1"). Records
        // and submits without waiting; the fence is signalled when the GPU
        // completes the work. Caller polls fence.is_signaled() per frame.
        auto* vk_cmd   = dynamic_cast<VulkanCommandBuffer*>(&command_buffer);
        auto* vk_fence = dynamic_cast<VulkanFence*>(&fence);
        CRD_ASSERT_MSG(vk_cmd   != nullptr, "Queue::submit(cmd, fence): non-Vulkan CommandBuffer");
        CRD_ASSERT_MSG(vk_fence != nullptr, "Queue::submit(cmd, fence): non-Vulkan Fence");

        VkCommandBuffer handle = vk_cmd->handle();
        VkSubmitInfo submit_info{};
        submit_info.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers    = &handle;
        static_cast<void>(vk_ok(
            vkQueueSubmit(m_queue, 1, &submit_info, vk_fence->handle()),
            "vkQueueSubmit(fence)"));
    }

    // Phase 3.1.7.6 v0d (ADR-0080 D10) — full submit shape: cmd + fence +
    // wait semaphores (with per-wait pipeline stage) + signal semaphores.
    // The one path async-compute consumers go through. Existing fence-only
    // and present-coupled overloads above stay for v0a/b/c callers.
    void submit(const SubmitInfo& info) override
    {
        auto* vk_cmd = dynamic_cast<VulkanCommandBuffer*>(info.command_buffer);
        CRD_ASSERT_MSG(vk_cmd != nullptr, "Queue::submit(SubmitInfo): non-Vulkan CommandBuffer");

        crd::containers::Array<VkSemaphore>          wait_handles;
        crd::containers::Array<VkPipelineStageFlags> wait_stages;
        wait_handles.resize(info.wait_semaphores.size());
        wait_stages.resize(info.wait_semaphores.size());
        for (crd::usize i = 0; i < info.wait_semaphores.size(); ++i)
        {
            auto* vk_sem = dynamic_cast<VulkanSemaphore*>(info.wait_semaphores[i].semaphore);
            CRD_ASSERT_MSG(vk_sem != nullptr, "SubmitInfo wait_semaphore: non-Vulkan Semaphore");
            wait_handles[i] = vk_sem->handle();
            wait_stages[i]  = to_vk_pipeline_stage(info.wait_semaphores[i].wait_stage);
        }

        crd::containers::Array<VkSemaphore> signal_handles;
        signal_handles.resize(info.signal_semaphores.size());
        for (crd::usize i = 0; i < info.signal_semaphores.size(); ++i)
        {
            auto* vk_sem = dynamic_cast<VulkanSemaphore*>(info.signal_semaphores[i]);
            CRD_ASSERT_MSG(vk_sem != nullptr, "SubmitInfo signal_semaphore: non-Vulkan Semaphore");
            signal_handles[i] = vk_sem->handle();
        }

        VkCommandBuffer cmd_handle = vk_cmd->handle();
        VkSubmitInfo submit_info{};
        submit_info.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.waitSemaphoreCount   = static_cast<crd::u32>(wait_handles.size());
        submit_info.pWaitSemaphores      = wait_handles.data();
        submit_info.pWaitDstStageMask    = wait_stages.data();
        submit_info.commandBufferCount   = 1;
        submit_info.pCommandBuffers      = &cmd_handle;
        submit_info.signalSemaphoreCount = static_cast<crd::u32>(signal_handles.size());
        submit_info.pSignalSemaphores    = signal_handles.data();

        VkFence vk_fence_handle = VK_NULL_HANDLE;
        if (info.signal_fence != nullptr)
        {
            auto* vk_fence = dynamic_cast<VulkanFence*>(info.signal_fence);
            CRD_ASSERT_MSG(vk_fence != nullptr, "SubmitInfo signal_fence: non-Vulkan Fence");
            vk_fence_handle = vk_fence->handle();
        }

        static_cast<void>(vk_ok(
            vkQueueSubmit(m_queue, 1, &submit_info, vk_fence_handle),
            "vkQueueSubmit(SubmitInfo)"));
    }

    void present(Swapchain& swapchain) override
    {
        auto* vk_swapchain = dynamic_cast<VulkanSwapchain*>(&swapchain);
        if (vk_swapchain == nullptr)
        {
            CRD_LOG_ERROR(detail::g_log_rhi_vulkan, "Queue::present received incompatible swapchain");
            return;
        }

        ImageSync& image_sync = vk_swapchain->current_image_sync();
        VkSwapchainKHR swapchain_handle = vk_swapchain->handle();
        crd::u32 image_index = vk_swapchain->current_image_index();
        VkPresentInfoKHR present_info{};
        present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present_info.waitSemaphoreCount = 1;
        present_info.pWaitSemaphores = &image_sync.render_finished;
        present_info.swapchainCount = 1;
        present_info.pSwapchains = &swapchain_handle;
        present_info.pImageIndices = &image_index;

        const VkResult result = vkQueuePresentKHR(m_queue, &present_info);
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        {
            CRD_LOG_ERROR(detail::g_log_rhi_vulkan, "vkQueuePresentKHR failed with VkResult={}",
                          static_cast<int>(result));
        }
        vk_swapchain->clear_image_acquired();
        vk_swapchain->advance_frame();
    }

    void wait_idle() override
    {
        if (m_queue != VK_NULL_HANDLE)
        {
            vkQueueWaitIdle(m_queue);
        }
    }

private:
    VkQueue m_queue = VK_NULL_HANDLE;
};

class VulkanDevice final : public Device
{
public:
    VulkanDevice(VkInstance instance, VkPhysicalDevice physical_device, VkDevice device, crd::u32 graphics_family_index,
                 crd::u32 compute_family_index, DeviceDesc desc, bool sync2_enabled,
                 bool dynamic_rendering_enabled, bool shader_int64_enabled)
        : m_instance(instance), m_physical_device(physical_device), m_device(device),
          m_graphics_family_index(graphics_family_index), m_compute_family_index(compute_family_index),
          m_desc(std::move(desc)), m_sync2_enabled(sync2_enabled),
          m_dynamic_rendering_enabled(dynamic_rendering_enabled),
          m_shader_int64_enabled(shader_int64_enabled),
          m_allocator(physical_device, device)
    {
        vkGetDeviceQueue(m_device, m_graphics_family_index, 0, &m_graphics_queue_handle);
        m_graphics_queue = std::make_unique<VulkanQueue>(m_graphics_queue_handle, m_sync2_enabled);

        // Phase 3.1.7.6 v0d (ADR-0080 D9) — dedicated compute queue if
        // hardware exposed one + the policy allowed it through device
        // creation. Otherwise compute_queue() returns the SAME Queue&
        // as graphics_queue() (pointer-identity contract).
        if (m_compute_family_index != UINT32_MAX)
        {
            vkGetDeviceQueue(m_device, m_compute_family_index, 0, &m_compute_queue_handle);
            m_compute_queue = std::make_unique<VulkanQueue>(m_compute_queue_handle, m_sync2_enabled);
            CRD_LOG_INFO(detail::g_log_rhi_vulkan,
                         "Dedicated compute queue acquired: family={}", m_compute_family_index);
        }
        else
        {
            CRD_LOG_INFO(detail::g_log_rhi_vulkan,
                         "No dedicated compute queue family; compute_queue() aliases graphics_queue() "
                         "(FallbackGracefully)");
        }
        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = m_graphics_family_index;
        if (!vk_ok(vkCreateCommandPool(m_device, &pool_info, nullptr, &m_command_pool), "vkCreateCommandPool"))
        {
            CRD_LOG_ERROR(detail::g_log_rhi_vulkan, "Fatal: command pool creation failed — device unusable");
        }
        // ADR-0085 S7: give the allocator a queue + pool for relocation copies.
        m_allocator.set_transfer_context(m_graphics_queue_handle, m_command_pool);

        // Phase 3.1.7 v9a-a-async-compute (2026-05-18) — separate
        // compute-family command pool when a dedicated compute family
        // exists. Required for `create_command_buffer_for_queue` to
        // route compute-queue submissions through a same-family pool
        // (otherwise VUID-vkQueueSubmit-pCommandBuffers-00074 fires).
        if (m_compute_family_index != UINT32_MAX
            && m_compute_family_index != m_graphics_family_index)
        {
            VkCommandPoolCreateInfo compute_pool_info{};
            compute_pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            compute_pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            compute_pool_info.queueFamilyIndex = m_compute_family_index;
            if (!vk_ok(vkCreateCommandPool(m_device, &compute_pool_info, nullptr,
                                              &m_compute_command_pool),
                       "vkCreateCommandPool(compute)"))
            {
                CRD_LOG_ERROR(detail::g_log_rhi_vulkan,
                              "Compute command pool creation failed — async-compute path disabled");
                m_compute_command_pool = VK_NULL_HANDLE;
            }
        }
    }

    ~VulkanDevice() noexcept override
    {
        if (m_compute_command_pool != VK_NULL_HANDLE)
        {
            vkDestroyCommandPool(m_device, m_compute_command_pool, nullptr);
            m_compute_command_pool = VK_NULL_HANDLE;
        }
        if (m_command_pool != VK_NULL_HANDLE)
        {
            vkDestroyCommandPool(m_device, m_command_pool, nullptr);
            m_command_pool = VK_NULL_HANDLE;
        }
        if (m_device != VK_NULL_HANDLE)
        {
            vkDeviceWaitIdle(m_device);
            // Free all suballocator blocks (VkDeviceMemory) BEFORE the device dies —
            // ~VulkanAllocator would otherwise run after vkDestroyDevice (member dtors
            // follow the dtor body) and free memory on a dead device.
            m_allocator.destroy_all();
            vkDestroyDevice(m_device, nullptr);
            m_device = VK_NULL_HANDLE;
        }
        if (m_surface != VK_NULL_HANDLE)
        {
            vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
            m_surface = VK_NULL_HANDLE;
        }
    }

    [[nodiscard]] BackendApi api() const noexcept override { return BackendApi::Vulkan; }
    [[nodiscard]] VkPhysicalDevice physical_device() const noexcept { return m_physical_device; }
    [[nodiscard]] VkDevice handle() const noexcept { return m_device; }
    [[nodiscard]] VkQueue graphics_queue_handle() const noexcept { return m_graphics_queue_handle; }
    [[nodiscard]] crd::u32 graphics_family_index() const noexcept { return m_graphics_family_index; }

    [[nodiscard]] std::unique_ptr<Swapchain> create_swapchain(const SwapchainDesc& desc) override
    {
        if (m_device == VK_NULL_HANDLE)
        {
            CRD_LOG_ERROR(detail::g_log_rhi_vulkan, "create_swapchain called on invalid VulkanDevice");
            return nullptr;
        }

        if (m_surface == VK_NULL_HANDLE)
        {
            if (desc.native_window_handle == nullptr)
            {
                CRD_LOG_ERROR(detail::g_log_rhi_vulkan, "SwapchainDesc.native_window_handle was null");
                return nullptr;
            }

            const VkResult surface_result = glfwCreateWindowSurface(
                m_instance, static_cast<GLFWwindow*>(desc.native_window_handle), nullptr, &m_surface);
            if (!vk_ok(surface_result, "glfwCreateWindowSurface"))
            {
                return nullptr;
            }

            VkBool32 present_supported = VK_FALSE;
            if (!vk_ok(vkGetPhysicalDeviceSurfaceSupportKHR(m_physical_device, m_graphics_family_index, m_surface,
                                                            &present_supported),
                       "vkGetPhysicalDeviceSurfaceSupportKHR") ||
                present_supported != VK_TRUE)
            {
                CRD_LOG_ERROR(detail::g_log_rhi_vulkan, "Selected graphics queue family cannot present to the surface");
                return nullptr;
            }
        }

        VkSurfaceCapabilitiesKHR capabilities{};
        if (!vk_ok(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physical_device, m_surface, &capabilities),
                   "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"))
        {
            return nullptr;
        }

        crd::u32 format_count = 0;
        if (!vk_ok(vkGetPhysicalDeviceSurfaceFormatsKHR(m_physical_device, m_surface, &format_count, nullptr),
                   "vkGetPhysicalDeviceSurfaceFormatsKHR(count)"))
        {
            return nullptr;
        }
        if (format_count == 0)
        {
            CRD_LOG_ERROR(detail::g_log_rhi_vulkan, "Surface reported zero supported formats");
            return nullptr;
        }

        crd::containers::Array<VkSurfaceFormatKHR> formats;
        formats.resize(format_count);
        if (!vk_ok(vkGetPhysicalDeviceSurfaceFormatsKHR(m_physical_device, m_surface, &format_count, formats.data()),
                   "vkGetPhysicalDeviceSurfaceFormatsKHR(data)"))
        {
            return nullptr;
        }

        crd::u32 present_mode_count = 0;
        if (!vk_ok(
                vkGetPhysicalDeviceSurfacePresentModesKHR(m_physical_device, m_surface, &present_mode_count, nullptr),
                "vkGetPhysicalDeviceSurfacePresentModesKHR(count)"))
        {
            return nullptr;
        }

        crd::containers::Array<VkPresentModeKHR> present_modes;
        present_modes.resize(present_mode_count);
        if (!vk_ok(vkGetPhysicalDeviceSurfacePresentModesKHR(m_physical_device, m_surface, &present_mode_count,
                                                             present_modes.data()),
                   "vkGetPhysicalDeviceSurfacePresentModesKHR(data)"))
        {
            return nullptr;
        }

        VkSurfaceFormatKHR chosen_format = formats[0];
        for (const auto& candidate : formats)
        {
            if (candidate.format == VK_FORMAT_B8G8R8A8_UNORM &&
                candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            {
                chosen_format = candidate;
                break;
            }
        }

        VkPresentModeKHR chosen_present_mode = VK_PRESENT_MODE_FIFO_KHR;
        for (const auto& candidate : present_modes)
        {
            if (desc.present_mode == PresentMode::Mailbox && candidate == VK_PRESENT_MODE_MAILBOX_KHR)
            {
                chosen_present_mode = candidate;
                break;
            }
            if (desc.present_mode == PresentMode::Immediate && candidate == VK_PRESENT_MODE_IMMEDIATE_KHR)
            {
                chosen_present_mode = candidate;
                break;
            }
        }

        VkExtent2D chosen_extent{};
        if (capabilities.currentExtent.width != UINT32_MAX)
        {
            chosen_extent = capabilities.currentExtent;
        }
        else
        {
            chosen_extent.width =
                std::clamp(desc.extent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
            chosen_extent.height =
                std::clamp(desc.extent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
        }

        crd::u32 image_count = desc.image_count;
        image_count = std::max(image_count, capabilities.minImageCount);
        if (capabilities.maxImageCount > 0)
        {
            image_count = std::min(image_count, capabilities.maxImageCount);
        }

        VkSwapchainCreateInfoKHR create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        create_info.surface = m_surface;
        create_info.minImageCount = image_count;
        create_info.imageFormat = chosen_format.format;
        create_info.imageColorSpace = chosen_format.colorSpace;
        create_info.imageExtent = chosen_extent;
        create_info.imageArrayLayers = 1;
        create_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        create_info.preTransform = capabilities.currentTransform;
        create_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        create_info.presentMode = chosen_present_mode;
        create_info.clipped = VK_TRUE;

        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        if (!vk_ok(vkCreateSwapchainKHR(m_device, &create_info, nullptr, &swapchain), "vkCreateSwapchainKHR"))
        {
            return nullptr;
        }

        crd::u32 swapchain_image_count = 0;
        if (!vk_ok(vkGetSwapchainImagesKHR(m_device, swapchain, &swapchain_image_count, nullptr),
                   "vkGetSwapchainImagesKHR(count)"))
        {
            vkDestroySwapchainKHR(m_device, swapchain, nullptr);
            return nullptr;
        }

        crd::containers::Array<VkImage> images;
        images.resize(swapchain_image_count);
        if (!vk_ok(vkGetSwapchainImagesKHR(m_device, swapchain, &swapchain_image_count, images.data()),
                   "vkGetSwapchainImagesKHR(data)"))
        {
            vkDestroySwapchainKHR(m_device, swapchain, nullptr);
            return nullptr;
        }

        crd::containers::Array<std::unique_ptr<VulkanImage>> wrapped_images;
        for (const VkImage image : images)
        {
            VkImageViewCreateInfo view_info{};
            view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view_info.image = image;
            view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view_info.format = chosen_format.format;
            view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

            VkImageView view = VK_NULL_HANDLE;
            if (!vk_ok(vkCreateImageView(m_device, &view_info, nullptr, &view), "vkCreateImageView"))
            {
                vkDestroySwapchainKHR(m_device, swapchain, nullptr);
                return nullptr;
            }

            wrapped_images.push_back(
                std::make_unique<VulkanImage>(m_device,
                                              ImageDesc{{chosen_extent.width, chosen_extent.height},
                                                        Format::B8G8R8A8Unorm,
                                                        enum_bits(ImageUsage::ColorAttachment),
                                                        1,
                                                        1},
                                              image, view));
        }

        SwapchainDesc resolved_desc = desc;
        resolved_desc.extent = {chosen_extent.width, chosen_extent.height};
        resolved_desc.color_format = Format::B8G8R8A8Unorm;
        return std::make_unique<VulkanSwapchain>(m_device, m_physical_device, m_surface,
                                                 chosen_format, chosen_present_mode,
                                                 swapchain, std::move(resolved_desc), m_desc.frames_in_flight,
                                                 std::move(wrapped_images));
    }

    [[nodiscard]] std::unique_ptr<Buffer> create_buffer(const BufferDesc& desc) override
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VulkanAllocation allocation;
        if (!m_allocator.allocate_buffer(desc, buffer, allocation))
        {
            return nullptr;
        }

        return std::make_unique<VulkanBuffer>(m_device, desc, buffer, allocation, &m_allocator);
    }

    // ADR-0085 S6 diagnostic (test-facing via crd::rhi::vulkan_resident_block_count).
    [[nodiscard]] crd::u32 resident_block_count() const noexcept { return m_allocator.block_count(); }

    // ADR-0085 S7: drive a defrag pass over the allocator's live resources using this
    // device's graphics queue + command pool (test-facing via crd::rhi::vulkan_defragment).
    void defragment(IDefragPolicy& policy) { m_allocator.defragment(policy, m_graphics_queue_handle, m_command_pool); }

    // ADR-0085 S7 residency (test-facing via the crd::rhi::vulkan_* free functions).
    void configure_residency(IResidencyPolicy* policy, crd::u64 device_local_budget)
    {
        m_allocator.set_residency(policy, device_local_budget);
    }
    [[nodiscard]] crd::u64 device_local_used() const noexcept { return m_allocator.device_local_used(); }
    crd::u64               make_resident(Buffer& buffer) { return m_allocator.make_resident(buffer); }
    crd::u64               evict_to_host(Buffer& buffer) { return m_allocator.evict_to_host(buffer); }

    [[nodiscard]] std::unique_ptr<Image> create_image(const ImageDesc& desc) override
    {
        VkImage image = VK_NULL_HANDLE;
        VulkanAllocation allocation;
        if (!m_allocator.allocate_image(desc, image, allocation))
        {
            return nullptr;
        }

        VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        if (desc.format == Format::D24UnormS8Uint)
        {
            aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        }
        else if (desc.format == Format::D32Sfloat)
        {
            aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
        }

        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = image;
        view_info.viewType = desc.array_layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = to_vk_format(desc.format);
        view_info.subresourceRange = {aspect, 0, desc.mip_levels, 0, desc.array_layers};

        VkImageView view = VK_NULL_HANDLE;
        if (!vk_ok(vkCreateImageView(m_device, &view_info, nullptr, &view), "vkCreateImageView(image)"))
        {
            m_allocator.destroy_allocation(allocation);
            vkDestroyImage(m_device, image, nullptr);
            return nullptr;
        }

        return std::make_unique<VulkanImage>(m_device, desc, image, view, allocation, true, &m_allocator);
    }

    [[nodiscard]] std::unique_ptr<ShaderModule> create_shader_module(const ShaderModuleDesc& desc) override
    {
        if (desc.code.empty() || (desc.code.size() % 4U) != 0U)
        {
            CRD_LOG_ERROR(detail::g_log_rhi_vulkan,
                          "Shader module code must be non-empty SPIR-V with 4-byte alignment");
            return nullptr;
        }

        VkShaderModuleCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        create_info.codeSize = desc.code.size();
        create_info.pCode = reinterpret_cast<const crd::u32*>(desc.code.data());

        VkShaderModule module = VK_NULL_HANDLE;
        if (!vk_ok(vkCreateShaderModule(m_device, &create_info, nullptr, &module), "vkCreateShaderModule"))
        {
            return nullptr;
        }

        return std::make_unique<VulkanShaderModule>(m_device, desc, module);
    }

    [[nodiscard]] std::unique_ptr<Pipeline> create_graphics_pipeline(const GraphicsPipelineDesc& desc) override
    {
        auto* vertex_shader = dynamic_cast<VulkanShaderModule*>(desc.vertex_shader);
        if (vertex_shader == nullptr)
        {
            CRD_LOG_ERROR(detail::g_log_rhi_vulkan,
                          "Graphics pipeline requires a VulkanShaderModule-backed vertex shader");
            return nullptr;
        }

        // Fragment shader is optional: null means depth-only pipeline (no color output).
        auto* fragment_shader = dynamic_cast<VulkanShaderModule*>(desc.fragment_shader);

        crd::u32 stage_count = 1;
        VkPipelineShaderStageCreateInfo shader_stages[2]{};
        shader_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shader_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        shader_stages[0].module = vertex_shader->handle();
        shader_stages[0].pName = vertex_shader->entry_point().data();

        if (fragment_shader != nullptr)
        {
            shader_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            shader_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            shader_stages[1].module = fragment_shader->handle();
            shader_stages[1].pName = fragment_shader->entry_point().data();
            stage_count = 2;
        }

        crd::containers::Array<VkVertexInputBindingDescription> binding_descs;
        for (const auto& binding : desc.vertex_bindings)
        {
            binding_descs.push_back(VkVertexInputBindingDescription{binding.binding, binding.stride_bytes,
                                                                    to_vk_input_rate(binding.input_rate)});
        }

        crd::containers::Array<VkVertexInputAttributeDescription> attr_descs;
        for (const auto& attr : desc.vertex_attributes)
        {
            attr_descs.push_back(VkVertexInputAttributeDescription{attr.location, attr.binding,
                                                                   to_vk_format(attr.format), attr.offset_bytes});
        }

        VkPipelineVertexInputStateCreateInfo vertex_input{};
        vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertex_input.vertexBindingDescriptionCount = static_cast<crd::u32>(binding_descs.size());
        vertex_input.pVertexBindingDescriptions = binding_descs.data();
        vertex_input.vertexAttributeDescriptionCount = static_cast<crd::u32>(attr_descs.size());
        vertex_input.pVertexAttributeDescriptions = attr_descs.data();

        VkPipelineInputAssemblyStateCreateInfo input_assembly{};
        input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        input_assembly.topology = to_vk_topology(desc.topology);

        VkViewport viewport{};
        viewport.x = 0.0F;
        viewport.y = 0.0F;
        viewport.width = static_cast<float>(desc.viewport_extent.width);
        viewport.height = static_cast<float>(desc.viewport_extent.height);
        viewport.minDepth = 0.0F;
        viewport.maxDepth = 1.0F;
        VkRect2D scissor{{0, 0}, {desc.viewport_extent.width, desc.viewport_extent.height}};
        VkPipelineViewportStateCreateInfo viewport_state{};
        viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport_state.viewportCount = 1;
        viewport_state.scissorCount  = 1;
        if (!desc.use_dynamic_viewport)
        {
            viewport_state.pViewports = &viewport;
            viewport_state.pScissors  = &scissor;
        }

        VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic_state{};
        dynamic_state.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic_state.dynamicStateCount = 2;
        dynamic_state.pDynamicStates    = dynamic_states;

        VkPipelineRasterizationStateCreateInfo raster{};
        raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode = desc.wireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
        raster.lineWidth = 1.0F;

        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        const bool has_color_output = desc.color_format != Format::Undefined;

        VkPipelineColorBlendAttachmentState blend_attachment{};
        blend_attachment.blendEnable = desc.enable_blend ? VK_TRUE : VK_FALSE;
        // Standard alpha blending: srcRGB * srcAlpha + dstRGB * (1 - srcAlpha).
        // Without these factors the defaults are VK_BLEND_FACTOR_ZERO -> every
        // blended pixel renders as (0,0,0,0). Bug surfaced by crd-draw d0d when
        // it became the first RHI consumer of enable_blend=true.
        blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend_attachment.colorBlendOp        = VK_BLEND_OP_ADD;
        blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend_attachment.alphaBlendOp        = VK_BLEND_OP_ADD;
        blend_attachment.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo blend_state{};
        blend_state.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend_state.attachmentCount = has_color_output ? 1U : 0U;
        blend_state.pAttachments = has_color_output ? &blend_attachment : nullptr;

        auto to_vk_compare = [](DepthCompareOp op) noexcept -> VkCompareOp {
            switch (op)
            {
                case DepthCompareOp::Never:          return VK_COMPARE_OP_NEVER;
                case DepthCompareOp::Less:           return VK_COMPARE_OP_LESS;
                case DepthCompareOp::Equal:          return VK_COMPARE_OP_EQUAL;
                case DepthCompareOp::LessOrEqual:    return VK_COMPARE_OP_LESS_OR_EQUAL;
                case DepthCompareOp::Greater:        return VK_COMPARE_OP_GREATER;
                case DepthCompareOp::NotEqual:       return VK_COMPARE_OP_NOT_EQUAL;
                case DepthCompareOp::GreaterOrEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
                case DepthCompareOp::Always:         return VK_COMPARE_OP_ALWAYS;
            }
            return VK_COMPARE_OP_GREATER_OR_EQUAL;
        };

        VkPipelineDepthStencilStateCreateInfo depth_stencil{};
        depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth_stencil.depthTestEnable  = desc.enable_depth_test ? VK_TRUE : VK_FALSE;
        depth_stencil.depthWriteEnable = (desc.enable_depth_test && desc.depth_write) ? VK_TRUE : VK_FALSE;
        // Default GreaterOrEqual matches Cerid's reverse-Z. crd-draw d2-depth
        // overrides to Less for the GreaterDimmed pipeline (XRay occluded).
        depth_stencil.depthCompareOp = to_vk_compare(desc.depth_compare_op);

        // Resolve pipeline layout: use the provided layout or synthesise an empty one.
        VkPipelineLayout resolved_layout = VK_NULL_HANDLE;
        VkPipelineLayout synthesised_empty_layout = VK_NULL_HANDLE;
        if (desc.pipeline_layout != nullptr)
        {
            auto* vk_layout = dynamic_cast<VulkanPipelineLayout*>(desc.pipeline_layout);
            if (vk_layout == nullptr)
            {
                CRD_LOG_ERROR(detail::g_log_rhi_vulkan,
                              "GraphicsPipelineDesc::pipeline_layout is not a VulkanPipelineLayout");
                return nullptr;
            }
            resolved_layout = vk_layout->handle();
        }
        else
        {
            VkPipelineLayoutCreateInfo layout_info{};
            layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            if (!vk_ok(vkCreatePipelineLayout(m_device, &layout_info, nullptr, &synthesised_empty_layout),
                       "vkCreatePipelineLayout(empty)"))
            {
                return nullptr;
            }
            resolved_layout = synthesised_empty_layout;
        }

        const VkFormat color_format = has_color_output ? to_vk_format(desc.color_format) : VK_FORMAT_UNDEFINED;
        VkPipelineRenderingCreateInfo rendering_info{};
        rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        rendering_info.colorAttachmentCount = has_color_output ? 1U : 0U;
        rendering_info.pColorAttachmentFormats = has_color_output ? &color_format : nullptr;
        rendering_info.depthAttachmentFormat = to_vk_format(desc.depth_format);

        VkGraphicsPipelineCreateInfo pipeline_info{};
        pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipeline_info.pNext = &rendering_info;
        pipeline_info.stageCount = stage_count;
        pipeline_info.pStages = shader_stages;
        pipeline_info.pVertexInputState = &vertex_input;
        pipeline_info.pInputAssemblyState = &input_assembly;
        pipeline_info.pViewportState = &viewport_state;
        pipeline_info.pRasterizationState = &raster;
        pipeline_info.pMultisampleState = &multisample;
        pipeline_info.pDepthStencilState = &depth_stencil;
        pipeline_info.pColorBlendState = &blend_state;
        pipeline_info.pDynamicState = desc.use_dynamic_viewport ? &dynamic_state : nullptr;
        pipeline_info.layout = resolved_layout;

        VkPipeline pipeline = VK_NULL_HANDLE;
        if (!vk_ok(vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline),
                   "vkCreateGraphicsPipelines"))
        {
            if (synthesised_empty_layout != VK_NULL_HANDLE)
            {
                vkDestroyPipelineLayout(m_device, synthesised_empty_layout, nullptr);
            }
            return nullptr;
        }

        return std::make_unique<VulkanPipeline>(m_device, desc, pipeline, synthesised_empty_layout);
    }

    // Phase 3.1.7.6 v0a (ADR-0080) — compute pipeline factory.
    // Returns nullptr cleanly on null shader / wrong stage / Vulkan failure.
    // Synthesises an empty pipeline layout if caller passed nullptr (matches
    // create_graphics_pipeline pattern).
    [[nodiscard]] std::unique_ptr<ComputePipeline> create_compute_pipeline(const ComputePipelineDesc& desc) override
    {
        auto* compute_shader = dynamic_cast<VulkanShaderModule*>(desc.compute_shader);
        if (compute_shader == nullptr)
        {
            CRD_LOG_ERROR(detail::g_log_rhi_vulkan,
                          "Compute pipeline requires a VulkanShaderModule-backed compute shader");
            return nullptr;
        }
        if (compute_shader->stage() != ShaderStage::Compute)
        {
            CRD_LOG_ERROR(detail::g_log_rhi_vulkan,
                          "Compute pipeline shader module stage must be Compute");
            return nullptr;
        }

        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipelineLayout synthesised_empty_layout = VK_NULL_HANDLE;
        if (desc.pipeline_layout != nullptr)
        {
            auto* user_layout = dynamic_cast<VulkanPipelineLayout*>(desc.pipeline_layout);
            if (user_layout == nullptr)
            {
                CRD_LOG_ERROR(detail::g_log_rhi_vulkan,
                              "Compute pipeline layout must be a VulkanPipelineLayout");
                return nullptr;
            }
            layout = user_layout->handle();
        }
        else
        {
            VkPipelineLayoutCreateInfo layout_info{};
            layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            if (!vk_ok(vkCreatePipelineLayout(m_device, &layout_info, nullptr, &synthesised_empty_layout),
                       "vkCreatePipelineLayout (compute synthesised empty)"))
            {
                return nullptr;
            }
            layout = synthesised_empty_layout;
        }

        VkPipelineShaderStageCreateInfo stage{};
        stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = compute_shader->handle();
        stage.pName  = compute_shader->entry_point().data();

        // Phase 3.1.7.6 v0b (ADR-0080 D6) — bake specialization constants
        // at create-time. Mapping entries + data are stack-local for the
        // duration of vkCreateComputePipelines; not retained afterwards.
        crd::containers::Array<VkSpecializationMapEntry> vk_map_entries;
        VkSpecializationInfo spec_info{};
        const bool has_spec = !desc.specialization_entries.empty();
        if (has_spec)
        {
            vk_map_entries.resize(desc.specialization_entries.size());
            for (crd::usize i = 0; i < desc.specialization_entries.size(); ++i)
            {
                const auto& e = desc.specialization_entries[i];
                vk_map_entries[i].constantID = e.constant_id;
                vk_map_entries[i].offset     = e.offset;
                vk_map_entries[i].size       = e.size;
            }
            spec_info.mapEntryCount = static_cast<crd::u32>(vk_map_entries.size());
            spec_info.pMapEntries   = vk_map_entries.data();
            spec_info.dataSize      = desc.specialization_data.size();
            spec_info.pData         = desc.specialization_data.data();
            stage.pSpecializationInfo = &spec_info;
        }

        VkComputePipelineCreateInfo info{};
        info.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        info.stage  = stage;
        info.layout = layout;

        VkPipeline pipeline = VK_NULL_HANDLE;
        if (!vk_ok(vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline),
                   "vkCreateComputePipelines"))
        {
            if (synthesised_empty_layout != VK_NULL_HANDLE)
            {
                vkDestroyPipelineLayout(m_device, synthesised_empty_layout, nullptr);
            }
            return nullptr;
        }

        return std::make_unique<VulkanComputePipeline>(m_device, desc, pipeline, synthesised_empty_layout);
    }

    [[nodiscard]] std::unique_ptr<CommandBuffer> create_command_buffer() override
    {
        if (!m_dynamic_rendering_enabled)
        {
            CRD_LOG_ERROR(detail::g_log_rhi_vulkan,
                          "Dynamic rendering is unavailable; command buffer path is disabled");
            return nullptr;
        }

        VkCommandBufferAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.commandPool = m_command_pool;
        alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandBufferCount = 1;

        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        if (!vk_ok(vkAllocateCommandBuffers(m_device, &alloc_info, &command_buffer), "vkAllocateCommandBuffers"))
        {
            return nullptr;
        }

        return std::make_unique<VulkanCommandBuffer>(m_device, m_command_pool, command_buffer, m_sync2_enabled);
    }

    [[nodiscard]] std::unique_ptr<Fence> create_fence() override
    {
        VkFenceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        info.flags = 0; // start unsignalled — matches Fence::is_signaled() initial contract
        VkFence fence = VK_NULL_HANDLE;
        if (!vk_ok(vkCreateFence(m_device, &info, nullptr, &fence), "vkCreateFence"))
        {
            return nullptr;
        }
        return std::make_unique<VulkanFence>(m_device, fence);
    }

    // Phase 3.1.7.6 v0d (ADR-0080 D10) — binary semaphore for cross-queue
    // GPU-GPU sync. Returns nullptr on Vulkan failure (rare; usually OOM).
    [[nodiscard]] std::unique_ptr<Semaphore> create_semaphore() override
    {
        VkSemaphoreCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkSemaphore handle = VK_NULL_HANDLE;
        if (!vk_ok(vkCreateSemaphore(m_device, &info, nullptr, &handle), "vkCreateSemaphore"))
        {
            return nullptr;
        }
        return std::make_unique<VulkanSemaphore>(m_device, handle);
    }

    [[nodiscard]] std::unique_ptr<DescriptorSetLayout>
    create_descriptor_set_layout(const DescriptorSetLayoutDesc& desc) override
    {
        crd::containers::Array<VkDescriptorSetLayoutBinding> vk_bindings;
        for (const auto& b : desc.bindings)
        {
            VkDescriptorSetLayoutBinding vk_binding{};
            vk_binding.binding = b.binding;
            vk_binding.descriptorType = to_vk_descriptor_type(b.type);
            vk_binding.descriptorCount = b.count;
            vk_binding.stageFlags = to_vk_shader_stage_flags(b.stages);
            vk_binding.pImmutableSamplers = nullptr;
            vk_bindings.push_back(vk_binding);
        }

        VkDescriptorSetLayoutCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        create_info.bindingCount = static_cast<crd::u32>(vk_bindings.size());
        create_info.pBindings = vk_bindings.data();

        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        if (!vk_ok(vkCreateDescriptorSetLayout(m_device, &create_info, nullptr, &layout),
                   "vkCreateDescriptorSetLayout"))
        {
            return nullptr;
        }

        return std::make_unique<VulkanDescriptorSetLayout>(m_device, desc, layout);
    }

    [[nodiscard]] std::unique_ptr<PipelineLayout>
    create_pipeline_layout(const PipelineLayoutDesc& desc) override
    {
        crd::containers::Array<VkDescriptorSetLayout> vk_set_layouts;
        for (const auto* set_layout : desc.set_layouts)
        {
            const auto* vk_set_layout = dynamic_cast<const VulkanDescriptorSetLayout*>(set_layout);
            CRD_ASSERT(vk_set_layout != nullptr);
            vk_set_layouts.push_back(vk_set_layout->handle());
        }

        crd::containers::Array<VkPushConstantRange> vk_push_ranges;
        for (const auto& range : desc.push_constant_ranges)
        {
            VkPushConstantRange vk_range{};
            vk_range.stageFlags = to_vk_shader_stage_flags(range.stages);
            vk_range.offset = range.offset;
            vk_range.size = range.size;
            vk_push_ranges.push_back(vk_range);
        }

        VkPipelineLayoutCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        create_info.setLayoutCount = static_cast<crd::u32>(vk_set_layouts.size());
        create_info.pSetLayouts = vk_set_layouts.data();
        create_info.pushConstantRangeCount = static_cast<crd::u32>(vk_push_ranges.size());
        create_info.pPushConstantRanges = vk_push_ranges.data();

        VkPipelineLayout layout = VK_NULL_HANDLE;
        if (!vk_ok(vkCreatePipelineLayout(m_device, &create_info, nullptr, &layout), "vkCreatePipelineLayout"))
        {
            return nullptr;
        }

        return std::make_unique<VulkanPipelineLayout>(m_device, desc, layout);
    }

    [[nodiscard]] std::unique_ptr<DescriptorAllocator>
    create_descriptor_allocator(const DescriptorAllocatorDesc& desc) override
    {
        return std::make_unique<VulkanDescriptorAllocator>(m_device, desc);
    }

    [[nodiscard]] Queue& graphics_queue() noexcept override { return *m_graphics_queue; }

    // Phase 3.1.7.6 v0d (ADR-0080 D9) — pointer-identity contract: when no
    // dedicated compute queue family was selected (FallbackGracefully + no
    // hardware support), return the SAME Queue& as graphics_queue().
    [[nodiscard]] Queue& compute_queue() noexcept override
    {
        return m_compute_queue ? *m_compute_queue : *m_graphics_queue;
    }
    [[nodiscard]] bool has_dedicated_compute_queue() const noexcept override
    {
        return m_compute_queue != nullptr;
    }

    [[nodiscard]] bool supports_shader_int64() const noexcept override
    {
        return m_shader_int64_enabled;
    }

    // Phase 3.1.7 v9a-a-async-compute (2026-05-18). Routes by pointer-
    // identity per D9 contract: `&queue == &graphics_queue()` -> graphics
    // pool; `&queue == &compute_queue()` AND a dedicated compute pool
    // exists -> compute pool. Anything else (or compute alias to graphics)
    // routes to graphics. Unknown queue (not from this device) -> nullptr.
    [[nodiscard]] std::unique_ptr<CommandBuffer>
    create_command_buffer_for_queue(Queue& queue) override
    {
        if (!m_dynamic_rendering_enabled)
        {
            CRD_LOG_ERROR(detail::g_log_rhi_vulkan,
                          "Dynamic rendering is unavailable; command buffer path is disabled");
            return nullptr;
        }
        VkCommandPool pool = VK_NULL_HANDLE;
        if (m_graphics_queue != nullptr && &queue == m_graphics_queue.get())
        {
            pool = m_command_pool;
        }
        else if (m_compute_queue != nullptr && &queue == m_compute_queue.get())
        {
            pool = (m_compute_command_pool != VK_NULL_HANDLE) ? m_compute_command_pool
                                                              : m_command_pool;
        }
        else
        {
            CRD_LOG_ERROR(detail::g_log_rhi_vulkan,
                          "create_command_buffer_for_queue: queue not owned by this device");
            return nullptr;
        }

        VkCommandBufferAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.commandPool = pool;
        alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandBufferCount = 1;

        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        if (!vk_ok(vkAllocateCommandBuffers(m_device, &alloc_info, &command_buffer),
                   "vkAllocateCommandBuffers(for_queue)"))
        {
            return nullptr;
        }
        return std::make_unique<VulkanCommandBuffer>(m_device, pool, command_buffer, m_sync2_enabled);
    }

    void wait_idle() override
    {
        if (m_device != VK_NULL_HANDLE)
        {
            vkDeviceWaitIdle(m_device);
        }
    }

private:
    VkInstance m_instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical_device = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkCommandPool m_command_pool = VK_NULL_HANDLE;
    VkCommandPool m_compute_command_pool = VK_NULL_HANDLE;
    crd::u32 m_graphics_family_index = 0;
    crd::u32 m_compute_family_index = UINT32_MAX;
    DeviceDesc m_desc{};
    bool m_sync2_enabled = false;
    bool m_dynamic_rendering_enabled = false;
    bool m_shader_int64_enabled      = false; // Phase 3.1.7 v9a-60bit-gpu
    VulkanAllocator m_allocator;
    VkQueue m_graphics_queue_handle = VK_NULL_HANDLE;
    VkQueue m_compute_queue_handle = VK_NULL_HANDLE;
    std::unique_ptr<VulkanQueue> m_graphics_queue{};
    std::unique_ptr<VulkanQueue> m_compute_queue{}; // null on fallback (alias graphics_queue)
};

class VulkanInstance final : public Instance
{
public:
    explicit VulkanInstance(const InstanceDesc& desc)
    {
        crd::u32 glfw_extension_count = 0;
        const char** glfw_extensions = glfwGetRequiredInstanceExtensions(&glfw_extension_count);

        crd::containers::Array<const char*> enabled_extensions;
        if (glfw_extensions != nullptr)
        {
            for (crd::u32 i = 0; i < glfw_extension_count; ++i)
            {
                enabled_extensions.push_back(glfw_extensions[i]);
            }
        }
        else
        {
            CRD_LOG_WARN(detail::g_log_rhi_vulkan,
                         "GLFW is not initialised; creating Vulkan instance without window-system extensions");
        }

        crd::u32 instance_extension_count = 0;
        if (!vk_ok(vkEnumerateInstanceExtensionProperties(nullptr, &instance_extension_count, nullptr),
                   "vkEnumerateInstanceExtensionProperties(count)"))
        {
            return;
        }
        crd::containers::Array<VkExtensionProperties> instance_extensions;
        instance_extensions.resize(instance_extension_count);
        if (!vk_ok(
                vkEnumerateInstanceExtensionProperties(nullptr, &instance_extension_count, instance_extensions.data()),
                "vkEnumerateInstanceExtensionProperties(data)"))
        {
            return;
        }

        if (desc.enable_validation &&
            has_extension(crd::containers::as_const_span(instance_extensions), VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
        {
            enabled_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            m_validation_enabled = true;
        }

        // Surfaced 2026-05-18 by Phase 3.1.7 v9a-a `ValidationCapture` test
        // (first consumer to attach a debug-utils messenger from outside the
        // engine): the device unconditionally enables `VK_KHR_swapchain`,
        // which per the Vulkan spec REQUIRES the instance to enable
        // `VK_KHR_surface` first
        // (VUID-vkCreateDevice-ppEnabledExtensionNames-01387). When GLFW
        // is initialised, `glfwGetRequiredInstanceExtensions` already adds
        // `VK_KHR_surface` (plus the platform variant). When GLFW is NOT
        // initialised (typical for compute-only tests), nothing did — the
        // validation layer reported a hard error on every device create
        // even though Cerid silently worked. Add it defensively here so
        // both paths comply.
        bool surface_already_enabled = false;
        for (const char* name : enabled_extensions)
        {
            if (std::strcmp(name, VK_KHR_SURFACE_EXTENSION_NAME) == 0)
            {
                surface_already_enabled = true;
                break;
            }
        }
        if (!surface_already_enabled
            && has_extension(crd::containers::as_const_span(instance_extensions),
                              VK_KHR_SURFACE_EXTENSION_NAME))
        {
            enabled_extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
        }

        crd::u32 layer_count = 0;
        if (!vk_ok(vkEnumerateInstanceLayerProperties(&layer_count, nullptr),
                   "vkEnumerateInstanceLayerProperties(count)"))
        {
            return;
        }
        crd::containers::Array<VkLayerProperties> layers;
        layers.resize(layer_count);
        if (!vk_ok(vkEnumerateInstanceLayerProperties(&layer_count, layers.data()),
                   "vkEnumerateInstanceLayerProperties(data)"))
        {
            return;
        }

        crd::containers::Array<const char*> enabled_layers;
        if (desc.enable_validation && has_layer(crd::containers::as_const_span(layers), "VK_LAYER_KHRONOS_validation"))
        {
            enabled_layers.push_back("VK_LAYER_KHRONOS_validation");
        }
        else if (desc.enable_validation)
        {
            CRD_LOG_WARN(detail::g_log_rhi_vulkan,
                         "Validation requested but VK_LAYER_KHRONOS_validation is unavailable");
            m_validation_enabled = false;
        }

        VkApplicationInfo app_info{};
        app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app_info.pApplicationName = desc.application_name.c_str();
        app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
        app_info.pEngineName = "Cerid";
        app_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
        app_info.apiVersion = VK_API_VERSION_1_3;

        VkInstanceCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        create_info.pApplicationInfo = &app_info;
        create_info.enabledLayerCount = static_cast<crd::u32>(enabled_layers.size());
        create_info.ppEnabledLayerNames = enabled_layers.data();
        create_info.enabledExtensionCount = static_cast<crd::u32>(enabled_extensions.size());
        create_info.ppEnabledExtensionNames = enabled_extensions.data();

        if (!vk_ok(vkCreateInstance(&create_info, nullptr, &m_instance), "vkCreateInstance"))
        {
            return;
        }

        if (m_validation_enabled)
        {
            VkDebugUtilsMessengerCreateInfoEXT debug_info{};
            debug_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            debug_info.messageSeverity =
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            debug_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                     VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            debug_info.pfnUserCallback = &debug_callback;

            const auto create_debug_utils_messenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
            if (create_debug_utils_messenger != nullptr)
            {
                if (!vk_ok(create_debug_utils_messenger(m_instance, &debug_info, nullptr, &m_debug_messenger),
                           "vkCreateDebugUtilsMessengerEXT"))
                {
                    return;
                }
            }
        }

        m_valid = true;
    }

    ~VulkanInstance() noexcept override
    {
        if (m_debug_messenger != VK_NULL_HANDLE)
        {
            const auto destroy_debug_utils_messenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
            if (destroy_debug_utils_messenger != nullptr)
            {
                destroy_debug_utils_messenger(m_instance, m_debug_messenger, nullptr);
            }
            m_debug_messenger = VK_NULL_HANDLE;
        }

        if (m_instance != VK_NULL_HANDLE)
        {
            vkDestroyInstance(m_instance, nullptr);
            m_instance = VK_NULL_HANDLE;
        }
    }

    [[nodiscard]] BackendApi api() const noexcept override { return BackendApi::Vulkan; }
    [[nodiscard]] VkInstance handle() const noexcept { return m_instance; }

    void enumerate_adapters(crd::containers::Array<AdapterInfo>& out) const override
    {
        if (!m_valid || m_instance == VK_NULL_HANDLE)
        {
            return;
        }

        crd::u32 device_count = 0;
        if (!vk_ok(vkEnumeratePhysicalDevices(m_instance, &device_count, nullptr), "vkEnumeratePhysicalDevices(count)"))
        {
            return;
        }

        crd::containers::Array<VkPhysicalDevice> devices;
        devices.resize(device_count);
        if (!vk_ok(vkEnumeratePhysicalDevices(m_instance, &device_count, devices.data()),
                   "vkEnumeratePhysicalDevices(data)"))
        {
            return;
        }

        for (const VkPhysicalDevice device : devices)
        {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(device, &props);

            AdapterType type = AdapterType::Other;
            switch (props.deviceType)
            {
                case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
                    type = AdapterType::IntegratedGpu;
                    break;
                case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
                    type = AdapterType::DiscreteGpu;
                    break;
                case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
                    type = AdapterType::VirtualGpu;
                    break;
                case VK_PHYSICAL_DEVICE_TYPE_CPU:
                    type = AdapterType::Cpu;
                    break;
                default:
                    break;
            }

            out.push_back(AdapterInfo{crd::containers::String(props.deviceName), type, 0, true, true});
        }
    }

    [[nodiscard]] std::unique_ptr<Device> create_device(const DeviceDesc& desc) override
    {
        if (!m_valid || m_instance == VK_NULL_HANDLE)
        {
            CRD_LOG_ERROR(detail::g_log_rhi_vulkan, "create_device called on invalid VulkanInstance");
            return nullptr;
        }

        crd::u32 physical_device_count = 0;
        if (!vk_ok(vkEnumeratePhysicalDevices(m_instance, &physical_device_count, nullptr),
                   "vkEnumeratePhysicalDevices(count)"))
        {
            return nullptr;
        }

        crd::containers::Array<VkPhysicalDevice> physical_devices;
        physical_devices.resize(physical_device_count);
        if (!vk_ok(vkEnumeratePhysicalDevices(m_instance, &physical_device_count, physical_devices.data()),
                   "vkEnumeratePhysicalDevices(data)"))
        {
            return nullptr;
        }

        if (desc.preferred_adapter_index >= physical_devices.size())
        {
            CRD_LOG_ERROR(detail::g_log_rhi_vulkan, "Preferred adapter index {} is out of range (adapter count={})",
                          desc.preferred_adapter_index, physical_devices.size());
            return nullptr;
        }

        const VkPhysicalDevice physical_device = physical_devices[desc.preferred_adapter_index];

        VkPhysicalDeviceDynamicRenderingFeatures dynamic_rendering_features{};
        dynamic_rendering_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;

        VkPhysicalDeviceSynchronization2Features synchronization2_features{};
        synchronization2_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
        dynamic_rendering_features.pNext = &synchronization2_features;
        VkPhysicalDeviceFeatures2 features2{};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features2.pNext = &dynamic_rendering_features;
        vkGetPhysicalDeviceFeatures2(physical_device, &features2);
        const bool fill_mode_non_solid_supported = features2.features.fillModeNonSolid == VK_TRUE;
        // Phase 3.1.7 v9a-60bit-gpu (2026-05-18) — `shaderInt64` is a
        // core 1.0 feature toggle (not an extension); we just read +
        // optionally enable it. Surfaced via Device::supports_shader_int64().
        const bool shader_int64_supported = features2.features.shaderInt64 == VK_TRUE;

        const bool dynamic_rendering_supported = dynamic_rendering_features.dynamicRendering == VK_TRUE;
        const bool sync2_supported = synchronization2_features.synchronization2 == VK_TRUE;
        if (!dynamic_rendering_supported)
        {
            CRD_LOG_ERROR(detail::g_log_rhi_vulkan, "Selected adapter does not support dynamic rendering");
            return nullptr;
        }

        crd::u32 queue_family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, nullptr);
        crd::containers::Array<VkQueueFamilyProperties> queue_families;
        queue_families.resize(queue_family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, queue_families.data());

        crd::u32 graphics_family_index = UINT32_MAX;
        for (crd::u32 i = 0; i < queue_family_count; ++i)
        {
            if ((queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
            {
                graphics_family_index = i;
                break;
            }
        }
        if (graphics_family_index == UINT32_MAX)
        {
            CRD_LOG_ERROR(detail::g_log_rhi_vulkan, "No graphics queue family found on selected adapter");
            return nullptr;
        }

        // Phase 3.1.7.6 v0d (ADR-0080 D9) — probe for dedicated compute
        // queue family (VK_QUEUE_COMPUTE_BIT && !VK_QUEUE_GRAPHICS_BIT).
        // RequireDedicated fails device creation if absent;
        // FallbackGracefully (default) reuses the graphics queue and the
        // Device::compute_queue() accessor returns the same Queue&.
        crd::u32 compute_family_index = UINT32_MAX;
        for (crd::u32 i = 0; i < queue_family_count; ++i)
        {
            const auto flags = queue_families[i].queueFlags;
            if ((flags & VK_QUEUE_COMPUTE_BIT) != 0 && (flags & VK_QUEUE_GRAPHICS_BIT) == 0)
            {
                compute_family_index = i;
                break;
            }
        }
        if (desc.async_compute_policy == AsyncComputePolicy::RequireDedicated &&
            compute_family_index == UINT32_MAX)
        {
            CRD_LOG_ERROR(detail::g_log_rhi_vulkan,
                          "RequireDedicated: no dedicated compute queue family on selected adapter");
            return nullptr;
        }

        const float queue_priority = 1.0F;
        VkDeviceQueueCreateInfo queue_create_infos[2]{};
        queue_create_infos[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_create_infos[0].queueFamilyIndex = graphics_family_index;
        queue_create_infos[0].queueCount = 1;
        queue_create_infos[0].pQueuePriorities = &queue_priority;
        crd::u32 queue_create_info_count = 1;
        if (compute_family_index != UINT32_MAX)
        {
            queue_create_infos[1].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queue_create_infos[1].queueFamilyIndex = compute_family_index;
            queue_create_infos[1].queueCount = 1;
            queue_create_infos[1].pQueuePriorities = &queue_priority;
            queue_create_info_count = 2;
        }

        VkPhysicalDeviceSynchronization2Features enabled_sync2{};
        enabled_sync2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
        enabled_sync2.synchronization2 = sync2_supported ? VK_TRUE : VK_FALSE;
        VkPhysicalDeviceDynamicRenderingFeatures enabled_dynamic_rendering{};
        enabled_dynamic_rendering.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
        enabled_dynamic_rendering.pNext = &enabled_sync2;
        enabled_dynamic_rendering.dynamicRendering = VK_TRUE;

        // Enable base features (fillModeNonSolid for wireframe rendering) via features2 pNext chain.
        VkPhysicalDeviceFeatures2 enabled_features2{};
        enabled_features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        enabled_features2.features.fillModeNonSolid = fill_mode_non_solid_supported ? VK_TRUE : VK_FALSE;
        enabled_features2.features.shaderInt64      = shader_int64_supported ? VK_TRUE : VK_FALSE;
        enabled_features2.pNext = &enabled_dynamic_rendering;

        const char* device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkDeviceCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        create_info.pNext = &enabled_features2;
        create_info.queueCreateInfoCount = queue_create_info_count;
        create_info.pQueueCreateInfos = queue_create_infos;
        create_info.enabledExtensionCount = 1;
        create_info.ppEnabledExtensionNames = device_extensions;

        VkDevice device = VK_NULL_HANDLE;
        if (!vk_ok(vkCreateDevice(physical_device, &create_info, nullptr, &device), "vkCreateDevice"))
        {
            return nullptr;
        }

        return std::make_unique<VulkanDevice>(m_instance, physical_device, device, graphics_family_index,
                                              compute_family_index, desc,
                                              sync2_supported, dynamic_rendering_supported,
                                              shader_int64_supported);
    }

private:
    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debug_messenger = VK_NULL_HANDLE;
    bool m_validation_enabled = false;
    bool m_valid = false;
};
} // namespace

std::unique_ptr<Instance> create_vulkan_instance(const InstanceDesc& desc)
{
    return std::make_unique<VulkanInstance>(desc);
}

crd::u32 vulkan_resident_block_count(const Device& device) noexcept
{
    const auto* vk = dynamic_cast<const VulkanDevice*>(&device);
    return vk != nullptr ? vk->resident_block_count() : 0U;
}

void vulkan_defragment(Device& device, IDefragPolicy& policy)
{
    auto* vk = dynamic_cast<VulkanDevice*>(&device);
    if (vk != nullptr)
    {
        vk->defragment(policy);
    }
}

void vulkan_configure_residency(Device& device, IResidencyPolicy* policy, crd::u64 device_local_budget)
{
    auto* vk = dynamic_cast<VulkanDevice*>(&device);
    if (vk != nullptr)
    {
        vk->configure_residency(policy, device_local_budget);
    }
}

crd::u64 vulkan_device_local_used(const Device& device) noexcept
{
    const auto* vk = dynamic_cast<const VulkanDevice*>(&device);
    return vk != nullptr ? vk->device_local_used() : 0U;
}

crd::u64 vulkan_make_resident(Device& device, Buffer& buffer)
{
    auto* vk = dynamic_cast<VulkanDevice*>(&device);
    return vk != nullptr ? vk->make_resident(buffer) : 0U;
}

crd::u64 vulkan_evict_to_host(Device& device, Buffer& buffer)
{
    auto* vk = dynamic_cast<VulkanDevice*>(&device);
    return vk != nullptr ? vk->evict_to_host(buffer) : 0U;
}

namespace detail
{
VkInstance vulkan_instance_impl(Instance& instance) noexcept
{
    auto* vk = dynamic_cast<VulkanInstance*>(&instance);
    CRD_ASSERT(vk != nullptr);
    return vk->handle();
}

VkPhysicalDevice vulkan_physical_device_impl(Device& device) noexcept
{
    auto* vk = dynamic_cast<VulkanDevice*>(&device);
    CRD_ASSERT(vk != nullptr);
    return vk->physical_device();
}

VkDevice vulkan_device_impl(Device& device) noexcept
{
    auto* vk = dynamic_cast<VulkanDevice*>(&device);
    CRD_ASSERT(vk != nullptr);
    return vk->handle();
}

VkQueue vulkan_graphics_queue_impl(Device& device) noexcept
{
    auto* vk = dynamic_cast<VulkanDevice*>(&device);
    CRD_ASSERT(vk != nullptr);
    return vk->graphics_queue_handle();
}

crd::u32 vulkan_graphics_queue_family_index_impl(Device& device) noexcept
{
    auto* vk = dynamic_cast<VulkanDevice*>(&device);
    CRD_ASSERT(vk != nullptr);
    return vk->graphics_family_index();
}

VkCommandBuffer vulkan_command_buffer_impl(CommandBuffer& command_buffer) noexcept
{
    auto* vk = dynamic_cast<VulkanCommandBuffer*>(&command_buffer);
    CRD_ASSERT(vk != nullptr);
    return vk->handle();
}

VkFormat vulkan_swapchain_color_format_impl(Swapchain& swapchain) noexcept
{
    auto* vk = dynamic_cast<VulkanSwapchain*>(&swapchain);
    CRD_ASSERT(vk != nullptr);
    return to_vk_format(vk->desc().color_format);
}

crd::u32 vulkan_swapchain_image_count_impl(Swapchain& swapchain) noexcept
{
    auto* vk = dynamic_cast<VulkanSwapchain*>(&swapchain);
    CRD_ASSERT(vk != nullptr);
    return vk->image_count();
}
} // namespace detail

VkInstance vulkan_instance(Instance& instance) noexcept
{
    return detail::vulkan_instance_impl(instance);
}

VkPhysicalDevice vulkan_physical_device(Device& device) noexcept
{
    return detail::vulkan_physical_device_impl(device);
}

VkDevice vulkan_device(Device& device) noexcept
{
    return detail::vulkan_device_impl(device);
}

VkQueue vulkan_graphics_queue(Device& device) noexcept
{
    return detail::vulkan_graphics_queue_impl(device);
}

crd::u32 vulkan_graphics_queue_family_index(Device& device) noexcept
{
    return detail::vulkan_graphics_queue_family_index_impl(device);
}

VkCommandBuffer vulkan_command_buffer(CommandBuffer& command_buffer) noexcept
{
    return detail::vulkan_command_buffer_impl(command_buffer);
}

VkFormat vulkan_swapchain_color_format(Swapchain& swapchain) noexcept
{
    return detail::vulkan_swapchain_color_format_impl(swapchain);
}

crd::u32 vulkan_swapchain_image_count(Swapchain& swapchain) noexcept
{
    return detail::vulkan_swapchain_image_count_impl(swapchain);
}
} // namespace crd::rhi
