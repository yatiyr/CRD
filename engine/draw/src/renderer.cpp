// crd-draw — Renderer impl (RET-6, ADR-0105): init/shutdown on the ONE graphics layer. The rhi original loaded a
// cooked-GLSL pack through the ResourceManager and built six pipelines; THIS builds the CKIR draw-shader suite
// through `create_program` (the ADR-0103 currency) and three shader-object raster programs — depth variants are
// per-draw `DepthCompare` arguments at submit time, not pipelines. Module-level singleton (one renderer per
// process), same as the original.

#include <crd/draw/renderer.hpp>

#include <crd/draw/detail/gpu_types.hpp>
#include <crd/draw/draw_assets.hpp> // REN-38-F7: the AUTHORED suite — ckir_draw.hpp (the C++ builders) is DELETED
#include <crd/gpu/vulkan_context.hpp>
#include <crd/kir/ckir_cook.hpp>
#include <crd/kir/ckir_material.hpp>
#include <crd/kir/ckir_technique.hpp>
#include <crd/log/log.hpp>
#include <crd/matcook/material_asset.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/vertexcook/vertex_asset.hpp>

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
    const crd::u32 line_words = cfg.max_lines_per_frame * kLineInstanceWords;
    const crd::u32 tri_words  = cfg.max_triangles_per_frame * kTriInstanceWords;
    const crd::u32 inst_words = line_words > tri_words ? line_words : tri_words;
    return (kHeaderWords + inst_words) * 4U;
}

// The `MaterialTemplate` adapter for the authored draw materials (the scene renderer's exact pattern).
int draw_material_surface(crd::kir::KGraph& g, int struct_id, const crd::kir::cook::SurfaceInputs& /*in*/,
                          void* user)
{
    return crd::matcook::cook_material(*static_cast<const crd::matcook::MaterialDesc*>(user), g, struct_id);
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

    // ⭐⭐ REN-38-F7: cook the six programs FROM THE AUTHORED DECLARATIONS (`draw_assets.hpp` — the same texts
    // ship under `assets/`, drift-gated). The 339-line C++ builder suite is DELETED; if these declarations could
    // not express the draw shaders, that code would have had to stay. A TLSF scratch feeds cooking; the compiled
    // programs own their device objects, so the graphs (and this allocator) die at scope end.
    crd::memory::TlsfAllocator graph_alloc(16U << 20U);
    const auto cook_vs_toml = [&](const char* toml, crd::kir::KGraph& g, crd::kir::KEntry& e) {
        crd::vertcook::VertexProgramDesc d(&graph_alloc);
        crd::containers::String          where(&graph_alloc);
        return crd::vertcook::parse_vertex_toml(crd::containers::StringView(toml), d, &where)
                   == crd::vertcook::VertexCookError::Ok
               && crd::vertcook::cook_vertex_program(d, g, e);
    };
    // The FS: the authored material through the `unlit` technique with CONSTANT surface inputs, so the cooked
    // graph reads exactly the varyings the material names (by location) and nothing else.
    const auto cook_fs_toml = [&](const char* toml, crd::kir::KGraph& g, crd::kir::KEntry& e) {
        namespace ck = crd::kir::cook;
        namespace tq = crd::kir::technique;
        crd::matcook::MaterialDesc d(&graph_alloc);
        crd::containers::String    where(&graph_alloc);
        if (crd::matcook::parse_material_toml(crd::containers::StringView(toml), d, &where)
            != crd::matcook::MaterialCookError::Ok)
        {
            return false;
        }
        const auto sh = crd::kir::make_shape({1});
        const auto k  = [&](double v) { return g.constant(v, sh, crd::kir::DType::F32); };
        ck::SurfaceInputs in;
        in.world_normal = g.vec3(k(0.0), k(0.0), k(1.0));
        in.world_pos    = g.vec3(k(0.0), k(0.0), k(0.0));
        in.view_dir     = g.vec3(k(0.0), k(0.0), k(1.0));
        const ck::MaterialTemplate tmpl{&draw_material_surface, &d};
        const ck::VariantOptions   opts{crd::kir::material::AlphaMode::Opaque, 0.5};
        const tq::Technique        un = tq::unlit();
        return tq::build_fs_for_pass(tmpl, un, ck::PassType::Forward, opts, in, g, e,
                                     g.vec3(k(0.0), k(0.0), k(1.0)), g.vec3(k(1.0), k(1.0), k(1.0)), nullptr,
                                     0, nullptr, 0);
    };
    const auto make = [&](const char* vs_toml, const char* fs_toml,
                          std::unique_ptr<crd::gpu::IGpuProgram>&    out_vs,
                          std::unique_ptr<crd::gpu::IGpuProgram>&    out_fs,
                          std::unique_ptr<crd::gpu::IRasterProgram>& out_prog) {
        crd::kir::KGraph vg(&graph_alloc);
        crd::kir::KEntry ve;
        if (!cook_vs_toml(vs_toml, vg, ve)) { return false; }
        crd::kir::KGraph fg(&graph_alloc);
        crd::kir::KEntry fe;
        if (!cook_fs_toml(fs_toml, fg, fe)) { return false; }
        out_vs = ctx.create_program(vg, ve);
        out_fs = ctx.create_program(fg, fe);
        if (out_vs == nullptr || out_fs == nullptr) { return false; }
        out_prog = raster.create_raster_program(*out_vs, *out_fs);
        return out_prog != nullptr;
    };

    if (!make(kDrawLineVs, kDrawLineMat, s.line_vs, s.line_fs, s.line_prog)
        || !make(kDrawTriVs, kDrawTriMat, s.tri_vs, s.tri_fs, s.tri_prog)
        || !make(kDrawGridVs, kDrawGridMat, s.grid_vs, s.grid_fs, s.grid_prog))
    {
        CRD_LOG_ERROR(g_log_draw, "draw::init: authored draw program cook/compile failed");
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
