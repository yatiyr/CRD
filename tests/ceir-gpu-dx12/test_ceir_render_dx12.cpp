// CEIR-14z-3 (DX12) — the DirectX 12 mirror of the FIRST render DEVICE pixel proof. The SAME shared CEIR
// `render.scope { render.draw }` is lowered by 14z and executed through `execute_render_lowered` onto a real DX12 raster
// `ICommandEncoder` (ADR-0126 / Option A test-surface), drawing the SHARED CKIR triangle: the centre texel reads RED
// (inside the IR-authored triangle), a corner reads BLUE (the scope's clear). ⛔ Windows-only + device-guarded (WARN-skip:
// no D3D12 device / no dxc). The device-free lower-shape guard lives in the Vulkan target (it builds on Linux). One CEIR
// render program → BOTH backends (ADR-0101).

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/render.hpp> // RenderMisuseKind

#include <crd/gpu/dx12_context.hpp>
#include <crd/gpu/dx12_raster_context.hpp>

#include <crd/kir/ckir.hpp>

#include <crd/memory/allocators/malloc_allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include "../gpu-shared/ceir_render_triangle.hpp" // the SHARED CEIR render program + the executor wrapper
#include "../gpu-shared/ckir_raster_triangle.hpp" // the SHARED CKIR triangle geometry (build_triangle_vs/fs) + build_gbuffer_two_output_fs
#include "../gpu-shared/ckir_vertex_pull.hpp"     // CEIR-14z-4c: build_vertex_pull_vs (the storage-pull VS for the MRT proof)

#include <catch2/catch_test_macros.hpp>

namespace ce  = crd::ceir;
namespace ceg = crd::ceir::gpu;
namespace cgt = crd::ceir_gpu_test;
namespace g   = crd::gpu;

TEST_CASE("ceir 14z-3: the CEIR triangle renders RED on a DX12 raster encoder (blue clear)",
          "[ceir][ceir-gpu][render][dx12][gpu]")
{
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);

    crd::memory::TlsfAllocator alloc(8U << 20U);
    // the SHARED CKIR triangle (the SAME graph the Vulkan proof draws) -> DX12 programs.
    crd::kir::KGraph vg(&alloc);
    crd::kir::KEntry ve;
    crd::gputest::build_triangle_vs(vg, ve);
    crd::kir::KGraph fg(&alloc);
    crd::kir::KEntry fe;
    crd::gputest::build_triangle_fs(fg, fe);
    auto vs = gctx->create_program(vg, ve); // KIR -> HLSL -> DXIL, behind the seam
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping the device draw"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    REQUIRE(program->valid());

    constexpr crd::u32 dim    = 32U;
    auto               target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);

    // build the CEIR render program (its resolvers return the REAL device target + program).
    crd::memory::MallocAllocator croot;
    ce::Context                  cctx(&croot);
    const cgt::CeirRenderProgram rp = cgt::build_ceir_triangle_render(cctx, dim);
    CHECK(cctx.find_render_misuse(*rp.module).kind == ce::RenderMisuseKind::None);

    {
        auto enc = raster->create_command_encoder();
        const ceg::ExecuteError err = cgt::run_ceir_render(cctx, *rp.body, croot, *enc, target.get(), program.get());
        CHECK(err == ceg::ExecuteError::None);
    } // enc destroyed -> submit+wait completes before read_pixel

    const crd::u32 centre = target->read_pixel(dim / 2U, dim / 2U);
    const crd::u32 corner = target->read_pixel(0U, 0U);
    CHECK((centre & 0xFFU) >= 250U);          // R high  => red (inside the IR-authored triangle)
    CHECK(((centre >> 16U) & 0xFFU) <= 5U);   // B low
    CHECK((corner & 0xFFU) <= 5U);            // R low   => blue clear (outside)
    CHECK(((corner >> 16U) & 0xFFU) >= 250U); // B high
}

// CEIR-14z-4a (DX12): the GOLD-STANDARD frame-recording drive — the SAME CEIR triangle through execute_render_frame (the
// Dx12FrameGraph, one pass per render.scope) instead of a standalone synchronous encoder. centre RED, corner BLUE.
TEST_CASE("ceir 14z-4a: the CEIR triangle renders RED through the DX12 FRAME GRAPH (frame-recording drive)",
          "[ceir][ceir-gpu][render][dx12][gpu]")
{
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    if (raster->create_frame_graph() == nullptr) { WARN("no frame graph on this raster context; skipping"); return; }

    crd::memory::TlsfAllocator alloc(8U << 20U);
    crd::kir::KGraph           vg(&alloc);
    crd::kir::KEntry           ve;
    crd::gputest::build_triangle_vs(vg, ve);
    crd::kir::KGraph fg(&alloc);
    crd::kir::KEntry fe;
    crd::gputest::build_triangle_fs(fg, fe);
    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping the device draw"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    REQUIRE(program->valid());

    constexpr crd::u32 dim    = 32U;
    auto               target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);

    crd::memory::MallocAllocator croot;
    ce::Context                  cctx(&croot);
    const cgt::CeirRenderProgram rp = cgt::build_ceir_triangle_render(cctx, dim);
    CHECK(cctx.find_render_misuse(*rp.module).kind == ce::RenderMisuseKind::None);

    const ceg::ExecuteError err = cgt::run_ceir_render_frame(cctx, *rp.body, croot, *raster, target.get(), program.get());
    CHECK(err == ceg::ExecuteError::None); // ⭐ the DX12 frame graph drove the CEIR draw + owns the readback

    const crd::u32 centre = target->read_pixel(dim / 2U, dim / 2U);
    const crd::u32 corner = target->read_pixel(0U, 0U);
    CHECK((centre & 0xFFU) >= 250U);          // R high  => red (inside the IR-authored triangle)
    CHECK(((centre >> 16U) & 0xFFU) <= 5U);   // B low
    CHECK((corner & 0xFFU) <= 5U);            // R low   => blue clear (outside)
    CHECK(((corner >> 16U) & 0xFFU) >= 250U); // B high
}

// CEIR-14z-4c(c1) (DX12): a TWO-SCOPE CEIR program through the Dx12FrameGraph — exercises execute_render_frame's N-scope
// path (reserve-then-push closures + scope-splitting walk + TWO passes into TWO targets). Scope 0 clears BLUE, scope 1
// clears GREEN; the DISTINCT corners prove each scope routed to its OWN target with its OWN clear.
TEST_CASE("ceir 14z-4c: a TWO-SCOPE CEIR program renders into TWO targets through the DX12 frame graph",
          "[ceir][ceir-gpu][render][dx12][gpu]")
{
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    if (raster->create_frame_graph() == nullptr) { WARN("no frame graph on this raster context; skipping"); return; }

    crd::memory::TlsfAllocator alloc(8U << 20U);
    crd::kir::KGraph           vg(&alloc);
    crd::kir::KEntry           ve;
    crd::gputest::build_triangle_vs(vg, ve);
    crd::kir::KGraph fg(&alloc);
    crd::kir::KEntry fe;
    crd::gputest::build_triangle_fs(fg, fe);
    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping the device draw"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 dim = 32U;
    auto               target_a  = raster->create_color_target(dim, dim); // scope 0 (BLUE clear)
    auto               target_b  = raster->create_color_target(dim, dim); // scope 1 (GREEN clear)
    REQUIRE(target_a != nullptr);
    REQUIRE(target_b != nullptr);

    crd::memory::MallocAllocator   croot;
    ce::Context                    cctx(&croot);
    const cgt::CeirTwoScopeProgram rp = cgt::build_ceir_two_scope_render(cctx, dim);
    CHECK(cctx.find_render_misuse(*rp.module).kind == ce::RenderMisuseKind::None);
    cgt::CeirTargetMap map;
    map.add(rp.att0, target_a.get());
    map.add(rp.att1, target_b.get());

    const ceg::ExecuteError err2 = cgt::run_ceir_render_frame_mapped(cctx, *rp.body, croot, *raster, map, program.get());
    CHECK(err2 == ceg::ExecuteError::None); // ⭐ TWO frame-graph passes, one per scope, into two targets

    const crd::u32 a_centre = target_a->read_pixel(dim / 2U, dim / 2U);
    const crd::u32 a_corner = target_a->read_pixel(0U, 0U);
    const crd::u32 b_centre = target_b->read_pixel(dim / 2U, dim / 2U);
    const crd::u32 b_corner = target_b->read_pixel(0U, 0U);
    CHECK((a_centre & 0xFFU) >= 250U);          // target_a centre RED (the triangle)
    CHECK(((a_corner >> 16U) & 0xFFU) >= 250U); // target_a corner BLUE (scope 0 clear)
    CHECK((a_corner & 0xFFU) < 8U);             // target_a corner red low
    CHECK((b_centre & 0xFFU) >= 250U);          // target_b centre RED (the triangle)
    CHECK(((b_corner >> 8U) & 0xFFU) >= 250U);  // target_b corner GREEN (scope 1 clear)
    CHECK((b_corner & 0xFFU) < 8U);             // target_b corner red low
    CHECK(((b_corner >> 16U) & 0xFFU) < 8U);    // target_b corner blue low — DISTINCT from target_a (each scope its own target + clear)
}

// CEIR-14z-4c(c1) (DX12): the CEIR→MRT device proof — a CEIR render.scope{2 color attachments, distinct clears BLUE@0/RED@1}
// + a StoragePull draw binding a vertex buffer, through the Dx12FrameGraph. The FIRST CEIR program to drive draw_storage_mrt
// on DX12: exercises the binding resolver + per-attachment typed clears. color0 centre RED/corner BLUE, color1 centre
// GREEN/corner RED (distinct corners ⇒ per-attachment clears; distinct centres ⇒ correct MRT ordering).
TEST_CASE("ceir 14z-4c: a CEIR MRT program drives draw_storage_mrt RED@0/GREEN@1 with distinct clears (DX12 frame graph)",
          "[ceir][ceir-gpu][render][dx12][gpu]")
{
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    if (raster->create_frame_graph() == nullptr) { WARN("no frame graph on this raster context; skipping"); return; }

    crd::memory::TlsfAllocator alloc(8U << 20U);
    crd::kir::KGraph           vg(&alloc);
    crd::kir::KEntry           ve;
    crd::gputest::build_vertex_pull_vs(vg, ve);
    crd::kir::KGraph fg(&alloc);
    crd::kir::KEntry fe;
    crd::gputest::build_gbuffer_two_output_fs(fg, fe); // RED@0 / GREEN@1
    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping the device draw"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 dim      = 32U;
    auto               target_a = raster->create_color_target(dim, dim); // color 0 (BLUE clear, RED output)
    auto               target_b = raster->create_color_target(dim, dim); // color 1 (RED clear, GREEN output)
    REQUIRE(target_a != nullptr);
    REQUIRE(target_b != nullptr);

    float verts[36] = {0.0F};
    verts[0]  = 0.0F;
    verts[1]  = -0.8F;
    verts[12] = 0.8F;
    verts[13] = 0.8F;
    verts[24] = -0.8F;
    verts[25] = 0.8F;
    auto geo = raster->create_storage_buffer(static_cast<crd::u32>(sizeof(verts)));
    REQUIRE(geo != nullptr);
    REQUIRE(raster->upload_storage(*geo, 0U, verts, static_cast<crd::u32>(sizeof(verts))));

    crd::memory::MallocAllocator croot;
    ce::Context                  cctx(&croot);
    const cgt::CeirMrtProgram    rp = cgt::build_ceir_mrt_render(cctx, dim);
    CHECK(cctx.find_render_misuse(*rp.module).kind == ce::RenderMisuseKind::None);
    cgt::CeirTargetMap map;
    map.add(rp.att0, target_a.get());
    map.add(rp.att1, target_b.get());

    const ceg::ExecuteError err = cgt::run_ceir_render_frame_mrt(cctx, *rp.body, croot, *raster, map, program.get(), geo.get());
    CHECK(err == ceg::ExecuteError::None); // ⭐ the CEIR MRT program drove draw_storage_mrt through the DX12 frame graph

    const crd::u32 a_centre = target_a->read_pixel(dim / 2U, dim / 2U);
    const crd::u32 a_corner = target_a->read_pixel(0U, 0U);
    const crd::u32 b_centre = target_b->read_pixel(dim / 2U, dim / 2U);
    const crd::u32 b_corner = target_b->read_pixel(0U, 0U);
    CHECK((a_centre & 0xFFU) >= 250U);          // color 0 centre RED (FS out0)
    CHECK(((a_centre >> 8U) & 0xFFU) < 8U);     // green low
    CHECK(((a_corner >> 16U) & 0xFFU) >= 250U); // color 0 corner BLUE (att0 clear)
    CHECK((a_corner & 0xFFU) < 8U);             // red low
    CHECK(((b_centre >> 8U) & 0xFFU) >= 250U);  // color 1 centre GREEN (FS out1 — correct MRT ordering)
    CHECK((b_centre & 0xFFU) < 8U);             // red low
    CHECK((b_corner & 0xFFU) >= 250U);          // color 1 corner RED (att1 clear)
    CHECK(((b_corner >> 8U) & 0xFFU) < 8U);     // green low
    CHECK(((b_corner >> 16U) & 0xFFU) < 8U);    // blue low — DISTINCT from color 0's clear (per-attachment clears)
}

// CEIR-14z-4c(c2) (DX12): the uint-HOMOGENEOUS MRT proof — TWO R32_UINT attachments with DISTINCT uint clears (100/200) +
// a uint 2-output FS (ids 7/9). ⭐ the FIRST DX12 non-zero uint clear (draw_visbuffer only ever clears to 0): if the
// draw_storage_mrt uint arm's ClearRenderTargetView reinterpret is correct, the corners read 100/200.
TEST_CASE("ceir 14z-4c: a CEIR uint MRT program drives distinct uint clears + ids through the DX12 frame graph",
          "[ceir][ceir-gpu][render][dx12][gpu]")
{
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    if (raster->create_frame_graph() == nullptr) { WARN("no frame graph on this raster context; skipping"); return; }

    crd::memory::TlsfAllocator alloc(8U << 20U);
    crd::kir::KGraph           vg(&alloc);
    crd::kir::KEntry           ve;
    crd::gputest::build_vertex_pull_vs(vg, ve);
    crd::kir::KGraph fg(&alloc);
    crd::kir::KEntry fe;
    crd::gputest::build_visbuffer_two_output_fs(fg, fe); // id 7 @0 / id 9 @1
    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping the device draw"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 dim      = 32U;
    auto               target_a = raster->create_visbuffer_target(dim, dim); // R32_UINT color 0 (clear 100, id 7)
    auto               target_b = raster->create_visbuffer_target(dim, dim); // R32_UINT color 1 (clear 200, id 9)
    REQUIRE(target_a != nullptr);
    REQUIRE(target_b != nullptr);

    float verts[36] = {0.0F};
    verts[0]  = 0.0F;
    verts[1]  = -0.8F;
    verts[12] = 0.8F;
    verts[13] = 0.8F;
    verts[24] = -0.8F;
    verts[25] = 0.8F;
    auto geo = raster->create_storage_buffer(static_cast<crd::u32>(sizeof(verts)));
    REQUIRE(geo != nullptr);
    REQUIRE(raster->upload_storage(*geo, 0U, verts, static_cast<crd::u32>(sizeof(verts))));

    crd::memory::MallocAllocator croot;
    ce::Context                  cctx(&croot);
    const cgt::CeirMrtProgram    rp = cgt::build_ceir_mrt_uint_render(cctx, dim, 100U, 200U);
    CHECK(cctx.find_render_misuse(*rp.module).kind == ce::RenderMisuseKind::None);
    cgt::CeirTargetMap map;
    map.add(rp.att0, target_a.get());
    map.add(rp.att1, target_b.get());

    const ceg::ExecuteError err = cgt::run_ceir_render_frame_mrt(cctx, *rp.body, croot, *raster, map, program.get(), geo.get());
    CHECK(err == ceg::ExecuteError::None);

    CHECK(target_a->read_pixel(dim / 2U, dim / 2U) == 7U);   // color 0 centre = uint id 7 (FS out0)
    CHECK(target_a->read_pixel(0U, 0U) == 100U);             // color 0 corner = uint clear 100 (per-attachment typed clear)
    CHECK(target_b->read_pixel(dim / 2U, dim / 2U) == 9U);   // color 1 centre = uint id 9 (FS out1 — MRT ordering)
    CHECK(target_b->read_pixel(0U, 0U) == 200U);             // color 1 corner = uint clear 200 (DISTINCT ⇒ per-attachment clears)
}

// CEIR-14z-4c(c3) (DX12): the HETEROGENEOUS (mixed-type) MRT proof — color0 R32_UINT (uint clear 100, id 7) + color1 RGBA8
// (float clear BLUE, GREEN) in ONE scope. ⭐ this exercises the c3 DX12 engine fix: pso_for's PER-ATTACHMENT RTV formats
// (a mixed-format PSO) + the format array in the cache identity. Without the fix, the RGBA8 attachment would get color0's
// R32_UINT format in the PSO ⇒ a malformed pipeline. color0 (uint) reads 7/100; color1 (float) reads GREEN/BLUE.
TEST_CASE("ceir 14z-4c: a CEIR MIXED uint+float MRT program renders through the DX12 frame graph (heterogeneous formats)",
          "[ceir][ceir-gpu][render][dx12][gpu]")
{
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    if (raster->create_frame_graph() == nullptr) { WARN("no frame graph on this raster context; skipping"); return; }

    crd::memory::TlsfAllocator alloc(8U << 20U);
    crd::kir::KGraph           vg(&alloc);
    crd::kir::KEntry           ve;
    crd::gputest::build_vertex_pull_vs(vg, ve);
    crd::kir::KGraph fg(&alloc);
    crd::kir::KEntry fe;
    crd::gputest::build_gbuffer_uint_float_fs(fg, fe); // uint id 7 @0 / GREEN @1
    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping the device draw"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 dim      = 32U;
    auto               target_a = raster->create_visbuffer_target(dim, dim); // color 0: R32_UINT (uint clear 100, id 7)
    auto               target_b = raster->create_color_target(dim, dim);     // color 1: RGBA8 (float clear BLUE, GREEN)
    REQUIRE(target_a != nullptr);
    REQUIRE(target_b != nullptr);

    float verts[36] = {0.0F};
    verts[0]  = 0.0F;
    verts[1]  = -0.8F;
    verts[12] = 0.8F;
    verts[13] = 0.8F;
    verts[24] = -0.8F;
    verts[25] = 0.8F;
    auto geo = raster->create_storage_buffer(static_cast<crd::u32>(sizeof(verts)));
    REQUIRE(geo != nullptr);
    REQUIRE(raster->upload_storage(*geo, 0U, verts, static_cast<crd::u32>(sizeof(verts))));

    crd::memory::MallocAllocator croot;
    ce::Context                  cctx(&croot);
    const cgt::CeirMrtProgram    rp = cgt::build_ceir_mrt_mixed_render(cctx, dim, 100U);
    CHECK(cctx.find_render_misuse(*rp.module).kind == ce::RenderMisuseKind::None);
    cgt::CeirTargetMap map;
    map.add(rp.att0, target_a.get());
    map.add(rp.att1, target_b.get());

    const ceg::ExecuteError err = cgt::run_ceir_render_frame_mrt(cctx, *rp.body, croot, *raster, map, program.get(), geo.get());
    CHECK(err == ceg::ExecuteError::None);

    CHECK(target_a->read_pixel(dim / 2U, dim / 2U) == 7U);   // color 0 (uint) centre = id 7 (FS out0)
    CHECK(target_a->read_pixel(0U, 0U) == 100U);             // color 0 (uint) corner = uint clear 100
    const crd::u32 b_centre = target_b->read_pixel(dim / 2U, dim / 2U);
    const crd::u32 b_corner = target_b->read_pixel(0U, 0U);
    CHECK(((b_centre >> 8U) & 0xFFU) >= 250U);  // color 1 (float) centre GREEN (FS out1)
    CHECK((b_centre & 0xFFU) < 8U);             // red low
    CHECK(((b_corner >> 16U) & 0xFFU) >= 250U); // color 1 (float) corner BLUE (float clear)
    CHECK((b_corner & 0xFFU) < 8U);             // red low
}

// CEIR-14z-5 (DX12): the DEPTH-ONLY proof by DEPTH-TEST OCCLUSION (attachment-only observation — no sampling). Scope 0 is a
// genuine DEPTH-ONLY pass (draw_storage_depth_only, an n_out=0 program — the ⛔ depth-only-≠-forward scar honored by
// construction): the shared StoragePull triangle writes depth 0.5 under it, clear 1.0 elsewhere, into T's depth buffer. Scope 1
// LOADS that depth and fullscreen-tests frag_depth 0.75 (LessEqual): centre (stored 0.5) FAILS ⇒ BLUE clear survives; corner
// (stored 1.0) PASSES ⇒ RED. The colour readback IS the proof scope 0 wrote real depth. (No ValidationCapture on DX12 — the
// gpu-context-dx12 info-queue capture is a 13z named-forward.)
TEST_CASE("ceir 14z-5: a CEIR depth-only pass renders depth; a later scope occludes against it (DX12 frame graph)",
          "[ceir][ceir-gpu][render][dx12][gpu]")
{
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    if (raster->create_frame_graph() == nullptr) { WARN("no frame graph on this raster context; skipping"); return; }

    crd::memory::TlsfAllocator alloc(8U << 20U);
    // scope-0 program: the storage-pull triangle VS + a DEPTH-ONLY FS (n_out = 0, frag_depth = 0.5).
    crd::kir::KGraph dvg(&alloc);
    crd::kir::KEntry dve;
    crd::gputest::build_vertex_pull_vs(dvg, dve);
    crd::kir::KGraph dfg(&alloc);
    crd::kir::KEntry dfe;
    crd::gputest::build_depth_only_const_fs(dfg, dfe, 0.5);
    auto dvs = gctx->create_program(dvg, dve);
    if (dvs == nullptr) { WARN("dxc/DXIL unavailable; skipping the device draw"); return; }
    auto dfs = gctx->create_program(dfg, dfe);
    REQUIRE(dfs != nullptr);
    auto depth_prog = raster->create_raster_program(*dvs, *dfs);
    REQUIRE(depth_prog != nullptr);
    // scope-1 program: a fullscreen VS + a COLOUR+DEPTH FS (n_out = 1 RED, frag_depth = 0.75).
    crd::kir::KGraph cvg(&alloc);
    crd::kir::KEntry cve;
    crd::gputest::build_fullscreen_vs(cvg, cve);
    crd::kir::KGraph cfg(&alloc);
    crd::kir::KEntry cfe;
    crd::gputest::build_color_depth_fs(cfg, cfe, 1.0, 0.0, 0.0, 0.75); // RED, frag_depth 0.75
    auto cvs = gctx->create_program(cvg, cve);
    REQUIRE(cvs != nullptr);
    auto cfs = gctx->create_program(cfg, cfe);
    REQUIRE(cfs != nullptr);
    auto cd_prog = raster->create_raster_program(*cvs, *cfs);
    REQUIRE(cd_prog != nullptr);

    constexpr crd::u32 dim = 32U;
    auto               target = raster->create_color_depth_target(dim, dim); // ONE colour+depth target (has_depth())
    REQUIRE(target != nullptr);

    float verts[36] = {0.0F};
    verts[0]  = 0.0F;
    verts[1]  = -0.8F;
    verts[12] = 0.8F;
    verts[13] = 0.8F;
    verts[24] = -0.8F;
    verts[25] = 0.8F;
    auto geo = raster->create_storage_buffer(static_cast<crd::u32>(sizeof(verts)));
    REQUIRE(geo != nullptr);
    REQUIRE(raster->upload_storage(*geo, 0U, verts, static_cast<crd::u32>(sizeof(verts))));

    crd::memory::MallocAllocator croot;
    ce::Context                  cctx(&croot);
    const cgt::CeirDepthProgram  rp = cgt::build_ceir_depth_occlusion_render(cctx, dim);
    CHECK(cctx.find_render_misuse(*rp.module).kind == ce::RenderMisuseKind::None);
    cgt::CeirProgramMap progs;
    progs.add(rp.draw0, depth_prog.get()); // scope 0 → the depth-only program
    progs.add(rp.draw1, cd_prog.get());    // scope 1 → the colour+depth program

    const ceg::ExecuteError err =
        cgt::run_ceir_render_frame_depth(cctx, *rp.body, croot, *raster, target.get(), progs, geo.get());
    CHECK(err == ceg::ExecuteError::None); // ⭐ the CEIR depth-only pass drove draw_storage_depth_only through the frame graph

    const crd::u32 centre = target->read_pixel(dim / 2U, dim / 2U); // under the triangle: stored depth 0.5, 0.75 FAILS
    const crd::u32 corner = target->read_pixel(0U, 0U);             // outside: stored depth 1.0, 0.75 PASSES
    CHECK(((centre >> 16U) & 0xFFU) >= 250U); // centre BLUE (the clear survives ⇒ 0.75 > 0.5, the depth-only pass wrote 0.5)
    CHECK((centre & 0xFFU) < 8U);             // centre red low
    CHECK((corner & 0xFFU) >= 250U);          // corner RED (0.75 <= 1.0 ⇒ the fragment passed + wrote)
    CHECK(((corner >> 16U) & 0xFFU) < 8U);    // corner blue low
}

// CEIR-14z-6(c1) (DX12): INDEXED-INDIRECT with the DrawIndex-PUSH proof. A CEIR render.draw_indirect(%args, %vbuf) {max_draws=2}
// drives draw_storage_multi_indexed_indirect: TWO sub-draws from an args buffer of TWO IDENTICAL commands. The VS shifts X by
// the pushed SV_DrawIndex ⇒ draw 0 → LEFT, draw 1 → RIGHT. On DX12 the DrawIndex arrives as ExecuteIndirect's leading root
// constant (the args buffer's 24-byte stride, arg_off 4, the leading u32 = i). If the row isn't pushed, both land LEFT ⇒
// right-centre reads the BLUE clear. The FIRST CEIR multi-buffer draw (%args + %vbuf from DISTINCT operands). No ValidationCapture
// on DX12 (a 13z named-forward).
TEST_CASE("ceir 14z-6: a CEIR indexed-indirect draw pushes the per-sub-draw DrawIndex row (DX12 frame graph)",
          "[ceir][ceir-gpu][render][dx12][gpu]")
{
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    if (raster->create_frame_graph() == nullptr) { WARN("no frame graph on this raster context; skipping"); return; }

    crd::memory::TlsfAllocator alloc(8U << 20U);
    crd::kir::KGraph           vg(&alloc);
    crd::kir::KEntry           ve;
    crd::gputest::build_vertex_pull_drawindex_vs(vg, ve); // pull + X-shift by the pushed SV_DrawIndex
    crd::kir::KGraph fg(&alloc);
    crd::kir::KEntry fe;
    crd::gputest::build_triangle_fs(fg, fe); // RED
    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping the device draw"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 dim    = 64U;
    auto               target = raster->create_color_depth_target(dim, dim);
    REQUIRE(target != nullptr);

    constexpr crd::u32 index_offset = 144U; // 3 verts x 48 bytes
    float              verts[36]    = {0.0F};
    verts[0]  = -0.5F;
    verts[1]  = -0.4F;
    verts[12] = -0.1F;
    verts[13] = 0.4F;
    verts[24] = -0.9F;
    verts[25] = 0.4F;
    auto geo = raster->create_storage_buffer(160U);
    REQUIRE(geo != nullptr);
    REQUIRE(raster->upload_storage(*geo, 0U, verts, static_cast<crd::u32>(sizeof(verts))));
    const crd::u32 indices[3] = {0U, 1U, 2U};
    REQUIRE(raster->upload_storage(*geo, index_offset, indices, static_cast<crd::u32>(sizeof(indices))));

    const crd::u32 stride   = raster->indirect_command_stride();      // 24 (DX12)
    const crd::u32 arg_off  = raster->indirect_command_arg_offset();  // 4 (DX12: leading u32 = DrawIndex)
    const crd::u32 stride_w = stride / 4U;
    const crd::u32 argw     = arg_off / 4U;
    crd::u32       args_words[12] = {0U};
    for (crd::u32 i = 0; i < 2U; ++i)
    {
        if (argw != 0U) { args_words[i * stride_w] = i; } // DX12: the per-command DrawIndex root constant
        args_words[i * stride_w + argw + 0U] = 3U;        // index_count
        args_words[i * stride_w + argw + 1U] = 1U;        // instance_count
        args_words[i * stride_w + argw + 2U] = 0U;        // first_index (IDENTICAL — the DrawIndex split is in the VS)
    }
    auto args_sb = raster->create_storage_buffer(2U * stride);
    REQUIRE(args_sb != nullptr);
    REQUIRE(raster->upload_storage(*args_sb, 0U, args_words, 2U * stride));

    crd::memory::MallocAllocator   croot;
    ce::Context                    cctx(&croot);
    const cgt::CeirIndirectProgram rp = cgt::build_ceir_draw_indirect_render(cctx, dim, index_offset);
    CHECK(cctx.find_render_misuse(*rp.module).kind == ce::RenderMisuseKind::None);
    cgt::CeirBufferMap buffers;
    buffers.add(rp.args->result(0U), args_sb.get()); // operand 0 → args
    buffers.add(rp.vbuf->result(0U), geo.get());     // operand 1 → the pull+index buffer

    const ceg::ExecuteError err =
        cgt::run_ceir_render_frame_indirect(cctx, *rp.body, croot, *raster, target.get(), program.get(), buffers);
    CHECK(err == ceg::ExecuteError::None); // ⭐ the CEIR indexed-indirect draw drove draw_storage_multi_indexed_indirect

    const crd::u32 left   = target->read_pixel(dim / 4U, dim / 2U);      // NDC (-0.5, 0) — sub-draw 0 (DrawIndex 0)
    const crd::u32 right  = target->read_pixel(3U * dim / 4U, dim / 2U); // NDC (+0.5, 0) — sub-draw 1 (DrawIndex 1)
    const crd::u32 corner = target->read_pixel(0U, 0U);
    CHECK((left & 0xFFU) >= 250U);            // left RED (sub-draw 0 rendered)
    CHECK((right & 0xFFU) >= 250U);           // ⭐ right RED ⇒ sub-draw 1 landed RIGHT ⇒ the DrawIndex root constant was PUSHED
    CHECK(((corner >> 16U) & 0xFFU) >= 250U); // corner BLUE (the scope clear)
    CHECK((corner & 0xFFU) < 8U);             // corner red low
}

// CEIR-14z-6(c2) (DX12): INDEXED-INDIRECT-COUNT — the DEVICE COUNT gates execution. render.draw_indirect_count(%args, %count,
// %vbuf) {max_draws=2} drives ExecuteIndirect with a pCountBuffer: count=2 ⇒ both sub-draws (left+right RED), count=1 ⇒ only
// sub-draw 0 (left RED, right BLUE). The CONTRAST proves the device count VARIES execution. Guarded on indirect_count_supported().
TEST_CASE("ceir 14z-6: a CEIR indexed-indirect-count draw gates sub-draws on the device count (DX12 frame graph)",
          "[ceir][ceir-gpu][render][dx12][gpu]")
{
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    if (raster->create_frame_graph() == nullptr) { WARN("no frame graph on this raster context; skipping"); return; }
    if (!raster->indirect_count_supported()) { WARN("no indirect-count support; skipping"); return; }

    crd::memory::TlsfAllocator alloc(8U << 20U);
    crd::kir::KGraph           vg(&alloc);
    crd::kir::KEntry           ve;
    crd::gputest::build_vertex_pull_drawindex_vs(vg, ve);
    crd::kir::KGraph fg(&alloc);
    crd::kir::KEntry fe;
    crd::gputest::build_triangle_fs(fg, fe); // RED
    auto vs = gctx->create_program(vg, ve);
    if (vs == nullptr) { WARN("dxc/DXIL unavailable; skipping the device draw"); return; }
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 dim          = 64U;
    constexpr crd::u32 index_offset = 144U;
    float              verts[36]    = {0.0F};
    verts[0]  = -0.5F;
    verts[1]  = -0.4F;
    verts[12] = -0.1F;
    verts[13] = 0.4F;
    verts[24] = -0.9F;
    verts[25] = 0.4F;
    auto geo = raster->create_storage_buffer(160U);
    REQUIRE(geo != nullptr);
    REQUIRE(raster->upload_storage(*geo, 0U, verts, static_cast<crd::u32>(sizeof(verts))));
    const crd::u32 indices[3] = {0U, 1U, 2U};
    REQUIRE(raster->upload_storage(*geo, index_offset, indices, static_cast<crd::u32>(sizeof(indices))));

    const crd::u32 stride   = raster->indirect_command_stride();
    const crd::u32 arg_off  = raster->indirect_command_arg_offset();
    const crd::u32 stride_w = stride / 4U;
    const crd::u32 argw     = arg_off / 4U;
    crd::u32       args_words[12] = {0U};
    for (crd::u32 i = 0; i < 2U; ++i)
    {
        if (argw != 0U) { args_words[i * stride_w] = i; } // DX12: leading DrawIndex root constant
        args_words[i * stride_w + argw + 0U] = 3U;
        args_words[i * stride_w + argw + 1U] = 1U;
        args_words[i * stride_w + argw + 2U] = 0U;
    }
    auto args_sb = raster->create_storage_buffer(2U * stride);
    REQUIRE(args_sb != nullptr);
    REQUIRE(raster->upload_storage(*args_sb, 0U, args_words, 2U * stride));
    auto count_sb = raster->create_storage_buffer(4U);
    REQUIRE(count_sb != nullptr);

    crd::memory::MallocAllocator        croot;
    ce::Context                         cctx(&croot);
    const cgt::CeirIndirectCountProgram rp = cgt::build_ceir_draw_indirect_count_render(cctx, dim, index_offset);
    CHECK(cctx.find_render_misuse(*rp.module).kind == ce::RenderMisuseKind::None);

    const auto run = [&](crd::u32 count_value, crd::u32& left, crd::u32& right) {
        REQUIRE(raster->upload_storage(*count_sb, 0U, &count_value, 4U));
        auto target = raster->create_color_depth_target(dim, dim);
        REQUIRE(target != nullptr);
        cgt::CeirBufferMap buffers;
        buffers.add(rp.args->result(0U), args_sb.get());
        buffers.add(rp.count->result(0U), count_sb.get());
        buffers.add(rp.vbuf->result(0U), geo.get());
        const ceg::ExecuteError err =
            cgt::run_ceir_render_frame_indirect(cctx, *rp.body, croot, *raster, target.get(), program.get(), buffers);
        CHECK(err == ceg::ExecuteError::None);
        left  = target->read_pixel(dim / 4U, dim / 2U);
        right = target->read_pixel(3U * dim / 4U, dim / 2U);
    };
    crd::u32 l2 = 0U;
    crd::u32 r2 = 0U;
    crd::u32 l1 = 0U;
    crd::u32 r1 = 0U;
    run(2U, l2, r2);
    run(1U, l1, r1);

    CHECK((l2 & 0xFFU) >= 250U);          // count=2 left RED
    CHECK((r2 & 0xFFU) >= 250U);          // count=2 right RED (both ran)
    CHECK((l1 & 0xFFU) >= 250U);          // count=1 left RED (sub-draw 0 still runs)
    CHECK(((r1 >> 16U) & 0xFFU) >= 250U); // ⭐ count=1 right BLUE ⇒ sub-draw 1 GATED by the device count
    CHECK((r1 & 0xFFU) < 8U);             // count=1 right red low
}

// CEIR-14z-7 (DX12): a CEIR mesh_dispatch(1,1,1) drives draw_mesh (a PROCEDURAL mesh shader) through the frame graph, rendering
// the shared triangle (build_triangle_mesh): centre RED, corner BLUE. ⛔ the mesh program is create_mesh_program (a distinct
// factory — a null return = no D3D12 mesh-shader tier, the DX12 mesh guard). No ValidationCapture on DX12 (a 13z named-forward).
TEST_CASE("ceir 14z-7: a CEIR mesh dispatch renders the shared triangle through the DX12 frame graph",
          "[ceir][ceir-gpu][render][dx12][gpu]")
{
    auto gctx = g::create_dx12_gpu_context();
    if (gctx == nullptr || !gctx->valid()) { WARN("no D3D12 device available; skipping"); return; }
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    if (raster->create_frame_graph() == nullptr) { WARN("no frame graph on this raster context; skipping"); return; }

    crd::memory::TlsfAllocator alloc(8U << 20U);
    crd::kir::KGraph           mg(&alloc);
    crd::kir::KEntry           me;
    crd::gputest::build_triangle_mesh(mg, me);
    crd::kir::KGraph fg(&alloc);
    crd::kir::KEntry fe;
    crd::gputest::build_triangle_fs(fg, fe); // RED
    auto mesh = gctx->create_program(mg, me);
    if (mesh == nullptr) { WARN("dxc/DXIL unavailable; skipping the device draw"); return; }
    REQUIRE(mesh->stage() == g::ShaderStage::Mesh);
    auto fs = gctx->create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster->create_mesh_program(*mesh, *fs);
    if (program == nullptr) { WARN("no D3D12 mesh-shader tier (OPTIONS7 MeshShaderTier); skipping the mesh draw"); return; }
    REQUIRE(program->valid());

    constexpr crd::u32 dim    = 32U;
    auto               target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);

    crd::memory::MallocAllocator croot;
    ce::Context                  cctx(&croot);
    const cgt::CeirMeshProgram   rp = cgt::build_ceir_mesh_dispatch_render(cctx, dim, 1U, 1U, 1U);
    CHECK(cctx.find_render_misuse(*rp.module).kind == ce::RenderMisuseKind::None);

    const ceg::ExecuteError err = cgt::run_ceir_render_frame(cctx, *rp.body, croot, *raster, target.get(), program.get());
    CHECK(err == ceg::ExecuteError::None); // ⭐ the CEIR mesh dispatch drove draw_mesh through the frame graph

    const crd::u32 centre = target->read_pixel(dim / 2U, dim / 2U);
    const crd::u32 corner = target->read_pixel(0U, 0U);
    CHECK((centre & 0xFFU) >= 250U);          // centre RED (inside the MESH-emitted triangle)
    CHECK(((corner >> 16U) & 0xFFU) >= 250U); // corner BLUE (the scope clear)
    CHECK((corner & 0xFFU) < 8U);             // corner red low
}
