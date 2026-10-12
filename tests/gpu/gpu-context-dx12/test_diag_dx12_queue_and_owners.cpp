// DIAG.10c: GPU queue depth at the DX12 completion seams, the native owners in the lifetime ledger, and their shutdown.
// Declared contract (docs/design/runtime-diagnostics.md, DIAG.10c):
// - Every fence step dx12_submit or dx12_signal queues is in flight until dx12_wait observes it; the context's
//   queue_pressure() reads the steps of its device's queues (D3D12 hands one device per adapter to the process, so a
//   compute or raster context on the default adapter is counted by the default-adapter context). A synchronous
//   submission leaves nothing in flight; a frame graph that defers its wait leaves its frame in flight until the next
//   execute or reset observes it. A recorded removal abandons what was pending and the depth is unread while the
//   device's queues live.
// - Each DX12 owner records its native objects in the lifetime ledger: a device reference per device scope, a created
//   queue, a fence and a swapchain as NativeGpuObject, and a fence event as OsObject; all of them end with the owner.
// - The shutdown workload: cycles of create, submit and destroy, each cycle's ledger window Balanced and every value
//   read back the one written. The expected counts are worked out from the contexts' own code: a compute context
//   holds one device reference, one queue, one fence and one event; a present surface one swapchain, one fence, one
//   event (its device and queue are the raster context's).
// Runs on the default adapter: WARP on the hosted Windows lanes, the real GPU here.

#include "dx12_execution.hpp" // src-private: the completion seam itself

#include <crd/core/budget_pressure.hpp>
#include <crd/core/fault_seam.hpp>
#include <crd/core/lifetime_ledger.hpp>
#include <crd/core/workload_probe.hpp>
#include <crd/gpu/compute.hpp>
#include <crd/gpu/dx12_compute_context.hpp>
#include <crd/gpu/dx12_context.hpp>
#include <crd/gpu/dx12_raster_context.hpp>
#include <crd/gpu/frame_graph.hpp>
#include <crd/gpu/queue_pressure.hpp>
#include <crd/gpu/raster_context.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/perf/diag_commands.hpp>
#include <crd/perf/fault_injection.hpp>

#include <catch2/catch_test_macros.hpp>

#include <win32_test_window.hpp>

#include <cstdio>
#include <cstring>
#include <memory>

#include <dxgi1_4.h>
#include <wrl/client.h>

namespace
{
namespace g  = crd::gpu;
namespace d  = crd::gpu::detail;
namespace cp = crd::perf;
using Microsoft::WRL::ComPtr;
using crd::LifetimeClass;
using crd::LifetimeVerdict;

constexpr crd::u64 kSecond = 1000ULL * 1000ULL * 1000ULL;

crd::LifetimeReport since(const crd::LifetimeSnapshot& start)
{
    return crd::lifetime_report(start, crd::lifetime_ledger().snapshot());
}

// Write `value` into an upload buffer, copy it to a readback buffer on the GPU, read it back into `out`.
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

cp::FaultPlan plan_of(crd::containers::StringView text)
{
    cp::FaultPlan           plan;
    crd::containers::String reason;
    const bool              parsed = cp::parse_fault_plan(text, plan, reason);
    INFO(reason.c_str());
    REQUIRE(parsed);
    return plan;
}

struct GpuHookGuard
{
    GpuHookGuard()                               = default;
    GpuHookGuard(const GpuHookGuard&)            = delete;
    GpuHookGuard& operator=(const GpuHookGuard&) = delete;
    GpuHookGuard(GpuHookGuard&&)                 = delete;
    GpuHookGuard& operator=(GpuHookGuard&&)      = delete;
    ~GpuHookGuard() { crd::uninstall_gpu_fault_hook(); }
};

// The shutdown workload: each cycle creates a compute context, round-trips values through it and destroys it.
struct ComputeShutdown
{
    crd::memory::IAllocator*               allocator = nullptr;
    const g::IGpuContext*                  owner     = nullptr; // the default-adapter context that reads the depth
    std::unique_ptr<g::Dx12ComputeContext> compute;
    crd::u32                               not_drained = 0U; // iterations after which a step was still in flight
    crd::u64                               submitted   = 0U; // steps the device's reading gained over the run
};

bool compute_setup(void* user, crd::u32 /*cycle*/) noexcept
{
    auto* w    = static_cast<ComputeShutdown*>(user);
    w->compute = std::make_unique<g::Dx12ComputeContext>(w->allocator);
    return w->compute->valid();
}

void compute_iterate(void* user, crd::u32 cycle, crd::u32 iteration, bool /*injected*/,
                     crd::WorkloadIteration& out) noexcept
{
    auto*                     w      = static_cast<ComputeShutdown*>(user);
    const g::QueueDepthCounts before = w->owner->queue_pressure().counts;
    const crd::u32            value  = 0xD1A60000U + cycle * 64U + iteration;
    crd::u32                  back   = 0U;
    out.ok                           = round_trip(*w->compute, value, back);
    out.signature                    = back;
    out.expected                     = value;
    out.expected_known               = true;
    const g::QueueDepthCounts after  = w->owner->queue_pressure().counts;
    if (after.in_flight != 0U)
    {
        ++w->not_drained;
    }
    w->submitted += after.submitted - before.submitted;
}

void compute_teardown(void* user, crd::u32 /*cycle*/) noexcept
{
    static_cast<ComputeShutdown*>(user)->compute.reset();
}
} // namespace

TEST_CASE("diag10c: a DX12 compute context holds its device reference, queue, fence and event, and drains its queue",
          "[dx12][gpu][lifetime][queue][diag10c]")
{
    crd::memory::TlsfAllocator allocator(16U << 20U, nullptr, "DX12 10c owners");
    auto                       ctx = g::create_dx12_gpu_context();
    REQUIRE(ctx != nullptr);
    REQUIRE(ctx->valid());
    const g::QueueDepthReading idle = ctx->queue_pressure();
    INFO(idle.reason);
    REQUIRE(idle.depth.read);

    const crd::LifetimeSnapshot   start     = crd::lifetime_ledger().snapshot();
    const g::QueueDepthCounts     process_0 = g::queue_depth_process();
    {
        g::Dx12ComputeContext compute(&allocator);
        REQUIRE(compute.valid());
        const crd::LifetimeReport held = since(start);
        CHECK(held.live_change(LifetimeClass::NativeGpuObject) == 3); // its device reference, queue and fence
        CHECK(held.live_change(LifetimeClass::OsObject) == 1);        // its fence event

        const g::QueueDepthCounts before = ctx->queue_pressure().counts;
        crd::u32                  back   = 0U;
        REQUIRE(round_trip(compute, 0x10C0FFEEU, back));
        CHECK(back == 0x10C0FFEEU);
        REQUIRE(round_trip(compute, 0x20C0FFEEU, back));
        CHECK(back == 0x20C0FFEEU);
        const g::QueueDepthReading after = ctx->queue_pressure();
        REQUIRE(after.depth.read);
        // Two synchronous submissions: two steps queued, both observed, nothing left in flight.
        CHECK(after.counts.submitted == before.submitted + 2U);
        CHECK(after.counts.retired == before.retired + 2U);
        CHECK(after.counts.abandoned == before.abandoned);
        CHECK(after.counts.in_flight == 0U);
        CHECK(after.counts.high_water >= 1U);
        CHECK(after.counts.queues >= 1U);
        CHECK(crd::pressure_level(after.depth, after.depth.in_use) == crd::PressureLevel::Unbounded);
    }
    // The context is gone: every native object it held is released, and nothing it queued was abandoned.
    const crd::LifetimeReport gone = since(start);
    char                      line[160];
    (void)crd::format_lifetime_report(gone, line, sizeof(line));
    INFO(line);
    CHECK(gone.verdict == LifetimeVerdict::Balanced);
    const g::QueueDepthCounts process_1 = g::queue_depth_process();
    CHECK(process_1.abandoned == process_0.abandoned);
    CHECK(process_1.in_flight == process_0.in_flight);
}

TEST_CASE("diag10c: a DX12 frame graph that defers its wait leaves its frame in flight until it is observed",
          "[dx12][gpu][frame-graph][queue][diag10c]")
{
    auto ctx    = g::create_dx12_gpu_context();
    auto raster = g::create_dx12_raster_context();
    REQUIRE(ctx != nullptr);
    REQUIRE(ctx->valid());
    REQUIRE(raster != nullptr);
    REQUIRE(raster->valid());
    const crd::LifetimeSnapshot start  = crd::lifetime_ledger().snapshot();
    const g::QueueDepthCounts   begin  = ctx->queue_pressure().counts;
    {
        auto target = raster->create_color_target(32U, 32U);
        auto graph  = raster->create_frame_graph();
        REQUIRE(target != nullptr);
        REQUIRE(graph != nullptr);
        graph->set_readback_enabled(false); // the deferred wait: one submission, observed at the next execute/reset
        for (crd::u32 frame = 0U; frame < 3U; ++frame)
        {
            graph->reset();
            // The previous frame's step was observed by the reset: nothing is left in flight.
            CHECK(ctx->queue_pressure().counts.in_flight == 0U);
            const g::FgImage out = graph->import_target(*target);
            graph->add_pass("paint").writes(out).execute(&record_nothing, nullptr);
            REQUIRE(graph->build());
            graph->execute();
            REQUIRE(raster->valid());
            REQUIRE(graph->last_submit_count() == 1U);
            const g::QueueDepthReading r = ctx->queue_pressure();
            REQUIRE(r.depth.read);
            CHECK(r.counts.in_flight >= 1U); // the frame's submission, not yet observed
        }
        graph->reset();
        const g::QueueDepthCounts end = ctx->queue_pressure().counts;
        CHECK(end.in_flight == 0U);
        CHECK(end.submitted >= begin.submitted + 3U);
        CHECK(end.retired == end.submitted - end.abandoned);
        CHECK(end.abandoned == begin.abandoned);
        CHECK(end.high_water >= 1U);
    }
    // The graph and its target are gone; the raster context is still alive and holds what it held before.
    CHECK(since(start).verdict == LifetimeVerdict::Balanced);
}

TEST_CASE("diag10c: a DX12 present surface holds its swapchain, fence and event until it ends",
          "[dx12][gpu][present][lifetime][diag10c]")
{
    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    REQUIRE(raster->valid());
    void* hwnd = crd::gputest::create_test_window(64U, 64U);
    if (hwnd == nullptr)
    {
        // No window station: a surface cannot exist, and creating none holds nothing.
        const crd::LifetimeSnapshot start   = crd::lifetime_ledger().snapshot();
        auto                        surface = raster->create_present_surface(nullptr, 64U, 64U, g::PresentMode::Fifo);
        CHECK((surface == nullptr || !surface->valid()));
        surface.reset();
        CHECK(since(start).verdict == LifetimeVerdict::Balanced);
        WARN("no platform window: the swapchain half did not run");
        return;
    }
    const crd::LifetimeSnapshot start = crd::lifetime_ledger().snapshot();
    {
        auto surface = raster->create_present_surface(hwnd, 64U, 64U, g::PresentMode::Fifo);
        REQUIRE(surface != nullptr);
        REQUIRE(surface->valid());
        const crd::LifetimeReport held = since(start);
        CHECK(held.live_change(LifetimeClass::NativeGpuObject) == 2); // its swapchain and fence
        CHECK(held.live_change(LifetimeClass::OsObject) == 1);        // its fence event
        auto target = raster->create_color_target(64U, 64U);
        REQUIRE(target != nullptr);
        raster->clear(*target, g::ClearColor{0.0F, 1.0F, 0.0F, 1.0F});
        CHECK(surface->present(*target));
        crd::gputest::pump_test_window();
    }
    CHECK(since(start).verdict == LifetimeVerdict::Balanced);
    crd::gputest::destroy_test_window(hwnd);
}

TEST_CASE("diag10c: DX12 shutdown workload: contexts created, used and destroyed leave nothing behind",
          "[dx12][gpu][workload][lifetime][queue][diag10c]")
{
    crd::memory::TlsfAllocator allocator(16U << 20U, nullptr, "DX12 10c shutdown workload");
    auto                       ctx = g::create_dx12_gpu_context();
    REQUIRE(ctx != nullptr);
    REQUIRE(ctx->valid());
    ComputeShutdown w;
    w.allocator = &allocator;
    w.owner     = ctx.get();

    crd::WorkloadSpec spec;
    spec.name       = "dx12-compute-shutdown";
    spec.cycles     = 3U;
    spec.iterations = 4U;
    spec.stressors.set(crd::Stressor::Cycles, spec.cycles);
    spec.stressors.set(crd::Stressor::Iterations, spec.iterations);
    spec.user     = &w;
    spec.setup    = &compute_setup;
    spec.iterate  = &compute_iterate;
    spec.teardown = &compute_teardown;
    spec.stall_ns = 20U * kSecond; // a hang bound only: device creation on a software adapter is slow
    const g::QueueDepthCounts process_0 = g::queue_depth_process();
    auto                      result    = std::make_unique<crd::WorkloadResult>();
    crd::run_workload(spec, *result);

    char line[256];
    (void)crd::format_workload_result(spec.name, *result, line, sizeof(line));
    std::printf("[diag10c dx12] %s\n", line);
    INFO(line);
    CHECK(result->verdict == crd::WorkloadVerdict::Clean);
    CHECK(result->cycles_run == 3U);
    CHECK(result->iterations_run == 12U);
    CHECK(result->worst_cycle.verdict == LifetimeVerdict::Balanced);
    CHECK(w.not_drained == 0U);
    CHECK(w.submitted == 12U); // one step per synchronous submission, each counted on the default-adapter device
    const g::QueueDepthCounts process_1 = g::queue_depth_process();
    CHECK(process_1.abandoned == process_0.abandoned);
    CHECK(process_1.in_flight == process_0.in_flight);
}

TEST_CASE("diag10c: an injected DX12 removal abandons the pending steps and the depth reads as lost",
          "[dx12][gpu][device-loss][queue][diag10c]")
{
    crd::memory::TlsfAllocator allocator(16U << 20U, nullptr, "DX12 10c lost queue");
    cp::FaultInjector          injector;
    if (!crd::fault_seams_compiled())
    {
        // Shipping compiles the seam out: nothing can be armed, so no injected removal exists to observe.
        CHECK(injector.arm(plan_of("seed=10;device-loss@0+0%1000000"), cp::authority_bit(cp::DiagAuthority::Inject))
              == cp::FaultArmStatus::Unavailable);
        return;
    }
    const GpuHookGuard hook_guard;
    auto               ctx = g::create_dx12_gpu_context();
    REQUIRE(ctx != nullptr);
    REQUIRE(ctx->valid());
    const crd::LifetimeSnapshot start = crd::lifetime_ledger().snapshot();
    {
        // The raster context's frame graph defers its wait, so its frame is still pending on the shared device when
        // the compute context's next submission removes that device.
        auto raster = g::create_dx12_raster_context();
        REQUIRE(raster != nullptr);
        REQUIRE(raster->valid());
        auto target = raster->create_color_target(32U, 32U);
        auto graph  = raster->create_frame_graph();
        REQUIRE(target != nullptr);
        REQUIRE(graph != nullptr);
        graph->set_readback_enabled(false);
        g::Dx12ComputeContext compute(&allocator);
        REQUIRE(compute.valid());

        const g::FgImage out = graph->import_target(*target);
        graph->add_pass("paint").writes(out).execute(&record_nothing, nullptr);
        REQUIRE(graph->build());
        graph->execute();
        const g::QueueDepthReading pending = ctx->queue_pressure();
        REQUIRE(pending.depth.read);
        const crd::u64 in_flight = pending.counts.in_flight;
        REQUIRE(in_flight >= 1U);

        REQUIRE(injector.arm(plan_of("seed=10;device-loss@0+1%1000000"),
                             cp::authority_bit(cp::DiagAuthority::Inject))
                == cp::FaultArmStatus::Armed);
        REQUIRE(crd::install_gpu_fault_hook(&cp::FaultInjector::gpu_hook, &injector));
        crd::u32 back = 0U;
        CHECK_FALSE(round_trip(compute, 8U, back));
        CHECK_FALSE(compute.valid());
        crd::uninstall_gpu_fault_hook();

        // The pending frame was abandoned, not completed, and the depth is unread while the device's queues live.
        const g::QueueDepthReading lost = ctx->queue_pressure();
        CHECK_FALSE(lost.depth.read);
        CHECK(lost.counts.lost);
        CHECK(std::strstr(lost.reason, "lost") != nullptr);
        CHECK(lost.counts.in_flight == 0U);
        CHECK(lost.counts.abandoned == pending.counts.abandoned + in_flight);
        CHECK(lost.counts.retired == pending.counts.retired);

        // Waiting on the removed device observes nothing.
        graph->reset();
        CHECK(ctx->queue_pressure().counts.retired == pending.counts.retired);
    }
    // The removed device's last queue is gone, so its counts ended; every native object was still released.
    CHECK(since(start).verdict == LifetimeVerdict::Balanced);
    CHECK_FALSE(ctx->queue_pressure().counts.lost);
}

TEST_CASE("diag10c: a DX12 device's queue counts end when the device itself is destroyed",
          "[dx12][gpu][queue][diag10c]")
{
    // The tracker keys a device by its address, and a later device may be created at the address of one that is gone.
    // A seam that drives a queue directly need not forget it, so the device's own destruction must end its counts: a
    // WARP device of the test's own, one bare signal through the seam, a recorded loss, then every reference released.
    ComPtr<IDXGIFactory4> factory;
    REQUIRE(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
    ComPtr<IDXGIAdapter> warp;
    REQUIRE(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))));
    ComPtr<ID3D12Device> device;
    REQUIRE(SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))));
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    REQUIRE(SUCCEEDED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue))));
    ComPtr<ID3D12Fence> fence;
    REQUIRE(SUCCEEDED(device->CreateFence(0U, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));

    const void* const key   = device.Get();
    UINT64            value = 0U;
    REQUIRE(SUCCEEDED(d::dx12_signal(device.Get(), queue.Get(), fence.Get(), value)));
    g::QueueDepthCounts c = g::queue_depth_of_device(key);
    CHECK(c.submitted == 1U);
    CHECK(c.queues == 1U);
    g::queue_device_lost(key);
    CHECK(g::queue_depth_of_device(key).lost);

    fence.Reset();
    queue.Reset();
    device.Reset();
    warp.Reset();
    // The device is gone: whatever is created at its address next starts with fresh counts, not lost.
    c = g::queue_depth_of_device(key);
    CHECK(c.submitted == 0U);
    CHECK(c.abandoned == 0U);
    CHECK_FALSE(c.lost);
    CHECK(c.queues == 0U);
}
