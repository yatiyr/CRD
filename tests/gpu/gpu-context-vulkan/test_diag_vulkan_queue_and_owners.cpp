// DIAG.10c: GPU queue depth at the Vulkan completion seam, the native owners in the lifetime ledger, and their
// shutdown. Declared contract (docs/design/runtime-diagnostics.md, DIAG.10c):
// - Every vk_submit is a step in flight until the seam observes it: a fence wait observes the submission that signalled
//   the fence and every earlier one on its queue, and an idle wait observes the queue (or the device). A submission
//   without a fence (the raster context's one-shot path) is observed by its idle wait. The context's queue_pressure()
//   reads its device's steps. A frame graph that defers its wait keeps a ring of two frame slots: a frame stays in
//   flight until the graph reuses its slot (or is destroyed), so at most two frames are pending and a third execute
//   observes the first. A recorded loss abandons what was pending and the depth is unread until the device ends.
// - The context records its instance and device, and every owner its fences and swapchains, as NativeGpuObject in the
//   lifetime ledger; all of them end with their owner. Queues are not created objects on Vulkan (vkGetDeviceQueue), so
//   they are not counted here, unlike DX12.
// - The shutdown workload: cycles of create (instance, device, compute context), submit and destroy, each cycle's
//   ledger window Balanced and every value read back the one written.
// Without a Vulkan device (the hosted Windows lanes) the context still creates and destroys its instance, and that
// failed start must balance too; the device halves report that they did not run.

#include "vulkan_execution.hpp" // src-private: the completion seam's injected loss

#include <crd/core/budget_pressure.hpp>
#include <crd/core/lifetime_ledger.hpp>
#include <crd/core/workload_probe.hpp>
#include <crd/gpu/compute.hpp>
#include <crd/gpu/frame_graph.hpp>
#include <crd/gpu/queue_pressure.hpp>
#include <crd/gpu/raster_context.hpp>
#include <crd/gpu/vulkan_compute_context.hpp>
#include <crd/gpu/vulkan_context.hpp>
#include <crd/gpu/vulkan_raster_context.hpp>
#include <crd/memory/allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <win32_test_window.hpp>

#include <cstdio>
#include <cstring>
#include <memory>

namespace
{
namespace g = crd::gpu;
namespace d = crd::gpu::detail;
using crd::LifetimeClass;
using crd::LifetimeVerdict;

constexpr crd::u64 kSecond = 1000ULL * 1000ULL * 1000ULL;

std::unique_ptr<g::IGpuContext> make_context()
{
    g::GpuContextConfig cfg;
    cfg.backend  = g::GpuBackend::Vulkan;
    cfg.headless = true;
    return g::create_vulkan_gpu_context(cfg);
}

crd::LifetimeReport since(const crd::LifetimeSnapshot& start)
{
    return crd::lifetime_report(start, crd::lifetime_ledger().snapshot());
}

// A context that could not start must still have given back what it created on the way. Returns whether it is usable.
bool usable_or_balanced(std::unique_ptr<g::IGpuContext>& ctx, const crd::LifetimeSnapshot& start, const char* what)
{
    if (ctx != nullptr && ctx->valid())
    {
        return true;
    }
    ctx.reset();
    CHECK(since(start).verdict == LifetimeVerdict::Balanced);
    WARN("no Vulkan device: " << what << " did not run (the failed start balanced)");
    return false;
}

bool round_trip(g::IComputeContext& compute, crd::u32 value, crd::u32& out)
{
    auto source      = compute.create_buffer(sizeof(crd::u32), 0U, g::ComputeMemory::CpuToGpu);
    auto destination = compute.create_buffer(sizeof(crd::u32), 0U, g::ComputeMemory::GpuToCpu);
    if (source == nullptr || destination == nullptr)
    {
        return false;
    }
    void* in = source->map();
    if (in == nullptr)
    {
        return false;
    }
    std::memcpy(in, &value, sizeof(value));
    source->unmap();
    compute.begin().copy(*source, *destination, 0U, 0U, sizeof(crd::u32));
    compute.submit_and_wait();
    if (!compute.valid())
    {
        return false;
    }
    const void* back = destination->map();
    if (back == nullptr)
    {
        return false;
    }
    std::memcpy(&out, back, sizeof(out));
    destination->unmap();
    return true;
}

void record_nothing(g::IFrameContext& /*ctx*/, void* /*user*/) {}

// The shutdown workload: each cycle creates a context and a compute context on it, round-trips values, destroys both.
struct VulkanShutdown
{
    std::unique_ptr<g::IGpuContext>          ctx;
    std::unique_ptr<g::VulkanComputeContext> compute;
    crd::u32                                 not_drained = 0U;
    crd::u64                                 submitted   = 0U;
};

bool vulkan_setup(void* user, crd::u32 /*cycle*/) noexcept
{
    auto* w = static_cast<VulkanShutdown*>(user);
    w->ctx  = make_context();
    if (w->ctx == nullptr || !w->ctx->valid())
    {
        return false;
    }
    w->compute = std::make_unique<g::VulkanComputeContext>(*static_cast<g::VulkanGpuContext*>(w->ctx.get()),
                                                           crd::memory::default_allocator());
    return w->compute->valid();
}

void vulkan_iterate(void* user, crd::u32 cycle, crd::u32 iteration, bool /*injected*/,
                    crd::WorkloadIteration& out) noexcept
{
    auto*                     w      = static_cast<VulkanShutdown*>(user);
    const g::QueueDepthCounts before = w->ctx->queue_pressure().counts;
    const crd::u32            value  = 0x7E570000U + cycle * 64U + iteration;
    crd::u32                  back   = 0U;
    out.ok                           = round_trip(*w->compute, value, back);
    out.signature                    = back;
    out.expected                     = value;
    out.expected_known               = true;
    const g::QueueDepthCounts after  = w->ctx->queue_pressure().counts;
    if (after.in_flight != 0U)
    {
        ++w->not_drained;
    }
    w->submitted += after.submitted - before.submitted;
}

void vulkan_teardown(void* user, crd::u32 /*cycle*/) noexcept
{
    auto* w = static_cast<VulkanShutdown*>(user);
    w->compute.reset(); // before the device it was created on
    w->ctx.reset();
}
} // namespace

TEST_CASE("diag10c: a Vulkan context holds its instance and device, a compute context its fence, until they end",
          "[vulkan][gpu][lifetime][queue][diag10c]")
{
    const crd::LifetimeSnapshot start = crd::lifetime_ledger().snapshot();
    auto                        ctx   = make_context();
    if (!usable_or_balanced(ctx, start, "the instance, device and fence holds"))
    {
        return;
    }
    CHECK(since(start).live_change(LifetimeClass::NativeGpuObject) == 2); // its instance and its device
    const g::QueueDepthReading idle = ctx->queue_pressure();
    INFO(idle.reason);
    REQUIRE(idle.depth.read);
    CHECK(idle.counts.submitted == 0U); // a fresh device: no seam has queued anything on it yet
    {
        const crd::LifetimeSnapshot with_ctx = crd::lifetime_ledger().snapshot();
        g::VulkanComputeContext compute(*static_cast<g::VulkanGpuContext*>(ctx.get()),
                                        crd::memory::default_allocator());
        REQUIRE(compute.valid());
        CHECK(since(with_ctx).live_change(LifetimeClass::NativeGpuObject) == 1); // its fence
        crd::u32 back = 0U;
        REQUIRE(round_trip(compute, 0x1234ABCDU, back));
        CHECK(back == 0x1234ABCDU);
        REQUIRE(round_trip(compute, 0x4321DCBAU, back));
        CHECK(back == 0x4321DCBAU);
        const g::QueueDepthReading after = ctx->queue_pressure();
        REQUIRE(after.depth.read);
        CHECK(after.counts.submitted == 2U);
        CHECK(after.counts.retired == 2U);
        CHECK(after.counts.in_flight == 0U);
        CHECK(after.counts.abandoned == 0U);
        CHECK(after.counts.high_water == 1U); // each submission was observed before the next was queued
        CHECK(after.counts.queues == 1U);
        CHECK(crd::pressure_level(after.depth, after.depth.in_use) == crd::PressureLevel::Unbounded);
    }
    ctx.reset();
    const crd::LifetimeReport gone = since(start);
    char                      line[160];
    (void)crd::format_lifetime_report(gone, line, sizeof(line));
    INFO(line);
    CHECK(gone.verdict == LifetimeVerdict::Balanced);
}

TEST_CASE("diag10c: Vulkan one-shot and deferred frame-graph submissions are in flight until a seam observes them",
          "[vulkan][gpu][frame-graph][queue][diag10c]")
{
    const crd::LifetimeSnapshot start = crd::lifetime_ledger().snapshot();
    auto                        ctx   = make_context();
    if (!usable_or_balanced(ctx, start, "the frame-graph depth half"))
    {
        return;
    }
    auto* const vk     = static_cast<g::VulkanGpuContext*>(ctx.get());
    auto        raster = g::create_vulkan_raster_context(*vk);
    if (raster == nullptr || !raster->valid())
    {
        raster.reset();
        ctx.reset();
        CHECK(since(start).verdict == LifetimeVerdict::Balanced);
        WARN("no graphics-capable Vulkan device: the frame-graph depth half did not run");
        return;
    }
    {
        auto target = raster->create_color_target(32U, 32U);
        REQUIRE(target != nullptr);
        // A clear is a one-shot submission without a fence, observed by its queue idle wait.
        const g::QueueDepthCounts before_clear = ctx->queue_pressure().counts;
        raster->clear(*target, g::ClearColor{1.0F, 0.0F, 0.0F, 1.0F});
        const g::QueueDepthCounts after_clear = ctx->queue_pressure().counts;
        CHECK(after_clear.submitted > before_clear.submitted);
        CHECK(after_clear.retired - before_clear.retired == after_clear.submitted - before_clear.submitted);
        CHECK(after_clear.in_flight == 0U);

        {
            auto graph = raster->create_frame_graph();
            REQUIRE(graph != nullptr);
            graph->set_readback_enabled(false); // the deferred wait: a frame is observed when its slot is reused
            for (crd::u32 frame = 0U; frame < 4U; ++frame)
            {
                graph->reset();
                const g::FgImage out = graph->import_target(*target);
                graph->add_pass("paint").writes(out).execute(&record_nothing, nullptr);
                REQUIRE(graph->build());
                graph->execute();
                REQUIRE(raster->valid());
                const g::QueueDepthReading r = ctx->queue_pressure();
                REQUIRE(r.depth.read);
                INFO("frame " << frame);
                // This frame is in flight, and so is the one before it (its slot is not reused yet): never more.
                CHECK(r.counts.in_flight == (frame == 0U ? 1U : 2U));
            }
            const g::QueueDepthCounts open = ctx->queue_pressure().counts;
            CHECK(open.high_water == 2U); // the ring's two slots bound the depth
            CHECK(open.submitted == after_clear.submitted + 4U);
        }
        // The graph is gone: its teardown observed every slot it had pending.
        const g::QueueDepthCounts end = ctx->queue_pressure().counts;
        CHECK(end.in_flight == 0U);
        CHECK(end.abandoned == 0U);
        CHECK(end.retired == end.submitted);
    }
    raster.reset();
    ctx.reset();
    CHECK(since(start).verdict == LifetimeVerdict::Balanced);
}

TEST_CASE("diag10c: Vulkan shutdown workload: contexts created, used and destroyed leave nothing behind",
          "[vulkan][gpu][workload][lifetime][queue][diag10c]")
{
    {
        const crd::LifetimeSnapshot start = crd::lifetime_ledger().snapshot();
        auto                        probe = make_context();
        if (!usable_or_balanced(probe, start, "the shutdown workload"))
        {
            return;
        }
    }
    VulkanShutdown    w;
    crd::WorkloadSpec spec;
    spec.name       = "vulkan-compute-shutdown";
    spec.cycles     = 3U;
    spec.iterations = 4U;
    spec.stressors.set(crd::Stressor::Cycles, spec.cycles);
    spec.stressors.set(crd::Stressor::Iterations, spec.iterations);
    spec.user     = &w;
    spec.setup    = &vulkan_setup;
    spec.iterate  = &vulkan_iterate;
    spec.teardown = &vulkan_teardown;
    spec.stall_ns = 20U * kSecond; // a hang bound only: instance and device creation on a software device is slow
    const g::QueueDepthCounts process_0 = g::queue_depth_process();
    auto                      result    = std::make_unique<crd::WorkloadResult>();
    crd::run_workload(spec, *result);

    char line[256];
    (void)crd::format_workload_result(spec.name, *result, line, sizeof(line));
    std::printf("[diag10c vulkan] %s\n", line);
    INFO(line);
    CHECK(result->verdict == crd::WorkloadVerdict::Clean);
    CHECK(result->cycles_run == 3U);
    CHECK(result->iterations_run == 12U);
    CHECK(result->worst_cycle.verdict == LifetimeVerdict::Balanced);
    CHECK(w.not_drained == 0U);
    CHECK(w.submitted == 12U);
    const g::QueueDepthCounts process_1 = g::queue_depth_process();
    CHECK(process_1.abandoned == process_0.abandoned);
    CHECK(process_1.in_flight == process_0.in_flight);
}

TEST_CASE("diag10c: an injected Vulkan loss abandons the pending submissions and the depth reads as lost",
          "[vulkan][gpu][device-loss][queue][diag10c]")
{
    const crd::LifetimeSnapshot start = crd::lifetime_ledger().snapshot();
    auto                        ctx   = make_context();
    if (!usable_or_balanced(ctx, start, "the lost-queue half"))
    {
        return;
    }
    auto* const vk     = static_cast<g::VulkanGpuContext*>(ctx.get());
    auto        raster = g::create_vulkan_raster_context(*vk);
    if (raster == nullptr || !raster->valid())
    {
        raster.reset();
        ctx.reset();
        CHECK(since(start).verdict == LifetimeVerdict::Balanced);
        WARN("no graphics-capable Vulkan device: the lost-queue half did not run");
        return;
    }
    {
        auto target = raster->create_color_target(32U, 32U);
        auto graph  = raster->create_frame_graph();
        REQUIRE(target != nullptr);
        REQUIRE(graph != nullptr);
        graph->set_readback_enabled(false);
        g::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
        REQUIRE(compute.valid());

        const g::FgImage out = graph->import_target(*target);
        graph->add_pass("paint").writes(out).execute(&record_nothing, nullptr);
        REQUIRE(graph->build());
        graph->execute();
        const g::QueueDepthReading pending = ctx->queue_pressure();
        REQUIRE(pending.depth.read);
        const crd::u64 in_flight = pending.counts.in_flight;
        REQUIRE(in_flight >= 1U);

        d::vk_inject_next_result(VK_ERROR_DEVICE_LOST);
        crd::u32 back = 0U;
        CHECK_FALSE(round_trip(compute, 9U, back));
        CHECK_FALSE(compute.valid());

        const g::QueueDepthReading lost = ctx->queue_pressure();
        CHECK_FALSE(lost.depth.read);
        CHECK(lost.counts.lost);
        CHECK(std::strstr(lost.reason, "lost") != nullptr);
        CHECK(lost.counts.in_flight == 0U);
        CHECK(lost.counts.abandoned == in_flight);
        CHECK(lost.counts.retired == pending.counts.retired);

        graph->reset(); // a wait on the lost device observes nothing
        CHECK(ctx->queue_pressure().counts.retired == pending.counts.retired);
    }
    raster.reset();
    ctx.reset();
    CHECK(since(start).verdict == LifetimeVerdict::Balanced);
}

TEST_CASE("diag10c: a Vulkan present surface holds its swapchain and fence ring until it ends",
          "[vulkan][gpu][present][lifetime][diag10c]")
{
    const crd::LifetimeSnapshot start = crd::lifetime_ledger().snapshot();
    g::GpuContextConfig         cfg;
    cfg.backend  = g::GpuBackend::Vulkan;
    cfg.headless = false;
    auto ctx     = g::create_vulkan_gpu_context(cfg);
    if (!usable_or_balanced(ctx, start, "the swapchain half"))
    {
        return;
    }
    auto* const vk     = static_cast<g::VulkanGpuContext*>(ctx.get());
    auto        raster = g::create_vulkan_raster_context(*vk);
    void*       native = nullptr;
    const bool  window = raster != nullptr && raster->valid() && vk->present_capable() && !vk->headless_surface();
    if (window)
    {
        native = crd::gputest::create_test_window(256U, 256U);
    }
    if (raster == nullptr || !raster->valid() || !vk->present_capable() || (window && native == nullptr))
    {
        raster.reset();
        ctx.reset();
        CHECK(since(start).verdict == LifetimeVerdict::Balanced);
        WARN("no present-capable Vulkan device or surface: the swapchain half did not run");
        return;
    }
    {
        const crd::LifetimeSnapshot before = crd::lifetime_ledger().snapshot();
        auto surface = raster->create_present_surface(native, 256U, 256U, g::PresentMode::Fifo);
        REQUIRE(surface != nullptr);
        REQUIRE(surface->valid());
        // Its swapchain and one fence per frame of its two-frame present ring.
        CHECK(since(before).live_change(LifetimeClass::NativeGpuObject) == 3);
        auto target = raster->create_color_target(256U, 256U);
        REQUIRE(target != nullptr);
        raster->clear(*target, g::ClearColor{0.0F, 0.0F, 1.0F, 1.0F});
        CHECK(surface->present(*target));
        CHECK(surface->present(*target));
        if (native != nullptr)
        {
            crd::gputest::pump_test_window();
        }
    }
    if (native != nullptr)
    {
        crd::gputest::destroy_test_window(native);
    }
    raster.reset();
    ctx.reset();
    CHECK(since(start).verdict == LifetimeVerdict::Balanced);
}
