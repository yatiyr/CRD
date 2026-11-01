// HW-RESILIENCE.3 -- the CUDA device check on a real device (crd/gpu/device_check.hpp).
//
// create_cuda_compute_context opens the first device, in CUDA's enumeration order, that passes the known-answer kernel
// (crd/kir/ckir_device_check.hpp through the CUDA C emitter and NVRTC), run once per device per process on a probe
// context of its own; the same check on a context reproduces the answers and names the part a wrong input breaks; a
// device the device-check fault seam fails is passed over for the next device (Demoted) or for no device
// (NoDevicePassed, the consumers' CPU path), with the startup line naming it. NVIDIA hosts only: a clean skip without
// a CUDA device (the hosted lanes have none).

#include <crd/core/fault_seam.hpp>
#include <crd/gpu/cuda_compute_context.hpp>
#include <crd/gpu/device_check.hpp>
#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_cuda.hpp>
#include <crd/kir/ckir_device_check.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstring>
#include <cuda.h>
#include <string_view>

namespace
{
namespace g = crd::gpu;
namespace kir = crd::kir;

bool fail_named(void* user, const char* backend, const char* name) noexcept
{
    return std::strcmp(backend, "cuda") == 0 && std::strcmp(name, static_cast<const char*>(user)) == 0;
}

bool fail_every_cuda(void* /*user*/, const char* backend, const char* /*name*/) noexcept
{
    return std::strcmp(backend, "cuda") == 0;
}

// A scratch stream the report writes to.
std::FILE* scratch_file()
{
#if defined(_WIN32)
    std::FILE* f = nullptr;
    return tmpfile_s(&f) == 0 ? f : nullptr; // MSVC deprecates std::tmpfile for tmpfile_s
#else
    return std::tmpfile();
#endif
}

// What the report wrote to `out`, which it closes.
std::string_view read_back(std::FILE* out, char* text, crd::usize bytes)
{
    REQUIRE(std::fseek(out, 0, SEEK_SET) == 0);
    const crd::usize n = std::fread(text, 1U, bytes - 1U, out);
    (void)std::fclose(out);
    return std::string_view{text, n};
}

// How many devices CUDA lists, read independently of the chooser; 0 without a driver or a device.
int cuda_device_count()
{
    int count = 0;
    if (cuInit(0) != CUDA_SUCCESS || cuDeviceGetCount(&count) != CUDA_SUCCESS)
    {
        return 0;
    }
    return count;
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

// The kernel's answer on `compute` from `input` (no pipeline: "build").
const char* run_kernel(g::CudaComputeContext& compute, const float* input, bool build, crd::memory::IAllocator* alloc)
{
    kir::KGraph graph(alloc);
    const kir::KEntry entry = kir::build_device_check_kernel(graph);
    std::unique_ptr<g::ComputePipeline> pipeline;
    if (build)
    {
        kir::GlslKernel kernel(alloc);
        REQUIRE(kir::emit_compute_kernel_cuda(graph, entry, alloc, kernel));
        pipeline = compute.create_pipeline_from_cuda(crd::containers::to_view(kernel.source),
                                                     crd::containers::StringView("ckir"), kir::kDeviceCheckBindings,
                                                     entry.local_size[0], 0U, false);
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

TEST_CASE("hw-resilience 3: a CUDA device passes its device check once, on a probe context of its own",
          "[cuda][gpu][hw-resilience]")
{
    if (cuda_device_count() == 0)
    {
        SKIP("CUDA device unavailable");
    }
    const Fresh fresh;
    crd::memory::TlsfAllocator alloc(4U << 20U);
    auto ctx = g::create_cuda_compute_context(alloc);
    REQUIRE(ctx != nullptr);
    REQUIRE(g::device_checks().check_count() >= 1U);
    const g::DeviceCheck check = g::device_checks().check_at(0U);
    INFO((check.failed != nullptr ? check.failed : "passed"));
    REQUIRE(ctx->valid()); // a sound device passes
    CHECK(g::device_checks().check_count() == 1U);
    CHECK(check.failed == nullptr);
    CHECK(check.unchecked == nullptr); // NVRTC is linked in: the check always runs
    CHECK(check.ordinal == 0U);
    CHECK(std::string_view{check.adapter.name} == ctx->adapter().name);
    CHECK(std::string_view{check.adapter.backend} == "cuda");
    CHECK(check.nanoseconds > 0U);
    std::printf("[hw-resilience 3] cuda device check on %s: %.3f ms\n", ctx->adapter().name,
                static_cast<double>(check.nanoseconds) / 1.0e6);

    REQUIRE(g::device_checks().selection_count() == 1U);
    const g::DeviceSelection selection = g::device_checks().selection_at(0U);
    CHECK(selection.status == g::DeviceCheckStatus::Passed);
    CHECK(selection.adapters_failed == 0U);
    CHECK(std::string_view{selection.runs.name} == ctx->adapter().name);

    // The context left this thread on the device's primary context, the one it retained, not on the probe's own.
    CUdevice device = 0;
    REQUIRE(cuDeviceGet(&device, 0) == CUDA_SUCCESS);
    CUcontext primary = nullptr;
    REQUIRE(cuDevicePrimaryCtxRetain(&primary, device) == CUDA_SUCCESS);
    CUcontext current = nullptr;
    CHECK(cuCtxGetCurrent(&current) == CUDA_SUCCESS);
    CHECK(current == primary);
    CHECK(cuDevicePrimaryCtxRelease(device) == CUDA_SUCCESS);

    // A second context opens the same checked device and checks nothing again.
    auto again = g::create_cuda_compute_context(alloc);
    REQUIRE(again != nullptr);
    REQUIRE(again->valid());
    CHECK(std::string_view{again->adapter().name} == ctx->adapter().name);
    CHECK(g::device_checks().check_count() == 1U);
    REQUIRE(g::device_checks().selection_count() == 1U);
    CHECK(g::device_checks().selection_at(0U).status == g::DeviceCheckStatus::Passed);
}

TEST_CASE("hw-resilience 3: the device check reproduces its answers on a CUDA device and names a wrong one",
          "[cuda][gpu][hw-resilience]")
{
    crd::memory::TlsfAllocator alloc(4U << 20U);
    auto compute = g::create_cuda_compute_context(alloc);
    REQUIRE(compute != nullptr);
    if (!compute->valid())
    {
        SKIP("CUDA device unavailable");
    }
    float input[kir::kDeviceCheckLanes] = {}; // NOLINT(modernize-avoid-c-arrays)
    for (crd::u32 i = 0U; i < kir::kDeviceCheckLanes; ++i)
    {
        input[i] = kir::device_check_input(i);
    }
    const char* const sound = run_kernel(*compute, input, true, &alloc);
    INFO((sound != nullptr ? sound : "passed"));
    CHECK(sound == nullptr);

    // One input off by one: the lane, its neighbour's exchange and its workgroup's sum all move; the first named is
    // the arithmetic.
    input[3] += 1.0F;
    const char* const wrong = run_kernel(*compute, input, true, &alloc);
    REQUIRE(wrong != nullptr);
    CHECK(std::string_view{wrong} == "arithmetic");
    input[3] -= 1.0F;

    const char* const unbuilt = run_kernel(*compute, input, false, &alloc);
    REQUIRE(unbuilt != nullptr);
    CHECK(std::string_view{unbuilt} == "build");
}

TEST_CASE("hw-resilience 3: a CUDA device that fails its device check is passed over and named at startup",
          "[cuda][gpu][hw-resilience]")
{
    const int devices = cuda_device_count();
    if (devices == 0)
    {
        SKIP("CUDA device unavailable");
    }
    const Fresh fresh;
    crd::memory::TlsfAllocator alloc(4U << 20U);
    char preferred[sizeof(g::GpuAdapter::name)] = {}; // NOLINT(modernize-avoid-c-arrays)
    {
        auto first = g::create_cuda_compute_context(alloc);
        REQUIRE(first != nullptr);
        REQUIRE(first->valid());
        (void)std::snprintf(preferred, sizeof(preferred), "%s", first->adapter().name);
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

    auto ctx = g::create_cuda_compute_context(alloc);
    REQUIRE(ctx != nullptr);
    REQUIRE(g::device_checks().selection_count() == 1U);
    const g::DeviceSelection selection = g::device_checks().selection_at(0U);
    CHECK(std::string_view{selection.selected.name} == preferred);
    REQUIRE(selection.failed != nullptr);
    CHECK(std::string_view{selection.failed} == "injected");
    CHECK(selection.injected);
    CHECK(selection.adapters_failed >= 1U);
    if (ctx->valid())
    {
        // Another device (a different model: the seam fails every device of the preferred one's name) passed.
        CHECK(selection.status == g::DeviceCheckStatus::Demoted);
        CHECK(std::string_view{ctx->adapter().name} != preferred);
        CHECK(std::string_view{selection.runs.name} == ctx->adapter().name);
    }
    else
    {
        // No other device: no device, which every consumer takes as its CPU path.
        CHECK(selection.status == g::DeviceCheckStatus::NoDevicePassed);
        CHECK(selection.adapters_failed == static_cast<crd::u32>(devices));
        CHECK_FALSE(selection.runs.known);
    }

    g::report_device_demotions(nullptr);
    char text[1024] = {}; // NOLINT(modernize-avoid-c-arrays)
    const std::string_view written = read_back(out, text, sizeof(text));
    char expected[512] = {}; // NOLINT(modernize-avoid-c-arrays)
    (void)std::snprintf(expected, sizeof(expected),
                        "[crd] GPU device path cuda on %s failed its known answers in injected (fault seam); it runs ",
                        preferred);
    INFO(written);
    CHECK(written.find(expected) == 0U);
}

TEST_CASE("hw-resilience 3: when no CUDA device passes its device check, the context has no device",
          "[cuda][gpu][hw-resilience]")
{
    const int devices = cuda_device_count();
    if (devices == 0)
    {
        SKIP("CUDA device unavailable");
    }
    const Fresh fresh;
    if (!crd::fault_seams_compiled())
    {
        CHECK_FALSE(crd::install_device_check_fault_hook(&fail_every_cuda, nullptr));
        return;
    }
    REQUIRE(crd::install_device_check_fault_hook(&fail_every_cuda, nullptr));
    std::FILE* const out = scratch_file();
    REQUIRE(out != nullptr);
    g::report_device_demotions(out);

    crd::memory::TlsfAllocator alloc(4U << 20U);
    auto ctx = g::create_cuda_compute_context(alloc);
    REQUIRE(ctx != nullptr);
    CHECK_FALSE(ctx->valid());
    CHECK(ctx->create_buffer(64U, g::compute_usage::storage, g::ComputeMemory::GpuOnly) == nullptr);

    // Every device CUDA lists was tried once and failed.
    const crd::u32 checks = g::device_checks().check_count();
    CHECK(checks == static_cast<crd::u32>(devices));
    for (crd::u32 i = 0U; i < checks; ++i)
    {
        const g::DeviceCheck check = g::device_checks().check_at(i);
        CHECK(check.injected);
        CHECK(check.ordinal == i);
        CHECK(std::string_view{check.adapter.backend} == "cuda");
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
    CHECK(written.find("[crd] GPU device path cuda on ") == 0U);
    CHECK(written.find("; it runs no device (no-device-passed, ") != std::string_view::npos);
}
