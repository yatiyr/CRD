// DIAG.10c: device and host pressure evidence and the driver-call mode witness on DX12.
// Declared contract (docs/design/runtime-diagnostics.md, DIAG.10c):
// - IGpuContext::device_memory reads DXGI's per-process budget and usage of the device's adapter (local and non-local
//   segment groups). The oracle is memory the test commits itself on a device of its own on the same adapter: the
//   process's usage must grow by at least that much, and fall back once it is released. Run on the default adapter and
//   on WARP, the software adapter the hosted Windows runners have.
// - The frame graph reports its transient budget as a reading: unbounded at the default, the footprint and its high
//   water from the graph's own counters, and every build the budget refused counted once.
// - Device creation, dxc, ExecuteCommandLists and Present run under the mode witness: each moves its kind's count.
// - A recorded removal carries the host and device pressure taken at it, with the removal's own sequence; an injected
//   removal attributes to Injected, never to the engine or the hardware.

#include "dx12_device_memory.hpp" // src-private: the DXGI reading and the loss pressure
#include "dx12_dred.hpp"          // src-private: the removal record

#include <crd/core/budget_pressure.hpp>
#include <crd/core/fault_seam.hpp>
#include <crd/core/fp_environment.hpp>
#include <crd/gpu/device_pressure.hpp>
#include <crd/gpu/driver_witness.hpp>
#include <crd/gpu/dx12_compute_context.hpp>
#include <crd/gpu/dx12_context.hpp>
#include <crd/gpu/dx12_raster_context.hpp>
#include <crd/gpu/frame_graph.hpp>
#include <crd/gpu/raster_context.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/perf/diag_commands.hpp>
#include <crd/perf/fault_injection.hpp>

#include <catch2/catch_test_macros.hpp>

#include <win32_test_window.hpp>

#include <cstdio>
#include <memory>

#include <dxgi1_4.h>
#include <wrl/client.h>

namespace
{
namespace g  = crd::gpu;
namespace d  = crd::gpu::detail;
namespace cp = crd::perf;
using Microsoft::WRL::ComPtr;

constexpr crd::u64 kCommitBytes = crd::u64{256} << 20U;

// The process's device usage over both segment groups (an adapter may count a committed buffer in either).
crd::u64 usage(const g::DeviceMemoryReading& r)
{
    return (r.local.read ? r.local.in_use : 0U) + (r.non_local.read ? r.non_local.in_use : 0U);
}

// A committed default-heap buffer of `bytes` on `device`.
ComPtr<ID3D12Resource> commit_buffer(ID3D12Device* device, crd::u64 bytes)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width            = bytes;
    desc.Height           = 1U;
    desc.DepthOrArraySize = 1U;
    desc.MipLevels        = 1U;
    desc.SampleDesc.Count = 1U;
    desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> resource;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                                               nullptr, IID_PPV_ARGS(&resource))))
    {
        return nullptr;
    }
    return resource;
}

// The reading of `device`'s adapter moves by at least a buffer committed on it, and back when it is released.
void check_usage_follows_commit(ID3D12Device* device, const char* which)
{
    INFO(which);
    const g::DeviceMemoryReading before = d::dx12_device_memory(device);
    INFO(before.reason);
    REQUIRE(before.source == g::DeviceMemorySource::Dxgi);
    REQUIRE((before.local.read || before.non_local.read));
    for (const crd::BudgetReading* r : {&before.local, &before.non_local})
    {
        if (r->read)
        {
            CHECK(r->bounded);
            CHECK_FALSE(r->high_water_tracked);
            // An adapter without a segment group answers a zero budget; that segment is absent, never read as full.
            CHECK(r->capacity > 0U);
        }
    }
    crd::u64 during_usage = 0U;
    {
        ComPtr<ID3D12Resource> buffer = commit_buffer(device, kCommitBytes);
        REQUIRE(buffer != nullptr);
        const g::DeviceMemoryReading during = d::dx12_device_memory(device);
        during_usage                        = usage(during);
        std::printf("[diag10c dx12 %s] local %llu/%llu non-local %llu/%llu, usage %llu -> %llu\n", which,
                    static_cast<unsigned long long>(during.local.in_use),
                    static_cast<unsigned long long>(during.local.capacity),
                    static_cast<unsigned long long>(during.non_local.in_use),
                    static_cast<unsigned long long>(during.non_local.capacity),
                    static_cast<unsigned long long>(usage(before)), static_cast<unsigned long long>(during_usage));
        CHECK(during_usage >= usage(before) + kCommitBytes);
    }
    const g::DeviceMemoryReading after = d::dx12_device_memory(device);
    CHECK(usage(after) + kCommitBytes <= during_usage);
}

void record_nothing(g::IFrameContext& /*ctx*/, void* /*user*/) {}

// Two equal side x side transients written by two passes, recorded and built once; whether the build succeeded.
bool build_two_transients(g::IFrameGraph& graph, crd::u32 side)
{
    g::FgImageDesc desc{};
    desc.width  = side;
    desc.height = side;
    desc.format = g::FgImageFormat::RGBA8Unorm;
    const g::FgImage x = graph.create_transient_image(desc);
    const g::FgImage y = graph.create_transient_image(desc);
    REQUIRE(x.valid());
    REQUIRE(y.valid());
    graph.add_pass("a").writes(x).execute(&record_nothing, nullptr);
    graph.add_pass("b").writes(y).execute(&record_nothing, nullptr);
    return graph.build();
}

crd::u64 calls_of(const g::DriverWitnessReport& r, g::DriverCall c)
{
    return r.calls[static_cast<crd::usize>(c)];
}

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
} // namespace

TEST_CASE("diag10c: DX12 device memory follows memory the process commits, on the default adapter and WARP",
          "[dx12][gpu][pressure][diag10c]")
{
    auto ctx = g::create_dx12_gpu_context();
    if (ctx == nullptr || !ctx->valid())
    {
        SKIP("no D3D12 device");
    }
    // The context's own reading is DXGI's, for its adapter.
    const g::DeviceMemoryReading own = ctx->device_memory();
    INFO(own.reason);
    CHECK(own.source == g::DeviceMemorySource::Dxgi);
    CHECK(own.local.read);

    ComPtr<ID3D12Device> device;
    REQUIRE(SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))));
    check_usage_follows_commit(device.Get(), "default adapter");

    ComPtr<IDXGIFactory4> factory;
    REQUIRE(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
    ComPtr<IDXGIAdapter> warp;
    ComPtr<ID3D12Device> warp_device;
    if (FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)))
        || FAILED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&warp_device))))
    {
        WARN("no WARP adapter on this system: the software-adapter half did not run");
        return;
    }
    check_usage_follows_commit(warp_device.Get(), "WARP");

    // A null device is not read and says so.
    const g::DeviceMemoryReading none = d::dx12_device_memory(nullptr);
    CHECK(none.source == g::DeviceMemorySource::Unavailable);
    CHECK_FALSE(none.local.read);
    CHECK(none.reason[0] != '\0');
}

TEST_CASE("diag10c: the DX12 frame graph reports its transient budget as a reading", "[dx12][gpu][pressure][diag10c]")
{
    auto ctx    = g::create_dx12_gpu_context();
    auto raster = g::create_dx12_raster_context();
    if (ctx == nullptr || !ctx->valid() || raster == nullptr || !raster->valid())
    {
        SKIP("no D3D12 device");
    }
    // 2048 x 2048 RGBA8: 16 MiB a transient, so a leaked build is far above the process's other device traffic.
    constexpr crd::u32 side = 2048U;
    auto               graph = raster->create_frame_graph();
    REQUIRE(graph != nullptr);
    const crd::BudgetReading fresh = graph->transient_pressure();
    CHECK(fresh.read);
    CHECK(fresh.in_use == 0U);
    REQUIRE(build_two_transients(*graph, side));
    const crd::BudgetReading open = graph->transient_pressure();
    CHECK(open.read);
    CHECK_FALSE(open.bounded); // no budget set: reported, never judged
    CHECK(crd::pressure_level(open, open.in_use) == crd::PressureLevel::Unbounded);
    CHECK(open.in_use == graph->transient_memory_bytes());
    CHECK(open.in_use >= crd::u64{side} * side * 4U); // the two disjoint transients share one slot
    CHECK(open.high_water_tracked);
    CHECK(open.high_water == open.in_use);
    CHECK(open.refusals == 0U);
    const crd::u64 footprint = open.in_use;

    const g::DeviceMemoryReading start = ctx->device_memory();
    REQUIRE(start.source == g::DeviceMemorySource::Dxgi);
    {
        // A budget one byte under the footprint refuses the build, and each refused build counts once.
        auto tight = raster->create_frame_graph();
        REQUIRE(tight != nullptr);
        tight->set_memory_budget(footprint - 1U);
        const crd::BudgetReading before = tight->transient_pressure();
        CHECK(before.bounded);
        CHECK(before.capacity == footprint - 1U);
        CHECK_FALSE(build_two_transients(*tight, side));
        CHECK(tight->last_build_exceeded_budget());
        const crd::BudgetReading refused = tight->transient_pressure();
        CHECK(refused.refusals == 1U);
        CHECK(refused.high_water == footprint); // the refused build's footprint was planned and allocated
        const crd::PressureReport report = crd::pressure_report(before, refused);
        CHECK(report.verdict == crd::PressureVerdict::Exhausted);
        CHECK(report.refused == 1U);

        // One build per recording: raising the budget does not make the same recording build again. The second
        // build is refused for that, not for the budget, and does not count as a budget refusal.
        tight->set_memory_budget(footprint);
        CHECK_FALSE(tight->build());
        CHECK_FALSE(tight->last_build_exceeded_budget());
        CHECK(tight->transient_pressure().refusals == 1U);
        CHECK(tight->transient_pressure().in_use == footprint); // the first build's transients, not a second set

        // The recovery an author takes: reset, record again, build within the raised budget.
        tight->reset();
        CHECK(tight->transient_pressure().in_use == 0U);
        REQUIRE(build_two_transients(*tight, side));
        const crd::BudgetReading rebuilt = tight->transient_pressure();
        CHECK(rebuilt.refusals == 1U);
        CHECK(rebuilt.in_use == footprint);
        CHECK(rebuilt.high_water == footprint);
        CHECK(crd::pressure_level(rebuilt, rebuilt.in_use) == crd::PressureLevel::Full);
    }
    // The graph is gone and so is every build's memory, as the OS counts it: a second build of one recording used to
    // place its transients over the first build's and leak them, with the device behind them (a footprint a build).
    const g::DeviceMemoryReading end = ctx->device_memory();
    std::printf("[diag10c dx12 frame graph] footprint %llu, usage %llu -> %llu\n",
                static_cast<unsigned long long>(footprint), static_cast<unsigned long long>(usage(start)),
                static_cast<unsigned long long>(usage(end)));
    CHECK(usage(end) < usage(start) + footprint / 2U);
}

TEST_CASE("diag10c: DX12 device creation, dxc, submission and present run under the mode witness",
          "[dx12][gpu][fp-mode][diag10c]")
{
    const crd::FpEnvironment     start  = crd::read_fp_environment();
    const g::DriverWitnessReport before = g::driver_witness();
    crd::memory::TlsfAllocator   allocator(16U << 20U, nullptr, "DX12 10c witness test");

    g::Dx12ComputeContext compute(&allocator);
    if (!compute.valid())
    {
        SKIP("no D3D12 device");
    }
    const g::DriverWitnessReport created = g::driver_witness();
    CHECK(calls_of(created, g::DriverCall::CreateDevice) > calls_of(before, g::DriverCall::CreateDevice));

    const g::DxilCompileResult dxil = g::compile_hlsl_to_dxil(
        g::ShaderStage::Compute, "RWByteAddressBuffer o : register(u0); [numthreads(1,1,1)] void cs_main() {}",
        "diag10c", &allocator);
    if (!dxil.ok)
    {
        WARN("dxc unavailable: " << dxil.error_message.c_str());
    }
    else
    {
        CHECK(calls_of(g::driver_witness(), g::DriverCall::CompileShader)
              > calls_of(created, g::DriverCall::CompileShader));
    }

    auto source      = compute.create_buffer(sizeof(crd::u32), 0U, g::ComputeMemory::CpuToGpu);
    auto destination = compute.create_buffer(sizeof(crd::u32), 0U, g::ComputeMemory::GpuToCpu);
    REQUIRE(source != nullptr);
    REQUIRE(destination != nullptr);
    const g::DriverWitnessReport compiled = g::driver_witness();
    compute.begin().copy(*source, *destination, 0U, 0U, sizeof(crd::u32));
    compute.submit_and_wait();
    REQUIRE(compute.valid());
    CHECK(calls_of(g::driver_witness(), g::DriverCall::Submit) > calls_of(compiled, g::DriverCall::Submit));

    auto raster = g::create_dx12_raster_context();
    REQUIRE(raster != nullptr);
    REQUIRE(raster->valid());
    void* hwnd = crd::gputest::create_test_window(64U, 64U);
    if (hwnd == nullptr)
    {
        WARN("no platform window: the present half did not run");
    }
    else
    {
        auto surface = raster->create_present_surface(hwnd, 64U, 64U, g::PresentMode::Fifo);
        REQUIRE(surface != nullptr);
        auto target = raster->create_color_target(64U, 64U);
        REQUIRE(target != nullptr);
        raster->clear(*target, g::ClearColor{0.0F, 0.0F, 1.0F, 1.0F});
        const g::DriverWitnessReport ready = g::driver_witness();
        CHECK(surface->present(*target));
        crd::gputest::pump_test_window();
        CHECK(calls_of(g::driver_witness(), g::DriverCall::Present) == calls_of(ready, g::DriverCall::Present) + 1U);
        surface.reset(); // the surface dies before its window
        crd::gputest::destroy_test_window(hwnd);
    }

    // Whatever a driver did, the thread's mode is the one it had; a divergence, if one happened, names its call.
    CHECK(crd::fp_differences(crd::read_fp_environment(), start) == 0U);
    const g::DriverWitnessReport end = g::driver_witness();
    std::printf("[diag10c dx12 witness] divergences %llu (first: %s)\n",
                static_cast<unsigned long long>(end.total_divergences()),
                end.first_operation != nullptr ? end.first_operation : "none");
    if (end.total_divergences() != 0U)
    {
        CHECK(end.first_operation != nullptr);
    }
}

TEST_CASE("diag10c: a recorded DX12 removal carries the pressure taken at it", "[dx12][gpu][pressure][diag10c]")
{
    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
    {
        SKIP("no D3D12 device");
    }
    // The recording path itself, on every lane: a removal recorded for a device that was never removed.
    d::dx12_record_removal(device.Get(), d::Dx12RemovalOrigin::Simulated);
    const d::Dx12RemovalRecord removal  = d::dx12_last_removal();
    const g::GpuLossPressure   pressure = d::dx12_last_loss_pressure();
    CHECK(pressure.taken);
    CHECK(pressure.loss_sequence == removal.sequence);
    CHECK(pressure.host.physical.read);
    CHECK(pressure.host.resident.read);
    CHECK(pressure.device.source == g::DeviceMemorySource::Dxgi);
    CHECK(pressure.device.local.read);

    g::DeviceFaultEvidence evidence;
    evidence.failure  = g::DeviceFailureKind::Simulated;
    evidence.pressure = pressure;
    CHECK(g::attribute_device_fault(evidence) == g::FaultAttribution::Injected);
}

TEST_CASE("diag10c: an injected DX12 device loss records its pressure and attributes to Injected",
          "[dx12][gpu][pressure][device-loss][diag10c]")
{
    crd::memory::TlsfAllocator allocator(16U << 20U, nullptr, "DX12 10c injected loss");
    cp::FaultInjector          injector;
    if (!crd::fault_seams_compiled())
    {
        // Shipping compiles the seam out: nothing can be armed, so no injected loss exists to attribute.
        CHECK(injector.arm(plan_of("seed=10;device-loss@0+0%1000000"), cp::authority_bit(cp::DiagAuthority::Inject))
              == cp::FaultArmStatus::Unavailable);
        return;
    }
    const GpuHookGuard    hook_guard;
    g::Dx12ComputeContext compute(&allocator);
    if (!compute.valid())
    {
        SKIP("no D3D12 device");
    }
    auto source      = compute.create_buffer(sizeof(crd::u32), 0U, g::ComputeMemory::CpuToGpu);
    auto destination = compute.create_buffer(sizeof(crd::u32), 0U, g::ComputeMemory::GpuToCpu);
    REQUIRE(source != nullptr);
    REQUIRE(destination != nullptr);
    const crd::u64 removals_before = d::dx12_last_removal().sequence;
    REQUIRE(injector.arm(plan_of("seed=10;device-loss@0+1%1000000"), cp::authority_bit(cp::DiagAuthority::Inject))
            == cp::FaultArmStatus::Armed);
    REQUIRE(crd::install_gpu_fault_hook(&cp::FaultInjector::gpu_hook, &injector));
    compute.begin().copy(*source, *destination, 0U, 0U, sizeof(crd::u32));
    compute.submit_and_wait();
    CHECK_FALSE(compute.valid());

    const d::Dx12RemovalRecord removal = d::dx12_last_removal();
    REQUIRE(removal.sequence == removals_before + 1U);
    REQUIRE(removal.origin == d::Dx12RemovalOrigin::Simulated);
    const g::GpuLossPressure pressure = d::dx12_last_loss_pressure();
    CHECK(pressure.taken);
    CHECK(pressure.loss_sequence == removal.sequence);
    CHECK(pressure.host.physical.read);
    CHECK(pressure.device.source == g::DeviceMemorySource::Dxgi); // the adapter answers after the device is removed

    g::DeviceFaultEvidence evidence;
    evidence.failure      = g::DeviceFailureKind::Simulated;
    evidence.reproduction = g::Reproduction::OtherConfiguration; // even a "reproduction" cannot make it real
    evidence.pressure     = pressure;
    CHECK(g::attribute_device_fault(evidence) == g::FaultAttribution::Injected);
}
