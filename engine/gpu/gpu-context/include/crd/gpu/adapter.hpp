#pragma once

// crd-gpu-context -- the adapter a GPU context or compute context runs on (DIAG.9a), and the process-wide log of every
// adapter this process opened a device on (HW-RESILIENCE.2).
//
// A device run's numerics belong to its adapter: a run record claims bit identity only on the adapter (and build) that
// recorded it. A crash or failure that only some machines show is often the GPU or its driver, so the hardware
// identity of every bundle and native crash names the GPUs the process used: each backend notes the adapter in
// opened_adapters() right after it creates a device (vkCreateDevice, D3D12CreateDevice, the CUDA primary context), and
// a host that wants them in its diagnostics listens there (crd-perf-gpu-bridge's attach_gpus_to_hardware_identity).
// The API is asked when the device is created, never when a bundle or dump is written.
//
// No vendor types appear here; each backend fills the adapter from its own API in its own .cpp.

#include <crd/core/types.hpp>

#include <mutex>

namespace crd::gpu
{

inline constexpr crd::u32 kAdapterDriverVersionBytes = 64U;

// `vendor` is the PCI vendor (0x10DE NVIDIA, 0x1002 AMD, 0x8086 Intel, ...), or the Khronos vendor id Vulkan reports
// for a device with none (0x10005 Mesa); `device` is the PCI device, 0 when the API reports none (CUDA).
struct GpuAdapter
{
    char backend[16] = {}; // "vulkan", "dx12", "cuda"
    char name[256] = {};   // the adapter's description (UTF-8, truncated to fit)
    crd::u32 vendor = 0U;
    crd::u32 device = 0U;
    crd::u64 driver = 0U; // Vulkan: driverVersion; D3D12: the user-mode driver version; CUDA: cuDriverGetVersion
    crd::u32 api = 0U;    // Vulkan: the device's apiVersion; D3D12 and CUDA: 0
    char driver_version[kAdapterDriverVersionBytes] = {}; // the driver version as its API reports it, as text
    bool known = false;                                   // false for a backend that does not report its adapter
};

// The four 16-bit parts of a Windows driver version (D3D12's user-mode driver version, the registry's DriverVersion),
// most significant first: "32.0.15.8157".
void format_windows_driver_version(crd::u64 version, char (&out)[kAdapterDriverVersionBytes]) noexcept;

// A Vulkan driverVersion, which each vendor packs its own way: NVIDIA 10.8.8.6 bits ("581.57.0.0"), Intel on Windows
// 18.14 bits ("101.6130"), every other driver VK_MAKE_API_VERSION's major.minor.patch. Used when the device does not
// report VkPhysicalDeviceDriverProperties::driverInfo, the driver's own text.
void format_vulkan_driver_version(crd::u32 vendor, crd::u32 version, bool windows,
                                  char (&out)[kAdapterDriverVersionBytes]) noexcept;

// The adapters a process opened devices on, each kept once (the same backend, vendor, device, name and driver version
// is one entry), at most kMaxOpenedAdapters. A listener receives every adapter noted before it was set, then each new
// one, in order. Thread-safe; the listener runs under the log's lock, so it must not note an adapter itself.
class OpenedAdapters
{
public:
    static constexpr crd::u32 kMaxOpenedAdapters = 16U;
    using Listener = void (*)(const GpuAdapter& adapter);

    OpenedAdapters() = default;

    OpenedAdapters(const OpenedAdapters&) = delete;
    OpenedAdapters& operator=(const OpenedAdapters&) = delete;
    OpenedAdapters(OpenedAdapters&&) = delete;
    OpenedAdapters& operator=(OpenedAdapters&&) = delete;

    // Note `adapter` (ignored unless known). False when it is new and the log is full; true otherwise.
    bool note(const GpuAdapter& adapter) noexcept;

    // Send every noted adapter, then each new one, to `listener` (nullptr stops sending).
    void listen(Listener listener) noexcept;

    [[nodiscard]] crd::u32 count() const noexcept;
    // The `index`th adapter noted (an unknown adapter past count()).
    [[nodiscard]] GpuAdapter at(crd::u32 index) const noexcept;

private:
    mutable std::mutex m_mutex;
    GpuAdapter m_adapters[kMaxOpenedAdapters]{};
    crd::u32 m_count = 0U;
    Listener m_listener = nullptr;
};

// The process's log; every backend notes into it.
[[nodiscard]] OpenedAdapters& opened_adapters() noexcept;

} // namespace crd::gpu
