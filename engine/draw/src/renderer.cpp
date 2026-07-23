// crd-draw — Renderer impl (RET-6, ADR-0105): init/shutdown on the ONE graphics layer. The rhi original loaded a
// cooked-GLSL pack through the ResourceManager and built six pipelines; THIS builds the CKIR draw-shader suite
// through `create_program` (the ADR-0103 currency) and three shader-object raster programs — depth variants are
// per-draw `DepthCompare` arguments at submit time, not pipelines. Module-level singleton (one renderer per
// process), same as the original.

#include <crd/draw/renderer.hpp>

#include <crd/draw/ckir_draw.hpp>
#include <crd/draw/detail/gpu_types.hpp>
#include <crd/gpu/vulkan_context.hpp>
#include <crd/log/log.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

CRD_DEFINE_LOG_CHANNEL(g_log_draw, "Draw", crd::log::LogLevel::Info)

namespace crd::draw
{
namespace detail
{
RendererState& renderer_state() noexcept
{
    static RendererState s_state;
    return s_state;
}
} // namespace detail

namespace
{
bool g_overlay_enabled = true;

[[nodiscard]] crd::u32 draw_buffer_bytes(const InitConfig& cfg) noexcept
{
    const crd::u32 line_words = cfg.max_lines_per_frame * ckir::kLineInstanceWords;
    const crd::u32 tri_words  = cfg.max_triangles_per_frame * ckir::kTriInstanceWords;
    const crd::u32 inst_words = line_words > tri_words ? line_words : tri_words;
    return (ckir::kHeaderWords + inst_words) * 4U;
}
} // namespace

bool init(crd::gpu::VulkanGpuContext& ctx, crd::gpu::IRasterContext& raster, const InitConfig& config) noexcept
{
    auto& s = detail::renderer_state();
    if (s.initialised)
    {
        CRD_LOG_WARN(g_log_draw, "draw::init called twice -- ignoring");
        return true;
    }
    if (!raster.valid())
    {
        CRD_LOG_ERROR(g_log_draw, "draw::init: raster context not graphics-capable");
        return false;
    }

    // Build the six CKIR graphs + compile. A TLSF scratch feeds graph construction; the compiled programs own their
    // device objects, so the graphs (and this allocator) die at scope end.
    crd::memory::TlsfAllocator graph_alloc(8U << 20U);
    const auto make = [&](void (*build_vs)(crd::kir::KGraph&, crd::kir::KEntry&),
                          void (*build_fs)(crd::kir::KGraph&, crd::kir::KEntry&),
                          std::unique_ptr<crd::gpu::IGpuProgram>&    out_vs,
                          std::unique_ptr<crd::gpu::IGpuProgram>&    out_fs,
                          std::unique_ptr<crd::gpu::IRasterProgram>& out_prog) {
        crd::kir::KGraph vg(&graph_alloc);
        crd::kir::KEntry ve;
        build_vs(vg, ve);
        crd::kir::KGraph fg(&graph_alloc);
        crd::kir::KEntry fe;
        build_fs(fg, fe);
        out_vs = ctx.create_program(vg, ve);
        out_fs = ctx.create_program(fg, fe);
        if (out_vs == nullptr || out_fs == nullptr) { return false; }
        out_prog = raster.create_raster_program(*out_vs, *out_fs);
        return out_prog != nullptr;
    };

    if (!make(&ckir::build_line_vs, &ckir::build_line_fs, s.line_vs, s.line_fs, s.line_prog)
        || !make(&ckir::build_tri_vs, &ckir::build_tri_fs, s.tri_vs, s.tri_fs, s.tri_prog)
        || !make(&ckir::build_grid_vs, &ckir::build_grid_fs, s.grid_vs, s.grid_fs, s.grid_prog))
    {
        CRD_LOG_ERROR(g_log_draw, "draw::init: CKIR draw program compilation failed");
        shutdown();
        return false;
    }

    s.storage = raster.create_storage_buffer(draw_buffer_bytes(config));
    if (s.storage == nullptr)
    {
        CRD_LOG_ERROR(g_log_draw, "draw::init: draw-buffer creation failed ({} bytes)", draw_buffer_bytes(config));
        shutdown();
        return false;
    }

    s.ctx         = &ctx;
    s.raster      = &raster;
    s.config      = config;
    s.initialised = true;
    CRD_LOG_INFO(g_log_draw, "draw renderer initialised on gpu-context (draw buffer {} KiB)",
                 draw_buffer_bytes(config) / 1024U);
    return true;
}

void shutdown() noexcept
{
    auto& s = detail::renderer_state();
    s.storage.reset();
    s.line_prog.reset();
    s.tri_prog.reset();
    s.grid_prog.reset();
    s.line_vs.reset();
    s.line_fs.reset();
    s.tri_vs.reset();
    s.tri_fs.reset();
    s.grid_vs.reset();
    s.grid_fs.reset();
    s.scratch.clear();
    s.ctx         = nullptr;
    s.raster      = nullptr;
    s.initialised = false;
}

bool is_initialised() noexcept { return detail::renderer_state().initialised; }

bool is_overlay_enabled() noexcept { return g_overlay_enabled; }
void set_overlay_enabled(bool enabled) noexcept { g_overlay_enabled = enabled; }

} // namespace crd::draw
