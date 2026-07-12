// vulkan_context.cpp — the headless Vulkan compute context (ADR-0099, v17-i-a). Raw Vulkan: instance (1.3, no surface)
// → discrete physical device → logical device with a compute queue + the cooperative-matrix / coopmat2 / fp16 /
// 16-bit-storage / memory-model feature chain (guarded by adapter support). No rendering, no swapchain.

#include <crd/gpu/vulkan_context.hpp>
#include <crd/gpu/vulkan_shader_compile.hpp> // the relocated GLSL→SPIR-V compiler (C1-c graph on-ramp)

#include <crd/core/platform.hpp> // CRD_OS_* for the platform surface extension (C2-a)

#include <crd/kir/ckir.hpp>      // KGraph / KEntry / KStage (ADR-0103 IR currency)
#include <crd/kir/ckir_glsl.hpp> // the crd-kir GLSL emitter: emit_(vec|elementwise)_glsl + graph_uses_vec

#include <crd/containers/array.hpp>
#include <crd/containers/string_view.hpp> // to_view
#include <crd/memory/allocator.hpp>

#include <cstdint>
#include <cstring>

namespace crd::gpu
{
namespace
{

// ADR-0103: the opaque program handle. Owns a VkShaderModule + RETAINS the cooked SPIR-V (so a raster consumer can build
// a `VkShaderEXT` shader object from it — D-008 C1-b) + the stage it was cooked for. This is the OUT currency of the
// shader seam — a portable consumer holds this, never SPIR-V and never GLSL.
class VulkanGpuProgramImpl final : public VulkanGpuProgram
{
public:
    VulkanGpuProgramImpl(VkDevice device, VkShaderModule module, ShaderStage stage,
                         crd::containers::ConstSpan<crd::u8> spirv) noexcept
        : m_device(device), m_module(module), m_stage(stage), m_spirv(crd::memory::default_allocator())
    {
        m_spirv.resize(spirv.size());
        for (crd::usize i = 0; i < spirv.size(); ++i) { m_spirv[i] = spirv[i]; }
    }
    ~VulkanGpuProgramImpl() override
    {
        if (m_module != VK_NULL_HANDLE) { vkDestroyShaderModule(m_device, m_module, nullptr); }
    }
    VulkanGpuProgramImpl(const VulkanGpuProgramImpl&)            = delete;
    VulkanGpuProgramImpl& operator=(const VulkanGpuProgramImpl&) = delete;
    VulkanGpuProgramImpl(VulkanGpuProgramImpl&&)                 = delete;
    VulkanGpuProgramImpl& operator=(VulkanGpuProgramImpl&&)      = delete;

    [[nodiscard]] bool           valid() const noexcept override { return m_module != VK_NULL_HANDLE; }
    [[nodiscard]] ShaderStage    stage() const noexcept override { return m_stage; }
    [[nodiscard]] VkShaderModule vk_module() const noexcept override { return m_module; }
    [[nodiscard]] crd::containers::ConstSpan<crd::u8> vk_spirv() const noexcept override
    {
        return crd::containers::ConstSpan<crd::u8>(m_spirv.data(), m_spirv.size());
    }

private:
    VkDevice                        m_device = VK_NULL_HANDLE;
    VkShaderModule                  m_module = VK_NULL_HANDLE;
    ShaderStage                     m_stage  = ShaderStage::Compute;
    crd::containers::Array<crd::u8> m_spirv;
};

class VulkanGpuContextImpl final : public VulkanGpuContext
{
public:
    explicit VulkanGpuContextImpl(const GpuContextConfig& config) { init(config); }
    ~VulkanGpuContextImpl() override
    {
        if (m_device != VK_NULL_HANDLE) { vkDestroyDevice(m_device, nullptr); }
        if (m_instance != VK_NULL_HANDLE) { vkDestroyInstance(m_instance, nullptr); }
    }

    [[nodiscard]] bool             valid() const noexcept override { return m_valid; }
    [[nodiscard]] GpuBackend       backend() const noexcept override { return GpuBackend::Vulkan; }
    [[nodiscard]] const char*      adapter_name() const noexcept override { return m_name; }
    [[nodiscard]] VkInstance       vk_instance() const noexcept override { return m_instance; }
    [[nodiscard]] VkPhysicalDevice vk_physical_device() const noexcept override { return m_physical; }
    [[nodiscard]] VkDevice         vk_device() const noexcept override { return m_device; }
    [[nodiscard]] VkQueue          compute_queue() const noexcept override { return m_compute_queue; }
    [[nodiscard]] crd::u32         compute_family() const noexcept override { return m_compute_family; }
    [[nodiscard]] bool             cooperative_matrix2() const noexcept override { return m_coopmat2; }
    [[nodiscard]] bool             shader_int64() const noexcept override { return m_int64; }

    [[nodiscard]] bool     graphics_capable() const noexcept override { return m_graphics_family != UINT32_MAX; }
    [[nodiscard]] VkQueue  graphics_queue() const noexcept override { return m_graphics_queue; }
    [[nodiscard]] crd::u32 graphics_family() const noexcept override { return m_graphics_family; }
    [[nodiscard]] bool     shader_object() const noexcept override { return m_shader_object; }
    [[nodiscard]] bool     render_capable() const noexcept override { return m_windowed; }
    [[nodiscard]] bool       fragment_shading_rate() const noexcept override { return m_fragment_shading_rate; } // B1-e
    [[nodiscard]] VkExtent2D vrs_tile_size() const noexcept override { return m_vrs_tile_size; }
    [[nodiscard]] bool       conservative_raster() const noexcept override { return m_conservative_raster; } // B1-f
    [[nodiscard]] bool       fragment_shader_interlock() const noexcept override { return m_fragment_interlock; } // B1-f
    [[nodiscard]] bool       bindless() const noexcept override { return m_bindless; } // B2-d

    [[nodiscard]] std::unique_ptr<IGpuProgram>
    create_program(ShaderStage stage, crd::containers::ConstSpan<crd::u8> cooked) override
    {
        // The ONE program constructor is the free `make_vulkan_program` factory (D-008 C2-d4) — the standalone rhi-vulkan
        // device (bare VkDevice, no context) routes through the same path so `ShaderModule` can retire (ADR-0103).
        return make_vulkan_program(m_device, stage, cooked);
    }

    // ADR-0103 IR on-ramp: emit GLSL from the graph (crd-kir's emitter) → SPIR-V (our compiler) → program. COMPUTE and
    // RASTER (D-007 B3-c: Vertex/Fragment via `emit_stage_glsl`); a stage/op this backend cannot lower returns nullptr, loudly.
    [[nodiscard]] std::unique_ptr<IGpuProgram>
    create_program(const crd::kir::KGraph& graph, const crd::kir::KEntry& entry) override
    {
        crd::memory::IAllocator* a = crd::memory::default_allocator();

        // B3-c: raster stages behind the SAME seam. `entry_valid` first — e.g. a `FragCoord` in a vertex entry is rejected.
        if (entry.stage == crd::kir::KStage::Vertex || entry.stage == crd::kir::KStage::Fragment)
        {
            if (!crd::kir::entry_valid(graph, entry)) { return nullptr; }
            crd::kir::GlslKernel kern(a);
            if (!crd::kir::emit_stage_glsl(graph, entry, a, kern)) { return nullptr; }
            const ShaderStage stage =
                (entry.stage == crd::kir::KStage::Vertex) ? ShaderStage::Vertex : ShaderStage::Fragment;
            const auto spv = compile_glsl_to_spirv(stage, crd::containers::to_view(kern.source), "ckir_stage", a);
            if (!spv.ok) { return nullptr; }
            return create_program(stage, crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()));
        }

        // Compute: the fused elementwise / vec-aware kernel path.
        if (entry.stage != crd::kir::KStage::Compute || entry.n_out < 1) { return nullptr; }
        const int output = entry.out[0].node;
        if (output < 0 || output >= graph.size()) { return nullptr; }

        crd::kir::GlslKernel kern(a);
        const bool           ok = crd::kir::graph_uses_vec(graph, output, a)
                                      ? crd::kir::emit_vec_glsl(graph, output, a, kern)
                                      : crd::kir::emit_elementwise_glsl(graph, output, a, kern);
        if (!ok) { return nullptr; } // a compute class this backend's emitter does not lower yet

        const auto spv = compile_glsl_to_spirv(ShaderStage::Compute, crd::containers::to_view(kern.source), "ckir", a);
        if (!spv.ok) { return nullptr; }
        return create_program(ShaderStage::Compute,
                              crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()));
    }

private:
    void init(const GpuContextConfig& config)
    {
        VkApplicationInfo app{};
        app.sType         = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pEngineName   = "Cerid";
        app.apiVersion    = VK_API_VERSION_1_3;
        VkInstanceCreateInfo ici{};
        ici.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ici.pApplicationInfo = &app;
        const char* layers[] = {"VK_LAYER_KHRONOS_validation"};
        if (config.enable_validation) { ici.enabledLayerCount = 1; ici.ppEnabledLayerNames = layers; }

        // C2-a: a WINDOWED context enables the surface instance extensions so the ONE device can present (ADR-0099).
        // Guarded + additive — headless (compute) leaves the instance byte-for-byte unchanged. Only enabled if available.
#if CRD_OS_WINDOWS
        const char* platform_surface = "VK_KHR_win32_surface";
#elif CRD_OS_LINUX
        const char* platform_surface = "VK_KHR_xcb_surface";
#else
        const char* platform_surface = nullptr;
#endif
        const char* inst_exts[2]  = {"VK_KHR_surface", platform_surface};
        bool        surface_ok    = false;
        if (!config.headless && platform_surface != nullptr)
        {
            std::uint32_t nie = 0;
            vkEnumerateInstanceExtensionProperties(nullptr, &nie, nullptr);
            auto iavail = std::make_unique<VkExtensionProperties[]>(nie == 0 ? 1 : nie);
            vkEnumerateInstanceExtensionProperties(nullptr, &nie, iavail.get());
            bool has_surf = false;
            bool has_plat = false;
            for (std::uint32_t i = 0; i < nie; ++i)
            {
                if (std::strcmp(iavail[i].extensionName, "VK_KHR_surface") == 0) { has_surf = true; }
                if (std::strcmp(iavail[i].extensionName, platform_surface) == 0) { has_plat = true; }
            }
            surface_ok = has_surf && has_plat;
        }
        if (surface_ok)
        {
            ici.enabledExtensionCount   = 2;
            ici.ppEnabledExtensionNames = inst_exts;
        }
        if (vkCreateInstance(&ici, nullptr, &m_instance) != VK_SUCCESS) { return; }

        std::uint32_t    npd = 16;
        VkPhysicalDevice pds[16];
        if (vkEnumeratePhysicalDevices(m_instance, &npd, pds) != VK_SUCCESS || npd == 0) { return; }
        m_physical = pds[0];
        for (std::uint32_t i = 0; i < npd; ++i)
        {
            VkPhysicalDeviceProperties pr{};
            vkGetPhysicalDeviceProperties(pds[i], &pr);
            if (pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { m_physical = pds[i]; break; }
        }
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(m_physical, &props);
        for (int i = 0; i < 255 && props.deviceName[i] != '\0'; ++i) { m_name[i] = props.deviceName[i]; } // m_name zero-init

        // Compute queue family — prefer a DEDICATED compute family (async vs a renderer), else any compute-capable one.
        std::uint32_t           nqf = 16;
        VkQueueFamilyProperties qf[16];
        vkGetPhysicalDeviceQueueFamilyProperties(m_physical, &nqf, qf);
        std::uint32_t any_compute  = UINT32_MAX;
        std::uint32_t dedicated    = UINT32_MAX;
        std::uint32_t any_graphics = UINT32_MAX;
        for (std::uint32_t i = 0; i < nqf; ++i)
        {
            if ((qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0U)
            {
                if (any_compute == UINT32_MAX) { any_compute = i; }
                if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0U && dedicated == UINT32_MAX) { dedicated = i; }
            }
            // D-008 C1: a GRAPHICS family for IRasterContext (distinct from the async-compute queue where possible).
            if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0U && any_graphics == UINT32_MAX) { any_graphics = i; }
        }
        m_compute_family  = (dedicated != UINT32_MAX) ? dedicated : any_compute;
        m_graphics_family = any_graphics; // UINT32_MAX ⇒ compute-only adapter; raster disabled but compute still works
        if (m_compute_family == UINT32_MAX) { return; }

        // Cooperative matrix (tensor cores) — enable coopmat + coopmat2 + fp16/16-bit/memory-model IF the adapter has them.
        std::uint32_t ne = 0;
        vkEnumerateDeviceExtensionProperties(m_physical, nullptr, &ne, nullptr);
        auto exts = std::make_unique<VkExtensionProperties[]>(ne == 0 ? 1 : ne);
        vkEnumerateDeviceExtensionProperties(m_physical, nullptr, &ne, exts.get());
        bool has_cm1       = false;
        bool has_cm2       = false;
        bool has_shobj     = false;
        bool has_swapchain = false;
        bool has_vrs       = false;
        bool has_conserv   = false;
        bool has_eds3      = false;
        bool has_interlock = false;
        for (std::uint32_t i = 0; i < ne; ++i)
        {
            if (std::strcmp(exts[i].extensionName, "VK_KHR_cooperative_matrix") == 0) { has_cm1 = true; }
            if (std::strcmp(exts[i].extensionName, "VK_NV_cooperative_matrix2") == 0) { has_cm2 = true; }
            if (std::strcmp(exts[i].extensionName, VK_EXT_SHADER_OBJECT_EXTENSION_NAME) == 0) { has_shobj = true; }
            if (std::strcmp(exts[i].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) { has_swapchain = true; }
            if (std::strcmp(exts[i].extensionName, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME) == 0) { has_vrs = true; }
            if (std::strcmp(exts[i].extensionName, VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME) == 0) { has_conserv = true; }
            if (std::strcmp(exts[i].extensionName, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME) == 0) { has_eds3 = true; }
            if (std::strcmp(exts[i].extensionName, VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME) == 0) { has_interlock = true; }
        }
        m_coopmat2      = has_cm1 && has_cm2;
        m_shader_object = has_shobj && m_graphics_family != UINT32_MAX; // no point on a compute-only adapter
        m_fragment_shading_rate = has_vrs && m_graphics_family != UINT32_MAX; // B1-e: raster-only feature
        // B1-f: conservative raster needs its extension AND the extended-dynamic-state-3 conservative-mode setter (the
        // shader-object model has no static pipeline state). Graphics-capable only.
        m_conservative_raster = has_conserv && has_eds3 && m_shader_object;
        // B1-f: pixel-ordered fragment-shader interlock (ROV) — graphics-capable only. The feature is checked below.
        m_fragment_interlock = has_interlock && m_graphics_family != UINT32_MAX;
        // C2-a: render-capable iff surface (instance) + swapchain (device) + a graphics queue all present.
        m_windowed = surface_ok && has_swapchain && m_graphics_family != UINT32_MAX;

        if (m_fragment_shading_rate) // B1-e: the attachment shading-rate-image texel size (device-reported)
        {
            VkPhysicalDeviceFragmentShadingRatePropertiesKHR sr_props{};
            sr_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR;
            VkPhysicalDeviceProperties2 props2{};
            props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            props2.pNext = &sr_props;
            vkGetPhysicalDeviceProperties2(m_physical, &props2);
            m_vrs_tile_size = sr_props.minFragmentShadingRateAttachmentTexelSize;
        }

        // shaderInt64 — the geometry 60-bit Morton / LBVH paths need u64 in the shader. Enabled if the adapter has it.
        VkPhysicalDeviceFeatures avail_feats{};
        vkGetPhysicalDeviceFeatures(m_physical, &avail_feats);
        m_int64 = avail_feats.shaderInt64 == VK_TRUE;
        VkPhysicalDeviceFeatures enabled_feats{};
        enabled_feats.shaderInt64 = avail_feats.shaderInt64;
        // B1-c: a `sample`-qualified fragment interpolant lowers to SPIR-V that declares the SampleRateShading capability,
        // which REQUIRES this device feature — without it, creating the shader is a validation error (a lenient driver may
        // still run it, but a strict one rejects it). A raster-only feature ⇒ enabled only for a graphics-capable context,
        // leaving a pure-compute device unchanged (the C2 convergence keeps that device minimal).
        if (m_graphics_family != UINT32_MAX) { enabled_feats.sampleRateShading = avail_feats.sampleRateShading; }
        // B1-f: a fragment shader that WRITES a storage buffer (the interlock RMW / OIT path) needs this feature — without
        // it the SPIR-V must mark every fragment-stage storage variable NonWritable (VUID-RuntimeSpirv-NonWritable-06340).
        if (m_graphics_family != UINT32_MAX) { enabled_feats.fragmentStoresAndAtomics = avail_feats.fragmentStoresAndAtomics; }
        // B2-c: a CUBE-ARRAY texture (view + the SampledCubeArray SPIR-V capability) needs this feature (VUID-...-viewType-01004
        // / VUID-...-pCode-08740). Graphics-capable only.
        if (m_graphics_family != UINT32_MAX) { enabled_feats.imageCubeArray = avail_feats.imageCubeArray; }
        // C2-c: a WINDOWED context matches what rhi-vulkan's own device enables so the renderer runs on the adopted
        // device unchanged — fillModeNonSolid (wireframe) here + synchronization2 in the feature chain below.
        if (m_windowed) { enabled_feats.fillModeNonSolid = avail_feats.fillModeNonSolid; }

        // Queues: the async-compute queue, plus a GRAPHICS queue for IRasterContext when the families differ (NVIDIA has
        // a dedicated compute family, so this is two distinct families → two VkDeviceQueueCreateInfo).
        const float             qp = 1.0F;
        VkDeviceQueueCreateInfo qcis[2]{};
        crd::u32                nqci      = 0;
        qcis[nqci].sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qcis[nqci].queueFamilyIndex = m_compute_family;
        qcis[nqci].queueCount       = 1;
        qcis[nqci].pQueuePriorities = &qp;
        ++nqci;
        if (m_graphics_family != UINT32_MAX && m_graphics_family != m_compute_family)
        {
            qcis[nqci].sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            qcis[nqci].queueFamilyIndex = m_graphics_family;
            qcis[nqci].queueCount       = 1;
            qcis[nqci].pQueuePriorities = &qp;
            ++nqci;
        }

        VkPhysicalDevice16BitStorageFeatures s16{};
        s16.sType                   = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES;
        s16.storageBuffer16BitAccess = VK_TRUE;
        VkPhysicalDeviceShaderFloat16Int8Features f16{};
        f16.sType         = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES;
        f16.shaderFloat16 = VK_TRUE;
        f16.pNext         = &s16;
        VkPhysicalDeviceVulkanMemoryModelFeatures vmm{};
        vmm.sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES;
        vmm.vulkanMemoryModel = VK_TRUE;
        vmm.pNext            = &f16;
        VkPhysicalDeviceCooperativeMatrixFeaturesKHR cmk{};
        cmk.sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR;
        cmk.cooperativeMatrix = VK_TRUE;
        cmk.pNext            = &vmm;
        VkPhysicalDeviceCooperativeMatrix2FeaturesNV cm2{};
        cm2.sType                              = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_2_FEATURES_NV;
        cm2.cooperativeMatrixWorkgroupScope    = VK_TRUE;
        cm2.cooperativeMatrixFlexibleDimensions = VK_TRUE;

        // D-008 C1 chain (always): DYNAMIC RENDERING (core 1.3 feature, the modern no-render-pass raster path) + SHADER
        // OBJECTS (VK_EXT_shader_object) when present — the frontier pipeline model. The coopmat chain links in after.
        VkPhysicalDeviceShaderObjectFeaturesEXT sho{};
        sho.sType        = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT;
        sho.shaderObject = VK_TRUE;
        VkPhysicalDeviceDynamicRenderingFeatures dyn{};
        dyn.sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
        dyn.dynamicRendering = VK_TRUE;
        // B1-b: a fragment `discard` lowers (modern glslang/DXC) to the DemoteToHelperInvocation SPIR-V capability, which
        // needs this 1.3-core feature — creating such a shader without it is a validation error. Graphics-capable only.
        VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures demote{};
        demote.sType                          = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES;
        demote.shaderDemoteToHelperInvocation = VK_TRUE;
        // C2-c: synchronization2 (core 1.3) — enabled for a WINDOWED context to match rhi-vulkan's device (its render
        // path uses sync2 barriers). Left off for headless/compute so that device stays byte-for-byte unchanged.
        VkPhysicalDeviceSynchronization2Features sync2{};
        sync2.sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
        sync2.synchronization2 = VK_TRUE;
        // B1-e: variable-rate shading — the three rate sources (pipeline per-draw · primitive shader-output · attachment
        // per-tile image). Graphics-capable only.
        VkPhysicalDeviceFragmentShadingRateFeaturesKHR vrs{};
        vrs.sType                          = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR;
        vrs.pipelineFragmentShadingRate    = VK_TRUE;
        vrs.primitiveFragmentShadingRate   = VK_TRUE;
        vrs.attachmentFragmentShadingRate  = VK_TRUE;
        // B1-f: the extended-dynamic-state-3 CONSERVATIVE-RASTERIZATION-MODE dynamic state (shader objects have no static
        // pipeline state, so the mode is only reachable as dynamic state). Only this one EDS3 feature is enabled.
        VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3{};
        eds3.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT;
        eds3.extendedDynamicState3ConservativeRasterizationMode     = VK_TRUE;
        // Overestimate raster with shader objects ALSO requires the extra-primitive-overestimation-size dynamic state to be
        // set before a draw (VUID-vkCmdDraw-None-07632) — so enable that EDS3 sub-feature too and set it in draw_conservative.
        eds3.extendedDynamicState3ExtraPrimitiveOverestimationSize = VK_TRUE;
        // B1-f: pixel-ordered fragment-shader interlock (ROV). Confirm the specific sub-feature is present (the extension
        // can be exposed with only sample/shading-rate ordering) before enabling; a false bit disables the interlock path.
        VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT interlock{};
        interlock.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT;
        if (m_fragment_interlock)
        {
            VkPhysicalDeviceFeatures2 f2{};
            f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            f2.pNext = &interlock;
            vkGetPhysicalDeviceFeatures2(m_physical, &f2);
            m_fragment_interlock = interlock.fragmentShaderPixelInterlock == VK_TRUE;
            interlock.fragmentShaderSampleInterlock      = VK_FALSE; // enable ONLY the pixel-ordered mode we emit
            interlock.fragmentShaderShadingRateInterlock = VK_FALSE;
        }
        // B2-d: BINDLESS — non-uniform sampled-image array indexing (Vulkan 1.2 core descriptor indexing). Query the bit,
        // enable only what the bindless texture path needs. Graphics-capable only (keeps the compute device minimal).
        VkPhysicalDeviceDescriptorIndexingFeatures descidx{};
        descidx.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
        if (m_graphics_family != UINT32_MAX)
        {
            VkPhysicalDeviceFeatures2 f2{};
            f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            f2.pNext = &descidx;
            vkGetPhysicalDeviceFeatures2(m_physical, &f2);
            m_bindless = descidx.shaderSampledImageArrayNonUniformIndexing == VK_TRUE;
            VkPhysicalDeviceDescriptorIndexingFeatures keep{};
            keep.sType                                       = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
            keep.shaderSampledImageArrayNonUniformIndexing   = descidx.shaderSampledImageArrayNonUniformIndexing;
            descidx                                          = keep; // enable ONLY the non-uniform sampled-image bit
        }

        // Build the pNext chain head-first: dyn → [demote] → [vrs] → [eds3] → [sync2] → [sho] → [coopmat…].
        void* chain = &dyn;
        if (m_graphics_family != UINT32_MAX) { demote.pNext = chain; chain = &demote; } // raster `discard` support
        if (m_fragment_shading_rate) { vrs.pNext = chain; chain = &vrs; }
        if (m_conservative_raster) { eds3.pNext = chain; chain = &eds3; }
        if (m_fragment_interlock) { interlock.pNext = chain; chain = &interlock; }
        if (m_bindless) { descidx.pNext = chain; chain = &descidx; }
        if (m_windowed) { sync2.pNext = chain; chain = &sync2; }
        if (m_shader_object) { sho.pNext = chain; chain = &sho; }
        if (m_coopmat2) { cm2.pNext = chain; cmk.pNext = &cm2; chain = &cmk; }

        const char* devexts[8];
        crd::u32    ndevext = 0;
        if (m_coopmat2)
        {
            devexts[ndevext++] = "VK_KHR_cooperative_matrix";
            devexts[ndevext++] = "VK_NV_cooperative_matrix2";
        }
        if (m_shader_object) { devexts[ndevext++] = VK_EXT_SHADER_OBJECT_EXTENSION_NAME; }
        if (m_windowed) { devexts[ndevext++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME; }
        if (m_fragment_shading_rate) { devexts[ndevext++] = VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME; }
        if (m_conservative_raster)
        {
            devexts[ndevext++] = VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME;
            devexts[ndevext++] = VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME;
        }
        if (m_fragment_interlock) { devexts[ndevext++] = VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME; }

        VkDeviceCreateInfo dci{};
        dci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount    = nqci;
        dci.pQueueCreateInfos       = qcis;
        dci.pEnabledFeatures        = &enabled_feats; // shaderInt64 (no VkPhysicalDeviceFeatures2 in the pNext chain)
        dci.pNext                   = chain;
        dci.enabledExtensionCount   = ndevext;
        dci.ppEnabledExtensionNames = (ndevext != 0U) ? devexts : nullptr;

        if (vkCreateDevice(m_physical, &dci, nullptr, &m_device) != VK_SUCCESS) { return; }
        vkGetDeviceQueue(m_device, m_compute_family, 0, &m_compute_queue);
        if (m_graphics_family != UINT32_MAX)
        {
            vkGetDeviceQueue(m_device, m_graphics_family, 0, &m_graphics_queue);
        }
        m_valid = true;
    }

    VkInstance       m_instance        = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical        = VK_NULL_HANDLE;
    VkDevice         m_device          = VK_NULL_HANDLE;
    VkQueue          m_compute_queue   = VK_NULL_HANDLE;
    crd::u32         m_compute_family  = UINT32_MAX;
    VkQueue          m_graphics_queue  = VK_NULL_HANDLE;
    crd::u32         m_graphics_family = UINT32_MAX;
    bool             m_coopmat2        = false;
    bool             m_int64           = false;
    bool             m_shader_object   = false;
    bool             m_windowed        = false;
    bool             m_fragment_shading_rate = false; // B1-e: VK_KHR_fragment_shading_rate enabled
    VkExtent2D       m_vrs_tile_size{};               // B1-e: attachment shading-rate-image texel size
    bool             m_conservative_raster   = false; // B1-f: conservative raster + EDS3 conservative mode enabled
    bool             m_fragment_interlock    = false; // B1-f: pixel-ordered fragment-shader interlock (ROV) enabled
    bool             m_bindless              = false; // B2-d: non-uniform sampled-image array indexing enabled
    bool             m_valid           = false;
    char             m_name[256]       = {};
};

} // namespace

std::unique_ptr<IGpuContext> create_vulkan_gpu_context(const GpuContextConfig& config)
{
    if (config.backend != GpuBackend::Vulkan) { return nullptr; }
    auto ctx = std::make_unique<VulkanGpuContextImpl>(config);
    if (!ctx->valid()) { return nullptr; }
    return ctx;
}

// D-008 C2-d4 — the ONE program constructor (see header). Cooked bytecode is SPIR-V: a stream of 32-bit words; reject
// anything that cannot be one. Defined in `crd::gpu` (external linkage) but sees the anon-namespace `VulkanGpuProgramImpl`
// because it lives in the same TU.
std::unique_ptr<IGpuProgram>
make_vulkan_program(VkDevice device, ShaderStage stage, crd::containers::ConstSpan<crd::u8> cooked)
{
    if (device == VK_NULL_HANDLE || cooked.size() < 4U || (cooked.size() % 4U) != 0U) { return nullptr; }
    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = cooked.size();
    ci.pCode    = reinterpret_cast<const std::uint32_t*>(cooked.data()); // SPIR-V is 4-byte aligned by construction
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &ci, nullptr, &module) != VK_SUCCESS) { return nullptr; }
    return std::make_unique<VulkanGpuProgramImpl>(device, module, stage, cooked);
}

} // namespace crd::gpu
