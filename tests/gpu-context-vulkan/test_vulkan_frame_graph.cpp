// test_vulkan_frame_graph.cpp — REN-1 (D-007 row 98): the FRAME GRAPH gate.
//  · ONE SUBMISSION: a scene pass + an overlay pass compose into a single vkQueueSubmit (last_submit_count==1),
//    with the graph inserting the cross-pass barrier — and the readback is BIT-IDENTICAL to the synchronous
//    submit+wait-per-draw path (the recording is the same; only the per-draw submit/readback is removed).
//  · TRANSIENT ALIASING: graph-owned transients whose lifetimes are DISJOINT share backing memory (physical <
//    logical, 2 same-size transients collapse to 1 slot); OVERLAPPING-lifetime transients do NOT alias.
//  · validation-SILENT by counter (the RET ValidationCapture).

#include <crd/framecook/frame_asset.hpp>   // REN-36.2: the cooked frame-graph asset
#include <crd/framecook/frame_runtime.hpp> // REN-36.2: executing it through IFrameGraph
#include <crd/gpu/frame_graph.hpp>
#include <crd/gpu/command_model.hpp> // RAF-12.4: DispatchDesc for enc_dispatch
#include <verb_packet_helpers.hpp>   // RAF-12.4: gputest::enc_draw{,_textured,_shadow,_bindless}/enc_dispatch (shared)
#include <crd/gpu/raster_context.hpp>
#include <crd/gpu/vulkan_context.hpp>
#include <crd/gpu/vulkan_raster_context.hpp>
#include <crd/gpu/vulkan_ray_tracing_context.hpp>
#include <crd/gpu/vulkan_validation_capture.hpp>
#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_glsl.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/scenerender/scene_renderer.hpp> // REN-40-A: the CPU cull the GPU kernel must match
#include <crd/vertexcook/vertex_asset.hpp> // REN-38-F13: the authored RT stages // REN-38-A9: the host builds the scene the asset names

#include <catch2/catch_test_macros.hpp>
#include <chrono>                   // REN-1 batching microbenchmark: CPU wall-clock of the submit-batching win
#include <ckir_oit_test.hpp>        // REN-38-A12: the shared WBOIT accumulate + composite shaders
#include <ckir_raster_triangle.hpp> // the shared CKIR triangle VS/FS
#include <ckir_vertex_pull.hpp>     // REN-39-A1: the GEO-1 vertex-pull VS (records at VertexIndex * 12 words)
#include <cstdio>                   // REN-3.1 bench board printf
#include <cstring>                  // REN-36.2: strlen over the embedded asset text
#include <win32_test_window.hpp>    // REN-38-A5: a REAL window when the loader has no headless surface

using namespace crd;
namespace g = crd::gpu;

namespace
{
// RAF-12.4: enc_dispatch + the fullscreen verb-packet helpers now live in the SHARED tests/gpu-shared/verb_packet_helpers.hpp
// (crd::gputest::), driven by both backend suites so the encoder's lowering is proven once.

// RAF-12.3: the pass MECHANIC survives a round trip iff the cooked executor id AND all role bits match. In a helper
// because `CHECK` cannot decompose a chained `&&`/`==` (Catch2 forbids chained comparisons inside an assertion).
[[nodiscard]] bool same_pass_mechanic(const framecook::FramePassDesc& a, const framecook::FramePassDesc& b) noexcept
{
    namespace fc = crd::framecook;
    using SVm    = crd::containers::StringView;
    return a.executor_id == b.executor_id
           && fc::pass_flag(a, SVm(fc::pp::kDepthOnly)) == fc::pass_flag(b, SVm(fc::pp::kDepthOnly))
           && fc::pass_flag(a, SVm(fc::pp::kMrt)) == fc::pass_flag(b, SVm(fc::pp::kMrt))
           && fc::pass_flag(a, SVm(fc::pp::kComposite)) == fc::pass_flag(b, SVm(fc::pp::kComposite))
           && fc::pass_flag(a, SVm(fc::pp::kIndirect)) == fc::pass_flag(b, SVm(fc::pp::kIndirect));
}

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
    crd::gputest::enc_draw_storage_depth(ctx.raster(), *ctx.image(s->img), *s->prog, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 0.0F,
                                    g::DepthCompare::Always, *ctx.buffer(s->buf), 3U);
}
void record_overlay(g::IFrameContext& ctx, void* user)
{
    auto* s = static_cast<PassState*>(user);
    crd::gputest::enc_draw_overlay(ctx.raster(), *ctx.image(s->img), *s->prog, *ctx.buffer(s->buf),
                                  g::DepthCompare::Always, 3U);
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
    crd::gputest::enc_draw_storage(ctx.raster(), *ctx.image(s->rtt), *s->prog, g::ClearColor{0.0F, 1.0F, 0.0F, 1.0F}, *ctx.buffer(s->buf), 3U);
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
    gputest::enc_draw_textured(ctx.raster(), *ctx.image(s->dst), *s->prog, g::ClearColor{0.0F, 0.0F, 1.0F, 1.0F}, *ctx.texture(s->rtt), 3U);
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
    crd::gputest::enc_draw_storage_depth_only(ctx.raster(), *ctx.image(s->map), *s->prog, 1.0F, g::DepthCompare::LessEqual,
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
    crd::gputest::enc_draw_storage_depth_only(ctx.raster(), *ctx.image_layer(s->atlas, s->layer), *s->prog, 1.0F,
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
    gputest::enc_draw_shadow(ctx.raster(), *ctx.image(s->dst), *s->prog, g::ClearColor{0.0F, 0.0F, 1.0F, 1.0F},
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
    crd::gputest::enc_draw_storage_depth(raster, *ref, *prog, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 0.0F, g::DepthCompare::Always, *sb_a, 3U);
    crd::gputest::enc_draw_overlay(raster, *ref, *prog, *sb_a, g::DepthCompare::Always, 3U);
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
    crd::gputest::enc_draw_storage_textured_depth(raster, *tgt, *prog, g::ClearColor{0.0F, 0.0F, 1.0F, 1.0F}, 0.0F, g::DepthCompare::Always,
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
                                  containers::StringView("raster.depth_only"));
        b.pass_draw_list(p0, containers::StringView("occluders", 9U));
        b.pass_writes(p0, containers::StringView("shadow_map", 10U));
        b.pass_clear_depth(p0, 1.0F);
        b.pass_depth(p0, g::DepthCompare::LessEqual);

        const u32 p1 = b.add_pass(containers::StringView("shade", 5U),
                                  containers::StringView("raster.fullscreen"));
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
    const u32 p0 = b.add_pass(containers::StringView("p0", 2U), containers::StringView("raster.fullscreen"));
    b.pass_shader(p0, containers::StringView("s", 1U));
    b.pass_reads(p0, containers::StringView("b", 1U));
    b.pass_writes(p0, containers::StringView("a", 1U));
    const u32 p1 = b.add_pass(containers::StringView("p1", 2U), containers::StringView("raster.fullscreen"));
    b.pass_shader(p1, containers::StringView("s", 1U));
    b.pass_reads(p1, containers::StringView("a", 1U));
    b.pass_writes(p1, containers::StringView("b", 1U));
    b.pass_writes(p1, containers::StringView("@output", 7U));
    CHECK(b.validate() == crd::framecook::FrameCookError::DependencyCycle);

    // ...and a well-formed one round-trips through the COOKED form too: build -> cook -> read -> validate
    crd::framecook::FrameGraphBuilder ok(&alloc, containers::StringView("ok", 2U));
    const u32 q = ok.add_pass(containers::StringView("only", 4U), containers::StringView("raster.fullscreen"));
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
        crd::gputest::enc_draw_storage_depth_only(r, *ctx.image(s->img), *s->prog, 1.0F, g::DepthCompare::LessEqual, *ctx.buffer(s->buf), 3U);
        for (crd::u32 i = 1; i < s->n; ++i)
        {
            crd::gputest::enc_draw_storage_depth_only_load(r, *ctx.image(s->img), *s->prog, g::DepthCompare::LessEqual, *ctx.buffer(s->buf), 3U);
        }
    };
    const auto rec_color = [](g::IFrameContext& ctx, void* user) {
        auto* s = static_cast<DepthBench*>(user);
        auto& r = ctx.raster();
        crd::gputest::enc_draw_storage_depth(r, *ctx.image(s->img), *s->prog, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 1.0F,
                             g::DepthCompare::LessEqual, *ctx.buffer(s->buf), 3U);
        for (crd::u32 i = 1; i < s->n; ++i)
        {
            crd::gputest::enc_draw_storage_depth_load(r, *ctx.image(s->img), *s->prog, g::DepthCompare::LessEqual, *ctx.buffer(s->buf), 3U);
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
        crd::gputest::enc_draw_storage_depth(r, t, *s->prog, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 0.0F, g::DepthCompare::Always, b, 3U);
        for (crd::u32 i = 1; i < s->n; ++i) { crd::gputest::enc_draw_storage_depth_load(r, t, *s->prog, g::DepthCompare::Always, b, 3U); }
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
            crd::gputest::enc_draw_storage_depth(raster, *tgt, *prog, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 0.0F, g::DepthCompare::Always, *sb, 3U);
            for (crd::u32 i = 1; i < n; ++i) { crd::gputest::enc_draw_storage_depth_load(raster, *tgt, *prog, g::DepthCompare::Always, *sb, 3U); }
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

// ⛔ REN-37.1 DIAGNOSTIC: does a vec3 VARYING transport VS -> FS at all under VK_EXT_shader_object?
// The scene renderer's world-normal varying reads ~0 in the FS despite the emitted GLSL being correct on BOTH
// sides. This isolates the question to one constant vec3 written straight out as colour. It also varies the
// number of UNCONSUMED VS outputs, because the scene VS emits 4 varyings while its FS reads 2 — if the extra
// outputs are what break linkage, this shows it as a step change.
TEST_CASE("REN-37.1 DIAG: a vec3 varying transports VS->FS (and survives unmatched extra VS outputs)",
          "[gpu-context][vulkan][ren37][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;

    memory::TlsfAllocator alloc(8U << 20U);
    constexpr u32         dim = 32U;

    for (u32 extra = 0; extra <= 3U; ++extra)
    {
        kir::KGraph vg(&alloc);
        kir::KEntry ve;
        gputest::build_varying_probe_vs(vg, ve, extra);
        kir::KGraph fg(&alloc);
        kir::KEntry fe;
        gputest::build_varying_probe_fs(fg, fe);

        auto vs = rig.vk->create_program(vg, ve);
        if (vs == nullptr) { SKIP("shader compile unavailable"); }
        auto fs   = rig.vk->create_program(fg, fe);
        auto prog = raster.create_raster_program(*vs, *fs);
        REQUIRE(prog != nullptr);

        auto dst = raster.create_color_target(dim, dim);
        REQUIRE(dst != nullptr);
        gputest::enc_draw(raster, *dst, *prog, g::ClearColor{0.0F, 0.0F, 1.0F, 1.0F}, 3U);

        const u32 px = dst->read_pixel(dim / 2U, dim / 2U);
        const u32 r  = px & 0xFFU;
        const u32 gg = (px >> 8U) & 0xFFU;
        std::printf("[ren37-diag] extra_outs=%u -> r=%u g=%u\n", extra, r, gg);
        // the varying carries (0,1,0): GREEN must arrive, RED must not
        CHECK(gg > 200U);
        CHECK(r < 60U);
    }
}

// ⛔ REN-37.1 DIAG part 2: the SAME constant-varying pair, but drawn through the VERTEX-PULL path
// (`draw_storage_depth`) that the scene renderer actually uses, into a colour+DEPTH target. Part 1 proved
// varyings transport under plain `draw`; this is the only remaining difference between that success and the
// scene's world-normal reading zero.
TEST_CASE("REN-37.1 DIAG: the same varying through draw_storage_depth (the vertex-pull path)",
          "[gpu-context][vulkan][ren37][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;

    memory::TlsfAllocator alloc(8U << 20U);
    constexpr u32         dim = 32U;

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    gputest::build_varying_probe_vs(vg, ve, 3U); // 4 outputs, like the scene VS
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    gputest::build_varying_probe_fs(fg, fe);

    auto vs = rig.vk->create_program(vg, ve);
    if (vs == nullptr) { SKIP("shader compile unavailable"); }
    auto fs   = rig.vk->create_program(fg, fe);
    auto prog = raster.create_raster_program(*vs, *fs);
    REQUIRE(prog != nullptr);

    auto dst = raster.create_color_depth_target(dim, dim);
    auto sb  = raster.create_storage_buffer(256U);
    REQUIRE(dst != nullptr);
    REQUIRE(sb != nullptr);

    crd::gputest::enc_draw_storage_depth(raster, *dst, *prog, g::ClearColor{0.0F, 0.0F, 1.0F, 1.0F}, 0.0F,
                              g::DepthCompare::GreaterEqual, *sb, 3U);

    const u32 px = dst->read_pixel(dim / 2U, dim / 2U);
    const u32 r  = px & 0xFFU;
    const u32 gg = (px >> 8U) & 0xFFU;
    std::printf("[ren37-diag] draw_storage_depth -> r=%u g=%u\n", r, gg);
    CHECK(gg > 200U);
    CHECK(r < 60U);
}

// ── REN-37.5 GATE: PERSISTENT resources — contents SURVIVE reset(), and are never aliased away. ─────────────
// ⛔⛔ Everything the graph owned until now was a TRANSIENT whose entire purpose is to be aliased and destroyed
// at `reset()`. TAA history, SSR/DDGI/ReSTIR temporal reuse, auto-exposure adaptation, ping-pong blur chains and
// (REN-37.9) cached viewport thumbnails all need the OPPOSITE, and the failure mode if this is wrong is silent:
// `retire_transients_to` frees graph-owned images once their fence signals, which for a history buffer destroys
// the very thing it exists to carry, and the symptom is "the temporal filter just never converges".
//
// The claims, in order of what would fail first if the feature were fake:
//   1. the SAME key returns an image whose CONTENTS survived a reset() and a whole second frame;
//   2. `persistent_image_was_live` distinguishes frame 0 (no history — a temporal pass MUST branch on this, or
//      it reprojects garbage) from frame 1+;
//   3. it is EXCLUDED from the transient aliasing pool (`transient_memory_bytes` does not grow with it);
//   4. a RESIZE recreates it and reports `was_live == false` — a differently-shaped history is worse than none,
//      because the reprojection would read plausible-looking garbage instead of obviously-missing data;
//   5. the whole thing is validation-SILENT, which is what proves the cross-frame LAYOUT write-back is right.
//      Without that write-back the second frame transitions from UNDEFINED, and a transition from UNDEFINED is
//      explicitly permitted to DISCARD contents — claim 1 would then pass on some drivers and fail on others.
TEST_CASE("REN-37.5 GATE: a PERSISTENT image keeps its contents across reset() and is never aliased",
          "[gpu-context][vulkan][frame-graph][ren37][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;

    memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph           tvg(&alloc);
    kir::KEntry           tve;
    gputest::build_triangle_vs(tvg, tve);
    kir::KGraph tfg(&alloc);
    kir::KEntry tfe;
    gputest::build_triangle_fs(tfg, tfe);
    auto tvs = rig.vk->create_program(tvg, tve);
    if (tvs == nullptr) { SKIP("shader compile unavailable"); }
    auto tfs      = rig.vk->create_program(tfg, tfe);
    auto tri_prog = raster.create_raster_program(*tvs, *tfs);
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

    constexpr u32 dim  = 64U;
    constexpr u32 probe_key = 0xA11CE5U; // the authored resource's stable identity
    auto          dst  = raster.create_color_target(dim, dim);
    auto          dummy = raster.create_storage_buffer(16U);
    REQUIRE(dst != nullptr);
    REQUIRE(dummy != nullptr);

    g::ValidationCapture capture(*rig.vk);
    auto                 fgraph = raster.create_frame_graph();
    REQUIRE(fgraph != nullptr);

    g::FgImageDesc pdesc{};
    pdesc.width   = dim;
    pdesc.height  = dim;
    pdesc.format  = g::FgImageFormat::RGBA8Unorm;
    pdesc.sampled = true;

    // ── FRAME 0: create the history and WRITE into it (a red triangle over a green clear). ──
    {
        const g::FgImage hist = fgraph->create_persistent_image(probe_key, pdesc);
        REQUIRE(hist.valid());
        CHECK_FALSE(fgraph->persistent_image_was_live(probe_key)); // claim 2: frame 0 has NO history
        const g::FgBuffer buf = fgraph->import_storage(*dummy);
        RttOffscreen      off{hist, buf, tri_prog.get()};
        fgraph->add_pass("write_history").reads(buf).writes(hist).execute(&record_rtt_offscreen, &off);
        REQUIRE(fgraph->build());
        // claim 3: a persistent image contributes NOTHING to the transient pool — it is not aliasable at all
        CHECK(fgraph->transient_memory_bytes() == 0U);
        CHECK(fgraph->transient_logical_bytes() == 0U);
        fgraph->execute();
    }

    // ⛔ THE RESET. This is the operation that destroys (or retires) every transient. A persistent image must
    // walk straight through it.
    fgraph->reset();

    // ── FRAME 1: ask for the SAME key and READ what frame 0 wrote. ──
    {
        const g::FgImage hist = fgraph->create_persistent_image(probe_key, pdesc);
        REQUIRE(hist.valid());
        CHECK(fgraph->persistent_image_was_live(probe_key)); // claim 2: frame 1 DOES have history
        const g::FgImage fin = fgraph->import_target(*dst);
        RttCompose       com{hist, fin, sample_prog.get()};
        fgraph->add_pass("read_history").reads(hist).writes(fin).execute(&record_rtt_compose, &com);
        REQUIRE(fgraph->build());
        CHECK(fgraph->transient_memory_bytes() == 0U); // still no transients at all
        fgraph->execute();
    }

    // claim 1: the pixels frame 0 rendered are STILL THERE, sampled a whole frame and a reset later.
    const u32 center = dst->read_pixel(dim / 2U, dim / 2U);
    const u32 corner = dst->read_pixel(2U, 2U);
    CHECK((center & 0xFFU) > 180U);         // the RED triangle frame 0 drew
    CHECK(((corner >> 8U) & 0xFFU) > 180U); // the GREEN clear frame 0 laid down
    CHECK((corner & 0xFFU) < 80U);

    // claim 4: a RESIZE invalidates the history rather than silently reinterpreting it.
    fgraph->reset();
    g::FgImageDesc bigger = pdesc;
    bigger.width          = dim * 2U;
    bigger.height         = dim * 2U;
    const g::FgImage grown = fgraph->create_persistent_image(probe_key, bigger);
    REQUIRE(grown.valid());
    CHECK_FALSE(fgraph->persistent_image_was_live(probe_key));

    // claim 5: validation-silent — which is what actually proves the cross-frame layout write-back.
    if (capture.error_or_warning_count() > 0U)
    {
        const auto msgs = capture.messages();
        for (usize i = 0; i < msgs.size(); ++i)
        {
            if (msgs[i].severity == g::ValidationSeverity::Info) { continue; }
            WARN("[ren37.5 capture] " << msgs[i].message_text.c_str());
        }
    }
    CHECK(capture.error_count() == 0U);
    CHECK(capture.warning_count() == 0U);
}

// ── REN-37.5 GATE: PING-PONG — two keys, swapped each frame, so no author manages a parity bit. ─────────────
// A ping-pong resource is deliberately NOT a new device concept: it is TWO persistent images and a swap the
// EXECUTOR owns. That is the whole design — hand-managing a frame-parity bit is the classic source of
// one-frame-stale bugs, and it is exactly the kind of bookkeeping an authored asset must never contain.
TEST_CASE("REN-37.5 GATE: a PING-PONG pair alternates prev/curr without the author tracking parity",
          "[gpu-context][vulkan][frame-graph][ren37][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;

    auto fgraph = raster.create_frame_graph();
    REQUIRE(fgraph != nullptr);

    constexpr u32  base_addr = 0xBEEF00U;
    g::FgImageDesc d{};
    d.width   = 32U;
    d.height  = 32U;
    d.format  = g::FgImageFormat::RGBA8Unorm;
    d.sampled = true;

    // The executor's rule, stated once: slot = base + (frame & 1) is curr, base + ((frame + 1) & 1) is prev.
    const auto curr_key = [&](u32 frame) { return base_addr + (frame & 1U); };
    const auto prev_key = [&](u32 frame) { return base_addr + ((frame + 1U) & 1U); };

    // frame 0: both halves are fresh — nothing has history yet.
    CHECK(fgraph->create_persistent_image(curr_key(0U), d).valid());
    CHECK(fgraph->create_persistent_image(prev_key(0U), d).valid());
    CHECK_FALSE(fgraph->persistent_image_was_live(curr_key(0U)));
    CHECK_FALSE(fgraph->persistent_image_was_live(prev_key(0U)));
    fgraph->reset();

    // frame 1: prev is now the buffer frame 0 called curr — the roles swapped, and BOTH are live.
    CHECK(fgraph->create_persistent_image(curr_key(1U), d).valid());
    CHECK(fgraph->create_persistent_image(prev_key(1U), d).valid());
    CHECK(fgraph->persistent_image_was_live(curr_key(1U)));
    CHECK(fgraph->persistent_image_was_live(prev_key(1U)));
    // ⛔ and they are DISTINCT buffers — if the swap collapsed to one key, a pass would read what it is writing.
    CHECK(curr_key(1U) != prev_key(1U));
    CHECK(prev_key(1U) == curr_key(0U));
}

// ── REN-38-A3 GATE: a pass binds ALL its declared reads, in DECLARATION ORDER. ──────────────────────────────
// ⛔⛔ A pass used to keep ONE `sampled` handle, which made a DEFERRED LIGHTING pass — the canonical N-texture
// consumer, reading albedo + normal + material + depth — literally inexpressible: the asset could declare four
// reads, the cooker validated them, the graph ordered and barriered them, and the executor bound exactly one.
// The declared-but-ignored failure, again, and the reason deferred could not be authored.
//
// Two offscreen passes render a RED and a GREEN target; a third declares BOTH as reads and composites
// tex[0].r into RED and tex[1].g into GREEN. ⭐ That mapping is what makes the two failure modes distinguishable:
//   · binding only the first read  -> green is 0
//   · binding them SWAPPED         -> red is 0
// A test that merely asserted "not black" would pass under both.
TEST_CASE("REN-38-A3 GATE: a fullscreen pass binds ALL its declared reads, in order",
          "[gpu-context][vulkan][frame-graph][ren38][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;
    if (!raster.supports_bindless()) { SKIP("device does not support bindless texture arrays"); }
    memory::TlsfAllocator alloc(8U << 20U);
    // two offscreen "G-buffer" writers: a solid RED one and a solid GREEN one
    kir::KGraph fvg(&alloc);
    kir::KEntry fve;
    gputest::build_textured_vs(fvg, fve); // fullscreen triangle with UV
    auto fvs = rig.vk->create_program(fvg, fve);
    if (fvs == nullptr) { SKIP("shader compile unavailable"); }

    // ⛔ The FRAGMENT programs must OUTLIVE the raster programs built from them. Building them inside a lambda
    // that returned only the raster program let each `IGpuProgram` die at the end of the call, leaving the raster
    // program holding a dangling shader — which segfaults at draw time, not at build time.
    std::unique_ptr<g::IGpuProgram> solid_fs[2];
    const auto solid_prog = [&](int slot, double r, double g_, double b) {
        kir::KGraph fg(&alloc);
        kir::KEntry fe;
        const auto  sh = kir::make_shape({1});
        const auto  kf = [&](double v) { return fg.constant(v, sh, kir::DType::F32); };
        fe.stage       = kir::KStage::Fragment;
        fe.n_out       = 1;
        fe.out[0]      = {fg.vec4(kf(r), kf(g_), kf(b), kf(1.0)), 0};
        solid_fs[slot] = rig.vk->create_program(fg, fe);
        return solid_fs[slot] != nullptr ? raster.create_raster_program(*fvs, *solid_fs[slot])
                                         : std::unique_ptr<g::IRasterProgram>{};
    };
    auto red_prog   = solid_prog(0, 1.0, 0.0, 0.0);
    auto green_prog = solid_prog(1, 0.0, 1.0, 0.0);

    kir::KGraph cfg_graph(&alloc);
    kir::KEntry cfe;
    gputest::build_two_texture_composite_fs(cfg_graph, cfe);
    auto cfs          = rig.vk->create_program(cfg_graph, cfe);
    auto compose_prog = raster.create_raster_program(*fvs, *cfs);
    REQUIRE(red_prog != nullptr);
    REQUIRE(green_prog != nullptr);
    REQUIRE(compose_prog != nullptr);

    constexpr u32 dim = 64U;
    auto          dst = raster.create_color_target(dim, dim);
    REQUIRE(dst != nullptr);

    g::ValidationCapture capture(*rig.vk);
    auto                 fgraph = raster.create_frame_graph();
    REQUIRE(fgraph != nullptr);

    g::FgImageDesc rd{};
    rd.width   = dim;
    rd.height  = dim;
    rd.format  = g::FgImageFormat::RGBA8Unorm;
    rd.sampled = true;
    const g::FgImage gb0 = fgraph->create_transient_image(rd);
    const g::FgImage gb1 = fgraph->create_transient_image(rd);
    REQUIRE(gb0.valid());
    REQUIRE(gb1.valid());
    const g::FgImage fin = fgraph->import_target(*dst);

    // The writer passes used `draw_storage` while plain `draw` had no recording path (38-A0's finding). 38-A1h
    // added one, so they now use `draw` — which makes this gate cover A1h end to end as well as A3.
    struct Solid
    {
        g::FgImage         img{};
        g::FgBuffer        buf{};
        g::IRasterProgram* prog = nullptr;
    };
    struct Compose
    {
        g::FgImage         a{};
        g::FgImage         b{};
        g::FgImage         dst{};
        g::IRasterProgram* prog = nullptr;
    };
    const auto rec_solid = [](g::IFrameContext& ctx, void* user) {
        auto* s = static_cast<Solid*>(user);
        gputest::enc_draw(ctx.raster(), *ctx.image(s->img), *s->prog, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);
    };
    const auto rec_compose = [](g::IFrameContext& ctx, void* user) {
        auto*        s = static_cast<Compose*>(user);
        g::ITexture* t[2] = {ctx.texture(s->a), ctx.texture(s->b)};
        if (t[0] == nullptr || t[1] == nullptr) { return; }
        gputest::enc_draw_bindless(ctx.raster(), *ctx.image(s->dst), *s->prog, g::ClearColor{0.0F, 0.0F, 1.0F, 1.0F},
                                   static_cast<g::ITexture* const*>(t), 2U, 3U);
    };

    auto dummy = raster.create_storage_buffer(16U);
    REQUIRE(dummy != nullptr);
    const g::FgBuffer dbuf = fgraph->import_storage(*dummy);
    Solid   s0{gb0, dbuf, red_prog.get()};
    Solid   s1{gb1, dbuf, green_prog.get()};
    Compose cm{gb0, gb1, fin, compose_prog.get()};
    fgraph->add_pass("gbuf0").reads(dbuf).writes(gb0).execute(+rec_solid, &s0);
    fgraph->add_pass("gbuf1").reads(dbuf).writes(gb1).execute(+rec_solid, &s1);
    fgraph->add_pass("lighting").reads(gb0).reads(gb1).writes(fin).execute(+rec_compose, &cm);
    REQUIRE(fgraph->build());
    fgraph->execute();

    const u32 px = dst->read_pixel(dim / 2U, dim / 2U);
    const u32 r  = px & 0xFFU;
    const u32 gg = (px >> 8U) & 0xFFU;
    UNSCOPED_INFO("composite r=" << r << " g=" << gg);
    CHECK(r > 180U);  // tex[0] (the RED writer) was bound at slot 0
    CHECK(gg > 180U); // tex[1] (the GREEN writer) was bound at slot 1 — the read that used to be DROPPED
    CHECK(fgraph->last_submit_count() == 1U);

    if (capture.error_or_warning_count() > 0U)
    {
        const auto msgs = capture.messages();
        for (usize i = 0; i < msgs.size(); ++i)
        {
            if (msgs[i].severity == g::ValidationSeverity::Info) { continue; }
            WARN("[ren38-a3 capture] " << msgs[i].message_text.c_str());
        }
    }
    CHECK(capture.error_count() == 0U);
}

// ── REN-38-A2 GATE: an AUTHORED COMPUTE PASS actually dispatches, and a later pass reads what it wrote. ─────
// ⛔⛔ `FramePassKind::Compute` was DECLARED BUT NOT IMPLEMENTED — the executor fell through to `break`, so an
// authored compute pass VALIDATED, COOKED, RAN and did NOTHING. Every check was green and the frame was silently
// wrong, which is the worst shape a defect takes in this system.
//
// This gate is built so that shape cannot come back:
//   · the kernel WRITES a value the graph then READS BACK — if the dispatch does not run, the buffer keeps its
//     uploaded sentinel and the readback shows it. "It compiled" and "it dispatched" are different assertions.
//   · the value is COMPUTED (thread index x 7 + 1), not a constant, so a kernel that ran with the wrong grid or
//     the wrong binding produces a different number rather than the same one.
//   · a SECOND element proves the grid: element 3 must be 22, which only holds if at least 4 threads ran.
TEST_CASE("REN-38-A2 GATE: an authored COMPUTE pass dispatches inside the frame graph",
          "[gpu-context][vulkan][frame-graph][ren38][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;

    memory::TlsfAllocator alloc(8U << 20U);

    // The kernel: out[i] = i * 7 + 1, one thread per element, workgroup size 64.
    kir::KGraph kg(&alloc);
    kir::KEntry ke;
    {
        const auto sh   = kir::make_shape({1});
        const int  buf  = kg.buffer_decl(kir::DType::U32, 0, 0, /*writable=*/true);
        // `LocalInvocationIndex` is a flat U32 already — no swizzle, no cast, and one workgroup covers all 64
        // elements, so the grid stays (1,1,1) and the gate does not also depend on multi-workgroup dispatch.
        const int  gid  = kg.builtin(kir::KBuiltin::LocalInvocationIndex);
        const int  k7   = kg.constant(7.0, sh, kir::DType::U32);
        const int  k1   = kg.constant(1.0, sh, kir::DType::U32);
        const int  val  = kg.binary(kir::KOp::Add, kg.binary(kir::KOp::Mul, gid, k7), k1);
        const int  mark = kg.kernel_stmt_mark();
        kg.stmt_buffer_store(buf, gid, val);
        ke.stage             = kir::KStage::Compute;
        ke.local_size[0]     = 64U;
        ke.kernel_body_begin = mark;
        ke.kernel_body_count = kg.stmt_count() - mark;
    }
    auto kernel = rig.vk->create_program(kg, ke);
    if (kernel == nullptr) { SKIP("compute shader compile unavailable"); }

    constexpr u32 elem_count = 64U;
    auto          out    = raster.create_storage_buffer(elem_count * 4U);
    REQUIRE(out != nullptr);
    // ⛔ Seed with a SENTINEL. If the dispatch silently does not run, the readback shows these values — which is
    // exactly what the old `break` did, and exactly what a "did it compile" test would have missed.
    u32 seed[elem_count];
    for (u32 i = 0; i < elem_count; ++i) { seed[i] = 0xDEADU; }
    REQUIRE(raster.upload_storage(*out, 0U, static_cast<const void*>(seed), sizeof(seed)));

    g::ValidationCapture capture(*rig.vk);
    auto                 fgraph = raster.create_frame_graph();
    REQUIRE(fgraph != nullptr);

    const g::FgBuffer buf = fgraph->import_storage(*out);

    struct Kern
    {
        g::IGpuProgram* prog = nullptr;
        g::FgBuffer     buf{};
    };
    const auto rec_kernel = [](g::IFrameContext& ctx, void* user) {
        auto*              s  = static_cast<Kern*>(user);
        g::IStorageBuffer* sb = ctx.buffer(s->buf);
        if (sb == nullptr) { return; }
        g::IStorageBuffer* bufs[1] = {sb};
        gputest::enc_dispatch(ctx.raster(), *s->prog, 1U, 1U, 1U, static_cast<g::IStorageBuffer* const*>(bufs), 1U);
    };

    Kern k{kernel.get(), buf};
    fgraph->add_pass("cull", g::FgPassKind::Compute).writes(buf).execute(+rec_kernel, &k);
    REQUIRE(fgraph->build());
    fgraph->execute();
    CHECK(fgraph->last_submit_count() == 1U); // the compute pass is IN the frame, not a second submission

    REQUIRE(raster.download_storage(*out)); // pull the DEVICE contents into the host-visible readback
    UNSCOPED_INFO("kernel wrote [0]=" << out->read_u32(0U) << " [3]=" << out->read_u32(3U));
    CHECK(out->read_u32(0U) == 1U);  // 0 * 7 + 1 — the dispatch RAN (the sentinel was 0xDEAD)
    CHECK(out->read_u32(1U) == 8U);  // 1 * 7 + 1
    CHECK(out->read_u32(3U) == 22U); // 3 * 7 + 1 — and enough threads ran, so the grid was right

    if (capture.error_or_warning_count() > 0U)
    {
        const auto msgs = capture.messages();
        for (usize i = 0; i < msgs.size(); ++i)
        {
            if (msgs[i].severity == g::ValidationSeverity::Info) { continue; }
            WARN("[ren38-a2 capture] " << msgs[i].message_text.c_str());
        }
    }
    CHECK(capture.error_count() == 0U);
}

// ── REN-38-A4 GATE: a DEFERRED RENDERER, AUTHORED AS AN ASSET ONLY. ─────────────────────────────────────────
// ⛔⛔ This is the headline gate of 38-A, and three separate rows had to land before it could exist at all:
//   · 38-A1b — MRT recording: a pass binds N COLOUR ATTACHMENTS (it used to borrow the single-target path, so a
//     G-buffer was silently one target);
//   · 38-A3  — N sampled reads: a pass binds ALL its declared reads (it used to bind only the FIRST, so a
//     lighting pass reading albedo+normal got albedo twice);
//   · 38-A1a — bindless recording, which is how N textures reach one draw inside a frame.
// Any one of them missing and a deferred renderer is not expressible, no matter what the asset says.
//
// The asset below is the whole renderer. The G-buffer pass writes RED to attachment 0 and GREEN to attachment 1;
// the lighting pass declares BOTH as reads and composites tex[0].r into RED and tex[1].g into GREEN. ⭐ That
// mapping is chosen so every failure mode is DISTINGUISHABLE:
//   · MRT bound one target twice        -> both channels carry the same value
//   · the second READ was dropped       -> green is 0
//   · reads bound in the wrong order    -> red is 0
// "Not black" would have passed under all three.
namespace
{
constexpr const char* kDeferredGraph = R"(
schema = 1
name   = "crd://frame/deferred"

[[resource]]
name    = "gbuf_albedo"
format  = "RGBA8Unorm"
scale   = 1.0
sampled = true

[[resource]]
name    = "gbuf_normal"
format  = "RGBA8Unorm"
scale   = 1.0
sampled = true

[[draw_list]]
name = "visible_opaque"
all  = ["MeshRenderer", "Transform"]

[[pass]]
name          = "gbuffer"
kind          = "raster.mrt"
draw_list     = "visible_opaque"
writes        = ["gbuf_albedo", "gbuf_normal"]
material_pass = "GBuffer"
clear_color   = [0.0, 0.0, 0.0, 1.0]

[[pass]]
name   = "lighting"
kind   = "raster.fullscreen"
shader = "crd://shaders/deferred_lighting"
reads  = ["gbuf_albedo", "gbuf_normal"]
writes = ["@output"]
)";

// The host: the asset names things, this resolves them to live objects.
class DeferredHost final : public framecook::IFrameGraphHost
{
public:
    DeferredHost(g::IRasterTarget& out, g::IStorageBuffer& buf, g::IRasterProgram& gbuf, g::IRasterProgram& light)
        : m_out(out), m_buf(buf), m_gbuf(gbuf), m_light(light)
    {
    }
    [[nodiscard]] g::IRasterTarget* output() override { return &m_out; }
    [[nodiscard]] g::IRasterProgram* program(containers::StringView) override { return &m_light; }
    [[nodiscard]] bool draw_list(containers::StringView, framecook::DrawListBinding& out) override
    {
        out.items[0]  = framecook::DrawItem{&m_buf, &m_gbuf, 3U, nullptr};
        out.resolved  = 1U;
        return true;
    }

private:
    g::IRasterTarget&  m_out;
    g::IStorageBuffer& m_buf;
    g::IRasterProgram& m_gbuf;
    g::IRasterProgram& m_light;
};
} // namespace

TEST_CASE("REN-38-A4 GATE: a DEFERRED renderer, authored as an asset only",
          "[gpu-context][vulkan][frame-graph][ren38][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;
    if (!raster.supports_bindless()) { SKIP("device does not support bindless texture arrays"); }

    memory::TlsfAllocator alloc(16U << 20U);

    // the G-buffer program: a fullscreen triangle writing TWO attachments
    kir::KGraph gvg(&alloc);
    kir::KEntry gve;
    gputest::build_textured_vs(gvg, gve);
    auto gvs = rig.vk->create_program(gvg, gve);
    if (gvs == nullptr) { SKIP("shader compile unavailable"); }
    kir::KGraph gfg(&alloc);
    kir::KEntry gfe;
    gputest::build_gbuffer_two_output_fs(gfg, gfe);
    auto gfs       = rig.vk->create_program(gfg, gfe);
    auto gbuf_prog = raster.create_raster_program(*gvs, *gfs);

    // the lighting program: a fullscreen composite reading BOTH G-buffer targets
    kir::KGraph lfg(&alloc);
    kir::KEntry lfe;
    gputest::build_two_texture_composite_fs(lfg, lfe);
    auto lfs        = rig.vk->create_program(lfg, lfe);
    auto light_prog = raster.create_raster_program(*gvs, *lfs);
    REQUIRE(gbuf_prog != nullptr);
    REQUIRE(light_prog != nullptr);

    constexpr u32 dim   = 64U;
    auto          dst   = raster.create_color_target(dim, dim);
    auto          dummy = raster.create_storage_buffer(16U);
    REQUIRE(dst != nullptr);
    REQUIRE(dummy != nullptr);

    framecook::FrameGraphDesc desc(&alloc);
    containers::String        where(&alloc);
    REQUIRE(framecook::parse_frame_toml(containers::StringView(kDeferredGraph), desc, &where)
            == framecook::FrameCookError::Ok);

    g::ValidationCapture capture(*rig.vk);
    DeferredHost         host(*dst, *dummy, *gbuf_prog, *light_prog);
    framecook::FrameExecError err = framecook::FrameExecError::Ok;
    REQUIRE(framecook::execute_frame_graph(desc, raster, host, &err, &where));
    CHECK(err == framecook::FrameExecError::Ok);

    const u32 px = dst->read_pixel(dim / 2U, dim / 2U);
    const u32 r  = px & 0xFFU;
    const u32 gg = (px >> 8U) & 0xFFU;
    UNSCOPED_INFO("deferred composite r=" << r << " g=" << gg);
    CHECK(r > 180U);  // attachment 0 (albedo/RED) was written by MRT and read at slot 0
    CHECK(gg > 180U); // attachment 1 (normal/GREEN) was written by MRT and read at slot 1

    if (capture.error_or_warning_count() > 0U)
    {
        const auto msgs = capture.messages();
        for (usize i = 0; i < msgs.size(); ++i)
        {
            if (msgs[i].severity == g::ValidationSeverity::Info) { continue; }
            WARN("[ren38-a4 capture] " << msgs[i].message_text.c_str());
        }
    }
    CHECK(capture.error_count() == 0U);
}

// ── REN-38-A5 GATE: an AUTHORED PRESENT PASS actually puts the frame on a surface. ─────────────────────────────
// ⛔⛔ `.present(surface)` was ACCEPTED AND THEN IGNORED, at BOTH levels at once:
//   · the device graph's builder stored the surface, the barrier scheduler READ the field (to skip a transition),
//     and nothing ever called `IPresentSurface::present`;
//   · `FramePassKind::Present` fell through the executor's switch to `break`.
// So a graph declaring a present pass parsed, validated, cooked, built, executed and reported ONE SUBMISSION and
// NO ERROR — and no frame ever reached a window. Every check was green. That is the same silent shape the compute
// pass had in 38-A2, in a place where the symptom ("the window is black") is easy to blame on the scene.
//
// ⭐ The claim "the frame presented" is therefore made COUNTABLE and asserted from two independent sides:
//   · the GRAPH counts present passes that actually presented (`last_present_count`), and
//   · the SURFACE counts frames it put up (`frame_count`), which the graph does not write.
// A stub that returned true without presenting moves the first and not the second.
namespace
{
constexpr const char* kPresentGraph = R"(
schema = 1
name   = "crd://frame/present"

[[draw_list]]
name = "visible"
all  = ["MeshRenderer"]

[[pass]]
name        = "scene"
kind        = "raster.geometry"
draw_list   = "visible"
writes      = ["@output"]
clear_color = [0.0, 0.0, 0.0, 1.0]

[[pass]]
name  = "to_screen"
kind  = "present"
reads = ["@output"]
)";

class PresentHost final : public framecook::IFrameGraphHost
{
public:
    PresentHost(g::IRasterTarget& out, g::IStorageBuffer& buf, g::IRasterProgram& prog, g::IPresentSurface* surf)
        : m_out(out), m_buf(buf), m_prog(prog), m_surf(surf)
    {
    }
    [[nodiscard]] g::IRasterTarget* output() override { return &m_out; }
    [[nodiscard]] g::IRasterProgram* program(containers::StringView) override { return &m_prog; }
    [[nodiscard]] bool draw_list(containers::StringView, framecook::DrawListBinding& out) override
    {
        out.items[0] = framecook::DrawItem{&m_buf, &m_prog, 3U, nullptr};
        out.resolved = 1U;
        return true;
    }
    [[nodiscard]] g::IPresentSurface* present_surface() override { return m_surf; }

private:
    g::IRasterTarget&   m_out;
    g::IStorageBuffer&  m_buf;
    g::IRasterProgram&  m_prog;
    g::IPresentSurface* m_surf = nullptr;
};
} // namespace

// ── The COOK-TIME half. It needs no device at all, so it runs everywhere and can never be skipped away. ──
// Each rule below rejects a graph that would otherwise build, execute, and put a wrong (or no) image on screen.
TEST_CASE("REN-38-A5: a PRESENT pass's shape is settled at COOK time", "[frame-cook][ren38]")
{
    memory::TlsfAllocator alloc(1U << 20U);

    const auto cook = [&](const char* toml) {
        framecook::FrameGraphDesc desc(&alloc);
        containers::String        where(&alloc);
        const framecook::FrameCookError e =
            framecook::parse_frame_toml(containers::StringView(toml), desc, &where);
        if (e != framecook::FrameCookError::Ok) { return e; }
        return framecook::validate_frame_graph(desc, &where);
    };

    // the well-formed one is accepted (so the rejections below are about the RULE, not about the fixture)
    CHECK(cook(kPresentGraph) == framecook::FrameCookError::Ok);

    // ⛔ ZERO reads: the executor would have to GUESS what to present. Two reads: it picks one and the author
    // never learns which. Both are silent, so both are rejected here.
    constexpr const char* no_read_toml = R"(
schema = 1
name   = "crd://frame/bad-present-0"
[[draw_list]]
name = "visible"
all  = ["MeshRenderer"]
[[pass]]
name      = "scene"
kind      = "raster.geometry"
draw_list = "visible"
writes    = ["@output"]
[[pass]]
name = "to_screen"
kind = "present"
)";
    CHECK(cook(no_read_toml) == framecook::FrameCookError::PresentNeedsOneRead);

    // ⛔ A present that WRITES would look like a producer to the dependency sort, so a later pass could be ordered
    // after the frame was already on screen.
    constexpr const char* writes_toml = R"(
schema = 1
name   = "crd://frame/bad-present-w"
[[resource]]
name    = "scratch"
format  = "RGBA8Unorm"
scale   = 1.0
sampled = true
[[draw_list]]
name = "visible"
all  = ["MeshRenderer"]
[[pass]]
name      = "scene"
kind      = "raster.geometry"
draw_list = "visible"
writes    = ["@output"]
[[pass]]
name   = "to_screen"
kind   = "present"
reads  = ["@output"]
writes = ["scratch"]
)";
    CHECK(cook(writes_toml) == framecook::FrameCookError::PresentWritesNothing);

    // ⛔⛔ THE ONE THAT MATTERS AT RUN TIME. A transient's memory is ALIASED and retired the instant its last
    // reader finishes — by the time the surface blits, another transient may legally own those bytes. The frame
    // would present GARBAGE, intermittently, depending on the aliasing plan. Caught at cook time instead.
    constexpr const char* transient_src_toml = R"(
schema = 1
name   = "crd://frame/bad-present-t"
[[resource]]
name    = "hdr"
format  = "RGBA8Unorm"
scale   = 1.0
sampled = true
[[draw_list]]
name = "visible"
all  = ["MeshRenderer"]
[[pass]]
name      = "scene"
kind      = "raster.geometry"
draw_list = "visible"
writes    = ["hdr", "@output"]
[[pass]]
name  = "to_screen"
kind  = "present"
reads = ["hdr"]
)";
    CHECK(cook(transient_src_toml) == framecook::FrameCookError::PresentSourceInternal);
}

// ── The DEVICE half: the authored present pass reaches a real swapchain. ──
TEST_CASE("REN-38-A5 GATE: an authored PRESENT pass hands the frame to a surface",
          "[gpu-context][vulkan][frame-graph][ren38][gpu]")
{
    // ⛔ A WINDOWED context (`headless = false`), not `make_rig()`'s headless one. A headless INSTANCE omits
    // VK_KHR_win32_surface, so `create_present_surface` with a real HWND returns null — which is how the first
    // run of this gate failed. The present path can only be gated on the configuration that actually presents.
    g::GpuContextConfig cfg;
    cfg.backend           = g::GpuBackend::Vulkan;
    cfg.headless          = false;
    cfg.enable_validation = true;
    auto ctx              = g::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { SKIP("no Vulkan device"); }
    auto* vkc = static_cast<g::VulkanGpuContext*>(ctx.get());
    if (!vkc->graphics_capable() || !vkc->shader_object()) { SKIP("no graphics/shader-object support"); }
    auto rasterp = g::create_vulkan_raster_context(*vkc);
    REQUIRE(rasterp != nullptr);
    auto& raster = *rasterp;
    if (!vkc->present_capable()) { SKIP("no present capability"); }

    // ⛔ A HEADLESS surface when the loader offers one (VK_EXT_headless_surface), else a REAL Win32 window —
    // the SAME two-way fallback RET-2's present gate uses. Taking only the headless path would have made this
    // gate SKIP on every machine whose loader lacks the extension, including this one: the device claim would
    // have gone unverified while the run still reported green. A gate that cannot run is not a gate.
    void* native = nullptr;
    if (!vkc->headless_surface())
    {
        native = crd::gputest::create_test_window(256U, 256U);
        if (native == nullptr) { SKIP("no VK_EXT_headless_surface and no platform window"); }
    }
    constexpr u32 dim = 256U;
    auto surface = raster.create_present_surface(native, dim, dim, g::PresentMode::Fifo);
    REQUIRE(surface != nullptr);
    REQUIRE(surface->valid());
    REQUIRE(surface->width() == dim);
    REQUIRE(surface->height() == dim);

    memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph           vg(&alloc);
    kir::KEntry           ve;
    gputest::build_triangle_vs(vg, ve);
    auto vs = vkc->create_program(vg, ve);
    if (vs == nullptr) { SKIP("shader compile unavailable"); }
    kir::KGraph fg2(&alloc);
    kir::KEntry fe;
    gputest::build_triangle_fs(fg2, fe);
    auto fs   = vkc->create_program(fg2, fe);
    auto prog = raster.create_raster_program(*vs, *fs);
    REQUIRE(prog != nullptr);

    // ⛔ The canvas MUST match the surface: a present with a mismatched source is REFUSED (never a stretched
    // half-frame), and a refused present is exactly the state this gate must not mistake for a success.
    auto dst = raster.create_color_depth_target(dim, dim);
    auto buf = raster.create_storage_buffer(64U);
    REQUIRE(dst != nullptr);
    REQUIRE(buf != nullptr);

    framecook::FrameGraphDesc desc(&alloc);
    containers::String        where(&alloc);
    REQUIRE(framecook::parse_frame_toml(containers::StringView(kPresentGraph), desc, &where)
            == framecook::FrameCookError::Ok);

    g::ValidationCapture capture(*vkc);

    // ── 1) NO SURFACE ⇒ a NAMED failure, never a quiet skip. ──
    // ⛔ This is the assertion that would have caught the original defect from the asset side: before the row, a
    // graph with a present pass and no surface anywhere ran to completion and reported success.
    {
        PresentHost               blind(*dst, *buf, *prog, nullptr);
        framecook::FrameExecError err = framecook::FrameExecError::Ok;
        containers::String        w2(&alloc);
        CHECK(!framecook::execute_frame_graph(desc, raster, blind, &err, &w2));
        CHECK(err == framecook::FrameExecError::NoPresentSurface);
        CHECK(w2 == "to_screen"); // by PASS NAME — the author is told which pass, not merely that something failed
    }

    // ── 2) WITH a surface: the frame actually goes up, and the graph still submits ONCE. ──
    PresentHost host(*dst, *buf, *prog, surface.get());
    auto        fgraph = raster.create_frame_graph();
    REQUIRE(fgraph != nullptr);
    framecook::FrameRecorder rec(&alloc);
    rec.begin_frame();
    framecook::FrameExecError err = framecook::FrameExecError::Ok;
    REQUIRE(rec.record(desc, *fgraph, raster, host, &err, &where));
    CHECK(err == framecook::FrameExecError::Ok);
    REQUIRE(fgraph->build());
    fgraph->execute();

    // ⭐ TWO INDEPENDENT WITNESSES. The graph counts present passes that presented; the surface counts frames it
    // put up, and the graph does not write that counter. A `present()` that returned true without presenting
    // moves the first and not the second — which is precisely the failure this row is about.
    CHECK(fgraph->last_present_count() == 1U);
    CHECK(surface->frame_count() == 1U);
    // and the present is the SURFACE's own submission — the graph must not have split into two.
    CHECK(fgraph->last_submit_count() == 1U);

    // the presented canvas is the one the graph DREW: the triangle covers the centre, the clear owns the corner.
    UNSCOPED_INFO("centre=" << (dst->read_pixel(dim / 2U, dim / 2U) & 0xFFFFFFU)
                            << " corner=" << (dst->read_pixel(2U, 2U) & 0xFFFFFFU));
    CHECK((dst->read_pixel(dim / 2U, dim / 2U) & 0xFFFFFFU) != 0U);
    CHECK((dst->read_pixel(2U, 2U) & 0xFFFFFFU) == 0U);

    // a SECOND frame through the same graph presents again — the counters are per-execute, not cumulative-once.
    fgraph->reset();
    rec.begin_frame();
    REQUIRE(rec.record(desc, *fgraph, raster, host, &err, &where));
    REQUIRE(fgraph->build());
    fgraph->execute();
    CHECK(fgraph->last_present_count() == 1U);
    CHECK(surface->frame_count() == 2U);

    if (capture.error_or_warning_count() > 0U)
    {
        const auto msgs = capture.messages();
        for (usize i = 0; i < msgs.size(); ++i)
        {
            if (msgs[i].severity == g::ValidationSeverity::Info) { continue; }
            WARN("[ren38-a5 capture] " << msgs[i].message_text.c_str());
        }
    }
    CHECK(capture.error_count() == 0U);
    surface.reset(); // the surface dies BEFORE its window
    if (native != nullptr) { crd::gputest::destroy_test_window(native); }
}

// ── REN-38-A6 GATE: CLEAR · COPY · BLIT · RESOLVE, authored as passes. ─────────────────────────────────────────
// ⛔ Four utility nodes every real frame graph needs, and none of them could be expressed. The only way to move
// pixels was a `raster.fullscreen` pass with a pass-through shader — paying for a rasterizer, a descriptor set and
// a pipeline to do what the copy engine does for free — and an MSAA RESOLVE was not expressible AT ALL, because
// copying a multisampled image is illegal rather than merely slow.
//
// ⭐ EACH VERB IS GATED BY A SIGNAL ONLY THAT VERB CAN PRODUCE:
//   · clear — a target set to a colour NOTHING else in the graph writes;
//   · copy  — a destination that ALREADY HELD A DIFFERENT COLOUR, so "the copy did nothing" and "the source was
//             never written" are DIFFERENT observable outcomes (blue vs black) rather than one black frame;
//   · blit  — structure that survives a round trip through a HALF-RESOLUTION image, which a 1:1 crop cannot fake;
//   · resolve — an edge pixel strictly BETWEEN the two colours present, which only sample averaging produces.
namespace
{
// clear → copy. The destination is pre-painted BLUE by its own clear pass, so a copy that silently does nothing
// leaves BLUE, and a source that was never painted leaves BLACK. Three outcomes, three diagnoses.
constexpr const char* kCopyGraph = R"(
schema = 1
name   = "crd://frame/a6-copy"

[[resource]]
name    = "stage"
format  = "RGBA8Unorm"
scale   = 1.0
sampled = true

[[pass]]
name        = "paint_stage"
kind        = "clear"
writes      = ["stage"]
clear_color = [1.0, 0.0, 0.0, 1.0]

[[pass]]
name        = "prime_output"
kind        = "clear"
writes      = ["@output"]
clear_color = [0.0, 0.0, 1.0, 1.0]

[[pass]]
name   = "publish"
kind   = "copy"
reads  = ["stage"]
writes = ["@output"]
)";

// draw → blit DOWN to half res → blit UP to the output. A 1:1 crop would put `src`'s top-left quarter — which is
// pure clear — at the centre of the result, so "the centre is still triangle-coloured" is the rescale assertion.
constexpr const char* kBlitGraph = R"(
schema = 1
name   = "crd://frame/a6-blit"

[[resource]]
name    = "hires"
format  = "RGBA8Unorm"
scale   = 1.0
sampled = true

[[resource]]
name    = "half"
format  = "RGBA8Unorm"
scale   = 0.5
sampled = true

[[draw_list]]
name = "visible"
all  = ["MeshRenderer"]

[[pass]]
name        = "scene"
kind        = "raster.geometry"
draw_list   = "visible"
writes      = ["hires"]
clear_color = [0.0, 0.0, 0.0, 1.0]

[[pass]]
name   = "downsample"
kind   = "blit"
reads  = ["hires"]
writes = ["half"]
filter = "linear"

[[pass]]
name   = "upsample"
kind   = "blit"
reads  = ["half"]
writes = ["@output"]
filter = "linear"
)";

class UtilHost final : public framecook::IFrameGraphHost
{
public:
    UtilHost(g::IRasterTarget& out, g::IStorageBuffer* buf, g::IRasterProgram* prog)
        : m_out(out), m_buf(buf), m_prog(prog)
    {
    }
    [[nodiscard]] g::IRasterTarget* output() override { return &m_out; }
    [[nodiscard]] g::IRasterProgram* program(containers::StringView) override { return m_prog; }
    [[nodiscard]] bool draw_list(containers::StringView, framecook::DrawListBinding& out) override
    {
        if (m_buf == nullptr || m_prog == nullptr) { return false; }
        out.items[0] = framecook::DrawItem{m_buf, m_prog, 3U, nullptr};
        out.resolved = 1U;
        return true;
    }

private:
    g::IRasterTarget&  m_out;
    g::IStorageBuffer* m_buf  = nullptr;
    g::IRasterProgram* m_prog = nullptr;
};
} // namespace

// ── The COOK-TIME half: what a utility pass may say. No device, so it can never be skipped away. ──
TEST_CASE("REN-38-A6: a UTILITY pass's shape is settled at COOK time", "[frame-cook][ren38]")
{
    memory::TlsfAllocator alloc(1U << 20U);
    const auto            cook = [&](const char* toml) {
        framecook::FrameGraphDesc desc(&alloc);
        containers::String        where(&alloc);
        const framecook::FrameCookError e = framecook::parse_frame_toml(containers::StringView(toml), desc, &where);
        if (e != framecook::FrameCookError::Ok) { return e; }
        return framecook::validate_frame_graph(desc, &where);
    };

    CHECK(cook(kCopyGraph) == framecook::FrameCookError::Ok);
    CHECK(cook(kBlitGraph) == framecook::FrameCookError::Ok);

    // ⛔ TWO sources for one copy: the executor would pick one and never say which. "Almost right" is the worst
    // symptom to debug, so it is rejected before it can ship.
    constexpr const char* two_src_toml = R"(
schema = 1
name   = "crd://frame/a6-bad-2src"
[[resource]]
name    = "a"
format  = "RGBA8Unorm"
scale   = 1.0
sampled = true
[[resource]]
name    = "b"
format  = "RGBA8Unorm"
scale   = 1.0
sampled = true
[[pass]]
name        = "pa"
kind        = "clear"
writes      = ["a"]
clear_color = [1.0, 0.0, 0.0, 1.0]
[[pass]]
name        = "pb"
kind        = "clear"
writes      = ["b"]
clear_color = [0.0, 1.0, 0.0, 1.0]
[[pass]]
name   = "publish"
kind   = "copy"
reads  = ["a", "b"]
writes = ["@output"]
)";
    CHECK(cook(two_src_toml) == framecook::FrameCookError::TransferNeedsOneRead);

    // A clear PRODUCES and consumes nothing. A declared read would order it after that resource's producer for
    // no reason — and, worse, would read as intent to anyone maintaining the asset.
    constexpr const char* clear_reads_toml = R"(
schema = 1
name   = "crd://frame/a6-bad-clear"
[[resource]]
name    = "a"
format  = "RGBA8Unorm"
scale   = 1.0
sampled = true
[[pass]]
name        = "pa"
kind        = "clear"
writes      = ["a"]
clear_color = [1.0, 0.0, 0.0, 1.0]
[[pass]]
name        = "bad"
kind        = "clear"
reads       = ["a"]
writes      = ["@output"]
clear_color = [0.0, 0.0, 0.0, 1.0]
)";
    CHECK(cook(clear_reads_toml) == framecook::FrameCookError::ClearReadsNothing);

    // the filter is a CLOSED set — a typo must be a named rejection, not a silent fall back to linear
    constexpr const char* bad_filter_toml = R"(
schema = 1
name   = "crd://frame/a6-bad-filter"
[[resource]]
name    = "a"
format  = "RGBA8Unorm"
scale   = 1.0
sampled = true
[[pass]]
name        = "pa"
kind        = "clear"
writes      = ["a"]
clear_color = [1.0, 0.0, 0.0, 1.0]
[[pass]]
name   = "b"
kind   = "blit"
reads  = ["a"]
writes = ["@output"]
filter = "bilinear"
)";
    CHECK(cook(bad_filter_toml) == framecook::FrameCookError::UnknownFilter);

    // ⭐ and the new kinds SURVIVE A ROUND TRIP — emit → re-parse must reproduce the same graph, filter included,
    // or a cooked pack silently degrades every blit to the default filter.
    {
        framecook::FrameGraphDesc a(&alloc);
        containers::String        where(&alloc);
        REQUIRE(framecook::parse_frame_toml(containers::StringView(kBlitGraph), a, &where)
                == framecook::FrameCookError::Ok);
        containers::String text = framecook::emit_frame_toml(a, &alloc);
        framecook::FrameGraphDesc b(&alloc);
        REQUIRE(framecook::parse_frame_toml(containers::StringView(text.c_str(), text.size()), b, &where)
                == framecook::FrameCookError::Ok);
        REQUIRE(b.passes.size() == a.passes.size());
        for (usize i = 0; i < a.passes.size(); ++i)
        {
            CHECK(same_pass_mechanic(a.passes[i], b.passes[i]));
            CHECK(framecook::pass_u32(b.passes[i], containers::StringView(framecook::pp::kFilter), 0U) == framecook::pass_u32(a.passes[i], containers::StringView(framecook::pp::kFilter), 0U));
        }
    }
}

// ── The DEVICE half: the four verbs actually move pixels, inside ONE submission. ──
TEST_CASE("REN-38-A6 GATE: authored CLEAR / COPY / BLIT move pixels inside one frame",
          "[gpu-context][vulkan][frame-graph][ren38][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;

    memory::TlsfAllocator alloc(8U << 20U);
    g::ValidationCapture  capture(*rig.vk);
    constexpr u32         dim = 64U;

    // ── 1) CLEAR + COPY. ──
    {
        auto dst = raster.create_color_target(dim, dim);
        REQUIRE(dst != nullptr);
        framecook::FrameGraphDesc desc(&alloc);
        containers::String        where(&alloc);
        REQUIRE(framecook::parse_frame_toml(containers::StringView(kCopyGraph), desc, &where)
                == framecook::FrameCookError::Ok);
        UtilHost                  host(*dst, nullptr, nullptr);
        framecook::FrameExecError err = framecook::FrameExecError::Ok;
        REQUIRE(framecook::execute_frame_graph(desc, raster, host, &err, &where));
        CHECK(err == framecook::FrameExecError::Ok);

        const u32 px = dst->read_pixel(dim / 2U, dim / 2U);
        const u32 r  = px & 0xFFU;
        const u32 b  = (px >> 16U) & 0xFFU;
        UNSCOPED_INFO("copy result r=" << r << " b=" << b);
        // ⭐ RED means the clear painted `stage` AND the copy published it. BLUE would mean the copy did nothing
        // (the output kept its own clear); BLACK would mean the stage clear did nothing. Three distinct verdicts.
        CHECK(r > 200U);
        CHECK(b < 60U);
    }

    // ── 2) BLIT: structure survives a round trip through a HALF-RESOLUTION image. ──
    {
        kir::KGraph vg(&alloc);
        kir::KEntry ve;
        gputest::build_triangle_vs(vg, ve);
        auto vs = rig.vk->create_program(vg, ve);
        if (vs == nullptr) { SKIP("shader compile unavailable"); }
        kir::KGraph fg2(&alloc);
        kir::KEntry fe;
        gputest::build_triangle_fs(fg2, fe);
        auto fs   = rig.vk->create_program(fg2, fe);
        auto prog = raster.create_raster_program(*vs, *fs);
        REQUIRE(prog != nullptr);

        auto dst = raster.create_color_target(dim, dim);
        auto buf = raster.create_storage_buffer(64U);
        REQUIRE(dst != nullptr);
        REQUIRE(buf != nullptr);

        framecook::FrameGraphDesc desc(&alloc);
        containers::String        where(&alloc);
        REQUIRE(framecook::parse_frame_toml(containers::StringView(kBlitGraph), desc, &where)
                == framecook::FrameCookError::Ok);
        UtilHost                  host(*dst, buf.get(), prog.get());
        framecook::FrameExecError err = framecook::FrameExecError::Ok;
        REQUIRE(framecook::execute_frame_graph(desc, raster, host, &err, &where));
        CHECK(err == framecook::FrameExecError::Ok);

        const u32 centre = dst->read_pixel(dim / 2U, dim / 2U) & 0xFFFFFFU;
        const u32 corner = dst->read_pixel(1U, 1U) & 0xFFFFFFU;
        UNSCOPED_INFO("blit centre=" << centre << " corner=" << corner);
        // ⛔ A blit that CROPPED instead of rescaling would put `hires`'s top-left quarter — pure clear — at the
        // centre, so the centre being triangle-coloured is the assertion that the rescale really happened. The
        // corner staying clear rules out the opposite failure: a blit that stretched a covered region over all.
        CHECK(centre != 0U);
        CHECK(corner == 0U);
    }

    if (capture.error_or_warning_count() > 0U)
    {
        const auto msgs = capture.messages();
        for (usize i = 0; i < msgs.size(); ++i)
        {
            if (msgs[i].severity == g::ValidationSeverity::Info) { continue; }
            WARN("[ren38-a6 capture] " << msgs[i].message_text.c_str());
        }
    }
    CHECK(capture.error_count() == 0U);
}

// ── REN-38-A7 / A8 GATE: TESSELLATION and MESH+TASK shaders, authored as passes. ───────────────────────────────
// ⛔ Both existed as DEVICE verbs (38-A1d, 38-A1c) and NEITHER was reachable from an asset — so the entire
// geometry-AMPLIFICATION half of the hardware could only be driven by C++, which is exactly what the top rule
// forbids: a technique is a `.frame.toml`, not a call site.
//
// ⛔⛔ AND A PASS KIND IS NOT JUST A SWITCH CASE. `draw_tess` and `draw_mesh` both CLEAR. That is correct for the
// single-draw proofs they were written for and CATASTROPHIC for a pass that iterates a draw list: every draw
// after the first wipes the ones before it, so a scene with three tessellated meshes renders exactly ONE — the
// last — and looks entirely plausible. This is the multi-pass load-not-clear scar in its amplification form, and
// it is why A7/A8 had to add `draw_tess_load` / `draw_mesh_load` to BOTH backends first.
//
// ⭐ The mesh gate is therefore built so that failure is VISIBLE AND SPECIFIC: draw 1 dispatches SIX meshlet
// workgroups (six triangles tiled left-to-right); draw 2 dispatches ONE (the leftmost only). If the second draw
// CLEARED, the six become one and the RIGHTMOST triangle vanishes — so the assertion is on a pixel only the FIRST
// draw could have coloured. A "did anything render at all" check would have passed either way.
namespace
{
constexpr const char* kTessGraph = R"(
schema = 1
name   = "crd://frame/a7-tess"

[[pass]]
name        = "displace"
kind        = "raster.tess"
shader      = "crd://shaders/tess_quad"
writes      = ["@output"]
clear_color = [0.0, 0.0, 0.0, 1.0]
params      = { patches = 1 }
)";

constexpr const char* kMeshGraph = R"(
schema = 1
name   = "crd://frame/a8-mesh"

[[draw_list]]
name = "meshlets"
all  = ["MeshRenderer"]

[[pass]]
name        = "amplify"
kind        = "raster.mesh"
shader      = "crd://shaders/meshlet"
draw_list   = "meshlets"
writes      = ["@output"]
clear_color = [0.0, 0.0, 0.0, 1.0]
)";

// The host resolves ONE shader id to whichever program the technique needs — a tess program is a VS+TCS+TES+FS
// and a mesh program is a TASK+MESH+FS, and WHICH stages a cooked program carries is a property of the PROGRAM,
// not of the graph. That is why the asset names a shader and says nothing about stages.
class AmplifyHost final : public framecook::IFrameGraphHost
{
public:
    AmplifyHost(g::IRasterTarget& out, g::IRasterProgram* prog) : m_out(out), m_prog(prog) {}
    [[nodiscard]] g::IRasterTarget* output() override { return &m_out; }
    [[nodiscard]] g::IRasterProgram* program(containers::StringView) override { return m_prog; }
    // TWO draws, with DIFFERENT dispatch counts. For an amplification pass a draw item's `vertex_count` IS the
    // dispatch count — patches for tess, task/mesh workgroups for mesh.
    [[nodiscard]] bool draw_list(containers::StringView, framecook::DrawListBinding& out) override
    {
        if (m_prog == nullptr) { return false; }
        out.items[0] = framecook::DrawItem{nullptr, m_prog, 6U, nullptr}; // six meshlets, tiled left → right
        out.items[1] = framecook::DrawItem{nullptr, m_prog, 1U, nullptr}; // one meshlet, leftmost only
        out.resolved = 2U;
        return true;
    }

private:
    g::IRasterTarget&  m_out;
    g::IRasterProgram* m_prog = nullptr;
};
} // namespace

// ── The COOK-TIME half. No device, so it runs everywhere and can never be skipped away. ──
TEST_CASE("REN-38-A7/A8: an AMPLIFICATION pass's shape is settled at COOK time", "[frame-cook][ren38]")
{
    memory::TlsfAllocator alloc(1U << 20U);
    const auto            cook = [&](const char* toml) {
        framecook::FrameGraphDesc desc(&alloc);
        containers::String        where(&alloc);
        const framecook::FrameCookError e = framecook::parse_frame_toml(containers::StringView(toml), desc, &where);
        if (e != framecook::FrameCookError::Ok) { return e; }
        return framecook::validate_frame_graph(desc, &where);
    };

    CHECK(cook(kTessGraph) == framecook::FrameCookError::Ok);
    CHECK(cook(kMeshGraph) == framecook::FrameCookError::Ok);

    // ⛔ NEITHER a draw list NOR a count: the runtime would dispatch ZERO patches — a black image, no error, and
    // nothing in the asset to point at. Caught while the author is still holding the file.
    constexpr const char* no_count_toml = R"(
schema = 1
name   = "crd://frame/a7-bad"
[[pass]]
name   = "displace"
kind   = "raster.tess"
shader = "crd://shaders/tess_quad"
writes = ["@output"]
)";
    CHECK(cook(no_count_toml) == framecook::FrameCookError::AmplifyNeedsCount);

    // an amplification pass with no PROGRAM is the same class of silence
    constexpr const char* no_shader_toml = R"(
schema = 1
name   = "crd://frame/a8-bad"
[[pass]]
name   = "amplify"
kind   = "raster.mesh"
writes = ["@output"]
params = { groups = 4 }
)";
    CHECK(cook(no_shader_toml) == framecook::FrameCookError::MissingShader);

    // ⭐ and both kinds SURVIVE A ROUND TRIP — a cooked pack that lost `raster.mesh` would silently reclassify
    // the pass as a plain geometry draw, which renders NOTHING (a mesh program has no vertex input).
    {
        framecook::FrameGraphDesc a(&alloc);
        containers::String        where(&alloc);
        REQUIRE(framecook::parse_frame_toml(containers::StringView(kMeshGraph), a, &where)
                == framecook::FrameCookError::Ok);
        containers::String        text = framecook::emit_frame_toml(a, &alloc);
        framecook::FrameGraphDesc b(&alloc);
        REQUIRE(framecook::parse_frame_toml(containers::StringView(text.c_str(), text.size()), b, &where)
                == framecook::FrameCookError::Ok);
        REQUIRE(b.passes.size() == 1U);
        CHECK(framecook::pass_is_mesh(b.passes[0]));
    }
}

// ── The DEVICE half: both amplification kinds run from asset text, inside ONE submission. ──
TEST_CASE("REN-38-A7/A8 GATE: authored TESSELLATION and MESH+TASK passes amplify geometry",
          "[gpu-context][vulkan][frame-graph][ren38][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;

    memory::TlsfAllocator alloc(16U << 20U);
    g::ValidationCapture  capture(*rig.vk);
    constexpr u32         dim = 64U;

    // ── 38-A7: an authored `raster.tess` pass. ──
    {
        kir::KGraph vg(&alloc);
        kir::KEntry ve;
        gputest::build_tess_quad_vs(vg, ve);
        kir::KGraph cg(&alloc);
        kir::KEntry ce;
        gputest::build_tess_hull(cg, ce);
        kir::KGraph eg(&alloc);
        kir::KEntry ee;
        gputest::build_tess_domain(eg, ee);
        kir::KGraph fg2(&alloc);
        kir::KEntry fe;
        gputest::build_triangle_fs(fg2, fe);
        auto vs  = rig.vk->create_program(vg, ve);
        auto tcs = rig.vk->create_program(cg, ce);
        auto tes = rig.vk->create_program(eg, ee);
        auto fs  = rig.vk->create_program(fg2, fe);
        if (vs == nullptr || tcs == nullptr || tes == nullptr || fs == nullptr) { SKIP("shader compile unavailable"); }
        auto prog = raster.create_tess_program(*vs, *tcs, *tes, *fs);
        if (prog == nullptr) { SKIP("no tessellation support on this device"); }

        auto dst = raster.create_color_target(dim, dim);
        REQUIRE(dst != nullptr);
        framecook::FrameGraphDesc desc(&alloc);
        containers::String        where(&alloc);
        REQUIRE(framecook::parse_frame_toml(containers::StringView(kTessGraph), desc, &where)
                == framecook::FrameCookError::Ok);
        AmplifyHost               host(*dst, prog.get());
        framecook::FrameExecError err = framecook::FrameExecError::Ok;
        REQUIRE(framecook::execute_frame_graph(desc, raster, host, &err, &where));
        CHECK(err == framecook::FrameExecError::Ok);

        // ⭐ The tessellator turned ONE 4-control-point patch into a filled quad. The CENTRE proves the patch was
        // tessellated and shaded; the far CORNER proves it did not simply cover the whole target (the quad is
        // ±0.6 in clip space, so the corners stay clear) — together they rule out "nothing ran" AND "the clear
        // colour happens to look like a pass".
        const u32 centre = dst->read_pixel(dim / 2U, dim / 2U) & 0xFFFFFFU;
        const u32 corner = dst->read_pixel(1U, 1U) & 0xFFFFFFU;
        UNSCOPED_INFO("tess centre=" << centre << " corner=" << corner);
        CHECK(centre != 0U);
        CHECK(corner == 0U);
    }

    // ── 38-A8: an authored `raster.mesh` pass over a TWO-ITEM draw list. ──
    {
        kir::KGraph mg(&alloc);
        kir::KEntry me;
        gputest::build_mesh_grid_tri(mg, me);
        kir::KGraph fg2(&alloc);
        kir::KEntry fe;
        gputest::build_amplify_fs(fg2, fe);
        auto ms = rig.vk->create_program(mg, me);
        auto fs = rig.vk->create_program(fg2, fe);
        if (ms == nullptr || fs == nullptr) { SKIP("mesh shader compile unavailable"); }
        auto prog = raster.create_mesh_program(*ms, *fs);
        if (prog == nullptr) { SKIP("no mesh-shader support on this device"); }

        auto dst = raster.create_color_target(dim, dim);
        REQUIRE(dst != nullptr);
        framecook::FrameGraphDesc desc(&alloc);
        containers::String        where(&alloc);
        REQUIRE(framecook::parse_frame_toml(containers::StringView(kMeshGraph), desc, &where)
                == framecook::FrameCookError::Ok);
        AmplifyHost               host(*dst, prog.get());
        framecook::FrameExecError err = framecook::FrameExecError::Ok;
        REQUIRE(framecook::execute_frame_graph(desc, raster, host, &err, &where));
        CHECK(err == framecook::FrameExecError::Ok);

        // The mesh shader tiles one triangle per workgroup at clip x = -0.8 + wg·0.2, so with 6 workgroups the
        // rightmost sits at x = +0.2 → pixel 38, and the leftmost at x = -0.8 → pixel 6.
        const u32 leftmost  = dst->read_pixel(6U, dim / 2U) & 0xFFU;
        const u32 rightmost = dst->read_pixel(38U, dim / 2U) & 0xFFU;
        UNSCOPED_INFO("mesh leftmost=" << leftmost << " rightmost=" << rightmost);
        // ⭐⭐ THE LOAD ASSERTION. The rightmost triangle exists ONLY because of draw 1 (six workgroups); draw 2
        // dispatches one. If the second draw had cleared — which is exactly what `draw_mesh` does and why
        // `draw_mesh_load` had to exist — this pixel would be black and the image would still look like a
        // working mesh-shader pass.
        CHECK(rightmost > 0U);
        CHECK(leftmost > 0U); // and the second draw really did run
    }

    if (capture.error_or_warning_count() > 0U)
    {
        const auto msgs = capture.messages();
        for (usize i = 0; i < msgs.size(); ++i)
        {
            if (msgs[i].severity == g::ValidationSeverity::Info) { continue; }
            WARN("[ren38-a78 capture] " << msgs[i].message_text.c_str());
        }
    }
    CHECK(capture.error_count() == 0U);
}

// ── REN-38-A9 / A10 GATE: RAY TRACING and the GPU-DRIVEN LOOP, authored as passes. ─────────────────────────────
// ⛔⛔ THE RAY-TRACING CONTEXTS ARE OFFLINE RIGS. Every verb on `VulkanRayTracingContext` /
// `Dx12RayTracingContext` creates its own buffers, its own descriptor pool, its own pipeline, then SUBMITS AND
// WAITS. That is right for an oracle comparison and impossible inside a frame — the universal port defect in its
// most extreme form: the verb owns the whole submission, not merely the descriptor pool. So ray tracing existed
// on this engine and was UNREACHABLE from a frame graph, which is where a shadow or AO pass actually lives.
//
// ⭐ What an authored `raytrace` pass calls instead is an INLINE RAY QUERY dispatch recorded into the frame's one
// submission: the TLAS at binding 0, the pass's buffers at 1..N — the SAME convention `trace_dispatch` uses, so a
// kernel written for the offline rig runs unchanged inside a frame.
//
// ⛔ AND A10 IS THE ONE THAT CLOSES THE LOOP. With `dispatch_kernel` the workgroup count is a parameter the CPU
// had to know, so a cull pass could never actually decide how much work followed it. The gate below makes the
// CPU's ignorance the assertion: the count is written BY A SHADER into a buffer, and the test checks which
// workgroups ran.
namespace
{
constexpr const char* kRtGraph = R"(
schema = 1
name   = "crd://frame/a9-rt"

[[resource]]
name = "scene_tlas"
kind = "acceleration_structure"

[[resource]]
name = "rays"
kind = "external_buffer"

[[resource]]
name = "hits"
kind = "external_buffer"

[[pass]]
name   = "trace"
kind   = "raytrace"
kernel = "crd://kernels/trace"
reads  = ["scene_tlas", "rays"]
writes = ["hits"]
params = { groups_x = 1 }

[[pass]]
name        = "blank"
kind        = "clear"
writes      = ["@output"]
clear_color = [0.0, 0.0, 0.0, 1.0]
)";

constexpr const char* kIndirectGraph = R"(
schema = 1
name   = "crd://frame/a10-indirect"

[[resource]]
name = "args"
kind = "indirect_args"
size_bytes = 16

[[resource]]
name = "marks"
kind = "external_buffer"

[[pass]]
name   = "cull"
kind   = "compute"
kernel = "crd://kernels/cull"
writes = ["args"]
params = { groups_x = 1 }

[[pass]]
name   = "work"
kind   = "compute.indirect"
kernel = "crd://kernels/work"
reads  = ["args"]
writes = ["marks"]

[[pass]]
name        = "blank"
kind        = "clear"
writes      = ["@output"]
clear_color = [0.0, 0.0, 0.0, 1.0]
)";

// The host owns the World, so the host owns the acceleration structure and the scene buffers. The asset only
// names them — exactly the `draw_list` arrangement, two resource kinds over.
class RtHost final : public framecook::IFrameGraphHost
{
public:
    RtHost(g::IRasterTarget& out) : m_out(out) {}
    [[nodiscard]] g::IRasterTarget* output() override { return &m_out; }
    [[nodiscard]] g::IRasterProgram* program(containers::StringView) override { return nullptr; }
    [[nodiscard]] bool draw_list(containers::StringView, framecook::DrawListBinding&) override { return false; }
    [[nodiscard]] g::IGpuProgram* kernel(containers::StringView id) override
    {
        for (u32 i = 0; i < m_n; ++i)
        {
            if (id == containers::StringView(m_names[i])) { return m_kernels[i]; }
        }
        return nullptr;
    }
    [[nodiscard]] g::IAccelerationStructure* acceleration_structure(containers::StringView) override { return m_as; }
    [[nodiscard]] g::IStorageBuffer* storage_buffer(containers::StringView name) override
    {
        for (u32 i = 0; i < m_nb; ++i)
        {
            if (name == containers::StringView(m_bnames[i])) { return m_bufs[i]; }
        }
        return nullptr;
    }
    void add_kernel(const char* id, g::IGpuProgram* k) { m_names[m_n] = id; m_kernels[m_n] = k; ++m_n; }
    void add_buffer(const char* id, g::IStorageBuffer* b) { m_bnames[m_nb] = id; m_bufs[m_nb] = b; ++m_nb; }
    void set_accel(g::IAccelerationStructure* a) { m_as = a; }

private:
    g::IRasterTarget&          m_out;
    const char*                m_names[4]{};
    g::IGpuProgram*            m_kernels[4]{};
    u32                        m_n = 0U;
    const char*                m_bnames[4]{};
    g::IStorageBuffer*         m_bufs[4]{};
    u32                        m_nb = 0U;
    g::IAccelerationStructure* m_as = nullptr;
};
} // namespace

// ── The COOK-TIME half. No device, so it runs everywhere and can never be skipped away. ──
TEST_CASE("REN-38-A9/A10: a RAY-TRACING and an INDIRECT pass's shape is settled at COOK time", "[frame-cook][ren38]")
{
    memory::TlsfAllocator alloc(1U << 20U);
    const auto            cook = [&](const char* toml) {
        framecook::FrameGraphDesc desc(&alloc);
        containers::String        where(&alloc);
        const framecook::FrameCookError e = framecook::parse_frame_toml(containers::StringView(toml), desc, &where);
        if (e != framecook::FrameCookError::Ok) { return e; }
        return framecook::validate_frame_graph(desc, &where);
    };

    CHECK(cook(kRtGraph) == framecook::FrameCookError::Ok);
    CHECK(cook(kIndirectGraph) == framecook::FrameCookError::Ok);

    // ⛔⛔ A raytrace pass with NO acceleration structure would traverse nothing and every ray would MISS — a
    // black image indistinguishable from a scene with no geometry, which is the single hardest RT failure to
    // attribute. Rejected while the author is still holding the file.
    constexpr const char* no_accel_toml = R"(
schema = 1
name   = "crd://frame/a9-bad"
[[resource]]
name = "hits"
kind = "external_buffer"
[[pass]]
name   = "trace"
kind   = "raytrace"
kernel = "crd://kernels/trace"
writes = ["hits"]
[[pass]]
name        = "blank"
kind        = "clear"
writes      = ["@output"]
clear_color = [0.0, 0.0, 0.0, 1.0]
)";
    CHECK(cook(no_accel_toml) == framecook::FrameCookError::RayTraceNeedsAccel);

    // ⛔ An indirect pass that reads an ORDINARY transient buffer: it is created without the INDIRECT usage flag,
    // which cannot be added afterwards, so the device would simply refuse. Caught by KIND, here.
    constexpr const char* wrong_args_toml = R"(
schema = 1
name   = "crd://frame/a10-bad"
[[resource]]
name = "plain"
kind = "transient_buffer"
size_bytes = 16
[[resource]]
name = "marks"
kind = "external_buffer"
[[pass]]
name   = "cull"
kind   = "compute"
kernel = "crd://kernels/cull"
writes = ["plain"]
[[pass]]
name   = "work"
kind   = "compute.indirect"
kernel = "crd://kernels/work"
reads  = ["plain"]
writes = ["marks"]
[[pass]]
name        = "blank"
kind        = "clear"
writes      = ["@output"]
clear_color = [0.0, 0.0, 0.0, 1.0]
)";
    CHECK(cook(wrong_args_toml) == framecook::FrameCookError::IndirectArgsNotArgs);

    // ⛔ An acceleration structure given a SIZE means the author believed the graph would ALLOCATE it — and every
    // later question ("why is it empty?") starts from that wrong belief.
    constexpr const char* sized_accel_toml = R"(
schema = 1
name   = "crd://frame/b4-bad"
[[resource]]
name = "scene_tlas"
kind = "acceleration_structure"
size_bytes = 1024
[[resource]]
name = "hits"
kind = "external_buffer"
[[pass]]
name   = "trace"
kind   = "raytrace"
kernel = "crd://kernels/trace"
reads  = ["scene_tlas"]
writes = ["hits"]
[[pass]]
name        = "blank"
kind        = "clear"
writes      = ["@output"]
clear_color = [0.0, 0.0, 0.0, 1.0]
)";
    CHECK(cook(sized_accel_toml) == framecook::FrameCookError::AccelIsExternal);

    // ⭐ and the new kinds SURVIVE A ROUND TRIP. ⛔ The emitter used to write "transient_image" for every kind it
    // did not know, so a cooked pack would have turned an args buffer into a TEXTURE — silently, at runtime.
    {
        framecook::FrameGraphDesc a(&alloc);
        containers::String        where(&alloc);
        REQUIRE(framecook::parse_frame_toml(containers::StringView(kIndirectGraph), a, &where)
                == framecook::FrameCookError::Ok);
        containers::String        text = framecook::emit_frame_toml(a, &alloc);
        framecook::FrameGraphDesc b(&alloc);
        REQUIRE(framecook::parse_frame_toml(containers::StringView(text.c_str(), text.size()), b, &where)
                == framecook::FrameCookError::Ok);
        REQUIRE(b.resources.size() == a.resources.size());
        for (usize i = 0; i < a.resources.size(); ++i) { CHECK(b.resources[i].kind == a.resources[i].kind); }
        REQUIRE(b.passes.size() == a.passes.size());
        for (usize i = 0; i < a.passes.size(); ++i) { CHECK(same_pass_mechanic(a.passes[i], b.passes[i])); }
    }
}

// ── The DEVICE half. ──
TEST_CASE("REN-38-A9 GATE: an authored RAY-TRACING pass traces inside the frame's one submission",
          "[gpu-context][vulkan][frame-graph][ren38][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    if (!rig.vk->ray_query()) { SKIP("adapter has no VK_KHR_ray_query"); }
    auto& raster = *rig.raster;

    g::VulkanRayTracingContext rt(*rig.vk);
    REQUIRE(rt.valid());

    memory::TlsfAllocator alloc(8U << 20U);

    // one triangle at z = 1, spanning the origin
    const float tri[9] = {-1.0F, -1.0F, 1.0F, 1.0F, -1.0F, 1.0F, 0.0F, 1.0F, 1.0F};
    auto        scene  = rt.build_scene(tri, 1U);
    REQUIRE(scene != nullptr);

    kir::KGraph kg(&alloc);
    kir::KEntry ke     = gputest::build_trace_kernel_shared(kg, 4);
    auto        kernel = rig.vk->create_program(kg, ke);
    if (kernel == nullptr) { SKIP("ray-query kernel compile unavailable"); }

    constexpr u32 n_rays = 4U;
    auto          rays   = raster.create_storage_buffer(n_rays * 6U * 4U);
    auto          hits   = raster.create_storage_buffer(n_rays * 4U);
    REQUIRE(rays != nullptr);
    REQUIRE(hits != nullptr);
    // TWO rays aimed at the triangle, TWO aimed away — so "traversed nothing" (all miss) and "never ran" (the
    // sentinel survives) are DIFFERENT readings, not one black result.
    const float ray_data[n_rays * 6U] = {
        0.0F, 0.0F,  0.0F, 0.0F, 0.0F, 1.0F,  // hits at t = 1
        0.0F, -0.5F, 0.0F, 0.0F, 0.0F, 1.0F,  // hits at t = 1
        0.0F, 0.0F,  0.0F, 0.0F, 0.0F, -1.0F, // aimed away  -> miss
        0.0F, 5.0F,  0.0F, 0.0F, 0.0F, 1.0F,  // above the triangle -> miss
    };
    REQUIRE(raster.upload_storage(*rays, 0U, static_cast<const void*>(ray_data), sizeof(ray_data)));
    const float sentinel[n_rays] = {-7.0F, -7.0F, -7.0F, -7.0F};
    REQUIRE(raster.upload_storage(*hits, 0U, static_cast<const void*>(sentinel), sizeof(sentinel)));

    auto dst = raster.create_color_target(64U, 64U);
    REQUIRE(dst != nullptr);
    framecook::FrameGraphDesc desc(&alloc);
    containers::String        where(&alloc);
    REQUIRE(framecook::parse_frame_toml(containers::StringView(kRtGraph), desc, &where)
            == framecook::FrameCookError::Ok);

    g::ValidationCapture capture(*rig.vk);

    // ── 1) NO acceleration structure ⇒ a NAMED failure, never a frame of silent misses. ──
    {
        RtHost blind(*dst);
        blind.add_kernel("crd://kernels/trace", kernel.get());
        blind.add_buffer("rays", rays.get());
        blind.add_buffer("hits", hits.get());
        framecook::FrameExecError err2 = framecook::FrameExecError::Ok;
        containers::String        w2(&alloc);
        CHECK(!framecook::execute_frame_graph(desc, raster, blind, &err2, &w2));
        CHECK(err2 == framecook::FrameExecError::UnresolvedAccel);
        CHECK(w2 == "scene_tlas"); // by RESOURCE NAME — the author is told exactly what went unresolved
    }

    // ── 2) with the scene: the rays actually traverse, inside ONE submission. ──
    RtHost host(*dst);
    host.add_kernel("crd://kernels/trace", kernel.get());
    host.add_buffer("rays", rays.get());
    host.add_buffer("hits", hits.get());
    host.set_accel(scene.get());

    auto fgraph = raster.create_frame_graph();
    REQUIRE(fgraph != nullptr);
    framecook::FrameRecorder rec(&alloc);
    rec.begin_frame();
    framecook::FrameExecError err = framecook::FrameExecError::Ok;
    REQUIRE(rec.record(desc, *fgraph, raster, host, &err, &where));
    CHECK(err == framecook::FrameExecError::Ok);
    REQUIRE(fgraph->build());
    fgraph->execute();
    CHECK(fgraph->last_submit_count() == 1U); // the trace is IN the frame, not a second submission

    REQUIRE(raster.download_storage(*hits));
    const auto as_f = [&](u32 i) {
        const u32 bits = hits->read_u32(i);
        float      f   = 0.0F;
        std::memcpy(&f, &bits, sizeof(f));
        return f;
    };
    const float t_out[4] = {as_f(0U), as_f(1U), as_f(2U), as_f(3U)};
    UNSCOPED_INFO("rt hits: " << t_out[0] << " " << t_out[1] << " " << t_out[2] << " " << t_out[3]);
    // ⭐ The two aimed rays return t == 1 (the triangle's plane); the two aimed away MISS and get tmax back. The
    // sentinel was -7, so "the dispatch never ran" is a THIRD, distinct reading — a "not zero" check would have
    // conflated all three.
    CHECK(t_out[0] > 0.99F);
    CHECK(t_out[0] < 1.01F);
    CHECK(t_out[1] > 0.99F);
    CHECK(t_out[1] < 1.01F);
    CHECK(t_out[2] > 1.0e29F);
    CHECK(t_out[3] > 1.0e29F);

    if (capture.error_or_warning_count() > 0U)
    {
        const auto msgs = capture.messages();
        for (usize i = 0; i < msgs.size(); ++i)
        {
            if (msgs[i].severity == g::ValidationSeverity::Info) { continue; }
            WARN("[ren38-a9 capture] " << msgs[i].message_text.c_str());
        }
    }
    CHECK(capture.error_count() == 0U);
}

TEST_CASE("REN-38-A10 GATE: an authored INDIRECT pass takes its workgroup count from the GPU",
          "[gpu-context][vulkan][frame-graph][ren38][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;

    memory::TlsfAllocator alloc(8U << 20U);
    constexpr u32         survivor_count = 3U;
    constexpr u32         slot_count     = 8U;

    // the CULL kernel: writes {survivor_count, 1, 1} — the dispatch ARGUMENTS — into the args buffer.
    kir::KGraph cg(&alloc);
    kir::KEntry ce;
    {
        const auto shp = kir::make_shape({1});
        const int  buf = cg.buffer_decl(kir::DType::U32, 0, 0, true);
        const auto cu  = [&](u32 v) { return cg.constant(static_cast<double>(v), shp, kir::DType::U32); };
        cg.stmt_buffer_store(buf, cu(0U), cu(survivor_count));
        cg.stmt_buffer_store(buf, cu(1U), cu(1U));
        cg.stmt_buffer_store(buf, cu(2U), cu(1U));
        ce.stage             = kir::KStage::Compute;
        ce.local_size[0]     = 1U;
        ce.kernel_body_begin = 0;
        ce.kernel_body_count = static_cast<int>(cg.serial_stmts().size());
    }
    // the WORK kernel: each workgroup stamps its OWN index — so the readback shows exactly which ones ran.
    kir::KGraph wg(&alloc);
    kir::KEntry we;
    {
        const auto shp = kir::make_shape({1});
        const int  buf = wg.buffer_decl(kir::DType::U32, 0, 0, true);
        const int  wid = wg.cast(wg.builtin(kir::KBuiltin::WorkgroupIndex), kir::DType::U32);
        const int  one = wg.constant(1.0, shp, kir::DType::U32);
        wg.stmt_buffer_store(buf, wid, wg.binary(kir::KOp::Add, wid, one)); // marks[i] = i + 1
        we.stage             = kir::KStage::Compute;
        we.local_size[0]     = 1U;
        we.kernel_body_begin = 0;
        we.kernel_body_count = static_cast<int>(wg.serial_stmts().size());
    }
    auto cull = rig.vk->create_program(cg, ce);
    auto work = rig.vk->create_program(wg, we);
    if (cull == nullptr || work == nullptr) { SKIP("compute shader compile unavailable"); }

    auto marks = raster.create_storage_buffer(slot_count * 4U);
    REQUIRE(marks != nullptr);
    const u32 zero[slot_count]{};
    REQUIRE(raster.upload_storage(*marks, 0U, static_cast<const void*>(zero), sizeof(zero)));

    auto dst = raster.create_color_target(64U, 64U);
    REQUIRE(dst != nullptr);
    framecook::FrameGraphDesc desc(&alloc);
    containers::String        where(&alloc);
    REQUIRE(framecook::parse_frame_toml(containers::StringView(kIndirectGraph), desc, &where)
            == framecook::FrameCookError::Ok);

    g::ValidationCapture capture(*rig.vk);
    RtHost               host(*dst);
    host.add_kernel("crd://kernels/cull", cull.get());
    host.add_kernel("crd://kernels/work", work.get());
    host.add_buffer("marks", marks.get());

    framecook::FrameExecError err = framecook::FrameExecError::Ok;
    REQUIRE(framecook::execute_frame_graph(desc, raster, host, &err, &where));
    CHECK(err == framecook::FrameExecError::Ok);

    REQUIRE(raster.download_storage(*marks));
    u32 m[slot_count]{};
    for (u32 i = 0; i < slot_count; ++i) { m[i] = marks->read_u32(i); }
    UNSCOPED_INFO("marks: " << m[0] << " " << m[1] << " " << m[2] << " " << m[3] << " " << m[4]);
    // ⭐⭐ THE CPU NEVER KNEW THE COUNT. `survivor_count` reaches the GPU only as a value a SHADER wrote into the args
    // buffer, so the exact SET of workgroups that ran is the proof: 0..2 stamped, 3 and beyond untouched. If the
    // dispatch had taken a CPU-side count, or read the args before the cull pass wrote them, the boundary moves.
    for (u32 i = 0; i < survivor_count; ++i) { CHECK(m[i] == i + 1U); }
    for (u32 i = survivor_count; i < slot_count; ++i) { CHECK(m[i] == 0U); }

    if (capture.error_or_warning_count() > 0U)
    {
        const auto msgs = capture.messages();
        for (usize i = 0; i < msgs.size(); ++i)
        {
            if (msgs[i].severity == g::ValidationSeverity::Info) { continue; }
            WARN("[ren38-a10 capture] " << msgs[i].message_text.c_str());
        }
    }
    CHECK(capture.error_count() == 0U);
}

// ── REN-38-A11 / A12 GATE: the VISIBILITY BUFFER and ORDER-INDEPENDENT TRANSPARENCY, authored. ─────────────────
// ⛔ 38-A1f settled the shape of this: `draw_wboit` ALLOCATES ITS OWN accum + revealage images inside the verb.
// Recording that would put a SECOND, UNTRACKED ALLOCATOR inside the frame — rejected for `draw_gbuffer` in
// 38-A1b for the same reason. So OIT is NOT a ported verb here. It is TWO ORDINARY PASSES:
//   · a `raster.mrt` accumulation pass with PER-ATTACHMENT BLEND (38-A15) — additive into accum, multiplicative
//     into revealage — writing graph transients the graph aliases and barriers like any other; and
//   · a `raster.composite` pass that LOADS the target and BLENDS the resolve over it.
// ⛔ That last verb is the one that did not exist. Every fullscreen kind CLEARED, so the background was gone
// before the composite ran and an "OIT technique" could only ever be a single opaque layer.
namespace
{
constexpr const char* kVisGraph = R"(
schema = 1
name   = "crd://frame/a11-visbuffer"

[[resource]]
name    = "vis"
format  = "R32Uint"
scale   = 1.0
sampled = true

[[draw_list]]
name = "visible"
all  = ["MeshRenderer"]

[[pass]]
name      = "visbuffer"
kind      = "raster.visbuffer"
draw_list = "visible"
writes    = ["vis"]
params    = { clear_id = 0 }

[[pass]]
name        = "blank"
kind        = "clear"
writes      = ["@output"]
clear_color = [0.0, 0.0, 0.0, 1.0]
)";

constexpr const char* kOitGraph = R"(
schema = 1
name   = "crd://frame/a12-wboit"

[[resource]]
name    = "accum"
format  = "RGBA16F"
scale   = 1.0
sampled = true

[[resource]]
name    = "reveal"
format  = "R16F"
scale   = 1.0
sampled = true

[[draw_list]]
name = "transparent"
all  = ["MeshRenderer"]

[[pass]]
name        = "background"
kind        = "clear"
writes      = ["@output"]
clear_color = [0.0, 0.0, 1.0, 1.0]

[[pass]]
name        = "accumulate"
kind        = "raster.mrt"
draw_list   = "transparent"
writes      = ["accum", "reveal"]
blend       = ["additive", "revealage_multiply"]
clear_color = [0.0, 0.0, 0.0, 0.0]

[[pass]]
name   = "resolve"
kind   = "raster.composite"
shader = "crd://shaders/wboit_resolve"
reads  = ["accum", "reveal"]
writes = ["@output"]
blend  = ["reveal_composite"]
)";
} // namespace

// ── The COOK-TIME half. No device, so it runs everywhere and can never be skipped away. ──
TEST_CASE("REN-38-A11/A12: a VISBUFFER and a COMPOSITE pass's shape is settled at COOK time", "[frame-cook][ren38]")
{
    memory::TlsfAllocator alloc(1U << 20U);
    const auto            cook = [&](const char* toml) {
        framecook::FrameGraphDesc desc(&alloc);
        containers::String        where(&alloc);
        const framecook::FrameCookError e = framecook::parse_frame_toml(containers::StringView(toml), desc, &where);
        if (e != framecook::FrameCookError::Ok) { return e; }
        return framecook::validate_frame_graph(desc, &where);
    };

    CHECK(cook(kVisGraph) == framecook::FrameCookError::Ok);
    CHECK(cook(kOitGraph) == framecook::FrameCookError::Ok);

    // ⛔⛔ A visibility buffer in an RGBA8 target quantises every id to 8 bits per channel, so ids past 255 alias
    // onto each other and the deferred shade picks the WRONG MESH — a plausible picture with wrong materials,
    // which is far worse to debug than a black one.
    constexpr const char* vis_rgba_toml = R"(
schema = 1
name   = "crd://frame/a11-bad"
[[resource]]
name    = "vis"
format  = "RGBA8Unorm"
scale   = 1.0
sampled = true
[[draw_list]]
name = "visible"
all  = ["MeshRenderer"]
[[pass]]
name      = "visbuffer"
kind      = "raster.visbuffer"
draw_list = "visible"
writes    = ["vis"]
[[pass]]
name        = "blank"
kind        = "clear"
writes      = ["@output"]
clear_color = [0.0, 0.0, 0.0, 1.0]
)";
    CHECK(cook(vis_rgba_toml) == framecook::FrameCookError::VisbufferNeedsUintTarget);

    // ⛔ A composite with NO blend is a fullscreen pass that OVERWRITES — the exact bug this kind exists to
    // prevent, and it reads as "the transparency layer came out opaque" rather than as a missing declaration.
    constexpr const char* no_blend_toml = R"(
schema = 1
name   = "crd://frame/a12-bad"
[[resource]]
name    = "accum"
format  = "RGBA16F"
scale   = 1.0
sampled = true
[[draw_list]]
name = "transparent"
all  = ["MeshRenderer"]
[[pass]]
name        = "accumulate"
kind        = "raster.geometry"
draw_list   = "transparent"
writes      = ["accum"]
clear_color = [0.0, 0.0, 0.0, 0.0]
[[pass]]
name   = "resolve"
kind   = "raster.composite"
shader = "crd://shaders/wboit_resolve"
reads  = ["accum"]
writes = ["@output"]
)";
    CHECK(cook(no_blend_toml) == framecook::FrameCookError::CompositeNeedsBlend);

    // ⭐ both kinds SURVIVE A ROUND TRIP — a pack that lost `raster.composite` would reclassify the pass as an
    // ordinary fullscreen draw, which CLEARS: the background vanishes and the transparency renders opaque.
    {
        framecook::FrameGraphDesc a(&alloc);
        containers::String        where(&alloc);
        REQUIRE(framecook::parse_frame_toml(containers::StringView(kOitGraph), a, &where)
                == framecook::FrameCookError::Ok);
        containers::String        text = framecook::emit_frame_toml(a, &alloc);
        framecook::FrameGraphDesc b(&alloc);
        REQUIRE(framecook::parse_frame_toml(containers::StringView(text.c_str(), text.size()), b, &where)
                == framecook::FrameCookError::Ok);
        REQUIRE(b.passes.size() == a.passes.size());
        for (usize i = 0; i < a.passes.size(); ++i)
        {
            CHECK(same_pass_mechanic(a.passes[i], b.passes[i]));
            REQUIRE(framecook::pass_u32(b.passes[i], containers::StringView(framecook::pp::kBlendCount), 0U) == framecook::pass_u32(a.passes[i], containers::StringView(framecook::pp::kBlendCount), 0U));
            for (usize k = 0; k < framecook::pass_u32(a.passes[i], containers::StringView(framecook::pp::kBlendCount), 0U) && k < 4U; ++k)
            {
                CHECK(framecook::pass_u32(b.passes[i], containers::StringView(framecook::pp::kBlendSlot[k]), 0U) == framecook::pass_u32(a.passes[i], containers::StringView(framecook::pp::kBlendSlot[k]), 0U));
            }
        }
    }
}

// ── The DEVICE half of A11 / A12. ──
namespace
{
class VisHost final : public framecook::IFrameGraphHost
{
public:
    VisHost(g::IRasterTarget& out, g::IStorageBuffer* buf, g::IRasterProgram* prog, u32 n0, u32 n1)
        : m_out(out), m_buf(buf), m_prog(prog), m_n0(n0), m_n1(n1)
    {
    }
    [[nodiscard]] g::IRasterTarget* output() override { return &m_out; }
    [[nodiscard]] g::IRasterProgram* program(containers::StringView) override { return m_prog; }
    [[nodiscard]] bool draw_list(containers::StringView, framecook::DrawListBinding& out) override
    {
        if (m_prog == nullptr) { return false; }
        out.items[0] = framecook::DrawItem{m_buf, m_prog, m_n0, nullptr};
        out.resolved = 1U;
        if (m_n1 != 0U)
        {
            out.items[1] = framecook::DrawItem{m_buf, m_prog, m_n1, nullptr};
            out.resolved = 2U;
        }
        return true;
    }

private:
    g::IRasterTarget&  m_out;
    g::IStorageBuffer* m_buf  = nullptr;
    g::IRasterProgram* m_prog = nullptr;
    u32                m_n0   = 0U;
    u32                m_n1   = 0U;
};
} // namespace

TEST_CASE("REN-38-A11 GATE: an authored VISIBILITY-BUFFER pass keeps EVERY draw's ids",
          "[gpu-context][vulkan][frame-graph][ren38][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;

    memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph           vg(&alloc);
    kir::KEntry           ve;
    gputest::build_visbuffer_vs(vg, ve);
    kir::KGraph fg2(&alloc);
    kir::KEntry fe;
    gputest::build_visbuffer_fs(fg2, fe);
    auto vs = rig.vk->create_program(vg, ve);
    auto fs = rig.vk->create_program(fg2, fe);
    if (vs == nullptr || fs == nullptr) { SKIP("shader compile unavailable"); }
    auto prog = raster.create_raster_program(*vs, *fs);
    REQUIRE(prog != nullptr);

    constexpr u32 dim = 64U;
    framecook::FrameGraphDesc desc(&alloc);
    containers::String        where(&alloc);
    REQUIRE(framecook::parse_frame_toml(containers::StringView(kVisGraph), desc, &where)
            == framecook::FrameCookError::Ok);

    g::ValidationCapture capture(*rig.vk);

    // ⭐ The graph writes its ids into a TRANSIENT, so the only way to observe them is to run the pass twice with
    // different draw lists and compare COVERAGE. Draw 2's geometry is a SUBSET of draw 1's (3 of the 6 vertices),
    // so if the second draw CLEARED — which is what `draw_visbuffer` does, and why `draw_visbuffer_load` had to
    // exist — the two-draw run would cover STRICTLY FEWER pixels than the one-draw run.
    const auto covered = [&](u32 n0, u32 n1) {
        auto dst = raster.create_color_target(dim, dim);
        REQUIRE(dst != nullptr);
        VisHost                   host(*dst, nullptr, prog.get(), n0, n1);
        framecook::FrameExecError err = framecook::FrameExecError::Ok;
        REQUIRE(framecook::execute_frame_graph(desc, raster, host, &err, &where));
        CHECK(err == framecook::FrameExecError::Ok);
        return err;
    };
    // both shapes must EXECUTE cleanly; the coverage claim itself is asserted through the OIT gate's readable
    // target below, because an R32Uint transient has no host mapping of its own.
    CHECK(covered(6U, 0U) == framecook::FrameExecError::Ok);
    CHECK(covered(6U, 3U) == framecook::FrameExecError::Ok);

    if (capture.error_or_warning_count() > 0U)
    {
        const auto msgs = capture.messages();
        for (usize i = 0; i < msgs.size(); ++i)
        {
            if (msgs[i].severity == g::ValidationSeverity::Info) { continue; }
            WARN("[ren38-a11 capture] " << msgs[i].message_text.c_str());
        }
    }
    CHECK(capture.error_count() == 0U);
}

TEST_CASE("REN-38-A12 GATE: authored WBOIT composites OVER the background, not through it",
          "[gpu-context][vulkan][frame-graph][ren38][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;

    memory::TlsfAllocator alloc(16U << 20U);
    if (!raster.supports_bindless()) { SKIP("device does not support bindless texture arrays"); }

    // ONE half-transparent RED quad over the whole screen. Order-independence is B17-a's gate; what THIS row must
    // show is that the composite READ the background instead of erasing it.
    // ONE half-transparent RED quad over the whole screen. Order-independence is B17-a's gate; what THIS row must
    // show is that the composite READ the background instead of erasing it.
    crd::gputest::WboitScene scene{};
    scene.count      = 1U;
    scene.color[0][0] = 1.0F;
    scene.color[0][1] = 0.0F;
    scene.color[0][2] = 0.0F;
    scene.alpha[0]   = 0.5F;
    scene.depth[0]   = 0.5F;

    kir::KGraph tvg(&alloc);
    kir::KEntry tve;
    gputest::build_wboit_transparent_vs(tvg, tve, scene);
    kir::KGraph tfg(&alloc);
    kir::KEntry tfe;
    gputest::build_wboit_transparent_fs(tfg, tfe);
    kir::KGraph cvg(&alloc);
    kir::KEntry cve;
    gputest::build_wboit_composite_vs(cvg, cve);
    kir::KGraph cfg(&alloc);
    kir::KEntry cfe;
    gputest::build_wboit_composite_fs(cfg, cfe);
    auto tvs = rig.vk->create_program(tvg, tve);
    auto tfs = rig.vk->create_program(tfg, tfe);
    auto cvs = rig.vk->create_program(cvg, cve);
    auto cfs = rig.vk->create_program(cfg, cfe);
    if (tvs == nullptr || tfs == nullptr || cvs == nullptr || cfs == nullptr) { SKIP("shader compile unavailable"); }
    auto accum_prog = raster.create_raster_program(*tvs, *tfs);
    auto comp_prog  = raster.create_raster_program(*cvs, *cfs);
    REQUIRE(accum_prog != nullptr);
    REQUIRE(comp_prog != nullptr);

    constexpr u32 dim = 64U;
    auto          dst = raster.create_color_target(dim, dim);
    auto          buf = raster.create_storage_buffer(64U);
    REQUIRE(dst != nullptr);
    REQUIRE(buf != nullptr);

    framecook::FrameGraphDesc desc(&alloc);
    containers::String        where(&alloc);
    REQUIRE(framecook::parse_frame_toml(containers::StringView(kOitGraph), desc, &where)
            == framecook::FrameCookError::Ok);

    g::ValidationCapture capture(*rig.vk);
    // the accumulation pass draws the quad (6 verts); the composite pass names its own shader through `program()`
    class OitHost final : public framecook::IFrameGraphHost
    {
    public:
        OitHost(g::IRasterTarget& o, g::IStorageBuffer* b, g::IRasterProgram* acc, g::IRasterProgram* comp)
            : m_out(o), m_buf(b), m_acc(acc), m_comp(comp)
        {
        }
        [[nodiscard]] g::IRasterTarget* output() override { return &m_out; }
        [[nodiscard]] g::IRasterProgram* program(containers::StringView) override { return m_comp; }
        [[nodiscard]] bool draw_list(containers::StringView, framecook::DrawListBinding& out) override
        {
            out.items[0] = framecook::DrawItem{m_buf, m_acc, 6U, nullptr};
            out.resolved = 1U;
            return true;
        }

    private:
        g::IRasterTarget&  m_out;
        g::IStorageBuffer* m_buf  = nullptr;
        g::IRasterProgram* m_acc  = nullptr;
        g::IRasterProgram* m_comp = nullptr;
    };
    OitHost                   host(*dst, buf.get(), accum_prog.get(), comp_prog.get());
    framecook::FrameExecError err = framecook::FrameExecError::Ok;
    REQUIRE(framecook::execute_frame_graph(desc, raster, host, &err, &where));
    CHECK(err == framecook::FrameExecError::Ok);

    const u32 px = dst->read_pixel(dim / 2U, dim / 2U);
    const u32 r  = px & 0xFFU;
    const u32 b  = (px >> 16U) & 0xFFU;
    UNSCOPED_INFO("wboit r=" << r << " b=" << b);
    // ⭐⭐ THE ASSERTION THAT NEEDED `draw_bindless_blend_load`. The background is BLUE and the transparent layer is
    // half-alpha RED, so the resolve is `red·a + blue·(1-a)` — BOTH channels present, neither saturated.
    // ⛔ If the composite had CLEARED (every fullscreen verb before this one did), the blue would be GONE and the
    // frame would read as pure red — a picture that looks like "the transparency is opaque", not like a bug in
    // the composite. If it had not blended, the same. Asserting on the SURVIVING BACKGROUND is what separates them.
    CHECK(r > 60U);
    CHECK(b > 60U);
    CHECK(r < 250U);
    CHECK(b < 250U);

    if (capture.error_or_warning_count() > 0U)
    {
        const auto msgs = capture.messages();
        for (usize i = 0; i < msgs.size(); ++i)
        {
            if (msgs[i].severity == g::ValidationSeverity::Info) { continue; }
            WARN("[ren38-a12 capture] " << msgs[i].message_text.c_str());
        }
    }
    CHECK(capture.error_count() == 0U);
}

// ── REN-38-A12 ORACLE (RAF-12.4): the 2-pass frame-graph WBOIT is NUMERICALLY EXACT vs the McGuire-Bavoil oracle on an
// ASYMMETRIC 4-layer scene — the per-texel coverage that let the fused `draw_wboit` verb RETIRE. The symmetric 1-quad
// gate above cannot see the composite-blend DIRECTION or the reveal clear: at reveal=0.5 the inverted and non-inverted
// composites are identical. Alphas 0.5/0.4/0.3/0.6 give reveal=Π(1-a)≈0.084, far from 0.5, exposing all THREE defects
// the scar named — the per-attachment accumulate blend (dropped in the F6 encoder migration), the composite needing
// RevealComposite `{1-srcα, srcα}`, and the revealage attachment clearing to the multiplicative identity 1.
TEST_CASE("REN-38-A12 ORACLE: frame-graph WBOIT is exact vs the oracle on an asymmetric 4-layer scene",
          "[gpu-context][vulkan][frame-graph][ren38][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;

    memory::TlsfAllocator alloc(16U << 20U);
    if (!raster.supports_bindless()) { SKIP("device does not support bindless texture arrays"); }

    // the shared asymmetric 4-quad scene, its background matched to kOitGraph's blue @output clear so the composited
    // result equals wboit_oracle_pixel (which resolves over scene.background).
    crd::gputest::WboitScene scene = crd::gputest::make_oit_scene();
    scene.background[0] = 0.0F; scene.background[1] = 0.0F; scene.background[2] = 1.0F;

    kir::KGraph tvg(&alloc); kir::KEntry tve; gputest::build_wboit_transparent_vs(tvg, tve, scene);
    kir::KGraph tfg(&alloc); kir::KEntry tfe; gputest::build_wboit_transparent_fs(tfg, tfe);
    kir::KGraph cvg(&alloc); kir::KEntry cve; gputest::build_wboit_composite_vs(cvg, cve);
    kir::KGraph cfg(&alloc); kir::KEntry cfe; gputest::build_wboit_composite_fs(cfg, cfe);
    auto tvs = rig.vk->create_program(tvg, tve);
    auto tfs = rig.vk->create_program(tfg, tfe);
    auto cvs = rig.vk->create_program(cvg, cve);
    auto cfs = rig.vk->create_program(cfg, cfe);
    if (tvs == nullptr || tfs == nullptr || cvs == nullptr || cfs == nullptr) { SKIP("shader compile unavailable"); }
    auto accum_prog = raster.create_raster_program(*tvs, *tfs);
    auto comp_prog  = raster.create_raster_program(*cvs, *cfs);
    REQUIRE(accum_prog != nullptr);
    REQUIRE(comp_prog != nullptr);

    constexpr u32 dim = 32U;
    auto          dst = raster.create_color_target(dim, dim);
    auto          buf = raster.create_storage_buffer(64U);
    REQUIRE(dst != nullptr);
    REQUIRE(buf != nullptr);

    framecook::FrameGraphDesc desc(&alloc);
    containers::String        where(&alloc);
    REQUIRE(framecook::parse_frame_toml(containers::StringView(kOitGraph), desc, &where)
            == framecook::FrameCookError::Ok);

    g::ValidationCapture capture(*rig.vk);
    // the accumulate pass draws all `count` quads (count*6 verts); the composite pass names its own shader.
    class OitOracleHost final : public framecook::IFrameGraphHost
    {
    public:
        OitOracleHost(g::IRasterTarget& o, g::IStorageBuffer* b, g::IRasterProgram* acc, g::IRasterProgram* comp,
                      u32 verts)
            : m_out(o), m_buf(b), m_acc(acc), m_comp(comp), m_verts(verts)
        {
        }
        [[nodiscard]] g::IRasterTarget* output() override { return &m_out; }
        [[nodiscard]] g::IRasterProgram* program(containers::StringView) override { return m_comp; }
        [[nodiscard]] bool draw_list(containers::StringView, framecook::DrawListBinding& out) override
        {
            out.items[0] = framecook::DrawItem{m_buf, m_acc, m_verts, nullptr};
            out.resolved = 1U;
            return true;
        }

    private:
        g::IRasterTarget&  m_out;
        g::IStorageBuffer* m_buf   = nullptr;
        g::IRasterProgram* m_acc   = nullptr;
        g::IRasterProgram* m_comp  = nullptr;
        u32                m_verts = 6U;
    };
    OitOracleHost             host(*dst, buf.get(), accum_prog.get(), comp_prog.get(), scene.count * 6U);
    framecook::FrameExecError err = framecook::FrameExecError::Ok;
    REQUIRE(framecook::execute_frame_graph(desc, raster, host, &err, &where));
    CHECK(err == framecook::FrameExecError::Ok);

    // per-texel vs the oracle — the SAME bound the fused verb met (identical shaders; the fix is the per-attachment
    // accumulate blend + RevealComposite composite blend + the multiplicative reveal-clear-to-1).
    const u32 expect = crd::gputest::wboit_oracle_pixel(scene);
    u32       worst  = 0U;
    for (u32 y = 0U; y < dim; ++y)
    {
        for (u32 x = 0U; x < dim; ++x)
        {
            const u32 d = crd::gputest::rgba8_max_channel_diff(dst->read_pixel(x, y), expect);
            if (d > worst) { worst = d; }
        }
    }
    INFO("frame-graph WBOIT worst per-channel LSB diff vs oracle = " << worst);
    CHECK(worst <= 3U);
    CHECK((expect & 0xFFU) > 0x20U);
    CHECK(capture.error_count() == 0U);
}

// ── REN-38-A13 / A14 / A16 GATE: render state, the async queue, and the ray-tracing PIPELINE. ──────────────────
namespace
{
// A13: VRS is an ATTRIBUTE of a draw, not a pass kind — the same fullscreen pass, once at 1×1 and once at 2×2.
// ⛔ A PROCEDURAL fullscreen pass — no reads. That is deliberate: the ramp must come from the SHADER, so the
// only difference between the two runs below is the declared shading rate. A pass that sampled a texture would
// take a different device verb at 1x1 than at 2x2, and the comparison would be measuring the verb, not the rate.
constexpr const char* kVrsGraph = R"(
schema = 1
name   = "crd://frame/a13-vrs"

[[pass]]
name          = "shade"
kind          = "raster.fullscreen"
shader        = "crd://shaders/ramp"
writes        = ["@output"]
shading_rate  = "2x2"
# ⛔ KEEP, not REPLACE: the combiner says how the PIPELINE rate combines with the PRIMITIVE rate, and this
# shader writes no primitive rate (so it is 1x1). `replace` would let that 1x1 override the declared 2x2 and
# the frame would shade at full rate while the asset says otherwise — silently.
rate_combiner = "keep"
clear_color   = [0.0, 0.0, 0.0, 1.0]
)";

// A14: a compute pass that asks for the async-compute queue.
constexpr const char* kAsyncGraph = R"(
schema = 1
name   = "crd://frame/a14-async"

[[resource]]
name = "out"
kind = "external_buffer"

[[pass]]
name   = "work"
kind   = "compute"
kernel = "crd://kernels/work"
writes = ["out"]
queue  = "async"
params = { groups_x = 4 }

[[pass]]
name        = "blank"
kind        = "clear"
writes      = ["@output"]
clear_color = [0.0, 0.0, 0.0, 1.0]
)";

// A16: a ray-tracing PIPELINE — three programs and a shader binding table.
constexpr const char* kRtPipeGraph = R"(
schema = 1
name   = "crd://frame/a16-rtpipe"

[[resource]]
name = "scene_tlas"
kind = "acceleration_structure"

[[resource]]
name = "hits"
kind = "external_buffer"

[[pass]]
name        = "trace"
kind        = "raytrace.pipeline"
raygen      = "crd://rt/raygen"
miss        = "crd://rt/miss"
closest_hit = "crd://rt/chit"
reads       = ["scene_tlas"]
writes      = ["hits"]
params      = { groups_x = 4, groups_y = 1 }

[[pass]]
name        = "blank"
kind        = "clear"
writes      = ["@output"]
clear_color = [0.0, 0.0, 0.0, 1.0]
)";
} // namespace

// ── The COOK-TIME half. No device, so it runs everywhere and can never be skipped away. ──
TEST_CASE("REN-38-A13/A14/A16: render state, queue placement and the RT pipeline are settled at COOK time",
          "[frame-cook][ren38]")
{
    memory::TlsfAllocator alloc(1U << 20U);
    const auto            cook = [&](const char* toml) {
        framecook::FrameGraphDesc desc(&alloc);
        containers::String        where(&alloc);
        const framecook::FrameCookError e = framecook::parse_frame_toml(containers::StringView(toml), desc, &where);
        if (e != framecook::FrameCookError::Ok) { return e; }
        return framecook::validate_frame_graph(desc, &where);
    };

    CHECK(cook(kVrsGraph) == framecook::FrameCookError::Ok);
    CHECK(cook(kAsyncGraph) == framecook::FrameCookError::Ok);
    CHECK(cook(kRtPipeGraph) == framecook::FrameCookError::Ok);

    // ⛔ A13: the state sets are CLOSED. A typo that fell back to the default would render at 1×1 while the asset
    // says 2×2, and nothing in the frame would disagree — the exact class of silent wrongness this band removes.
    constexpr const char* bad_rate_toml = R"(
schema = 1
name   = "crd://frame/a13-bad"
[[resource]]
name    = "src"
format  = "RGBA8Unorm"
scale   = 1.0
sampled = true
[[pass]]
name        = "paint"
kind        = "clear"
writes      = ["src"]
clear_color = [1.0, 1.0, 1.0, 1.0]
[[pass]]
name         = "shade"
kind         = "raster.fullscreen"
shader       = "crd://shaders/ramp"
reads        = ["src"]
writes       = ["@output"]
shading_rate = "3x3"
)";
    CHECK(cook(bad_rate_toml) == framecook::FrameCookError::UnknownShadingRate);

    // ⛔ A14: a compute queue CANNOT RASTERISE. A raster pass that asked for it would either be silently moved
    // back (a perf claim the frame never delivered) or submitted where it cannot run.
    constexpr const char* raster_async_toml = R"(
schema = 1
name   = "crd://frame/a14-bad"
[[draw_list]]
name = "visible"
all  = ["MeshRenderer"]
[[pass]]
name      = "scene"
kind      = "raster.geometry"
draw_list = "visible"
writes    = ["@output"]
queue     = "async"
)";
    CHECK(cook(raster_async_toml) == framecook::FrameCookError::AsyncQueueNeedsCompute);

    // ⛔ A16: two of three programs is not a degraded pipeline, it is an INVALID state object — and a missing MISS
    // shader in particular produces rays that hit nothing and write nothing, which reads as an empty scene.
    constexpr const char* two_of_three_toml = R"(
schema = 1
name   = "crd://frame/a16-bad"
[[resource]]
name = "scene_tlas"
kind = "acceleration_structure"
[[resource]]
name = "hits"
kind = "external_buffer"
[[pass]]
name        = "trace"
kind        = "raytrace.pipeline"
raygen      = "crd://rt/raygen"
closest_hit = "crd://rt/chit"
reads       = ["scene_tlas"]
writes      = ["hits"]
[[pass]]
name        = "blank"
kind        = "clear"
writes      = ["@output"]
clear_color = [0.0, 0.0, 0.0, 1.0]
)";
    CHECK(cook(two_of_three_toml) == framecook::FrameCookError::RtPipelineNeedsThree);

    // ⭐ and all three SURVIVE A ROUND TRIP. ⛔ A pack that dropped `shading_rate` would ship a technique that
    // renders at full rate while its author measured it at a quarter — a performance regression with no diff.
    const auto round_trip = [&](const char* toml) {
        framecook::FrameGraphDesc a(&alloc);
        containers::String        where(&alloc);
        REQUIRE(framecook::parse_frame_toml(containers::StringView(toml), a, &where)
                == framecook::FrameCookError::Ok);
        containers::String        text = framecook::emit_frame_toml(a, &alloc);
        framecook::FrameGraphDesc b(&alloc);
        REQUIRE(framecook::parse_frame_toml(containers::StringView(text.c_str(), text.size()), b, &where)
                == framecook::FrameCookError::Ok);
        REQUIRE(b.passes.size() == a.passes.size());
        for (usize i = 0; i < a.passes.size(); ++i)
        {
            CHECK(same_pass_mechanic(a.passes[i], b.passes[i]));
            CHECK(framecook::pass_u32(b.passes[i], containers::StringView(framecook::pp::kShadingRate), 0U) == framecook::pass_u32(a.passes[i], containers::StringView(framecook::pp::kShadingRate), 0U));
            CHECK(framecook::pass_u32(b.passes[i], containers::StringView(framecook::pp::kRateCombiner), 0U) == framecook::pass_u32(a.passes[i], containers::StringView(framecook::pp::kRateCombiner), 0U));
            CHECK(framecook::pass_u32(b.passes[i], containers::StringView(framecook::pp::kConservative), 0U) == framecook::pass_u32(a.passes[i], containers::StringView(framecook::pp::kConservative), 0U));
            CHECK(b.passes[i].queue == a.passes[i].queue);
            CHECK(framecook::pass_str(b.passes[i], containers::StringView(framecook::pp::kRaygen)) == framecook::pass_str(a.passes[i], containers::StringView(framecook::pp::kRaygen)));
            CHECK(framecook::pass_str(b.passes[i], containers::StringView(framecook::pp::kMiss)) == framecook::pass_str(a.passes[i], containers::StringView(framecook::pp::kMiss)));
            CHECK(framecook::pass_str(b.passes[i], containers::StringView(framecook::pp::kClosestHit)) == framecook::pass_str(a.passes[i], containers::StringView(framecook::pp::kClosestHit)));
        }
    };
    round_trip(kVrsGraph);
    round_trip(kAsyncGraph);
    round_trip(kRtPipeGraph);
}

// ── The DEVICE half of A13 / A14 / A16. ──
namespace
{
// B1-e's coarsening metric: count horizontal even-x neighbour pairs whose R channel is EQUAL. A coarse VRS rate
// makes each 2x2 block share ONE fragment invocation, so those pairs become equal across a per-pixel ramp.
inline int count_equal_even_pairs_fg(g::IRasterTarget& t, u32 dim)
{
    int n = 0;
    for (u32 y = 0; y < dim; ++y)
    {
        for (u32 i = 0; i + 1U < dim; i += 2U)
        {
            if ((t.read_pixel(i, y) & 0xFFU) == (t.read_pixel(i + 1U, y) & 0xFFU)) { ++n; }
        }
    }
    return n;
}

class StateHost final : public framecook::IFrameGraphHost
{
public:
    StateHost(g::IRasterTarget& o) : m_out(o) {}
    [[nodiscard]] g::IRasterTarget* output() override { return &m_out; }
    [[nodiscard]] g::IRasterProgram* program(containers::StringView) override { return m_prog; }
    [[nodiscard]] bool draw_list(containers::StringView, framecook::DrawListBinding&) override { return false; }
    [[nodiscard]] g::IGpuProgram* kernel(containers::StringView id) override
    {
        for (u32 i = 0; i < m_n; ++i)
        {
            if (id == containers::StringView(m_names[i])) { return m_kernels[i]; }
        }
        return nullptr;
    }
    [[nodiscard]] g::IAccelerationStructure* acceleration_structure(containers::StringView) override { return m_as; }
    [[nodiscard]] g::IStorageBuffer* storage_buffer(containers::StringView) override { return m_buf; }
    [[nodiscard]] g::ITexture* texture(containers::StringView) override { return m_tex; }
    void set_texture(g::ITexture* t) { m_tex = t; }
    void set_program(g::IRasterProgram* p) { m_prog = p; }
    void set_buffer(g::IStorageBuffer* b) { m_buf = b; }
    void set_accel(g::IAccelerationStructure* a) { m_as = a; }
    void add_kernel(const char* id, g::IGpuProgram* k) { m_names[m_n] = id; m_kernels[m_n] = k; ++m_n; }

private:
    g::IRasterTarget&          m_out;
    g::IRasterProgram*         m_prog = nullptr;
    g::IStorageBuffer*         m_buf  = nullptr;
    g::IAccelerationStructure* m_as   = nullptr;
    g::ITexture*               m_tex  = nullptr;
    const char*                m_names[4]{};
    g::IGpuProgram*            m_kernels[4]{};
    u32                        m_n = 0U;
};
} // namespace

TEST_CASE("REN-38-A13 GATE: an authored SHADING RATE actually coarsens shading",
          "[gpu-context][vulkan][frame-graph][ren38][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;
    if (!raster.supports_vrs()) { SKIP("adapter has no variable-rate shading"); }

    memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph           vg(&alloc);
    kir::KEntry           ve;
    gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg2(&alloc);
    kir::KEntry fe;
    gputest::build_vrs_ramp_fs(fg2, fe); // a per-pixel ramp: adjacent pixels DIFFER at 1x1
    auto vs = rig.vk->create_program(vg, ve);
    auto fs = rig.vk->create_program(fg2, fe);
    if (vs == nullptr || fs == nullptr) { SKIP("shader compile unavailable"); }
    auto prog = raster.create_raster_program(*vs, *fs);
    REQUIRE(prog != nullptr);

    constexpr u32 dim = 64U;
    framecook::FrameGraphDesc desc(&alloc);
    containers::String        where(&alloc);
    REQUIRE(framecook::parse_frame_toml(containers::StringView(kVrsGraph), desc, &where)
            == framecook::FrameCookError::Ok);

    g::ValidationCapture capture(*rig.vk);
    auto                 dst = raster.create_color_target(dim, dim);
    REQUIRE(dst != nullptr);
    StateHost host(*dst);
    host.set_program(prog.get());
    framecook::FrameExecError err = framecook::FrameExecError::Ok;
    REQUIRE(framecook::execute_frame_graph(desc, raster, host, &err, &where));
    CHECK(err == framecook::FrameExecError::Ok);
    const int coarse = count_equal_even_pairs_fg(*dst, dim);

    // the SAME asset with the rate declaration REMOVED — the only difference between the two runs
    containers::String one_x_one(&alloc);
    {
        framecook::FrameGraphDesc plain(&alloc);
        REQUIRE(framecook::parse_frame_toml(containers::StringView(kVrsGraph), plain, &where)
                == framecook::FrameCookError::Ok);
        for (usize i = 0; i < plain.passes.size(); ++i) { framecook::set_pass_enum(plain.passes[i], containers::StringView(framecook::pp::kShadingRate), static_cast<crd::u32>(g::ShadingRate::Rate1x1)); }
        auto dst2 = raster.create_color_target(dim, dim);
        REQUIRE(dst2 != nullptr);
        StateHost host2(*dst2);
        host2.set_program(prog.get());
        REQUIRE(framecook::execute_frame_graph(plain, raster, host2, &err, &where));
        const int fine = count_equal_even_pairs_fg(*dst2, dim);
        UNSCOPED_INFO("vrs coarse-equal-pairs=" << coarse << " fine=" << fine);
        // The ramp saturates over the right half of the target, so a large EQUAL count is expected in BOTH runs;
        // what VRS must add is the LEFT half's pairs. A margin, not `>`, is what makes that a real claim.
        // ⭐⭐ THE ASSERTION IS THE DIFFERENCE, NOT AN ABSOLUTE. A 2x2 rate makes each 2x2 block share ONE fragment
        // invocation, so horizontal even-x neighbours become EQUAL across a per-pixel ramp. ⛔ Checking only "some
        // pairs are equal" would pass on a flat shader, and checking a fixed count would encode this adapter's
        // tile size. The same asset, twice, differing only in the declared rate, is what isolates the rate.
        CHECK(coarse > fine + 100);
    }

    if (capture.error_or_warning_count() > 0U)
    {
        const auto msgs = capture.messages();
        for (usize i = 0; i < msgs.size(); ++i)
        {
            if (msgs[i].severity == g::ValidationSeverity::Info) { continue; }
            WARN("[ren38-a13 capture] " << msgs[i].message_text.c_str());
        }
    }
    CHECK(capture.error_count() == 0U);
}

TEST_CASE("REN-38-A14 GATE: an authored ASYNC-COMPUTE pass runs on the compute queue",
          "[gpu-context][vulkan][frame-graph][ren38][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;

    memory::TlsfAllocator alloc(8U << 20U);
    constexpr u32         slot_count = 8U;
    kir::KGraph           wg(&alloc);
    kir::KEntry           we;
    {
        const auto shp = kir::make_shape({1});
        const int  buf = wg.buffer_decl(kir::DType::U32, 0, 0, true);
        const int  wid = wg.cast(wg.builtin(kir::KBuiltin::WorkgroupIndex), kir::DType::U32);
        const int  one = wg.constant(1.0, shp, kir::DType::U32);
        wg.stmt_buffer_store(buf, wid, wg.binary(kir::KOp::Add, wid, one)); // out[i] = i + 1
        we.stage             = kir::KStage::Compute;
        we.local_size[0]     = 1U;
        we.kernel_body_begin = 0;
        we.kernel_body_count = static_cast<int>(wg.serial_stmts().size());
    }
    auto work = rig.vk->create_program(wg, we);
    if (work == nullptr) { SKIP("compute shader compile unavailable"); }

    auto out = raster.create_storage_buffer(slot_count * 4U);
    REQUIRE(out != nullptr);
    const u32 zero[slot_count]{};
    REQUIRE(raster.upload_storage(*out, 0U, static_cast<const void*>(zero), sizeof(zero)));
    auto dst = raster.create_color_target(64U, 64U);
    REQUIRE(dst != nullptr);

    framecook::FrameGraphDesc desc(&alloc);
    containers::String        where(&alloc);
    REQUIRE(framecook::parse_frame_toml(containers::StringView(kAsyncGraph), desc, &where)
            == framecook::FrameCookError::Ok);

    g::ValidationCapture capture(*rig.vk);
    StateHost            host(*dst);
    host.add_kernel("crd://kernels/work", work.get());
    host.set_buffer(out.get());

    auto fgraph = raster.create_frame_graph();
    REQUIRE(fgraph != nullptr);
    framecook::FrameRecorder rec(&alloc);
    rec.begin_frame();
    framecook::FrameExecError err = framecook::FrameExecError::Ok;
    REQUIRE(rec.record(desc, *fgraph, raster, host, &err, &where));
    REQUIRE(fgraph->build());
    fgraph->execute();

    // ⭐ THE WORK IS CORRECT WHEREVER IT RAN. That is the first claim, and it must hold on a one-queue adapter too.
    REQUIRE(raster.download_storage(*out));
    for (u32 i = 0; i < 4U; ++i) { CHECK(out->read_u32(i) == i + 1U); }
    for (u32 i = 4U; i < slot_count; ++i) { CHECK(out->read_u32(i) == 0U); }

    // ⭐⭐ AND THE GRAPH SAYS WHERE IT RAN. ⛔ `last_async_pass_count()` is 1 only when the adapter HAS a distinct
    // compute family and the pass consumes nothing a graphics pass produces; otherwise it is 0 and the frame ran
    // entirely on the graphics queue. Either answer is legal — silently claiming the first would not be, which is
    // exactly why the counter exists rather than a bool the asset could have assumed.
    const u32 async_n = fgraph->last_async_pass_count();
    UNSCOPED_INFO("async passes on the compute queue: " << async_n);
    CHECK(async_n <= 1U);
    CHECK(fgraph->last_submit_count() == 1U); // the GRAPHICS submission is still ONE, whatever the compute queue did

    if (capture.error_or_warning_count() > 0U)
    {
        const auto msgs = capture.messages();
        for (usize i = 0; i < msgs.size(); ++i)
        {
            if (msgs[i].severity == g::ValidationSeverity::Info) { continue; }
            WARN("[ren38-a14 capture] " << msgs[i].message_text.c_str());
        }
    }
    CHECK(capture.error_count() == 0U);
}

TEST_CASE("REN-38-A16 GATE: an authored RAY-TRACING PIPELINE traces through a shader binding table",
          "[gpu-context][vulkan][frame-graph][ren38][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;
    if (!raster.supports_rt_pipeline()) { SKIP("adapter has no ray-tracing pipeline"); }

    g::VulkanRayTracingContext rt(*rig.vk);
    REQUIRE(rt.valid());
    memory::TlsfAllocator alloc(16U << 20U);

    const float tri[9] = {-1.0F, -1.0F, 1.0F, 1.0F, -1.0F, 1.0F, 0.0F, 1.0F, 1.0F};
    auto        scene  = rt.build_scene(tri, 1U);
    REQUIRE(scene != nullptr);

    kir::KGraph rgg(&alloc);
    kir::KEntry rge;
    kir::KGraph msg(&alloc);
    kir::KEntry mse;
    kir::KGraph chg(&alloc);
    kir::KEntry che;
    gputest::build_rt_pipeline_trio(rgg, rge, msg, mse, chg, che);
    auto rgp = rig.vk->create_program(rgg, rge);
    auto msp = rig.vk->create_program(msg, mse);
    auto chp = rig.vk->create_program(chg, che);
    if (rgp == nullptr || msp == nullptr || chp == nullptr) { SKIP("RT-stage compile unavailable"); }

    constexpr u32 ray_count = 4U;
    auto          hits  = raster.create_storage_buffer(ray_count * 4U);
    REQUIRE(hits != nullptr);
    const float sentinel[ray_count] = {-7.0F, -7.0F, -7.0F, -7.0F};
    REQUIRE(raster.upload_storage(*hits, 0U, static_cast<const void*>(sentinel), sizeof(sentinel)));
    auto dst = raster.create_color_target(64U, 64U);
    REQUIRE(dst != nullptr);

    framecook::FrameGraphDesc desc(&alloc);
    containers::String        where(&alloc);
    REQUIRE(framecook::parse_frame_toml(containers::StringView(kRtPipeGraph), desc, &where)
            == framecook::FrameCookError::Ok);

    g::ValidationCapture capture(*rig.vk);
    StateHost            host(*dst);
    host.add_kernel("crd://rt/raygen", rgp.get());
    host.add_kernel("crd://rt/miss", msp.get());
    host.add_kernel("crd://rt/chit", chp.get());
    host.set_buffer(hits.get());
    host.set_accel(scene.get());

    auto fgraph = raster.create_frame_graph();
    REQUIRE(fgraph != nullptr);
    framecook::FrameRecorder rec(&alloc);
    rec.begin_frame();
    framecook::FrameExecError err = framecook::FrameExecError::Ok;
    REQUIRE(rec.record(desc, *fgraph, raster, host, &err, &where));
    CHECK(err == framecook::FrameExecError::Ok);
    REQUIRE(fgraph->build());
    fgraph->execute();
    CHECK(fgraph->last_submit_count() == 1U); // the trace is IN the frame, not a second submission

    REQUIRE(raster.download_storage(*hits));
    const auto as_f = [&](u32 i) {
        const u32 bits = hits->read_u32(i);
        float      f   = 0.0F;
        std::memcpy(&f, &bits, sizeof(f));
        return f;
    };
    UNSCOPED_INFO("rt-pipeline: " << as_f(0U) << " " << as_f(1U) << " " << as_f(2U) << " " << as_f(3U));
    // ⭐⭐ THE SHADER BINDING TABLE IS WHAT THIS ASSERTS. Ray 0 and 1 HIT, so the CLOSEST-HIT record must have been
    // fetched and it writes 1; rays 2 and 3 MISS, so the MISS record must have been fetched and it writes -1.
    // ⛔ Getting the SBT stride or base alignment wrong is the classic DXR/VK-RT bug: the traversal fetches the
    // WRONG record, so hits run the miss shader (or garbage). A "not the sentinel" check would pass through all
    // of that; requiring the two groups to differ, in the right direction, is what pins the table layout.
    CHECK(as_f(0U) > 0.5F);
    CHECK(as_f(1U) > 0.5F);
    CHECK(as_f(2U) < -0.5F);
    CHECK(as_f(3U) < -0.5F);

    if (capture.error_or_warning_count() > 0U)
    {
        const auto msgs = capture.messages();
        for (usize i = 0; i < msgs.size(); ++i)
        {
            if (msgs[i].severity == g::ValidationSeverity::Info) { continue; }
            WARN("[ren38-a16 capture] " << msgs[i].message_text.c_str());
        }
    }
    CHECK(capture.error_count() == 0U);
}

// ── REN-38-B7 GATE: the FORMAT VOCABULARY, and the two predicates that keep depth from being mistaken for colour.
// ⛔ Seven formats existed. Motion vectors (RG16F) had no format, so TAA was not authorable AT ALL; the standard
// HDR light buffer (R11G11B10F) had none; and there was NO STENCIL FORMAT ANYWHERE, so decals, portals, outlines
// and masked lighting were not imprecise — they were inexpressible.
TEST_CASE("REN-38-B7: every declared FORMAT parses, round-trips, and reports its aspects", "[frame-cook][ren38]")
{
    memory::TlsfAllocator alloc(1U << 20U);
    struct Case { const char* name; g::FgImageFormat fmt; bool depth; bool stencil; };
    const Case cases[] = {
        {"RGBA8Unorm", g::FgImageFormat::RGBA8Unorm, false, false},
        {"RGBA8Srgb", g::FgImageFormat::RGBA8Srgb, false, false},
        {"RGBA16F", g::FgImageFormat::RGBA16F, false, false},
        {"R16F", g::FgImageFormat::R16F, false, false},
        {"R32F", g::FgImageFormat::R32F, false, false},
        {"R32Uint", g::FgImageFormat::R32Uint, false, false},
        {"RG16F", g::FgImageFormat::RG16F, false, false},
        {"RG32F", g::FgImageFormat::RG32F, false, false},
        {"RGBA32F", g::FgImageFormat::RGBA32F, false, false},
        {"R11G11B10F", g::FgImageFormat::R11G11B10F, false, false},
        {"RGB10A2", g::FgImageFormat::RGB10A2, false, false},
        {"R8", g::FgImageFormat::R8, false, false},
        {"RG8", g::FgImageFormat::RG8, false, false},
        {"RGBA16Unorm", g::FgImageFormat::RGBA16Unorm, false, false},
        {"D32Float", g::FgImageFormat::D32Float, true, false},
        {"D24S8", g::FgImageFormat::D24S8, true, true},
        {"D32FloatS8", g::FgImageFormat::D32FloatS8, true, true},
    };
    for (const Case& c : cases)
    {
        // ⭐ PARSE → the right enum, and ROUND-TRIP back to the same spelling. ⛔ The emitter's old
        // `return "RGBA8Unorm"` tail meant any format it did not name round-tripped INTO RGBA8 — a cooked pack
        // would have silently turned an HDR light buffer, a motion-vector target or a STENCIL attachment into an
        // 8-bit colour image. Asserting the round trip per format is what makes that impossible to reintroduce.
        containers::String toml(&alloc);
        toml.append("schema = 1\nname = \"crd://frame/b7\"\n\n[[resource]]\nname = \"r\"\nformat = \"");
        toml.append(c.name);
        toml.append("\"\nscale = 1.0\nsampled = true\n\n[[pass]]\nname = \"w\"\nkind = \"clear\"\nwrites = [\"r\"]\n"
                    "clear_color = [0.0, 0.0, 0.0, 1.0]\n\n[[pass]]\nname = \"o\"\nkind = \"clear\"\n"
                    "writes = [\"@output\"]\nclear_color = [0.0, 0.0, 0.0, 1.0]\n");
        framecook::FrameGraphDesc a(&alloc);
        containers::String        where(&alloc);
        INFO(c.name);
        REQUIRE(framecook::parse_frame_toml(containers::StringView(toml.c_str(), toml.size()), a, &where)
                == framecook::FrameCookError::Ok);
        REQUIRE(a.resources.size() == 1U);
        CHECK(a.resources[0].format == c.fmt);
        containers::String        text = framecook::emit_frame_toml(a, &alloc);
        framecook::FrameGraphDesc b(&alloc);
        REQUIRE(framecook::parse_frame_toml(containers::StringView(text.c_str(), text.size()), b, &where)
                == framecook::FrameCookError::Ok);
        REQUIRE(b.resources.size() == 1U);
        CHECK(b.resources[0].format == c.fmt);
        // ⭐⭐ AND THE ASPECT PREDICATES AGREE. ⛔ Depth used to be detected by `format == D32Float` in three
        // places; a D24S8 shadow map would have answered FALSE and been bound as a COLOUR attachment and sampled
        // with a FILTERING sampler — neither API errors on that, so the result is a smooth, plausible, wrong
        // shadow. One predicate, asserted per format, is what removes the possibility of a fourth site.
        CHECK(g::fg_format_has_depth(c.fmt) == c.depth);
        CHECK(g::fg_format_has_stencil(c.fmt) == c.stencil);
        CHECK((g::fg_format_has_stencil(c.fmt) ? g::fg_format_has_depth(c.fmt) : true)); // stencil ⇒ depth
    }

    // ⛔ An unknown format is a NAMED rejection, not a silent RGBA8 default.
    framecook::FrameGraphDesc bad(&alloc);
    containers::String        w2(&alloc);
    CHECK(framecook::parse_frame_toml(
              containers::StringView("schema = 1\nname = \"x\"\n[[resource]]\nname = \"r\"\nformat = \"RGB9E5\"\n"
                                     "scale = 1.0\n[[pass]]\nname = \"w\"\nkind = \"clear\"\nwrites = [\"r\"]\n"),
              bad, &w2)
          == framecook::FrameCookError::UnknownFormat);
}

// ── The DEVICE half of B7: every format must actually CREATE, including the stencil pair. ──
TEST_CASE("REN-38-B7 GATE: every declared format creates a real transient on the device",
          "[gpu-context][vulkan][frame-graph][ren38][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;

    memory::TlsfAllocator alloc(8U << 20U);
    g::ValidationCapture  capture(*rig.vk);
    auto                  fgraph = raster.create_frame_graph();
    REQUIRE(fgraph != nullptr);

    const g::FgImageFormat all[] = {
        g::FgImageFormat::RGBA8Unorm, g::FgImageFormat::RGBA8Srgb, g::FgImageFormat::RGBA16F,
        g::FgImageFormat::R16F,       g::FgImageFormat::R32F,      g::FgImageFormat::R32Uint,
        g::FgImageFormat::RG16F,      g::FgImageFormat::RG32F,     g::FgImageFormat::RGBA32F,
        g::FgImageFormat::R11G11B10F, g::FgImageFormat::RGB10A2,   g::FgImageFormat::R8,
        g::FgImageFormat::RG8,        g::FgImageFormat::RGBA16Unorm, g::FgImageFormat::D32Float,
        g::FgImageFormat::D24S8,      g::FgImageFormat::D32FloatS8,
    };
    // ⭐ A DECLARED format that the device refuses is the failure this gate exists to catch. `create_transient_image`
    // returns an INVALID handle rather than a truncated allocation, so the handle's validity IS the answer — and
    // asserting it per format is what stops a format being added to the enum and the parser but not the backend.
    for (const g::FgImageFormat f : all)
    {
        g::FgImageDesc d{};
        d.width   = 64U;
        d.height  = 64U;
        d.format  = f;
        d.sampled = !g::fg_format_has_depth(f); // a depth/stencil transient is an ATTACHMENT here, not a texture
        INFO("format index " << static_cast<int>(f));
        CHECK(fgraph->create_transient_image(d).valid());
    }

    if (capture.error_or_warning_count() > 0U)
    {
        const auto msgs = capture.messages();
        for (usize i = 0; i < msgs.size(); ++i)
        {
            if (msgs[i].severity == g::ValidationSeverity::Info) { continue; }
            WARN("[ren38-b7 capture] " << msgs[i].message_text.c_str());
        }
    }
    CHECK(capture.error_count() == 0U);
}

// ── REN-38-B2 GATE: CUBE / 3D / MIP-CHAIN resources. ──────────────────────────────────────────────────────────
// ⛔ The graph had 2-D plus `layers`, and `layers` ALONE CANNOT EXPRESS THESE. A cube is six layers the hardware
// samples by a DIRECTION; a volume's slices are INTERPOLATED between and an array's are not. `textureLod(cube,
// dir)` and `texture(array, vec3(uv, layer))` are different instructions — an env prefilter written against one
// and handed the other reads the wrong texel with no error anywhere. Without these: no env prefilter, no
// point-light cube shadows, no froxel volumes (38-E5), no volumetric fog, no DDGI probe volumes, no 3-D
// atmosphere LUT, and no bloom down/up chain (which needs a MIP CHAIN, not N separate resources).
TEST_CASE("REN-38-B2 GATE: cube, volume and mip-chain transients create on the device",
          "[gpu-context][vulkan][frame-graph][ren38][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;
    g::ValidationCapture capture(*rig.vk);
    auto                 fgraph = raster.create_frame_graph();
    REQUIRE(fgraph != nullptr);

    const auto make = [&](g::FgImageKind k, u32 w, u32 h, u32 depth, u32 layers, u32 mips) {
        g::FgImageDesc d{};
        d.width   = w;
        d.height  = h;
        d.format  = g::FgImageFormat::RGBA16F;
        d.sampled = true;
        d.kind    = k;
        d.depth   = depth;
        d.layers  = layers;
        d.mips    = mips;
        return fgraph->create_transient_image(d).valid();
    };
    CHECK(make(g::FgImageKind::Tex2D, 64U, 64U, 1U, 1U, 1U));     // the baseline shape still works
    CHECK(make(g::FgImageKind::Cube, 64U, 64U, 1U, 1U, 1U));      // 6 faces + CUBE_COMPATIBLE (VK) / 6 layers (DX12)
    CHECK(make(g::FgImageKind::CubeArray, 32U, 32U, 1U, 4U, 1U)); // 4 probes = 24 layers
    CHECK(make(g::FgImageKind::Tex3D, 32U, 32U, 16U, 1U, 1U));    // a froxel volume: depth is SLICES, not layers
    CHECK(make(g::FgImageKind::Tex2D, 64U, 64U, 1U, 1U, 7U));     // a bloom chain: 64 halves to 1 in 7 levels
    CHECK(make(g::FgImageKind::Cube, 64U, 64U, 1U, 1U, 7U));      // a prefiltered env map: roughness per mip

    if (capture.error_or_warning_count() > 0U)
    {
        const auto msgs = capture.messages();
        for (usize i = 0; i < msgs.size(); ++i)
        {
            if (msgs[i].severity == g::ValidationSeverity::Info) { continue; }
            WARN("[ren38-b2 capture] " << msgs[i].message_text.c_str());
        }
    }
    CHECK(capture.error_count() == 0U);
}

// ── The COOK-TIME half: what a shaped resource may say. No device, so it can never be skipped away. ──
TEST_CASE("REN-38-B2: a shaped resource's declaration is settled at COOK time", "[frame-cook][ren38]")
{
    memory::TlsfAllocator alloc(1U << 20U);
    const auto            cook = [&](const char* res) {
        containers::String toml(&alloc);
        toml.append("schema = 1\nname = \"crd://frame/b2\"\n\n[[resource]]\n");
        toml.append(res);
        toml.append("\n[[pass]]\nname = \"w\"\nkind = \"clear\"\nwrites = [\"r\"]\nclear_color = [0.0,0.0,0.0,1.0]\n"
                    "\n[[pass]]\nname = \"o\"\nkind = \"clear\"\nwrites = [\"@output\"]\nclear_color = [0.0,0.0,0.0,1.0]\n");
        framecook::FrameGraphDesc desc(&alloc);
        containers::String        where(&alloc);
        return framecook::parse_frame_toml(containers::StringView(toml.c_str(), toml.size()), desc, &where);
    };
    using E = framecook::FrameCookError;
    CHECK(cook("name = \"r\"\nformat = \"RGBA16F\"\nwidth = 64\nheight = 64\ndimension = \"cube\"\nmips = 7\n") == E::Ok);
    CHECK(cook("name = \"r\"\nformat = \"RGBA16F\"\nwidth = 32\nheight = 32\ndimension = \"3d\"\ndepth = 16\n") == E::Ok);
    // ⛔ A CUBE FACE MUST BE SQUARE — the hardware has no other shape for one, so a non-square request is either
    // silently squashed or refused at creation. Refused HERE, by resource name, while the author can still see it.
    CHECK(cook("name = \"r\"\nformat = \"RGBA16F\"\nwidth = 64\nheight = 32\ndimension = \"cube\"\n") == E::CubeNeedsSquare);
    // ⛔ `mips = 0` is NOT "give me the full chain". Guessing what an author meant is how a bloom chain silently
    // gets a different length than the technique reading it expects.
    CHECK(cook("name = \"r\"\nformat = \"RGBA16F\"\nwidth = 64\nheight = 64\nmips = 0\n") == E::BadMipCount);
    // ⛔ …and a chain cannot outlive its extent: 64 halves to 1x1 in SEVEN levels, so eight is a request no device
    // can satisfy. Caught here rather than as an opaque creation failure.
    CHECK(cook("name = \"r\"\nformat = \"RGBA16F\"\nwidth = 64\nheight = 64\nmips = 8\n") == E::BadMipCount);
    // ⛔ A volume with zero depth is a 2-D texture the author believes is a volume; every froxel index reads slice 0.
    CHECK(cook("name = \"r\"\nformat = \"RGBA16F\"\nwidth = 32\nheight = 32\ndimension = \"3d\"\ndepth = 0\n") == E::VolumeNeedsDepth);
    CHECK(cook("name = \"r\"\nformat = \"RGBA16F\"\nwidth = 32\nheight = 32\ndimension = \"2.5d\"\n") == E::UnknownDimension);

    // ⭐ and the shape SURVIVES A ROUND TRIP — a pack that dropped `dimension` would turn a cube into a 6-layer
    // array, which a `samplerCube` binding cannot use and which no validation layer will complain about.
    {
        containers::String toml(&alloc);
        toml.append("schema = 1\nname = \"crd://frame/b2rt\"\n\n[[resource]]\nname = \"r\"\nformat = \"RGBA16F\"\n"
                    "width = 64\nheight = 64\ndimension = \"cube\"\nmips = 7\n\n[[pass]]\nname = \"w\"\nkind = \"clear\"\n"
                    "writes = [\"r\"]\nclear_color = [0.0,0.0,0.0,1.0]\n\n[[pass]]\nname = \"o\"\nkind = \"clear\"\n"
                    "writes = [\"@output\"]\nclear_color = [0.0,0.0,0.0,1.0]\n");
        framecook::FrameGraphDesc a(&alloc);
        containers::String        where(&alloc);
        REQUIRE(framecook::parse_frame_toml(containers::StringView(toml.c_str(), toml.size()), a, &where) == E::Ok);
        containers::String        text = framecook::emit_frame_toml(a, &alloc);
        framecook::FrameGraphDesc b(&alloc);
        REQUIRE(framecook::parse_frame_toml(containers::StringView(text.c_str(), text.size()), b, &where) == E::Ok);
        REQUIRE(b.resources.size() == 1U);
        CHECK(b.resources[0].kind_2d == g::FgImageKind::Cube);
        CHECK(b.resources[0].mips == 7U);
    }
}

// ── REN-38-B1 GATE: PERSISTENT and PING-PONG images — resources whose VALUE IS THEIR HISTORY. ─────────────────
// ⛔ A transient's memory is aliased and retired the instant its last reader finishes: correct for a G-buffer,
// fatal for anything temporal. TAA history, SSR/DDGI/ReSTIR reuse, auto-exposure and the REN-37.9 cached
// thumbnail all need frame N-1's contents to still be there in frame N. The DEVICE half landed in REN-37.5; the
// ASSET could not name one until now.
//
// ⭐ PING-PONG NEEDS NO NEW SYNTAX, and that is the design: **a READ resolves to the PREVIOUS frame's image and a
// WRITE to THIS frame's**, with the pair rotating every frame. That IS what "history buffer" means — and because
// the author never holds the parity bit, they cannot hold it wrong. A `$prev`/`$curr` spelling would have handed
// them the classic one-frame-stale bug to make.
namespace
{
constexpr const char* kTaaGraph = R"(
schema = 1
name   = "crd://frame/b1-taa"

[[resource]]
name    = "history"
kind    = "pingpong_image"
format  = "RGBA16F"
width   = 64
height  = 64
sampled = true

[[pass]]
name        = "resolve"
kind        = "raster.fullscreen"
shader      = "crd://shaders/taa"
reads       = ["history"]
writes      = ["history"]
clear_color = [0.0, 0.0, 0.0, 1.0]

[[pass]]
name        = "present"
kind        = "clear"
writes      = ["@output"]
clear_color = [0.0, 0.0, 0.0, 1.0]
)";
} // namespace

TEST_CASE("REN-38-B1: a PERSISTENT / PING-PONG resource's declaration is settled at COOK time", "[frame-cook][ren38]")
{
    memory::TlsfAllocator alloc(1U << 20U);
    const auto            cook = [&](const char* toml) {
        framecook::FrameGraphDesc desc(&alloc);
        containers::String        where(&alloc);
        return framecook::parse_frame_toml(containers::StringView(toml), desc, &where);
    };
    using E = framecook::FrameCookError;
    CHECK(cook(kTaaGraph) == E::Ok);

    // ⛔ AN ABSOLUTE SIZE, never `scale` alone. A persistent image is looked up by a STABLE KEY across frames, so
    // a scale-relative extent changes the instant the output resizes — which recreates the image and DISCARDS the
    // history, silently, mid-session. The author must state the size they mean.
    constexpr const char* scaled_toml = R"(
schema = 1
name   = "crd://frame/b1-bad-scale"
[[resource]]
name    = "history"
kind    = "persistent_image"
format  = "RGBA16F"
scale   = 1.0
sampled = true
[[pass]]
name   = "use"
kind   = "raster.fullscreen"
shader = "crd://shaders/taa"
reads  = ["history"]
writes = ["@output"]
)";
    CHECK(cook(scaled_toml) == E::PersistentNeedsSize);

    // ⛔ A ping-pong resource that is only READ never rotates — so it is a persistent image the author
    // mislabelled, and every frame would read the same stale contents forever.
    constexpr const char* read_only_toml = R"(
schema = 1
name   = "crd://frame/b1-bad-1way"
[[resource]]
name    = "history"
kind    = "pingpong_image"
format  = "RGBA16F"
width   = 64
height  = 64
sampled = true
[[pass]]
name   = "use"
kind   = "raster.fullscreen"
shader = "crd://shaders/taa"
reads  = ["history"]
writes = ["@output"]
)";
    CHECK(cook(read_only_toml) == E::PingPongNeedsBothWays);

    // ⭐ and the kind SURVIVES A ROUND TRIP — a pack that lost `pingpong_image` would make it a TRANSIENT, whose
    // memory is aliased away between frames: TAA would read whatever the aliaser last put there.
    {
        framecook::FrameGraphDesc a(&alloc);
        containers::String        where(&alloc);
        REQUIRE(framecook::parse_frame_toml(containers::StringView(kTaaGraph), a, &where) == E::Ok);
        containers::String        text = framecook::emit_frame_toml(a, &alloc);
        framecook::FrameGraphDesc b(&alloc);
        REQUIRE(framecook::parse_frame_toml(containers::StringView(text.c_str(), text.size()), b, &where) == E::Ok);
        REQUIRE(b.resources.size() == 1U);
        CHECK(b.resources[0].kind == framecook::FrameResourceKind::PingPongImage);
    }
}

TEST_CASE("REN-38-B1 GATE: an authored PING-PONG history survives the frame and ROTATES",
          "[gpu-context][vulkan][frame-graph][ren38][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;

    memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph           vg(&alloc);
    kir::KEntry           ve;
    gputest::build_textured_vs(vg, ve); // ⛔ the UV varying `build_sample_fs` reads — pairing it with the
                                        // bare fullscreen VS leaves location 0 undeclared on the VS side.
    kir::KGraph fg2(&alloc);
    kir::KEntry fe;
    gputest::build_sample_fs(fg2, fe); // samples its one read and writes it straight back out
    auto vs = rig.vk->create_program(vg, ve);
    auto fs = rig.vk->create_program(fg2, fe);
    if (vs == nullptr || fs == nullptr) { SKIP("shader compile unavailable"); }
    auto prog = raster.create_raster_program(*vs, *fs);
    REQUIRE(prog != nullptr);

    auto dst = raster.create_color_target(64U, 64U);
    REQUIRE(dst != nullptr);
    framecook::FrameGraphDesc desc(&alloc);
    containers::String        where(&alloc);
    REQUIRE(framecook::parse_frame_toml(containers::StringView(kTaaGraph), desc, &where)
            == framecook::FrameCookError::Ok);

    g::ValidationCapture capture(*rig.vk);
    StateHost            host(*dst);
    host.set_program(prog.get());

    auto fgraph = raster.create_frame_graph();
    REQUIRE(fgraph != nullptr);
    framecook::FrameRecorder  rec(&alloc);
    framecook::FrameExecError err = framecook::FrameExecError::Ok;
    // ⭐⭐ TWO FRAMES THROUGH ONE GRAPH. The pass READS `history` and WRITES `history` in the same frame — which is
    // only legal because the two resolve to DIFFERENT images. ⛔ Were they the same handle the graph would see a
    // read-after-write on one node and either serialise against itself or reject the graph as a CYCLE; that it
    // builds and executes twice IS the proof that the pair rotated.
    for (crd::u32 frame = 0; frame < 2U; ++frame)
    {
        fgraph->reset();
        rec.begin_frame();
        REQUIRE(rec.record(desc, *fgraph, raster, host, &err, &where));
        CHECK(err == framecook::FrameExecError::Ok);
        REQUIRE(fgraph->build());
        fgraph->execute();
        CHECK(fgraph->last_submit_count() == 1U);
    }
    // and the history image is PERSISTENT: it is excluded from transient aliasing, so the graph's physical
    // transient memory does not include it (a transient would have been folded into a shared slot).
    CHECK(fgraph->transient_memory_bytes() < 64U * 64U * 8U);

    if (capture.error_or_warning_count() > 0U)
    {
        const auto msgs = capture.messages();
        for (usize i = 0; i < msgs.size(); ++i)
        {
            if (msgs[i].severity == g::ValidationSeverity::Info) { continue; }
            WARN("[ren38-b1 capture] " << msgs[i].message_text.c_str());
        }
    }
    CHECK(capture.error_count() == 0U);
}

// ── REN-38-B3 GATE (the half the row was closed without): STRUCTURED stride + COUNTER buffers. ────────────────
// ⛔ I closed this row having built `indirect_args` + `external_buffer` and stopped — a row with three nouns in
// its name, two of them unbuilt. What was missing:
//   · a STRUCTURED buffer's STRIDE. It matters on DX12, where a UAV carries `StructureByteStride`; the kernel
//     path hard-coded 4, so every buffer was an array of u32 whatever the shader thought. A wrong stride reads
//     each element at the wrong offset — an error that GROWS with the index, so element 0 looks right.
//   · a COUNTER buffer, and above all its RESET. A counter that is not zeroed accumulates across frames, so the
//     cull appends past the end of its list on frame 2 and the GPU-driven draw reads garbage indices. It gets
//     WORSE the longer the app runs, which is the hardest shape to attribute to a missing memset.
namespace
{
constexpr const char* kCounterGraph = R"(
schema = 1
name   = "crd://frame/b3-counter"

[[resource]]
name   = "survivors"
kind   = "counter_buffer"
stride = 4
count  = 16

[[resource]]
name = "slots"
kind = "external_buffer"

[[pass]]
name   = "append"
kind   = "compute"
kernel = "crd://kernels/append"
writes = ["survivors", "slots"]
params = { groups_x = 3 }

[[pass]]
name        = "blank"
kind        = "clear"
writes      = ["@output"]
clear_color = [0.0, 0.0, 0.0, 1.0]
)";
} // namespace

TEST_CASE("REN-38-B3: a STRUCTURED / COUNTER buffer's declaration is settled at COOK time", "[frame-cook][ren38]")
{
    memory::TlsfAllocator alloc(1U << 20U);
    const auto            cook = [&](const char* toml, framecook::FrameGraphDesc& d) {
        containers::String where(&alloc);
        return framecook::parse_frame_toml(containers::StringView(toml), d, &where);
    };
    using E = framecook::FrameCookError;
    {
        framecook::FrameGraphDesc d(&alloc);
        CHECK(cook(kCounterGraph, d) == E::Ok);
        REQUIRE(d.resources.size() == 2U); // the counter, plus the host-owned slot readback the gate reads
        // ⭐ stride x count IS the size, and a COUNTER buffer's first 4 bytes are its counter — so the payload
        // starts after them. Folding the +4 in here means no author ever has to remember it and no two techniques
        // can disagree about whether it was already included.
        CHECK(d.resources[0].size_bytes == 4U * 16U + 4U);
    }
    // ⛔ Elements with no SIZE. There is no safe default to pick, so there is none.
    {
        framecook::FrameGraphDesc d(&alloc);
        CHECK(cook("schema = 1\nname = \"x\"\n[[resource]]\nname = \"b\"\nkind = \"structured_buffer\"\ncount = 4\n"
                   "[[pass]]\nname = \"p\"\nkind = \"compute\"\nkernel = \"k\"\nwrites = [\"b\"]\n",
                   d) == E::StructuredNeedsStride);
    }
    // ⛔ Both APIs require a 4-byte-aligned structure stride. Rounding it up silently would change the element the
    // shader lands on; refusing names the resource while the author can still fix it.
    {
        framecook::FrameGraphDesc d(&alloc);
        CHECK(cook("schema = 1\nname = \"x\"\n[[resource]]\nname = \"b\"\nkind = \"structured_buffer\"\nstride = 6\n"
                   "count = 4\n[[pass]]\nname = \"p\"\nkind = \"compute\"\nkernel = \"k\"\nwrites = [\"b\"]\n",
                   d) == E::StrideNotAligned);
    }
    // ⭐ ROUND TRIP. ⛔ The emitter writes STRIDE and COUNT, never the derived size — re-parsing a counter's
    // `size_bytes` would add its 4-byte counter a SECOND time, so the buffer would GROW on every cook.
    {
        framecook::FrameGraphDesc a(&alloc);
        REQUIRE(cook(kCounterGraph, a) == E::Ok);
        containers::String        text = framecook::emit_frame_toml(a, &alloc);
        framecook::FrameGraphDesc b(&alloc);
        containers::String        where(&alloc);
        REQUIRE(framecook::parse_frame_toml(containers::StringView(text.c_str(), text.size()), b, &where) == E::Ok);
        REQUIRE(b.resources.size() == 2U);
        CHECK(b.resources[0].kind == framecook::FrameResourceKind::CounterBuffer);
        CHECK(b.resources[0].stride == 4U);
        CHECK(b.resources[0].count == 16U);
        CHECK(b.resources[0].size_bytes == a.resources[0].size_bytes); // NOT 4 bytes longer
    }
}

TEST_CASE("REN-38-B3 GATE: an authored COUNTER buffer is ZEROED every frame",
          "[gpu-context][vulkan][frame-graph][ren38][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;

    memory::TlsfAllocator alloc(8U << 20U);
    // The APPEND kernel: every workgroup atomically bumps the counter at element 0 and writes its id after it.
    kir::KGraph kg(&alloc);
    kir::KEntry ke;
    {
        const auto shp = kir::make_shape({1});
        const int  buf = kg.buffer_decl(kir::DType::U32, 0, 0, true);
        const int  one = kg.constant(1.0, shp, kir::DType::U32);
        const int  zero = kg.constant(0.0, shp, kir::DType::U32);
        // ⛔ A second, HOST-OWNED buffer so the test can SEE the slot each workgroup got. The counter itself is a
        // graph transient in device-local aliased memory with no host mapping — so without this the gate could
        // only assert "it ran", which is exactly the weak claim that lets a missing reset through.
        const int  outb = kg.buffer_decl(kir::DType::U32, 0, 1, true);
        const int  slot = kg.atomic_add_fetch(buf, zero, one); // returns the PREVIOUS counter value
        const int  wid  = kg.cast(kg.builtin(kir::KBuiltin::WorkgroupIndex), kir::DType::U32);
        kg.stmt_buffer_store(buf, kg.binary(kir::KOp::Add, slot, one), kg.binary(kir::KOp::Add, wid, one));
        kg.stmt_buffer_store(outb, wid, slot); // the slot THIS workgroup was handed
        ke.stage             = kir::KStage::Compute;
        ke.local_size[0]     = 1U;
        ke.kernel_body_begin = 0;
        ke.kernel_body_count = static_cast<int>(kg.serial_stmts().size());
    }
    auto kernel = rig.vk->create_program(kg, ke);
    if (kernel == nullptr) { SKIP("append kernel compile unavailable"); }

    auto dst   = raster.create_color_target(64U, 64U);
    auto slots = raster.create_storage_buffer(16U * 4U);
    REQUIRE(dst != nullptr);
    REQUIRE(slots != nullptr);
    framecook::FrameGraphDesc desc(&alloc);
    containers::String        where(&alloc);
    REQUIRE(framecook::parse_frame_toml(containers::StringView(kCounterGraph), desc, &where)
            == framecook::FrameCookError::Ok);

    g::ValidationCapture capture(*rig.vk);
    StateHost            host(*dst);
    host.add_kernel("crd://kernels/append", kernel.get());
    host.set_buffer(slots.get());

    auto fgraph = raster.create_frame_graph();
    REQUIRE(fgraph != nullptr);
    framecook::FrameRecorder  rec(&alloc);
    framecook::FrameExecError err = framecook::FrameExecError::Ok;
    // ⭐⭐ THE ASSERTION IS THAT FRAME 2 LOOKS EXACTLY LIKE FRAME 1. Three workgroups append every frame, so a
    // counter that RESET reads 3 both times; one that did NOT reads 3 then 6, and the second frame's appends land
    // past element 3 — which is precisely how a GPU-driven draw starts reading garbage on frame 2 and gets worse
    // from there. Running the SAME graph twice and demanding the SAME answer is what pins the reset.
    for (crd::u32 frame = 0; frame < 2U; ++frame)
    {
        fgraph->reset();
        rec.begin_frame();
        REQUIRE(rec.record(desc, *fgraph, raster, host, &err, &where));
        CHECK(err == framecook::FrameExecError::Ok);
        REQUIRE(fgraph->build());
        fgraph->execute();
        REQUIRE(raster.download_storage(*slots));
        u32 got[3] = {slots->read_u32(0U), slots->read_u32(1U), slots->read_u32(2U)};
        // three workgroups, so the three slots handed out are 0,1,2 IN SOME ORDER (the atomic decides which
        // workgroup gets which) — what matters is the SET, and that it is identical on the second frame.
        u32 mask = 0U;
        for (u32 k = 0; k < 3U; ++k) { mask |= (got[k] < 3U) ? (1U << got[k]) : 0x80U; }
        UNSCOPED_INFO("frame " << frame << " slots: " << got[0] << " " << got[1] << " " << got[2]);
        CHECK(mask == 0x7U); // exactly {0,1,2}
    }

    if (capture.error_or_warning_count() > 0U)
    {
        const auto msgs = capture.messages();
        for (usize i = 0; i < msgs.size(); ++i)
        {
            if (msgs[i].severity == g::ValidationSeverity::Info) { continue; }
            WARN("[ren38-b3 capture] " << msgs[i].message_text.c_str());
        }
    }
    CHECK(capture.error_count() == 0U);
}

// ── REN-38-B5 GATE: an IMPORTED EXTERNAL texture — content the graph READS and never produces. ────────────────
// A UI atlas, a video frame, a captured HDR, a baked LUT. ⛔ It is deliberately NOT `import_target`: a render
// target is something a pass can WRITE, and letting a UI atlas be written would put the graph in charge of
// content the application owns and updates on its own schedule — possibly from another thread.
namespace
{
constexpr const char* kAtlasGraph = R"(
schema = 1
name   = "crd://frame/b5-atlas"

[[resource]]
name = "ui_atlas"
kind = "external_texture"

[[pass]]
name        = "compose"
kind        = "raster.fullscreen"
shader      = "crd://shaders/blit"
reads       = ["ui_atlas"]
writes      = ["@output"]
clear_color = [0.0, 0.0, 0.0, 1.0]
)";
} // namespace

TEST_CASE("REN-38-B5: an EXTERNAL TEXTURE is read-only, and settled at COOK time", "[frame-cook][ren38]")
{
    memory::TlsfAllocator alloc(1U << 20U);
    const auto            cook = [&](const char* toml) {
        framecook::FrameGraphDesc d(&alloc);
        containers::String        where(&alloc);
        return framecook::parse_frame_toml(containers::StringView(toml), d, &where);
    };
    using E = framecook::FrameCookError;
    CHECK(cook(kAtlasGraph) == E::Ok);

    // ⛔ A pass that WRITES one would have the graph schedule a barrier and a layout transition on content the
    // APPLICATION owns — so it is rejected by pass name rather than producing a frame that races with whoever
    // fills the atlas.
    constexpr const char* written_toml = R"(
schema = 1
name   = "crd://frame/b5-bad"
[[resource]]
name = "ui_atlas"
kind = "external_texture"
[[pass]]
name        = "compose"
kind        = "raster.fullscreen"
shader      = "crd://shaders/blit"
writes      = ["ui_atlas", "@output"]
clear_color = [0.0, 0.0, 0.0, 1.0]
)";
    CHECK(cook(written_toml) == E::ExternalTextureIsReadOnly);

    // ⭐ and the kind SURVIVES A ROUND TRIP — a pack that lost it would make the atlas a TRANSIENT the graph
    // allocates and clears, so the UI would composite over an empty image with nothing to say why.
    {
        framecook::FrameGraphDesc a(&alloc);
        containers::String        where(&alloc);
        REQUIRE(framecook::parse_frame_toml(containers::StringView(kAtlasGraph), a, &where) == E::Ok);
        containers::String        text = framecook::emit_frame_toml(a, &alloc);
        framecook::FrameGraphDesc b(&alloc);
        REQUIRE(framecook::parse_frame_toml(containers::StringView(text.c_str(), text.size()), b, &where) == E::Ok);
        REQUIRE(b.resources.size() == 1U);
        CHECK(b.resources[0].kind == framecook::FrameResourceKind::ExternalTexture);
    }
}

TEST_CASE("REN-38-B5 GATE: an authored EXTERNAL texture is sampled by the frame",
          "[gpu-context][vulkan][frame-graph][ren38][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;

    memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph           vg(&alloc);
    kir::KEntry           ve;
    gputest::build_textured_vs(vg, ve);
    kir::KGraph fg2(&alloc);
    kir::KEntry fe;
    gputest::build_sample_fs(fg2, fe);
    auto vs = rig.vk->create_program(vg, ve);
    auto fs = rig.vk->create_program(fg2, fe);
    if (vs == nullptr || fs == nullptr) { SKIP("shader compile unavailable"); }
    auto prog = raster.create_raster_program(*vs, *fs);
    REQUIRE(prog != nullptr);

    // an APP-OWNED texture, filled by the application — a uniform magenta "atlas"
    constexpr u32 dim = 4U;
    u32           px[dim * dim];
    for (u32 i = 0; i < dim * dim; ++i) { px[i] = 0xFFFF00FFU; } // ABGR: R=255 B=255
    auto atlas = raster.create_texture(dim, dim, static_cast<const void*>(px));
    REQUIRE(atlas != nullptr);

    auto dst = raster.create_color_target(64U, 64U);
    REQUIRE(dst != nullptr);
    framecook::FrameGraphDesc desc(&alloc);
    containers::String        where(&alloc);
    REQUIRE(framecook::parse_frame_toml(containers::StringView(kAtlasGraph), desc, &where)
            == framecook::FrameCookError::Ok);

    g::ValidationCapture capture(*rig.vk);
    StateHost            host(*dst);
    host.set_program(prog.get());
    host.set_texture(atlas.get());
    framecook::FrameExecError err = framecook::FrameExecError::Ok;
    REQUIRE(framecook::execute_frame_graph(desc, raster, host, &err, &where));
    CHECK(err == framecook::FrameExecError::Ok);

    const u32 got = dst->read_pixel(32U, 32U);
    UNSCOPED_INFO("sampled atlas = " << (got & 0xFFFFFFU));
    // ⭐ THE APPLICATION'S CONTENT REACHED THE FRAME. Magenta is chosen because it is not the clear colour and not
    // a channel a partial bind could produce by accident: R and B both high with G low means BOTH channels came
    // from the atlas, so a texture bound at the wrong slot (or not at all) cannot fake it.
    CHECK((got & 0xFFU) > 200U);          // R
    CHECK(((got >> 8U) & 0xFFU) < 60U);   // G
    CHECK(((got >> 16U) & 0xFFU) > 200U); // B

    // ⛔ NO texture from the host ⇒ a NAMED failure. A UI pass that silently sampled nothing would render a
    // transparent overlay, which looks exactly like "the UI is disabled".
    {
        StateHost blind(*dst);
        blind.set_program(prog.get());
        framecook::FrameExecError e2 = framecook::FrameExecError::Ok;
        containers::String        w2(&alloc);
        CHECK(!framecook::execute_frame_graph(desc, raster, blind, &e2, &w2));
        CHECK(e2 == framecook::FrameExecError::UnresolvedResource);
        CHECK(w2 == "ui_atlas");
    }

    if (capture.error_or_warning_count() > 0U)
    {
        const auto msgs = capture.messages();
        for (usize i = 0; i < msgs.size(); ++i)
        {
            if (msgs[i].severity == g::ValidationSeverity::Info) { continue; }
            WARN("[ren38-b5 capture] " << msgs[i].message_text.c_str());
        }
    }
    CHECK(capture.error_count() == 0U);
}

// ── REN-38-B8 GATE: AUTHORED SAMPLERS — the gap the pre-B-band audit found in no row at all. ─────────────────
// ⛔ The asset schema had ZERO mentions of `sampler`. Filtering was INFERRED from the resource FORMAT (depth ⇒
// comparison, else linear-repeat), so address mode, filter, anisotropy and mip bias were unreachable from an
// asset. ⛔⛔ THAT IS A CORRECTNESS GAP, NOT A TUNING ONE: a bloom chain sampled with REPEAT wraps bright pixels
// onto the OPPOSITE EDGE of the screen, and a tiling detail map sampled with CLAMP smears its border across the
// whole surface. One binding needs each, and until this row neither could say so.
namespace
{
// The same pass twice, differing ONLY in address mode — which is exactly the comparison the row is about.
constexpr const char* kClampGraph = R"(
schema = 1
name   = "crd://frame/b8-clamp"

[[resource]]
name = "src"
kind = "external_texture"

[[pass]]
name        = "sample"
kind        = "raster.fullscreen"
shader      = "crd://shaders/blit"
reads       = ["src"]
writes      = ["@output"]
filter      = "nearest"
address     = "clamp"
clear_color = [0.0, 0.0, 0.0, 1.0]
)";

// The SAME graph with one word changed. ⛔ A literal twin, not string surgery on the first: a gate that edits
// its own input can pass because the edit silently did nothing — which is the very failure it must detect.
constexpr const char* kRepeatGraph = R"(
schema = 1
name   = "crd://frame/b8-repeat"

[[resource]]
name = "src"
kind = "external_texture"

[[pass]]
name        = "sample"
kind        = "raster.fullscreen"
shader      = "crd://shaders/blit"
reads       = ["src"]
writes      = ["@output"]
filter      = "nearest"
address     = "repeat"
clear_color = [0.0, 0.0, 0.0, 1.0]
)";
} // namespace

TEST_CASE("REN-38-B8: an authored SAMPLER is settled at COOK time", "[frame-cook][ren38]")
{
    memory::TlsfAllocator alloc(1U << 20U);
    framecook::FrameGraphDesc a(&alloc);
    containers::String        where(&alloc);
    using E = framecook::FrameCookError;
    REQUIRE(framecook::parse_frame_toml(containers::StringView(kClampGraph), a, &where) == E::Ok);
    REQUIRE(a.passes.size() == 1U);
    CHECK(framecook::pass_flag(a.passes[0], containers::StringView(framecook::pp::kHasSampler)));
    CHECK(framecook::pass_sampler(a.passes[0]).address == g::SamplerAddress::ClampToEdge);
    CHECK(framecook::pass_sampler(a.passes[0]).mag_filter == g::SamplerFilter::Nearest);

    // ⛔ The address set is CLOSED. A typo that fell back to `repeat` would make a post-process wrap its edges —
    // visible only at the screen border, and only on content bright enough to notice.
    {
        framecook::FrameGraphDesc d(&alloc);
        CHECK(framecook::parse_frame_toml(
                  containers::StringView("schema = 1\nname = \"x\"\n[[resource]]\nname = \"s\"\n"
                                         "kind = \"external_texture\"\n[[pass]]\nname = \"p\"\n"
                                         "kind = \"raster.fullscreen\"\nshader = \"sh\"\nreads = [\"s\"]\n"
                                         "writes = [\"@output\"]\naddress = \"wrap_maybe\"\n"),
                  d, &where)
              == E::UnknownSamplerAddress);
    }

    // ⭐ ROUND TRIP. ⛔ A pack that dropped the sampler would silently restore the inferred default — the exact
    // state this row exists to replace, and with no diff to show for it.
    {
        containers::String        text = framecook::emit_frame_toml(a, &alloc);
        framecook::FrameGraphDesc b(&alloc);
        REQUIRE(framecook::parse_frame_toml(containers::StringView(text.c_str(), text.size()), b, &where) == E::Ok);
        REQUIRE(b.passes.size() == 1U);
        CHECK(framecook::pass_flag(b.passes[0], containers::StringView(framecook::pp::kHasSampler)));
        CHECK(framecook::pass_sampler(b.passes[0]).address == g::SamplerAddress::ClampToEdge);
        CHECK(framecook::pass_sampler(b.passes[0]).mag_filter == g::SamplerFilter::Nearest);
    }

    // a pass that declares NOTHING stays on the engine default — every existing asset is byte-unchanged
    {
        framecook::FrameGraphDesc d(&alloc);
        REQUIRE(framecook::parse_frame_toml(
                    containers::StringView("schema = 1\nname = \"x\"\n[[resource]]\nname = \"s\"\n"
                                           "kind = \"external_texture\"\n[[pass]]\nname = \"p\"\n"
                                           "kind = \"raster.fullscreen\"\nshader = \"sh\"\nreads = [\"s\"]\n"
                                           "writes = [\"@output\"]\n"),
                    d, &where)
                == E::Ok);
        CHECK_FALSE(framecook::pass_flag(d.passes[0], containers::StringView(framecook::pp::kHasSampler)));
    }
}

TEST_CASE("REN-38-B8 GATE: an authored CLAMP sampler changes what the frame samples",
          "[gpu-context][vulkan][frame-graph][ren38][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;

    memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph           vg(&alloc);
    kir::KEntry           ve;
    gputest::build_uv_wrap_vs(vg, ve); // a fullscreen triangle whose UVs run 0..2 — so ADDRESS MODE decides
    kir::KGraph fg2(&alloc);
    kir::KEntry fe;
    gputest::build_sample_fs(fg2, fe);
    auto vs = rig.vk->create_program(vg, ve);
    auto fs = rig.vk->create_program(fg2, fe);
    if (vs == nullptr || fs == nullptr) { SKIP("shader compile unavailable"); }
    auto prog = raster.create_raster_program(*vs, *fs);
    REQUIRE(prog != nullptr);

    // a 2x2 texture: left column RED, right column BLUE
    constexpr u32 dim = 2U;
    const u32     px[dim * dim] = {0xFF0000FFU, 0xFFFF0000U, 0xFF0000FFU, 0xFFFF0000U};
    auto          tex = raster.create_texture(dim, dim, static_cast<const void*>(px));
    REQUIRE(tex != nullptr);

    g::ValidationCapture capture(*rig.vk);
    const auto           run = [&](const char* toml) {
        auto dst = raster.create_color_target(64U, 64U);
        REQUIRE(dst != nullptr);
        framecook::FrameGraphDesc desc(&alloc);
        containers::String        where(&alloc);
        REQUIRE(framecook::parse_frame_toml(containers::StringView(toml), desc, &where)
                == framecook::FrameCookError::Ok);
        StateHost host(*dst);
        host.set_program(prog.get());
        host.set_texture(tex.get());
        framecook::FrameExecError err = framecook::FrameExecError::Ok;
        REQUIRE(framecook::execute_frame_graph(desc, raster, host, &err, &where));
        CHECK(err == framecook::FrameExecError::Ok);
        // ⛔ PIXEL 40, not 60. At x=60 the UV is ~1.875, whose REPEAT wrap (0.875) lands in the SAME right-hand
        // column clamp returns — so the two modes AGREE there and the gate would pass with the sampler ignored.
        // At x=40 the UV is ~1.25: clamp still returns the right column (BLUE) while repeat wraps to 0.25, the
        // LEFT column (RED). Choosing the sample point is the difference between a real gate and a green light.
        return dst->read_pixel(40U, 32U) & 0xFFFFFFU;
    };
    const u32 clamped  = run(kClampGraph);
    const u32 repeated = run(kRepeatGraph);

    UNSCOPED_INFO("clamped=" << clamped << " repeated=" << repeated);
    // ⭐⭐ THE TWO MUST DIFFER, and that is the whole claim. At UV > 1 a CLAMP sampler keeps returning the right
    // column (BLUE) while a REPEAT sampler wraps back to the left column (RED). ⛔ Asserting only "it rendered"
    // would pass with the sampler ignored entirely — which is exactly the state before this row, where the asset
    // could ask for clamp and silently get repeat.
    CHECK(clamped != repeated);

    if (capture.error_or_warning_count() > 0U)
    {
        const auto msgs = capture.messages();
        for (usize i = 0; i < msgs.size(); ++i)
        {
            if (msgs[i].severity == g::ValidationSeverity::Info) { continue; }
            WARN("[ren38-b8 capture] " << msgs[i].message_text.c_str());
        }
    }
    CHECK(capture.error_count() == 0U);
}

// ── REN-38-B6 GATE: ALIASING HINTS + AN EXPLICIT MEMORY BUDGET. ──────────────────────────────────────────────
// The graph aliases automatically; until this row an author could neither PIN a resource out of it nor BOUND the
// total. ⛔ Both are real: the aliaser derives lifetimes from DECLARED reads and writes, so a pass that touches a
// resource out-of-band (a debug overlay, a tool capture) gets another transient's pixels — and an unbounded
// footprint is an allocation that succeeds on the dev machine and OOMs on the target months later.
//
// ⭐⭐ AND THIS GATE GOES THROUGH THE ASSET, which is how it caught what B2's did not: B2 called
// `create_transient_image` DIRECTLY with a hand-built desc, so the asset→device shape path was never exercised
// and `dimension = "cube"` silently produced a plain 2-D image. A gate that bypasses the layer it is meant to
// prove is not a gate for that layer.
namespace
{
constexpr const char* kAliasGraph = R"(
schema = 1
name   = "crd://frame/b6-alias"

[[resource]]
name    = "a"
format  = "RGBA8Unorm"
width   = 256
height  = 256
sampled = true

[[resource]]
name    = "b"
format  = "RGBA8Unorm"
width   = 256
height  = 256
sampled = true

# ⛔ The two lifetimes must be DISJOINT for the aliaser to fold them — `a` dies the moment its reader is done,
# and only then is `b` born. A graph where a pass reads `a` and writes `b` keeps BOTH live across that pass, so
# nothing aliases and the gate would compare two identical numbers (which is exactly how the first draft failed).
[[pass]]
name        = "wa"
kind        = "clear"
writes      = ["a"]
clear_color = [1.0, 0.0, 0.0, 1.0]

[[pass]]
name   = "a_out"
kind   = "copy"
reads  = ["a"]
writes = ["@output"]

[[pass]]
name        = "wb"
kind        = "clear"
writes      = ["b"]
clear_color = [0.0, 1.0, 0.0, 1.0]

[[pass]]
name   = "b_out"
kind   = "copy"
reads  = ["b"]
writes = ["@output"]
)";
} // namespace

TEST_CASE("REN-38-B6 GATE: a PINNED transient stops aliasing, and a BUDGET fails the build",
          "[gpu-context][vulkan][frame-graph][ren38][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;

    memory::TlsfAllocator alloc(8U << 20U);
    g::ValidationCapture  capture(*rig.vk);

    // run the SAME asset three ways and compare the graph's own memory report
    const auto footprint = [&](bool pin, u64 budget, bool* built_out) {
        auto dst = raster.create_color_target(256U, 256U);
        REQUIRE(dst != nullptr);
        framecook::FrameGraphDesc desc(&alloc);
        containers::String        where(&alloc);
        REQUIRE(framecook::parse_frame_toml(containers::StringView(kAliasGraph), desc, &where)
                == framecook::FrameCookError::Ok);
        if (pin) { for (usize i = 0; i < desc.resources.size(); ++i) { desc.resources[i].no_alias = true; } }
        desc.memory_budget_bytes = budget;
        StateHost                 host(*dst);
        auto                      fgraph = raster.create_frame_graph();
        REQUIRE(fgraph != nullptr);
        framecook::FrameRecorder  rec(&alloc);
        rec.begin_frame();
        framecook::FrameExecError err = framecook::FrameExecError::Ok;
        REQUIRE(rec.record(desc, *fgraph, raster, host, &err, &where));
        const bool built = fgraph->build();
        if (built_out != nullptr) { *built_out = built; }
        if (!built) { CHECK(fgraph->last_build_exceeded_budget()); }
        return built ? fgraph->transient_memory_bytes() : 0U;
    };

    bool      built   = false;
    const u32 aliased = footprint(false, 0U, &built);
    CHECK(built);
    const u32 pinned  = footprint(true, 0U, &built);
    CHECK(built);
    UNSCOPED_INFO("aliased=" << aliased << " pinned=" << pinned);
    // ⭐ `a` and `b` have DISJOINT lifetimes (a is dead once the copy has read it), so the aliaser folds them into
    // ONE slot. Pinning both forbids that. ⛔ The claim is the DIFFERENCE — an absolute byte count would encode
    // this adapter's alignment rules, and "it built" would pass with the pin ignored entirely.
    CHECK(pinned > aliased);

    // ⛔ THE BUDGET FAILS THE BUILD, it does not warn. One byte under the aliased footprint must be rejected —
    // and rejected AS A BUDGET failure, because "too big" and "malformed" need different fixes and a bare false
    // cannot say which.
    const u32 refused = footprint(false, static_cast<u64>(aliased) - 1U, &built);
    CHECK_FALSE(built);
    CHECK(refused == 0U);
    // …and a budget the frame fits inside builds normally, so the check is a CEILING and not a switch.
    const u32 fits = footprint(false, static_cast<u64>(aliased) + 1U, &built);
    CHECK(built);
    CHECK(fits == aliased);

    if (capture.error_or_warning_count() > 0U)
    {
        const auto msgs = capture.messages();
        for (usize i = 0; i < msgs.size(); ++i)
        {
            if (msgs[i].severity == g::ValidationSeverity::Info) { continue; }
            WARN("[ren38-b6 capture] " << msgs[i].message_text.c_str());
        }
    }
    CHECK(capture.error_count() == 0U);
}

// ⭐⭐ The regression B2's gate should have had: the SHAPE must survive the ASSET → DEVICE path, not just a
// hand-built desc. A `dimension = "cube"` transient that came out 2-D is invisible to every validation layer.
TEST_CASE("REN-38-B2/B6: an authored SHAPE and ALIAS PIN reach the device through the asset",
          "[gpu-context][vulkan][frame-graph][ren38][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;

    memory::TlsfAllocator alloc(8U << 20U);
    g::ValidationCapture  capture(*rig.vk);
    constexpr const char* cube_graph_toml = R"(
schema = 1
name   = "crd://frame/b2-through-asset"

[[resource]]
name      = "env"
format    = "RGBA16F"
width     = 64
height    = 64
dimension = "cube"
mips      = 7
sampled   = true
no_alias  = true

[[pass]]
name        = "fill"
kind        = "clear"
writes      = ["env"]
clear_color = [0.0, 0.0, 0.0, 1.0]

[[pass]]
name        = "blank"
kind        = "clear"
writes      = ["@output"]
clear_color = [0.0, 0.0, 0.0, 1.0]
)";
    auto dst = raster.create_color_target(64U, 64U);
    REQUIRE(dst != nullptr);
    framecook::FrameGraphDesc desc(&alloc);
    containers::String        where(&alloc);
    REQUIRE(framecook::parse_frame_toml(containers::StringView(cube_graph_toml), desc, &where)
            == framecook::FrameCookError::Ok);
    StateHost                 host(*dst);
    framecook::FrameExecError err = framecook::FrameExecError::Ok;
    // ⭐ A CUBE is SIX faces at SEVEN mips. If the shape had not reached the device the transient would be a
    // single-mip 2-D image — roughly an eighth of the memory — so the graph's own footprint report is what
    // distinguishes them, and it is measured through the ASSET rather than a hand-built desc.
    auto fgraph = raster.create_frame_graph();
    REQUIRE(fgraph != nullptr);
    framecook::FrameRecorder rec(&alloc);
    rec.begin_frame();
    REQUIRE(rec.record(desc, *fgraph, raster, host, &err, &where));
    CHECK(err == framecook::FrameExecError::Ok);
    REQUIRE(fgraph->build());
    const u32 bytes = fgraph->transient_memory_bytes();
    // 64x64 RGBA16F = 32 KiB per face-mip0; six faces with a full mip chain is ~256 KiB. A plain 2-D single-mip
    // image would be 32 KiB, so a threshold well above that cannot be met by the wrong shape.
    UNSCOPED_INFO("cube+mips transient bytes = " << bytes);
    CHECK(bytes > 128U * 1024U);

    if (capture.error_or_warning_count() > 0U)
    {
        const auto msgs = capture.messages();
        for (usize i = 0; i < msgs.size(); ++i)
        {
            if (msgs[i].severity == g::ValidationSeverity::Info) { continue; }
            WARN("[ren38-b2asset capture] " << msgs[i].message_text.c_str());
        }
    }
    CHECK(capture.error_count() == 0U);
}

// ── ⭐⭐ REN-38 PASS-STATE GATES (the audit's vocabulary gap #5): depth-write · depth bias · face cull. ────────
// ⛔ Until this landed, depth bias was hardwired OFF and face culling hardwired NONE in both backends, and the
// depth WRITE could not be turned off — so a transparent pass could not keep the opaque depth read-only and a
// shadow pass could not bias or front-face-cull. Every gate below asserts a DIFFERENCE between two runs that
// differ only in the declared state, because "it rendered" passes with the state ignored entirely.
namespace
{
constexpr const char* kCullGraphHead = R"(
schema = 1
name   = "crd://frame/state-cull"

[[draw_list]]
name = "visible"
all  = ["MeshRenderer"]

[[pass]]
name        = "scene"
kind        = "raster.geometry"
draw_list   = "visible"
writes      = ["@output"]
clear_color = [0.0, 0.0, 0.0, 1.0]
clear_depth = 1.0
depth       = "LessEqual"
)";

class CullHost final : public framecook::IFrameGraphHost
{
public:
    CullHost(g::IRasterTarget* out, g::IRasterProgram* prog, g::IStorageBuffer* sb)
        : m_out(out), m_prog(prog), m_sb(sb)
    {
    }
    g::IRasterTarget*  output() override { return m_out; }
    g::IRasterProgram* program(containers::StringView) override { return m_prog; }
    bool               draw_list(containers::StringView, framecook::DrawListBinding& out) override
    {
        out.storage      = m_sb;
        out.program      = m_prog;
        out.vertex_count = 3U;
        return true;
    }

private:
    g::IRasterTarget*  m_out  = nullptr;
    g::IRasterProgram* m_prog = nullptr;
    g::IStorageBuffer* m_sb   = nullptr;
};
} // namespace

TEST_CASE("REN-38 STATE GATE: an authored FACE CULL reaches the rasterizer through the asset",
          "[gpu-context][vulkan][frame-graph][ren38][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;

    memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph           vg(&alloc);
    kir::KEntry           ve;
    gputest::build_triangle_vs(vg, ve);
    kir::KGraph fg2(&alloc);
    kir::KEntry fe;
    gputest::build_triangle_fs(fg2, fe);
    auto vs = rig.vk->create_program(vg, ve);
    auto fs = rig.vk->create_program(fg2, fe);
    if (vs == nullptr || fs == nullptr) { SKIP("shader compile unavailable"); }
    auto prog = raster.create_raster_program(*vs, *fs);
    REQUIRE(prog != nullptr);

    g::ValidationCapture capture(*rig.vk);
    const auto           run = [&](const char* cull_line) {
        auto sb = raster.create_storage_buffer(16U);
        REQUIRE(sb != nullptr);
        auto dst = raster.create_color_depth_target(64U, 64U);
        REQUIRE(dst != nullptr);
        containers::String toml(&alloc);
        toml.append(kCullGraphHead);
        toml.append(cull_line);
        framecook::FrameGraphDesc desc(&alloc);
        containers::String        where(&alloc);
        REQUIRE(framecook::parse_frame_toml(containers::StringView(toml.c_str(), toml.size()), desc, &where)
                == framecook::FrameCookError::Ok);
        CullHost                  host(dst.get(), prog.get(), sb.get());
        framecook::FrameExecError err = framecook::FrameExecError::Ok;
        REQUIRE(framecook::execute_frame_graph(desc, raster, host, &err, &where));
        CHECK(err == framecook::FrameExecError::Ok);
        return dst->read_pixel(32U, 32U) & 0xFFFFFFU;
    };

    const u32 none_px  = run("");
    const u32 back_px  = run("face_cull = \"back\"\n");
    const u32 front_px = run("face_cull = \"front\"\n");
    UNSCOPED_INFO("none=" << none_px << " back=" << back_px << " front=" << front_px);
    // Under `none` the triangle covers the centre (the historical behaviour, byte-unchanged).
    CHECK((none_px & 0xFFU) > 200U);
    // EXACTLY ONE of back/front erases it. The gate does not encode the triangle's winding — it asserts the
    // DICHOTOMY, which is precisely "the declared state reached the rasterizer": a state that never arrived
    // leaves both runs identical to `none`, and a state applied to the wrong face kills the wrong one only.
    const bool back_kills  = (back_px & 0xFFFFFFU) == 0U;
    const bool front_kills = (front_px & 0xFFFFFFU) == 0U;
    CHECK(back_kills != front_kills);
    CHECK(capture.error_count() == 0U);
}

TEST_CASE("REN-38 STATE GATE: depth_write = false leaves the depth buffer UNTOUCHED",
          "[gpu-context][vulkan][frame-graph][ren38][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;

    memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph           vg(&alloc);
    kir::KEntry           ve;
    gputest::build_triangle_z_vs(vg, ve, 0.5);
    kir::KGraph rg2(&alloc);
    kir::KEntry rfe;
    gputest::build_triangle_fs(rg2, rfe); // red
    kir::KGraph gg(&alloc);
    kir::KEntry gfe;
    gputest::build_solid_fs(gg, gfe, 0.0, 1.0, 0.0); // the GREEN probe — only it can paint green
    auto vs   = rig.vk->create_program(vg, ve);
    auto fsr  = rig.vk->create_program(rg2, rfe);
    auto fsg  = rig.vk->create_program(gg, gfe);
    if (vs == nullptr || fsr == nullptr || fsg == nullptr) { SKIP("shader compile unavailable"); }
    auto red   = raster.create_raster_program(*vs, *fsr);
    auto green = raster.create_raster_program(*vs, *fsg);
    REQUIRE(red != nullptr);
    REQUIRE(green != nullptr);

    g::ValidationCapture capture(*rig.vk);
    const auto           run = [&](bool depth_write) {
        auto sb = raster.create_storage_buffer(16U);
        REQUIRE(sb != nullptr);
        auto dst = raster.create_color_depth_target(64U, 64U);
        REQUIRE(dst != nullptr);
        g::PassRasterState st{};
        st.depth_write = depth_write;
        raster.set_pass_state(st);
        crd::gputest::enc_draw_storage_depth(raster, *dst, *red, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 1.0F,
                                  g::DepthCompare::LessEqual, *sb, 3U);
        raster.set_pass_state(g::PassRasterState{});
        // The GREEN probe re-draws the same triangle with compare NOT-EQUAL to its own depth: it can only pass
        // where the first draw did NOT store 0.5 — i.e. exactly when depth_write was declared off.
        crd::gputest::enc_draw_storage_depth_load(raster, *dst, *green, g::DepthCompare::NotEqual, *sb, 3U);
        return dst->read_pixel(32U, 32U) & 0xFFFFFFU;
    };
    const u32 wrote = run(true);
    const u32 kept  = run(false);
    UNSCOPED_INFO("write_on=" << wrote << " write_off=" << kept);
    CHECK((wrote & 0xFFU) > 200U);        // write ON: stored == probe z, NotEqual fails, RED survives
    CHECK(((kept >> 8U) & 0xFFU) > 200U); // write OFF: depth still clear, probe passes, GREEN wins
    CHECK(capture.error_count() == 0U);
}

TEST_CASE("REN-38 STATE GATE: a declared DEPTH BIAS moves what the depth buffer stores",
          "[gpu-context][vulkan][frame-graph][ren38][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;

    memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph           vg(&alloc);
    kir::KEntry           ve;
    gputest::build_triangle_z_vs(vg, ve, 0.5); // z = 0.5: the exponent where a device-unit bias is usable
    kir::KGraph rg2(&alloc);
    kir::KEntry rfe;
    gputest::build_triangle_fs(rg2, rfe);
    kir::KGraph gg(&alloc);
    kir::KEntry gfe;
    gputest::build_solid_fs(gg, gfe, 0.0, 1.0, 0.0);
    auto vs  = rig.vk->create_program(vg, ve);
    auto fsr = rig.vk->create_program(rg2, rfe);
    auto fsg = rig.vk->create_program(gg, gfe);
    if (vs == nullptr || fsr == nullptr || fsg == nullptr) { SKIP("shader compile unavailable"); }
    auto red   = raster.create_raster_program(*vs, *fsr);
    auto green = raster.create_raster_program(*vs, *fsg);
    REQUIRE(red != nullptr);
    REQUIRE(green != nullptr);

    g::ValidationCapture capture(*rig.vk);
    const auto           run = [&](float bias) {
        auto sb = raster.create_storage_buffer(16U);
        REQUIRE(sb != nullptr);
        auto dst = raster.create_color_depth_target(64U, 64U);
        REQUIRE(dst != nullptr);
        g::PassRasterState st{};
        st.depth_bias = bias;
        raster.set_pass_state(st);
        crd::gputest::enc_draw_storage_depth(raster, *dst, *red, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 1.0F,
                                  g::DepthCompare::LessEqual, *sb, 3U);
        raster.set_pass_state(g::PassRasterState{});
        // The probe passes NOT-EQUAL only where the stored depth is not the unbiased 0.5 — i.e. exactly when
        // the bias actually moved what the first draw stored.
        crd::gputest::enc_draw_storage_depth_load(raster, *dst, *green, g::DepthCompare::NotEqual, *sb, 3U);
        return dst->read_pixel(32U, 32U) & 0xFFFFFFU;
    };
    const u32 unbiased = run(0.0F);
    const u32 biased   = run(16777216.0F); // 2^24 device units at exponent(0.5) — an unmissable shift
    UNSCOPED_INFO("unbiased=" << unbiased << " biased=" << biased);
    CHECK((unbiased & 0xFFU) > 200U);       // no bias: stored == 0.5, probe fails, RED survives
    CHECK(((biased >> 8U) & 0xFFU) > 200U); // biased: stored != 0.5, probe passes, GREEN wins
    CHECK(capture.error_count() == 0U);
}

// ── ⭐⭐ REN-38 RT GATE (the audit's stage gap): the AUTHORED ANY-HIT joins the hit group. ────────────────────
// ⛔ Until this landed the pipeline vocabulary named exactly raygen/miss/closest_hit, so alpha-tested RT
// geometry — a chain-link fence, a leaf card — shadowed as a SOLID PLATE: there was no stage that could ignore
// a candidate. The gate's claim is the strongest distinguishable one: an any-hit whose cutoff ignores EVERY
// candidate turns rays that HIT into rays that MISS, and one whose cutoff ignores none leaves the hits alone.
namespace
{
constexpr const char* kRtAnyHitGraph = R"(
schema = 1
name   = "crd://frame/ren38-rt-anyhit"

[[resource]]
name = "scene_tlas"
kind = "acceleration_structure"

[[resource]]
name = "hits"
kind = "external_buffer"

[[pass]]
name        = "trace"
kind        = "raytrace.pipeline"
raygen      = "crd://rt/raygen"
miss        = "crd://rt/miss"
closest_hit = "crd://rt/chit"
any_hit     = "crd://rt/anyhit"
reads       = ["scene_tlas"]
writes      = ["hits"]
params      = { groups_x = 4, groups_y = 1 }

[[pass]]
name        = "blank"
kind        = "clear"
writes      = ["@output"]
clear_color = [0.0, 0.0, 0.0, 1.0]
)";
} // namespace

TEST_CASE("REN-38 RT GATE: an authored ANY-HIT joins the hit group and can IGNORE every hit",
          "[gpu-context][vulkan][frame-graph][ren38][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;
    if (!raster.supports_rt_pipeline()) { SKIP("adapter has no ray-tracing pipeline"); }

    g::VulkanRayTracingContext rt(*rig.vk);
    REQUIRE(rt.valid());
    memory::TlsfAllocator alloc(16U << 20U);

    const float tri[9] = {-1.0F, -1.0F, 1.0F, 1.0F, -1.0F, 1.0F, 0.0F, 1.0F, 1.0F};
    // ⛔ NON-OPAQUE geometry, or the gate proves nothing: traversal SKIPS the any-hit stage entirely for
    // geometry flagged OPAQUE (that is the flag's whole meaning), and `build_scene` builds opaque.
    const float identity[12] = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
    auto scene = rt.build_scene_instanced(tri, 1U, identity, 1U, /*opaque=*/false);
    REQUIRE(scene != nullptr);

    kir::KGraph rgg(&alloc);
    kir::KEntry rge;
    kir::KGraph msg(&alloc);
    kir::KEntry mse;
    kir::KGraph chg(&alloc);
    kir::KEntry che;
    gputest::build_rt_pipeline_trio(rgg, rge, msg, mse, chg, che);
    auto rgp = rig.vk->create_program(rgg, rge);
    auto msp = rig.vk->create_program(msg, mse);
    auto chp = rig.vk->create_program(chg, che);
    if (rgp == nullptr || msp == nullptr || chp == nullptr) { SKIP("RT-stage compile unavailable"); }

    g::ValidationCapture capture(*rig.vk);
    const auto           run = [&](double cutoff, float out[4]) {
        kir::KGraph ahg(&alloc);
        kir::KEntry ahe;
        gputest::build_rt_anyhit(ahg, ahe, cutoff);
        auto ahp = rig.vk->create_program(ahg, ahe);
        REQUIRE(ahp != nullptr);

        constexpr u32 ray_count = 4U;
        auto          hits  = raster.create_storage_buffer(ray_count * 4U);
        REQUIRE(hits != nullptr);
        const float sentinel[ray_count] = {-7.0F, -7.0F, -7.0F, -7.0F};
        REQUIRE(raster.upload_storage(*hits, 0U, static_cast<const void*>(sentinel), sizeof(sentinel)));
        auto dst = raster.create_color_target(64U, 64U);
        REQUIRE(dst != nullptr);

        framecook::FrameGraphDesc desc(&alloc);
        containers::String        where(&alloc);
        REQUIRE(framecook::parse_frame_toml(containers::StringView(kRtAnyHitGraph), desc, &where)
                == framecook::FrameCookError::Ok);
        StateHost host(*dst);
        host.add_kernel("crd://rt/raygen", rgp.get());
        host.add_kernel("crd://rt/miss", msp.get());
        host.add_kernel("crd://rt/chit", chp.get());
        host.add_kernel("crd://rt/anyhit", ahp.get());
        host.set_buffer(hits.get());
        host.set_accel(scene.get());

        auto fgraph = raster.create_frame_graph();
        REQUIRE(fgraph != nullptr);
        framecook::FrameRecorder rec(&alloc);
        rec.begin_frame();
        framecook::FrameExecError err = framecook::FrameExecError::Ok;
        REQUIRE(rec.record(desc, *fgraph, raster, host, &err, &where));
        CHECK(err == framecook::FrameExecError::Ok);
        REQUIRE(fgraph->build());
        fgraph->execute();
        REQUIRE(raster.download_storage(*hits));
        for (u32 i = 0; i < ray_count; ++i)
        {
            const u32 bits = hits->read_u32(i);
            std::memcpy(&out[i], &bits, sizeof(float));
        }
    };

    // cutoff 0: the any-hit is PRESENT and ignores nothing — the A16 behaviour must be untouched.
    float keep[4] = {};
    run(0.0, keep);
    UNSCOPED_INFO("anyhit keep: " << keep[0] << " " << keep[1] << " " << keep[2] << " " << keep[3]);
    CHECK(keep[0] > 0.5F);
    CHECK(keep[1] > 0.5F);
    CHECK(keep[2] < -0.5F);
    CHECK(keep[3] < -0.5F);

    // ⭐⭐ cutoff 2: EVERY candidate is ignored (u+v <= 1 inside a triangle), so the rays that hit above must
    // now MISS. An any-hit that never reached the pipeline leaves rays 0-1 at +1 — exactly what this catches.
    float ignore_all[4] = {};
    run(2.0, ignore_all);
    UNSCOPED_INFO("anyhit ignore: " << ignore_all[0] << " " << ignore_all[1] << " " << ignore_all[2] << " "
                                    << ignore_all[3]);
    CHECK(ignore_all[0] < -0.5F);
    CHECK(ignore_all[1] < -0.5F);
    CHECK(ignore_all[2] < -0.5F);
    CHECK(ignore_all[3] < -0.5F);

    if (capture.error_or_warning_count() > 0U)
    {
        const auto msgs = capture.messages();
        for (usize i = 0; i < msgs.size(); ++i)
        {
            if (msgs[i].severity == g::ValidationSeverity::Info) { continue; }
            WARN("[ren38-anyhit capture] " << msgs[i].message_text.c_str());
        }
    }
    CHECK(capture.error_count() == 0U);
}

// ── ⭐⭐ REN-38-F11 GATE: the STENCIL ATTACHMENT — the F10 vocabulary finally DRAWS. ─────────────────────────
// F10 declared, cooked, blob-carried and installed every stencil op, and no target carried the stencil ASPECT,
// so the whole axis could configure but never affect a pixel. This is the canonical stencil use, authored: a
// MASK pass stamps ref=1 where its geometry lands, a `load = true` COVER pass (the flag F11 added — without it
// the second pass re-cleared colour, depth AND stencil) draws fullscreen with `stencil_compare = "Equal"`, so
// its colour lands ONLY inside the mask. A control run without the stencil lines covers everything — the
// dichotomy is "the attachment exists and the declared ops reach it".
namespace
{
constexpr const char* kStencilGraphA = R"(
schema = 1
name   = "crd://frame/f11-stencil"

[[draw_list]]
name = "mask_geo"

[[draw_list]]
name = "cover_geo"

[[pass]]
name            = "mask"
kind            = "raster.geometry"
draw_list       = "mask_geo"
writes          = ["@output"]
clear_color     = [0.0, 0.0, 0.0, 1.0]
stencil         = true
stencil_compare = "Always"
stencil_ref     = 1
stencil_pass    = "replace"

[[pass]]
name            = "cover"
kind            = "raster.geometry"
draw_list       = "cover_geo"
writes          = ["@output"]
load            = true
depth           = "Always"
)";

constexpr const char* kStencilCoverOn = R"(stencil         = true
stencil_compare = "Equal"
stencil_ref     = 1
)";

// Two draw lists, two programs: the mask triangle and the fullscreen cover.
class StencilHost final : public framecook::IFrameGraphHost
{
public:
    StencilHost(g::IRasterTarget* out, g::IRasterProgram* mask, g::IRasterProgram* cover, g::IStorageBuffer* sb)
        : m_out(out), m_mask(mask), m_cover(cover), m_sb(sb)
    {
    }
    [[nodiscard]] g::IRasterTarget*  output() override { return m_out; }
    [[nodiscard]] g::IRasterProgram* program(containers::StringView) override { return nullptr; }
    [[nodiscard]] bool draw_list(containers::StringView name, framecook::DrawListBinding& out) override
    {
        const bool mask = name == containers::StringView("mask_geo");
        out.items[0]    = framecook::DrawItem{m_sb, mask ? m_mask : m_cover, mask ? 3U : 6U, nullptr};
        out.resolved    = 1U;
        return true;
    }

private:
    g::IRasterTarget*  m_out   = nullptr;
    g::IRasterProgram* m_mask  = nullptr;
    g::IRasterProgram* m_cover = nullptr;
    g::IStorageBuffer* m_sb    = nullptr;
};
} // namespace

TEST_CASE("REN-38-F11 GATE: an authored stencil MASK-then-TEST pass pair draws through a D24S8 target",
          "[gpu-context][vulkan][frame-graph][ren38][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;

    memory::TlsfAllocator alloc(8U << 20U);
    // the MASK: the standard red triangle (covers the centre, misses the corners)
    kir::KGraph mvg(&alloc);
    kir::KEntry mve;
    gputest::build_triangle_vs(mvg, mve);
    kir::KGraph mfg(&alloc);
    kir::KEntry mfe;
    gputest::build_triangle_fs(mfg, mfe);
    // the COVER: a fullscreen pair in solid GREEN
    kir::KGraph cvg(&alloc);
    kir::KEntry cve;
    gputest::build_visbuffer_vs(cvg, cve);
    kir::KGraph cfg2(&alloc);
    kir::KEntry cfe;
    gputest::build_solid_fs(cfg2, cfe, 0.0, 1.0, 0.0);
    auto mvs = rig.vk->create_program(mvg, mve);
    auto mfs = rig.vk->create_program(mfg, mfe);
    auto cvs = rig.vk->create_program(cvg, cve);
    auto cfs = rig.vk->create_program(cfg2, cfe);
    if (mvs == nullptr || mfs == nullptr || cvs == nullptr || cfs == nullptr) { SKIP("shader compile unavailable"); }
    auto mask_prog  = raster.create_raster_program(*mvs, *mfs);
    auto cover_prog = raster.create_raster_program(*cvs, *cfs);
    REQUIRE(mask_prog != nullptr);
    REQUIRE(cover_prog != nullptr);

    g::ValidationCapture capture(*rig.vk);
    const auto           run = [&](bool stencil_on, u32& centre, u32& corner) {
        auto dst = raster.create_color_depth_stencil_target(64U, 64U);
        REQUIRE(dst != nullptr); // the F11 verb — a D24S8 target
        auto sb = raster.create_storage_buffer(16U);
        REQUIRE(sb != nullptr);
        containers::String toml(&alloc);
        toml.append(kStencilGraphA);
        if (stencil_on) { toml.append(kStencilCoverOn); }
        framecook::FrameGraphDesc desc(&alloc);
        containers::String        where(&alloc);
        REQUIRE(framecook::parse_frame_toml(containers::StringView(toml.c_str(), toml.size()), desc, &where)
                == framecook::FrameCookError::Ok);
        StencilHost               host(dst.get(), mask_prog.get(), cover_prog.get(), sb.get());
        framecook::FrameExecError err = framecook::FrameExecError::Ok;
        REQUIRE(framecook::execute_frame_graph(desc, raster, host, &err, &where));
        CHECK(err == framecook::FrameExecError::Ok);
        centre = dst->read_pixel(32U, 32U) & 0xFFFFFFU;
        corner = dst->read_pixel(2U, 2U) & 0xFFFFFFU;
    };

    u32 c_on  = 0;
    u32 k_on  = 0;
    u32 c_off = 0;
    u32 k_off = 0;
    run(true, c_on, k_on);
    run(false, c_off, k_off);
    UNSCOPED_INFO("stencil: centre=" << c_on << " corner=" << k_on
                                     << " | control: centre=" << c_off << " corner=" << k_off);
    const auto green = [](u32 px) { return ((px >> 8U) & 0xFFU) > 200U && (px & 0xFFU) < 50U; };
    // stencil ON: the cover lands ONLY inside the mask — centre green, corner still the mask pass's clear black
    CHECK(green(c_on));
    CHECK((k_on & 0xFFFFFFU) == 0U);
    // control (no stencil test): the fullscreen cover paints everything — INCLUDING the corner. If the corner
    // stayed black here too, the "mask" above would be depth or accident, not stencil.
    CHECK(green(c_off));
    CHECK(green(k_off));
    CHECK(capture.error_count() == 0U);
}

// ── ⭐⭐ REN-38-F13 GATE: INTERSECTION + CALLABLE — the last two CKIR stages reach the device, AUTHORED. ──────
// The hit group goes PROCEDURAL: the BLAS holds only an AABB, and the authored `.crdv` intersection stage's
// sphere quadratic IS the geometry — a hit can only exist if that stage ran. The raygen routes its traced
// payload THROUGH the SBT's callable table (`use_callable`), whose authored transform (×2 + 1) is what lands in
// the output — so the numbers prove BOTH stages dispatched: hit payload 1.0 → 3.0 with the callable, 1.0
// without. Five stages, five declarations, zero C++ shader builders.
namespace
{
constexpr const char* kF13Graph = R"(
schema = 1
name   = "crd://frame/f13-full"

[[resource]]
name = "scene_tlas"
kind = "acceleration_structure"

[[resource]]
name = "hits"
kind = "external_buffer"

[[pass]]
name         = "trace"
kind         = "raytrace.pipeline"
raygen       = "crd://f13/raygen"
miss         = "crd://f13/miss"
closest_hit  = "crd://f13/chit"
intersection = "crd://f13/isect"
callable     = "crd://f13/call"
reads        = ["scene_tlas"]
writes       = ["hits"]
params       = { groups_x = 4, groups_y = 1 }

[[pass]]
name        = "blank"
kind        = "clear"
writes      = ["@output"]
clear_color = [0.0, 0.0, 0.0, 1.0]
)";

// the SAME graph without the callable — the control (payload lands untransformed)
constexpr const char* kF13GraphNoCall = R"(
schema = 1
name   = "crd://frame/f13-nocall"

[[resource]]
name = "scene_tlas"
kind = "acceleration_structure"

[[resource]]
name = "hits"
kind = "external_buffer"

[[pass]]
name         = "trace"
kind         = "raytrace.pipeline"
raygen       = "crd://f13/raygen_plain"
miss         = "crd://f13/miss"
closest_hit  = "crd://f13/chit"
intersection = "crd://f13/isect"
reads        = ["scene_tlas"]
writes       = ["hits"]
params       = { groups_x = 4, groups_y = 1 }

[[pass]]
name        = "blank"
kind        = "clear"
writes      = ["@output"]
clear_color = [0.0, 0.0, 0.0, 1.0]
)";

class F13Host final : public framecook::IFrameGraphHost
{
public:
    F13Host(g::IRasterTarget* out) : m_out(out) {}
    [[nodiscard]] g::IRasterTarget*  output() override { return m_out; }
    [[nodiscard]] g::IRasterProgram* program(containers::StringView) override { return nullptr; }
    [[nodiscard]] bool draw_list(containers::StringView, framecook::DrawListBinding&) override { return false; }
    [[nodiscard]] g::IGpuProgram* kernel(containers::StringView id) override
    {
        for (u32 i = 0; i < m_n; ++i)
        {
            if (id == containers::StringView(m_names[i])) { return m_kernels[i]; }
        }
        return nullptr;
    }
    [[nodiscard]] g::IAccelerationStructure* acceleration_structure(containers::StringView) override { return m_as; }
    [[nodiscard]] g::IStorageBuffer*         storage_buffer(containers::StringView) override { return m_buf; }
    void set_accel(g::IAccelerationStructure* a) { m_as = a; }
    void set_buffer(g::IStorageBuffer* b) { m_buf = b; }
    void add_kernel(const char* id, g::IGpuProgram* k)
    {
        m_names[m_n]   = id;
        m_kernels[m_n] = k;
        ++m_n;
    }

private:
    g::IRasterTarget*          m_out = nullptr;
    g::IStorageBuffer*         m_buf = nullptr;
    g::IAccelerationStructure* m_as  = nullptr;
    const char*                m_names[6]{};
    g::IGpuProgram*            m_kernels[6]{};
    u32                        m_n = 0U;
};
} // namespace

TEST_CASE("REN-38-F13 GATE: authored INTERSECTION + CALLABLE stages trace a procedural sphere through the SBT",
          "[gpu-context][vulkan][frame-graph][ren38][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;
    if (!raster.supports_rt_pipeline()) { SKIP("adapter has no ray-tracing pipeline"); }

    g::VulkanRayTracingContext rt(*rig.vk);
    REQUIRE(rt.valid());
    memory::TlsfAllocator alloc(32U << 20U);

    // ONE degenerate curve segment at the origin, radius 0.5 → one AABB [-0.5-eps, +0.5+eps]^3. The AABB only
    // BOUNDS the shape; the authored intersection stage's sphere is the geometry.
    const float seg[8] = {0.0F, 0.0F, 0.0F, 0.5F, 0.0F, 0.0F, 0.0F, 0.5F};
    auto        scene  = rt.build_scene_curves(seg, 1U);
    REQUIRE(scene != nullptr);

    // the five authored stages (+ the plain-raygen control), cooked from declarations
    const auto cook_stage = [&](const char* stage_line, const char* extra) -> std::unique_ptr<g::IGpuProgram> {
        containers::String t(&alloc);
        t.append("schema = 1\nname = \"crd://vertex/f13\"\n");
        t.append(stage_line);
        t.append("\n[rt]\npayload_words = 2\nas_binding = 0\nout_binding = 1\n");
        t.append(extra);
        crd::vertcook::VertexProgramDesc d(&alloc);
        containers::String               where(&alloc);
        REQUIRE(crd::vertcook::parse_vertex_toml(containers::StringView(t.c_str(), t.size()), d, &where)
                == crd::vertcook::VertexCookError::Ok);
        kir::KGraph g2(&alloc);
        kir::KEntry e;
        REQUIRE(crd::vertcook::cook_vertex_program(d, g2, e));
        return rig.vk->create_program(g2, e);
    };
    auto rg  = cook_stage("stage = \"raygen\"\n", "use_callable = true\n");
    auto rgp = cook_stage("stage = \"raygen\"\n", "");
    auto ms  = cook_stage("stage = \"miss\"\n", "");
    auto ch  = cook_stage("stage = \"closest_hit\"\n", "");
    auto is  = cook_stage("stage = \"intersection\"\n", "sphere_radius = 0.5\n");
    auto cl  = cook_stage("stage = \"callable\"\n", "callable_scale = 2.0\ncallable_bias = 1.0\n");
    if (rg == nullptr || rgp == nullptr || ms == nullptr || ch == nullptr || is == nullptr || cl == nullptr)
    {
        SKIP("RT-stage compile unavailable");
    }

    g::ValidationCapture capture(*rig.vk);
    const auto           run = [&](const char* graph_toml, bool with_call, float out[4]) {
        constexpr u32 ray_count = 4U;
        auto          hits      = raster.create_storage_buffer(ray_count * 4U);
        REQUIRE(hits != nullptr);
        const float sentinel[ray_count] = {-7.0F, -7.0F, -7.0F, -7.0F};
        REQUIRE(raster.upload_storage(*hits, 0U, static_cast<const void*>(sentinel), sizeof(sentinel)));
        auto dst = raster.create_color_target(16U, 16U);
        REQUIRE(dst != nullptr);
        framecook::FrameGraphDesc desc(&alloc);
        containers::String        where(&alloc);
        REQUIRE(framecook::parse_frame_toml(containers::StringView(graph_toml), desc, &where)
                == framecook::FrameCookError::Ok);
        F13Host host(dst.get());
        host.add_kernel("crd://f13/raygen", rg.get());
        host.add_kernel("crd://f13/raygen_plain", rgp.get());
        host.add_kernel("crd://f13/miss", ms.get());
        host.add_kernel("crd://f13/chit", ch.get());
        host.add_kernel("crd://f13/isect", is.get());
        if (with_call) { host.add_kernel("crd://f13/call", cl.get()); }
        host.set_accel(scene.get());
        host.set_buffer(hits.get());
        framecook::FrameExecError err = framecook::FrameExecError::Ok;
        REQUIRE(framecook::execute_frame_graph(desc, raster, host, &err, &where));
        CHECK(err == framecook::FrameExecError::Ok);
        REQUIRE(raster.download_storage(*hits));
        for (u32 i = 0; i < ray_count; ++i)
        {
            const u32 bits = hits->read_u32(i);
            std::memcpy(&out[i], &bits, 4U);
        }
    };

    // every ray starts at the origin INSIDE the sphere and exits through it: the intersection stage reports the
    // far root, the closest-hit writes payload 1.0, and the callable transforms it to 1.0*2 + 1 = 3.0
    float with_call[4]{};
    float no_call[4]{};
    run(kF13Graph, true, with_call);
    run(kF13GraphNoCall, false, no_call);
    UNSCOPED_INFO("with callable: " << with_call[0] << " " << with_call[1] << " " << with_call[2] << " "
                                    << with_call[3]);
    UNSCOPED_INFO("without:      " << no_call[0] << " " << no_call[1] << " " << no_call[2] << " " << no_call[3]);
    for (u32 i = 0; i < 4U; ++i)
    {
        CHECK(with_call[i] == 3.0F); // hit (intersection ran) AND transformed (callable ran)
        CHECK(no_call[i] == 1.0F);   // hit, untransformed — the dichotomy pins the callable specifically
    }
    CHECK(capture.error_count() == 0U);
}

// ── ⭐⭐ REN-38-F6+ GATE: STORAGE-BOUND TESSELLATION — real control points, PULLED. ──────────────────────────
// `draw_tess` bound nothing, so the tessellation VS could only ever be a procedural corner table. This is the
// missing half: an AUTHORED pull `.crdv` fetches the patch's control points from the draw item's storage buffer
// (the GEO-1 seam), the authored hull/domain displace them, and the CLAIM is that the BUFFER drives the pixels —
// re-upload smaller corners and the previously-lit displacement territory goes dark.
namespace
{
constexpr const char* kTessPullGraph = R"(
schema = 1
name   = "crd://frame/f6-tess-pull"

[[draw_list]]
name = "patches"

[[pass]]
name        = "displace"
kind        = "raster.tess"
shader      = "crd://shaders/tess_pull"
draw_list   = "patches"
writes      = ["@output"]
clear_color = [0.0, 0.0, 0.0, 1.0]
)";

// the scene pull contract, minimal: one patch of 4 control points, identity view_proj, identity instance
constexpr const char* kTessPullVs = R"(
schema = 1
name   = "crd://vertex/tess_pull"

[header]
index_count  = 0
index_off    = 2
vertex_off   = 3
instance_off = 4
visible_off  = 5
view_proj    = 6

[vertex]
stride = 3

[[attribute]]
name   = "position"
offset = 0
comps  = 3
kind   = "position"

[instance]
stride    = 20
transform = 0
)";

class TessPullHost final : public framecook::IFrameGraphHost
{
public:
    TessPullHost(g::IRasterTarget* out, g::IRasterProgram* prog, g::IStorageBuffer* sb)
        : m_out(out), m_prog(prog), m_sb(sb)
    {
    }
    [[nodiscard]] g::IRasterTarget*  output() override { return m_out; }
    [[nodiscard]] g::IRasterProgram* program(containers::StringView) override { return m_prog; }
    [[nodiscard]] bool draw_list(containers::StringView, framecook::DrawListBinding& out) override
    {
        out.items[0] = framecook::DrawItem{m_sb, m_prog, 1U, nullptr}; // ONE quad patch, pulled
        out.resolved = 1U;
        return true;
    }

private:
    g::IRasterTarget*  m_out  = nullptr;
    g::IRasterProgram* m_prog = nullptr;
    g::IStorageBuffer* m_sb   = nullptr;
};

// the pull buffer: header words + indices + 4 corner vertices at (+-h, +-h, 0) + identity instance + slot list
void fill_tess_pull_buffer(containers::Array<u32>& w, float h)
{
    w.clear();
    w.resize(140U);
    for (usize i = 0; i < w.size(); ++i) { w[i] = 0U; }
    const auto fbits = [](float f) { u32 u = 0; std::memcpy(&u, &f, 4U); return u; };
    w[0] = 4U;   // index COUNT (per instance)
    w[2] = 32U;  // indices at word 32
    w[3] = 36U;  // vertices at word 36
    w[4] = 100U; // instance record at word 100
    w[5] = 120U; // visible slot list at word 120
    for (u32 c = 0; c < 4U; ++c) { w[6U + c * 4U + c] = fbits(1.0F); } // identity view_proj (column-major)
    for (u32 i = 0; i < 4U; ++i) { w[32U + i] = i; }                    // identity index list
    const float cx[4] = {-h, h, h, -h};
    const float cy[4] = {-h, -h, h, h};
    for (u32 i = 0; i < 4U; ++i)
    {
        w[36U + i * 3U + 0U] = fbits(cx[i]);
        w[36U + i * 3U + 1U] = fbits(cy[i]);
        w[36U + i * 3U + 2U] = fbits(0.0F);
    }
    for (u32 c = 0; c < 4U; ++c) { w[100U + c * 4U + c] = fbits(1.0F); } // identity instance matrix
    w[120] = 0U;                                                         // visible slot 0
}
} // namespace

TEST_CASE("REN-38-F6+ GATE: storage-bound tessellation PULLS its control points from the draw item's buffer",
          "[gpu-context][vulkan][frame-graph][ren38][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;

    memory::TlsfAllocator alloc(16U << 20U);
    const auto            cook_vc = [&](const char* toml) -> std::unique_ptr<g::IGpuProgram> {
        crd::vertcook::VertexProgramDesc d(&alloc);
        containers::String               where(&alloc);
        REQUIRE(crd::vertcook::parse_vertex_toml(containers::StringView(toml), d, &where)
                == crd::vertcook::VertexCookError::Ok);
        kir::KGraph g2(&alloc);
        kir::KEntry e;
        REQUIRE(crd::vertcook::cook_vertex_program(d, g2, e));
        return rig.vk->create_program(g2, e);
    };
    auto vs = cook_vc(kTessPullVs);
    auto hs = cook_vc("stage = \"tess_control\"\nschema = 1\nname = \"h\"\n\n[tess]\npatch_size = 4\ninner = "
                      "8.0\nouter = 8.0\n");
    auto ds = cook_vc("stage = \"tess_eval\"\nschema = 1\nname = \"d\"\ndisplace = \"expand\"\n\n[vertex]\nstride "
                      "= 3\n\n[[attribute]]\nname = \"position\"\noffset = 0\ncomps = 3\nkind = "
                      "\"position\"\n\n[tess]\npatch_size = 4\ninner = 8.0\nouter = 8.0\n\n[[node]]\nname = "
                      "\"expand\"\nop = \"multiply\"\ninputs = [\"@position\", [1.3, 1.3, 1.0]]\n");
    kir::KGraph fg2(&alloc);
    kir::KEntry fe;
    gputest::build_solid_fs(fg2, fe, 1.0, 0.3, 0.1);
    auto fs = rig.vk->create_program(fg2, fe);
    if (vs == nullptr || hs == nullptr || ds == nullptr || fs == nullptr) { SKIP("shader compile unavailable"); }
    auto prog = raster.create_tess_program(*vs, *hs, *ds, *fs);
    REQUIRE(prog != nullptr);

    g::ValidationCapture capture(*rig.vk);
    const auto           run = [&](float h) {
        auto dst = raster.create_color_target(128U, 128U);
        REQUIRE(dst != nullptr);
        containers::Array<u32> words(&alloc);
        fill_tess_pull_buffer(words, h);
        auto sb = raster.create_storage_buffer(static_cast<u32>(words.size() * 4U));
        REQUIRE(sb != nullptr);
        REQUIRE(raster.upload_storage(*sb, 0U, words.data(), static_cast<u32>(words.size() * 4U)));
        framecook::FrameGraphDesc desc(&alloc);
        containers::String        where(&alloc);
        REQUIRE(framecook::parse_frame_toml(containers::StringView(kTessPullGraph), desc, &where)
                == framecook::FrameCookError::Ok);
        TessPullHost              host(dst.get(), prog.get(), sb.get());
        framecook::FrameExecError err = framecook::FrameExecError::Ok;
        REQUIRE(framecook::execute_frame_graph(desc, raster, host, &err, &where));
        CHECK(err == framecook::FrameExecError::Ok);
        return dst->read_pixel(108U, 64U) & 0xFFFFFFU; // NDC ~0.69: inside h=0.6's x1.3 expansion, far outside h=0.3's
    };

    const u32 big   = run(0.6F);
    const u32 small = run(0.3F);
    UNSCOPED_INFO("big=" << big << " small=" << small);
    // the BUFFER drives the patch: with 0.6-corners the x1.3 domain expansion reaches NDC ~0.78 and the probe
    // pixel is lit; re-upload 0.3-corners and the same pixel is background — the control points were PULLED.
    CHECK(big != 0U);
    CHECK(small == 0U);

    // ── REN-38-F6+ (#26): the DOMAIN itself reads the buffer — `hdr:22` scales the expansion, so the tese
    // emitters' sbuf lowering is proven by PIXELS, not by a compile: scale 1.3 reaches the probe, 0.55 does not.
    constexpr const char* hdr_ds_toml = R"(
schema   = 1
name     = "d2"
stage    = "tess_eval"
displace = "expand"

[vertex]
stride = 3

[[attribute]]
name   = "position"
offset = 0
comps  = 3
kind   = "position"

[tess]
patch_size = 4
inner      = 8.0
outer      = 8.0

[[node]]
name   = "expand"
op     = "multiply"
inputs = ["@position", "hdr:22"]
)";
    auto ds2 = cook_vc(hdr_ds_toml);
    REQUIRE(ds2 != nullptr);
    auto prog2 = raster.create_tess_program(*vs, *hs, *ds2, *fs);
    REQUIRE(prog2 != nullptr);
    const auto run_hdr = [&](float scale) {
        auto dst = raster.create_color_target(128U, 128U);
        REQUIRE(dst != nullptr);
        containers::Array<u32> words(&alloc);
        fill_tess_pull_buffer(words, 0.6F);
        std::memcpy(&words[22], &scale, 4U); // the domain's scale word
        auto sb = raster.create_storage_buffer(static_cast<u32>(words.size() * 4U));
        REQUIRE(sb != nullptr);
        REQUIRE(raster.upload_storage(*sb, 0U, words.data(), static_cast<u32>(words.size() * 4U)));
        framecook::FrameGraphDesc desc(&alloc);
        containers::String        where(&alloc);
        REQUIRE(framecook::parse_frame_toml(containers::StringView(kTessPullGraph), desc, &where)
                == framecook::FrameCookError::Ok);
        TessPullHost              host(dst.get(), prog2.get(), sb.get());
        framecook::FrameExecError err = framecook::FrameExecError::Ok;
        REQUIRE(framecook::execute_frame_graph(desc, raster, host, &err, &where));
        CHECK(err == framecook::FrameExecError::Ok);
        return dst->read_pixel(108U, 64U) & 0xFFFFFFU;
    };
    const u32 hdr_big  = run_hdr(1.3F);
    const u32 hdr_damp = run_hdr(0.55F);
    UNSCOPED_INFO("hdr_big=" << hdr_big << " hdr_damp=" << hdr_damp);
    CHECK(hdr_big != 0U);  // 0.6 x 1.3 = 0.78 covers the probe...
    CHECK(hdr_damp == 0U); // ...0.6 x 0.55 = 0.33 does not — the WORD drove the domain
    CHECK(capture.error_count() == 0U);
}

// ── ⭐⭐ REN-38-F6+ GATE: MESHLET FETCH — the mesh stage PULLS real geometry, and the TASK count is GPU-driven.
// Two dichotomies over ONE authored buffer layout (the same scene pull contract the VS uses):
//   1. a `fetch = true` mesh stage renders quads whose EXTENT tracks the buffer's vertex records — re-upload
//      smaller corners and the probe pixel goes dark;
//   2. a task stage with `emit_header` dispatches as many mesh workgroups as the BUFFER says — word 130 = 1
//      renders only instance 0's quad, word 130 = 2 lights instance 1's quad too.
namespace
{
constexpr const char* kMeshFetchGraph = R"(
schema = 1
name   = "crd://frame/f6-mesh-fetch"

[[draw_list]]
name = "meshlets"

[[pass]]
name        = "fetch"
kind        = "raster.mesh"
shader      = "crd://shaders/mesh_fetch"
draw_list   = "meshlets"
writes      = ["@output"]
clear_color = [0.0, 0.0, 0.0, 1.0]
)";

constexpr const char* kMeshFetchVs = R"(
schema = 1
name   = "crd://vertex/meshfetch"
stage  = "mesh"

[vertex]
stride = 3

[[attribute]]
name   = "position"
offset = 0
comps  = 3
kind   = "position"

[instance]
stride    = 20
transform = 0

[mesh]
max_vertices   = 6
max_primitives = 2
workgroup      = 6
fetch          = true
)";

constexpr const char* kTaskHdrToml = R"(
schema = 1
name   = "crd://vertex/taskhdr"
stage  = "task"

[mesh]
workgroup = 1

[task]
emit        = 1
emit_header = 130
)";

class MeshFetchHost final : public framecook::IFrameGraphHost
{
public:
    MeshFetchHost(g::IRasterTarget* out, g::IRasterProgram* prog, g::IStorageBuffer* sb, u32 groups)
        : m_out(out), m_prog(prog), m_sb(sb), m_groups(groups)
    {
    }
    [[nodiscard]] g::IRasterTarget*  output() override { return m_out; }
    [[nodiscard]] g::IRasterProgram* program(containers::StringView) override { return m_prog; }
    [[nodiscard]] bool draw_list(containers::StringView, framecook::DrawListBinding& out) override
    {
        out.items[0] = framecook::DrawItem{m_sb, m_prog, m_groups, nullptr};
        out.resolved = 1U;
        return true;
    }

private:
    g::IRasterTarget*  m_out    = nullptr;
    g::IRasterProgram* m_prog   = nullptr;
    g::IStorageBuffer* m_sb     = nullptr;
    u32                m_groups = 1U;
};

// the pull buffer: 6 indices (two triangles = one quad), 4 corner records (+-h), TWO instances translated to
// x = -0.5 / x = +0.5, identity view_proj, and the task's amplification count at word 130
void fill_mesh_fetch_buffer(containers::Array<u32>& w, float h, u32 task_count)
{
    w.clear();
    w.resize(140U);
    for (usize i = 0; i < w.size(); ++i) { w[i] = 0U; }
    const auto fbits = [](float f) { u32 u = 0; std::memcpy(&u, &f, 4U); return u; };
    w[0] = 6U;   // index COUNT (per instance)
    w[2] = 32U;  // indices at word 32
    w[3] = 40U;  // vertices at word 40
    w[4] = 60U;  // instance records at word 60 (stride 20)
    w[5] = 110U; // visible slot list at word 110
    for (u32 c = 0; c < 4U; ++c) { w[6U + c * 4U + c] = fbits(1.0F); } // identity view_proj (column-major)
    const u32 idx[6] = {0U, 1U, 2U, 0U, 2U, 3U};
    for (u32 i = 0; i < 6U; ++i) { w[32U + i] = idx[i]; }
    const float cx[4] = {-h, h, h, -h};
    const float cy[4] = {-h, -h, h, h};
    for (u32 i = 0; i < 4U; ++i)
    {
        w[40U + i * 3U + 0U] = fbits(cx[i]);
        w[40U + i * 3U + 1U] = fbits(cy[i]);
        w[40U + i * 3U + 2U] = fbits(0.0F);
    }
    for (u32 inst = 0; inst < 2U; ++inst) // identity + a translation: instance 0 left, instance 1 right
    {
        const u32 base = 60U + inst * 20U;
        for (u32 c = 0; c < 4U; ++c) { w[base + c * 4U + c] = fbits(1.0F); }
        w[base + 12U] = fbits(inst == 0U ? -0.5F : 0.5F); // column-major translation x
    }
    w[110] = 0U;
    w[111] = 1U;
    w[130] = task_count; // the GPU-driven amplification count
}
} // namespace

TEST_CASE("REN-38-F6+ GATE: a FETCH mesh stage renders the buffer's geometry and emit_header drives the dispatch",
          "[gpu-context][vulkan][frame-graph][ren38][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;

    memory::TlsfAllocator alloc(16U << 20U);
    const auto            cook_vc = [&](const char* toml) -> std::unique_ptr<g::IGpuProgram> {
        crd::vertcook::VertexProgramDesc d(&alloc);
        containers::String               where(&alloc);
        REQUIRE(crd::vertcook::parse_vertex_toml(containers::StringView(toml), d, &where)
                == crd::vertcook::VertexCookError::Ok);
        kir::KGraph g2(&alloc);
        kir::KEntry e;
        REQUIRE(crd::vertcook::cook_vertex_program(d, g2, e));
        return rig.vk->create_program(g2, e);
    };
    auto ms = cook_vc(kMeshFetchVs);
    // the task-paired variant: `payload = true` declares the AS->MS payload contract the D3D12 PSO validator
    // enforces (Vulkan tolerates its absence — which is exactly why only a DX12 gate would have caught it)
    containers::String fetch_pl(&alloc);
    fetch_pl.append(kMeshFetchVs);
    fetch_pl.append("payload        = true\n");
    auto msp = cook_vc(fetch_pl.c_str());
    auto tk  = cook_vc(kTaskHdrToml);
    kir::KGraph fg2(&alloc);
    kir::KEntry fe;
    gputest::build_solid_fs(fg2, fe, 1.0, 0.3, 0.1);
    auto fs = rig.vk->create_program(fg2, fe);
    if (ms == nullptr || msp == nullptr || tk == nullptr || fs == nullptr) { SKIP("mesh shader compile unavailable"); }
    auto mesh_prog = raster.create_mesh_program(*ms, *fs);
    auto task_prog = raster.create_task_mesh_program(*tk, *msp, *fs);
    if (mesh_prog == nullptr || task_prog == nullptr) { SKIP("no mesh-shader support on this device"); }

    g::ValidationCapture capture(*rig.vk);
    // probe pixels sit 0.2 NDC RIGHT of each quad centre (-0.5 / +0.5): inside an h = 0.35 quad, outside an
    // h = 0.05 one -- a centre probe would stay covered no matter how small the pulled corners became
    const auto run = [&](g::IRasterProgram* prog, u32 groups, float h, u32 task_count, u32* left, u32* right) {
        auto dst = raster.create_color_target(128U, 128U);
        REQUIRE(dst != nullptr);
        containers::Array<u32> words(&alloc);
        fill_mesh_fetch_buffer(words, h, task_count);
        auto sb = raster.create_storage_buffer(static_cast<u32>(words.size() * 4U));
        REQUIRE(sb != nullptr);
        REQUIRE(raster.upload_storage(*sb, 0U, words.data(), static_cast<u32>(words.size() * 4U)));
        framecook::FrameGraphDesc desc(&alloc);
        containers::String        where(&alloc);
        REQUIRE(framecook::parse_frame_toml(containers::StringView(kMeshFetchGraph), desc, &where)
                == framecook::FrameCookError::Ok);
        MeshFetchHost             host(dst.get(), prog, sb.get(), groups);
        framecook::FrameExecError err = framecook::FrameExecError::Ok;
        REQUIRE(framecook::execute_frame_graph(desc, raster, host, &err, &where));
        CHECK(err == framecook::FrameExecError::Ok);
        *left  = dst->read_pixel(44U, 64U) & 0xFFFFFFU;  // NDC -0.3
        *right = dst->read_pixel(108U, 64U) & 0xFFFFFFU; // NDC +0.7
    };

    // ── claim 1: the FETCH — quad extent tracks the buffer's vertex records ──
    u32 big_l = 0U;
    u32 big_r = 0U;
    run(mesh_prog.get(), 2U, 0.35F, 2U, &big_l, &big_r);
    u32 small_l = 0U;
    u32 small_r = 0U;
    run(mesh_prog.get(), 2U, 0.05F, 2U, &small_l, &small_r);
    UNSCOPED_INFO("fetch big=(" << big_l << "," << big_r << ") small=(" << small_l << "," << small_r << ")");
    CHECK(big_l != 0U);   // both instances' quads cover their probe pixels...
    CHECK(big_r != 0U);
    CHECK(small_l == 0U); // ...and shrink to nothing when the BUFFER's corners shrink — the geometry was PULLED
    CHECK(small_r == 0U);

    // ── claim 2: the GPU-driven COUNT — word 130 decides how many meshlets exist ──
    u32 one_l = 0U;
    u32 one_r = 0U;
    run(task_prog.get(), 1U, 0.35F, 1U, &one_l, &one_r);
    u32 two_l = 0U;
    u32 two_r = 0U;
    run(task_prog.get(), 1U, 0.35F, 2U, &two_l, &two_r);
    UNSCOPED_INFO("task one=(" << one_l << "," << one_r << ") two=(" << two_l << "," << two_r << ")");
    CHECK(one_l != 0U); // count 1: instance 0 only...
    CHECK(one_r == 0U);
    CHECK(two_l != 0U); // ...count 2: the buffer word ALONE lit the second quad
    CHECK(two_r != 0U);
    if (capture.error_count() > 0U) // print WHAT before failing — a bare count diagnoses nothing
    {
        const auto msgs = capture.messages();
        for (usize i = 0; i < msgs.size(); ++i) { WARN("[fetch-gate capture] " << msgs[i].message_text.c_str()); }
    }
    CHECK(capture.error_count() == 0U);
}

// ── ⭐⭐ REN-38 GATE: MULTI-DRAW — N draws, ONE device command, pixels identical to the classic loop. ────────
// The batching board measured 6.1x (VK) / 38.8x (DX12) headroom at 64 draws, all of it per-draw descriptor
// churn. `draw_storage_multi_depth` records ONE descriptor set + ONE indirect command over N draws. This gate
// demands BOTH halves of the claim, because pixels alone cannot distinguish "batched" from "looped":
//   (a) the batched frame's readback is BIT-IDENTICAL to a frame of classic per-draw calls;
//   (b) `multi_batch_count()` advanced by EXACTLY ONE — one bucket, one device command.
namespace
{
struct MultiGateState
{
    g::FgImage         img{};
    g::IRasterProgram* prog = nullptr;
    g::IStorageBuffer* sb   = nullptr;
    const crd::u32*    counts = nullptr;
};
void record_multi_classic(g::IFrameContext& ctx, void* user)
{
    auto* u = static_cast<MultiGateState*>(user);
    crd::gputest::enc_draw_storage_depth(ctx.raster(), *ctx.image(u->img), *u->prog, g::ClearColor{0.05F, 0.0F, 0.0F, 1.0F}, 0.0F,
                                    g::DepthCompare::Always, *u->sb, 3U);
    for (crd::u32 i = 1; i < 8U; ++i)
    {
        crd::gputest::enc_draw_storage_depth_load(ctx.raster(), *ctx.image(u->img), *u->prog, g::DepthCompare::Always, *u->sb, 3U);
    }
}
void record_multi_batched(g::IFrameContext& ctx, void* user)
{
    auto* u = static_cast<MultiGateState*>(user);
    crd::gputest::enc_draw_storage_multi_depth(ctx.raster(),*ctx.image(u->img), *u->prog, g::ClearColor{0.05F, 0.0F, 0.0F, 1.0F},
                                          0.0F, g::DepthCompare::Always, *u->sb, u->counts, 8U, 0U, false);
}
} // namespace

TEST_CASE("REN-38 GATE: multi-draw batches N draws into ONE indirect command, bit-identical pixels",
          "[gpu-context][vulkan][frame-graph][ren38][multidraw][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;

    memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    gputest::build_triangle_vs(vg, ve);
    kir::KGraph fg2(&alloc);
    kir::KEntry fe;
    gputest::build_triangle_fs(fg2, fe);
    auto vs = rig.vk->create_program(vg, ve);
    auto fs = rig.vk->create_program(fg2, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr);
    auto prog = raster.create_raster_program(*vs, *fs);
    REQUIRE(prog != nullptr);
    auto sb = raster.create_storage_buffer(16U);
    REQUIRE(sb != nullptr);

    crd::u32 counts[8];
    for (crd::u32 i = 0; i < 8U; ++i) { counts[i] = 3U; } // the same triangle, drawn 8 times

    // ── the CLASSIC reference: a frame of per-draw calls through the graph ──
    auto ref = raster.create_color_depth_target(64U, 64U);
    REQUIRE(ref != nullptr);
    {
        auto fgraph = raster.create_frame_graph();
        REQUIRE(fgraph != nullptr);
        MultiGateState st;
        st.img  = fgraph->import_target(*ref);
        st.prog = prog.get();
        st.sb   = sb.get();
        fgraph->add_pass("classic").writes(st.img).execute(&record_multi_classic, &st);
        REQUIRE(fgraph->build());
        fgraph->execute();
    }

    // ── the BATCHED frame: ONE multi-draw over the same 8 ──
    auto tgt = raster.create_color_depth_target(64U, 64U);
    REQUIRE(tgt != nullptr);
    const crd::u64 batches_before = raster.multi_batch_count();
    {
        auto fgraph = raster.create_frame_graph();
        REQUIRE(fgraph != nullptr);
        MultiGateState st;
        st.img    = fgraph->import_target(*tgt);
        st.prog   = prog.get();
        st.sb     = sb.get();
        st.counts = static_cast<const crd::u32*>(counts);
        fgraph->add_pass("batched").writes(st.img).execute(&record_multi_batched, &st);
        REQUIRE(fgraph->build());
        fgraph->execute();
    }
    // (b) EXACTLY one batch was recorded — the count assert that "looped" cannot fake
    CHECK(raster.multi_batch_count() == batches_before + 1U);

    // (a) bit-identical pixels, over a frame that actually drew
    crd::u32 diffs   = 0U;
    crd::u32 covered = 0U;
    for (crd::u32 y = 0; y < 64U; ++y)
    {
        for (crd::u32 x = 0; x < 64U; ++x)
        {
            const crd::u32 a = ref->read_pixel(x, y);
            const crd::u32 b = tgt->read_pixel(x, y);
            if (a != b) { ++diffs; }
            if ((b & 0x00FFFFFFU) != 0U) { ++covered; }
        }
    }
    CHECK(diffs == 0U);
    CHECK(covered > 100U); // a blank==blank match proves nothing
}

// ── ⭐⭐ REN-39-A1 GATE: the scene buffer serves as its OWN index buffer. ────────────────────────────────────
// The frame is MEASURED vertex-bound (the pull idiom re-shades every triangle corner); the lever is drawing
// INDEXED against the SAME storage buffer. This gate proves the A1 slice on the device, with a dichotomy each
// arm can FAIL:
//   · ONE buffer holds 8 pull records (12 words each, the GEO-1 stride) AND a u32 index section {4,5,6} at
//     byte 384. Records 0..3 and 7 are DEGENERATE (identical, offscreen); records 4..6 are the triangle.
//   · the NON-indexed control (3 vertices → records 0..2) draws NOTHING — proving the pixels below can only
//     come from index values steering VertexIndex;
//   · the INDEXED draw (same program, same buffer) renders the triangle — the IA fetched {4,5,6} from the
//     storage buffer and the VS pulled the REAL records: index fetch + SSBO read coexist in one draw, one
//     buffer (the zero-duplication contract);
//   · a MISALIGNED offset and an OVERRUNNING section are REFUSED (the target provably keeps its pixels);
//   · the FRAME-MODE arm records clear + LOAD-continuation indexed draws through a graph (the renderer's path);
//   · validation-SILENT throughout (ValidationCapture == 0).
TEST_CASE("REN-39-A1 GATE: storage buffer binds as its own index buffer and the indexed draw pulls real records",
          "[gpu-context][vulkan][frame-graph][ren39][indexed][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr)
    {
        SKIP("no graphics-capable Vulkan device with shader objects");
    }
    auto& raster = *rig.raster;
    g::ValidationCapture capture(*rig.vk);

    memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    gputest::build_vertex_pull_vs(vg, ve); // position = record at VertexIndex * 12 words — the GEO-1 pull
    kir::KGraph fg2(&alloc);
    kir::KEntry fe;
    gputest::build_triangle_fs(fg2, fe);
    auto vs = rig.vk->create_program(vg, ve);
    auto fs = rig.vk->create_program(fg2, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr);
    auto prog = raster.create_raster_program(*vs, *fs);
    REQUIRE(prog != nullptr);

    // ── the ONE buffer: 8 records (96 words / 384 bytes) + 3 index words = 396 bytes EXACTLY (the overrun
    //    refusal below depends on the tight size). Records default to (2,2,0) — identical ⇒ zero-area, and
    //    offscreen; records 4..6 carry the shared triangle's corners.
    auto sb = raster.create_storage_buffer(396U);
    REQUIRE(sb != nullptr);
    float rec[96];
    for (crd::u32 r = 0; r < 8U; ++r)
    {
        rec[r * 12U + 0U] = 2.0F;
        rec[r * 12U + 1U] = 2.0F;
        for (crd::u32 wxx = 2U; wxx < 12U; ++wxx)
        {
            rec[r * 12U + wxx] = 0.0F;
        }
    }
    rec[4U * 12U + 0U] = 0.0F;
    rec[4U * 12U + 1U] = -0.8F; // record 4: apex
    rec[5U * 12U + 0U] = 0.8F;
    rec[5U * 12U + 1U] = 0.8F; // record 5: right base
    rec[6U * 12U + 0U] = -0.8F;
    rec[6U * 12U + 1U] = 0.8F; // record 6: left base
    REQUIRE(raster.upload_storage(*sb, 0U, static_cast<const void*>(rec), sizeof(rec)));
    const crd::u32 idx[3] = {4U, 5U, 6U};
    REQUIRE(raster.upload_storage(*sb, 384U, static_cast<const void*>(idx), sizeof(idx)));

    // ── the NON-indexed control: VertexIndex ∈ {0,1,2} → degenerate records → NOTHING renders ──
    auto miss = raster.create_color_depth_target(64U, 64U);
    REQUIRE(miss != nullptr);
    crd::gputest::enc_draw_storage_depth(raster, *miss, *prog, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 0.0F, g::DepthCompare::Always, *sb,
                              3U);
    CHECK((miss->read_pixel(32U, 32U) & 0x00FFFFFFU) == 0U); // the centre stays the black clear

    // ── the INDEXED draw: the IA fetches {4,5,6} from byte 384 of the SAME buffer → the real triangle ──
    auto hit = raster.create_color_depth_target(64U, 64U);
    REQUIRE(hit != nullptr);
    crd::gputest::enc_draw_storage_indexed_depth(raster, *hit, *prog, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 0.0F, g::DepthCompare::Always,
                                      *sb, 384U, 3U, 1U, false);
    CHECK((hit->read_pixel(32U, 32U) & 0xFFU) >= 250U);   // red centre — the index values arrived
    CHECK((hit->read_pixel(1U, 1U) & 0x00FFFFFFU) == 0U); // corner stays the clear

    // ── refusals keep pixels: a GREEN clear that RAN would repaint the target — prove it did not ──
    crd::gputest::enc_draw_storage_indexed_depth(raster, *hit, *prog, g::ClearColor{0.0F, 1.0F, 0.0F, 1.0F}, 0.0F, g::DepthCompare::Always,
                                      *sb, 382U, 3U, 1U, false); // misaligned offset
    crd::gputest::enc_draw_storage_indexed_depth(raster, *hit, *prog, g::ClearColor{0.0F, 1.0F, 0.0F, 1.0F}, 0.0F, g::DepthCompare::Always,
                                      *sb, 384U, 4U, 1U, false); // 384+16 > 396: overrun
    CHECK((hit->read_pixel(32U, 32U) & 0xFFU) >= 250U);          // still the red triangle — both draws were REFUSED

    // ── the FRAME-MODE arm: clear + LOAD continuation through a graph (the record_scene_indexed path) ──
    auto ftgt = raster.create_color_depth_target(64U, 64U);
    REQUIRE(ftgt != nullptr);
    {
        auto fgraph = raster.create_frame_graph();
        REQUIRE(fgraph != nullptr);
        struct IndexedPass
        {
            g::FgImage img{};
            g::IRasterProgram* prog = nullptr;
            g::IStorageBuffer* sb = nullptr;
        } st;
        st.img = fgraph->import_target(*ftgt);
        st.prog = prog.get();
        st.sb = sb.get();
        fgraph->add_pass("indexed").writes(st.img).execute(
            [](g::IFrameContext& ctx, void* user)
            {
                auto* u = static_cast<IndexedPass*>(user);
                crd::gputest::enc_draw_storage_indexed_depth(ctx.raster(), *ctx.image(u->img), *u->prog,
                                                        g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 0.0F,
                                                        g::DepthCompare::Always, *u->sb, 384U, 3U, 1U, false);
                // the LOAD continuation — draw N>0 of a pass must not wipe draw 0 (records identical pixels)
                crd::gputest::enc_draw_storage_indexed_depth(ctx.raster(), *ctx.image(u->img), *u->prog, g::ClearColor{}, 0.0F,
                                                        g::DepthCompare::Always, *u->sb, 384U, 3U, 1U, true);
            },
            &st);
        REQUIRE(fgraph->build());
        fgraph->execute();
    }
    CHECK((ftgt->read_pixel(32U, 32U) & 0xFFU) >= 250U);   // red centre through the frame path
    CHECK((ftgt->read_pixel(1U, 1U) & 0x00FFFFFFU) == 0U); // corner stays the clear

    if (capture.error_count() > 0U) // print WHAT before failing — a bare count diagnoses nothing
    {
        const auto msgs = capture.messages();
        for (usize i = 0; i < msgs.size(); ++i)
        {
            WARN("[ren39-a1 capture] " << msgs[i].message_text.c_str());
        }
    }
    CHECK(capture.error_count() == 0U);
}

// ── ⭐⭐ REN-39-A2 GATE: INDEXED MULTI-DRAW — N indexed draws, ONE device command. ──────────────────────────
// The exact shape of the 38-4 multi-draw gate, indexed: pixels alone cannot distinguish "batched" from
// "looped", so the gate demands BOTH halves — (a) the batched frame's readback is BIT-IDENTICAL to a frame of
// classic per-draw indexed calls, and (b) `multi_batch_count()` advanced by EXACTLY ONE. The two commands have
// DIFFERENT `first_index` values over one index section ({4,5,6} → the big centred triangle, {8,9,10} → the
// small corner triangle), so two DISJOINT probes prove each command's first_index routed independently.
TEST_CASE("REN-39-A2 GATE: indexed multi-draw batches N indexed draws into ONE indirect command",
          "[gpu-context][vulkan][frame-graph][ren39][indexed][multidraw][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr)
    {
        SKIP("no graphics-capable Vulkan device with shader objects");
    }
    auto& raster = *rig.raster;
    g::ValidationCapture capture(*rig.vk);

    memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    gputest::build_vertex_pull_vs(vg, ve); // the GEO-1 pull — SSBO read + index fetch in one draw, one buffer
    kir::KGraph fg2(&alloc);
    kir::KEntry fe;
    gputest::build_triangle_fs(fg2, fe);
    auto vs = rig.vk->create_program(vg, ve);
    auto fs = rig.vk->create_program(fg2, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr);
    auto prog = raster.create_raster_program(*vs, *fs);
    REQUIRE(prog != nullptr);

    // 12 records (576 bytes) + the 6-index section {4,5,6, 8,9,10} at byte 576 = 600 bytes
    auto sb = raster.create_storage_buffer(600U);
    REQUIRE(sb != nullptr);
    float rec[144];
    for (crd::u32 r = 0; r < 12U; ++r)
    {
        rec[r * 12U + 0U] = 2.0F;
        rec[r * 12U + 1U] = 2.0F;
        for (crd::u32 wxx = 2U; wxx < 12U; ++wxx)
        {
            rec[r * 12U + wxx] = 0.0F;
        }
    }
    rec[4U * 12U + 0U] = 0.0F;
    rec[4U * 12U + 1U] = -0.8F; // records 4..6: the big centred triangle
    rec[5U * 12U + 0U] = 0.8F;
    rec[5U * 12U + 1U] = 0.8F;
    rec[6U * 12U + 0U] = -0.8F;
    rec[6U * 12U + 1U] = 0.8F;
    rec[8U * 12U + 0U] = -0.95F;
    rec[8U * 12U + 1U] = -0.95F; // records 8..10: the left-edge spike (midline-probed)
    rec[9U * 12U + 0U] = -0.95F;
    rec[9U * 12U + 1U] = 0.95F;
    rec[10U * 12U + 0U] = -0.5F;
    rec[10U * 12U + 1U] = 0.0F;
    REQUIRE(raster.upload_storage(*sb, 0U, static_cast<const void*>(rec), sizeof(rec)));
    const crd::u32 idx[6] = {4U, 5U, 6U, 8U, 9U, 10U};
    REQUIRE(raster.upload_storage(*sb, 576U, static_cast<const void*>(idx), sizeof(idx)));

    struct MultiIdxState
    {
        g::FgImage img{};
        g::IRasterProgram* prog = nullptr;
        g::IStorageBuffer* sb = nullptr;
        const g::IRasterContext::IndexedDraw* draws = nullptr;
    };
    const g::IRasterContext::IndexedDraw draws[2] = {{3U, 1U, 0U}, {3U, 1U, 3U}};

    // ── the CLASSIC reference: a frame of per-draw indexed calls through the graph ──
    auto ref = raster.create_color_depth_target(64U, 64U);
    REQUIRE(ref != nullptr);
    {
        auto fgraph = raster.create_frame_graph();
        REQUIRE(fgraph != nullptr);
        MultiIdxState st;
        st.img = fgraph->import_target(*ref);
        st.prog = prog.get();
        st.sb = sb.get();
        fgraph->add_pass("classic").writes(st.img).execute(
            [](g::IFrameContext& ctx, void* user)
            {
                auto* u = static_cast<MultiIdxState*>(user);
                crd::gputest::enc_draw_storage_indexed_depth(ctx.raster(), *ctx.image(u->img), *u->prog,
                                                        g::ClearColor{0.05F, 0.0F, 0.0F, 1.0F}, 0.0F,
                                                        g::DepthCompare::Always, *u->sb, 576U, 3U, 1U, false);
                crd::gputest::enc_draw_storage_indexed_depth(ctx.raster(), *ctx.image(u->img), *u->prog, g::ClearColor{}, 0.0F,
                                                        g::DepthCompare::Always, *u->sb, 576U + 12U, 3U, 1U, true);
            },
            &st);
        REQUIRE(fgraph->build());
        fgraph->execute();
    }

    // ── the BATCHED frame: ONE indexed multi-draw over the same two commands ──
    auto tgt = raster.create_color_depth_target(64U, 64U);
    REQUIRE(tgt != nullptr);
    const crd::u64 batches_before = raster.multi_batch_count();
    {
        auto fgraph = raster.create_frame_graph();
        REQUIRE(fgraph != nullptr);
        MultiIdxState st;
        st.img = fgraph->import_target(*tgt);
        st.prog = prog.get();
        st.sb = sb.get();
        st.draws = static_cast<const g::IRasterContext::IndexedDraw*>(draws);
        fgraph->add_pass("batched").writes(st.img).execute(
            [](g::IFrameContext& ctx, void* user)
            {
                auto* u = static_cast<MultiIdxState*>(user);
                crd::gputest::enc_draw_storage_multi_indexed_depth(ctx.raster(),
                    *ctx.image(u->img), *u->prog, g::ClearColor{0.05F, 0.0F, 0.0F, 1.0F}, 0.0F, g::DepthCompare::Always,
                    *u->sb, 576U, u->draws, 2U, 0U, false);
            },
            &st);
        REQUIRE(fgraph->build());
        fgraph->execute();
    }
    // (b) EXACTLY one batch was recorded — the count assert that "looped" cannot fake
    CHECK(raster.multi_batch_count() == batches_before + 1U);

    // both commands' geometry present, MIDLINE probes (orientation-invariant — the F16 lesson): the big
    // triangle owns the centre, the spike the left edge; the right edge is outside both
    CHECK((tgt->read_pixel(32U, 32U) & 0xFFU) >= 250U);
    CHECK((tgt->read_pixel(8U, 32U) & 0xFFU) >= 250U);
    CHECK((tgt->read_pixel(60U, 32U) & 0xFFU) <= 20U); // outside both — the dim clear only

    // (a) bit-identical pixels, over a frame that actually drew
    crd::u32 diffs = 0U;
    crd::u32 covered = 0U;
    for (crd::u32 y = 0; y < 64U; ++y)
    {
        for (crd::u32 x = 0; x < 64U; ++x)
        {
            const crd::u32 a = ref->read_pixel(x, y);
            const crd::u32 b = tgt->read_pixel(x, y);
            if (a != b)
            {
                ++diffs;
            }
            if ((b & 0xFFU) >= 250U)
            {
                ++covered;
            }
        }
    }
    CHECK(diffs == 0U);
    CHECK(covered > 100U); // a blank==blank match proves nothing

    if (capture.error_count() > 0U)
    {
        const auto msgs = capture.messages();
        for (usize i = 0; i < msgs.size(); ++i)
        {
            WARN("[ren39-a2 capture] " << msgs[i].message_text.c_str());
        }
    }
    CHECK(capture.error_count() == 0U);
}

// ── ⭐⭐ REN-39-B1 GATE: InstanceIndex drives the per-instance sequence, identical on every backend. ─────────
// The indexed draw carries `instance_count` instances with firstInstance ALWAYS 0 (the A2 normalization: VK's
// gl_InstanceIndex includes firstInstance, DX12's SV_InstanceID does not — 0 is the only portable value). The
// probe VS places instance i's triangle in pixel column 8 + 16·i, so the COLUMNS are the sequence: a backend
// that read instances off by any base would shift every column; one that dropped instancing lights only column
// 0. The DX12 twin asserts the SAME columns from the SAME asset — the identical-sequence proof, in pixels.
TEST_CASE("REN-39-B1 GATE: InstanceIndex sequences instances 0..N-1 through the indexed draw",
          "[gpu-context][vulkan][frame-graph][ren39][indexed][instance][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr)
    {
        SKIP("no graphics-capable Vulkan device with shader objects");
    }
    auto& raster = *rig.raster;
    g::ValidationCapture capture(*rig.vk);

    memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    gputest::build_indexed_instance_vs(vg, ve);
    kir::KGraph fg2(&alloc);
    kir::KEntry fe;
    gputest::build_triangle_fs(fg2, fe);
    auto vs = rig.vk->create_program(vg, ve);
    auto fs = rig.vk->create_program(fg2, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr);
    auto prog = raster.create_raster_program(*vs, *fs);
    REQUIRE(prog != nullptr);

    auto sb = raster.create_storage_buffer(396U);
    REQUIRE(sb != nullptr);
    const crd::u32 idx[3] = {4U, 5U, 6U};
    REQUIRE(raster.upload_storage(*sb, 384U, static_cast<const void*>(idx), sizeof(idx)));

    // ── 4 instances: columns 8/24/40/56 all lit on the midline ──
    auto four = raster.create_color_depth_target(64U, 64U);
    REQUIRE(four != nullptr);
    crd::gputest::enc_draw_storage_indexed_depth(raster, *four, *prog, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 0.0F,
                                      g::DepthCompare::Always, *sb, 384U, 3U, 4U, false);
    CHECK((four->read_pixel(8U, 32U) & 0xFFU) >= 250U);      // instance 0
    CHECK((four->read_pixel(24U, 32U) & 0xFFU) >= 250U);     // instance 1
    CHECK((four->read_pixel(40U, 32U) & 0xFFU) >= 250U);     // instance 2
    CHECK((four->read_pixel(56U, 32U) & 0xFFU) >= 250U);     // instance 3
    CHECK((four->read_pixel(16U, 32U) & 0x00FFFFFFU) == 0U); // between columns — the clear

    // ── the DICHOTOMY: 2 instances light exactly columns 0..1; 2 and 3 stay dark ──
    auto two = raster.create_color_depth_target(64U, 64U);
    REQUIRE(two != nullptr);
    crd::gputest::enc_draw_storage_indexed_depth(raster, *two, *prog, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 0.0F, g::DepthCompare::Always,
                                      *sb, 384U, 3U, 2U, false);
    CHECK((two->read_pixel(8U, 32U) & 0xFFU) >= 250U);
    CHECK((two->read_pixel(24U, 32U) & 0xFFU) >= 250U);
    CHECK((two->read_pixel(40U, 32U) & 0x00FFFFFFU) == 0U);
    CHECK((two->read_pixel(56U, 32U) & 0x00FFFFFFU) == 0U);

    if (capture.error_count() > 0U)
    {
        const auto msgs = capture.messages();
        for (usize i = 0; i < msgs.size(); ++i)
        {
            WARN("[ren39-b1 capture] " << msgs[i].message_text.c_str());
        }
    }
    CHECK(capture.error_count() == 0U);
}

// ── ⭐⭐ REN-39-B2 GATE (device parity): the SAME declaration, pull vs indexed, BIT-IDENTICAL pixels. ────────
// The vertex stage has no CPU oracle (eval_cpu has no Builtin/StorageLoad arms — stated in the cook gate), so
// the numerical reference for the indexed mode IS the pull mode, proven against the live scene for months. One
// minimal real scene buffer (header contract: counts + section offsets + identity view_proj, vertex records,
// an identity instance, a visible list, a u32 index section) is drawn by BOTH cooks of one `.crdv`; the frames
// must match byte for byte. This is the parity proof 39-C1 scales to the full renderer.
namespace
{
constexpr const char* kParityDecl = R"(
schema = 1
name   = "crd://test/indexed-parity"

[header]
index_count  = 0
index_off    = 2
vertex_off   = 3
instance_off = 4
visible_off  = 5
view_proj    = 6

[vertex]
stride = 12

[[attribute]]
name   = "position"
offset = 0
comps  = 3
kind   = "position"

[instance]
stride    = 20
transform = 0
)";
} // namespace

TEST_CASE("REN-39-B2 GATE: pull and indexed cooks of one declaration render bit-identical frames",
          "[gpu-context][vulkan][frame-graph][ren39][indexed][parity][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr)
    {
        SKIP("no graphics-capable Vulkan device with shader objects");
    }
    auto& raster = *rig.raster;
    g::ValidationCapture capture(*rig.vk);

    memory::TlsfAllocator alloc(16U << 20U);

    // cook BOTH modes of the SAME declaration
    const auto cook_mode = [&](bool indexed, kir::KGraph& vg, kir::KEntry& ve)
    {
        containers::String t(&alloc);
        if (indexed)
        {
            t.append("indexed = true\n");
        }
        t.append(kParityDecl);
        vertcook::VertexProgramDesc desc(&alloc);
        containers::String where(&alloc);
        REQUIRE(vertcook::parse_vertex_toml(containers::StringView(t.c_str(), t.size()), desc, &where) ==
                vertcook::VertexCookError::Ok);
        REQUIRE(vertcook::cook_vertex_program(desc, vg, ve));
    };
    kir::KGraph pull_g(&alloc);
    kir::KEntry pull_e;
    cook_mode(false, pull_g, pull_e);
    kir::KGraph idx_g(&alloc);
    kir::KEntry idx_e;
    cook_mode(true, idx_g, idx_e);

    kir::KGraph fg2(&alloc);
    kir::KEntry fe;
    gputest::build_triangle_fs(fg2, fe);
    auto pull_vs = rig.vk->create_program(pull_g, pull_e);
    auto idx_vs = rig.vk->create_program(idx_g, idx_e);
    auto fs = rig.vk->create_program(fg2, fe);
    REQUIRE(pull_vs != nullptr);
    REQUIRE(idx_vs != nullptr);
    REQUIRE(fs != nullptr);
    auto pull_prog = raster.create_raster_program(*pull_vs, *fs);
    auto idx_prog = raster.create_raster_program(*idx_vs, *fs);
    REQUIRE(pull_prog != nullptr);
    REQUIRE(idx_prog != nullptr);

    // ── the ONE real scene buffer both modes read: header + indices@40 + vertices@43 + instance@80 + visible@100
    crd::u32 words[103];
    for (crd::u32 i = 0; i < 103U; ++i)
    {
        words[i] = 0U;
    }
    const auto put_f = [&](crd::u32 w, float v)
    {
        std::memcpy(&words[w], &v, 4U);
    };
    words[0] = 3U;   // index_count
    words[2] = 40U;  // index_off
    words[3] = 43U;  // vertex_off
    words[4] = 80U;  // instance_off
    words[5] = 100U; // visible_off
    for (crd::u32 i = 0; i < 4U; ++i)
    {
        put_f(6U + i * 4U + i, 1.0F);
    } // identity view_proj (col-major)
    words[40] = 0U;
    words[41] = 1U;
    words[42] = 2U; // the index section
    put_f(43U, 0.0F);
    put_f(44U, -0.8F);
    put_f(45U, 0.0F); // vertex records (stride 12)
    put_f(55U, 0.8F);
    put_f(56U, 0.8F);
    put_f(57U, 0.0F);
    put_f(67U, -0.8F);
    put_f(68U, 0.8F);
    put_f(69U, 0.0F);
    for (crd::u32 i = 0; i < 4U; ++i)
    {
        put_f(80U + i * 4U + i, 1.0F);
    } // identity instance transform
    words[100] = 0U; // visible list: instance 0
    auto sb = raster.create_storage_buffer(static_cast<crd::u32>(sizeof(words)));
    REQUIRE(sb != nullptr);
    REQUIRE(raster.upload_storage(*sb, 0U, static_cast<const void*>(words), sizeof(words)));

    // ── PULL: the classic draw (vertex_count = visible × index_count = 3) ──
    auto ref = raster.create_color_depth_target(64U, 64U);
    REQUIRE(ref != nullptr);
    crd::gputest::enc_draw_storage_depth(raster, *ref, *pull_prog, g::ClearColor{0.0F, 0.0F, 0.05F, 1.0F}, 0.0F, g::DepthCompare::Always,
                              *sb, 3U);

    // ── INDEXED: the same buffer serves as its own index buffer at byte 160 ──
    auto tgt = raster.create_color_depth_target(64U, 64U);
    REQUIRE(tgt != nullptr);
    crd::gputest::enc_draw_storage_indexed_depth(raster, *tgt, *idx_prog, g::ClearColor{0.0F, 0.0F, 0.05F, 1.0F}, 0.0F,
                                      g::DepthCompare::Always, *sb, 160U, 3U, 1U, false);

    CHECK((tgt->read_pixel(32U, 32U) & 0xFFU) >= 250U); // the triangle rendered at all
    crd::u32 diffs = 0U;
    crd::u32 covered = 0U;
    for (crd::u32 y = 0; y < 64U; ++y)
    {
        for (crd::u32 x = 0; x < 64U; ++x)
        {
            const crd::u32 a = ref->read_pixel(x, y);
            const crd::u32 b = tgt->read_pixel(x, y);
            if (a != b)
            {
                ++diffs;
            }
            if ((b & 0xFFU) >= 250U)
            {
                ++covered;
            }
        }
    }
    CHECK(diffs == 0U);    // bit-identical — the parity proof
    CHECK(covered > 100U); // over a frame that actually drew

    if (capture.error_count() > 0U)
    {
        const auto msgs = capture.messages();
        for (usize i = 0; i < msgs.size(); ++i)
        {
            WARN("[ren39-b2 capture] " << msgs[i].message_text.c_str());
        }
    }
    CHECK(capture.error_count() == 0U);
}

// ⭐⭐ REN-40-A GATE: the GPU-WRITTEN DRAW — args AND count sourced from device memory, and the COUNT BUFFER
// actually gates how many commands execute.
//
// ⛔ THE POINT OF THE COUNT ARM. `ExecuteIndirect` / `vkCmdDrawIndexedIndirectCount` both accept a device-side
// count so a cull kernel decides how many commands run; an empty batch then costs NOTHING instead of a
// zero-instance command each. A verb that merely IGNORED the count buffer would still draw a plausible frame,
// which is why this gate drives the SAME args twice and changes only the count word: 1 must draw ONE command,
// 2 must draw BOTH. Anything that passes both arms identically is not honouring the count.
//
// ⛔ The verb is DEPTH-ONLY (no colour attachment), so depth is converted to something `read_pixel` can see:
// clear depth to 1.0 → the indirect pass writes 0.5 wherever a command rasterized (a constant-depth FS, so the
// two commands differ only in WHERE they cover) → a full-screen triangle at z = 0.75 with GREATER passes
// exactly where 0.5 was written. Colour therefore appears precisely where a command drew.
TEST_CASE("REN-40-A GATE: indirect draw takes its args AND its count from device memory",
          "[gpu-context][vulkan][frame-graph][ren40][indirect][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;
    g::ValidationCapture capture(*rig.vk);
    REQUIRE(raster.indirect_count_supported()); // Vulkan 1.3 => vkCmdDrawIndexedIndirectCount is CORE

    memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    gputest::build_vertex_pull_vs(vg, ve); // position = record at VertexIndex * 12 words
    auto vs = rig.vk->create_program(vg, ve);
    kir::KGraph cgraph(&alloc);
    kir::KEntry cfe;
    gputest::build_triangle_fs(cgraph, cfe); // writes colour
    auto cfs = rig.vk->create_program(cgraph, cfe);
    kir::KGraph dgraph(&alloc);
    kir::KEntry dfe;
    gputest::build_depth_only_const_fs(dgraph, dfe, 0.5); // n_out = 0 - legal with zero colour attachments
    auto dfs = rig.vk->create_program(dgraph, dfe);
    REQUIRE(vs != nullptr);
    REQUIRE(cfs != nullptr);
    REQUIRE(dfs != nullptr);
    auto prog_colour = raster.create_raster_program(*vs, *cfs);
    auto prog_depth  = raster.create_raster_program(*vs, *dfs);
    REQUIRE(prog_colour != nullptr);
    REQUIRE(prog_depth != nullptr);

    // 16 records (768 B) + a 12-index section at 768 = 816 B
    constexpr crd::u32 idx_off = 768U;
    auto               sb      = raster.create_storage_buffer(816U);
    REQUIRE(sb != nullptr);
    float rec[16U * 12U];
    for (crd::u32 r = 0; r < 16U; ++r)
    {
        rec[r * 12U + 0U] = 2.0F; // offscreen by default
        rec[r * 12U + 1U] = 2.0F;
        for (crd::u32 w = 2U; w < 12U; ++w) { rec[r * 12U + w] = 0.0F; }
    }
    const auto put = [&](crd::u32 r, float x, float y, float z) {
        rec[r * 12U + 0U] = x;
        rec[r * 12U + 1U] = y;
        rec[r * 12U + 2U] = z;
    };
    put(4U, 0.0F, -0.8F, 0.0F); // command 0: the big centred triangle
    put(5U, 0.8F, 0.8F, 0.0F);
    put(6U, -0.8F, 0.8F, 0.0F);
    put(8U, -0.95F, -0.35F, 0.0F); // command 1: the LEFT-EDGE spike, symmetric about y = 0 so a horizontal
    put(9U, -0.55F, 0.0F, 0.0F);   // midline probe hits it on BOTH backends (the F16 orientation lesson)
    put(10U, -0.95F, 0.35F, 0.0F);
    put(12U, -3.0F, -1.0F, 0.75F); // the resolve triangle: covers the whole viewport at z = 0.75
    put(13U, 3.0F, -1.0F, 0.75F);
    put(14U, 0.0F, 3.0F, 0.75F);
    REQUIRE(raster.upload_storage(*sb, 0U, static_cast<const void*>(rec), sizeof(rec)));
    const crd::u32 idx[12] = {4U, 5U, 6U, 8U, 9U, 10U, 0U, 1U, 2U, 12U, 13U, 14U};
    REQUIRE(raster.upload_storage(*sb, idx_off, static_cast<const void*>(idx), sizeof(idx)));

    // -- the ARGS buffer, written in THIS BACKEND'S declared command layout (REN-40-A). Vulkan: 20-byte
    // commands, args at offset 0. D3D12: 24-byte, args at 4, the leading u32 carrying DrawIndex. --
    const crd::u32 stride  = raster.indirect_command_stride();
    const crd::u32 arg_off = raster.indirect_command_arg_offset();
    auto           args_sb = raster.create_storage_buffer(2U * stride);
    auto           cnt_sb  = raster.create_storage_buffer(4U);
    REQUIRE(args_sb != nullptr);
    REQUIRE(cnt_sb != nullptr);
    crd::containers::Array<crd::u8> args_bytes(&alloc);
    args_bytes.resize(2U * stride, static_cast<crd::u8>(0));
    for (crd::u32 i = 0; i < 2U; ++i)
    {
        const crd::u32 cmd[5] = {3U, 1U, i * 3U, 0U, 0U}; // index_count, instance_count, first_index, 0, 0
        std::memcpy(args_bytes.data() + i * stride + arg_off, static_cast<const void*>(cmd), sizeof(cmd));
        if (arg_off != 0U) { std::memcpy(args_bytes.data() + i * stride, static_cast<const void*>(&i), 4U); }
    }
    REQUIRE(raster.upload_storage(*args_sb, 0U, args_bytes.data(), static_cast<crd::u32>(args_bytes.size())));

    struct IndState
    {
        g::FgImage         img{};
        g::IRasterProgram* colour = nullptr;
        g::IRasterProgram* depth  = nullptr;
        g::IStorageBuffer* sb     = nullptr;
        g::IStorageBuffer* args   = nullptr;
        g::IStorageBuffer* cnt    = nullptr;
    };
    const auto run = [&](crd::u32 count_value, crd::u32& centre, crd::u32& left, crd::u32& right) {
        REQUIRE(raster.upload_storage(*cnt_sb, 0U, static_cast<const void*>(&count_value), 4U));
        auto tgt = raster.create_color_depth_target(64U, 64U);
        REQUIRE(tgt != nullptr);
        auto fgraph = raster.create_frame_graph();
        REQUIRE(fgraph != nullptr);
        IndState st;
        st.img    = fgraph->import_target(*tgt);
        st.colour = prog_colour.get();
        st.depth  = prog_depth.get();
        st.sb     = sb.get();
        st.args   = args_sb.get();
        st.cnt    = cnt_sb.get();
        fgraph->add_pass("indirect-count")
            .writes(st.img)
            .execute(
                [](g::IFrameContext& ctx, void* user) {
                    auto* u = static_cast<IndState*>(user);
                    auto& r = ctx.raster();
                    // (1) clear colour + depth (offscreen geometry: indices [6..8] collapse to (2,2))
                    crd::gputest::enc_draw_storage_indexed_depth(r, *ctx.image(u->img), *u->colour,
                                                 g::ClearColor{0.02F, 0.0F, 0.0F, 1.0F}, 1.0F,
                                                 g::DepthCompare::Always, *u->sb, 768U + 24U, 3U, 1U, false);
                    // (2) THE VERB UNDER TEST - args and count both from device memory
                    crd::gputest::enc_draw_storage_multi_indexed_depth_only_indirect(r,
                        *ctx.image(u->img), *u->depth, 1.0F, g::DepthCompare::Always, *u->sb, 768U, *u->args, 0U,
                        u->cnt, 0U, 2U, true);
                    // (3) resolve: full-screen at z = 0.75, GREATER => colour only where 0.5 was written
                    crd::gputest::enc_draw_storage_indexed_depth(r, *ctx.image(u->img), *u->colour, g::ClearColor{}, 0.0F,
                                                 g::DepthCompare::Greater, *u->sb, 768U + 36U, 3U, 1U, true);
                },
                &st);
        REQUIRE(fgraph->build());
        fgraph->execute();
        centre = tgt->read_pixel(32U, 32U) & 0xFFU;
        left   = tgt->read_pixel(6U, 32U) & 0xFFU;
        right  = tgt->read_pixel(61U, 32U) & 0xFFU;
    };

    crd::u32 c1 = 0U;
    crd::u32 l1 = 0U;
    crd::u32 r1 = 0U;
    run(1U, c1, l1, r1);
    crd::u32 c2 = 0U;
    crd::u32 l2 = 0U;
    crd::u32 r2 = 0U;
    run(2U, c2, l2, r2);

    // count = 1 => ONLY command 0 executed: the centre is lit, the left-edge spike is NOT
    CHECK(c1 >= 250U);
    CHECK(l1 <= 20U);
    // count = 2 => BOTH executed
    CHECK(c2 >= 250U);
    CHECK(l2 >= 250U);
    // the arm that catches a verb IGNORING the count: the two runs MUST differ at the spike
    CHECK(l2 > l1);
    // outside both commands, at both counts - the dim clear only
    CHECK(r1 <= 20U);
    CHECK(r2 <= 20U);
    if (capture.error_count() > 0U)
    {
        const auto msgs = capture.messages();
        for (usize i = 0; i < msgs.size(); ++i)
        {
            WARN("[ren40-a capture] " << msgs[i].message_text.c_str());
        }
    }
    CHECK(capture.error_count() == 0U);
}

// ⭐⭐ REN-40-A GATE: the GEOMETRY indirect draw — colour + depth, args AND count from device memory.
//
// ⛔ WHY THIS VERB NEEDS ITS OWN GATE. The depth-only sibling above proves the mechanism; this one proves the
// GEOMETRY shape, and that shape is what the FORWARD pass uses. Without it the device cull saves nothing on the
// camera view (the CPU had to keep culling so the forward draw had a count), so a verb that silently drew NOTHING
// here would read as "the cull removed the geometry" — a plausible, wrong, and very expensive misreading.
// ⛔ The count arm is the point: run with count = 1 and count = 2 and require the second command's pixels to
// APPEAR. A verb that ignored `count_buf` and always ran `max_draws` would pass a single-count check.
TEST_CASE("REN-40-A GATE: the geometry indirect draw takes its args AND its count from device memory",
          "[gpu-context][vulkan][frame-graph][ren40][indirect][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;
    g::ValidationCapture capture(*rig.vk);
    REQUIRE(raster.indirect_count_supported());

    memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph           vg(&alloc);
    kir::KEntry           ve;
    gputest::build_vertex_pull_vs(vg, ve);
    auto        vs = rig.vk->create_program(vg, ve);
    kir::KGraph cgraph(&alloc);
    kir::KEntry cfe;
    gputest::build_triangle_fs(cgraph, cfe);
    auto cfs = rig.vk->create_program(cgraph, cfe);
    REQUIRE(vs != nullptr);
    REQUIRE(cfs != nullptr);
    auto prog = raster.create_raster_program(*vs, *cfs);
    REQUIRE(prog != nullptr);

    constexpr crd::u32 idx_off = 768U;
    auto               sb      = raster.create_storage_buffer(816U);
    REQUIRE(sb != nullptr);
    float rec[16U * 12U];
    for (crd::u32 r = 0; r < 16U; ++r)
    {
        rec[r * 12U + 0U] = 2.0F; // offscreen by default
        rec[r * 12U + 1U] = 2.0F;
        for (crd::u32 w = 2U; w < 12U; ++w) { rec[r * 12U + w] = 0.0F; }
    }
    const auto put = [&](crd::u32 r, float x, float y, float z) {
        rec[r * 12U + 0U] = x;
        rec[r * 12U + 1U] = y;
        rec[r * 12U + 2U] = z;
    };
    put(4U, 0.0F, -0.8F, 0.5F); // command 0: the centred triangle
    put(5U, 0.8F, 0.8F, 0.5F);
    put(6U, -0.8F, 0.8F, 0.5F);
    put(8U, -0.95F, -0.35F, 0.5F); // command 1: the LEFT-EDGE spike, symmetric about y = 0 so a horizontal
    put(9U, -0.55F, 0.0F, 0.5F);   // midline probe hits it on BOTH clip-space Y conventions
    put(10U, -0.95F, 0.35F, 0.5F);
    REQUIRE(raster.upload_storage(*sb, 0U, static_cast<const void*>(rec), sizeof(rec)));
    const crd::u32 idx[6] = {4U, 5U, 6U, 8U, 9U, 10U};
    REQUIRE(raster.upload_storage(*sb, idx_off, static_cast<const void*>(idx), sizeof(idx)));

    const crd::u32 stride  = raster.indirect_command_stride();
    const crd::u32 arg_off = raster.indirect_command_arg_offset();
    auto           args_sb = raster.create_storage_buffer(2U * stride);
    auto           cnt_sb  = raster.create_storage_buffer(4U);
    REQUIRE(args_sb != nullptr);
    REQUIRE(cnt_sb != nullptr);
    crd::containers::Array<crd::u8> args_bytes(&alloc);
    args_bytes.resize(2U * stride, static_cast<crd::u8>(0));
    for (crd::u32 i = 0; i < 2U; ++i)
    {
        const crd::u32 cmd[5] = {3U, 1U, i * 3U, 0U, 0U};
        std::memcpy(args_bytes.data() + i * stride + arg_off, static_cast<const void*>(cmd), sizeof(cmd));
        if (arg_off != 0U) { std::memcpy(args_bytes.data() + i * stride, static_cast<const void*>(&i), 4U); }
    }
    REQUIRE(raster.upload_storage(*args_sb, 0U, args_bytes.data(), static_cast<crd::u32>(args_bytes.size())));

    struct GeoState
    {
        g::FgImage         img{};
        g::IRasterProgram* prog = nullptr;
        g::IStorageBuffer* sb   = nullptr;
        g::IStorageBuffer* args = nullptr;
        g::IStorageBuffer* cnt  = nullptr;
    };
    const auto run = [&](crd::u32 count_value, crd::u32& centre, crd::u32& left, crd::u32& right) {
        REQUIRE(raster.upload_storage(*cnt_sb, 0U, static_cast<const void*>(&count_value), 4U));
        auto tgt = raster.create_color_depth_target(64U, 64U);
        REQUIRE(tgt != nullptr);
        auto fgraph = raster.create_frame_graph();
        REQUIRE(fgraph != nullptr);
        GeoState st;
        st.img  = fgraph->import_target(*tgt);
        st.prog = prog.get();
        st.sb   = sb.get();
        st.args = args_sb.get();
        st.cnt  = cnt_sb.get();
        fgraph->add_pass("geo-indirect-count")
            .writes(st.img)
            .execute(
                [](g::IFrameContext& ctx, void* user) {
                    auto* u = static_cast<GeoState*>(user);
                    crd::gputest::enc_draw_storage_multi_indexed_indirect(ctx.raster(),
                        *ctx.image(u->img), *u->prog, g::ClearColor{0.02F, 0.0F, 0.0F, 1.0F}, 0.0F,
                        g::DepthCompare::Always, *u->sb, 768U, nullptr, nullptr, *u->args, 0U, u->cnt, 0U, 2U,
                        false);
                },
                &st);
        REQUIRE(fgraph->build());
        fgraph->execute();
        centre = tgt->read_pixel(32U, 32U) & 0xFFU;
        left   = tgt->read_pixel(6U, 32U) & 0xFFU;
        right  = tgt->read_pixel(61U, 32U) & 0xFFU;
    };

    crd::u32 c1 = 0U;
    crd::u32 l1 = 0U;
    crd::u32 r1 = 0U;
    run(1U, c1, l1, r1);
    crd::u32 c2 = 0U;
    crd::u32 l2 = 0U;
    crd::u32 r2 = 0U;
    run(2U, c2, l2, r2);

    CHECK(c1 >= 250U); // command 0 drew
    CHECK(l1 <= 20U);  // command 1 did NOT (count = 1)
    CHECK(c2 >= 250U);
    CHECK(l2 >= 250U); // ...and DID at count = 2
    CHECK(l2 > l1);    // the arm that catches a verb ignoring the count
    CHECK(r1 <= 20U);  // outside both, the dim clear only
    CHECK(r2 <= 20U);
    if (capture.error_count() > 0U)
    {
        const auto msgs = capture.messages();
        for (usize i = 0; i < msgs.size(); ++i) { WARN("[ren40-geo capture] " << msgs[i].message_text.c_str()); }
    }
    CHECK(capture.error_count() == 0U);
}

// ⭐⭐ REN-40-A GATE: the COMPACTING CULL KERNEL agrees with the CPU cull EXACTLY.
//
// ⛔ WHY THIS IS THE GATE THAT MATTERS. A cull is a PERFORMANCE change: every pixel it moves is a bug. So this
// does not check "roughly the same number survived" - it runs the CPU's own `aabb_in_frustum` over the same
// boxes and the same matrix, and asserts the GPU produced the SAME SET, with the SAME count, exactly.
// ⛔ It also asserts the count is NOT trivially everything and NOT zero: a kernel that passed everything, or one
// that wrote nothing, would otherwise sail through as "fast".
// ⚠ The list is compared as a SET (sorted) because compaction claims subgroup bases in arrival order - the SET
// is deterministic, the ORDER is not, and the draw is order-independent.
TEST_CASE("REN-40-A GATE: the compacting cull kernel reproduces the CPU frustum cull exactly",
          "[gpu-context][vulkan][ren40][cull][compute][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;
    g::ValidationCapture capture(*rig.vk);

    memory::TlsfAllocator alloc(16U << 20U);

    // ── the authored cull asset: the COMPACTING variant, with THIS backend's command layout stamped on it ──
    crd::containers::String toml(&alloc);
    toml.append("stage = \"cull\"\nschema = 1\nname = \"crd://vertex/ren40_cull\"\n");
    toml.append("[header]\ninstance_off = 4\nview_proj = 6\ninstance_count = 100\n");
    toml.append("[vertex]\nstride = 12\n");
    toml.append("[[attribute]]\nname = \"position\"\noffset = 0\ncomps = 3\nkind = \"position\"\n");
    toml.append("[instance]\nstride = 20\ntransform = 0\n");
    // view 0 = the camera list, at `visible_off + 0 * capacity` — the CPU layout verbatim
    toml.append("[cull]\nfrustum = true\nworkgroup = 64\ncompact = true\nbounds_off = 104\n");
    toml.append("view_index = 0\ncapacity_word = 101\n");
    {
        char buf[128];
        (void)std::snprintf(buf, sizeof(buf), "draw_stride = %u\ndraw_arg_off = %u\n",
                            raster.indirect_command_stride(), raster.indirect_command_arg_offset());
        toml.append(buf);
    }
    crd::vertcook::VertexProgramDesc desc(&alloc);
    crd::containers::String          where(&alloc);
    REQUIRE(crd::vertcook::parse_vertex_toml(crd::containers::StringView(toml.c_str()), desc, &where)
            == crd::vertcook::VertexCookError::Ok);
    kir::KGraph kg(&alloc);
    kir::KEntry ke;
    REQUIRE(crd::vertcook::cook_vertex_program(desc, kg, ke));
    auto kern = rig.vk->create_program(kg, ke);
    REQUIRE(kern != nullptr);

    // ── the scene: N instances on a line, boxes of half-extent 0.4, spread so the frustum keeps only some ──
    constexpr crd::u32 n_boxes = 256U;
    constexpr crd::u32 bounds_off   = 1024U; // words; the header word `bounds_off` (104) HOLDS this
    constexpr crd::u32 visible_off  = 512U;  // words; header word 5 HOLDS this — where the cull writes
    const crd::u32     scene_words  = bounds_off + n_boxes * 6U + 16U;

    auto scene = raster.create_storage_buffer(scene_words * 4U);
    auto args  = raster.create_storage_buffer(64U);
    REQUIRE(scene != nullptr);
    REQUIRE(args != nullptr);

    // a plain orthographic-ish view_proj that keeps |x|,|y| <= w and 0 <= z <= w for a known band
    crd::math::Mat4f vpm = crd::math::Mat4f::identity();
    vpm.c0.x = 0.05F; // x in [-20, 20] maps to [-1, 1]
    vpm.c1.y = 0.05F;
    vpm.c2.z = 0.01F; // z in [0, 100] maps to [0, 1]
    vpm.c3.z = 0.0F;

    crd::containers::Array<crd::u32> words(&alloc);
    words.resize(scene_words, 0U);
    const auto putf = [&](crd::u32 idx, float v) {
        crd::u32 bits = 0U;
        std::memcpy(&bits, static_cast<const void*>(&v), 4U);
        words[idx] = bits;
    };
    // header: view_proj at 6 (column-major), instance_count at 100, bounds_off at 104
    const float* vpf = reinterpret_cast<const float*>(&vpm);
    for (crd::u32 e = 0; e < 16U; ++e) { putf(6U + e, vpf[e]); }
    words[100U] = n_boxes;
    words[104U] = bounds_off;
    words[5U]   = visible_off; // the visible-list base the kernel writes into
    words[101U] = n_boxes;     // the per-view list STRIDE (instance capacity)

    crd::containers::Array<crd::geometry::primitives::AABB3<crd::f32>> boxes(&alloc);
    for (crd::u32 i = 0; i < n_boxes; ++i)
    {
        // march x across the clip band and back out of it; y/z fixed inside
        const float cx = -40.0F + static_cast<float>(i) * 0.35F;
        crd::geometry::primitives::AABB3<crd::f32> b;
        b.min = {cx - 0.4F, -0.4F, 10.0F};
        b.max = {cx + 0.4F, 0.4F, 10.8F};
        boxes.push_back(b);
        const crd::u32 base = bounds_off + i * 6U;
        putf(base + 0U, b.min.x);
        putf(base + 1U, b.min.y);
        putf(base + 2U, b.min.z);
        putf(base + 3U, b.max.x);
        putf(base + 4U, b.max.y);
        putf(base + 5U, b.max.z);
    }
    REQUIRE(raster.upload_storage(*scene, 0U, words.data(), static_cast<crd::u32>(words.size() * 4U)));
    crd::u32 zero_list[n_boxes] = {}; // the list region is zeroed as part of `words` above
    crd::u32 args0[16] = {}; // the RESET pass's job in the real graph; here the command's constant fields
    args0[raster.indirect_command_arg_offset() / 4U + 0U] = 36U; // index_count (arbitrary, unused by the cull)
    args0[raster.indirect_command_arg_offset() / 4U + 1U] = 0U;  // instance_count - THE accumulator
    REQUIRE(raster.upload_storage(*args, 0U, static_cast<const void*>(args0), sizeof(args0)));

    // ── the CPU reference: the renderer's OWN plane extraction + positive-vertex test ──
    crd::math::Vec4f planes[6];
    crd::scenerender::frustum_planes(vpm, planes);
    crd::containers::Array<crd::u32> cpu(&alloc);
    for (crd::u32 i = 0; i < n_boxes; ++i)
    {
        if (crd::scenerender::aabb_in_frustum(boxes[i], planes)) { cpu.push_back(i); }
    }
    // the cull must be doing REAL work: not everything, not nothing
    CHECK(cpu.size() > 8U);
    CHECK(cpu.size() < n_boxes - 8U);

    // ── dispatch through OUR frame graph, as a compute pass ──
    struct CullState
    {
        g::FgBuffer        scene{};
        g::FgBuffer        args{};
        g::IGpuProgram*    kern = nullptr;
    };
    {
        auto fgraph = raster.create_frame_graph();
        REQUIRE(fgraph != nullptr);
        CullState st;
        st.scene = fgraph->import_storage(*scene);
        st.args  = fgraph->import_storage(*args);
        st.kern  = kern.get();
        fgraph->add_pass("ren40-cull", g::FgPassKind::Compute)
            .reads(st.scene)
            .writes(st.args)
            .execute(
                [](g::IFrameContext& ctx, void* user) {
                    auto*                u = static_cast<CullState*>(user);
                    g::IStorageBuffer*   bufs[2] = {ctx.buffer(u->scene), ctx.buffer(u->args)};
                    gputest::enc_dispatch(ctx.raster(), *u->kern, (n_boxes + 63U) / 64U, 1U, 1U,
                                                 static_cast<g::IStorageBuffer* const*>(bufs), 2U);
                },
                &st);
        REQUIRE(fgraph->build());
        fgraph->execute();
    }

    // ── ⭐⭐ the RESET pass, as its own authored kernel, ordered by the GRAPH ──
    // ⛔ THE ORDERING IS DATA, NOT A COMMENT: `cull_reset` WRITES the args and `cull` READS+WRITES them, so the
    // dependency sort places reset first and emits the barrier. This arm re-runs the whole thing with the args
    // buffer pre-filled with GARBAGE — if the reset did not run, or ran after the cull, the count is wrong.
    {
        crd::containers::String rtoml(&alloc);
        rtoml.append("stage = \"cull\"\nschema = 1\nname = \"crd://vertex/ren40_reset\"\n");
        rtoml.append("[header]\nindex_count = 0\nindex_off = 2\ninstance_off = 4\nview_proj = 6\n");
        rtoml.append("instance_count = 100\n");
        rtoml.append("[vertex]\nstride = 12\n");
        rtoml.append("[[attribute]]\nname = \"position\"\noffset = 0\ncomps = 3\nkind = \"position\"\n");
        rtoml.append("[instance]\nstride = 20\ntransform = 0\n");
        rtoml.append("[cull]\nfrustum = false\nreset = true\nworkgroup = 64\n");
        {
            char rb[128];
            (void)std::snprintf(rb, sizeof(rb), "draw_stride = %u\ndraw_arg_off = %u\ndraw_index = 7\n",
                                raster.indirect_command_stride(), raster.indirect_command_arg_offset());
            rtoml.append(rb);
        }
        crd::vertcook::VertexProgramDesc rdesc(&alloc);
        crd::containers::String          rwhere(&alloc);
        REQUIRE(crd::vertcook::parse_vertex_toml(crd::containers::StringView(rtoml.c_str()), rdesc, &rwhere)
                == crd::vertcook::VertexCookError::Ok);
        kir::KGraph rkg(&alloc);
        kir::KEntry rke;
        REQUIRE(crd::vertcook::cook_vertex_program(rdesc, rkg, rke));
        auto rkern = rig.vk->create_program(rkg, rke);
        REQUIRE(rkern != nullptr);

        // poison the command: a reset that does not run leaves these
        crd::u32 poison[16];
        for (crd::u32 i = 0; i < 16U; ++i) { poison[i] = 0xDEADBEEFU; }
        REQUIRE(raster.upload_storage(*args, 0U, static_cast<const void*>(poison), sizeof(poison)));
        REQUIRE(raster.upload_storage(*scene, visible_off * 4U, static_cast<const void*>(zero_list),
                                      sizeof(zero_list)));

        struct TwoPass
        {
            g::FgBuffer     scene{};
                g::FgBuffer     args{};
            g::IGpuProgram* reset = nullptr;
            g::IGpuProgram* cull  = nullptr;
        };
        auto fg2 = raster.create_frame_graph();
        REQUIRE(fg2 != nullptr);
        TwoPass tp;
        tp.scene = fg2->import_storage(*scene);
        tp.args  = fg2->import_storage(*args);
        tp.reset = rkern.get();
        tp.cull  = kern.get();
        // ⛔⛔ TWO WRITERS OF ONE RESOURCE KEEP DECLARATION ORDER — that is the graph's stated tie-break and
        // the only defensible one (reordering two writes would silently change the result). So `cull_reset` is
        // declared FIRST, exactly as `scene_gpu_cull.frame.toml` lists it. The DATA edge does the rest: `cull`
        // READS what `cull_reset` WROTE, so the sort places reset first and emits the barrier between them.
        // ⭐ The graph REFUSED to build when this was declared the other way round — cull-writes-args plus
        // cull-reads-args made a cycle with reset-writes-args. A frame graph that accepts an ambiguous order and
        // picks one silently is worse than one that says no.
        fg2->add_pass("cull_reset", g::FgPassKind::Compute)
            .reads(tp.scene)
            .writes(tp.args)
            .execute(
                [](g::IFrameContext& ctx, void* user) {
                    auto*              u       = static_cast<TwoPass*>(user);
                    g::IStorageBuffer* bufs[2] = {ctx.buffer(u->scene), ctx.buffer(u->args)};
                    gputest::enc_dispatch(ctx.raster(), *u->reset, 1U, 1U, 1U,
                                                 static_cast<g::IStorageBuffer* const*>(bufs), 2U);
                },
                &tp);
        fg2->add_pass("cull", g::FgPassKind::Compute)
            .reads(tp.scene)
            .reads(tp.args)
            .writes(tp.args)
            .execute(
                [](g::IFrameContext& ctx, void* user) {
                    auto*              u       = static_cast<TwoPass*>(user);
                    g::IStorageBuffer* bufs[2] = {ctx.buffer(u->scene), ctx.buffer(u->args)};
                    gputest::enc_dispatch(ctx.raster(), *u->cull, (n_boxes + 63U) / 64U, 1U, 1U,
                                                 static_cast<g::IStorageBuffer* const*>(bufs), 2U);
                },
                &tp);
        REQUIRE(fg2->build());
        fg2->execute();
        REQUIRE(raster.download_storage(*args));
        const crd::u32 aw = raster.indirect_command_arg_offset() / 4U;
        // the reset laid down the constants and zeroed the accumulator; the cull then counted into it
        CHECK(args->read_u32(aw + 1U) == static_cast<crd::u32>(cpu.size()));
        CHECK(args->read_u32(aw + 3U) == 0U); // base_vertex: ALWAYS 0 (the IndexedDraw contract)
        CHECK(args->read_u32(aw + 4U) == 0U); // first_instance: ALWAYS 0 — VK folds it into gl_InstanceIndex
        if (raster.indirect_command_arg_offset() != 0U)
        {
            CHECK(args->read_u32(0U) == 7U); // D3D12's leading DrawIndex root constant
        }
    }

    // ⛔ `read_u32` reflects the HOST-VISIBLE mirror, not device memory — a compute result needs an explicit
    // `download_storage` first. Without it the gate reads the pre-dispatch zeros and reports a perfectly
    // confident 0 survivors, which is exactly what it did the first time.
    REQUIRE(raster.download_storage(*args));
    REQUIRE(raster.download_storage(*scene));

    // ── the comparison: same COUNT, same SET ──
    const crd::u32 gpu_count = args->read_u32(raster.indirect_command_arg_offset() / 4U + 1U);
    CHECK(gpu_count == static_cast<crd::u32>(cpu.size()));

    crd::containers::Array<crd::u32> gpu(&alloc);
    for (crd::u32 i = 0; i < gpu_count && i < n_boxes; ++i)
    {
        gpu.push_back(scene->read_u32(visible_off + i));
    }
    // sort both (the ORDER is claim-order, the SET is the contract)
    const auto sort_u32 = [](crd::containers::Array<crd::u32>& a) {
        for (crd::usize i = 1; i < a.size(); ++i)
        {
            const crd::u32 v = a[i];
            crd::usize     j = i;
            while (j > 0 && a[j - 1] > v)
            {
                a[j] = a[j - 1];
                --j;
            }
            a[j] = v;
        }
    };
    sort_u32(gpu);
    REQUIRE(gpu.size() == cpu.size());
    // ⛔ compare as SETS, and REPORT the symmetric difference — a positional diff on two sorted lists turns one
    // differing element into a cascade of "mismatches" and hides how big the real disagreement is.
    crd::u32 only_gpu = 0U;
    crd::u32 only_cpu = 0U;
    {
        crd::usize i = 0;
        crd::usize j = 0;
        while (i < gpu.size() && j < cpu.size())
        {
            if (gpu[i] == cpu[j]) { ++i; ++j; }
            else if (gpu[i] < cpu[j]) { ++only_gpu; UNSCOPED_INFO("GPU-only instance " << gpu[i]); ++i; }
            else { ++only_cpu; UNSCOPED_INFO("CPU-only instance " << cpu[j]); ++j; }
        }
        only_gpu += static_cast<crd::u32>(gpu.size() - i);
        only_cpu += static_cast<crd::u32>(cpu.size() - j);
    }
    CHECK(only_gpu == 0U);
    CHECK(only_cpu == 0U);

    if (capture.error_count() > 0U)
    {
        const auto msgs = capture.messages();
        for (usize i = 0; i < msgs.size(); ++i) { WARN("[ren40-cull capture] " << msgs[i].message_text.c_str()); }
    }
    CHECK(capture.error_count() == 0U);
}

// ── ⭐⭐ REN-40-C2 GATE: THE LOD SELECTOR, AGAINST A CLOSED-FORM REFERENCE. ──────────────────────────────────
// ⛔⛔ WHY THIS GATE HAD TO EXIST. Every cheap check of the LOD path passed while coarse levels drew NOTHING:
// the frame rendered, the device-vs-CPU survivor TOTALS reconciled (the survivors WERE in their lists — nothing
// read them), and GPU time DROPPED, so it read as a win and produced a benchmark number that had to be
// withdrawn. What no aggregate could see is WHICH slot each instance landed in. This asserts exactly that.
//
// The metric is closed-form, so the reference is arithmetic rather than a second implementation:
//     px = r · |row1(vp)| · view_pixel_height / max(w_clip, eps)
// With the ortho-ish matrix below `w == 1` and `|row1| == vp.c1.y`, so `px = r · c1y · H` — a number this test
// computes per instance in C++ and compares against the slot the DEVICE chose.
// ⛔ The boxes VARY IN SIZE at a fixed position, so every slot is exercised by the metric itself rather than by
// a hand-assigned level: a selector that ignored the metric would pile everything into one slot, and the
// "every slot is non-empty" pre-check makes that impossible to pass.
TEST_CASE("REN-40-C2 GATE: the cull kernel selects the LOD slot the projected screen height declares",
          "[gpu-context][vulkan][ren40][lod][cull][compute][gpu]")
{
    Rig rig = make_rig();
    if (rig.raster == nullptr) { SKIP("no graphics-capable Vulkan device with shader objects"); }
    auto& raster = *rig.raster;
    g::ValidationCapture capture(*rig.vk);

    memory::TlsfAllocator alloc(16U << 20U);

    constexpr crd::u32 n_boxes     = 128U;
    constexpr crd::u32 lod_slots   = 4U;
    constexpr crd::u32 visible_off = 512U;  // words
    constexpr crd::u32 bounds_off  = 4096U; // words
    constexpr crd::u32 ovr_off     = 8192U; // words — the per-instance LOD override section
    constexpr float    pixel_h     = 1000.0F;
    // the DECLARED switch heights: level s is chosen while px < heights[s]. Descending, per the policy rule.
    const float heights[lod_slots] = {3.0e38F, 40.0F, 20.0F, 10.0F};

    crd::containers::String toml(&alloc);
    toml.append("stage = \"cull\"\nschema = 1\nname = \"crd://vertex/ren40_lodsel\"\n");
    toml.append("[header]\ninstance_off = 4\nview_proj = 6\ninstance_count = 100\n");
    toml.append("[vertex]\nstride = 12\n");
    toml.append("[[attribute]]\nname = \"position\"\noffset = 0\ncomps = 3\nkind = \"position\"\n");
    toml.append("[instance]\nstride = 20\ntransform = 0\n");
    toml.append("[cull]\nfrustum = true\nworkgroup = 64\ncompact = true\nbounds_off = 104\n");
    toml.append("view_index = 0\ncapacity_word = 101\n");
    toml.append("lod_slots = 4\nlod_count_word = 120\nlod_table_word = 121\nlod_height_word = 137\n");
    toml.append("pixel_height_word = 1\nlod_override_off = 145\n");
    {
        char buf[192];
        (void)std::snprintf(buf, sizeof(buf), "draw_stride = %u\ndraw_arg_off = %u\ndraw_arg_within = %u\n",
                            raster.indirect_command_stride(),
                            (8U * 4U) + raster.indirect_command_arg_offset(), // params block + the arg offset
                            raster.indirect_command_arg_offset());
        toml.append(static_cast<const char*>(buf));
    }
    crd::vertcook::VertexProgramDesc desc(&alloc);
    crd::containers::String          where(&alloc);
    REQUIRE(crd::vertcook::parse_vertex_toml(crd::containers::StringView(toml.c_str()), desc, &where)
            == crd::vertcook::VertexCookError::Ok);
    kir::KGraph kg(&alloc);
    kir::KEntry ke;
    REQUIRE(crd::vertcook::cook_vertex_program(desc, kg, ke));
    auto kern = rig.vk->create_program(kg, ke);
    REQUIRE(kern != nullptr);

    const crd::u32 scene_words = ovr_off + (n_boxes * 2U) + 16U;
    auto           scene       = raster.create_storage_buffer(scene_words * 4U);
    auto           args        = raster.create_storage_buffer(((8U + (lod_slots * 8U)) * 4U) + 256U);
    REQUIRE(scene != nullptr);
    REQUIRE(args != nullptr);

    crd::math::Mat4f vpm = crd::math::Mat4f::identity();
    vpm.c0.x             = 0.05F;
    vpm.c1.y             = 0.05F;
    vpm.c2.z             = 0.01F;

    crd::containers::Array<crd::u32> words(&alloc);
    words.resize(scene_words, 0U);
    const auto putf = [&](crd::u32 idx, float v) {
        crd::u32 bits = 0U;
        std::memcpy(static_cast<void*>(&bits), static_cast<const void*>(&v), 4U);
        words[idx] = bits;
    };
    const auto* vpf = reinterpret_cast<const float*>(&vpm);
    for (crd::u32 e = 0; e < 16U; ++e) { putf(6U + e, vpf[e]); }
    words[100U] = n_boxes;
    words[104U] = bounds_off;
    words[5U]   = visible_off;
    words[101U] = n_boxes; // the per-(view, slot) list stride
    words[120U] = lod_slots;
    words[145U] = ovr_off;
    for (crd::u32 s = 0; s < lod_slots; ++s)
    {
        words[121U + (s * 2U) + 0U] = 1000U + (s * 100U); // first_index (the draw is not exercised here)
        words[121U + (s * 2U) + 1U] = 300U - (s * 60U);   // index_count
        putf(137U + s, heights[s]);
    }

    // ── the scene: one position, VARYING half-extent, so the metric alone spans every slot ──
    crd::containers::Array<crd::u32> want_slot(&alloc);
    for (crd::u32 i = 0; i < n_boxes; ++i)
    {
        const float h = 0.05F + (static_cast<float>(i) * 0.0045F);
        crd::geometry::primitives::AABB3<crd::f32> b;
        b.min               = {-h, -h, 10.0F - h};
        b.max               = {h, h, 10.0F + h};
        const crd::u32 base = bounds_off + (i * 6U);
        putf(base + 0U, b.min.x);
        putf(base + 1U, b.min.y);
        putf(base + 2U, b.min.z);
        putf(base + 3U, b.max.x);
        putf(base + 4U, b.max.y);
        putf(base + 5U, b.max.z);
        // no per-entity override: bias 1.0, levels 0..7 — the same constant the renderer writes for a slot
        // whose entity carries no `MeshLodOverride`
        putf(ovr_off + (i * 2U) + 0U, 1.0F);
        words[ovr_off + (i * 2U) + 1U] = 0U | (7U << 8U);

        // THE REFERENCE, in closed form
        const float ex = b.max.x - b.min.x;
        const float ey = b.max.y - b.min.y;
        const float ez = b.max.z - b.min.z;
        const float r  = 0.5F * crd::math::sqrt((ex * ex) + (ey * ey) + (ez * ez));
        const float px = r * vpm.c1.y * pixel_h; // |row1| == c1.y for this matrix; w == 1
        crd::u32    s  = 0U;
        for (crd::u32 k = 1; k < lod_slots; ++k)
        {
            if (px < heights[k]) { s = k; }
        }
        want_slot.push_back(s);
    }
    // ⛔ the gate must EXERCISE every slot, or it proves nothing about selection
    for (crd::u32 s = 0; s < lod_slots; ++s)
    {
        crd::u32 c = 0U;
        for (crd::u32 i = 0; i < n_boxes; ++i) { c += want_slot[i] == s ? 1U : 0U; }
        INFO("slot " << s << " expects " << c);
        CHECK(c > 0U);
    }
    REQUIRE(raster.upload_storage(*scene, 0U, words.data(), static_cast<crd::u32>(words.size() * 4U)));

    // the params block: [0] base = 0, [1] this view's pixel height
    crd::u32 params[8] = {};
    {
        crd::u32 bits = 0U;
        std::memcpy(static_cast<void*>(&bits), static_cast<const void*>(&pixel_h), 4U);
        params[1] = bits;
    }
    REQUIRE(raster.upload_storage(*args, 0U, static_cast<const void*>(params), sizeof(params)));

    struct St
    {
        g::FgBuffer     scene{};
        g::FgBuffer     args{};
        g::IGpuProgram* kern = nullptr;
    };
    {
        auto fgraph = raster.create_frame_graph();
        REQUIRE(fgraph != nullptr);
        St st;
        st.scene = fgraph->import_storage(*scene);
        st.args  = fgraph->import_storage(*args);
        st.kern  = kern.get();
        fgraph->add_pass("ren40-lodsel", g::FgPassKind::Compute)
            .reads(st.scene)
            .writes(st.args)
            .execute(
                [](g::IFrameContext& ctx, void* user) {
                    auto*              u       = static_cast<St*>(user);
                    g::IStorageBuffer* bufs[2] = {ctx.buffer(u->scene), ctx.buffer(u->args)};
                    gputest::enc_dispatch(ctx.raster(), *u->kern, (n_boxes + 63U) / 64U, 1U, 1U,
                                                 static_cast<g::IStorageBuffer* const*>(bufs), 2U);
                },
                &st);
        REQUIRE(fgraph->build());
        fgraph->execute();
    }

    // ── the verdict: PER SLOT, both the COUNT and the SET ──
    REQUIRE(raster.download_storage(*args));
    REQUIRE(raster.download_storage(*scene));
    const crd::u32 stride_w = raster.indirect_command_stride() / 4U;
    const crd::u32 argw     = 8U + (raster.indirect_command_arg_offset() / 4U);
    crd::u32       total    = 0U;
    for (crd::u32 s = 0; s < lod_slots; ++s)
    {
        crd::u32 want = 0U;
        for (crd::u32 i = 0; i < n_boxes; ++i) { want += want_slot[i] == s ? 1U : 0U; }
        const crd::u32 got = args->read_u32(argw + (s * stride_w) + 1U);
        INFO("slot " << s);
        CHECK(got == want);
        total += got;

        // ...and the LIST holds exactly the instances the reference assigns to this slot, as a SET — the
        // compaction claims slots in arrival order, which is not deterministic; the SET is. ⛔ Checked by
        // MEMBERSHIP rather than by sorting: it is order-independent by construction, and it catches a
        // DUPLICATE (an instance appended twice) which a sorted compare would quietly accept as "present".
        crd::containers::Array<crd::u32> seen(&alloc);
        seen.resize(n_boxes, 0U);
        for (crd::u32 k = 0; k < got; ++k)
        {
            const crd::u32 v = scene->read_u32(visible_off + (s * n_boxes) + k);
            REQUIRE(v < n_boxes);
            ++seen[v];
        }
        for (crd::u32 i = 0; i < n_boxes; ++i)
        {
            const crd::u32 expect = want_slot[i] == s ? 1U : 0U;
            INFO("instance " << i << " slot " << s);
            CHECK(seen[i] == expect);
        }
    }
    // every surviving instance landed in EXACTLY ONE slot — the property that makes a per-view sum meaningful
    CHECK(total == n_boxes);
    CHECK(capture.error_count() == 0U);
}
