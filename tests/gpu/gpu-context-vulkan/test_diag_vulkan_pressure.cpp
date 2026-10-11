// DIAG.10c: device and host pressure evidence and the driver-call mode witness on Vulkan.
// Declared contract (docs/design/runtime-diagnostics.md, DIAG.10c):
// - IGpuContext::device_memory reads VK_EXT_memory_budget when the device offers it (the context enables it then),
//   summing the device-local heaps and the other heaps; without it the reading is unavailable with a reason. The
//   oracles: whether the physical device advertises the extension, asked by the test itself, and memory the test
//   allocates itself on the context's device: the process's usage must grow by at least that much, and fall back once
//   it is freed.
// - The frame graph reports its transient budget as a reading, refusals counted once per refused build.
// - Instance and device creation, shader compilation and submission run under the mode witness: each moves its
//   kind's count.
// - The first time a device is recorded lost, the host and device pressure is taken with the failure's sequence; an
//   injected loss attributes to Injected.
// Device-gated: skips without a Vulkan device (the hosted Windows lanes); runs on lavapipe and on a real GPU.

#include "vulkan_device_memory.hpp" // src-private: the budget reading and the loss pressure
#include "vulkan_execution.hpp"     // src-private: the completion seam and its failure record

#include <crd/core/budget_pressure.hpp>
#include <crd/core/fp_environment.hpp>
#include <crd/gpu/device_pressure.hpp>
#include <crd/gpu/driver_witness.hpp>
#include <crd/gpu/frame_graph.hpp>
#include <crd/gpu/raster_context.hpp>
#include <crd/gpu/vulkan_compute_context.hpp>
#include <crd/gpu/vulkan_context.hpp>
#include <crd/gpu/vulkan_raster_context.hpp>
#include <crd/gpu/vulkan_shader_compile.hpp>
#include <crd/memory/allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstring>
#include <memory>

namespace
{
namespace g = crd::gpu;
namespace d = crd::gpu::detail;

constexpr VkDeviceSize kAllocBytes = VkDeviceSize{256} << 20U;

std::unique_ptr<g::IGpuContext> make_context()
{
    g::GpuContextConfig cfg;
    cfg.backend  = g::GpuBackend::Vulkan;
    cfg.headless = true;
    return g::create_vulkan_gpu_context(cfg);
}

// Does `physical` advertise VK_EXT_memory_budget? Asked by the test, not by the context.
bool advertises_budget(VkPhysicalDevice physical)
{
    crd::u32 count = 0U;
    REQUIRE(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr) == VK_SUCCESS);
    auto exts = std::make_unique<VkExtensionProperties[]>(count);
    REQUIRE(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, exts.get()) == VK_SUCCESS);
    for (crd::u32 i = 0U; i < count; ++i)
    {
        if (std::strcmp(exts[i].extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) == 0)
        {
            return true;
        }
    }
    return false;
}

VkDriverId driver_id(VkPhysicalDevice physical)
{
    VkPhysicalDeviceDriverProperties driver{};
    driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    VkPhysicalDeviceProperties2 props{};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props.pNext = &driver;
    vkGetPhysicalDeviceProperties2(physical, &props);
    return driver.driverID;
}

crd::u64 usage(const g::DeviceMemoryReading& r)
{
    return (r.local.read ? r.local.in_use : 0U) + (r.non_local.read ? r.non_local.in_use : 0U);
}

// The first memory type in a device-local heap.
crd::u32 device_local_type(VkPhysicalDevice physical)
{
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physical, &props);
    for (crd::u32 t = 0U; t < props.memoryTypeCount; ++t)
    {
        if ((props.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0U)
        {
            return t;
        }
    }
    return 0U;
}

void record_nothing(g::IFrameContext& /*ctx*/, void* /*user*/) {}

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
} // namespace

TEST_CASE("diag10c: Vulkan device memory follows memory the process allocates", "[vulkan][gpu][pressure][diag10c]")
{
    auto ctx = make_context();
    if (ctx == nullptr || !ctx->valid())
    {
        SKIP("no Vulkan device");
    }
    auto* const                  vk     = static_cast<g::VulkanGpuContext*>(ctx.get());
    const g::DeviceMemoryReading before = ctx->device_memory();
    INFO(ctx->adapter_name() << ": " << before.reason);
    if (!advertises_budget(vk->vk_physical_device()))
    {
        // The context cannot ask, and says so instead of reporting zero use.
        CHECK(before.source == g::DeviceMemorySource::Unavailable);
        CHECK_FALSE(before.local.read);
        CHECK(before.reason[0] != '\0');
        return;
    }
    REQUIRE(before.source == g::DeviceMemorySource::VulkanMemoryBudget);
    REQUIRE(before.local.read);
    CHECK(before.local.bounded);
    CHECK(before.local.capacity > 0U);
    CHECK_FALSE(before.local.high_water_tracked);
    if (before.non_local.read)
    {
        CHECK(before.non_local.capacity > 0U); // a heap granted nothing is absent, never read as full
    }

    VkMemoryAllocateInfo info{};
    info.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    info.allocationSize  = kAllocBytes;
    info.memoryTypeIndex = device_local_type(vk->vk_physical_device());
    VkDeviceMemory memory = VK_NULL_HANDLE;
    REQUIRE(vkAllocateMemory(vk->vk_device(), &info, nullptr, &memory) == VK_SUCCESS);
    const g::DeviceMemoryReading during = ctx->device_memory();
    vkFreeMemory(vk->vk_device(), memory, nullptr);
    const g::DeviceMemoryReading after = ctx->device_memory();
    std::printf("[diag10c vulkan %s] local %llu/%llu non-local %s, usage %llu -> %llu -> %llu\n",
                ctx->adapter_name(), static_cast<unsigned long long>(during.local.in_use),
                static_cast<unsigned long long>(during.local.capacity), during.non_local.read ? "read" : "none",
                static_cast<unsigned long long>(usage(before)), static_cast<unsigned long long>(usage(during)),
                static_cast<unsigned long long>(usage(after)));
    if (driver_id(vk->vk_physical_device()) == VK_DRIVER_ID_MESA_LLVMPIPE)
    {
        // llvmpipe (the hosted Linux lanes' device) offers the extension, but its heap usage does not follow this
        // process's allocations: observed constant across this 256 MiB allocation and its free (Mesa 25.2 on the
        // reference host). Its reading is the driver's estimate, so the usage oracle has nothing to hold it to here;
        // the reading's shape is still checked above.
        WARN("llvmpipe heap usage " << usage(before) << " -> " << usage(during) << " -> " << usage(after)
                                    << " across a 256 MiB allocation: not tracked by this driver");
    }
    else
    {
        CHECK(usage(during) >= usage(before) + kAllocBytes);
        CHECK(usage(after) + kAllocBytes <= usage(during));
    }

    // Without the extension the reading is unavailable, never zero use.
    const g::DeviceMemoryReading refused = d::vk_device_memory(vk->vk_physical_device(), false);
    CHECK(refused.source == g::DeviceMemorySource::Unavailable);
    CHECK_FALSE(refused.local.read);
    CHECK(refused.reason[0] != '\0');
}

TEST_CASE("diag10c: the Vulkan frame graph reports its transient budget as a reading",
          "[vulkan][gpu][pressure][frame-graph][diag10c]")
{
    g::GpuContextConfig cfg;
    cfg.backend  = g::GpuBackend::Vulkan;
    cfg.headless = true;
    auto  ctx    = g::create_vulkan_gpu_context(cfg);
    auto* vk     = ctx != nullptr ? static_cast<g::VulkanGpuContext*>(ctx.get()) : nullptr;
    if (vk == nullptr || !vk->graphics_capable() || !vk->shader_object())
    {
        SKIP("no graphics-capable Vulkan device with shader objects");
    }
    auto raster = g::create_vulkan_raster_context(*vk);
    REQUIRE(raster != nullptr);
    REQUIRE(raster->valid());

    // 2048 x 2048 RGBA8: 16 MiB a transient, so a leaked build is far above the process's other device traffic.
    constexpr crd::u32 side = 2048U;
    auto               graph = raster->create_frame_graph();
    REQUIRE(graph != nullptr);
    CHECK(graph->transient_pressure().read);
    CHECK(graph->transient_pressure().in_use == 0U);
    REQUIRE(build_two_transients(*graph, side));
    const crd::BudgetReading open = graph->transient_pressure();
    CHECK_FALSE(open.bounded);
    CHECK(crd::pressure_level(open, open.in_use) == crd::PressureLevel::Unbounded);
    CHECK(open.in_use == graph->transient_memory_bytes());
    CHECK(open.in_use >= crd::u64{side} * side * 4U); // the two disjoint transients share one slot
    CHECK(open.high_water_tracked);
    CHECK(open.high_water == open.in_use);
    CHECK(open.refusals == 0U);
    const crd::u64 footprint = open.in_use;

    const g::DeviceMemoryReading start = ctx->device_memory();
    {
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
        CHECK(refused.high_water == footprint);
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
    // The graph is gone and so is every build's memory, as the OS counts it, where the device reports its usage.
    const g::DeviceMemoryReading end = ctx->device_memory();
    std::printf("[diag10c vulkan frame graph] %s footprint %llu, usage %llu -> %llu (%s)\n", ctx->adapter_name(),
                static_cast<unsigned long long>(footprint), static_cast<unsigned long long>(usage(start)),
                static_cast<unsigned long long>(usage(end)), g::device_memory_source_name(end.source));
    if (end.source != g::DeviceMemorySource::Unavailable)
    {
        CHECK(usage(end) < usage(start) + footprint / 2U);
    }
}

TEST_CASE("diag10c: Vulkan instance and device creation, shader compilation and submission run under the mode witness",
          "[vulkan][gpu][fp-mode][diag10c]")
{
    const crd::FpEnvironment     start  = crd::read_fp_environment();
    const g::DriverWitnessReport before = g::driver_witness();
    auto                         ctx    = make_context();
    if (ctx == nullptr || !ctx->valid())
    {
        SKIP("no Vulkan device");
    }
    auto* const                  vk      = static_cast<g::VulkanGpuContext*>(ctx.get());
    const g::DriverWitnessReport created = g::driver_witness();
    CHECK(calls_of(created, g::DriverCall::CreateInstance) > calls_of(before, g::DriverCall::CreateInstance));
    CHECK(calls_of(created, g::DriverCall::CreateDevice) > calls_of(before, g::DriverCall::CreateDevice));

    const g::ShaderCompileResult spirv = g::compile_glsl_to_spirv(
        g::ShaderStage::Compute, "#version 450\nlayout(local_size_x = 1) in;\nvoid main() {}\n", "diag10c");
    if (!spirv.ok)
    {
        WARN("shaderc unavailable: " << spirv.error_message.c_str());
    }
    else
    {
        CHECK(calls_of(g::driver_witness(), g::DriverCall::CompileShader)
              > calls_of(created, g::DriverCall::CompileShader));
    }

    g::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());
    const g::DriverWitnessReport ready = g::driver_witness();
    (void)compute.begin();
    compute.submit_and_wait();
    REQUIRE(compute.valid());
    CHECK(calls_of(g::driver_witness(), g::DriverCall::Submit) > calls_of(ready, g::DriverCall::Submit));

    CHECK(crd::fp_differences(crd::read_fp_environment(), start) == 0U);
    const g::DriverWitnessReport end = g::driver_witness();
    std::printf("[diag10c vulkan witness] %s divergences %llu (first: %s)\n", ctx->adapter_name(),
                static_cast<unsigned long long>(end.total_divergences()),
                end.first_operation != nullptr ? end.first_operation : "none");
    if (end.total_divergences() != 0U)
    {
        CHECK(end.first_operation != nullptr);
    }
}

TEST_CASE("diag10c: an injected Vulkan device loss records its pressure and attributes to Injected",
          "[vulkan][gpu][pressure][device-loss][diag10c]")
{
    auto ctx = make_context();
    if (ctx == nullptr || !ctx->valid())
    {
        SKIP("no Vulkan device");
    }
    auto* const               vk = static_cast<g::VulkanGpuContext*>(ctx.get());
    g::VulkanComputeContext   compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());
    d::vk_inject_next_result(VK_ERROR_DEVICE_LOST);
    (void)compute.begin();
    compute.submit_and_wait();
    CHECK_FALSE(compute.valid());

    const d::VkDeviceFailure failure = d::vk_device_failure(vk->vk_device());
    REQUIRE(failure.loss_origin == d::VkFailureOrigin::Simulated);
    const g::GpuLossPressure pressure = d::vk_last_loss_pressure();
    CHECK(pressure.taken);
    CHECK(pressure.loss_sequence == failure.sequence);
    CHECK(pressure.host.physical.read);
    CHECK(pressure.host.resident.read);
    CHECK(pressure.device.source == ctx->device_memory().source);

    g::DeviceFaultEvidence evidence;
    evidence.failure      = g::DeviceFailureKind::Simulated;
    evidence.reproduction = g::Reproduction::OtherConfiguration; // even a "reproduction" cannot make it real
    evidence.pressure     = pressure;
    CHECK(g::attribute_device_fault(evidence) == g::FaultAttribution::Injected);

    // A later failure on the same lost device does not take the pressure again.
    (void)compute.begin();
    compute.submit_and_wait();
    CHECK(d::vk_last_loss_pressure().loss_sequence == pressure.loss_sequence);
}
