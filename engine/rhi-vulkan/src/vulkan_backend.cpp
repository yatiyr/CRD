#include "log_channel.hpp"

#include <crd/log/log.hpp>
#include <crd/rhi/vulkan_backend.hpp>

#define GLFW_INCLUDE_VULKAN
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
        if ((type_bits & (1u << i)) != 0u && (memory_properties.memoryTypes[i].propertyFlags & required) == required)
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
            return {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL};
        case ImageAccess::Present:
            return {VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
        case ImageAccess::Undefined:
        default:
            return {VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED};
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
        case DescriptorType::StorageBuffer:        return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        case DescriptorType::CombinedImageSampler: return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        case DescriptorType::SampledImage:         return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        case DescriptorType::Sampler:              return VK_DESCRIPTOR_TYPE_SAMPLER;
        case DescriptorType::StorageImage:         return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        case DescriptorType::UniformBuffer:
        default:                                   return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    }
}

[[nodiscard]] VkShaderStageFlags to_vk_shader_stage_flags(ShaderStage stages) noexcept
{
    VkShaderStageFlags flags = 0;
    if (has_stage(stages, ShaderStage::Vertex))   flags |= VK_SHADER_STAGE_VERTEX_BIT;
    if (has_stage(stages, ShaderStage::Fragment))  flags |= VK_SHADER_STAGE_FRAGMENT_BIT;
    if (has_stage(stages, ShaderStage::Compute))   flags |= VK_SHADER_STAGE_COMPUTE_BIT;
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

struct VulkanAllocation
{
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size_bytes = 0;
    crd::u32 memory_type_index = 0;
};

class VulkanAllocator
{
public:
    VulkanAllocator(VkPhysicalDevice physical_device, VkDevice device)
        : m_physical_device(physical_device), m_device(device)
    {
    }

    [[nodiscard]] bool allocate_buffer(const BufferDesc& desc, VkBuffer& out_buffer, VulkanAllocation& out_allocation)
    {
        VkBufferCreateInfo create_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        create_info.size = desc.size_bytes;
        create_info.usage = to_vk_buffer_usage(desc.usage);
        create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (!vk_ok(vkCreateBuffer(m_device, &create_info, nullptr, &out_buffer), "vkCreateBuffer"))
        {
            return false;
        }

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(m_device, out_buffer, &requirements);
        if (!allocate_memory(requirements, to_vk_memory_properties(desc.memory_usage), out_allocation,
                             "vkAllocateMemory(buffer)"))
        {
            vkDestroyBuffer(m_device, out_buffer, nullptr);
            out_buffer = VK_NULL_HANDLE;
            return false;
        }

        if (!vk_ok(vkBindBufferMemory(m_device, out_buffer, out_allocation.memory, 0), "vkBindBufferMemory"))
        {
            destroy_allocation(out_allocation);
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

        VkImageCreateInfo create_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        create_info.imageType = VK_IMAGE_TYPE_2D;
        create_info.format = to_vk_format(desc.format);
        create_info.extent = {desc.extent.width, desc.extent.height, 1};
        create_info.mipLevels = desc.mip_levels;
        create_info.arrayLayers = desc.array_layers;
        create_info.samples = VK_SAMPLE_COUNT_1_BIT;
        create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        create_info.usage = usage;
        create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (!vk_ok(vkCreateImage(m_device, &create_info, nullptr, &out_image), "vkCreateImage"))
        {
            return false;
        }

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(m_device, out_image, &requirements);
        if (!allocate_memory(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, out_allocation,
                             "vkAllocateMemory(image)"))
        {
            vkDestroyImage(m_device, out_image, nullptr);
            out_image = VK_NULL_HANDLE;
            return false;
        }

        if (!vk_ok(vkBindImageMemory(m_device, out_image, out_allocation.memory, 0), "vkBindImageMemory"))
        {
            destroy_allocation(out_allocation);
            vkDestroyImage(m_device, out_image, nullptr);
            out_image = VK_NULL_HANDLE;
            return false;
        }

        return true;
    }

    void destroy_allocation(VulkanAllocation& allocation) noexcept
    {
        if (allocation.memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(m_device, allocation.memory, nullptr);
            allocation.memory = VK_NULL_HANDLE;
            allocation.size_bytes = 0;
        }
    }

private:
    [[nodiscard]] bool allocate_memory(const VkMemoryRequirements& requirements,
                                       VkMemoryPropertyFlags required_properties, VulkanAllocation& out_allocation,
                                       const char* what)
    {
        const crd::u32 memory_type_index =
            find_memory_type(m_physical_device, requirements.memoryTypeBits, required_properties);
        if (memory_type_index == UINT32_MAX)
        {
            CRD_LOG_ERROR(detail::g_log_rhi_vulkan, "No compatible Vulkan memory type for {}", what);
            return false;
        }

        VkMemoryAllocateInfo alloc_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc_info.allocationSize = requirements.size;
        alloc_info.memoryTypeIndex = memory_type_index;

        VkDeviceMemory memory = VK_NULL_HANDLE;
        if (!vk_ok(vkAllocateMemory(m_device, &alloc_info, nullptr, &memory), what))
        {
            return false;
        }

        out_allocation.memory = memory;
        out_allocation.size_bytes = requirements.size;
        out_allocation.memory_type_index = memory_type_index;
        return true;
    }

    VkPhysicalDevice m_physical_device = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
};

class VulkanImage final : public Image
{
public:
    VulkanImage(VkDevice device, ImageDesc desc, VkImage image, VkImageView image_view,
                VulkanAllocation allocation = {}, bool owns_image = false)
        : m_device(device), m_desc(std::move(desc)), m_image(image), m_image_view(image_view), m_allocation(allocation),
          m_owns_image(owns_image)
    {
    }

    ~VulkanImage() noexcept override
    {
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
        if (m_allocation.memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(m_device, m_allocation.memory, nullptr);
            m_allocation.memory = VK_NULL_HANDLE;
        }
    }

    [[nodiscard]] const ImageDesc& desc() const noexcept override { return m_desc; }
    [[nodiscard]] VkImage handle() const noexcept { return m_image; }
    [[nodiscard]] VkImageView image_view() const noexcept { return m_image_view; }
    [[nodiscard]] VkImageLayout layout() const noexcept { return m_layout; }
    void set_layout(VkImageLayout layout) noexcept { m_layout = layout; }

private:
    VkDevice m_device = VK_NULL_HANDLE;
    ImageDesc m_desc{};
    VkImage m_image = VK_NULL_HANDLE;
    VkImageView m_image_view = VK_NULL_HANDLE;
    VkImageLayout m_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VulkanAllocation m_allocation{};
    bool m_owns_image = false;
};

class VulkanBuffer final : public Buffer
{
public:
    VulkanBuffer(VkDevice device, BufferDesc desc, VkBuffer buffer, VulkanAllocation allocation)
        : m_device(device), m_desc(desc), m_buffer(buffer), m_allocation(allocation)
    {
    }

    ~VulkanBuffer() noexcept override
    {
        if (m_buffer != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(m_device, m_buffer, nullptr);
            m_buffer = VK_NULL_HANDLE;
        }
        if (m_allocation.memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(m_device, m_allocation.memory, nullptr);
            m_allocation.memory = VK_NULL_HANDLE;
        }
    }

    [[nodiscard]] const BufferDesc& desc() const noexcept override { return m_desc; }

    [[nodiscard]] void* map() noexcept override
    {
        if (m_desc.memory_usage == MemoryUsage::GpuOnly)
        {
            return nullptr;
        }
        void* mapped = nullptr;
        if (vkMapMemory(m_device, m_allocation.memory, 0, m_desc.size_bytes, 0, &mapped) != VK_SUCCESS)
        {
            return nullptr;
        }
        return mapped;
    }

    void unmap() noexcept override
    {
        if (m_desc.memory_usage != MemoryUsage::GpuOnly)
        {
            vkUnmapMemory(m_device, m_allocation.memory);
        }
    }

    [[nodiscard]] VkBuffer handle() const noexcept { return m_buffer; }

private:
    VkDevice m_device = VK_NULL_HANDLE;
    BufferDesc m_desc{};
    VkBuffer m_buffer = VK_NULL_HANDLE;
    VulkanAllocation m_allocation{};
};

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
    VulkanDescriptorSetLayout(VkDevice device, DescriptorSetLayoutDesc desc, VkDescriptorSetLayout layout)
        : m_device(device), m_desc(std::move(desc)), m_layout(layout)
    {
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
    DescriptorSetLayoutDesc m_desc{};
    VkDescriptorSetLayout m_layout = VK_NULL_HANDLE;
};

class VulkanPipelineLayout final : public PipelineLayout
{
public:
    VulkanPipelineLayout(VkDevice device, PipelineLayoutDesc desc, VkPipelineLayout layout)
        : m_device(device), m_desc(std::move(desc)), m_layout(layout)
    {
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
    PipelineLayoutDesc m_desc{};
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
};

class VulkanDescriptorSet final : public DescriptorSet
{
public:
    VulkanDescriptorSet(VkDevice device, VkDescriptorSet set,
                        crd::containers::Array<DescriptorBinding> bindings)
        : m_device(device), m_set(set), m_bindings(std::move(bindings))
    {
    }

    // The VkDescriptorSet is owned by its pool and not freed individually —
    // the pool is reset en-masse in VulkanDescriptorAllocator::begin_frame().
    ~VulkanDescriptorSet() noexcept override = default;

    void update_buffer(crd::u32 binding_slot, Buffer& buffer,
                       crd::u64 offset_bytes, crd::u64 size_bytes) override
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
        buffer_info.range  = (size_bytes == 0) ? VK_WHOLE_SIZE : size_bytes;

        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet          = m_set;
        write.dstBinding      = binding_slot;
        write.dstArrayElement = 0;
        write.descriptorType  = vk_type;
        write.descriptorCount = 1;
        write.pBufferInfo     = &buffer_info;

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
        m_frames_in_flight  = desc.frames_in_flight;
        m_alloc_desc        = desc;
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
        VkDescriptorSetAllocateInfo alloc_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc_info.descriptorPool     = m_pools[m_current_pool_index];
        alloc_info.descriptorSetCount = 1;
        alloc_info.pSetLayouts        = &raw_layout;

        VkDescriptorSet raw_set = VK_NULL_HANDLE;
        if (!vk_ok(vkAllocateDescriptorSets(m_device, &alloc_info, &raw_set), "vkAllocateDescriptorSets"))
        {
            CRD_LOG_ERROR(detail::g_log_rhi_vulkan,
                          "DescriptorAllocator pool exhausted — increase max_sets_per_frame");
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
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         desc.max_uniform_buffers_per_frame},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         desc.max_storage_buffers_per_frame},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, desc.max_combined_image_samplers_per_frame},
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,          desc.max_sampled_images_per_frame},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          desc.max_storage_images_per_frame},
            {VK_DESCRIPTOR_TYPE_SAMPLER,                desc.max_samplers_per_frame},
        };

        VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool_info.maxSets       = desc.max_sets_per_frame;
        pool_info.poolSizeCount = static_cast<crd::u32>(std::size(pool_sizes));
        pool_info.pPoolSizes    = pool_sizes;

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
    crd::u32 m_frames_in_flight  = 2;
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
        CRD_ASSERT(vkResetCommandBuffer(m_command_buffer, 0) == VK_SUCCESS);
        VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        CRD_ASSERT(vkBeginCommandBuffer(m_command_buffer, &begin_info) == VK_SUCCESS);
    }

    void end() override { CRD_ASSERT(vkEndCommandBuffer(m_command_buffer) == VK_SUCCESS); }

    void reset() override
    {
        CRD_ASSERT(vkResetCommandBuffer(m_command_buffer, 0) == VK_SUCCESS);
    }

    void begin_rendering(const RenderingInfo& info) override
    {
        // Color attachment is optional (null = depth-only pass).
        auto* color_image = dynamic_cast<VulkanImage*>(info.color_attachment.image);
        const bool has_color = color_image != nullptr;

        VkRenderingAttachmentInfo color_attachment_info{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        if (has_color)
        {
            color_attachment_info.imageView   = color_image->image_view();
            color_attachment_info.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color_attachment_info.loadOp      = to_vk_load_op(info.color_attachment.load_op);
            color_attachment_info.storeOp     = to_vk_store_op(info.color_attachment.store_op);
            color_attachment_info.clearValue.color = {{info.color_attachment.clear_color.r,
                                                       info.color_attachment.clear_color.g,
                                                       info.color_attachment.clear_color.b,
                                                       info.color_attachment.clear_color.a}};
        }

        VkRenderingAttachmentInfo depth_attachment_info{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        const bool has_depth = info.depth_attachment != nullptr && info.depth_attachment->image != nullptr;
        if (has_depth)
        {
            auto* depth_image = dynamic_cast<VulkanImage*>(info.depth_attachment->image);
            CRD_ASSERT(depth_image != nullptr);
            depth_attachment_info.imageView   = depth_image->image_view();
            depth_attachment_info.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depth_attachment_info.loadOp      = to_vk_load_op(info.depth_attachment->load_op);
            depth_attachment_info.storeOp     = to_vk_store_op(info.depth_attachment->store_op);
            depth_attachment_info.clearValue.depthStencil = {info.depth_attachment->clear_depth_stencil.depth,
                                                              info.depth_attachment->clear_depth_stencil.stencil};
        }

        VkRenderingInfo rendering_info{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering_info.renderArea.offset        = {0, 0};
        rendering_info.renderArea.extent        = {info.extent.width, info.extent.height};
        rendering_info.layerCount               = 1;
        rendering_info.colorAttachmentCount     = has_color ? 1u : 0u;
        rendering_info.pColorAttachments        = has_color ? &color_attachment_info : nullptr;
        rendering_info.pDepthAttachment         = has_depth ? &depth_attachment_info : nullptr;
        vkCmdBeginRendering(m_command_buffer, &rendering_info);
    }

    void end_rendering() override
    {
        vkCmdEndRendering(m_command_buffer);
    }

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
        const VkIndexType vk_type = (type == rhi::IndexType::Uint16) ? VK_INDEX_TYPE_UINT16
                                                                      : VK_INDEX_TYPE_UINT32;
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

    void blit_image(Image& src, Image& dst,
                    Extent2D src_extent, Extent2D dst_extent) noexcept override
    {
        auto* vk_src = dynamic_cast<VulkanImage*>(&src);
        auto* vk_dst = dynamic_cast<VulkanImage*>(&dst);
        CRD_ASSERT(vk_src != nullptr && vk_dst != nullptr);

        VkImageBlit region{};
        region.srcSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        region.srcSubresource.mipLevel       = 0;
        region.srcSubresource.baseArrayLayer = 0;
        region.srcSubresource.layerCount     = 1;
        region.srcOffsets[0]                 = {0, 0, 0};
        region.srcOffsets[1]                 = {static_cast<crd::i32>(src_extent.width),
                                                static_cast<crd::i32>(src_extent.height), 1};
        region.dstSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        region.dstSubresource.mipLevel       = 0;
        region.dstSubresource.baseArrayLayer = 0;
        region.dstSubresource.layerCount     = 1;
        region.dstOffsets[0]                 = {0, 0, 0};
        region.dstOffsets[1]                 = {static_cast<crd::i32>(dst_extent.width),
                                                static_cast<crd::i32>(dst_extent.height), 1};

        vkCmdBlitImage(m_command_buffer,
                       vk_src->handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       vk_dst->handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &region,
                       VK_FILTER_LINEAR);
    }

    void push_constants(PipelineLayout& layout, ShaderStage stages,
                        crd::u32 offset, crd::u32 size, const void* data) override
    {
        auto* vk_layout = dynamic_cast<VulkanPipelineLayout*>(&layout);
        CRD_ASSERT(vk_layout != nullptr);
        vkCmdPushConstants(m_command_buffer, vk_layout->handle(),
                           to_vk_shader_stage_flags(stages), offset, size, data);
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

        vkCmdBindDescriptorSets(m_command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_layout->handle(),
                                first_set, static_cast<crd::u32>(raw_sets.size()), raw_sets.data(),
                                0, nullptr);
    }

    void transition_image(Image& image, ImageAccess from, ImageAccess to) noexcept override
    {
        if (from == to)
            return;
        auto* vk_image = dynamic_cast<VulkanImage*>(&image);
        CRD_ASSERT(vk_image != nullptr);

        const auto src = to_vk_access_info(from);
        const auto dst = to_vk_access_info(to);

        const VkImageAspectFlags aspect =
            (to == ImageAccess::DepthWrite || to == ImageAccess::DepthRead ||
             from == ImageAccess::DepthWrite || from == ImageAccess::DepthRead)
                ? VK_IMAGE_ASPECT_DEPTH_BIT
                : VK_IMAGE_ASPECT_COLOR_BIT;

        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = src.access;
        barrier.dstAccessMask = dst.access;
        barrier.oldLayout = src.layout;
        barrier.newLayout = dst.layout;
        barrier.image = vk_image->handle();
        barrier.subresourceRange.aspectMask = aspect;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;

        vkCmdPipelineBarrier(m_command_buffer, src.stage, dst.stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        vk_image->set_layout(dst.layout);
    }

    [[nodiscard]] VkCommandBuffer handle() const noexcept { return m_command_buffer; }

private:
    VkDevice        m_device         = VK_NULL_HANDLE;
    VkCommandPool   m_command_pool   = VK_NULL_HANDLE;
    VkCommandBuffer m_command_buffer = VK_NULL_HANDLE;
};

class VulkanSwapchain final : public Swapchain
{
public:
    VulkanSwapchain(VkDevice device, VkSwapchainKHR swapchain, SwapchainDesc desc, crd::u32 frames_in_flight,
                    crd::containers::Array<std::unique_ptr<VulkanImage>> images)
        : m_device(device), m_swapchain(swapchain), m_desc(std::move(desc)), m_frames_in_flight(frames_in_flight),
          m_images(std::move(images))
    {
        CRD_ASSERT(create_frame_sync_objects());
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
        if (!vk_ok(vkResetFences(m_device, 1, &frame.in_flight), "vkResetFences"))
        {
            return false;
        }

        const VkResult result = vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX, frame.image_available,
                                                      VK_NULL_HANDLE, &m_current_image_index);
        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
        {
            m_image_acquired = true;
            return true;
        }

        CRD_LOG_ERROR(detail::g_log_rhi_vulkan, "vkAcquireNextImageKHR failed with VkResult={}",
                      static_cast<int>(result));
        return false;
    }

    [[nodiscard]] crd::u32 current_image_index() const noexcept override { return m_current_image_index; }
    [[nodiscard]] Image& current_image() noexcept override { return *m_images[m_current_image_index]; }
    [[nodiscard]] VkSwapchainKHR handle() const noexcept { return m_swapchain; }
    [[nodiscard]] crd::u32 image_count() const noexcept { return static_cast<crd::u32>(m_images.size()); }

    [[nodiscard]] FrameSync& current_frame_sync() noexcept { return m_frames[m_frame_index]; }
    [[nodiscard]] ImageSync& current_image_sync() noexcept { return m_image_sync_array[m_current_image_index]; }
    [[nodiscard]] crd::u32 frame_index() const noexcept { return m_frame_index; }
    void advance_frame() noexcept { m_frame_index = (m_frame_index + 1u) % m_frames_in_flight; }
    [[nodiscard]] bool image_acquired() const noexcept { return m_image_acquired; }
    void clear_image_acquired() noexcept { m_image_acquired = false; }

private:
    [[nodiscard]] bool create_frame_sync_objects()
    {
        m_frames.resize(m_frames_in_flight);
        for (auto& frame : m_frames)
        {
            VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
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
            VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            if (!vk_ok(vkCreateSemaphore(m_device, &semaphore_info, nullptr, &image_sync.render_finished),
                       "vkCreateSemaphore(render_finished)"))
            {
                return false;
            }
        }
        return true;
    }

    VkDevice m_device = VK_NULL_HANDLE;
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
        VkSubmitInfo submit_info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit_info.waitSemaphoreCount = 1;
        submit_info.pWaitSemaphores = &frame.image_available;
        submit_info.pWaitDstStageMask = &wait_stage;
        VkCommandBuffer handle = vk_command_buffer->handle();
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &handle;
        submit_info.signalSemaphoreCount = 1;
        submit_info.pSignalSemaphores = &image_sync.render_finished;
        return vk_ok(vkQueueSubmit(m_queue, 1, &submit_info, frame.in_flight), "vkQueueSubmit");
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

        VkPresentInfoKHR present_info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present_info.waitSemaphoreCount = 1;
        present_info.pWaitSemaphores = &image_sync.render_finished;
        present_info.swapchainCount = 1;
        present_info.pSwapchains = &swapchain_handle;
        crd::u32 image_index = vk_swapchain->current_image_index();
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
                 DeviceDesc desc, bool sync2_enabled, bool dynamic_rendering_enabled)
        : m_instance(instance), m_physical_device(physical_device), m_device(device),
          m_graphics_family_index(graphics_family_index), m_desc(std::move(desc)), m_sync2_enabled(sync2_enabled),
          m_dynamic_rendering_enabled(dynamic_rendering_enabled), m_allocator(physical_device, device)
    {
        vkGetDeviceQueue(m_device, m_graphics_family_index, 0, &m_graphics_queue_handle);
        m_graphics_queue = std::make_unique<VulkanQueue>(m_graphics_queue_handle, m_sync2_enabled);

        VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = m_graphics_family_index;
        CRD_ASSERT(vkCreateCommandPool(m_device, &pool_info, nullptr, &m_command_pool) == VK_SUCCESS);
    }

    ~VulkanDevice() noexcept override
    {
        if (m_command_pool != VK_NULL_HANDLE)
        {
            vkDestroyCommandPool(m_device, m_command_pool, nullptr);
            m_command_pool = VK_NULL_HANDLE;
        }
        if (m_device != VK_NULL_HANDLE)
        {
            vkDeviceWaitIdle(m_device);
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

        VkSwapchainCreateInfoKHR create_info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
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
            VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view_info.image = image;
            view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view_info.format = chosen_format.format;
            view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            view_info.subresourceRange.baseMipLevel = 0;
            view_info.subresourceRange.levelCount = 1;
            view_info.subresourceRange.baseArrayLayer = 0;
            view_info.subresourceRange.layerCount = 1;

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
        return std::make_unique<VulkanSwapchain>(m_device, swapchain, std::move(resolved_desc), m_desc.frames_in_flight,
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

        return std::make_unique<VulkanBuffer>(m_device, desc, buffer, allocation);
    }

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

        VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_info.image = image;
        view_info.viewType = desc.array_layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = to_vk_format(desc.format);
        view_info.subresourceRange.aspectMask = aspect;
        view_info.subresourceRange.baseMipLevel = 0;
        view_info.subresourceRange.levelCount = desc.mip_levels;
        view_info.subresourceRange.baseArrayLayer = 0;
        view_info.subresourceRange.layerCount = desc.array_layers;

        VkImageView view = VK_NULL_HANDLE;
        if (!vk_ok(vkCreateImageView(m_device, &view_info, nullptr, &view), "vkCreateImageView(image)"))
        {
            m_allocator.destroy_allocation(allocation);
            vkDestroyImage(m_device, image, nullptr);
            return nullptr;
        }

        return std::make_unique<VulkanImage>(m_device, desc, image, view, allocation, true);
    }

    [[nodiscard]] std::unique_ptr<ShaderModule> create_shader_module(const ShaderModuleDesc& desc) override
    {
        if (desc.code.empty() || (desc.code.size() % 4u) != 0u)
        {
            CRD_LOG_ERROR(detail::g_log_rhi_vulkan,
                          "Shader module code must be non-empty SPIR-V with 4-byte alignment");
            return nullptr;
        }

        VkShaderModuleCreateInfo create_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
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
        shader_stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shader_stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        shader_stages[0].module = vertex_shader->handle();
        shader_stages[0].pName  = vertex_shader->entry_point().data();

        if (fragment_shader != nullptr)
        {
            shader_stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            shader_stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
            shader_stages[1].module = fragment_shader->handle();
            shader_stages[1].pName  = fragment_shader->entry_point().data();
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

        VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vertex_input.vertexBindingDescriptionCount = static_cast<crd::u32>(binding_descs.size());
        vertex_input.pVertexBindingDescriptions = binding_descs.data();
        vertex_input.vertexAttributeDescriptionCount = static_cast<crd::u32>(attr_descs.size());
        vertex_input.pVertexAttributeDescriptions = attr_descs.data();

        VkPipelineInputAssemblyStateCreateInfo input_assembly{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        input_assembly.topology = to_vk_topology(desc.topology);

        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(desc.viewport_extent.width);
        viewport.height = static_cast<float>(desc.viewport_extent.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        VkRect2D scissor{{0, 0}, {desc.viewport_extent.width, desc.viewport_extent.height}};
        VkPipelineViewportStateCreateInfo viewport_state{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport_state.viewportCount = 1;
        viewport_state.pViewports = &viewport;
        viewport_state.scissorCount = 1;
        viewport_state.pScissors = &scissor;

        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
        raster.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        const bool has_color_output = desc.color_format != Format::Undefined;

        VkPipelineColorBlendAttachmentState blend_attachment{};
        blend_attachment.blendEnable = desc.enable_blend ? VK_TRUE : VK_FALSE;
        blend_attachment.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo blend_state{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend_state.attachmentCount = has_color_output ? 1u : 0u;
        blend_state.pAttachments    = has_color_output ? &blend_attachment : nullptr;

        VkPipelineDepthStencilStateCreateInfo depth_stencil{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depth_stencil.depthTestEnable = desc.enable_depth_test ? VK_TRUE : VK_FALSE;
        depth_stencil.depthWriteEnable = desc.enable_depth_test ? VK_TRUE : VK_FALSE;
        depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

        // Resolve pipeline layout: use the provided layout or synthesise an empty one.
        VkPipelineLayout resolved_layout = VK_NULL_HANDLE;
        VkPipelineLayout synthesised_empty_layout = VK_NULL_HANDLE;
        if (desc.pipeline_layout != nullptr)
        {
            auto* vk_layout = dynamic_cast<VulkanPipelineLayout*>(desc.pipeline_layout);
            if (vk_layout == nullptr)
            {
                CRD_LOG_ERROR(detail::g_log_rhi_vulkan, "GraphicsPipelineDesc::pipeline_layout is not a VulkanPipelineLayout");
                return nullptr;
            }
            resolved_layout = vk_layout->handle();
        }
        else
        {
            VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            if (!vk_ok(vkCreatePipelineLayout(m_device, &layout_info, nullptr, &synthesised_empty_layout),
                       "vkCreatePipelineLayout(empty)"))
            {
                return nullptr;
            }
            resolved_layout = synthesised_empty_layout;
        }

        VkPipelineRenderingCreateInfo rendering_info{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        const VkFormat color_format = has_color_output ? to_vk_format(desc.color_format) : VK_FORMAT_UNDEFINED;
        rendering_info.colorAttachmentCount    = has_color_output ? 1u : 0u;
        rendering_info.pColorAttachmentFormats = has_color_output ? &color_format : nullptr;
        rendering_info.depthAttachmentFormat   = to_vk_format(desc.depth_format);

        VkGraphicsPipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pipeline_info.pNext      = &rendering_info;
        pipeline_info.stageCount = stage_count;
        pipeline_info.pStages    = shader_stages;
        pipeline_info.pVertexInputState = &vertex_input;
        pipeline_info.pInputAssemblyState = &input_assembly;
        pipeline_info.pViewportState = &viewport_state;
        pipeline_info.pRasterizationState = &raster;
        pipeline_info.pMultisampleState = &multisample;
        pipeline_info.pColorBlendState = &blend_state;
        pipeline_info.pDepthStencilState = &depth_stencil;
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

    [[nodiscard]] std::unique_ptr<CommandBuffer> create_command_buffer() override
    {
        if (!m_dynamic_rendering_enabled)
        {
            CRD_LOG_ERROR(detail::g_log_rhi_vulkan,
                          "Dynamic rendering is unavailable; command buffer path is disabled");
            return nullptr;
        }

        VkCommandBufferAllocateInfo alloc_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
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

    [[nodiscard]] std::unique_ptr<DescriptorSetLayout>
    create_descriptor_set_layout(const DescriptorSetLayoutDesc& desc) override
    {
        crd::containers::Array<VkDescriptorSetLayoutBinding> vk_bindings;
        for (const auto& b : desc.bindings)
        {
            VkDescriptorSetLayoutBinding vk_binding{};
            vk_binding.binding            = b.binding;
            vk_binding.descriptorType     = to_vk_descriptor_type(b.type);
            vk_binding.descriptorCount    = b.count;
            vk_binding.stageFlags         = to_vk_shader_stage_flags(b.stages);
            vk_binding.pImmutableSamplers = nullptr;
            vk_bindings.push_back(vk_binding);
        }

        VkDescriptorSetLayoutCreateInfo create_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        create_info.bindingCount = static_cast<crd::u32>(vk_bindings.size());
        create_info.pBindings    = vk_bindings.data();

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
            vk_range.offset     = range.offset;
            vk_range.size       = range.size;
            vk_push_ranges.push_back(vk_range);
        }

        VkPipelineLayoutCreateInfo create_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        create_info.setLayoutCount         = static_cast<crd::u32>(vk_set_layouts.size());
        create_info.pSetLayouts            = vk_set_layouts.data();
        create_info.pushConstantRangeCount = static_cast<crd::u32>(vk_push_ranges.size());
        create_info.pPushConstantRanges    = vk_push_ranges.data();

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
    crd::u32 m_graphics_family_index = 0;
    DeviceDesc m_desc{};
    bool m_sync2_enabled = false;
    bool m_dynamic_rendering_enabled = false;
    VulkanAllocator m_allocator;
    VkQueue m_graphics_queue_handle = VK_NULL_HANDLE;
    std::unique_ptr<VulkanQueue> m_graphics_queue{};
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

        VkApplicationInfo app_info{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app_info.pApplicationName = desc.application_name.c_str();
        app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
        app_info.pEngineName = "Cerid";
        app_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
        app_info.apiVersion = VK_API_VERSION_1_3;

        VkInstanceCreateInfo create_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        create_info.pApplicationInfo = &app_info;
        create_info.enabledExtensionCount = static_cast<crd::u32>(enabled_extensions.size());
        create_info.ppEnabledExtensionNames = enabled_extensions.data();
        create_info.enabledLayerCount = static_cast<crd::u32>(enabled_layers.size());
        create_info.ppEnabledLayerNames = enabled_layers.data();

        if (!vk_ok(vkCreateInstance(&create_info, nullptr, &m_instance), "vkCreateInstance"))
        {
            return;
        }

        if (m_validation_enabled)
        {
            VkDebugUtilsMessengerCreateInfoEXT debug_info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
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

        VkPhysicalDeviceDynamicRenderingFeatures dynamic_rendering_features{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
        VkPhysicalDeviceSynchronization2Features synchronization2_features{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
        dynamic_rendering_features.pNext = &synchronization2_features;
        VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features2.pNext = &dynamic_rendering_features;
        vkGetPhysicalDeviceFeatures2(physical_device, &features2);

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

        const float queue_priority = 1.0f;
        VkDeviceQueueCreateInfo queue_create_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue_create_info.queueFamilyIndex = graphics_family_index;
        queue_create_info.queueCount = 1;
        queue_create_info.pQueuePriorities = &queue_priority;

        VkPhysicalDeviceDynamicRenderingFeatures enabled_dynamic_rendering{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
        enabled_dynamic_rendering.dynamicRendering = VK_TRUE;
        VkPhysicalDeviceSynchronization2Features enabled_sync2{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
        enabled_sync2.synchronization2 = sync2_supported ? VK_TRUE : VK_FALSE;
        enabled_dynamic_rendering.pNext = &enabled_sync2;

        const char* device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkDeviceCreateInfo create_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        create_info.pNext = &enabled_dynamic_rendering;
        create_info.queueCreateInfoCount = 1;
        create_info.pQueueCreateInfos = &queue_create_info;
        create_info.enabledExtensionCount = 1;
        create_info.ppEnabledExtensionNames = device_extensions;

        VkDevice device = VK_NULL_HANDLE;
        if (!vk_ok(vkCreateDevice(physical_device, &create_info, nullptr, &device), "vkCreateDevice"))
        {
            return nullptr;
        }

        return std::make_unique<VulkanDevice>(m_instance, physical_device, device, graphics_family_index, desc,
                                              sync2_supported, dynamic_rendering_supported);
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
