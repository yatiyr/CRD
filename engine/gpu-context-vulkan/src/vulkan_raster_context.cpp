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
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT; // B1-d: depth buffer format (universally supported for attachment)

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
    PFN_vkCmdSetColorBlendEquationEXT  set_color_blend_equation  = nullptr; // B17-a: per-attachment blend eq (WBOIT accum/reveal)
    PFN_vkCmdDrawMeshTasksEXT          draw_mesh_tasks           = nullptr; // B4: VK_EXT_mesh_shader (optional — null if absent)
    PFN_vkCmdDrawMeshTasksIndirectEXT  draw_mesh_tasks_indirect  = nullptr; // B4: GPU-driven indirect meshlet dispatch
    PFN_vkCmdSetPatchControlPointsEXT  set_patch_control_points  = nullptr; // B4-tess: EDS2 patch size (optional — null if absent)

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
    a.set_color_blend_equation = // B17-a: WBOIT needs per-attachment blend equations (additive accum + multiplicative reveal)
        reinterpret_cast<PFN_vkCmdSetColorBlendEquationEXT>(vkGetDeviceProcAddr(d, "vkCmdSetColorBlendEquationEXT"));
    a.draw_mesh_tasks = reinterpret_cast<PFN_vkCmdDrawMeshTasksEXT>(vkGetDeviceProcAddr(d, "vkCmdDrawMeshTasksEXT")); // B4
    a.draw_mesh_tasks_indirect =
        reinterpret_cast<PFN_vkCmdDrawMeshTasksIndirectEXT>(vkGetDeviceProcAddr(d, "vkCmdDrawMeshTasksIndirectEXT")); // B4
    a.set_patch_control_points =
        reinterpret_cast<PFN_vkCmdSetPatchControlPointsEXT>(vkGetDeviceProcAddr(d, "vkCmdSetPatchControlPointsEXT")); // B4-tess
    return a;
}

inline constexpr crd::u32 kBindlessMax = 8U; // B2-d: bindless texture-array capacity (binding 3 descriptorCount)

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

// An image + its memory + a colour view. For a single-sample target only `color` is used; an MSAA target also carries a
// single-sample `resolve` bundle that read_pixel/readback reads from (the MSAA image itself can't be copied to a buffer).
struct ImageBundle
{
    VkImage        image = VK_NULL_HANDLE;
    VkDeviceMemory mem   = VK_NULL_HANDLE;
    VkImageView    view  = VK_NULL_HANDLE;
};

inline void destroy_image_bundle(VkDevice d, const ImageBundle& b) noexcept
{
    if (b.view != VK_NULL_HANDLE) { vkDestroyImageView(d, b.view, nullptr); }
    if (b.image != VK_NULL_HANDLE) { vkDestroyImage(d, b.image, nullptr); }
    if (b.mem != VK_NULL_HANDLE) { vkFreeMemory(d, b.mem, nullptr); }
}

// B1-d: the backend-neutral DepthCompare → VkCompareOp (the enum orders match, but map explicitly, not by cast).
[[nodiscard]] inline VkCompareOp to_vk_compare(DepthCompare c) noexcept
{
    switch (c)
    {
    case DepthCompare::Never:        return VK_COMPARE_OP_NEVER;
    case DepthCompare::Less:         return VK_COMPARE_OP_LESS;
    case DepthCompare::Equal:        return VK_COMPARE_OP_EQUAL;
    case DepthCompare::LessEqual:    return VK_COMPARE_OP_LESS_OR_EQUAL;
    case DepthCompare::Greater:      return VK_COMPARE_OP_GREATER;
    case DepthCompare::NotEqual:     return VK_COMPARE_OP_NOT_EQUAL;
    case DepthCompare::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case DepthCompare::Always:       return VK_COMPARE_OP_ALWAYS;
    }
    return VK_COMPARE_OP_ALWAYS;
}

// B1-e: ShadingRate → the VkExtent2D fragment size {width, height} for vkCmdSetFragmentShadingRateKHR.
[[nodiscard]] inline VkExtent2D vrs_extent(ShadingRate r) noexcept
{
    switch (r)
    {
    case ShadingRate::Rate1x2: return {1U, 2U};
    case ShadingRate::Rate2x1: return {2U, 1U};
    case ShadingRate::Rate2x2: return {2U, 2U};
    case ShadingRate::Rate2x4: return {2U, 4U};
    case ShadingRate::Rate4x2: return {4U, 2U};
    case ShadingRate::Rate4x4: return {4U, 4U};
    default:                   return {1U, 1U};
    }
}

// B1-e: ShadingRate → the packed attachment/primitive byte (Yshift<<2)|Xshift, shift 0=1×,1=2×,2=4× (2×2 = 5). Identical
// encoding for gl_PrimitiveShadingRateEXT, the VRS attachment image, and D3D12 SV_ShadingRate.
[[nodiscard]] inline crd::u8 vrs_packed(ShadingRate r) noexcept
{
    const VkExtent2D e     = vrs_extent(r);
    const auto       shift = [](crd::u32 v) -> crd::u32 {
        if (v >= 4U) { return 2U; }
        if (v >= 2U) { return 1U; }
        return 0U;
    };
    return static_cast<crd::u8>((shift(e.height) << 2U) | shift(e.width));
}

// B1-e: ShadingRateCombiner → VkFragmentShadingRateCombinerOpKHR (map explicitly).
[[nodiscard]] inline VkFragmentShadingRateCombinerOpKHR vrs_combiner(ShadingRateCombiner c) noexcept
{
    switch (c)
    {
    case ShadingRateCombiner::Replace: return VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR;
    case ShadingRateCombiner::Min:     return VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MIN_KHR;
    case ShadingRateCombiner::Max:     return VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MAX_KHR;
    case ShadingRateCombiner::Mul:     return VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MUL_KHR;
    default:                           return VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
    }
}

// B1-f: ConservativeMode → VkConservativeRasterizationModeEXT.
[[nodiscard]] inline VkConservativeRasterizationModeEXT to_conservative_mode(ConservativeMode m) noexcept
{
    switch (m)
    {
    case ConservativeMode::Overestimate:  return VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT;
    case ConservativeMode::Underestimate: return VK_CONSERVATIVE_RASTERIZATION_MODE_UNDERESTIMATE_EXT;
    default:                              return VK_CONSERVATIVE_RASTERIZATION_MODE_DISABLED_EXT;
    }
}

class VulkanRasterTarget final : public IRasterTarget
{
public:
    VulkanRasterTarget(VkDevice device, const ImageBundle& color, const ImageBundle& resolve, const ImageBundle& depth,
                       VkBuffer readback, VkDeviceMemory readback_mem, void* mapped, crd::u32 samples, crd::u32 w,
                       crd::u32 h) noexcept
        : m_device(device), m_color(color), m_resolve(resolve), m_depth(depth), m_readback(readback),
          m_readback_mem(readback_mem), m_mapped(mapped), m_samples(samples), m_w(w), m_h(h)
    {
    }
    ~VulkanRasterTarget() override
    {
        if (m_readback_mem != VK_NULL_HANDLE) { vkUnmapMemory(m_device, m_readback_mem); }
        destroy_image_bundle(m_device, m_color);
        destroy_image_bundle(m_device, m_resolve);
        destroy_image_bundle(m_device, m_depth);
        destroy_image_bundle(m_device, m_vrs);
        if (m_readback != VK_NULL_HANDLE) { vkDestroyBuffer(m_device, m_readback, nullptr); }
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

    [[nodiscard]] VkImage     image() const noexcept { return m_color.image; }  // the colour attachment (MSAA if samples>1)
    [[nodiscard]] VkImageView view() const noexcept { return m_color.view; }
    [[nodiscard]] VkBuffer    readback() const noexcept { return m_readback; }
    [[nodiscard]] crd::u32    samples() const noexcept { return m_samples; }
    [[nodiscard]] bool        multisampled() const noexcept { return m_samples > 1U; }
    [[nodiscard]] VkImage     resolve_image() const noexcept { return m_resolve.image; } // single-sample resolve (MSAA only)
    [[nodiscard]] VkImageView resolve_view() const noexcept { return m_resolve.view; }
    // The image the readback buffer is copied from: the resolve image for MSAA, else the (single-sample) colour image.
    [[nodiscard]] VkImage src_image() const noexcept { return m_samples > 1U ? m_resolve.image : m_color.image; }
    [[nodiscard]] bool        has_depth() const noexcept { return m_depth.image != VK_NULL_HANDLE; } // B1-d
    [[nodiscard]] VkImage     depth_image() const noexcept { return m_depth.image; }
    [[nodiscard]] VkImageView depth_view() const noexcept { return m_depth.view; }
    [[nodiscard]] bool        has_vrs() const noexcept { return m_vrs.image != VK_NULL_HANDLE; } // B1-e attachment VRS
    [[nodiscard]] VkImage     vrs_image() const noexcept { return m_vrs.image; }
    [[nodiscard]] VkImageView vrs_view() const noexcept { return m_vrs.view; }
    void                      set_vrs(const ImageBundle& b) noexcept { m_vrs = b; } // owned after this; freed in the dtor

private:
    VkDevice       m_device       = VK_NULL_HANDLE;
    ImageBundle    m_color{};
    ImageBundle    m_resolve{}; // empty for a single-sample target
    ImageBundle    m_depth{};   // B1-d: empty unless created via create_color_depth_target
    ImageBundle    m_vrs{};     // B1-e: empty unless created via create_color_vrs_target (R8_UINT per-tile rate image)
    VkBuffer       m_readback     = VK_NULL_HANDLE;
    VkDeviceMemory m_readback_mem = VK_NULL_HANDLE;
    void*          m_mapped       = nullptr;
    crd::u32       m_samples      = 1U;
    crd::u32       m_w            = 0;
    crd::u32       m_h            = 0;
};

// A linked VS+FS as two `VkShaderEXT` + an (empty, for the trivial shaders) pipeline layout. Built once, drawn many.
class VulkanRasterProgram final : public IRasterProgram
{
public:
    VulkanRasterProgram(VkDevice device, const ShaderObjectApi* api, VkPipelineLayout layout, VkShaderEXT vs,
                        VkShaderEXT fs, bool is_mesh = false, VkShaderEXT task = VK_NULL_HANDLE,
                        VkShaderEXT tcs = VK_NULL_HANDLE, VkShaderEXT tes = VK_NULL_HANDLE) noexcept // B4-tess: hull/domain
        : m_device(device), m_api(api), m_layout(layout), m_vs(vs), m_fs(fs), m_is_mesh(is_mesh), m_task(task), m_tcs(tcs),
          m_tes(tes)
    {
    }
    ~VulkanRasterProgram() override
    {
        if (m_task != VK_NULL_HANDLE) { m_api->destroy(m_device, m_task, nullptr); } // B4: the amplification (task) shader
        if (m_tcs != VK_NULL_HANDLE) { m_api->destroy(m_device, m_tcs, nullptr); }   // B4-tess: hull
        if (m_tes != VK_NULL_HANDLE) { m_api->destroy(m_device, m_tes, nullptr); }   // B4-tess: domain
        if (m_vs != VK_NULL_HANDLE) { m_api->destroy(m_device, m_vs, nullptr); }
        if (m_fs != VK_NULL_HANDLE) { m_api->destroy(m_device, m_fs, nullptr); }
        if (m_layout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(m_device, m_layout, nullptr); }
    }
    VulkanRasterProgram(const VulkanRasterProgram&)            = delete;
    VulkanRasterProgram& operator=(const VulkanRasterProgram&) = delete;
    VulkanRasterProgram(VulkanRasterProgram&&)                 = delete;
    VulkanRasterProgram& operator=(VulkanRasterProgram&&)      = delete;

    [[nodiscard]] bool             valid() const noexcept override { return m_vs != VK_NULL_HANDLE && m_fs != VK_NULL_HANDLE; }
    [[nodiscard]] VkShaderEXT      vs() const noexcept { return m_vs; } // B4: the MESH shader object when is_mesh()
    [[nodiscard]] VkShaderEXT      fs() const noexcept { return m_fs; }
    [[nodiscard]] bool             is_mesh() const noexcept { return m_is_mesh; } // B4: bind with VK_SHADER_STAGE_MESH_BIT_EXT
    [[nodiscard]] VkShaderEXT      task() const noexcept { return m_task; }       // B4: the amplification (task) shader object
    [[nodiscard]] bool             has_task() const noexcept { return m_task != VK_NULL_HANDLE; } // B4: task→mesh amplification
    [[nodiscard]] VkShaderEXT      tcs() const noexcept { return m_tcs; }         // B4-tess: hull (tess control) shader object
    [[nodiscard]] VkShaderEXT      tes() const noexcept { return m_tes; }         // B4-tess: domain (tess eval) shader object
    [[nodiscard]] bool             is_tess() const noexcept { return m_tcs != VK_NULL_HANDLE; } // B4-tess: a tessellation program
    [[nodiscard]] VkPipelineLayout layout() const noexcept { return m_layout; } // B1-f: for binding the storage set

private:
    VkDevice               m_device  = VK_NULL_HANDLE;
    const ShaderObjectApi* m_api     = nullptr;
    VkPipelineLayout       m_layout  = VK_NULL_HANDLE;
    VkShaderEXT            m_vs      = VK_NULL_HANDLE;
    VkShaderEXT            m_fs      = VK_NULL_HANDLE;
    bool                   m_is_mesh = false;
    VkShaderEXT            m_task    = VK_NULL_HANDLE; // B4: amplification shader (task→mesh); NULL for a plain mesh program
    VkShaderEXT            m_tcs     = VK_NULL_HANDLE; // B4-tess: tess-control (hull); NULL unless a tessellation program
    VkShaderEXT            m_tes     = VK_NULL_HANDLE; // B4-tess: tess-eval (domain)
};

// B1-f: a fragment-shader STORAGE buffer — a device-local SSBO the FS reads/writes, plus a host-visible readback the CPU
// reads after a draw_storage. Zero-initialised on creation.
class VulkanStorageBuffer final : public IStorageBuffer
{
public:
    VulkanStorageBuffer(VkDevice device, VkBuffer buf, VkDeviceMemory buf_mem, VkBuffer readback,
                        VkDeviceMemory readback_mem, void* mapped, crd::u32 size_bytes) noexcept
        : m_device(device), m_buf(buf), m_buf_mem(buf_mem), m_readback(readback), m_readback_mem(readback_mem),
          m_mapped(mapped), m_size(size_bytes)
    {
    }
    ~VulkanStorageBuffer() override
    {
        if (m_readback_mem != VK_NULL_HANDLE) { vkUnmapMemory(m_device, m_readback_mem); }
        if (m_buf != VK_NULL_HANDLE) { vkDestroyBuffer(m_device, m_buf, nullptr); }
        if (m_readback != VK_NULL_HANDLE) { vkDestroyBuffer(m_device, m_readback, nullptr); }
        if (m_buf_mem != VK_NULL_HANDLE) { vkFreeMemory(m_device, m_buf_mem, nullptr); }
        if (m_readback_mem != VK_NULL_HANDLE) { vkFreeMemory(m_device, m_readback_mem, nullptr); }
    }
    VulkanStorageBuffer(const VulkanStorageBuffer&)            = delete;
    VulkanStorageBuffer& operator=(const VulkanStorageBuffer&) = delete;
    VulkanStorageBuffer(VulkanStorageBuffer&&)                 = delete;
    VulkanStorageBuffer& operator=(VulkanStorageBuffer&&)      = delete;

    [[nodiscard]] crd::u32 size_bytes() const noexcept override { return m_size; }
    [[nodiscard]] crd::u32 read_u32(crd::u32 index) const noexcept override
    {
        if (m_mapped == nullptr || (index + 1U) * 4U > m_size) { return 0U; }
        crd::u32 v = 0U;
        const auto* bytes = static_cast<const crd::u8*>(m_mapped) + static_cast<crd::usize>(index) * 4U;
        for (int i = 0; i < 4; ++i) { v |= static_cast<crd::u32>(bytes[i]) << (8 * i); }
        return v;
    }
    [[nodiscard]] VkBuffer buf() const noexcept { return m_buf; }
    [[nodiscard]] VkBuffer readback() const noexcept { return m_readback; }

private:
    VkDevice       m_device       = VK_NULL_HANDLE;
    VkBuffer       m_buf          = VK_NULL_HANDLE;
    VkDeviceMemory m_buf_mem      = VK_NULL_HANDLE;
    VkBuffer       m_readback     = VK_NULL_HANDLE;
    VkDeviceMemory m_readback_mem = VK_NULL_HANDLE;
    void*          m_mapped       = nullptr;
    crd::u32       m_size         = 0;
};

// B2: a sampled texture — a device-local image+view (owned) sampled through the context's default sampler in draw_textured.
class VulkanTexture final : public ITexture
{
public:
    VulkanTexture(VkDevice device, const ImageBundle& img, crd::u32 w, crd::u32 h) noexcept
        : m_device(device), m_img(img), m_w(w), m_h(h)
    {
    }
    ~VulkanTexture() override { destroy_image_bundle(m_device, m_img); }
    VulkanTexture(const VulkanTexture&)            = delete;
    VulkanTexture& operator=(const VulkanTexture&) = delete;
    VulkanTexture(VulkanTexture&&)                 = delete;
    VulkanTexture& operator=(VulkanTexture&&)      = delete;

    [[nodiscard]] crd::u32    width() const noexcept override { return m_w; }
    [[nodiscard]] crd::u32    height() const noexcept override { return m_h; }
    [[nodiscard]] VkImageView view() const noexcept { return m_img.view; }

private:
    VkDevice    m_device = VK_NULL_HANDLE;
    ImageBundle m_img{};
    crd::u32    m_w = 0;
    crd::u32    m_h = 0;
};

inline constexpr crd::u32 kMaxGBuffer = 8U; // B5: max deferred G-buffer colour attachments

// B5: a deferred G-BUFFER — `n` RGBA8 colour attachments, each with its own host-visible readback buffer. `read_pixel`
// takes an attachment index. The material writes all `n` in one MRT draw (draw_gbuffer).
class VulkanGBufferTarget final : public IGBufferTarget
{
public:
    VulkanGBufferTarget(VkDevice device, crd::u32 n, const ImageBundle* imgs, const VkBuffer* bufs,
                        const VkDeviceMemory* mems, void* const* mapped, crd::u32 w, crd::u32 h) noexcept
        : m_device(device), m_n(n), m_w(w), m_h(h)
    {
        for (crd::u32 i = 0; i < n; ++i) { m_img[i] = imgs[i]; m_buf[i] = bufs[i]; m_mem[i] = mems[i]; m_mapped[i] = mapped[i]; }
    }
    ~VulkanGBufferTarget() override
    {
        for (crd::u32 i = 0; i < m_n; ++i)
        {
            if (m_mapped[i] != nullptr) { vkUnmapMemory(m_device, m_mem[i]); }
            if (m_buf[i] != VK_NULL_HANDLE) { vkDestroyBuffer(m_device, m_buf[i], nullptr); }
            if (m_mem[i] != VK_NULL_HANDLE) { vkFreeMemory(m_device, m_mem[i], nullptr); }
            destroy_image_bundle(m_device, m_img[i]);
        }
    }
    VulkanGBufferTarget(const VulkanGBufferTarget&)            = delete;
    VulkanGBufferTarget& operator=(const VulkanGBufferTarget&) = delete;
    VulkanGBufferTarget(VulkanGBufferTarget&&)                 = delete;
    VulkanGBufferTarget& operator=(VulkanGBufferTarget&&)      = delete;

    [[nodiscard]] crd::u32 width() const noexcept override { return m_w; }
    [[nodiscard]] crd::u32 height() const noexcept override { return m_h; }
    [[nodiscard]] crd::u32 attachment_count() const noexcept override { return m_n; }
    [[nodiscard]] crd::u32 read_pixel(crd::u32 attachment, crd::u32 x, crd::u32 y) const noexcept override
    {
        if (attachment >= m_n || m_mapped[attachment] == nullptr || x >= m_w || y >= m_h) { return 0U; }
        const auto*      bytes  = static_cast<const crd::u8*>(m_mapped[attachment]);
        const crd::usize offset = (static_cast<crd::usize>(y) * m_w + x) * 4U;
        crd::u32         px     = 0U;
        for (int i = 0; i < 4; ++i) { px |= static_cast<crd::u32>(bytes[offset + static_cast<crd::usize>(i)]) << (8 * i); }
        return px;
    }
    [[nodiscard]] VkImageView view(crd::u32 i) const noexcept { return m_img[i].view; }
    [[nodiscard]] VkImage     image(crd::u32 i) const noexcept { return m_img[i].image; }
    [[nodiscard]] VkBuffer    readback(crd::u32 i) const noexcept { return m_buf[i]; }

private:
    VkDevice       m_device = VK_NULL_HANDLE;
    crd::u32       m_n      = 0;
    ImageBundle    m_img[kMaxGBuffer]{};
    VkBuffer       m_buf[kMaxGBuffer]{};
    VkDeviceMemory m_mem[kMaxGBuffer]{};
    void*          m_mapped[kMaxGBuffer]{};
    crd::u32       m_w = 0;
    crd::u32       m_h = 0;
};

class VulkanRasterContext final : public IRasterContext
{
public:
    VulkanRasterContext(VulkanGpuContext& ctx, VkCommandPool pool, const ShaderObjectApi& api) noexcept
        : m_ctx(&ctx), m_device(ctx.vk_device()), m_queue(ctx.graphics_queue()), m_pool(pool), m_api(api)
    {
        if (ctx.fragment_shading_rate()) // B1-e: the VRS dynamic-state setter (extension fn ⇒ vkGetDeviceProcAddr)
        {
            m_set_vrs = reinterpret_cast<PFN_vkCmdSetFragmentShadingRateKHR>(
                vkGetDeviceProcAddr(m_device, "vkCmdSetFragmentShadingRateKHR"));
        }
        if (ctx.conservative_raster()) // B1-f: the conservative-mode dynamic-state setters (EDS3 extension fns)
        {
            m_set_conservative = reinterpret_cast<PFN_vkCmdSetConservativeRasterizationModeEXT>(
                vkGetDeviceProcAddr(m_device, "vkCmdSetConservativeRasterizationModeEXT"));
            m_set_overest_size = reinterpret_cast<PFN_vkCmdSetExtraPrimitiveOverestimationSizeEXT>(
                vkGetDeviceProcAddr(m_device, "vkCmdSetExtraPrimitiveOverestimationSizeEXT"));
        }

        // The material set-0 layout shared by EVERY raster program (so an FS may bind any of these; unused ones stay
        // unaccessed): binding 0 = a fragment-stage storage buffer (B1-f) · binding 1 = a sampled image (B2) · binding 2 =
        // a sampler (B2). Separable image+sampler ⇒ the GLSL `sampler2D(tex,samp)` combiner. Plus a small pool.
        // binding 3 (B2-d) = a BINDLESS array of up to kBindlessMax sampled images (draw_bindless writes all of them).
        // The texture/sampler bindings are VERTEX+FRAGMENT visible so a VS can sample the FFT DISPLACEMENT to move geometry
        // (the displaced-ocean path) — not just the FS. The storage buffer stays fragment-only (its only user is the FS ROV).
        // B4: include the MESH stage when the device has mesh shaders, so a mesh shader can sample the bindless cascade
        // textures (the ocean mesh path displaces from the FFT via SampleIndexedLod). VERTEX+FRAGMENT otherwise (unchanged).
        const VkShaderStageFlags vs_fs = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
                                         | (m_ctx->mesh_shader() ? static_cast<VkShaderStageFlags>(VK_SHADER_STAGE_MESH_BIT_EXT) : 0U);
        VkDescriptorSetLayoutBinding slb[4]{};
        slb[0].binding = 0U; slb[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; slb[0].descriptorCount = 1U;            slb[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        slb[1].binding = 1U; slb[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;  slb[1].descriptorCount = 1U;            slb[1].stageFlags = vs_fs;
        slb[2].binding = 2U; slb[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;        slb[2].descriptorCount = 1U;            slb[2].stageFlags = vs_fs;
        slb[3].binding = 3U; slb[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;  slb[3].descriptorCount = kBindlessMax; slb[3].stageFlags = vs_fs;
        VkDescriptorSetLayoutCreateInfo slci{};
        slci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        slci.bindingCount = 4U;
        slci.pBindings    = slb;
        vkCreateDescriptorSetLayout(m_device, &slci, nullptr, &m_storage_set_layout);
        VkDescriptorPoolSize        dps[3]{{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4U}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4U * (kBindlessMax + 1U)}, {VK_DESCRIPTOR_TYPE_SAMPLER, 4U}};
        VkDescriptorPoolCreateInfo  dpci{};
        dpci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpci.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        dpci.maxSets       = 4U;
        dpci.poolSizeCount = 3U;
        dpci.pPoolSizes    = dps;
        vkCreateDescriptorPool(m_device, &dpci, nullptr, &m_desc_pool);

        // B2: one default bilinear/repeat sampler bound for every draw_textured (per-sampler params arrive with B2-b).
        VkSamplerCreateInfo sci{};
        sci.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter    = VK_FILTER_LINEAR;
        sci.minFilter    = VK_FILTER_LINEAR;
        sci.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.maxLod       = VK_LOD_CLAMP_NONE;
        vkCreateSampler(m_device, &sci, nullptr, &m_default_sampler);
        // B2-b: a COMPARISON sampler for shadow sampling (sampler2DShadow / SampleCmp): ref <= stored ⇒ 1.
        sci.compareEnable = VK_TRUE;
        sci.compareOp     = VK_COMPARE_OP_LESS_OR_EQUAL;
        vkCreateSampler(m_device, &sci, nullptr, &m_cmp_sampler);
    }
    ~VulkanRasterContext() override
    {
        if (m_default_sampler != VK_NULL_HANDLE) { vkDestroySampler(m_device, m_default_sampler, nullptr); }
        if (m_cmp_sampler != VK_NULL_HANDLE) { vkDestroySampler(m_device, m_cmp_sampler, nullptr); }
        if (m_desc_pool != VK_NULL_HANDLE) { vkDestroyDescriptorPool(m_device, m_desc_pool, nullptr); }
        if (m_storage_set_layout != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(m_device, m_storage_set_layout, nullptr); }
        if (m_pool != VK_NULL_HANDLE) { vkDestroyCommandPool(m_device, m_pool, nullptr); }
    }
    VulkanRasterContext(const VulkanRasterContext&)            = delete;
    VulkanRasterContext& operator=(const VulkanRasterContext&) = delete;
    VulkanRasterContext(VulkanRasterContext&&)                 = delete;
    VulkanRasterContext& operator=(VulkanRasterContext&&)      = delete;

    [[nodiscard]] bool valid() const noexcept override { return m_queue != VK_NULL_HANDLE && m_pool != VK_NULL_HANDLE; }
    [[nodiscard]] bool supports_vrs() const noexcept override { return m_set_vrs != nullptr; } // B1-e
    [[nodiscard]] bool supports_conservative_raster() const noexcept override { return m_set_conservative != nullptr; }
    // B1-f: on Vulkan the inner-coverage builtin (FullyCoveredEXT) + underestimate mode both ride VK_EXT_conservative_
    // rasterization, so inner coverage is usable exactly when conservative raster is.
    [[nodiscard]] bool supports_inner_coverage() const noexcept override { return m_set_conservative != nullptr; }
    [[nodiscard]] bool supports_fragment_interlock() const noexcept override { return m_ctx->fragment_shader_interlock(); }

    [[nodiscard]] std::unique_ptr<IRasterTarget> create_color_target(crd::u32 width, crd::u32 height) override
    {
        return make_target(width, height, 1U, false);
    }

    [[nodiscard]] std::unique_ptr<IRasterTarget> create_color_target_ms(crd::u32 width, crd::u32 height,
                                                                        crd::u32 samples) override
    {
        return make_target(width, height, samples, false);
    }

    [[nodiscard]] std::unique_ptr<IRasterTarget> create_color_depth_target(crd::u32 width, crd::u32 height) override
    {
        return make_target(width, height, 1U, true); // B1-d: single-sample colour + D32 depth
    }

    // B4-vis-4: a R32_UINT VISIBILITY-BUFFER target — the HW-raster path's fragment shader writes a per-pixel primitive id
    // (SV_PrimitiveId), the hybrid Nanite split for big triangles HW raster beats the compute software rasterizer on. Colour
    // -only; `read_pixel` returns the raw u32 id. Draw with draw_visbuffer.
    [[nodiscard]] std::unique_ptr<IRasterTarget> create_visbuffer_target(crd::u32 width, crd::u32 height) override
    {
        return make_target(width, height, 1U, false, VK_FORMAT_R32_UINT);
    }

    [[nodiscard]] std::unique_ptr<IRasterTarget> create_color_vrs_target(crd::u32 width, crd::u32 height,
                                                                         ShadingRate tile_rate) override
    {
        auto target = make_target(width, height, 1U, false); // a plain single-sample colour target first
        const VkExtent2D tile = m_ctx->vrs_tile_size();
        if (target == nullptr || m_set_vrs == nullptr || tile.width == 0U || tile.height == 0U)
        {
            return target; // VRS unsupported ⇒ a plain colour target (draw_vrs shades it at 1x1)
        }

        // A per-tile R8_UINT shading-rate image (each texel covers `tile` framebuffer pixels), filled with the packed rate.
        const crd::u32 vw = (width + tile.width - 1U) / tile.width;
        const crd::u32 vh = (height + tile.height - 1U) / tile.height;
        ImageBundle    vrs{};
        if (!create_image_bundle(vw, vh, VK_SAMPLE_COUNT_1_BIT, VK_FORMAT_R8_UINT, VK_IMAGE_ASPECT_COLOR_BIT,
                                 VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                 vrs))
        {
            return target; // couldn't make the rate image ⇒ fall back to plain colour
        }

        VkCommandBuffer cmd = begin_cmd();
        if (cmd == VK_NULL_HANDLE) { destroy_image_bundle(m_device, vrs); return target; }
        transition(cmd, vrs.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearColorValue cv{};
        cv.uint32[0] = vrs_packed(tile_rate); // every tile ⇒ the requested rate
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0U, 1U, 0U, 1U};
        vkCmdClearColorImage(cmd, vrs.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1U, &range);
        transition(cmd, vrs.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR, VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_ACCESS_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR);
        end_and_wait(cmd);

        static_cast<VulkanRasterTarget&>(*target).set_vrs(vrs); // the target now owns + frees the rate image
        return target;
    }

    [[nodiscard]] std::unique_ptr<IStorageBuffer> create_storage_buffer(crd::u32 size_bytes) override
    {
        if (size_bytes == 0U) { return nullptr; }
        VkBuffer       buf     = VK_NULL_HANDLE;
        VkDeviceMemory buf_mem = VK_NULL_HANDLE;
        if (!make_buffer(size_bytes,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                             | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, buf, buf_mem, nullptr))
        {
            return nullptr;
        }
        VkBuffer       rb     = VK_NULL_HANDLE;
        VkDeviceMemory rb_mem = VK_NULL_HANDLE;
        void*          mapped = nullptr;
        if (!make_buffer(size_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, rb, rb_mem, &mapped))
        {
            vkDestroyBuffer(m_device, buf, nullptr);
            vkFreeMemory(m_device, buf_mem, nullptr);
            return nullptr;
        }
        VkCommandBuffer cmd = begin_cmd(); // zero-initialise the SSBO
        if (cmd != VK_NULL_HANDLE)
        {
            vkCmdFillBuffer(cmd, buf, 0, size_bytes, 0U);
            end_and_wait(cmd);
        }
        return std::make_unique<VulkanStorageBuffer>(m_device, buf, buf_mem, rb, rb_mem, mapped, size_bytes);
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

        // B1-f: the layout carries the storage-buffer set (binding 0) so an FS *may* bind a storage buffer via draw_storage.
        // A program whose FS doesn't use it leaves the descriptor unaccessed (no set bound) — harmless.
        VkPipelineLayoutCreateInfo lci{};
        lci.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        lci.setLayoutCount = 1U;
        lci.pSetLayouts    = &m_storage_set_layout;
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
        infos[0].setLayoutCount         = 1U;
        infos[0].pSetLayouts            = &m_storage_set_layout;
        infos[1].sType                  = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT;
        infos[1].flags                  = VK_SHADER_CREATE_LINK_STAGE_BIT_EXT;
        infos[1].stage                  = VK_SHADER_STAGE_FRAGMENT_BIT;
        infos[1].nextStage              = 0;
        infos[1].codeType               = VK_SHADER_CODE_TYPE_SPIRV_EXT;
        infos[1].codeSize               = fp.size();
        infos[1].pCode                  = fp.data();
        infos[1].pName                  = "main";
        infos[1].setLayoutCount         = 1U;
        infos[1].pSetLayouts            = &m_storage_set_layout;

        VkShaderEXT shaders[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
        if (m_api.create(m_device, 2U, infos, nullptr, shaders) != VK_SUCCESS)
        {
            vkDestroyPipelineLayout(m_device, layout, nullptr);
            return nullptr;
        }
        return std::make_unique<VulkanRasterProgram>(m_device, &m_api, layout, shaders[0], shaders[1]);
    }

    // B4: a MESH+FRAGMENT program (the modern amplification path). `mesh` generates geometry directly (no vertex input); the
    // fragment stage is unchanged. Returns nullptr if the device has no mesh shaders (the caller falls back to vertex-pull).
    [[nodiscard]] std::unique_ptr<IRasterProgram> create_mesh_program(IGpuProgram& mesh, IGpuProgram& fragment) override
    {
        if (!m_api.valid() || m_api.draw_mesh_tasks == nullptr || mesh.stage() != ShaderStage::Mesh
            || fragment.stage() != ShaderStage::Fragment)
        {
            return nullptr;
        }
        const auto& mp = static_cast<VulkanGpuProgram&>(mesh).vk_spirv();
        const auto& fp = static_cast<VulkanGpuProgram&>(fragment).vk_spirv();

        VkPipelineLayoutCreateInfo lci{};
        lci.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        lci.setLayoutCount = 1U;
        lci.pSetLayouts    = &m_storage_set_layout;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        if (vkCreatePipelineLayout(m_device, &lci, nullptr, &layout) != VK_SUCCESS) { return nullptr; }

        VkShaderCreateInfoEXT infos[2]{};
        infos[0].sType          = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT;
        // NO_TASK_SHADER: this mesh shader runs standalone (no task/amplification stage). Without it the driver expects a task
        // shader to precede the mesh and FAULTS (device-lost) when vkCmdDrawMeshTasksEXT dispatches with no task bound.
        infos[0].flags          = VK_SHADER_CREATE_LINK_STAGE_BIT_EXT | VK_SHADER_CREATE_NO_TASK_SHADER_BIT_EXT;
        infos[0].stage          = VK_SHADER_STAGE_MESH_BIT_EXT;
        infos[0].nextStage      = VK_SHADER_STAGE_FRAGMENT_BIT;
        infos[0].codeType       = VK_SHADER_CODE_TYPE_SPIRV_EXT;
        infos[0].codeSize       = mp.size();
        infos[0].pCode          = mp.data();
        infos[0].pName          = "main";
        infos[0].setLayoutCount = 1U;
        infos[0].pSetLayouts    = &m_storage_set_layout;
        infos[1].sType          = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT;
        infos[1].flags          = VK_SHADER_CREATE_LINK_STAGE_BIT_EXT;
        infos[1].stage          = VK_SHADER_STAGE_FRAGMENT_BIT;
        infos[1].nextStage      = 0;
        infos[1].codeType       = VK_SHADER_CODE_TYPE_SPIRV_EXT;
        infos[1].codeSize       = fp.size();
        infos[1].pCode          = fp.data();
        infos[1].pName          = "main";
        infos[1].setLayoutCount = 1U;
        infos[1].pSetLayouts    = &m_storage_set_layout;

        VkShaderEXT shaders[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
        if (m_api.create(m_device, 2U, infos, nullptr, shaders) != VK_SUCCESS)
        {
            vkDestroyPipelineLayout(m_device, layout, nullptr);
            return nullptr;
        }
        return std::make_unique<VulkanRasterProgram>(m_device, &m_api, layout, shaders[0], shaders[1], /*is_mesh=*/true);
    }

    // B4: a TASK→MESH→FRAGMENT program (the AMPLIFICATION path). The task shader runs first, computes how many mesh workgroups
    // to launch (EmitMeshTasksEXT) + a payload; the mesh reads the payload. Unlike create_mesh_program the mesh MUST NOT carry
    // NO_TASK_SHADER (a task now precedes it — setting it would device-lost). Draw with draw_mesh(group_count = TASK groups).
    [[nodiscard]] std::unique_ptr<IRasterProgram>
    create_task_mesh_program(IGpuProgram& task, IGpuProgram& mesh, IGpuProgram& fragment) override
    {
        if (!m_api.valid() || m_api.draw_mesh_tasks == nullptr || !m_ctx->mesh_shader() || task.stage() != ShaderStage::Task
            || mesh.stage() != ShaderStage::Mesh || fragment.stage() != ShaderStage::Fragment)
        {
            return nullptr;
        }
        const auto& tp = static_cast<VulkanGpuProgram&>(task).vk_spirv();
        const auto& mp = static_cast<VulkanGpuProgram&>(mesh).vk_spirv();
        const auto& fp = static_cast<VulkanGpuProgram&>(fragment).vk_spirv();

        VkPipelineLayoutCreateInfo lci{};
        lci.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        lci.setLayoutCount = 1U;
        lci.pSetLayouts    = &m_storage_set_layout;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        if (vkCreatePipelineLayout(m_device, &lci, nullptr, &layout) != VK_SUCCESS) { return nullptr; }

        VkShaderCreateInfoEXT infos[3]{};
        infos[0].sType          = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT;
        infos[0].flags          = VK_SHADER_CREATE_LINK_STAGE_BIT_EXT; // a task precedes the mesh — do NOT set NO_TASK_SHADER
        infos[0].stage          = VK_SHADER_STAGE_TASK_BIT_EXT;
        infos[0].nextStage      = VK_SHADER_STAGE_MESH_BIT_EXT;
        infos[0].codeType       = VK_SHADER_CODE_TYPE_SPIRV_EXT;
        infos[0].codeSize       = tp.size();
        infos[0].pCode          = tp.data();
        infos[0].pName          = "main";
        infos[0].setLayoutCount = 1U;
        infos[0].pSetLayouts    = &m_storage_set_layout;
        infos[1].sType          = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT;
        infos[1].flags          = VK_SHADER_CREATE_LINK_STAGE_BIT_EXT; // mesh: task-fed, so NO NO_TASK_SHADER flag
        infos[1].stage          = VK_SHADER_STAGE_MESH_BIT_EXT;
        infos[1].nextStage      = VK_SHADER_STAGE_FRAGMENT_BIT;
        infos[1].codeType       = VK_SHADER_CODE_TYPE_SPIRV_EXT;
        infos[1].codeSize       = mp.size();
        infos[1].pCode          = mp.data();
        infos[1].pName          = "main";
        infos[1].setLayoutCount = 1U;
        infos[1].pSetLayouts    = &m_storage_set_layout;
        infos[2].sType          = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT;
        infos[2].flags          = VK_SHADER_CREATE_LINK_STAGE_BIT_EXT;
        infos[2].stage          = VK_SHADER_STAGE_FRAGMENT_BIT;
        infos[2].nextStage      = 0;
        infos[2].codeType       = VK_SHADER_CODE_TYPE_SPIRV_EXT;
        infos[2].codeSize       = fp.size();
        infos[2].pCode          = fp.data();
        infos[2].pName          = "main";
        infos[2].setLayoutCount = 1U;
        infos[2].pSetLayouts    = &m_storage_set_layout;

        VkShaderEXT shaders[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
        if (m_api.create(m_device, 3U, infos, nullptr, shaders) != VK_SUCCESS)
        {
            vkDestroyPipelineLayout(m_device, layout, nullptr);
            return nullptr;
        }
        // vs() slot = the MESH shader (shaders[1]); fs() = fragment (shaders[2]); task() = shaders[0].
        return std::make_unique<VulkanRasterProgram>(m_device, &m_api, layout, shaders[1], shaders[2], /*is_mesh=*/true,
                                                     shaders[0]);
    }

    // B4-tess: a VS→TESS-CONTROL→TESS-EVAL→FRAGMENT program (the PORTABLE displacement path for HW without mesh shaders). The
    // VS emits patch control points; the hull sets the tess levels + passes them through; the domain interpolates + displaces.
    // Draw with draw_tess (PATCH_LIST + a dynamic patch size). nullptr if the device lacks tessellation + the patch-CP state.
    [[nodiscard]] std::unique_ptr<IRasterProgram> create_tess_program(IGpuProgram& vertex, IGpuProgram& tess_control,
                                                                      IGpuProgram& tess_eval, IGpuProgram& fragment) override
    {
        if (!m_api.valid() || !m_ctx->tessellation() || m_api.set_patch_control_points == nullptr
            || vertex.stage() != ShaderStage::Vertex || tess_control.stage() != ShaderStage::TessControl
            || tess_eval.stage() != ShaderStage::TessEval || fragment.stage() != ShaderStage::Fragment)
        {
            return nullptr;
        }
        const auto& vp = static_cast<VulkanGpuProgram&>(vertex).vk_spirv();
        const auto& cp = static_cast<VulkanGpuProgram&>(tess_control).vk_spirv();
        const auto& ep = static_cast<VulkanGpuProgram&>(tess_eval).vk_spirv();
        const auto& fp = static_cast<VulkanGpuProgram&>(fragment).vk_spirv();

        VkPipelineLayoutCreateInfo lci{};
        lci.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        lci.setLayoutCount = 1U;
        lci.pSetLayouts    = &m_storage_set_layout;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        if (vkCreatePipelineLayout(m_device, &lci, nullptr, &layout) != VK_SUCCESS) { return nullptr; }

        const VkShaderStageFlagBits st[4]   = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                                               VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
        const VkShaderStageFlagBits next[4] = {VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                                               VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, VK_SHADER_STAGE_FRAGMENT_BIT,
                                               static_cast<VkShaderStageFlagBits>(0)};
        const void*  code_ptr[4]  = {vp.data(), cp.data(), ep.data(), fp.data()};
        const size_t code_size[4] = {vp.size(), cp.size(), ep.size(), fp.size()};
        VkShaderCreateInfoEXT infos[4]{};
        for (int i = 0; i < 4; ++i)
        {
            infos[i].sType          = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT;
            infos[i].flags          = VK_SHADER_CREATE_LINK_STAGE_BIT_EXT;
            infos[i].stage          = st[i];
            infos[i].nextStage      = next[i];
            infos[i].codeType       = VK_SHADER_CODE_TYPE_SPIRV_EXT;
            infos[i].codeSize       = code_size[i];
            infos[i].pCode          = code_ptr[i];
            infos[i].pName          = "main";
            infos[i].setLayoutCount = 1U;
            infos[i].pSetLayouts    = &m_storage_set_layout;
        }
        VkShaderEXT shaders[4] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
        if (m_api.create(m_device, 4U, infos, nullptr, shaders) != VK_SUCCESS)
        {
            vkDestroyPipelineLayout(m_device, layout, nullptr);
            return nullptr;
        }
        // vs()=VS (shaders[0]); fs()=FS (shaders[3]); tcs=shaders[1]; tes=shaders[2].
        return std::make_unique<VulkanRasterProgram>(m_device, &m_api, layout, shaders[0], shaders[3], /*is_mesh=*/false,
                                                     VK_NULL_HANDLE, shaders[1], shaders[2]);
    }

    // B4-tess: draw `patch_count` QUAD patches through the tessellator (VS→TCS→TES→FS) with PATCH_LIST topology + a dynamic
    // patch size of 4. The domain shader displaces + writes the clip position. Colour-only; result host-readable via read_pixel.
    void draw_tess(IRasterTarget& target, IRasterProgram& program, ClearColor clear_color, crd::u32 patch_count) override
    {
        auto& t = static_cast<VulkanRasterTarget&>(target);
        auto& p = static_cast<VulkanRasterProgram&>(program);
        if (!m_api.valid() || m_api.set_patch_control_points == nullptr || !p.valid() || !p.is_tess()) { return; }

        VkCommandBuffer cmd = begin_cmd();
        if (cmd == VK_NULL_HANDLE) { return; }
        const bool ms = t.multisampled();
        transition(cmd, t.image(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        if (ms)
        {
            transition(cmd, t.resolve_image(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                       VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        }
        VkRenderingAttachmentInfo att = colour_clear_attachment(t.view(), clear_color);
        if (ms)
        {
            att.resolveMode        = VK_RESOLVE_MODE_AVERAGE_BIT;
            att.resolveImageView   = t.resolve_view();
            att.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        VkRenderingInfo ri{};
        ri.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea.extent    = {t.width(), t.height()};
        ri.layerCount           = 1U;
        ri.colorAttachmentCount = 1U;
        ri.pColorAttachments    = &att;
        vkCmdBeginRendering(cmd, &ri);

        set_draw_state(cmd, t.width(), t.height(), t.samples(), false, VK_COMPARE_OP_ALWAYS);
        vkCmdSetPrimitiveTopology(cmd, VK_PRIMITIVE_TOPOLOGY_PATCH_LIST); // tessellation consumes patches, not triangles
        m_api.set_patch_control_points(cmd, 4U);                          // quad patch (matches the layout(quads) domain shader)
        const VkShaderStageFlagBits stages[4] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                                                 VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
        const VkShaderEXT           objs[4]   = {p.vs(), p.tcs(), p.tes(), p.fs()};
        m_api.bind(cmd, 4U, stages, objs);
        vkCmdDraw(cmd, patch_count * 4U, 1U, 0U, 0U); // patch_count QUAD patches × 4 control points

        vkCmdEndRendering(cmd);
        const VkImage src = t.src_image();
        transition(cmd, src, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1U;
        region.imageExtent                 = {t.width(), t.height(), 1U};
        vkCmdCopyImageToBuffer(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.readback(), 1U, &region);
        end_and_wait(cmd);
    }

    // B4: bind a MESH program and dispatch `group_count` mesh workgroups. Colour-only (the mesh-triangle proof). VERTEX is bound
    // null (mutually exclusive with MESH); TESS/GEOM/TASK default to null in a fresh command buffer (as the vertex draw relies on).
    void draw_mesh(IRasterTarget& target, IRasterProgram& program, ClearColor clear_color, crd::u32 group_count) override
    {
        auto& t = static_cast<VulkanRasterTarget&>(target);
        auto& p = static_cast<VulkanRasterProgram&>(program);
        if (!m_api.valid() || m_api.draw_mesh_tasks == nullptr || !p.valid() || !p.is_mesh()) { return; }

        VkCommandBuffer cmd = begin_cmd();
        if (cmd == VK_NULL_HANDLE) { return; }
        const bool ms = t.multisampled();
        transition(cmd, t.image(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        if (ms)
        {
            transition(cmd, t.resolve_image(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                       VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        }
        VkRenderingAttachmentInfo att = colour_clear_attachment(t.view(), clear_color);
        if (ms)
        {
            att.resolveMode        = VK_RESOLVE_MODE_AVERAGE_BIT;
            att.resolveImageView   = t.resolve_view();
            att.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        VkRenderingInfo ri{};
        ri.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea.extent    = {t.width(), t.height()};
        ri.layerCount           = 1U;
        ri.colorAttachmentCount = 1U;
        ri.pColorAttachments    = &att;
        vkCmdBeginRendering(cmd, &ri);

        set_draw_state(cmd, t.width(), t.height(), t.samples(), false, VK_COMPARE_OP_ALWAYS, 1U, /*mesh_draw=*/true);
        // Bind VERTEX=null (mutually exclusive with mesh) + MESH + FRAGMENT. TESS/GEOM/TASK stay unbound (their features are off,
        // so they must NOT appear in pStages; unbound = null in a fresh command buffer, so the mesh runs standalone).
        const VkShaderStageFlagBits vnull[1] = {VK_SHADER_STAGE_VERTEX_BIT};
        const VkShaderEXT           vnob[1]  = {VK_NULL_HANDLE};
        m_api.bind(cmd, 1U, vnull, vnob);
        // B4: bind the TASK (amplification) stage if this is a task→mesh program; else it stays null (mesh has NO_TASK_SHADER).
        if (p.has_task())
        {
            const VkShaderStageFlagBits tstage[1] = {VK_SHADER_STAGE_TASK_BIT_EXT};
            const VkShaderEXT           tobj[1]   = {p.task()};
            m_api.bind(cmd, 1U, tstage, tobj);
        }
        const VkShaderStageFlagBits mstages[2] = {VK_SHADER_STAGE_MESH_BIT_EXT, VK_SHADER_STAGE_FRAGMENT_BIT};
        const VkShaderEXT           mobjs[2]   = {p.vs(), p.fs()};
        m_api.bind(cmd, 2U, mstages, mobjs);
        m_api.draw_mesh_tasks(cmd, group_count, 1U, 1U); // group_count = TASK workgroups when has_task() (they amplify)
        vkCmdEndRendering(cmd);

        const VkImage src = t.src_image();
        transition(cmd, src, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1U;
        region.imageExtent                 = {t.width(), t.height(), 1U};
        vkCmdCopyImageToBuffer(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.readback(), 1U, &region);
        end_and_wait(cmd);
    }

    // B4: DISPATCH a MESH program with PER-PRIMITIVE VRS. Identical to draw_mesh, but sets the VRS combiner to REPLACE so the
    // mesh's per-primitive `gl_MeshPrimitivesEXT[].gl_PrimitiveShadingRateEXT` output drives the coarse fragment rate (the
    // pipeline rate stays 1×1). A distant/low-detail meshlet can shade itself at a lower rate — a real perf lever.
    void draw_mesh_vrs(IRasterTarget& target, IRasterProgram& program, ClearColor clear_color, crd::u32 group_count) override
    {
        auto& t = static_cast<VulkanRasterTarget&>(target);
        auto& p = static_cast<VulkanRasterProgram&>(program);
        if (!m_api.valid() || m_api.draw_mesh_tasks == nullptr || !p.valid() || !p.is_mesh()) { return; }
        if (m_set_vrs == nullptr) { draw_mesh(target, program, clear_color, group_count); return; } // no VRS ⇒ full-rate mesh draw

        VkCommandBuffer cmd = begin_cmd();
        if (cmd == VK_NULL_HANDLE) { return; }
        transition(cmd, t.image(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        VkRenderingAttachmentInfo att = colour_clear_attachment(t.view(), clear_color);
        VkRenderingInfo           ri{};
        ri.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea.extent    = {t.width(), t.height()};
        ri.layerCount           = 1U;
        ri.colorAttachmentCount = 1U;
        ri.pColorAttachments    = &att;
        vkCmdBeginRendering(cmd, &ri);

        set_draw_state(cmd, t.width(), t.height(), t.samples(), false, VK_COMPARE_OP_ALWAYS, 1U, /*mesh_draw=*/true);
        const VkExtent2D                         frag1x1 = {1U, 1U}; // pipeline rate 1×1; the primitive rate REPLACES it
        const VkFragmentShadingRateCombinerOpKHR comb[2] = {VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR,
                                                            VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR};
        m_set_vrs(cmd, &frag1x1, comb);
        const VkShaderStageFlagBits vnull[1] = {VK_SHADER_STAGE_VERTEX_BIT};
        const VkShaderEXT           vnob[1]  = {VK_NULL_HANDLE};
        m_api.bind(cmd, 1U, vnull, vnob);
        if (p.has_task())
        {
            const VkShaderStageFlagBits tstage[1] = {VK_SHADER_STAGE_TASK_BIT_EXT};
            const VkShaderEXT           tobj[1]   = {p.task()};
            m_api.bind(cmd, 1U, tstage, tobj);
        }
        const VkShaderStageFlagBits mstages[2] = {VK_SHADER_STAGE_MESH_BIT_EXT, VK_SHADER_STAGE_FRAGMENT_BIT};
        const VkShaderEXT           mobjs[2]   = {p.vs(), p.fs()};
        m_api.bind(cmd, 2U, mstages, mobjs);
        m_api.draw_mesh_tasks(cmd, group_count, 1U, 1U);
        vkCmdEndRendering(cmd);

        const VkImage vsrc = t.src_image();
        transition(cmd, vsrc, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy vregion{};
        vregion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vregion.imageSubresource.layerCount = 1U;
        vregion.imageExtent                 = {t.width(), t.height(), 1U};
        vkCmdCopyImageToBuffer(cmd, vsrc, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.readback(), 1U, &vregion);
        end_and_wait(cmd);
    }

    // B4: GPU-DRIVEN INDIRECT MESHLET DISPATCH. Identical to draw_mesh, but the mesh-workgroup count comes from `native_args`
    // (a `VkBuffer` an earlier compute CULL pass wrote as {groupCountX, 1, 1} — see build_meshlet_cull), consumed by
    // vkCmdDrawMeshTasksIndirectEXT. The culled meshlets never dispatch and the CPU never learns the count — the Nanite scale
    // loop. The args buffer is `compute_usage::indirect` (CONCURRENT-shared across the compute/graphics queues); the compute
    // submit fenced before this raster submit, so its write is visible without an explicit ownership transfer.
    void draw_mesh_indirect(IRasterTarget& target, IRasterProgram& program, ClearColor clear_color, void* native_args,
                            crd::u64 args_offset) override
    {
        auto& t = static_cast<VulkanRasterTarget&>(target);
        auto& p = static_cast<VulkanRasterProgram&>(program);
        if (!m_api.valid() || m_api.draw_mesh_tasks_indirect == nullptr || !p.valid() || !p.is_mesh()
            || native_args == nullptr)
        {
            return;
        }
        const VkBuffer args = reinterpret_cast<VkBuffer>(native_args);

        VkCommandBuffer cmd = begin_cmd();
        if (cmd == VK_NULL_HANDLE) { return; }
        transition(cmd, t.image(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        VkRenderingAttachmentInfo att = colour_clear_attachment(t.view(), clear_color);
        VkRenderingInfo           ri{};
        ri.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea.extent    = {t.width(), t.height()};
        ri.layerCount           = 1U;
        ri.colorAttachmentCount = 1U;
        ri.pColorAttachments    = &att;
        vkCmdBeginRendering(cmd, &ri);

        set_draw_state(cmd, t.width(), t.height(), t.samples(), false, VK_COMPARE_OP_ALWAYS, 1U, /*mesh_draw=*/true);
        const VkShaderStageFlagBits vnull[1] = {VK_SHADER_STAGE_VERTEX_BIT};
        const VkShaderEXT           vnob[1]  = {VK_NULL_HANDLE};
        m_api.bind(cmd, 1U, vnull, vnob);
        if (p.has_task())
        {
            const VkShaderStageFlagBits tstage[1] = {VK_SHADER_STAGE_TASK_BIT_EXT};
            const VkShaderEXT           tobj[1]   = {p.task()};
            m_api.bind(cmd, 1U, tstage, tobj);
        }
        const VkShaderStageFlagBits mstages[2] = {VK_SHADER_STAGE_MESH_BIT_EXT, VK_SHADER_STAGE_FRAGMENT_BIT};
        const VkShaderEXT           mobjs[2]   = {p.vs(), p.fs()};
        m_api.bind(cmd, 2U, mstages, mobjs);
        m_api.draw_mesh_tasks_indirect(cmd, args, args_offset, 1U,
                                       static_cast<crd::u32>(sizeof(VkDrawMeshTasksIndirectCommandEXT))); // ONE indirect command
        vkCmdEndRendering(cmd);

        const VkImage isrc = t.src_image();
        transition(cmd, isrc, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy iregion{};
        iregion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        iregion.imageSubresource.layerCount = 1U;
        iregion.imageExtent                 = {t.width(), t.height(), 1U};
        vkCmdCopyImageToBuffer(cmd, isrc, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.readback(), 1U, &iregion);
        end_and_wait(cmd);
    }

    void draw(IRasterTarget& target, IRasterProgram& program, ClearColor clear_color, crd::u32 vertex_count) override
    {
        auto& t = static_cast<VulkanRasterTarget&>(target);
        auto& p = static_cast<VulkanRasterProgram&>(program);
        if (!m_api.valid() || !p.valid()) { return; }

        VkCommandBuffer cmd = begin_cmd();
        if (cmd == VK_NULL_HANDLE) { return; }

        const bool ms = t.multisampled();

        // The colour attachment (MSAA image when ms). For MSAA the resolve image is also an attachment (the resolve writes
        // it), so it must be in COLOR_ATTACHMENT_OPTIMAL before rendering too.
        transition(cmd, t.image(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        if (ms)
        {
            transition(cmd, t.resolve_image(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                       VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        }

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
        if (ms) // AVERAGE-resolve the MSAA image into the single-sample resolve image, which read_pixel reads
        {
            att.resolveMode        = VK_RESOLVE_MODE_AVERAGE_BIT;
            att.resolveImageView   = t.resolve_view();
            att.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }

        VkRenderingInfo ri{};
        ri.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea.extent    = {t.width(), t.height()};
        ri.layerCount           = 1U;
        ri.colorAttachmentCount = 1U;
        ri.pColorAttachments    = &att;
        vkCmdBeginRendering(cmd, &ri);

        set_draw_state(cmd, t.width(), t.height(), t.samples(), false, VK_COMPARE_OP_ALWAYS);
        const VkShaderStageFlagBits stages[2] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
        const VkShaderEXT           objs[2]   = {p.vs(), p.fs()};
        m_api.bind(cmd, 2U, stages, objs);
        vkCmdDraw(cmd, vertex_count, 1U, 0U, 0U);

        vkCmdEndRendering(cmd);

        // Copy the READBACK SOURCE (the resolved single-sample image for MSAA, else the colour image) to the buffer.
        const VkImage src = t.src_image();
        transition(cmd, src, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1U;
        region.imageExtent                 = {t.width(), t.height(), 1U};
        vkCmdCopyImageToBuffer(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.readback(), 1U, &region);

        end_and_wait(cmd);
    }

    // B4-vis-4: HW-raster into a R32_UINT VISIBILITY BUFFER (create_visbuffer_target). Identical to draw() but single-sample +
    // a UINT clear (R32_UINT requires the uint32 clear union). The FS writes SV_PrimitiveId, so each pixel records which
    // triangle the HW rasterizer covered it with — the hybrid Nanite path for big triangles. read_pixel returns the raw id.
    void draw_visbuffer(IRasterTarget& target, IRasterProgram& program, crd::u32 clear_id, crd::u32 vertex_count) override
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
        att.sType                      = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        att.imageView                  = t.view();
        att.imageLayout                = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.loadOp                     = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp                    = VK_ATTACHMENT_STORE_OP_STORE;
        att.clearValue.color.uint32[0] = clear_id; // R32_UINT: the empty/background id
        VkRenderingInfo ri{};
        ri.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea.extent    = {t.width(), t.height()};
        ri.layerCount           = 1U;
        ri.colorAttachmentCount = 1U;
        ri.pColorAttachments    = &att;
        vkCmdBeginRendering(cmd, &ri);

        set_draw_state(cmd, t.width(), t.height(), t.samples(), false, VK_COMPARE_OP_ALWAYS);
        const VkShaderStageFlagBits stages[2] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
        const VkShaderEXT           objs[2]   = {p.vs(), p.fs()};
        m_api.bind(cmd, 2U, stages, objs);
        vkCmdDraw(cmd, vertex_count, 1U, 0U, 0U);

        vkCmdEndRendering(cmd);
        const VkImage src = t.src_image();
        transition(cmd, src, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy vregion{};
        vregion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vregion.imageSubresource.layerCount = 1U;
        vregion.imageExtent                 = {t.width(), t.height(), 1U};
        vkCmdCopyImageToBuffer(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.readback(), 1U, &vregion);
        end_and_wait(cmd);
    }

    void draw_depth(IRasterTarget& target, IRasterProgram& program, ClearColor clear_color, float clear_depth,
                    DepthCompare compare, crd::u32 vertex_count) override
    {
        auto& t = static_cast<VulkanRasterTarget&>(target);
        auto& p = static_cast<VulkanRasterProgram&>(program);
        if (!m_api.valid() || !p.valid() || !t.has_depth()) { return; } // needs a create_color_depth_target target

        VkCommandBuffer cmd = begin_cmd();
        if (cmd == VK_NULL_HANDLE) { return; }

        transition(cmd, t.image(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        transition_depth(cmd, t.depth_image());

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

        VkRenderingAttachmentInfo dep{};
        dep.sType                              = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        dep.imageView                          = t.depth_view();
        dep.imageLayout                        = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        dep.loadOp                             = VK_ATTACHMENT_LOAD_OP_CLEAR;
        dep.storeOp                            = VK_ATTACHMENT_STORE_OP_DONT_CARE; // depth is never read back
        dep.clearValue.depthStencil.depth      = clear_depth;

        VkRenderingInfo ri{};
        ri.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea.extent    = {t.width(), t.height()};
        ri.layerCount           = 1U;
        ri.colorAttachmentCount = 1U;
        ri.pColorAttachments    = &att;
        ri.pDepthAttachment     = &dep;
        vkCmdBeginRendering(cmd, &ri);

        set_draw_state(cmd, t.width(), t.height(), 1U, true, to_vk_compare(compare));
        const VkShaderStageFlagBits stages[2] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
        const VkShaderEXT           objs[2]   = {p.vs(), p.fs()};
        m_api.bind(cmd, 2U, stages, objs);
        vkCmdDraw(cmd, vertex_count, 1U, 0U, 0U);

        vkCmdEndRendering(cmd);

        const VkImage src = t.src_image(); // single-sample ⇒ the colour image
        transition(cmd, src, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1U;
        region.imageExtent                 = {t.width(), t.height(), 1U};
        vkCmdCopyImageToBuffer(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.readback(), 1U, &region);

        end_and_wait(cmd);
    }

    void draw_vrs(IRasterTarget& target, IRasterProgram& program, ClearColor clear_color, ShadingRate pipeline_rate,
                  ShadingRateCombiner primitive_combiner, crd::u32 vertex_count) override
    {
        auto& t = static_cast<VulkanRasterTarget&>(target);
        auto& p = static_cast<VulkanRasterProgram&>(program);
        if (!m_api.valid() || !p.valid()) { return; }
        if (m_set_vrs == nullptr) { draw(target, program, clear_color, vertex_count); return; } // no VRS ⇒ a plain 1x1 draw

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

        VkRenderingFragmentShadingRateAttachmentInfoKHR vrs_att{}; // the per-tile ATTACHMENT rate (third source), if present
        vrs_att.sType                          = VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR;
        vrs_att.imageView                      = t.vrs_view();
        vrs_att.imageLayout                    = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
        vrs_att.shadingRateAttachmentTexelSize = m_ctx->vrs_tile_size();

        VkRenderingInfo ri{};
        ri.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea.extent    = {t.width(), t.height()};
        ri.layerCount           = 1U;
        ri.colorAttachmentCount = 1U;
        ri.pColorAttachments    = &att;
        if (t.has_vrs()) { ri.pNext = &vrs_att; }
        vkCmdBeginRendering(cmd, &ri);

        set_draw_state(cmd, t.width(), t.height(), 1U, false, VK_COMPARE_OP_ALWAYS); // sets a 1x1 VRS default
        // Override: pipeline rate + combiners. comb[0] = pipeline∘primitive (Replace ⇒ the shader-output rate wins);
        // comb[1] = ∘attachment (Replace when the target carries a per-tile rate image).
        const VkExtent2D                         frag    = vrs_extent(pipeline_rate);
        const VkFragmentShadingRateCombinerOpKHR comb[2] = {
            vrs_combiner(primitive_combiner),
            t.has_vrs() ? VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR : VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR};
        m_set_vrs(cmd, &frag, comb);

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

    void draw_conservative(IRasterTarget& target, IRasterProgram& program, ClearColor clear_color, ConservativeMode mode,
                           crd::u32 vertex_count) override
    {
        auto& t = static_cast<VulkanRasterTarget&>(target);
        auto& p = static_cast<VulkanRasterProgram&>(program);
        if (!m_api.valid() || !p.valid()) { return; }
        if (m_set_conservative == nullptr) { draw(target, program, clear_color, vertex_count); return; } // unsupported

        VkCommandBuffer cmd = begin_cmd();
        if (cmd == VK_NULL_HANDLE) { return; }

        transition(cmd, t.image(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

        VkRenderingAttachmentInfo att = colour_clear_attachment(t.view(), clear_color);
        VkRenderingInfo           ri  = one_colour_rendering(t, att);
        vkCmdBeginRendering(cmd, &ri);

        set_draw_state(cmd, t.width(), t.height(), 1U, false, VK_COMPARE_OP_ALWAYS); // sets DISABLED conservative default
        m_set_conservative(cmd, to_conservative_mode(mode));                          // override to the requested mode
        // Overestimate + shader objects requires the extra-overestimation-size dynamic state set too (VUID-vkCmdDraw-07632).
        if (mode != ConservativeMode::Off && m_set_overest_size != nullptr) { m_set_overest_size(cmd, 0.0F); }
        bind_and_draw(cmd, p, vertex_count);

        vkCmdEndRendering(cmd);
        copy_colour_to_readback(cmd, t);
        end_and_wait(cmd);
    }

    void draw_storage(IRasterTarget& target, IRasterProgram& program, ClearColor clear_color, IStorageBuffer& storage,
                      crd::u32 vertex_count) override
    {
        auto& t = static_cast<VulkanRasterTarget&>(target);
        auto& p = static_cast<VulkanRasterProgram&>(program);
        auto& s = static_cast<VulkanStorageBuffer&>(storage);
        if (!m_api.valid() || !p.valid() || m_desc_pool == VK_NULL_HANDLE) { return; }

        // Allocate + point the storage descriptor (set 0, binding 0) at the buffer.
        vkResetDescriptorPool(m_device, m_desc_pool, 0);
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool     = m_desc_pool;
        dsai.descriptorSetCount = 1U;
        dsai.pSetLayouts        = &m_storage_set_layout;
        VkDescriptorSet dset = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(m_device, &dsai, &dset) != VK_SUCCESS) { return; }
        VkDescriptorBufferInfo dbi{s.buf(), 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet   wr{};
        wr.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr.dstSet          = dset;
        wr.dstBinding      = 0U;
        wr.descriptorCount = 1U;
        wr.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        wr.pBufferInfo     = &dbi;
        vkUpdateDescriptorSets(m_device, 1U, &wr, 0U, nullptr);

        VkCommandBuffer cmd = begin_cmd();
        if (cmd == VK_NULL_HANDLE) { return; }
        transition(cmd, t.image(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

        VkRenderingAttachmentInfo att = colour_clear_attachment(t.view(), clear_color);
        VkRenderingInfo           ri  = one_colour_rendering(t, att);
        vkCmdBeginRendering(cmd, &ri);
        set_draw_state(cmd, t.width(), t.height(), 1U, false, VK_COMPARE_OP_ALWAYS);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.layout(), 0U, 1U, &dset, 0U, nullptr);
        bind_and_draw(cmd, p, vertex_count);
        vkCmdEndRendering(cmd);

        // FS storage writes → transfer read, then copy the SSBO into its host-visible readback.
        VkBufferMemoryBarrier bmb{};
        bmb.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        bmb.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
        bmb.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
        bmb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bmb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bmb.buffer              = s.buf();
        bmb.size                = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1U,
                             &bmb, 0, nullptr);
        VkBufferCopy bc{0, 0, s.size_bytes()};
        vkCmdCopyBuffer(cmd, s.buf(), s.readback(), 1U, &bc);

        copy_colour_to_readback(cmd, t);
        end_and_wait(cmd);
    }

    [[nodiscard]] std::unique_ptr<ITexture> create_texture(crd::u32 width, crd::u32 height, const void* rgba) override
    {
        if (width == 0U || height == 0U || rgba == nullptr) { return nullptr; }
        ImageBundle img{};
        if (!create_image_bundle(width, height, VK_SAMPLE_COUNT_1_BIT, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT,
                                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, img))
        {
            return nullptr;
        }
        // Upload the pixels through a host-visible staging buffer (device-local sampled images are not host-mappable).
        const VkDeviceSize bytes   = static_cast<VkDeviceSize>(width) * height * 4U;
        VkBuffer           staging = VK_NULL_HANDLE;
        VkDeviceMemory     smem    = VK_NULL_HANDLE;
        void*              mapped  = nullptr;
        if (!make_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging, smem,
                         &mapped)
            || mapped == nullptr)
        {
            destroy_image_bundle(m_device, img);
            if (staging != VK_NULL_HANDLE) { vkDestroyBuffer(m_device, staging, nullptr); }
            if (smem != VK_NULL_HANDLE) { vkFreeMemory(m_device, smem, nullptr); }
            return nullptr;
        }
        std::memcpy(mapped, rgba, static_cast<size_t>(bytes));

        VkCommandBuffer cmd = begin_cmd();
        if (cmd != VK_NULL_HANDLE)
        {
            transition(cmd, img.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy region{};
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1U;
            region.imageExtent                 = {width, height, 1U};
            vkCmdCopyBufferToImage(cmd, staging, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1U, &region);
            transition(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            end_and_wait(cmd);
        }
        vkDestroyBuffer(m_device, staging, nullptr);
        vkFreeMemory(m_device, smem, nullptr);
        return std::make_unique<VulkanTexture>(m_device, img, width, height);
    }

    // B16: a mip-mapped RGBA8 texture — upload level 0, then vkCmdBlitImage down the pyramid (box/linear filter). The default
    // sampler is LINEAR-mipmap, so minified `KOp::TexSample` filters instead of aliasing (the ocean tile viewed to the horizon).
    [[nodiscard]] std::unique_ptr<ITexture> create_texture_mipped(crd::u32 width, crd::u32 height, const void* rgba) override
    {
        if (width == 0U || height == 0U || rgba == nullptr) { return nullptr; }
        crd::u32 mips    = 1U;
        crd::u32 dim_max = width > height ? width : height;
        while ((dim_max >> mips) > 0U) { ++mips; } // floor(log2(max(w,h))) + 1
        ImageBundle img{};
        if (!create_image_bundle(width, height, VK_SAMPLE_COUNT_1_BIT, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT,
                                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                 img, mips))
        {
            return nullptr;
        }
        const VkDeviceSize bytes   = static_cast<VkDeviceSize>(width) * height * 4U;
        VkBuffer           staging = VK_NULL_HANDLE;
        VkDeviceMemory     smem    = VK_NULL_HANDLE;
        void*              mapped  = nullptr;
        if (!make_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging, smem, &mapped)
            || mapped == nullptr)
        {
            destroy_image_bundle(m_device, img);
            if (staging != VK_NULL_HANDLE) { vkDestroyBuffer(m_device, staging, nullptr); }
            if (smem != VK_NULL_HANDLE) { vkFreeMemory(m_device, smem, nullptr); }
            return nullptr;
        }
        std::memcpy(mapped, rgba, static_cast<size_t>(bytes));

        const auto mbar = [&](VkCommandBuffer c, VkImageLayout from, VkImageLayout to, VkAccessFlags sa, VkAccessFlags da,
                              VkPipelineStageFlags ss, VkPipelineStageFlags ds, crd::u32 level, crd::u32 count) {
            VkImageMemoryBarrier b{};
            b.sType                         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.oldLayout                     = from;
            b.newLayout                     = to;
            b.srcQueueFamilyIndex           = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex           = VK_QUEUE_FAMILY_IGNORED;
            b.image                         = img.image;
            b.subresourceRange.aspectMask   = VK_IMAGE_ASPECT_COLOR_BIT;
            b.subresourceRange.baseMipLevel = level;
            b.subresourceRange.levelCount   = count;
            b.subresourceRange.layerCount   = 1U;
            b.srcAccessMask                 = sa;
            b.dstAccessMask                 = da;
            vkCmdPipelineBarrier(c, ss, ds, 0, 0, nullptr, 0, nullptr, 1U, &b);
        };

        VkCommandBuffer cmd = begin_cmd();
        if (cmd != VK_NULL_HANDLE)
        {
            mbar(cmd, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, mips);
            VkBufferImageCopy region{};
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1U;
            region.imageExtent                 = {width, height, 1U};
            vkCmdCopyBufferToImage(cmd, staging, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1U, &region);

            crd::i32 mw = static_cast<crd::i32>(width);
            crd::i32 mh = static_cast<crd::i32>(height);
            for (crd::u32 i = 1U; i < mips; ++i) // blit level i-1 → level i (each dimension halved)
            {
                mbar(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, i - 1U, 1U);
                const crd::i32 nw = mw > 1 ? mw / 2 : 1;
                const crd::i32 nh = mh > 1 ? mh / 2 : 1;
                VkImageBlit    blit{};
                blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                blit.srcSubresource.mipLevel   = i - 1U;
                blit.srcSubresource.layerCount = 1U;
                blit.srcOffsets[1]             = {mw, mh, 1};
                blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                blit.dstSubresource.mipLevel   = i;
                blit.dstSubresource.layerCount = 1U;
                blit.dstOffsets[1]             = {nw, nh, 1};
                vkCmdBlitImage(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1U, &blit, VK_FILTER_LINEAR);
                mw = nw;
                mh = nh;
            }
            if (mips > 1U) // levels 0..mips-2 are TRANSFER_SRC, the last level is still TRANSFER_DST → all to SHADER_READ
            {
                mbar(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0U, mips - 1U);
            }
            mbar(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, mips - 1U, 1U);
            end_and_wait(cmd);
        }
        vkDestroyBuffer(m_device, staging, nullptr);
        vkFreeMemory(m_device, smem, nullptr);
        return std::make_unique<VulkanTexture>(m_device, img, width, height);
    }

    [[nodiscard]] std::unique_ptr<ITexture> create_texture_dim(TextureKind kind, crd::u32 width, crd::u32 height,
                                                               crd::u32 depth_or_layers, const void* rgba) override
    {
        if (width == 0U || height == 0U || rgba == nullptr) { return nullptr; }
        // Resolve the image shape from the kind: 3D uses extent.depth; cube/array use arrayLayers (6 per cube).
        crd::u32           depth      = 1U;
        crd::u32           layers     = 1U;
        VkImageType        image_type = VK_IMAGE_TYPE_2D;
        VkImageViewType    view_type  = VK_IMAGE_VIEW_TYPE_2D;
        VkImageCreateFlags flags      = 0;
        switch (kind)
        {
        case TextureKind::Tex1D:      image_type = VK_IMAGE_TYPE_1D; view_type = VK_IMAGE_VIEW_TYPE_1D; height = 1U; break;
        case TextureKind::Tex2D:      break;
        case TextureKind::Tex3D:      image_type = VK_IMAGE_TYPE_3D; view_type = VK_IMAGE_VIEW_TYPE_3D; depth = depth_or_layers > 0U ? depth_or_layers : 1U; break;
        case TextureKind::Cube:       view_type = VK_IMAGE_VIEW_TYPE_CUBE;       flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT; layers = 6U; break;
        case TextureKind::Tex2DArray: view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;   layers = depth_or_layers > 0U ? depth_or_layers : 1U; break;
        case TextureKind::CubeArray:  view_type = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY; flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT; layers = depth_or_layers > 0U ? depth_or_layers : 6U; break;
        }

        const VkPhysicalDevice pd = m_ctx->vk_physical_device();
        ImageBundle            img{};
        VkImageCreateInfo      ici{};
        ici.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.flags         = flags;
        ici.imageType     = image_type;
        ici.format        = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent        = {width, height, depth};
        ici.mipLevels     = 1U;
        ici.arrayLayers   = layers;
        ici.samples       = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ici.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(m_device, &ici, nullptr, &img.image) != VK_SUCCESS) { return nullptr; }
        VkMemoryRequirements ir{};
        vkGetImageMemoryRequirements(m_device, img.image, &ir);
        VkMemoryAllocateInfo iai{};
        iai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        iai.allocationSize  = ir.size;
        iai.memoryTypeIndex = find_memory_type(pd, ir.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (iai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(m_device, &iai, nullptr, &img.mem) != VK_SUCCESS)
        {
            destroy_image_bundle(m_device, img);
            return nullptr;
        }
        vkBindImageMemory(m_device, img.image, img.mem, 0);
        VkImageViewCreateInfo vci{};
        vci.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image                       = img.image;
        vci.viewType                    = view_type;
        vci.format                      = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount = 1U;
        vci.subresourceRange.layerCount = layers;
        if (vkCreateImageView(m_device, &vci, nullptr, &img.view) != VK_SUCCESS)
        {
            destroy_image_bundle(m_device, img);
            return nullptr;
        }

        const VkDeviceSize bytes   = static_cast<VkDeviceSize>(width) * height * depth * layers * 4U;
        VkBuffer           staging = VK_NULL_HANDLE;
        VkDeviceMemory     smem    = VK_NULL_HANDLE;
        void*              mapped  = nullptr;
        if (!make_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging, smem, &mapped)
            || mapped == nullptr)
        {
            destroy_image_bundle(m_device, img);
            if (staging != VK_NULL_HANDLE) { vkDestroyBuffer(m_device, staging, nullptr); }
            if (smem != VK_NULL_HANDLE) { vkFreeMemory(m_device, smem, nullptr); }
            return nullptr;
        }
        std::memcpy(mapped, rgba, static_cast<size_t>(bytes));

        VkCommandBuffer cmd = begin_cmd();
        if (cmd != VK_NULL_HANDLE)
        {
            transition_layers(cmd, img.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                              VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              layers);
            VkBufferImageCopy region{};
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = layers; // one copy covers every array layer (layer-major buffer)
            region.imageExtent                 = {width, height, depth};
            vkCmdCopyBufferToImage(cmd, staging, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1U, &region);
            transition_layers(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, layers);
            end_and_wait(cmd);
        }
        vkDestroyBuffer(m_device, staging, nullptr);
        vkFreeMemory(m_device, smem, nullptr);
        return std::make_unique<VulkanTexture>(m_device, img, width, height);
    }

    void draw_textured(IRasterTarget& target, IRasterProgram& program, ClearColor clear_color, ITexture& texture,
                       crd::u32 vertex_count) override
    {
        draw_sampled(target, program, clear_color, static_cast<VulkanTexture&>(texture).view(), m_default_sampler,
                     vertex_count);
    }

    [[nodiscard]] std::unique_ptr<ITexture> create_depth_texture(crd::u32 width, crd::u32 height,
                                                                 const float* depth) override
    {
        if (width == 0U || height == 0U || depth == nullptr) { return nullptr; }
        ImageBundle img{};
        // D32_SFLOAT is the format that supports depth-comparison sampling (VK_FORMAT_FEATURE_..._DEPTH_COMPARISON_BIT).
        if (!create_image_bundle(width, height, VK_SAMPLE_COUNT_1_BIT, VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT,
                                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, img))
        {
            return nullptr;
        }
        const VkDeviceSize bytes   = static_cast<VkDeviceSize>(width) * height * 4U; // one float per texel
        VkBuffer           staging = VK_NULL_HANDLE;
        VkDeviceMemory     smem    = VK_NULL_HANDLE;
        void*              mapped  = nullptr;
        if (!make_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging, smem, &mapped)
            || mapped == nullptr)
        {
            destroy_image_bundle(m_device, img);
            if (staging != VK_NULL_HANDLE) { vkDestroyBuffer(m_device, staging, nullptr); }
            if (smem != VK_NULL_HANDLE) { vkFreeMemory(m_device, smem, nullptr); }
            return nullptr;
        }
        std::memcpy(mapped, depth, static_cast<size_t>(bytes));

        VkCommandBuffer cmd = begin_cmd();
        if (cmd != VK_NULL_HANDLE)
        {
            depth_barrier(cmd, img.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                          VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy region{};
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            region.imageSubresource.layerCount = 1U;
            region.imageExtent                 = {width, height, 1U};
            vkCmdCopyBufferToImage(cmd, staging, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1U, &region);
            depth_barrier(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            end_and_wait(cmd);
        }
        vkDestroyBuffer(m_device, staging, nullptr);
        vkFreeMemory(m_device, smem, nullptr);
        return std::make_unique<VulkanTexture>(m_device, img, width, height);
    }

    void draw_shadow(IRasterTarget& target, IRasterProgram& program, ClearColor clear_color, ITexture& depth,
                     crd::u32 vertex_count) override
    {
        draw_sampled(target, program, clear_color, static_cast<VulkanTexture&>(depth).view(), m_cmp_sampler, vertex_count);
    }

    [[nodiscard]] bool supports_bindless() const noexcept override { return m_ctx->bindless(); }

    void draw_bindless(IRasterTarget& target, IRasterProgram& program, ClearColor clear_color,
                       ITexture* const* textures, crd::u32 count, crd::u32 vertex_count) override
    {
        auto& t = static_cast<VulkanRasterTarget&>(target);
        auto& p = static_cast<VulkanRasterProgram&>(program);
        if (!m_api.valid() || !p.valid() || m_desc_pool == VK_NULL_HANDLE || count == 0U || textures == nullptr) { return; }
        const crd::u32 n = count < kBindlessMax ? count : kBindlessMax;

        vkResetDescriptorPool(m_device, m_desc_pool, 0);
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool     = m_desc_pool;
        dsai.descriptorSetCount = 1U;
        dsai.pSetLayouts        = &m_storage_set_layout;
        VkDescriptorSet dset = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(m_device, &dsai, &dset) != VK_SUCCESS) { return; }
        // Fill the whole array (binding 3): elements 0..n-1 are the given textures, the rest replicate element 0 (so every
        // array slot is a valid descriptor — no partially-bound feature needed). Binding 2 = the default sampler.
        VkDescriptorImageInfo imgs[kBindlessMax]{};
        for (crd::u32 i = 0; i < kBindlessMax; ++i)
        {
            auto& tex = static_cast<VulkanTexture&>(*textures[i < n ? i : 0U]);
            imgs[i]   = {VK_NULL_HANDLE, tex.view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        }
        VkDescriptorImageInfo samp_info{m_default_sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        VkWriteDescriptorSet  wr[2]{};
        wr[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[0].dstSet = dset; wr[0].dstBinding = 2U; wr[0].descriptorCount = 1U;            wr[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;       wr[0].pImageInfo = &samp_info;
        wr[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[1].dstSet = dset; wr[1].dstBinding = 3U; wr[1].descriptorCount = kBindlessMax; wr[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; wr[1].pImageInfo = imgs;
        vkUpdateDescriptorSets(m_device, 2U, wr, 0U, nullptr);
        render_dset(t, p, clear_color, dset, vertex_count);
    }

    // B16: draw_bindless INTO A DEPTH TARGET with a depth test — the depth-occluded displaced ocean grid. Same bindless
    // descriptor set (cascade textures, now VS+FS visible); depth attachment + test via render_dset_depth.
    void draw_bindless_depth(IRasterTarget& target, IRasterProgram& program, ClearColor clear_color, float clear_depth,
                             DepthCompare compare, ITexture* const* textures, crd::u32 count, crd::u32 vertex_count) override
    {
        auto& t = static_cast<VulkanRasterTarget&>(target);
        auto& p = static_cast<VulkanRasterProgram&>(program);
        if (!m_api.valid() || !p.valid() || m_desc_pool == VK_NULL_HANDLE || count == 0U || textures == nullptr || !t.has_depth())
        {
            return;
        }
        const crd::u32 n = count < kBindlessMax ? count : kBindlessMax;
        vkResetDescriptorPool(m_device, m_desc_pool, 0);
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool     = m_desc_pool;
        dsai.descriptorSetCount = 1U;
        dsai.pSetLayouts        = &m_storage_set_layout;
        VkDescriptorSet dset = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(m_device, &dsai, &dset) != VK_SUCCESS) { return; }
        VkDescriptorImageInfo imgs[kBindlessMax]{};
        for (crd::u32 i = 0; i < kBindlessMax; ++i)
        {
            auto& tex = static_cast<VulkanTexture&>(*textures[i < n ? i : 0U]);
            imgs[i]   = {VK_NULL_HANDLE, tex.view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        }
        VkDescriptorImageInfo samp_info{m_default_sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        VkWriteDescriptorSet  wr[2]{};
        wr[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[0].dstSet = dset; wr[0].dstBinding = 2U; wr[0].descriptorCount = 1U;            wr[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;       wr[0].pImageInfo = &samp_info;
        wr[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[1].dstSet = dset; wr[1].dstBinding = 3U; wr[1].descriptorCount = kBindlessMax; wr[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; wr[1].pImageInfo = imgs;
        vkUpdateDescriptorSets(m_device, 2U, wr, 0U, nullptr);
        render_dset_depth(t, p, clear_color, clear_depth, compare, dset, vertex_count);
    }

    // B4: like draw_bindless_depth, but the geometry is emitted by a MESH program (create_mesh_program) — `group_count` meshlet
    // workgroups instead of a vertex count. The bindless cascade textures are bound the same way (the mesh shader samples the
    // FFT displacement via SampleIndexedLod). The ocean fast path. No-op if the device lacks mesh shaders.
    void draw_mesh_bindless_depth(IRasterTarget& target, IRasterProgram& program, ClearColor clear_color, float clear_depth,
                                  DepthCompare compare, ITexture* const* textures, crd::u32 count, crd::u32 group_count) override
    {
        auto& t = static_cast<VulkanRasterTarget&>(target);
        auto& p = static_cast<VulkanRasterProgram&>(program);
        if (!m_api.valid() || m_api.draw_mesh_tasks == nullptr || !p.valid() || !p.is_mesh() || m_desc_pool == VK_NULL_HANDLE
            || count == 0U || textures == nullptr || !t.has_depth())
        {
            return;
        }
        const crd::u32 n = count < kBindlessMax ? count : kBindlessMax;
        vkResetDescriptorPool(m_device, m_desc_pool, 0);
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool     = m_desc_pool;
        dsai.descriptorSetCount = 1U;
        dsai.pSetLayouts        = &m_storage_set_layout;
        VkDescriptorSet dset = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(m_device, &dsai, &dset) != VK_SUCCESS) { return; }
        VkDescriptorImageInfo imgs[kBindlessMax]{};
        for (crd::u32 i = 0; i < kBindlessMax; ++i)
        {
            auto& tex = static_cast<VulkanTexture&>(*textures[i < n ? i : 0U]);
            imgs[i]   = {VK_NULL_HANDLE, tex.view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        }
        VkDescriptorImageInfo samp_info{m_default_sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        VkWriteDescriptorSet  wr[2]{};
        wr[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[0].dstSet = dset; wr[0].dstBinding = 2U; wr[0].descriptorCount = 1U;            wr[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;       wr[0].pImageInfo = &samp_info;
        wr[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[1].dstSet = dset; wr[1].dstBinding = 3U; wr[1].descriptorCount = kBindlessMax; wr[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; wr[1].pImageInfo = imgs;
        vkUpdateDescriptorSets(m_device, 2U, wr, 0U, nullptr);
        render_dset_depth(t, p, clear_color, clear_depth, compare, dset, group_count, /*mesh_draw=*/true);
    }

    [[nodiscard]] std::unique_ptr<IGBufferTarget> create_gbuffer_target(crd::u32 width, crd::u32 height,
                                                                        crd::u32 attachments) override
    {
        if (width == 0U || height == 0U || attachments < 2U || attachments > kMaxGBuffer) { return nullptr; }
        ImageBundle    imgs[kMaxGBuffer]{};
        VkBuffer       bufs[kMaxGBuffer]{};
        VkDeviceMemory mems[kMaxGBuffer]{};
        void*          mapped[kMaxGBuffer]{};
        for (crd::u32 i = 0; i < attachments; ++i)
        {
            if (!create_image_bundle(width, height, VK_SAMPLE_COUNT_1_BIT, kColorFormat, VK_IMAGE_ASPECT_COLOR_BIT,
                                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, imgs[i])
                || !create_readback(width, height, bufs[i], mems[i], mapped[i]))
            {
                destroy_image_bundle(m_device, imgs[i]);
                for (crd::u32 j = 0; j < i; ++j) // roll back the attachments already created
                {
                    if (mapped[j] != nullptr) { vkUnmapMemory(m_device, mems[j]); }
                    vkDestroyBuffer(m_device, bufs[j], nullptr);
                    vkFreeMemory(m_device, mems[j], nullptr);
                    destroy_image_bundle(m_device, imgs[j]);
                }
                return nullptr;
            }
        }
        return std::make_unique<VulkanGBufferTarget>(m_device, attachments, imgs, bufs, mems, mapped, width, height);
    }

    void draw_gbuffer(IGBufferTarget& target, IRasterProgram& program, ClearColor clear_color,
                      crd::u32 vertex_count) override
    {
        auto&          t = static_cast<VulkanGBufferTarget&>(target);
        auto&          p = static_cast<VulkanRasterProgram&>(program);
        const crd::u32 n = t.attachment_count();
        if (!m_api.valid() || !p.valid() || n == 0U) { return; }

        VkCommandBuffer cmd = begin_cmd();
        if (cmd == VK_NULL_HANDLE) { return; }
        VkRenderingAttachmentInfo att[kMaxGBuffer]{};
        for (crd::u32 i = 0; i < n; ++i)
        {
            transition(cmd, t.image(i), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                       VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            att[i] = colour_clear_attachment(t.view(i), clear_color);
        }
        VkRenderingInfo ri{};
        ri.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea.extent    = {t.width(), t.height()};
        ri.layerCount           = 1U;
        ri.colorAttachmentCount = n;
        ri.pColorAttachments    = att;
        vkCmdBeginRendering(cmd, &ri);
        set_draw_state(cmd, t.width(), t.height(), 1U, false, VK_COMPARE_OP_ALWAYS, n); // N-attachment blend/write masks
        bind_and_draw(cmd, p, vertex_count);
        vkCmdEndRendering(cmd);
        for (crd::u32 i = 0; i < n; ++i) // copy each attachment to its host-visible readback
        {
            transition(cmd, t.image(i), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy region{};
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1U;
            region.imageExtent                 = {t.width(), t.height(), 1U};
            vkCmdCopyImageToBuffer(cmd, t.image(i), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.readback(i), 1U, &region);
        }
        end_and_wait(cmd);
    }

    // B17-a: WEIGHTED-BLENDED OIT (McGuire-Bavoil 2013). Two internal float targets (accum RGBA16F blended ADDITIVELY,
    // reveal R16F blended MULTIPLICATIVELY) accumulate ALL transparent fragments in ONE order-independent pass; a full-screen
    // composite then resolves them over a `background`-cleared target. Contract: see raster_context.hpp. No-op where the
    // per-attachment blend-equation dynamic state is unavailable.
    void draw_wboit(IRasterTarget& target, IRasterProgram& transparent, IRasterProgram& composite, ClearColor background,
                    crd::u32 vertex_count) override
    {
        auto& t  = static_cast<VulkanRasterTarget&>(target);
        auto& tp = static_cast<VulkanRasterProgram&>(transparent);
        auto& cp = static_cast<VulkanRasterProgram&>(composite);
        if (!m_api.valid() || !tp.valid() || !cp.valid() || m_desc_pool == VK_NULL_HANDLE
            || m_api.set_color_blend_equation == nullptr)
        {
            return; // per-attachment blend equations unavailable ⇒ graceful no-op
        }
        const crd::u32          w      = t.width();
        const crd::u32          h      = t.height();
        const VkImageUsageFlags rt_use = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ImageBundle             accum{};
        ImageBundle             reveal{};
        if (!create_image_bundle(w, h, VK_SAMPLE_COUNT_1_BIT, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT,
                                 rt_use, accum)
            || !create_image_bundle(w, h, VK_SAMPLE_COUNT_1_BIT, VK_FORMAT_R16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, rt_use,
                                    reveal))
        {
            destroy_image_bundle(m_device, accum);
            destroy_image_bundle(m_device, reveal);
            return;
        }
        VkCommandBuffer cmd = begin_cmd();
        if (cmd == VK_NULL_HANDLE)
        {
            destroy_image_bundle(m_device, accum);
            destroy_image_bundle(m_device, reveal);
            return;
        }

        // ---- Pass 1: accumulate (MRT: accum additively, reveal multiplicatively) — draw order irrelevant ------------
        transition(cmd, accum.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        transition(cmd, reveal.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        VkRenderingAttachmentInfo att[2]{};
        att[0] = colour_clear_attachment(accum.view, ClearColor{0.0F, 0.0F, 0.0F, 0.0F});
        att[1] = colour_clear_attachment(reveal.view, ClearColor{1.0F, 0.0F, 0.0F, 0.0F}); // revealage starts at full 1
        VkRenderingInfo ri{};
        ri.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea.extent    = {w, h};
        ri.layerCount           = 1U;
        ri.colorAttachmentCount = 2U;
        ri.pColorAttachments    = att;
        vkCmdBeginRendering(cmd, &ri);
        set_draw_state(cmd, w, h, 1U, false, VK_COMPARE_OP_ALWAYS, 2U);
        const VkBool32 en[2] = {VK_TRUE, VK_TRUE}; // set_draw_state left blend disabled — enable + set both equations
        m_api.set_color_blend_enable(cmd, 0U, 2U, en);
        VkColorBlendEquationEXT eq[2]{};
        eq[0] = {VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD, // accum += weighted premultiplied colour
                 VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD};
        eq[1] = {VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_OP_ADD, // reveal *= (1 - coverage)
                 VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD};
        m_api.set_color_blend_equation(cmd, 0U, 2U, eq);
        bind_and_draw(cmd, tp, vertex_count);
        vkCmdEndRendering(cmd);

        transition(cmd, accum.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        transition(cmd, reveal.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

        // ---- Pass 2: composite `rgb·(1-reveal) + background·reveal` into the RGBA8 target --------------------------
        vkResetDescriptorPool(m_device, m_desc_pool, 0);
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool     = m_desc_pool;
        dsai.descriptorSetCount = 1U;
        dsai.pSetLayouts        = &m_storage_set_layout;
        VkDescriptorSet dset = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(m_device, &dsai, &dset) == VK_SUCCESS)
        {
            VkDescriptorImageInfo imgs[kBindlessMax]{};
            for (crd::u32 i = 0; i < kBindlessMax; ++i) // bindless[0]=accum, [1]=reveal; rest replicate accum (all valid)
            {
                imgs[i] = {VK_NULL_HANDLE, i == 1U ? reveal.view : accum.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            }
            VkDescriptorImageInfo samp_info{m_default_sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
            VkWriteDescriptorSet  wr[2]{};
            wr[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[0].dstSet = dset; wr[0].dstBinding = 2U; wr[0].descriptorCount = 1U;            wr[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;       wr[0].pImageInfo = &samp_info;
            wr[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[1].dstSet = dset; wr[1].dstBinding = 3U; wr[1].descriptorCount = kBindlessMax; wr[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; wr[1].pImageInfo = imgs;
            vkUpdateDescriptorSets(m_device, 2U, wr, 0U, nullptr);

            transition(cmd, t.image(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                       VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            VkRenderingAttachmentInfo catt = colour_clear_attachment(t.view(), background);
            VkRenderingInfo           cri  = one_colour_rendering(t, catt);
            vkCmdBeginRendering(cmd, &cri);
            set_draw_state(cmd, w, h, 1U, false, VK_COMPARE_OP_ALWAYS, 1U);
            const VkBool32 cen[1] = {VK_TRUE};
            m_api.set_color_blend_enable(cmd, 0U, 1U, cen);
            VkColorBlendEquationEXT ceq[1]{};
            ceq[0] = {VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_OP_ADD,
                      VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_OP_ADD};
            m_api.set_color_blend_equation(cmd, 0U, 1U, ceq);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cp.layout(), 0U, 1U, &dset, 0U, nullptr);
            bind_and_draw(cmd, cp, 3U); // full-screen triangle
            vkCmdEndRendering(cmd);
            copy_colour_to_readback(cmd, t);
        }
        end_and_wait(cmd);
        destroy_image_bundle(m_device, accum);
        destroy_image_bundle(m_device, reveal);
    }

private:
    // B2: clear `target`, bind `view` (binding 1) + `sampler` (binding 2) into the material set, draw, copy to readback.
    // Shared by draw_textured (default sampler) and draw_shadow (comparison sampler).
    void draw_sampled(IRasterTarget& target, IRasterProgram& program, ClearColor clear_color, VkImageView view,
                      VkSampler sampler, crd::u32 vertex_count)
    {
        auto& t = static_cast<VulkanRasterTarget&>(target);
        auto& p = static_cast<VulkanRasterProgram&>(program);
        if (!m_api.valid() || !p.valid() || m_desc_pool == VK_NULL_HANDLE || view == VK_NULL_HANDLE) { return; }

        vkResetDescriptorPool(m_device, m_desc_pool, 0);
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool     = m_desc_pool;
        dsai.descriptorSetCount = 1U;
        dsai.pSetLayouts        = &m_storage_set_layout;
        VkDescriptorSet dset = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(m_device, &dsai, &dset) != VK_SUCCESS) { return; }
        VkDescriptorImageInfo img_info{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo samp_info{sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        VkWriteDescriptorSet  wr[2]{};
        wr[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[0].dstSet = dset; wr[0].dstBinding = 1U; wr[0].descriptorCount = 1U; wr[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; wr[0].pImageInfo = &img_info;
        wr[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[1].dstSet = dset; wr[1].dstBinding = 2U; wr[1].descriptorCount = 1U; wr[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER; wr[1].pImageInfo = &samp_info;
        vkUpdateDescriptorSets(m_device, 2U, wr, 0U, nullptr);
        render_dset(t, p, clear_color, dset, vertex_count);
    }

    // B2: the shared render tail — transition the target, begin dynamic rendering, bind `dset`, draw, copy to readback.
    void render_dset(VulkanRasterTarget& t, VulkanRasterProgram& p, ClearColor clear_color, VkDescriptorSet dset,
                     crd::u32 vertex_count)
    {
        VkCommandBuffer cmd = begin_cmd();
        if (cmd == VK_NULL_HANDLE) { return; }
        transition(cmd, t.image(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        VkRenderingAttachmentInfo att = colour_clear_attachment(t.view(), clear_color);
        VkRenderingInfo           ri  = one_colour_rendering(t, att);
        vkCmdBeginRendering(cmd, &ri);
        set_draw_state(cmd, t.width(), t.height(), 1U, false, VK_COMPARE_OP_ALWAYS);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.layout(), 0U, 1U, &dset, 0U, nullptr);
        bind_and_draw(cmd, p, vertex_count);
        vkCmdEndRendering(cmd);
        copy_colour_to_readback(cmd, t);
        end_and_wait(cmd);
    }

    // B16: like render_dset but with a DEPTH attachment + depth test — depth-occluded geometry (the displaced ocean grid), with
    // the material descriptor set bound (cascade textures the VS/FS sample). Target MUST be a create_color_depth_target.
    void render_dset_depth(VulkanRasterTarget& t, VulkanRasterProgram& p, ClearColor clear_color, float clear_depth,
                           DepthCompare compare, VkDescriptorSet dset, crd::u32 vertex_count, bool mesh_draw = false)
    {
        VkCommandBuffer cmd = begin_cmd();
        if (cmd == VK_NULL_HANDLE) { return; }
        transition(cmd, t.image(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        transition_depth(cmd, t.depth_image());
        VkRenderingAttachmentInfo att = colour_clear_attachment(t.view(), clear_color);
        VkRenderingAttachmentInfo dep{};
        dep.sType                         = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        dep.imageView                     = t.depth_view();
        dep.imageLayout                   = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        dep.loadOp                        = VK_ATTACHMENT_LOAD_OP_CLEAR;
        dep.storeOp                       = VK_ATTACHMENT_STORE_OP_DONT_CARE; // depth is never read back
        dep.clearValue.depthStencil.depth = clear_depth;
        VkRenderingInfo ri  = one_colour_rendering(t, att);
        ri.pDepthAttachment = &dep;
        vkCmdBeginRendering(cmd, &ri);
        set_draw_state(cmd, t.width(), t.height(), 1U, true, to_vk_compare(compare), 1U, mesh_draw);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.layout(), 0U, 1U, &dset, 0U, nullptr);
        if (mesh_draw) // B4: bind VERTEX=null (mutually exclusive with mesh) + MESH + FS, then dispatch `vertex_count` meshlets
        {
            const VkShaderStageFlagBits vnull[1] = {VK_SHADER_STAGE_VERTEX_BIT};
            const VkShaderEXT           vnob[1]  = {VK_NULL_HANDLE};
            m_api.bind(cmd, 1U, vnull, vnob);
            if (p.has_task()) // B4: bind the amplification stage for a task→mesh program
            {
                const VkShaderStageFlagBits tstage[1] = {VK_SHADER_STAGE_TASK_BIT_EXT};
                const VkShaderEXT           tobj[1]   = {p.task()};
                m_api.bind(cmd, 1U, tstage, tobj);
            }
            const VkShaderStageFlagBits mstages[2] = {VK_SHADER_STAGE_MESH_BIT_EXT, VK_SHADER_STAGE_FRAGMENT_BIT};
            const VkShaderEXT           mobjs[2]   = {p.vs(), p.fs()};
            m_api.bind(cmd, 2U, mstages, mobjs);
            m_api.draw_mesh_tasks(cmd, vertex_count, 1U, 1U); // vertex_count = TASK workgroups when has_task() (they amplify)
        }
        else { bind_and_draw(cmd, p, vertex_count); }
        vkCmdEndRendering(cmd);
        copy_colour_to_readback(cmd, t);
        end_and_wait(cmd);
    }

    // B2-c: a COLOR-aspect barrier over `layers` array layers (the shared `transition` covers a single layer).
    static void transition_layers(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                                  VkAccessFlags src_access, VkAccessFlags dst_access, VkPipelineStageFlags src_stage,
                                  VkPipelineStageFlags dst_stage, crd::u32 layers)
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
        b.subresourceRange.layerCount = layers;
        b.srcAccessMask               = src_access;
        b.dstAccessMask               = dst_access;
        vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1U, &b);
    }

    // B2-b: a DEPTH-aspect image barrier (the shared `transition` is COLOR-aspect only).
    static void depth_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
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
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        b.subresourceRange.levelCount = 1U;
        b.subresourceRange.layerCount = 1U;
        b.srcAccessMask               = src_access;
        b.dstAccessMask               = dst_access;
        vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1U, &b);
    }

    // Shared draw scaffolding (B1-f factored these out so draw_conservative/draw_storage don't re-copy the boilerplate).
    static VkRenderingAttachmentInfo colour_clear_attachment(VkImageView view, ClearColor c)
    {
        VkRenderingAttachmentInfo att{};
        att.sType                       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        att.imageView                   = view;
        att.imageLayout                 = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.loadOp                      = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp                     = VK_ATTACHMENT_STORE_OP_STORE;
        att.clearValue.color.float32[0] = c.r;
        att.clearValue.color.float32[1] = c.g;
        att.clearValue.color.float32[2] = c.b;
        att.clearValue.color.float32[3] = c.a;
        return att;
    }
    static VkRenderingInfo one_colour_rendering(const VulkanRasterTarget& t, const VkRenderingAttachmentInfo& att)
    {
        VkRenderingInfo ri{};
        ri.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea.extent    = {t.width(), t.height()};
        ri.layerCount           = 1U;
        ri.colorAttachmentCount = 1U;
        ri.pColorAttachments    = &att;
        return ri;
    }
    void bind_and_draw(VkCommandBuffer cmd, const VulkanRasterProgram& p, crd::u32 vertex_count) const
    {
        const VkShaderStageFlagBits stages[2] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
        const VkShaderEXT           objs[2]   = {p.vs(), p.fs()};
        m_api.bind(cmd, 2U, stages, objs);
        vkCmdDraw(cmd, vertex_count, 1U, 0U, 0U);
    }
    void copy_colour_to_readback(VkCommandBuffer cmd, VulkanRasterTarget& t)
    {
        transition(cmd, t.image(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1U;
        region.imageExtent                 = {t.width(), t.height(), 1U};
        vkCmdCopyImageToBuffer(cmd, t.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.readback(), 1U, &region);
    }

    // B1-d: transition the depth image from UNDEFINED to DEPTH_ATTACHMENT_OPTIMAL before rendering (aspect = DEPTH).
    static void transition_depth(VkCommandBuffer cmd, VkImage image)
    {
        VkImageMemoryBarrier b{};
        b.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout                   = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout                   = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        b.srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        b.image                       = image;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        b.subresourceRange.levelCount = 1U;
        b.subresourceRange.layerCount = 1U;
        b.srcAccessMask               = 0;
        b.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, 0, 0,
                             nullptr, 0, nullptr, 1U, &b);
    }

    // Set ALL graphics dynamic state a shader-object draw requires (no pipeline bakes it). Attributeless, no blend.
    // `samples` is the target's sample count (1 = single-sample; >1 = MSAA — the rasterizer runs at that rate, and a
    // `sample`-qualified fragment input then forces per-sample shading via its SPIR-V Sample decoration). B1-d: when
    // `depth_test`, the depth test/write is enabled with `depth_op`; otherwise depth is off (the plain colour path).
    void set_draw_state(VkCommandBuffer cmd, crd::u32 w, crd::u32 h, crd::u32 samples, bool depth_test,
                        VkCompareOp depth_op, crd::u32 color_attachments = 1U, bool mesh_draw = false) const
    {
        VkSampleCountFlagBits sc = VK_SAMPLE_COUNT_1_BIT;
        (void)sample_bit(samples, sc);
        const VkViewport vp{0.0F, 0.0F, static_cast<float>(w), static_cast<float>(h), 0.0F, 1.0F};
        vkCmdSetViewportWithCount(cmd, 1U, &vp);
        const VkRect2D scissor{{0, 0}, {w, h}};
        vkCmdSetScissorWithCount(cmd, 1U, &scissor);
        vkCmdSetRasterizerDiscardEnable(cmd, VK_FALSE);
        vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
        vkCmdSetFrontFace(cmd, VK_FRONT_FACE_COUNTER_CLOCKWISE);
        vkCmdSetDepthTestEnable(cmd, depth_test ? VK_TRUE : VK_FALSE);
        vkCmdSetDepthWriteEnable(cmd, depth_test ? VK_TRUE : VK_FALSE);
        if (depth_test) { vkCmdSetDepthCompareOp(cmd, depth_op); }
        vkCmdSetDepthBiasEnable(cmd, VK_FALSE);
        vkCmdSetStencilTestEnable(cmd, VK_FALSE);
        // B4: the vertex-input / primitive-topology / restart dynamic states are IGNORED when a mesh shader is bound, and setting
        // them alongside a mesh shader faults some drivers (device-lost). Skip them for a mesh draw.
        if (!mesh_draw)
        {
            vkCmdSetPrimitiveTopology(cmd, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
            vkCmdSetPrimitiveRestartEnable(cmd, VK_FALSE);
        }
        m_api.set_polygon_mode(cmd, VK_POLYGON_MODE_FILL);
        m_api.set_rasterization_samples(cmd, sc);
        const VkSampleMask mask = 0xFFFFFFFFU;
        m_api.set_sample_mask(cmd, sc, &mask);
        m_api.set_alpha_to_coverage(cmd, VK_FALSE);
        if (!mesh_draw) { m_api.set_vertex_input(cmd, 0U, nullptr, 0U, nullptr); } // attributeless (N/A for mesh)
        // B5: set blend-off + full write-mask for EVERY colour attachment (1 normally · N for a G-buffer MRT draw).
        const crd::u32              nca_cap = color_attachments > kMaxGBuffer ? kMaxGBuffer : color_attachments;
        const crd::u32              nca     = color_attachments == 0U ? 1U : nca_cap;
        VkBool32                    blend_enable[kMaxGBuffer];
        VkColorComponentFlags       write_mask[kMaxGBuffer];
        const VkColorComponentFlags all_rgba =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        for (crd::u32 i = 0; i < nca; ++i) { blend_enable[i] = VK_FALSE; write_mask[i] = all_rgba; }
        m_api.set_color_blend_enable(cmd, 0U, nca, blend_enable);
        m_api.set_color_write_mask(cmd, 0U, nca, write_mask);
        // B1-e: with VK_KHR_fragment_shading_rate enabled, a shader-object draw MUST set the fragment shading rate — default
        // it to 1x1 (no VRS). draw_vrs re-sets it AFTER this to the requested rate.
        if (m_set_vrs != nullptr)
        {
            const VkExtent2D                        one{1U, 1U};
            const VkFragmentShadingRateCombinerOpKHR keep[2] = {VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR,
                                                                VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR};
            m_set_vrs(cmd, &one, keep);
        }
        // B1-f: with the EDS3 conservative-mode feature enabled, a shader-object draw MUST set the mode — default to DISABLED.
        if (m_set_conservative != nullptr) { m_set_conservative(cmd, VK_CONSERVATIVE_RASTERIZATION_MODE_DISABLED_EXT); }
        // B4: once the meshShader feature is enabled, a plain vkCmdDraw REQUIRES the MESH stage explicitly unbound
        // (VUID-vkCmdDraw-None-08690) so the driver knows this is a vertex draw. A mesh draw binds VERTEX=null + MESH itself.
        if (!mesh_draw && m_ctx->mesh_shader())
        {
            const VkShaderStageFlagBits mnull[1] = {VK_SHADER_STAGE_MESH_BIT_EXT};
            const VkShaderEXT           mnob[1]  = {VK_NULL_HANDLE};
            m_api.bind(cmd, 1U, mnull, mnob);
        }
    }

    // Map a sample count (1/2/4/8) to its Vulkan bit; false for an unsupported value.
    [[nodiscard]] static bool sample_bit(crd::u32 samples, VkSampleCountFlagBits& out) noexcept
    {
        switch (samples)
        {
        case 1U: out = VK_SAMPLE_COUNT_1_BIT; return true;
        case 2U: out = VK_SAMPLE_COUNT_2_BIT; return true;
        case 4U: out = VK_SAMPLE_COUNT_4_BIT; return true;
        case 8U: out = VK_SAMPLE_COUNT_8_BIT; return true;
        default: return false;
        }
    }

    // Create an image + its device-local memory + a view, at `samples` sample count, of `format` with `aspect`
    // (colour attachments use kColorFormat/COLOR; the B1-d depth buffer uses D32_SFLOAT/DEPTH).
    [[nodiscard]] bool create_image_bundle(crd::u32 w, crd::u32 h, VkSampleCountFlagBits samples, VkFormat format,
                                           VkImageAspectFlags aspect, VkImageUsageFlags usage, ImageBundle& out,
                                           crd::u32 mip_levels = 1U) const
    {
        const VkPhysicalDevice pd = m_ctx->vk_physical_device();
        VkImageCreateInfo      ici{};
        ici.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType     = VK_IMAGE_TYPE_2D;
        ici.format        = format;
        ici.extent        = {w, h, 1U};
        ici.mipLevels     = mip_levels;
        ici.arrayLayers   = 1U;
        ici.samples       = samples;
        ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ici.usage         = usage;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(m_device, &ici, nullptr, &out.image) != VK_SUCCESS) { out = {}; return false; }

        VkMemoryRequirements ir{};
        vkGetImageMemoryRequirements(m_device, out.image, &ir);
        VkMemoryAllocateInfo iai{};
        iai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        iai.allocationSize  = ir.size;
        iai.memoryTypeIndex = find_memory_type(pd, ir.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (iai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(m_device, &iai, nullptr, &out.mem) != VK_SUCCESS)
        {
            destroy_image_bundle(m_device, out);
            out = {};
            return false;
        }
        vkBindImageMemory(m_device, out.image, out.mem, 0);

        VkImageViewCreateInfo vci{};
        vci.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image                       = out.image;
        vci.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
        vci.format                      = format;
        vci.subresourceRange.aspectMask = aspect;
        vci.subresourceRange.levelCount = 1U;
        vci.subresourceRange.layerCount = 1U;
        if (vkCreateImageView(m_device, &vci, nullptr, &out.view) != VK_SUCCESS)
        {
            destroy_image_bundle(m_device, out);
            out = {};
            return false;
        }
        return true;
    }

    // Create + map the host-visible readback buffer (single-sample RGBA8, w*h*4 bytes).
    // B1-f: create a buffer + memory of `usage`/`props`; maps it when `mapped != nullptr`. Cleans up fully on failure.
    [[nodiscard]] bool make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props, VkBuffer& buf,
                                   VkDeviceMemory& mem, void** mapped) const
    {
        const VkPhysicalDevice pd = m_ctx->vk_physical_device();
        VkBufferCreateInfo     bci{};
        bci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size        = size;
        bci.usage       = usage;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(m_device, &bci, nullptr, &buf) != VK_SUCCESS) { buf = VK_NULL_HANDLE; return false; }
        VkMemoryRequirements mr{};
        vkGetBufferMemoryRequirements(m_device, buf, &mr);
        VkMemoryAllocateInfo mai{};
        mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize  = mr.size;
        mai.memoryTypeIndex = find_memory_type(pd, mr.memoryTypeBits, props);
        if (mai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(m_device, &mai, nullptr, &mem) != VK_SUCCESS
            || vkBindBufferMemory(m_device, buf, mem, 0) != VK_SUCCESS)
        {
            if (mem != VK_NULL_HANDLE) { vkFreeMemory(m_device, mem, nullptr); }
            vkDestroyBuffer(m_device, buf, nullptr);
            buf = VK_NULL_HANDLE;
            mem = VK_NULL_HANDLE;
            return false;
        }
        if (mapped != nullptr && vkMapMemory(m_device, mem, 0, size, 0, mapped) != VK_SUCCESS) { *mapped = nullptr; }
        return true;
    }

    [[nodiscard]] bool create_readback(crd::u32 w, crd::u32 h, VkBuffer& buf, VkDeviceMemory& mem, void*& mapped) const
    {
        const VkPhysicalDevice pd    = m_ctx->vk_physical_device();
        const VkDeviceSize     bytes = static_cast<VkDeviceSize>(w) * h * 4U;
        VkBufferCreateInfo     bci{};
        bci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size        = bytes;
        bci.usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(m_device, &bci, nullptr, &buf) != VK_SUCCESS)
        {
            buf = VK_NULL_HANDLE;
            return false;
        }
        VkMemoryRequirements br{};
        vkGetBufferMemoryRequirements(m_device, buf, &br);
        VkMemoryAllocateInfo bai{};
        bai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        bai.allocationSize  = br.size;
        bai.memoryTypeIndex = find_memory_type(
            pd, br.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (bai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(m_device, &bai, nullptr, &mem) != VK_SUCCESS
            || vkBindBufferMemory(m_device, buf, mem, 0) != VK_SUCCESS
            || vkMapMemory(m_device, mem, 0, bytes, 0, &mapped) != VK_SUCCESS)
        {
            if (mem != VK_NULL_HANDLE) { vkFreeMemory(m_device, mem, nullptr); }
            vkDestroyBuffer(m_device, buf, nullptr);
            buf    = VK_NULL_HANDLE;
            mem    = VK_NULL_HANDLE;
            mapped = nullptr;
            return false;
        }
        return true;
    }

    // Build a colour target at `samples` sample count (+ a B1-d D32 depth buffer when `with_depth`). samples==1 → a plain
    // single-sample image (read back directly); samples>1 → an MSAA colour image + a single-sample AVERAGE-resolve image.
    [[nodiscard]] std::unique_ptr<IRasterTarget> make_target(crd::u32 width, crd::u32 height, crd::u32 samples,
                                                             bool with_depth, VkFormat color_fmt = kColorFormat)
    {
        if (width == 0U || height == 0U) { return nullptr; }
        VkSampleCountFlagBits sc = VK_SAMPLE_COUNT_1_BIT;
        if (!sample_bit(samples, sc)) { return nullptr; }
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(m_ctx->vk_physical_device(), &props);
        if ((props.limits.framebufferColorSampleCounts & sc) == 0U) { return nullptr; } // count unsupported for colour

        const bool ms = samples > 1U;
        // Colour attachment. Single-sample doubles as the readback source (transfer-src); MSAA is attachment-only (resolved).
        // `color_fmt` is normally RGBA8; the B4-vis-4 visibility buffer passes VK_FORMAT_R32_UINT (read_pixel returns the id).
        ImageBundle color{};
        const VkImageUsageFlags color_usage =
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (ms ? 0U : VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        if (!create_image_bundle(width, height, sc, color_fmt, VK_IMAGE_ASPECT_COLOR_BIT, color_usage, color))
        {
            return nullptr;
        }

        // MSAA: a single-sample resolve image (resolve target + readback source).
        ImageBundle resolve{};
        if (ms
            && !create_image_bundle(width, height, VK_SAMPLE_COUNT_1_BIT, color_fmt, VK_IMAGE_ASPECT_COLOR_BIT,
                                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, resolve))
        {
            destroy_image_bundle(m_device, color);
            return nullptr;
        }

        // B1-d: a matching-sample-count D32 depth buffer (never read back — attachment only).
        ImageBundle depth{};
        if (with_depth
            && !create_image_bundle(width, height, sc, kDepthFormat, VK_IMAGE_ASPECT_DEPTH_BIT,
                                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, depth))
        {
            destroy_image_bundle(m_device, color);
            destroy_image_bundle(m_device, resolve);
            return nullptr;
        }

        VkBuffer       buf    = VK_NULL_HANDLE;
        VkDeviceMemory mem    = VK_NULL_HANDLE;
        void*          mapped = nullptr;
        if (!create_readback(width, height, buf, mem, mapped))
        {
            destroy_image_bundle(m_device, color);
            destroy_image_bundle(m_device, resolve);
            destroy_image_bundle(m_device, depth);
            return nullptr;
        }
        return std::make_unique<VulkanRasterTarget>(m_device, color, resolve, depth, buf, mem, mapped, samples, width,
                                                    height);
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

    VulkanGpuContext*                    m_ctx     = nullptr;
    VkDevice                             m_device  = VK_NULL_HANDLE;
    VkQueue                              m_queue   = VK_NULL_HANDLE;
    VkCommandPool                        m_pool    = VK_NULL_HANDLE;
    ShaderObjectApi                      m_api{};
    PFN_vkCmdSetFragmentShadingRateKHR   m_set_vrs          = nullptr; // B1-e: null unless VRS is enabled
    PFN_vkCmdSetConservativeRasterizationModeEXT m_set_conservative = nullptr; // B1-f: null unless conservative raster on
    PFN_vkCmdSetExtraPrimitiveOverestimationSizeEXT m_set_overest_size = nullptr; // B1-f: companion overestimate dyn-state
    VkDescriptorSetLayout                m_storage_set_layout = VK_NULL_HANDLE; // material set 0: storage(0)+image(1)+sampler(2)
    VkDescriptorPool                     m_desc_pool          = VK_NULL_HANDLE; // pool for draw_storage / draw_textured sets
    VkSampler                            m_default_sampler    = VK_NULL_HANDLE; // B2: the default bilinear/repeat sampler
    VkSampler                            m_cmp_sampler        = VK_NULL_HANDLE; // B2-b: comparison sampler (shadow)
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
