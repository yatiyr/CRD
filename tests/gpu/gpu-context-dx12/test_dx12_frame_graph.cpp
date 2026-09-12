// test_dx12_frame_graph.cpp — REN-1 pt-2 (D-007 row 98): the DX12 FRAME GRAPH gate (the Vulkan gate's mirror).
//  · ONE SUBMISSION: two vertex-pull scene passes (clear + depth-LOAD) compose into a single ExecuteCommandLists
//    (last_submit_count==1) with the graph inserting the cross-pass barriers — and the readback is BIT-IDENTICAL to
//    the synchronous submit+wait-per-draw path.
//  · THE PER-DRAW DESCRIPTOR RING: the two passes read DIFFERENT storage buffers (a red field, then a green centre).
//    DX12's single storage-UAV heap slot is consumed at EXECUTE time, so a broken ring would make BOTH draws read the
//    last buffer (all green). The red field surviving off-centre proves each recorded draw got its OWN heap slot.
//  · TRANSIENT ALIASING: graph-owned transients whose lifetimes are DISJOINT share a placed-resource heap (physical <
//    logical); OVERLAPPING-lifetime transients do NOT alias.
// (DX12 has no ValidationCapture — Vulkan-only; the D3D12 debug layer breaks on error, and the bit-match + submit
//  count are the observable proof.)

#include <crd/gpu/dx12_raster_context.hpp>

#include <crd/gpu/dx12_context.hpp> // create_dx12_gpu_context — the KGraph -> DXIL program seam
#include <crd/gpu/dx12_ray_tracing_context.hpp>
#include <crd/vertexcook/vertex_asset.hpp> // REN-38-F13: the authored RT stages // REN-38-A9: the host builds the scene the asset names
#include <crd/gpu/frame_graph.hpp>
#include <crd/gpu/raster_context.hpp>

#include <crd/framecook/frame_asset.hpp>   // REN-36.2: the cooked frame-graph asset
#include <crd/framecook/frame_runtime.hpp> // REN-36.2: executing it through IFrameGraph
#include <crd/renderasset/diagnostic.hpp>  // CEIR-18c: DiagnosticList for build_frame_plans at the direct rec.record site
#include <crd/kir/ckir.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <ckir_oit_test.hpp>        // REN-38-A12: the shared WBOIT accumulate + composite shaders
#include <ckir_raster_triangle.hpp> // REN-2: the shared triangle (offscreen) + textured/sample (compose) CKIR builders
#include <ckir_vertex_pull.hpp>     // REN-40-A: the GEO-1 pull VS the indirect-count gate draws with
#include <verb_packet_helpers.hpp>  // RAF-12.4: crd::gputest::enc_draw{,_textured,_shadow,_bindless} (shared)
#include <win32_test_window.hpp>    // REN-38-A5: a REAL window — DXGI has no headless surface

#include <catch2/catch_test_macros.hpp>

#include <chrono> // REN-1 pt-2 batching microbenchmark: CPU wall-clock of the submit-batching win
#include <cstdio> // REN-3.1 bench board printf
#include <cstring> // REN-36.2: strlen over the embedded asset text

namespace g   = crd::gpu;
namespace kir = crd::kir;

namespace
{

// The vertex-pull scene shader (the GEO-7 DX12 pull path): VS fetches {x,y,z,c} per vertex from the storage buffer
// by VertexIndex; FS writes (c, 1-c, 0, 1) — c=1 → red, c=0 → green.
void build_pull_depth_vs(kir::KGraph& gr, kir::KEntry& ve)
{
    const auto sh  = kir::make_shape({1});
    const auto ku  = [&](crd::u32 v) { return gr.constant(static_cast<double>(v), sh, kir::DType::U32); };
    const int  vid  = gr.cast(gr.builtin(kir::KBuiltin::VertexIndex), kir::DType::U32);
    const int  base = gr.binary(kir::KOp::Mul, vid, ku(4U));
    const auto ldf  = [&](int idx) { return gr.int_bits_to_float(gr.cast(gr.storage_load(idx), kir::DType::I32)); };
    const int  x    = ldf(base);
    const int  y    = ldf(gr.binary(kir::KOp::Add, base, ku(1U)));
    const int  z    = ldf(gr.binary(kir::KOp::Add, base, ku(2U)));
    const int  c    = ldf(gr.binary(kir::KOp::Add, base, ku(3U)));
    ve.stage    = kir::KStage::Vertex;
    ve.position = gr.vec4(x, y, z, gr.constant(1.0, sh, kir::DType::F32));
    ve.n_out    = 1;
    ve.out[0]   = {c, 0, kir::Interp::Flat};
}
void build_pull_depth_fs(kir::KGraph& gr, kir::KEntry& fe)
{
    const auto sh = kir::make_shape({1});
    const int  c  = gr.stage_in(kir::KType::make_scalar(kir::DType::F32), 0, kir::Interp::Flat);
    const int  k1 = gr.constant(1.0, sh, kir::DType::F32);
    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {gr.vec4(c, gr.binary(kir::KOp::Sub, k1, c), gr.constant(0.0, sh, kir::DType::F32), k1), 0};
}

// per-pass recording state (carried through FgExecuteFn's void* user)
struct ScenePass
{
    g::FgImage         img{};
    g::FgBuffer        buf{};
    g::IRasterProgram* prog = nullptr;
};

void record_clear(g::IFrameContext& ctx, void* user)
{
    auto* s = static_cast<ScenePass*>(user);
    crd::gputest::enc_draw_storage_depth(ctx.raster(), *ctx.image(s->img), *s->prog, g::ClearColor{0.0F, 0.0F, 1.0F, 1.0F}, 0.0F,
                                    g::DepthCompare::GreaterEqual, *ctx.buffer(s->buf), 3U);
}
void record_load(g::IFrameContext& ctx, void* user)
{
    auto* s = static_cast<ScenePass*>(user);
    crd::gputest::enc_draw_storage_depth_load(ctx.raster(), *ctx.image(s->img), *s->prog, g::DepthCompare::GreaterEqual,
                                         *ctx.buffer(s->buf), 3U);
}
void record_noop(g::IFrameContext& /*ctx*/, void* /*user*/) {}

// a big red field (z=0.8, c=1) covering most of the screen
constexpr crd::f32 kRedField[12] = {-3.0F, -1.0F, 0.8F, 1.0F, 3.0F, -1.0F, 0.8F, 1.0F, 0.0F, 3.0F, 0.8F, 1.0F};
// a small green centre triangle (z=0.9, c=0), NEARER — wins the centre through the loaded depth
constexpr crd::f32 kGreenTip[12] = {-0.4F, -0.4F, 0.9F, 0.0F, 0.4F, -0.4F, 0.9F, 0.0F, 0.0F, 0.4F, 0.9F, 0.0F};

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
    crd::gputest::enc_draw_textured(ctx.raster(), *ctx.image(s->dst), *s->prog, g::ClearColor{0.0F, 0.0F, 1.0F, 1.0F}, *ctx.texture(s->rtt), 3U);
}

} // namespace

// REN-3.1 (DX12): pass 1 RENDERS a depth map into a D32Float+sampled transient — no render target bound at all.
struct ShadowDepthPass
{
    g::FgImage         map{};
    g::FgBuffer        buf{};
    g::IRasterProgram* prog = nullptr;
};
void record_shadow_depth(g::IFrameContext& ctx, void* user)
{
    auto* s = static_cast<ShadowDepthPass*>(user);
    crd::gputest::enc_draw_storage_depth_only(ctx.raster(), *ctx.image(s->map), *s->prog, 1.0F, g::DepthCompare::LessEqual,
                                         *ctx.buffer(s->buf), 3U);
}
// REN-3.2 (DX12): one CASCADE pass — renders a constant depth into ONE SLICE of the layered depth atlas.
// `image_layer` is the only difference from the single-map pass above.
struct CascadeDepthPass
{
    g::FgImage         atlas{};
    g::FgBuffer        buf{};
    g::IRasterProgram* prog  = nullptr;
    crd::u32           layer = 0;
};
void record_cascade_depth(g::IFrameContext& ctx, void* user)
{
    auto* s = static_cast<CascadeDepthPass*>(user);
    crd::gputest::enc_draw_storage_depth_only(ctx.raster(), *ctx.image_layer(s->atlas, s->layer), *s->prog, 1.0F,
                                         g::DepthCompare::LessEqual, *ctx.buffer(s->buf), 3U);
}
// REN-3.1 (DX12): pass 2 SAMPLES the rendered depth map through the COMPARISON sampler and shades.
struct ShadowSamplePass
{
    g::FgImage         map{};
    g::FgImage         dst{};
    g::IRasterProgram* prog = nullptr;
};
void record_shadow_sample(g::IFrameContext& ctx, void* user)
{
    auto* s = static_cast<ShadowSamplePass*>(user);
    crd::gputest::enc_draw_shadow(ctx.raster(), *ctx.image(s->dst), *s->prog, g::ClearColor{0.0F, 0.0F, 1.0F, 1.0F},
                                  *ctx.texture(s->map), 3U);
}

TEST_CASE("REN-1 GATE (DX12): frame graph composes two pull passes in ONE submission, readback bit-matches sync",
          "[dx12][raster][frame-graph][ren1][gpu]")
{
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);

    crd::memory::TlsfAllocator alloc(4U << 20U);
    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    build_pull_depth_vs(vg, ve);
    kir::KGraph fgg(&alloc);
    kir::KEntry fe;
    build_pull_depth_fs(fgg, fe);
    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { SKIP("dxc/DXIL unavailable"); }
    auto fs = gctx->create_program(fgg, fe);
    REQUIRE(fs != nullptr);
    auto prog = raster->create_raster_program(*vs, *fs);
    REQUIRE(prog != nullptr);

    constexpr crd::u32 dim = 64U;

    // two DIFFERENT storage buffers — the red field and the green tip (the per-draw descriptor-ring probe)
    auto red = raster->create_storage_buffer(sizeof(kRedField));
    auto grn = raster->create_storage_buffer(sizeof(kGreenTip));
    REQUIRE(red != nullptr);
    REQUIRE(grn != nullptr);
    REQUIRE(raster->upload_storage(*red, 0U, kRedField, sizeof(kRedField)));
    REQUIRE(raster->upload_storage(*grn, 0U, kGreenTip, sizeof(kGreenTip)));

    // ── the SYNCHRONOUS reference on target A (submit+wait per draw) ──
    auto ref = raster->create_color_depth_target(dim, dim);
    REQUIRE(ref != nullptr);
    crd::gputest::enc_draw_storage_depth(*raster, *ref, *prog, g::ClearColor{0.0F, 0.0F, 1.0F, 1.0F}, 0.0F, g::DepthCompare::GreaterEqual, *red, 3U);
    crd::gputest::enc_draw_storage_depth_load(*raster, *ref, *prog, g::DepthCompare::GreaterEqual, *grn, 3U);
    // The big red field (z=0.8) covers the whole 64x64 screen; the small green tip (z=0.9) wins ONLY the centre
    // through the loaded depth. So centre=green (pass 1's buffer), off-centre=red (pass 0's buffer) — the two points
    // that prove the per-draw descriptor RING (each pass read its OWN storage buffer).
    const crd::u32 sync_centre = ref->read_pixel(dim / 2U, dim / 2U); // green tip wins
    const crd::u32 sync_left   = ref->read_pixel(6U, dim / 2U);       // red field only
    const crd::u32 sync_right  = ref->read_pixel(dim - 6U, dim / 2U); // red field only
    CHECK(((sync_centre >> 8U) & 0xFFU) > 200U); // green centre
    CHECK((sync_centre & 0xFFU) < 60U);          // ... and NOT red
    CHECK((sync_left & 0xFFU) > 200U);           // red field left
    CHECK((sync_right & 0xFFU) > 200U);          // red field right

    // ── the FRAME GRAPH on target B (ONE submission, two passes, two DIFFERENT buffers) ──
    auto out = raster->create_color_depth_target(dim, dim);
    REQUIRE(out != nullptr);
    auto fgraph = raster->create_frame_graph();
    REQUIRE(fgraph != nullptr);

    const g::FgImage  img  = fgraph->import_target(*out);
    const g::FgBuffer fred = fgraph->import_storage(*red);
    const g::FgBuffer fgrn = fgraph->import_storage(*grn);
    ScenePass         p_red{img, fred, prog.get()};
    ScenePass         p_grn{img, fgrn, prog.get()};
    fgraph->add_pass("scene").reads(fred).writes(img).execute(&record_clear, &p_red);
    fgraph->add_pass("compose").reads(fgrn).read_writes(img).execute(&record_load, &p_grn);
    REQUIRE(fgraph->build());
    fgraph->execute();

    CHECK(fgraph->last_submit_count() == 1U);  // scene + compose in ONE ExecuteCommandLists
    CHECK(fgraph->last_barrier_count() >= 1U);  // at least the COMMON->RENDER_TARGET + readback barriers
    CHECK(out->read_pixel(dim / 2U, dim / 2U) == sync_centre);   // BIT-IDENTICAL to the synchronous path (green centre)
    CHECK(out->read_pixel(6U, dim / 2U) == sync_left);           // the red field SURVIVED (ring: pass 0 read `red`)
    CHECK(out->read_pixel(dim - 6U, dim / 2U) == sync_right);

    // reuse across frames: reset + rebuild + re-execute still submits exactly once
    fgraph->reset();
    const g::FgImage  img2  = fgraph->import_target(*out);
    const g::FgBuffer fred2 = fgraph->import_storage(*red);
    ScenePass         p2{img2, fred2, prog.get()};
    fgraph->add_pass("scene").reads(fred2).writes(img2).execute(&record_clear, &p2);
    REQUIRE(fgraph->build());
    fgraph->execute();
    CHECK(fgraph->last_submit_count() == 1U);
}

TEST_CASE("REN-1 (DX12): transient resources ALIAS a placed-resource heap across disjoint lifetimes",
          "[dx12][raster][frame-graph][ren1][gpu]")
{
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);

    g::FgImageDesc desc{};
    desc.width  = 128U;
    desc.height = 128U;
    desc.format = g::FgImageFormat::RGBA8Unorm;

    // DISJOINT lifetimes: X written in pass 0, Y written in pass 1 → they never coexist → SHARE a heap.
    {
        auto fgraph = raster->create_frame_graph();
        REQUIRE(fgraph != nullptr);
        const g::FgImage x = fgraph->create_transient_image(desc);
        const g::FgImage y = fgraph->create_transient_image(desc);
        REQUIRE(x.valid());
        REQUIRE(y.valid());
        fgraph->add_pass("a").writes(x).execute(&record_noop, nullptr);
        fgraph->add_pass("b").writes(y).execute(&record_noop, nullptr);
        REQUIRE(fgraph->build());

        CHECK(fgraph->transient_logical_bytes() > 0U);
        CHECK(fgraph->transient_memory_bytes() < fgraph->transient_logical_bytes());       // ALIASED
        CHECK(fgraph->transient_memory_bytes() * 2U == fgraph->transient_logical_bytes()); // 2 equal → 1 heap
    }

    // OVERLAPPING lifetimes: P and Q both written in the SAME pass → they coexist → NO aliasing.
    {
        auto fgraph = raster->create_frame_graph();
        REQUIRE(fgraph != nullptr);
        const g::FgImage p = fgraph->create_transient_image(desc);
        const g::FgImage q = fgraph->create_transient_image(desc);
        fgraph->add_pass("both").writes(p).writes(q).execute(&record_noop, nullptr);
        REQUIRE(fgraph->build());
        CHECK(fgraph->transient_memory_bytes() == fgraph->transient_logical_bytes()); // coexisting → not shared
    }

    // a transient that NO pass writes is a build error (never a partial schedule)
    {
        auto fgraph = raster->create_frame_graph();
        const g::FgImage orphan = fgraph->create_transient_image(desc);
        REQUIRE(orphan.valid());
        fgraph->add_pass("reads-only").reads(orphan).execute(&record_noop, nullptr);
        CHECK_FALSE(fgraph->build());
    }
}

TEST_CASE("REN-2 GATE (DX12): render-to-texture -- a pass renders a transient, a LATER pass SAMPLES it",
          "[dx12][raster][frame-graph][ren2][gpu]")
{
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);

    crd::memory::TlsfAllocator alloc(8U << 20U);
    // offscreen: a red triangle (build_triangle) over a green clear, INTO the transient
    kir::KGraph tvg(&alloc);
    kir::KEntry tve;
    crd::gputest::build_triangle_vs(tvg, tve);
    kir::KGraph tfg(&alloc);
    kir::KEntry tfe;
    crd::gputest::build_triangle_fs(tfg, tfe);
    auto tvs = gctx->create_program(tvg, tve);
    if (tvs == nullptr) { SKIP("dxc/DXIL unavailable"); }
    auto tfs      = gctx->create_program(tfg, tfe);
    auto tri_prog = raster->create_raster_program(*tvs, *tfs);
    // compose: a FULL-SCREEN quad sampling the bound texture at UV
    kir::KGraph svg(&alloc);
    kir::KEntry sve;
    crd::gputest::build_textured_vs(svg, sve);
    kir::KGraph sfg(&alloc);
    kir::KEntry sfe;
    crd::gputest::build_sample_fs(sfg, sfe);
    auto svs         = gctx->create_program(svg, sve);
    auto sfs         = gctx->create_program(sfg, sfe);
    auto sample_prog = raster->create_raster_program(*svs, *sfs);
    REQUIRE(tri_prog != nullptr);
    REQUIRE(sample_prog != nullptr);

    constexpr crd::u32 dim   = 64U;
    auto               dst   = raster->create_color_target(dim, dim);
    auto               dummy = raster->create_storage_buffer(16U);
    REQUIRE(dst != nullptr);
    REQUIRE(dummy != nullptr);

    auto fgraph = raster->create_frame_graph();
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
    CHECK(fgraph->last_barrier_count() >= 1U);  // incl. the RENDER_TARGET->PIXEL_SHADER_RESOURCE RTT barrier
    const crd::u32 center = dst->read_pixel(dim / 2U, dim / 2U); // samples the transient's RED triangle centre
    const crd::u32 corner = dst->read_pixel(2U, 2U);            // samples the transient's GREEN clear corner
    CHECK((center & 0xFFU) > 180U);          // red -> the compose pass SAMPLED what the offscreen pass rendered
    CHECK(((corner >> 8U) & 0xFFU) > 180U);  // green -> the RTT round-trip is faithful (not the blue compose clear)
    CHECK((corner & 0xFFU) < 80U);           // ... and NOT red at the corner
}

TEST_CASE("REN-2 Half B (DX12): the textured scene draw SAMPLES the material base-color map, not a flat colour",
          "[dx12][raster][ren2][material][gpu]")
{
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);

    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_pull_textured_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_pull_textured_fs(fg, fe);
    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { SKIP("dxc/DXIL unavailable"); }
    auto fs   = gctx->create_program(fg, fe);
    auto prog = raster->create_raster_program(*vs, *fs);
    REQUIRE(prog != nullptr);

    constexpr crd::u32 dim = 64U;
    auto               tgt = raster->create_color_depth_target(dim, dim);
    const crd::u8      texels[8] = {255U, 0U, 0U, 255U, 0U, 255U, 0U, 255U}; // left RED, right GREEN
    auto               tex       = raster->create_texture(2U, 1U, texels);
    const float        verts[15] = {-1.0F, -1.0F, 0.0F, 0.0F, 0.0F, 3.0F, -1.0F, 0.0F, 2.0F,
                                    0.0F,  -1.0F, 3.0F, 0.0F, 0.0F, 2.0F};
    auto               sb        = raster->create_storage_buffer(sizeof(verts));
    REQUIRE(tgt != nullptr);
    REQUIRE(tex != nullptr);
    REQUIRE(sb != nullptr);
    REQUIRE(raster->upload_storage(*sb, 0U, verts, sizeof(verts)));

    crd::gputest::enc_draw_storage_textured_depth(*raster, *tgt, *prog, g::ClearColor{0.0F, 0.0F, 1.0F, 1.0F}, 0.0F, g::DepthCompare::Always,
                                        *sb, *tex, 3U);

    const crd::u32 left  = tgt->read_pixel(16U, dim / 2U); // UV.x~0.25 → RED base-color texel
    const crd::u32 right = tgt->read_pixel(48U, dim / 2U); // UV.x~0.75 → GREEN base-color texel
    CHECK((left & 0xFFU) > 180U);          // sampled RED — the forward pass sampled the map, not the flat/blue clear
    CHECK(((left >> 8U) & 0xFFU) < 80U);
    CHECK(((right >> 8U) & 0xFFU) > 180U); // sampled GREEN
    CHECK((right & 0xFFU) < 80U);
}

// REN-1 pt-2 BATCHING BENCHMARK (hidden [.] — run with [ren1-bench]). The DX12 mirror of the Vulkan board: an
// N-draw frame timed two ways — (a) the synchronous substrate (N ExecuteCommandLists + N fence WAITs) vs (b) the
// frame graph (N draws recorded into one list, ONE ExecuteCommandLists + wait). The win is the collapse of N
// CPU<->GPU fence stalls to one; it grows with N. Timings -> docs/bench; the only hard assertion is a NON-regression
// at N=64.
// REN-3.1 GATE (DX12) — the mirror of the Vulkan gate, and the one that proves the DX12-ONLY defect is fixed.
// ⛔ A D3D12 resource created with a fully-typed depth format (D32_FLOAT) can NEVER carry an SRV, so before this
// slice a `D32Float`+`sampled` transient got NEITHER a target NOR a texture (the transient build path literally
// skipped depth with `if (!n.is_depth)`). The fix is the three-format dance over ONE resource:
//     resource = R32_TYPELESS  ·  DSV = D32_FLOAT  ·  SRV = R32_FLOAT
// `texture()` returning non-null below is the negative gate for exactly that.
TEST_CASE("REN-3.1 GATE (DX12): a depth-only pass RENDERS a shadow map, a later pass SAMPLES it (R32_TYPELESS)",
          "[dx12][raster][frame-graph][ren3][gpu]")
{
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);

    crd::memory::TlsfAllocator alloc(8U << 20U);
    constexpr crd::u32         dim = 32U;

    kir::KGraph dvg(&alloc);
    kir::KEntry dve;
    crd::gputest::build_fullscreen_vs(dvg, dve);
    kir::KGraph dfg(&alloc);
    kir::KEntry dfe;
    crd::gputest::build_depth_only_const_fs(dfg, dfe, 0.5);
    auto dvs = gctx->create_program(dvg, dve);
    if (dvs == nullptr) { SKIP("dxc/DXIL unavailable"); }
    auto dfs        = gctx->create_program(dfg, dfe);
    auto depth_prog = raster->create_raster_program(*dvs, *dfs);

    kir::KGraph svg(&alloc);
    kir::KEntry sve;
    crd::gputest::build_textured_vs(svg, sve);
    kir::KGraph sfg(&alloc);
    kir::KEntry sfe;
    crd::gputest::build_shadow_fs(sfg, sfe);
    auto svs         = gctx->create_program(svg, sve);
    auto sfs         = gctx->create_program(sfg, sfe);
    auto shadow_prog = raster->create_raster_program(*svs, *sfs);
    REQUIRE(depth_prog != nullptr);
    REQUIRE(shadow_prog != nullptr);

    auto dst   = raster->create_color_target(dim, dim);
    auto dummy = raster->create_storage_buffer(16U);
    REQUIRE(dst != nullptr);
    REQUIRE(dummy != nullptr);

    auto fgraph = raster->create_frame_graph();
    REQUIRE(fgraph != nullptr);

    g::FgImageDesc ddesc{};
    ddesc.width   = dim;
    ddesc.height  = dim;
    ddesc.format  = g::FgImageFormat::D32Float;
    ddesc.sampled = true;
    const g::FgImage shadow_map = fgraph->create_transient_image(ddesc);
    REQUIRE(shadow_map.valid());
    const g::FgImage  fin = fgraph->import_target(*dst);
    const g::FgBuffer buf = fgraph->import_storage(*dummy);

    ShadowDepthPass  dpass{shadow_map, buf, depth_prog.get()};
    ShadowSamplePass spass{shadow_map, fin, shadow_prog.get()};
    fgraph->add_pass("shadow_depth").reads(buf).writes(shadow_map).execute(&record_shadow_depth, &dpass);
    fgraph->add_pass("shade").reads(shadow_map).writes(fin).execute(&record_shadow_sample, &spass);
    REQUIRE(fgraph->build());
    fgraph->execute();

    CHECK(fgraph->last_submit_count() == 1U);
    CHECK(fgraph->last_barrier_count() >= 1U); // incl. DEPTH_WRITE -> PIXEL_SHADER_RESOURCE

    const crd::u32 lit      = dst->read_pixel(2U, dim / 2U);
    const crd::u32 shadowed = dst->read_pixel(dim - 3U, dim / 2U);
    CHECK(lit != shadowed);
    CHECK((lit & 0xFFU) > 180U);
    CHECK((shadowed & 0xFFU) < 80U);
}

// REN-3.2 GATE (DX12) — the CSM per-cascade depth-ARRAY atlas, the DX12 half. Same four-cascade shape as the
// Vulkan gate, and it must produce the same 4-bit code, but the DX12 view rules are where a layered depth atlas
// actually breaks:
//   · the resource is R32_TYPELESS with DepthOrArraySize = 4 (the REN-3.1 three-format rule, now over an array),
//   · each slice needs its OWN heap holding a TEXTURE2DARRAY DSV with FirstArraySlice = l / ArraySize = 1
//     (a TEXTURE2D DSV over an array resource silently addresses slice 0, so all four cascades would stack), and
//   · the SRV must be TEXTURE2DARRAY with ArraySize = 4 (a TEXTURE2D SRV reads only slice 0),
//   · the frame graph's transition must use ALL_SUBRESOURCES — `Subresource = 0` moved only cascade 0 out of
//     DEPTH_WRITE and left slices 1..3 in the wrong state when the probe sampled them.
// Every one of those four defects collapses the four results to EQUAL values, i.e. code 0 or 255. Only genuinely
// distinct slices give the intermediate 0b1100 = 204.
TEST_CASE("REN-3.2 GATE (DX12): four cascade passes write four SLICES of a depth-array atlas; one pass samples all",
          "[dx12][raster][frame-graph][ren3][gpu]")
{
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);

    crd::memory::TlsfAllocator alloc(16U << 20U);
    constexpr crd::u32         dim      = 32U;
    constexpr crd::u32         cascades = 4U;
    const double               depths[cascades] = {0.2, 0.4, 0.6, 0.8};

    kir::KGraph dvg(&alloc);
    kir::KEntry dve;
    crd::gputest::build_fullscreen_vs(dvg, dve);
    auto dvs = gctx->create_program(dvg, dve);
    if (dvs == nullptr) { SKIP("dxc/DXIL unavailable"); }

    crd::containers::Array<std::unique_ptr<g::IRasterProgram>> cascade_progs(&alloc);
    crd::containers::Array<std::unique_ptr<g::IGpuProgram>>    cascade_fs(&alloc);
    for (crd::u32 c = 0; c < cascades; ++c)
    {
        kir::KGraph dfg(&alloc);
        kir::KEntry dfe;
        crd::gputest::build_depth_only_const_fs(dfg, dfe, depths[c]);
        auto fs = gctx->create_program(dfg, dfe);
        REQUIRE(fs != nullptr);
        auto prog = raster->create_raster_program(*dvs, *fs);
        REQUIRE(prog != nullptr);
        cascade_progs.push_back(static_cast<std::unique_ptr<g::IRasterProgram>&&>(prog));
        cascade_fs.push_back(static_cast<std::unique_ptr<g::IGpuProgram>&&>(fs)); // keep the FS alive
    }

    kir::KGraph pvg(&alloc);
    kir::KEntry pve;
    crd::gputest::build_textured_vs(pvg, pve);
    kir::KGraph pfg(&alloc);
    kir::KEntry pfe;
    crd::gputest::build_cascade_probe_fs(pfg, pfe, 0.5);
    auto pvs = gctx->create_program(pvg, pve);
    auto pfs = gctx->create_program(pfg, pfe);
    REQUIRE(pfs != nullptr); // the arrayed-shadow emitter path must COMPILE on HLSL too
    auto probe_prog = raster->create_raster_program(*pvs, *pfs);
    REQUIRE(probe_prog != nullptr);

    auto dst   = raster->create_color_target(dim, dim);
    auto dummy = raster->create_storage_buffer(16U);
    REQUIRE(dst != nullptr);
    REQUIRE(dummy != nullptr);

    auto fgraph = raster->create_frame_graph();
    REQUIRE(fgraph != nullptr);

    g::FgImageDesc adesc{};
    adesc.width   = dim;
    adesc.height  = dim;
    adesc.format  = g::FgImageFormat::D32Float;
    adesc.sampled = true;
    adesc.layers  = cascades;
    const g::FgImage atlas = fgraph->create_transient_image(adesc);
    REQUIRE(atlas.valid());

    // the stated cap is a REJECTION, not a clamp — identical behaviour to Vulkan's
    g::FgImageDesc over = adesc;
    over.layers         = g::kFgMaxImageLayers + 1U;
    CHECK_FALSE(fgraph->create_transient_image(over).valid());
    over.layers = 0U;
    CHECK_FALSE(fgraph->create_transient_image(over).valid());

    const g::FgImage  fin = fgraph->import_target(*dst);
    const g::FgBuffer buf = fgraph->import_storage(*dummy);

    crd::containers::Array<CascadeDepthPass> cpasses(&alloc);
    cpasses.reserve(cascades); // ⛔ reserve FIRST — the passes hold pointers into this array
    for (crd::u32 c = 0; c < cascades; ++c)
    {
        cpasses.push_back(CascadeDepthPass{atlas, buf, cascade_progs[c].get(), c});
    }
    for (crd::u32 c = 0; c < cascades; ++c)
    {
        fgraph->add_pass("cascade").reads(buf).writes(atlas).execute(&record_cascade_depth, &cpasses[c]);
    }
    ShadowSamplePass ppass{atlas, fin, probe_prog.get()};
    fgraph->add_pass("cascade_probe").reads(atlas).writes(fin).execute(&record_shadow_sample, &ppass);
    REQUIRE(fgraph->build());
    fgraph->execute();

    CHECK(fgraph->last_submit_count() == 1U); // four cascades + the probe in ONE submission

    const crd::u32 px     = dst->read_pixel(dim / 2U, dim / 2U);
    const crd::u32 code   = px & 0xFFU;
    const crd::u32 slice1 = (px >> 8U) & 0xFFU;
    const crd::u32 slice3 = (px >> 16U) & 0xFFU;

    // the SAME window the Vulkan gate asserts — one atlas description, two backends, one answer
    CHECK(code > 185U);
    CHECK(code < 225U);
    CHECK(slice1 < 80U);
    CHECK(slice3 > 180U);
}

// ── REN-36.2 GATE (DX12) — THE API-AGNOSTICISM PROOF. ────────────────────────────────────────────────────────
// The asset text below is CHARACTER-FOR-CHARACTER the one the Vulkan gate uses. Nothing in it names a graphics
// API. If the format ever needed a backend-specific escape hatch, this test could not exist — which is exactly
// why it is written this way: "API agnostic" is a GATE here, not a promise in a doc.
namespace
{
constexpr const char* kShadowFrameTomlDx12 = R"(
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

class TestHostDx12 final : public crd::framecook::IFrameGraphHost
{
public:
    TestHostDx12(g::IRasterTarget* out, g::IRasterProgram* depth_prog, g::IRasterProgram* shade_prog,
                 g::IStorageBuffer* sb)
        : m_out(out), m_depth(depth_prog), m_shade(shade_prog), m_sb(sb)
    {
    }
    g::IRasterTarget*  output() override { return m_out; }
    g::IRasterProgram* program(crd::containers::StringView id) override { return (id.size() > 0U) ? m_shade : nullptr; }
    bool draw_list(crd::containers::StringView /*name*/, crd::framecook::DrawListBinding& out) override
    {
        out.storage = m_sb; out.program = m_depth; out.vertex_count = 3U;
        return true;
    }

private:
    g::IRasterTarget*  m_out   = nullptr;
    g::IRasterProgram* m_depth = nullptr;
    g::IRasterProgram* m_shade = nullptr;
    g::IStorageBuffer* m_sb    = nullptr;
};
} // namespace

TEST_CASE("REN-36.2 GATE (DX12): the SAME cooked asset renders BIT-IDENTICALLY to the hand-written C++ frame",
          "[dx12][raster][frame-graph][ren36][gpu]")
{
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);

    crd::memory::TlsfAllocator alloc(8U << 20U);
    constexpr crd::u32         dim = 32U;

    kir::KGraph dvg(&alloc);
    kir::KEntry dve;
    crd::gputest::build_fullscreen_vs(dvg, dve);
    kir::KGraph dfg(&alloc);
    kir::KEntry dfe;
    crd::gputest::build_depth_only_const_fs(dfg, dfe, 0.5);
    auto dvs = gctx->create_program(dvg, dve);
    if (dvs == nullptr) { SKIP("dxc/DXIL unavailable"); }
    auto dfs        = gctx->create_program(dfg, dfe);
    auto depth_prog = raster->create_raster_program(*dvs, *dfs);

    kir::KGraph svg(&alloc);
    kir::KEntry sve;
    crd::gputest::build_textured_vs(svg, sve);
    kir::KGraph sfg(&alloc);
    kir::KEntry sfe;
    crd::gputest::build_shadow_fs(sfg, sfe);
    auto svs        = gctx->create_program(svg, sve);
    auto sfs        = gctx->create_program(sfg, sfe);
    auto shade_prog = raster->create_raster_program(*svs, *sfs);
    REQUIRE(depth_prog != nullptr);
    REQUIRE(shade_prog != nullptr);

    auto sb = raster->create_storage_buffer(16U);
    REQUIRE(sb != nullptr);

    // (a) the hand-written C++ frame
    auto ref = raster->create_color_target(dim, dim);
    REQUIRE(ref != nullptr);
    {
        auto fgraph = raster->create_frame_graph();
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

    // (b) the SAME cooked asset
    auto out = raster->create_color_target(dim, dim);
    REQUIRE(out != nullptr);
    {
        crd::framecook::FrameGraphDesc desc(&alloc);
        REQUIRE(crd::framecook::parse_frame_toml(
                    crd::containers::StringView(kShadowFrameTomlDx12, std::strlen(kShadowFrameTomlDx12)), desc)
                == crd::framecook::FrameCookError::Ok);
        const auto blob = crd::framecook::cook_frame_graph(desc, &alloc);
        crd::framecook::FrameGraphDesc loaded(&alloc);
        REQUIRE(crd::framecook::read_frame_graph(crd::containers::ConstSpan<crd::u8>(blob.data(), blob.size()), loaded));
        TestHostDx12 host(out.get(), depth_prog.get(), shade_prog.get(), sb.get());
        REQUIRE(crd::framecook::execute_frame_graph(loaded, *raster, host));
    }

    crd::u32 diffs = 0;
    for (crd::u32 y = 0; y < dim; ++y)
    {
        for (crd::u32 x = 0; x < dim; ++x)
        {
            if (ref->read_pixel(x, y) != out->read_pixel(x, y)) { ++diffs; }
        }
    }
    CHECK(diffs == 0U);
    CHECK((out->read_pixel(2U, dim / 2U) & 0xFFU) > 180U);
    CHECK((out->read_pixel(dim - 3U, dim / 2U) & 0xFFU) < 80U);
}

// REN-3.1 BENCH (DX12): the mirror of the Vulkan depth-pre-pass board. Same methodology — ONE imported
// colour+depth target and an identical graph shape in both arms, so the only difference is which draw runs.
TEST_CASE("REN-3.1 BENCH (DX12): depth-only pre-pass cost vs the equivalent colour pass", "[.][ren3-bench][dx12][gpu]")
{
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);

    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph                dvg(&alloc);
    kir::KEntry                dve;
    crd::gputest::build_fullscreen_vs(dvg, dve);
    kir::KGraph dfg(&alloc);
    kir::KEntry dfe;
    crd::gputest::build_depth_only_const_fs(dfg, dfe, 0.5);
    auto dvs = gctx->create_program(dvg, dve);
    if (dvs == nullptr) { SKIP("dxc/DXIL unavailable"); }
    auto dfs        = gctx->create_program(dfg, dfe);
    auto depth_prog = raster->create_raster_program(*dvs, *dfs);

    kir::KGraph cvg(&alloc);
    kir::KEntry cve;
    crd::gputest::build_triangle_vs(cvg, cve);
    kir::KGraph cfg(&alloc);
    kir::KEntry cfe;
    crd::gputest::build_triangle_fs(cfg, cfe);
    auto cvs        = gctx->create_program(cvg, cve);
    auto cfs        = gctx->create_program(cfg, cfe);
    auto color_prog = raster->create_raster_program(*cvs, *cfs);
    REQUIRE(depth_prog != nullptr);
    REQUIRE(color_prog != nullptr);

    auto sb = raster->create_storage_buffer(16U);
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
    std::printf("\n[ren3-bench] depth-only pre-pass vs colour pass (DX12, CPU ms/frame, min-of-5)\n");
    std::printf("  res    draws |  depth-only |  colour+depth |  ratio\n");
    for (const crd::u32 res : {512U, 1024U, 2048U})
    {
        for (const crd::u32 n : {1U, 16U})
        {
            double best_d = 1e30;
            double best_c = 1e30;
            auto   tgt    = raster->create_color_depth_target(res, res);
            REQUIRE(tgt != nullptr);
            for (crd::u32 rep = 0; rep < 5U; ++rep)
            {
                for (int arm = 0; arm < 2; ++arm)
                {
                    auto fgraph = raster->create_frame_graph();
                    REQUIRE(fgraph != nullptr);
                    const auto t0 = std::chrono::steady_clock::now();
                    for (crd::u32 f = 0; f < frames; ++f)
                    {
                        const g::FgImage  im = fgraph->import_target(*tgt);
                        const g::FgBuffer b  = fgraph->import_storage(*sb);
                        DepthBench        st{im, b, arm == 0 ? depth_prog.get() : color_prog.get(), n};
                        fgraph->add_pass("p").reads(b).writes(im).execute(arm == 0 ? rec_depth : rec_color, &st);
                        REQUIRE(fgraph->build());
                        fgraph->execute();
                        fgraph->reset();
                    }
                    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / frames;
                    if (arm == 0) { if (ms < best_d) { best_d = ms; } }
                    else { if (ms < best_c) { best_c = ms; } }
                }
            }
            std::printf("  %4u %8u | %10.4f | %13.4f | %6.2fx\n", res, n, best_d, best_c, best_c / best_d);
        }
    }
    CHECK(true);
}

TEST_CASE("REN-1 BENCH (DX12): one-submission batching vs the synchronous per-draw substrate", "[.][ren1-bench][dx12][gpu]")
{
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);

    crd::memory::TlsfAllocator alloc(4U << 20U);
    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    build_pull_depth_vs(vg, ve);
    kir::KGraph fgg(&alloc);
    kir::KEntry fe;
    build_pull_depth_fs(fgg, fe);
    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { SKIP("dxc/DXIL unavailable"); }
    auto fs   = gctx->create_program(fgg, fe);
    auto prog = raster->create_raster_program(*vs, *fs);
    REQUIRE(prog != nullptr);

    auto tgt = raster->create_color_depth_target(256U, 256U);
    auto sb  = raster->create_storage_buffer(sizeof(kRedField));
    REQUIRE(tgt != nullptr);
    REQUIRE(sb != nullptr);
    REQUIRE(raster->upload_storage(*sb, 0U, kRedField, sizeof(kRedField)));

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
        const auto s0 = std::chrono::steady_clock::now();
        for (crd::u32 f = 0; f < frames; ++f)
        {
            crd::gputest::enc_draw_storage_depth(*raster, *tgt, *prog, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 0.0F, g::DepthCompare::Always, *sb, 3U);
            for (crd::u32 i = 1; i < n; ++i) { crd::gputest::enc_draw_storage_depth_load(*raster, *tgt, *prog, g::DepthCompare::Always, *sb, 3U); }
        }
        const double sync_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s0).count() / frames;

        auto fgraph = raster->create_frame_graph();
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
            CHECK(fgraph->last_submit_count() == 1U);
        }
        const double graph_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g0).count() / frames;

        WARN("[ren1-bench dx12] N=" << n << " draws/frame  sync=" << sync_ms << "ms/frame  graph=" << graph_ms
                                    << "ms/frame  speedup=" << (graph_ms > 0.0 ? sync_ms / graph_ms : 0.0) << "x");
        if (n == 64U) { graph_ms_at_64 = graph_ms; sync_ms_at_64 = sync_ms; }
    }
    CHECK(graph_ms_at_64 <= sync_ms_at_64);
}

// ── REN-36.3 GATE (DX12) — the API-AGNOSTICISM half of multi-view expansion. ────────────────────────────────
// The asset text below is CHARACTER-FOR-CHARACTER the one the Vulkan gate parses, and it asserts the SAME
// numeric window. `for_each` is therefore proven to be a property of the ASSET FORMAT, not of a backend: one
// declaration expands to four cascades identically on Vulkan and DX12, over completely different device
// machinery (per-slice TEXTURE2DARRAY DSVs in their own heaps vs single-slice VK_IMAGE_VIEW_TYPE_2D views).
namespace
{
constexpr const char* kCascadeFrameTomlDx12 = R"(
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

class CascadeHostDx12 final : public crd::framecook::IFrameGraphHost
{
public:
    CascadeHostDx12(g::IRasterTarget* out, g::IRasterProgram* const* progs, crd::u32 count,
                    g::IRasterProgram* probe, g::IStorageBuffer* sb)
        : m_out(out), m_cascades(progs), m_count(count), m_probe(probe), m_sb(sb)
    {
    }
    g::IRasterTarget*  output() override { return m_out; }
    g::IRasterProgram* program(crd::containers::StringView id) override { return (id.size() > 0U) ? m_probe : nullptr; }
    bool draw_list(crd::containers::StringView /*name*/, crd::framecook::DrawListBinding& out) override
    {
        out.storage = m_sb; out.program = m_cascades[0]; out.vertex_count = 3U;
        return true;
    }
    crd::u32 for_each_count(crd::framecook::FrameForEach kind, crd::u32 arg) override
    {
        return (kind == crd::framecook::FrameForEach::LightCascades && arg == 0U) ? m_count : 0U;
    }
    g::IRasterProgram* instance_program(crd::containers::StringView /*pass*/, crd::u32 index) override
    {
        return index < m_count ? m_cascades[index] : nullptr;
    }

private:
    g::IRasterTarget*         m_out      = nullptr;
    g::IRasterProgram* const* m_cascades = nullptr;
    crd::u32                  m_count    = 0U;
    g::IRasterProgram*        m_probe    = nullptr;
    g::IStorageBuffer*        m_sb       = nullptr;
};
} // namespace

TEST_CASE("REN-36.3 GATE (DX12): ONE authored pass declaration expands to FOUR cascades writing four atlas slices",
          "[dx12][raster][frame-graph][ren36][ren3][gpu]")
{
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);

    crd::memory::TlsfAllocator alloc(16U << 20U);
    constexpr crd::u32         dim      = 32U;
    constexpr crd::u32         cascades = 4U;
    const double               depths[cascades] = {0.2, 0.4, 0.6, 0.8};

    kir::KGraph dvg(&alloc);
    kir::KEntry dve;
    crd::gputest::build_fullscreen_vs(dvg, dve);
    auto dvs = gctx->create_program(dvg, dve);
    if (dvs == nullptr) { SKIP("dxc/DXIL unavailable"); }

    crd::containers::Array<std::unique_ptr<g::IRasterProgram>> cprogs(&alloc);
    crd::containers::Array<std::unique_ptr<g::IGpuProgram>>    cfs(&alloc);
    for (crd::u32 c = 0; c < cascades; ++c)
    {
        kir::KGraph dfg(&alloc);
        kir::KEntry dfe;
        crd::gputest::build_depth_only_const_fs(dfg, dfe, depths[c]);
        auto fs = gctx->create_program(dfg, dfe);
        REQUIRE(fs != nullptr);
        auto prog = raster->create_raster_program(*dvs, *fs);
        REQUIRE(prog != nullptr);
        cprogs.push_back(static_cast<std::unique_ptr<g::IRasterProgram>&&>(prog));
        cfs.push_back(static_cast<std::unique_ptr<g::IGpuProgram>&&>(fs));
    }
    g::IRasterProgram* cascade_ptrs[cascades] = {cprogs[0].get(), cprogs[1].get(), cprogs[2].get(), cprogs[3].get()};

    kir::KGraph pvg(&alloc);
    kir::KEntry pve;
    crd::gputest::build_textured_vs(pvg, pve);
    kir::KGraph pfg(&alloc);
    kir::KEntry pfe;
    crd::gputest::build_cascade_probe_fs(pfg, pfe, 0.5);
    auto pvs        = gctx->create_program(pvg, pve);
    auto pfs        = gctx->create_program(pfg, pfe);
    auto probe_prog = raster->create_raster_program(*pvs, *pfs);
    auto sb         = raster->create_storage_buffer(16U);
    REQUIRE(probe_prog != nullptr);
    REQUIRE(sb != nullptr);

    crd::framecook::FrameGraphDesc d(&alloc);
    REQUIRE(crd::framecook::parse_frame_toml(
                crd::containers::StringView(kCascadeFrameTomlDx12, std::strlen(kCascadeFrameTomlDx12)), d)
            == crd::framecook::FrameCookError::Ok);

    auto dst = raster->create_color_target(dim, dim);
    REQUIRE(dst != nullptr);
    CascadeHostDx12 host(dst.get(), static_cast<g::IRasterProgram* const*>(cascade_ptrs), cascades,
                         probe_prog.get(), sb.get());
    REQUIRE(crd::framecook::execute_frame_graph(d, *raster, host));

    const crd::u32 px     = dst->read_pixel(dim / 2U, dim / 2U);
    const crd::u32 code   = px & 0xFFU;
    const crd::u32 slice1 = (px >> 8U) & 0xFFU;
    const crd::u32 slice3 = (px >> 16U) & 0xFFU;
    CHECK(code > 185U);
    CHECK(code < 225U);
    CHECK(slice1 < 80U);
    CHECK(slice3 > 180U);

    // ⛔ 0 instances must FAIL BY NAME on DX12 too — identical semantics, not an accident of one backend
    auto empty_dst = raster->create_color_target(dim, dim);
    REQUIRE(empty_dst != nullptr);
    CascadeHostDx12 none(empty_dst.get(), static_cast<g::IRasterProgram* const*>(cascade_ptrs), 0U,
                         probe_prog.get(), sb.get());
    crd::containers::String    where(&alloc);
    crd::framecook::FrameExecError err = crd::framecook::FrameExecError::Ok;
    CHECK_FALSE(crd::framecook::execute_frame_graph(d, *raster, none, &err, &where));
    CHECK(err == crd::framecook::FrameExecError::UnresolvedForEach);
}

// ── REN-8 GATE (DX12) — PER-PASS GPU TIMING, the parity half. ────────────────────────────────────────────────
// Same contract as the Vulkan gate: timestamps bracket every pass inside the frame's ONE command list, resolved
// via ResolveQueryData into a readback buffer and converted with the queue's GetTimestampFrequency.
// ⛔ Parity matters beyond tidiness: REN-36 promises ONE authored asset runs on both backends, and an author who
// can profile a graph on Vulkan but not on DX12 cannot honour that promise when tuning.
TEST_CASE("REN-8 GATE (DX12): the frame graph reports PER-PASS GPU time from device timestamps",
          "[dx12][raster][frame-graph][ren8][gpu]")
{
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);

    crd::memory::TlsfAllocator alloc(8U << 20U);
    constexpr crd::u32         dim = 64U;

    kir::KGraph dvg(&alloc);
    kir::KEntry dve;
    crd::gputest::build_fullscreen_vs(dvg, dve);
    kir::KGraph dfg(&alloc);
    kir::KEntry dfe;
    crd::gputest::build_depth_only_const_fs(dfg, dfe, 0.5);
    auto dvs = gctx->create_program(dvg, dve);
    if (dvs == nullptr) { SKIP("dxc/DXIL unavailable"); }
    auto dfs        = gctx->create_program(dfg, dfe);
    auto depth_prog = raster->create_raster_program(*dvs, *dfs);

    kir::KGraph svg(&alloc);
    kir::KEntry sve;
    crd::gputest::build_textured_vs(svg, sve);
    kir::KGraph sfg(&alloc);
    kir::KEntry sfe;
    crd::gputest::build_shadow_fs(sfg, sfe);
    auto svs         = gctx->create_program(svg, sve);
    auto sfs         = gctx->create_program(sfg, sfe);
    auto shadow_prog = raster->create_raster_program(*svs, *sfs);
    auto dst         = raster->create_color_target(dim, dim);
    auto dummy       = raster->create_storage_buffer(16U);
    REQUIRE(depth_prog != nullptr);
    REQUIRE(shadow_prog != nullptr);
    REQUIRE(dst != nullptr);

    auto fgraph = raster->create_frame_graph();
    REQUIRE(fgraph != nullptr);
    if (!fgraph->gpu_timing_available()) { SKIP("queue does not support timestamp queries"); }

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

    CHECK(fgraph->pass_count() == 0U); // no stale numbers before the first execute
    fgraph->execute();

    REQUIRE(fgraph->pass_count() == 2U);
    CHECK(std::strcmp(fgraph->pass_name(0), "shadow_depth") == 0);
    CHECK(std::strcmp(fgraph->pass_name(1), "shade") == 0);

    const double p0  = fgraph->pass_gpu_ms(0);
    const double p1  = fgraph->pass_gpu_ms(1);
    const double tot = fgraph->gpu_ms_total();
    CHECK(p0 > 0.0);
    CHECK(p1 > 0.0);
    CHECK(tot > 0.0);
    CHECK(tot >= p0);
    CHECK(tot >= p1);
    CHECK(tot < 200.0); // a wild value would mean the tick->ms scale is wrong
    CHECK(fgraph->pass_gpu_ms(99U) == 0.0);
    CHECK(fgraph->pass_name(99U) == nullptr);

    std::printf("[ren8][dx12] shadow_depth %.4f ms | shade %.4f ms | span %.4f ms\n", p0, p1, tot);
}

// ── REN-1 GATE (DX12): DEPENDENCY ORDER, the parity half. ────────────────────────────────────────────────────
// Identical shape to the Vulkan gate: the shadow-depth PRODUCER is declared SECOND, after the pass that samples
// it. Both backends must schedule it FIRST and produce identical pixels — a graph that ordered differently per
// API would break REN-36's "one asset, both backends" claim in the worst way: right on one, garbage on the other.
TEST_CASE("REN-1 GATE (DX12): a pass declared AFTER its consumer still executes FIRST (dependency order)",
          "[dx12][raster][frame-graph][ren1][gpu]")
{
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);

    crd::memory::TlsfAllocator alloc(8U << 20U);
    constexpr crd::u32         dim = 32U;

    kir::KGraph dvg(&alloc);
    kir::KEntry dve;
    crd::gputest::build_fullscreen_vs(dvg, dve);
    kir::KGraph dfg(&alloc);
    kir::KEntry dfe;
    crd::gputest::build_depth_only_const_fs(dfg, dfe, 0.5);
    auto dvs = gctx->create_program(dvg, dve);
    if (dvs == nullptr) { SKIP("dxc/DXIL unavailable"); }
    auto dfs        = gctx->create_program(dfg, dfe);
    auto depth_prog = raster->create_raster_program(*dvs, *dfs);

    kir::KGraph svg(&alloc);
    kir::KEntry sve;
    crd::gputest::build_textured_vs(svg, sve);
    kir::KGraph sfg(&alloc);
    kir::KEntry sfe;
    crd::gputest::build_shadow_fs(sfg, sfe);
    auto svs         = gctx->create_program(svg, sve);
    auto sfs         = gctx->create_program(sfg, sfe);
    auto shadow_prog = raster->create_raster_program(*svs, *sfs);
    auto dst         = raster->create_color_target(dim, dim);
    auto dummy       = raster->create_storage_buffer(16U);
    REQUIRE(depth_prog != nullptr);
    REQUIRE(shadow_prog != nullptr);
    REQUIRE(dst != nullptr);

    auto fgraph = raster->create_frame_graph();
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
    // REVERSED: consumer first, producer second
    fgraph->add_pass("shade").reads(map).writes(fin).execute(&record_shadow_sample, &spass);
    fgraph->add_pass("shadow_depth").reads(buf).writes(map).execute(&record_shadow_depth, &dpass);
    REQUIRE(fgraph->build());
    fgraph->execute();

    CHECK(fgraph->last_submit_count() == 1U);
    REQUIRE(fgraph->pass_count() == 2U);
    CHECK(std::strcmp(fgraph->pass_name(0), "shadow_depth") == 0);
    CHECK(std::strcmp(fgraph->pass_name(1), "shade") == 0);

    const crd::u32 lit      = dst->read_pixel(2U, dim / 2U);
    const crd::u32 shadowed = dst->read_pixel(dim - 3U, dim / 2U);
    CHECK(lit != shadowed);
    CHECK((lit & 0xFFU) > 180U);
    CHECK((shadowed & 0xFFU) < 80U);
}

// A true CYCLE must be REJECTED by build() on DX12 too.
TEST_CASE("REN-1 GATE (DX12): build() REJECTS a dependency cycle", "[dx12][raster][frame-graph][ren1][gpu]")
{
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    auto a_t = raster->create_color_target(16U, 16U);
    auto b_t = raster->create_color_target(16U, 16U);
    REQUIRE(a_t != nullptr);
    REQUIRE(b_t != nullptr);

    auto fgraph = raster->create_frame_graph();
    REQUIRE(fgraph != nullptr);
    const g::FgImage x = fgraph->import_target(*a_t);
    const g::FgImage y = fgraph->import_target(*b_t);
    fgraph->add_pass("A").reads(y).writes(x).execute(&record_noop, nullptr);
    fgraph->add_pass("B").reads(x).writes(y).execute(&record_noop, nullptr);
    CHECK_FALSE(fgraph->build());
}

// ── REN-37.5 GATE (DX12): PERSISTENT resources — the Vulkan gate's mirror, same claims, same keys. ──────────
// ⛔⛔ One asset, both backends. A persistent image is the substrate for TAA history, SSR/DDGI/ReSTIR temporal
// reuse, auto-exposure, ping-pong blur chains and (REN-37.9) cached viewport thumbnails, so it MUST behave
// identically here — a technique authored against it cannot be allowed to work on Vulkan and quietly not on DX12.
//
// The DX12-specific hazard this pins: a persistent image is a COMMITTED resource, not a PLACED one in the
// aliasing heap, and its RESOURCE STATE has to be carried across the frame boundary by hand. Reset the state at
// frame start (which is right for every other node) and the barrier scheduler emits a transition FROM a state the
// resource is not in.
TEST_CASE("REN-37.5 GATE (DX12): a PERSISTENT image keeps its contents across reset() and is never aliased",
          "[dx12][raster][frame-graph][ren37][gpu]")
{
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);

    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph                tvg(&alloc);
    kir::KEntry                tve;
    crd::gputest::build_triangle_vs(tvg, tve);
    kir::KGraph tfg(&alloc);
    kir::KEntry tfe;
    crd::gputest::build_triangle_fs(tfg, tfe);
    auto tvs = gctx->create_program(tvg, tve);
    if (tvs == nullptr) { SKIP("dxc/DXIL unavailable"); }
    auto tfs      = gctx->create_program(tfg, tfe);
    auto tri_prog = raster->create_raster_program(*tvs, *tfs);
    kir::KGraph svg(&alloc);
    kir::KEntry sve;
    crd::gputest::build_textured_vs(svg, sve);
    kir::KGraph sfg(&alloc);
    kir::KEntry sfe;
    crd::gputest::build_sample_fs(sfg, sfe);
    auto svs         = gctx->create_program(svg, sve);
    auto sfs         = gctx->create_program(sfg, sfe);
    auto sample_prog = raster->create_raster_program(*svs, *sfs);
    REQUIRE(tri_prog != nullptr);
    REQUIRE(sample_prog != nullptr);

    constexpr crd::u32 dim   = 64U;
    constexpr crd::u32 probe_key = 0xA11CE5U; // the same stable identity the Vulkan gate uses
    auto               dst   = raster->create_color_target(dim, dim);
    auto               dummy = raster->create_storage_buffer(16U);
    REQUIRE(dst != nullptr);
    REQUIRE(dummy != nullptr);

    auto fgraph = raster->create_frame_graph();
    REQUIRE(fgraph != nullptr);

    g::FgImageDesc pdesc{};
    pdesc.width   = dim;
    pdesc.height  = dim;
    pdesc.format  = g::FgImageFormat::RGBA8Unorm;
    pdesc.sampled = true;

    // ── FRAME 0: create the history and WRITE into it. ──
    {
        const g::FgImage hist = fgraph->create_persistent_image(probe_key, pdesc);
        REQUIRE(hist.valid());
        CHECK_FALSE(fgraph->persistent_image_was_live(probe_key));
        const g::FgBuffer buf = fgraph->import_storage(*dummy);
        RttOffscreen      off{hist, buf, tri_prog.get()};
        fgraph->add_pass("write_history").reads(buf).writes(hist).execute(&record_rtt_offscreen, &off);
        REQUIRE(fgraph->build());
        CHECK(fgraph->transient_memory_bytes() == 0U);  // never in the aliasing heap
        CHECK(fgraph->transient_logical_bytes() == 0U);
        fgraph->execute();
    }

    fgraph->reset(); // ⛔ the operation that destroys every transient

    // ── FRAME 1: same key, READ what frame 0 wrote. ──
    {
        const g::FgImage hist = fgraph->create_persistent_image(probe_key, pdesc);
        REQUIRE(hist.valid());
        CHECK(fgraph->persistent_image_was_live(probe_key));
        const g::FgImage fin = fgraph->import_target(*dst);
        RttCompose       com{hist, fin, sample_prog.get()};
        fgraph->add_pass("read_history").reads(hist).writes(fin).execute(&record_rtt_compose, &com);
        REQUIRE(fgraph->build());
        CHECK(fgraph->transient_memory_bytes() == 0U);
        fgraph->execute();
    }

    const crd::u32 center = dst->read_pixel(dim / 2U, dim / 2U);
    const crd::u32 corner = dst->read_pixel(2U, 2U);
    CHECK((center & 0xFFU) > 180U);         // the RED triangle frame 0 drew, still there
    CHECK(((corner >> 8U) & 0xFFU) > 180U); // the GREEN clear frame 0 laid down
    CHECK((corner & 0xFFU) < 80U);

    // a RESIZE invalidates the history rather than silently reinterpreting it
    fgraph->reset();
    g::FgImageDesc bigger = pdesc;
    bigger.width          = dim * 2U;
    bigger.height         = dim * 2U;
    REQUIRE(fgraph->create_persistent_image(probe_key, bigger).valid());
    CHECK_FALSE(fgraph->persistent_image_was_live(probe_key));
}

// ── REN-38-A1a GATE (DX12): the bindless RECORDING path, and PARITY with Vulkan. ────────────────────────────
// ⛔ Parity is part of this row's gate, not a follow-up. A verb recordable on ONE backend is a WORSE state than
// one recordable on neither: it renders correctly right up until someone switches API, and the failure then looks
// like a driver bug rather than a missing port.
//
// The DX12-specific hazard: `draw_bindless` mints its SRV array into the GLOBAL heap at FIXED slots and resets
// the command list. Inside a frame that stomps descriptors earlier passes still use and throws away everything
// already recorded. `record_bindless` reserves a CONTIGUOUS RUN from the frame ring instead — contiguous because
// a root descriptor table addresses N consecutive slots from one GPU handle.
TEST_CASE("REN-38-A1a GATE (DX12): a fullscreen pass binds ALL its declared reads, in order",
          "[dx12][raster][frame-graph][ren38][gpu]")
{
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    if (!raster->supports_bindless()) { SKIP("device does not support bindless"); }

    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph                fvg(&alloc);
    kir::KEntry                fve;
    crd::gputest::build_textured_vs(fvg, fve);
    auto fvs = gctx->create_program(fvg, fve);
    if (fvs == nullptr) { SKIP("dxc/DXIL unavailable"); }

    std::unique_ptr<g::IGpuProgram> solid_fs[2];
    const auto solid_prog = [&](int slot, double r, double gg, double b) {
        kir::KGraph fg(&alloc);
        kir::KEntry fe;
        const auto  sh = kir::make_shape({1});
        const auto  kf = [&](double v) { return fg.constant(v, sh, kir::DType::F32); };
        fe.stage       = kir::KStage::Fragment;
        fe.n_out       = 1;
        fe.out[0]      = {fg.vec4(kf(r), kf(gg), kf(b), kf(1.0)), 0};
        solid_fs[slot] = gctx->create_program(fg, fe);
        return solid_fs[slot] != nullptr ? raster->create_raster_program(*fvs, *solid_fs[slot])
                                         : std::unique_ptr<g::IRasterProgram>{};
    };
    auto red_prog   = solid_prog(0, 1.0, 0.0, 0.0);
    auto green_prog = solid_prog(1, 0.0, 1.0, 0.0);

    kir::KGraph cfg_graph(&alloc);
    kir::KEntry cfe;
    crd::gputest::build_two_texture_composite_fs(cfg_graph, cfe);
    auto cfs          = gctx->create_program(cfg_graph, cfe);
    auto compose_prog = raster->create_raster_program(*fvs, *cfs);
    REQUIRE(red_prog != nullptr);
    REQUIRE(green_prog != nullptr);
    REQUIRE(compose_prog != nullptr);

    constexpr crd::u32 dim   = 64U;
    auto               dst   = raster->create_color_target(dim, dim);
    auto               dummy = raster->create_storage_buffer(16U);
    REQUIRE(dst != nullptr);
    REQUIRE(dummy != nullptr);

    auto fgraph = raster->create_frame_graph();
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
    const g::FgImage  fin  = fgraph->import_target(*dst);
    const g::FgBuffer dbuf = fgraph->import_storage(*dummy);

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
        // 38-A1h gave plain `draw` a recording path, so this gate covers A1h as well as A3.
        crd::gputest::enc_draw(ctx.raster(), *ctx.image(s->img), *s->prog, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);
    };
    const auto rec_compose = [](g::IFrameContext& ctx, void* user) {
        auto*        s    = static_cast<Compose*>(user);
        g::ITexture* t[2] = {ctx.texture(s->a), ctx.texture(s->b)};
        if (t[0] == nullptr || t[1] == nullptr) { return; }
        crd::gputest::enc_draw_bindless(ctx.raster(), *ctx.image(s->dst), *s->prog, g::ClearColor{0.0F, 0.0F, 1.0F, 1.0F},
                                        static_cast<g::ITexture* const*>(t), 2U, 3U);
    };

    Solid   s0{gb0, dbuf, red_prog.get()};
    Solid   s1{gb1, dbuf, green_prog.get()};
    Compose cm{gb0, gb1, fin, compose_prog.get()};
    fgraph->add_pass("gbuf0").reads(dbuf).writes(gb0).execute(+rec_solid, &s0);
    fgraph->add_pass("gbuf1").reads(dbuf).writes(gb1).execute(+rec_solid, &s1);
    fgraph->add_pass("lighting").reads(gb0).reads(gb1).writes(fin).execute(+rec_compose, &cm);
    REQUIRE(fgraph->build());
    fgraph->execute();

    const crd::u32 px = dst->read_pixel(dim / 2U, dim / 2U);
    const crd::u32 r  = px & 0xFFU;
    const crd::u32 gg = (px >> 8U) & 0xFFU;
    UNSCOPED_INFO("dx12 composite r=" << r << " g=" << gg);
    CHECK(r > 180U);  // tex[0] (RED writer) at slot 0
    CHECK(gg > 180U); // tex[1] (GREEN writer) at slot 1 — the read that used to be dropped
    CHECK(fgraph->last_submit_count() == 1U);
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
constexpr const char* kDeferredGraphDx = R"(
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
class DeferredHostDx final : public crd::framecook::IFrameGraphHost
{
public:
    DeferredHostDx(g::IRasterTarget& out, g::IStorageBuffer& buf, g::IRasterProgram& gbuf, g::IRasterProgram& light)
        : m_out(out), m_buf(buf), m_gbuf(gbuf), m_light(light)
    {
    }
    [[nodiscard]] g::IRasterTarget* output() override { return &m_out; }
    [[nodiscard]] g::IRasterProgram* program(crd::containers::StringView) override { return &m_light; }
    [[nodiscard]] bool draw_list(crd::containers::StringView, crd::framecook::DrawListBinding& out) override
    {
        out.items[0]  = crd::framecook::DrawItem{&m_buf, &m_gbuf, 3U, nullptr};
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

TEST_CASE("REN-38-A4 GATE (DX12): a DEFERRED renderer, authored as an asset only",
          "[dx12][raster][frame-graph][ren38][gpu]")
{
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto rasterp = g::create_dx12_raster_context();
    REQUIRE(rasterp != nullptr);
    auto& raster = *rasterp;
    if (!raster.supports_bindless()) { SKIP("device does not support bindless"); }

    crd::memory::TlsfAllocator alloc(16U << 20U);

    // the G-buffer program: a fullscreen triangle writing TWO attachments
    kir::KGraph gvg(&alloc);
    kir::KEntry gve;
    crd::gputest::build_textured_vs(gvg, gve);
    auto gvs = gctx->create_program(gvg, gve);
    if (gvs == nullptr) { SKIP("dxc/DXIL unavailable"); }
    kir::KGraph gfg(&alloc);
    kir::KEntry gfe;
    crd::gputest::build_gbuffer_two_output_fs(gfg, gfe);
    auto gfs       = gctx->create_program(gfg, gfe);
    auto gbuf_prog = raster.create_raster_program(*gvs, *gfs);

    // the lighting program: a fullscreen composite reading BOTH G-buffer targets
    kir::KGraph lfg(&alloc);
    kir::KEntry lfe;
    crd::gputest::build_two_texture_composite_fs(lfg, lfe);
    auto lfs        = gctx->create_program(lfg, lfe);
    auto light_prog = raster.create_raster_program(*gvs, *lfs);
    REQUIRE(gbuf_prog != nullptr);
    REQUIRE(light_prog != nullptr);

    constexpr crd::u32 dim   = 64U;
    auto          dst   = raster.create_color_target(dim, dim);
    auto          dummy = raster.create_storage_buffer(16U);
    REQUIRE(dst != nullptr);
    REQUIRE(dummy != nullptr);

    crd::framecook::FrameGraphDesc desc(&alloc);
    crd::containers::String        where(&alloc);
    REQUIRE(crd::framecook::parse_frame_toml(crd::containers::StringView(kDeferredGraphDx), desc, &where)
            == crd::framecook::FrameCookError::Ok);

    DeferredHostDx         host(*dst, *dummy, *gbuf_prog, *light_prog);
    crd::framecook::FrameExecError err = crd::framecook::FrameExecError::Ok;
    REQUIRE(crd::framecook::execute_frame_graph(desc, raster, host, &err, &where));
    CHECK(err == crd::framecook::FrameExecError::Ok);

    const crd::u32 px = dst->read_pixel(dim / 2U, dim / 2U);
    const crd::u32 r  = px & 0xFFU;
    const crd::u32 gg = (px >> 8U) & 0xFFU;
    UNSCOPED_INFO("deferred composite r=" << r << " g=" << gg);
    CHECK(r > 180U);  // attachment 0 (albedo/RED) was written by MRT and read at slot 0
    CHECK(gg > 180U); // attachment 1 (normal/GREEN) was written by MRT and read at slot 1

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
constexpr const char* kPresentGraphDx = R"(
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

class PresentHostDx final : public crd::framecook::IFrameGraphHost
{
public:
    PresentHostDx(g::IRasterTarget& out, g::IStorageBuffer& buf, g::IRasterProgram& prog, g::IPresentSurface* surf)
        : m_out(out), m_buf(buf), m_prog(prog), m_surf(surf)
    {
    }
    [[nodiscard]] g::IRasterTarget* output() override { return &m_out; }
    [[nodiscard]] g::IRasterProgram* program(crd::containers::StringView) override { return &m_prog; }
    [[nodiscard]] bool draw_list(crd::containers::StringView, crd::framecook::DrawListBinding& out) override
    {
        out.items[0] = crd::framecook::DrawItem{&m_buf, &m_prog, 3U, nullptr};
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

// ── The DEVICE half: the authored present pass reaches a real swapchain. ──
TEST_CASE("REN-38-A5 GATE (DX12): an authored PRESENT pass hands the frame to a swapchain",
          "[dx12][raster][frame-graph][ren38][gpu]")
{
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto rasterp = g::create_dx12_raster_context();
    REQUIRE(rasterp != nullptr);
    auto& raster = *rasterp;

    // ⛔ DXGI has NO headless surface, so this gate needs a REAL window — the same isolated <windows.h> helper
    // RET-2's DX12 present gate uses. Skipping when none exists is honest; presenting to nothing would not be.
    void* native = crd::gputest::create_test_window(256U, 256U);
    if (native == nullptr) { SKIP("no platform window available"); }
    constexpr crd::u32 dim = 256U;
    auto surface = raster.create_present_surface(native, dim, dim, g::PresentMode::Fifo);
    REQUIRE(surface != nullptr);
    REQUIRE(surface->valid());
    REQUIRE(surface->width() == dim);
    REQUIRE(surface->height() == dim);

    crd::memory::TlsfAllocator alloc(8U << 20U);
    crd::kir::KGraph           vg(&alloc);
    crd::kir::KEntry           ve;
    crd::gputest::build_triangle_vs(vg, ve);
    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { SKIP("dxc/DXIL unavailable"); }
    crd::kir::KGraph fg2(&alloc);
    crd::kir::KEntry fe;
    crd::gputest::build_triangle_fs(fg2, fe);
    auto fs   = gctx->create_program(fg2, fe);
    auto prog = raster.create_raster_program(*vs, *fs);
    REQUIRE(prog != nullptr);

    // ⛔ The canvas MUST match the surface: a present with a mismatched source is REFUSED (never a stretched
    // half-frame), and a refused present is exactly the state this gate must not mistake for a success.
    auto dst = raster.create_color_depth_target(dim, dim);
    auto buf = raster.create_storage_buffer(64U);
    REQUIRE(dst != nullptr);
    REQUIRE(buf != nullptr);

    crd::framecook::FrameGraphDesc desc(&alloc);
    crd::containers::String        where(&alloc);
    REQUIRE(crd::framecook::parse_frame_toml(crd::containers::StringView(kPresentGraphDx), desc, &where)
            == crd::framecook::FrameCookError::Ok);


    // ── 1) NO SURFACE ⇒ a NAMED failure, never a quiet skip. ──
    // ⛔ This is the assertion that would have caught the original defect from the asset side: before the row, a
    // graph with a present pass and no surface anywhere ran to completion and reported success.
    {
        PresentHostDx               blind(*dst, *buf, *prog, nullptr);
        crd::framecook::FrameExecError err = crd::framecook::FrameExecError::Ok;
        crd::containers::String        w2(&alloc);
        CHECK(!crd::framecook::execute_frame_graph(desc, raster, blind, &err, &w2));
        CHECK(err == crd::framecook::FrameExecError::NoPresentSurface);
        CHECK(w2 == "to_screen"); // by PASS NAME — the author is told which pass, not merely that something failed
    }

    // ⛔ CEIR-18c (a PRE-EXISTING §128 DX12 hole caught by the CEIR-18c blast-radius gate — the CEIR-17z fix landed on
    // the VULKAN REN-38-A5 twin but CEIR-17's gate ran gpu-context-vulkan ONLY, so this DX12 twin's MissingCeirPlan
    // went uncaught): the migrated PRESENT executor needs a CEIR replay plan; a direct rec.record with no plan is a
    // loud MissingCeirPlan. Build it ONCE (exactly as execute_frame_graph does) and pass it to BOTH frames' records.
    crd::renderasset::DiagnosticList plan_diags(&alloc);
    crd::framecook::FramePlans       plans(&alloc);
    REQUIRE(crd::framecook::build_frame_plans(desc, plans, plan_diags));

    // ── 2) WITH a surface: the frame actually goes up, and the graph still submits ONCE. ──
    PresentHostDx host(*dst, *buf, *prog, surface.get());
    auto        fgraph = raster.create_frame_graph();
    REQUIRE(fgraph != nullptr);
    crd::framecook::FrameRecorder rec(&alloc);
    rec.begin_frame();
    crd::framecook::FrameExecError err = crd::framecook::FrameExecError::Ok;
    REQUIRE(rec.record(desc, *fgraph, raster, host, &err, &where, &plans));
    CHECK(err == crd::framecook::FrameExecError::Ok);
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
    REQUIRE(rec.record(desc, *fgraph, raster, host, &err, &where, &plans)); // CEIR-18c: reuse the one replay plan
    REQUIRE(fgraph->build());
    fgraph->execute();
    CHECK(fgraph->last_present_count() == 1U);
    CHECK(surface->frame_count() == 2U);

    surface.reset(); // the surface dies BEFORE its window
    crd::gputest::destroy_test_window(native);
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
constexpr const char* kCopyGraphDx = R"(
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
constexpr const char* kBlitGraphDx = R"(
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

class UtilHostDx final : public crd::framecook::IFrameGraphHost
{
public:
    UtilHostDx(g::IRasterTarget& out, g::IStorageBuffer* buf, g::IRasterProgram* prog)
        : m_out(out), m_buf(buf), m_prog(prog)
    {
    }
    [[nodiscard]] g::IRasterTarget* output() override { return &m_out; }
    [[nodiscard]] g::IRasterProgram* program(crd::containers::StringView) override { return m_prog; }
    [[nodiscard]] bool draw_list(crd::containers::StringView, crd::framecook::DrawListBinding& out) override
    {
        if (m_buf == nullptr || m_prog == nullptr) { return false; }
        out.items[0] = crd::framecook::DrawItem{m_buf, m_prog, 3U, nullptr};
        out.resolved = 1U;
        return true;
    }

private:
    g::IRasterTarget&  m_out;
    g::IStorageBuffer* m_buf  = nullptr;
    g::IRasterProgram* m_prog = nullptr;
};
} // namespace

// ── The DEVICE half: the four verbs actually move pixels, inside ONE submission. ──
TEST_CASE("REN-38-A6 GATE (DX12): authored CLEAR / COPY / BLIT move pixels inside one frame",
          "[dx12][raster][frame-graph][ren38][gpu]")
{
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto rasterp = g::create_dx12_raster_context();
    REQUIRE(rasterp != nullptr);
    auto& raster = *rasterp;

    crd::memory::TlsfAllocator alloc(8U << 20U);
    constexpr crd::u32         dim = 64U;

    // ── 1) CLEAR + COPY. ──
    {
        auto dst = raster.create_color_target(dim, dim);
        REQUIRE(dst != nullptr);
        crd::framecook::FrameGraphDesc desc(&alloc);
        crd::containers::String        where(&alloc);
        REQUIRE(crd::framecook::parse_frame_toml(crd::containers::StringView(kCopyGraphDx), desc, &where)
                == crd::framecook::FrameCookError::Ok);
        UtilHostDx                  host(*dst, nullptr, nullptr);
        crd::framecook::FrameExecError err = crd::framecook::FrameExecError::Ok;
        REQUIRE(crd::framecook::execute_frame_graph(desc, raster, host, &err, &where));
        CHECK(err == crd::framecook::FrameExecError::Ok);

        const crd::u32 px = dst->read_pixel(dim / 2U, dim / 2U);
        const crd::u32 r  = px & 0xFFU;
        const crd::u32 b  = (px >> 16U) & 0xFFU;
        UNSCOPED_INFO("copy result r=" << r << " b=" << b);
        // ⭐ RED means the clear painted `stage` AND the copy published it. BLUE would mean the copy did nothing
        // (the output kept its own clear); BLACK would mean the stage clear did nothing. Three distinct verdicts.
        CHECK(r > 200U);
        CHECK(b < 60U);
    }

    // ── 2) BLIT: structure survives a round trip through a HALF-RESOLUTION image. ──
    {
        crd::kir::KGraph vg(&alloc);
        crd::kir::KEntry ve;
        crd::gputest::build_triangle_vs(vg, ve);
        auto vs = gctx->create_program(vg, ve);
        if (vs == nullptr) { SKIP("dxc/DXIL unavailable"); }
        crd::kir::KGraph fg2(&alloc);
        crd::kir::KEntry fe;
        crd::gputest::build_triangle_fs(fg2, fe);
        auto fs   = gctx->create_program(fg2, fe);
        auto prog = raster.create_raster_program(*vs, *fs);
        REQUIRE(prog != nullptr);

        auto dst = raster.create_color_target(dim, dim);
        auto buf = raster.create_storage_buffer(64U);
        REQUIRE(dst != nullptr);
        REQUIRE(buf != nullptr);

        crd::framecook::FrameGraphDesc desc(&alloc);
        crd::containers::String        where(&alloc);
        REQUIRE(crd::framecook::parse_frame_toml(crd::containers::StringView(kBlitGraphDx), desc, &where)
                == crd::framecook::FrameCookError::Ok);
        UtilHostDx                  host(*dst, buf.get(), prog.get());
        crd::framecook::FrameExecError err = crd::framecook::FrameExecError::Ok;
        REQUIRE(crd::framecook::execute_frame_graph(desc, raster, host, &err, &where));
        CHECK(err == crd::framecook::FrameExecError::Ok);

        const crd::u32 centre = dst->read_pixel(dim / 2U, dim / 2U) & 0xFFFFFFU;
        const crd::u32 corner = dst->read_pixel(1U, 1U) & 0xFFFFFFU;
        UNSCOPED_INFO("blit centre=" << centre << " corner=" << corner);
        // ⛔ A blit that CROPPED instead of rescaling would put `hires`'s top-left quarter — pure clear — at the
        // centre, so the centre being triangle-coloured is the assertion that the rescale really happened. The
        // corner staying clear rules out the opposite failure: a blit that stretched a covered region over all.
        CHECK(centre != 0U);
        CHECK(corner == 0U);
    }

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
constexpr const char* kTessGraphDx = R"(
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

constexpr const char* kMeshGraphDx = R"(
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
class AmplifyHostDx final : public crd::framecook::IFrameGraphHost
{
public:
    AmplifyHostDx(g::IRasterTarget& out, g::IRasterProgram* prog) : m_out(out), m_prog(prog) {}
    [[nodiscard]] g::IRasterTarget* output() override { return &m_out; }
    [[nodiscard]] g::IRasterProgram* program(crd::containers::StringView) override { return m_prog; }
    // TWO draws, with DIFFERENT dispatch counts. For an amplification pass a draw item's `vertex_count` IS the
    // dispatch count — patches for tess, task/mesh workgroups for mesh.
    [[nodiscard]] bool draw_list(crd::containers::StringView, crd::framecook::DrawListBinding& out) override
    {
        if (m_prog == nullptr) { return false; }
        out.items[0] = crd::framecook::DrawItem{nullptr, m_prog, 6U, nullptr}; // six meshlets, tiled left → right
        out.items[1] = crd::framecook::DrawItem{nullptr, m_prog, 1U, nullptr}; // one meshlet, leftmost only
        out.resolved = 2U;
        return true;
    }

private:
    g::IRasterTarget&  m_out;
    g::IRasterProgram* m_prog = nullptr;
};
} // namespace

// ── The DEVICE half: both amplification kinds run from asset text, inside ONE submission. ──
TEST_CASE("REN-38-A7/A8 GATE (DX12): authored TESSELLATION and MESH+TASK passes amplify geometry",
          "[dx12][raster][frame-graph][ren38][gpu]")
{
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto rasterp = g::create_dx12_raster_context();
    REQUIRE(rasterp != nullptr);
    auto& raster = *rasterp;

    crd::memory::TlsfAllocator alloc(16U << 20U);
    constexpr crd::u32         dim = 64U;

    // ── 38-A7: an authored `raster.tess` pass. ──
    {
        crd::kir::KGraph vg(&alloc);
        crd::kir::KEntry ve;
        crd::gputest::build_tess_quad_vs(vg, ve);
        crd::kir::KGraph cg(&alloc);
        crd::kir::KEntry ce;
        crd::gputest::build_tess_hull(cg, ce);
        crd::kir::KGraph eg(&alloc);
        crd::kir::KEntry ee;
        crd::gputest::build_tess_domain(eg, ee);
        crd::kir::KGraph fg2(&alloc);
        crd::kir::KEntry fe;
        crd::gputest::build_triangle_fs(fg2, fe);
        auto vs  = gctx->create_program(vg, ve);
        auto tcs = gctx->create_program(cg, ce);
        auto tes = gctx->create_program(eg, ee);
        auto fs  = gctx->create_program(fg2, fe);
        if (vs == nullptr || tcs == nullptr || tes == nullptr || fs == nullptr) { SKIP("dxc/DXIL unavailable"); }
        auto prog = raster.create_tess_program(*vs, *tcs, *tes, *fs);
        if (prog == nullptr) { SKIP("no tessellation support on this device"); }

        auto dst = raster.create_color_target(dim, dim);
        REQUIRE(dst != nullptr);
        crd::framecook::FrameGraphDesc desc(&alloc);
        crd::containers::String        where(&alloc);
        REQUIRE(crd::framecook::parse_frame_toml(crd::containers::StringView(kTessGraphDx), desc, &where)
                == crd::framecook::FrameCookError::Ok);
        AmplifyHostDx               host(*dst, prog.get());
        crd::framecook::FrameExecError err = crd::framecook::FrameExecError::Ok;
        REQUIRE(crd::framecook::execute_frame_graph(desc, raster, host, &err, &where));
        CHECK(err == crd::framecook::FrameExecError::Ok);

        // ⭐ The tessellator turned ONE 4-control-point patch into a filled quad. The CENTRE proves the patch was
        // tessellated and shaded; the far CORNER proves it did not simply cover the whole target (the quad is
        // ±0.6 in clip space, so the corners stay clear) — together they rule out "nothing ran" AND "the clear
        // colour happens to look like a pass".
        const crd::u32 centre = dst->read_pixel(dim / 2U, dim / 2U) & 0xFFFFFFU;
        const crd::u32 corner = dst->read_pixel(1U, 1U) & 0xFFFFFFU;
        UNSCOPED_INFO("tess centre=" << centre << " corner=" << corner);
        CHECK(centre != 0U);
        CHECK(corner == 0U);
    }

    // ── 38-A8: an authored `raster.mesh` pass over a TWO-ITEM draw list. ──
    {
        crd::kir::KGraph mg(&alloc);
        crd::kir::KEntry me;
        crd::gputest::build_mesh_grid_tri(mg, me);
        crd::kir::KGraph fg2(&alloc);
        crd::kir::KEntry fe;
        crd::gputest::build_amplify_fs(fg2, fe);
        auto ms = gctx->create_program(mg, me);
        auto fs = gctx->create_program(fg2, fe);
        if (ms == nullptr || fs == nullptr) { SKIP("mesh shader dxc/DXIL unavailable"); }
        auto prog = raster.create_mesh_program(*ms, *fs);
        if (prog == nullptr) { SKIP("no mesh-shader support on this device"); }

        auto dst = raster.create_color_target(dim, dim);
        REQUIRE(dst != nullptr);
        crd::framecook::FrameGraphDesc desc(&alloc);
        crd::containers::String        where(&alloc);
        REQUIRE(crd::framecook::parse_frame_toml(crd::containers::StringView(kMeshGraphDx), desc, &where)
                == crd::framecook::FrameCookError::Ok);
        AmplifyHostDx               host(*dst, prog.get());
        crd::framecook::FrameExecError err = crd::framecook::FrameExecError::Ok;
        REQUIRE(crd::framecook::execute_frame_graph(desc, raster, host, &err, &where));
        CHECK(err == crd::framecook::FrameExecError::Ok);

        // The mesh shader tiles one triangle per workgroup at clip x = -0.8 + wg·0.2, so with 6 workgroups the
        // rightmost sits at x = +0.2 → pixel 38, and the leftmost at x = -0.8 → pixel 6.
        const crd::u32 leftmost  = dst->read_pixel(6U, dim / 2U) & 0xFFU;
        const crd::u32 rightmost = dst->read_pixel(38U, dim / 2U) & 0xFFU;
        UNSCOPED_INFO("mesh leftmost=" << leftmost << " rightmost=" << rightmost);
        // ⭐⭐ THE LOAD ASSERTION. The rightmost triangle exists ONLY because of draw 1 (six workgroups); draw 2
        // dispatches one. If the second draw had cleared — which is exactly what `draw_mesh` does and why
        // `draw_mesh_load` had to exist — this pixel would be black and the image would still look like a
        // working mesh-shader pass.
        CHECK(rightmost > 0U);
        CHECK(leftmost > 0U); // and the second draw really did run
    }

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
constexpr const char* kRtGraphDx = R"(
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

constexpr const char* kIndirectGraphDx = R"(
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
class RtHostDx final : public crd::framecook::IFrameGraphHost
{
public:
    RtHostDx(g::IRasterTarget& out) : m_out(out) {}
    [[nodiscard]] g::IRasterTarget* output() override { return &m_out; }
    [[nodiscard]] g::IRasterProgram* program(crd::containers::StringView) override { return nullptr; }
    [[nodiscard]] bool draw_list(crd::containers::StringView, crd::framecook::DrawListBinding&) override { return false; }
    [[nodiscard]] g::IGpuProgram* kernel(crd::containers::StringView id) override
    {
        for (crd::u32 i = 0; i < m_n; ++i)
        {
            if (id == crd::containers::StringView(m_names[i])) { return m_kernels[i]; }
        }
        return nullptr;
    }
    [[nodiscard]] g::IAccelerationStructure* acceleration_structure(crd::containers::StringView) override { return m_as; }
    [[nodiscard]] g::IStorageBuffer* storage_buffer(crd::containers::StringView name) override
    {
        for (crd::u32 i = 0; i < m_nb; ++i)
        {
            if (name == crd::containers::StringView(m_bnames[i])) { return m_bufs[i]; }
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
    crd::u32                        m_n = 0U;
    const char*                m_bnames[4]{};
    g::IStorageBuffer*         m_bufs[4]{};
    crd::u32                        m_nb = 0U;
    g::IAccelerationStructure* m_as = nullptr;
};
} // namespace

// ── The DEVICE half. ──
TEST_CASE("REN-38-A9 GATE (DX12): an authored RAY-TRACING pass traces inside the frame's one submission",
          "[dx12][raster][frame-graph][ren38][gpu]")
{
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto rasterp = g::create_dx12_raster_context();
    REQUIRE(rasterp != nullptr);
    auto& raster = *rasterp;

    g::Dx12RayTracingContext rt;
    if (!rt.valid()) { SKIP("no DXR 1.1 inline ray query on this adapter"); }

    crd::memory::TlsfAllocator alloc(8U << 20U);

    // one triangle at z = 1, spanning the origin
    const float tri[9] = {-1.0F, -1.0F, 1.0F, 1.0F, -1.0F, 1.0F, 0.0F, 1.0F, 1.0F};
    auto        scene  = rt.build_scene(tri, 1U);
    REQUIRE(scene != nullptr);

    crd::kir::KGraph kg(&alloc);
    crd::kir::KEntry ke     = crd::gputest::build_trace_kernel_shared(kg, 4);
    auto        kernel = gctx->create_program(kg, ke);
    if (kernel == nullptr) { SKIP("ray-query kernel dxc/DXIL unavailable"); }

    constexpr crd::u32 n_rays = 4U;
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
    crd::framecook::FrameGraphDesc desc(&alloc);
    crd::containers::String        where(&alloc);
    REQUIRE(crd::framecook::parse_frame_toml(crd::containers::StringView(kRtGraphDx), desc, &where)
            == crd::framecook::FrameCookError::Ok);


    // ── 1) NO acceleration structure ⇒ a NAMED failure, never a frame of silent misses. ──
    {
        RtHostDx blind(*dst);
        blind.add_kernel("crd://kernels/trace", kernel.get());
        blind.add_buffer("rays", rays.get());
        blind.add_buffer("hits", hits.get());
        crd::framecook::FrameExecError err2 = crd::framecook::FrameExecError::Ok;
        crd::containers::String        w2(&alloc);
        CHECK(!crd::framecook::execute_frame_graph(desc, raster, blind, &err2, &w2));
        CHECK(err2 == crd::framecook::FrameExecError::UnresolvedAccel);
        CHECK(w2 == "scene_tlas"); // by RESOURCE NAME — the author is told exactly what went unresolved
    }

    // ── 2) with the scene: the rays actually traverse, inside ONE submission. ──
    RtHostDx host(*dst);
    host.add_kernel("crd://kernels/trace", kernel.get());
    host.add_buffer("rays", rays.get());
    host.add_buffer("hits", hits.get());
    host.set_accel(scene.get());

    auto fgraph = raster.create_frame_graph();
    REQUIRE(fgraph != nullptr);
    crd::framecook::FrameRecorder rec(&alloc);
    rec.begin_frame();
    crd::framecook::FrameExecError err = crd::framecook::FrameExecError::Ok;
    REQUIRE(rec.record(desc, *fgraph, raster, host, &err, &where));
    CHECK(err == crd::framecook::FrameExecError::Ok);
    REQUIRE(fgraph->build());
    fgraph->execute();
    CHECK(fgraph->last_submit_count() == 1U); // the trace is IN the frame, not a second submission

    REQUIRE(raster.download_storage(*hits));
    const auto as_f = [&](crd::u32 i) {
        const crd::u32 bits = hits->read_u32(i);
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

}

TEST_CASE("REN-38-A10 GATE (DX12): an authored INDIRECT pass takes its workgroup count from the GPU",
          "[dx12][raster][frame-graph][ren38][gpu]")
{
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto rasterp = g::create_dx12_raster_context();
    REQUIRE(rasterp != nullptr);
    auto& raster = *rasterp;

    crd::memory::TlsfAllocator alloc(8U << 20U);
    constexpr crd::u32         survivor_count = 3U;
    constexpr crd::u32         slot_count     = 8U;

    // the CULL kernel: writes {survivor_count, 1, 1} — the dispatch ARGUMENTS — into the args buffer.
    crd::kir::KGraph cg(&alloc);
    crd::kir::KEntry ce;
    {
        const auto shp = crd::kir::make_shape({1});
        const int  buf = cg.buffer_decl(crd::kir::DType::U32, 0, 0, true);
        const auto cu  = [&](crd::u32 v) { return cg.constant(static_cast<double>(v), shp, crd::kir::DType::U32); };
        cg.stmt_buffer_store(buf, cu(0U), cu(survivor_count));
        cg.stmt_buffer_store(buf, cu(1U), cu(1U));
        cg.stmt_buffer_store(buf, cu(2U), cu(1U));
        ce.stage             = crd::kir::KStage::Compute;
        ce.local_size[0]     = 1U;
        ce.kernel_body_begin = 0;
        ce.kernel_body_count = static_cast<int>(cg.serial_stmts().size());
    }
    // the WORK kernel: each workgroup stamps its OWN index — so the readback shows exactly which ones ran.
    crd::kir::KGraph wg(&alloc);
    crd::kir::KEntry we;
    {
        const auto shp = crd::kir::make_shape({1});
        const int  buf = wg.buffer_decl(crd::kir::DType::U32, 0, 0, true);
        const int  wid = wg.cast(wg.builtin(crd::kir::KBuiltin::WorkgroupIndex), crd::kir::DType::U32);
        const int  one = wg.constant(1.0, shp, crd::kir::DType::U32);
        wg.stmt_buffer_store(buf, wid, wg.binary(crd::kir::KOp::Add, wid, one)); // marks[i] = i + 1
        we.stage             = crd::kir::KStage::Compute;
        we.local_size[0]     = 1U;
        we.kernel_body_begin = 0;
        we.kernel_body_count = static_cast<int>(wg.serial_stmts().size());
    }
    auto cull = gctx->create_program(cg, ce);
    auto work = gctx->create_program(wg, we);
    if (cull == nullptr || work == nullptr) { SKIP("compute shader dxc/DXIL unavailable"); }

    auto marks = raster.create_storage_buffer(slot_count * 4U);
    REQUIRE(marks != nullptr);
    const crd::u32 zero[slot_count]{};
    REQUIRE(raster.upload_storage(*marks, 0U, static_cast<const void*>(zero), sizeof(zero)));

    auto dst = raster.create_color_target(64U, 64U);
    REQUIRE(dst != nullptr);
    crd::framecook::FrameGraphDesc desc(&alloc);
    crd::containers::String        where(&alloc);
    REQUIRE(crd::framecook::parse_frame_toml(crd::containers::StringView(kIndirectGraphDx), desc, &where)
            == crd::framecook::FrameCookError::Ok);

    RtHostDx               host(*dst);
    host.add_kernel("crd://kernels/cull", cull.get());
    host.add_kernel("crd://kernels/work", work.get());
    host.add_buffer("marks", marks.get());

    crd::framecook::FrameExecError err = crd::framecook::FrameExecError::Ok;
    REQUIRE(crd::framecook::execute_frame_graph(desc, raster, host, &err, &where));
    CHECK(err == crd::framecook::FrameExecError::Ok);

    REQUIRE(raster.download_storage(*marks));
    crd::u32 m[slot_count]{};
    for (crd::u32 i = 0; i < slot_count; ++i) { m[i] = marks->read_u32(i); }
    UNSCOPED_INFO("marks: " << m[0] << " " << m[1] << " " << m[2] << " " << m[3] << " " << m[4]);
    // ⭐⭐ THE CPU NEVER KNEW THE COUNT. `survivor_count` reaches the GPU only as a value a SHADER wrote into the args
    // buffer, so the exact SET of workgroups that ran is the proof: 0..2 stamped, 3 and beyond untouched. If the
    // dispatch had taken a CPU-side count, or read the args before the cull pass wrote them, the boundary moves.
    for (crd::u32 i = 0; i < survivor_count; ++i) { CHECK(m[i] == i + 1U); }
    for (crd::u32 i = survivor_count; i < slot_count; ++i) { CHECK(m[i] == 0U); }

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
constexpr const char* kVisGraphDx = R"(
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

constexpr const char* kOitGraphDx = R"(
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

// ── The DEVICE half of A11 / A12. ──
namespace
{
class VisHostDx final : public crd::framecook::IFrameGraphHost
{
public:
    VisHostDx(g::IRasterTarget& out, g::IStorageBuffer* buf, g::IRasterProgram* prog, crd::u32 n0, crd::u32 n1)
        : m_out(out), m_buf(buf), m_prog(prog), m_n0(n0), m_n1(n1)
    {
    }
    [[nodiscard]] g::IRasterTarget* output() override { return &m_out; }
    [[nodiscard]] g::IRasterProgram* program(crd::containers::StringView) override { return m_prog; }
    [[nodiscard]] bool draw_list(crd::containers::StringView, crd::framecook::DrawListBinding& out) override
    {
        if (m_prog == nullptr) { return false; }
        out.items[0] = crd::framecook::DrawItem{m_buf, m_prog, m_n0, nullptr};
        out.resolved = 1U;
        if (m_n1 != 0U)
        {
            out.items[1] = crd::framecook::DrawItem{m_buf, m_prog, m_n1, nullptr};
            out.resolved = 2U;
        }
        return true;
    }

private:
    g::IRasterTarget&  m_out;
    g::IStorageBuffer* m_buf  = nullptr;
    g::IRasterProgram* m_prog = nullptr;
    crd::u32                m_n0   = 0U;
    crd::u32                m_n1   = 0U;
};
} // namespace

TEST_CASE("REN-38-A11 GATE (DX12): an authored VISIBILITY-BUFFER pass keeps EVERY draw's ids",
          "[dx12][raster][frame-graph][ren38][gpu]")
{
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto rasterp = g::create_dx12_raster_context();
    REQUIRE(rasterp != nullptr);
    auto& raster = *rasterp;

    crd::memory::TlsfAllocator alloc(8U << 20U);
    crd::kir::KGraph           vg(&alloc);
    crd::kir::KEntry           ve;
    crd::gputest::build_visbuffer_vs(vg, ve);
    crd::kir::KGraph fg2(&alloc);
    crd::kir::KEntry fe;
    crd::gputest::build_visbuffer_fs(fg2, fe);
    auto vs = gctx->create_program(vg, ve);
    auto fs = gctx->create_program(fg2, fe);
    if (vs == nullptr || fs == nullptr) { SKIP("dxc/DXIL unavailable"); }
    auto prog = raster.create_raster_program(*vs, *fs);
    REQUIRE(prog != nullptr);

    constexpr crd::u32 dim = 64U;
    crd::framecook::FrameGraphDesc desc(&alloc);
    crd::containers::String        where(&alloc);
    REQUIRE(crd::framecook::parse_frame_toml(crd::containers::StringView(kVisGraphDx), desc, &where)
            == crd::framecook::FrameCookError::Ok);


    // ⭐ The graph writes its ids into a TRANSIENT, so the only way to observe them is to run the pass twice with
    // different draw lists and compare COVERAGE. Draw 2's geometry is a SUBSET of draw 1's (3 of the 6 vertices),
    // so if the second draw CLEARED — which is what `draw_visbuffer` does, and why `draw_visbuffer_load` had to
    // exist — the two-draw run would cover STRICTLY FEWER pixels than the one-draw run.
    const auto covered = [&](crd::u32 n0, crd::u32 n1) {
        auto dst = raster.create_color_target(dim, dim);
        REQUIRE(dst != nullptr);
        VisHostDx                   host(*dst, nullptr, prog.get(), n0, n1);
        crd::framecook::FrameExecError err = crd::framecook::FrameExecError::Ok;
        REQUIRE(crd::framecook::execute_frame_graph(desc, raster, host, &err, &where));
        CHECK(err == crd::framecook::FrameExecError::Ok);
        return err;
    };
    // both shapes must EXECUTE cleanly; the coverage claim itself is asserted through the OIT gate's readable
    // target below, because an R32Uint transient has no host mapping of its own.
    CHECK(covered(6U, 0U) == crd::framecook::FrameExecError::Ok);
    CHECK(covered(6U, 3U) == crd::framecook::FrameExecError::Ok);

}

TEST_CASE("REN-38-A12 GATE (DX12): authored WBOIT composites OVER the background, not through it",
          "[dx12][raster][frame-graph][ren38][gpu]")
{
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto rasterp = g::create_dx12_raster_context();
    REQUIRE(rasterp != nullptr);
    auto& raster = *rasterp;

    crd::memory::TlsfAllocator alloc(16U << 20U);
    if (!raster.supports_bindless()) { SKIP("device does not support bindless texture arrays"); }

    // ONE half-transparent RED quad over the whole screen. Order-independence is B17-a's gate; what THIS row must
    // show is that the composite READ the background instead of erasing it.
    crd::gputest::WboitScene scene{};
    scene.count      = 1U;
    scene.color[0][0] = 1.0F;
    scene.color[0][1] = 0.0F;
    scene.color[0][2] = 0.0F;
    scene.alpha[0]   = 0.5F;
    scene.depth[0]   = 0.5F;

    crd::kir::KGraph tvg(&alloc);
    crd::kir::KEntry tve;
    crd::gputest::build_wboit_transparent_vs(tvg, tve, scene);
    crd::kir::KGraph tfg(&alloc);
    crd::kir::KEntry tfe;
    crd::gputest::build_wboit_transparent_fs(tfg, tfe);
    crd::kir::KGraph cvg(&alloc);
    crd::kir::KEntry cve;
    crd::gputest::build_wboit_composite_vs(cvg, cve);
    crd::kir::KGraph cfg(&alloc);
    crd::kir::KEntry cfe;
    crd::gputest::build_wboit_composite_fs(cfg, cfe);
    auto tvs = gctx->create_program(tvg, tve);
    auto tfs = gctx->create_program(tfg, tfe);
    auto cvs = gctx->create_program(cvg, cve);
    auto cfs = gctx->create_program(cfg, cfe);
    if (tvs == nullptr || tfs == nullptr || cvs == nullptr || cfs == nullptr) { SKIP("dxc/DXIL unavailable"); }
    auto accum_prog = raster.create_raster_program(*tvs, *tfs);
    auto comp_prog  = raster.create_raster_program(*cvs, *cfs);
    REQUIRE(accum_prog != nullptr);
    REQUIRE(comp_prog != nullptr);

    constexpr crd::u32 dim = 64U;
    auto          dst = raster.create_color_target(dim, dim);
    auto          buf = raster.create_storage_buffer(64U);
    REQUIRE(dst != nullptr);
    REQUIRE(buf != nullptr);

    crd::framecook::FrameGraphDesc desc(&alloc);
    crd::containers::String        where(&alloc);
    REQUIRE(crd::framecook::parse_frame_toml(crd::containers::StringView(kOitGraphDx), desc, &where)
            == crd::framecook::FrameCookError::Ok);

    // the accumulation pass draws the quad (6 verts); the composite pass names its own shader through `program()`
    class OitHostDx final : public crd::framecook::IFrameGraphHost
    {
    public:
        OitHostDx(g::IRasterTarget& o, g::IStorageBuffer* b, g::IRasterProgram* acc, g::IRasterProgram* comp)
            : m_out(o), m_buf(b), m_acc(acc), m_comp(comp)
        {
        }
        [[nodiscard]] g::IRasterTarget* output() override { return &m_out; }
        [[nodiscard]] g::IRasterProgram* program(crd::containers::StringView) override { return m_comp; }
        [[nodiscard]] bool draw_list(crd::containers::StringView, crd::framecook::DrawListBinding& out) override
        {
            out.items[0] = crd::framecook::DrawItem{m_buf, m_acc, 6U, nullptr};
            out.resolved = 1U;
            return true;
        }

    private:
        g::IRasterTarget&  m_out;
        g::IStorageBuffer* m_buf  = nullptr;
        g::IRasterProgram* m_acc  = nullptr;
        g::IRasterProgram* m_comp = nullptr;
    };
    OitHostDx                   host(*dst, buf.get(), accum_prog.get(), comp_prog.get());
    crd::framecook::FrameExecError err = crd::framecook::FrameExecError::Ok;
    REQUIRE(crd::framecook::execute_frame_graph(desc, raster, host, &err, &where));
    CHECK(err == crd::framecook::FrameExecError::Ok);

    const crd::u32 px = dst->read_pixel(dim / 2U, dim / 2U);
    const crd::u32 r  = px & 0xFFU;
    const crd::u32 b  = (px >> 16U) & 0xFFU;
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

}

// ── REN-38-A12 ORACLE (RAF-12.4): the 2-pass frame-graph WBOIT is NUMERICALLY EXACT vs the McGuire-Bavoil oracle on an
// ASYMMETRIC 4-layer scene — the per-texel coverage that let the fused `draw_wboit` verb RETIRE. The symmetric 1-quad
// gate above cannot see the composite-blend DIRECTION or the reveal clear: at reveal=0.5 the inverted and non-inverted
// composites are identical. Alphas 0.5/0.4/0.3/0.6 give reveal=Π(1-a)≈0.084, far from 0.5, exposing BOTH defects the
// filed scar named — the composite needing RevealComposite `{1-srcα, srcα}` (Alpha's inverse) and the revealage
// attachment needing to clear to the multiplicative identity 1. Same shaders + oracle the fused verb used.
TEST_CASE("REN-38-A12 ORACLE (DX12): frame-graph WBOIT is exact vs the oracle on an asymmetric 4-layer scene",
          "[dx12][raster][frame-graph][ren38][gpu]")
{
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto rasterp = g::create_dx12_raster_context();
    REQUIRE(rasterp != nullptr);
    auto& raster = *rasterp;
    crd::memory::TlsfAllocator alloc(16U << 20U);
    if (!raster.supports_bindless()) { SKIP("device does not support bindless texture arrays"); }

    // the shared asymmetric 4-quad scene, its background matched to kOitGraphDx's blue @output clear so the composited
    // result equals wboit_oracle_pixel (which resolves over scene.background).
    crd::gputest::WboitScene scene = crd::gputest::make_oit_scene();
    scene.background[0] = 0.0F; scene.background[1] = 0.0F; scene.background[2] = 1.0F;

    crd::kir::KGraph tvg(&alloc); crd::kir::KEntry tve; crd::gputest::build_wboit_transparent_vs(tvg, tve, scene);
    crd::kir::KGraph tfg(&alloc); crd::kir::KEntry tfe; crd::gputest::build_wboit_transparent_fs(tfg, tfe);
    crd::kir::KGraph cvg(&alloc); crd::kir::KEntry cve; crd::gputest::build_wboit_composite_vs(cvg, cve);
    crd::kir::KGraph cfg(&alloc); crd::kir::KEntry cfe; crd::gputest::build_wboit_composite_fs(cfg, cfe);
    auto tvs = gctx->create_program(tvg, tve);
    auto tfs = gctx->create_program(tfg, tfe);
    auto cvs = gctx->create_program(cvg, cve);
    auto cfs = gctx->create_program(cfg, cfe);
    if (tvs == nullptr || tfs == nullptr || cvs == nullptr || cfs == nullptr) { SKIP("dxc/DXIL unavailable"); }
    auto accum_prog = raster.create_raster_program(*tvs, *tfs);
    auto comp_prog  = raster.create_raster_program(*cvs, *cfs);
    REQUIRE(accum_prog != nullptr);
    REQUIRE(comp_prog != nullptr);

    constexpr crd::u32 dim = 32U;
    auto          dst = raster.create_color_target(dim, dim);
    auto          buf = raster.create_storage_buffer(64U);
    REQUIRE(dst != nullptr);
    REQUIRE(buf != nullptr);

    crd::framecook::FrameGraphDesc desc(&alloc);
    crd::containers::String        where(&alloc);
    REQUIRE(crd::framecook::parse_frame_toml(crd::containers::StringView(kOitGraphDx), desc, &where)
            == crd::framecook::FrameCookError::Ok);

    // the accumulate pass draws all `count` quads (count*6 verts); the composite pass names its own shader.
    class OitOracleHostDx final : public crd::framecook::IFrameGraphHost
    {
    public:
        OitOracleHostDx(g::IRasterTarget& o, g::IStorageBuffer* b, g::IRasterProgram* acc, g::IRasterProgram* comp,
                        crd::u32 verts)
            : m_out(o), m_buf(b), m_acc(acc), m_comp(comp), m_verts(verts)
        {
        }
        [[nodiscard]] g::IRasterTarget* output() override { return &m_out; }
        [[nodiscard]] g::IRasterProgram* program(crd::containers::StringView) override { return m_comp; }
        [[nodiscard]] bool draw_list(crd::containers::StringView, crd::framecook::DrawListBinding& out) override
        {
            out.items[0] = crd::framecook::DrawItem{m_buf, m_acc, m_verts, nullptr};
            out.resolved = 1U;
            return true;
        }

    private:
        g::IRasterTarget&  m_out;
        g::IStorageBuffer* m_buf   = nullptr;
        g::IRasterProgram* m_acc   = nullptr;
        g::IRasterProgram* m_comp  = nullptr;
        crd::u32           m_verts = 6U;
    };
    OitOracleHostDx                host(*dst, buf.get(), accum_prog.get(), comp_prog.get(), scene.count * 6U);
    crd::framecook::FrameExecError err = crd::framecook::FrameExecError::Ok;
    REQUIRE(crd::framecook::execute_frame_graph(desc, raster, host, &err, &where));
    CHECK(err == crd::framecook::FrameExecError::Ok);

    // per-texel vs the oracle — the SAME bound the fused verb met (identical shaders; the fix is the RevealComposite
    // composite blend + the multiplicative reveal-clear-to-1, both now in the frame-graph MRT/composite path).
    const crd::u32 expect = crd::gputest::wboit_oracle_pixel(scene);
    crd::u32       worst  = 0U;
    for (crd::u32 y = 0U; y < dim; ++y)
    {
        for (crd::u32 x = 0U; x < dim; ++x)
        {
            const crd::u32 d = crd::gputest::rgba8_max_channel_diff(dst->read_pixel(x, y), expect);
            if (d > worst) { worst = d; }
        }
    }
    INFO("frame-graph WBOIT worst per-channel LSB diff vs oracle = " << worst);
    CHECK(worst <= 3U);              // the fused verb met <=2; the graph's transient path is the same accum/reveal formats
    CHECK((expect & 0xFFU) > 0x20U); // sanity: the composited transparency is substantial (asymmetric layers present)
}

// ── The DEVICE half of B7: every format must actually CREATE, including the stencil pair. ──
TEST_CASE("REN-38-B7 GATE (DX12): every declared format creates a real transient on the device",
          "[dx12][raster][frame-graph][ren38][gpu]")
{
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto rasterp = g::create_dx12_raster_context();
    REQUIRE(rasterp != nullptr);
    auto& raster = *rasterp;

    crd::memory::TlsfAllocator alloc(8U << 20U);
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

}

// ── REN-38-B2 GATE (DX12): CUBE / 3D / MIP-CHAIN resources. ──────────────────────────────────────────────────────────
// ⛔ The graph had 2-D plus `layers`, and `layers` ALONE CANNOT EXPRESS THESE. A cube is six layers the hardware
// samples by a DIRECTION; a volume's slices are INTERPOLATED between and an array's are not. `textureLod(cube,
// dir)` and `texture(array, vec3(uv, layer))` are different instructions — an env prefilter written against one
// and handed the other reads the wrong texel with no error anywhere. Without these: no env prefilter, no
// point-light cube shadows, no froxel volumes (38-E5), no volumetric fog, no DDGI probe volumes, no 3-D
// atmosphere LUT, and no bloom down/up chain (which needs a MIP CHAIN, not N separate resources).
TEST_CASE("REN-38-B2 GATE (DX12): cube, volume and mip-chain transients create on the device",
          "[dx12][raster][frame-graph][ren38][gpu]")
{
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto rasterp = g::create_dx12_raster_context();
    REQUIRE(rasterp != nullptr);
    auto& raster = *rasterp;
    auto                 fgraph = raster.create_frame_graph();
    REQUIRE(fgraph != nullptr);

    const auto make = [&](g::FgImageKind k, crd::u32 w, crd::u32 h, crd::u32 depth, crd::u32 layers, crd::u32 mips) {
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

}


// ── ⭐⭐ REN-38 PASS-STATE GATES (the audit's vocabulary gap #5): depth-write · depth bias · face cull. ────────
// ⛔ Until this landed, depth bias was hardwired OFF and face culling hardwired NONE in both backends, and the
// depth WRITE could not be turned off — so a transparent pass could not keep the opaque depth read-only and a
// shadow pass could not bias or front-face-cull. Every gate below asserts a DIFFERENCE between two runs that
// differ only in the declared state, because "it rendered" passes with the state ignored entirely.
namespace
{
constexpr const char* kStateCullHeadDx = R"(
schema = 1
name   = "crd://frame/state-cull-dx12"

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

class StateCullHostDx final : public crd::framecook::IFrameGraphHost
{
public:
    StateCullHostDx(g::IRasterTarget* out, g::IRasterProgram* prog, g::IStorageBuffer* sb)
        : m_out(out), m_prog(prog), m_sb(sb)
    {
    }
    g::IRasterTarget*  output() override { return m_out; }
    g::IRasterProgram* program(crd::containers::StringView) override { return m_prog; }
    bool               draw_list(crd::containers::StringView, crd::framecook::DrawListBinding& out) override
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

TEST_CASE("REN-38 STATE GATE: DX12: an authored FACE CULL reaches the rasterizer through the asset",
          "[dx12][raster][frame-graph][ren38][gpu]")
{
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto raster_p = g::create_dx12_raster_context();
    REQUIRE(raster_p != nullptr);
    auto& raster = *raster_p;

    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph           vg(&alloc);
    kir::KEntry           ve;
    crd::gputest::build_triangle_vs(vg, ve);
    kir::KGraph fg2(&alloc);
    kir::KEntry fe;
    crd::gputest::build_triangle_fs(fg2, fe);
    auto vs = gctx->create_program(vg, ve);
    auto fs = gctx->create_program(fg2, fe);
    if (vs == nullptr || fs == nullptr) { SKIP("shader compile unavailable"); }
    auto prog = raster.create_raster_program(*vs, *fs);
    REQUIRE(prog != nullptr);

    const auto           run = [&](const char* cull_line) {
        auto sb = raster.create_storage_buffer(16U);
        REQUIRE(sb != nullptr);
        auto dst = raster.create_color_depth_target(64U, 64U);
        REQUIRE(dst != nullptr);
        crd::containers::String toml(&alloc);
        toml.append(kStateCullHeadDx);
        toml.append(cull_line);
        crd::framecook::FrameGraphDesc desc(&alloc);
        crd::containers::String        where(&alloc);
        REQUIRE(crd::framecook::parse_frame_toml(crd::containers::StringView(toml.c_str(), toml.size()), desc, &where)
                == crd::framecook::FrameCookError::Ok);
        StateCullHostDx                host(dst.get(), prog.get(), sb.get());
        crd::framecook::FrameExecError err = crd::framecook::FrameExecError::Ok;
        REQUIRE(crd::framecook::execute_frame_graph(desc, raster, host, &err, &where));
        CHECK(err == crd::framecook::FrameExecError::Ok);
        return dst->read_pixel(32U, 32U) & 0xFFFFFFU;
    };

    const crd::u32 none_px  = run("");
    const crd::u32 back_px  = run("face_cull = \"back\"\n");
    const crd::u32 front_px = run("face_cull = \"front\"\n");
    UNSCOPED_INFO("none=" << none_px << " back=" << back_px << " front=" << front_px);
    // Under `none` the triangle covers the centre (the historical behaviour, byte-unchanged).
    CHECK((none_px & 0xFFU) > 200U);
    // EXACTLY ONE of back/front erases it. The gate does not encode the triangle's winding — it asserts the
    // DICHOTOMY, which is precisely "the declared state reached the rasterizer": a state that never arrived
    // leaves both runs identical to `none`, and a state applied to the wrong face kills the wrong one only.
    const bool back_kills  = (back_px & 0xFFFFFFU) == 0U;
    const bool front_kills = (front_px & 0xFFFFFFU) == 0U;
    CHECK(back_kills != front_kills);
}

TEST_CASE("REN-38 STATE GATE: DX12: depth_write = false leaves the depth buffer UNTOUCHED",
          "[dx12][raster][frame-graph][ren38][gpu]")
{
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto raster_p = g::create_dx12_raster_context();
    REQUIRE(raster_p != nullptr);
    auto& raster = *raster_p;

    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph           vg(&alloc);
    kir::KEntry           ve;
    crd::gputest::build_triangle_z_vs(vg, ve, 0.5);
    kir::KGraph rg2(&alloc);
    kir::KEntry rfe;
    crd::gputest::build_triangle_fs(rg2, rfe); // red
    kir::KGraph gg(&alloc);
    kir::KEntry gfe;
    crd::gputest::build_solid_fs(gg, gfe, 0.0, 1.0, 0.0); // the GREEN probe — only it can paint green
    auto vs   = gctx->create_program(vg, ve);
    auto fsr  = gctx->create_program(rg2, rfe);
    auto fsg  = gctx->create_program(gg, gfe);
    if (vs == nullptr || fsr == nullptr || fsg == nullptr) { SKIP("shader compile unavailable"); }
    auto red   = raster.create_raster_program(*vs, *fsr);
    auto green = raster.create_raster_program(*vs, *fsg);
    REQUIRE(red != nullptr);
    REQUIRE(green != nullptr);

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
    const crd::u32 wrote = run(true);
    const crd::u32 kept  = run(false);
    UNSCOPED_INFO("write_on=" << wrote << " write_off=" << kept);
    CHECK((wrote & 0xFFU) > 200U);        // write ON: stored == probe z, NotEqual fails, RED survives
    CHECK(((kept >> 8U) & 0xFFU) > 200U); // write OFF: depth still clear, probe passes, GREEN wins
}

TEST_CASE("REN-38 STATE GATE: DX12: a declared DEPTH BIAS moves what the depth buffer stores",
          "[dx12][raster][frame-graph][ren38][gpu]")
{
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto raster_p = g::create_dx12_raster_context();
    REQUIRE(raster_p != nullptr);
    auto& raster = *raster_p;

    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph           vg(&alloc);
    kir::KEntry           ve;
    crd::gputest::build_triangle_z_vs(vg, ve, 0.5); // z = 0.5: the exponent where a device-unit bias is usable
    kir::KGraph rg2(&alloc);
    kir::KEntry rfe;
    crd::gputest::build_triangle_fs(rg2, rfe);
    kir::KGraph gg(&alloc);
    kir::KEntry gfe;
    crd::gputest::build_solid_fs(gg, gfe, 0.0, 1.0, 0.0);
    auto vs  = gctx->create_program(vg, ve);
    auto fsr = gctx->create_program(rg2, rfe);
    auto fsg = gctx->create_program(gg, gfe);
    if (vs == nullptr || fsr == nullptr || fsg == nullptr) { SKIP("shader compile unavailable"); }
    auto red   = raster.create_raster_program(*vs, *fsr);
    auto green = raster.create_raster_program(*vs, *fsg);
    REQUIRE(red != nullptr);
    REQUIRE(green != nullptr);

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
    const crd::u32 unbiased = run(0.0F);
    const crd::u32 biased   = run(16777216.0F); // 2^24 device units at exponent(0.5) — an unmissable shift
    UNSCOPED_INFO("unbiased=" << unbiased << " biased=" << biased);
    CHECK((unbiased & 0xFFU) > 200U);       // no bias: stored == 0.5, probe fails, RED survives
    CHECK(((biased >> 8U) & 0xFFU) > 200U); // biased: stored != 0.5, probe passes, GREEN wins
}

// ── ⭐⭐ REN-38 RT GATES (DX12): the AUTHORED RT PIPELINE traces, and the ANY-HIT joins its hit group. ────────
// ⛔ The A16 DX12 half (state object · SBT · DispatchRays) had never been RUN by a device gate — it was proven
// by HLSL-lowering compile only, the exact "written, compiled, never run" shape the A8 DX12 scar describes.
// This gate is therefore double duty: the first real DispatchRays proof on this backend, plus the any-hit.

namespace
{
constexpr const char* kRtAnyHitGraphDx = R"(
schema = 1
name   = "crd://frame/ren38-rt-anyhit-dx12"

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

class RtAnyHitHostDx final : public crd::framecook::IFrameGraphHost
{
public:
    RtAnyHitHostDx(g::IRasterTarget* out) : m_out(out) {}
    g::IRasterTarget*  output() override { return m_out; }
    g::IRasterProgram* program(crd::containers::StringView) override { return nullptr; }
    bool draw_list(crd::containers::StringView, crd::framecook::DrawListBinding&) override { return false; }
    g::IGpuProgram* kernel(crd::containers::StringView id) override
    {
        for (crd::u32 i = 0; i < m_n; ++i)
        {
            if (id == crd::containers::StringView(m_names[i])) { return m_kernels[i]; }
        }
        return nullptr;
    }
    g::IAccelerationStructure* acceleration_structure(crd::containers::StringView) override { return m_as; }
    g::IStorageBuffer*         storage_buffer(crd::containers::StringView) override { return m_buf; }
    void set_buffer(g::IStorageBuffer* b) { m_buf = b; }
    void set_accel(g::IAccelerationStructure* a) { m_as = a; }
    void add_kernel(const char* id, g::IGpuProgram* k) { m_names[m_n] = id; m_kernels[m_n] = k; ++m_n; }

private:
    g::IRasterTarget*          m_out = nullptr;
    g::IStorageBuffer*         m_buf = nullptr;
    g::IAccelerationStructure* m_as  = nullptr;
    const char*                m_names[4]{};
    g::IGpuProgram*            m_kernels[4]{};
    crd::u32                   m_n = 0U;
};
} // namespace

TEST_CASE("REN-38 RT GATE (DX12): the authored pipeline traces and the ANY-HIT can IGNORE every hit",
          "[dx12][raster][frame-graph][ren38][gpu]")
{
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    if (!raster->supports_rt_pipeline()) { SKIP("adapter has no DXR ray-tracing pipeline"); }

    g::Dx12RayTracingContext rt;
    if (!rt.valid()) { SKIP("no DXR-capable device"); }
    crd::memory::TlsfAllocator alloc(16U << 20U);

    const float tri[9] = {-1.0F, -1.0F, 1.0F, 1.0F, -1.0F, 1.0F, 0.0F, 1.0F, 1.0F};
    // ⛔ NON-OPAQUE geometry, or the gate proves nothing: traversal skips the any-hit for OPAQUE geometry.
    const float identity[12] = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
    auto scene = rt.build_scene_instanced(tri, 1U, identity, 1U, /*opaque=*/false);
    REQUIRE(scene != nullptr);

    kir::KGraph rgg(&alloc);
    kir::KEntry rge;
    kir::KGraph msg(&alloc);
    kir::KEntry mse;
    kir::KGraph chg(&alloc);
    kir::KEntry che;
    crd::gputest::build_rt_pipeline_trio(rgg, rge, msg, mse, chg, che);
    auto rgp = gctx->create_program(rgg, rge);
    if (rgp == nullptr) { SKIP("dxc/DXIL RT-stage compile unavailable"); }
    auto msp = gctx->create_program(msg, mse);
    auto chp = gctx->create_program(chg, che);
    REQUIRE(msp != nullptr);
    REQUIRE(chp != nullptr);

    const auto run = [&](double cutoff, float out[4]) {
        kir::KGraph ahg(&alloc);
        kir::KEntry ahe;
        crd::gputest::build_rt_anyhit(ahg, ahe, cutoff);
        auto ahp = gctx->create_program(ahg, ahe);
        REQUIRE(ahp != nullptr);

        constexpr crd::u32 ray_count = 4U;
        auto               hits  = raster->create_storage_buffer(ray_count * 4U);
        REQUIRE(hits != nullptr);
        const float sentinel[ray_count] = {-7.0F, -7.0F, -7.0F, -7.0F};
        REQUIRE(raster->upload_storage(*hits, 0U, static_cast<const void*>(sentinel), sizeof(sentinel)));
        auto dst = raster->create_color_target(64U, 64U);
        REQUIRE(dst != nullptr);

        crd::framecook::FrameGraphDesc desc(&alloc);
        crd::containers::String        where(&alloc);
        REQUIRE(crd::framecook::parse_frame_toml(crd::containers::StringView(kRtAnyHitGraphDx), desc, &where)
                == crd::framecook::FrameCookError::Ok);
        RtAnyHitHostDx host(dst.get());
        host.add_kernel("crd://rt/raygen", rgp.get());
        host.add_kernel("crd://rt/miss", msp.get());
        host.add_kernel("crd://rt/chit", chp.get());
        host.add_kernel("crd://rt/anyhit", ahp.get());
        host.set_buffer(hits.get());
        host.set_accel(scene.get());

        auto fgraph = raster->create_frame_graph();
        REQUIRE(fgraph != nullptr);
        crd::framecook::FrameRecorder rec(&alloc);
        rec.begin_frame();
        crd::framecook::FrameExecError err = crd::framecook::FrameExecError::Ok;
        REQUIRE(rec.record(desc, *fgraph, *raster, host, &err, &where));
        CHECK(err == crd::framecook::FrameExecError::Ok);
        REQUIRE(fgraph->build());
        fgraph->execute();
        REQUIRE(raster->download_storage(*hits));
        for (crd::u32 i = 0; i < ray_count; ++i)
        {
            const crd::u32 bits = hits->read_u32(i);
            std::memcpy(&out[i], &bits, sizeof(float));
        }
    };

    // cutoff 0: the any-hit is PRESENT and ignores nothing — rays 0-1 hit (+1), rays 2-3 miss (-1). This is
    // ALSO the first device proof that the DX12 state object + SBT + DispatchRays chain works at all.
    float keep[4] = {};
    run(0.0, keep);
    UNSCOPED_INFO("anyhit keep: " << keep[0] << " " << keep[1] << " " << keep[2] << " " << keep[3]);
    CHECK(keep[0] > 0.5F);
    CHECK(keep[1] > 0.5F);
    CHECK(keep[2] < -0.5F);
    CHECK(keep[3] < -0.5F);

    // cutoff 2: EVERY candidate is ignored, so the rays that hit above must now MISS.
    float ignore_all[4] = {};
    run(2.0, ignore_all);
    UNSCOPED_INFO("anyhit ignore: " << ignore_all[0] << " " << ignore_all[1] << " " << ignore_all[2] << " "
                                    << ignore_all[3]);
    CHECK(ignore_all[0] < -0.5F);
    CHECK(ignore_all[1] < -0.5F);
    CHECK(ignore_all[2] < -0.5F);
    CHECK(ignore_all[3] < -0.5F);
}

// ── ⭐⭐ REN-38-F11 GATE (DX12): the STENCIL ATTACHMENT — the Vulkan gate, joint for joint, on D3D12. ─────────
// The DX12-specific claims: the PSO's DSVFormat comes from the TARGET (D24_UNORM_S8_UINT — a D32 PSO on a
// stencil target is a device error), the depth clear carries D3D12_CLEAR_FLAG_STENCIL, and OMSetStencilRef
// installs the authored reference.
namespace
{
constexpr const char* kStencilGraphDx = R"(
schema = 1
name   = "crd://frame/f11-stencil-dx"

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

constexpr const char* kStencilCoverOnDx = R"(stencil         = true
stencil_compare = "Equal"
stencil_ref     = 1
)";

class StencilHostDx final : public crd::framecook::IFrameGraphHost
{
public:
    StencilHostDx(g::IRasterTarget* out, g::IRasterProgram* mask, g::IRasterProgram* cover, g::IStorageBuffer* sb)
        : m_out(out), m_mask(mask), m_cover(cover), m_sb(sb)
    {
    }
    [[nodiscard]] g::IRasterTarget*  output() override { return m_out; }
    [[nodiscard]] g::IRasterProgram* program(crd::containers::StringView) override { return nullptr; }
    [[nodiscard]] bool draw_list(crd::containers::StringView name, crd::framecook::DrawListBinding& out) override
    {
        const bool mask = name == crd::containers::StringView("mask_geo");
        out.items[0]    = crd::framecook::DrawItem{m_sb, mask ? m_mask : m_cover, mask ? 3U : 6U, nullptr};
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

TEST_CASE("REN-38-F11 GATE (DX12): an authored stencil MASK-then-TEST pass pair draws through a D24S8 target",
          "[dx12][raster][frame-graph][ren38][gpu]")
{
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto raster_p = g::create_dx12_raster_context();
    REQUIRE(raster_p != nullptr);
    auto& raster = *raster_p;

    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph mvg(&alloc);
    kir::KEntry mve;
    crd::gputest::build_triangle_vs(mvg, mve);
    kir::KGraph mfg(&alloc);
    kir::KEntry mfe;
    crd::gputest::build_triangle_fs(mfg, mfe);
    kir::KGraph cvg(&alloc);
    kir::KEntry cve;
    crd::gputest::build_visbuffer_vs(cvg, cve);
    kir::KGraph cfg2(&alloc);
    kir::KEntry cfe;
    crd::gputest::build_solid_fs(cfg2, cfe, 0.0, 1.0, 0.0);
    auto mvs = gctx->create_program(mvg, mve);
    auto mfs = gctx->create_program(mfg, mfe);
    auto cvs = gctx->create_program(cvg, cve);
    auto cfs = gctx->create_program(cfg2, cfe);
    if (mvs == nullptr || mfs == nullptr || cvs == nullptr || cfs == nullptr) { SKIP("dxc/DXIL unavailable"); }
    auto mask_prog  = raster.create_raster_program(*mvs, *mfs);
    auto cover_prog = raster.create_raster_program(*cvs, *cfs);
    REQUIRE(mask_prog != nullptr);
    REQUIRE(cover_prog != nullptr);

    const auto run = [&](bool stencil_on, crd::u32& centre, crd::u32& corner) {
        auto dst = raster.create_color_depth_stencil_target(64U, 64U);
        REQUIRE(dst != nullptr); // the F11 verb — a D24S8 target
        auto sb = raster.create_storage_buffer(16U);
        REQUIRE(sb != nullptr);
        crd::containers::String toml(&alloc);
        toml.append(kStencilGraphDx);
        if (stencil_on) { toml.append(kStencilCoverOnDx); }
        crd::framecook::FrameGraphDesc desc(&alloc);
        crd::containers::String        where(&alloc);
        REQUIRE(crd::framecook::parse_frame_toml(crd::containers::StringView(toml.c_str(), toml.size()), desc, &where)
                == crd::framecook::FrameCookError::Ok);
        StencilHostDx                  host(dst.get(), mask_prog.get(), cover_prog.get(), sb.get());
        crd::framecook::FrameExecError err = crd::framecook::FrameExecError::Ok;
        REQUIRE(crd::framecook::execute_frame_graph(desc, raster, host, &err, &where));
        CHECK(err == crd::framecook::FrameExecError::Ok);
        centre = dst->read_pixel(32U, 32U) & 0xFFFFFFU;
        corner = dst->read_pixel(2U, 2U) & 0xFFFFFFU;
    };

    crd::u32 c_on  = 0;
    crd::u32 k_on  = 0;
    crd::u32 c_off = 0;
    crd::u32 k_off = 0;
    run(true, c_on, k_on);
    run(false, c_off, k_off);
    UNSCOPED_INFO("stencil: centre=" << c_on << " corner=" << k_on
                                     << " | control: centre=" << c_off << " corner=" << k_off);
    const auto green = [](crd::u32 px) { return ((px >> 8U) & 0xFFU) > 200U && (px & 0xFFU) < 50U; };
    CHECK(green(c_on));
    CHECK((k_on & 0xFFFFFFU) == 0U);
    CHECK(green(c_off));
    CHECK(green(k_off));
}

// ── ⭐⭐ REN-38-F13 GATE: INTERSECTION + CALLABLE — the last two CKIR stages reach the device, AUTHORED. ──────
// The hit group goes PROCEDURAL: the BLAS holds only an AABB, and the authored `.crdv` intersection stage's
// sphere quadratic IS the geometry — a hit can only exist if that stage ran. The raygen routes its traced
// payload THROUGH the SBT's callable table (`use_callable`), whose authored transform (×2 + 1) is what lands in
// the output — so the numbers prove BOTH stages dispatched: hit payload 1.0 → 3.0 with the callable, 1.0
// without. Five stages, five declarations, zero C++ shader builders.
namespace
{
constexpr const char* kF13GraphDx = R"(
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
constexpr const char* kF13GraphDxNoCall = R"(
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

class F13HostDx final : public crd::framecook::IFrameGraphHost
{
public:
    F13HostDx(g::IRasterTarget* out) : m_out(out) {}
    [[nodiscard]] g::IRasterTarget*  output() override { return m_out; }
    [[nodiscard]] g::IRasterProgram* program(crd::containers::StringView) override { return nullptr; }
    [[nodiscard]] bool draw_list(crd::containers::StringView, crd::framecook::DrawListBinding&) override { return false; }
    [[nodiscard]] g::IGpuProgram* kernel(crd::containers::StringView id) override
    {
        for (crd::u32 i = 0; i < m_n; ++i)
        {
            if (id == crd::containers::StringView(m_names[i])) { return m_kernels[i]; }
        }
        return nullptr;
    }
    [[nodiscard]] g::IAccelerationStructure* acceleration_structure(crd::containers::StringView) override { return m_as; }
    [[nodiscard]] g::IStorageBuffer*         storage_buffer(crd::containers::StringView) override { return m_buf; }
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
    crd::u32                        m_n = 0U;
};
} // namespace

TEST_CASE("REN-38-F13 GATE (DX12): authored INTERSECTION + CALLABLE stages trace a procedural sphere through the SBT",
          "[dx12][raster][frame-graph][ren38][gpu]")
{
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto raster_p = g::create_dx12_raster_context();
    REQUIRE(raster_p != nullptr);
    auto& raster = *raster_p;
    if (!raster.supports_rt_pipeline()) { SKIP("adapter has no DXR ray-tracing pipeline"); }

    g::Dx12RayTracingContext rt;
    if (!rt.valid()) { SKIP("no DXR-capable device"); }
    crd::memory::TlsfAllocator alloc(32U << 20U);

    // ONE degenerate curve segment at the origin, radius 0.5 → one AABB [-0.5-eps, +0.5+eps]^3. The AABB only
    // BOUNDS the shape; the authored intersection stage's sphere is the geometry.
    const float seg[8] = {0.0F, 0.0F, 0.0F, 0.5F, 0.0F, 0.0F, 0.0F, 0.5F};
    auto        scene  = rt.build_scene_curves(seg, 1U);
    REQUIRE(scene != nullptr);

    // the five authored stages (+ the plain-raygen control), cooked from declarations
    const auto cook_stage = [&](const char* stage_line, const char* extra) -> std::unique_ptr<g::IGpuProgram> {
        crd::containers::String t(&alloc);
        t.append("schema = 1\nname = \"crd://vertex/f13\"\n");
        t.append(stage_line);
        t.append("\n[rt]\npayload_words = 2\nas_binding = 0\nout_binding = 1\n");
        t.append(extra);
        crd::vertcook::VertexProgramDesc d(&alloc);
        crd::containers::String               where(&alloc);
        REQUIRE(crd::vertcook::parse_vertex_toml(crd::containers::StringView(t.c_str(), t.size()), d, &where)
                == crd::vertcook::VertexCookError::Ok);
        kir::KGraph g2(&alloc);
        kir::KEntry e;
        REQUIRE(crd::vertcook::cook_vertex_program(d, g2, e));
        return gctx->create_program(g2, e);
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

    
    const auto           run = [&](const char* graph_toml, bool with_call, float out[4]) {
        constexpr crd::u32 ray_count = 4U;
        auto          hits      = raster.create_storage_buffer(ray_count * 4U);
        REQUIRE(hits != nullptr);
        const float sentinel[ray_count] = {-7.0F, -7.0F, -7.0F, -7.0F};
        REQUIRE(raster.upload_storage(*hits, 0U, static_cast<const void*>(sentinel), sizeof(sentinel)));
        auto dst = raster.create_color_target(16U, 16U);
        REQUIRE(dst != nullptr);
        crd::framecook::FrameGraphDesc desc(&alloc);
        crd::containers::String        where(&alloc);
        REQUIRE(crd::framecook::parse_frame_toml(crd::containers::StringView(graph_toml), desc, &where)
                == crd::framecook::FrameCookError::Ok);
        F13HostDx host(dst.get());
        host.add_kernel("crd://f13/raygen", rg.get());
        host.add_kernel("crd://f13/raygen_plain", rgp.get());
        host.add_kernel("crd://f13/miss", ms.get());
        host.add_kernel("crd://f13/chit", ch.get());
        host.add_kernel("crd://f13/isect", is.get());
        if (with_call) { host.add_kernel("crd://f13/call", cl.get()); }
        host.set_accel(scene.get());
        host.set_buffer(hits.get());
        crd::framecook::FrameExecError err = crd::framecook::FrameExecError::Ok;
        REQUIRE(crd::framecook::execute_frame_graph(desc, raster, host, &err, &where));
        CHECK(err == crd::framecook::FrameExecError::Ok);
        REQUIRE(raster.download_storage(*hits));
        for (crd::u32 i = 0; i < ray_count; ++i)
        {
            const crd::u32 bits = hits->read_u32(i);
            std::memcpy(&out[i], &bits, 4U);
        }
    };

    // every ray starts at the origin INSIDE the sphere and exits through it: the intersection stage reports the
    // far root, the closest-hit writes payload 1.0, and the callable transforms it to 1.0*2 + 1 = 3.0
    float with_call[4]{};
    float no_call[4]{};
    run(kF13GraphDx, true, with_call);
    run(kF13GraphDxNoCall, false, no_call);
    UNSCOPED_INFO("with callable: " << with_call[0] << " " << with_call[1] << " " << with_call[2] << " "
                                    << with_call[3]);
    UNSCOPED_INFO("without:      " << no_call[0] << " " << no_call[1] << " " << no_call[2] << " " << no_call[3]);
    for (crd::u32 i = 0; i < 4U; ++i)
    {
        CHECK(with_call[i] == 3.0F); // hit (intersection ran) AND transformed (callable ran)
        CHECK(no_call[i] == 1.0F);   // hit, untransformed — the dichotomy pins the callable specifically
    }
}

// ── ⭐⭐ REN-38-F6+ GATE (DX12): STORAGE-BOUND TESSELLATION — real control points, PULLED. ───────────────────
// The DX12 twin of the Vulkan gate: an AUTHORED pull `.crdv` fetches the patch's control points from the draw
// item's storage buffer through `draw_tess_storage`, and re-uploading smaller corners darkens the probe pixel —
// the BUFFER, not a procedural table, drives the tessellated patch.
namespace
{
constexpr const char* kTessPullGraphDx = R"(
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

constexpr const char* kTessPullVsDx = R"(
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

class TessPullHostDx final : public crd::framecook::IFrameGraphHost
{
public:
    TessPullHostDx(g::IRasterTarget* out, g::IRasterProgram* prog, g::IStorageBuffer* sb)
        : m_out(out), m_prog(prog), m_sb(sb)
    {
    }
    [[nodiscard]] g::IRasterTarget*  output() override { return m_out; }
    [[nodiscard]] g::IRasterProgram* program(crd::containers::StringView) override { return m_prog; }
    [[nodiscard]] bool draw_list(crd::containers::StringView, crd::framecook::DrawListBinding& out) override
    {
        out.items[0] = crd::framecook::DrawItem{m_sb, m_prog, 1U, nullptr}; // ONE quad patch, pulled
        out.resolved = 1U;
        return true;
    }

private:
    g::IRasterTarget*  m_out  = nullptr;
    g::IRasterProgram* m_prog = nullptr;
    g::IStorageBuffer* m_sb   = nullptr;
};

// the pull buffer: header words + indices + 4 corner vertices at (+-h, +-h, 0) + identity instance + slot list
void fill_tess_pull_buffer_dx(crd::containers::Array<crd::u32>& w, float h)
{
    w.clear();
    w.resize(140U);
    for (crd::usize i = 0; i < w.size(); ++i) { w[i] = 0U; }
    const auto fbits = [](float f) { crd::u32 u = 0; std::memcpy(&u, &f, 4U); return u; };
    w[0] = 4U;   // index COUNT (per instance)
    w[2] = 32U;  // indices at word 32
    w[3] = 36U;  // vertices at word 36
    w[4] = 100U; // instance record at word 100
    w[5] = 120U; // visible slot list at word 120
    for (crd::u32 c = 0; c < 4U; ++c) { w[6U + c * 4U + c] = fbits(1.0F); } // identity view_proj (column-major)
    for (crd::u32 i = 0; i < 4U; ++i) { w[32U + i] = i; }                    // identity index list
    const float cx[4] = {-h, h, h, -h};
    const float cy[4] = {-h, -h, h, h};
    for (crd::u32 i = 0; i < 4U; ++i)
    {
        w[36U + i * 3U + 0U] = fbits(cx[i]);
        w[36U + i * 3U + 1U] = fbits(cy[i]);
        w[36U + i * 3U + 2U] = fbits(0.0F);
    }
    for (crd::u32 c = 0; c < 4U; ++c) { w[100U + c * 4U + c] = fbits(1.0F); } // identity instance matrix
    w[120] = 0U;                                                              // visible slot 0
}
} // namespace

TEST_CASE("REN-38-F6+ GATE (DX12): storage-bound tessellation PULLS its control points from the draw item's buffer",
          "[dx12][raster][frame-graph][ren38][gpu]")
{
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto rasterp = g::create_dx12_raster_context();
    REQUIRE(rasterp != nullptr);
    auto& raster = *rasterp;

    crd::memory::TlsfAllocator alloc(16U << 20U);
    const auto                 cook_vc = [&](const char* toml) -> std::unique_ptr<g::IGpuProgram> {
        crd::vertcook::VertexProgramDesc d(&alloc);
        crd::containers::String          where(&alloc);
        REQUIRE(crd::vertcook::parse_vertex_toml(crd::containers::StringView(toml), d, &where)
                == crd::vertcook::VertexCookError::Ok);
        crd::kir::KGraph g2(&alloc);
        crd::kir::KEntry e;
        REQUIRE(crd::vertcook::cook_vertex_program(d, g2, e));
        return gctx->create_program(g2, e);
    };
    auto vs = cook_vc(kTessPullVsDx);
    auto hs = cook_vc("stage = \"tess_control\"\nschema = 1\nname = \"h\"\n\n[tess]\npatch_size = 4\ninner = "
                      "8.0\nouter = 8.0\n");
    auto ds = cook_vc("stage = \"tess_eval\"\nschema = 1\nname = \"d\"\ndisplace = \"expand\"\n\n[vertex]\nstride "
                      "= 3\n\n[[attribute]]\nname = \"position\"\noffset = 0\ncomps = 3\nkind = "
                      "\"position\"\n\n[tess]\npatch_size = 4\ninner = 8.0\nouter = 8.0\n\n[[node]]\nname = "
                      "\"expand\"\nop = \"multiply\"\ninputs = [\"@position\", [1.3, 1.3, 1.0]]\n");
    crd::kir::KGraph fg2(&alloc);
    crd::kir::KEntry fe;
    crd::gputest::build_solid_fs(fg2, fe, 1.0, 0.3, 0.1);
    auto fs = gctx->create_program(fg2, fe);
    if (vs == nullptr || hs == nullptr || ds == nullptr || fs == nullptr) { SKIP("shader compile unavailable"); }
    auto prog = raster.create_tess_program(*vs, *hs, *ds, *fs);
    REQUIRE(prog != nullptr);

    const auto run = [&](float h) {
        auto dst = raster.create_color_target(128U, 128U);
        REQUIRE(dst != nullptr);
        crd::containers::Array<crd::u32> words(&alloc);
        fill_tess_pull_buffer_dx(words, h);
        auto sb = raster.create_storage_buffer(static_cast<crd::u32>(words.size() * 4U));
        REQUIRE(sb != nullptr);
        REQUIRE(raster.upload_storage(*sb, 0U, words.data(), static_cast<crd::u32>(words.size() * 4U)));
        crd::framecook::FrameGraphDesc desc(&alloc);
        crd::containers::String        where(&alloc);
        REQUIRE(crd::framecook::parse_frame_toml(crd::containers::StringView(kTessPullGraphDx), desc, &where)
                == crd::framecook::FrameCookError::Ok);
        TessPullHostDx                 host(dst.get(), prog.get(), sb.get());
        crd::framecook::FrameExecError err = crd::framecook::FrameExecError::Ok;
        REQUIRE(crd::framecook::execute_frame_graph(desc, raster, host, &err, &where));
        CHECK(err == crd::framecook::FrameExecError::Ok);
        return dst->read_pixel(108U, 64U) & 0xFFFFFFU; // NDC ~0.69: inside h=0.6 x1.3, far outside h=0.3 x1.3
    };

    const crd::u32 big   = run(0.6F);
    const crd::u32 small = run(0.3F);
    UNSCOPED_INFO("big=" << big << " small=" << small);
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
        crd::containers::Array<crd::u32> words(&alloc);
        fill_tess_pull_buffer_dx(words, 0.6F);
        std::memcpy(&words[22], &scale, 4U); // the domain's scale word
        auto sb = raster.create_storage_buffer(static_cast<crd::u32>(words.size() * 4U));
        REQUIRE(sb != nullptr);
        REQUIRE(raster.upload_storage(*sb, 0U, words.data(), static_cast<crd::u32>(words.size() * 4U)));
        crd::framecook::FrameGraphDesc desc(&alloc);
        crd::containers::String        where(&alloc);
        REQUIRE(crd::framecook::parse_frame_toml(crd::containers::StringView(kTessPullGraphDx), desc, &where)
                == crd::framecook::FrameCookError::Ok);
        TessPullHostDx              host(dst.get(), prog2.get(), sb.get());
        crd::framecook::FrameExecError err = crd::framecook::FrameExecError::Ok;
        REQUIRE(crd::framecook::execute_frame_graph(desc, raster, host, &err, &where));
        CHECK(err == crd::framecook::FrameExecError::Ok);
        return dst->read_pixel(108U, 64U) & 0xFFFFFFU;
    };
    const crd::u32 hdr_big  = run_hdr(1.3F);
    const crd::u32 hdr_damp = run_hdr(0.55F);
    UNSCOPED_INFO("hdr_big=" << hdr_big << " hdr_damp=" << hdr_damp);
    CHECK(hdr_big != 0U);  // 0.6 x 1.3 = 0.78 covers the probe...
    CHECK(hdr_damp == 0U); // ...0.6 x 0.55 = 0.33 does not — the WORD drove the domain
}

// ── ⭐⭐ REN-38-F6+ GATE: MESHLET FETCH — the mesh stage PULLS real geometry, and the TASK count is GPU-driven.
// Two dichotomies over ONE authored buffer layout (the same scene pull contract the VS uses):
//   1. a `fetch = true` mesh stage renders quads whose EXTENT tracks the buffer's vertex records — re-upload
//      smaller corners and the probe pixel goes dark;
//   2. a task stage with `emit_header` dispatches as many mesh workgroups as the BUFFER says — word 130 = 1
//      renders only instance 0's quad, word 130 = 2 lights instance 1's quad too.
namespace
{
constexpr const char* kMeshFetchGraphDx = R"(
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

constexpr const char* kMeshFetchVsDx = R"(
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

constexpr const char* kTaskHdrTomlDx = R"(
schema = 1
name   = "crd://vertex/taskhdr"
stage  = "task"

[mesh]
workgroup = 1

[task]
emit        = 1
emit_header = 130
)";

class MeshFetchHostDx final : public crd::framecook::IFrameGraphHost
{
public:
    MeshFetchHostDx(g::IRasterTarget* out, g::IRasterProgram* prog, g::IStorageBuffer* sb, crd::u32 groups)
        : m_out(out), m_prog(prog), m_sb(sb), m_groups(groups)
    {
    }
    [[nodiscard]] g::IRasterTarget*  output() override { return m_out; }
    [[nodiscard]] g::IRasterProgram* program(crd::containers::StringView) override { return m_prog; }
    [[nodiscard]] bool draw_list(crd::containers::StringView, crd::framecook::DrawListBinding& out) override
    {
        out.items[0] = crd::framecook::DrawItem{m_sb, m_prog, m_groups, nullptr};
        out.resolved = 1U;
        return true;
    }

private:
    g::IRasterTarget*  m_out    = nullptr;
    g::IRasterProgram* m_prog   = nullptr;
    g::IStorageBuffer* m_sb     = nullptr;
    crd::u32                m_groups = 1U;
};

// the pull buffer: 6 indices (two triangles = one quad), 4 corner records (+-h), TWO instances translated to
// x = -0.5 / x = +0.5, identity view_proj, and the task's amplification count at word 130
void fill_mesh_fetch_buffer_dx(crd::containers::Array<crd::u32>& w, float h, crd::u32 task_count)
{
    w.clear();
    w.resize(140U);
    for (crd::usize i = 0; i < w.size(); ++i) { w[i] = 0U; }
    const auto fbits = [](float f) { crd::u32 u = 0; std::memcpy(&u, &f, 4U); return u; };
    w[0] = 6U;   // index COUNT (per instance)
    w[2] = 32U;  // indices at word 32
    w[3] = 40U;  // vertices at word 40
    w[4] = 60U;  // instance records at word 60 (stride 20)
    w[5] = 110U; // visible slot list at word 110
    for (crd::u32 c = 0; c < 4U; ++c) { w[6U + c * 4U + c] = fbits(1.0F); } // identity view_proj (column-major)
    const crd::u32 idx[6] = {0U, 1U, 2U, 0U, 2U, 3U};
    for (crd::u32 i = 0; i < 6U; ++i) { w[32U + i] = idx[i]; }
    const float cx[4] = {-h, h, h, -h};
    const float cy[4] = {-h, -h, h, h};
    for (crd::u32 i = 0; i < 4U; ++i)
    {
        w[40U + i * 3U + 0U] = fbits(cx[i]);
        w[40U + i * 3U + 1U] = fbits(cy[i]);
        w[40U + i * 3U + 2U] = fbits(0.0F);
    }
    for (crd::u32 inst = 0; inst < 2U; ++inst) // identity + a translation: instance 0 left, instance 1 right
    {
        const crd::u32 base = 60U + inst * 20U;
        for (crd::u32 c = 0; c < 4U; ++c) { w[base + c * 4U + c] = fbits(1.0F); }
        w[base + 12U] = fbits(inst == 0U ? -0.5F : 0.5F); // column-major translation x
    }
    w[110] = 0U;
    w[111] = 1U;
    w[130] = task_count; // the GPU-driven amplification count
}
} // namespace

TEST_CASE("REN-38-F6+ GATE (DX12): a FETCH mesh stage renders the buffer's geometry and emit_header drives the dispatch",
          "[dx12][raster][frame-graph][ren38][gpu]")
{
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto rasterp = g::create_dx12_raster_context();
    REQUIRE(rasterp != nullptr);
    auto& raster = *rasterp;

    crd::memory::TlsfAllocator alloc(16U << 20U);
    const auto            cook_vc = [&](const char* toml) -> std::unique_ptr<g::IGpuProgram> {
        crd::vertcook::VertexProgramDesc d(&alloc);
        crd::containers::String               where(&alloc);
        REQUIRE(crd::vertcook::parse_vertex_toml(crd::containers::StringView(toml), d, &where)
                == crd::vertcook::VertexCookError::Ok);
        crd::kir::KGraph g2(&alloc);
        crd::kir::KEntry e;
        REQUIRE(crd::vertcook::cook_vertex_program(d, g2, e));
        return gctx->create_program(g2, e);
    };
    auto ms = cook_vc(kMeshFetchVsDx);
    // the task-paired variant: `payload = true` declares the AS->MS payload contract the D3D12 PSO validator
    // enforces — an MS with no payload input cannot pair with an AS whose DispatchMesh always passes one
    crd::containers::String fetch_pl(&alloc);
    fetch_pl.append(kMeshFetchVsDx);
    fetch_pl.append("payload        = true\n");
    auto msp = cook_vc(fetch_pl.c_str());
    auto tk  = cook_vc(kTaskHdrTomlDx);
    crd::kir::KGraph fg2(&alloc);
    crd::kir::KEntry fe;
    crd::gputest::build_solid_fs(fg2, fe, 1.0, 0.3, 0.1);
    auto fs = gctx->create_program(fg2, fe);
    if (ms == nullptr || msp == nullptr || tk == nullptr || fs == nullptr) { SKIP("mesh shader compile unavailable"); }
    auto mesh_prog = raster.create_mesh_program(*ms, *fs);
    auto task_prog = raster.create_task_mesh_program(*tk, *msp, *fs);
    if (mesh_prog == nullptr || task_prog == nullptr)
    {
        WARN("mesh_prog null=" << (mesh_prog == nullptr) << " task_prog null=" << (task_prog == nullptr));
        SKIP("no mesh-shader support on this device");
    }

    // probe pixels sit 0.2 NDC RIGHT of each quad centre (-0.5 / +0.5): inside an h = 0.35 quad, outside an
    // h = 0.05 one -- a centre probe would stay covered no matter how small the pulled corners became
    const auto run = [&](g::IRasterProgram* prog, crd::u32 groups, float h, crd::u32 task_count, crd::u32* left, crd::u32* right) {
        auto dst = raster.create_color_target(128U, 128U);
        REQUIRE(dst != nullptr);
        crd::containers::Array<crd::u32> words(&alloc);
        fill_mesh_fetch_buffer_dx(words, h, task_count);
        auto sb = raster.create_storage_buffer(static_cast<crd::u32>(words.size() * 4U));
        REQUIRE(sb != nullptr);
        REQUIRE(raster.upload_storage(*sb, 0U, words.data(), static_cast<crd::u32>(words.size() * 4U)));
        crd::framecook::FrameGraphDesc desc(&alloc);
        crd::containers::String        where(&alloc);
        REQUIRE(crd::framecook::parse_frame_toml(crd::containers::StringView(kMeshFetchGraphDx), desc, &where)
                == crd::framecook::FrameCookError::Ok);
        MeshFetchHostDx             host(dst.get(), prog, sb.get(), groups);
        crd::framecook::FrameExecError err = crd::framecook::FrameExecError::Ok;
        REQUIRE(crd::framecook::execute_frame_graph(desc, raster, host, &err, &where));
        CHECK(err == crd::framecook::FrameExecError::Ok);
        *left  = dst->read_pixel(44U, 64U) & 0xFFFFFFU;  // NDC -0.3
        *right = dst->read_pixel(108U, 64U) & 0xFFFFFFU; // NDC +0.7
    };

    // ── claim 1: the FETCH — quad extent tracks the buffer's vertex records ──
    crd::u32 big_l = 0U;
    crd::u32 big_r = 0U;
    run(mesh_prog.get(), 2U, 0.35F, 2U, &big_l, &big_r);
    crd::u32 small_l = 0U;
    crd::u32 small_r = 0U;
    run(mesh_prog.get(), 2U, 0.05F, 2U, &small_l, &small_r);
    UNSCOPED_INFO("fetch big=(" << big_l << "," << big_r << ") small=(" << small_l << "," << small_r << ")");
    CHECK(big_l != 0U);   // both instances' quads cover their probe pixels...
    CHECK(big_r != 0U);
    CHECK(small_l == 0U); // ...and shrink to nothing when the BUFFER's corners shrink — the geometry was PULLED
    CHECK(small_r == 0U);

    // ── claim 2: the GPU-driven COUNT — word 130 decides how many meshlets exist ──
    crd::u32 one_l = 0U;
    crd::u32 one_r = 0U;
    run(task_prog.get(), 1U, 0.35F, 1U, &one_l, &one_r);
    crd::u32 two_l = 0U;
    crd::u32 two_r = 0U;
    run(task_prog.get(), 1U, 0.35F, 2U, &two_l, &two_r);
    UNSCOPED_INFO("task one=(" << one_l << "," << one_r << ") two=(" << two_l << "," << two_r << ")");
    CHECK(one_l != 0U); // count 1: instance 0 only...
    CHECK(one_r == 0U);
    CHECK(two_l != 0U); // ...count 2: the buffer word ALONE lit the second quad
    CHECK(two_r != 0U);
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
// ⭐⭐ REN-40-A GATE (DX12): the GEOMETRY indirect draw — the forward pass's shape, args AND count from device
// memory. ⛔ The DX12 half matters most here: its command is [u32 draw_index][5×u32 args] at a 24-byte stride
// while Vulkan's is [5×u32] at 20, so a gate that only ran on Vulkan would prove nothing about the layout the
// GPU producer must write for THIS backend (`indirect_command_stride()` / `_arg_offset()` are what it reads).
TEST_CASE("REN-40-A GATE (DX12): the geometry indirect draw takes its args AND its count from device memory",
          "[dx12][raster][frame-graph][ren40][indirect][gpu]")
{
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto rasterp = g::create_dx12_raster_context();
    REQUIRE(rasterp != nullptr);
    auto& raster = *rasterp;
    REQUIRE(raster.indirect_count_supported());

    crd::memory::TlsfAllocator alloc(8U << 20U);
    crd::kir::KGraph           vg(&alloc);
    crd::kir::KEntry           ve;
    crd::gputest::build_vertex_pull_vs(vg, ve);
    auto             vs = gctx->create_program(vg, ve);
    crd::kir::KGraph cgraph(&alloc);
    crd::kir::KEntry cfe;
    crd::gputest::build_triangle_fs(cgraph, cfe);
    auto cfs = gctx->create_program(cgraph, cfe);
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
        rec[r * 12U + 0U] = 2.0F;
        rec[r * 12U + 1U] = 2.0F;
        for (crd::u32 w = 2U; w < 12U; ++w) { rec[r * 12U + w] = 0.0F; }
    }
    const auto put = [&](crd::u32 r, float x, float y, float z) {
        rec[r * 12U + 0U] = x;
        rec[r * 12U + 1U] = y;
        rec[r * 12U + 2U] = z;
    };
    put(4U, 0.0F, -0.8F, 0.5F);
    put(5U, 0.8F, 0.8F, 0.5F);
    put(6U, -0.8F, 0.8F, 0.5F);
    put(8U, -0.95F, -0.35F, 0.5F); // symmetric about y = 0 — the midline probe hits it under BOTH clip-Y rules
    put(9U, -0.55F, 0.0F, 0.5F);
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

    CHECK(c1 >= 250U);
    CHECK(l1 <= 20U);
    CHECK(c2 >= 250U);
    CHECK(l2 >= 250U);
    CHECK(l2 > l1); // the arm that catches a verb ignoring the count
    CHECK(r1 <= 20U);
    CHECK(r2 <= 20U);
}

TEST_CASE("REN-40-A GATE (DX12): indirect draw takes its args AND its count from device memory",
          "[dx12][raster][frame-graph][ren40][indirect][gpu]")
{
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { SKIP("no D3D12 device available"); }
    auto rasterp = g::create_dx12_raster_context();
    REQUIRE(rasterp != nullptr);
    auto& raster = *rasterp;
    // D3D12 has taken a pCountBuffer on ExecuteIndirect since 12.0 - no feature bit, unlike Vulkan's
    // drawIndirectCount. The DECLARED capability is what both gates assert, not the mechanism.
    REQUIRE(raster.indirect_count_supported());

    crd::memory::TlsfAllocator alloc(8U << 20U);
    crd::kir::KGraph vg(&alloc);
    crd::kir::KEntry ve;
    crd::gputest::build_vertex_pull_vs(vg, ve); // position = record at VertexIndex * 12 words
    auto vs = gctx->create_program(vg, ve);
    crd::kir::KGraph cgraph(&alloc);
    crd::kir::KEntry cfe;
    crd::gputest::build_triangle_fs(cgraph, cfe); // writes colour
    auto cfs = gctx->create_program(cgraph, cfe);
    crd::kir::KGraph dgraph(&alloc);
    crd::kir::KEntry dfe;
    crd::gputest::build_depth_only_const_fs(dgraph, dfe, 0.5); // n_out = 0 - legal with zero colour attachments
    auto dfs = gctx->create_program(dgraph, dfe);
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
}
