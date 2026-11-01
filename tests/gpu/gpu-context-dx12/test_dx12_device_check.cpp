// HW-RESILIENCE.3 -- the DX12 device check on a real device (crd/gpu/device_check.hpp).
//
// Every DX12 device opened with no named adapter (Dx12DeviceScope::create: the GPU, compute, raster, ray-tracing and
// work-graph contexts) runs the known-answer kernel (crd/kir/ckir_device_check.hpp) once per adapter on a probe
// device of its own, so the device a context receives has queued nothing; the same check on a compute context
// reproduces the answers and names the part a wrong input breaks; an adapter the device-check fault seam fails is
// passed over for the next adapter, WARP last (Demoted), or for no device (NoDevicePassed, the consumers' CPU path),
// with the startup line naming it. Runs on the real GPU here and on WARP on the hosted Windows lanes.

#include "dx12_adapter_classification.hpp" // src-private: the evidence-based adapter kind

#include <crd/core/fault_seam.hpp>
#include <crd/gpu/device_check.hpp>
#include <crd/gpu/dx12_compute_context.hpp>
#include <crd/gpu/dx12_context.hpp>
#include <crd/gpu/dx12_raster_context.hpp>
#include <crd/gpu/dx12_validation_capture.hpp>
#include <crd/gpu/queue_pressure.hpp>
#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_device_check.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstring>
#include <dxgi1_4.h>
#include <string_view>
#include <wrl/client.h>

namespace
{
namespace g = crd::gpu;
namespace kir = crd::kir;

bool fail_named(void* user, const char* backend, const char* name) noexcept
{
    return std::strcmp(backend, "dx12") == 0 && std::strcmp(name, static_cast<const char*>(user)) == 0;
}

bool fail_every_dx12(void* /*user*/, const char* backend, const char* /*name*/) noexcept
{
    return std::strcmp(backend, "dx12") == 0;
}

// A scratch stream the report writes to (MSVC deprecates std::tmpfile for tmpfile_s).
std::FILE* scratch_file()
{
    std::FILE* f = nullptr;
    return tmpfile_s(&f) == 0 ? f : nullptr;
}

// What the report wrote to `out`, which it closes.
std::string_view read_back(std::FILE* out, char* text, crd::usize bytes)
{
    REQUIRE(std::fseek(out, 0, SEEK_SET) == 0);
    const crd::usize n = std::fread(text, 1U, bytes - 1U, out);
    (void)std::fclose(out);
    return std::string_view{text, n};
}

// What DXGI offers, read independently of the chooser: the adapters it lists (WARP added when it is not among them)
// and whether any of them is hardware.
struct DxgiAdapters
{
    crd::u32 distinct = 0U;
    bool hardware = false;
};

DxgiAdapters dxgi_adapters()
{
    using Microsoft::WRL::ComPtr;
    DxgiAdapters out;
    ComPtr<IDXGIFactory4> factory;
    REQUIRE(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
    LUID listed[32] = {}; // NOLINT(modernize-avoid-c-arrays)
    crd::u32 count = 0U;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0U; count < 32U && SUCCEEDED(factory->EnumAdapters1(i, &adapter)); ++i)
    {
        DXGI_ADAPTER_DESC1 desc{};
        REQUIRE(SUCCEEDED(adapter->GetDesc1(&desc)));
        listed[count++] = desc.AdapterLuid;
        const g::Dx12AdapterKind kind =
            g::detail::query_dx12_adapter_kind(desc.AdapterLuid.LowPart, desc.AdapterLuid.HighPart);
        out.hardware = out.hardware || kind == g::Dx12AdapterKind::Hardware;
        adapter.Reset();
    }
    out.distinct = count;
    ComPtr<IDXGIAdapter1> warp;
    if (SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))))
    {
        DXGI_ADAPTER_DESC1 desc{};
        REQUIRE(SUCCEEDED(warp->GetDesc1(&desc)));
        bool known = false;
        for (crd::u32 i = 0U; i < count; ++i)
        {
            const bool same =
                listed[i].LowPart == desc.AdapterLuid.LowPart && listed[i].HighPart == desc.AdapterLuid.HighPart;
            known = known || same;
        }
        out.distinct += known ? 0U : 1U;
    }
    return out;
}

// Every case starts from an unchecked process and leaves no listener, report or hook behind.
struct Fresh
{
    Fresh() { g::device_checks().forget(); }
    ~Fresh()
    {
        g::report_device_demotions(nullptr);
        crd::uninstall_device_check_fault_hook();
        g::device_checks().forget();
    }
    Fresh(const Fresh&) = delete;
    Fresh& operator=(const Fresh&) = delete;
    Fresh(Fresh&&) = delete;
    Fresh& operator=(Fresh&&) = delete;
};

// The kernel's answer on `compute` from `input` (nullptr pipeline: "build").
const char* run_kernel(g::Dx12ComputeContext& compute, const float* input, bool build, crd::memory::IAllocator* alloc)
{
    kir::KGraph graph(alloc);
    const kir::KEntry entry = kir::build_device_check_kernel(graph);
    std::unique_ptr<g::ComputePipeline> pipeline;
    if (build)
    {
        pipeline = compute.create_pipeline_from_ckir(graph, entry, kir::kDeviceCheckBindings, 0U, alloc);
        REQUIRE(pipeline != nullptr);
    }
    g::DeviceCheckProgram program;
    program.input = crd::containers::ConstSpan<float>(input, kir::kDeviceCheckLanes);
    program.floats = kir::kDeviceCheckFloats;
    program.words = kir::kDeviceCheckWords;
    program.workgroups = kir::kDeviceCheckGroups;
    program.verify = &kir::device_check_mismatch;
    return g::run_device_check(compute, pipeline.get(), program);
}
} // namespace

TEST_CASE("hw-resilience 3: a DX12 adapter passes its device check once, on a probe device of its own",
          "[dx12][gpu][hw-resilience]")
{
    const Fresh fresh;
    crd::memory::GrowableTlsfAllocator alloc;
    auto ctx = g::create_dx12_gpu_context(&alloc);
    REQUIRE(g::device_checks().check_count() >= 1U);
    const g::DeviceCheck check = g::device_checks().check_at(0U);
    INFO((check.failed != nullptr ? check.failed : "passed"));
    REQUIRE(ctx != nullptr); // DXGI always lists an adapter (WARP at the least): the preferred one must pass
    CHECK(g::device_checks().check_count() == 1U);
    CHECK(check.failed == nullptr);
    CHECK(std::string_view{check.adapter.name} == ctx->adapter_name());
    CHECK(std::string_view{check.adapter.backend} == "dx12");
    CHECK(check.nanoseconds > 0U);
    std::printf("[hw-resilience 3] dx12 device check on %s: %.3f ms\n", ctx->adapter_name(),
                static_cast<double>(check.nanoseconds) / 1.0e6);

    REQUIRE(g::device_checks().selection_count() == 1U);
    const g::DeviceSelection selection = g::device_checks().selection_at(0U);
    if (check.unchecked != nullptr)
    {
        // A host with no dxc cannot build the kernel: the device runs, reported unchecked.
        WARN("device check could not run: " << check.unchecked);
        CHECK(selection.status == g::DeviceCheckStatus::Unchecked);
    }
    else
    {
        CHECK(selection.status == g::DeviceCheckStatus::Passed);
    }
    CHECK(selection.adapters_failed == 0U);
    CHECK(std::string_view{selection.runs.name} == ctx->adapter_name());
    if (dxgi_adapters().hardware)
    {
        // Hardware comes before every software adapter (WARP, the Basic Render Driver).
        CHECK(g::dx12_default_adapter_kind() == g::Dx12AdapterKind::Hardware);
    }

    // The check ran on the probe: the returned device has queued nothing.
    const g::QueueDepthReading idle = ctx->queue_pressure();
    INFO(idle.reason);
    REQUIRE(idle.depth.read);
    CHECK(idle.counts.submitted == 0U);

    // Every other context opens the same checked adapter and checks nothing again.
    g::Dx12ComputeContext compute(&alloc);
    REQUIRE(compute.valid());
    CHECK(std::string_view{compute.adapter().name} == ctx->adapter_name());
    auto raster = g::create_dx12_raster_context(&alloc);
    REQUIRE(raster != nullptr);
    CHECK(g::device_checks().check_count() == 1U);
    REQUIRE(g::device_checks().selection_count() == 1U);
    CHECK(g::device_checks().selection_at(0U).status == selection.status);
}

TEST_CASE("hw-resilience 3: the DX12 device check's probe runs clean under the debug layer",
          "[dx12][gpu][hw-resilience][validation]")
{
    const Fresh fresh;
    crd::memory::TlsfAllocator allocator(64U << 20U, nullptr, "DX12 device check validation test");
    g::Dx12ValidationCapture capture(&allocator);
    REQUIRE(capture.report().readiness == g::Dx12ValidationReadiness::Ready);
    {
        auto ctx = g::create_dx12_gpu_context(&allocator);
        REQUIRE(ctx != nullptr);
    }
    REQUIRE(g::device_checks().check_count() == 1U);
    const g::DeviceCheck check = g::device_checks().check_at(0U);
    CHECK(check.failed == nullptr);
    if (check.unchecked != nullptr)
    {
        WARN("device check could not run: " << check.unchecked);
        return;
    }
    // The probe device and the context's own: two devices, every message of both read, none raised.
    const g::Dx12ValidationReport report = capture.report();
    CHECK(report.contexts_started == 2U);
    CHECK(report.contexts_finished == 2U);
    CHECK(report.errors == 0U);
    CHECK(report.warnings == 0U);
    CHECK(report.complete_and_silent());
}

TEST_CASE("hw-resilience 3: the device check reproduces its answers on a DX12 device and names a wrong one",
          "[dx12][gpu][hw-resilience]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    g::Dx12ComputeContext compute(&alloc);
    REQUIRE(compute.valid());
    float input[kir::kDeviceCheckLanes] = {};
    for (crd::u32 i = 0U; i < kir::kDeviceCheckLanes; ++i)
    {
        input[i] = kir::device_check_input(i);
    }
    const char* const sound = run_kernel(compute, input, true, &alloc);
    INFO((sound != nullptr ? sound : "passed"));
    CHECK(sound == nullptr);

    // One input off by one: the lane, its neighbour's exchange and its workgroup's sum all move; the first named is
    // the arithmetic.
    input[3] += 1.0F;
    const char* const wrong = run_kernel(compute, input, true, &alloc);
    REQUIRE(wrong != nullptr);
    CHECK(std::string_view{wrong} == "arithmetic");
    input[3] -= 1.0F;

    const char* const unbuilt = run_kernel(compute, input, false, &alloc);
    REQUIRE(unbuilt != nullptr);
    CHECK(std::string_view{unbuilt} == "build");
}

TEST_CASE("hw-resilience 3: a DX12 adapter that fails its device check is passed over and named at startup",
          "[dx12][gpu][hw-resilience]")
{
    const Fresh fresh;
    crd::memory::GrowableTlsfAllocator alloc;
    char preferred[sizeof(g::GpuAdapter::name)] = {}; // NOLINT(modernize-avoid-c-arrays)
    {
        auto first = g::create_dx12_gpu_context(&alloc);
        REQUIRE(first != nullptr);
        (void)std::snprintf(preferred, sizeof(preferred), "%s", first->adapter_name());
    }
    g::device_checks().forget();
    if (!crd::fault_seams_compiled())
    {
        CHECK_FALSE(crd::install_device_check_fault_hook(&fail_named, preferred));
        return;
    }
    REQUIRE(crd::install_device_check_fault_hook(&fail_named, preferred));
    std::FILE* const out = scratch_file();
    REQUIRE(out != nullptr);
    g::report_device_demotions(out);

    auto ctx = g::create_dx12_gpu_context(&alloc);
    REQUIRE(g::device_checks().selection_count() == 1U);
    const g::DeviceSelection selection = g::device_checks().selection_at(0U);
    CHECK(std::string_view{selection.selected.name} == preferred);
    REQUIRE(selection.failed != nullptr);
    CHECK(std::string_view{selection.failed} == "injected");
    CHECK(selection.injected);
    CHECK(selection.adapters_failed >= 1U);
    if (ctx != nullptr)
    {
        // Another adapter passed (WARP at the latest, when it is not the adapter that failed): the process runs there.
        CHECK(selection.status == g::DeviceCheckStatus::Demoted);
        CHECK(std::string_view{ctx->adapter_name()} != preferred);
        CHECK(std::string_view{selection.runs.name} == ctx->adapter_name());
        // The compute context follows the selection.
        g::Dx12ComputeContext compute(&alloc);
        REQUIRE(compute.valid());
        CHECK(std::string_view{compute.adapter().name} == ctx->adapter_name());
        for (crd::u32 i = 0U; i < g::device_checks().check_count(); ++i)
        {
            const g::DeviceCheck check = g::device_checks().check_at(i);
            std::printf("[hw-resilience 3] dx12 check of %s (ordinal %u): %s, %.3f ms\n", check.adapter.name,
                        check.ordinal, check.failed != nullptr ? check.failed : "passed",
                        static_cast<double>(check.nanoseconds) / 1.0e6);
        }
    }
    else
    {
        // No other adapter (a host whose only adapter is WARP): no device, which every consumer takes as its CPU path.
        CHECK(selection.status == g::DeviceCheckStatus::NoDevicePassed);
        CHECK_FALSE(selection.runs.known);
    }

    g::report_device_demotions(nullptr);
    char text[1024] = {}; // NOLINT(modernize-avoid-c-arrays)
    const std::string_view written = read_back(out, text, sizeof(text));
    char expected[512] = {}; // NOLINT(modernize-avoid-c-arrays)
    (void)std::snprintf(expected, sizeof(expected),
                        "[crd] GPU device path dx12 on %s failed its known answers in injected (fault seam); it runs ",
                        preferred);
    INFO(written);
    CHECK(written.find(expected) == 0U);
}

TEST_CASE("hw-resilience 3: when no DX12 adapter passes its device check, every context has no device",
          "[dx12][gpu][hw-resilience]")
{
    const Fresh fresh;
    if (!crd::fault_seams_compiled())
    {
        CHECK_FALSE(crd::install_device_check_fault_hook(&fail_every_dx12, nullptr));
        return;
    }
    REQUIRE(crd::install_device_check_fault_hook(&fail_every_dx12, nullptr));
    std::FILE* const out = scratch_file();
    REQUIRE(out != nullptr);
    g::report_device_demotions(out);

    crd::memory::GrowableTlsfAllocator alloc;
    CHECK(g::create_dx12_gpu_context(&alloc) == nullptr);
    CHECK(g::create_dx12_raster_context(&alloc) == nullptr);
    g::Dx12ComputeContext compute(&alloc);
    CHECK_FALSE(compute.valid());

    // Every adapter DXGI lists, and WARP when it lists no WARP, was tried once and failed.
    const crd::u32 checks = g::device_checks().check_count();
    CHECK(checks == dxgi_adapters().distinct);
    for (crd::u32 i = 0U; i < checks; ++i)
    {
        const g::DeviceCheck check = g::device_checks().check_at(i);
        CHECK(check.injected);
        CHECK(std::string_view{check.adapter.backend} == "dx12");
    }
    REQUIRE(g::device_checks().selection_count() == 1U);
    const g::DeviceSelection selection = g::device_checks().selection_at(0U);
    CHECK(selection.status == g::DeviceCheckStatus::NoDevicePassed);
    CHECK(selection.adapters_failed == checks);
    CHECK_FALSE(selection.runs.known);

    g::report_device_demotions(nullptr);
    char text[1024] = {}; // NOLINT(modernize-avoid-c-arrays)
    const std::string_view written = read_back(out, text, sizeof(text));
    INFO(written);
    CHECK(written.find("[crd] GPU device path dx12 on ") == 0U);
    CHECK(written.find("; it runs no device (no-device-passed, ") != std::string_view::npos);
}
