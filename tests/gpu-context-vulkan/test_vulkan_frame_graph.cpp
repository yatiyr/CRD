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

#include <crd/kir/ckir.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <ckir_raster_triangle.hpp> // the shared CKIR triangle VS/FS

#include <catch2/catch_test_macros.hpp>

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
