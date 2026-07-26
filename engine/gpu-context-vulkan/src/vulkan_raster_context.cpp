// vulkan_raster_context.cpp — the Vulkan IRasterContext (ADR-0103 / D-008 C1-a). Offscreen RGBA8 targets + a
// dynamic-rendering CLEAR with pixel readback, on a graphics queue from the VulkanGpuContext. Raw Vulkan, no crd-rhi.
// The shader-object DRAW path (bind VS+FS programs + dynamic state + vkCmdDraw) appends in C1-b.

#include <crd/gpu/vulkan_raster_context.hpp>

#include <crd/gpu/frame_graph.hpp>          // REN-1: the frame-graph interface this TU implements
#include <crd/gpu/vulkan_gpu_allocator.hpp> // RET-4 pt 2: the ADR-0085 S6 suballocation core, absorbed

#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstring> // std::memcpy — MSVC provides it transitively, GCC does not

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
    VkDeviceMemory mem   = VK_NULL_HANDLE; // legacy direct allocation; UNUSED when `owner` is set (alloc owns memory)
    VkImageView    view  = VK_NULL_HANDLE;
    // RET-4 pt 2: when suballocated, the bundle carries its allocation + owner (resources must die before the
    // allocator — the standard device-before-resources rule, one level down; the allocator tombstones defensively).
    VulkanGpuAllocator* owner = nullptr;
    GpuAllocation       alloc{};
    // RET-4 pt 5 (S7 image relocation): the creation parameters, retained so a defrag pass can RECREATE the image
    // at a new suballocation (recreate + copy + swap — the bundle is self-describing).
    VkFormat              format     = VK_FORMAT_UNDEFINED;
    crd::u32              width      = 0;
    crd::u32              height     = 0;
    crd::u32              mip_levels = 1;
    VkSampleCountFlagBits samples    = VK_SAMPLE_COUNT_1_BIT;
    VkImageUsageFlags     usage      = 0;
    VkImageAspectFlags    aspect     = 0;
};

inline void destroy_image_bundle(VkDevice d, const ImageBundle& b) noexcept
{
    if (b.view != VK_NULL_HANDLE) { vkDestroyImageView(d, b.view, nullptr); }
    if (b.image != VK_NULL_HANDLE) { vkDestroyImage(d, b.image, nullptr); }
    if (b.owner != nullptr) { b.owner->free(b.alloc); }
    else if (b.mem != VK_NULL_HANDLE) { vkFreeMemory(d, b.mem, nullptr); }
}

// RET-4 pt 4: the BUFFER twin of ImageBundle — a VkBuffer + its POOLED allocation (the S6 suballocator). `mapped`
// is this allocation's bytes (block-base + offset; blocks map ONCE — nobody ever vkUnmapMemory's a pooled buffer).
// The bundle owns its cleanup, so holders and staging sites stay one-liner `destroy_buffer_bundle` calls.
struct BufferBundle
{
    VkBuffer            buffer = VK_NULL_HANDLE;
    void*               mapped = nullptr;
    VulkanGpuAllocator* owner  = nullptr;
    GpuAllocation       alloc{};
};

inline void destroy_buffer_bundle(VkDevice d, const BufferBundle& b) noexcept
{
    if (b.buffer != VK_NULL_HANDLE) { vkDestroyBuffer(d, b.buffer, nullptr); }
    if (b.owner != nullptr) { b.owner->free(b.alloc); }
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
                       const BufferBundle& readback, crd::u32 samples, crd::u32 w, crd::u32 h) noexcept
        : m_device(device), m_color(color), m_resolve(resolve), m_depth(depth), m_readback(readback),
          m_samples(samples), m_w(w), m_h(h)
    {
    }
    ~VulkanRasterTarget() override
    {
        if (m_borrowed) { return; } // REN-2: a frame-graph RTT transient — the ImageNode/slot owns the bundles
        destroy_image_bundle(m_device, m_color);
        destroy_image_bundle(m_device, m_resolve);
        destroy_image_bundle(m_device, m_depth);
        destroy_image_bundle(m_device, m_vrs);
        destroy_buffer_bundle(m_device, m_readback);
    }
    VulkanRasterTarget(const VulkanRasterTarget&)            = delete;
    VulkanRasterTarget& operator=(const VulkanRasterTarget&) = delete;
    VulkanRasterTarget(VulkanRasterTarget&&)                 = delete;
    VulkanRasterTarget& operator=(VulkanRasterTarget&&)      = delete;

    [[nodiscard]] crd::u32 width() const noexcept override { return m_w; }
    [[nodiscard]] crd::u32 height() const noexcept override { return m_h; }
    [[nodiscard]] crd::u32 read_pixel(crd::u32 x, crd::u32 y) const noexcept override
    {
        if (m_readback.mapped == nullptr || x >= m_w || y >= m_h) { return 0U; }
        const auto*      bytes  = static_cast<const crd::u8*>(m_readback.mapped);
        const crd::usize offset = (static_cast<crd::usize>(y) * m_w + x) * 4U;
        crd::u32         px     = 0U;
        for (int i = 0; i < 4; ++i) { px |= static_cast<crd::u32>(bytes[offset + static_cast<crd::usize>(i)]) << (8 * i); }
        return px; // little-endian RGBA8: R low byte
    }

    [[nodiscard]] VkImage     image() const noexcept { return m_color.image; }  // the colour attachment (MSAA if samples>1)
    [[nodiscard]] VkImageView view() const noexcept { return m_color.view; }
    [[nodiscard]] VkBuffer    readback() const noexcept { return m_readback.buffer; }
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
    // REN-2: mark this a BORROWED view over frame-graph-owned bundles (a transient RTT target) — the dtor frees nothing.
    void                      set_borrowed() noexcept { m_borrowed = true; }

private:
    VkDevice     m_device = VK_NULL_HANDLE;
    ImageBundle  m_color{};
    ImageBundle  m_resolve{}; // empty for a single-sample target
    ImageBundle  m_depth{};   // B1-d: empty unless created via create_color_depth_target
    ImageBundle  m_vrs{};     // B1-e: empty unless created via create_color_vrs_target (R8_UINT per-tile rate image)
    BufferBundle m_readback{}; // pooled host-visible readback (RET-4 pt 4)
    crd::u32     m_samples = 1U;
    crd::u32     m_w       = 0;
    crd::u32     m_h       = 0;
    bool         m_borrowed = false; // REN-2: true ⇒ a frame-graph RTT transient view; the dtor frees nothing
};

// ── RET-2 (ADR-0105): the present surface — the swapchain seam of the ONE graphics layer ──────────────────────────────
// A pure SINK over the unchanged draw machinery: the app renders into a normal color target and `present(target)`
// blits it into the acquired backbuffer (the target's post-draw layout is TRANSFER_SRC_OPTIMAL — the readback copy is
// every draw's final op — so the blit needs no source barrier). Fully serialized per frame (acquire → blit submit →
// present → fence wait), matching the context's synchronous submission style; the frame graph takes over pacing later.

#if CRD_OS_WINDOWS
// NOLINTNEXTLINE(readability-identifier-naming) — the Win32 ABI name, declared to keep <windows.h> out of this TU
extern "C" __declspec(dllimport) void* __stdcall GetModuleHandleW(const wchar_t* module_name);
#endif

class VulkanPresentSurface final : public IPresentSurface
{
public:
    VulkanPresentSurface(VulkanGpuContext& ctx, void* native_window, crd::u32 w, crd::u32 h, PresentMode mode) noexcept
        : m_instance(ctx.vk_instance()), m_physical(ctx.vk_physical_device()), m_device(ctx.vk_device()),
          m_queue(ctx.graphics_queue()), m_family(ctx.graphics_family()), m_mode(mode)
    {
        if (native_window != nullptr)
        {
#if CRD_OS_WINDOWS
            // vkCreateWin32SurfaceKHR through the loader — the create-info mirrored locally so this TU stays free of
            // <windows.h> (hinstance = the process module; hwnd = the caller's window)
            struct Win32SurfaceCreateInfo
            {
                // NOLINTNEXTLINE(readability-identifier-naming) — mirrors the Vulkan ABI struct field name exactly
                VkStructureType sType;
                const void*     next;
                VkFlags         flags;
                void*           hinstance;
                void*           hwnd;
            };
            using CreateWin32Fn = VkResult(VKAPI_PTR*)(VkInstance, const Win32SurfaceCreateInfo*,
                                                       const VkAllocationCallbacks*, VkSurfaceKHR*);
            auto create_fn =
                reinterpret_cast<CreateWin32Fn>(vkGetInstanceProcAddr(m_instance, "vkCreateWin32SurfaceKHR"));
            if (create_fn == nullptr) { return; }
            Win32SurfaceCreateInfo sci{};
            sci.sType     = static_cast<VkStructureType>(1000009000); // WIN32_SURFACE_CREATE_INFO_KHR
            sci.hinstance = GetModuleHandleW(nullptr);
            sci.hwnd      = native_window;
            if (create_fn(m_instance, &sci, nullptr, &m_surface) != VK_SUCCESS) { return; }
#else
            return; // real-window surfaces: Windows today; the Linux platform surface lands with the RET linux sweep
#endif
        }
        else // the HEADLESS surface — the full swapchain machinery, no window system (VK_EXT_headless_surface)
        {
            using CreateHeadlessFn = VkResult(VKAPI_PTR*)(VkInstance, const VkHeadlessSurfaceCreateInfoEXT*,
                                                          const VkAllocationCallbacks*, VkSurfaceKHR*);
            auto create_fn =
                reinterpret_cast<CreateHeadlessFn>(vkGetInstanceProcAddr(m_instance, "vkCreateHeadlessSurfaceEXT"));
            if (create_fn == nullptr) { return; }
            VkHeadlessSurfaceCreateInfoEXT sci{};
            sci.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT;
            if (create_fn(m_instance, &sci, nullptr, &m_surface) != VK_SUCCESS) { return; }
        }

        VkBool32 supported = VK_FALSE;
        if (vkGetPhysicalDeviceSurfaceSupportKHR(m_physical, m_family, m_surface, &supported) != VK_SUCCESS
            || supported != VK_TRUE)
        {
            return;
        }

        VkCommandPoolCreateInfo pci{};
        pci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = m_family;
        if (vkCreateCommandPool(m_device, &pci, nullptr, &m_pool) != VK_SUCCESS) { return; }
        VkCommandBufferAllocateInfo cai{};
        cai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool        = m_pool;
        cai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(m_device, &cai, &m_cmd) != VK_SUCCESS) { return; }
        VkSemaphoreCreateInfo sci2{};
        sci2.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkCreateSemaphore(m_device, &sci2, nullptr, &m_sem_acquire) != VK_SUCCESS
            || vkCreateSemaphore(m_device, &sci2, nullptr, &m_sem_present) != VK_SUCCESS
            || vkCreateFence(m_device, &fci, nullptr, &m_fence) != VK_SUCCESS)
        {
            return;
        }
        m_valid = create_swapchain(w, h);
    }

    ~VulkanPresentSurface() override
    {
        if (m_device != VK_NULL_HANDLE) { vkDeviceWaitIdle(m_device); }
        destroy_backbuffer_views();
        if (m_swapchain != VK_NULL_HANDLE) { vkDestroySwapchainKHR(m_device, m_swapchain, nullptr); }
        if (m_sem_acquire != VK_NULL_HANDLE) { vkDestroySemaphore(m_device, m_sem_acquire, nullptr); }
        if (m_sem_present != VK_NULL_HANDLE) { vkDestroySemaphore(m_device, m_sem_present, nullptr); }
        if (m_fence != VK_NULL_HANDLE) { vkDestroyFence(m_device, m_fence, nullptr); }
        if (m_pool != VK_NULL_HANDLE) { vkDestroyCommandPool(m_device, m_pool, nullptr); }
        if (m_surface != VK_NULL_HANDLE) { vkDestroySurfaceKHR(m_instance, m_surface, nullptr); }
    }
    VulkanPresentSurface(const VulkanPresentSurface&)            = delete;
    VulkanPresentSurface& operator=(const VulkanPresentSurface&) = delete;
    VulkanPresentSurface(VulkanPresentSurface&&)                 = delete;
    VulkanPresentSurface& operator=(VulkanPresentSurface&&)      = delete;

    [[nodiscard]] bool     valid() const noexcept override { return m_valid; }
    [[nodiscard]] crd::u32 width() const noexcept override { return m_w; }
    [[nodiscard]] crd::u32 height() const noexcept override { return m_h; }
    [[nodiscard]] crd::u64 frame_count() const noexcept override { return m_frames; }
    [[nodiscard]] crd::u32 image_count() const noexcept { return m_n_images; }   // RET-5: ImGui init needs these
    [[nodiscard]] VkFormat color_format() const noexcept { return m_format; }

    [[nodiscard]] bool present(IRasterTarget& target) override { return present(target, nullptr, nullptr); }

    [[nodiscard]] bool present(IRasterTarget& target, OverlayFn overlay, void* user) override
    {
        if (!m_valid) { return false; }
        auto& t = static_cast<VulkanRasterTarget&>(target);
        if (t.width() != m_w || t.height() != m_h) { return false; }

        crd::u32       idx = 0;
        const VkResult ar =
            vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX, m_sem_acquire, VK_NULL_HANDLE, &idx);
        if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) { return false; } // OUT_OF_DATE ⇒ the caller resizes

        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(m_cmd, &bi) != VK_SUCCESS) { return false; }

        VkImageMemoryBarrier b{};
        b.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        b.image                       = m_images[idx];
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.levelCount = 1U;
        b.subresourceRange.layerCount = 1U;
        b.oldLayout                   = VK_IMAGE_LAYOUT_UNDEFINED; // the backbuffer's prior contents are discardable
        b.newLayout                   = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.dstAccessMask               = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1U, &b);

        VkImageBlit blit{};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.layerCount = 1U;
        blit.srcOffsets[1]             = {static_cast<crd::i32>(m_w), static_cast<crd::i32>(m_h), 1};
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.layerCount = 1U;
        blit.dstOffsets[1]             = {static_cast<crd::i32>(m_w), static_cast<crd::i32>(m_h), 1};
        vkCmdBlitImage(m_cmd, t.src_image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_images[idx],
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1U, &blit, VK_FILTER_NEAREST);

        if (overlay != nullptr) // RET-5: composite the overlay ONTO the blitted backbuffer (dynamic rendering, LOAD)
        {
            // the swapchain image needs a VIEW for rendering — created lazily per image, cached, freed on resize
            if (m_views[idx] == VK_NULL_HANDLE)
            {
                VkImageViewCreateInfo vci{};
                vci.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                vci.image                       = m_images[idx];
                vci.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
                vci.format                      = m_format;
                vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                vci.subresourceRange.levelCount = 1U;
                vci.subresourceRange.layerCount = 1U;
                if (vkCreateImageView(m_device, &vci, nullptr, &m_views[idx]) != VK_SUCCESS) { return false; }
            }
            b.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.newLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            vkCmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1U, &b);
            VkRenderingAttachmentInfo att{};
            att.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            att.imageView   = m_views[idx];
            att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            att.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD; // the blitted scene stays; the overlay composites on top
            att.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfo ri{};
            ri.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
            ri.renderArea.extent    = {m_w, m_h};
            ri.layerCount           = 1U;
            ri.colorAttachmentCount = 1U;
            ri.pColorAttachments    = &att;
            vkCmdBeginRendering(m_cmd, &ri);
            overlay(static_cast<void*>(m_cmd), user); // the backend-aware layer records (ImGui draw data etc.)
            vkCmdEndRendering(m_cmd);
            b.oldLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            b.newLayout     = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            b.dstAccessMask = 0;
            vkCmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1U, &b);
        }
        else
        {
            b.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.newLayout     = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstAccessMask = 0;
            vkCmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1U, &b);
        }
        if (vkEndCommandBuffer(m_cmd) != VK_SUCCESS) { return false; }

        const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo               si{};
        si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount   = 1;
        si.pWaitSemaphores      = &m_sem_acquire;
        si.pWaitDstStageMask    = &wait_stage;
        si.commandBufferCount   = 1;
        si.pCommandBuffers      = &m_cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores    = &m_sem_present;
        if (vkQueueSubmit(m_queue, 1, &si, m_fence) != VK_SUCCESS) { return false; }

        VkPresentInfoKHR pi{};
        pi.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores    = &m_sem_present;
        pi.swapchainCount     = 1;
        pi.pSwapchains        = &m_swapchain;
        pi.pImageIndices      = &idx;
        const VkResult pr     = vkQueuePresentKHR(m_queue, &pi);

        (void)vkWaitForFences(m_device, 1, &m_fence, VK_TRUE, UINT64_MAX); // full per-frame serialization (v1 pacing)
        (void)vkResetFences(m_device, 1, &m_fence);
        if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR) { return false; }
        ++m_frames;
        return true;
    }

    [[nodiscard]] bool resize(crd::u32 width, crd::u32 height) override
    {
        if (m_surface == VK_NULL_HANDLE) { return false; }
        vkDeviceWaitIdle(m_device);
        m_valid = create_swapchain(width, height);
        return m_valid;
    }

private:
    [[nodiscard]] bool create_swapchain(crd::u32 w, crd::u32 h)
    {
        VkSurfaceCapabilitiesKHR caps{};
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physical, m_surface, &caps) != VK_SUCCESS) { return false; }
        // a real window reports its extent; a headless surface reports 0xFFFFFFFF (caller-defined) — clamp the request
        VkExtent2D extent = caps.currentExtent;
        if (extent.width == 0xFFFFFFFFU)
        {
            const auto clamp_extent = [](crd::u32 v, crd::u32 lo, crd::u32 hi) {
                if (v < lo) { return lo; }
                if (v > hi) { return hi; }
                return v;
            };
            extent.width  = clamp_extent(w, caps.minImageExtent.width, caps.maxImageExtent.width);
            extent.height = clamp_extent(h, caps.minImageExtent.height, caps.maxImageExtent.height);
        }

        crd::u32 nfmt = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(m_physical, m_surface, &nfmt, nullptr);
        if (nfmt == 0U || nfmt > 64U) { nfmt = nfmt > 64U ? 64U : nfmt; }
        VkSurfaceFormatKHR fmts[64];
        vkGetPhysicalDeviceSurfaceFormatsKHR(m_physical, m_surface, &nfmt, fmts);
        if (nfmt == 0U) { return false; }
        VkSurfaceFormatKHR chosen = fmts[0];
        for (crd::u32 i = 0; i < nfmt; ++i) // prefer an 8-bit RGBA/BGRA UNORM backbuffer (the blit handles swizzle)
        {
            if (fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM || fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM)
            {
                chosen = fmts[i];
                break;
            }
        }

        VkPresentModeKHR want = VK_PRESENT_MODE_FIFO_KHR; // always available per spec
        if (m_mode == PresentMode::Mailbox) { want = VK_PRESENT_MODE_MAILBOX_KHR; }
        if (m_mode == PresentMode::Immediate) { want = VK_PRESENT_MODE_IMMEDIATE_KHR; }
        if (want != VK_PRESENT_MODE_FIFO_KHR)
        {
            crd::u32         npm = 0;
            VkPresentModeKHR pms[16];
            vkGetPhysicalDeviceSurfacePresentModesKHR(m_physical, m_surface, &npm, nullptr);
            npm = npm > 16U ? 16U : npm;
            vkGetPhysicalDeviceSurfacePresentModesKHR(m_physical, m_surface, &npm, pms);
            bool offered = false;
            for (crd::u32 i = 0; i < npm; ++i) { offered = offered || pms[i] == want; }
            if (!offered) { want = VK_PRESENT_MODE_FIFO_KHR; } // a pacing preference never fails creation
        }

        crd::u32 count = caps.minImageCount + 1U;
        if (caps.maxImageCount > 0U && count > caps.maxImageCount) { count = caps.maxImageCount; }

        VkSwapchainCreateInfoKHR sci{};
        sci.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        sci.surface          = m_surface;
        sci.minImageCount    = count;
        sci.imageFormat      = chosen.format;
        sci.imageColorSpace  = chosen.colorSpace;
        sci.imageExtent      = extent;
        sci.imageArrayLayers = 1;
        sci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        sci.preTransform     = caps.currentTransform;
        sci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        sci.presentMode      = want;
        sci.clipped          = VK_TRUE;
        sci.oldSwapchain     = m_swapchain;
        VkSwapchainKHR next  = VK_NULL_HANDLE;
        if (vkCreateSwapchainKHR(m_device, &sci, nullptr, &next) != VK_SUCCESS) { return false; }
        if (m_swapchain != VK_NULL_HANDLE) { vkDestroySwapchainKHR(m_device, m_swapchain, nullptr); }
        m_swapchain = next;

        destroy_backbuffer_views(); // stale views die with the old swapchain's images (resize/recreate)
        m_n_images = 16U;
        if (vkGetSwapchainImagesKHR(m_device, m_swapchain, &m_n_images, m_images) != VK_SUCCESS) { return false; }
        m_format = chosen.format; // RET-5: overlay rendering + ImGui pipeline creation key on this
        m_w      = extent.width;
        m_h      = extent.height;
        return true;
    }

    void destroy_backbuffer_views() noexcept
    {
        for (crd::u32 i = 0; i < 16U; ++i)
        {
            if (m_views[i] != VK_NULL_HANDLE)
            {
                vkDestroyImageView(m_device, m_views[i], nullptr);
                m_views[i] = VK_NULL_HANDLE;
            }
        }
    }

    VkInstance       m_instance    = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical    = VK_NULL_HANDLE;
    VkDevice         m_device      = VK_NULL_HANDLE;
    VkQueue          m_queue       = VK_NULL_HANDLE;
    crd::u32         m_family      = 0;
    PresentMode      m_mode        = PresentMode::Fifo;
    VkSurfaceKHR     m_surface     = VK_NULL_HANDLE;
    VkSwapchainKHR   m_swapchain   = VK_NULL_HANDLE;
    VkImage          m_images[16]  = {};
    VkImageView      m_views[16]   = {}; // RET-5: lazy per-backbuffer views for overlay rendering
    VkFormat         m_format      = VK_FORMAT_UNDEFINED;
    crd::u32         m_n_images    = 0;
    VkCommandPool    m_pool        = VK_NULL_HANDLE;
    VkCommandBuffer  m_cmd         = VK_NULL_HANDLE;
    VkSemaphore      m_sem_acquire = VK_NULL_HANDLE;
    VkSemaphore      m_sem_present = VK_NULL_HANDLE;
    VkFence          m_fence       = VK_NULL_HANDLE;
    crd::u32         m_w           = 0;
    crd::u32         m_h           = 0;
    crd::u64         m_frames      = 0;
    bool             m_valid       = false;
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
    VulkanStorageBuffer(VkDevice device, const BufferBundle& buf, const BufferBundle& readback,
                        crd::u32 size_bytes) noexcept
        : m_device(device), m_buf(buf), m_readback(readback), m_size(size_bytes)
    {
    }
    ~VulkanStorageBuffer() override
    {
        if (m_registry != nullptr) // RET-4 pt 5: leave the live-storage registry (defrag never sees a dead buffer)
        {
            for (crd::usize i = 0; i < m_registry->size(); ++i)
            {
                if ((*m_registry)[i] == this)
                {
                    (*m_registry)[i] = (*m_registry)[m_registry->size() - 1U];
                    m_registry->pop_back();
                    break;
                }
            }
        }
        destroy_buffer_bundle(m_device, m_buf);
        destroy_buffer_bundle(m_device, m_readback);
    }
    VulkanStorageBuffer(const VulkanStorageBuffer&)            = delete;
    VulkanStorageBuffer& operator=(const VulkanStorageBuffer&) = delete;
    VulkanStorageBuffer(VulkanStorageBuffer&&)                 = delete;
    VulkanStorageBuffer& operator=(VulkanStorageBuffer&&)      = delete;

    [[nodiscard]] crd::u32 size_bytes() const noexcept override { return m_size; }
    [[nodiscard]] crd::u32 read_u32(crd::u32 index) const noexcept override
    {
        if (m_readback.mapped == nullptr || (index + 1U) * 4U > m_size) { return 0U; }
        crd::u32    v     = 0U;
        const auto* bytes = static_cast<const crd::u8*>(m_readback.mapped) + static_cast<crd::usize>(index) * 4U;
        for (int i = 0; i < 4; ++i) { v |= static_cast<crd::u32>(bytes[i]) << (8 * i); }
        return v;
    }
    [[nodiscard]] VkBuffer buf() const noexcept { return m_buf.buffer; }
    [[nodiscard]] VkBuffer readback() const noexcept { return m_readback.buffer; }

    // RET-4 pt 5 (S7 relocation): install the freshly-copied device bundle and destroy the old one. The context's
    // defrag pass calls this AFTER the copy submit completed (idle-gated) — nothing in flight references the old.
    void swap_device_bundle(const BufferBundle& moved) noexcept
    {
        destroy_buffer_bundle(m_device, m_buf);
        m_buf = moved;
    }
    void set_registry(crd::containers::Array<VulkanStorageBuffer*>* registry) noexcept { m_registry = registry; }

private:
    VkDevice                                    m_device = VK_NULL_HANDLE;
    BufferBundle                                m_buf{};
    BufferBundle                                m_readback{};
    crd::u32                                    m_size     = 0;
    crd::containers::Array<VulkanStorageBuffer*>* m_registry = nullptr; // the context's live-storage list (S7 defrag)
};

// B2: a sampled texture — a device-local image+view (owned) sampled through the context's default sampler in draw_textured.
class VulkanTexture final : public ITexture
{
public:
    VulkanTexture(VkDevice device, const ImageBundle& img, crd::u32 w, crd::u32 h) noexcept
        : m_device(device), m_img(img), m_w(w), m_h(h)
    {
    }
    ~VulkanTexture() override
    {
        if (m_borrowed) { return; } // REN-2: a frame-graph sampled transient — the ImageNode/slot owns the bundle
        if (m_registry != nullptr) // RET-4 pt 5: leave the live-texture registry (defrag never sees a dead texture)
        {
            for (crd::usize i = 0; i < m_registry->size(); ++i)
            {
                if ((*m_registry)[i] == this)
                {
                    (*m_registry)[i] = (*m_registry)[m_registry->size() - 1U];
                    m_registry->pop_back();
                    break;
                }
            }
        }
        destroy_image_bundle(m_device, m_img);
    }
    VulkanTexture(const VulkanTexture&)            = delete;
    VulkanTexture& operator=(const VulkanTexture&) = delete;
    VulkanTexture(VulkanTexture&&)                 = delete;
    VulkanTexture& operator=(VulkanTexture&&)      = delete;

    [[nodiscard]] crd::u32           width() const noexcept override { return m_w; }
    [[nodiscard]] crd::u32           height() const noexcept override { return m_h; }
    [[nodiscard]] VkImageView        view() const noexcept { return m_img.view; }
    [[nodiscard]] const ImageBundle& bundle() const noexcept { return m_img; } // RET-4 pt 5: self-describing
    void                             set_borrowed() noexcept { m_borrowed = true; } // REN-2: view-only, dtor frees nothing

    // RET-4 pt 5 (S7 image relocation): install the freshly-copied bundle, destroy the old. Called by the defrag
    // pass AFTER the copy submit completed; views are read per-draw, so nothing stale survives a swap between draws.
    void swap_bundle(const ImageBundle& moved) noexcept
    {
        destroy_image_bundle(m_device, m_img);
        m_img = moved;
    }
    void set_registry(crd::containers::Array<VulkanTexture*>* registry) noexcept { m_registry = registry; }

private:
    VkDevice                               m_device = VK_NULL_HANDLE;
    ImageBundle                            m_img{};
    crd::u32                               m_w = 0;
    crd::u32                               m_h = 0;
    crd::containers::Array<VulkanTexture*>* m_registry = nullptr; // the context's live-texture list (S7 defrag)
    bool                                   m_borrowed = false; // REN-2: view over a frame-graph transient; dtor frees nothing
};

inline constexpr crd::u32 kMaxGBuffer = 8U; // B5: max deferred G-buffer colour attachments

// B5: a deferred G-BUFFER — `n` RGBA8 colour attachments, each with its own host-visible readback buffer. `read_pixel`
// takes an attachment index. The material writes all `n` in one MRT draw (draw_gbuffer).
class VulkanGBufferTarget final : public IGBufferTarget
{
public:
    VulkanGBufferTarget(VkDevice device, crd::u32 n, const ImageBundle* imgs, const BufferBundle* readbacks,
                        crd::u32 w, crd::u32 h) noexcept
        : m_device(device), m_n(n), m_w(w), m_h(h)
    {
        for (crd::u32 i = 0; i < n; ++i)
        {
            m_img[i] = imgs[i];
            m_rb[i]  = readbacks[i];
        }
    }
    ~VulkanGBufferTarget() override
    {
        for (crd::u32 i = 0; i < m_n; ++i)
        {
            destroy_buffer_bundle(m_device, m_rb[i]);
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
        if (attachment >= m_n || m_rb[attachment].mapped == nullptr || x >= m_w || y >= m_h) { return 0U; }
        const auto*      bytes  = static_cast<const crd::u8*>(m_rb[attachment].mapped);
        const crd::usize offset = (static_cast<crd::usize>(y) * m_w + x) * 4U;
        crd::u32         px     = 0U;
        for (int i = 0; i < 4; ++i) { px |= static_cast<crd::u32>(bytes[offset + static_cast<crd::usize>(i)]) << (8 * i); }
        return px;
    }
    [[nodiscard]] VkImageView view(crd::u32 i) const noexcept { return m_img[i].view; }
    [[nodiscard]] VkImage     image(crd::u32 i) const noexcept { return m_img[i].image; }
    [[nodiscard]] VkBuffer    readback(crd::u32 i) const noexcept { return m_rb[i].buffer; }

private:
    VkDevice     m_device = VK_NULL_HANDLE;
    crd::u32     m_n      = 0;
    ImageBundle  m_img[kMaxGBuffer]{};
    BufferBundle m_rb[kMaxGBuffer]{}; // pooled host-visible readbacks (RET-4 pt 4)
    crd::u32     m_w = 0;
    crd::u32     m_h = 0;
};

class VulkanRasterContext final : public IRasterContext
{
public:
    [[nodiscard]] crd::u32 gpu_block_count() const noexcept { return m_gpu_alloc->block_count(); } // RET-4 diagnostic
    crd::u32               gpu_compact() noexcept { return m_gpu_alloc->compact(); }               // RET-4 S7 verb

    VulkanRasterContext(VulkanGpuContext& ctx, VkCommandPool pool, const ShaderObjectApi& api) noexcept
        : m_ctx(&ctx), m_device(ctx.vk_device()), m_queue(ctx.graphics_queue()), m_pool(pool), m_api(api)
    {
        // RET-4 pt 2 (ADR-0085 S6 absorbed): pooled device-memory suballocation for every image bundle
        m_gpu_alloc = std::make_unique<VulkanGpuAllocator>(ctx.vk_physical_device(), m_device);
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
        slb[0].binding = 0U; slb[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; slb[0].descriptorCount = 1U;            slb[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT; // GEO-1: + VERTEX for vertex pulling
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
        BufferBundle buf;
        if (!make_buffer(size_bytes,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                             | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, buf))
        {
            return nullptr;
        }
        BufferBundle rb;
        if (!make_buffer(size_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, rb))
        {
            destroy_buffer_bundle(m_device, buf);
            return nullptr;
        }
        VkCommandBuffer cmd = begin_cmd(); // zero-initialise the SSBO
        if (cmd != VK_NULL_HANDLE)
        {
            vkCmdFillBuffer(cmd, buf.buffer, 0, size_bytes, 0U);
            end_and_wait(cmd);
        }
        auto sb = std::make_unique<VulkanStorageBuffer>(m_device, buf, rb, size_bytes);
        sb->set_registry(&m_live_storage); // RET-4 pt 5: the S7 defrag pass walks the live set
        m_live_storage.push_back(sb.get());
        return sb;
    }

    // RET-4 pt 5: copy the SSBO's CURRENT device contents into its host-visible readback, so `read_u32` reflects
    // them WITHOUT a draw (upload_storage's twin — compute/defrag verification reads ride this).
    [[nodiscard]] bool download_storage(IStorageBuffer& storage) override
    {
        auto&           sb  = static_cast<VulkanStorageBuffer&>(storage);
        VkCommandBuffer cmd = begin_cmd();
        if (cmd == VK_NULL_HANDLE) { return false; }
        VkBufferCopy region{};
        region.size = sb.size_bytes();
        vkCmdCopyBuffer(cmd, sb.buf(), sb.readback(), 1U, &region);
        end_and_wait(cmd); // queue idle + coherent readback ⇒ the bytes are visible to read_u32 on return
        return true;
    }

    // RET-4 pt 5 (ADR-0085 S7 relocation, absorbed): move every live resource's DEVICE allocation to a fresh
    // suballocation (recreate + GPU copy + swap inside the bundle) — the fragmentation-healing primitive. Idle-gated
    // by construction (every submit here is end_and_wait; nothing external is in flight between draws — gpu-context's
    // per-draw descriptor model caches nothing, so between-draw relocation is structurally safe). Returns relocations.
    [[nodiscard]] crd::u32 defragment_resources()
    {
        crd::u32 relocations = 0;
        for (crd::usize i = 0; i < m_live_storage.size(); ++i) // ── storage buffers ───────────────────────────────
        {
            VulkanStorageBuffer* sb = m_live_storage[i];
            BufferBundle         moved;
            if (!make_buffer(sb->size_bytes(),
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                                 | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, moved))
            {
                continue; // no room to relocate this one — the pass degrades gracefully, never destructively
            }
            VkCommandBuffer cmd = begin_cmd();
            if (cmd == VK_NULL_HANDLE)
            {
                destroy_buffer_bundle(m_device, moved);
                continue;
            }
            VkBufferCopy region{};
            region.size = sb->size_bytes();
            vkCmdCopyBuffer(cmd, sb->buf(), moved.buffer, 1U, &region);
            end_and_wait(cmd);             // the copy completed — the old allocation has no readers left
            sb->swap_device_bundle(moved); // destroys the old bundle, installs the relocated one
            ++relocations;
        }
        for (crd::usize i = 0; i < m_live_textures.size(); ++i) // ── sampled textures (self-describing bundles) ────
        {
            VulkanTexture&     tex = *m_live_textures[i];
            const ImageBundle& old = tex.bundle();
            // only SELF-DESCRIBING single-layer 2D bundles relocate (cube/3D/array textures retain no layer info —
            // their relocation lands with the streaming-residency work that needs it); the guard is the format field
            if (old.format == VK_FORMAT_UNDEFINED) { continue; }
            // the copy needs TRANSFER on both sides; a texture without TRANSFER_SRC cannot be read back — skip
            if ((old.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0U) { continue; }
            ImageBundle moved{};
            if (!create_image_bundle(old.width, old.height, old.samples, old.format, old.aspect, old.usage, moved,
                                     old.mip_levels))
            {
                continue;
            }
            VkCommandBuffer cmd = begin_cmd();
            if (cmd == VK_NULL_HANDLE)
            {
                destroy_image_bundle(m_device, moved);
                continue;
            }
            const auto full_barrier = [&](VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags sa,
                                          VkAccessFlags da) {
                VkImageMemoryBarrier b{};
                b.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                b.oldLayout                   = from;
                b.newLayout                   = to;
                b.srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
                b.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
                b.image                       = image;
                b.subresourceRange.aspectMask = old.aspect;
                b.subresourceRange.levelCount = old.mip_levels;
                b.subresourceRange.layerCount = 1U;
                b.srcAccessMask               = sa;
                b.dstAccessMask               = da;
                // ALL_COMMANDS both sides: every access mask is legal, and this single-shot pass is idle-gated —
                // precision buys nothing here (the narrow-stage version tripped dstAccess SHADER_READ under TRANSFER)
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                                     nullptr, 0, nullptr, 1U, &b);
            };
            // post-create textures live in SHADER_READ_ONLY (every upload path ends there)
            full_barrier(old.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            full_barrier(moved.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                         VK_ACCESS_TRANSFER_WRITE_BIT);
            VkImageCopy regions[16] = {};
            crd::u32    mw          = old.width;
            crd::u32    mh          = old.height;
            for (crd::u32 lvl = 0; lvl < old.mip_levels && lvl < 16U; ++lvl)
            {
                regions[lvl].srcSubresource.aspectMask = old.aspect;
                regions[lvl].srcSubresource.mipLevel   = lvl;
                regions[lvl].srcSubresource.layerCount = 1U;
                regions[lvl].dstSubresource            = regions[lvl].srcSubresource;
                regions[lvl].extent                    = {mw, mh, 1U};
                mw                                     = mw > 1U ? mw / 2U : 1U;
                mh                                     = mh > 1U ? mh / 2U : 1U;
            }
            vkCmdCopyImage(cmd, old.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, moved.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, old.mip_levels, regions);
            full_barrier(moved.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
            end_and_wait(cmd);     // the copy completed — the old image has no readers left
            tex.swap_bundle(moved); // destroys the old bundle, installs the relocated one (views read per-draw)
            ++relocations;
        }
        return relocations;
    }

    // GEO-1: staged CPU→SSBO upload (vertex pulling — a cooked vertex stream the VS fetches by VertexIndex). A transient
    // host-visible staging buffer + vkCmdCopyBuffer; end_and_wait (queue idle) makes the bytes visible before any draw.
    [[nodiscard]] bool upload_storage(IStorageBuffer& storage, crd::u32 byte_offset, const void* data,
                                      crd::u32 size_bytes) override
    {
        auto& sb = static_cast<VulkanStorageBuffer&>(storage);
        if (data == nullptr || size_bytes == 0U) { return false; }
        if (static_cast<crd::u64>(byte_offset) + size_bytes > sb.size_bytes()) { return false; }

        BufferBundle stg;
        if (!make_buffer(size_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stg)
            || stg.mapped == nullptr)
        {
            destroy_buffer_bundle(m_device, stg);
            return false;
        }
        std::memcpy(stg.mapped, data, size_bytes);

        bool            ok  = false;
        VkCommandBuffer cmd = begin_cmd();
        if (cmd != VK_NULL_HANDLE)
        {
            VkBufferCopy region{};
            region.srcOffset = 0;
            region.dstOffset = byte_offset;
            region.size      = size_bytes;
            vkCmdCopyBuffer(cmd, stg.buffer, sb.buf(), 1U, &region);
            end_and_wait(cmd); // queue idle ⇒ transfer complete + visible before any subsequent draw submission
            ok = true;
        }
        destroy_buffer_bundle(m_device, stg);
        return ok;
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

        // REN-2: in frame-graph recording mode, a draw_storage into an RTT transient records color-only (no readback).
        if (frame_recording()) { record_offscreen(t, p, s, clear_color, vertex_count); return; }

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

    // GEO-7: the scene-geometry draw — draw_storage's descriptor seam + draw_depth's clear-colour+depth pass (depth
    // write ON). No per-draw SSBO readback (scene buffers are consumed by the GPU, downloaded on demand via
    // download_storage); the colour readback keeps the RET-2 post-draw TRANSFER_SRC contract overlays rely on.
    void draw_storage_depth(IRasterTarget& target, IRasterProgram& program, ClearColor clear_color, float clear_depth,
                            DepthCompare compare, IStorageBuffer& storage, crd::u32 vertex_count) override
    {
        auto& t = static_cast<VulkanRasterTarget&>(target);
        auto& p = static_cast<VulkanRasterProgram&>(program);
        auto& s = static_cast<VulkanStorageBuffer&>(storage);
        if (!m_api.valid() || !p.valid() || m_desc_pool == VK_NULL_HANDLE || !t.has_depth()) { return; }

        if (frame_recording()) { record_scene(t, p, s, true, clear_color, clear_depth, compare, vertex_count); return; }

        // the storage descriptor at set 0 / binding 0 (the draw_storage seam — VERTEX+FRAGMENT visible)
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
        transition_depth(cmd, t.depth_image());

        VkRenderingAttachmentInfo att = colour_clear_attachment(t.view(), clear_color);

        VkRenderingAttachmentInfo dep{};
        dep.sType                         = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        dep.imageView                     = t.depth_view();
        dep.imageLayout                   = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        dep.loadOp                        = VK_ATTACHMENT_LOAD_OP_CLEAR;
        dep.storeOp                       = VK_ATTACHMENT_STORE_OP_STORE; // overlays test against the scene's depth
        dep.clearValue.depthStencil.depth = clear_depth;

        VkRenderingInfo ri{};
        ri.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea.extent    = {t.width(), t.height()};
        ri.layerCount           = 1U;
        ri.colorAttachmentCount = 1U;
        ri.pColorAttachments    = &att;
        ri.pDepthAttachment     = &dep;
        vkCmdBeginRendering(cmd, &ri);

        set_draw_state(cmd, t.width(), t.height(), 1U, true, to_vk_compare(compare));
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.layout(), 0U, 1U, &dset, 0U, nullptr);
        bind_and_draw(cmd, p, vertex_count);
        vkCmdEndRendering(cmd);

        copy_colour_to_readback(cmd, t);
        end_and_wait(cmd);
    }

    // GEO-8: the CONTINUING scene draw — draw_storage_depth minus the clear: colour LOADs from the previous scene
    // draw (whose post-draw layout is TRANSFER_SRC — the readback contract), depth LOADs and keeps WRITING (the
    // depth image stays DEPTH_ATTACHMENT_OPTIMAL between draws; submits are serialized by end_and_wait).
    void draw_storage_depth_load(IRasterTarget& target, IRasterProgram& program, DepthCompare compare,
                                 IStorageBuffer& storage, crd::u32 vertex_count) override
    {
        auto& t = static_cast<VulkanRasterTarget&>(target);
        auto& p = static_cast<VulkanRasterProgram&>(program);
        auto& s = static_cast<VulkanStorageBuffer&>(storage);
        if (!m_api.valid() || !p.valid() || m_desc_pool == VK_NULL_HANDLE || !t.has_depth()) { return; }

        if (frame_recording()) { record_scene(t, p, s, false, ClearColor{}, 0.0F, compare, vertex_count); return; }

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

        // preserve the previous scene draw's colour: TRANSFER_SRC (post-readback) → COLOR_ATTACHMENT
        transition(cmd, t.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

        VkRenderingAttachmentInfo att{};
        att.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        att.imageView   = t.view();
        att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;
        att.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

        VkRenderingAttachmentInfo dep{};
        dep.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        dep.imageView   = t.depth_view();
        dep.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        dep.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;   // the frame's depth so far
        dep.storeOp     = VK_ATTACHMENT_STORE_OP_STORE; // and this group's writes join it

        VkRenderingInfo ri{};
        ri.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea.extent    = {t.width(), t.height()};
        ri.layerCount           = 1U;
        ri.colorAttachmentCount = 1U;
        ri.pColorAttachments    = &att;
        ri.pDepthAttachment     = &dep;
        vkCmdBeginRendering(cmd, &ri);

        set_draw_state(cmd, t.width(), t.height(), 1U, true, to_vk_compare(compare));
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.layout(), 0U, 1U, &dset, 0U, nullptr);
        bind_and_draw(cmd, p, vertex_count);
        vkCmdEndRendering(cmd);

        copy_colour_to_readback(cmd, t);
        end_and_wait(cmd);
    }

    // REN-3.1: the DEPTH-ONLY (shadow) pass — render storage-pulled geometry writing ONLY depth into `target`'s
    // depth attachment, no colour attachment bound. For a frame-graph `D32Float`+`sampled` transient this PRODUCES a
    // shadow map on the device, which a later pass samples through the comparison sampler. Frame-graph recording is
    // the intended path; the standalone form exists so the capability is testable without a graph.
    void draw_storage_depth_only(IRasterTarget& target, IRasterProgram& program, float clear_depth,
                                 DepthCompare compare, IStorageBuffer& storage, crd::u32 vertex_count) override
    {
        auto& t = static_cast<VulkanRasterTarget&>(target);
        auto& p = static_cast<VulkanRasterProgram&>(program);
        auto& s = static_cast<VulkanStorageBuffer&>(storage);
        if (!m_api.valid() || !p.valid() || m_desc_pool == VK_NULL_HANDLE || !t.has_depth()) { return; }
        if (frame_recording()) { record_depth_only(t, p, s, true, clear_depth, compare, vertex_count); return; }

        // Standalone: own descriptor pool + command buffer + layout transition (mirrors draw_storage_depth, minus
        // every colour-attachment step).
        vkResetDescriptorPool(m_device, m_desc_pool, 0);
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool     = m_desc_pool;
        dsai.descriptorSetCount = 1U;
        dsai.pSetLayouts        = &m_storage_set_layout;
        VkDescriptorSet dset    = VK_NULL_HANDLE;
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
        transition_depth(cmd, t.depth_image());

        VkRenderingAttachmentInfo dep{};
        dep.sType                         = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        dep.imageView                     = t.depth_view();
        dep.imageLayout                   = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        dep.loadOp                        = VK_ATTACHMENT_LOAD_OP_CLEAR;
        dep.storeOp                       = VK_ATTACHMENT_STORE_OP_STORE;
        dep.clearValue.depthStencil.depth = clear_depth;

        VkRenderingInfo ri{};
        ri.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea.extent    = {t.width(), t.height()};
        ri.layerCount           = 1U;
        ri.colorAttachmentCount = 0U; // no colour attachment at all
        ri.pColorAttachments    = nullptr;
        ri.pDepthAttachment     = &dep;
        vkCmdBeginRendering(cmd, &ri);
        set_draw_state(cmd, t.width(), t.height(), 1U, true, to_vk_compare(compare), 0U);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.layout(), 0U, 1U, &dset, 0U, nullptr);
        bind_and_draw(cmd, p, vertex_count);
        vkCmdEndRendering(cmd);
        end_and_wait(cmd);
    }

    // REN-3.1: the CONTINUING depth-only draw — mesh N>0 of a shadow pass joins the SAME depth map (no clear).
    void draw_storage_depth_only_load(IRasterTarget& target, IRasterProgram& program, DepthCompare compare,
                                      IStorageBuffer& storage, crd::u32 vertex_count) override
    {
        auto& t = static_cast<VulkanRasterTarget&>(target);
        auto& p = static_cast<VulkanRasterProgram&>(program);
        auto& s = static_cast<VulkanStorageBuffer&>(storage);
        if (!m_api.valid() || !p.valid() || m_desc_pool == VK_NULL_HANDLE || !t.has_depth()) { return; }
        if (frame_recording()) { record_depth_only(t, p, s, false, 0.0F, compare, vertex_count); return; }
        // standalone continuation is not meaningful (each standalone draw owns its own submit); the frame-graph
        // recording path is the one a shadow pass uses.
    }

    // REN-2 Half B: the TEXTURED scene draw — draw_storage_depth + a sampled material (base-color) texture. Records
    // into the frame graph (record_scene_textured); standalone, allocates a storage(0)+sampled(1)+sampler(2) set and
    // renders through the depth tail.
    void draw_storage_textured_depth(IRasterTarget& target, IRasterProgram& program, ClearColor clear_color,
                                     float clear_depth, DepthCompare compare, IStorageBuffer& storage, ITexture& texture,
                                     crd::u32 vertex_count) override
    {
        auto& t   = static_cast<VulkanRasterTarget&>(target);
        auto& p   = static_cast<VulkanRasterProgram&>(program);
        auto& s   = static_cast<VulkanStorageBuffer&>(storage);
        auto& tex = static_cast<VulkanTexture&>(texture);
        if (!m_api.valid() || !p.valid() || m_desc_pool == VK_NULL_HANDLE || !t.has_depth()) { return; }
        if (frame_recording())
        {
            record_scene_textured(t, p, s, tex, true, clear_color, clear_depth, compare, vertex_count);
            return;
        }
        vkResetDescriptorPool(m_device, m_desc_pool, 0);
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool     = m_desc_pool;
        dsai.descriptorSetCount = 1U;
        dsai.pSetLayouts        = &m_storage_set_layout;
        VkDescriptorSet dset = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(m_device, &dsai, &dset) != VK_SUCCESS) { return; }
        write_scene_textured(m_device, dset, s.buf(), tex.view(), m_default_sampler);
        render_dset_depth(t, p, clear_color, clear_depth, compare, dset, vertex_count);
    }
    void draw_storage_textured_depth_load(IRasterTarget& target, IRasterProgram& program, DepthCompare compare,
                                          IStorageBuffer& storage, ITexture& texture, crd::u32 vertex_count) override
    {
        auto& t   = static_cast<VulkanRasterTarget&>(target);
        auto& p   = static_cast<VulkanRasterProgram&>(program);
        auto& s   = static_cast<VulkanStorageBuffer&>(storage);
        auto& tex = static_cast<VulkanTexture&>(texture);
        if (!m_api.valid() || !p.valid() || !t.has_depth()) { return; }
        if (frame_recording())
        {
            record_scene_textured(t, p, s, tex, false, ClearColor{}, 0.0F, compare, vertex_count);
            return;
        }
        // standalone LOAD is used only through the frame graph in practice (the SceneRenderer path); a direct call
        // clears (last-drawn wins) rather than silently dropping the texture.
        draw_storage_textured_depth(target, program, ClearColor{}, 0.0F, compare, storage, texture, vertex_count);
    }

    // ── REN-3.2-b: the SHADOWED scene draw — identical to the textured one but binding the COMPARISON sampler.
    void draw_storage_shadowed_depth(IRasterTarget& target, IRasterProgram& program, ClearColor clear_color,
                                     float clear_depth, DepthCompare compare, IStorageBuffer& storage,
                                     ITexture& shadow_atlas, crd::u32 vertex_count) override
    {
        auto& t   = static_cast<VulkanRasterTarget&>(target);
        auto& p   = static_cast<VulkanRasterProgram&>(program);
        auto& s   = static_cast<VulkanStorageBuffer&>(storage);
        auto& tex = static_cast<VulkanTexture&>(shadow_atlas);
        if (!m_api.valid() || !p.valid() || m_desc_pool == VK_NULL_HANDLE || !t.has_depth()) { return; }
        if (frame_recording())
        {
            record_scene_textured(t, p, s, tex, true, clear_color, clear_depth, compare, vertex_count, m_cmp_sampler);
            return;
        }
        vkResetDescriptorPool(m_device, m_desc_pool, 0);
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool     = m_desc_pool;
        dsai.descriptorSetCount = 1U;
        dsai.pSetLayouts        = &m_storage_set_layout;
        VkDescriptorSet dset = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(m_device, &dsai, &dset) != VK_SUCCESS) { return; }
        write_scene_textured(m_device, dset, s.buf(), tex.view(), m_cmp_sampler);
        render_dset_depth(t, p, clear_color, clear_depth, compare, dset, vertex_count);
    }
    void draw_storage_shadowed_depth_load(IRasterTarget& target, IRasterProgram& program, DepthCompare compare,
                                          IStorageBuffer& storage, ITexture& shadow_atlas,
                                          crd::u32 vertex_count) override
    {
        auto& t   = static_cast<VulkanRasterTarget&>(target);
        auto& p   = static_cast<VulkanRasterProgram&>(program);
        auto& s   = static_cast<VulkanStorageBuffer&>(storage);
        auto& tex = static_cast<VulkanTexture&>(shadow_atlas);
        if (!m_api.valid() || !p.valid() || !t.has_depth()) { return; }
        if (frame_recording())
        {
            record_scene_textured(t, p, s, tex, false, ClearColor{}, 0.0F, compare, vertex_count, m_cmp_sampler);
            return;
        }
        draw_storage_shadowed_depth(target, program, ClearColor{}, 0.0F, compare, storage, shadow_atlas,
                                    vertex_count);
    }

    // RET-6 (ADR-0105): the OVERLAY draw — see IRasterContext::draw_overlay. Composites onto the target's EXISTING
    // contents (color loadOp=LOAD; the target's post-draw layout is TRANSFER_SRC — the RET-2 contract — so the
    // preserving transition is TRANSFER_SRC → COLOR_ATTACHMENT, never UNDEFINED, which would discard). Standard alpha
    // blending over set_draw_state's blend-off baseline; a READ-ONLY depth test when the target carries depth (write
    // explicitly disabled — the overlay never modifies the scene's depth, so chained overlay draws all test against
    // the same scene). Single-sample targets only (the overlay canvas contract).
    [[nodiscard]] bool draw_overlay(IRasterTarget& target, IRasterProgram& program, IStorageBuffer& storage,
                                    DepthCompare compare, crd::u32 vertex_count) override
    {
        auto& t = static_cast<VulkanRasterTarget&>(target);
        auto& p = static_cast<VulkanRasterProgram&>(program);
        auto& s = static_cast<VulkanStorageBuffer&>(storage);
        if (!m_api.valid() || !p.valid() || m_desc_pool == VK_NULL_HANDLE || t.samples() != 1U || vertex_count == 0U)
        {
            return false;
        }

        if (frame_recording()) { return record_overlay(t, p, s, compare, vertex_count); }

        // the storage descriptor at set 0 / binding 0 (the draw_storage seam — VERTEX+FRAGMENT visible)
        vkResetDescriptorPool(m_device, m_desc_pool, 0);
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool     = m_desc_pool;
        dsai.descriptorSetCount = 1U;
        dsai.pSetLayouts        = &m_storage_set_layout;
        VkDescriptorSet dset = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(m_device, &dsai, &dset) != VK_SUCCESS) { return false; }
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
        if (cmd == VK_NULL_HANDLE) { return false; }

        // preserve the existing contents: post-draw the colour sits in TRANSFER_SRC (readback copied from it)
        transition(cmd, t.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

        VkRenderingAttachmentInfo att{};
        att.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        att.imageView   = t.view();
        att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD; // the scene stays — the overlay composites on top
        att.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

        const bool                depth_on = t.has_depth() && compare != DepthCompare::Always;
        VkRenderingAttachmentInfo dep{};
        if (depth_on)
        {
            dep.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            dep.imageView   = t.depth_view();
            dep.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            dep.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;  // the scene's depth is the test source
            dep.storeOp     = VK_ATTACHMENT_STORE_OP_STORE; // preserved — chained overlay draws re-test against it
        }

        VkRenderingInfo ri{};
        ri.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea.extent    = {t.width(), t.height()};
        ri.layerCount           = 1U;
        ri.colorAttachmentCount = 1U;
        ri.pColorAttachments    = &att;
        ri.pDepthAttachment     = depth_on ? &dep : nullptr;
        vkCmdBeginRendering(cmd, &ri);

        set_draw_state(cmd, t.width(), t.height(), 1U, depth_on, to_vk_compare(compare));
        vkCmdSetDepthWriteEnable(cmd, VK_FALSE); // the overlay READS the scene's depth, never writes it
        const VkBool32 blend_on[1] = {VK_TRUE};  // standard alpha over set_draw_state's blend-off baseline
        m_api.set_color_blend_enable(cmd, 0U, 1U, blend_on);
        VkColorBlendEquationEXT eq[1]{};
        eq[0] = {VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD,
                 VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD};
        m_api.set_color_blend_equation(cmd, 0U, 1U, eq);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.layout(), 0U, 1U, &dset, 0U, nullptr);
        bind_and_draw(cmd, p, vertex_count);
        vkCmdEndRendering(cmd);

        copy_colour_to_readback(cmd, t); // read_pixel stays valid + the layout returns to TRANSFER_SRC for chaining
        end_and_wait(cmd);
        return true;
    }

    // ── REN-1: frame-graph RECORDING-MODE bodies (called by the branched public draws while a graph executes) ─────

    // Allocate + write a storage-buffer descriptor set from the FRAME pool (reset once/frame by the graph — no
    // per-draw reset, so N draws coexist in one command buffer).
    [[nodiscard]] VkDescriptorSet frame_alloc_storage_set(VulkanStorageBuffer& s)
    {
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool     = m_frame_rec.pool;
        dsai.descriptorSetCount = 1U;
        dsai.pSetLayouts        = &m_storage_set_layout;
        VkDescriptorSet dset = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(m_device, &dsai, &dset) != VK_SUCCESS) { return VK_NULL_HANDLE; }
        VkDescriptorBufferInfo dbi{s.buf(), 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet   wr{};
        wr.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr.dstSet          = dset;
        wr.dstBinding      = 0U;
        wr.descriptorCount = 1U;
        wr.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        wr.pBufferInfo     = &dbi;
        vkUpdateDescriptorSets(m_device, 1U, &wr, 0U, nullptr);
        return dset;
    }

    // Before drawing to `t` AGAIN within the current pass, order this draw after the prior group's colour+depth
    // writes (a self-barrier — the intra-pass analog of the graph's cross-pass barriers). The FIRST draw of a
    // pass skips it (the graph already transitioned the target into COLOR/DEPTH_ATTACHMENT before the pass).
    // ⛔ A DEPTH-ONLY target has NO colour image, so `t.image()` is VK_NULL_HANDLE — and `pass_last` also starts
    // as VK_NULL_HANDLE, which made `pass_last == t.image()` fire on the FIRST draw of every depth-only pass and
    // emit a barrier with a null image (VUID-VkImageMemoryBarrier-image-parameter). Latent since REN-3.1; the
    // REN-3.2-b cascade passes are the first code to record MULTIPLE draws into a depth-only target, which is
    // what surfaced it. Identity is now the target's PRIMARY image — colour when it has one, depth otherwise —
    // so depth-only targets still get their between-draw self-barrier, keyed on something real.
    [[nodiscard]] static VkImage primary_image(VulkanRasterTarget& t) noexcept
    {
        return t.image() != VK_NULL_HANDLE ? t.image() : t.depth_image();
    }
    void frame_self_barrier_if_needed(VulkanRasterTarget& t)
    {
        const VkImage id = primary_image(t);
        if (id != VK_NULL_HANDLE && m_frame_rec.pass_last == id)
        {
            if (t.image() != VK_NULL_HANDLE)
            {
                transition(m_frame_rec.cmd, t.image(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            }
            if (t.has_depth())
            {
                VkImageMemoryBarrier db{};
                db.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                db.oldLayout                   = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
                db.newLayout                   = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
                db.srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
                db.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
                db.image                       = t.depth_image();
                db.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                db.subresourceRange.levelCount = 1U;
                db.subresourceRange.layerCount = 1U;
                db.srcAccessMask               = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                db.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                vkCmdPipelineBarrier(m_frame_rec.cmd,
                                     VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                                     VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                                     0, 0, nullptr, 0, nullptr, 1U, &db);
            }
        }
        m_frame_rec.pass_last = primary_image(t); // colour when present, else depth (depth-only passes)
    }

    // The frame-mode body of draw_storage_depth (clear=true) / draw_storage_depth_load (clear=false): one
    // begin/end-rendering block into the shared cmd, no per-draw transition/readback.
    void record_scene(VulkanRasterTarget& t, VulkanRasterProgram& p, VulkanStorageBuffer& s, bool clear,
                      ClearColor clear_color, float clear_depth, DepthCompare compare, crd::u32 vertex_count)
    {
        VkCommandBuffer cmd  = m_frame_rec.cmd;
        VkDescriptorSet dset = frame_alloc_storage_set(s);
        if (dset == VK_NULL_HANDLE) { return; }
        frame_self_barrier_if_needed(t);

        VkRenderingAttachmentInfo att{};
        att.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        att.imageView   = t.view();
        att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.loadOp      = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        att.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
        if (clear) { att.clearValue.color = {{clear_color.r, clear_color.g, clear_color.b, clear_color.a}}; }

        VkRenderingAttachmentInfo dep{};
        dep.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        dep.imageView   = t.depth_view();
        dep.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        dep.loadOp      = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        dep.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
        if (clear) { dep.clearValue.depthStencil.depth = clear_depth; }

        VkRenderingInfo ri{};
        ri.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea.extent    = {t.width(), t.height()};
        ri.layerCount           = 1U;
        ri.colorAttachmentCount = 1U;
        ri.pColorAttachments    = &att;
        ri.pDepthAttachment     = &dep;
        vkCmdBeginRendering(cmd, &ri);
        set_draw_state(cmd, t.width(), t.height(), 1U, true, to_vk_compare(compare));
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.layout(), 0U, 1U, &dset, 0U, nullptr);
        bind_and_draw(cmd, p, vertex_count);
        vkCmdEndRendering(cmd);
    }

    // REN-2: allocate a frame descriptor set binding a SAMPLED image (binding 1) + sampler (binding 2) — the
    // frame-pool analog of draw_sampled's set. Used by record_textured (RTT sample pass / material forward pass).
    [[nodiscard]] VkDescriptorSet frame_alloc_sampled_set(VkImageView view, VkSampler sampler)
    {
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool     = m_frame_rec.pool;
        dsai.descriptorSetCount = 1U;
        dsai.pSetLayouts        = &m_storage_set_layout;
        VkDescriptorSet dset = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(m_device, &dsai, &dset) != VK_SUCCESS) { return VK_NULL_HANDLE; }
        VkDescriptorImageInfo img_info{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo samp_info{sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        VkWriteDescriptorSet  wr[2]{};
        wr[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[0].dstSet = dset; wr[0].dstBinding = 1U; wr[0].descriptorCount = 1U; wr[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; wr[0].pImageInfo = &img_info;
        wr[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[1].dstSet = dset; wr[1].dstBinding = 2U; wr[1].descriptorCount = 1U; wr[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;       wr[1].pImageInfo = &samp_info;
        vkUpdateDescriptorSets(m_device, 2U, wr, 0U, nullptr);
        return dset;
    }

    // ── REN-38-A1a: the FRAME-MODE bindless set + draw. ────────────────────────────────────────────────────
    // ⛔⛔ WHY `draw_bindless` COULD NOT BE RECORDED. Its synchronous body starts with `vkResetDescriptorPool`
    // on the GLOBAL pool — which is correct for a self-contained submit-and-wait draw and CATASTROPHIC inside a
    // frame: it invalidates every descriptor set already bound by every earlier pass in the same command buffer.
    // That single line is why the verb segfaulted when an authored pass called it, and it is the shape shared by
    // most of the 14 verbs the REN-38-A0 audit found unrecordable — they own the pool, so they cannot be guests
    // in someone else's frame.
    //
    // The frame-mode body allocates from the FRAME's pool instead (which `execute()` resets exactly once per
    // frame, per slot) and touches nothing global.
    [[nodiscard]] VkDescriptorSet frame_alloc_bindless_set(ITexture* const* textures, crd::u32 n)
    {
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool     = m_frame_rec.pool;
        dsai.descriptorSetCount = 1U;
        dsai.pSetLayouts        = &m_storage_set_layout;
        VkDescriptorSet dset = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(m_device, &dsai, &dset) != VK_SUCCESS) { return VK_NULL_HANDLE; }
        // Elements 0..n-1 are the given textures; the rest REPLICATE element 0 so every array slot is a valid
        // descriptor and no partially-bound feature is required (the same rule the synchronous path uses).
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
        return dset;
    }

    void record_bindless(VulkanRasterTarget& t, VulkanRasterProgram& p, ITexture* const* textures, crd::u32 n,
                         ClearColor clear_color, crd::u32 vertex_count)
    {
        VkCommandBuffer cmd  = m_frame_rec.cmd;
        VkDescriptorSet dset = frame_alloc_bindless_set(textures, n);
        if (dset == VK_NULL_HANDLE) { return; }
        frame_self_barrier_if_needed(t);
        VkRenderingAttachmentInfo att = colour_clear_attachment(t.view(), clear_color);
        VkRenderingInfo           ri  = one_colour_rendering(t, att);
        vkCmdBeginRendering(cmd, &ri);
        set_draw_state(cmd, t.width(), t.height(), 1U, false, VK_COMPARE_OP_ALWAYS);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.layout(), 0U, 1U, &dset, 0U, nullptr);
        bind_and_draw(cmd, p, vertex_count);
        vkCmdEndRendering(cmd);
    }

    // REN-2: the frame-mode body of draw_storage — a COLOR-ONLY render into an RTT transient (no depth attachment,
    // no readback), into the shared cmd. Pass 1 of render-to-texture; a later pass samples it via record_textured.
    void record_offscreen(VulkanRasterTarget& t, VulkanRasterProgram& p, VulkanStorageBuffer& s, ClearColor clear_color,
                          crd::u32 vertex_count)
    {
        VkCommandBuffer cmd  = m_frame_rec.cmd;
        VkDescriptorSet dset = frame_alloc_storage_set(s);
        if (dset == VK_NULL_HANDLE) { return; }
        frame_self_barrier_if_needed(t);
        VkRenderingAttachmentInfo att = colour_clear_attachment(t.view(), clear_color);
        VkRenderingInfo           ri  = one_colour_rendering(t, att);
        vkCmdBeginRendering(cmd, &ri);
        set_draw_state(cmd, t.width(), t.height(), 1U, false, VK_COMPARE_OP_ALWAYS);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.layout(), 0U, 1U, &dset, 0U, nullptr);
        bind_and_draw(cmd, p, vertex_count);
        vkCmdEndRendering(cmd);
    }

    // REN-3.1: the frame-mode body of draw_storage_depth_only — the SHADOW PASS. Renders storage-pulled geometry
    // writing ONLY depth: `colorAttachmentCount = 0`, `pDepthAttachment` = the (transient) depth target. A later
    // pass samples the result through the comparison sampler. This is the device capability that lets us RENDER a
    // shadow map instead of uploading one from the CPU (`ckir_lighting.hpp:992`).
    void record_depth_only(VulkanRasterTarget& t, VulkanRasterProgram& p, VulkanStorageBuffer& s, bool clear,
                           float clear_depth, DepthCompare compare, crd::u32 vertex_count)
    {
        VkCommandBuffer cmd  = m_frame_rec.cmd;
        VkDescriptorSet dset = frame_alloc_storage_set(s);
        if (dset == VK_NULL_HANDLE) { return; }

        VkRenderingAttachmentInfo dep{};
        dep.sType                          = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        dep.imageView                      = t.depth_view();
        dep.imageLayout                    = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        // clear ⇒ the FIRST mesh of the shadow pass; load ⇒ every later mesh joins the SAME map (without this a
        // multi-mesh shadow pass would wipe itself, see draw_storage_depth_only_load).
        dep.loadOp                         = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        dep.storeOp                        = VK_ATTACHMENT_STORE_OP_STORE;
        if (clear) { dep.clearValue.depthStencil.depth = clear_depth; }

        VkRenderingInfo ri{};
        ri.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea.extent    = {t.width(), t.height()};
        ri.layerCount           = 1U;
        ri.colorAttachmentCount = 0U;      // ⛔ the whole point: NO colour attachment is bound
        ri.pColorAttachments    = nullptr;
        ri.pDepthAttachment     = &dep;
        vkCmdBeginRendering(cmd, &ri);
        // samples = 1, depth_test on, and COLOR_ATTACHMENTS = 0 (the 6th arg) — the blend/colour-write state must
        // agree with the zero colour attachments in VkRenderingInfo or the pipeline is invalid.
        set_draw_state(cmd, t.width(), t.height(), 1U, true, to_vk_compare(compare), 0U);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.layout(), 0U, 1U, &dset, 0U, nullptr);
        bind_and_draw(cmd, p, vertex_count);
        vkCmdEndRendering(cmd);
    }

    // REN-2: the frame-mode body of draw_sampled — bind a SAMPLED image (an RTT transient or a material map) +
    // sampler, COLOR-render into the target, into the shared cmd. Pass 2 of RTT / the material forward pass.
    void record_textured(VulkanRasterTarget& t, VulkanRasterProgram& p, VkImageView view, VkSampler sampler,
                         ClearColor clear_color, crd::u32 vertex_count)
    {
        VkCommandBuffer cmd  = m_frame_rec.cmd;
        VkDescriptorSet dset = frame_alloc_sampled_set(view, sampler);
        if (dset == VK_NULL_HANDLE) { return; }
        frame_self_barrier_if_needed(t);
        VkRenderingAttachmentInfo att = colour_clear_attachment(t.view(), clear_color);
        VkRenderingInfo           ri  = one_colour_rendering(t, att);
        vkCmdBeginRendering(cmd, &ri);
        set_draw_state(cmd, t.width(), t.height(), 1U, false, VK_COMPARE_OP_ALWAYS);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.layout(), 0U, 1U, &dset, 0U, nullptr);
        bind_and_draw(cmd, p, vertex_count);
        vkCmdEndRendering(cmd);
    }

    // REN-2 Half B: write a set with storage(0) + sampled-image(1) + sampler(2) — the TEXTURED scene draw's descriptors
    // (the VS vertex-pulls position+UV from the buffer, the FS samples the material base-color map at UV).
    static void write_scene_textured(VkDevice dev, VkDescriptorSet dset, VkBuffer buf, VkImageView view, VkSampler samp)
    {
        VkDescriptorBufferInfo dbi{buf, 0, VK_WHOLE_SIZE};
        VkDescriptorImageInfo  img_info{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo  samp_info{samp, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        VkWriteDescriptorSet   wr[3]{};
        wr[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[0].dstSet = dset; wr[0].dstBinding = 0U; wr[0].descriptorCount = 1U; wr[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wr[0].pBufferInfo = &dbi;
        wr[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[1].dstSet = dset; wr[1].dstBinding = 1U; wr[1].descriptorCount = 1U; wr[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;  wr[1].pImageInfo  = &img_info;
        wr[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[2].dstSet = dset; wr[2].dstBinding = 2U; wr[2].descriptorCount = 1U; wr[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;        wr[2].pImageInfo  = &samp_info;
        vkUpdateDescriptorSets(dev, 3U, wr, 0U, nullptr);
    }

    // REN-2 Half B: the frame-mode body of draw_storage_textured_depth (clear) / _load (no clear) — a depth-tested
    // scene draw that ALSO samples the material texture, into the shared cmd. The SceneRenderer's textured forward pass.
    // REN-3.2-b:  selects the sampler bound at slot 2. VK_NULL_HANDLE means the default FILTERING sampler
    // (the textured scene draw); the shadowed draw passes the COMPARISON sampler. Same descriptor layout either
    // way, so there is one recording path rather than two that could drift apart.
    void record_scene_textured(VulkanRasterTarget& t, VulkanRasterProgram& p, VulkanStorageBuffer& s, VulkanTexture& tex,
                               bool clear, ClearColor clear_color, float clear_depth, DepthCompare compare,
                               crd::u32 vertex_count, VkSampler samp = VK_NULL_HANDLE)
    {
        VkCommandBuffer             cmd = m_frame_rec.cmd;
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool     = m_frame_rec.pool;
        dsai.descriptorSetCount = 1U;
        dsai.pSetLayouts        = &m_storage_set_layout;
        VkDescriptorSet dset = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(m_device, &dsai, &dset) != VK_SUCCESS) { return; }
        write_scene_textured(m_device, dset, s.buf(), tex.view(), samp != VK_NULL_HANDLE ? samp : m_default_sampler);
        frame_self_barrier_if_needed(t);

        VkRenderingAttachmentInfo att{};
        att.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        att.imageView   = t.view();
        att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.loadOp      = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        att.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
        if (clear) { att.clearValue.color = {{clear_color.r, clear_color.g, clear_color.b, clear_color.a}}; }
        VkRenderingAttachmentInfo dep{};
        dep.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        dep.imageView   = t.depth_view();
        dep.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        dep.loadOp      = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        dep.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
        if (clear) { dep.clearValue.depthStencil.depth = clear_depth; }
        VkRenderingInfo ri{};
        ri.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea.extent    = {t.width(), t.height()};
        ri.layerCount           = 1U;
        ri.colorAttachmentCount = 1U;
        ri.pColorAttachments    = &att;
        ri.pDepthAttachment     = &dep;
        vkCmdBeginRendering(cmd, &ri);
        set_draw_state(cmd, t.width(), t.height(), 1U, true, to_vk_compare(compare));
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.layout(), 0U, 1U, &dset, 0U, nullptr);
        bind_and_draw(cmd, p, vertex_count);
        vkCmdEndRendering(cmd);
    }

    // The frame-mode body of draw_overlay: LOAD + alpha-blend + read-only depth, into the shared cmd.
    [[nodiscard]] bool record_overlay(VulkanRasterTarget& t, VulkanRasterProgram& p, VulkanStorageBuffer& s,
                                      DepthCompare compare, crd::u32 vertex_count)
    {
        VkCommandBuffer cmd  = m_frame_rec.cmd;
        VkDescriptorSet dset = frame_alloc_storage_set(s);
        if (dset == VK_NULL_HANDLE) { return false; }
        frame_self_barrier_if_needed(t);

        VkRenderingAttachmentInfo att{};
        att.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        att.imageView   = t.view();
        att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;
        att.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

        const bool                depth_on = t.has_depth() && compare != DepthCompare::Always;
        VkRenderingAttachmentInfo dep{};
        if (depth_on)
        {
            dep.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            dep.imageView   = t.depth_view();
            dep.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            dep.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;
            dep.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
        }

        VkRenderingInfo ri{};
        ri.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea.extent    = {t.width(), t.height()};
        ri.layerCount           = 1U;
        ri.colorAttachmentCount = 1U;
        ri.pColorAttachments    = &att;
        ri.pDepthAttachment     = depth_on ? &dep : nullptr;
        vkCmdBeginRendering(cmd, &ri);
        set_draw_state(cmd, t.width(), t.height(), 1U, depth_on, to_vk_compare(compare));
        vkCmdSetDepthWriteEnable(cmd, VK_FALSE);
        const VkBool32 blend_on[1] = {VK_TRUE};
        m_api.set_color_blend_enable(cmd, 0U, 1U, blend_on);
        VkColorBlendEquationEXT eq[1]{};
        eq[0] = {VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD,
                 VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD};
        m_api.set_color_blend_equation(cmd, 0U, 1U, eq);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.layout(), 0U, 1U, &dset, 0U, nullptr);
        bind_and_draw(cmd, p, vertex_count);
        vkCmdEndRendering(cmd);
        return true;
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
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * 4U;
        BufferBundle       staging;
        if (!make_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging)
            || staging.mapped == nullptr)
        {
            destroy_image_bundle(m_device, img);
            destroy_buffer_bundle(m_device, staging);
            return nullptr;
        }
        std::memcpy(staging.mapped, rgba, static_cast<size_t>(bytes));

        VkCommandBuffer cmd = begin_cmd();
        if (cmd != VK_NULL_HANDLE)
        {
            transition(cmd, img.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy region{};
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1U;
            region.imageExtent                 = {width, height, 1U};
            vkCmdCopyBufferToImage(cmd, staging.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1U, &region);
            transition(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            end_and_wait(cmd);
        }
        destroy_buffer_bundle(m_device, staging);
        auto tex = std::make_unique<VulkanTexture>(m_device, img, width, height);
        tex->set_registry(&m_live_textures); // RET-4 pt 5: the S7 image-defrag pass walks the live set
        m_live_textures.push_back(tex.get());
        return tex;
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
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * 4U;
        BufferBundle       staging;
        if (!make_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging)
            || staging.mapped == nullptr)
        {
            destroy_image_bundle(m_device, img);
            destroy_buffer_bundle(m_device, staging);
            return nullptr;
        }
        std::memcpy(staging.mapped, rgba, static_cast<size_t>(bytes));

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
            vkCmdCopyBufferToImage(cmd, staging.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1U, &region);

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
        destroy_buffer_bundle(m_device, staging);
        auto tex = std::make_unique<VulkanTexture>(m_device, img, width, height);
        tex->set_registry(&m_live_textures); // RET-4 pt 5: the S7 image-defrag pass walks the live set
        m_live_textures.push_back(tex.get());
        return tex;
    }

    // GEO-3 stage 4 / RET-3: the cooked chain uploads VERBATIM (one copy region per level, no blits — the cook's
    // linear-space-filtered mips are authoritative); `srgb` picks the sRGB format so sampling hardware-decodes.
    [[nodiscard]] std::unique_ptr<ITexture> create_texture_from_mips(crd::u32 width, crd::u32 height, crd::u32 mip_count,
                                                                    const void* const* mips, bool srgb) override
    {
        if (width == 0U || height == 0U || mip_count == 0U || mip_count > 16U || mips == nullptr) { return nullptr; }
        for (crd::u32 i = 0; i < mip_count; ++i)
        {
            if (mips[i] == nullptr) { return nullptr; }
        }
        const VkFormat fmt = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        ImageBundle    img{};
        if (!create_image_bundle(width, height, VK_SAMPLE_COUNT_1_BIT, fmt, VK_IMAGE_ASPECT_COLOR_BIT,
                                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, img, mip_count))
        {
            return nullptr;
        }

        // one staging buffer holding every level back to back
        VkDeviceSize total = 0;
        {
            crd::u32 mw = width;
            crd::u32 mh = height;
            for (crd::u32 i = 0; i < mip_count; ++i)
            {
                total += static_cast<VkDeviceSize>(mw) * mh * 4U;
                mw = mw > 1U ? mw / 2U : 1U;
                mh = mh > 1U ? mh / 2U : 1U;
            }
        }
        BufferBundle staging;
        if (!make_buffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging)
            || staging.mapped == nullptr)
        {
            destroy_image_bundle(m_device, img);
            destroy_buffer_bundle(m_device, staging);
            return nullptr;
        }

        VkBufferImageCopy regions[16] = {};
        {
            crd::u8*     dst = static_cast<crd::u8*>(staging.mapped);
            VkDeviceSize off = 0;
            crd::u32     mw  = width;
            crd::u32     mh  = height;
            for (crd::u32 i = 0; i < mip_count; ++i)
            {
                const VkDeviceSize bytes = static_cast<VkDeviceSize>(mw) * mh * 4U;
                std::memcpy(dst + off, mips[i], static_cast<size_t>(bytes));
                regions[i].bufferOffset                = off;
                regions[i].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                regions[i].imageSubresource.mipLevel   = i;
                regions[i].imageSubresource.layerCount = 1U;
                regions[i].imageExtent                 = {mw, mh, 1U};
                off += bytes;
                mw = mw > 1U ? mw / 2U : 1U;
                mh = mh > 1U ? mh / 2U : 1U;
            }
        }

        VkCommandBuffer cmd = begin_cmd();
        if (cmd != VK_NULL_HANDLE)
        {
            VkImageMemoryBarrier b{};
            b.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.oldLayout                   = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout                   = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
            b.image                       = img.image;
            b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            b.subresourceRange.levelCount = mip_count;
            b.subresourceRange.layerCount = 1U;
            b.dstAccessMask               = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                                 nullptr, 1U, &b);
            vkCmdCopyBufferToImage(cmd, staging.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, mip_count,
                                   regions);
            b.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                                 0, nullptr, 1U, &b);
            end_and_wait(cmd);
        }
        destroy_buffer_bundle(m_device, staging);
        auto tex = std::make_unique<VulkanTexture>(m_device, img, width, height);
        tex->set_registry(&m_live_textures); // RET-4 pt 5: the S7 image-defrag pass walks the live set
        m_live_textures.push_back(tex.get());
        return tex;
    }

    // RET-2 (ADR-0105): the present seam — a real window (HWND) or a HEADLESS surface (nullptr) behind one interface.
    [[nodiscard]] std::unique_ptr<IPresentSurface> create_present_surface(void* native_window, crd::u32 width,
                                                                          crd::u32 height, PresentMode mode) override
    {
        if (!m_ctx->present_capable()) { return nullptr; }
        if (native_window == nullptr && !m_ctx->headless_surface()) { return nullptr; }
        auto surface = std::make_unique<VulkanPresentSurface>(*m_ctx, native_window, width, height, mode);
        if (!surface->valid()) { return nullptr; }
        return surface;
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

        const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * depth * layers * 4U;
        BufferBundle       staging;
        if (!make_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging)
            || staging.mapped == nullptr)
        {
            destroy_image_bundle(m_device, img);
            destroy_buffer_bundle(m_device, staging);
            return nullptr;
        }
        std::memcpy(staging.mapped, rgba, static_cast<size_t>(bytes));

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
            vkCmdCopyBufferToImage(cmd, staging.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1U, &region);
            transition_layers(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, layers);
            end_and_wait(cmd);
        }
        destroy_buffer_bundle(m_device, staging);
        auto tex = std::make_unique<VulkanTexture>(m_device, img, width, height);
        tex->set_registry(&m_live_textures); // RET-4 pt 5: the S7 image-defrag pass walks the live set
        m_live_textures.push_back(tex.get());
        return tex;
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
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * 4U; // one float per texel
        BufferBundle       staging;
        if (!make_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging)
            || staging.mapped == nullptr)
        {
            destroy_image_bundle(m_device, img);
            destroy_buffer_bundle(m_device, staging);
            return nullptr;
        }
        std::memcpy(staging.mapped, depth, static_cast<size_t>(bytes));

        VkCommandBuffer cmd = begin_cmd();
        if (cmd != VK_NULL_HANDLE)
        {
            depth_barrier(cmd, img.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                          VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy region{};
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            region.imageSubresource.layerCount = 1U;
            region.imageExtent                 = {width, height, 1U};
            vkCmdCopyBufferToImage(cmd, staging.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1U, &region);
            depth_barrier(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            end_and_wait(cmd);
        }
        destroy_buffer_bundle(m_device, staging);
        auto tex = std::make_unique<VulkanTexture>(m_device, img, width, height);
        tex->set_registry(&m_live_textures); // RET-4 pt 5: the S7 image-defrag pass walks the live set
        m_live_textures.push_back(tex.get());
        return tex;
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
        if (!m_api.valid() || !p.valid() || count == 0U || textures == nullptr) { return; }
        const crd::u32 n = count < kBindlessMax ? count : kBindlessMax;
        // REN-38-A1a: inside a frame, RECORD — never reset the global pool out from under the frame's other passes.
        if (frame_recording()) { record_bindless(t, p, textures, n, clear_color, vertex_count); return; }
        if (m_desc_pool == VK_NULL_HANDLE) { return; }

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
        ImageBundle  imgs[kMaxGBuffer]{};
        BufferBundle readbacks[kMaxGBuffer]{};
        for (crd::u32 i = 0; i < attachments; ++i)
        {
            if (!create_image_bundle(width, height, VK_SAMPLE_COUNT_1_BIT, kColorFormat, VK_IMAGE_ASPECT_COLOR_BIT,
                                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, imgs[i])
                || !create_readback(width, height, readbacks[i]))
            {
                destroy_image_bundle(m_device, imgs[i]);
                destroy_buffer_bundle(m_device, readbacks[i]);
                for (crd::u32 j = 0; j < i; ++j) // roll back the attachments already created
                {
                    destroy_buffer_bundle(m_device, readbacks[j]);
                    destroy_image_bundle(m_device, imgs[j]);
                }
                return nullptr;
            }
        }
        return std::make_unique<VulkanGBufferTarget>(m_device, attachments, imgs, readbacks, width, height);
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

        // REN-2: in frame-graph recording mode, sampling records into the shared cmd (RTT compose / material forward).
        if (frame_recording()) { record_textured(t, p, view, sampler, clear_color, vertex_count); return; }

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
        // RET-4 (caught by the ported ValidationCapture — a latent error in EVERY draw): when the device enables
        // `tessellationShader` (B4-tess), vkCmdDraw requires the tess stages to be PROVIDED to vkCmdBindShadersEXT —
        // VK_NULL_HANDLE for non-tessellation draws. Bound here (before every per-draw shader bind) so a tess draw's
        // later real tcs/tes bind simply overrides the nulls.
        if (m_ctx->tessellation())
        {
            const VkShaderStageFlagBits tess_stages[2] = {VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                                                          VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT};
            const VkShaderEXT           tess_null[2]   = {VK_NULL_HANDLE, VK_NULL_HANDLE};
            m_api.bind(cmd, 2U, tess_stages, tess_null);
        }
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

        // RET-4 pt 2: image memory comes from the ABSORBED S6 suballocator (pooled blocks, dedicated ≥16 MiB) —
        // never a per-image vkAllocateMemory (maxMemoryAllocationCount is a real ceiling the old path burned).
        (void)pd;
        VkMemoryRequirements ir{};
        vkGetImageMemoryRequirements(m_device, out.image, &ir);
        GpuAllocation alloc;
        if (!m_gpu_alloc->allocate(ir, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, /*linear=*/false, /*map=*/false, alloc)
            || vkBindImageMemory(m_device, out.image, alloc.memory, alloc.offset) != VK_SUCCESS)
        {
            if (alloc.valid()) { m_gpu_alloc->free(alloc); }
            destroy_image_bundle(m_device, out);
            out = {};
            return false;
        }
        out.owner      = m_gpu_alloc.get();
        out.alloc      = alloc;
        out.format     = format; // RET-4 pt 5: self-describing bundle (image relocation recreates from these)
        out.width      = w;
        out.height     = h;
        out.mip_levels = mip_levels;
        out.samples    = samples;
        out.usage      = usage;
        out.aspect     = aspect;

        VkImageViewCreateInfo vci{};
        vci.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image                       = out.image;
        vci.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
        vci.format                      = format;
        vci.subresourceRange.aspectMask = aspect;
        // the view exposes EVERY level the image has — a 1-level view over a mipped image silently zeroes any
        // texelFetch/sample beyond the base (the GEO-3 close gate caught exactly that)
        vci.subresourceRange.levelCount = mip_levels;
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
    // RET-4 pt 4: EVERY buffer pools through the S6 suballocator (linear pools, separate from optimal images).
    // Host-visible requests come back MAPPED (block-base + offset — pooled memory is never per-buffer unmapped).
    [[nodiscard]] bool make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                                   BufferBundle& out) const
    {
        out = {};
        VkBufferCreateInfo bci{};
        bci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size        = size;
        bci.usage       = usage;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(m_device, &bci, nullptr, &out.buffer) != VK_SUCCESS) { out = {}; return false; }
        VkMemoryRequirements mr{};
        vkGetBufferMemoryRequirements(m_device, out.buffer, &mr);
        const bool    want_map = (props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0U;
        GpuAllocation alloc;
        if (!m_gpu_alloc->allocate(mr, props, /*linear=*/true, want_map, alloc)
            || vkBindBufferMemory(m_device, out.buffer, alloc.memory, alloc.offset) != VK_SUCCESS)
        {
            if (alloc.valid()) { m_gpu_alloc->free(alloc); }
            vkDestroyBuffer(m_device, out.buffer, nullptr);
            out = {};
            return false;
        }
        out.mapped = alloc.mapped;
        out.owner  = m_gpu_alloc.get();
        out.alloc  = alloc;
        return true;
    }

    [[nodiscard]] bool create_readback(crd::u32 w, crd::u32 h, BufferBundle& out) const
    {
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h * 4U;
        return make_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, out)
            && out.mapped != nullptr;
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
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (ms ? 0U : static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
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

        BufferBundle readback;
        if (!create_readback(width, height, readback))
        {
            destroy_image_bundle(m_device, color);
            destroy_image_bundle(m_device, resolve);
            destroy_image_bundle(m_device, depth);
            return nullptr;
        }
        return std::make_unique<VulkanRasterTarget>(m_device, color, resolve, depth, readback, samples, width, height);
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
    // RET-4 pt 2: the absorbed S6 suballocator — every image bundle's memory pools here (mutable: allocation is an
    // implementation detail of logically-const creation helpers; the allocator is internally serialized).
    mutable std::unique_ptr<VulkanGpuAllocator> m_gpu_alloc;
    PFN_vkCmdSetFragmentShadingRateKHR   m_set_vrs          = nullptr; // B1-e: null unless VRS is enabled
    PFN_vkCmdSetConservativeRasterizationModeEXT m_set_conservative = nullptr; // B1-f: null unless conservative raster on
    PFN_vkCmdSetExtraPrimitiveOverestimationSizeEXT m_set_overest_size = nullptr; // B1-f: companion overestimate dyn-state
    VkDescriptorSetLayout                m_storage_set_layout = VK_NULL_HANDLE; // material set 0: storage(0)+image(1)+sampler(2)
    VkDescriptorPool                     m_desc_pool          = VK_NULL_HANDLE; // pool for draw_storage / draw_textured sets
    VkSampler                            m_default_sampler    = VK_NULL_HANDLE; // B2: the default bilinear/repeat sampler
    VkSampler                            m_cmp_sampler        = VK_NULL_HANDLE; // B2-b: comparison sampler (shadow)

    // REN-1: frame-graph RECORDING MODE. When `cmd` is non-null a frame graph is executing: the public draw_*
    // methods RECORD into this shared command buffer + frame descriptor pool (no per-draw submit/readback).
    struct FrameRec
    {
        VkCommandBuffer  cmd         = VK_NULL_HANDLE; // the frame's one command buffer (owned by the graph)
        VkDescriptorPool pool        = VK_NULL_HANDLE; // the graph's per-frame descriptor pool (reset once/frame)
        VkImage          pass_last   = VK_NULL_HANDLE; // last target drawn in the CURRENT pass (self-barrier key)
    };
    FrameRec m_frame_rec{};

public:
    // REN-1: the frame graph brackets a frame's recording with these (defined in the anon namespace's
    // VulkanFrameGraph). While recording, draw_storage_depth / _load / draw_overlay record into `cmd`.
    void frame_rec_begin(VkCommandBuffer cmd, VkDescriptorPool pool) noexcept
    {
        m_frame_rec.cmd = cmd;
        m_frame_rec.pool = pool;
        m_frame_rec.pass_last = VK_NULL_HANDLE;
    }
    void frame_rec_new_pass() noexcept { m_frame_rec.pass_last = VK_NULL_HANDLE; } // clear the self-barrier key
    void frame_rec_end() noexcept { m_frame_rec = FrameRec{}; }
    [[nodiscard]] bool frame_recording() const noexcept { return m_frame_rec.cmd != VK_NULL_HANDLE; }

    // REN-1: the graph's end-of-frame readback (COLOR_ATTACHMENT → TRANSFER_SRC + copy) so read_pixel stays
    // bit-identical to the sync path. `t` must be in COLOR_ATTACHMENT (the last frame-mode draw left it there).
    void frame_readback(VkCommandBuffer cmd, IRasterTarget& target)
    {
        copy_colour_to_readback(cmd, static_cast<VulkanRasterTarget&>(target));
    }

    // REN-1: the frame graph (defined out-of-line after VulkanFrameGraph, which needs this class complete).
    [[nodiscard]] std::unique_ptr<IFrameGraph> create_frame_graph() override;
    // expose the shared internals the graph needs to record its own barriers / present / readback
    [[nodiscard]] VkDevice          frame_vk_device() const noexcept { return m_device; }
    [[nodiscard]] VkQueue           frame_vk_queue() const noexcept { return m_queue; }
    [[nodiscard]] VulkanGpuContext& frame_ctx() const noexcept { return *m_ctx; }

private:
    // RET-4 pt 5: the live-resource registries the S7 defrag pass walks (resources leave them in their dtors).
    crd::containers::Array<VulkanStorageBuffer*> m_live_storage{crd::memory::default_allocator()};
    crd::containers::Array<VulkanTexture*>       m_live_textures{crd::memory::default_allocator()};
};

// ── REN-1 (D-007 row 98): the FRAME GRAPH ─────────────────────────────────────────────────────────────────────────
// Records a frame's passes into ONE command buffer (replacing the synchronous submit+wait-per-draw substrate) with
// automatic cross-pass barriers, plus graph-owned TRANSIENT resources whose backing memory is ALIASED when their
// lifetimes are disjoint. Passes record via the raster context in frame-recording mode (see VulkanRasterContext's
// record_*). REN-1 executes passes in DECLARATION ORDER (a valid topological order by the API contract — a pass is
// declared after its producers); the declared read/write DAG drives the barrier schedule + transient lifetimes.

// REN-2: a transient used as a render target is a BORROWED VulkanRasterTarget over the graph-owned image+view (built
// in VulkanFrameGraph::build); `sampled` transients also get a borrowed VulkanTexture. No dedicated adapter needed.

class VulkanFrameGraph final : public IFrameGraph, public IFrameContext
{
public:
    explicit VulkanFrameGraph(VulkanRasterContext& rc) : m_rc(&rc), m_device(rc.frame_vk_device()), m_queue(rc.frame_vk_queue())
    {
        vkGetPhysicalDeviceMemoryProperties(rc.frame_ctx().vk_physical_device(), &m_mem_props);

        VkCommandPoolCreateInfo pci{};
        pci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = rc.frame_ctx().graphics_family();
        vkCreateCommandPool(m_device, &pci, nullptr, &m_pool);
        VkCommandBufferAllocateInfo cbai{};
        cbai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool        = m_pool;
        cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1U;
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        for (crd::u32 s = 0; s < kFramesInFlight; ++s)
        {
            vkAllocateCommandBuffers(m_device, &cbai, &m_slots_if[s].cmd);
            vkCreateFence(m_device, &fci, nullptr, &m_slots_if[s].fence);
        }
        m_cmd   = m_slots_if[0].cmd;
        m_fence = m_slots_if[0].fence;

        // a large per-frame descriptor pool the passes allocate storage sets from (reset once per execute). The
        // shared set-0 layout carries storage(0) + sampled image(1) + sampler(2) + a bindless array(3), so the
        // pool must supply all four types (else vkAllocateDescriptorSets validation-warns) — sized ×kFrameSets.
        VkDescriptorPoolSize dps[3] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kFrameSets},
                                       {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kFrameSets * (kBindlessMax + 1U)},
                                       {VK_DESCRIPTOR_TYPE_SAMPLER, kFrameSets}};
        VkDescriptorPoolCreateInfo dpci{};
        dpci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpci.maxSets       = kFrameSets;
        dpci.poolSizeCount = 3U;
        dpci.pPoolSizes    = dps;
        for (crd::u32 s = 0; s < kFramesInFlight; ++s)
        {
            vkCreateDescriptorPool(m_device, &dpci, nullptr, &m_slots_if[s].pool);
        }
        m_frame_desc_pool = m_slots_if[0].pool;

        // REN-8: the timestamp pool. `timestampPeriod == 0` means the device cannot timestamp at all — then
        // `gpu_timing_available()` stays false and the frame renders exactly as before. Timing is never
        // load-bearing: it must not be able to change what is drawn.
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(rc.frame_ctx().vk_physical_device(), &props);
        m_ts_period = static_cast<double>(props.limits.timestampPeriod);
        if (m_ts_period > 0.0)
        {
            VkQueryPoolCreateInfo qpci{};
            qpci.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            qpci.queryType  = VK_QUERY_TYPE_TIMESTAMP;
            qpci.queryCount = kMaxTimedPasses * 2U;
            for (crd::u32 s = 0; s < kFramesInFlight; ++s)
            {
                if (vkCreateQueryPool(m_device, &qpci, nullptr, &m_slots_if[s].ts) != VK_SUCCESS)
                {
                    m_slots_if[s].ts = VK_NULL_HANDLE;
                }
            }
            m_ts_pool = m_slots_if[0].ts;
        }
    }
    ~VulkanFrameGraph() override
    {
        // ⛔ never tear down a pool/fence, or free a transient, with work still in flight — drain EVERY slot
        wait_all_slots();
        free_transients();
        // REN-37.5: the persistent registry is the one thing `reset()` never touches, so the DESTRUCTOR is the
        // only place it is released. Ordered after `wait_all_slots()` for the same reason transients are.
        for (Persistent& p : m_persist) { destroy_persistent_impl(p); }
        m_persist.clear();
        for (crd::u32 s = 0; s < kFramesInFlight; ++s)
        {
            FrameSlot& fs = m_slots_if[s];
            if (fs.ts != VK_NULL_HANDLE) { vkDestroyQueryPool(m_device, fs.ts, nullptr); }
            if (fs.pool != VK_NULL_HANDLE) { vkDestroyDescriptorPool(m_device, fs.pool, nullptr); }
            if (fs.fence != VK_NULL_HANDLE) { vkDestroyFence(m_device, fs.fence, nullptr); }
        }
        if (m_pool != VK_NULL_HANDLE) { vkDestroyCommandPool(m_device, m_pool, nullptr); }
    }
    VulkanFrameGraph(const VulkanFrameGraph&)            = delete;
    VulkanFrameGraph& operator=(const VulkanFrameGraph&) = delete;
    VulkanFrameGraph(VulkanFrameGraph&&)                 = delete;
    VulkanFrameGraph& operator=(VulkanFrameGraph&&)      = delete;

    // ── IFrameContext ──
    [[nodiscard]] IRasterContext& raster() noexcept override { return *m_rc; }
    [[nodiscard]] IRasterTarget*  image(FgImage h) noexcept override
    {
        if (!h.valid() || h.id > m_images.size()) { return nullptr; }
        return m_images[h.id - 1U].target;
    }
    [[nodiscard]] ITexture* texture(FgImage h) noexcept override // REN-2: a `sampled` transient resolves to its view
    {
        if (!h.valid() || h.id > m_images.size()) { return nullptr; }
        return m_images[h.id - 1U].texture;
    }
    [[nodiscard]] IStorageBuffer* buffer(FgBuffer h) noexcept override
    {
        if (!h.valid() || h.id > m_buffers.size()) { return nullptr; }
        return m_buffers[h.id - 1U].buffer;
    }
    // REN-3.2: one SLICE of a layered transient as a render target (the per-cascade shadow write). A non-layered
    // image has no layer_targets, so layer 0 falls through to the single target — `image_layer(h,0)` is exactly
    // `image(h)` there, which is what lets a for_each-expanded pass use ONE code path for both shapes.
    [[nodiscard]] IRasterTarget* image_layer(FgImage h, crd::u32 layer) noexcept override
    {
        if (!h.valid() || h.id > m_images.size()) { return nullptr; }
        ImageNode& n = m_images[h.id - 1U];
        if (layer < n.layer_targets.size()) { return n.layer_targets[layer]; }
        return layer == 0U ? n.target : nullptr;
    }

    // ── IFrameGraph ──
    [[nodiscard]] FgImage import_target(IRasterTarget& target) override
    {
        for (crd::usize i = 0; i < m_images.size(); ++i)
        {
            if (m_images[i].target == &target) { return FgImage{static_cast<crd::u32>(i + 1U)}; }
        }
        ImageNode n{};
        n.target = &target;
        n.own    = Own::Imported;
        m_images.push_back(n);
        return FgImage{static_cast<crd::u32>(m_images.size())};
    }
    [[nodiscard]] FgBuffer import_storage(IStorageBuffer& buffer) override
    {
        for (crd::usize i = 0; i < m_buffers.size(); ++i)
        {
            if (m_buffers[i].buffer == &buffer) { return FgBuffer{static_cast<crd::u32>(i + 1U)}; }
        }
        BufferNode n{};
        n.buffer    = &buffer;
        n.transient = false;
        m_buffers.push_back(n);
        return FgBuffer{static_cast<crd::u32>(m_buffers.size())};
    }

    [[nodiscard]] FgImage create_transient_image(const FgImageDesc& desc) override
    {
        if (desc.width == 0U || desc.height == 0U) { return FgImage{0U}; }
        // REN-3.2: reject a bad layer count by RETURN VALUE — an invalid handle build() then refuses — rather
        // than clamping. A silently truncated cascade atlas renders a plausible-looking image with missing
        // cascades, which is the worst class of graphics bug: it looks like art direction.
        if (desc.layers == 0U || desc.layers > kFgMaxImageLayers) { return FgImage{0U}; }
        ImageNode n{};
        n.own  = Own::Transient;
        n.desc = desc;
        VkImageUsageFlags usage = 0;
        VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        VkFormat fmt = to_vk_format(desc.format, usage, aspect);
        if (desc.sampled) { usage |= VK_IMAGE_USAGE_SAMPLED_BIT; }
        if (desc.storage) { usage |= VK_IMAGE_USAGE_STORAGE_BIT; }

        VkImageCreateInfo ici{};
        ici.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.flags         = VK_IMAGE_CREATE_ALIAS_BIT; // graph aliases this image's memory with a disjoint-lifetime peer
        ici.imageType     = VK_IMAGE_TYPE_2D;
        ici.format        = fmt;
        ici.extent        = {desc.width, desc.height, 1U};
        ici.mipLevels     = 1U;
        ici.arrayLayers   = desc.layers; // REN-3.2: >1 ⇒ the 2D-ARRAY cascade/cube/stereo atlas
        ici.samples       = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ici.usage         = usage;
        ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(m_device, &ici, nullptr, &n.image) != VK_SUCCESS) { return FgImage{0U}; }
        vkGetImageMemoryRequirements(m_device, n.image, &n.mem_req);
        n.aspect = aspect;
        n.fmt    = fmt;
        n.target = nullptr; // adapter created after aliasing binds memory (in build)
        m_images.push_back(n);
        return FgImage{static_cast<crd::u32>(m_images.size())};
    }
    // ── REN-37.5: PERSISTENT images — created ONCE, kept across `reset()`, never aliased, never retired. ──
    // The substrate for TAA history, SSR/DDGI/ReSTIR temporal reuse, auto-exposure, ping-pong blur chains, and
    // (REN-37.9) cached viewport thumbnails whose steady-state cost must be ZERO passes.
    [[nodiscard]] FgImage create_persistent_image(crd::u32 key, const FgImageDesc& desc) override
    {
        if (desc.width == 0U || desc.height == 0U) { return FgImage{0U}; }
        if (desc.layers == 0U || desc.layers > kFgMaxImageLayers) { return FgImage{0U}; }

        crd::i32 found = -1;
        for (crd::u32 i = 0; i < m_persist.size(); ++i)
        {
            if (m_persist[i].key != key) { continue; }
            const FgImageDesc& d = m_persist[i].node.desc;
            // ⛔ A desc change (a resize, a format switch) genuinely INVALIDATES the history. Reusing a
            // differently-shaped image would be worse than losing it — the reprojection would read garbage that
            // looks like plausible motion. Destroy and recreate, and report it via `persistent_image_was_live`.
            if (d.width == desc.width && d.height == desc.height && d.format == desc.format
                && d.layers == desc.layers && d.sampled == desc.sampled && d.storage == desc.storage)
            {
                found = static_cast<crd::i32>(i);
            }
            else { destroy_persistent_impl(m_persist[i]); m_persist[i].key = 0U; }
            break;
        }

        if (found < 0)
        {
            Persistent p{};
            p.key            = key;
            p.node.desc      = desc;
            p.node.own = Own::Persistent;
            VkImageUsageFlags  usage  = 0;
            VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
            const VkFormat     fmt    = to_vk_format(desc.format, usage, aspect);
            if (desc.sampled) { usage |= VK_IMAGE_USAGE_SAMPLED_BIT; }
            if (desc.storage) { usage |= VK_IMAGE_USAGE_STORAGE_BIT; }
            // ⛔ NO `VK_IMAGE_CREATE_ALIAS_BIT`: this image's memory is dedicated and must never be handed to a
            // disjoint-lifetime peer. Aliasing is precisely the thing a history buffer must be exempt from.
            VkImageCreateInfo ici{};
            ici.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            ici.imageType     = VK_IMAGE_TYPE_2D;
            ici.format        = fmt;
            ici.extent        = {desc.width, desc.height, 1U};
            ici.mipLevels     = 1U;
            ici.arrayLayers   = desc.layers;
            ici.samples       = VK_SAMPLE_COUNT_1_BIT;
            ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
            ici.usage         = usage;
            ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
            ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (vkCreateImage(m_device, &ici, nullptr, &p.node.image) != VK_SUCCESS) { return FgImage{0U}; }
            vkGetImageMemoryRequirements(m_device, p.node.image, &p.node.mem_req);
            p.node.aspect = aspect;
            p.node.fmt    = fmt;

            const crd::u32 mt = find_memory_type(p.node.mem_req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (mt == 0xFFFFFFFFU) { vkDestroyImage(m_device, p.node.image, nullptr); return FgImage{0U}; }
            VkMemoryAllocateInfo mai{};
            mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            mai.allocationSize  = p.node.mem_req.size;
            mai.memoryTypeIndex = mt;
            if (vkAllocateMemory(m_device, &mai, nullptr, &p.memory) != VK_SUCCESS)
            {
                vkDestroyImage(m_device, p.node.image, nullptr);
                return FgImage{0U};
            }
            if (vkBindImageMemory(m_device, p.node.image, p.memory, 0) != VK_SUCCESS)
            {
                vkFreeMemory(m_device, p.memory, nullptr);
                vkDestroyImage(m_device, p.node.image, nullptr);
                return FgImage{0U};
            }
            // reuse a dead entry if a resize freed one, so keys do not leak slots across resizes
            crd::i32 dead = -1;
            for (crd::u32 i = 0; i < m_persist.size(); ++i)
            {
                if (m_persist[i].key == 0U) { dead = static_cast<crd::i32>(i); break; }
            }
            if (dead >= 0) { m_persist[static_cast<crd::u32>(dead)] = static_cast<Persistent&&>(p); found = dead; }
            else
            {
                m_persist.push_back(static_cast<Persistent&&>(p));
                found = static_cast<crd::i32>(m_persist.size() - 1U);
            }
            if (!materialize_image(m_persist[static_cast<crd::u32>(found)].node)) { return FgImage{0U}; }
            m_persist[static_cast<crd::u32>(found)].was_live = false;
        }
        else { m_persist[static_cast<crd::u32>(found)].was_live = true; }

        // The per-frame tracked node BORROWS the persistent entry's device objects and its LIVE LAYOUT — carrying
        // the layout across frames is what lets the barrier scheduler transition it correctly on frame 2 without
        // an UNDEFINED (contents-discarding) transition.
        Persistent& p = m_persist[static_cast<crd::u32>(found)];
        ImageNode   n{};
        n.target        = p.node.target;
        n.texture       = p.node.texture;
        n.own           = Own::Persistent;
        n.persist_index = found;
        n.desc          = p.node.desc;
        n.image         = p.node.image;
        n.view          = p.node.view;
        for (VkImageView v : p.node.layer_views) { n.layer_views.push_back(v); }
        for (IRasterTarget* t : p.node.layer_targets) { n.layer_targets.push_back(t); }
        n.fmt          = p.node.fmt;
        n.aspect       = p.node.aspect;
        n.mem_req      = p.node.mem_req;
        n.layout       = p.node.layout;
        n.depth_layout = p.node.depth_layout;
        m_images.push_back(static_cast<ImageNode&&>(n));
        return FgImage{static_cast<crd::u32>(m_images.size())};
    }

    [[nodiscard]] bool persistent_image_was_live(crd::u32 key) const noexcept override
    {
        for (crd::u32 i = 0; i < m_persist.size(); ++i)
        {
            if (m_persist[i].key == key) { return m_persist[i].was_live; }
        }
        return false;
    }

    [[nodiscard]] FgBuffer create_transient_buffer(crd::u32 size_bytes) override
    {
        if (size_bytes == 0U) { return FgBuffer{0U}; }
        BufferNode n{};
        n.transient = true;
        n.size      = size_bytes;
        VkBufferCreateInfo bci{};
        bci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size        = size_bytes;
        bci.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(m_device, &bci, nullptr, &n.vkbuf) != VK_SUCCESS) { return FgBuffer{0U}; }
        vkGetBufferMemoryRequirements(m_device, n.vkbuf, &n.mem_req);
        m_buffers.push_back(n);
        return FgBuffer{static_cast<crd::u32>(m_buffers.size())};
    }

    [[nodiscard]] IFramePassBuilder& add_pass(const char* name, FgPassKind kind) override
    {
        Pass p{};
        p.name = name;
        p.kind = kind;
        m_passes.push_back(p);
        m_builder.bind(this, m_passes.size() - 1U);
        return m_builder;
    }

    [[nodiscard]] bool build() override;
    void               execute() override;
    void               reset() override
    {
        // ⛔ REN-8: RETIRE, do not destroy. The last submission may still be reading these images/memory; they
        // are handed to that slot's retire list and freed when its fence signals. Destroying here (what this
        // did before frames-in-flight) is a use-after-free that renders correctly almost every time.
        if (m_last_slot >= 0 && m_slots_if[static_cast<crd::u32>(m_last_slot)].pending)
        {
            retire_transients_to(m_slots_if[static_cast<crd::u32>(m_last_slot)]);
        }
        else { free_transients(); } // nothing in flight — free immediately (keeps the peak footprint down)
        m_images.clear();
        m_buffers.clear();
        m_passes.clear();
        m_barrier_count = 0U;
    }

    [[nodiscard]] crd::u32 last_barrier_count() const noexcept override { return m_barrier_count; }
    [[nodiscard]] crd::u32 last_submit_count() const noexcept override { return m_submit_count; }
    [[nodiscard]] crd::u32 transient_memory_bytes() const noexcept override { return m_physical_bytes; }
    [[nodiscard]] crd::u32 transient_logical_bytes() const noexcept override { return m_logical_bytes; }

    // ── REN-8: GPU timing ──
    [[nodiscard]] crd::u32 pass_count() const noexcept override { return m_timed_passes; }
    [[nodiscard]] const char* pass_name(crd::u32 i) const noexcept override
    {
        return i < m_timed_passes ? m_pass_names[i] : nullptr;
    }
    [[nodiscard]] double pass_gpu_ms(crd::u32 i) const noexcept override
    {
        return i < m_timed_passes ? m_pass_ms[i] : 0.0;
    }
    [[nodiscard]] double gpu_ms_total() const noexcept override { return m_gpu_ms_total; }
    [[nodiscard]] bool   gpu_timing_available() const noexcept override { return m_ts_pool != VK_NULL_HANDLE; }
    void                 set_readback_enabled(bool on) noexcept override { m_readback = on; }

    // REN-8: the frames-in-flight machinery. The two that name FrameSlot are declared with it, below.
    void wait_pending_submit() noexcept; // wait the CURRENT slot, then drain its retire list + timestamps
    void wait_all_slots() noexcept;      // drain every slot (teardown / reset-to-idle)
    void resolve_timestamps() noexcept;

private:
    // one physical memory slot (one VkDeviceMemory) reusable across disjoint-lifetime transients
    struct Slot
    {
        VkDeviceMemory memory      = VK_NULL_HANDLE;
        VkDeviceSize   size        = 0;
        crd::i32       free_after  = -1; // the last pass index whose transient occupied this slot (-1 = free)
        crd::u32       type_bits   = 0xFFFFFFFFU;
    };
    // ⛔ WHO OWNS THIS, and FOR HOW LONG. This used to be a single `bool transient`, and that bool silently
    // conflated THREE INDEPENDENT QUESTIONS:
    //     · is the resource GRAPH-OWNED?   -> RTT barrier semantics, and NO end-of-frame readback (a borrowed
    //                                         wrapper has no readback buffer to copy into)
    //     · is it ALIASABLE?               -> the transient memory pool, the retire queue, the free path
    //     · is its state FRAME-LOCAL?      -> whether frame-start resets apply to it
    // The third question has NO predicate on purpose: it is answered by WHERE THE STATE LIVES. A persistent
    // image's live layout is stored in the registry entry (`live_layout()`), which the frame-start reset
    // cannot reach, so "skip persistent here" is not a rule anyone has to remember.
    // For a transient all three answers are "yes" and for an import all three are "no", so one bool served — right
    // up until a PERSISTENT resource, which is graph-owned, NOT aliasable, and NOT frame-local. Every site that
    // read `transient` then meant one of the three and got the other two for free, incorrectly.
    // The enum + the three NAMED predicates below make each site say which question it is asking, so a fourth
    // ownership kind cannot silently inherit the wrong answer.
    enum class Own : crd::u8
    {
        Imported = 0, // the application owns it; the graph only tracks its access
        Transient,    // the graph owns it for ONE frame: aliased, retired, reset
        Persistent,   // the graph owns it ACROSS frames: never aliased, never retired, never reset
    };
    struct ImageNode
    {
        IRasterTarget* target    = nullptr; // imported: the real target; graph-owned: the borrowed target
        ITexture*      texture   = nullptr; // REN-2: sampled + graph-owned ⇒ the borrowed sampled view
        Own            own       = Own::Imported;
        FgImageDesc    desc{};
        VkImage        image     = VK_NULL_HANDLE; // transient only
        VkImageView    view      = VK_NULL_HANDLE; // transient only; layers>1 ⇒ the whole-array SAMPLING view
        // REN-3.2: layers>1 ⇒ one single-slice VIEW_TYPE_2D view + borrowed target per layer. Attachment views
        // must be single-slice (a pass renders into ONE cascade), while the sampling view spans the array — so
        // a layered transient carries BOTH, never one reinterpreted as the other.
        crd::containers::Array<VkImageView>    layer_views{crd::memory::default_allocator()};
        crd::containers::Array<IRasterTarget*> layer_targets{crd::memory::default_allocator()};
        VkFormat       fmt       = VK_FORMAT_UNDEFINED;
        VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        VkMemoryRequirements mem_req{};
        crd::i32       first_pass = -1;
        crd::i32       last_pass  = -1;
        crd::i32       slot       = -1;
        bool           has_write  = false;
        VkImageLayout  layout       = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageLayout  depth_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        // REN-37.5: >= 0 ⇒ this node BORROWS a persistent entry's device objects AND its LIVE LAYOUT. The
        // layout of a persistent image lives in the REGISTRY, never here — see `live_layout()`. That is what
        // makes the frame-start reset below harmless for it BY CONSTRUCTION rather than by remembering a skip.
        crd::i32       persist_index = -1;
    };

    // The two questions that DO need a predicate, each answerable on its own.
    [[nodiscard]] static bool graph_owned(const ImageNode& n) noexcept { return n.own != Own::Imported; }
    [[nodiscard]] static bool aliasable(const ImageNode& n) noexcept { return n.own == Own::Transient; }

    // REN-37.5: one persistent image, keyed by a caller-chosen stable identity. Survives `reset()`, owns its own
    // dedicated memory, and is never handed to the aliasing allocator or the retire queue.
    struct Persistent
    {
        crd::u32       key      = 0U; // 0 = a dead slot, reusable after a resize freed one
        bool           was_live = false; // did it already carry history when this frame asked for it?
        VkDeviceMemory memory   = VK_NULL_HANDLE;
        ImageNode      node{};
    };
    struct BufferNode
    {
        IStorageBuffer* buffer    = nullptr;
        bool            transient = false;
        crd::u32        size      = 0;
        VkBuffer        vkbuf     = VK_NULL_HANDLE; // transient only
        VkMemoryRequirements mem_req{};
        crd::i32        first_pass = -1;
        crd::i32        last_pass  = -1;
        crd::i32        slot       = -1;
        bool            has_write  = false;
    };
    struct Access
    {
        crd::u32 handle = 0; // 1-based image/buffer id
        FgAccess access = FgAccess::Read;
    };
    struct Pass
    {
        const char*      name = nullptr;
        FgPassKind       kind = FgPassKind::Raster;
        crd::containers::Array<Access> img_access{crd::memory::default_allocator()};
        crd::containers::Array<Access> buf_access{crd::memory::default_allocator()};
        FgExecuteFn      fn    = nullptr;
        void*            user  = nullptr;
        IPresentSurface* present = nullptr;
    };

    // a fluent builder that rebinds to the current pass (one instance reused — add_pass returns it)
    class Builder final : public IFramePassBuilder
    {
    public:
        void bind(VulkanFrameGraph* g, crd::usize pass) noexcept { m_g = g; m_pass = pass; }
        IFramePassBuilder& reads(FgImage h) override { add_img(h, FgAccess::Read); return *this; }
        IFramePassBuilder& reads(FgBuffer h) override { add_buf(h, FgAccess::Read); return *this; }
        IFramePassBuilder& writes(FgImage h) override { add_img(h, FgAccess::Write); return *this; }
        IFramePassBuilder& writes(FgBuffer h) override { add_buf(h, FgAccess::Write); return *this; }
        IFramePassBuilder& read_writes(FgImage h) override { add_img(h, FgAccess::ReadWrite); return *this; }
        IFramePassBuilder& read_writes(FgBuffer h) override { add_buf(h, FgAccess::ReadWrite); return *this; }
        IFramePassBuilder& execute(FgExecuteFn fn, void* user) override
        {
            m_g->m_passes[m_pass].fn = fn;
            m_g->m_passes[m_pass].user = user;
            return *this;
        }
        IFramePassBuilder& present(IPresentSurface& surface) override
        {
            m_g->m_passes[m_pass].present = &surface;
            return *this;
        }
    private:
        void add_img(FgImage h, FgAccess a) { m_g->m_passes[m_pass].img_access.push_back({h.id, a}); }
        void add_buf(FgBuffer h, FgAccess a) { m_g->m_passes[m_pass].buf_access.push_back({h.id, a}); }
        VulkanFrameGraph* m_g = nullptr;
        crd::usize        m_pass = 0;
    };

    [[nodiscard]] crd::u32 find_memory_type(crd::u32 type_bits, VkMemoryPropertyFlags props) const noexcept
    {
        for (crd::u32 i = 0; i < m_mem_props.memoryTypeCount; ++i)
        {
            if ((type_bits & (1U << i)) != 0U && (m_mem_props.memoryTypes[i].propertyFlags & props) == props) { return i; }
        }
        return 0xFFFFFFFFU;
    }

    static VkFormat to_vk_format(FgImageFormat f, VkImageUsageFlags& usage, VkImageAspectFlags& aspect) noexcept
    {
        switch (f)
        {
        case FgImageFormat::RGBA8Unorm: usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT; aspect = VK_IMAGE_ASPECT_COLOR_BIT; return VK_FORMAT_R8G8B8A8_UNORM;
        case FgImageFormat::RGBA8Srgb:  usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT; aspect = VK_IMAGE_ASPECT_COLOR_BIT; return VK_FORMAT_R8G8B8A8_SRGB;
        case FgImageFormat::RGBA16F:    usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; aspect = VK_IMAGE_ASPECT_COLOR_BIT; return VK_FORMAT_R16G16B16A16_SFLOAT;
        case FgImageFormat::R16F:       usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; aspect = VK_IMAGE_ASPECT_COLOR_BIT; return VK_FORMAT_R16_SFLOAT;
        case FgImageFormat::R32F:       usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; aspect = VK_IMAGE_ASPECT_COLOR_BIT; return VK_FORMAT_R32_SFLOAT;
        case FgImageFormat::R32Uint:    usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; aspect = VK_IMAGE_ASPECT_COLOR_BIT; return VK_FORMAT_R32_UINT;
        case FgImageFormat::D32Float:
        default:                        usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT; aspect = VK_IMAGE_ASPECT_DEPTH_BIT; return VK_FORMAT_D32_SFLOAT;
        }
    }

    void free_transients() noexcept
    {
        for (ImageNode& n : m_images)
        {
            if (aliasable(n))
            {
                // REN-2: the wrappers are BORROWED (their dtors free nothing) — delete the wrapper objects, then the
                // graph-owned view+image they viewed. Delete via the base (virtual dtor); the target is now a borrowed
                // VulkanRasterTarget, the texture a borrowed VulkanTexture.
                delete n.texture;
                n.texture = nullptr;
                // REN-3.2: on a layered transient `target` ALIASES layer_targets[0] — free the per-layer targets
                // and null `target` FIRST, or the shared slice-0 wrapper is deleted twice.
                for (IRasterTarget* lt : n.layer_targets) { delete lt; }
                if (n.layer_targets.size() > 0) { n.target = nullptr; }
                n.layer_targets.clear();
                for (VkImageView lv : n.layer_views) { vkDestroyImageView(m_device, lv, nullptr); }
                n.layer_views.clear();
                delete n.target;
                n.target = nullptr;
                if (n.view != VK_NULL_HANDLE) { vkDestroyImageView(m_device, n.view, nullptr); n.view = VK_NULL_HANDLE; }
                if (n.image != VK_NULL_HANDLE) { vkDestroyImage(m_device, n.image, nullptr); n.image = VK_NULL_HANDLE; }
            }
        }
        for (BufferNode& n : m_buffers)
        {
            if (n.transient && n.vkbuf != VK_NULL_HANDLE) { vkDestroyBuffer(m_device, n.vkbuf, nullptr); n.vkbuf = VK_NULL_HANDLE; }
        }
        for (Slot& s : m_slots) { if (s.memory != VK_NULL_HANDLE) { vkFreeMemory(m_device, s.memory, nullptr); } }
        m_slots.clear();
        m_physical_bytes = 0U;
        m_logical_bytes  = 0U;
    }

    // REN-37.5: release ONE persistent entry. Same teardown order as `free_transients` (the layered `target`
    // aliases `layer_targets[0]`, so null it before the plain delete or slice 0 is destroyed twice) plus its own
    // dedicated memory, which no transient has.
    void destroy_persistent_impl(Persistent& p) noexcept
    {
        ImageNode& n = p.node;
        delete n.texture;
        n.texture = nullptr;
        for (IRasterTarget* lt : n.layer_targets) { delete lt; }
        if (n.layer_targets.size() > 0) { n.target = nullptr; }
        n.layer_targets.clear();
        for (VkImageView lv : n.layer_views) { vkDestroyImageView(m_device, lv, nullptr); }
        n.layer_views.clear();
        delete n.target;
        n.target = nullptr;
        if (n.view != VK_NULL_HANDLE) { vkDestroyImageView(m_device, n.view, nullptr); n.view = VK_NULL_HANDLE; }
        if (n.image != VK_NULL_HANDLE) { vkDestroyImage(m_device, n.image, nullptr); n.image = VK_NULL_HANDLE; }
        if (p.memory != VK_NULL_HANDLE) { vkFreeMemory(m_device, p.memory, nullptr); p.memory = VK_NULL_HANDLE; }
        n.layout       = VK_IMAGE_LAYOUT_UNDEFINED;
        n.depth_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    static constexpr crd::u32 kFrameSets = 256U;
    // REN-8: timing capacity. A frame with more passes than this still RENDERS correctly — the extra passes are
    // simply not timed (`pass_count()` reports how many were), so timing can never change what is drawn.
    static constexpr crd::u32 kMaxTimedPasses = 64U;

    VulkanRasterContext* m_rc     = nullptr;
    VkDevice             m_device = VK_NULL_HANDLE;
    VkQueue              m_queue  = VK_NULL_HANDLE;
    VkCommandPool        m_pool   = VK_NULL_HANDLE;
    // ── REN-8: FRAMES IN FLIGHT. ────────────────────────────────────────────────────────────────────────────
    // With ONE command buffer, frame N+1 cannot start recording until frame N's fence signals — deferring the
    // wait bought one frame of overlap but the re-record still serialized on the GPU (measured: 1.49 ms of
    // residual stall). A RING of per-frame resources removes that: frame N+1 records into its own buffer while
    // the GPU is still consuming frame N's.
    //
    // ⛔ Everything a submission reads must be per-slot, or the ring is a data race: the command buffer, the
    // fence, the descriptor pool the passes allocate sets from, and the timestamp pool. Sharing any one of them
    // reintroduces the hazard silently — the frame still renders, just occasionally from the wrong data.
    //
    // ⛔ Transients are the subtle half. `reset()` used to DESTROY them immediately, which is a use-after-free
    // once a previous frame can still be reading them. They now go to the submitting slot's RETIRE LIST and are
    // destroyed only when that slot's fence signals. This is what makes the ring safe for any authored graph,
    // including ones that own transients — the author never has to think about it.
    static constexpr crd::u32 kFramesInFlight = 2U;
    struct FrameSlot
    {
        VkCommandBuffer  cmd     = VK_NULL_HANDLE;
        VkFence          fence   = VK_NULL_HANDLE;
        VkDescriptorPool pool    = VK_NULL_HANDLE;
        VkQueryPool      ts      = VK_NULL_HANDLE;
        bool             pending = false;
        crd::containers::Array<VkImage>        dead_images{crd::memory::default_allocator()};
        crd::containers::Array<VkImageView>    dead_views{crd::memory::default_allocator()};
        crd::containers::Array<VkBuffer>       dead_buffers{crd::memory::default_allocator()};
        crd::containers::Array<VkDeviceMemory> dead_memory{crd::memory::default_allocator()};
        crd::containers::Array<IRasterTarget*> dead_targets{crd::memory::default_allocator()};
        crd::containers::Array<ITexture*>      dead_textures{crd::memory::default_allocator()};
    };
    FrameSlot m_slots_if[kFramesInFlight];
    crd::u32  m_slot      = 0U;      // the slot the NEXT execute() records into
    crd::i32  m_last_slot = -1;      // the slot that last submitted (owns the live transients)

    // REN-1/REN-8: the DEPENDENCY-SORTED execution order (declaration indices). execute() walks THIS, never
    // m_passes directly — that is what lets a pass be inserted anywhere and still run in the right place.
    crd::containers::Array<crd::u32> m_order{crd::memory::default_allocator()};

    void retire_transients_to(FrameSlot& slot) noexcept; // hand live transients to a slot's retire list
    void drain_retired(FrameSlot& slot) noexcept;        // destroy what that slot retired (fence has signalled)

    // The CURRENT slot's handles, refreshed at the top of execute(). Every recording path already reads these,
    // so the ring needs no changes anywhere below this point.
    VkCommandBuffer      m_cmd    = VK_NULL_HANDLE;
    VkFence              m_fence  = VK_NULL_HANDLE;
    VkDescriptorPool     m_frame_desc_pool = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties m_mem_props{};

    crd::containers::Array<ImageNode>  m_images{crd::memory::default_allocator()};
    crd::containers::Array<BufferNode> m_buffers{crd::memory::default_allocator()};
    // REN-37.5: the persistent registry — the ONE thing in this graph that outlives `reset()`.
    crd::containers::Array<Persistent> m_persist{crd::memory::default_allocator()};

    // ⭐ THE LIVE LAYOUT HAS EXACTLY ONE HOME. For a frame-local node that is the node's own field; for a
    // PERSISTENT one it is the registry entry, which `reset()` and the frame-start reset never touch. Routing
    // every read and write through here means there is nothing to copy back at end of frame and nothing to
    // remember to skip at the start of one — the durable state simply lives with the resource that owns it.
    [[nodiscard]] VkImageLayout& live_layout(ImageNode& n) noexcept
    {
        return n.persist_index >= 0 ? m_persist[static_cast<crd::u32>(n.persist_index)].node.layout : n.layout;
    }
    [[nodiscard]] VkImageLayout& live_depth_layout(ImageNode& n) noexcept
    {
        return n.persist_index >= 0 ? m_persist[static_cast<crd::u32>(n.persist_index)].node.depth_layout
                                    : n.depth_layout;
    }

    [[nodiscard]] bool materialize_image(ImageNode& n);
    [[nodiscard]] bool build_tail(crd::u32 img_slot_end, crd::containers::Array<crd::u32>& border);
    
    crd::containers::Array<Pass>       m_passes{crd::memory::default_allocator()};
    crd::containers::Array<Slot>       m_slots{crd::memory::default_allocator()};
    Builder m_builder{};

    crd::u32 m_barrier_count  = 0U;
    crd::u32 m_submit_count   = 0U;
    // REN-8: device timestamp queries around each pass
    VkQueryPool                             m_ts_pool   = VK_NULL_HANDLE;
    double                                  m_ts_period = 1.0; // ns per tick
    crd::containers::Array<const char*>      m_pass_names{crd::memory::default_allocator()};
    double                                  m_pass_ms[kMaxTimedPasses]{};
    crd::u32                                m_timed_passes = 0U;
    double                                  m_gpu_ms_total = 0.0;
    bool                                    m_readback     = true; // REN-8: opt-out; gates keep read_pixel
    bool                                    m_pending_submit = false; // REN-8: a submit not yet waited on
    crd::u32 m_physical_bytes = 0U;
    crd::u32 m_logical_bytes  = 0U;
};

// A minimal IStorageBuffer adapter over a transient buffer (REN-1: aliased + tracked; the drawn/compute-written
// path is REN-4's — a transient buffer is device-local, so read_u32 is 0).
class VulkanTransientBuffer final : public IStorageBuffer
{
public:
    explicit VulkanTransientBuffer(crd::u32 size) noexcept : m_size(size) {}
    [[nodiscard]] crd::u32 size_bytes() const noexcept override { return m_size; }
    [[nodiscard]] crd::u32 read_u32(crd::u32) const noexcept override { return 0U; }
private:
    crd::u32 m_size = 0;
};

bool VulkanFrameGraph::build()
{
    m_barrier_count = 0U;
    m_submit_count  = 0U;

    // 1) every pass access must reference an existing resource
    for (const Pass& p : m_passes)
    {
        for (const Access& a : p.img_access) { if (a.handle == 0U || a.handle > m_images.size()) { return false; } }
        for (const Access& a : p.buf_access) { if (a.handle == 0U || a.handle > m_buffers.size()) { return false; } }
    }

    // ── 1b) TOPOLOGICAL SORT — the frame graph's actual promise. ────────────────────────────────────────────
    // Passes execute in DEPENDENCY order, not declaration order: for every resource, all its WRITERS run before
    // all its READERS. That is what makes a graph composable — a pass can be added ANYWHERE (appended last, or
    // inserted between two existing passes) and still land in the right place, because the ORDER IS DERIVED FROM
    // THE DATA, not from when the author happened to declare it. Insert a depth pre-pass after the lighting pass
    // that reads its depth, and the graph still runs the pre-pass first.
    //
    // ⛔ Until this existed, `execute()` walked `m_passes` in declaration order while the interface header
    // promised a topo-sort and a CYCLE rejection. The docs were right and the code was not: a correctly-declared
    // graph whose passes were added out of order rendered garbage, silently.
    //
    // Ties break on declaration index, so the order is DETERMINISTIC and a dependency-free graph keeps exactly
    // the order the author wrote — no surprise reshuffling, and the same graph always schedules identically.
    {
        const crd::u32 np = static_cast<crd::u32>(m_passes.size());
        m_order.clear();
        crd::containers::Array<crd::u32> indeg(crd::memory::default_allocator());
        crd::containers::Array<crd::u32> edges(crd::memory::default_allocator()); // flattened adjacency: np x np bits
        indeg.resize(np, 0U);
        edges.resize(static_cast<crd::usize>(np) * np, 0U);
        const auto add_edge = [&](crd::u32 from, crd::u32 to) {
            if (from == to) { return; }
            crd::u32& e = edges[static_cast<crd::usize>(from) * np + to];
            if (e == 0U) { e = 1U; ++indeg[to]; }
        };
        // writer -> reader for every resource, in BOTH directions of the handle space (images + buffers)
        const auto link = [&](auto accessor) {
            for (crd::u32 w = 0; w < np; ++w)
            {
                for (const Access& aw : accessor(m_passes[w]))
                {
                    if (aw.access == FgAccess::Read) { continue; }
                    for (crd::u32 r = 0; r < np; ++r)
                    {
                        for (const Access& ar : accessor(m_passes[r]))
                        {
                            if (ar.handle != aw.handle) { continue; }
                            // A pure reader must follow the writer. Two WRITERS of the same resource keep
                            // declaration order (w < r) — that is the author's stated intent and the only
                            // defensible tie-break; reordering two writes would silently change the result.
                            if (ar.access == FgAccess::Read || w < r) { add_edge(w, r); }
                        }
                    }
                }
            }
        };
        link([](Pass& p) -> crd::containers::Array<Access>& { return p.img_access; });
        link([](Pass& p) -> crd::containers::Array<Access>& { return p.buf_access; });

        // Kahn, always taking the LOWEST ready index so the result is deterministic
        for (crd::u32 done = 0; done < np; ++done)
        {
            crd::u32 pick = np;
            for (crd::u32 i = 0; i < np; ++i)
            {
                if (indeg[i] == 0U) { pick = i; break; }
            }
            if (pick == np) { return false; } // ⛔ a dependency CYCLE — never a partial schedule
            indeg[pick] = 0xFFFFFFFFU;        // consumed
            m_order.push_back(pick);
            for (crd::u32 t = 0; t < np; ++t)
            {
                if (edges[static_cast<crd::usize>(pick) * np + t] != 0U && indeg[t] != 0xFFFFFFFFU) { --indeg[t]; }
            }
        }
    }

    // 2) transient LIFETIME analysis — [first pass touching .. last pass touching] + whether any pass writes it
    // ⛔ Positions are indices into the SORTED order, not declaration indices: aliasing reuses memory between
    // transients whose lifetimes are disjoint, and "disjoint" only means anything in EXECUTION order.
    for (ImageNode& n : m_images) { n.first_pass = -1; n.last_pass = -1; n.has_write = false; n.slot = -1; }
    for (BufferNode& n : m_buffers) { n.first_pass = -1; n.last_pass = -1; n.has_write = false; n.slot = -1; }
    for (crd::usize oi = 0; oi < m_order.size(); ++oi)
    {
        const crd::usize pi = m_order[oi];
        for (const Access& a : m_passes[pi].img_access)
        {
            ImageNode& n = m_images[a.handle - 1U];
            if (n.first_pass < 0) { n.first_pass = static_cast<crd::i32>(oi); }
            n.last_pass = static_cast<crd::i32>(oi);
            if (a.access != FgAccess::Read) { n.has_write = true; }
        }
        for (const Access& a : m_passes[pi].buf_access)
        {
            BufferNode& n = m_buffers[a.handle - 1U];
            if (n.first_pass < 0) { n.first_pass = static_cast<crd::i32>(oi); }
            n.last_pass = static_cast<crd::i32>(oi);
            if (a.access != FgAccess::Read) { n.has_write = true; }
        }
    }
    for (const ImageNode& n : m_images) { if (aliasable(n) && !n.has_write) { return false; } } // a transient no pass writes
    for (const BufferNode& n : m_buffers) { if (n.transient && !n.has_write) { return false; } }

    // 3) ALIASING — greedy interval assignment: process transients in first_pass order; reuse a slot whose last
    //    occupant's lifetime ended before this one begins (disjoint ⇒ shared memory). Images then buffers (each a
    //    memory class); physical = Σ slot sizes (post-aliasing), logical = Σ transient sizes (no aliasing).
    m_physical_bytes = 0U;
    m_logical_bytes  = 0U;

    // -- images --
    crd::containers::Array<crd::u32> order{crd::memory::default_allocator()};
    for (crd::u32 i = 0; i < m_images.size(); ++i)
    {
        if (aliasable(m_images[i])) { order.push_back(i); }
    }
    for (crd::usize a = 1; a < order.size(); ++a) // insertion sort by first_pass (small N)
    {
        const crd::u32 v = order[a];
        crd::usize     b = a;
        while (b > 0 && m_images[order[b - 1]].first_pass > m_images[v].first_pass) { order[b] = order[b - 1]; --b; }
        order[b] = v;
    }
    for (crd::u32 idx : order)
    {
        ImageNode& n = m_images[idx];
        m_logical_bytes += static_cast<crd::u32>(n.mem_req.size);
        crd::i32 chosen = -1;
        for (crd::u32 si = 0; si < m_slots.size(); ++si)
        {
            Slot& s = m_slots[si];
            if (s.free_after < n.first_pass && (s.type_bits & n.mem_req.memoryTypeBits) != 0U)
            {
                chosen = static_cast<crd::i32>(si);
                break;
            }
        }
        if (chosen < 0)
        {
            Slot s{};
            s.type_bits = n.mem_req.memoryTypeBits;
            m_slots.push_back(s);
            chosen = static_cast<crd::i32>(m_slots.size() - 1U);
        }
        Slot& s      = m_slots[static_cast<crd::u32>(chosen)];
        s.free_after = n.last_pass;
        s.type_bits &= n.mem_req.memoryTypeBits;
        if (n.mem_req.size > s.size) { s.size = n.mem_req.size; }
        n.slot = chosen;
    }

    // -- allocate each image slot's memory + bind + view --
    for (Slot& s : m_slots)
    {
        const crd::u32 mt = find_memory_type(s.type_bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mt == 0xFFFFFFFFU) { return false; }
        VkMemoryAllocateInfo mai{};
        mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize  = s.size;
        mai.memoryTypeIndex = mt;
        if (vkAllocateMemory(m_device, &mai, nullptr, &s.memory) != VK_SUCCESS) { return false; }
        m_physical_bytes += static_cast<crd::u32>(s.size);
    }
    for (ImageNode& n : m_images)
    {
        if (!aliasable(n) || n.slot < 0) { continue; }
        if (vkBindImageMemory(m_device, n.image, m_slots[static_cast<crd::u32>(n.slot)].memory, 0) != VK_SUCCESS) { return false; }
        if (!materialize_image(n)) { return false; }
    }

    // -- buffers (their own slots, appended after the image slots) --
    const crd::u32 img_slot_end = static_cast<crd::u32>(m_slots.size());
    crd::containers::Array<crd::u32> border{crd::memory::default_allocator()};
    for (crd::u32 i = 0; i < m_buffers.size(); ++i)
    {
        if (m_buffers[i].transient) { border.push_back(i); }
    }
    for (crd::usize a = 1; a < border.size(); ++a)
    {
        const crd::u32 v = border[a];
        crd::usize     b = a;
        while (b > 0 && m_buffers[border[b - 1]].first_pass > m_buffers[v].first_pass) { border[b] = border[b - 1]; --b; }
        border[b] = v;
    }
    return build_tail(img_slot_end, border);
}

// REN-37.5: MATERIALIZE an image node — its views, its borrowed render target(s) and (if `sampled`) its borrowed
// sampled view. Factored out of `build()` because a PERSISTENT image needs exactly the same materialization but
// at CREATION time (it has dedicated memory and is never part of the aliasing pass). Two copies of this would be
// two places for the layered/depth/array-view rules to drift apart.
bool VulkanFrameGraph::materialize_image(ImageNode& n)
{
    {
        const bool is_depth  = (n.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0U;
        const bool is_array  = n.desc.layers > 1U;

        // REN-2: the transient is a BORROWED render target (drawn into by a raster pass) — a VulkanRasterTarget view
        // over the graph-owned image+view (no memory, no readback; set_borrowed ⇒ its dtor frees nothing). If the
        // transient is `sampled`, it is ALSO a borrowed sampled texture a later pass reads via texture() (RTT).
        // REN-3.1: a DEPTH transient is a depth ATTACHMENT, not a colour one — hand the bundle to the target's
        // DEPTH slot so `depth_view()` resolves it and `record_depth_only` can render into it with no colour
        // attachment at all. (The colour path is unchanged: bundle → colour slot.)
        const auto make_target = [&](VkImageView v) {
            ImageBundle cb{};
            cb.image  = n.image;
            cb.view   = v;
            auto* t = is_depth ? new VulkanRasterTarget(m_device, ImageBundle{}, ImageBundle{}, cb,
                                                        BufferBundle{}, 1U, n.desc.width, n.desc.height)
                               : new VulkanRasterTarget(m_device, cb, ImageBundle{}, ImageBundle{},
                                                        BufferBundle{}, 1U, n.desc.width, n.desc.height);
            t->set_borrowed();
            return t;
        };

        // REN-3.2: the SAMPLING view spans every slice (VIEW_TYPE_2D_ARRAY when layered) — that is what a later
        // pass binds to select a cascade in the shader. Attachment views are made separately, one slice each,
        // because `vkCmdBeginRendering` renders into exactly the subresource its view names.
        VkImageViewCreateInfo vci{};
        vci.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image                       = n.image;
        vci.viewType                    = is_array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        vci.format                      = n.fmt;
        vci.subresourceRange.aspectMask = n.aspect;
        vci.subresourceRange.levelCount = 1U;
        vci.subresourceRange.layerCount = n.desc.layers;
        vkCreateImageView(m_device, &vci, nullptr, &n.view);

        if (is_array)
        {
            for (crd::u32 l = 0; l < n.desc.layers; ++l)
            {
                VkImageViewCreateInfo lci     = vci;
                lci.viewType                  = VK_IMAGE_VIEW_TYPE_2D;
                lci.subresourceRange.baseArrayLayer = l;
                lci.subresourceRange.layerCount     = 1U;
                VkImageView lv = VK_NULL_HANDLE;
                if (vkCreateImageView(m_device, &lci, nullptr, &lv) != VK_SUCCESS) { return false; }
                n.layer_views.push_back(lv);
                n.layer_targets.push_back(make_target(lv));
            }
            n.target = n.layer_targets[0]; // image() on a layered transient = slice 0 (never the array view)
        }
        else
        {
            n.target = make_target(n.view);
        }
        if (n.desc.sampled)
        {
            // The same image is ALSO a borrowed sampled view — the ARRAY view when layered. Vulkan needs no
            // format gymnastics for depth: one VK_FORMAT_D32_SFLOAT image serves attachment + sampling via
            // aspect + layout. (DX12 does; see the R32_TYPELESS dance in dx12_raster_context.cpp.)
            ImageBundle sb{};
            sb.image  = n.image;
            sb.view   = n.view;
            auto* tex = new VulkanTexture(m_device, sb, n.desc.width, n.desc.height);
            tex->set_borrowed();
            n.texture = tex;
        }
    }
    return true;
}

bool VulkanFrameGraph::build_tail(crd::u32 img_slot_end, crd::containers::Array<crd::u32>& border)
{
    for (crd::u32 idx : border)
    {
        BufferNode& n = m_buffers[idx];
        m_logical_bytes += static_cast<crd::u32>(n.mem_req.size);
        crd::i32 chosen = -1;
        for (crd::u32 si = img_slot_end; si < m_slots.size(); ++si)
        {
            Slot& s = m_slots[si];
            if (s.free_after < n.first_pass && (s.type_bits & n.mem_req.memoryTypeBits) != 0U) { chosen = static_cast<crd::i32>(si); break; }
        }
        if (chosen < 0)
        {
            Slot s{};
            s.type_bits = n.mem_req.memoryTypeBits;
            const crd::u32 mt = find_memory_type(s.type_bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (mt == 0xFFFFFFFFU) { return false; }
            s.size = n.mem_req.size;
            VkMemoryAllocateInfo mai{};
            mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            mai.allocationSize  = s.size;
            mai.memoryTypeIndex = mt;
            if (vkAllocateMemory(m_device, &mai, nullptr, &s.memory) != VK_SUCCESS) { return false; }
            m_slots.push_back(s);
            chosen = static_cast<crd::i32>(m_slots.size() - 1U);
            m_physical_bytes += static_cast<crd::u32>(s.size);
        }
        Slot& s      = m_slots[static_cast<crd::u32>(chosen)];
        s.free_after = n.last_pass;
        n.slot       = chosen;
        vkBindBufferMemory(m_device, n.vkbuf, s.memory, 0);
        n.buffer = new VulkanTransientBuffer(n.size);
    }

    return true;
}

// map a tracked layout to its (access, stage) for a barrier's SOURCE side
void layout_src(VkImageLayout layout, VkAccessFlags& access, VkPipelineStageFlags& stage) noexcept
{
    switch (layout)
    {
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; break;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        access = VK_ACCESS_TRANSFER_READ_BIT; stage = VK_PIPELINE_STAGE_TRANSFER_BIT; break;
    case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
        access = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        stage  = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT; break;
    default: access = 0; stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT; break; // UNDEFINED
    }
}

void VulkanFrameGraph::execute()
{
    // ⛔ REN-8: the deferred wait lands HERE, before the descriptor pool and command buffer are reset — both are
    // still being read by the previous frame's submission until its fence signals. This is the point where the
    // CPU/GPU overlap is actually cashed in: by now the GPU has usually finished, so the wait costs ~nothing.
    // REN-8: bind THIS frame's slot, then wait only on IT. With kFramesInFlight slots that fence is from
    // `kFramesInFlight - 1` frames ago and has almost always signalled, so the wait costs ~nothing — that is the
    // whole point of the ring versus a single buffer.
    FrameSlot& fs     = m_slots_if[m_slot];
    m_cmd             = fs.cmd;
    m_fence           = fs.fence;
    m_frame_desc_pool = fs.pool;
    m_ts_pool         = fs.ts;
    wait_pending_submit();
    m_submit_count = 0U;
    vkResetDescriptorPool(m_device, m_frame_desc_pool, 0);
    vkResetCommandBuffer(m_cmd, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(m_cmd, &bi);

    m_rc->frame_rec_begin(m_cmd, m_frame_desc_pool);
    // Every FRAME-LOCAL node starts the frame undefined: a transient is a brand-new image and an imported
    // target's layout is tracked by the raster context. This loop needs no exception for persistent images —
    // their live layout is not stored here at all (`live_layout`), so clearing these fields cannot reach it.
    for (ImageNode& n : m_images)
    {
        n.layout       = VK_IMAGE_LAYOUT_UNDEFINED;
        n.depth_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    // REN-8: a timestamp pool MUST be reset on the device before it is written, or results are undefined.
    // ⛔ Only the NAMES are cleared here. `m_timed_passes` / `m_gpu_ms_total` belong to `resolve_timestamps()`,
    // which — on the deferred-wait path — ran moments ago at the top of this call and published the PREVIOUS
    // frame's numbers. Zeroing them here wiped exactly those results, so the sandbox reported `0 passes` and
    // `0.000 ms` while timing was in fact working. One-frame-late timings are the price of CPU/GPU overlap;
    // no timings at all would just be a broken instrument.
    m_pass_names.clear();
    crd::u32 pass_index = 0U;
    if (m_ts_pool != VK_NULL_HANDLE) { vkCmdResetQueryPool(m_cmd, m_ts_pool, 0U, kMaxTimedPasses * 2U); }

    const auto img_barrier = [this](VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                                    VkAccessFlags dst_access, VkPipelineStageFlags dst_stage) {
        VkAccessFlags        src_access = 0;
        VkPipelineStageFlags src_stage  = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        layout_src(from, src_access, src_stage);
        VkImageMemoryBarrier b{};
        b.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout                   = from;
        b.newLayout                   = to;
        b.srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        b.image                       = image;
        b.subresourceRange.aspectMask = aspect;
        b.subresourceRange.levelCount = 1U;
        // REN-3.2: a layered transient is ONE graph node, so its transitions cover EVERY slice. Per-slice
        // layout tracking would need per-slice access in the DAG; whole-resource is conservative and always
        // correct, and the per-cascade writes are already ordered by the declared dependencies.
        b.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
        b.srcAccessMask               = src_access;
        b.dstAccessMask               = dst_access;
        vkCmdPipelineBarrier(m_cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1U, &b);
        ++m_barrier_count;
    };

    // ⛔ walk the DEPENDENCY-SORTED order from build(), not m_passes — see the topo-sort in build().
    for (const crd::u32 pass_idx : m_order)
    {
        Pass& p = m_passes[pass_idx];
        m_rc->frame_rec_new_pass();
        // barriers for imported render targets this pass writes / read-writes (transients aren't drawn in REN-1)
        for (const Access& a : p.img_access)
        {
            ImageNode& n = m_images[a.handle - 1U];
            if (n.target == nullptr) { continue; }
            const bool writes = (a.access != FgAccess::Read);
            if (graph_owned(n) && (n.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0U)
            {
                // REN-3.1: a DEPTH RTT transient — a depth-only pass renders into it (DEPTH_ATTACHMENT_OPTIMAL),
                // then a later pass SAMPLES it through the comparison sampler. Same shape as the colour RTT
                // barrier below, with the depth aspect + the early/late-fragment-test stages.
                // ⛔ ONE sampling layout, tracked in `depth_layout`: SHADER_READ_ONLY_OPTIMAL. It MUST equal the
                // layout the sampled-image DESCRIPTOR declares (`frame_alloc_sampled_set` writes
                // VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL). DEPTH_READ_ONLY_OPTIMAL is equally legal for sampling
                // a depth image, but mixing the two is not — the first run of the REN-3.1 gate failed on exactly
                // that (VUID-vkCmdDraw-imageLayout-00344), which is why the spec listed it as risk #2.
                auto& dt = static_cast<VulkanRasterTarget&>(*n.target);
                if (writes && live_depth_layout(n) != VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL)
                {
                    img_barrier(dt.depth_image(), VK_IMAGE_ASPECT_DEPTH_BIT, live_depth_layout(n),
                                VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
                    live_depth_layout(n) = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
                }
                else if (!writes && live_depth_layout(n) == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL)
                {
                    // the DEPTH RTT barrier: the shadow pass's depth writes complete → this pass samples it
                    img_barrier(dt.depth_image(), VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                    live_depth_layout(n) = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                }
                continue;
            }
            if (graph_owned(n)) // REN-2: an RTT image — render into it (COLOR_ATTACHMENT), then a later pass SAMPLES it
            {
                auto& tt = static_cast<VulkanRasterTarget&>(*n.target);
                if (writes && live_layout(n) != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
                {
                    img_barrier(tt.image(), VK_IMAGE_ASPECT_COLOR_BIT, live_layout(n),
                                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
                    live_layout(n) = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                }
                else if (!writes && live_layout(n) == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
                {
                    // the RTT barrier: the render pass's writes complete → this pass samples it
                    img_barrier(tt.image(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                    live_layout(n) = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                }
                continue;
            }
            auto& t = static_cast<VulkanRasterTarget&>(*n.target);
            if (p.present != nullptr) { continue; } // present-pass reads → the final readback loop transitions
            if (writes)
            {
                if (live_layout(n) != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
                {
                    img_barrier(t.image(), VK_IMAGE_ASPECT_COLOR_BIT, live_layout(n), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
                    live_layout(n) = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                }
                else // already an attachment (a prior pass wrote it): a WRITE→READ|WRITE cross-pass ordering barrier
                {
                    img_barrier(t.image(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
                }
                if (t.has_depth())
                {
                    const VkImageLayout dfrom = live_depth_layout(n);
                    img_barrier(t.depth_image(), VK_IMAGE_ASPECT_DEPTH_BIT, dfrom, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
                    live_depth_layout(n) = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
                }
            }
        }
        // REN-8: bracket THIS pass with device timestamps. BOTTOM_OF_PIPE for the end stamp means "after all
        // prior work completed", which is what makes the [start,end] delta the pass's own GPU cost rather than
        // a submission-relative wall-clock.
        const bool stamp = m_ts_pool != VK_NULL_HANDLE && pass_index < kMaxTimedPasses;
        if (stamp) { vkCmdWriteTimestamp(m_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_ts_pool, pass_index * 2U); }
        if (p.fn != nullptr) { p.fn(*this, p.user); }
        if (stamp)
        {
            vkCmdWriteTimestamp(m_cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_ts_pool, pass_index * 2U + 1U);
            m_pass_names.push_back(p.name);
        }
        ++pass_index;
    }

    // final readback: every imported target left in COLOR_ATTACHMENT → TRANSFER_SRC + copy to its readback (so
    // read_pixel is bit-identical to the sync path). The direct-to-backbuffer present is REN-8.
    for (ImageNode& n : m_images)
    {
        // Only APPLICATION-owned targets have a readback buffer — every graph-owned wrapper is a view over
        // graph memory built with an empty BufferBundle, and `read_pixel` is only ever about imported targets.
        if (!graph_owned(n) && n.target != nullptr && n.layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
        {
            if (m_readback)
            {
                m_rc->frame_readback(m_cmd, *n.target);
                ++m_barrier_count; // copy_colour_to_readback inserts a COLOR→TRANSFER_SRC barrier
            }
            else
            {
                // ⛔ REN-8: skipping the readback must skip the COPY, never the TRANSITION. The readback did two
                // things — a 3.7 MB host copy AND a COLOR_ATTACHMENT → TRANSFER_SRC move — and the present path
                // depends on the second: its compose descriptor declares TRANSFER_SRC_OPTIMAL (the RET-2
                // contract). Dropping both left the target in COLOR_ATTACHMENT and produced
                // VUID-vkCmdDraw-None-09600 on every presented frame.
                // This went unnoticed because every GATE runs with readback ON — only the sandbox, which turns
                // it off, was affected, and only under validation. Cheap fix: the barrier without the copy.
                img_barrier(static_cast<VulkanRasterTarget&>(*n.target).image(), VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            }
            n.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        }
    }

    m_rc->frame_rec_end();
    vkEndCommandBuffer(m_cmd);
    VkSubmitInfo si{};
    si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1U;
    si.pCommandBuffers    = &m_cmd;
    vkResetFences(m_device, 1U, &m_fence);
    vkQueueSubmit(m_queue, 1U, &si, m_fence);
    m_submit_count = 1U;

    // ⛔ REN-8: THE STALL. Waiting here blocks the CPU on the GPU every single frame — measured at ~4.9 ms of a
    // ~12.5 ms sandbox frame, against only ~1.5 ms of actual pass work. The wait exists so `read_pixel` is valid
    // the instant execute() returns, which is a TEST affordance, exactly like the readback copy.
    //
    // So: when readback is on, wait here (every gate keeps its exact semantics). When it is off, DEFER the wait
    // to the top of the next execute()/reset(). The GPU then renders frame N while the CPU builds frame N+1 —
    // one frame of overlap, which is the whole point of a submitted-once frame graph.
    //
    // ⛔ Deferring is only SAFE because every destructive operation is behind `wait_pending()`: reset() frees
    // transients, the dtor tears down the pool, and execute() re-records the one command buffer. A deferred wait
    // that skipped any of those would be a use-after-free on in-flight GPU work, not a speedup.
    fs.pending       = true;
    m_pending_submit = true;
    m_last_slot      = static_cast<crd::i32>(m_slot);
    if (m_readback)
    {
        wait_pending_submit(); // gates read pixels the instant execute() returns — unchanged semantics
    }
    else
    {
        m_slot = (m_slot + 1U) % kFramesInFlight; // advance ONLY when pipelining; the readback path stays in-place
    }
}

// REN-8: block until the in-flight submission (if any) has completed, then resolve its timestamps. Called at the
// top of execute()/reset() and from the dtor — every point where the graph is about to touch something the GPU
// could still be reading.
void VulkanFrameGraph::wait_pending_submit() noexcept
{
    FrameSlot& fs = m_slots_if[m_slot];
    if (!fs.pending) { return; }
    vkWaitForFences(m_device, 1U, &fs.fence, VK_TRUE, ~0ULL);
    fs.pending       = false;
    m_pending_submit = false;
    drain_retired(fs);
    resolve_timestamps();
}

void VulkanFrameGraph::wait_all_slots() noexcept
{
    for (crd::u32 s = 0; s < kFramesInFlight; ++s)
    {
        FrameSlot& fs = m_slots_if[s];
        if (fs.pending)
        {
            vkWaitForFences(m_device, 1U, &fs.fence, VK_TRUE, ~0ULL);
            fs.pending = false;
        }
        drain_retired(fs);
    }
    m_pending_submit = false;
}

// Hand the graph's CURRENTLY-LIVE transients to `slot`, which will destroy them once its fence signals. The node
// arrays keep their handles nulled so a later free_transients() cannot double-free them.
void VulkanFrameGraph::retire_transients_to(FrameSlot& slot) noexcept
{
    for (ImageNode& n : m_images)
    {
        if (!aliasable(n)) { continue; }
        if (n.texture != nullptr) { slot.dead_textures.push_back(n.texture); n.texture = nullptr; }
        // a layered transient's `target` ALIASES layer_targets[0] — retire the per-slice ones and null `target`
        // FIRST, exactly as free_transients() does, or the slice-0 wrapper is destroyed twice.
        for (IRasterTarget* lt : n.layer_targets) { slot.dead_targets.push_back(lt); }
        if (n.layer_targets.size() > 0) { n.target = nullptr; }
        n.layer_targets.clear();
        for (VkImageView lv : n.layer_views) { slot.dead_views.push_back(lv); }
        n.layer_views.clear();
        if (n.target != nullptr) { slot.dead_targets.push_back(n.target); n.target = nullptr; }
        if (n.view != VK_NULL_HANDLE) { slot.dead_views.push_back(n.view); n.view = VK_NULL_HANDLE; }
        if (n.image != VK_NULL_HANDLE) { slot.dead_images.push_back(n.image); n.image = VK_NULL_HANDLE; }
    }
    for (BufferNode& n : m_buffers)
    {
        if (n.transient && n.vkbuf != VK_NULL_HANDLE) { slot.dead_buffers.push_back(n.vkbuf); n.vkbuf = VK_NULL_HANDLE; }
    }
    for (Slot& s : m_slots) { if (s.memory != VK_NULL_HANDLE) { slot.dead_memory.push_back(s.memory); } }
    m_slots.clear();
    m_physical_bytes = 0U;
    m_logical_bytes  = 0U;
}

// ⛔ ORDER MATTERS: views before their images, and the backing memory LAST — freeing a VkDeviceMemory that an
// undestroyed VkImage is still bound to is undefined behaviour.
void VulkanFrameGraph::drain_retired(FrameSlot& slot) noexcept
{
    for (ITexture* t : slot.dead_textures) { delete t; }
    for (IRasterTarget* t : slot.dead_targets) { delete t; }
    for (VkImageView v : slot.dead_views) { vkDestroyImageView(m_device, v, nullptr); }
    for (VkImage i : slot.dead_images) { vkDestroyImage(m_device, i, nullptr); }
    for (VkBuffer b : slot.dead_buffers) { vkDestroyBuffer(m_device, b, nullptr); }
    for (VkDeviceMemory m : slot.dead_memory) { vkFreeMemory(m_device, m, nullptr); }
    slot.dead_textures.clear();
    slot.dead_targets.clear();
    slot.dead_views.clear();
    slot.dead_images.clear();
    slot.dead_buffers.clear();
    slot.dead_memory.clear();
}

void VulkanFrameGraph::resolve_timestamps() noexcept
{
    // REN-8: the fence has signalled, so every timestamp is resolvable. Ticks → ms via the device's
    // timestampPeriod (ns/tick).
    m_timed_passes = 0U;
    if (m_ts_pool != VK_NULL_HANDLE && !m_pass_names.empty())
    {
        const crd::u32 n = static_cast<crd::u32>(m_pass_names.size());
        crd::u64       raw[kMaxTimedPasses * 2U]{};
        if (vkGetQueryPoolResults(m_device, m_ts_pool, 0U, n * 2U, sizeof(crd::u64) * n * 2U,
                                  static_cast<void*>(raw), sizeof(crd::u64),
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT)
            == VK_SUCCESS)
        {
            const double ns_per_tick = m_ts_period;
            for (crd::u32 i = 0; i < n; ++i)
            {
                const crd::u64 a = raw[i * 2U];
                const crd::u64 b = raw[i * 2U + 1U];
                m_pass_ms[i]     = b > a ? (static_cast<double>(b - a) * ns_per_tick) / 1.0e6 : 0.0;
            }
            m_gpu_ms_total = raw[n * 2U - 1U] > raw[0]
                                 ? (static_cast<double>(raw[n * 2U - 1U] - raw[0]) * ns_per_tick) / 1.0e6
                                 : 0.0;
            m_timed_passes = n;
        }
    }
}

std::unique_ptr<IFrameGraph> VulkanRasterContext::create_frame_graph()
{
    return std::make_unique<VulkanFrameGraph>(*this);
}

} // namespace

crd::u32 vulkan_raster_block_count(const IRasterContext& raster) noexcept
{
    return static_cast<const VulkanRasterContext&>(raster).gpu_block_count();
}

crd::u32 vulkan_raster_compact(IRasterContext& raster) noexcept
{
    return static_cast<VulkanRasterContext&>(raster).gpu_compact();
}

crd::u32 vulkan_raster_defragment(IRasterContext& raster) noexcept
{
    return static_cast<VulkanRasterContext&>(raster).defragment_resources();
}

crd::u32 vulkan_present_image_count(const IPresentSurface& surface) noexcept
{
    return static_cast<const VulkanPresentSurface&>(surface).image_count();
}

crd::u32 vulkan_present_color_format_raw(const IPresentSurface& surface) noexcept
{
    return static_cast<crd::u32>(static_cast<const VulkanPresentSurface&>(surface).color_format());
}

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
