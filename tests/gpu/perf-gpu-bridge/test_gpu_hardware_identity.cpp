// HW-RESILIENCE.2: the GPUs of the machine's hardware identity, device-free. Adapters noted in the process's
// opened-adapter log, before and after the bridge attaches, join crd::perf::host_hardware_identity() with the backend
// and its driver version as the driver.

#include <crd/gpu/adapter.hpp>
#include <crd/perf/gpu/gpu_hardware_identity.hpp>
#include <crd/perf/hardware_identity.hpp>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdio>

namespace
{
namespace g = crd::gpu;
namespace cp = crd::perf;

g::GpuAdapter adapter(const char* backend, crd::u32 device, const char* version)
{
    g::GpuAdapter a{};
    (void)std::snprintf(a.backend, sizeof(a.backend), "%s", backend);
    (void)std::snprintf(a.name, sizeof(a.name), "%s", "Bridge Test GPU");
    (void)std::snprintf(a.driver_version, sizeof(a.driver_version), "%s", version);
    a.vendor = 0x1002U;
    a.device = device;
    a.known = true;
    return a;
}

[[nodiscard]] bool host_has(crd::u32 device, crd::containers::StringView driver)
{
    const auto& gpus = cp::host_hardware_identity().gpus;
    return std::any_of(gpus.begin(), gpus.end(),
                       [device, driver](const cp::GpuIdentity& gpu)
                       {
                           return gpu.vendor_id == 0x1002U && gpu.device_id == device &&
                                  crd::containers::StringView{gpu.name.data(), gpu.name.size()} == "Bridge Test GPU" &&
                                  crd::containers::StringView{gpu.driver.data(), gpu.driver.size()} == driver;
                       });
}
} // namespace

TEST_CASE("gpu hardware identity: an adapter reads as its ids, name, and backend with its driver version",
          "[perf][gpu][diag][hw-resilience]")
{
    const cp::GpuIdentity vk = cp::gpu::gpu_identity(adapter("vulkan", 0x744CU, "2.0.302"));
    CHECK(vk.vendor_id == 0x1002U);
    CHECK(vk.device_id == 0x744CU);
    CHECK(crd::containers::StringView{vk.name.data(), vk.name.size()} == "Bridge Test GPU");
    CHECK(crd::containers::StringView{vk.driver.data(), vk.driver.size()} == "vulkan 2.0.302");
    const cp::GpuIdentity bare = cp::gpu::gpu_identity(adapter("dx12", 0x744CU, "")); // no version reported
    CHECK(crd::containers::StringView{bare.driver.data(), bare.driver.size()} == "dx12");
}

TEST_CASE("gpu hardware identity: every adapter the process opens joins the machine, the ones before the attach too",
          "[perf][gpu][diag][hw-resilience]")
{
    g::OpenedAdapters& opened = g::opened_adapters();
    REQUIRE(opened.note(adapter("vulkan", 0x7550U, "2.0.302"))); // opened before the host attaches
    CHECK_FALSE(host_has(0x7550U, "vulkan 2.0.302"));

    cp::gpu::attach_gpus_to_hardware_identity();
    CHECK(host_has(0x7550U, "vulkan 2.0.302"));
    REQUIRE(opened.note(adapter("dx12", 0x7550U, "32.0.21025.1"))); // opened after
    CHECK(host_has(0x7550U, "dx12 32.0.21025.1"));
    const crd::usize gpus = cp::host_hardware_identity().gpus.size();
    REQUIRE(opened.note(adapter("vulkan", 0x7550U, "2.0.302"))); // the same adapter again
    CHECK(cp::host_hardware_identity().gpus.size() == gpus);
    opened.listen(nullptr);
}
