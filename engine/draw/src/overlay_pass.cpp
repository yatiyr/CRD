// crd-draw — submit_overlay impl (RET-6, ADR-0105): the RenderBuffer → gpu-context composition. The frame-graph pass
// died with the rhi renderer; THIS packs the 32-word header + per-bin instances into the ONE u32 draw buffer and
// chains `draw_overlay` calls — grid first, then triangles, then lines, each bin in Test → Always → GreaterDimmed
// order with XRay's dimmed double-emit (ADR-0066 §19.1 semantics, preserved bit for bit).

#include <crd/draw/overlay_pass.hpp>

#include <crd/draw/draw_assets.hpp>
#include <crd/draw/detail/gpu_types.hpp>
#include <crd/draw/render_buffer.hpp>
#include <crd/draw/renderer.hpp>
#include <crd/gpu/command_model.hpp> // RAF-12.4: record the overlay composites through the canonical command encoder
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

    // ⛔ REN-39: ONE upload per buffer per submission, ALL buckets packed contiguously, each bucket drawn as a
    // RANGE (`draw_overlay_range`'s first-vertex offset). The old scheme re-uploaded the SAME instance region
    // between the bucket draws — but uploads complete BEFORE the frame's command buffer executes a single draw
    // (the 38-G1 batch contract; the synchronous path equally), so every bucket rendered from the LAST bucket's
    // bytes: dashed lines, vanishing solids, a different corruption every frame. Upload-then-draw interleaving
    // on ONE region is not a slow path — it is a WRONG one.
    const crd::gpu::DepthCompare compare_of[kVariantCount] = {config.depth_test, crd::gpu::DepthCompare::Always,
                                                              complement(config.depth_test)};

    bool ok = true;

    // Pack `is_tri`'s three buckets after the shared header and upload ONCE; record each bucket's instance range.
    struct BucketRange
    {
        crd::u32 first = 0U; // first instance
        crd::u32 count = 0U;
    };
    BucketRange ranges[2][kVariantCount]; // [is_tri][variant]

    const auto pack_and_upload = [&](bool is_tri) {
        auto&            storage = is_tri ? *s.storage : *s.line_storage;
        const crd::u32   cap     = is_tri ? s.config.max_triangles_per_frame : s.config.max_lines_per_frame;
        const crd::usize count   = is_tri ? triangles.size() : lines.size();

        pack_header(s.scratch, config); // both buffers carry the header — the VS reads it at words 0..31
        crd::u32 packed  = 0U;
        crd::u32 dropped = 0U;
        for (crd::u32 v = 0; v < kVariantCount; ++v)
        {
            const bool dim          = (v == kVariantGreaterDimmed);
            ranges[is_tri ? 1 : 0][v].first = packed;
            for (crd::usize i = 0; i < count; ++i)
            {
                const auto m       = is_tri ? triangles[i].flags.depth() : lines[i].flags.depth();
                const bool in_this = variant_of(m) == v || (m == DepthMode::XRay && v == kVariantGreaterDimmed);
                if (!in_this) { continue; }
                if (packed == cap) // never silent: a clamped overlay says so (the no-silent-caps rule)
                {
                    ++dropped;
                    continue;
                }
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
                ++packed;
            }
            ranges[is_tri ? 1 : 0][v].count = packed - ranges[is_tri ? 1 : 0][v].first;
        }
        if (dropped > 0U)
        {
            CRD_LOG_WARN(g_log_overlay, "overlay {} bin over its per-frame cap ({}) -- {} instance(s) dropped",
                         is_tri ? "triangle" : "line", cap, dropped);
        }
        if (s.scratch.size() == static_cast<crd::usize>(kHeaderWords) && !config.grid.enabled) { return; }
        if (!s.raster->upload_storage(storage, 0U, s.scratch.data(), static_cast<crd::u32>(s.scratch.size() * 4U)))
        {
            CRD_LOG_ERROR(g_log_overlay, "draw-buffer upload refused -- skipping {} bins", is_tri ? "tri" : "line");
            for (crd::u32 v = 0; v < kVariantCount; ++v) { ranges[is_tri ? 1 : 0][v].count = 0U; }
            ok = false;
        }
    };
    pack_and_upload(true);
    pack_and_upload(false);

    // ── the draws, AFTER every upload landed: grid first (under the primitives), then triangles, then lines,
    // each variant in compose-on-top order. The grid depth-tests like any world-anchored geometry — `Always`
    // here is what let it ghost through the scene.
    //
    // RAF-12.4: recorded through the canonical command encoder. draw_overlay / draw_overlay_range are no longer
    // IRasterContext verbs; the encoder lowers the overlay SHAPE — a StoragePull draw with a SINGLE colour
    // attachment that LOADs and Alpha-blends, plus a read-only depth test carried by `compare` — straight to the
    // backend's per-draw overlay body. first_vertex>0 selects the ranged twin. One encoder scope per draw is
    // byte-identical to the chained verb calls (each began/ended its own read-only-depth rendering internally).
    namespace gpu    = crd::gpu;
    auto overlay_draw = [&](gpu::IRasterProgram& prog, gpu::IStorageBuffer& buf, gpu::DepthCompare compare,
                            crd::u32 first_vertex, crd::u32 vertex_count) -> bool
    {
        auto enc = s.raster->create_command_encoder();
        if (enc == nullptr) { return false; }
        gpu::RenderingDesc       rd{};
        gpu::ColorAttachmentDesc c{};
        c.target = &target;
        c.load   = gpu::LoadOp::Load;     // compose OVER the existing contents (the overlay's LOAD contract)
        c.blend  = gpu::BlendMode::Alpha; // srcAlpha·(1-srcAlpha) — the encoder's overlay signal
        rd.color.push_back(c);
        rd.depth.enabled = (compare != gpu::DepthCompare::Always); // read-only depth test carried by `compare`
        rd.depth.compare = compare;
        enc->begin_rendering(rd);
        gpu::RasterDrawPacket pk{};
        pk.program                        = &prog;
        pk.geometry.kind                  = gpu::GeometryKind::StoragePull;
        pk.geometry.vertex_or_index_count = vertex_count;
        pk.geometry.first_vertex          = first_vertex;
        gpu::ResourceBinding sb{};
        sb.kind   = gpu::BindingKind::StorageBuffer;
        sb.buffer = &buf;
        pk.bindings.push_back(sb);
        enc->draw(pk);
        enc->end_rendering();
        return true;
    };
    if (config.grid.enabled)
    {
        ok = overlay_draw(*s.grid_prog, *s.storage, config.depth_test, 0U, 6U) && ok;
    }
    for (crd::u32 v = 0; v < kVariantCount; ++v)
    {
        const BucketRange& r = ranges[1][v];
        if (r.count == 0U) { continue; }
        ok = overlay_draw(*s.tri_prog, *s.storage, compare_of[v], r.first * 3U, r.count * 3U) && ok;
    }
    for (crd::u32 v = 0; v < kVariantCount; ++v)
    {
        const BucketRange& r = ranges[0][v];
        if (r.count == 0U) { continue; }
        ok = overlay_draw(*s.line_prog, *s.line_storage, compare_of[v], r.first * 6U, r.count * 6U) && ok;
    }

    return ok;
}

} // namespace crd::draw
