// DIAG.9b: device-loss simulation through the GPU fault seam on DX12. An armed FaultInjector, installed on crd-core's
// GPU seam, answers LoseDevice at the plan's submission: dx12_submit queues nothing, removes the device through
// ID3D12Device5::RemoveDevice (so every later call meets a removed device, as after a real loss), reads DRED and
// records the removal labelled Simulated, which the DIAG.7b removal bundle carries. The injector records the fault into
// the flight recorder and triggers it with a recipe whose one-hit plan reproduces the loss alone. A real device fault
// stays the hardware gate (DIAG.7b(h2)); nothing here is presented as one. On a hosted runner this runs on WARP.
//
// A shipping build compiles the seam out: arming is Unavailable, the hook cannot be installed and every submission
// runs. That lane asserts the refusal instead.
//
// Contract: docs/design/runtime-diagnostics.md (DIAG.9b); ADR-0133.

#include <crd/core/crash.hpp>
#include <crd/core/fault_seam.hpp>
#include <crd/gpu/dx12_compute_context.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/perf/diag_commands.hpp>
#include <crd/perf/fault_injection.hpp>
#include <crd/perf/flight_recorder.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>
#include <memory>

#include "dx12_dred.hpp" // the removal record and bundle reader (src-private)

namespace
{
namespace g    = crd::gpu;
namespace cp   = crd::perf;
namespace cont = crd::containers;

constexpr cp::DiagAuthoritySet kInject = cp::authority_bit(cp::DiagAuthority::Inject);
constexpr crd::u32             kFrame  = 0x0042U;

cp::FaultPlan plan_of(cont::StringView text)
{
    cp::FaultPlan plan;
    cont::String  reason;
    const bool    parsed = cp::parse_fault_plan(text, plan, reason);
    INFO(reason.c_str());
    REQUIRE(parsed);
    return plan;
}

// Uninstalls the GPU hook on every exit, so a failed REQUIRE never leaves the seam pointing at a dead injector.
struct GpuHookGuard
{
    GpuHookGuard()                               = default;
    GpuHookGuard(const GpuHookGuard&)            = delete;
    GpuHookGuard& operator=(const GpuHookGuard&) = delete;
    GpuHookGuard(GpuHookGuard&&)                 = delete;
    GpuHookGuard& operator=(GpuHookGuard&&)      = delete;
    ~GpuHookGuard() { crd::uninstall_gpu_fault_hook(); }
};

// One compute submission (a 4-byte copy); whether the context is still valid afterwards.
bool submit_once(g::Dx12ComputeContext& compute, g::ComputeBuffer& source, g::ComputeBuffer& destination)
{
    compute.begin().copy(source, destination, 0U, 0U, sizeof(crd::u32));
    compute.submit_and_wait();
    return compute.valid();
}

[[nodiscard]] std::filesystem::path only_dump_with_prefix(const std::filesystem::path& dir, const char* prefix,
                                                          crd::u32& count)
{
    std::filesystem::path found;
    count = 0U;
    std::error_code ec;
    for (std::filesystem::directory_iterator it{dir, ec}, end; it != end; it.increment(ec))
    {
        if (it->path().extension() == ".dmp" && it->path().filename().string().starts_with(prefix))
        {
            found = it->path();
            ++count;
        }
    }
    return found;
}

// A compute context with a source and a destination buffer for submit_once.
struct CopyRig
{
    explicit CopyRig(crd::memory::IAllocator* allocator) : compute(allocator)
    {
        if (compute.valid())
        {
            source      = compute.create_buffer(sizeof(crd::u32), 0U, g::ComputeMemory::CpuToGpu);
            destination = compute.create_buffer(sizeof(crd::u32), 0U, g::ComputeMemory::GpuToCpu);
        }
    }
    [[nodiscard]] bool ready() const { return compute.valid() && source != nullptr && destination != nullptr; }
    bool               submit() { return submit_once(compute, *source, *destination); }

    g::Dx12ComputeContext             compute;
    std::unique_ptr<g::ComputeBuffer> source;
    std::unique_ptr<g::ComputeBuffer> destination;
};
} // namespace

TEST_CASE("DIAG.9b: an injected device loss removes the DX12 device and is captured with its recipe",
          "[dx12][device-loss][diag9b][fault]")
{
    using g::detail::Dx12RemovalOrigin;
    crd::memory::TlsfAllocator allocator(16U << 20U, nullptr, "DX12 9b device-loss test");
    cp::FaultInjector          injector;

    if (!crd::fault_seams_compiled())
    {
        // Shipping: nothing can be armed or installed, and every submission reaches the device.
        CHECK(injector.arm(plan_of("seed=22;device-loss@0+0%1000000"), kInject) == cp::FaultArmStatus::Unavailable);
        CHECK_FALSE(crd::install_gpu_fault_hook(&cp::FaultInjector::gpu_hook, &injector));
        CopyRig rig(&allocator);
        if (!rig.ready())
        {
            SKIP("no D3D12 device");
        }
        for (crd::u32 i = 0U; i < 3U; ++i)
        {
            CHECK(rig.submit());
        }
        return;
    }

    cp::FlightPolicy policy;
    policy.capacity       = 64U;
    policy.pre_events     = 32U;
    policy.post_events    = 2U;
    policy.post_window_ns = 0U;
    cp::FlightRecorder recorder;
    REQUIRE(recorder.init(policy) == cp::FlightPolicyStatus::Committed);
    injector.attach_flight_recorder(&recorder);
    const GpuHookGuard hook_guard;

    namespace fs = std::filesystem;
    char leaf[64];
    (void)std::snprintf(leaf, sizeof(leaf), "crd_dx12_fault_loss_%lu",
                        static_cast<unsigned long>(GetCurrentProcessId()));
    const fs::path  dir = fs::temp_directory_path() / leaf;
    std::error_code ec;
    fs::remove_all(dir, ec);

    const crd::u64 removals_before = g::detail::dx12_last_removal().sequence;
    {
        CopyRig rig(&allocator);
        if (!rig.ready())
        {
            SKIP("no D3D12 device");
        }
        REQUIRE(crd::crash::InstallResult::Ok == crd::crash::install(dir.string().c_str()));
        REQUIRE(injector.arm(plan_of("seed=22;device-loss@1+1%1000000"), kInject) == cp::FaultArmStatus::Armed);
        REQUIRE(crd::install_gpu_fault_hook(&cp::FaultInjector::gpu_hook, &injector));

        // Submission 0 reaches the device and completes.
        recorder.record(kFrame, cp::Severity::Info, cont::StringView{"frame"}, 0U, 0U);
        CHECK(rig.submit());
        CHECK(g::detail::dx12_last_removal().sequence == removals_before);

        // Submission 1 removes the device before anything is queued; the context latches invalid.
        recorder.record(kFrame, cp::Severity::Info, cont::StringView{"frame"}, 1U, 0U);
        CHECK_FALSE(rig.submit());
        const g::detail::Dx12RemovalRecord removal = g::detail::dx12_last_removal();
        CHECK(removal.sequence == removals_before + 1U);
        CHECK(removal.origin == Dx12RemovalOrigin::Simulated);
        CHECK(removal.dred.removal_reason == static_cast<crd::i32>(DXGI_ERROR_DEVICE_REMOVED));
        CHECK(removal.bundle_result == static_cast<crd::u32>(crd::crash::WriteResult::Ok));

        // The latched context submits nothing more, so a later submission is not a device-loss hit.
        CHECK_FALSE(rig.submit());
        CHECK(injector.hits(cp::FaultSite::DeviceLoss) == 2U);
        CHECK(injector.fired(cp::FaultSite::DeviceLoss) == 1U);
        crd::crash::uninstall();
    }

    // The DIAG.7b removal bundle carries the injected removal, labelled Simulated.
    crd::u32       bundles     = 0U;
    const fs::path bundle_path = only_dump_with_prefix(dir, "gpu_", bundles);
    CHECK(bundles == 1U);
    REQUIRE_FALSE(bundle_path.empty());
    g::detail::Dx12RemovalRecord read{};
    REQUIRE(g::detail::dx12_read_removal_bundle(reinterpret_cast<const char*>(bundle_path.u8string().c_str()), read) ==
            g::detail::Dx12BundleRead::Ok);
    CHECK(read.origin == Dx12RemovalOrigin::Simulated);
    CHECK(read.sequence == removals_before + 1U);
    CHECK(read.dred.removal_reason == static_cast<crd::i32>(DXGI_ERROR_DEVICE_REMOVED));
    fs::remove_all(dir, ec);

    // The flight capture: both frames before the lost submission and the fault itself, the InjectedFault trigger at
    // the site and hit, and a recipe that leads with the one-hit plan.
    recorder.record(kFrame, cp::Severity::Info, cont::StringView{"after"}, 100U, 0U);
    recorder.record(kFrame, cp::Severity::Info, cont::StringView{"after"}, 101U, 0U); // fills the post window
    cp::FlightCapture c;
    REQUIRE(recorder.take_capture(c));
    CHECK(c.trigger.kind == cp::FlightTrigger::InjectedFault);
    CHECK(c.trigger.value_a == static_cast<crd::u64>(cp::FaultSite::DeviceLoss));
    CHECK(c.trigger.value_b == 1U);
    REQUIRE(c.pre_retained == 3U);
    CHECK(c.events[0].value_a == 0U);
    CHECK(c.events[1].value_a == 1U);
    CHECK(c.events[2].code == cp::kFlightCodeFaultInjected);
    CHECK_FALSE(c.post_truncated);
    REQUIRE(c.replay_available);
    const cont::StringView recipe{c.replay.data(), c.replay.size()};
    const cont::StringView lead{"fault-plan/1 fired=device-loss#1 minimal="};
    REQUIRE(recipe.find(lead) == 0U);
    const crd::usize minimal_end = recipe.find(' ', lead.size());
    REQUIRE(minimal_end != cont::StringView::npos);
    const cont::StringView minimal = recipe.substr(lead.size(), minimal_end - lead.size());
    CHECK(minimal == "seed=22;device-loss@1+1%1000000");
    injector.disarm();

    // The one-hit plan reproduces the removal alone, at the same submission of a fresh device (the removed one was
    // released with its context, so the adapter's next device is a new one).
    {
        CopyRig rig(&allocator);
        REQUIRE(rig.ready());
        REQUIRE(injector.arm(plan_of(minimal), kInject) == cp::FaultArmStatus::Armed);
        CHECK(rig.submit());
        CHECK_FALSE(rig.submit());
        CHECK(g::detail::dx12_last_removal().origin == Dx12RemovalOrigin::Simulated);
        CHECK(g::detail::dx12_last_removal().sequence == removals_before + 2U);
        CHECK(injector.fired(cp::FaultSite::DeviceLoss) == 1U);
    }
    injector.disarm();
    crd::uninstall_gpu_fault_hook();

    // Recovery is a new device: with the hook removed, a fresh context submits, completes and copies.
    {
        CopyRig rig(&allocator);
        REQUIRE(rig.ready());
        auto* mapped = static_cast<crd::u32*>(rig.source->map());
        REQUIRE(mapped != nullptr);
        *mapped = 0x9B10557U;
        rig.source->unmap();
        CHECK(rig.submit());
        mapped = static_cast<crd::u32*>(rig.destination->map());
        REQUIRE(mapped != nullptr);
        CHECK(*mapped == 0x9B10557U);
        rig.destination->unmap();
        CHECK(g::detail::dx12_last_removal().sequence == removals_before + 2U);
    }
    recorder.shutdown();
}
