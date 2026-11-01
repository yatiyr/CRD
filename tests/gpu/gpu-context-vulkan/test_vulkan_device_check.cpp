// HW-RESILIENCE.3 -- the Vulkan device check on a real device (crd/gpu/device_check.hpp).
//
// create_vulkan_gpu_context runs the known-answer kernel (crd/kir/ckir_device_check.hpp) once per adapter on a probe
// device of its own, so the device it returns has queued nothing; the same check on the returned device reproduces
// the answers and names the part a wrong input breaks; and an adapter the device-check fault seam fails is passed over
// for the next adapter (Demoted) or for no device (NoDevicePassed, the consumers' CPU path), with the startup line
// naming it. Device-gated: runs on lavapipe and a real GPU, skips without a Vulkan device.

#include <crd/core/fault_seam.hpp>
#include <crd/gpu/device_check.hpp>
#include <crd/gpu/queue_pressure.hpp>
#include <crd/gpu/vulkan_compute_context.hpp>
#include <crd/gpu/vulkan_context.hpp>
#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_device_check.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace
{
namespace g = crd::gpu;
namespace kir = crd::kir;

std::unique_ptr<g::IGpuContext> make_context()
{
    g::GpuContextConfig cfg;
    cfg.backend = g::GpuBackend::Vulkan;
    cfg.headless = true;
    return g::create_vulkan_gpu_context(cfg);
}

bool fail_named(void* user, const char* backend, const char* name) noexcept
{
    return std::strcmp(backend, "vulkan") == 0 && std::strcmp(name, static_cast<const char*>(user)) == 0;
}

// A scratch stream the report writes to (MSVC deprecates std::tmpfile for tmpfile_s).
std::FILE* scratch_file()
{
#if defined(_MSC_VER)
    std::FILE* f = nullptr;
    return tmpfile_s(&f) == 0 ? f : nullptr;
#else
    return std::tmpfile();
#endif
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
const char* run_kernel(g::VulkanComputeContext& compute, const float* input, bool build, crd::memory::IAllocator* alloc)
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

TEST_CASE("hw-resilience 3: a Vulkan adapter passes its device check once, on a probe device of its own",
          "[vulkan][gpu][hw-resilience]")
{
    const Fresh fresh;
    auto ctx = make_context();
    if (ctx == nullptr && g::device_checks().check_count() == 0U)
    {
        SKIP("no Vulkan device");
    }
    REQUIRE(g::device_checks().check_count() == 1U);
    const g::DeviceCheck check = g::device_checks().check_at(0U);
    INFO((check.failed != nullptr ? check.failed : "passed"));
    REQUIRE(ctx != nullptr); // an adapter was there: it must pass
    INFO((check.failed != nullptr ? check.failed : "passed"));
    CHECK(check.failed == nullptr);
    CHECK(std::string_view{check.adapter.name} == ctx->adapter_name());
    CHECK(std::string_view{check.adapter.backend} == "vulkan");
    CHECK(check.nanoseconds > 0U);
    std::printf("[hw-resilience 3] vulkan device check on %s: %.3f ms\n", ctx->adapter_name(),
                static_cast<double>(check.nanoseconds) / 1.0e6);

    REQUIRE(g::device_checks().selection_count() == 1U);
    const g::DeviceSelection selection = g::device_checks().selection_at(0U);
    if (check.unchecked != nullptr)
    {
        // A host with no shader compiler cannot build the kernel: the device runs, reported unchecked.
        WARN("device check could not run: " << check.unchecked);
        CHECK(selection.status == g::DeviceCheckStatus::Unchecked);
    }
    else
    {
        CHECK(selection.status == g::DeviceCheckStatus::Passed);
    }
    CHECK(selection.adapters_failed == 0U);
    CHECK(std::string_view{selection.runs.name} == ctx->adapter_name());

    // The check ran on the probe: the returned device has queued nothing.
    const g::QueueDepthReading idle = ctx->queue_pressure();
    INFO(idle.reason);
    REQUIRE(idle.depth.read);
    CHECK(idle.counts.submitted == 0U);

    auto second = make_context();
    REQUIRE(second != nullptr);
    CHECK(g::device_checks().check_count() == 1U); // the logged outcome: not checked again
}

TEST_CASE("hw-resilience 3: the device check reproduces its answers on a Vulkan device and names a wrong one",
          "[vulkan][gpu][hw-resilience]")
{
    auto ctx = make_context();
    if (ctx == nullptr)
    {
        SKIP("no Vulkan device");
    }
    crd::memory::GrowableTlsfAllocator alloc;
    g::VulkanComputeContext compute(*static_cast<g::VulkanGpuContext*>(ctx.get()), &alloc);
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

TEST_CASE("hw-resilience 3: a Vulkan adapter that fails its device check is passed over and named at startup",
          "[vulkan][gpu][hw-resilience]")
{
    const Fresh fresh;
    char preferred[sizeof(g::GpuAdapter::name)] = {}; // NOLINT(modernize-avoid-c-arrays)
    {
        auto first = make_context();
        if (first == nullptr)
        {
            SKIP("no Vulkan device");
        }
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

    auto ctx = make_context();
    REQUIRE(g::device_checks().selection_count() == 1U);
    const g::DeviceSelection selection = g::device_checks().selection_at(0U);
    CHECK(std::string_view{selection.selected.name} == preferred);
    REQUIRE(selection.failed != nullptr);
    CHECK(std::string_view{selection.failed} == "injected");
    CHECK(selection.injected);
    CHECK(selection.adapters_failed >= 1U);
    if (ctx != nullptr)
    {
        // Another adapter passed: the process runs there.
        CHECK(selection.status == g::DeviceCheckStatus::Demoted);
        CHECK(std::string_view{ctx->adapter_name()} != preferred);
        CHECK(std::string_view{selection.runs.name} == ctx->adapter_name());
        std::printf("[hw-resilience 3] %s failed by the seam; the process runs %s\n", preferred, ctx->adapter_name());
    }
    else
    {
        // No other adapter: no device, which every consumer takes as its CPU path.
        CHECK(selection.status == g::DeviceCheckStatus::NoDevicePassed);
        CHECK_FALSE(selection.runs.known);
    }

    g::report_device_demotions(nullptr);
    REQUIRE(std::fseek(out, 0, SEEK_SET) == 0);
    char text[1024] = {}; // NOLINT(modernize-avoid-c-arrays)
    const crd::usize n = std::fread(text, 1U, sizeof(text) - 1U, out);
    (void)std::fclose(out);
    char expected[512] = {}; // NOLINT(modernize-avoid-c-arrays)
    (void)std::snprintf(
        expected, sizeof(expected),
        "[crd] GPU device path vulkan on %s failed its known answers in injected (fault seam); it runs ", preferred);
    INFO(text);
    CHECK(std::string_view{text, n}.find(expected) == 0U);
}
