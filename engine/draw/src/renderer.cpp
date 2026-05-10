// crd-draw -- Renderer (Phase 3.1 v1a-draw d0b, ADR-0066 sec 10).
//
// Loads cooked SHDR resources via ResourceManager, creates Vulkan
// ShaderModule + PipelineLayout + GraphicsPipeline, owns a per-frame
// instance buffer ring. Module-level singleton (one renderer per process)
// because the application has exactly one Device + one shader pack.

#include <crd/draw/renderer.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/draw/detail/gpu_types.hpp>
#include <crd/log/log.hpp>
#include <crd/resources/resource_manager.hpp>
#include <crd/rhi/buffer.hpp>
#include <crd/rhi/device.hpp>
#include <crd/rhi/pipeline.hpp>
#include <crd/rhi/shader_module.hpp>
#include <crd/shader/shader_resource_loader.hpp>
#include <crd/shader/types.hpp>

#include <cstddef>
#include <memory>

CRD_DEFINE_LOG_CHANNEL(g_log_draw, "Draw", crd::log::LogLevel::Info)

namespace crd::draw
{
namespace
{

// Resource IDs assigned by the asset cooker on first cook of
// engine/draw/shaders/*.glsl. Stored in the .meta sidecar files committed
// alongside the GLSL sources. Hardcoded here so the runtime doesn't need
// to read .meta at startup. If the .meta UUIDs change (deletion +
// regeneration), update these constants together.
constexpr const char* kLineVertIdStr     = "b9a6c083-c63c-495c-8ac7-999cdec58a35";
constexpr const char* kLineFragIdStr     = "b3fb90d9-5f03-4838-9d6f-555d18723c05";
constexpr const char* kTriangleVertIdStr = "18d71b87-e838-45e8-b3df-771be2d4b8b6";
constexpr const char* kTriangleFragIdStr = "33565f02-9465-4e7b-b0e2-e36e7d1a1495";
constexpr const char* kGridVertIdStr     = "b81df591-1dcb-427d-96b0-1e45d21555fa";
constexpr const char* kGridFragIdStr     = "3b68d3a4-b28a-4e75-92df-7bb2a51b783e";

// Per-instance vertex layout + push-constant layout live in
// crd/draw/detail/gpu_types.hpp so the overlay-pass impl can use them too.
using detail::LineInstanceGpu;
using detail::TriangleInstanceGpu;
using detail::DrawPushConstants;

} // namespace

// RendererState definition lives in crd/draw/detail/gpu_types.hpp (so the
// overlay-pass impl can also access it). Only the singleton accessor lives
// here.
namespace detail
{
RendererState& renderer_state() noexcept
{
    static RendererState s;
    return s;
}
} // namespace detail

namespace
{

using State = detail::RendererState;

State& state() noexcept { return detail::renderer_state(); }

[[nodiscard]] bool create_pipeline_layout(State& s) noexcept
{
    // Push constants are visible to BOTH vertex (line/triangle/grid vert
    // shaders) and fragment (grid frag uses camera_pos + colors + cell sizes
    // for AA + fade) stages.
    const crd::rhi::PushConstantRange range{
        crd::rhi::ShaderStage::Vertex | crd::rhi::ShaderStage::Fragment, 0,
        static_cast<crd::u32>(sizeof(DrawPushConstants))};
    s.pipeline_layout = s.device->create_pipeline_layout({
        /* set_layouts          */ {},
        /* push_constant_ranges */ crd::containers::make_span(&range, 1U),
    });
    if (!s.pipeline_layout)
    {
        CRD_LOG_ERROR(g_log_draw, "Failed to create pipeline layout");
        return false;
    }
    return true;
}

// d2-depth: 6-pipeline matrix per ADR-0066 sec 19.1.
//
// Variant 0 = Test           -> depth_test ON, GreaterOrEqual (reverse-Z visible)
// Variant 1 = Always         -> depth_test OFF (current d0d default; selection outlines, etc.)
// Variant 2 = GreaterDimmed  -> depth_test ON, Less (reverse-Z occluded; XRay's dimmed half)
//
// XRay primitives at submit time emit twice: into Test (full color, only
// the visible portion shows) AND into GreaterDimmed (auto-dimmed alpha,
// only the occluded portion shows). See overlay_pass.cpp.
constexpr crd::u32 kDepthVariantTest          = 0;
constexpr crd::u32 kDepthVariantAlways        = 1;
constexpr crd::u32 kDepthVariantGreaterDimmed = 2;

void apply_depth_variant(crd::rhi::GraphicsPipelineDesc& desc, crd::u32 variant) noexcept
{
    switch (variant)
    {
        case kDepthVariantTest:
            desc.enable_depth_test = true;
            desc.depth_write       = false;
            desc.depth_compare_op  = crd::rhi::DepthCompareOp::GreaterOrEqual;
            break;
        case kDepthVariantAlways:
            desc.enable_depth_test = false;
            desc.depth_write       = false;
            desc.depth_compare_op  = crd::rhi::DepthCompareOp::Always;
            break;
        case kDepthVariantGreaterDimmed:
            desc.enable_depth_test = true;
            desc.depth_write       = false;
            desc.depth_compare_op  = crd::rhi::DepthCompareOp::Less;
            break;
        default:
            desc.enable_depth_test = false;
            desc.depth_write       = false;
            break;
    }
}

[[nodiscard]] bool create_line_pipelines(State& s) noexcept
{
    const crd::rhi::VertexBindingDesc binding{
        0, static_cast<crd::u32>(sizeof(LineInstanceGpu)), crd::rhi::VertexInputRate::Instance};
    const crd::rhi::VertexAttributeDesc attrs[] = {
        {0, 0, crd::rhi::Format::R32G32B32Sfloat, offsetof(LineInstanceGpu, start)},
        {1, 0, crd::rhi::Format::R32G32B32Sfloat, offsetof(LineInstanceGpu, end)},
        {2, 0, crd::rhi::Format::R32Uint,         offsetof(LineInstanceGpu, color_packed)},
        {3, 0, crd::rhi::Format::R32Uint,         offsetof(LineInstanceGpu, flags_raw)},
        {4, 0, crd::rhi::Format::R32Sfloat,       offsetof(LineInstanceGpu, width)},
    };

    for (crd::u32 v = 0; v < detail::RendererState::kDepthVariantCount; ++v)
    {
        crd::rhi::GraphicsPipelineDesc desc;
        desc.vertex_shader        = s.line_vert_module.get();
        desc.fragment_shader      = s.line_frag_module.get();
        desc.topology             = crd::rhi::PrimitiveTopology::TriangleList;
        desc.color_format         = s.config.color_format;
        desc.depth_format         = s.config.depth_format;
        desc.vertex_bindings      = crd::containers::make_span(&binding, 1U);
        desc.vertex_attributes    = crd::containers::make_span(attrs, 5U);
        desc.enable_blend         = true;
        desc.use_dynamic_viewport = true;
        desc.pipeline_layout      = s.pipeline_layout.get();
        apply_depth_variant(desc, v);

        s.line_pipelines[v] = s.device->create_graphics_pipeline(desc);
        if (!s.line_pipelines[v])
        {
            CRD_LOG_ERROR(g_log_draw, "Failed to create line pipeline (variant {})", v);
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool create_grid_pipeline(State& s) noexcept
{
    // Grid: no per-instance binding, no vertex buffer. The vertex shader
    // emits 4 corners via gl_VertexIndex; one draw call covers the whole
    // grid.
    crd::rhi::GraphicsPipelineDesc desc;
    desc.vertex_shader        = s.grid_vert_module.get();
    desc.fragment_shader      = s.grid_frag_module.get();
    desc.topology             = crd::rhi::PrimitiveTopology::TriangleList;
    desc.color_format         = s.config.color_format;
    desc.depth_format         = s.config.depth_format;
    desc.enable_blend         = true;  // alpha-blended fade-to-horizon
    desc.use_dynamic_viewport = true;
    desc.pipeline_layout      = s.pipeline_layout.get();
    // Grid is depth-tested against scene depth (so closer geometry occludes
    // it). Reverse-Z visible-when-closer = GreaterOrEqual.
    desc.enable_depth_test    = (s.config.depth_format != crd::rhi::Format::Undefined);
    desc.depth_write          = false;
    desc.depth_compare_op     = crd::rhi::DepthCompareOp::GreaterOrEqual;

    s.grid_pipeline = s.device->create_graphics_pipeline(desc);
    if (!s.grid_pipeline)
    {
        CRD_LOG_ERROR(g_log_draw, "Failed to create grid pipeline");
        return false;
    }
    return true;
}

[[nodiscard]] bool create_triangle_pipelines(State& s) noexcept
{
    const crd::rhi::VertexBindingDesc binding{
        0, static_cast<crd::u32>(sizeof(TriangleInstanceGpu)), crd::rhi::VertexInputRate::Instance};
    const crd::rhi::VertexAttributeDesc attrs[] = {
        {0, 0, crd::rhi::Format::R32G32B32Sfloat, offsetof(TriangleInstanceGpu, v0)},
        {1, 0, crd::rhi::Format::R32G32B32Sfloat, offsetof(TriangleInstanceGpu, v1)},
        {2, 0, crd::rhi::Format::R32G32B32Sfloat, offsetof(TriangleInstanceGpu, v2)},
        {3, 0, crd::rhi::Format::R32Uint,         offsetof(TriangleInstanceGpu, color_packed)},
        {4, 0, crd::rhi::Format::R32Uint,         offsetof(TriangleInstanceGpu, flags_raw)},
    };

    for (crd::u32 v = 0; v < detail::RendererState::kDepthVariantCount; ++v)
    {
        crd::rhi::GraphicsPipelineDesc desc;
        desc.vertex_shader        = s.tri_vert_module.get();
        desc.fragment_shader      = s.tri_frag_module.get();
        desc.topology             = crd::rhi::PrimitiveTopology::TriangleList;
        desc.color_format         = s.config.color_format;
        desc.depth_format         = s.config.depth_format;
        desc.vertex_bindings      = crd::containers::make_span(&binding, 1U);
        desc.vertex_attributes    = crd::containers::make_span(attrs, 5U);
        desc.enable_blend         = true;
        desc.use_dynamic_viewport = true;
        desc.pipeline_layout      = s.pipeline_layout.get();
        apply_depth_variant(desc, v);

        s.triangle_pipelines[v] = s.device->create_graphics_pipeline(desc);
        if (!s.triangle_pipelines[v])
        {
            CRD_LOG_ERROR(g_log_draw, "Failed to create triangle pipeline (variant {})", v);
            return false;
        }
    }
    return true;
}

template <typename InstanceT>
[[nodiscard]] bool create_instance_buffer_ring(
    State& s, crd::containers::Array<std::unique_ptr<crd::rhi::Buffer>>& out,
    crd::u32 max_per_frame, const char* label) noexcept
{
    const crd::u64 size_bytes = static_cast<crd::u64>(max_per_frame) * sizeof(InstanceT);
    out.reserve(s.config.frames_in_flight);
    for (crd::u32 i = 0; i < s.config.frames_in_flight; ++i)
    {
        crd::rhi::BufferDesc bd;
        bd.size_bytes   = size_bytes;
        bd.usage        = crd::rhi::enum_bits(crd::rhi::BufferUsage::Vertex);
        bd.memory_usage = crd::rhi::MemoryUsage::CpuToGpu;
        auto buf = s.device->create_buffer(bd);
        if (!buf)
        {
            CRD_LOG_ERROR(g_log_draw, "Failed to allocate {} instance buffer {} ({} bytes)",
                          label, i, size_bytes);
            return false;
        }
        out.push_back(std::move(buf));
    }
    return true;
}

} // namespace

bool init(crd::resources::ResourceManager& rm,
          crd::rhi::Device&                device,
          const InitConfig&                config) noexcept
{
    State& s = state();
    if (s.initialised)
    {
        CRD_LOG_WARN(g_log_draw, "init() called twice -- ignoring");
        return true;
    }

    s.device = &device;
    s.config = config;

    // Load cooked SHDR resources. ResourceManager must have a
    // ShaderResourceLoader registered + the draw_shaders.crdr pack mounted.
    auto load_shader = [&](const char* uuid_str, crd::rhi::ShaderStage stage,
                           crd::resources::ResourceHandle<crd::shader::ShaderResource>& handle,
                           std::unique_ptr<crd::rhi::ShaderModule>&                      module,
                           const char* label) -> bool
    {
        const auto id = crd::resources::ResourceId::parse(uuid_str);
        handle = rm.load_sync<crd::shader::ShaderResource>(id);
        if (!handle.is_ready() || handle.get() == nullptr || handle.get()->spirv.empty())
        {
            CRD_LOG_ERROR(g_log_draw, "Failed to load shader '{}' (uuid={})", label, uuid_str);
            return false;
        }
        const auto* res = handle.get();
        module = device.create_shader_module(
            {stage, "main", crd::containers::make_span(res->spirv.data(), res->spirv.size())});
        if (!module)
        {
            CRD_LOG_ERROR(g_log_draw, "Failed to create shader module for '{}'", label);
            return false;
        }
        return true;
    };

    if (!load_shader(kLineVertIdStr,     crd::rhi::ShaderStage::Vertex,
                     s.line_vert_handle, s.line_vert_module, "line.vert")  ||
        !load_shader(kLineFragIdStr,     crd::rhi::ShaderStage::Fragment,
                     s.line_frag_handle, s.line_frag_module, "line.frag")  ||
        !load_shader(kTriangleVertIdStr, crd::rhi::ShaderStage::Vertex,
                     s.tri_vert_handle,  s.tri_vert_module,  "triangle.vert") ||
        !load_shader(kTriangleFragIdStr, crd::rhi::ShaderStage::Fragment,
                     s.tri_frag_handle,  s.tri_frag_module,  "triangle.frag") ||
        !load_shader(kGridVertIdStr,     crd::rhi::ShaderStage::Vertex,
                     s.grid_vert_handle, s.grid_vert_module, "grid.vert") ||
        !load_shader(kGridFragIdStr,     crd::rhi::ShaderStage::Fragment,
                     s.grid_frag_handle, s.grid_frag_module, "grid.frag"))
    {
        shutdown();
        return false;
    }

    if (!create_pipeline_layout(s) ||
        !create_line_pipelines(s) ||
        !create_triangle_pipelines(s) ||
        !create_grid_pipeline(s) ||
        !create_instance_buffer_ring<LineInstanceGpu>(s, s.line_instance_buffers,
                                                       s.config.max_lines_per_frame, "line") ||
        !create_instance_buffer_ring<TriangleInstanceGpu>(s, s.triangle_instance_buffers,
                                                           s.config.max_triangles_per_frame, "triangle"))
    {
        shutdown();
        return false;
    }

    s.initialised = true;
    CRD_LOG_INFO(g_log_draw,
                 "init OK -- color_format={} depth_format={} frames_in_flight={} max_lines={} max_triangles={}",
                 static_cast<int>(s.config.color_format),
                 static_cast<int>(s.config.depth_format),
                 s.config.frames_in_flight, s.config.max_lines_per_frame,
                 s.config.max_triangles_per_frame);
    return true;
}

void shutdown() noexcept
{
    State& s = state();
    s.line_instance_buffers.clear();
    s.triangle_instance_buffers.clear();
    for (auto& p : s.line_pipelines)     { p.reset(); }
    for (auto& p : s.triangle_pipelines) { p.reset(); }
    s.grid_pipeline.reset();
    s.pipeline_layout.reset();
    s.line_vert_module.reset();
    s.line_frag_module.reset();
    s.tri_vert_module.reset();
    s.tri_frag_module.reset();
    s.grid_vert_module.reset();
    s.grid_frag_module.reset();
    s.line_vert_handle = {};
    s.line_frag_handle = {};
    s.tri_vert_handle  = {};
    s.tri_frag_handle  = {};
    s.grid_vert_handle = {};
    s.grid_frag_handle = {};
    s.device      = nullptr;
    s.initialised = false;
}

bool is_initialised() noexcept
{
    return state().initialised;
}

// d4: master overlay enable. Stored as a process-global bool, same set-once-
// read-many contract as the active theme.
namespace
{
bool& mutable_overlay_enabled() noexcept
{
    static bool s = true;
    return s;
}
} // namespace

bool is_overlay_enabled() noexcept
{
    return mutable_overlay_enabled();
}

void set_overlay_enabled(bool enabled) noexcept
{
    mutable_overlay_enabled() = enabled;
}

} // namespace crd::draw
