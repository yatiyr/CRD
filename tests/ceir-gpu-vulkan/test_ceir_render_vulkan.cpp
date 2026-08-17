// CEIR-14z-3 (Vulkan) — the FIRST render DEVICE pixel proof. A CEIR `render.scope { render.draw }` is lowered by 14z and
// executed through `execute_render_lowered` onto a REAL Vulkan raster `ICommandEncoder` (ADR-0126 / Option A test-surface),
// drawing the SHARED CKIR triangle: the centre texel reads RED (inside the IR-authored triangle), a corner reads BLUE
// (the scope's clear), and the whole device lifecycle is validation-SILENT. ⛔ device-guarded (WARN-skip: no
// graphics-capable device / no shader objects / no glslc). The device-FREE lower-shape case (the CEIR triangle lowers to
// [BeginRender, Draw, EndRender] + find_render_misuse clean) is the all-skip false-green guard for THIS target — and it
// builds on Linux, so it is the 4-config always-runs leg. The seam is unit-tested device-free in
// tests/ceir-gpu/test_execute.cpp (14z-1/14z-2); here it drives real pixels.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/gpu/lower.hpp>
#include <crd/ceir/render.hpp> // RenderMisuseKind

#include <crd/gpu/vulkan_context.hpp>
#include <crd/gpu/vulkan_raster_context.hpp>
#include <crd/gpu/vulkan_validation_capture.hpp>

#include <crd/kir/ckir.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include "../gpu-shared/ceir_render_triangle.hpp" // the SHARED CEIR render program + the executor wrapper
#include "../gpu-shared/ckir_raster_triangle.hpp" // the SHARED CKIR triangle geometry (build_triangle_vs/fs) + build_gbuffer_two_output_fs
#include "../gpu-shared/ckir_vertex_pull.hpp"     // CEIR-14z-4c: build_vertex_pull_vs (the storage-pull VS for the MRT proof)

#include <catch2/catch_test_macros.hpp>

namespace ce  = crd::ceir;
namespace ceg = crd::ceir::gpu;
namespace cgt = crd::ceir_gpu_test;
namespace g   = crd::gpu;

// The device-FREE always-runs guard: the shared CEIR triangle program is well-formed (find_render_misuse clean) and
// lowers to exactly [BeginRender, Draw, EndRender]. Guards the all-skip false-green for this target on all 4 configs.
TEST_CASE("ceir 14z-3: the CEIR triangle render is clean and lowers to BeginRender Draw EndRender (device-free)",
          "[ceir][ceir-gpu][render][vulkan]")
{
    crd::memory::GrowableTlsfAllocator      root;
    ce::Context                       ctx(&root);
    const cgt::CeirRenderProgram      prog = cgt::build_ceir_triangle_render(ctx, 32U);
    REQUIRE(prog.module != nullptr);
    REQUIRE(prog.body != nullptr);
    CHECK(ctx.find_render_misuse(*prog.module).kind == ce::RenderMisuseKind::None);

    crd::containers::Array<ceg::LoweredCommand> cmds(&root);
    ceg::lower_region(ctx, *prog.body, cmds);
    REQUIRE(cmds.size() == 3U);
    CHECK(cmds[0].kind == ceg::LoweredKind::BeginRender);
    CHECK(cmds[1].kind == ceg::LoweredKind::Draw);
    CHECK(cmds[2].kind == ceg::LoweredKind::EndRender);
}

// The DEVICE proof: the SAME CEIR program, executed on a real Vulkan raster encoder, renders the shared triangle.
TEST_CASE("ceir 14z-3: the CEIR triangle renders RED on a Vulkan raster encoder (blue clear, validation-silent)",
          "[ceir][ceir-gpu][render][vulkan][gpu]")
{
    g::GpuContextConfig cfg;
    cfg.backend           = g::GpuBackend::Vulkan;
    cfg.headless          = true;
    cfg.enable_validation = true;
    auto gc               = g::create_vulkan_gpu_context(cfg);
    if (gc == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<g::VulkanGpuContext*>(gc.get());
    if (!vk->graphics_capable() || !vk->shader_object())
    {
        WARN("no graphics-capable Vulkan device with shader objects; skipping");
        return;
    }
    auto raster = g::create_vulkan_raster_context(*vk);
    REQUIRE(raster != nullptr);

    crd::memory::TlsfAllocator alloc(8U << 20U);
    // the SHARED CKIR triangle (the SAME graph the B3-e raster proof draws) -> Vulkan programs.
    crd::kir::KGraph vg(&alloc);
    crd::kir::KEntry ve;
    crd::gputest::build_triangle_vs(vg, ve);
    crd::kir::KGraph fg(&alloc);
    crd::kir::KEntry fe;
    crd::gputest::build_triangle_fs(fg, fe);
    auto vs = vk->create_program(vg, ve);
    if (vs == nullptr) { WARN("glslc/SPIR-V unavailable; skipping the device draw"); return; }
    auto fs = vk->create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    REQUIRE(program->valid());

    constexpr crd::u32 dim    = 32U;
    auto               target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);

    // build the CEIR render program (its resolvers return the REAL device target + program).
    crd::memory::GrowableTlsfAllocator croot;
    ce::Context                  cctx(&croot);
    const cgt::CeirRenderProgram rp = cgt::build_ceir_triangle_render(cctx, dim);
    CHECK(cctx.find_render_misuse(*rp.module).kind == ce::RenderMisuseKind::None);

    g::ValidationCapture capture(*vk);
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
    CHECK(capture.error_count() == 0U);       // validation-SILENT
    CHECK(capture.warning_count() == 0U);
}

// CEIR-14z-4a: the GOLD-STANDARD frame-recording drive. The SAME CEIR triangle program, executed through
// execute_render_frame (one frame-graph pass per render.scope) instead of a standalone synchronous encoder — the mode the
// frame-recording render verbs (MRT/indirect/mesh) require. Proves the drive mechanism on the known-good triangle before
// any engine edit: centre RED, corner BLUE, ONE submission, validation-SILENT.
TEST_CASE("ceir 14z-4a: the CEIR triangle renders RED through the Vulkan FRAME GRAPH (frame-recording drive)",
          "[ceir][ceir-gpu][render][vulkan][gpu]")
{
    g::GpuContextConfig cfg;
    cfg.backend           = g::GpuBackend::Vulkan;
    cfg.headless          = true;
    cfg.enable_validation = true;
    auto gc               = g::create_vulkan_gpu_context(cfg);
    if (gc == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<g::VulkanGpuContext*>(gc.get());
    if (!vk->graphics_capable() || !vk->shader_object())
    {
        WARN("no graphics-capable Vulkan device with shader objects; skipping");
        return;
    }
    auto raster = g::create_vulkan_raster_context(*vk);
    REQUIRE(raster != nullptr);
    if (raster->create_frame_graph() == nullptr) { WARN("no frame graph on this raster context; skipping"); return; }

    crd::memory::TlsfAllocator alloc(8U << 20U);
    crd::kir::KGraph           vg(&alloc);
    crd::kir::KEntry           ve;
    crd::gputest::build_triangle_vs(vg, ve);
    crd::kir::KGraph fg(&alloc);
    crd::kir::KEntry fe;
    crd::gputest::build_triangle_fs(fg, fe);
    auto vs = vk->create_program(vg, ve);
    if (vs == nullptr) { WARN("glslc/SPIR-V unavailable; skipping the device draw"); return; }
    auto fs = vk->create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    REQUIRE(program->valid());

    constexpr crd::u32 dim    = 32U;
    auto               target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);

    crd::memory::GrowableTlsfAllocator croot;
    ce::Context                  cctx(&croot);
    const cgt::CeirRenderProgram rp = cgt::build_ceir_triangle_render(cctx, dim);
    CHECK(cctx.find_render_misuse(*rp.module).kind == ce::RenderMisuseKind::None);

    g::ValidationCapture    capture(*vk);
    const ceg::ExecuteError err = cgt::run_ceir_render_frame(cctx, *rp.body, croot, *raster, target.get(), program.get());
    CHECK(err == ceg::ExecuteError::None); // ⭐ the frame graph drove the CEIR draw + owns the readback

    const crd::u32 centre = target->read_pixel(dim / 2U, dim / 2U);
    const crd::u32 corner = target->read_pixel(0U, 0U);
    CHECK((centre & 0xFFU) >= 250U);          // R high  => red (inside the IR-authored triangle)
    CHECK(((centre >> 16U) & 0xFFU) <= 5U);   // B low
    CHECK((corner & 0xFFU) <= 5U);            // R low   => blue clear (outside)
    CHECK(((corner >> 16U) & 0xFFU) >= 250U); // B high
    CHECK(capture.error_count() == 0U);       // validation-SILENT
    CHECK(capture.warning_count() == 0U);
}

// CEIR-14z-4c(c1): a TWO-SCOPE CEIR program through the frame graph — exercises execute_render_frame's N-scope path (the
// reserve-then-push closure array = the 11b MIRROR scar shape + the scope-splitting walk + TWO passes into TWO targets).
// Scope 0 clears BLUE, scope 1 clears GREEN; each draws the red triangle. The DISTINCT corners (blue vs green) prove the
// two scopes routed to their OWN targets with their OWN clears (a per-op target map, not the identity sentinel).
TEST_CASE("ceir 14z-4c: a TWO-SCOPE CEIR program renders into TWO targets through the Vulkan frame graph",
          "[ceir][ceir-gpu][render][vulkan][gpu]")
{
    g::GpuContextConfig cfg;
    cfg.backend           = g::GpuBackend::Vulkan;
    cfg.headless          = true;
    cfg.enable_validation = true;
    auto gc               = g::create_vulkan_gpu_context(cfg);
    if (gc == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<g::VulkanGpuContext*>(gc.get());
    if (!vk->graphics_capable() || !vk->shader_object())
    {
        WARN("no graphics-capable Vulkan device with shader objects; skipping");
        return;
    }
    auto raster = g::create_vulkan_raster_context(*vk);
    REQUIRE(raster != nullptr);
    if (raster->create_frame_graph() == nullptr) { WARN("no frame graph on this raster context; skipping"); return; }

    crd::memory::TlsfAllocator alloc(8U << 20U);
    crd::kir::KGraph           vg(&alloc);
    crd::kir::KEntry           ve;
    crd::gputest::build_triangle_vs(vg, ve);
    crd::kir::KGraph fg(&alloc);
    crd::kir::KEntry fe;
    crd::gputest::build_triangle_fs(fg, fe);
    auto vs = vk->create_program(vg, ve);
    if (vs == nullptr) { WARN("glslc/SPIR-V unavailable; skipping the device draw"); return; }
    auto fs = vk->create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 dim = 32U;
    auto               target_a  = raster->create_color_target(dim, dim); // scope 0 (BLUE clear)
    auto               target_b  = raster->create_color_target(dim, dim); // scope 1 (GREEN clear)
    REQUIRE(target_a != nullptr);
    REQUIRE(target_b != nullptr);

    crd::memory::GrowableTlsfAllocator   croot;
    ce::Context                    cctx(&croot);
    const cgt::CeirTwoScopeProgram rp = cgt::build_ceir_two_scope_render(cctx, dim);
    CHECK(cctx.find_render_misuse(*rp.module).kind == ce::RenderMisuseKind::None);
    cgt::CeirTargetMap map;
    map.add(rp.att0, target_a.get());
    map.add(rp.att1, target_b.get());

    g::ValidationCapture    capture(*vk);
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
    CHECK(capture.error_count() == 0U);
    CHECK(capture.warning_count() == 0U);
}

// CEIR-14z-4c(c1): the CEIR→MRT device proof — a CEIR render.scope with TWO color attachments (distinct clears BLUE@0 /
// RED@1) + a StoragePull draw binding a vertex buffer, driven through execute_render_frame. This is the FIRST CEIR program
// to drive draw_storage_mrt: it exercises the 14z-4c BINDING RESOLVER (the %vbuf operand → the storage buffer → the packet's
// StoragePull binding → the MRT arm) AND the per-attachment typed clears (14z-4b). The storage-pull VS pulls the triangle
// from the buffer; build_gbuffer_two_output_fs writes RED@0 / GREEN@1. Asserts: color0 centre RED + corner BLUE; color1
// centre GREEN + corner RED (distinct corners ⇒ per-attachment clears; distinct centres ⇒ correct MRT ordering).
TEST_CASE("ceir 14z-4c: a CEIR MRT program drives draw_storage_mrt RED@0/GREEN@1 with distinct clears (Vulkan frame graph)",
          "[ceir][ceir-gpu][render][vulkan][gpu]")
{
    g::GpuContextConfig cfg;
    cfg.backend           = g::GpuBackend::Vulkan;
    cfg.headless          = true;
    cfg.enable_validation = true;
    auto gc               = g::create_vulkan_gpu_context(cfg);
    if (gc == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<g::VulkanGpuContext*>(gc.get());
    if (!vk->graphics_capable() || !vk->shader_object())
    {
        WARN("no graphics-capable Vulkan device with shader objects; skipping");
        return;
    }
    auto raster = g::create_vulkan_raster_context(*vk);
    REQUIRE(raster != nullptr);
    if (raster->create_frame_graph() == nullptr) { WARN("no frame graph on this raster context; skipping"); return; }

    crd::memory::TlsfAllocator alloc(8U << 20U);
    crd::kir::KGraph           vg(&alloc);
    crd::kir::KEntry           ve;
    crd::gputest::build_vertex_pull_vs(vg, ve); // the storage-pull VS (pulls the triangle from the bound buffer)
    crd::kir::KGraph fg(&alloc);
    crd::kir::KEntry fe;
    crd::gputest::build_gbuffer_two_output_fs(fg, fe); // RED@0 / GREEN@1
    auto vs = vk->create_program(vg, ve);
    if (vs == nullptr) { WARN("glslc/SPIR-V unavailable; skipping the device draw"); return; }
    auto fs = vk->create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 dim      = 32U;
    auto               target_a = raster->create_color_target(dim, dim); // color 0 (BLUE clear, RED output)
    auto               target_b = raster->create_color_target(dim, dim); // color 1 (RED clear, GREEN output)
    REQUIRE(target_a != nullptr);
    REQUIRE(target_b != nullptr);

    // the vertex stream the VS pulls (3 verts x 12-float stride; only pos set — the shared triangle {(0,-0.8),(0.8,0.8),(-0.8,0.8)}).
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

    crd::memory::GrowableTlsfAllocator croot;
    ce::Context                  cctx(&croot);
    const cgt::CeirMrtProgram    rp = cgt::build_ceir_mrt_render(cctx, dim);
    CHECK(cctx.find_render_misuse(*rp.module).kind == ce::RenderMisuseKind::None);
    cgt::CeirTargetMap map;
    map.add(rp.att0, target_a.get());
    map.add(rp.att1, target_b.get());

    g::ValidationCapture    capture(*vk);
    const ceg::ExecuteError err = cgt::run_ceir_render_frame_mrt(cctx, *rp.body, croot, *raster, map, program.get(), geo.get());
    CHECK(err == ceg::ExecuteError::None); // ⭐ the CEIR MRT program drove draw_storage_mrt through the frame graph

    const crd::u32 a_centre = target_a->read_pixel(dim / 2U, dim / 2U);
    const crd::u32 a_corner = target_a->read_pixel(0U, 0U);
    const crd::u32 b_centre = target_b->read_pixel(dim / 2U, dim / 2U);
    const crd::u32 b_corner = target_b->read_pixel(0U, 0U);
    CHECK((a_centre & 0xFFU) >= 250U);          // color 0 centre RED (FS out0)
    CHECK(((a_centre >> 8U) & 0xFFU) < 8U);     // green low
    CHECK(((a_corner >> 16U) & 0xFFU) >= 250U); // color 0 corner BLUE (att0 clear)
    CHECK((a_corner & 0xFFU) < 8U);             // red low
    CHECK(((b_centre >> 8U) & 0xFFU) >= 250U);  // color 1 centre GREEN (FS out1 — proves correct MRT ordering)
    CHECK((b_centre & 0xFFU) < 8U);             // red low
    CHECK((b_corner & 0xFFU) >= 250U);          // color 1 corner RED (att1 clear)
    CHECK(((b_corner >> 8U) & 0xFFU) < 8U);     // green low
    CHECK(((b_corner >> 16U) & 0xFFU) < 8U);    // blue low — DISTINCT from color 0's clear (per-attachment clears)
    CHECK(capture.error_count() == 0U);
    CHECK(capture.warning_count() == 0U);
}

// CEIR-14z-4c(c2): the uint-HOMOGENEOUS MRT proof — TWO R32_UINT attachments with DISTINCT TYPED (uint) clears (100 / 200),
// a uint 2-output FS (writes id 7 / 9), through execute_render_frame. Proves the per-attachment UINT clear arm (14z-4b,
// implemented-but-unproven): the corners read the distinct uint CLEARS (100 / 200 ⇒ per-attachment typed clears); the
// centres read the distinct uint OUTPUTS (7 / 9 ⇒ MRT ordering). read_pixel on an R32_UINT target returns the raw u32.
TEST_CASE("ceir 14z-4c: a CEIR uint MRT program drives distinct uint clears + ids through the Vulkan frame graph",
          "[ceir][ceir-gpu][render][vulkan][gpu]")
{
    g::GpuContextConfig cfg;
    cfg.backend           = g::GpuBackend::Vulkan;
    cfg.headless          = true;
    cfg.enable_validation = true;
    auto gc               = g::create_vulkan_gpu_context(cfg);
    if (gc == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<g::VulkanGpuContext*>(gc.get());
    if (!vk->graphics_capable() || !vk->shader_object())
    {
        WARN("no graphics-capable Vulkan device with shader objects; skipping");
        return;
    }
    auto raster = g::create_vulkan_raster_context(*vk);
    REQUIRE(raster != nullptr);
    if (raster->create_frame_graph() == nullptr) { WARN("no frame graph on this raster context; skipping"); return; }

    crd::memory::TlsfAllocator alloc(8U << 20U);
    crd::kir::KGraph           vg(&alloc);
    crd::kir::KEntry           ve;
    crd::gputest::build_vertex_pull_vs(vg, ve);
    crd::kir::KGraph fg(&alloc);
    crd::kir::KEntry fe;
    crd::gputest::build_visbuffer_two_output_fs(fg, fe); // id 7 @0 / id 9 @1
    auto vs = vk->create_program(vg, ve);
    if (vs == nullptr) { WARN("glslc/SPIR-V unavailable; skipping the device draw"); return; }
    auto fs = vk->create_program(fg, fe);
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

    crd::memory::GrowableTlsfAllocator croot;
    ce::Context                  cctx(&croot);
    const cgt::CeirMrtProgram    rp = cgt::build_ceir_mrt_uint_render(cctx, dim, 100U, 200U);
    CHECK(cctx.find_render_misuse(*rp.module).kind == ce::RenderMisuseKind::None);
    cgt::CeirTargetMap map;
    map.add(rp.att0, target_a.get());
    map.add(rp.att1, target_b.get());

    g::ValidationCapture    capture(*vk);
    const ceg::ExecuteError err = cgt::run_ceir_render_frame_mrt(cctx, *rp.body, croot, *raster, map, program.get(), geo.get());
    CHECK(err == ceg::ExecuteError::None);

    CHECK(target_a->read_pixel(dim / 2U, dim / 2U) == 7U);   // color 0 centre = uint id 7 (FS out0)
    CHECK(target_a->read_pixel(0U, 0U) == 100U);             // color 0 corner = uint clear 100 (per-attachment typed clear)
    CHECK(target_b->read_pixel(dim / 2U, dim / 2U) == 9U);   // color 1 centre = uint id 9 (FS out1 — MRT ordering)
    CHECK(target_b->read_pixel(0U, 0U) == 200U);             // color 1 corner = uint clear 200 (DISTINCT ⇒ per-attachment clears)
    CHECK(capture.error_count() == 0U);
    CHECK(capture.warning_count() == 0U);
}

// CEIR-14z-4c(c3): the HETEROGENEOUS (mixed-type) MRT proof — color0 R32_UINT (uint clear 100, id 7) + color1 RGBA8 (float
// clear BLUE, GREEN output) in ONE scope. The plainest reading of "typed clears asserted per-target". ⭐ VULKAN is the
// CONTROL: it derives attachment formats from the per-attachment views, so this must pass with NO engine change (the DX12
// pass_pso single-format fix is c3's engine work, next tick). A Vulkan failure would mean the problem is in a SHARED layer
// (verifier/materializer/emitter/encoder), not DX12 — so this isolates the DX12 variable.
TEST_CASE("ceir 14z-4c: a CEIR MIXED uint+float MRT program renders through the Vulkan frame graph (heterogeneous formats)",
          "[ceir][ceir-gpu][render][vulkan][gpu]")
{
    g::GpuContextConfig cfg;
    cfg.backend           = g::GpuBackend::Vulkan;
    cfg.headless          = true;
    cfg.enable_validation = true;
    auto gc               = g::create_vulkan_gpu_context(cfg);
    if (gc == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<g::VulkanGpuContext*>(gc.get());
    if (!vk->graphics_capable() || !vk->shader_object())
    {
        WARN("no graphics-capable Vulkan device with shader objects; skipping");
        return;
    }
    auto raster = g::create_vulkan_raster_context(*vk);
    REQUIRE(raster != nullptr);
    if (raster->create_frame_graph() == nullptr) { WARN("no frame graph on this raster context; skipping"); return; }

    crd::memory::TlsfAllocator alloc(8U << 20U);
    crd::kir::KGraph           vg(&alloc);
    crd::kir::KEntry           ve;
    crd::gputest::build_vertex_pull_vs(vg, ve);
    crd::kir::KGraph fg(&alloc);
    crd::kir::KEntry fe;
    crd::gputest::build_gbuffer_uint_float_fs(fg, fe); // uint id 7 @0 / GREEN @1
    auto vs = vk->create_program(vg, ve);
    if (vs == nullptr) { WARN("glslc/SPIR-V unavailable; skipping the device draw"); return; }
    auto fs = vk->create_program(fg, fe);
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

    crd::memory::GrowableTlsfAllocator croot;
    ce::Context                  cctx(&croot);
    const cgt::CeirMrtProgram    rp = cgt::build_ceir_mrt_mixed_render(cctx, dim, 100U);
    CHECK(cctx.find_render_misuse(*rp.module).kind == ce::RenderMisuseKind::None); // ⭐ verifier accepts a MIXED-format scope
    cgt::CeirTargetMap map;
    map.add(rp.att0, target_a.get());
    map.add(rp.att1, target_b.get());

    g::ValidationCapture    capture(*vk);
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
    CHECK(capture.error_count() == 0U);
    CHECK(capture.warning_count() == 0U);
}

// CEIR-14z-5: the DEPTH-ONLY proof by DEPTH-TEST OCCLUSION (attachment-only observation — no sampling). Scope 0 is a genuine
// DEPTH-ONLY pass (draw_storage_depth_only, an n_out=0 program — the ⛔ depth-only-≠-forward scar honored by construction):
// the shared StoragePull triangle writes depth 0.5 under it, clear 1.0 elsewhere, into T's depth buffer. Scope 1 LOADS that
// depth and fullscreen-tests frag_depth 0.75 (LessEqual): centre (under the triangle, stored 0.5) FAILS ⇒ the BLUE clear
// survives; corner (stored 1.0) PASSES ⇒ RED is written. The colour readback IS the proof scope 0 wrote real depth. Discriminates
// a verb no-op / a broken depth-LOAD (either flips centre→RED) and a clear-to-0 (flips corner→BLUE).
TEST_CASE("ceir 14z-5: a CEIR depth-only pass renders depth; a later scope occludes against it (Vulkan frame graph)",
          "[ceir][ceir-gpu][render][vulkan][gpu]")
{
    g::GpuContextConfig cfg;
    cfg.backend           = g::GpuBackend::Vulkan;
    cfg.headless          = true;
    cfg.enable_validation = true;
    auto gc               = g::create_vulkan_gpu_context(cfg);
    if (gc == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<g::VulkanGpuContext*>(gc.get());
    if (!vk->graphics_capable() || !vk->shader_object())
    {
        WARN("no graphics-capable Vulkan device with shader objects; skipping");
        return;
    }
    auto raster = g::create_vulkan_raster_context(*vk);
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
    auto dvs = vk->create_program(dvg, dve);
    if (dvs == nullptr) { WARN("glslc/SPIR-V unavailable; skipping the device draw"); return; }
    auto dfs = vk->create_program(dfg, dfe);
    REQUIRE(dfs != nullptr);
    auto depth_prog = raster->create_raster_program(*dvs, *dfs);
    REQUIRE(depth_prog != nullptr);
    // scope-1 program: a fullscreen VS + a COLOUR+DEPTH FS (n_out = 1 RED, frag_depth = 0.75).
    crd::kir::KGraph cvg(&alloc);
    crd::kir::KEntry cve;
    crd::gputest::build_fullscreen_vs(cvg, cve);
    crd::kir::KGraph cfg2(&alloc);
    crd::kir::KEntry cfe;
    crd::gputest::build_color_depth_fs(cfg2, cfe, 1.0, 0.0, 0.0, 0.75); // RED, frag_depth 0.75
    auto cvs = vk->create_program(cvg, cve);
    REQUIRE(cvs != nullptr);
    auto cfs = vk->create_program(cfg2, cfe);
    REQUIRE(cfs != nullptr);
    auto cd_prog = raster->create_raster_program(*cvs, *cfs);
    REQUIRE(cd_prog != nullptr);

    constexpr crd::u32 dim = 32U;
    auto               target = raster->create_color_depth_target(dim, dim); // ONE colour+depth target (has_depth())
    REQUIRE(target != nullptr);

    // the vertex stream scope 0's VS pulls (the shared triangle; 3 verts x 12-float stride, only pos set).
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

    crd::memory::GrowableTlsfAllocator croot;
    ce::Context                  cctx(&croot);
    const cgt::CeirDepthProgram  rp = cgt::build_ceir_depth_occlusion_render(cctx, dim);
    CHECK(cctx.find_render_misuse(*rp.module).kind == ce::RenderMisuseKind::None);
    cgt::CeirProgramMap progs;
    progs.add(rp.draw0, depth_prog.get()); // scope 0 → the depth-only program
    progs.add(rp.draw1, cd_prog.get());    // scope 1 → the colour+depth program

    g::ValidationCapture capture(*vk);
    const ceg::ExecuteError err =
        cgt::run_ceir_render_frame_depth(cctx, *rp.body, croot, *raster, target.get(), progs, geo.get());
    CHECK(err == ceg::ExecuteError::None); // ⭐ the CEIR depth-only pass drove draw_storage_depth_only through the frame graph

    const crd::u32 centre = target->read_pixel(dim / 2U, dim / 2U); // under the triangle: stored depth 0.5, 0.75 FAILS
    const crd::u32 corner = target->read_pixel(0U, 0U);             // outside: stored depth 1.0, 0.75 PASSES
    CHECK(((centre >> 16U) & 0xFFU) >= 250U); // centre BLUE (the clear survives ⇒ 0.75 > 0.5, the depth-only pass wrote 0.5)
    CHECK((centre & 0xFFU) < 8U);             // centre red low
    CHECK((corner & 0xFFU) >= 250U);          // corner RED (0.75 <= 1.0 ⇒ the fragment passed + wrote)
    CHECK(((corner >> 16U) & 0xFFU) < 8U);    // corner blue low
    CHECK(capture.error_count() == 0U);
    CHECK(capture.warning_count() == 0U);
}

// CEIR-14z-6(c1): INDEXED-INDIRECT with the DrawIndex-PUSH proof. A CEIR render.draw_indirect(%args, %vbuf) {max_draws=2}
// drives draw_storage_multi_indexed_indirect: TWO sub-draws from an args buffer of TWO IDENTICAL commands (first_index=0
// both). The VS (build_vertex_pull_drawindex_vs) shifts X by the pushed SV_DrawIndex ⇒ draw 0 → LEFT, draw 1 → RIGHT. If the
// executor did NOT push the per-sub-draw row (the REN-40 scar), both would read DrawIndex 0 and land LEFT ⇒ the right-centre
// reads the BLUE clear. ⛔ NO batch-counter assertion: the REN-40 GPU-driven indirect verb ticks no multi_(indexed_)batch_count
// (that counter is the CPU-args multi verbs'); the POSITIONAL DrawIndex split is the discriminator instead — it catches every
// no-op mode (a silent no-op ⇒ left BLUE; a missing DrawIndex push ⇒ right BLUE; only-first-sub-draw ⇒ right BLUE), and the
// batch-vs-loop distinction the counter uniquely adds is moot on a REAL device (the fallback loop is stub-only; real backends
// always ExecuteIndirect). ⛔ args buffer is BACKEND-specific (stride/arg_off; DX12's leading u32 = DrawIndex). This is the
// FIRST CEIR multi-buffer draw (%args + %vbuf resolve from DISTINCT operands).
TEST_CASE("ceir 14z-6: a CEIR indexed-indirect draw pushes the per-sub-draw DrawIndex row (Vulkan frame graph)",
          "[ceir][ceir-gpu][render][vulkan][gpu]")
{
    g::GpuContextConfig cfg;
    cfg.backend           = g::GpuBackend::Vulkan;
    cfg.headless          = true;
    cfg.enable_validation = true;
    auto gc               = g::create_vulkan_gpu_context(cfg);
    if (gc == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<g::VulkanGpuContext*>(gc.get());
    if (!vk->graphics_capable() || !vk->shader_object())
    {
        WARN("no graphics-capable Vulkan device with shader objects; skipping");
        return;
    }
    auto raster = g::create_vulkan_raster_context(*vk);
    REQUIRE(raster != nullptr);
    if (raster->create_frame_graph() == nullptr) { WARN("no frame graph on this raster context; skipping"); return; }

    crd::memory::TlsfAllocator alloc(8U << 20U);
    crd::kir::KGraph           vg(&alloc);
    crd::kir::KEntry           ve;
    crd::gputest::build_vertex_pull_drawindex_vs(vg, ve); // pull + X-shift by the pushed SV_DrawIndex
    crd::kir::KGraph fg(&alloc);
    crd::kir::KEntry fe;
    crd::gputest::build_triangle_fs(fg, fe); // RED
    auto vs = vk->create_program(vg, ve);
    if (vs == nullptr) { WARN("glslc/SPIR-V unavailable; skipping the device draw"); return; }
    auto fs = vk->create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 dim    = 64U;
    auto               target = raster->create_color_depth_target(dim, dim);
    REQUIRE(target != nullptr);

    // %vbuf: 3 verts (a triangle centred at NDC x=-0.5, symmetric about y=0 so the midline probe hits both backends) at the
    // 48-byte pull stride, then the 3-index section [0,1,2] at index_offset. Draw 0 (DrawIndex 0) covers the LEFT half; draw 1
    // (DrawIndex 1, X+1) covers the RIGHT half.
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

    // the ARGS buffer in THIS backend's declared indexed-indirect layout (REN-40-A): stride (20 VK / 24 DX12) + arg_off (0 / 4,
    // DX12's leading u32 = the DrawIndex root constant). TWO IDENTICAL commands {index_count=3, instance_count=1, first_index=0}.
    const crd::u32 stride   = raster->indirect_command_stride();
    const crd::u32 arg_off  = raster->indirect_command_arg_offset();
    const crd::u32 stride_w = stride / 4U;
    const crd::u32 argw     = arg_off / 4U;
    crd::u32       args_words[12] = {0U}; // max 2 x 6 u32 (DX12)
    for (crd::u32 i = 0; i < 2U; ++i)
    {
        if (argw != 0U) { args_words[i * stride_w] = i; }    // DX12: the per-command DrawIndex root constant
        args_words[i * stride_w + argw + 0U] = 3U;           // index_count
        args_words[i * stride_w + argw + 1U] = 1U;           // instance_count
        args_words[i * stride_w + argw + 2U] = 0U;           // first_index (IDENTICAL — the DrawIndex split is in the VS)
    }
    auto args_sb = raster->create_storage_buffer(2U * stride);
    REQUIRE(args_sb != nullptr);
    REQUIRE(raster->upload_storage(*args_sb, 0U, args_words, 2U * stride));

    crd::memory::GrowableTlsfAllocator  croot;
    ce::Context                   cctx(&croot);
    const cgt::CeirIndirectProgram rp = cgt::build_ceir_draw_indirect_render(cctx, dim, index_offset);
    CHECK(cctx.find_render_misuse(*rp.module).kind == ce::RenderMisuseKind::None);
    cgt::CeirBufferMap buffers;
    buffers.add(rp.args->result(0U), args_sb.get()); // operand 0 → args
    buffers.add(rp.vbuf->result(0U), geo.get());     // operand 1 → the pull+index buffer

    g::ValidationCapture capture(*vk);
    const ceg::ExecuteError err =
        cgt::run_ceir_render_frame_indirect(cctx, *rp.body, croot, *raster, target.get(), program.get(), buffers);
    CHECK(err == ceg::ExecuteError::None); // ⭐ the CEIR indexed-indirect draw drove draw_storage_multi_indexed_indirect

    const crd::u32 left   = target->read_pixel(dim / 4U, dim / 2U);      // NDC (-0.5, 0) — sub-draw 0 (DrawIndex 0)
    const crd::u32 right  = target->read_pixel(3U * dim / 4U, dim / 2U); // NDC (+0.5, 0) — sub-draw 1 (DrawIndex 1)
    const crd::u32 corner = target->read_pixel(0U, 0U);
    CHECK((left & 0xFFU) >= 250U);            // left RED (sub-draw 0 rendered)
    CHECK((right & 0xFFU) >= 250U);           // ⭐ right RED ⇒ sub-draw 1 landed RIGHT ⇒ SV_DrawIndex=1 was PUSHED (the REN-40 proof)
    CHECK(((corner >> 16U) & 0xFFU) >= 250U); // corner BLUE (the scope clear)
    CHECK((corner & 0xFFU) < 8U);             // corner red low
    CHECK(capture.error_count() == 0U);
    CHECK(capture.warning_count() == 0U);
}

// CEIR-14z-6(c2): INDEXED-INDIRECT-COUNT — the DEVICE COUNT gates execution. A CEIR render.draw_indirect_count(%args, %count,
// %vbuf) {max_draws=2} drives vkCmdDrawIndexedIndirectCount: the GPU reads the ACTUAL draw count from %count (bounded by
// max_draws). SAME setup run twice — count=2 ⇒ BOTH sub-draws (left+right RED); count=1 ⇒ ONLY sub-draw 0 (left RED, right
// BLUE). The CONTRAST (right RED vs right BLUE) proves the device count VARIES execution — not merely that the count buffer is
// plumbed. Guarded on indirect_count_supported() (Vulkan 1.3 core).
TEST_CASE("ceir 14z-6: a CEIR indexed-indirect-count draw gates sub-draws on the device count (Vulkan frame graph)",
          "[ceir][ceir-gpu][render][vulkan][gpu]")
{
    g::GpuContextConfig cfg;
    cfg.backend           = g::GpuBackend::Vulkan;
    cfg.headless          = true;
    cfg.enable_validation = true;
    auto gc               = g::create_vulkan_gpu_context(cfg);
    if (gc == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<g::VulkanGpuContext*>(gc.get());
    if (!vk->graphics_capable() || !vk->shader_object())
    {
        WARN("no graphics-capable Vulkan device with shader objects; skipping");
        return;
    }
    auto raster = g::create_vulkan_raster_context(*vk);
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
    auto vs = vk->create_program(vg, ve);
    if (vs == nullptr) { WARN("glslc/SPIR-V unavailable; skipping the device draw"); return; }
    auto fs = vk->create_program(fg, fe);
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
        if (argw != 0U) { args_words[i * stride_w] = i; }
        args_words[i * stride_w + argw + 0U] = 3U; // index_count
        args_words[i * stride_w + argw + 1U] = 1U; // instance_count
        args_words[i * stride_w + argw + 2U] = 0U; // first_index (identical)
    }
    auto args_sb = raster->create_storage_buffer(2U * stride);
    REQUIRE(args_sb != nullptr);
    REQUIRE(raster->upload_storage(*args_sb, 0U, args_words, 2U * stride));
    auto count_sb = raster->create_storage_buffer(4U);
    REQUIRE(count_sb != nullptr);

    crd::memory::GrowableTlsfAllocator        croot;
    ce::Context                         cctx(&croot);
    const cgt::CeirIndirectCountProgram rp = cgt::build_ceir_draw_indirect_count_render(cctx, dim, index_offset);
    CHECK(cctx.find_render_misuse(*rp.module).kind == ce::RenderMisuseKind::None);

    g::ValidationCapture capture(*vk);
    // run the SAME program with a given device draw count into a fresh target; report left/right centres.
    const auto run = [&](crd::u32 count_value, crd::u32& left, crd::u32& right) {
        REQUIRE(raster->upload_storage(*count_sb, 0U, &count_value, 4U));
        auto target = raster->create_color_depth_target(dim, dim);
        REQUIRE(target != nullptr);
        cgt::CeirBufferMap buffers;
        buffers.add(rp.args->result(0U), args_sb.get());  // operand 0 → args
        buffers.add(rp.count->result(0U), count_sb.get()); // operand 1 → the device count
        buffers.add(rp.vbuf->result(0U), geo.get());       // operand 2 → the pull+index buffer
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
    run(2U, l2, r2); // device count = 2 ⇒ both sub-draws
    run(1U, l1, r1); // device count = 1 ⇒ only sub-draw 0

    CHECK((l2 & 0xFFU) >= 250U);            // count=2 left RED (sub-draw 0)
    CHECK((r2 & 0xFFU) >= 250U);            // count=2 right RED (sub-draw 1 ran)
    CHECK((l1 & 0xFFU) >= 250U);            // count=1 left RED (sub-draw 0 still runs)
    CHECK(((r1 >> 16U) & 0xFFU) >= 250U);   // ⭐ count=1 right BLUE ⇒ sub-draw 1 was GATED by the device count
    CHECK((r1 & 0xFFU) < 8U);              // count=1 right red low
    CHECK(capture.error_count() == 0U);
    CHECK(capture.warning_count() == 0U);
}

// CEIR-14z-7: the DEVICE-FREE mesh lower-shape leg (the all-skip false-green guard — runs on EVERY config, incl. a linux box
// whose llvmpipe advertises no VK_EXT_mesh_shader). A CEIR mesh_dispatch(1,1,1) is clean + lowers to [BeginRender, Draw,
// EndRender] (the mesh dispatch is a Draw at the lowered layer — the GeometryKind carries the mesh-ness).
TEST_CASE("ceir 14z-7: the CEIR mesh dispatch is clean and lowers to BeginRender Draw EndRender (device-free)",
          "[ceir][ceir-gpu][render][vulkan]")
{
    crd::memory::GrowableTlsfAllocator croot;
    ce::Context                  cctx(&croot);
    const cgt::CeirMeshProgram   rp = cgt::build_ceir_mesh_dispatch_render(cctx, 32U, 1U, 1U, 1U);
    REQUIRE(rp.module != nullptr);
    CHECK(cctx.find_render_misuse(*rp.module).kind == ce::RenderMisuseKind::None);

    crd::containers::Array<ceg::LoweredCommand> cmds(&croot);
    ceg::lower_region(cctx, *rp.body, cmds);
    REQUIRE(cmds.size() == 3U);
    CHECK(cmds[0].kind == ceg::LoweredKind::BeginRender);
    CHECK(cmds[1].kind == ceg::LoweredKind::Draw);
    CHECK(cmds[2].kind == ceg::LoweredKind::EndRender);
}

// CEIR-14z-7: the DEVICE proof — a CEIR mesh_dispatch(1,1,1) drives draw_mesh (a PROCEDURAL mesh shader) through the frame
// graph, rendering the shared triangle (build_triangle_mesh, one meshlet workgroup): centre RED, corner BLUE. ⛔ the mesh
// program is created via create_mesh_program (a distinct factory; draw_mesh no-ops on a non-mesh program — is_mesh()). ⛔ mesh
// scar: if it renders NOTHING (even the clear), it is a silent DEVICE_LOST (submit ok / wait -4), not shader logic — but B4
// wired NO_TASK_SHADER, so the ValidationCapture-silent corner-BLUE check suffices here.
TEST_CASE("ceir 14z-7: a CEIR mesh dispatch renders the shared triangle through the Vulkan frame graph",
          "[ceir][ceir-gpu][render][vulkan][gpu]")
{
    g::GpuContextConfig cfg;
    cfg.backend           = g::GpuBackend::Vulkan;
    cfg.headless          = true;
    cfg.enable_validation = true;
    auto gc               = g::create_vulkan_gpu_context(cfg);
    if (gc == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<g::VulkanGpuContext*>(gc.get());
    if (!vk->shader_object()) { WARN("no VK_EXT_shader_object; skipping"); return; }
    if (!vk->mesh_shader()) { WARN("adapter has no VK_EXT_mesh_shader; skipping the mesh draw"); return; }
    auto raster = g::create_vulkan_raster_context(*vk);
    REQUIRE(raster != nullptr);
    if (raster->create_frame_graph() == nullptr) { WARN("no frame graph on this raster context; skipping"); return; }

    crd::memory::TlsfAllocator alloc(8U << 20U);
    crd::kir::KGraph           mg(&alloc);
    crd::kir::KEntry           me;
    crd::gputest::build_triangle_mesh(mg, me); // the mesh KIR entry (one meshlet = the shared triangle)
    crd::kir::KGraph fg(&alloc);
    crd::kir::KEntry fe;
    crd::gputest::build_triangle_fs(fg, fe); // RED
    auto mesh = vk->create_program(mg, me);
    if (mesh == nullptr) { WARN("glslc/SPIR-V unavailable; skipping the device draw"); return; }
    REQUIRE(mesh->stage() == g::ShaderStage::Mesh);
    auto fs = vk->create_program(fg, fe);
    REQUIRE(fs != nullptr);
    auto program = raster->create_mesh_program(*mesh, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 dim    = 32U;
    auto               target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);

    crd::memory::GrowableTlsfAllocator croot;
    ce::Context                  cctx(&croot);
    const cgt::CeirMeshProgram   rp = cgt::build_ceir_mesh_dispatch_render(cctx, dim, 1U, 1U, 1U);
    CHECK(cctx.find_render_misuse(*rp.module).kind == ce::RenderMisuseKind::None);

    g::ValidationCapture    capture(*vk);
    const ceg::ExecuteError err = cgt::run_ceir_render_frame(cctx, *rp.body, croot, *raster, target.get(), program.get());
    CHECK(err == ceg::ExecuteError::None); // ⭐ the CEIR mesh dispatch drove draw_mesh through the frame graph

    const crd::u32 centre = target->read_pixel(dim / 2U, dim / 2U);
    const crd::u32 corner = target->read_pixel(0U, 0U);
    CHECK((centre & 0xFFU) >= 250U);          // centre RED (inside the MESH-emitted triangle)
    CHECK(((corner >> 16U) & 0xFFU) >= 250U); // corner BLUE (the scope clear — proves the mesh draw did not device-lost)
    CHECK((corner & 0xFFU) < 8U);             // corner red low
    CHECK(capture.error_count() == 0U);
    CHECK(capture.warning_count() == 0U);
}
