// crd-draw — submit_overlay impl (RET-6, ADR-0105): the RenderBuffer → gpu-context composition. The frame-graph pass
// died with the rhi renderer; THIS packs the 32-word header + per-bin instances into the ONE u32 draw buffer and
// chains `draw_overlay` calls — grid first, then triangles, then lines, each bin in Test → Always → GreaterDimmed
// order with XRay's dimmed double-emit (ADR-0066 §19.1 semantics, preserved bit for bit).

#include <crd/draw/overlay_pass.hpp>

#include <crd/draw/draw_assets.hpp>
#include <crd/draw/detail/gpu_types.hpp>
#include <crd/draw/render_buffer.hpp>
#include <crd/draw/renderer.hpp>
#include <crd/log/log.hpp>

#include <bit>

CRD_DEFINE_LOG_CHANNEL(g_log_overlay, "DrawOverlay", crd::log::LogLevel::Info)

namespace crd::draw
{
namespace
{
[[nodiscard]] crd::u32 fbits(crd::f32 f) noexcept { return std::bit_cast<crd::u32>(f); }

// GreaterDimmed tests where the scene OCCLUDES the primitive — the exact complement of the Test comparison.
[[nodiscard]] crd::gpu::DepthCompare complement(crd::gpu::DepthCompare c) noexcept
{
    using DC = crd::gpu::DepthCompare;
    switch (c)
    {
    case DC::Less: return DC::GreaterEqual;
    case DC::LessEqual: return DC::Greater;
    case DC::Greater: return DC::LessEqual;
    case DC::GreaterEqual: return DC::Less;
    case DC::Equal: return DC::NotEqual;
    case DC::NotEqual: return DC::Equal;
    case DC::Never: return DC::Always;
    case DC::Always: return DC::Never;
    }
    return DC::Always;
}

// XRay's occluded emission: alpha multiplied by ~0.3 (77/256 — the rhi original's exact factor).
[[nodiscard]] crd::u32 dim_color(crd::u32 packed) noexcept
{
    const crd::u32 a        = (packed >> 24U) & 0xFFU;
    const crd::u32 dimmed_a = (a * 77U) >> 8U;
    return (packed & 0x00FF'FFFFU) | (dimmed_a << 24U);
}

constexpr crd::u32 kVariantTest          = 0U;
constexpr crd::u32 kVariantAlways        = 1U;
constexpr crd::u32 kVariantGreaterDimmed = 2U;
constexpr crd::u32 kVariantCount         = 3U;

[[nodiscard]] crd::u32 variant_of(DepthMode m) noexcept
{
    switch (m)
    {
    case DepthMode::Test: return kVariantTest;
    case DepthMode::Always: return kVariantAlways;
    case DepthMode::XRay: return kVariantTest; // primary emission; the dimmed second is added explicitly
    }
    return kVariantAlways;
}

// Pack the 32-word header (the draw_assets.hpp contract) into the scratch's first words.
void pack_header(crd::containers::Array<crd::u32>& w, const OverlayPassConfig& cfg)
{
    w.clear();
    const crd::math::Vec4f* cols = &cfg.view_proj.c0;
    for (crd::u32 c = 0; c < 4U; ++c)
    {
        w.push_back(fbits(cols[c].x));
        w.push_back(fbits(cols[c].y));
        w.push_back(fbits(cols[c].z));
        w.push_back(fbits(cols[c].w));
    }
    w.push_back(fbits(cfg.viewport_px.x));      // [16]
    w.push_back(fbits(cfg.viewport_px.y));      // [17]
    w.push_back(cfg.category_mask);             // [18]
    w.push_back(fbits(cfg.time_s));             // [19]
    w.push_back(fbits(cfg.grid.camera_pos.x));  // [20]
    w.push_back(fbits(cfg.grid.camera_pos.y));  // [21]
    w.push_back(fbits(cfg.grid.camera_pos.z));  // [22]
    w.push_back(fbits(cfg.grid.plane_y));       // [23]
    w.push_back(cfg.grid.primary_color);        // [24]
    w.push_back(cfg.grid.secondary_color);      // [25]
    w.push_back(fbits(cfg.grid.primary_cell));  // [26]
    w.push_back(fbits(cfg.grid.secondary_cell)); // [27]
    w.push_back(fbits(cfg.grid.fade_distance)); // [28]
    w.push_back(cfg.grid.axis_x_color);         // [29]
    w.push_back(cfg.grid.axis_z_color);         // [30]
    w.push_back(0U);                            // [31] reserved
}
} // namespace

bool submit_overlay(crd::gpu::IRasterTarget& target, const RenderBuffer& buffer, const OverlayPassConfig& config)
{
    if (!is_initialised() || !is_overlay_enabled()) { return true; } // wire-unconditionally contract — a quiet no-op
    auto& s = detail::renderer_state();

    const auto lines     = buffer.lines();
    const auto triangles = buffer.triangles();
    if (lines.size() == 0U && triangles.size() == 0U && !config.grid.enabled) { return true; }

    // ONE header upload serves every draw in this submission (the instance region re-uploads per bin batch).
    pack_header(s.scratch, config);
    if (!s.raster->upload_storage(*s.storage, 0U, s.scratch.data(), kHeaderWords * 4U))
    {
        CRD_LOG_ERROR(g_log_overlay, "draw-buffer header upload refused -- skipping overlay");
        return false;
    }

    bool ok = true;

    // ── the grid: under everything (the rhi original drew it first) ──────────────────────────────────────────────
    if (config.grid.enabled)
    {
        ok = s.raster->draw_overlay(target, *s.grid_prog, *s.storage, crd::gpu::DepthCompare::Always, 6U) && ok;
    }

    // ── bin by (primitive, variant); XRay lands in BOTH Test (full color) and GreaterDimmed (dimmed) ─────────────
    const crd::gpu::DepthCompare compare_of[kVariantCount] = {config.depth_test, crd::gpu::DepthCompare::Always,
                                                              complement(config.depth_test)};

    // Triangles, then lines — each variant in compose-on-top order. Instances pack into the scratch tail and
    // upload at the instance region (word 32); batches over the configured cap keep unbounded counts rendering.
    const auto draw_bins = [&](bool is_tri) {
        const crd::u32 words_per = is_tri ? kTriInstanceWords : kLineInstanceWords;
        const crd::u32 cap       = is_tri ? s.config.max_triangles_per_frame : s.config.max_lines_per_frame;
        const crd::u32 verts_per = is_tri ? 3U : 6U;
        auto&          prog      = is_tri ? *s.tri_prog : *s.line_prog;
        const crd::usize count   = is_tri ? triangles.size() : lines.size();

        for (crd::u32 v = 0; v < kVariantCount; ++v)
        {
            const bool dim = (v == kVariantGreaterDimmed);
            s.scratch.clear();
            crd::u32 in_batch = 0U;
            const auto flush  = [&]() {
                if (in_batch == 0U) { return; }
                if (!s.raster->upload_storage(*s.storage, kHeaderWords * 4U, s.scratch.data(),
                                              static_cast<crd::u32>(s.scratch.size() * 4U))
                    || !s.raster->draw_overlay(target, prog, *s.storage, compare_of[v], in_batch * verts_per))
                {
                    ok = false;
                }
                s.scratch.clear();
                in_batch = 0U;
            };
            for (crd::usize i = 0; i < count; ++i)
            {
                const auto      m       = is_tri ? triangles[i].flags.depth() : lines[i].flags.depth();
                const bool      in_this = variant_of(m) == v || (m == DepthMode::XRay && v == kVariantGreaterDimmed);
                if (!in_this) { continue; }
                if (is_tri)
                {
                    const auto& t = triangles[i];
                    s.scratch.push_back(fbits(t.a.x)); s.scratch.push_back(fbits(t.a.y)); s.scratch.push_back(fbits(t.a.z));
                    s.scratch.push_back(fbits(t.b.x)); s.scratch.push_back(fbits(t.b.y)); s.scratch.push_back(fbits(t.b.z));
                    s.scratch.push_back(fbits(t.c.x)); s.scratch.push_back(fbits(t.c.y)); s.scratch.push_back(fbits(t.c.z));
                    s.scratch.push_back(dim ? dim_color(t.color) : t.color);
                    s.scratch.push_back(t.flags.raw);
                }
                else
                {
                    const auto& l = lines[i];
                    s.scratch.push_back(fbits(l.a.x)); s.scratch.push_back(fbits(l.a.y)); s.scratch.push_back(fbits(l.a.z));
                    s.scratch.push_back(fbits(l.b.x)); s.scratch.push_back(fbits(l.b.y)); s.scratch.push_back(fbits(l.b.z));
                    s.scratch.push_back(dim ? dim_color(l.color) : l.color);
                    s.scratch.push_back(l.flags.raw);
                    s.scratch.push_back(fbits(l.width));
                }
                ++in_batch;
                if (in_batch == cap) { flush(); }
            }
            flush();
        }
        (void)words_per;
    };
    draw_bins(true);  // solid triangles
    draw_bins(false); // AA lines on top

    return ok;
}

} // namespace crd::draw
