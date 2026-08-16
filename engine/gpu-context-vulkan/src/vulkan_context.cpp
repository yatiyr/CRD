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
#include <crd/log/log.hpp> // loud-failure doctrine: name WHICH create_program link failed + the shaderc error text
#include <crd/memory/allocator.hpp>

#include <cstdint>
#include <cstring>

namespace crd::gpu
{
CRD_DEFINE_LOG_CHANNEL(g_log_vkctx, "VkContext", crd::log::LogLevel::Info)
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
    [[nodiscard]] bool             cooperative_vector() const noexcept override { return m_coopvec; }
    [[nodiscard]] bool             cooperative_vector_training() const noexcept override { return m_coopvec_train; }
    [[nodiscard]] crd::u32         coopvec_max_components() const noexcept override { return m_coopvec_max_components; }
    [[nodiscard]] crd::u32         coopvec_supported_stages() const noexcept override { return m_coopvec_stages; }
    [[nodiscard]] bool             device_generated_commands() const noexcept override { return m_dgc; } // C5: VK_NV_device_generated_commands(+compute)
    [[nodiscard]] bool             shader_int64() const noexcept override { return m_int64; }

    [[nodiscard]] bool     graphics_capable() const noexcept override { return m_graphics_family != UINT32_MAX; }
    [[nodiscard]] VkQueue  graphics_queue() const noexcept override { return m_graphics_queue; }
    [[nodiscard]] crd::u32 graphics_family() const noexcept override { return m_graphics_family; }
    [[nodiscard]] bool     shader_object() const noexcept override { return m_shader_object; }
    [[nodiscard]] bool     mesh_shader() const noexcept override { return m_mesh_shader; } // B4: VK_EXT_mesh_shader + meshShader
    [[nodiscard]] bool     task_shader() const noexcept override { return m_task_shader; }    // REN-38: + taskShader (amplification)
    [[nodiscard]] bool multi_draw_indirect() const noexcept override { return m_multi_draw_indirect; } // REN-39-A2
    [[nodiscard]] bool     partially_bound() const noexcept override { return m_partially_bound; } // REN-38: bindless heap
    [[nodiscard]] bool     draw_indirect_count() const noexcept override { return m_draw_indirect_count; }
    [[nodiscard]] bool     ray_query() const noexcept override { return m_ray_query; } // B9/RT: VK_KHR_ray_query + acceleration_structure
    [[nodiscard]] bool     opacity_micromap() const noexcept override { return m_opacity_micromap; } // FA-1
    [[nodiscard]] bool     rt_pipeline() const noexcept override { return m_rt_pipeline; }           // FA-2
    [[nodiscard]] bool     invocation_reorder() const noexcept override { return m_invocation_reorder; } // FA-2 SER
    [[nodiscard]] bool     cluster_as() const noexcept override { return m_cluster_as; }             // FA-3
    [[nodiscard]] bool     linear_swept_spheres() const noexcept override { return m_lss; }          // B18-f
    [[nodiscard]] bool     tessellation() const noexcept override { return m_tessellation; } // B4-tess: tess + patch-ctrl-points
    [[nodiscard]] bool     geometry_shader() const noexcept override { return m_geometry_shader; } // REN-38-A11
    [[nodiscard]] bool     render_capable() const noexcept override { return m_windowed; }
    [[nodiscard]] bool     present_capable() const noexcept override { return m_present_capable; }   // RET-2
    [[nodiscard]] bool     headless_surface() const noexcept override { return m_headless_surface; } // RET-2
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

        // ⛔ LOUD FAILURE (the loud-failure doctrine): create_program has several silent `return nullptr` exits — a GLSL
        // emit that cannot lower, or a shaderc/SPIR-V compile error whose `error_message` the code otherwise DISCARDS.
        // The caller only ever sees a bare nullptr, so a real cook failure (e.g. an emitter scope bug that references an
        // undeclared temp at a high LOD-slot count) surfaces upstream as "create_program failed" with no cause. Name
        // WHICH link failed, the emitted-GLSL size, and the shaderc error text so the next failure is diagnosable at a
        // glance instead of over a full instrumentation session.
        auto diag = [&](const char* where, const crd::kir::GlslKernel* k, const ShaderCompileResult* s) {
            CRD_LOG_ERROR(g_log_vkctx, "create_program failed at '{}' (stage={}, kernel={}, glsl_bytes={}): {}", where,
                          static_cast<int>(entry.stage), static_cast<int>(entry.is_kernel()),
                          k != nullptr ? k->source.size() : static_cast<crd::usize>(0),
                          s != nullptr ? s->error_message.c_str() : "(emit produced no source)");
        };

        // B3-c: raster stages behind the SAME seam. `entry_valid` first — e.g. a `FragCoord` in a vertex entry is rejected.
        if (entry.stage == crd::kir::KStage::Vertex || entry.stage == crd::kir::KStage::Fragment)
        {
            if (!crd::kir::entry_valid(graph, entry)) { diag("vtx.entry_valid", nullptr, nullptr); return nullptr; }
            crd::kir::GlslKernel kern(a);
            if (!crd::kir::emit_stage_glsl(graph, entry, a, kern)) { diag("vtx.emit", &kern, nullptr); return nullptr; }
            const ShaderStage stage =
                (entry.stage == crd::kir::KStage::Vertex) ? ShaderStage::Vertex : ShaderStage::Fragment;
            const auto spv = compile_glsl_to_spirv(stage, crd::containers::to_view(kern.source), "ckir_stage", a);
            if (!spv.ok) { diag("vtx.spv", &kern, &spv); return nullptr; }
            return create_program(stage, crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()));
        }

        // B4: a MESH shader (the modern amplification path) — emit GL_EXT_mesh_shader GLSL → SPIR-V (1.6) → a mesh shader object.
        // ── ⛔⛔ REN-38-A16: the RAY-TRACING STAGES were MISSING from this dispatch. ──
        // `emit_rt_stage_glsl` has existed since FA-2 and the SPIR-V compiler has mapped RayGen/AnyHit/ClosestHit/
        // Miss to their shaderc kinds all along — but `create_program(KGraph, KEntry)` never routed to it, so a
        // CKIR ray-tracing entry fell through to the elementwise-compute tail and returned NULL. The whole
        // ray-tracing-PIPELINE half of the IR was unreachable from the one entry point every consumer uses.
        // Exactly the shape of the DX12 `KStage::Compute` gap 38-A10 found: an emitter written, wired to nothing.
        if (entry.stage == crd::kir::KStage::RayGen || entry.stage == crd::kir::KStage::ClosestHit
            || entry.stage == crd::kir::KStage::Miss || entry.stage == crd::kir::KStage::AnyHit
            || entry.stage == crd::kir::KStage::Intersection || entry.stage == crd::kir::KStage::Callable)
        {
            crd::kir::GlslKernel kern(a);
            if (!crd::kir::emit_rt_stage_glsl(graph, entry, a, kern, invocation_reorder())) { return nullptr; }
            ShaderStage stage = ShaderStage::RayGen;
            if (entry.stage == crd::kir::KStage::ClosestHit) { stage = ShaderStage::ClosestHit; }
            else if (entry.stage == crd::kir::KStage::Miss)  { stage = ShaderStage::Miss; }
            else if (entry.stage == crd::kir::KStage::AnyHit) { stage = ShaderStage::AnyHit; }
            // REN-38-F13: the last two stages route through the SAME emitter + compile seam
            else if (entry.stage == crd::kir::KStage::Intersection) { stage = ShaderStage::Intersection; }
            else if (entry.stage == crd::kir::KStage::Callable) { stage = ShaderStage::Callable; }
            const auto spv = compile_glsl_to_spirv(stage, crd::containers::to_view(kern.source), "ckir_rt", a);
            if (!spv.ok) { return nullptr; }
            return create_program(stage, crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()));
        }
        if (entry.stage == crd::kir::KStage::Mesh)
        {
            if (!crd::kir::entry_valid(graph, entry)) { return nullptr; }
            crd::kir::GlslKernel kern(a);
            if (!crd::kir::emit_mesh_glsl(graph, entry, a, kern)) { return nullptr; }
            const auto spv = compile_glsl_to_spirv(ShaderStage::Mesh, crd::containers::to_view(kern.source), "ckir_mesh", a);
            if (!spv.ok) { return nullptr; }
            return create_program(ShaderStage::Mesh, crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()));
        }

        // B4: a TASK / AMPLIFICATION shader — emit GL_EXT_mesh_shader task GLSL → SPIR-V → a task shader object. It precedes a
        // mesh shader (create_task_mesh_program) and drives how many mesh workgroups launch (EmitMeshTasksEXT) + the payload.
        if (entry.stage == crd::kir::KStage::Task)
        {
            if (!crd::kir::entry_valid(graph, entry)) { return nullptr; }
            crd::kir::GlslKernel kern(a);
            if (!crd::kir::emit_task_glsl(graph, entry, a, kern)) { return nullptr; }
            const auto spv = compile_glsl_to_spirv(ShaderStage::Task, crd::containers::to_view(kern.source), "ckir_task", a);
            if (!spv.ok) { return nullptr; }
            return create_program(ShaderStage::Task, crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()));
        }

        // B4-tess: TESS-CONTROL (hull) / TESS-EVAL (domain) — the portable displacement path. Emit the tess GLSL → SPIR-V.
        if (entry.stage == crd::kir::KStage::TessControl || entry.stage == crd::kir::KStage::TessEval)
        {
            if (!crd::kir::entry_valid(graph, entry)) { return nullptr; }
            const bool           is_tcs = entry.stage == crd::kir::KStage::TessControl;
            crd::kir::GlslKernel kern(a);
            const bool           ok = is_tcs ? crd::kir::emit_tesc_glsl(graph, entry, a, kern)
                                             : crd::kir::emit_tese_glsl(graph, entry, a, kern);
            if (!ok) { return nullptr; }
            const ShaderStage stage = is_tcs ? ShaderStage::TessControl : ShaderStage::TessEval;
            const auto        spv   = compile_glsl_to_spirv(stage, crd::containers::to_view(kern.source), "ckir_tess", a);
            if (!spv.ok) { return nullptr; }
            return create_program(stage, crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()));
        }

        // B-cmp: an imperative COMPUTE KERNEL (workgroup shared memory + barriers + storage buffers), authored in CKIR.
        if (entry.stage == crd::kir::KStage::Compute && entry.is_kernel())
        {
            crd::kir::GlslKernel kern(a);
            if (!crd::kir::emit_compute_kernel_glsl(graph, entry, a, kern)) { diag("cmpk.emit", &kern, nullptr); return nullptr; }
            const auto spv = compile_glsl_to_spirv(ShaderStage::Compute, crd::containers::to_view(kern.source), "ckir_kernel", a);
            if (!spv.ok) { diag("cmpk.spv", &kern, &spv); return nullptr; }
            return create_program(ShaderStage::Compute, crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()));
        }

        // Compute: the fused elementwise / vec-aware kernel path.
        if (entry.stage != crd::kir::KStage::Compute || entry.n_out < 1) { return nullptr; }
        const int output = entry.out[0].node;
        if (output < 0 || output >= graph.size()) { return nullptr; }

        crd::kir::GlslKernel kern(a);
        const bool           ok = crd::kir::graph_uses_vec(graph, output, a)
                                      ? crd::kir::emit_vec_glsl(graph, output, a, kern)
                                      : crd::kir::emit_elementwise_glsl(graph, output, a, kern);
        if (!ok) { diag("elem.emit", &kern, nullptr); return nullptr; } // a compute class this backend's emitter does not lower yet

        const auto spv = compile_glsl_to_spirv(ShaderStage::Compute, crd::containers::to_view(kern.source), "ckir", a);
        if (!spv.ok) { diag("elem.spv", &kern, &spv); return nullptr; }
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
        // RET-2 (ADR-0105): surface enablement is AVAILABILITY-driven — VK_KHR_surface + (windowed) the platform
        // surface + VK_EXT_headless_surface (a swapchain WITHOUT a window — the fully-testable present path) are all
        // enabled whenever the loader offers them. A headless context can therefore still drive the present machinery
        // through a headless surface; a windowed one presents to a real window. Purely additive.
        const char*   inst_exts[4];
        std::uint32_t n_inst_exts = 0;
        bool          surface_ok  = false;
        {
            std::uint32_t nie = 0;
            vkEnumerateInstanceExtensionProperties(nullptr, &nie, nullptr);
            auto iavail = std::make_unique<VkExtensionProperties[]>(nie == 0 ? 1 : nie);
            vkEnumerateInstanceExtensionProperties(nullptr, &nie, iavail.get());
            bool has_surf     = false;
            bool has_plat     = false;
            bool has_headless = false;
            bool has_dbg      = false;
            for (std::uint32_t i = 0; i < nie; ++i)
            {
                if (std::strcmp(iavail[i].extensionName, "VK_KHR_surface") == 0) { has_surf = true; }
                if (platform_surface != nullptr && std::strcmp(iavail[i].extensionName, platform_surface) == 0)
                {
                    has_plat = true;
                }
                if (std::strcmp(iavail[i].extensionName, "VK_EXT_headless_surface") == 0) { has_headless = true; }
                if (std::strcmp(iavail[i].extensionName, "VK_EXT_debug_utils") == 0) { has_dbg = true; }
            }
            if (has_surf)
            {
                inst_exts[n_inst_exts++] = "VK_KHR_surface";
                surface_ok               = true;
                if (!config.headless && has_plat)
                {
                    inst_exts[n_inst_exts++] = platform_surface;
                    m_platform_surface_ext   = true; // a REAL window is presentable (the original `windowed` meaning)
                }
                if (has_headless)
                {
                    inst_exts[n_inst_exts++] = "VK_EXT_headless_surface";
                    m_headless_surface       = true;
                }
            }
            // RET-4: debug_utils enabled EXPLICITLY with validation (ValidationCapture's messenger rides it — the
            // layer resolving the entry points anyway is an accident, never a contract)
            if (config.enable_validation && has_dbg) { inst_exts[n_inst_exts++] = "VK_EXT_debug_utils"; }
        }
        if (n_inst_exts > 0U)
        {
            ici.enabledExtensionCount   = n_inst_exts;
            ici.ppEnabledExtensionNames = inst_exts;
        }
        m_surface_ext = surface_ok;
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
        bool has_dic       = false; // REN-40-A: device-side draw COUNT
        bool has_swapchain = false;
        bool has_vrs       = false;
        bool has_conserv   = false;
        bool has_eds3      = false;
        bool has_eds2      = false; // B4-tess: patch-control-points dynamic state
        bool has_interlock = false;
        bool has_sgpart    = false;
        bool has_mesh      = false;
        bool has_accel     = false; // B9/RT: VK_KHR_acceleration_structure (BLAS/TLAS)
        bool has_rayquery  = false; // B9/RT: VK_KHR_ray_query (inline ray tracing in compute)
        bool has_defhost   = false; // B9/RT: VK_KHR_deferred_host_operations (an acceleration_structure prerequisite)
        bool has_omm       = false; // FA-1: VK_EXT_opacity_micromap (alpha-tested geometry resolved in traversal)
        bool has_rtpipe    = false; // FA-2: VK_KHR_ray_tracing_pipeline (raygen/hit/miss + SBT)
        bool has_ser       = false; // FA-2: VK_NV_ray_tracing_invocation_reorder (shader execution reordering)
        bool has_cluster   = false; // FA-3: VK_NV_cluster_acceleration_structure (mega-geometry cluster BLAS)
        bool has_lss       = false; // B18-f: VK_NV_ray_tracing_linear_swept_spheres (native curve/strand primitive)
        bool has_coopvec   = false; // C6: VK_NV_cooperative_vector (per-invocation matrix×vector — the B10 neural-shading device half)
        bool has_dgc       = false; // C5: VK_NV_device_generated_commands (GPU-authored command streams)
        bool has_dgc_comp  = false; // C5: VK_NV_device_generated_commands_compute (compute dispatch + indirect-bindable pipelines)
        for (std::uint32_t i = 0; i < ne; ++i)
        {
            if (std::strcmp(exts[i].extensionName, "VK_NV_device_generated_commands") == 0) { has_dgc = true; }
            if (std::strcmp(exts[i].extensionName, "VK_NV_device_generated_commands_compute") == 0) { has_dgc_comp = true; }
            if (std::strcmp(exts[i].extensionName, "VK_EXT_opacity_micromap") == 0) { has_omm = true; }
            if (std::strcmp(exts[i].extensionName, "VK_KHR_ray_tracing_pipeline") == 0) { has_rtpipe = true; }
            if (std::strcmp(exts[i].extensionName, "VK_NV_ray_tracing_invocation_reorder") == 0) { has_ser = true; }
            if (std::strcmp(exts[i].extensionName, "VK_NV_cluster_acceleration_structure") == 0) { has_cluster = true; }
            if (std::strcmp(exts[i].extensionName, VK_NV_RAY_TRACING_LINEAR_SWEPT_SPHERES_EXTENSION_NAME) == 0) { has_lss = true; }
            if (std::strcmp(exts[i].extensionName, VK_EXT_MESH_SHADER_EXTENSION_NAME) == 0) { has_mesh = true; } // B4
            if (std::strcmp(exts[i].extensionName, "VK_KHR_cooperative_matrix") == 0) { has_cm1 = true; }
            if (std::strcmp(exts[i].extensionName, "VK_NV_cooperative_matrix2") == 0) { has_cm2 = true; }
            if (std::strcmp(exts[i].extensionName, "VK_NV_cooperative_vector") == 0) { has_coopvec = true; }
            if (std::strcmp(exts[i].extensionName, "VK_NV_shader_subgroup_partitioned") == 0) { has_sgpart = true; }
            if (std::strcmp(exts[i].extensionName, VK_EXT_SHADER_OBJECT_EXTENSION_NAME) == 0) { has_shobj = true; }
            if (std::strcmp(exts[i].extensionName, "VK_KHR_draw_indirect_count") == 0) { has_dic = true; }
            if (std::strcmp(exts[i].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) { has_swapchain = true; }
            if (std::strcmp(exts[i].extensionName, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME) == 0) { has_vrs = true; }
            if (std::strcmp(exts[i].extensionName, VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME) == 0) { has_conserv = true; }
            if (std::strcmp(exts[i].extensionName, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME) == 0) { has_eds3 = true; }
            if (std::strcmp(exts[i].extensionName, VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME) == 0) { has_eds2 = true; } // B4-tess
            if (std::strcmp(exts[i].extensionName, VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME) == 0) { has_interlock = true; }
            if (std::strcmp(exts[i].extensionName, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) == 0) { has_accel = true; }
            if (std::strcmp(exts[i].extensionName, VK_KHR_RAY_QUERY_EXTENSION_NAME) == 0) { has_rayquery = true; }
            if (std::strcmp(exts[i].extensionName, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME) == 0) { has_defhost = true; }
        }
        m_coopmat2      = has_cm1 && has_cm2;
        m_shader_object = has_shobj && m_graphics_family != UINT32_MAX; // no point on a compute-only adapter
        m_fragment_shading_rate = has_vrs && m_graphics_family != UINT32_MAX; // B1-e: raster-only feature
        // B1-f: conservative raster needs its extension AND the extended-dynamic-state-3 conservative-mode setter (the
        // shader-object model has no static pipeline state). Graphics-capable only.
        m_conservative_raster = has_conserv && has_eds3 && m_shader_object;
        // B4-tess: tessellation via shader objects needs the patch-control-points DYNAMIC state (EDS2) + the tessellationShader
        // core feature (confirmed below). Graphics + shader-object only. m_tessellation is finalised after the feature query.
        m_tessellation = has_eds2 && m_shader_object;
        // B1-f: pixel-ordered fragment-shader interlock (ROV) — graphics-capable only. The feature is checked below.
        m_fragment_interlock = has_interlock && m_graphics_family != UINT32_MAX;
        // B4: mesh shaders (the modern amplification path) — needs the extension + a graphics queue + shader objects (our draw
        // path creates a MESH shader object). The meshShader feature bit is confirmed below before we commit to it.
        m_mesh_shader = has_mesh && m_shader_object;
        // B9/RT: inline ray query needs the acceleration-structure + ray-query + deferred-host-ops extensions (+ buffer device
        // address, enabled below). Confirmed against the feature bits before we commit. A COMPUTE capability (works on a
        // compute-only adapter — the inline query rides the compute dispatch, no graphics queue / RT pipeline required).
        m_ray_query = has_accel && has_rayquery && has_defhost;
        // FA-1/2/3: the vendor RT frontier — opacity micromaps, the RT pipeline, SER (invocation reorder), cluster-AS. All ride
        // on the AS infrastructure (need m_ray_query) and are confirmed against their feature bits below.
        m_opacity_micromap  = m_ray_query && has_omm;
        m_rt_pipeline       = m_ray_query && has_rtpipe;
        m_invocation_reorder = m_ray_query && has_rtpipe && has_ser;
        m_cluster_as        = m_ray_query && has_cluster;
        m_lss               = m_ray_query && has_lss;
        // C6: cooperative VECTOR (VK_NV_cooperative_vector) — PER-INVOCATION matrix×vector, the inference primitive for neural
        // shading (each pixel/thread evaluates a small MLP inline; the device half of the B10 moat). Unlike coopmat (workgroup
        // GEMM, whole-kernel templates), coopvec maps onto CKIR's per-invocation statement tier. Query the FEATURE bits first so
        // we never REQUEST an unsupported feature (cooperativeVector is implied by the ext; cooperativeVectorTraining — the
        // OuterProductAccumulate/ReduceSum backward ops for B10 differentiable neural shading — is optional), and the coopvec
        // PROPERTIES for the supported stages + max component dimension. Gated ⇒ a device without it is byte-identical.
        if (has_coopvec)
        {
            VkPhysicalDeviceCooperativeVectorFeaturesNV cvf{};
            cvf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_VECTOR_FEATURES_NV;
            VkPhysicalDeviceFeatures2 cvf2{};
            cvf2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            cvf2.pNext = &cvf;
            vkGetPhysicalDeviceFeatures2(m_physical, &cvf2);
            m_coopvec       = cvf.cooperativeVector == VK_TRUE;
            m_coopvec_train = m_coopvec && cvf.cooperativeVectorTraining == VK_TRUE;
            if (m_coopvec)
            {
                VkPhysicalDeviceCooperativeVectorPropertiesNV cvp{};
                cvp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_VECTOR_PROPERTIES_NV;
                VkPhysicalDeviceProperties2 cvp2{};
                cvp2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
                cvp2.pNext = &cvp;
                vkGetPhysicalDeviceProperties2(m_physical, &cvp2);
                m_coopvec_stages         = cvp.cooperativeVectorSupportedStages;
                m_coopvec_max_components = cvp.maxCooperativeVectorComponents;
            }
        }

        // C2-a: render-capable iff a REAL platform window surface + swapchain + a graphics queue all present. (RET-2
        // split the meanings: `m_windowed` keeps its original real-window semantics; `m_present_capable` is the wider
        // "can drive a swapchain at all" — a headless context presenting to a HEADLESS surface qualifies for the
        // latter, never the former.)
        m_windowed        = m_platform_surface_ext && has_swapchain && m_graphics_family != UINT32_MAX;
        m_present_capable = surface_ok && has_swapchain && m_graphics_family != UINT32_MAX;

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
        // ⛔ REN-38-A11: a FRAGMENT shader that reads `gl_PrimitiveID` — which is the ENTIRE POINT of a visibility
        // buffer — lowers to SPIR-V declaring the GEOMETRY capability, and that capability REQUIRES this feature
        // (VUID-VkShaderCreateInfoEXT-pCode-08740). Without it a strict driver refuses to create the shader and a
        // lenient one runs it anyway, so the visibility-buffer path worked on this machine while emitting a
        // validation error on every program creation — found when the A11 gate ran it under a capture.
        if (m_graphics_family != UINT32_MAX) { enabled_feats.geometryShader = avail_feats.geometryShader; }
        m_geometry_shader = m_graphics_family != UINT32_MAX && avail_feats.geometryShader == VK_TRUE;
        // B4-tess: the tessellation control/eval stages need this core feature (the portable displacement path). Graphics-only.
        if (m_graphics_family != UINT32_MAX) { enabled_feats.tessellationShader = avail_feats.tessellationShader; }
        m_tessellation = m_tessellation && avail_feats.tessellationShader == VK_TRUE; // finalise: EDS2 + shader-obj + the feature
        // C2-c: a WINDOWED context matches what rhi-vulkan's own device enables so the renderer runs on the adopted
        // device unchanged — fillModeNonSolid (wireframe) here + synchronization2 in the feature chain below.
        if (m_windowed) { enabled_feats.fillModeNonSolid = avail_feats.fillModeNonSolid; }
        // ⛔ REN-39-A2: ONE vkCmdDraw(Indexed)Indirect with drawCount > 1 REQUIRES this core feature
        // (VUID-…-drawCount-02718). The 38-4 non-indexed multi-draw had issued drawCount = N WITHOUT it since it
        // shipped — a lenient driver ran it while emitting a validation error nobody captured (the 38-4 gate
        // asserts pixels + batch count, not validation); the REN-39-A2 gate runs under a ValidationCapture and
        // surfaced it. Graphics-only; the raster context loops per-draw when the device does not offer it.
        if (m_graphics_family != UINT32_MAX)
        {
            enabled_feats.multiDrawIndirect = avail_feats.multiDrawIndirect;
        }
        m_multi_draw_indirect = m_graphics_family != UINT32_MAX && avail_feats.multiDrawIndirect == VK_TRUE;

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
        // C6: cooperative-vector features to ENABLE at device creation (only chained when m_coopvec; training only when supported).
        VkPhysicalDeviceCooperativeVectorFeaturesNV cv{};
        cv.sType                     = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_VECTOR_FEATURES_NV;
        cv.cooperativeVector         = VK_TRUE;
        cv.cooperativeVectorTraining = m_coopvec_train ? VK_TRUE : VK_FALSE;
        // C5: DEVICE-GENERATED COMMANDS — the GPU authors a stream of varied compute commands (pipeline switch + per-sequence push
        // constants + dispatch) executed via `vkCmdExecuteGeneratedCommandsNV`. `deviceGeneratedComputePipelines` = the indirect-
        // bindable pipeline path (the PIPELINE token). Needs buffer-device-address (pipeline device addresses). Only chained when m_dgc.
        VkPhysicalDeviceDeviceGeneratedCommandsFeaturesNV dgc_feat{};
        dgc_feat.sType                   = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_NV;
        dgc_feat.deviceGeneratedCommands = VK_TRUE;
        VkPhysicalDeviceDeviceGeneratedCommandsComputeFeaturesNV dgcc_feat{};
        dgcc_feat.sType                          = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_COMPUTE_FEATURES_NV;
        dgcc_feat.deviceGeneratedCompute         = VK_TRUE;
        dgcc_feat.deviceGeneratedComputePipelines = VK_TRUE;

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
        // B4-tess: the patch-control-points DYNAMIC state (shader objects have no static pipeline, so patch size is dynamic).
        VkPhysicalDeviceExtendedDynamicState2FeaturesEXT eds2{};
        eds2.sType                                   = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT;
        eds2.extendedDynamicState2PatchControlPoints = VK_TRUE;
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
        // ⭐⭐ REN-38 (multi-draw): `shaderDrawParameters` (Vulkan 1.1 core) — gl_DrawID, the batched-draw id.
        // Queried and enabled whenever the device offers it; the rebased scene VS cannot compile without it.
        VkPhysicalDeviceShaderDrawParametersFeatures sdp{};
        sdp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES;
        if (m_graphics_family != UINT32_MAX)
        {
            VkPhysicalDeviceFeatures2 f2s{};
            f2s.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            f2s.pNext = &sdp;
            vkGetPhysicalDeviceFeatures2(m_physical, &f2s);
            sdp.pNext = nullptr; // keep only the queried bit; chained below when TRUE
        }
        // ⭐⭐ REN-40-A: `drawIndirectCount` — the DEVICE-SIDE DRAW COUNT, which is what lets a cull kernel decide
        // how many indirect commands run so an empty batch costs NOTHING instead of a zero-instance command.
        // ⛔ `vkCmdDrawIndexedIndirectCount` is CORE since Vulkan 1.2 and it is STILL GATED: the command exists
        // unconditionally but calling it ungated is VUID-…-None-04445. The gate caught exactly that — a hardcoded
        // `indirect_count_supported() → true` was a LIE on a device where nothing had been requested.
        // ⛔⛔ TWO ways to satisfy 04445, and only ONE of them composes here. The feature bit lives solely in
        // `VkPhysicalDeviceVulkan12Features`, and chaining that struct is MUTUALLY EXCLUSIVE with the individual
        // promoted structs this device already needs (`…DescriptorIndexingFeatures` for bindless,
        // `…BufferDeviceAddressFeatures` for RT/DGC) — VUID-VkDeviceCreateInfo-pNext-02830, which validation
        // reported the moment the sandbox ran. So we take the OTHER way: enable `VK_KHR_draw_indirect_count`, whose
        // presence satisfies 04445 with no feature struct at all and therefore no aggregation conflict.
        m_draw_indirect_count = has_dic && m_graphics_family != UINT32_MAX;

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
            // REN-38 bindless-heap slice: PARTIALLY BOUND is what turns the array from "write every slot per
            // draw" into a real heap — only the slots a frame registered are written, the rest stay invalid
            // and legal as long as no shader reads them. Enabled whenever the device offers it.
            m_partially_bound = descidx.descriptorBindingPartiallyBound == VK_TRUE;
            VkPhysicalDeviceDescriptorIndexingFeatures keep{};
            keep.sType                                       = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
            keep.shaderSampledImageArrayNonUniformIndexing   = descidx.shaderSampledImageArrayNonUniformIndexing;
            keep.descriptorBindingPartiallyBound             = descidx.descriptorBindingPartiallyBound;
            descidx                                          = keep; // enable ONLY what the bindless path uses
        }

        // B4: mesh shaders — confirm the meshShader bit before committing (the extension can be exposed without it).
        // ⛔⛔ REN-38 llvmpipe campaign: taskShader is ENABLED TOO (when present). The original comment said "no
        // amplification stage" — but B4 then grew `create_task_mesh_program`, which creates a TASK shader object,
        // and creating one with taskShader DISABLED violates VUID-VkShaderCreateInfoEXT-stage-08421 on EVERY
        // platform. NVIDIA's runtime tolerated it silently; Ubuntu's validation layer was the first to say it.
        VkPhysicalDeviceMeshShaderFeaturesEXT mesh{};
        mesh.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
        if (m_mesh_shader)
        {
            VkPhysicalDeviceFeatures2 f2{};
            f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            f2.pNext = &mesh;
            vkGetPhysicalDeviceFeatures2(m_physical, &f2);
            m_mesh_shader = mesh.meshShader == VK_TRUE;
            m_task_shader = m_mesh_shader && mesh.taskShader == VK_TRUE; // amplification needs BOTH bits
            VkPhysicalDeviceMeshShaderFeaturesEXT keep{};
            keep.sType      = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
            keep.meshShader = mesh.meshShader;
            keep.taskShader = mesh.taskShader; // enable exactly what the device offers of the pair
            mesh            = keep;
        }
        // B4-tess: confirm the patch-control-points feature (the EDS2 extension can be exposed without this optional bit).
        if (m_tessellation)
        {
            VkPhysicalDeviceExtendedDynamicState2FeaturesEXT probe{};
            probe.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT;
            VkPhysicalDeviceFeatures2 f2{};
            f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            f2.pNext = &probe;
            vkGetPhysicalDeviceFeatures2(m_physical, &f2);
            m_tessellation = probe.extendedDynamicState2PatchControlPoints == VK_TRUE;
        }
        // B4: glslang emits the SPIR-V `LocalSizeId` execution mode for the mesh workgroup size (SPIR-V 1.6) — that requires
        // `maintenance4` (VUID-RuntimeSpirv-LocalSizeId-06434). Enabled ONLY with mesh, so the non-mesh device is unchanged.
        VkPhysicalDeviceMaintenance4Features maint4{};
        maint4.sType        = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES;
        maint4.maintenance4 = VK_TRUE;

        // B9/RT: acceleration-structure + ray-query + buffer-device-address. Confirm the bits (an extension can be exposed
        // without them), then keep ONLY those bits so a non-RT device is unchanged.
        VkPhysicalDeviceAccelerationStructureFeaturesKHR accel_feat{};
        accel_feat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
        VkPhysicalDeviceRayQueryFeaturesKHR rq_feat{};
        rq_feat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
        VkPhysicalDeviceBufferDeviceAddressFeatures bda_feat{};
        bda_feat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
        if (m_ray_query)
        {
            accel_feat.pNext = &rq_feat;
            rq_feat.pNext    = &bda_feat;
            VkPhysicalDeviceFeatures2 f2{};
            f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            f2.pNext = &accel_feat;
            vkGetPhysicalDeviceFeatures2(m_physical, &f2);
            m_ray_query = accel_feat.accelerationStructure == VK_TRUE && rq_feat.rayQuery == VK_TRUE && bda_feat.bufferDeviceAddress == VK_TRUE;
            VkPhysicalDeviceAccelerationStructureFeaturesKHR ak{}; ak.sType = accel_feat.sType; ak.accelerationStructure = accel_feat.accelerationStructure; accel_feat = ak;
            VkPhysicalDeviceRayQueryFeaturesKHR rk{}; rk.sType = rq_feat.sType; rk.rayQuery = rq_feat.rayQuery; rq_feat = rk;
            VkPhysicalDeviceBufferDeviceAddressFeatures bk{}; bk.sType = bda_feat.sType; bk.bufferDeviceAddress = bda_feat.bufferDeviceAddress; bda_feat = bk;
        }
        // C5: device-generated commands need buffer-device-address (pipeline device addresses for the PIPELINE token). BDA is
        // queried in the RT block above; this adapter always has RT, so it is available when DGC is present.
        m_dgc = has_dgc && has_dgc_comp && bda_feat.bufferDeviceAddress == VK_TRUE;

        // FA-1/2/3: vendor RT frontier feature bits (opacity micromap · RT pipeline · SER invocation-reorder · cluster-AS). Live
        // to vkCreateDevice (referenced by the chain). Confirm each bit, then keep ONLY it (so the struct is clean for creation).
        VkPhysicalDeviceOpacityMicromapFeaturesEXT omm_feat{}; omm_feat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_EXT;
        VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtp_feat{}; rtp_feat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
        VkPhysicalDeviceRayTracingInvocationReorderFeaturesNV ser_feat{}; ser_feat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_FEATURES_NV;
        VkPhysicalDeviceClusterAccelerationStructureFeaturesNV clu_feat{}; clu_feat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CLUSTER_ACCELERATION_STRUCTURE_FEATURES_NV;
        VkPhysicalDeviceRayTracingLinearSweptSpheresFeaturesNV lss_feat{}; lss_feat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_LINEAR_SWEPT_SPHERES_FEATURES_NV;
        if (m_opacity_micromap || m_rt_pipeline || m_invocation_reorder || m_cluster_as || m_lss)
        {
            void* pf = nullptr;
            if (m_opacity_micromap) { omm_feat.pNext = pf; pf = &omm_feat; }
            if (m_rt_pipeline) { rtp_feat.pNext = pf; pf = &rtp_feat; }
            if (m_invocation_reorder) { ser_feat.pNext = pf; pf = &ser_feat; }
            if (m_cluster_as) { clu_feat.pNext = pf; pf = &clu_feat; }
            if (m_lss) { lss_feat.pNext = pf; pf = &lss_feat; }
            VkPhysicalDeviceFeatures2 f2{}; f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2; f2.pNext = pf;
            vkGetPhysicalDeviceFeatures2(m_physical, &f2);
            m_opacity_micromap   = m_opacity_micromap && omm_feat.micromap == VK_TRUE;
            m_rt_pipeline        = m_rt_pipeline && rtp_feat.rayTracingPipeline == VK_TRUE;
            m_invocation_reorder = m_invocation_reorder && ser_feat.rayTracingInvocationReorder == VK_TRUE;
            m_cluster_as         = m_cluster_as && clu_feat.clusterAccelerationStructure == VK_TRUE;
            // the extension can be PRESENT while the feature bit is false — trust the bit, not the string
            m_lss                = m_lss && lss_feat.linearSweptSpheres == VK_TRUE;
            { VkPhysicalDeviceOpacityMicromapFeaturesEXT z{}; z.sType = omm_feat.sType; z.micromap = omm_feat.micromap; omm_feat = z; }
            { VkPhysicalDeviceRayTracingPipelineFeaturesKHR z{}; z.sType = rtp_feat.sType; z.rayTracingPipeline = rtp_feat.rayTracingPipeline; rtp_feat = z; }
            { VkPhysicalDeviceRayTracingInvocationReorderFeaturesNV z{}; z.sType = ser_feat.sType; z.rayTracingInvocationReorder = ser_feat.rayTracingInvocationReorder; ser_feat = z; }
            { VkPhysicalDeviceClusterAccelerationStructureFeaturesNV z{}; z.sType = clu_feat.sType; z.clusterAccelerationStructure = clu_feat.clusterAccelerationStructure; clu_feat = z; }
            { VkPhysicalDeviceRayTracingLinearSweptSpheresFeaturesNV z{}; z.sType = lss_feat.sType; z.spheres = lss_feat.spheres; z.linearSweptSpheres = lss_feat.linearSweptSpheres; lss_feat = z; }
        }

        // Build the pNext chain head-first: dyn → [demote] → [vrs] → [eds3] → [sync2] → [sho] → [mesh] → [coopmat…] → [RT].
        void* chain = &dyn;
        if (m_graphics_family != UINT32_MAX) { demote.pNext = chain; chain = &demote; } // raster `discard` support
        if (m_fragment_shading_rate) { vrs.pNext = chain; chain = &vrs; }
        if (m_conservative_raster) { eds3.pNext = chain; chain = &eds3; }
        if (m_tessellation) { eds2.pNext = chain; chain = &eds2; } // B4-tess: patch-control-points dynamic state
        if (m_fragment_interlock) { interlock.pNext = chain; chain = &interlock; }
        if (m_bindless) { descidx.pNext = chain; chain = &descidx; }
        // REN-38: gl_DrawID for the batched scene VS — chained whenever the device offers it
        if (sdp.shaderDrawParameters == VK_TRUE) { sdp.pNext = chain; chain = &sdp; }
        // REN-40-A: the device-side draw COUNT — core command, feature-gated bit (see the query above)
        if (m_windowed) { sync2.pNext = chain; chain = &sync2; }
        if (m_shader_object) { sho.pNext = chain; chain = &sho; }
        if (m_mesh_shader) { mesh.pNext = chain; chain = &mesh; maint4.pNext = chain; chain = &maint4; }
        if (m_coopmat2) { cm2.pNext = chain; cmk.pNext = &cm2; chain = &cmk; }
        if (m_coopvec) { cv.pNext = chain; chain = &cv; } // C6: cooperative-vector inference (+ training when supported)
        if (m_ray_query) { accel_feat.pNext = chain; chain = &accel_feat; rq_feat.pNext = chain; chain = &rq_feat; }
        if (m_ray_query || m_dgc) { bda_feat.pNext = chain; chain = &bda_feat; } // BDA: RT (AS build) + DGC (pipeline device addrs) — chained ONCE
        if (m_dgc) { dgc_feat.pNext = chain; chain = &dgc_feat; dgcc_feat.pNext = chain; chain = &dgcc_feat; } // C5
        if (m_opacity_micromap) { omm_feat.pNext = chain; chain = &omm_feat; }
        if (m_rt_pipeline) { rtp_feat.pNext = chain; chain = &rtp_feat; }
        if (m_invocation_reorder) { ser_feat.pNext = chain; chain = &ser_feat; }
        if (m_cluster_as) { clu_feat.pNext = chain; chain = &clu_feat; }
        if (m_lss) { lss_feat.pNext = chain; chain = &lss_feat; }

        const char* devexts[32];
        crd::u32    ndevext = 0;
        // B-cmp: hardware subgroup partition (match_any) — the radix-sort rank's cheap deterministic match. Shader-only
        // capability (no feature struct); enabling the extension unlocks the SPIR-V GroupNonUniformPartitionedNV cap.
        if (has_sgpart) { devexts[ndevext++] = "VK_NV_shader_subgroup_partitioned"; }
        if (m_coopmat2)
        {
            devexts[ndevext++] = "VK_KHR_cooperative_matrix";
            devexts[ndevext++] = "VK_NV_cooperative_matrix2";
        }
        if (m_coopvec) { devexts[ndevext++] = "VK_NV_cooperative_vector"; } // C6: per-invocation MLP inference
        if (m_dgc) { devexts[ndevext++] = "VK_NV_device_generated_commands"; devexts[ndevext++] = "VK_NV_device_generated_commands_compute"; } // C5
        if (m_shader_object) { devexts[ndevext++] = VK_EXT_SHADER_OBJECT_EXTENSION_NAME; }
        // RET-2: swapchain enablement follows AVAILABILITY (given the instance enabled VK_KHR_surface) — a headless
        // context presents to a headless surface, a windowed one to a window; one extension, both paths.
        if (m_surface_ext && has_swapchain) { devexts[ndevext++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME; }
        // REN-40-A: the extension form of the device-side draw count (see the 02830 note above).
        if (m_draw_indirect_count) { devexts[ndevext++] = "VK_KHR_draw_indirect_count"; }
        if (m_fragment_shading_rate) { devexts[ndevext++] = VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME; }
        if (m_conservative_raster)
        {
            devexts[ndevext++] = VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME;
            devexts[ndevext++] = VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME;
        }
        if (m_tessellation) { devexts[ndevext++] = VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME; } // B4-tess
        if (m_fragment_interlock) { devexts[ndevext++] = VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME; }
        if (m_mesh_shader) { devexts[ndevext++] = VK_EXT_MESH_SHADER_EXTENSION_NAME; } // B4
        if (m_ray_query) // B9/RT: inline ray query — the AS + ray-query + deferred-host-ops trio
        {
            devexts[ndevext++] = VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME;
            devexts[ndevext++] = VK_KHR_RAY_QUERY_EXTENSION_NAME;
            devexts[ndevext++] = VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME;
        }
        if (m_opacity_micromap) { devexts[ndevext++] = "VK_EXT_opacity_micromap"; }                      // FA-1
        if (m_rt_pipeline) { devexts[ndevext++] = "VK_KHR_ray_tracing_pipeline"; }                        // FA-2
        if (m_invocation_reorder) { devexts[ndevext++] = "VK_NV_ray_tracing_invocation_reorder"; }        // FA-2 SER
        if (m_cluster_as) { devexts[ndevext++] = "VK_NV_cluster_acceleration_structure"; }                // FA-3
        if (m_lss) { devexts[ndevext++] = VK_NV_RAY_TRACING_LINEAR_SWEPT_SPHERES_EXTENSION_NAME; }        // B18-f

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
    bool             m_coopvec         = false; // C6: VK_NV_cooperative_vector enabled (per-invocation MLP inference)
    bool             m_coopvec_train   = false; // C6: + the training ops (OuterProductAccumulate/ReduceSum) for B10 backprop
    bool             m_dgc             = false; // C5: VK_NV_device_generated_commands + _compute enabled (GPU-authored command streams)
    crd::u32         m_coopvec_stages  = 0U;    // C6: shader stages that support coopvec (compute always; fragment on this HW)
    crd::u32         m_coopvec_max_components = 0U; // C6: max cooperative-vector component dimension
    bool             m_int64           = false;
    bool             m_shader_object   = false;
    bool             m_windowed        = false;
    bool             m_surface_ext          = false; // RET-2: VK_KHR_surface enabled on the instance
    bool             m_platform_surface_ext = false; // RET-2: the platform (win32/xcb) surface enabled — real windows
    bool             m_headless_surface     = false; // RET-2: VK_EXT_headless_surface enabled — windowless swapchains
    bool             m_present_capable      = false; // RET-2: surface + swapchain + graphics queue — any present path
    bool             m_fragment_shading_rate = false; // B1-e: VK_KHR_fragment_shading_rate enabled
    VkExtent2D       m_vrs_tile_size{};               // B1-e: attachment shading-rate-image texel size
    bool             m_conservative_raster   = false; // B1-f: conservative raster + EDS3 conservative mode enabled
    bool             m_fragment_interlock    = false; // B1-f: pixel-ordered fragment-shader interlock (ROV) enabled
    bool             m_bindless              = false; // B2-d: non-uniform sampled-image array indexing enabled
    bool             m_mesh_shader           = false; // B4: VK_EXT_mesh_shader + meshShader feature enabled
    bool             m_task_shader           = false; // REN-38: + taskShader (amplification) — enabled when offered
    bool m_multi_draw_indirect = false;               // REN-39-A2: core multiDrawIndirect — drawCount > 1 legality
    bool             m_partially_bound       = false; // REN-38: descriptorBindingPartiallyBound — the bindless heap flag
    // ⭐⭐ REN-40-A: VkPhysicalDeviceVulkan12Features::drawIndirectCount — the device-side draw COUNT.
    // The command is CORE since 1.2 but calling it needs this bit ENABLED, so it is queried and reported
    // rather than assumed (a hardcoded "supported" is how a step-down becomes invisible).
    bool             m_draw_indirect_count   = false;
    bool             m_ray_query             = false; // B9/RT: VK_KHR_ray_query + acceleration_structure + BDA enabled
    bool             m_opacity_micromap      = false; // FA-1: VK_EXT_opacity_micromap
    bool             m_rt_pipeline           = false; // FA-2: VK_KHR_ray_tracing_pipeline
    bool             m_invocation_reorder    = false; // FA-2: VK_NV_ray_tracing_invocation_reorder (SER)
    bool             m_cluster_as            = false; // FA-3: VK_NV_cluster_acceleration_structure
    bool             m_lss                   = false; // B18-f: VK_NV_ray_tracing_linear_swept_spheres
    bool             m_geometry_shader       = false; // REN-38-A11: gl_PrimitiveID in a FS declares the Geometry capability
    bool             m_tessellation          = false; // B4-tess: tessellationShader + EDS2 patch-control-points enabled
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
