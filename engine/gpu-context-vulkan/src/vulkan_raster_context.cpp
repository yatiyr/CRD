// vulkan_raster_context.cpp — the Vulkan IRasterContext (ADR-0103 / D-008 C1-a). Offscreen RGBA8 targets + a
// dynamic-rendering CLEAR with pixel readback, on a graphics queue from the VulkanGpuContext. Raw Vulkan, no crd-rhi.
// The shader-object DRAW path (bind VS+FS programs + dynamic state + vkCmdDraw) appends in C1-b.

#include <crd/gpu/vulkan_raster_context.hpp>

#include <vulkan/vulkan.h>

#include <cstdint>

namespace crd::gpu
{
namespace
{

constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;

// VK_EXT_shader_object entry points (extension functions ⇒ loaded via vkGetDeviceProcAddr, not the core loader). The
// "*Enable"/topology/cull/viewport-with-count dynamic-state setters are core in 1.3 and called directly.
struct ShaderObjectApi
{
    PFN_vkCreateShadersEXT              create                    = nullptr;
    PFN_vkDestroyShaderEXT              destroy                   = nullptr;
    PFN_vkCmdBindShadersEXT            bind                      = nullptr;
    PFN_vkCmdSetVertexInputEXT         set_vertex_input          = nullptr;
    PFN_vkCmdSetPolygonModeEXT         set_polygon_mode          = nullptr;
    PFN_vkCmdSetRasterizationSamplesEXT set_rasterization_samples = nullptr;
    PFN_vkCmdSetSampleMaskEXT          set_sample_mask           = nullptr;
    PFN_vkCmdSetAlphaToCoverageEnableEXT set_alpha_to_coverage   = nullptr;
    PFN_vkCmdSetColorBlendEnableEXT    set_color_blend_enable    = nullptr;
    PFN_vkCmdSetColorWriteMaskEXT      set_color_write_mask      = nullptr;

    [[nodiscard]] bool valid() const noexcept
    {
        return create != nullptr && destroy != nullptr && bind != nullptr && set_vertex_input != nullptr
               && set_polygon_mode != nullptr && set_rasterization_samples != nullptr && set_sample_mask != nullptr
               && set_alpha_to_coverage != nullptr && set_color_blend_enable != nullptr
               && set_color_write_mask != nullptr;
    }
};

[[nodiscard]] ShaderObjectApi load_shader_object_api(VkDevice d)
{
    ShaderObjectApi a;
    a.create  = reinterpret_cast<PFN_vkCreateShadersEXT>(vkGetDeviceProcAddr(d, "vkCreateShadersEXT"));
    a.destroy = reinterpret_cast<PFN_vkDestroyShaderEXT>(vkGetDeviceProcAddr(d, "vkDestroyShaderEXT"));
    a.bind    = reinterpret_cast<PFN_vkCmdBindShadersEXT>(vkGetDeviceProcAddr(d, "vkCmdBindShadersEXT"));
    a.set_vertex_input = reinterpret_cast<PFN_vkCmdSetVertexInputEXT>(vkGetDeviceProcAddr(d, "vkCmdSetVertexInputEXT"));
    a.set_polygon_mode = reinterpret_cast<PFN_vkCmdSetPolygonModeEXT>(vkGetDeviceProcAddr(d, "vkCmdSetPolygonModeEXT"));
    a.set_rasterization_samples =
        reinterpret_cast<PFN_vkCmdSetRasterizationSamplesEXT>(vkGetDeviceProcAddr(d, "vkCmdSetRasterizationSamplesEXT"));
    a.set_sample_mask = reinterpret_cast<PFN_vkCmdSetSampleMaskEXT>(vkGetDeviceProcAddr(d, "vkCmdSetSampleMaskEXT"));
    a.set_alpha_to_coverage =
        reinterpret_cast<PFN_vkCmdSetAlphaToCoverageEnableEXT>(vkGetDeviceProcAddr(d, "vkCmdSetAlphaToCoverageEnableEXT"));
    a.set_color_blend_enable =
        reinterpret_cast<PFN_vkCmdSetColorBlendEnableEXT>(vkGetDeviceProcAddr(d, "vkCmdSetColorBlendEnableEXT"));
    a.set_color_write_mask =
        reinterpret_cast<PFN_vkCmdSetColorWriteMaskEXT>(vkGetDeviceProcAddr(d, "vkCmdSetColorWriteMaskEXT"));
    return a;
}

[[nodiscard]] std::uint32_t find_memory_type(VkPhysicalDevice pd, std::uint32_t type_bits, VkMemoryPropertyFlags props)
{
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i)
    {
        if ((type_bits & (1U << i)) != 0U && (mp.memoryTypes[i].propertyFlags & props) == props) { return i; }
    }
    return UINT32_MAX;
}

class VulkanRasterTarget final : public IRasterTarget
{
public:
    VulkanRasterTarget(VkDevice device, VkImage image, VkDeviceMemory image_mem, VkImageView view, VkBuffer readback,
                       VkDeviceMemory readback_mem, void* mapped, crd::u32 w, crd::u32 h) noexcept
        : m_device(device), m_image(image), m_image_mem(image_mem), m_view(view), m_readback(readback),
          m_readback_mem(readback_mem), m_mapped(mapped), m_w(w), m_h(h)
    {
    }
    ~VulkanRasterTarget() override
    {
        if (m_readback_mem != VK_NULL_HANDLE) { vkUnmapMemory(m_device, m_readback_mem); }
        if (m_view != VK_NULL_HANDLE) { vkDestroyImageView(m_device, m_view, nullptr); }
        if (m_image != VK_NULL_HANDLE) { vkDestroyImage(m_device, m_image, nullptr); }
        if (m_readback != VK_NULL_HANDLE) { vkDestroyBuffer(m_device, m_readback, nullptr); }
        if (m_image_mem != VK_NULL_HANDLE) { vkFreeMemory(m_device, m_image_mem, nullptr); }
        if (m_readback_mem != VK_NULL_HANDLE) { vkFreeMemory(m_device, m_readback_mem, nullptr); }
    }
    VulkanRasterTarget(const VulkanRasterTarget&)            = delete;
    VulkanRasterTarget& operator=(const VulkanRasterTarget&) = delete;
    VulkanRasterTarget(VulkanRasterTarget&&)                 = delete;
    VulkanRasterTarget& operator=(VulkanRasterTarget&&)      = delete;

    [[nodiscard]] crd::u32 width() const noexcept override { return m_w; }
    [[nodiscard]] crd::u32 height() const noexcept override { return m_h; }
    [[nodiscard]] crd::u32 read_pixel(crd::u32 x, crd::u32 y) const noexcept override
    {
        if (m_mapped == nullptr || x >= m_w || y >= m_h) { return 0U; }
        const auto*      bytes  = static_cast<const crd::u8*>(m_mapped);
        const crd::usize offset = (static_cast<crd::usize>(y) * m_w + x) * 4U;
        crd::u32         px     = 0U;
        for (int i = 0; i < 4; ++i) { px |= static_cast<crd::u32>(bytes[offset + static_cast<crd::usize>(i)]) << (8 * i); }
        return px; // little-endian RGBA8: R low byte
    }

    [[nodiscard]] VkImage     image() const noexcept { return m_image; }
    [[nodiscard]] VkImageView view() const noexcept { return m_view; }
    [[nodiscard]] VkBuffer    readback() const noexcept { return m_readback; }

private:
    VkDevice       m_device       = VK_NULL_HANDLE;
    VkImage        m_image        = VK_NULL_HANDLE;
    VkDeviceMemory m_image_mem    = VK_NULL_HANDLE;
    VkImageView    m_view         = VK_NULL_HANDLE;
    VkBuffer       m_readback     = VK_NULL_HANDLE;
    VkDeviceMemory m_readback_mem = VK_NULL_HANDLE;
    void*          m_mapped       = nullptr;
    crd::u32       m_w            = 0;
    crd::u32       m_h            = 0;
};

// A linked VS+FS as two `VkShaderEXT` + an (empty, for the trivial shaders) pipeline layout. Built once, drawn many.
class VulkanRasterProgram final : public IRasterProgram
{
public:
    VulkanRasterProgram(VkDevice device, const ShaderObjectApi* api, VkPipelineLayout layout, VkShaderEXT vs,
                        VkShaderEXT fs) noexcept
        : m_device(device), m_api(api), m_layout(layout), m_vs(vs), m_fs(fs)
    {
    }
    ~VulkanRasterProgram() override
    {
        if (m_vs != VK_NULL_HANDLE) { m_api->destroy(m_device, m_vs, nullptr); }
        if (m_fs != VK_NULL_HANDLE) { m_api->destroy(m_device, m_fs, nullptr); }
        if (m_layout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(m_device, m_layout, nullptr); }
    }
    VulkanRasterProgram(const VulkanRasterProgram&)            = delete;
    VulkanRasterProgram& operator=(const VulkanRasterProgram&) = delete;
    VulkanRasterProgram(VulkanRasterProgram&&)                 = delete;
    VulkanRasterProgram& operator=(VulkanRasterProgram&&)      = delete;

    [[nodiscard]] bool        valid() const noexcept override { return m_vs != VK_NULL_HANDLE && m_fs != VK_NULL_HANDLE; }
    [[nodiscard]] VkShaderEXT vs() const noexcept { return m_vs; }
    [[nodiscard]] VkShaderEXT fs() const noexcept { return m_fs; }

private:
    VkDevice               m_device = VK_NULL_HANDLE;
    const ShaderObjectApi* m_api    = nullptr;
    VkPipelineLayout       m_layout = VK_NULL_HANDLE;
    VkShaderEXT            m_vs     = VK_NULL_HANDLE;
    VkShaderEXT            m_fs     = VK_NULL_HANDLE;
};

class VulkanRasterContext final : public IRasterContext
{
public:
    VulkanRasterContext(VulkanGpuContext& ctx, VkCommandPool pool, const ShaderObjectApi& api) noexcept
        : m_ctx(&ctx), m_device(ctx.vk_device()), m_queue(ctx.graphics_queue()), m_pool(pool), m_api(api)
    {
    }
    ~VulkanRasterContext() override
    {
        if (m_pool != VK_NULL_HANDLE) { vkDestroyCommandPool(m_device, m_pool, nullptr); }
    }
    VulkanRasterContext(const VulkanRasterContext&)            = delete;
    VulkanRasterContext& operator=(const VulkanRasterContext&) = delete;
    VulkanRasterContext(VulkanRasterContext&&)                 = delete;
    VulkanRasterContext& operator=(VulkanRasterContext&&)      = delete;

    [[nodiscard]] bool valid() const noexcept override { return m_queue != VK_NULL_HANDLE && m_pool != VK_NULL_HANDLE; }

    [[nodiscard]] std::unique_ptr<IRasterTarget> create_color_target(crd::u32 width, crd::u32 height) override
    {
        if (width == 0U || height == 0U) { return nullptr; }
        const VkPhysicalDevice pd = m_ctx->vk_physical_device();

        // Device-local colour image (attachment + transfer-src for readback).
        VkImageCreateInfo ici{};
        ici.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType     = VK_IMAGE_TYPE_2D;
        ici.format        = kColorFormat;
        ici.extent        = {width, height, 1U};
        ici.mipLevels     = 1U;
        ici.arrayLayers   = 1U;
        ici.samples       = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ici.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImage image = VK_NULL_HANDLE;
        if (vkCreateImage(m_device, &ici, nullptr, &image) != VK_SUCCESS) { return nullptr; }

        VkMemoryRequirements ir{};
        vkGetImageMemoryRequirements(m_device, image, &ir);
        VkMemoryAllocateInfo iai{};
        iai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        iai.allocationSize  = ir.size;
        iai.memoryTypeIndex = find_memory_type(pd, ir.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkDeviceMemory image_mem = VK_NULL_HANDLE;
        if (iai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(m_device, &iai, nullptr, &image_mem) != VK_SUCCESS)
        {
            vkDestroyImage(m_device, image, nullptr);
            return nullptr;
        }
        vkBindImageMemory(m_device, image, image_mem, 0);

        VkImageViewCreateInfo vci{};
        vci.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image                       = image;
        vci.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
        vci.format                      = kColorFormat;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount = 1U;
        vci.subresourceRange.layerCount = 1U;
        VkImageView view = VK_NULL_HANDLE;
        if (vkCreateImageView(m_device, &vci, nullptr, &view) != VK_SUCCESS)
        {
            vkDestroyImage(m_device, image, nullptr);
            vkFreeMemory(m_device, image_mem, nullptr);
            return nullptr;
        }

        // Host-visible readback buffer (image copied here so the CPU can read pixels).
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * 4U;
        VkBufferCreateInfo bci{};
        bci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size        = bytes;
        bci.usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer readback = VK_NULL_HANDLE;
        if (vkCreateBuffer(m_device, &bci, nullptr, &readback) != VK_SUCCESS)
        {
            vkDestroyImageView(m_device, view, nullptr);
            vkDestroyImage(m_device, image, nullptr);
            vkFreeMemory(m_device, image_mem, nullptr);
            return nullptr;
        }
        VkMemoryRequirements br{};
        vkGetBufferMemoryRequirements(m_device, readback, &br);
        VkMemoryAllocateInfo bai{};
        bai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        bai.allocationSize  = br.size;
        bai.memoryTypeIndex = find_memory_type(
            pd, br.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkDeviceMemory readback_mem = VK_NULL_HANDLE;
        void*          mapped       = nullptr;
        if (bai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(m_device, &bai, nullptr, &readback_mem) != VK_SUCCESS
            || vkBindBufferMemory(m_device, readback, readback_mem, 0) != VK_SUCCESS
            || vkMapMemory(m_device, readback_mem, 0, bytes, 0, &mapped) != VK_SUCCESS)
        {
            if (readback_mem != VK_NULL_HANDLE) { vkFreeMemory(m_device, readback_mem, nullptr); }
            vkDestroyBuffer(m_device, readback, nullptr);
            vkDestroyImageView(m_device, view, nullptr);
            vkDestroyImage(m_device, image, nullptr);
            vkFreeMemory(m_device, image_mem, nullptr);
            return nullptr;
        }

        return std::make_unique<VulkanRasterTarget>(m_device, image, image_mem, view, readback, readback_mem, mapped,
                                                    width, height);
    }

    void clear(IRasterTarget& target, ClearColor color) override
    {
        auto& t = static_cast<VulkanRasterTarget&>(target);

        VkCommandBuffer cmd = begin_cmd();
        if (cmd == VK_NULL_HANDLE) { return; }

        transition(cmd, t.image(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

        VkRenderingAttachmentInfo att{};
        att.sType                       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        att.imageView                   = t.view();
        att.imageLayout                 = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.loadOp                      = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp                     = VK_ATTACHMENT_STORE_OP_STORE;
        att.clearValue.color.float32[0] = color.r;
        att.clearValue.color.float32[1] = color.g;
        att.clearValue.color.float32[2] = color.b;
        att.clearValue.color.float32[3] = color.a;

        VkRenderingInfo ri{};
        ri.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea.extent    = {t.width(), t.height()};
        ri.layerCount           = 1U;
        ri.colorAttachmentCount = 1U;
        ri.pColorAttachments    = &att;
        vkCmdBeginRendering(cmd, &ri);
        // C1-b binds shader objects + dynamic state + vkCmdDraw here; C1-a proves the clear path.
        vkCmdEndRendering(cmd);

        transition(cmd, t.image(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1U;
        region.imageExtent                 = {t.width(), t.height(), 1U};
        vkCmdCopyImageToBuffer(cmd, t.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.readback(), 1U, &region);

        end_and_wait(cmd);
    }

    [[nodiscard]] std::unique_ptr<IRasterProgram> create_raster_program(IGpuProgram& vertex, IGpuProgram& fragment) override
    {
        if (!m_api.valid() || vertex.stage() != ShaderStage::Vertex || fragment.stage() != ShaderStage::Fragment)
        {
            return nullptr;
        }
        const auto& vp = static_cast<VulkanGpuProgram&>(vertex).vk_spirv();
        const auto& fp = static_cast<VulkanGpuProgram&>(fragment).vk_spirv();

        VkPipelineLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO; // empty: the trivial shaders bind no resources
        VkPipelineLayout layout = VK_NULL_HANDLE;
        if (vkCreatePipelineLayout(m_device, &lci, nullptr, &layout) != VK_SUCCESS) { return nullptr; }

        VkShaderCreateInfoEXT infos[2]{};
        infos[0].sType                  = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT;
        infos[0].flags                  = VK_SHADER_CREATE_LINK_STAGE_BIT_EXT; // linked VS+FS
        infos[0].stage                  = VK_SHADER_STAGE_VERTEX_BIT;
        infos[0].nextStage              = VK_SHADER_STAGE_FRAGMENT_BIT;
        infos[0].codeType               = VK_SHADER_CODE_TYPE_SPIRV_EXT;
        infos[0].codeSize               = vp.size();
        infos[0].pCode                  = vp.data();
        infos[0].pName                  = "main";
        infos[1].sType                  = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT;
        infos[1].flags                  = VK_SHADER_CREATE_LINK_STAGE_BIT_EXT;
        infos[1].stage                  = VK_SHADER_STAGE_FRAGMENT_BIT;
        infos[1].nextStage              = 0;
        infos[1].codeType               = VK_SHADER_CODE_TYPE_SPIRV_EXT;
        infos[1].codeSize               = fp.size();
        infos[1].pCode                  = fp.data();
        infos[1].pName                  = "main";

        VkShaderEXT shaders[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
        if (m_api.create(m_device, 2U, infos, nullptr, shaders) != VK_SUCCESS)
        {
            vkDestroyPipelineLayout(m_device, layout, nullptr);
            return nullptr;
        }
        return std::make_unique<VulkanRasterProgram>(m_device, &m_api, layout, shaders[0], shaders[1]);
    }

    void draw(IRasterTarget& target, IRasterProgram& program, ClearColor clear_color, crd::u32 vertex_count) override
    {
        auto& t = static_cast<VulkanRasterTarget&>(target);
        auto& p = static_cast<VulkanRasterProgram&>(program);
        if (!m_api.valid() || !p.valid()) { return; }

        VkCommandBuffer cmd = begin_cmd();
        if (cmd == VK_NULL_HANDLE) { return; }

        transition(cmd, t.image(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

        VkRenderingAttachmentInfo att{};
        att.sType                       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        att.imageView                   = t.view();
        att.imageLayout                 = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.loadOp                      = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp                     = VK_ATTACHMENT_STORE_OP_STORE;
        att.clearValue.color.float32[0] = clear_color.r;
        att.clearValue.color.float32[1] = clear_color.g;
        att.clearValue.color.float32[2] = clear_color.b;
        att.clearValue.color.float32[3] = clear_color.a;

        VkRenderingInfo ri{};
        ri.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea.extent    = {t.width(), t.height()};
        ri.layerCount           = 1U;
        ri.colorAttachmentCount = 1U;
        ri.pColorAttachments    = &att;
        vkCmdBeginRendering(cmd, &ri);

        set_draw_state(cmd, t.width(), t.height());
        const VkShaderStageFlagBits stages[2] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
        const VkShaderEXT           objs[2]   = {p.vs(), p.fs()};
        m_api.bind(cmd, 2U, stages, objs);
        vkCmdDraw(cmd, vertex_count, 1U, 0U, 0U);

        vkCmdEndRendering(cmd);

        transition(cmd, t.image(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1U;
        region.imageExtent                 = {t.width(), t.height(), 1U};
        vkCmdCopyImageToBuffer(cmd, t.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.readback(), 1U, &region);

        end_and_wait(cmd);
    }

private:
    // Set ALL graphics dynamic state a shader-object draw requires (no pipeline bakes it). Attributeless, no blend/depth.
    void set_draw_state(VkCommandBuffer cmd, crd::u32 w, crd::u32 h) const
    {
        const VkViewport vp{0.0F, 0.0F, static_cast<float>(w), static_cast<float>(h), 0.0F, 1.0F};
        vkCmdSetViewportWithCount(cmd, 1U, &vp);
        const VkRect2D scissor{{0, 0}, {w, h}};
        vkCmdSetScissorWithCount(cmd, 1U, &scissor);
        vkCmdSetRasterizerDiscardEnable(cmd, VK_FALSE);
        vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
        vkCmdSetFrontFace(cmd, VK_FRONT_FACE_COUNTER_CLOCKWISE);
        vkCmdSetDepthTestEnable(cmd, VK_FALSE);
        vkCmdSetDepthWriteEnable(cmd, VK_FALSE);
        vkCmdSetDepthBiasEnable(cmd, VK_FALSE);
        vkCmdSetStencilTestEnable(cmd, VK_FALSE);
        vkCmdSetPrimitiveTopology(cmd, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
        vkCmdSetPrimitiveRestartEnable(cmd, VK_FALSE);
        m_api.set_polygon_mode(cmd, VK_POLYGON_MODE_FILL);
        m_api.set_rasterization_samples(cmd, VK_SAMPLE_COUNT_1_BIT);
        const VkSampleMask mask = 0xFFFFFFFFU;
        m_api.set_sample_mask(cmd, VK_SAMPLE_COUNT_1_BIT, &mask);
        m_api.set_alpha_to_coverage(cmd, VK_FALSE);
        m_api.set_vertex_input(cmd, 0U, nullptr, 0U, nullptr); // attributeless
        const VkBool32 blend_enable = VK_FALSE;
        m_api.set_color_blend_enable(cmd, 0U, 1U, &blend_enable);
        const VkColorComponentFlags write_mask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        m_api.set_color_write_mask(cmd, 0U, 1U, &write_mask);
    }

    [[nodiscard]] VkCommandBuffer begin_cmd()
    {
        VkCommandBufferAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool        = m_pool;
        ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1U;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(m_device, &ai, &cmd) != VK_SUCCESS) { return VK_NULL_HANDLE; }
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        return cmd;
    }

    void end_and_wait(VkCommandBuffer cmd)
    {
        vkEndCommandBuffer(cmd);
        VkSubmitInfo si{};
        si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1U;
        si.pCommandBuffers    = &cmd;
        vkQueueSubmit(m_queue, 1U, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(m_queue); // synchronous: host-coherent readback is valid after the wait
        vkFreeCommandBuffers(m_device, m_pool, 1U, &cmd);
    }

    static void transition(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                           VkAccessFlags src_access, VkAccessFlags dst_access, VkPipelineStageFlags src_stage,
                           VkPipelineStageFlags dst_stage)
    {
        VkImageMemoryBarrier b{};
        b.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout                   = from;
        b.newLayout                   = to;
        b.srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        b.image                       = image;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.levelCount = 1U;
        b.subresourceRange.layerCount = 1U;
        b.srcAccessMask               = src_access;
        b.dstAccessMask               = dst_access;
        vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1U, &b);
    }

    VulkanGpuContext* m_ctx    = nullptr;
    VkDevice          m_device = VK_NULL_HANDLE;
    VkQueue           m_queue  = VK_NULL_HANDLE;
    VkCommandPool     m_pool   = VK_NULL_HANDLE;
    ShaderObjectApi   m_api{};
};

} // namespace

std::unique_ptr<IRasterContext> create_vulkan_raster_context(VulkanGpuContext& ctx, crd::memory::IAllocator* /*alloc*/)
{
    if (!ctx.graphics_capable() || ctx.graphics_queue() == VK_NULL_HANDLE) { return nullptr; }

    VkCommandPoolCreateInfo pci{};
    pci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = ctx.graphics_family();
    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(ctx.vk_device(), &pci, nullptr, &pool) != VK_SUCCESS) { return nullptr; }

    // Load VK_EXT_shader_object entry points (present iff ctx.shader_object()); clear/readback works either way, DRAW
    // needs them. An unloaded api ⇒ create_raster_program returns nullptr (a caller can still use clear()).
    const ShaderObjectApi api = ctx.shader_object() ? load_shader_object_api(ctx.vk_device()) : ShaderObjectApi{};
    return std::make_unique<VulkanRasterContext>(ctx, pool, api);
}

} // namespace crd::gpu
