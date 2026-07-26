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
#include <crd/gpu/frame_graph.hpp>
#include <crd/gpu/raster_context.hpp>

#include <crd/framecook/frame_asset.hpp>   // REN-36.2: the cooked frame-graph asset
#include <crd/framecook/frame_runtime.hpp> // REN-36.2: executing it through IFrameGraph
#include <crd/kir/ckir.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <ckir_raster_triangle.hpp> // REN-2: the shared triangle (offscreen) + textured/sample (compose) CKIR builders

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
    ctx.raster().draw_storage_depth(*ctx.image(s->img), *s->prog, g::ClearColor{0.0F, 0.0F, 1.0F, 1.0F}, 0.0F,
                                    g::DepthCompare::GreaterEqual, *ctx.buffer(s->buf), 3U);
}
void record_load(g::IFrameContext& ctx, void* user)
{
    auto* s = static_cast<ScenePass*>(user);
    ctx.raster().draw_storage_depth_load(*ctx.image(s->img), *s->prog, g::DepthCompare::GreaterEqual,
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
    ctx.raster().draw_storage_depth_only(*ctx.image(s->map), *s->prog, 1.0F, g::DepthCompare::LessEqual,
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
    ctx.raster().draw_storage_depth_only(*ctx.image_layer(s->atlas, s->layer), *s->prog, 1.0F,
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
    ctx.raster().draw_shadow(*ctx.image(s->dst), *s->prog, g::ClearColor{0.0F, 0.0F, 1.0F, 1.0F},
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
    raster->draw_storage_depth(*ref, *prog, g::ClearColor{0.0F, 0.0F, 1.0F, 1.0F}, 0.0F, g::DepthCompare::GreaterEqual, *red, 3U);
    raster->draw_storage_depth_load(*ref, *prog, g::DepthCompare::GreaterEqual, *grn, 3U);
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

    raster->draw_storage_textured_depth(*tgt, *prog, g::ClearColor{0.0F, 0.0F, 1.0F, 1.0F}, 0.0F, g::DepthCompare::Always,
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
        r.draw_storage_depth(t, *s->prog, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 0.0F, g::DepthCompare::Always, b, 3U);
        for (crd::u32 i = 1; i < s->n; ++i) { r.draw_storage_depth_load(t, *s->prog, g::DepthCompare::Always, b, 3U); }
    };

    constexpr crd::u32 frames = 20U;
    double             graph_ms_at_64 = 0.0;
    double             sync_ms_at_64  = 0.0;
    for (const crd::u32 n : {1U, 4U, 16U, 64U})
    {
        const auto s0 = std::chrono::steady_clock::now();
        for (crd::u32 f = 0; f < frames; ++f)
        {
            raster->draw_storage_depth(*tgt, *prog, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 0.0F, g::DepthCompare::Always, *sb, 3U);
            for (crd::u32 i = 1; i < n; ++i) { raster->draw_storage_depth_load(*tgt, *prog, g::DepthCompare::Always, *sb, 3U); }
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
    constexpr crd::u32 kKey  = 0xA11CE5U; // the same stable identity the Vulkan gate uses
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
        const g::FgImage hist = fgraph->create_persistent_image(kKey, pdesc);
        REQUIRE(hist.valid());
        CHECK_FALSE(fgraph->persistent_image_was_live(kKey));
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
        const g::FgImage hist = fgraph->create_persistent_image(kKey, pdesc);
        REQUIRE(hist.valid());
        CHECK(fgraph->persistent_image_was_live(kKey));
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
    REQUIRE(fgraph->create_persistent_image(kKey, bigger).valid());
    CHECK_FALSE(fgraph->persistent_image_was_live(kKey));
}
