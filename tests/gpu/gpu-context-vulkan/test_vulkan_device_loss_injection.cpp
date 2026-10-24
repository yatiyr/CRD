// DIAG.9b: device-loss simulation through the GPU fault seam. An armed FaultInjector, installed on crd-core's GPU seam,
// answers LoseDevice at the plan's submission: the Vulkan completion seam queues nothing, records VK_ERROR_DEVICE_LOST
// labelled Simulated and takes the DIAG.7c loss path (fault report, loss bundle), while the injector records the
// fault into the flight recorder and triggers it with a recipe whose one-hit plan reproduces the loss alone. A real
// loss stays the hardware gate (DIAG.7c(h)); nothing here is presented as one.
//
// A shipping build compiles the seam out: arming is Unavailable, the hook cannot be installed and every submission
// runs. That lane asserts the refusal instead.
//
// Contract: docs/design/runtime-diagnostics.md (DIAG.9b); ADR-0133.

#include "vulkan_execution.hpp" // the completion seam's loss record (src-private)

#include <crd/core/crash.hpp>
#include <crd/core/fault_seam.hpp>
#include <crd/gpu/vulkan_compute_context.hpp>
#include <crd/gpu/vulkan_context.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/perf/diag_commands.hpp>
#include <crd/perf/fault_injection.hpp>
#include <crd/perf/flight_recorder.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>
#if defined(_WIN32)
#include <process.h> // _getpid: a per-process scratch directory
#endif

namespace
{
namespace gpu  = crd::gpu;
namespace d    = crd::gpu::detail;
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

// One empty compute submission; whether the context is still valid afterwards.
bool submit_once(gpu::VulkanComputeContext& compute)
{
    (void)compute.begin();
    compute.submit_and_wait();
    return compute.valid();
}

#if defined(_WIN32)
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
#endif
} // namespace

TEST_CASE("DIAG.9b: an injected device loss takes the Vulkan loss path and is captured with its recipe",
          "[gpu-context][vulkan][gpu][device-loss][diag9b][fault]")
{
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto first   = gpu::create_vulkan_gpu_context(cfg);
    if (first == nullptr || !first->valid())
    {
        SKIP("no Vulkan device");
    }
    auto* const       vk = static_cast<gpu::VulkanGpuContext*>(first.get());
    cp::FaultInjector injector;

    if (!crd::fault_seams_compiled())
    {
        // Shipping: nothing can be armed or installed, and every submission reaches the driver.
        CHECK(injector.arm(plan_of("seed=21;device-loss@0+0%1000000"), kInject) == cp::FaultArmStatus::Unavailable);
        CHECK_FALSE(crd::install_gpu_fault_hook(&cp::FaultInjector::gpu_hook, &injector));
        gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
        REQUIRE(compute.valid());
        for (crd::u32 i = 0U; i < 3U; ++i)
        {
            CHECK(submit_once(compute));
        }
        CHECK_FALSE(d::vk_device_failure(vk->vk_device()).lost());
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

#if defined(_WIN32)
    namespace fs = std::filesystem;
    char leaf[64];
    (void)std::snprintf(leaf, sizeof(leaf), "crd_vk_fault_loss_%d", _getpid());
    const fs::path  dir = fs::temp_directory_path() / leaf;
    std::error_code ec;
    fs::remove_all(dir, ec);
    REQUIRE(crd::crash::InstallResult::Ok == crd::crash::install(dir.string().c_str()));
#else
    crd::crash::uninstall(); // Linux keeps the loss in process; the write reports Unsupported
#endif

    const VkDevice device = vk->vk_device();
    {
        gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
        REQUIRE(compute.valid());
        REQUIRE(injector.arm(plan_of("seed=21;device-loss@1+1%1000000"), kInject) == cp::FaultArmStatus::Armed);
        REQUIRE(crd::install_gpu_fault_hook(&cp::FaultInjector::gpu_hook, &injector));

        // Submission 0 reaches the driver and completes.
        recorder.record(kFrame, cp::Severity::Info, cont::StringView{"frame"}, 0U, 0U);
        CHECK(submit_once(compute));
        CHECK_FALSE(d::vk_device_failure(device).lost());

        // Submission 1 loses the device: nothing is queued, the context latches invalid.
        recorder.record(kFrame, cp::Severity::Info, cont::StringView{"frame"}, 1U, 0U);
        CHECK_FALSE(submit_once(compute));
        const d::VkDeviceFailure lost = d::vk_device_failure(device);
        CHECK(lost.origin == d::VkFailureOrigin::Simulated);
        CHECK(lost.loss_origin == d::VkFailureOrigin::Simulated);
        CHECK(lost.first_result == static_cast<crd::i32>(VK_ERROR_DEVICE_LOST));
        REQUIRE(lost.operation != nullptr);
        CHECK(std::strcmp(lost.operation, "compute submit") == 0);
        CHECK(lost.failures == 1U);

        // A later submission on the lost device is answered without the driver and is not a device-loss hit.
        VkSubmitInfo empty{};
        empty.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        CHECK(d::vk_submit(device, vk->compute_queue(), empty, VK_NULL_HANDLE, "later submit") == VK_ERROR_DEVICE_LOST);
        CHECK(d::vk_device_failure(device).failures == 2U);
        CHECK(injector.hits(cp::FaultSite::DeviceLoss) == 2U);
        CHECK(injector.fired(cp::FaultSite::DeviceLoss) == 1U);
    }

    // The DIAG.7c loss record (and on Windows its bundle) carries the injected loss, labelled Simulated.
    const d::VkLossRecord loss = d::vk_last_loss();
    CHECK(loss.loss_origin == d::VkFailureOrigin::Simulated);
    CHECK(std::strcmp(loss.loss_operation, "compute submit") == 0);
    CHECK(std::strcmp(loss.adapter.name, vk->adapter_name()) == 0);
#if defined(_WIN32)
    CHECK(loss.bundle_result == static_cast<crd::u32>(crd::crash::WriteResult::Ok));
    crd::u32       bundles     = 0U;
    const fs::path bundle_path = only_dump_with_prefix(dir, "gpu_", bundles);
    CHECK(bundles == 1U);
    REQUIRE_FALSE(bundle_path.empty());
    d::VkLossRecord read{};
    REQUIRE(d::vk_read_loss_bundle(reinterpret_cast<const char*>(bundle_path.u8string().c_str()), read) ==
            d::VkBundleRead::Ok);
    CHECK(read.loss_origin == d::VkFailureOrigin::Simulated);
    CHECK(std::strcmp(read.loss_operation, "compute submit") == 0);
    crd::crash::uninstall();
    fs::remove_all(dir, ec);
#else
    CHECK(loss.bundle_result == static_cast<crd::u32>(crd::crash::WriteResult::Unsupported));
#endif

    // The flight capture: both frames before the lost submission and the fault itself, the InjectedFault trigger at
    // the site and hit, and a recipe that leads with the one-hit plan.
    recorder.record(kFrame, cp::Severity::Info, cont::StringView{"after"}, 100U, 0U);
    recorder.record(kFrame, cp::Severity::Info, cont::StringView{"after"}, 101U, 0U); // fills the post window
    cp::FlightCapture c;
    REQUIRE(recorder.take_capture(c));
    CHECK(c.trigger.kind == cp::FlightTrigger::InjectedFault);
    CHECK(c.trigger.code == cp::kFlightCodeFaultInjected);
    CHECK(c.trigger.value_a == static_cast<crd::u64>(cp::FaultSite::DeviceLoss));
    CHECK(c.trigger.value_b == 1U);
    REQUIRE(c.pre_retained == 3U);
    CHECK(c.events[0].code == kFrame);
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
    CHECK(minimal == "seed=21;device-loss@1+1%1000000");
    injector.disarm();

    // The one-hit plan reproduces the loss alone, at the same submission of a fresh device.
    auto second = gpu::create_vulkan_gpu_context(cfg);
    REQUIRE(second != nullptr);
    REQUIRE(second->valid());
    auto* const    vk_second     = static_cast<gpu::VulkanGpuContext*>(second.get());
    const VkDevice second_device = vk_second->vk_device();
    {
        gpu::VulkanComputeContext compute(*vk_second, crd::memory::default_allocator());
        REQUIRE(compute.valid());
        REQUIRE(injector.arm(plan_of(minimal), kInject) == cp::FaultArmStatus::Armed);
        CHECK(submit_once(compute));
        CHECK_FALSE(submit_once(compute));
        CHECK(d::vk_device_failure(second_device).loss_origin == d::VkFailureOrigin::Simulated);
        CHECK(injector.fired(cp::FaultSite::DeviceLoss) == 1U);
    }
    injector.disarm();
    crd::uninstall_gpu_fault_hook();

    // Recovery is a new device: with the hook removed, a fresh context submits and completes.
    auto third = gpu::create_vulkan_gpu_context(cfg);
    REQUIRE(third != nullptr);
    REQUIRE(third->valid());
    auto* const vk_third = static_cast<gpu::VulkanGpuContext*>(third.get());
    {
        gpu::VulkanComputeContext compute(*vk_third, crd::memory::default_allocator());
        REQUIRE(compute.valid());
        CHECK(submit_once(compute));
        CHECK(submit_once(compute));
        CHECK_FALSE(d::vk_device_failure(vk_third->vk_device()).lost());
    }
    recorder.shutdown();
}
