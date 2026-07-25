// test_vulkan_frame_graph.cpp — REN-1 (D-007 row 98): the FRAME GRAPH gate.
//  · ONE SUBMISSION: a scene pass + an overlay pass compose into a single vkQueueSubmit (last_submit_count==1),
//    with the graph inserting the cross-pass barrier — and the readback is BIT-IDENTICAL to the synchronous
//    submit+wait-per-draw path (the recording is the same; only the per-draw submit/readback is removed).
//  · TRANSIENT ALIASING: graph-owned transients whose lifetimes are DISJOINT share backing memory (physical <
//    logical, 2 same-size transients collapse to 1 slot); OVERLAPPING-lifetime transients do NOT alias.
//  · validation-SILENT by counter (the RET ValidationCapture).

#include <crd/gpu/frame_graph.hpp>
#include <crd/gpu/raster_context.hpp>
#include <crd/gpu/vulkan_context.hpp>
#include <crd/gpu/vulkan_raster_context.hpp>
#include <crd/gpu/vulkan_validation_capture.hpp>

#include <crd/framecook/frame_asset.hpp>   // REN-36.2: the cooked frame-graph asset
#include <crd/framecook/frame_runtime.hpp> // REN-36.2: executing it through IFrameGraph
#include <crd/kir/ckir.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <ckir_raster_triangle.hpp> // the shared CKIR triangle VS/FS

#include <catch2/catch_test_macros.hpp>

#include <chrono> // REN-1 batching microbenchmark: CPU wall-clock of the submit-batching win
#include <cstdio> // REN-3.1 bench board printf
#include <cstring> // REN-36.2: strlen over the embedded asset text

using namespace crd;
namespace g = crd::gpu;

namespace
{

// per-pass recording state (carried through FgExecuteFn's void* user)
struct PassState
{
    g::FgImage         img{};
    g::FgBuffer        buf{};
    g::IRasterProgram* prog = nullptr;
};

void record_scene(g::IFrameContext& ctx, void* user)
{
    auto* s = static_cast<PassState*>(user);
    ctx.raster().draw_storage_depth(*ctx.image(s->img), *s->prog, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 0.0F,
                                    g::DepthCompare::Always, *ctx.buffer(s->buf), 3U);
}
void record_overlay(g::IFrameContext& ctx, void* user)
{
    auto* s = static_cast<PassState*>(user);
    (void)ctx.raster().draw_overlay(*ctx.image(s->img), *s->prog, *ctx.buffer(s->buf), g::DepthCompare::Always, 3U);
}
void record_noop(g::IFrameContext& /*ctx*/, void* /*user*/) {}

// REN-2 RTT: pass 1 renders a red triangle over a green clear INTO the sampled transient (draw_storage, color-only).
struct RttOffscreen
{
    g::FgImage         rtt{};
    g::FgBuffer        buf{};
    g::IRasterProgram* prog = nullptr;
};
void record_rtt_offscreen(g::IFrameContext& ctx, void* user)
{
    auto* s = static_cast<RttOffscreen*>(user);
    ctx.raster().draw_storage(*ctx.image(s->rtt), *s->prog, g::ClearColor{0.0F, 1.0F, 0.0F, 1.0F}, *ctx.buffer(s->buf), 3U);
}
// REN-2 RTT: pass 2 FULL-SCREEN SAMPLES the transient into the final target (draw_textured) — the round-trip.
struct RttCompose
{
    g::FgImage         rtt{};
    g::FgImage         dst{};
    g::IRasterProgram* prog = nullptr;
};
void record_rtt_compose(g::IFrameContext& ctx, void* user)
{
    auto* s = static_cast<RttCompose*>(user);
    ctx.raster().draw_textured(*ctx.image(s->dst), *s->prog, g::ClearColor{0.0F, 0.0F, 1.0F, 1.0F}, *ctx.texture(s->rtt), 3U);
}

// REN-3.1: pass 1 RENDERS a depth map into a D32Float+sampled transient — no colour attachment at all.
struct ShadowDepthPass
{
    g::FgImage         map{};
    g::FgBuffer        buf{};
    g::IRasterProgram* prog = nullptr;
};
void record_shadow_depth(g::IFrameContext& ctx, void* user)
{
    auto* s = static_cast<ShadowDepthPass*>(user);
    // clear depth to 1.0 (far); the FS writes 0.5, LessEqual so the written depth always beats the clear
    ctx.raster().draw_storage_depth_only(*ctx.image(s->map), *s->prog, 1.0F, g::DepthCompare::LessEqual,
                                         *ctx.buffer(s->buf), 3U);
}
// REN-3.2: one CASCADE pass — renders a constant depth into ONE SLICE of the layered depth atlas. `image_layer`
// is the only difference from the REN-3.1 single-map pass; everything else is the same depth-only draw.
struct CascadeDepthPass
{
    g::FgImage         atlas{};
    g::FgBuffer        buf{};
    g::IRasterProgram* prog  = nullptr;
    u32                layer = 0;
};
void record_cascade_depth(g::IFrameContext& ctx, void* user)
{
    auto* s = static_cast<CascadeDepthPass*>(user);
    ctx.raster().draw_storage_depth_only(*ctx.image_layer(s->atlas, s->layer), *s->prog, 1.0F,
                                         g::DepthCompare::LessEqual, *ctx.buffer(s->buf), 3U);
}
// REN-3.1: pass 2 SAMPLES the rendered depth map through the COMPARISON sampler (draw_shadow) and shades.
struct ShadowSamplePass
{
    g::FgImage         map{};
    g::FgImage         dst{};
    g::IRasterProgram* prog = nullptr;
};
void record_shadow_sample(g::IFrameContext& ctx, void* user)
{
    auto* s = static_cast<ShadowSamplePass*>(user);
    ctx.raster().draw_shadow(*ctx.image(s->dst), *s->prog, g::ClearColor{0.0F, 0.0F, 1.0F, 1.0F},
                             *ctx.texture(s->map), 3U);
}

// stand up a graphics-capable Vulkan raster context (or return nullptrs to SKIP)
struct Rig
{
    std::unique_ptr<g::IGpuContext>     ctx;
    g::VulkanGpuContext*                vk = nullptr;
    std::unique_ptr<g::IRasterContext>  raster;
};
Rig make_rig()
{
    Rig r;
    g::GpuContextConfig cfg;
    cfg.backend           = g::GpuBackend::Vulkan;
    cfg.headless          = true;
    cfg.enable_validation = true;
    r.ctx = g::create_vulkan_gpu_context(cfg);
    r.vk  = r.ctx != nullptr ? static_cast<g::VulkanGpuContext*>(r.ctx.get()) : nullptr;
    if (r.vk == nullptr || !r.vk->graphics_capable() || !r.vk->shader_object()) { return r; }
    r.raster = g::create_vulkan_raster_context(*r.vk);
    return r;
}

} // namespace

TEST_CASE("REN-1 GATE: frame graph composes scene + overlay in ONE submission, readback bit-matches sync",
          "[gpu-context][vulkan][frame-graph][ren1][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;

    memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    gputest::build_triangle_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    gputest::build_triangle_fs(fg, fe);
    auto vs = rig.vk->create_program(vg, ve);
    auto fs = rig.vk->create_program(fg, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr);
    auto prog = raster.create_raster_program(*vs, *fs);
    REQUIRE(prog != nullptr);

    // ── the SYNCHRONOUS reference on target A (submit+wait per draw) ──
    auto ref  = raster.create_color_depth_target(64U, 64U);
    auto sb_a = raster.create_storage_buffer(16U);
    REQUIRE(ref != nullptr);
    REQUIRE(sb_a != nullptr);
    raster.draw_storage_depth(*ref, *prog, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 0.0F, g::DepthCompare::Always, *sb_a, 3U);
    (void)raster.draw_overlay(*ref, *prog, *sb_a, g::DepthCompare::Always, 3U);
    const u32 sync_center = ref->read_pixel(32U, 32U);
    const u32 sync_corner = ref->read_pixel(1U, 1U);
    CHECK((sync_center & 0xFFU) > 200U); // the red triangle covers the centre
    CHECK((sync_corner & 0x00FFFFFFU) == 0U); // the corner stays the black clear

    // ── the FRAME GRAPH on target B (ONE submission) ──
    g::ValidationCapture capture(*rig.vk);
    auto out  = raster.create_color_depth_target(64U, 64U);
    auto sb_b = raster.create_storage_buffer(16U);
    REQUIRE(out != nullptr);
    REQUIRE(sb_b != nullptr);
    auto fgraph = raster.create_frame_graph();
    REQUIRE(fgraph != nullptr);

    const g::FgImage  img = fgraph->import_target(*out);
    const g::FgBuffer buf = fgraph->import_storage(*sb_b);
    PassState         state{img, buf, prog.get()};
    fgraph->add_pass("scene").reads(buf).writes(img).execute(&record_scene, &state);
    fgraph->add_pass("overlay").reads(buf).read_writes(img).execute(&record_overlay, &state);
    REQUIRE(fgraph->build());
    fgraph->execute();

    CHECK(fgraph->last_submit_count() == 1U);       // scene + overlay in ONE vkQueueSubmit
    CHECK(fgraph->last_barrier_count() >= 1U);       // at least the scene→overlay cross-pass barrier
    CHECK(out->read_pixel(32U, 32U) == sync_center); // BIT-IDENTICAL to the synchronous path
    CHECK(out->read_pixel(1U, 1U) == sync_corner);

    if (capture.error_or_warning_count() > 0U)
    {
        const auto msgs = capture.messages();
        crd::u32   shown = 0;
        for (usize i = 0; i < msgs.size() && shown < 4U; ++i)
        {
            if (msgs[i].severity == g::ValidationSeverity::Info) { continue; }
            WARN("[ren1 capture] id=" << msgs[i].message_id_number << " " << msgs[i].message_text.c_str());
            ++shown;
        }
    }
    CHECK(capture.error_count() == 0U);   // the whole frame-graph lifecycle validation-SILENT
    CHECK(capture.warning_count() == 0U);

    // reuse across frames: reset + rebuild + re-execute still submits exactly once
    fgraph->reset();
    const g::FgImage  img2 = fgraph->import_target(*out);
    const g::FgBuffer buf2 = fgraph->import_storage(*sb_b);
    PassState         st2{img2, buf2, prog.get()};
    fgraph->add_pass("scene").reads(buf2).writes(img2).execute(&record_scene, &st2);
    REQUIRE(fgraph->build());
    fgraph->execute();
    CHECK(fgraph->last_submit_count() == 1U);
}

TEST_CASE("REN-1: transient resources ALIAS memory across disjoint lifetimes", "[gpu-context][vulkan][frame-graph][ren1][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;

    g::FgImageDesc desc{};
    desc.width  = 128U;
    desc.height = 128U;
    desc.format = g::FgImageFormat::RGBA8Unorm;

    // DISJOINT lifetimes: X written in pass 0, Y written in pass 1 → they never coexist → SHARE memory.
    {
        auto fgraph = raster.create_frame_graph();
        REQUIRE(fgraph != nullptr);
        const g::FgImage x = fgraph->create_transient_image(desc);
        const g::FgImage y = fgraph->create_transient_image(desc);
        REQUIRE(x.valid());
        REQUIRE(y.valid());
        fgraph->add_pass("a").writes(x).execute(&record_noop, nullptr);
        fgraph->add_pass("b").writes(y).execute(&record_noop, nullptr);
        REQUIRE(fgraph->build());

        CHECK(fgraph->transient_logical_bytes() > 0U);
        CHECK(fgraph->transient_memory_bytes() < fgraph->transient_logical_bytes());     // ALIASED
        CHECK(fgraph->transient_memory_bytes() * 2U == fgraph->transient_logical_bytes()); // 2 equal → 1 slot

        g::ValidationCapture capture(*rig.vk);
        fgraph->execute(); // an aliased-but-unused pair executes validation-silent
        CHECK(capture.error_count() == 0U);
        CHECK(capture.warning_count() == 0U);
    }

    // OVERLAPPING lifetimes: P and Q both written in the SAME pass → they coexist → NO aliasing.
    {
        auto fgraph = raster.create_frame_graph();
        REQUIRE(fgraph != nullptr);
        const g::FgImage p = fgraph->create_transient_image(desc);
        const g::FgImage q = fgraph->create_transient_image(desc);
        fgraph->add_pass("both").writes(p).writes(q).execute(&record_noop, nullptr);
        REQUIRE(fgraph->build());
        CHECK(fgraph->transient_memory_bytes() == fgraph->transient_logical_bytes()); // coexisting → not shared
    }

    // a transient that NO pass writes is a build error (never a partial schedule)
    {
        auto fgraph = raster.create_frame_graph();
        const g::FgImage orphan = fgraph->create_transient_image(desc);
        REQUIRE(orphan.valid());
        fgraph->add_pass("reads-only").reads(orphan).execute(&record_noop, nullptr);
        CHECK_FALSE(fgraph->build());
    }
}

TEST_CASE("REN-2 GATE: render-to-texture -- a pass renders a transient, a LATER pass SAMPLES it",
          "[gpu-context][vulkan][frame-graph][ren2][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;

    memory::TlsfAllocator alloc(8U << 20U);
    // offscreen program: a red triangle (build_triangle) drawn into the transient over a green clear
    kir::KGraph tvg(&alloc);
    kir::KEntry tve;
    gputest::build_triangle_vs(tvg, tve);
    kir::KGraph tfg(&alloc);
    kir::KEntry tfe;
    gputest::build_triangle_fs(tfg, tfe);
    auto tvs = rig.vk->create_program(tvg, tve);
    if (tvs == nullptr) { SKIP("shader compile unavailable"); }
    auto tfs      = rig.vk->create_program(tfg, tfe);
    auto tri_prog = raster.create_raster_program(*tvs, *tfs);
    // compose program: a FULL-SCREEN quad sampling the bound texture at UV (build_textured_vs + build_sample_fs)
    kir::KGraph svg(&alloc);
    kir::KEntry sve;
    gputest::build_textured_vs(svg, sve);
    kir::KGraph sfg(&alloc);
    kir::KEntry sfe;
    gputest::build_sample_fs(sfg, sfe);
    auto svs         = rig.vk->create_program(svg, sve);
    auto sfs         = rig.vk->create_program(sfg, sfe);
    auto sample_prog = raster.create_raster_program(*svs, *sfs);
    REQUIRE(tri_prog != nullptr);
    REQUIRE(sample_prog != nullptr);

    constexpr u32 dim   = 64U;
    auto          dst   = raster.create_color_target(dim, dim);
    auto          dummy = raster.create_storage_buffer(16U); // draw_storage binds a buffer (the triangle ignores it)
    REQUIRE(dst != nullptr);
    REQUIRE(dummy != nullptr);

    g::ValidationCapture capture(*rig.vk);
    auto                 fgraph = raster.create_frame_graph();
    REQUIRE(fgraph != nullptr);

    g::FgImageDesc rdesc{};
    rdesc.width   = dim;
    rdesc.height  = dim;
    rdesc.format  = g::FgImageFormat::RGBA8Unorm;
    rdesc.sampled = true; // ← the RTT transient: drawn into, then SAMPLED
    const g::FgImage  rtt = fgraph->create_transient_image(rdesc);
    REQUIRE(rtt.valid());
    const g::FgImage  fin = fgraph->import_target(*dst);
    const g::FgBuffer buf = fgraph->import_storage(*dummy);

    RttOffscreen off{rtt, buf, tri_prog.get()};
    RttCompose   com{rtt, fin, sample_prog.get()};
    fgraph->add_pass("offscreen").reads(buf).writes(rtt).execute(&record_rtt_offscreen, &off);
    fgraph->add_pass("compose").reads(rtt).writes(fin).execute(&record_rtt_compose, &com);
    REQUIRE(fgraph->build());
    fgraph->execute();

    CHECK(fgraph->last_submit_count() == 1U);  // offscreen + compose in ONE submission
    CHECK(fgraph->last_barrier_count() >= 1U);  // incl. the COLOR_ATTACHMENT->SHADER_READ_ONLY RTT barrier
    const u32 center = dst->read_pixel(dim / 2U, dim / 2U); // samples the transient's RED triangle centre
    const u32 corner = dst->read_pixel(2U, 2U);             // samples the transient's GREEN clear corner
    CHECK((center & 0xFFU) > 180U);          // red -> the compose pass SAMPLED what the offscreen pass rendered
    CHECK(((corner >> 8U) & 0xFFU) > 180U);  // green -> the RTT round-trip is faithful (not the blue compose clear)
    CHECK((corner & 0xFFU) < 80U);           // ... and NOT red at the corner

    if (capture.error_or_warning_count() > 0U)
    {
        const auto msgs = capture.messages();
        for (usize i = 0; i < msgs.size(); ++i)
        {
            if (msgs[i].severity == g::ValidationSeverity::Info) { continue; }
            WARN("[ren2 capture] " << msgs[i].message_text.c_str());
        }
    }
    CHECK(capture.error_count() == 0U);   // the RTT frame is validation-SILENT
    CHECK(capture.warning_count() == 0U);
}

TEST_CASE("REN-2 Half B: the textured scene draw SAMPLES the material base-color map, not a flat colour (Vulkan)",
          "[gpu-context][vulkan][ren2][material][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;

    // the material shader: vertex-pull {x,y,z,u,v} + sample the base-color map at UV
    memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    gputest::build_pull_textured_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    gputest::build_pull_textured_fs(fg, fe);
    auto vs = rig.vk->create_program(vg, ve);
    if (vs == nullptr) { SKIP("shader compile unavailable"); }
    auto fs   = rig.vk->create_program(fg, fe);
    auto prog = raster.create_raster_program(*vs, *fs);
    REQUIRE(prog != nullptr);

    constexpr u32 dim = 64U;
    auto          tgt = raster.create_color_depth_target(dim, dim);
    // the base-color (albedo) map: left texel RED, right texel GREEN (2x1 RGBA8)
    const u8 texels[8] = {255U, 0U, 0U, 255U, 0U, 255U, 0U, 255U};
    auto     tex       = raster.create_texture(2U, 1U, texels);
    // a full-screen triangle, vertex-pulled as {x,y,z,u,v} — UV.x spans 0..1 across the screen
    const float verts[15] = {-1.0F, -1.0F, 0.0F, 0.0F, 0.0F, 3.0F, -1.0F, 0.0F, 2.0F,
                             0.0F,  -1.0F, 3.0F, 0.0F, 0.0F, 2.0F};
    auto        sb        = raster.create_storage_buffer(sizeof(verts));
    REQUIRE(tgt != nullptr);
    REQUIRE(tex != nullptr);
    REQUIRE(sb != nullptr);
    REQUIRE(raster.upload_storage(*sb, 0U, verts, sizeof(verts)));

    g::ValidationCapture capture(*rig.vk);
    raster.draw_storage_textured_depth(*tgt, *prog, g::ClearColor{0.0F, 0.0F, 1.0F, 1.0F}, 0.0F, g::DepthCompare::Always,
                                       *sb, *tex, 3U);

    const u32 left  = tgt->read_pixel(16U, dim / 2U); // UV.x~0.25 → the RED base-color texel
    const u32 right = tgt->read_pixel(48U, dim / 2U); // UV.x~0.75 → the GREEN base-color texel
    CHECK((left & 0xFFU) > 180U);           // sampled RED — the forward pass sampled the map, not the flat/blue clear
    CHECK(((left >> 8U) & 0xFFU) < 80U);
    CHECK(((right >> 8U) & 0xFFU) > 180U);  // sampled GREEN — and UV.x drives it
    CHECK((right & 0xFFU) < 80U);
    CHECK(capture.error_count() == 0U);
    CHECK(capture.warning_count() == 0U);
}

// REN-1 BATCHING BENCHMARK (hidden [.] — run with the [ren1-bench] tag). Composes an N-draw "frame" two ways and
// times the CPU wall-clock per frame: (a) the SYNCHRONOUS substrate — N draw_storage_depth[_load] calls, each its
// OWN vkQueueSubmit + fence WAIT; (b) the FRAME GRAPH — N passes recorded into one command buffer, ONE submit+wait.
// The win is the collapse of N CPU↔GPU fence stalls to one; it grows with N. Reports the ratio (WARN); the only
// hard assertion is that the graph is not SLOWER at N=64 (a batching regression would fail it) — timings go to the
// docs/bench board, never a brittle magnitude gate (the SAME-PASS timing doctrine).
// REN-3.1 GATE — the device RENDERS a shadow map. Pass 1 writes a depth ramp into a D32Float+`sampled` transient with
// NO colour attachment; pass 2 samples that transient through the COMPARISON sampler with ref = uv.x and shades. Since
// stored depth ≈ ref along the diagonal, the compare flips across the image: this proves the depth RTT round-trip
// (render depth → barrier → sample depth), which is the capability `ckir_lighting.hpp:992` says the device lacked.
TEST_CASE("REN-3.1 GATE: a depth-only pass RENDERS a shadow map, a later pass SAMPLES it (comparison sampler)",
          "[gpu-context][vulkan][frame-graph][ren3][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;

    memory::TlsfAllocator alloc(8U << 20U);
    constexpr u32         dim = 32U;

    // pass-1 program: full-screen triangle + a DEPTH-ONLY fs (no colour output)
    kir::KGraph dvg(&alloc);
    kir::KEntry dve;
    gputest::build_fullscreen_vs(dvg, dve);
    kir::KGraph dfg(&alloc);
    kir::KEntry dfe;
    gputest::build_depth_only_const_fs(dfg, dfe, 0.5); // render the SAME 0.5 map the old tests uploaded
    auto dvs = rig.vk->create_program(dvg, dve);
    if (dvs == nullptr) { SKIP("shader compile unavailable"); }
    auto dfs        = rig.vk->create_program(dfg, dfe);
    auto depth_prog = raster.create_raster_program(*dvs, *dfs);

    // pass-2 program: full-screen quad sampling the depth map with SampleCmp (ref = uv.x)
    kir::KGraph svg(&alloc);
    kir::KEntry sve;
    gputest::build_textured_vs(svg, sve);
    kir::KGraph sfg(&alloc);
    kir::KEntry sfe;
    gputest::build_shadow_fs(sfg, sfe);
    auto svs         = rig.vk->create_program(svg, sve);
    auto sfs         = rig.vk->create_program(sfg, sfe);
    auto shadow_prog = raster.create_raster_program(*svs, *sfs);
    REQUIRE(depth_prog != nullptr);
    REQUIRE(shadow_prog != nullptr);

    auto dst   = raster.create_color_target(dim, dim);
    auto dummy = raster.create_storage_buffer(16U);
    REQUIRE(dst != nullptr);
    REQUIRE(dummy != nullptr);

    g::ValidationCapture capture(*rig.vk);
    auto                 fgraph = raster.create_frame_graph();
    REQUIRE(fgraph != nullptr);

    g::FgImageDesc ddesc{};
    ddesc.width   = dim;
    ddesc.height  = dim;
    ddesc.format  = g::FgImageFormat::D32Float; // ← the DEPTH RTT transient
    ddesc.sampled = true;                       // ← rendered into, then SAMPLED
    const g::FgImage shadow_map = fgraph->create_transient_image(ddesc);
    REQUIRE(shadow_map.valid());
    // the DX12-shaped negative gate, asserted on both backends: a sampled depth transient must resolve to a real
    // texture. On DX12 a typed-D32 resource cannot carry an SRV, so this is null unless the R32_TYPELESS dance ran.
    const g::FgImage  fin = fgraph->import_target(*dst);
    const g::FgBuffer buf = fgraph->import_storage(*dummy);

    ShadowDepthPass  dpass{shadow_map, buf, depth_prog.get()};
    ShadowSamplePass spass{shadow_map, fin, shadow_prog.get()};
    fgraph->add_pass("shadow_depth").reads(buf).writes(shadow_map).execute(&record_shadow_depth, &dpass);
    fgraph->add_pass("shade").reads(shadow_map).writes(fin).execute(&record_shadow_sample, &spass);
    REQUIRE(fgraph->build());
    fgraph->execute();

    CHECK(fgraph->last_submit_count() == 1U); // depth pass + shade pass in ONE submission
    CHECK(fgraph->last_barrier_count() >= 1U); // incl. the DEPTH_ATTACHMENT -> read-only RTT barrier

    // The RENDERED map holds 0.5 everywhere — byte-for-byte what `fill_uniform_depth` used to UPLOAD. build_shadow_fs
    // compares ref = uv.x against it with LessEqual: lit where uv.x <= 0.5 (screen-left, white), shadowed where
    // uv.x > 0.5 (screen-right, black). Identical behaviour on a device-rendered map as on an uploaded one is the claim.
    const u32 lit      = dst->read_pixel(2U, dim / 2U);
    const u32 shadowed = dst->read_pixel(dim - 3U, dim / 2U);
    CHECK(lit != shadowed);           // the depth RTT round-trip produced a real, varying shadow term
    CHECK((lit & 0xFFU) > 180U);      // left: comparison passes -> lit
    CHECK((shadowed & 0xFFU) < 80U);  // right: comparison fails  -> shadowed

    if (capture.error_or_warning_count() > 0U)
    {
        const auto msgs = capture.messages();
        for (usize i = 0; i < msgs.size(); ++i)
        {
            if (msgs[i].severity == g::ValidationSeverity::Info) { continue; }
            WARN("[ren3 capture] " << msgs[i].message_text.c_str());
        }
    }
    CHECK(capture.error_count() == 0U); // the depth-RTT frame is validation-SILENT
}

// REN-3.2 GATE — the CSM per-cascade depth-ARRAY atlas. FOUR depth-only passes each render a DIFFERENT constant
// depth (0.2/0.4/0.6/0.8) into their OWN slice of one layered `D32Float`+`sampled` transient; a fifth pass samples
// the whole array through `sampler2DArrayShadow` at all four slices with ref = 0.5 and packs the results.
// ⛔ A hardware comparison sampler computes `compare(REF, STORED)`, not `compare(stored, ref)` — so with
// LessEqual and ref = 0.5 the slices that PASS are the FAR ones (0.6, 0.8 >= 0.5), i.e. slices 2 and 3, giving
// the 4-bit code 0b1100 = 12/15 = 0.8. The first run of this gate asserted the mirror image and failed on all
// three channels while the implementation was already correct; the operand order is the thing to check first.
// ⛔ This is the gate's whole point: the two ways a layered atlas silently degrades — per-slice attachment views
// that all address slice 0, and a plain TEXTURE2D SRV that reads only slice 0 — BOTH make all four results equal,
// giving code 0 or 15. Neither can produce an intermediate pattern, so an intermediate code proves the slices are
// genuinely distinct. A `CHECK(atlas.valid())`-style test would have passed under both defects.
TEST_CASE("REN-3.2 GATE: four cascade passes write four SLICES of a depth-array atlas; one pass samples all four",
          "[gpu-context][vulkan][frame-graph][ren3][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;

    memory::TlsfAllocator alloc(16U << 20U);
    constexpr u32         dim      = 32U;
    constexpr u32         cascades = 4U;
    const double          depths[cascades] = {0.2, 0.4, 0.6, 0.8};

    // one depth-only program PER cascade — each writes its own constant depth
    kir::KGraph dvg(&alloc);
    kir::KEntry dve;
    gputest::build_fullscreen_vs(dvg, dve);
    auto dvs = rig.vk->create_program(dvg, dve);
    if (dvs == nullptr) { SKIP("shader compile unavailable"); }

    containers::Array<std::unique_ptr<g::IRasterProgram>> cascade_progs(&alloc);
    containers::Array<std::unique_ptr<g::IGpuProgram>>    cascade_fs(&alloc);
    for (u32 c = 0; c < cascades; ++c)
    {
        kir::KGraph dfg(&alloc);
        kir::KEntry dfe;
        gputest::build_depth_only_const_fs(dfg, dfe, depths[c]);
        auto fs = rig.vk->create_program(dfg, dfe);
        REQUIRE(fs != nullptr);
        auto prog = raster.create_raster_program(*dvs, *fs);
        REQUIRE(prog != nullptr);
        cascade_progs.push_back(static_cast<std::unique_ptr<g::IRasterProgram>&&>(prog));
        cascade_fs.push_back(static_cast<std::unique_ptr<g::IGpuProgram>&&>(fs)); // keep the FS alive
    }

    // the probe program: samples the ARRAY at all four slices with ref = 0.5
    kir::KGraph pvg(&alloc);
    kir::KEntry pve;
    gputest::build_textured_vs(pvg, pve);
    kir::KGraph pfg(&alloc);
    kir::KEntry pfe;
    gputest::build_cascade_probe_fs(pfg, pfe, 0.5);
    auto pvs        = rig.vk->create_program(pvg, pve);
    auto pfs        = rig.vk->create_program(pfg, pfe);
    REQUIRE(pfs != nullptr); // the arrayed-shadow emitter path must COMPILE (vec4(uv, layer, ref))
    auto probe_prog = raster.create_raster_program(*pvs, *pfs);
    REQUIRE(probe_prog != nullptr);

    auto dst   = raster.create_color_target(dim, dim);
    auto dummy = raster.create_storage_buffer(16U);
    REQUIRE(dst != nullptr);
    REQUIRE(dummy != nullptr);

    g::ValidationCapture capture(*rig.vk);
    auto                 fgraph = raster.create_frame_graph();
    REQUIRE(fgraph != nullptr);

    g::FgImageDesc adesc{};
    adesc.width   = dim;
    adesc.height  = dim;
    adesc.format  = g::FgImageFormat::D32Float;
    adesc.sampled = true;
    adesc.layers  = cascades; // ← the CSM cascade ATLAS
    const g::FgImage atlas = fgraph->create_transient_image(adesc);
    REQUIRE(atlas.valid());

    // the stated cap is a REJECTION, not a clamp — an over-range atlas must come back invalid, not truncated
    g::FgImageDesc over = adesc;
    over.layers         = g::kFgMaxImageLayers + 1U;
    CHECK_FALSE(fgraph->create_transient_image(over).valid());
    over.layers = 0U;
    CHECK_FALSE(fgraph->create_transient_image(over).valid());

    const g::FgImage  fin = fgraph->import_target(*dst);
    const g::FgBuffer buf = fgraph->import_storage(*dummy);

    containers::Array<CascadeDepthPass> cpasses(&alloc);
    cpasses.reserve(cascades); // ⛔ reserve FIRST — the passes hold pointers into this array
    for (u32 c = 0; c < cascades; ++c)
    {
        cpasses.push_back(CascadeDepthPass{atlas, buf, cascade_progs[c].get(), c});
    }
    for (u32 c = 0; c < cascades; ++c)
    {
        fgraph->add_pass("cascade").reads(buf).writes(atlas).execute(&record_cascade_depth, &cpasses[c]);
    }
    ShadowSamplePass ppass{atlas, fin, probe_prog.get()};
    fgraph->add_pass("cascade_probe").reads(atlas).writes(fin).execute(&record_shadow_sample, &ppass);
    REQUIRE(fgraph->build());
    fgraph->execute();

    CHECK(fgraph->last_submit_count() == 1U); // four cascades + the probe in ONE submission

    const u32 px     = dst->read_pixel(dim / 2U, dim / 2U);
    const u32 code   = px & 0xFFU;          // R: the 4-bit pattern over ALL FOUR slices
    const u32 slice1 = (px >> 8U) & 0xFFU;  // G: slice 1 alone (stored 0.4; 0.5 <= 0.4 fails)
    const u32 slice3 = (px >> 16U) & 0xFFU; // B: slice 3 alone (stored 0.8; 0.5 <= 0.8 passes)

    // 0b1100 = 12/15 = 0.8 -> 204. Both degenerate failures land OUTSIDE this window: every slice reading
    // cascade 0 (stored 0.2) gives code 0, and every slice reading the last-written cascade gives code 255.
    CHECK(code > 185U);
    CHECK(code < 225U);
    // ...and these two pin the distinctness on their own: same texture, same ref, OPPOSITE results.
    CHECK(slice1 < 80U);  // cascade 1 stored 0.4 -> the 0.5 ref does NOT pass
    CHECK(slice3 > 180U); // cascade 3 stored 0.8 -> it does. Different from slice 1 => the slices are DISTINCT.

    if (capture.error_or_warning_count() > 0U)
    {
        const auto msgs = capture.messages();
        for (usize i = 0; i < msgs.size(); ++i)
        {
            if (msgs[i].severity == g::ValidationSeverity::Info) { continue; }
            WARN("[ren3.2 capture] " << msgs[i].message_text.c_str());
        }
    }
    CHECK(capture.error_count() == 0U); // the layered depth-RTT frame is validation-SILENT
    CHECK(capture.warning_count() == 0U);
}

// REN-3.1: a depth transient that is NOT sampled must still work as a plain depth buffer — the no-regression guard
// for the SceneRenderer's forward pass, which rides draw_storage_depth through the graph.
TEST_CASE("REN-3.1: a non-sampled depth transient still serves as a plain depth buffer (no regression)",
          "[gpu-context][vulkan][frame-graph][ren3][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;
    auto  fgraph = raster.create_frame_graph();
    REQUIRE(fgraph != nullptr);

    g::FgImageDesc ddesc{};
    ddesc.width   = 32U;
    ddesc.height  = 32U;
    ddesc.format  = g::FgImageFormat::D32Float;
    ddesc.sampled = false; // the ordinary depth-buffer case
    const g::FgImage d = fgraph->create_transient_image(ddesc);
    CHECK(d.valid());
}

// ── REN-36.2 GATE: the SAME frame, authored as an ASSET, must match the hand-written C++ frame EXACTLY. ──────
// This is the payoff of the whole "everything is an asset" direction, and it is deliberately the strictest form:
// not "the asset renders something plausible" but "the asset's readback is BIT-IDENTICAL to the C++ frame's".
// The asset below is the REN-3.1 shadow frame — a depth-only pass renders a shadow map into a D32Float+sampled
// transient, a later pass samples it through the comparison sampler. Nothing about it names Vulkan or DX12; the
// identical text is used by the DX12 gate.
namespace
{
constexpr const char* kShadowFrameToml = R"(
schema = 1
name   = "ren31_shadow"

[[resource]]
name = "shadow_map"
format = "D32Float"
width = 32
height = 32
sampled = true

[[draw_list]]
name = "occluders"
all  = ["MeshRenderer"]

[[pass]]
name = "shadow_depth"
kind = "raster.depth_only"
draw_list = "occluders"
writes = ["shadow_map"]
clear_depth = 1.0
depth = "LessEqual"

[[pass]]
name = "shade"
kind = "raster.fullscreen"
reads = ["shadow_map"]
writes = ["@output"]
shader = "test://shaders/shadow_sample"
)";

// The host: resolves the asset's NAMES to live objects. In a game this is the renderer; in the editor it is the
// viewport; here it is the test. Same asset, different hosts — that is the point of the seam.
class TestHost final : public crd::framecook::IFrameGraphHost
{
public:
    TestHost(g::IRasterTarget* out, g::IRasterProgram* depth_prog, g::IRasterProgram* shade_prog, g::IStorageBuffer* sb)
        : m_out(out), m_depth(depth_prog), m_shade(shade_prog), m_sb(sb)
    {
    }
    g::IRasterTarget*  output() override { return m_out; }
    g::IRasterProgram* program(containers::StringView id) override
    {
        return (id.size() > 0U) ? m_shade : nullptr; // this frame has exactly one named shader
    }
    bool draw_list(containers::StringView /*name*/, crd::framecook::DrawListBinding& out) override
    {
        out.storage      = m_sb;
        out.program      = m_depth;
        out.vertex_count = 3U;
        return true;
    }

private:
    g::IRasterTarget*  m_out   = nullptr;
    g::IRasterProgram* m_depth = nullptr;
    g::IRasterProgram* m_shade = nullptr;
    g::IStorageBuffer* m_sb    = nullptr;
};

// ── REN-36.3: the CASCADE asset. ONE `[[pass]]` declaration, four cascades. ──────────────────────────────────
// `for_each = "light.0.cascades"` + `writes = ["shadow_atlas[$index]"]` is the whole multi-view mechanism: the
// executor asks the HOST how many instances exist this frame and expands the declaration into that many ordinary
// passes before build(), so aliasing and the barrier schedule see nothing special. Note what the asset does NOT
// say: no layer loop, no slice indices, no backend concept — and no cascade COUNT either, because the count is
// scene state, not authored state.
constexpr const char* kCascadeFrameToml = R"(
schema = 1
name   = "ren36_cascades"

[[resource]]
name = "shadow_atlas"
format = "D32Float"
width = 32
height = 32
layers = 4
sampled = true

[[draw_list]]
name = "occluders"
all  = ["MeshRenderer"]

[[pass]]
name = "cascade"
kind = "raster.depth_only"
draw_list = "occluders"
for_each = "light.0.cascades"
writes = ["shadow_atlas[$index]"]
clear_depth = 1.0
depth = "LessEqual"

[[pass]]
name = "cascade_probe"
kind = "raster.fullscreen"
reads = ["shadow_atlas"]
writes = ["@output"]
shader = "test://shaders/cascade_probe"
)";

// The cascade host: answers the instance COUNT and hands each cascade its own program. Using four DIFFERENT
// programs (rather than one shader + a per-cascade uniform) is deliberate for the gate — it makes each expanded
// instance write a distinguishable depth, which is what proves the expansion targeted four distinct SLICES.
class CascadeHost final : public crd::framecook::IFrameGraphHost
{
public:
    CascadeHost(g::IRasterTarget* out, g::IRasterProgram* const* cascade_progs, u32 count,
                g::IRasterProgram* probe, g::IStorageBuffer* sb)
        : m_out(out), m_cascades(cascade_progs), m_count(count), m_probe(probe), m_sb(sb)
    {
    }
    g::IRasterTarget*  output() override { return m_out; }
    g::IRasterProgram* program(containers::StringView id) override { return (id.size() > 0U) ? m_probe : nullptr; }
    bool               draw_list(containers::StringView /*name*/, crd::framecook::DrawListBinding& out) override
    {
        out.storage      = m_sb;
        out.program      = m_cascades[0];
        out.vertex_count = 3U;
        return true;
    }
    u32 for_each_count(crd::framecook::FrameForEach kind, u32 arg) override
    {
        // the host is the authority on scene state: light 0 has m_count cascades this frame
        return (kind == crd::framecook::FrameForEach::LightCascades && arg == 0U) ? m_count : 0U;
    }
    g::IRasterProgram* instance_program(containers::StringView /*pass*/, u32 index) override
    {
        return index < m_count ? m_cascades[index] : nullptr;
    }

private:
    g::IRasterTarget*         m_out      = nullptr;
    g::IRasterProgram* const* m_cascades = nullptr;
    u32                       m_count    = 0U;
    g::IRasterProgram*        m_probe    = nullptr;
    g::IStorageBuffer*        m_sb       = nullptr;
};
} // namespace

TEST_CASE("REN-36.2 GATE: a COOKED frame asset renders BIT-IDENTICALLY to the hand-written C++ frame (Vulkan)",
          "[gpu-context][vulkan][frame-graph][ren36][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;

    memory::TlsfAllocator alloc(8U << 20U);
    constexpr u32         dim = 32U;

    kir::KGraph dvg(&alloc);
    kir::KEntry dve;
    gputest::build_fullscreen_vs(dvg, dve);
    kir::KGraph dfg(&alloc);
    kir::KEntry dfe;
    gputest::build_depth_only_const_fs(dfg, dfe, 0.5);
    auto dvs = rig.vk->create_program(dvg, dve);
    if (dvs == nullptr) { SKIP("shader compile unavailable"); }
    auto dfs        = rig.vk->create_program(dfg, dfe);
    auto depth_prog = raster.create_raster_program(*dvs, *dfs);

    kir::KGraph svg(&alloc);
    kir::KEntry sve;
    gputest::build_textured_vs(svg, sve);
    kir::KGraph sfg(&alloc);
    kir::KEntry sfe;
    gputest::build_shadow_fs(sfg, sfe);
    auto svs         = rig.vk->create_program(svg, sve);
    auto sfs         = rig.vk->create_program(sfg, sfe);
    auto shade_prog  = raster.create_raster_program(*svs, *sfs);
    REQUIRE(depth_prog != nullptr);
    REQUIRE(shade_prog != nullptr);

    auto sb = raster.create_storage_buffer(16U);
    REQUIRE(sb != nullptr);

    // (a) the HAND-WRITTEN C++ frame — the REN-3.1 gate's frame, verbatim
    auto ref = raster.create_color_target(dim, dim);
    REQUIRE(ref != nullptr);
    {
        auto fgraph = raster.create_frame_graph();
        REQUIRE(fgraph != nullptr);
        g::FgImageDesc dd{};
        dd.width = dim; dd.height = dim; dd.format = g::FgImageFormat::D32Float; dd.sampled = true;
        const g::FgImage  map = fgraph->create_transient_image(dd);
        const g::FgImage  fin = fgraph->import_target(*ref);
        const g::FgBuffer buf = fgraph->import_storage(*sb);
        ShadowDepthPass   dp{map, buf, depth_prog.get()};
        ShadowSamplePass  sp{map, fin, shade_prog.get()};
        fgraph->add_pass("shadow_depth").reads(buf).writes(map).execute(&record_shadow_depth, &dp);
        fgraph->add_pass("shade").reads(map).writes(fin).execute(&record_shadow_sample, &sp);
        REQUIRE(fgraph->build());
        fgraph->execute();
    }

    // (b) the SAME frame from the COOKED ASSET — parsed, cooked, read back, executed
    auto out = raster.create_color_target(dim, dim);
    REQUIRE(out != nullptr);
    {
        crd::framecook::FrameGraphDesc desc(&alloc);
        REQUIRE(crd::framecook::parse_frame_toml(
                    containers::StringView(kShadowFrameToml, std::strlen(kShadowFrameToml)), desc)
                == crd::framecook::FrameCookError::Ok);
        const auto blob = crd::framecook::cook_frame_graph(desc, &alloc);
        crd::framecook::FrameGraphDesc loaded(&alloc);
        REQUIRE(crd::framecook::read_frame_graph(containers::ConstSpan<crd::u8>(blob.data(), blob.size()), loaded));

        TestHost host(out.get(), depth_prog.get(), shade_prog.get(), sb.get());
        REQUIRE(crd::framecook::execute_frame_graph(loaded, raster, host));
    }

    // ⛔ BIT-IDENTICAL, not "close". Every texel.
    u32 diffs = 0;
    for (u32 y = 0; y < dim; ++y)
    {
        for (u32 x = 0; x < dim; ++x)
        {
            if (ref->read_pixel(x, y) != out->read_pixel(x, y)) { ++diffs; }
        }
    }
    CHECK(diffs == 0U);
    // ...and the frame is the real one, not two identical blanks: lit left, shadowed right.
    CHECK((out->read_pixel(2U, dim / 2U) & 0xFFU) > 180U);
    CHECK((out->read_pixel(dim - 3U, dim / 2U) & 0xFFU) < 80U);
}

// ── REN-36.2: a FAILING authored graph must REPORT and FALL BACK — never fail silently, never look fine. ─────
// Three rungs, each gated: authored → the graph its `fallback` names → the built-in ERROR GRAPH (loud magenta).
// The point is that a caller can always tell WHICH ran; a fallback that reports success is the same class of lie
// as a missing program rendering something plausible.
namespace
{
// A host that refuses to resolve the shader, so the authored graph cannot run. Optionally offers a fallback.
class FailingHost final : public crd::framecook::IFrameGraphHost
{
public:
    FailingHost(g::IRasterTarget* out, g::IRasterProgram* depth, g::IStorageBuffer* sb,
                const crd::framecook::FrameGraphDesc* fb)
        : m_out(out), m_depth(depth), m_sb(sb), m_fb(fb)
    {
    }
    g::IRasterTarget*  output() override { return m_out; }
    g::IRasterProgram* program(containers::StringView) override { return nullptr; } // ← the induced failure
    bool draw_list(containers::StringView, crd::framecook::DrawListBinding& out) override
    {
        out.storage = m_sb; out.program = m_depth; out.vertex_count = 3U;
        return true;
    }
    const crd::framecook::FrameGraphDesc* fallback_graph(containers::StringView) override { return m_fb; }

private:
    g::IRasterTarget*                     m_out   = nullptr;
    g::IRasterProgram*                    m_depth = nullptr;
    g::IStorageBuffer*                    m_sb    = nullptr;
    const crd::framecook::FrameGraphDesc* m_fb    = nullptr;
};

// A graph that requires a capability no device advertises here — the REN-35 tier path.
constexpr const char* kNeedsCapToml = R"(
schema = 1
name = "needs_exotic"
requires = ["exotic_feature_that_does_not_exist"]
[[pass]]
name = "p"
kind = "raster.fullscreen"
writes = ["@output"]
shader = "test://shaders/shadow_sample"
)";
} // namespace

TEST_CASE("REN-36.2: a failing authored graph REPORTS and falls back to the ERROR GRAPH (magenta, not silence)",
          "[gpu-context][vulkan][frame-graph][ren36][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;

    memory::TlsfAllocator alloc(8U << 20U);
    constexpr u32         dim = 32U;

    kir::KGraph dvg(&alloc);
    kir::KEntry dve;
    gputest::build_fullscreen_vs(dvg, dve);
    kir::KGraph dfg(&alloc);
    kir::KEntry dfe;
    gputest::build_depth_only_const_fs(dfg, dfe, 0.5);
    auto dvs = rig.vk->create_program(dvg, dve);
    if (dvs == nullptr) { SKIP("shader compile unavailable"); }
    auto dfs        = rig.vk->create_program(dfg, dfe);
    auto depth_prog = raster.create_raster_program(*dvs, *dfs);
    auto sb         = raster.create_storage_buffer(16U);
    auto out        = raster.create_color_target(dim, dim);
    REQUIRE(depth_prog != nullptr);
    REQUIRE(sb != nullptr);
    REQUIRE(out != nullptr);

    crd::framecook::FrameGraphDesc desc(&alloc);
    REQUIRE(crd::framecook::parse_frame_toml(
                containers::StringView(kShadowFrameToml, std::strlen(kShadowFrameToml)), desc)
            == crd::framecook::FrameCookError::Ok);

    SECTION("no fallback available -> the ERROR GRAPH runs and says so")
    {
        FailingHost host(out.get(), depth_prog.get(), sb.get(), nullptr);
        const auto  r = crd::framecook::execute_frame_graph_with_fallback(desc, raster, host, &alloc);

        CHECK(r.status == crd::framecook::FrameExecStatus::FellBackToErrorGraph);
        CHECK(r.error == crd::framecook::FrameExecError::UnresolvedProgram); // the REASON is reported
        CHECK(std::strcmp(r.where.c_str(), "test://shaders/shadow_sample") == 0); // ...and the OFFENDING name
        // the screen says it too: loud magenta, impossible to mistake for a working frame
        const u32 px = out->read_pixel(dim / 2U, dim / 2U);
        CHECK((px & 0xFFU) > 200U);          // R
        CHECK(((px >> 8U) & 0xFFU) < 60U);   // G
        CHECK(((px >> 16U) & 0xFFU) > 200U); // B
    }

    SECTION("a fallback graph is available -> it runs, and the status SAYS it was a fallback")
    {
        // the fallback is a trivially-satisfiable graph: one depth-only pass into @output's depth is not valid,
        // so use the SAME shadow graph but resolved by a host that CAN provide the shader.
        crd::framecook::FrameGraphDesc fb(&alloc);
        REQUIRE(crd::framecook::parse_frame_toml(
                    containers::StringView(kShadowFrameToml, std::strlen(kShadowFrameToml)), fb)
                == crd::framecook::FrameCookError::Ok);
        FailingHost host(out.get(), depth_prog.get(), sb.get(), &fb);
        const auto  r = crd::framecook::execute_frame_graph_with_fallback(desc, raster, host, &alloc);

        // this host cannot resolve the shader for EITHER graph, so the chain still ends at the error graph —
        // and crucially it reports THAT, rather than reporting the fallback as a success.
        CHECK(r.status == crd::framecook::FrameExecStatus::FellBackToErrorGraph);
        CHECK(r.error == crd::framecook::FrameExecError::UnresolvedProgram);
    }

    SECTION("an unmet `requires` capability is reported as UnsupportedCapability, not as a render error")
    {
        crd::framecook::FrameGraphDesc cap(&alloc);
        REQUIRE(crd::framecook::parse_frame_toml(
                    containers::StringView(kNeedsCapToml, std::strlen(kNeedsCapToml)), cap)
                == crd::framecook::FrameCookError::Ok);
        FailingHost host(out.get(), depth_prog.get(), sb.get(), nullptr);
        const auto  r = crd::framecook::execute_frame_graph_with_fallback(cap, raster, host, &alloc);

        CHECK(r.status == crd::framecook::FrameExecStatus::FellBackToErrorGraph);
        CHECK(r.error == crd::framecook::FrameExecError::UnsupportedCapability);
        CHECK(std::strcmp(r.where.c_str(), "exotic_feature_that_does_not_exist") == 0);
    }
}

TEST_CASE("REN-36.2: every exec status and error has readable text", "[framecook][ren36]")
{
    // A report nobody can read is barely better than silence.
    for (crd::u32 i = 0; i <= static_cast<crd::u32>(crd::framecook::FrameExecError::UnsupportedCapability); ++i)
    {
        CHECK(std::strcmp(crd::framecook::frame_exec_error_text(
                              static_cast<crd::framecook::FrameExecError>(i)), "unknown error") != 0);
    }
    for (crd::u32 i = 0; i <= static_cast<crd::u32>(crd::framecook::FrameExecStatus::Failed); ++i)
    {
        CHECK(std::strcmp(crd::framecook::frame_exec_status_text(
                              static_cast<crd::framecook::FrameExecStatus>(i)), "unknown status") != 0);
    }
}

// ── REN-36.2: a graph BUILT IN MEMORY is a first-class graph. ────────────────────────────────────────────────
// A frame graph does not have to come from a file. The node editor building one as the user drags wires, a test,
// the C++ scripting layer, an agent composing a renderer — all produce a `FrameGraphDesc` and hand it to the same
// executor. This gate proves the three provenances are INTERCHANGEABLE, pixel for pixel:
//     (a) authored TOML   (b) TOML -> cooked -> read back   (c) built programmatically with FrameGraphBuilder
// If those ever diverge, one of the paths is lying about what the graph means.
TEST_CASE("REN-36.2: a PROGRAMMATIC graph renders identically to the authored and cooked ones (3 provenances)",
          "[gpu-context][vulkan][frame-graph][ren36][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;

    memory::TlsfAllocator alloc(8U << 20U);
    constexpr u32         dim = 32U;

    kir::KGraph dvg(&alloc);
    kir::KEntry dve;
    gputest::build_fullscreen_vs(dvg, dve);
    kir::KGraph dfg(&alloc);
    kir::KEntry dfe;
    gputest::build_depth_only_const_fs(dfg, dfe, 0.5);
    auto dvs = rig.vk->create_program(dvg, dve);
    if (dvs == nullptr) { SKIP("shader compile unavailable"); }
    auto dfs        = rig.vk->create_program(dfg, dfe);
    auto depth_prog = raster.create_raster_program(*dvs, *dfs);

    kir::KGraph svg(&alloc);
    kir::KEntry sve;
    gputest::build_textured_vs(svg, sve);
    kir::KGraph sfg(&alloc);
    kir::KEntry sfe;
    gputest::build_shadow_fs(sfg, sfe);
    auto svs        = rig.vk->create_program(svg, sve);
    auto sfs        = rig.vk->create_program(sfg, sfe);
    auto shade_prog = raster.create_raster_program(*svs, *sfs);
    auto sb         = raster.create_storage_buffer(16U);
    REQUIRE(depth_prog != nullptr);
    REQUIRE(shade_prog != nullptr);
    REQUIRE(sb != nullptr);

    // (a) authored TOML
    auto from_toml = raster.create_color_target(dim, dim);
    REQUIRE(from_toml != nullptr);
    {
        crd::framecook::FrameGraphDesc d(&alloc);
        REQUIRE(crd::framecook::parse_frame_toml(
                    containers::StringView(kShadowFrameToml, std::strlen(kShadowFrameToml)), d)
                == crd::framecook::FrameCookError::Ok);
        TestHost host(from_toml.get(), depth_prog.get(), shade_prog.get(), sb.get());
        REQUIRE(crd::framecook::execute_frame_graph(d, raster, host));
    }

    // (c) BUILT PROGRAMMATICALLY — no file, no cook, no TOML anywhere in this block
    auto from_code = raster.create_color_target(dim, dim);
    REQUIRE(from_code != nullptr);
    {
        crd::framecook::FrameGraphBuilder b(&alloc, containers::StringView("ren31_shadow", 12U));
        b.add_image(containers::StringView("shadow_map", 10U), g::FgImageFormat::D32Float, dim, dim, /*sampled*/ true);
        const u32 dl = b.add_draw_list(containers::StringView("occluders", 9U));
        b.draw_list_all(dl, containers::StringView("MeshRenderer", 12U));

        const u32 p0 = b.add_pass(containers::StringView("shadow_depth", 12U),
                                  crd::framecook::FramePassKind::RasterDepthOnly);
        b.pass_draw_list(p0, containers::StringView("occluders", 9U));
        b.pass_writes(p0, containers::StringView("shadow_map", 10U));
        b.pass_clear_depth(p0, 1.0F);
        b.pass_depth(p0, g::DepthCompare::LessEqual);

        const u32 p1 = b.add_pass(containers::StringView("shade", 5U),
                                  crd::framecook::FramePassKind::RasterFullscreen);
        b.pass_reads(p1, containers::StringView("shadow_map", 10U));
        b.pass_writes(p1, containers::StringView("@output", 7U));
        b.pass_shader(p1, containers::StringView("test://shaders/shadow_sample", 28U));

        // ⛔ the SAME validator a typed graph goes through — a programmatic graph gets no weaker contract
        containers::String where(&alloc);
        REQUIRE(b.validate(&where) == crd::framecook::FrameCookError::Ok);

        TestHost host(from_code.get(), depth_prog.get(), shade_prog.get(), sb.get());
        REQUIRE(crd::framecook::execute_frame_graph(b.desc(), raster, host));
    }

    // the three provenances must agree on every texel
    u32 diffs = 0;
    for (u32 y = 0; y < dim; ++y)
    {
        for (u32 x = 0; x < dim; ++x)
        {
            if (from_toml->read_pixel(x, y) != from_code->read_pixel(x, y)) { ++diffs; }
        }
    }
    CHECK(diffs == 0U);
    CHECK((from_code->read_pixel(2U, dim / 2U) & 0xFFU) > 180U);
    CHECK((from_code->read_pixel(dim - 3U, dim / 2U) & 0xFFU) < 80U);
}

TEST_CASE("REN-36.2: a programmatically-built graph is rejected by the SAME validator as an authored one",
          "[framecook][ren36]")
{
    memory::TlsfAllocator alloc(2U << 20U);
    // an agent/editor assembling a CYCLE in memory must be caught exactly as a typed one is
    crd::framecook::FrameGraphBuilder b(&alloc, containers::StringView("cyclic", 6U));
    b.add_image(containers::StringView("a", 1U), g::FgImageFormat::RGBA8Unorm, 8U, 8U);
    b.add_image(containers::StringView("b", 1U), g::FgImageFormat::RGBA8Unorm, 8U, 8U);
    const u32 p0 = b.add_pass(containers::StringView("p0", 2U), crd::framecook::FramePassKind::RasterFullscreen);
    b.pass_shader(p0, containers::StringView("s", 1U));
    b.pass_reads(p0, containers::StringView("b", 1U));
    b.pass_writes(p0, containers::StringView("a", 1U));
    const u32 p1 = b.add_pass(containers::StringView("p1", 2U), crd::framecook::FramePassKind::RasterFullscreen);
    b.pass_shader(p1, containers::StringView("s", 1U));
    b.pass_reads(p1, containers::StringView("a", 1U));
    b.pass_writes(p1, containers::StringView("b", 1U));
    b.pass_writes(p1, containers::StringView("@output", 7U));
    CHECK(b.validate() == crd::framecook::FrameCookError::DependencyCycle);

    // ...and a well-formed one round-trips through the COOKED form too: build -> cook -> read -> validate
    crd::framecook::FrameGraphBuilder ok(&alloc, containers::StringView("ok", 2U));
    const u32 q = ok.add_pass(containers::StringView("only", 4U), crd::framecook::FramePassKind::RasterFullscreen);
    ok.pass_shader(q, containers::StringView("s", 1U));
    ok.pass_writes(q, containers::StringView("@output", 7U));
    REQUIRE(ok.validate() == crd::framecook::FrameCookError::Ok);
    const auto blob = crd::framecook::cook_frame_graph(ok.desc(), &alloc);
    crd::framecook::FrameGraphDesc back(&alloc);
    REQUIRE(crd::framecook::read_frame_graph(containers::ConstSpan<crd::u8>(blob.data(), blob.size()), back));
    CHECK(crd::framecook::validate_frame_graph(back) == crd::framecook::FrameCookError::Ok);
}

// REN-3.1 BENCH: the DEPTH-PRE-PASS cost — the per-frame baseline REN-3.2 multiplies by cascade count. Measures a
// depth-only pass (no colour attachment) against the equivalent colour+depth pass at the same resolution and draw
// count, so the number answers "what does adding a shadow pass cost?" rather than an abstract throughput figure.
// CPU wall-clock per frame, min-of-5 over `frames` iterations (the REN-1 bench's methodology).
TEST_CASE("REN-3.1 BENCH: depth-only pre-pass cost vs the equivalent colour pass", "[.][ren3-bench][gpu][vulkan]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;

    memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph           dvg(&alloc);
    kir::KEntry           dve;
    gputest::build_fullscreen_vs(dvg, dve);
    kir::KGraph dfg(&alloc);
    kir::KEntry dfe;
    gputest::build_depth_only_const_fs(dfg, dfe, 0.5);
    auto dvs = rig.vk->create_program(dvg, dve);
    if (dvs == nullptr) { SKIP("shader compile unavailable"); }
    auto dfs        = rig.vk->create_program(dfg, dfe);
    auto depth_prog = raster.create_raster_program(*dvs, *dfs);

    kir::KGraph cvg(&alloc);
    kir::KEntry cve;
    gputest::build_triangle_vs(cvg, cve);
    kir::KGraph cfg(&alloc);
    kir::KEntry cfe;
    gputest::build_triangle_fs(cfg, cfe);
    auto cvs        = rig.vk->create_program(cvg, cve);
    auto cfs        = rig.vk->create_program(cfg, cfe);
    auto color_prog = raster.create_raster_program(*cvs, *cfs);
    REQUIRE(depth_prog != nullptr);
    REQUIRE(color_prog != nullptr);

    auto sb = raster.create_storage_buffer(16U);
    REQUIRE(sb != nullptr);

    struct DepthBench { g::FgImage img; g::FgBuffer buf; g::IRasterProgram* prog; crd::u32 n; };
    const auto rec_depth = [](g::IFrameContext& ctx, void* user) {
        auto* s = static_cast<DepthBench*>(user);
        auto& r = ctx.raster();
        // clear ONCE then LOAD — exactly how a shadow pass draws N meshes into one map (a clear per draw would
        // wipe the previous occluder, and would also make this comparison unfair vs the colour arm below).
        r.draw_storage_depth_only(*ctx.image(s->img), *s->prog, 1.0F, g::DepthCompare::LessEqual, *ctx.buffer(s->buf), 3U);
        for (crd::u32 i = 1; i < s->n; ++i)
        {
            r.draw_storage_depth_only_load(*ctx.image(s->img), *s->prog, g::DepthCompare::LessEqual, *ctx.buffer(s->buf), 3U);
        }
    };
    const auto rec_color = [](g::IFrameContext& ctx, void* user) {
        auto* s = static_cast<DepthBench*>(user);
        auto& r = ctx.raster();
        r.draw_storage_depth(*ctx.image(s->img), *s->prog, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 1.0F,
                             g::DepthCompare::LessEqual, *ctx.buffer(s->buf), 3U);
        for (crd::u32 i = 1; i < s->n; ++i)
        {
            r.draw_storage_depth_load(*ctx.image(s->img), *s->prog, g::DepthCompare::LessEqual, *ctx.buffer(s->buf), 3U);
        }
    };

    constexpr crd::u32 frames = 30U;
    std::printf("\n[ren3-bench] depth-only pre-pass vs colour pass (Vulkan, CPU ms/frame, min-of-5)\n");
    std::printf("  res    draws |  depth-only |  colour+depth |  ratio\n");
    for (const crd::u32 res : {512U, 1024U, 2048U})
    {
        for (const crd::u32 n : {1U, 16U})
        {
            double best_d = 1e30;
            double best_c = 1e30;
            // ⛔ APPLES-TO-APPLES. The first draft of this bench created a fresh D32Float TRANSIENT inside the
            // depth loop while the colour loop imported a pre-made target — so it measured transient allocation +
            // graph rebuild, not pass cost, and reported depth-only as 3x SLOWER at 512. Both paths now use the
            // SAME imported colour+depth target and the SAME graph shape; the ONLY difference is which draw runs.
            auto tgt = raster.create_color_depth_target(res, res);
            REQUIRE(tgt != nullptr);
            for (crd::u32 rep = 0; rep < 5U; ++rep)
            {
                {   // depth-only pass: no colour attachment bound, depth writes only
                    auto fgraph = raster.create_frame_graph();
                    REQUIRE(fgraph != nullptr);
                    const auto t0 = std::chrono::steady_clock::now();
                    for (crd::u32 f = 0; f < frames; ++f)
                    {
                        const g::FgImage  im = fgraph->import_target(*tgt);
                        const g::FgBuffer b  = fgraph->import_storage(*sb);
                        DepthBench        st{im, b, depth_prog.get(), n};
                        fgraph->add_pass("depth").reads(b).writes(im).execute(rec_depth, &st);
                        REQUIRE(fgraph->build());
                        fgraph->execute();
                        fgraph->reset();
                    }
                    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / frames;
                    if (ms < best_d) { best_d = ms; }
                }
                {   // the equivalent colour+depth pass — same target, same graph shape, same draw count
                    auto fgraph = raster.create_frame_graph();
                    REQUIRE(fgraph != nullptr);
                    const auto t0 = std::chrono::steady_clock::now();
                    for (crd::u32 f = 0; f < frames; ++f)
                    {
                        const g::FgImage  im = fgraph->import_target(*tgt);
                        const g::FgBuffer b  = fgraph->import_storage(*sb);
                        DepthBench        st{im, b, color_prog.get(), n};
                        fgraph->add_pass("colour").reads(b).writes(im).execute(rec_color, &st);
                        REQUIRE(fgraph->build());
                        fgraph->execute();
                        fgraph->reset();
                    }
                    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / frames;
                    if (ms < best_c) { best_c = ms; }
                }
            }
            std::printf("  %4u %8u | %10.4f | %13.4f | %6.2fx\n", res, n, best_d, best_c, best_c / best_d);
        }
    }
    CHECK(true);
}

TEST_CASE("REN-1 BENCH: the frame graph's one-submission batching vs the synchronous per-draw substrate",
          "[.][ren1-bench][gpu][vulkan]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;

    memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    gputest::build_triangle_vs(vg, ve);
    kir::KGraph fgg(&alloc);
    kir::KEntry fe;
    gputest::build_triangle_fs(fgg, fe);
    auto vs = rig.vk->create_program(vg, ve);
    if (vs == nullptr) { SKIP("shader compile unavailable"); }
    auto fs   = rig.vk->create_program(fgg, fe);
    auto prog = raster.create_raster_program(*vs, *fs);
    REQUIRE(prog != nullptr);

    auto tgt = raster.create_color_depth_target(256U, 256U);
    auto sb  = raster.create_storage_buffer(16U);
    REQUIRE(tgt != nullptr);
    REQUIRE(sb != nullptr);

    struct BenchState { g::FgImage img; g::FgBuffer buf; g::IRasterProgram* prog; crd::u32 n; };
    const auto record_n = [](g::IFrameContext& ctx, void* user) {
        auto* s = static_cast<BenchState*>(user);
        auto& r = ctx.raster();
        auto& t = *ctx.image(s->img);
        auto& b = *ctx.buffer(s->buf);
        r.draw_storage_depth(t, *s->prog, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 0.0F, g::DepthCompare::Always, b, 3U);
        for (crd::u32 i = 1; i < s->n; ++i) { r.draw_storage_depth_load(t, *s->prog, g::DepthCompare::Always, b, 3U); }
    };

    constexpr crd::u32 frames = 20U;
    double             graph_ms_at_64 = 0.0;
    double             sync_ms_at_64  = 0.0;
    for (const crd::u32 n : {1U, 4U, 16U, 64U})
    {
        // (a) synchronous: N submits + N waits per frame
        const auto s0 = std::chrono::steady_clock::now();
        for (crd::u32 f = 0; f < frames; ++f)
        {
            raster.draw_storage_depth(*tgt, *prog, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 0.0F, g::DepthCompare::Always, *sb, 3U);
            for (crd::u32 i = 1; i < n; ++i) { raster.draw_storage_depth_load(*tgt, *prog, g::DepthCompare::Always, *sb, 3U); }
        }
        const double sync_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s0).count() / frames;

        // (b) frame graph: N passes, ONE submit + ONE wait per frame
        auto fgraph = raster.create_frame_graph();
        REQUIRE(fgraph != nullptr);
        const auto g0 = std::chrono::steady_clock::now();
        for (crd::u32 f = 0; f < frames; ++f)
        {
            fgraph->reset();
            const g::FgImage  img = fgraph->import_target(*tgt);
            const g::FgBuffer buf = fgraph->import_storage(*sb);
            BenchState        st{img, buf, prog.get(), n};
            fgraph->add_pass("scene").reads(buf).read_writes(img).execute(record_n, &st);
            REQUIRE(fgraph->build());
            fgraph->execute();
            CHECK(fgraph->last_submit_count() == 1U); // N draws, ONE submit — regardless of N
        }
        const double graph_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g0).count() / frames;

        WARN("[ren1-bench] N=" << n << " draws/frame  sync=" << sync_ms << "ms/frame  graph=" << graph_ms
                               << "ms/frame  speedup=" << (graph_ms > 0.0 ? sync_ms / graph_ms : 0.0) << "x");
        if (n == 64U) { graph_ms_at_64 = graph_ms; sync_ms_at_64 = sync_ms; }
    }
    // The batching must never be a REGRESSION at scale: one submit for 64 draws is not slower than 64 submits.
    CHECK(graph_ms_at_64 <= sync_ms_at_64);
}

// ── REN-36.3 GATE — ONE PASS DECLARATION, FOUR CASCADES. ─────────────────────────────────────────────────────
// The asset declares a SINGLE `[[pass]]` with `for_each = "light.0.cascades"` writing `shadow_atlas[$index]`.
// The executor asks the host for the instance count and expands it into four ordinary depth-only passes before
// build(). The claim is EXACT EQUIVALENCE: the expanded graph must produce the same image as four hand-written
// passes — which the REN-3.2 gate already pinned at the 4-bit code 0b1100 = 204. So this test asserts the SAME
// window, and the two gates together say: authoring a cascade shadow pipeline needs ZERO engine code.
// ⛔ Also gated here: a host that answers 0 instances must FAIL BY NAME, not silently render a shadowless frame.
TEST_CASE("REN-36.3 GATE: ONE authored pass declaration expands to FOUR cascades writing four atlas slices",
          "[gpu-context][vulkan][frame-graph][ren36][ren3][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;

    memory::TlsfAllocator alloc(16U << 20U);
    constexpr u32         dim      = 32U;
    constexpr u32         cascades = 4U;
    const double          depths[cascades] = {0.2, 0.4, 0.6, 0.8};

    kir::KGraph dvg(&alloc);
    kir::KEntry dve;
    gputest::build_fullscreen_vs(dvg, dve);
    auto dvs = rig.vk->create_program(dvg, dve);
    if (dvs == nullptr) { SKIP("shader compile unavailable"); }

    containers::Array<std::unique_ptr<g::IRasterProgram>> cprogs(&alloc);
    containers::Array<std::unique_ptr<g::IGpuProgram>>    cfs(&alloc);
    for (u32 c = 0; c < cascades; ++c)
    {
        kir::KGraph dfg(&alloc);
        kir::KEntry dfe;
        gputest::build_depth_only_const_fs(dfg, dfe, depths[c]);
        auto fs = rig.vk->create_program(dfg, dfe);
        REQUIRE(fs != nullptr);
        auto prog = raster.create_raster_program(*dvs, *fs);
        REQUIRE(prog != nullptr);
        cprogs.push_back(static_cast<std::unique_ptr<g::IRasterProgram>&&>(prog));
        cfs.push_back(static_cast<std::unique_ptr<g::IGpuProgram>&&>(fs));
    }
    g::IRasterProgram* cascade_ptrs[cascades] = {cprogs[0].get(), cprogs[1].get(), cprogs[2].get(), cprogs[3].get()};

    kir::KGraph pvg(&alloc);
    kir::KEntry pve;
    gputest::build_textured_vs(pvg, pve);
    kir::KGraph pfg(&alloc);
    kir::KEntry pfe;
    gputest::build_cascade_probe_fs(pfg, pfe, 0.5);
    auto pvs        = rig.vk->create_program(pvg, pve);
    auto pfs        = rig.vk->create_program(pfg, pfe);
    auto probe_prog = raster.create_raster_program(*pvs, *pfs);
    auto sb         = raster.create_storage_buffer(16U);
    REQUIRE(probe_prog != nullptr);
    REQUIRE(sb != nullptr);

    crd::framecook::FrameGraphDesc d(&alloc);
    REQUIRE(crd::framecook::parse_frame_toml(
                containers::StringView(kCascadeFrameToml, std::strlen(kCascadeFrameToml)), d)
            == crd::framecook::FrameCookError::Ok);

    auto dst = raster.create_color_target(dim, dim);
    REQUIRE(dst != nullptr);
    {
        g::ValidationCapture capture(*rig.vk);
        CascadeHost host(dst.get(), static_cast<g::IRasterProgram* const*>(cascade_ptrs), cascades,
                         probe_prog.get(), sb.get());
        REQUIRE(crd::framecook::execute_frame_graph(d, raster, host));

        const u32 px     = dst->read_pixel(dim / 2U, dim / 2U);
        const u32 code   = px & 0xFFU;
        const u32 slice1 = (px >> 8U) & 0xFFU;
        const u32 slice3 = (px >> 16U) & 0xFFU;
        // the SAME window the hand-written REN-3.2 gate asserts — one declaration, four cascades, same image
        CHECK(code > 185U);
        CHECK(code < 225U);
        CHECK(slice1 < 80U);
        CHECK(slice3 > 180U);
        CHECK(capture.error_count() == 0U); // the EXPANDED graph is validation-silent too
    }

    // ⛔ a host that reports 0 instances must FAIL BY NAME. Rendering an unshadowed frame instead would be
    // indistinguishable from a scene that simply has no shadows — the exact silent-degradation this engine refuses.
    {
        auto empty_dst = raster.create_color_target(dim, dim);
        REQUIRE(empty_dst != nullptr);
        CascadeHost none(empty_dst.get(), static_cast<g::IRasterProgram* const*>(cascade_ptrs), 0U,
                         probe_prog.get(), sb.get());
        crd::containers::String where(&alloc);
        crd::framecook::FrameExecError err = crd::framecook::FrameExecError::Ok;
        CHECK_FALSE(crd::framecook::execute_frame_graph(d, raster, none, &err, &where));
        CHECK(err == crd::framecook::FrameExecError::UnresolvedForEach);
    }
}

// ── REN-8 GATE — PER-PASS GPU TIMING. ────────────────────────────────────────────────────────────────────────
// The frame graph now brackets every pass with device timestamps inside the frame's ONE command buffer. This is
// the prerequisite for any performance work on the renderer: the sandbox costs ~12 ms/frame that neither full
// optimization (release ≈ debug) nor removing the vsync cap (immediate ≈ fifo) moves, and until a pass can be
// timed there is no way to say WHERE that goes. Measure, attribute, then fix — in that order.
// ⛔ Timestamps are only comparable WITHIN one submission (the SAME-PASS doctrine): across submits the queue can
// idle between them, so a cross-submit delta measures wall-clock, not work. Everything here is one submission.
TEST_CASE("REN-8 GATE: the frame graph reports PER-PASS GPU time from device timestamps",
          "[gpu-context][vulkan][frame-graph][ren8][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;

    memory::TlsfAllocator alloc(8U << 20U);
    constexpr u32         dim = 64U;

    kir::KGraph dvg(&alloc);
    kir::KEntry dve;
    gputest::build_fullscreen_vs(dvg, dve);
    kir::KGraph dfg(&alloc);
    kir::KEntry dfe;
    gputest::build_depth_only_const_fs(dfg, dfe, 0.5);
    auto dvs = rig.vk->create_program(dvg, dve);
    if (dvs == nullptr) { SKIP("shader compile unavailable"); }
    auto dfs        = rig.vk->create_program(dfg, dfe);
    auto depth_prog = raster.create_raster_program(*dvs, *dfs);

    kir::KGraph svg(&alloc);
    kir::KEntry sve;
    gputest::build_textured_vs(svg, sve);
    kir::KGraph sfg(&alloc);
    kir::KEntry sfe;
    gputest::build_shadow_fs(sfg, sfe);
    auto svs         = rig.vk->create_program(svg, sve);
    auto sfs         = rig.vk->create_program(sfg, sfe);
    auto shadow_prog = raster.create_raster_program(*svs, *sfs);
    auto dst         = raster.create_color_target(dim, dim);
    auto dummy       = raster.create_storage_buffer(16U);
    REQUIRE(depth_prog != nullptr);
    REQUIRE(shadow_prog != nullptr);
    REQUIRE(dst != nullptr);

    auto fgraph = raster.create_frame_graph();
    REQUIRE(fgraph != nullptr);
    if (!fgraph->gpu_timing_available()) { SKIP("device does not support timestamp queries"); }

    g::FgImageDesc ddesc{};
    ddesc.width   = dim;
    ddesc.height  = dim;
    ddesc.format  = g::FgImageFormat::D32Float;
    ddesc.sampled = true;
    const g::FgImage map = fgraph->create_transient_image(ddesc);
    REQUIRE(map.valid());
    const g::FgImage  fin = fgraph->import_target(*dst);
    const g::FgBuffer buf = fgraph->import_storage(*dummy);

    ShadowDepthPass  dpass{map, buf, depth_prog.get()};
    ShadowSamplePass spass{map, fin, shadow_prog.get()};
    fgraph->add_pass("shadow_depth").reads(buf).writes(map).execute(&record_shadow_depth, &dpass);
    fgraph->add_pass("shade").reads(map).writes(fin).execute(&record_shadow_sample, &spass);
    REQUIRE(fgraph->build());

    // BEFORE execute() there is nothing to report — timing must never carry stale numbers into a new frame
    CHECK(fgraph->pass_count() == 0U);
    fgraph->execute();

    REQUIRE(fgraph->pass_count() == 2U);
    CHECK(std::strcmp(fgraph->pass_name(0), "shadow_depth") == 0);
    CHECK(std::strcmp(fgraph->pass_name(1), "shade") == 0);

    const double p0 = fgraph->pass_gpu_ms(0);
    const double p1 = fgraph->pass_gpu_ms(1);
    const double tot = fgraph->gpu_ms_total();
    // Real work on a real device takes real time — a zero here means the timestamps never resolved.
    CHECK(p0 > 0.0);
    CHECK(p1 > 0.0);
    CHECK(tot > 0.0);
    // The span from the first pass's start to the last pass's end must cover both passes: it is a WALL span over
    // the submission, so it is >= either individual pass but need not equal their sum (barriers/gaps live in it).
    CHECK(tot >= p0);
    CHECK(tot >= p1);
    // sanity: a 64x64 two-pass frame is sub-millisecond-ish on any real GPU; a wild number means a bad tick scale
    CHECK(tot < 200.0);
    // out-of-range indices are safe + zero, never UB
    CHECK(fgraph->pass_gpu_ms(99U) == 0.0);
    CHECK(fgraph->pass_name(99U) == nullptr);

    std::printf("[ren8] shadow_depth %.4f ms | shade %.4f ms | span %.4f ms\n", p0, p1, tot);
}

// ── REN-1 GATE: DEPENDENCY ORDER, not declaration order. ─────────────────────────────────────────────────────
// The graph's core promise: a pass runs where its DATA says it belongs, so one can be INSERTED anywhere — at the
// end, or between two existing passes — and still land correctly. Here the shadow-depth producer is declared
// SECOND, after the pass that samples it. A declaration-order scheduler samples an unwritten depth map and
// produces a uniform (wrong) image; a dependency-ordered one runs the producer first and reproduces the REN-3.1
// result exactly. Same passes, same resources, reversed declaration — identical pixels is the claim.
TEST_CASE("REN-1 GATE: a pass declared AFTER its consumer still executes FIRST (dependency order)",
          "[gpu-context][vulkan][frame-graph][ren1][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;

    memory::TlsfAllocator alloc(8U << 20U);
    constexpr u32         dim = 32U;

    kir::KGraph dvg(&alloc);
    kir::KEntry dve;
    gputest::build_fullscreen_vs(dvg, dve);
    kir::KGraph dfg(&alloc);
    kir::KEntry dfe;
    gputest::build_depth_only_const_fs(dfg, dfe, 0.5);
    auto dvs = rig.vk->create_program(dvg, dve);
    if (dvs == nullptr) { SKIP("shader compile unavailable"); }
    auto dfs        = rig.vk->create_program(dfg, dfe);
    auto depth_prog = raster.create_raster_program(*dvs, *dfs);

    kir::KGraph svg(&alloc);
    kir::KEntry sve;
    gputest::build_textured_vs(svg, sve);
    kir::KGraph sfg(&alloc);
    kir::KEntry sfe;
    gputest::build_shadow_fs(sfg, sfe);
    auto svs         = rig.vk->create_program(svg, sve);
    auto sfs         = rig.vk->create_program(sfg, sfe);
    auto shadow_prog = raster.create_raster_program(*svs, *sfs);
    auto dst         = raster.create_color_target(dim, dim);
    auto dummy       = raster.create_storage_buffer(16U);
    REQUIRE(depth_prog != nullptr);
    REQUIRE(shadow_prog != nullptr);
    REQUIRE(dst != nullptr);

    g::ValidationCapture capture(*rig.vk);
    auto                 fgraph = raster.create_frame_graph();
    REQUIRE(fgraph != nullptr);

    g::FgImageDesc ddesc{};
    ddesc.width   = dim;
    ddesc.height  = dim;
    ddesc.format  = g::FgImageFormat::D32Float;
    ddesc.sampled = true;
    const g::FgImage map = fgraph->create_transient_image(ddesc);
    REQUIRE(map.valid());
    const g::FgImage  fin = fgraph->import_target(*dst);
    const g::FgBuffer buf = fgraph->import_storage(*dummy);

    ShadowDepthPass  dpass{map, buf, depth_prog.get()};
    ShadowSamplePass spass{map, fin, shadow_prog.get()};
    // ⛔ REVERSED: the CONSUMER is declared first, its PRODUCER second.
    fgraph->add_pass("shade").reads(map).writes(fin).execute(&record_shadow_sample, &spass);
    fgraph->add_pass("shadow_depth").reads(buf).writes(map).execute(&record_shadow_depth, &dpass);
    REQUIRE(fgraph->build());
    fgraph->execute();

    CHECK(fgraph->last_submit_count() == 1U);
    // the producer must have been SCHEDULED first despite being declared second
    REQUIRE(fgraph->pass_count() == 2U);
    CHECK(std::strcmp(fgraph->pass_name(0), "shadow_depth") == 0);
    CHECK(std::strcmp(fgraph->pass_name(1), "shade") == 0);

    // ...and the PIXELS match the correctly-ordered REN-3.1 gate exactly: lit left, shadowed right.
    const u32 lit      = dst->read_pixel(2U, dim / 2U);
    const u32 shadowed = dst->read_pixel(dim - 3U, dim / 2U);
    CHECK(lit != shadowed);
    CHECK((lit & 0xFFU) > 180U);
    CHECK((shadowed & 0xFFU) < 80U);
    CHECK(capture.error_count() == 0U);
}

// A true CYCLE must be REJECTED by build(), never scheduled partially: pass A reads what B writes AND writes
// what B reads. `build()` returning false is the contract the interface header states.
TEST_CASE("REN-1 GATE: build() REJECTS a dependency cycle", "[gpu-context][vulkan][frame-graph][ren1][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;

    auto a_t = raster.create_color_target(16U, 16U);
    auto b_t = raster.create_color_target(16U, 16U);
    REQUIRE(a_t != nullptr);
    REQUIRE(b_t != nullptr);

    auto fgraph = raster.create_frame_graph();
    REQUIRE(fgraph != nullptr);
    const g::FgImage x = fgraph->import_target(*a_t);
    const g::FgImage y = fgraph->import_target(*b_t);

    fgraph->add_pass("A").reads(y).writes(x).execute(&record_noop, nullptr);
    fgraph->add_pass("B").reads(x).writes(y).execute(&record_noop, nullptr);
    CHECK_FALSE(fgraph->build()); // A before B and B before A - unschedulable, and said so
}
