// test_dx12_raster.cpp — the D3D12 IRasterContext (ADR-0103 / D-008 C4-a) on the GPU. The DX12 mirror of the Vulkan
// raster clear test: the SAME backend-agnostic crd::gpu::IRasterContext surface that clears offscreen targets on Vulkan
// clears an RGBA8 render target on DirectX 12, and read_pixel returns the exact cleared colour. This proves the DX12 half
// of the render seam is reachable (create_color_target -> clear -> read_pixel), so it stops being an "unreachable" gate.
// The shader DRAW is C4-b. A non-square target (100x64) forces a padded readback row pitch (400 -> 512) so the 256-byte
// D3D12 alignment is genuinely exercised.

#include <crd/containers/string_view.hpp>
#include <crd/gpu/dx12_compute_context.hpp> // B4: the compute cull that writes the indirect-dispatch args
#include <crd/gpu/dx12_context.hpp>         // C4-b: create_dx12_gpu_context, compile_hlsl_to_dxil, Dx12GpuProgram
#include <crd/gpu/dx12_raster_context.hpp>
#include <crd/gpu/frame_graph.hpp> // REN-39-A1: the frame-mode indexed-draw arm records through a graph
#include <crd/gpu/raster_context.hpp>
#include <crd/kir/ckir.hpp>           // C4-b: KGraph/KEntry/KStage — the create_program(KGraph) seam
#include <crd/kir/ckir_hlsl.hpp>      // B4: emit_compute_kernel_hlsl (the cull kernel)
#include <crd/kir/ckir_visbuffer.hpp> // B4: build_meshlet_cull
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <ckir_oit_test.hpp>        // B17: the SHARED order-independent-transparency shaders + CPU oracle (WBOIT/...)
#include <ckir_raster_triangle.hpp> // B3-e: the SHARED, backend-neutral CKIR triangle (identical on Vulkan + DX12)
#include <verb_packet_helpers.hpp>  // RAF-12.4: crd::gputest::enc_draw* (fullscreen verbs recorded via the encoder)
#include <win32_test_window.hpp>    // RET-2: the isolated real-window helper for the present gate

namespace g = crd::gpu;

// B1-e: count horizontal even-x neighbour pairs whose R channel is EQUAL — a coarse VRS rate makes each 2x2 block share
// one fragment invocation ⇒ those become equal; at 1x1 the ramp FS leaves them distinct.
namespace
{
inline int count_equal_even_pairs(g::IRasterTarget& t, crd::u32 dim)
{
    int n = 0;
    for (crd::u32 y = 0; y < dim; ++y)
    {
        for (crd::u32 i = 0; i + 1U < dim; i += 2U)
        {
            const int rl = static_cast<int>(t.read_pixel(i, y) & 0xFFU);
            const int rr = static_cast<int>(t.read_pixel(i + 1U, y) & 0xFFU);
            if (rl == rr) { ++n; }
        }
    }
    return n;
}
} // namespace

// ═══ RET-2 (D-007 row 90, ADR-0105): DX12 PRESENTS — the DXGI mirror of the Vulkan sink design ═════════════════════════
TEST_CASE("RET-2: DX12 gpu-context PRESENTS -- acquire/copy/present/resize through a DXGI swapchain",
          "[dx12][raster][gpu][ret]")
{
    auto ctx = g::create_dx12_raster_context();
    if (ctx == nullptr || !ctx->valid()) { WARN("no D3D12 device available; skipping"); return; }

    void* hwnd = crd::gputest::create_test_window(256U, 256U);
    if (hwnd == nullptr) { WARN("no platform window available; skipping"); return; }

    auto surface = ctx->create_present_surface(hwnd, 256U, 256U, g::PresentMode::Fifo);
    REQUIRE(surface != nullptr);
    CHECK(surface->valid());
    CHECK(surface->width() == 256U);
    CHECK(surface->height() == 256U);

    // the canvas: the normal offscreen clear path (post-draw state COMMON — exactly what present() consumes)
    auto target = ctx->create_color_target(256U, 256U);
    REQUIRE(target != nullptr);
    for (int frame = 0; frame < 3; ++frame)
    {
        ctx->clear(*target, g::ClearColor{1.0F, 0.0F, 0.0F, 1.0F});
        CHECK(surface->present(*target));
        crd::gputest::pump_test_window();
    }
    CHECK(surface->frame_count() == 3U);

    // a mismatched canvas is REFUSED (never a stretched half-frame)
    auto small = ctx->create_color_target(128U, 128U);
    REQUIRE(small != nullptr);
    ctx->clear(*small, g::ClearColor{0.0F, 1.0F, 0.0F, 1.0F});
    CHECK(!surface->present(*small));

    // resize exercises ResizeBuffers; the matching canvas presents again
    REQUIRE(surface->resize(256U, 256U));
    ctx->clear(*target, g::ClearColor{0.0F, 0.0F, 1.0F, 1.0F});
    CHECK(surface->present(*target));
    CHECK(surface->frame_count() == 4U);

    WARN("[ret2-present dx12] 4 frames presented through a DXGI swapchain (win32 window, 256x256)");
    surface.reset(); // the surface dies BEFORE its window
    crd::gputest::destroy_test_window(hwnd);
}

TEST_CASE("D-007 C4-a: D3D12 IRasterContext clears an offscreen target and reads it back", "[dx12][raster][gpu]")
{
    auto ctx = g::create_dx12_raster_context();
    if (ctx == nullptr || !ctx->valid()) { WARN("no D3D12 device available; skipping"); return; }

    constexpr crd::u32 img_w = 100U; // *4 = 400 bytes/row -> padded to 512 (256-byte aligned) in the readback
    constexpr crd::u32 img_h = 64U;
    auto               target = ctx->create_color_target(img_w, img_h);
    REQUIRE(target != nullptr);
    REQUIRE(target->width() == img_w);
    REQUIRE(target->height() == img_h);

    // Pure-channel clears (0/1 map exactly to 0x00/0xFF, no unorm rounding ambiguity): each isolates one channel so a
    // swap (e.g. RGBA vs BGRA) would be caught. read_pixel is packed little-endian RGBA8 = 0xAABBGGRR (R in the low byte).
    struct Case
    {
        g::ClearColor color;
        crd::u32      expect;
    };
    const Case cases[] = {
        {{1.0F, 0.0F, 0.0F, 1.0F}, 0xFF0000FFU}, // red
        {{0.0F, 1.0F, 0.0F, 1.0F}, 0xFF00FF00U}, // green
        {{0.0F, 0.0F, 1.0F, 1.0F}, 0xFFFF0000U}, // blue
        {{1.0F, 1.0F, 1.0F, 1.0F}, 0xFFFFFFFFU}, // white
    };

    for (const auto& c : cases)
    {
        ctx->clear(*target, c.color);
        CHECK(target->read_pixel(0U, 0U) == c.expect);                    // first texel
        CHECK(target->read_pixel(img_w - 1U, img_h - 1U) == c.expect);    // last texel (row-pitch edge)
        CHECK(target->read_pixel(img_w - 1U, 0U) == c.expect);           // last column of row 0 (within-row edge)
    }

    // Out-of-range reads are defined to return 0.
    CHECK(target->read_pixel(img_w, 0U) == 0U);
    CHECK(target->read_pixel(0U, img_h) == 0U);
}

TEST_CASE("D-007 C4-b: DX12 IGpuContext create_program(KGraph) mints raster DXIL programs", "[dx12][raster][program][gpu]")
{
    namespace kir = crd::kir;

    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    crd::memory::TlsfAllocator alloc(4U << 20U);

    // Probe dxc/DXIL (dxcompiler.dll missing on some CI hosts). Soft-skip like the Vulkan B3-d gate, so a genuine seam
    // regression on a dxc-capable host still fails loudly below.
    const auto probe = g::compile_hlsl_to_dxil(
        g::ShaderStage::Vertex, crd::containers::StringView("float4 main() : SV_Position { return float4(0,0,0,1); }"),
        "c4b_probe", &alloc);
    if (!probe.ok) { WARN("dxc/DXIL unavailable; skipping C4-b create_program seam"); return; }

    // VERTEX: attribute(vec4, loc 0) -> clip position; attribute(vec4, loc 1) -> interpolant(loc 0).
    kir::KGraph vg(&alloc);
    const int   a_pos = vg.stage_in(kir::KType::vec(kir::DType::F32, 4), 0);
    const int   a_col = vg.stage_in(kir::KType::vec(kir::DType::F32, 4), 1);
    kir::KEntry ve;
    ve.stage    = kir::KStage::Vertex;
    ve.position = a_pos;
    ve.n_out    = 1;
    ve.out[0]   = {a_col, 0};
    auto vprog  = gctx->create_program(vg, ve);
    REQUIRE(vprog != nullptr); // KIR vertex entry -> HLSL (emit_stage_hlsl) -> DXIL -> Dx12GpuProgram, all behind the seam
    CHECK(vprog->stage() == g::ShaderStage::Vertex);
    CHECK(static_cast<g::Dx12GpuProgram*>(vprog.get())->dxil().size() >= 4U);

    // FRAGMENT: interpolant(vec4, loc 0) -> colour attachment(loc 0).
    kir::KGraph fg(&alloc);
    const int   f_in = fg.stage_in(kir::KType::vec(kir::DType::F32, 4), 0);
    kir::KEntry fe;
    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {f_in, 0};
    auto fprog = gctx->create_program(fg, fe);
    REQUIRE(fprog != nullptr);
    CHECK(fprog->stage() == g::ShaderStage::Fragment);

    // The gate BITES: a vertex entry that names no clip position is refused loudly (never guessed).
    kir::KGraph bg(&alloc);
    const int   b_pos = bg.stage_in(kir::KType::vec(kir::DType::F32, 4), 0);
    kir::KEntry be;
    be.stage  = kir::KStage::Vertex; // position left at -1
    be.n_out  = 1;
    be.out[0] = {b_pos, 0};
    CHECK(gctx->create_program(bg, be) == nullptr);
}

TEST_CASE("D-007 C4-b: DX12 graphics-PSO DRAW -- a red triangle over a blue clear", "[dx12][raster][gpu]")
{
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    crd::memory::TlsfAllocator alloc(4U << 20U);

    // Attributeless triangle: the VS emits 3 clip-space positions from SV_VertexID (a big triangle over the centre but
    // not the corners); the PS paints it red. Hand-written HLSL -> DXIL through the DX12 backend's own dxc (mirror C1-b).
    static constexpr const char* kVs = "float4 main(uint vid : SV_VertexID) : SV_Position {\n"
                                       "  float2 p[3] = { float2(0.0,-0.8), float2(0.8,0.8), float2(-0.8,0.8) };\n"
                                       "  return float4(p[vid], 0.0, 1.0);\n"
                                       "}\n";
    static constexpr const char* kFs = "float4 main() : SV_Target { return float4(1.0,0.0,0.0,1.0); }\n";

    const auto vs_dxil =
        g::compile_hlsl_to_dxil(g::ShaderStage::Vertex, crd::containers::StringView(kVs), "tri_vs", &alloc);
    if (!vs_dxil.ok) { WARN("dxc/DXIL unavailable; skipping C4-b draw"); return; }
    const auto fs_dxil =
        g::compile_hlsl_to_dxil(g::ShaderStage::Fragment, crd::containers::StringView(kFs), "tri_fs", &alloc);
    REQUIRE(fs_dxil.ok);

    auto vs = gctx->create_program(g::ShaderStage::Vertex,
                                   crd::containers::ConstSpan<crd::u8>(vs_dxil.dxil.data(), vs_dxil.dxil.size()));
    auto fs = gctx->create_program(g::ShaderStage::Fragment,
                                   crd::containers::ConstSpan<crd::u8>(fs_dxil.dxil.data(), fs_dxil.dxil.size()));
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr);

    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    REQUIRE(program->valid());

    constexpr crd::u32 dim    = 32U;
    auto               target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);

    crd::gputest::enc_draw(*raster,*target, *program, g::ClearColor{0.0F, 0.0F, 1.0F, 1.0F}, 3U); // blue clear, draw the triangle

    // Centre is inside the triangle -> RED; a corner is outside -> BLUE clear. The whole DX12 seam end-to-end: trivial
    // HLSL -> dxc -> DXIL -> Dx12GpuProgram -> graphics PSO -> DrawInstanced -> readback.
    const crd::u32 centre = target->read_pixel(dim / 2U, dim / 2U);
    const crd::u32 corner = target->read_pixel(0U, 0U);
    CHECK((centre & 0xFFU) >= 250U);          // R high
    CHECK(((centre >> 16U) & 0xFFU) <= 5U);   // B low  => red
    CHECK((corner & 0xFFU) <= 5U);            // R low
    CHECK(((corner >> 16U) & 0xFFU) >= 250U); // B high => blue clear
}

TEST_CASE("D-007 B3-e: IR-authored triangle draws on DX12 (CKIR graph -> DXIL -> PSO -> pixels)", "[dx12][raster][gpu][ir]")
{
    namespace kir = crd::kir;

    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    crd::memory::TlsfAllocator alloc(4U << 20U);

    // The SAME shared CKIR triangle the Vulkan B3-e test draws — one IR, both backends.
    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_triangle_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_triangle_fs(fg, fe);

    auto vs = gctx->create_program(vg, ve); // KIR -> HLSL -> DXIL, all behind the seam
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping B3-e DX12 draw"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr);

    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    REQUIRE(program->valid());

    constexpr crd::u32 dim    = 32U;
    auto               target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);

    crd::gputest::enc_draw(*raster,*target, *program, g::ClearColor{0.0F, 0.0F, 1.0F, 1.0F}, 3U);

    const crd::u32 centre = target->read_pixel(dim / 2U, dim / 2U);
    const crd::u32 corner = target->read_pixel(0U, 0U);
    CHECK((centre & 0xFFU) >= 250U);          // R high  => red (inside the IR-authored triangle)
    CHECK(((centre >> 16U) & 0xFFU) <= 5U);   // B low
    CHECK((corner & 0xFFU) <= 5U);            // R low   => blue clear (outside)
    CHECK(((corner >> 16U) & 0xFFU) >= 250U); // B high
}

// D-007 B17-a: WEIGHTED-BLENDED OIT (McGuire-Bavoil 2013) on DX12 — the SAME shared CKIR shaders + CPU oracle the Vulkan
// B17-a test drives. Four translucent full-screen quads accumulate in one order-independent pass (RGBA16F additive accum +
// R16F multiplicative revealage, INDEPENDENT per-RT blend PSO) → a composite PSO resolves them over an opaque background.
// Uniform frame ⇒ every texel matches the oracle within a tight LSB tolerance; DX12 == Vulkan (one IR, both backends).
TEST_CASE("D-007 B17-a: IR-authored WBOIT draws on DX12 (accum/reveal MRT + blend PSO + composite -> pixels)",
          "[dx12][raster][gpu][oit]")
{
    namespace kir = crd::kir;

    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    if (raster == nullptr || !raster->valid()) { WARN("no D3D12 raster device; skipping"); return; }
    crd::memory::TlsfAllocator alloc(8U << 20U);

    crd::gputest::WboitScene scene;
    scene.count          = 4U;
    scene.background[0]  = 0.10F; scene.background[1] = 0.10F; scene.background[2] = 0.12F;
    scene.color[0][0] = 0.90F; scene.color[0][1] = 0.15F; scene.color[0][2] = 0.10F; scene.alpha[0] = 0.50F; scene.depth[0] = 0.20F;
    scene.color[1][0] = 0.15F; scene.color[1][1] = 0.85F; scene.color[1][2] = 0.20F; scene.alpha[1] = 0.40F; scene.depth[1] = 0.55F;
    scene.color[2][0] = 0.20F; scene.color[2][1] = 0.25F; scene.color[2][2] = 0.90F; scene.alpha[2] = 0.30F; scene.depth[2] = 0.80F;
    scene.color[3][0] = 0.90F; scene.color[3][1] = 0.85F; scene.color[3][2] = 0.10F; scene.alpha[3] = 0.60F; scene.depth[3] = 0.35F;

    kir::KGraph tvg(&alloc); kir::KEntry tve; crd::gputest::build_wboit_transparent_vs(tvg, tve, scene);
    kir::KGraph tfg(&alloc); kir::KEntry tfe; crd::gputest::build_wboit_transparent_fs(tfg, tfe);
    kir::KGraph cvg(&alloc); kir::KEntry cve; crd::gputest::build_wboit_composite_vs(cvg, cve);
    kir::KGraph cfg(&alloc); kir::KEntry cfe; crd::gputest::build_wboit_composite_fs(cfg, cfe);

    auto tvp = gctx->create_program(tvg, tve);
    if (tvp == nullptr) { WARN("dxc/DXIL unavailable; skipping B17-a DX12 WBOIT"); return; }
    auto tfp = gctx->create_program(tfg, tfe);
    auto cvp = gctx->create_program(cvg, cve);
    auto cfp = gctx->create_program(cfg, cfe);
    REQUIRE(tfp != nullptr); REQUIRE(cvp != nullptr); REQUIRE(cfp != nullptr);

    auto transparent = raster->create_raster_program(*tvp, *tfp);
    auto composite   = raster->create_raster_program(*cvp, *cfp);
    REQUIRE(transparent != nullptr); REQUIRE(transparent->valid());
    REQUIRE(composite != nullptr);   REQUIRE(composite->valid());

    constexpr crd::u32 dim    = 32U;
    auto               target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);

    raster->draw_wboit(*target, *transparent, *composite,
                       g::ClearColor{scene.background[0], scene.background[1], scene.background[2], 1.0F},
                       scene.count * 6U);

    const crd::u32 expect = crd::gputest::wboit_oracle_pixel(scene);
    crd::u32       worst  = 0U;
    for (crd::u32 y = 0U; y < dim; ++y)
    {
        for (crd::u32 x = 0U; x < dim; ++x)
        {
            const crd::u32 d = crd::gputest::rgba8_max_channel_diff(target->read_pixel(x, y), expect);
            if (d > worst) { worst = d; }
        }
    }
    INFO("WBOIT worst per-channel LSB diff vs oracle = " << worst << " (expect 0x" << std::hex << expect << ")");
    CHECK(worst <= 2U);
    CHECK((expect & 0xFFU) > 0x30U); // sanity: red channel substantial (transparency actually composited)
}

// D-007 B17 OIT QUALITY SCOREBOARD (pure CPU): the approximate WBOIT tier vs the EXACT A-buffer reference on the shared
// scene. WBOIT is a single-pass depth-weighted approximation — it stays in the neighbourhood of the exact sorted composite
// but is measurably off, which is precisely why the exact A-buffer (B17-c) reference tier exists to score against.
TEST_CASE("D-007 B17: OIT quality scoreboard -- WBOIT approximates the exact A-buffer reference", "[oit][scoreboard]")
{
    const auto     scene = crd::gputest::make_oit_scene();
    const crd::u32 wboit = crd::gputest::wboit_oracle_pixel(scene);
    const crd::u32 exact = crd::gputest::oit_exact_composite_rgba8(scene);
    const crd::u32 err   = crd::gputest::rgba8_max_channel_diff(wboit, exact);
    INFO("WBOIT=0x" << std::hex << wboit << " exact=0x" << exact << std::dec << " maxLSBerr=" << err);
    CHECK(err > 4U);   // WBOIT is a genuine approximation (measurably off, ~14 LSB) -- the reason the exact tier exists
    CHECK(err <= 24U); // ...but stays in the right neighbourhood of the exact reference (bounded depth-weighted error)
}

// D-007 B4: the DX12 MESH-shader DEVICE path — a CKIR Mesh KEntry lowers to ms_6_5 HLSL -> DXIL, a STREAM PSO (MS+PS) built
// via ID3D12Device2::CreatePipelineState, and DispatchMesh renders the SAME shared triangle the vertex-pull path draws. This
// proves the mesh IR fans out to the DX12 DEVICE (pixels), not just DXC-validation. Guarded on the mesh-shader tier (OPTIONS7).
TEST_CASE("D-007 B4: DX12 MESH-shader DispatchMesh renders a triangle (CKIR mesh entry -> DXIL -> stream PSO -> pixels)",
          "[dx12][raster][gpu][ir][mesh]")
{
    namespace kir = crd::kir;

    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    if (raster == nullptr || !raster->valid()) { WARN("no D3D12 raster device; skipping"); return; }
    crd::memory::TlsfAllocator alloc(4U << 20U);

    // The SAME shared CKIR triangle the B3-e tests draw — but EMITTED BY A MESH SHADER (build_triangle_mesh), not vertex-pulled.
    kir::KGraph mg(&alloc);
    kir::KEntry me;
    crd::gputest::build_triangle_mesh(mg, me);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_triangle_fs(fg, fe);

    auto mesh = gctx->create_program(mg, me); // CKIR mesh entry -> ms_6_5 HLSL -> DXIL, behind the seam
    if (mesh == nullptr) { WARN("dxc/DXIL unavailable; skipping B4 DX12 mesh draw"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr);
    REQUIRE(mesh->stage() == g::ShaderStage::Mesh);

    auto program = raster->create_mesh_program(*mesh, *fs);
    if (program == nullptr) { WARN("no D3D12 mesh-shader tier (OPTIONS7 MeshShaderTier); skipping the mesh draw"); return; }
    REQUIRE(program->valid());

    constexpr crd::u32 dim    = 32U;
    auto               target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);

    raster->draw_mesh(*target, *program, g::ClearColor{0.0F, 0.0F, 1.0F, 1.0F}, 1U); // one meshlet workgroup

    const crd::u32 centre = target->read_pixel(dim / 2U, dim / 2U);
    const crd::u32 corner = target->read_pixel(0U, 0U);
    CHECK((centre & 0xFFU) >= 250U);          // R high  => red (inside the MESH-emitted triangle)
    CHECK(((centre >> 16U) & 0xFFU) <= 5U);   // B low
    CHECK((corner & 0xFFU) <= 5U);            // R low   => blue clear (outside)
    CHECK(((corner >> 16U) & 0xFFU) >= 250U); // B high  => blue clear
}

// D-007 B4: the DX12 TASK / AMPLIFICATION path — a CKIR Task entry lowers to as_6_5 amplification HLSL → DXIL, an AS+MS+PS
// stream PSO, and DispatchMesh from the AS launches N mesh workgroups (GPU-driven amplification) + a payload the mesh reads.
// ONE task workgroup ⇒ N triangles coloured by the payload. Proves amplification + the task→mesh payload channel on DX12.
TEST_CASE("D-007 B4: DX12 TASK amplification -- 1 task workgroup emits N mesh triangles + payload",
          "[dx12][raster][gpu][ir][mesh][task]")
{
    namespace kir = crd::kir;

    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    if (raster == nullptr || !raster->valid()) { WARN("no D3D12 raster device; skipping"); return; }
    crd::memory::TlsfAllocator alloc(4U << 20U);

    constexpr crd::u32 n_tri   = 4U;
    constexpr crd::u32 payload = 220U;
    kir::KGraph        tg(&alloc);
    kir::KEntry        te;
    crd::gputest::build_task_amplify(tg, te, n_tri, payload);
    kir::KGraph mg(&alloc);
    kir::KEntry me;
    crd::gputest::build_mesh_amplified_tri(mg, me);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_amplify_fs(fg, fe);

    auto task = gctx->create_program(tg, te); // CKIR task -> as_6_5 amplification HLSL -> DXIL
    if (task == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
    auto mesh = gctx->create_program(mg, me);
    auto fs   = gctx->create_program(fg, fe);
    REQUIRE(mesh != nullptr);
    REQUIRE(fs != nullptr);
    REQUIRE(task->stage() == g::ShaderStage::Task);

    auto program = raster->create_task_mesh_program(*task, *mesh, *fs);
    if (program == nullptr) { WARN("no D3D12 mesh-shader tier (OPTIONS7); skipping"); return; }
    REQUIRE(program->valid());

    constexpr crd::u32 dim    = 64U;
    auto               target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    raster->draw_mesh(*target, *program, g::ClearColor{0.0F, 0.0F, 0.2F, 1.0F}, 1U); // 1 TASK workgroup ⇒ N mesh triangles

    int lit = 0;
    for (crd::u32 c = 0; c < n_tri; ++c)
    {
        const double   xc = -0.7 + static_cast<double>(c) * 0.45;
        const crd::u32 sx = static_cast<crd::u32>((xc + 1.0) * 0.5 * static_cast<double>(dim));
        if ((target->read_pixel(sx, dim / 2U) & 0xFFU) > 180U) { ++lit; } // red ≈ payload(220)
    }
    CHECK(lit == static_cast<int>(n_tri)); // all N amplified triangles rendered with the payload colour
}

// D-007 B4: the MULTI-FIELD task→mesh payload on DX12 — a task passes a 3-uint payload (v0,v1,v2), each read by the mesh via
// KBuiltin::TaskPayload{,1,2} and coloured into R/G/B. Proves the DX12 groupshared MeshPayload struct carries all three fields.
TEST_CASE("D-007 B4: DX12 TASK multi-field payload -- a 3-uint payload flows task->mesh",
          "[dx12][raster][gpu][ir][mesh][task]")
{
    namespace kir = crd::kir;

    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    if (raster == nullptr || !raster->valid()) { WARN("no D3D12 raster device; skipping"); return; }
    crd::memory::TlsfAllocator alloc(4U << 20U);

    constexpr crd::u32 pay_r = 200U;
    constexpr crd::u32 pay_g = 120U;
    constexpr crd::u32 pay_b = 60U;
    kir::KGraph        tg(&alloc);
    kir::KEntry        te;
    crd::gputest::build_task_amplify_rgb(tg, te, 1U, pay_r, pay_g, pay_b);
    kir::KGraph mg(&alloc);
    kir::KEntry me;
    crd::gputest::build_mesh_amplified_rgb(mg, me);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_amplify_rgb_fs(fg, fe);

    auto task = gctx->create_program(tg, te);
    if (task == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
    auto mesh = gctx->create_program(mg, me);
    auto fs   = gctx->create_program(fg, fe);
    REQUIRE(mesh != nullptr);
    REQUIRE(fs != nullptr);
    auto program = raster->create_task_mesh_program(*task, *mesh, *fs);
    if (program == nullptr) { WARN("no D3D12 mesh-shader tier (OPTIONS7); skipping"); return; }
    REQUIRE(program->valid());

    constexpr crd::u32 dim    = 64U;
    auto               target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    raster->draw_mesh(*target, *program, g::ClearColor{0.0F, 0.0F, 0.1F, 1.0F}, 1U);

    const crd::u32 px = target->read_pixel(dim / 2U, dim / 2U); // inside the centred triangle
    CHECK((px & 0xFFU) > pay_r - 6U);          // R ≈ payload v0
    CHECK((px & 0xFFU) < pay_r + 6U);
    CHECK(((px >> 8U) & 0xFFU) > pay_g - 6U);  // G ≈ payload v1
    CHECK(((px >> 8U) & 0xFFU) < pay_g + 6U);
    CHECK(((px >> 16U) & 0xFFU) > pay_b - 6U); // B ≈ payload v2 — all three fields flowed
    CHECK(((px >> 16U) & 0xFFU) < pay_b + 6U);
}

// D-007 B4: GPU-DRIVEN INDIRECT MESHLET DISPATCH on DX12. A compute CULL pass tests 8 meshlets (5 visible) and writes the
// survivor count into an INDIRECT-dispatch args buffer; ExecuteIndirect with a DISPATCH_MESH command signature then launches
// EXACTLY that many mesh workgroups — the count decided on the GPU. Only the 5 survivors render; the 3 culled never dispatch.
TEST_CASE("D-007 B4: DX12 GPU-driven indirect meshlet dispatch -- a compute cull writes the dispatch count",
          "[dx12][raster][gpu][mesh][indirect]")
{
    namespace kir = crd::kir;
    namespace vb  = crd::kir::visbuffer;
    namespace cu  = crd::gpu::compute_usage;

    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    if (raster == nullptr || !raster->valid()) { WARN("no D3D12 raster device; skipping"); return; }
    crd::memory::TlsfAllocator alloc(4U << 20U);
    g::Dx12ComputeContext      compute(&alloc);
    if (!compute.valid()) { WARN("no D3D12 compute device; skipping"); return; }

    // 1) the compute CULL: 8 meshlets, keys[i] = (i < 5) → 5 survivors written into args[0] (ThreadGroupCountX).
    constexpr crd::u32    n_meshlets = 8U;
    constexpr crd::u32    survivors  = 5U;
    vb::MeshletCullConfig ccfg;
    ccfg.n_meshlets = n_meshlets;
    ccfg.local_size = 64U;
    kir::KGraph       cg(&alloc);
    const kir::KEntry ce = vb::build_meshlet_cull(cg, ccfg);
    kir::GlslKernel   kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_hlsl(cg, ce, &alloc, kern));
    auto pipe = compute.create_pipeline_from_hlsl(crd::containers::to_view(kern.source), 2, 0U);
    if (pipe == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }

    auto keys_dev = compute.create_buffer(n_meshlets * 4U, cu::storage | cu::transfer_dst, g::ComputeMemory::GpuOnly);
    auto args_dev = compute.create_buffer(3U * 4U, cu::storage | cu::indirect | cu::transfer_dst | cu::transfer_src,
                                          g::ComputeMemory::GpuOnly);
    auto keys_up  = compute.create_buffer(n_meshlets * 4U, cu::transfer_src, g::ComputeMemory::CpuToGpu);
    auto args_up  = compute.create_buffer(3U * 4U, cu::transfer_src, g::ComputeMemory::CpuToGpu);
    auto args_rb  = compute.create_buffer(3U * 4U, cu::transfer_dst, g::ComputeMemory::GpuToCpu);
    REQUIRE(args_dev != nullptr);
    auto* kp = static_cast<crd::u32*>(keys_up->map());
    for (crd::u32 i = 0; i < n_meshlets; ++i) { kp[i] = (i < survivors) ? 1U : 0U; }
    keys_up->unmap();
    auto* ap = static_cast<crd::u32*>(args_up->map());
    ap[0] = 0U;
    ap[1] = 1U;
    ap[2] = 1U;
    args_up->unmap();

    auto&                    rec      = compute.begin();
    crd::gpu::ComputeBuffer* binds[2] = {keys_dev.get(), args_dev.get()};
    rec.copy(*keys_up, *keys_dev, 0U, 0U, n_meshlets * 4U);
    rec.copy(*args_up, *args_dev, 0U, 0U, 3U * 4U);
    rec.barrier(*keys_dev, g::ComputeAccess::TransferDst, g::ComputeAccess::ShaderRead);
    rec.barrier(*args_dev, g::ComputeAccess::TransferDst, g::ComputeAccess::ShaderRead);
    rec.dispatch(*pipe, crd::containers::ConstSpan<crd::gpu::ComputeBuffer*>(binds, 2), nullptr, 0U, 1U, 1U, 1U);
    rec.barrier(*args_dev, g::ComputeAccess::ShaderWrite, g::ComputeAccess::TransferSrc);
    rec.copy(*args_dev, *args_rb, 0U, 0U, 3U * 4U);
    compute.submit_and_wait();

    const auto*    arp   = static_cast<const crd::u32*>(args_rb->map());
    const crd::u32 count = arp[0];
    args_rb->unmap();
    CHECK(count == survivors); // the compute cull computed the mesh-dispatch count on the GPU

    // 2) the raster INDIRECT mesh draw reads args_dev (the compute-written count) via ExecuteIndirect.
    kir::KGraph mg(&alloc);
    kir::KEntry me;
    crd::gputest::build_mesh_grid_tri(mg, me);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_amplify_fs(fg, fe);
    auto meshp = gctx->create_program(mg, me);
    auto fsp   = gctx->create_program(fg, fe);
    REQUIRE(meshp != nullptr);
    REQUIRE(fsp != nullptr);
    auto program = raster->create_mesh_program(*meshp, *fsp);
    if (program == nullptr) { WARN("no D3D12 mesh-shader tier (OPTIONS7); skipping"); return; }
    REQUIRE(program->valid());

    constexpr crd::u32 dim    = 64U;
    auto               target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    raster->draw_mesh_indirect(*target, *program, g::ClearColor{0.0F, 0.0F, 0.1F, 1.0F}, args_dev->native_handle(), 0U);

    int rendered = 0;
    int culled   = 0;
    for (crd::u32 w = 0; w < n_meshlets; ++w)
    {
        const double   xc  = -0.8 + static_cast<double>(w) * 0.2;
        const crd::u32 sx  = static_cast<crd::u32>((xc + 1.0) * 0.5 * static_cast<double>(dim));
        const bool     red = (target->read_pixel(sx, dim / 2U) & 0xFFU) > 180U;
        if (w < survivors) { if (red) { ++rendered; } }
        else if (!red) { ++culled; }
    }
    CHECK(rendered == static_cast<int>(survivors));            // all 5 survivors rendered via the indirect count
    CHECK(culled == static_cast<int>(n_meshlets - survivors)); // the 3 culled meshlets never dispatched
}

// D-007 B4: PER-PRIMITIVE VRS from a MESH shader on DX12 — the mesh's SV_ShadingRate output (2×2) drives the coarse fragment
// rate via draw_mesh_vrs (OVERRIDE combiner). Same ramp FS + coarsening check as Vulkan: a 2×2 rate makes each 2×2 block share
// one fragment invocation, so horizontal even-x pairs become EQUAL across the gradient.
TEST_CASE("D-007 B4: per-primitive VRS from a MESH shader coarsens shading (DX12)", "[dx12][raster][gpu][mesh][vrs]")
{
    namespace kir = crd::kir;

    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    if (raster == nullptr || !raster->valid()) { WARN("no D3D12 raster device; skipping"); return; }
    if (!raster->supports_vrs()) { WARN("no D3D12 VRS tier 2; skipping"); return; }
    crd::memory::TlsfAllocator alloc(4U << 20U);

    kir::KGraph mg(&alloc);
    kir::KEntry me;
    crd::gputest::build_vrs_primitive_mesh(mg, me); // mesh emits SV_ShadingRate = 2x2
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_vrs_ramp_fs(fg, fe);
    auto mesh = gctx->create_program(mg, me);
    if (mesh == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster->create_mesh_program(*mesh, *fs);
    if (program == nullptr) { WARN("no D3D12 mesh-shader tier (OPTIONS7); skipping"); return; }
    REQUIRE(program->valid());

    constexpr crd::u32 dim    = 32U;
    auto               target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    raster->draw_mesh_vrs(*target, *program, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 1U);

    const int nmesh = count_equal_even_pairs(*target, dim);
    WARN("[vrs mesh dx12] n_equal=" << nmesh);
    CHECK(nmesh > static_cast<int>(dim * dim / 4U)); // the mesh's per-primitive 2x2 rate coarsened the ramp
}

// D-007 B4-tess: the PORTABLE displacement path on DX12 — VS→HS→DS→PS. The VS emits a 4-corner quad patch (±0.6), the hull
// sets 8x8 tess factors, the domain reads TessPatchPosition (the emitter's bilerp) + EXPANDS the quad x1.3 (a per-vertex domain
// transform), the FS paints it red. Tessellation is core D3D12 (no feature gate). Proves the tessellator ran end to end: a pixel
// between the base edge (0.6) and the expanded edge (0.78) is red ONLY because the domain shader displaced the generated vertices.
TEST_CASE("D-007 B4-tess: DX12 tessellation -- a VS->HS->DS->PS quad subdivides + displaces",
          "[dx12][raster][gpu][ir][tess]")
{
    namespace kir = crd::kir;

    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    if (raster == nullptr || !raster->valid()) { WARN("no D3D12 raster device; skipping"); return; }
    crd::memory::TlsfAllocator alloc(4U << 20U);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_tess_quad_vs(vg, ve);
    kir::KGraph cg(&alloc);
    kir::KEntry ce;
    crd::gputest::build_tess_hull(cg, ce);
    kir::KGraph eg(&alloc);
    kir::KEntry ee;
    crd::gputest::build_tess_domain(eg, ee);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_triangle_fs(fg, fe);

    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
    auto tcs = gctx->create_program(cg, ce); // CKIR TessControl -> hs_6_0 hull HLSL -> DXIL
    auto tes = gctx->create_program(eg, ee); // CKIR TessEval    -> ds_6_0 domain HLSL -> DXIL
    auto fs  = gctx->create_program(fg, fe);
    REQUIRE(tcs != nullptr);
    REQUIRE(tes != nullptr);
    REQUIRE(fs != nullptr);
    REQUIRE(tcs->stage() == g::ShaderStage::TessControl);
    REQUIRE(tes->stage() == g::ShaderStage::TessEval);

    auto program = raster->create_tess_program(*vs, *tcs, *tes, *fs);
    REQUIRE(program != nullptr);
    REQUIRE(program->valid());

    constexpr crd::u32 dim    = 64U;
    auto               target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    raster->draw_tess(*target, *program, g::ClearColor{0.0F, 0.0F, 0.2F, 1.0F}, 1U); // ONE quad patch

    // Screen x for a clip x: sx = (x+1)/2·dim. The base quad edge is 0.6 (sx=51); the domain-expanded edge is 0.78 (sx=57).
    CHECK((target->read_pixel(dim / 2U, dim / 2U) & 0xFFU) >= 250U); // centre red — the quad renders
    const crd::u32 sx_expanded = static_cast<crd::u32>((0.72 + 1.0) * 0.5 * static_cast<double>(dim)); // clip x≈0.72 (sx≈55)
    CHECK((target->read_pixel(sx_expanded, dim / 2U) & 0xFFU) >= 250U); // between base (0.6) + expanded (0.78) — domain ran
    CHECK((target->read_pixel(63U, dim / 2U) & 0xFFU) < 40U); // clip x≈0.97, beyond the expanded quad — still the blue clear
}

// D-007 B4-vis-4: the HW-RASTER VISIBILITY BUFFER on DX12 — the hybrid Nanite path (HW raster wins on big triangles). A
// fullscreen quad (2 triangles) rasterizes into a R32_UINT target whose FS writes SV_PrimitiveID, so every pixel records
// which triangle covered it. Proves KBuiltin::PrimitiveId → DXIL + the R32_UINT PSO/target renders + read_pixel returns the id.
TEST_CASE("D-007 B4-vis-4: DX12 HW-raster visibility buffer writes SV_PrimitiveId per pixel",
          "[dx12][raster][gpu][ir][visbuffer]")
{
    namespace kir = crd::kir;

    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    if (raster == nullptr || !raster->valid()) { WARN("no D3D12 raster device; skipping"); return; }
    crd::memory::TlsfAllocator alloc(4U << 20U);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_visbuffer_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_visbuffer_fs(fg, fe);
    auto vs = gctx->create_program(vg, ve); // VS → vs_6_0 DXIL
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
    auto fs = gctx->create_program(fg, fe); // FS (SV_PrimitiveID → uint SV_Target) → ps_6_0 DXIL
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    REQUIRE(program->valid());

    constexpr crd::u32 dim    = 64U;
    auto               target = raster->create_visbuffer_target(dim, dim);
    REQUIRE(target != nullptr);
    crd::gputest::enc_draw_visbuffer(*raster,*target, *program, 0xFFFFFFFFU, 6U); // 6 verts = 2 triangles

    int n0 = 0;
    int n1 = 0;
    for (crd::u32 y = 0; y < dim; ++y)
    {
        for (crd::u32 x = 0; x < dim; ++x)
        {
            const crd::u32 id = target->read_pixel(x, y);
            if (id == 0U) { ++n0; }
            else if (id == 1U) { ++n1; }
        }
    }
    CHECK(n0 + n1 == static_cast<int>(dim * dim)); // fullscreen coverage → every pixel is primitive id 0 or 1
    CHECK(n0 > static_cast<int>(dim * dim) / 4);    // triangle 0 covers a substantial half (its distinct SV_PrimitiveID)
    CHECK(n1 > static_cast<int>(dim * dim) / 4);    // triangle 1 covers the other half (robust to the viewport y-flip)
}

// D-007 B4: the DX12 OCEAN-MESHLET path — draw_mesh_bindless_depth DISPATCHES the projected-grid ocean as MESHLETS into a
// colour+DEPTH target, the FFT cascade textures bound BINDLESS so the mesh shader samples the displacement (mirrors the Vulkan
// ocean-mesh render). Uses SYNTHETIC cascade textures (the point is the DEVICE path, not the FFT bake) and asserts the ocean
// surface rendered over the black clear. Guarded on the mesh-shader tier (OPTIONS7).
TEST_CASE("D-007 B4: DX12 ocean meshlets render via draw_mesh_bindless_depth (bindless cascades + depth)",
          "[dx12][raster][gpu][ir][mesh][ocean]")
{
    namespace kir = crd::kir;

    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    if (raster == nullptr || !raster->valid()) { WARN("no D3D12 raster device; skipping"); return; }
    crd::memory::TlsfAllocator alloc(16U << 20U);

    crd::gputest::OceanCascadeRender ocr; // the production 4-cascade config (defaults)
    const int                        nc = ocr.count;

    // The ocean MESH (projected-grid meshlets) + the water FS — the SAME shared CKIR builders the Vulkan ocean renders.
    constexpr int mnp = 16; // patches per side (16x7 = 112 lattice cells)
    constexpr int mkk = 8;  // 8x8 verts/patch -> 98 tris, within the SM6.5 mesh cap of 128
    kir::KGraph   mmg(&alloc);
    kir::KEntry   mme;
    crd::gputest::build_ocean_displaced_mesh(mmg, mme, mnp, mkk, ocr);
    kir::KGraph mfg(&alloc);
    kir::KEntry mfe;
    crd::gputest::build_ocean_water_geo_fs(mfg, mfe, ocr);

    auto mesh = gctx->create_program(mmg, mme); // CKIR ocean mesh -> ms_6_5 HLSL (SampleIndexedLod) -> DXIL
    if (mesh == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
    auto fs = gctx->create_program(mfg, mfe);
    REQUIRE(fs != nullptr);
    auto program = raster->create_mesh_program(*mesh, *fs);
    if (program == nullptr) { WARN("no D3D12 mesh-shader tier (OPTIONS7); skipping"); return; }
    REQUIRE(program->valid());

    // Synthetic RGBA8 cascade textures [nx, nz, height, foam]: a gentle deterministic ripple (no transcendentals) so the
    // projected grid displaces visibly. Even if displacement read 0, the grid still renders at the water plane and the FS
    // shades it — the test proves the DEVICE mesh+bindless+depth path, so any ocean coverage over the clear suffices.
    constexpr crd::u32 cw = 64U;
    crd::u8            px[cw * cw * 4U];
    for (crd::u32 y = 0U; y < cw; ++y)
    {
        for (crd::u32 x = 0U; x < cw; ++x)
        {
            const crd::u32   ripple = ((x * 8U) & 0xFFU) ^ ((y * 8U) & 0xFFU); // deterministic pattern in [0,255]
            const crd::usize o      = (static_cast<crd::usize>(y) * cw + x) * 4U;
            px[o + 0U] = 128U;                                          // nx -> 0
            px[o + 1U] = 128U;                                          // nz -> 0
            px[o + 2U] = static_cast<crd::u8>(96U + (ripple >> 2U));    // height field
            px[o + 3U] = 0U;                                            // foam
        }
    }
    std::unique_ptr<g::ITexture> tex_owned[8];
    g::ITexture*                 texs[8]{};
    for (int c = 0; c < nc; ++c)
    {
        tex_owned[c] = raster->create_texture(cw, cw, px);
        REQUIRE(tex_owned[c] != nullptr);
        texs[c] = tex_owned[c].get();
    }

    constexpr crd::u32 rdim   = 96U;
    auto               target = raster->create_color_depth_target(rdim, rdim);
    REQUIRE(target != nullptr);
    raster->draw_mesh_bindless_depth(*target, *program, g::ClearColor{0.0F, 0.0F, 0.0F, 0.0F}, 1.0F, g::DepthCompare::Less,
                                     texs, static_cast<crd::u32>(nc), static_cast<crd::u32>(mnp * mnp));

    // The ocean surface must have rendered over the black clear — count non-black pixels across the frame.
    crd::u32 lit = 0U;
    for (crd::u32 y = 0U; y < rdim; ++y)
    {
        for (crd::u32 x = 0U; x < rdim; ++x)
        {
            if ((target->read_pixel(x, y) & 0x00FFFFFFU) != 0U) { ++lit; }
        }
    }
    WARN("[dx12 ocean mesh] lit pixels = " << lit << " / " << (rdim * rdim));
    CHECK(lit > (rdim * rdim) / 20U); // >= ~5% of the frame is ocean geometry, not the black clear
}

// B16-a-4: the WATER SURFACE renders on DX12 too — the SAME shared build_water_fs the Vulkan test draws (`water_shade` behind
// the create_program seam), proving the fragment shading FANS OUT to both raster backends: a bluish deep-water body + a bright
// sun glint, identical shape to the Vulkan render.
TEST_CASE("B16-a-4: water_shade RENDERS on DX12 (bluish body + sun glint)", "[dx12][raster][ir][ocean]")
{
    namespace kir = crd::kir;

    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    crd::memory::TlsfAllocator alloc(8U << 20U);

    constexpr crd::u32 dim = 64U;
    kir::KGraph        vg(&alloc);
    kir::KEntry        ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_water_fs(fg, fe, dim);

    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping water DX12 draw"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    REQUIRE(program->valid());
    auto target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    crd::gputest::enc_draw(*raster,*target, *program, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);

    const crd::u32 c  = target->read_pixel(dim / 2U, dim / 2U);
    const int      cr = static_cast<int>(c & 0xFFU);
    const int      cg = static_cast<int>((c >> 8U) & 0xFFU);
    const int      cb = static_cast<int>((c >> 16U) & 0xFFU);
    int            maxlum = 0;
    for (crd::u32 y = 0; y < dim; ++y)
    {
        for (crd::u32 x = 0; x < dim; ++x)
        {
            const crd::u32 p   = target->read_pixel(x, y);
            const int      lum = static_cast<int>(p & 0xFFU) + static_cast<int>((p >> 8U) & 0xFFU) + static_cast<int>((p >> 16U) & 0xFFU);
            if (lum > maxlum) { maxlum = lum; }
        }
    }
    WARN("[water-render-dx12] centre RGB=(" << cr << "," << cg << "," << cb << ") maxlum=" << maxlum);
    CHECK(cb > cr);                      // deep water reads bluish
    CHECK(cb > 10);                      // and it rendered
    CHECK(maxlum > (cr + cg + cb) + 90); // a bright sun glint exists (same shape as Vulkan)
}

TEST_CASE("D-007 B1-a: IR fragment derivatives (dFdx/dFdy of FragCoord.x) draw on DX12", "[dx12][raster][gpu][ir]")
{
    namespace kir = crd::kir;

    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    crd::memory::TlsfAllocator alloc(4U << 20U);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_triangle_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_derivative_fs(fg, fe); // colour = (dFdx(FragCoord.x), dFdy(FragCoord.x), 0, 1)

    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping B1-a DX12 draw"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr);

    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    REQUIRE(program->valid());

    constexpr crd::u32 dim    = 32U;
    auto               target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    crd::gputest::enc_draw(*raster,*target, *program, g::ClearColor{0.0F, 0.0F, 1.0F, 1.0F}, 3U);

    // dFdx(FragCoord.x) == 1 (screen x rises 1/pixel) → R≈255; dFdy(FragCoord.x) == 0 → G≈0. The derivative RAN.
    const crd::u32 centre = target->read_pixel(dim / 2U, dim / 2U);
    CHECK((centre & 0xFFU) >= 250U);         // R  = dFdx == 1.0
    CHECK(((centre >> 8U) & 0xFFU) <= 5U);   // G  = dFdy == 0.0
}

TEST_CASE("D-007 B1-b: IR fragment discard (alpha-test on FragCoord.x) draws on DX12", "[dx12][raster][gpu][ir]")
{
    namespace kir = crd::kir;

    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    crd::memory::TlsfAllocator alloc(4U << 20U);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_triangle_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_discard_fs(fg, fe); // red, but discards where FragCoord.x < 16 (left half → clear)

    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping B1-b DX12 draw"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr);

    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    REQUIRE(program->valid());

    constexpr crd::u32 dim    = 32U;
    auto               target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    crd::gputest::enc_draw(*raster,*target, *program, g::ClearColor{0.0F, 0.0F, 1.0F, 1.0F}, 3U);

    // Both (12,16) and (20,16) are inside the triangle. Right of x=16 → kept (red); left → discarded (blue clear shows).
    const crd::u32 kept = target->read_pixel(20U, 16U);
    const crd::u32 cut  = target->read_pixel(12U, 16U);
    CHECK((kept & 0xFFU) >= 250U);          // R high  → red survived
    CHECK((cut & 0xFFU) <= 5U);             // R low
    CHECK(((cut >> 16U) & 0xFFU) >= 250U);  // B high  → discarded, blue clear shows through
}

TEST_CASE("D-007 B1-c: IR flat integer interpolant (VS->FS) draws on DX12", "[dx12][raster][gpu][ir]")
{
    namespace kir = crd::kir;

    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    crd::memory::TlsfAllocator alloc(4U << 20U);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_flat_vs(vg, ve); // flat int payload = 200 at location 0
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_flat_fs(fg, fe); // reads the flat int, colour = (200/255, 0, 0, 1)

    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping B1-c DX12 draw"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr); // an int varying only compiles because `nointerpolation` was emitted

    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    REQUIRE(program->valid());

    constexpr crd::u32 dim    = 32U;
    auto               target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    crd::gputest::enc_draw(*raster,*target, *program, g::ClearColor{0.0F, 0.0F, 1.0F, 1.0F}, 3U);

    // The flat int (200) reached the fragment intact → R ≈ 200/255 → unorm8 200.
    const crd::u32 centre = target->read_pixel(dim / 2U, dim / 2U);
    const crd::u32 red    = centre & 0xFFU;
    CHECK(red >= 196U);
    CHECK(red <= 204U);
    CHECK(((centre >> 16U) & 0xFFU) <= 5U); // B low (not the clear)
}

TEST_CASE("D-007 B1-c: IR noperspective vs smooth interpolant diverge on a perspective triangle (DX12)",
          "[dx12][raster][gpu][ir]")
{
    namespace kir = crd::kir;

    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    crd::memory::TlsfAllocator alloc(4U << 20U);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_noperspective_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_noperspective_fs(fg, fe);

    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 dim    = 32U;
    auto               target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    crd::gputest::enc_draw(*raster,*target, *program, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);

    // R = perspective-correct (smooth ≈ 0.069 → ~18), G = screen-linear (noperspective ≈ 0.225 → ~57). Dropping
    // `noperspective` would make both perspective-correct and R == G — so the gap is the biting gate.
    const crd::u32 centre = target->read_pixel(dim / 2U, dim / 2U);
    const auto     r      = static_cast<int>(centre & 0xFFU);
    const auto     grn    = static_cast<int>((centre >> 8U) & 0xFFU);
    CHECK(grn > r + 12);
    CHECK(r < 70);
    CHECK(grn < 110);
}

TEST_CASE("D-007 B1-c: IR centroid interpolation samples inside coverage on an MSAA target (DX12)",
          "[dx12][raster][gpu][ir]")
{
    namespace kir = crd::kir;

    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    crd::memory::TlsfAllocator alloc(4U << 20U);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_centroid_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_centroid_fs(fg, fe);

    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 dim    = 32U;
    auto               target = raster->create_color_target_ms(dim, dim, 4U);
    REQUIRE(target != nullptr); // 4x MSAA unsupported for RGBA8 would be a hard fail on any modern adapter
    crd::gputest::enc_draw(*raster,*target, *program, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);

    // R = centre-sampled (smooth), G = centroid-sampled. Equal on fully-covered interior; they diverge at partially
    // covered EDGE pixels. Dropping `centroid` ⇒ R == G everywhere ⇒ zero differing pixels — a band of them is the gate.
    int max_diff = 0;
    int n_diff   = 0;
    for (crd::u32 y = 0; y < dim; ++y)
    {
        for (crd::u32 x = 0; x < dim; ++x)
        {
            const crd::u32 px = target->read_pixel(x, y);
            const auto     r  = static_cast<int>(px & 0xFFU);
            const auto     gg = static_cast<int>((px >> 8U) & 0xFFU);
            const int      d  = r > gg ? r - gg : gg - r;
            if (d > max_diff) { max_diff = d; }
            if (d >= 2) { ++n_diff; }
        }
    }
    WARN("[centroid dx12] max|R-G| = " << max_diff << "  n_diff(>=2) = " << n_diff);
    CHECK(n_diff >= 6);
}

TEST_CASE("D-007 B1-c: IR sample interpolation forces per-sample shading on an MSAA target (DX12)",
          "[dx12][raster][gpu][ir]")
{
    namespace kir = crd::kir;

    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    crd::memory::TlsfAllocator alloc(8U << 20U);
    constexpr crd::u32         dim = 32U;

    const auto count_intermediate = [&](kir::Interp interp) -> int
    {
        kir::KGraph vg(&alloc);
        kir::KEntry ve;
        crd::gputest::build_ramp_vs(vg, ve, interp);
        kir::KGraph fg(&alloc);
        kir::KEntry fe;
        crd::gputest::build_step_fs(fg, fe, interp);
        auto vs = gctx->create_program(vg, ve);
        REQUIRE(vs != nullptr);
        auto fs = gctx->create_program(fg, fe);
        REQUIRE(fs != nullptr);
        auto program = raster->create_raster_program(*vs, *fs);
        REQUIRE(program != nullptr);
        auto target = raster->create_color_target_ms(dim, dim, 4U);
        REQUIRE(target != nullptr);
        crd::gputest::enc_draw(*raster,*target, *program, g::ClearColor{0.0F, 0.0F, 1.0F, 1.0F}, 3U);
        int n = 0;
        for (crd::u32 y = 0; y < dim; ++y)
        {
            for (crd::u32 x = 0; x < dim; ++x)
            {
                const auto rr = static_cast<int>(target->read_pixel(x, y) & 0xFFU);
                if (rr >= 40 && rr <= 215) { ++n; }
            }
        }
        return n;
    };

    const int n_sample = count_intermediate(kir::Interp::Sample);
    const int n_smooth = count_intermediate(kir::Interp::Smooth);
    WARN("[sample dx12] n_sample=" << n_sample << " n_smooth=" << n_smooth);
    CHECK(n_smooth == 0); // per-PIXEL shading of a step ⇒ hard 0/255 edge, no intermediates
    CHECK(n_sample >= 4); // per-SAMPLE shading antialiases the diagonal threshold ⇒ intermediate greys
}

TEST_CASE("D-007 B1-d: IR frag-depth write drives the depth test (DX12)", "[dx12][raster][gpu][ir]")
{
    namespace kir = crd::kir;
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    crd::memory::TlsfAllocator alloc(4U << 20U);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_fragdepth_fs(fg, fe);

    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 dim    = 32U;
    auto               target = raster->create_color_depth_target(dim, dim);
    REQUIRE(target != nullptr);
    crd::gputest::enc_draw_depth(*raster,*target, *program, g::ClearColor{0.0F, 0.0F, 1.0F, 1.0F}, 0.5F, g::DepthCompare::LessEqual, 3U);

    const crd::u32 left  = target->read_pixel(4U, dim / 2U);  // written depth ≈ 0.14 ≤ 0.5 ⇒ red
    const crd::u32 right = target->read_pixel(28U, dim / 2U); // written depth ≈ 0.89 > 0.5 ⇒ blue clear
    CHECK((left & 0xFFU) > 200U);
    CHECK(((left >> 16U) & 0xFFU) < 60U);
    CHECK((right & 0xFFU) < 60U);
    CHECK(((right >> 16U) & 0xFFU) > 200U);
}

TEST_CASE("D-007 B1-d: IR conservative depth (DepthGreater) frag-depth write (DX12)", "[dx12][raster][gpu][ir]")
{
    namespace kir = crd::kir;
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    crd::memory::TlsfAllocator alloc(4U << 20U);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_fragdepth_fs(fg, fe);
    fe.depth_mode = kir::DepthMode::Greater; // emits SV_DepthGreaterEqual; the ramp honours the promise

    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr); // SV_DepthGreaterEqual must lower to valid DXIL
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 dim    = 32U;
    auto               target = raster->create_color_depth_target(dim, dim);
    REQUIRE(target != nullptr);
    crd::gputest::enc_draw_depth(*raster,*target, *program, g::ClearColor{0.0F, 0.0F, 1.0F, 1.0F}, 0.5F, g::DepthCompare::LessEqual, 3U);
    CHECK((target->read_pixel(4U, dim / 2U) & 0xFFU) > 200U);           // left red
    CHECK(((target->read_pixel(28U, dim / 2U) >> 16U) & 0xFFU) > 200U); // right blue
}

TEST_CASE("D-007 B1-d: IR early_fragment_tests forces early-Z (DX12)", "[dx12][raster][gpu][ir]")
{
    namespace kir = crd::kir;
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    crd::memory::TlsfAllocator alloc(4U << 20U);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_early_fragment_fs(fg, fe); // red, [earlydepthstencil]

    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr); // [earlydepthstencil] must lower to valid DXIL
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 dim    = 32U;
    auto               target = raster->create_color_depth_target(dim, dim);
    REQUIRE(target != nullptr);
    crd::gputest::enc_draw_depth(*raster,*target, *program, g::ClearColor{0.0F, 0.0F, 1.0F, 1.0F}, 0.5F, g::DepthCompare::LessEqual, 3U);
    const crd::u32 centre = target->read_pixel(dim / 2U, dim / 2U);
    CHECK((centre & 0xFFU) > 200U);         // red (the FS ran, early test passed)
    CHECK(((centre >> 16U) & 0xFFU) < 60U); // not the blue clear
}

TEST_CASE("D-007 B1-e: per-draw VRS 2x2 coarsens shading (DX12)", "[dx12][raster][gpu][ir]")
{
    namespace kir = crd::kir;
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    if (!raster->supports_vrs()) { WARN("adapter has no Tier-2 VRS; skipping"); return; }
    crd::memory::TlsfAllocator alloc(4U << 20U);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_vrs_ramp_fs(fg, fe);

    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 dim = 32U;
    auto               t1  = raster->create_color_target(dim, dim);
    auto               t2  = raster->create_color_target(dim, dim);
    REQUIRE(t1 != nullptr);
    REQUIRE(t2 != nullptr);
    crd::gputest::enc_draw(*raster,*t1, *program, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);
    crd::gputest::enc_draw_vrs(*raster,*t2, *program, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, g::ShadingRate::Rate2x2,
                     g::ShadingRateCombiner::Keep, 3U);

    const int n1 = count_equal_even_pairs(*t1, dim);
    const int n2 = count_equal_even_pairs(*t2, dim);
    WARN("[vrs per-draw dx12] n_1x1=" << n1 << " n_2x2=" << n2);
    CHECK(n2 > n1 + 100);
    CHECK(n1 < 80);
}

TEST_CASE("D-007 B1-e: per-primitive VRS out (SV_ShadingRate) coarsens shading (DX12)", "[dx12][raster][gpu][ir]")
{
    namespace kir = crd::kir;
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    if (!raster->supports_vrs()) { WARN("adapter has no Tier-2 VRS; skipping"); return; }
    crd::memory::TlsfAllocator alloc(4U << 20U);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_vrs_primitive_vs(vg, ve); // VS outputs SV_ShadingRate = 2x2
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_vrs_ramp_fs(fg, fe);

    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr); // the VS emits SV_ShadingRate ⇒ must lower to valid DXIL
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 dim    = 32U;
    auto               target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    crd::gputest::enc_draw_vrs(*raster,*target, *program, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, g::ShadingRate::Rate1x1,
                     g::ShadingRateCombiner::Replace, 3U); // the primitive 2x2 rate replaces the 1x1 pipeline rate

    const int n = count_equal_even_pairs(*target, dim);
    WARN("[vrs per-primitive dx12] n_equal=" << n);
    CHECK(n > static_cast<int>(dim * dim / 4U));
}

TEST_CASE("D-007 B1-e: attachment (image) VRS 2x2 coarsens shading (DX12)", "[dx12][raster][gpu][ir]")
{
    namespace kir = crd::kir;
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    if (!raster->supports_vrs()) { WARN("adapter has no Tier-2 VRS; skipping"); return; }
    crd::memory::TlsfAllocator alloc(4U << 20U);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_vrs_ramp_fs(fg, fe);

    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 dim    = 32U;
    auto               target = raster->create_color_vrs_target(dim, dim, g::ShadingRate::Rate2x2);
    REQUIRE(target != nullptr);
    crd::gputest::enc_draw_vrs(*raster,*target, *program, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, g::ShadingRate::Rate1x1,
                     g::ShadingRateCombiner::Keep, 3U); // the per-tile attachment 2x2 rate replaces

    const int n = count_equal_even_pairs(*target, dim);
    WARN("[vrs attachment dx12] n_equal=" << n);
    CHECK(n > static_cast<int>(dim * dim / 4U));
}

// B1-f: count target pixels whose R channel is high (the constant-red triangle's coverage), over a non-red clear.
namespace
{
inline int count_red(g::IRasterTarget& t, crd::u32 dim)
{
    int n = 0;
    for (crd::u32 y = 0; y < dim; ++y)
    {
        for (crd::u32 x = 0; x < dim; ++x) { if ((t.read_pixel(x, y) & 0xFFU) > 200U) { ++n; } }
    }
    return n;
}
} // namespace

TEST_CASE("D-007 B1-f: conservative OVERESTIMATE raster covers more pixels (DX12)", "[dx12][raster][gpu][ir]")
{
    namespace kir = crd::kir;
    auto        gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    if (!raster->supports_conservative_raster()) { WARN("adapter has no conservative raster; skipping"); return; }
    crd::memory::TlsfAllocator alloc(4U << 20U);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_small_triangle_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_triangle_fs(fg, fe); // constant red

    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 dim    = 64U;
    auto               t_norm = raster->create_color_target(dim, dim);
    auto               t_over = raster->create_color_target(dim, dim);
    const g::ClearColor blue{0.0F, 0.0F, 1.0F, 1.0F};
    REQUIRE(t_norm != nullptr);
    REQUIRE(t_over != nullptr);
    crd::gputest::enc_draw_conservative(*raster,*t_norm, *program, blue, g::ConservativeMode::Off, 3U);
    crd::gputest::enc_draw_conservative(*raster,*t_over, *program, blue, g::ConservativeMode::Overestimate, 3U);

    const int n_norm = count_red(*t_norm, dim);
    const int n_over = count_red(*t_over, dim);
    WARN("[conservative dx12] n_normal=" << n_norm << " n_over=" << n_over);
    CHECK(n_norm > 0);
    CHECK(n_over > n_norm); // overestimate additionally covers the partially-touched edge pixels
}

TEST_CASE("D-007 B1-f: inner coverage distinguishes fully-covered from edge pixels (DX12)", "[dx12][raster][gpu][ir]")
{
    namespace kir = crd::kir;
    auto        gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    if (!raster->supports_inner_coverage()) { WARN("adapter has no Tier-3 conservative raster (inner coverage); skipping"); return; }
    crd::memory::TlsfAllocator alloc(4U << 20U);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_small_triangle_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_inner_coverage_fs(fg, fe); // SV_InnerCoverage → white/black

    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr); // reads SV_InnerCoverage ⇒ must lower to valid DXIL
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 dim    = 64U;
    auto               target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    crd::gputest::enc_draw_conservative(*raster,*target, *program, g::ClearColor{0.0F, 0.0F, 1.0F, 1.0F}, g::ConservativeMode::Overestimate,
                              3U);

    int white = 0;
    int black = 0;
    for (crd::u32 y = 0; y < dim; ++y)
    {
        for (crd::u32 x = 0; x < dim; ++x)
        {
            const crd::u32 px = target->read_pixel(x, y);
            const crd::u32 rr = px & 0xFFU;
            const crd::u32 gg = (px >> 8U) & 0xFFU;
            const crd::u32 bb = (px >> 16U) & 0xFFU;
            if (rr > 200U && gg > 200U && bb > 200U) { ++white; }
            else if (rr < 50U && gg < 50U && bb < 50U) { ++black; }
        }
    }
    WARN("[inner coverage dx12] white=" << white << " black=" << black);
    CHECK(white > 0);
    CHECK(black > 0); // inner coverage VARIES across the primitive
}

TEST_CASE("D-007 B1-f: fragment interlock RMW counter is deterministic (DX12)", "[dx12][raster][gpu][ir]")
{
    namespace kir = crd::kir;
    auto        gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    if (!raster->supports_fragment_interlock()) { WARN("adapter has no ROVs (fragment interlock); skipping"); return; }
    crd::memory::TlsfAllocator alloc(4U << 20U);

    constexpr crd::u32 dim = 32U;
    kir::KGraph        vg(&alloc);
    kir::KEntry        ve;
    crd::gputest::build_interlock_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_interlock_fs(fg, fe, dim); // RasterizerOrderedStructuredBuffer RMW: storage[y*dim + x] += 1

    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr); // RasterizerOrderedStructuredBuffer ⇒ must lower to valid DXIL (ROV)
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    auto target  = raster->create_color_target(dim, dim);
    auto storage = raster->create_storage_buffer(dim * dim * 4U);
    REQUIRE(target != nullptr);
    REQUIRE(storage != nullptr);
    raster->draw_storage(*target, *program, g::ClearColor{0.0F, 0.0F, 1.0F, 1.0F}, *storage, 6U); // two triangles

    const crd::u32 c_centre = storage->read_u32((dim / 2U) * dim + dim / 2U);
    const crd::u32 c_corner = storage->read_u32(0U);
    WARN("[interlock dx12] centre=" << c_centre << " corner=" << c_corner);
    CHECK(c_centre == 2U); // both primitives cover the centre; ROV serialises the RMW ⇒ exactly 2
    CHECK(c_corner == 0U);
    CHECK((target->read_pixel(dim / 2U, dim / 2U) & 0xFFU) > 200U);
}

TEST_CASE("D-007 B2-a: IR 2D texture sample (left-red/right-green) draws on DX12", "[dx12][raster][gpu][ir]")
{
    namespace kir = crd::kir;
    auto        gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    crd::memory::TlsfAllocator alloc(4U << 20U);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_textured_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_sample_fs(fg, fe); // Texture2D tex_0_1 + SamplerState samp_0_2 → tex.Sample(samp, uv)

    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr); // the FS declares Texture2D + SamplerState ⇒ must lower to valid DXIL
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 tw = 16U;
    crd::u8            tex_data[tw * tw * 4U];
    crd::gputest::fill_left_red_right_green(tex_data, tw, tw);
    auto texture = raster->create_texture(tw, tw, tex_data);
    REQUIRE(texture != nullptr);

    constexpr crd::u32 dim    = 32U;
    auto               target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    crd::gputest::enc_draw_textured(*raster,*target, *program, g::ClearColor{0.0F, 0.0F, 1.0F, 1.0F}, *texture, 3U);

    const crd::u32 left  = target->read_pixel(dim / 4U, dim / 2U);
    const crd::u32 right = target->read_pixel(3U * dim / 4U, dim / 2U);
    WARN("[texture dx12] left R=" << (left & 0xFFU) << " G=" << ((left >> 8U) & 0xFFU) << " | right R=" << (right & 0xFFU)
                                  << " G=" << ((right >> 8U) & 0xFFU));
    CHECK((left & 0xFFU) > 200U);          // left: R high (red)
    CHECK(((left >> 8U) & 0xFFU) < 60U);   // left: G low
    CHECK(((right >> 8U) & 0xFFU) > 200U); // right: G high (green)
    CHECK((right & 0xFFU) < 60U);          // right: R low
}

TEST_CASE("D-007 B2-b: IR sample-op family (Lod/Grad/texelFetch/gather/textureSize) on DX12", "[dx12][raster][gpu][ir]")
{
    namespace kir = crd::kir;
    auto        gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    crd::memory::TlsfAllocator alloc(16U << 20U);

    constexpr crd::u32 tw = 16U;
    crd::u8            tex_data[tw * tw * 4U];
    crd::gputest::fill_left_red_right_green(tex_data, tw, tw);
    auto texture = raster->create_texture(tw, tw, tex_data);
    REQUIRE(texture != nullptr);

    constexpr crd::u32 dim   = 32U;
    bool               dxc_ok = true;
    const auto run = [&](void (*build_fs)(kir::KGraph&, kir::KEntry&), crd::u32& left, crd::u32& right) {
        kir::KGraph vg(&alloc); kir::KEntry ve; crd::gputest::build_textured_vs(vg, ve);
        kir::KGraph fg(&alloc); kir::KEntry fe; build_fs(fg, fe);
        auto vs = gctx->create_program(vg, ve);
        if (vs == nullptr) { dxc_ok = false; return; }
        auto fs = gctx->create_program(fg, fe);
        REQUIRE(fs != nullptr);
        auto program = raster->create_raster_program(*vs, *fs);
        REQUIRE(program != nullptr);
        auto target = raster->create_color_target(dim, dim);
        REQUIRE(target != nullptr);
        crd::gputest::enc_draw_textured(*raster,*target, *program, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, *texture, 3U);
        left  = target->read_pixel(dim / 4U, dim / 2U);
        right = target->read_pixel(3U * dim / 4U, dim / 2U);
    };

    crd::u32 l = 0;
    crd::u32 rt = 0;
    run(crd::gputest::build_samplelod_fs, l, rt);
    if (!dxc_ok) { WARN("dxc/DXIL unavailable; skipping"); return; }
    CHECK((l & 0xFFU) > 200U); CHECK(((rt >> 8U) & 0xFFU) > 200U);
    run(crd::gputest::build_samplegrad_fs, l, rt);
    CHECK((l & 0xFFU) > 200U); CHECK(((rt >> 8U) & 0xFFU) > 200U);
    run([](kir::KGraph& gg, kir::KEntry& e) { crd::gputest::build_texelfetch_fs(gg, e, tw); }, l, rt);
    CHECK((l & 0xFFU) > 200U); CHECK(((l >> 8U) & 0xFFU) < 60U); CHECK(((rt >> 8U) & 0xFFU) > 200U); CHECK((rt & 0xFFU) < 60U);
    run(crd::gputest::build_gather_fs, l, rt);
    WARN("[gather dx12] left R=" << (l & 0xFFU) << " right R=" << (rt & 0xFFU));
    CHECK((l & 0xFFU) > 200U); CHECK((rt & 0xFFU) < 60U);
    run(crd::gputest::build_texsize_fs, l, rt);
    WARN("[texsize dx12] R=" << (l & 0xFFU) << " G=" << ((l >> 8U) & 0xFFU));
    CHECK((l & 0xFFU) == tw); CHECK(((l >> 8U) & 0xFFU) == tw);
}

TEST_CASE("D-007 B2-b: IR shadow-compare sample (SampleCmp on a depth texture) on DX12", "[dx12][raster][gpu][ir]")
{
    namespace kir = crd::kir;
    auto        gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    crd::memory::TlsfAllocator alloc(4U << 20U);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_textured_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_shadow_fs(fg, fe); // Texture2D<float> + SamplerComparisonState → SampleCmp

    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr); // SampleCmp on a Texture2D<float> + SamplerComparisonState ⇒ must lower to valid DXIL
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 tw = 16U;
    float             depth[tw * tw];
    crd::gputest::fill_uniform_depth(depth, tw, tw, 0.5F);
    auto dtex = raster->create_depth_texture(tw, tw, depth);
    REQUIRE(dtex != nullptr);

    constexpr crd::u32 dim    = 32U;
    auto               target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    crd::gputest::enc_draw_shadow(*raster,*target, *program, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, *dtex, 3U);

    const crd::u32 left  = target->read_pixel(dim / 4U, dim / 2U);
    const crd::u32 right = target->read_pixel(3U * dim / 4U, dim / 2U);
    WARN("[shadow dx12] left R=" << (left & 0xFFU) << " right R=" << (right & 0xFFU));
    CHECK((left & 0xFFU) > 200U);  // left passed (white)
    CHECK((right & 0xFFU) < 60U);  // right failed (black)
}

TEST_CASE("D-007 B8-f: IR shadow-map foundation + bias stack renders on DX12", "[dx12][raster][gpu][ir][lighting]")
{
    namespace kir = crd::kir;
    auto        gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    crd::memory::TlsfAllocator alloc(4U << 20U);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_shadow_foundation_fs(fg, fe);
    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 tw = 16U;
    float              depth[tw * tw];
    crd::gputest::fill_uniform_depth(depth, tw, tw, 0.5F);
    auto dtex = raster->create_depth_texture(tw, tw, depth);
    REQUIRE(dtex != nullptr);
    constexpr crd::u32 dim    = 32U;
    auto               target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    crd::gputest::enc_draw_shadow(*raster,*target, *program, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, *dtex, 3U);

    const auto     rc   = [](crd::u32 px) { return static_cast<int>(px & 0xFFU); };
    const crd::u32 left = target->read_pixel(7U, dim / 2U);
    const crd::u32 rght = target->read_pixel(24U, dim / 2U);
    WARN("[shadow-foundation dx12] left R=" << rc(left) << " right R=" << rc(rght));
    CHECK(rc(left) > 200);  // lit
    CHECK(rc(rght) < 40);   // shadowed
}

TEST_CASE("D-007 B8-g: IR PCF filtered soft shadows render on DX12", "[dx12][raster][gpu][ir][lighting]")
{
    namespace kir = crd::kir;
    auto        gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    crd::memory::TlsfAllocator alloc(4U << 20U);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_pcf_shadow_fs(fg, fe);
    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 tw = 16U;
    float              depth[tw * tw];
    crd::gputest::fill_uniform_depth(depth, tw, tw, 0.5F);
    auto dtex = raster->create_depth_texture(tw, tw, depth);
    REQUIRE(dtex != nullptr);
    constexpr crd::u32 dim    = 32U;
    auto               target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    crd::gputest::enc_draw_shadow(*raster,*target, *program, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, *dtex, 3U);

    const auto     rc   = [](crd::u32 px) { return static_cast<int>(px & 0xFFU); };
    const crd::u32 left = target->read_pixel(6U, dim / 2U);
    const crd::u32 rght = target->read_pixel(25U, dim / 2U);
    WARN("[pcf dx12] left R=" << rc(left) << " right R=" << rc(rght));
    CHECK(rc(left) > 200);  // lit
    CHECK(rc(rght) < 40);   // shadowed
}

TEST_CASE("D-007 B2-c: IR texture dimensions (1D/3D/Cube/2DArray/CubeArray) on DX12", "[dx12][raster][gpu][ir]")
{
    namespace kir = crd::kir;
    auto        gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    crd::memory::TlsfAllocator alloc(16U << 20U);

    constexpr crd::u32 dim    = 32U;
    bool               dxc_ok = true;
    const auto run = [&](void (*build_fs)(kir::KGraph&, kir::KEntry&), g::ITexture& tex) {
        kir::KGraph vg(&alloc); kir::KEntry ve; crd::gputest::build_textured_vs(vg, ve);
        kir::KGraph fg(&alloc); kir::KEntry fe; build_fs(fg, fe);
        auto vs = gctx->create_program(vg, ve);
        if (vs == nullptr) { dxc_ok = false; return; }
        auto fs = gctx->create_program(fg, fe);
        REQUIRE(fs != nullptr);
        auto program = raster->create_raster_program(*vs, *fs);
        REQUIRE(program != nullptr);
        auto target = raster->create_color_target(dim, dim);
        REQUIRE(target != nullptr);
        crd::gputest::enc_draw_textured(*raster,*target, *program, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, tex, 3U);
        const crd::u32 l = target->read_pixel(dim / 4U, dim / 2U);
        const crd::u32 rr = target->read_pixel(3U * dim / 4U, dim / 2U);
        CHECK((l & 0xFFU) > 200U); CHECK(((l >> 8U) & 0xFFU) < 60U);
        CHECK(((rr >> 8U) & 0xFFU) > 200U); CHECK((rr & 0xFFU) < 60U);
    };

    {
        crd::u8 d[16U * 1U * 4U];
        crd::gputest::fill_left_red_right_green(d, 16U, 1U);
        auto t = raster->create_texture_dim(g::TextureKind::Tex1D, 16U, 1U, 1U, d);
        REQUIRE(t != nullptr);
        run(crd::gputest::build_sample_1d_fs, *t);
        if (!dxc_ok) { WARN("dxc/DXIL unavailable; skipping"); return; }
    }
    {
        crd::u8 d[16U * 16U * 2U * 4U];
        crd::gputest::fill_left_red_right_green(d, 16U, 16U);
        crd::gputest::fill_left_red_right_green(d + 16U * 16U * 4U, 16U, 16U);
        auto t = raster->create_texture_dim(g::TextureKind::Tex3D, 16U, 16U, 2U, d);
        REQUIRE(t != nullptr);
        run(crd::gputest::build_sample_3d_fs, *t);
    }
    {
        crd::u8 d[8U * 8U * 6U * 4U];
        crd::gputest::fill_solid(d + 0U * 64U * 4U, 64U, 0U, 255U, 0U);   // +X green (right)
        crd::gputest::fill_solid(d + 1U * 64U * 4U, 64U, 255U, 0U, 0U);   // -X red   (left)
        for (crd::u32 f = 2; f < 6; ++f) { crd::gputest::fill_solid(d + f * 64U * 4U, 64U, 0U, 0U, 255U); }
        auto t = raster->create_texture_dim(g::TextureKind::Cube, 8U, 8U, 6U, d);
        REQUIRE(t != nullptr);
        run(crd::gputest::build_sample_cube_fs, *t);
    }
    {
        crd::u8 d[8U * 8U * 2U * 4U];
        crd::gputest::fill_solid(d + 0U * 64U * 4U, 64U, 255U, 0U, 0U);
        crd::gputest::fill_solid(d + 1U * 64U * 4U, 64U, 0U, 255U, 0U);
        auto t = raster->create_texture_dim(g::TextureKind::Tex2DArray, 8U, 8U, 2U, d);
        REQUIRE(t != nullptr);
        run(crd::gputest::build_sample_array_fs, *t);
    }
    {
        crd::u8 d[8U * 8U * 12U * 4U];
        crd::gputest::fill_solid(d + 0U, 6U * 64U, 255U, 0U, 0U);
        crd::gputest::fill_solid(d + 6U * 64U * 4U, 6U * 64U, 0U, 255U, 0U);
        auto t = raster->create_texture_dim(g::TextureKind::CubeArray, 8U, 8U, 12U, d);
        REQUIRE(t != nullptr);
        run(crd::gputest::build_sample_cubearray_fs, *t);
    }
}

TEST_CASE("D-007 B2-d: IR bindless texture array (dynamic index) on DX12", "[dx12][raster][gpu][ir]")
{
    namespace kir = crd::kir;
    auto        gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    if (!raster->supports_bindless()) { WARN("adapter below resource-binding Tier 2; skipping"); return; }
    crd::memory::TlsfAllocator alloc(4U << 20U);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_textured_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_bindless_fs(fg, fe); // Texture2D tex[8] : register(t3) + NonUniformResourceIndex

    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr); // Texture2D tex[8] + NonUniformResourceIndex ⇒ must lower to valid DXIL
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    crd::u8 red[4U * 4U * 4U];
    crd::u8 green[4U * 4U * 4U];
    crd::gputest::fill_solid(red, 16U, 255U, 0U, 0U);
    crd::gputest::fill_solid(green, 16U, 0U, 255U, 0U);
    auto t_red   = raster->create_texture(4U, 4U, red);
    auto t_green = raster->create_texture(4U, 4U, green);
    REQUIRE(t_red != nullptr);
    REQUIRE(t_green != nullptr);
    g::ITexture* texs[2] = {t_red.get(), t_green.get()};

    constexpr crd::u32 dim    = 32U;
    auto               target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    crd::gputest::enc_draw_bindless(*raster,*target, *program, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, texs, 2U, 3U);

    const crd::u32 left  = target->read_pixel(dim / 4U, dim / 2U);
    const crd::u32 right = target->read_pixel(3U * dim / 4U, dim / 2U);
    WARN("[bindless dx12] left R=" << (left & 0xFFU) << " right G=" << ((right >> 8U) & 0xFFU));
    CHECK((left & 0xFFU) > 200U);          // left: texture[0] red
    CHECK(((right >> 8U) & 0xFFU) > 200U); // right: texture[1] green
    CHECK((right & 0xFFU) < 60U);
}

TEST_CASE("D-007 B5-a: IR OpenPBR surface material writes the deferred G-buffer (MRT) on DX12", "[dx12][raster][gpu][ir]")
{
    namespace kir = crd::kir;
    auto        gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    crd::memory::TlsfAllocator alloc(4U << 20U);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_surface_material_fs(fg, fe);

    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr); // 4 SV_Target outputs (MRT) ⇒ must lower to valid DXIL
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 dim  = 16U;
    auto               gbuf = raster->create_gbuffer_target(dim, dim, 4U);
    REQUIRE(gbuf != nullptr);
    REQUIRE(gbuf->attachment_count() == 4U);
    raster->draw_gbuffer(*gbuf, *program, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);

    const auto     ch   = [](crd::u32 px, int c) { return static_cast<int>((px >> (8 * c)) & 0xFFU); };
    const auto     near = [](int got, int want) { return got >= want - 6 && got <= want + 6; };
    const crd::u32 g0 = gbuf->read_pixel(0U, dim / 2U, dim / 2U);
    const crd::u32 g1 = gbuf->read_pixel(1U, dim / 2U, dim / 2U);
    const crd::u32 g2 = gbuf->read_pixel(2U, dim / 2U, dim / 2U);
    const crd::u32 g3 = gbuf->read_pixel(3U, dim / 2U, dim / 2U);
    WARN("[gbuffer dx12] g0=" << ch(g0, 0) << "," << ch(g0, 1) << "," << ch(g0, 2) << "," << ch(g0, 3) << " g1="
                              << ch(g1, 0) << "," << ch(g1, 2) << "," << ch(g1, 3) << " g2G=" << ch(g2, 1) << " g3R=" << ch(g3, 0));
    CHECK(near(ch(g0, 0), 204)); CHECK(near(ch(g0, 1), 51)); CHECK(near(ch(g0, 2), 26)); CHECK(near(ch(g0, 3), 128));
    CHECK(near(ch(g1, 0), 128)); CHECK(ch(g1, 2) > 250); CHECK(near(ch(g1, 3), 77));
    CHECK(near(ch(g2, 1), 230)); CHECK(near(ch(g2, 3), 179));
    CHECK(ch(g3, 0) > 250);
}

TEST_CASE("D-007 B5-b: IR full OpenPBR 1.1 slab (coat/fuzz/transmission/thin-film/subsurface) on DX12",
          "[dx12][raster][gpu][ir]")
{
    namespace kir = crd::kir;
    auto        gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    crd::memory::TlsfAllocator alloc(8U << 20U);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_surface_full_material_fs(fg, fe);

    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr); // 8 SV_Target outputs ⇒ must lower to valid DXIL
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 dim  = 16U;
    auto               gbuf = raster->create_gbuffer_target(dim, dim, 8U);
    REQUIRE(gbuf != nullptr);
    REQUIRE(gbuf->attachment_count() == 8U);
    raster->draw_gbuffer(*gbuf, *program, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);

    const auto ch   = [&](crd::u32 att, int c) { return static_cast<int>((gbuf->read_pixel(att, dim / 2U, dim / 2U) >> (8 * c)) & 0xFFU); };
    const auto near = [](int got, int want) { return got >= want - 6 && got <= want + 6; };
    WARN("[slab dx12] spec_w=" << ch(3, 1) << " coat_w=" << ch(3, 2) << " fuzz_w=" << ch(3, 3) << " coat_b=" << ch(4, 2)
                               << " trans_w=" << ch(6, 3) << " tf_w=" << ch(7, 0) << " ss_w=" << ch(7, 2)
                               << " thinwall=" << ch(7, 3));
    CHECK(near(ch(3, 1), 153)); CHECK(near(ch(3, 2), 102)); CHECK(near(ch(3, 3), 204));
    CHECK(near(ch(4, 2), 230)); CHECK(near(ch(4, 3), 51));
    CHECK(near(ch(5, 0), 230)); CHECK(near(ch(5, 3), 153));
    CHECK(near(ch(6, 1), 204)); CHECK(near(ch(6, 3), 64));
    CHECK(near(ch(7, 0), 230)); CHECK(near(ch(7, 1), 140)); CHECK(near(ch(7, 2), 89)); CHECK(ch(7, 3) > 250);
}

TEST_CASE("D-007 B5-c: IR shading-model tag (Gooch) + masked alpha domain on DX12", "[dx12][raster][gpu][ir]")
{
    namespace kir = crd::kir;
    auto        gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    crd::memory::TlsfAllocator alloc(4U << 20U);

    bool       dxc_ok = true;
    const auto link = [&](void (*build_fs)(kir::KGraph&, kir::KEntry&)) -> std::unique_ptr<g::IRasterProgram> {
        kir::KGraph vg(&alloc); kir::KEntry ve; crd::gputest::build_fullscreen_vs(vg, ve);
        kir::KGraph fg(&alloc); kir::KEntry fe; build_fs(fg, fe);
        auto vs = gctx->create_program(vg, ve);
        if (vs == nullptr) { dxc_ok = false; return nullptr; }
        auto fs = gctx->create_program(fg, fe);
        REQUIRE(fs != nullptr);
        return raster->create_raster_program(*vs, *fs);
    };

    { // shading-model tag (Gooch = 4)
        auto program = link(crd::gputest::build_gooch_material_fs);
        if (!dxc_ok) { WARN("dxc/DXIL unavailable; skipping"); return; }
        REQUIRE(program != nullptr);
        auto gbuf = raster->create_gbuffer_target(16U, 16U, 4U);
        REQUIRE(gbuf != nullptr);
        raster->draw_gbuffer(*gbuf, *program, g::ClearColor{0.0F, 0.0F, 0.0F, 0.0F}, 3U);
        const int sm = static_cast<int>((gbuf->read_pixel(3U, 8U, 8U) >> 8U) & 0xFFU);
        WARN("[shading-model dx12] gbuf3.G=" << sm << " (Gooch=4)");
        CHECK(sm == static_cast<int>(kir::material::ShadingModel::Gooch));
    }
    { // masked
        constexpr crd::u32 dim = 32U;
        kir::KGraph vg(&alloc); kir::KEntry ve; crd::gputest::build_fullscreen_vs(vg, ve);
        kir::KGraph fg(&alloc); kir::KEntry fe; crd::gputest::build_masked_material_fs(fg, fe, dim);
        auto vs = gctx->create_program(vg, ve);
        REQUIRE(vs != nullptr);
        auto fs = gctx->create_program(fg, fe);
        REQUIRE(fs != nullptr);
        auto program = raster->create_raster_program(*vs, *fs);
        REQUIRE(program != nullptr);
        auto gbuf = raster->create_gbuffer_target(dim, dim, 4U);
        REQUIRE(gbuf != nullptr);
        raster->draw_gbuffer(*gbuf, *program, g::ClearColor{0.0F, 0.0F, 0.0F, 0.0F}, 3U);
        const int left  = static_cast<int>(gbuf->read_pixel(0U, dim / 4U, dim / 2U) & 0xFFU);
        const int right = static_cast<int>(gbuf->read_pixel(0U, 3U * dim / 4U, dim / 2U) & 0xFFU);
        WARN("[masked dx12] left R=" << left << " right R=" << right);
        CHECK(left < 20);
        CHECK(right > 200);
    }
}

TEST_CASE("D-007 B6-a: IR MaterialX operator nodes (overlay per-channel branch) render on DX12", "[dx12][raster][gpu][ir][nodes]")
{
    namespace kir = crd::kir;
    auto        gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    crd::memory::TlsfAllocator alloc(4U << 20U);

    constexpr crd::u32 dim = 32U;
    kir::KGraph        vg(&alloc);
    kir::KEntry        ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_nodes_overlay_fs(fg, fe, dim); // overlay(fg,bg-ramp,1): branch flips at centre

    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    auto target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    crd::gputest::enc_draw(*raster,*target, *program, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);

    const auto     ch   = [](crd::u32 px, int c) { return static_cast<int>((px >> (8 * c)) & 0xFFU); };
    const auto     near = [](int got, int want) { return got >= want - 6 && got <= want + 6; };
    const crd::u32 left  = target->read_pixel(4U, dim / 2U);  // bg≈0.1406<0.5 ⇒ multiply: 2*fg*bg
    const crd::u32 right = target->read_pixel(28U, dim / 2U); // bg≈0.8906≥0.5 ⇒ screen: 1-2*(1-bg)*(1-fg)
    WARN("[nodes overlay dx12] left=" << ch(left, 0) << "," << ch(left, 1) << "," << ch(left, 2)
                                      << " right=" << ch(right, 0) << "," << ch(right, 1) << "," << ch(right, 2));
    CHECK(near(ch(left, 0), 57)); CHECK(near(ch(left, 1), 36)); CHECK(near(ch(left, 2), 14));
    CHECK(near(ch(right, 0), 244)); CHECK(near(ch(right, 1), 227)); CHECK(near(ch(right, 2), 210));
}

TEST_CASE("D-007 B6-b: IR MaterialX perlin noise (U32 Bob-Jenkins hash) renders on DX12", "[dx12][raster][gpu][ir][nodes][noise]")
{
    namespace kir = crd::kir;
    auto        gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    crd::memory::TlsfAllocator alloc(16U << 20U);

    constexpr crd::u32 dim = 32U;
    kir::KGraph        vg(&alloc);
    kir::KEntry        ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_noise_perlin_fs(fg, fe);

    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr); // the U32 hash must lower to valid DXIL (uint ops, logical >>)
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    auto target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    crd::gputest::enc_draw(*raster,*target, *program, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);

    // Same expected as Vulkan: each column equals the library's own F32 eval — pixel-identical across backends.
    int  bad = 0;
    bool any = false;
    for (crd::u32 x = 2U; x < dim - 2U; x += 5U)
    {
        const int got  = static_cast<int>(target->read_pixel(x, dim / 2U) & 0xFFU);
        const int want = crd::gputest::build_noise_perlin_expected(x);
        if (got < want - 4 || got > want + 4) { ++bad; }
        if (got != 128) { any = true; }
    }
    WARN("[noise perlin dx12] col2 got=" << (target->read_pixel(2U, dim / 2U) & 0xFFU) << " want=" << crd::gputest::build_noise_perlin_expected(2U));
    CHECK(bad == 0);
    CHECK(any);
}

TEST_CASE("D-007 B6-b: IR MaterialX worley (cellular) noise renders on DX12", "[dx12][raster][gpu][ir][nodes][noise]")
{
    namespace kir = crd::kir;
    auto        gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    crd::memory::TlsfAllocator alloc(16U << 20U);

    constexpr crd::u32 dim = 32U;
    kir::KGraph        vg(&alloc);
    kir::KEntry        ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_noise_worley_fs(fg, fe);

    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    auto target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    crd::gputest::enc_draw(*raster,*target, *program, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);

    int  bad = 0;
    bool any = false;
    for (crd::u32 x = 2U; x < dim - 2U; x += 5U)
    {
        const int got  = static_cast<int>(target->read_pixel(x, dim / 2U) & 0xFFU);
        const int want = crd::gputest::build_noise_worley_expected(x);
        if (got < want - 4 || got > want + 4) { ++bad; }
        if (got != 0) { any = true; }
    }
    WARN("[noise worley dx12] col7 got=" << (target->read_pixel(7U, dim / 2U) & 0xFFU) << " want=" << crd::gputest::build_noise_worley_expected(7U));
    CHECK(bad == 0);
    CHECK(any);
}

TEST_CASE("D-007 B6-c: IR MaterialX UV place2d (rotate2d: radians/sin/cos) renders on DX12", "[dx12][raster][gpu][ir][nodes][uv]")
{
    namespace kir = crd::kir;
    auto        gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    crd::memory::TlsfAllocator alloc(8U << 20U);

    constexpr crd::u32 dim = 32U;
    kir::KGraph        vg(&alloc);
    kir::KEntry        ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_uv_place2d_fs(fg, fe);

    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    auto target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    crd::gputest::enc_draw(*raster,*target, *program, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);

    int  bad = 0;
    bool any = false;
    for (crd::u32 x = 2U; x < dim - 2U; x += 5U)
    {
        const int got  = static_cast<int>(target->read_pixel(x, dim / 2U) & 0xFFU);
        const int want = crd::gputest::build_uv_place2d_expected(x);
        if (got < want - 4 || got > want + 4) { ++bad; }
        if (got != 0 && got != 255) { any = true; }
    }
    WARN("[uv place2d dx12] col7 got=" << (target->read_pixel(7U, dim / 2U) & 0xFFU) << " want=" << crd::gputest::build_uv_place2d_expected(7U));
    CHECK(bad == 0);
    CHECK(any);
}

TEST_CASE("D-007 B6-d: IR MaterialX NPR gooch_shade (normalize/dot/reflect/mix/pow) renders on DX12", "[dx12][raster][gpu][ir][nodes][npr]")
{
    namespace kir = crd::kir;
    auto        gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    crd::memory::TlsfAllocator alloc(8U << 20U);

    constexpr crd::u32 dim = 32U;
    kir::KGraph        vg(&alloc);
    kir::KEntry        ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_npr_gooch_fs(fg, fe);

    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    auto target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    crd::gputest::enc_draw(*raster,*target, *program, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);

    const auto ch = [](crd::u32 px, int c) { return static_cast<int>((px >> (8 * c)) & 0xFFU); };
    int        bad = 0;
    bool       any = false;
    for (crd::u32 x = 2U; x < dim - 2U; x += 5U)
    {
        const crd::u32 px = target->read_pixel(x, dim / 2U);
        for (int c = 0; c < 3; ++c) { const int want = crd::gputest::build_npr_gooch_expected(x, c); if (ch(px, c) < want - 4 || ch(px, c) > want + 4) { ++bad; } }
        if (ch(px, 0) != ch(2U, 0)) { any = true; }
    }
    WARN("[npr gooch dx12] col7 rgb=" << ch(target->read_pixel(7U, dim / 2U), 0) << "," << ch(target->read_pixel(7U, dim / 2U), 1) << "," << ch(target->read_pixel(7U, dim / 2U), 2)
                                      << " want=" << crd::gputest::build_npr_gooch_expected(7U, 0) << "," << crd::gputest::build_npr_gooch_expected(7U, 1) << "," << crd::gputest::build_npr_gooch_expected(7U, 2));
    CHECK(bad == 0);
    CHECK(any);
}

TEST_CASE("D-007 B7-c: IR a LOWERED material (const-fold+DCE+CSE) renders identically on DX12", "[dx12][raster][gpu][ir][lower]")
{
    namespace kir = crd::kir;
    auto        gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    crd::memory::TlsfAllocator alloc(4U << 20U);

    constexpr crd::u32 dim = 32U;
    kir::KGraph        vg(&alloc);
    kir::KEntry        ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_lowered_overlay_fs(fg, fe, dim);

    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    auto target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    crd::gputest::enc_draw(*raster,*target, *program, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);

    const auto     ch   = [](crd::u32 px, int c) { return static_cast<int>((px >> (8 * c)) & 0xFFU); };
    const auto     near = [](int got, int want) { return got >= want - 6 && got <= want + 6; };
    const crd::u32 left  = target->read_pixel(4U, dim / 2U);
    const crd::u32 right = target->read_pixel(28U, dim / 2U);
    WARN("[lowered overlay dx12] left=" << ch(left, 0) << "," << ch(left, 1) << "," << ch(left, 2) << " right=" << ch(right, 0) << "," << ch(right, 1) << "," << ch(right, 2));
    CHECK(near(ch(left, 0), 57)); CHECK(near(ch(left, 1), 36)); CHECK(near(ch(left, 2), 14));
    CHECK(near(ch(right, 0), 244)); CHECK(near(ch(right, 1), 227)); CHECK(near(ch(right, 2), 210));
}

TEST_CASE("D-007 B8-a: IR Cook-Torrance BRDF (GGX + multiscatter) renders on DX12", "[dx12][raster][gpu][ir][lighting]")
{
    namespace kir = crd::kir;
    auto        gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    crd::memory::TlsfAllocator alloc(8U << 20U);

    constexpr crd::u32 dim = 32U;
    kir::KGraph        vg(&alloc);
    kir::KEntry        ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_lighting_brdf_fs(fg, fe);

    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    auto target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    crd::gputest::enc_draw(*raster,*target, *program, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);

    const auto ch = [](crd::u32 px, int c) { return static_cast<int>((px >> (8 * c)) & 0xFFU); };
    int        bad = 0;
    bool       any = false;
    for (crd::u32 x = 2U; x < dim - 2U; x += 5U)
    {
        const crd::u32 px = target->read_pixel(x, dim / 2U);
        for (int c = 0; c < 3; ++c) { const int want = crd::gputest::build_lighting_brdf_expected(x, c); if (ch(px, c) < want - 4 || ch(px, c) > want + 4) { ++bad; } }
        if (ch(px, 0) != ch(2U, 0)) { any = true; }
    }
    WARN("[brdf dx12] col7 rgb=" << ch(target->read_pixel(7U, dim / 2U), 0) << "," << ch(target->read_pixel(7U, dim / 2U), 1) << "," << ch(target->read_pixel(7U, dim / 2U), 2)
                                 << " want=" << crd::gputest::build_lighting_brdf_expected(7U, 0) << "," << crd::gputest::build_lighting_brdf_expected(7U, 1) << "," << crd::gputest::build_lighting_brdf_expected(7U, 2));
    CHECK(bad == 0);
    CHECK(any);
}

TEST_CASE("D-007 B8-b: IR OpenPBR lobes (clearcoat + sheen layered) render on DX12", "[dx12][raster][gpu][ir][lighting]")
{
    namespace kir = crd::kir;
    auto        gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    crd::memory::TlsfAllocator alloc(8U << 20U);

    constexpr crd::u32 dim = 32U;
    kir::KGraph        vg(&alloc);
    kir::KEntry        ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_lighting_layered_fs(fg, fe);

    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    auto target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    crd::gputest::enc_draw(*raster,*target, *program, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);

    const auto ch = [](crd::u32 px, int c) { return static_cast<int>((px >> (8 * c)) & 0xFFU); };
    int        bad = 0;
    bool       any = false;
    for (crd::u32 x = 2U; x < dim - 2U; x += 5U)
    {
        const crd::u32 px = target->read_pixel(x, dim / 2U);
        for (int c = 0; c < 3; ++c) { const int want = crd::gputest::build_lighting_layered_expected(x, c); if (ch(px, c) < want - 4 || ch(px, c) > want + 4) { ++bad; } }
        if (ch(px, 0) != ch(2U, 0)) { any = true; }
    }
    WARN("[layered dx12] col7 rgb=" << ch(target->read_pixel(7U, dim / 2U), 0) << "," << ch(target->read_pixel(7U, dim / 2U), 1) << "," << ch(target->read_pixel(7U, dim / 2U), 2)
                                    << " want=" << crd::gputest::build_lighting_layered_expected(7U, 0) << "," << crd::gputest::build_lighting_layered_expected(7U, 1) << "," << crd::gputest::build_lighting_layered_expected(7U, 2));
    CHECK(bad == 0);
    CHECK(any);
}

TEST_CASE("D-007 B8-b: IR thin-film iridescence + transmission (glass) render on DX12", "[dx12][raster][gpu][ir][lighting]")
{
    namespace kir = crd::kir;
    auto        gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    crd::memory::TlsfAllocator alloc(8U << 20U);

    constexpr crd::u32 dim = 32U;
    kir::KGraph        vg(&alloc);
    kir::KEntry        ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_lighting_glass_fs(fg, fe);

    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    auto target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    crd::gputest::enc_draw(*raster,*target, *program, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);

    const auto ch = [](crd::u32 px, int c) { return static_cast<int>((px >> (8 * c)) & 0xFFU); };
    int        bad = 0;
    bool       any = false;
    for (crd::u32 x = 2U; x < dim - 2U; x += 5U)
    {
        const crd::u32 px = target->read_pixel(x, dim / 2U);
        for (int c = 0; c < 3; ++c) { const int want = crd::gputest::build_lighting_glass_expected(x, c); if (ch(px, c) < want - 4 || ch(px, c) > want + 4) { ++bad; } }
        if (ch(px, 0) != ch(2U, 0)) { any = true; }
    }
    WARN("[glass dx12] col7 rgb=" << ch(target->read_pixel(7U, dim / 2U), 0) << "," << ch(target->read_pixel(7U, dim / 2U), 1) << "," << ch(target->read_pixel(7U, dim / 2U), 2)
                                  << " want=" << crd::gputest::build_lighting_glass_expected(7U, 0) << "," << crd::gputest::build_lighting_glass_expected(7U, 1) << "," << crd::gputest::build_lighting_glass_expected(7U, 2));
    CHECK(bad == 0);
    CHECK(any);
}

TEST_CASE("D-007 B8-c: IR punctual lights (directional + point + spot) render on DX12", "[dx12][raster][gpu][ir][lighting]")
{
    namespace kir = crd::kir;
    auto        gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    crd::memory::TlsfAllocator alloc(8U << 20U);

    constexpr crd::u32 dim = 32U;
    kir::KGraph        vg(&alloc);
    kir::KEntry        ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_lighting_lights_fs(fg, fe);

    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    auto target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    crd::gputest::enc_draw(*raster,*target, *program, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);

    const auto ch = [](crd::u32 px, int c) { return static_cast<int>((px >> (8 * c)) & 0xFFU); };
    int        bad = 0;
    bool       any = false;
    for (crd::u32 x = 2U; x < dim - 2U; x += 5U)
    {
        const crd::u32 px = target->read_pixel(x, dim / 2U);
        for (int c = 0; c < 3; ++c) { const int want = crd::gputest::build_lighting_lights_expected(x, c); if (ch(px, c) < want - 4 || ch(px, c) > want + 4) { ++bad; } }
        if (ch(px, 0) != ch(2U, 0)) { any = true; }
    }
    WARN("[lights dx12] col7 rgb=" << ch(target->read_pixel(7U, dim / 2U), 0) << "," << ch(target->read_pixel(7U, dim / 2U), 1) << "," << ch(target->read_pixel(7U, dim / 2U), 2)
                                   << " want=" << crd::gputest::build_lighting_lights_expected(7U, 0) << "," << crd::gputest::build_lighting_lights_expected(7U, 1) << "," << crd::gputest::build_lighting_lights_expected(7U, 2));
    CHECK(bad == 0);
    CHECK(any);
}

TEST_CASE("D-007 B8-d: IR area light (LTC diffuse rectangle) renders on DX12", "[dx12][raster][gpu][ir][lighting]")
{
    namespace kir = crd::kir;
    auto        gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    crd::memory::TlsfAllocator alloc(8U << 20U);

    constexpr crd::u32 dim = 32U;
    kir::KGraph        vg(&alloc);
    kir::KEntry        ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_lighting_area_fs(fg, fe);

    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    auto target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    crd::gputest::enc_draw(*raster,*target, *program, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);

    const auto ch = [](crd::u32 px, int c) { return static_cast<int>((px >> (8 * c)) & 0xFFU); };
    int        bad = 0;
    bool       any = false;
    for (crd::u32 x = 2U; x < dim - 2U; x += 5U)
    {
        const crd::u32 px = target->read_pixel(x, dim / 2U);
        for (int c = 0; c < 3; ++c) { const int want = crd::gputest::build_lighting_area_expected(x, c); if (ch(px, c) < want - 4 || ch(px, c) > want + 4) { ++bad; } }
        if (ch(px, 0) != ch(2U, 0)) { any = true; }
    }
    WARN("[area dx12] col7 rgb=" << ch(target->read_pixel(7U, dim / 2U), 0) << "," << ch(target->read_pixel(7U, dim / 2U), 1) << "," << ch(target->read_pixel(7U, dim / 2U), 2)
                                 << " want=" << crd::gputest::build_lighting_area_expected(7U, 0) << "," << crd::gputest::build_lighting_area_expected(7U, 1) << "," << crd::gputest::build_lighting_area_expected(7U, 2));
    CHECK(bad == 0);
    CHECK(any);
}

TEST_CASE("D-007 B8-d: IR tube area light (LTC line integral) renders on DX12", "[dx12][raster][gpu][ir][lighting]")
{
    namespace kir = crd::kir;
    auto        gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    crd::memory::TlsfAllocator alloc(8U << 20U);

    constexpr crd::u32 dim = 32U;
    kir::KGraph        vg(&alloc);
    kir::KEntry        ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_lighting_tube_fs(fg, fe);

    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    auto target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    crd::gputest::enc_draw(*raster,*target, *program, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);

    const auto ch = [](crd::u32 px, int c) { return static_cast<int>((px >> (8 * c)) & 0xFFU); };
    int        bad = 0;
    bool       any = false;
    for (crd::u32 x = 2U; x < dim - 2U; x += 5U)
    {
        const crd::u32 px = target->read_pixel(x, dim / 2U);
        for (int c = 0; c < 3; ++c) { const int want = crd::gputest::build_lighting_tube_expected(x, c); if (ch(px, c) < want - 4 || ch(px, c) > want + 4) { ++bad; } }
        if (ch(px, 0) != ch(2U, 0)) { any = true; }
    }
    WARN("[tube dx12] col7 rgb=" << ch(target->read_pixel(7U, dim / 2U), 0) << "," << ch(target->read_pixel(7U, dim / 2U), 1) << "," << ch(target->read_pixel(7U, dim / 2U), 2)
                                 << " want=" << crd::gputest::build_lighting_tube_expected(7U, 0) << "," << crd::gputest::build_lighting_tube_expected(7U, 1) << "," << crd::gputest::build_lighting_tube_expected(7U, 2));
    CHECK(bad == 0);
    CHECK(any);
}

TEST_CASE("D-007 B8-d: IR disk area light (LTC ellipse + SolveCubic) renders on DX12", "[dx12][raster][gpu][ir][lighting]")
{
    namespace kir = crd::kir;
    auto        gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    crd::memory::TlsfAllocator alloc(8U << 20U);

    constexpr crd::u32 dim = 32U;
    kir::KGraph        vg(&alloc);
    kir::KEntry        ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_lighting_disk_fs(fg, fe);

    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    auto target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    crd::gputest::enc_draw(*raster,*target, *program, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);

    const auto ch = [](crd::u32 px, int c) { return static_cast<int>((px >> (8 * c)) & 0xFFU); };
    int        bad = 0;
    bool       any = false;
    for (crd::u32 x = 2U; x < dim - 2U; x += 5U)
    {
        const crd::u32 px = target->read_pixel(x, dim / 2U);
        for (int c = 0; c < 3; ++c) { const int want = crd::gputest::build_lighting_disk_expected(x, c); if (ch(px, c) < want - 4 || ch(px, c) > want + 4) { ++bad; } }
        if (ch(px, 0) != ch(2U, 0)) { any = true; }
    }
    WARN("[disk dx12] col7 rgb=" << ch(target->read_pixel(7U, dim / 2U), 0) << "," << ch(target->read_pixel(7U, dim / 2U), 1) << "," << ch(target->read_pixel(7U, dim / 2U), 2)
                                 << " want=" << crd::gputest::build_lighting_disk_expected(7U, 0) << "," << crd::gputest::build_lighting_disk_expected(7U, 1) << "," << crd::gputest::build_lighting_disk_expected(7U, 2));
    CHECK(bad == 0);
    CHECK(any);
}

TEST_CASE("D-007 B8-e: IR image-based lighting (SH irradiance + split-sum specular) renders on DX12", "[dx12][raster][gpu][ir][lighting]")
{
    namespace kir = crd::kir;
    auto        gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    crd::memory::TlsfAllocator alloc(8U << 20U);

    constexpr crd::u32 dim = 32U;
    kir::KGraph        vg(&alloc);
    kir::KEntry        ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_lighting_ibl_fs(fg, fe);

    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    auto target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    crd::gputest::enc_draw(*raster,*target, *program, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);

    const auto ch = [](crd::u32 px, int c) { return static_cast<int>((px >> (8 * c)) & 0xFFU); };
    int        bad = 0;
    bool       any = false;
    for (crd::u32 x = 2U; x < dim - 2U; x += 5U)
    {
        const crd::u32 px = target->read_pixel(x, dim / 2U);
        for (int c = 0; c < 3; ++c) { const int want = crd::gputest::build_lighting_ibl_expected(x, c); if (ch(px, c) < want - 4 || ch(px, c) > want + 4) { ++bad; } }
        if (ch(px, 0) != ch(2U, 0)) { any = true; }
    }
    WARN("[ibl dx12] col7 rgb=" << ch(target->read_pixel(7U, dim / 2U), 0) << "," << ch(target->read_pixel(7U, dim / 2U), 1) << "," << ch(target->read_pixel(7U, dim / 2U), 2)
                                << " want=" << crd::gputest::build_lighting_ibl_expected(7U, 0) << "," << crd::gputest::build_lighting_ibl_expected(7U, 1) << "," << crd::gputest::build_lighting_ibl_expected(7U, 2));
    CHECK(bad == 0);
    CHECK(any);
}

TEST_CASE("D-007 B8-h: IR cascaded shadow-map selection (split/select/snap/blend) renders on DX12", "[dx12][raster][gpu][ir][lighting]")
{
    namespace kir = crd::kir;
    auto        gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    crd::memory::TlsfAllocator alloc(8U << 20U);

    constexpr crd::u32 dim = 32U;
    kir::KGraph        vg(&alloc);
    kir::KEntry        ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_lighting_csm_fs(fg, fe);

    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    auto target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    crd::gputest::enc_draw(*raster,*target, *program, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);

    const auto ch = [](crd::u32 px, int c) { return static_cast<int>((px >> (8 * c)) & 0xFFU); };
    int        bad = 0;
    for (crd::u32 x = 2U; x < dim - 2U; x += 5U)
    {
        const crd::u32 px = target->read_pixel(x, dim / 2U);
        for (int c = 0; c < 3; ++c) { const int want = crd::gputest::build_lighting_csm_expected(x, c); if (ch(px, c) < want - 4 || ch(px, c) > want + 4) { ++bad; } }
    }
    // cascade index (R) rises left→right across the three splits: near band = cascade 0, far band = cascade 3.
    CHECK(ch(target->read_pixel(3U, dim / 2U), 0) < ch(target->read_pixel(29U, dim / 2U), 0));
    WARN("[csm dx12] col28 rgb=" << ch(target->read_pixel(28U, dim / 2U), 0) << "," << ch(target->read_pixel(28U, dim / 2U), 1) << "," << ch(target->read_pixel(28U, dim / 2U), 2)
                                 << " want=" << crd::gputest::build_lighting_csm_expected(28U, 0) << "," << crd::gputest::build_lighting_csm_expected(28U, 1) << "," << crd::gputest::build_lighting_csm_expected(28U, 2));
    CHECK(bad == 0);
}

TEST_CASE("D-007 B8-i: IR screen-space + translucent shadows (contact / Fourier-opacity / VSM) render on DX12", "[dx12][raster][gpu][ir][lighting]")
{
    namespace kir = crd::kir;
    auto        gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    crd::memory::TlsfAllocator alloc(8U << 20U);
    constexpr crd::u32         dim = 32U;

    const auto ch  = [](crd::u32 px, int c) { return static_cast<int>((px >> (8 * c)) & 0xFFU); };
    using fs_fn    = void (*)(kir::KGraph&, kir::KEntry&);
    using exp_fn   = int (*)(crd::u32, int);
    struct Obs { fs_fn fs; exp_fn ex; const char* tag; };
    const Obs cases[] = {{crd::gputest::build_lighting_contact_fs, crd::gputest::build_lighting_contact_expected, "contact"},
                         {crd::gputest::build_lighting_fom_fs, crd::gputest::build_lighting_fom_expected, "fom"},
                         {crd::gputest::build_lighting_vsm_fs, crd::gputest::build_lighting_vsm_expected, "vsm"}};
    for (const auto& tc : cases)
    {
        kir::KGraph vg(&alloc);
        kir::KEntry ve;
        crd::gputest::build_fullscreen_vs(vg, ve);
        kir::KGraph fg(&alloc);
        kir::KEntry fe;
        tc.fs(fg, fe);
        auto vs = gctx->create_program(vg, ve);
        if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
        auto fs = gctx->create_program(fg, fe);
        REQUIRE(fs != nullptr);
        auto program = raster->create_raster_program(*vs, *fs);
        REQUIRE(program != nullptr);
        auto target = raster->create_color_target(dim, dim);
        REQUIRE(target != nullptr);
        crd::gputest::enc_draw(*raster,*target, *program, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);
        int bad = 0;
        for (crd::u32 x = 2U; x < dim - 2U; x += 5U)
        {
            const crd::u32 px = target->read_pixel(x, dim / 2U);
            for (int c = 0; c < 3; ++c) { const int want = tc.ex(x, c); if (ch(px, c) < want - 4 || ch(px, c) > want + 4) { ++bad; } }
        }
        WARN("[" << tc.tag << " dx12] col27 rgb=" << ch(target->read_pixel(27U, dim / 2U), 0) << "," << ch(target->read_pixel(27U, dim / 2U), 1) << "," << ch(target->read_pixel(27U, dim / 2U), 2)
                 << " want=" << tc.ex(27U, 0) << "," << tc.ex(27U, 1) << "," << tc.ex(27U, 2));
        CHECK(bad == 0);
    }
}

TEST_CASE("D-007 B8-j: IR skinning (linear-blend + dual-quaternion) renders on DX12", "[dx12][raster][gpu][ir][lighting]")
{
    namespace kir = crd::kir;
    auto        gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    crd::memory::TlsfAllocator alloc(8U << 20U);
    constexpr crd::u32         dim = 32U;

    const auto ch  = [](crd::u32 px, int c) { return static_cast<int>((px >> (8 * c)) & 0xFFU); };
    using fs_fn    = void (*)(kir::KGraph&, kir::KEntry&);
    using exp_fn   = int (*)(crd::u32, int);
    struct Obs { fs_fn fs; exp_fn ex; const char* tag; };
    const Obs cases[] = {{crd::gputest::build_lighting_lbsskin_fs, crd::gputest::build_lighting_lbsskin_expected, "lbs"},
                         {crd::gputest::build_lighting_dqskin_fs, crd::gputest::build_lighting_dqskin_expected, "dquat"}};
    for (const auto& tc : cases)
    {
        kir::KGraph vg(&alloc);
        kir::KEntry ve;
        crd::gputest::build_fullscreen_vs(vg, ve);
        kir::KGraph fg(&alloc);
        kir::KEntry fe;
        tc.fs(fg, fe);
        auto vs = gctx->create_program(vg, ve);
        if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
        auto fs = gctx->create_program(fg, fe);
        REQUIRE(fs != nullptr);
        auto program = raster->create_raster_program(*vs, *fs);
        REQUIRE(program != nullptr);
        auto target = raster->create_color_target(dim, dim);
        REQUIRE(target != nullptr);
        crd::gputest::enc_draw(*raster,*target, *program, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);
        int bad = 0;
        for (crd::u32 x = 2U; x < dim - 2U; x += 5U)
        {
            const crd::u32 px = target->read_pixel(x, dim / 2U);
            for (int c = 0; c < 3; ++c) { const int want = tc.ex(x, c); if (ch(px, c) < want - 4 || ch(px, c) > want + 4) { ++bad; } }
        }
        CHECK(ch(target->read_pixel(3U, dim / 2U), 0) != ch(target->read_pixel(29U, dim / 2U), 0));
        WARN("[" << tc.tag << " dx12] col27 rgb=" << ch(target->read_pixel(27U, dim / 2U), 0) << "," << ch(target->read_pixel(27U, dim / 2U), 1) << "," << ch(target->read_pixel(27U, dim / 2U), 2)
                 << " want=" << tc.ex(27U, 0) << "," << tc.ex(27U, 1) << "," << tc.ex(27U, 2));
        CHECK(bad == 0);
    }
}

TEST_CASE("D-007 B8-k: IR material cook seam (Forward variant renders + GBuffer variant compiles) on DX12", "[dx12][raster][gpu][ir][lighting]")
{
    namespace kir = crd::kir;
    auto        gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    crd::memory::TlsfAllocator alloc(8U << 20U);
    constexpr crd::u32         dim = 32U;

    // Forward variant — a material cooked into its forward pass renders LIT.
    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_cook_forward_fs(fg, fe);
    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    auto target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    crd::gputest::enc_draw(*raster,*target, *program, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);
    const auto ch  = [](crd::u32 px, int c) { return static_cast<int>((px >> (8 * c)) & 0xFFU); };
    int        bad = 0;
    for (crd::u32 x = 2U; x < dim - 2U; x += 5U)
    {
        const crd::u32 px = target->read_pixel(x, dim / 2U);
        for (int c = 0; c < 3; ++c) { const int want = crd::gputest::build_cook_forward_expected(x, c); if (ch(px, c) < want - 4 || ch(px, c) > want + 4) { ++bad; } }
    }
    CHECK(ch(target->read_pixel(3U, dim / 2U), 0) != ch(target->read_pixel(29U, dim / 2U), 0));
    WARN("[cook-forward dx12] col27 rgb=" << ch(target->read_pixel(27U, dim / 2U), 0) << "," << ch(target->read_pixel(27U, dim / 2U), 1) << "," << ch(target->read_pixel(27U, dim / 2U), 2)
                                          << " want=" << crd::gputest::build_cook_forward_expected(27U, 0) << "," << crd::gputest::build_cook_forward_expected(27U, 1) << "," << crd::gputest::build_cook_forward_expected(27U, 2));
    CHECK(bad == 0);

    // GBuffer (deferred) variant — cooked from the SAME material, compiles to a valid program on the same backend.
    kir::KGraph gg(&alloc);
    kir::KEntry ge;
    crd::gputest::build_cook_gbuffer_fs(gg, ge);
    auto        gfs = gctx->create_program(gg, ge);
    CHECK(gfs != nullptr);
}

TEST_CASE("D-007 B8-l: IR render paths (deferred G-buffer lighting / clustered light-cull / decal projection) on DX12", "[dx12][raster][gpu][ir][lighting]")
{
    namespace kir = crd::kir;
    auto        gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    crd::memory::TlsfAllocator alloc(8U << 20U);
    constexpr crd::u32         dim = 32U;

    const auto ch  = [](crd::u32 px, int c) { return static_cast<int>((px >> (8 * c)) & 0xFFU); };
    using fs_fn    = void (*)(kir::KGraph&, kir::KEntry&);
    using exp_fn   = int (*)(crd::u32, int);
    struct Obs { fs_fn fs; exp_fn ex; const char* tag; };
    const Obs cases[] = {{crd::gputest::build_lighting_deferred_fs, crd::gputest::build_lighting_deferred_expected, "deferred"},
                         {crd::gputest::build_lighting_cluster_fs, crd::gputest::build_lighting_cluster_expected, "cluster"},
                         {crd::gputest::build_lighting_decal_fs, crd::gputest::build_lighting_decal_expected, "decal"}};
    for (const auto& tc : cases)
    {
        kir::KGraph vg(&alloc);
        kir::KEntry ve;
        crd::gputest::build_fullscreen_vs(vg, ve);
        kir::KGraph fg(&alloc);
        kir::KEntry fe;
        tc.fs(fg, fe);
        auto vs = gctx->create_program(vg, ve);
        if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
        auto fs = gctx->create_program(fg, fe);
        REQUIRE(fs != nullptr);
        auto program = raster->create_raster_program(*vs, *fs);
        REQUIRE(program != nullptr);
        auto target = raster->create_color_target(dim, dim);
        REQUIRE(target != nullptr);
        crd::gputest::enc_draw(*raster,*target, *program, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);
        int bad = 0;
        for (crd::u32 x = 2U; x < dim - 2U; x += 5U)
        {
            const crd::u32 px = target->read_pixel(x, dim / 2U);
            for (int c = 0; c < 3; ++c) { const int want = tc.ex(x, c); if (ch(px, c) < want - 4 || ch(px, c) > want + 4) { ++bad; } }
        }
        WARN("[" << tc.tag << " dx12] col27 rgb=" << ch(target->read_pixel(27U, dim / 2U), 0) << "," << ch(target->read_pixel(27U, dim / 2U), 1) << "," << ch(target->read_pixel(27U, dim / 2U), 2)
                 << " want=" << tc.ex(27U, 0) << "," << tc.ex(27U, 1) << "," << tc.ex(27U, 2));
        CHECK(bad == 0);
    }
}

TEST_CASE("D-007 B8-m: THE CULMINATION -- skinned + textured + lit + IBL + PCF-shadowed master material on DX12", "[dx12][raster][gpu][ir][lighting]")
{
    namespace kir = crd::kir;
    auto        gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    crd::memory::TlsfAllocator alloc(8U << 20U);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_master_material_fs(fg, fe);
    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 tw = 16U;
    float              depth[tw * tw];
    crd::gputest::fill_uniform_depth(depth, tw, tw, 0.5F);
    auto dtex = raster->create_depth_texture(tw, tw, depth);
    REQUIRE(dtex != nullptr);
    constexpr crd::u32 dim    = 32U;
    auto               target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    crd::gputest::enc_draw_shadow(*raster,*target, *program, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, *dtex, 3U);

    const auto ch  = [](crd::u32 px, int c) { return static_cast<int>((px >> (8 * c)) & 0xFFU); };
    int        bad = 0;
    for (crd::u32 x = 2U; x < 13U; x += 2U)
    {
        const crd::u32 px = target->read_pixel(x, dim / 2U);
        for (int c = 0; c < 3; ++c) { const int want = crd::gputest::build_master_lit_expected(x, c); if (ch(px, c) < want - 4 || ch(px, c) > want + 4) { ++bad; } }
    }
    CHECK(bad == 0);
    const int lit_r = ch(target->read_pixel(6U, dim / 2U), 0);
    const int shd_r = ch(target->read_pixel(27U, dim / 2U), 0);
    WARN("[master dx12] lit col6=" << ch(target->read_pixel(6U, dim / 2U), 0) << "," << ch(target->read_pixel(6U, dim / 2U), 1) << "," << ch(target->read_pixel(6U, dim / 2U), 2)
                                   << " (want " << crd::gputest::build_master_lit_expected(6U, 0) << "," << crd::gputest::build_master_lit_expected(6U, 1) << "," << crd::gputest::build_master_lit_expected(6U, 2) << ") shadowed col27 R=" << shd_r);
    CHECK(shd_r < lit_r - 20); // the shadow visibly darkens the direct term
    CHECK(shd_r > 0);          // ...but the ambient floor SURVIVES in shadow
}

TEST_CASE("D-007 B12: IR screen-space lighting frontier (AO/SSILVB - SSR - SSGI - volumetrics - SSS) renders on DX12", "[dx12][raster][gpu][ir][lighting]")
{
    namespace kir = crd::kir;
    auto        gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    crd::memory::TlsfAllocator alloc(8U << 20U);
    constexpr crd::u32         dim = 32U;

    const auto ch  = [](crd::u32 px, int c) { return static_cast<int>((px >> (8 * c)) & 0xFFU); };
    using fs_fn    = void (*)(kir::KGraph&, kir::KEntry&);
    using exp_fn   = int (*)(crd::u32, int);
    struct Obs { fs_fn fs; exp_fn ex; const char* tag; };
    const Obs cases[] = {{crd::gputest::build_lighting_ssao_fs, crd::gputest::build_lighting_ssao_expected, "ssao"},
                         {crd::gputest::build_lighting_ssr_fs, crd::gputest::build_lighting_ssr_expected, "ssr"},
                         {crd::gputest::build_lighting_ssgi_fs, crd::gputest::build_lighting_ssgi_expected, "ssgi"},
                         {crd::gputest::build_lighting_volumetric_fs, crd::gputest::build_lighting_volumetric_expected, "volumetric"},
                         {crd::gputest::build_lighting_sss_fs, crd::gputest::build_lighting_sss_expected, "sss"}};
    for (const auto& tc : cases)
    {
        kir::KGraph vg(&alloc);
        kir::KEntry ve;
        crd::gputest::build_fullscreen_vs(vg, ve);
        kir::KGraph fg(&alloc);
        kir::KEntry fe;
        tc.fs(fg, fe);
        auto vs = gctx->create_program(vg, ve);
        if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
        auto fs = gctx->create_program(fg, fe);
        REQUIRE(fs != nullptr);
        auto program = raster->create_raster_program(*vs, *fs);
        REQUIRE(program != nullptr);
        auto target = raster->create_color_target(dim, dim);
        REQUIRE(target != nullptr);
        crd::gputest::enc_draw(*raster,*target, *program, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);
        int bad = 0;
        for (crd::u32 x = 2U; x < dim - 2U; x += 5U)
        {
            const crd::u32 px = target->read_pixel(x, dim / 2U);
            for (int c = 0; c < 3; ++c) { const int want = tc.ex(x, c); if (ch(px, c) < want - 4 || ch(px, c) > want + 4) { ++bad; } }
        }
        WARN("[" << tc.tag << " dx12] col27 rgb=" << ch(target->read_pixel(27U, dim / 2U), 0) << "," << ch(target->read_pixel(27U, dim / 2U), 1) << "," << ch(target->read_pixel(27U, dim / 2U), 2)
                 << " want=" << tc.ex(27U, 0) << "," << tc.ex(27U, 1) << "," << tc.ex(27U, 2));
        CHECK(bad == 0);
    }
}

TEST_CASE("D-007 B13 post: IR HDR + TAA + bloom + cinematic + finish (specAA/CA/vignette/grain/CAS) render on DX12", "[dx12][raster][gpu][ir][lighting]")
{
    namespace kir = crd::kir;
    auto        gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    crd::memory::TlsfAllocator alloc(8U << 20U);
    constexpr crd::u32         dim = 32U;

    const auto ch  = [](crd::u32 px, int c) { return static_cast<int>((px >> (8 * c)) & 0xFFU); };
    using fs_fn    = void (*)(kir::KGraph&, kir::KEntry&);
    using exp_fn   = int (*)(crd::u32, int);
    struct Obs { fs_fn fs; exp_fn ex; const char* tag; };
    const Obs cases[] = {{crd::gputest::build_lighting_hdragx_fs, crd::gputest::build_lighting_hdragx_expected, "agx"},
                         {crd::gputest::build_lighting_hdrneutral_fs, crd::gputest::build_lighting_hdrneutral_expected, "neutral"},
                         {crd::gputest::build_lighting_hdrpq_fs, crd::gputest::build_lighting_hdrpq_expected, "pq"},
                         {crd::gputest::build_lighting_taa_fs, crd::gputest::build_lighting_taa_expected, "taa"}, // B13-a temporal resolve
                         {crd::gputest::build_lighting_bloom_fs, crd::gputest::build_lighting_bloom_expected, "bloom"}, // B13-b bloom
                         {crd::gputest::build_lighting_cine_fs, crd::gputest::build_lighting_cine_expected, "cine"}, // B13-d cinematic
                         {crd::gputest::build_lighting_finish_fs, crd::gputest::build_lighting_finish_expected, "finish"}}; // B13-e finish
    for (const auto& tc : cases)
    {
        kir::KGraph vg2(&alloc);
        kir::KEntry ve2;
        crd::gputest::build_fullscreen_vs(vg2, ve2);
        kir::KGraph fg(&alloc);
        kir::KEntry fe;
        tc.fs(fg, fe);
        auto vs = gctx->create_program(vg2, ve2);
        if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
        auto fs = gctx->create_program(fg, fe);
        REQUIRE(fs != nullptr);
        auto program = raster->create_raster_program(*vs, *fs);
        REQUIRE(program != nullptr);
        auto target = raster->create_color_target(dim, dim);
        REQUIRE(target != nullptr);
        crd::gputest::enc_draw(*raster,*target, *program, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);
        int bad = 0;
        for (crd::u32 x = 2U; x < dim - 2U; x += 5U)
        {
            const crd::u32 px = target->read_pixel(x, dim / 2U);
            for (int c = 0; c < 3; ++c) { const int want = tc.ex(x, c); if (ch(px, c) < want - 4 || ch(px, c) > want + 4) { ++bad; } }
        }
        CHECK(ch(target->read_pixel(3U, dim / 2U), 0) != ch(target->read_pixel(29U, dim / 2U), 0));
        WARN("[hdr-" << tc.tag << " dx12] col27 rgb=" << ch(target->read_pixel(27U, dim / 2U), 0) << "," << ch(target->read_pixel(27U, dim / 2U), 1) << "," << ch(target->read_pixel(27U, dim / 2U), 2)
                     << " want=" << tc.ex(27U, 0) << "," << tc.ex(27U, 1) << "," << tc.ex(27U, 2));
        CHECK(bad == 0);
    }
}

TEST_CASE("D-007 B8-d: IR area light SPECULAR (LTC LUT Minv reconstruction) renders on DX12", "[dx12][raster][gpu][ir][lighting]")
{
    namespace kir = crd::kir;
    auto        gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    crd::memory::TlsfAllocator alloc(8U << 20U);
    constexpr crd::u32         dim = 32U;
    kir::KGraph                vg(&alloc);
    kir::KEntry                ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
    for (int which = 0; which < 2; ++which)
    {
        kir::KGraph fg(&alloc);
        kir::KEntry fe;
        if (which == 0) { crd::gputest::build_lighting_specular_fs(fg, fe); }
        else { crd::gputest::build_lighting_aniso_fs(fg, fe); }
        auto fs = gctx->create_program(fg, fe);
        REQUIRE(fs != nullptr);
        auto program = raster->create_raster_program(*vs, *fs);
        REQUIRE(program != nullptr);
        auto target = raster->create_color_target(dim, dim);
        REQUIRE(target != nullptr);
        crd::gputest::enc_draw(*raster,*target, *program, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);
        const auto ch = [](crd::u32 px, int c) { return static_cast<int>((px >> (8 * c)) & 0xFFU); };
        int        bad = 0;
        bool       any = false;
        for (crd::u32 x = 2U; x < dim - 2U; x += 5U)
        {
            const crd::u32 px = target->read_pixel(x, dim / 2U);
            for (int c = 0; c < 3; ++c) { const int want = (which == 0) ? crd::gputest::build_lighting_specular_expected(x, c) : crd::gputest::build_lighting_aniso_expected(x, c); if (ch(px, c) < want - 4 || ch(px, c) > want + 4) { ++bad; } }
            if (ch(px, 0) != ch(2U, 0)) { any = true; }
        }
        WARN("[area-spec dx12 which=" << which << "] col7=" << ch(target->read_pixel(7U, dim / 2U), 0) << "," << ch(target->read_pixel(7U, dim / 2U), 1) << "," << ch(target->read_pixel(7U, dim / 2U), 2));
        CHECK(bad == 0);
        CHECK(any);
    }
}

// ── GEO-7 (D-007 row 72): draw_storage_depth — the scene-geometry draw (storage pulling + a REAL depth pass) ────────
//
// Two clip-space triangles pulled from the storage buffer, NEAR first (reverse-Z: larger z), FAR second. A depthless
// draw would leave the last-drawn (far) colour at the centre; with the depth test at GreaterEqual the near triangle
// must occlude — proving depth clear + test + write all live on the storage-pull path.
namespace
{
inline void build_pull_depth_vs(crd::kir::KGraph& g, crd::kir::KEntry& ve)
{
    namespace kir = crd::kir;
    const auto sh = kir::make_shape({1});
    const auto ku = [&](crd::u32 v) { return g.constant(static_cast<double>(v), sh, kir::DType::U32); };
    const int  vid  = g.cast(g.builtin(kir::KBuiltin::VertexIndex), kir::DType::U32);
    const int  base = g.binary(kir::KOp::Mul, vid, ku(4U));
    const auto ldf  = [&](int idx) { return g.int_bits_to_float(g.cast(g.storage_load(idx), kir::DType::I32)); };
    const int  x    = ldf(base);
    const int  y    = ldf(g.binary(kir::KOp::Add, base, ku(1U)));
    const int  z    = ldf(g.binary(kir::KOp::Add, base, ku(2U)));
    const int  c    = ldf(g.binary(kir::KOp::Add, base, ku(3U)));
    ve.stage    = kir::KStage::Vertex;
    ve.position = g.vec4(x, y, z, g.constant(1.0, sh, kir::DType::F32));
    ve.n_out    = 1;
    ve.out[0]   = {c, 0, kir::Interp::Flat};
}

inline void build_pull_depth_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    const auto sh = kir::make_shape({1});
    const int  c  = g.stage_in(kir::KType::make_scalar(kir::DType::F32), 0, kir::Interp::Flat);
    const int  k1 = g.constant(1.0, sh, kir::DType::F32);
    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {g.vec4(c, g.binary(kir::KOp::Sub, k1, c), g.constant(0.0, sh, kir::DType::F32), k1), 0};
}
} // namespace

TEST_CASE("GEO-7: draw_storage_depth -- near occludes far on the storage-pull path (DX12)", "[dx12][raster][gpu][ir]")
{
    namespace kir = crd::kir;
    auto        gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    crd::memory::TlsfAllocator alloc(4U << 20U);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    build_pull_depth_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    build_pull_depth_fs(fg, fe);
    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 dim    = 64U;
    auto               target = raster->create_color_depth_target(dim, dim);
    auto               storage = raster->create_storage_buffer(6U * 4U * 4U);
    REQUIRE(target != nullptr);
    REQUIRE(storage != nullptr);

    // NEAR (z=0.8, c=1 → red) FIRST, FAR (z=0.3, c=0 → green) SECOND — draw order alone would show green
    const crd::f32 verts[24] = {-3.0F, -1.0F, 0.8F, 1.0F, 3.0F, -1.0F, 0.8F, 1.0F, 0.0F, 3.0F, 0.8F, 1.0F,
                                -3.0F, -1.0F, 0.3F, 0.0F, 3.0F, -1.0F, 0.3F, 0.0F, 0.0F, 3.0F, 0.3F, 0.0F};
    REQUIRE(raster->upload_storage(*storage, 0U, verts, sizeof(verts)));

    raster->draw_storage_depth(*target, *program, g::ClearColor{0.0F, 0.0F, 1.0F, 1.0F}, 0.0F,
                               g::DepthCompare::GreaterEqual, *storage, 6U);

    const crd::u32 centre = target->read_pixel(dim / 2U, dim / 2U);
    const crd::u32 rr     = centre & 0xFFU;
    const crd::u32 gg     = (centre >> 8U) & 0xFFU;
    WARN("[storage-depth dx12] centre r=" << rr << " g=" << gg);
    CHECK(rr > 200U); // the NEAR triangle survived the far one drawn after it — the depth test is REAL
    CHECK(gg < 50U);

    // the LOAD variant (multi-group composition): a SECOND draw of a SMALL centre triangle NEARER than red
    // (z=0.9, c=0 → green). It must WIN the centre through the frame's loaded depth (0.9 ≥ 0.8) while the rest
    // of the frame — red everywhere the big triangles covered — SURVIVES untouched: it composes, never wipes.
    const crd::f32 near_tri[12] = {-0.2F, -0.2F, 0.9F, 0.0F, 0.2F, -0.2F, 0.9F, 0.0F, 0.0F, 0.2F, 0.9F, 0.0F};
    REQUIRE(raster->upload_storage(*storage, 0U, near_tri, sizeof(near_tri)));
    raster->draw_storage_depth_load(*target, *program, g::DepthCompare::GreaterEqual, *storage, 3U);
    const crd::u32 centre2 = target->read_pixel(dim / 2U, dim / 2U);
    const crd::u32 away2   = target->read_pixel(4U, dim / 2U); // covered by the FIRST pass only
    CHECK(((centre2 >> 8U) & 0xFFU) > 200U); // the near green triangle won the loaded depth at the centre
    CHECK((away2 & 0xFFU) > 200U);           // the first pass's red SURVIVED the load draw — nothing cleared
}

// ── ⭐⭐ REN-39-A1 GATE (DX12): the scene buffer serves as its OWN index buffer. ─────────────────────────────
// The DX12 half of the Vulkan gate, with the SAME dichotomy: the probe VS renders IFF VertexIndex carries the
// index VALUES {4,5,6} fetched by the IA from byte 384 of the storage buffer — a non-indexed draw of 3 vertices
// (VertexIndex ∈ {0,1,2}) collapses every corner to the degenerate (2,2) and draws NOTHING. The draw brackets
// the buffer UNORDERED_ACCESS → INDEX_BUFFER|NPSR|PSR → UNORDERED_ACCESS (the state-walk contract).
// ⛔ STATED, NOT HIDDEN: this arm is PROCEDURAL (no storage read in the shader) — a u0 UAV read is illegal in
// the index-state combo, so the ONE-buffer pull+index proof on DX12 lands with 39-B2's read-only (t-register)
// storage seam; 39-C1's pixel-parity gate then covers it. The Vulkan gate proves the one-buffer coexistence NOW.
TEST_CASE("REN-39-A1 GATE (DX12): storage buffer binds as its own index buffer, indexed draw fetches real indices",
          "[dx12][raster][gpu][ren39][indexed]")
{
    namespace kir = crd::kir;

    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid())
    {
        WARN("no D3D12 device available; skipping");
        return;
    }
    auto raster = g::create_dx12_raster_context();
    if (raster == nullptr || !raster->valid())
    {
        WARN("no D3D12 raster device; skipping");
        return;
    }
    crd::memory::TlsfAllocator alloc(4U << 20U);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_indexed_probe_vs(vg, ve); // renders IFF VertexIndex ∈ {4,5,6} — the index-value probe
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_triangle_fs(fg, fe);
    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr)
    {
        WARN("dxc/DXIL unavailable; skipping REN-39-A1 DX12 gate");
        return;
    }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    REQUIRE(program->valid());

    // 396 bytes EXACTLY (96 record words + 3 index words) — the overrun refusal depends on the tight size
    auto sb = raster->create_storage_buffer(396U);
    REQUIRE(sb != nullptr);
    const crd::u32 idx[3] = {4U, 5U, 6U};
    REQUIRE(raster->upload_storage(*sb, 384U, static_cast<const void*>(idx), sizeof(idx)));

    constexpr crd::u32 dim = 64U;

    // ── the NON-indexed control: VertexIndex ∈ {0,1,2} → every corner at (2,2) → NOTHING renders ──
    auto miss = raster->create_color_depth_target(dim, dim);
    REQUIRE(miss != nullptr);
    raster->draw_storage_depth(*miss, *program, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 0.0F, g::DepthCompare::Always,
                               *sb, 3U);
    CHECK((miss->read_pixel(dim / 2U, dim / 2U) & 0x00FFFFFFU) == 0U);

    // ── the INDEXED draw: the IA fetches {4,5,6} from byte 384 → the triangle appears ──
    auto hit = raster->create_color_depth_target(dim, dim);
    REQUIRE(hit != nullptr);
    raster->draw_storage_indexed_depth(*hit, *program, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 0.0F,
                                       g::DepthCompare::Always, *sb, 384U, 3U, 1U, false);
    CHECK((hit->read_pixel(dim / 2U, dim / 2U) & 0xFFU) >= 250U); // red centre — the index values arrived
    CHECK((hit->read_pixel(1U, 1U) & 0x00FFFFFFU) == 0U);         // corner stays the clear

    // ── refusals keep pixels: a GREEN clear that RAN would repaint the target — prove it did not ──
    raster->draw_storage_indexed_depth(*hit, *program, g::ClearColor{0.0F, 1.0F, 0.0F, 1.0F}, 0.0F,
                                       g::DepthCompare::Always, *sb, 382U, 3U, 1U, false); // misaligned offset
    raster->draw_storage_indexed_depth(*hit, *program, g::ClearColor{0.0F, 1.0F, 0.0F, 1.0F}, 0.0F,
                                       g::DepthCompare::Always, *sb, 384U, 4U, 1U, false); // 384+16 > 396: overrun
    CHECK((hit->read_pixel(dim / 2U, dim / 2U) & 0xFFU) >= 250U); // still red — both draws were REFUSED

    // ── the FRAME-MODE arm: clear + LOAD continuation through a graph (the record_scene_indexed path,
    //    with the frame_transition state pair bracketing each draw) ──
    auto ftgt = raster->create_color_depth_target(dim, dim);
    REQUIRE(ftgt != nullptr);
    {
        auto fgraph = raster->create_frame_graph();
        REQUIRE(fgraph != nullptr);
        struct IndexedPass
        {
            g::FgImage img{};
            g::IRasterProgram* prog = nullptr;
            g::IStorageBuffer* sb = nullptr;
        } st;
        st.img = fgraph->import_target(*ftgt);
        st.prog = program.get();
        st.sb = sb.get();
        fgraph->add_pass("indexed").writes(st.img).execute(
            [](g::IFrameContext& ctx, void* user)
            {
                auto* u = static_cast<IndexedPass*>(user);
                ctx.raster().draw_storage_indexed_depth(*ctx.image(u->img), *u->prog,
                                                        g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 0.0F,
                                                        g::DepthCompare::Always, *u->sb, 384U, 3U, 1U, false);
                // the LOAD continuation — draw N>0 of a pass must not wipe draw 0
                ctx.raster().draw_storage_indexed_depth(*ctx.image(u->img), *u->prog, g::ClearColor{}, 0.0F,
                                                        g::DepthCompare::Always, *u->sb, 384U, 3U, 1U, true);
            },
            &st);
        REQUIRE(fgraph->build());
        fgraph->execute();
    }
    CHECK((ftgt->read_pixel(dim / 2U, dim / 2U) & 0xFFU) >= 250U);
    CHECK((ftgt->read_pixel(1U, 1U) & 0x00FFFFFFU) == 0U);
}

// ── ⭐⭐ REN-39-A2 GATE (DX12): INDEXED MULTI-DRAW — N indexed draws, ONE ExecuteIndirect. ───────────────────
// The DX12 half of the Vulkan A2 gate: (a) the batched frame's readback is BIT-IDENTICAL to a frame of classic
// per-draw indexed calls, and (b) `multi_batch_count()` advanced by EXACTLY ONE. The probe VS is PROCEDURAL
// (the 39-A1 rule — no storage read in index state before the 39-B2 read-only seam) with two index ranges:
// {4,5,6} → the big centred triangle, {8,9,10} → the left-edge spike, both probed on the horizontal MIDLINE
// (orientation-invariant — the F16 y-flip lesson). Different `first_index` per command proves the routing.
TEST_CASE("REN-39-A2 GATE (DX12): indexed multi-draw batches N indexed draws into ONE ExecuteIndirect",
          "[dx12][raster][gpu][ren39][indexed][multidraw]")
{
    namespace kir = crd::kir;

    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid())
    {
        WARN("no D3D12 device available; skipping");
        return;
    }
    auto raster = g::create_dx12_raster_context();
    if (raster == nullptr || !raster->valid())
    {
        WARN("no D3D12 raster device; skipping");
        return;
    }
    crd::memory::TlsfAllocator alloc(4U << 20U);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_indexed_probe2_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_triangle_fs(fg, fe);
    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr)
    {
        WARN("dxc/DXIL unavailable; skipping REN-39-A2 DX12 gate");
        return;
    }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    REQUIRE(program->valid());

    // the 6-index section {4,5,6, 8,9,10} at byte 384 (records unused by the procedural VS) = 408 bytes
    auto sb = raster->create_storage_buffer(408U);
    REQUIRE(sb != nullptr);
    const crd::u32 idx[6] = {4U, 5U, 6U, 8U, 9U, 10U};
    REQUIRE(raster->upload_storage(*sb, 384U, static_cast<const void*>(idx), sizeof(idx)));

    constexpr crd::u32 dim = 64U;
    struct MultiIdxState
    {
        g::FgImage img{};
        g::IRasterProgram* prog = nullptr;
        g::IStorageBuffer* sb = nullptr;
        const g::IRasterContext::IndexedDraw* draws = nullptr;
    };
    const g::IRasterContext::IndexedDraw draws[2] = {{3U, 1U, 0U}, {3U, 1U, 3U}};

    // ── the CLASSIC reference: a frame of per-draw indexed calls through the graph ──
    auto ref = raster->create_color_depth_target(dim, dim);
    REQUIRE(ref != nullptr);
    {
        auto fgraph = raster->create_frame_graph();
        REQUIRE(fgraph != nullptr);
        MultiIdxState st;
        st.img = fgraph->import_target(*ref);
        st.prog = program.get();
        st.sb = sb.get();
        fgraph->add_pass("classic").writes(st.img).execute(
            [](g::IFrameContext& ctx, void* user)
            {
                auto* u = static_cast<MultiIdxState*>(user);
                ctx.raster().draw_storage_indexed_depth(*ctx.image(u->img), *u->prog,
                                                        g::ClearColor{0.05F, 0.0F, 0.0F, 1.0F}, 0.0F,
                                                        g::DepthCompare::Always, *u->sb, 384U, 3U, 1U, false);
                ctx.raster().draw_storage_indexed_depth(*ctx.image(u->img), *u->prog, g::ClearColor{}, 0.0F,
                                                        g::DepthCompare::Always, *u->sb, 384U + 12U, 3U, 1U, true);
            },
            &st);
        REQUIRE(fgraph->build());
        fgraph->execute();
    }

    // ── the BATCHED frame: ONE indexed multi-draw over the same two commands ──
    auto tgt = raster->create_color_depth_target(dim, dim);
    REQUIRE(tgt != nullptr);
    const crd::u64 batches_before = raster->multi_batch_count();
    {
        auto fgraph = raster->create_frame_graph();
        REQUIRE(fgraph != nullptr);
        MultiIdxState st;
        st.img = fgraph->import_target(*tgt);
        st.prog = program.get();
        st.sb = sb.get();
        st.draws = static_cast<const g::IRasterContext::IndexedDraw*>(draws);
        fgraph->add_pass("batched").writes(st.img).execute(
            [](g::IFrameContext& ctx, void* user)
            {
                auto* u = static_cast<MultiIdxState*>(user);
                ctx.raster().draw_storage_multi_indexed_depth(
                    *ctx.image(u->img), *u->prog, g::ClearColor{0.05F, 0.0F, 0.0F, 1.0F}, 0.0F, g::DepthCompare::Always,
                    *u->sb, 384U, u->draws, 2U, 0U, false);
            },
            &st);
        REQUIRE(fgraph->build());
        fgraph->execute();
    }
    // (b) EXACTLY one batch was recorded — the count assert that "looped" cannot fake
    CHECK(raster->multi_batch_count() == batches_before + 1U);

    // both commands' geometry present, MIDLINE probes: centre = big triangle, left edge = spike, right = clear
    CHECK((tgt->read_pixel(dim / 2U, dim / 2U) & 0xFFU) >= 250U);
    CHECK((tgt->read_pixel(8U, dim / 2U) & 0xFFU) >= 250U);
    CHECK((tgt->read_pixel(60U, dim / 2U) & 0xFFU) <= 20U);

    // (a) bit-identical pixels, over a frame that actually drew
    crd::u32 diffs = 0U;
    crd::u32 covered = 0U;
    for (crd::u32 y = 0; y < dim; ++y)
    {
        for (crd::u32 x = 0; x < dim; ++x)
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
}

// ── ⭐⭐ REN-39-B1 GATE (DX12): InstanceIndex — the SAME columns from the SAME asset as the Vulkan gate. ─────
// SV_InstanceID excludes StartInstanceLocation; the verb always draws with StartInstance = 0, so the sequence
// here must equal Vulkan's gl_InstanceIndex sequence EXACTLY — identical probe asserts are the proof.
TEST_CASE("REN-39-B1 GATE (DX12): InstanceIndex sequences instances 0..N-1 through the indexed draw",
          "[dx12][raster][gpu][ren39][indexed][instance]")
{
    namespace kir = crd::kir;

    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid())
    {
        WARN("no D3D12 device available; skipping");
        return;
    }
    auto raster = g::create_dx12_raster_context();
    if (raster == nullptr || !raster->valid())
    {
        WARN("no D3D12 raster device; skipping");
        return;
    }
    crd::memory::TlsfAllocator alloc(4U << 20U);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_indexed_instance_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_triangle_fs(fg, fe);
    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr)
    {
        WARN("dxc/DXIL unavailable; skipping REN-39-B1 DX12 gate");
        return;
    }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    REQUIRE(program->valid());

    auto sb = raster->create_storage_buffer(396U);
    REQUIRE(sb != nullptr);
    const crd::u32 idx[3] = {4U, 5U, 6U};
    REQUIRE(raster->upload_storage(*sb, 384U, static_cast<const void*>(idx), sizeof(idx)));

    auto four = raster->create_color_depth_target(64U, 64U);
    REQUIRE(four != nullptr);
    raster->draw_storage_indexed_depth(*four, *program, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 0.0F,
                                       g::DepthCompare::Always, *sb, 384U, 3U, 4U, false);
    CHECK((four->read_pixel(8U, 32U) & 0xFFU) >= 250U);      // instance 0
    CHECK((four->read_pixel(24U, 32U) & 0xFFU) >= 250U);     // instance 1
    CHECK((four->read_pixel(40U, 32U) & 0xFFU) >= 250U);     // instance 2
    CHECK((four->read_pixel(56U, 32U) & 0xFFU) >= 250U);     // instance 3
    CHECK((four->read_pixel(16U, 32U) & 0x00FFFFFFU) == 0U); // between columns — the clear

    auto two = raster->create_color_depth_target(64U, 64U);
    REQUIRE(two != nullptr);
    raster->draw_storage_indexed_depth(*two, *program, g::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 0.0F,
                                       g::DepthCompare::Always, *sb, 384U, 3U, 2U, false);
    CHECK((two->read_pixel(8U, 32U) & 0xFFU) >= 250U);
    CHECK((two->read_pixel(24U, 32U) & 0xFFU) >= 250U);
    CHECK((two->read_pixel(40U, 32U) & 0x00FFFFFFU) == 0U);
    CHECK((two->read_pixel(56U, 32U) & 0x00FFFFFFU) == 0U);
}
