#pragma once

// crd-gpu-context -- device and host pressure evidence, and what a device failure may be attributed to (DIAG.10c).
//
// Three pieces, vendor-free, so both backends and every consumer share them:
//
// - `DeviceMemoryReading`: a device's memory as the OS sees it for this process, as two budget readings
//   (crd/core/budget_pressure.hpp). The bound is the budget the OS currently grants this process, in use is what the
//   process uses: DX12 asks DXGI (IDXGIAdapter3::QueryVideoMemoryInfo, the local and non-local segment groups);
//   Vulkan asks VK_EXT_memory_budget (summed over the device-local heaps, and over the others). Both are per process
//   and per adapter, whichever device of the process allocated. The budget is soft: the OS may let use pass it, and
//   then pages leave the device. A backend, device or heap that cannot be asked is `read == false` with a reason,
//   never zero use.
//
// - `GpuLossPressure`: the host and device readings a backend takes the first time it records a device lost, so the
//   failure carries the pressure it happened under.
//
// - `attribute_device_fault`: what a failure may be blamed on. A device fault is labelled an engine defect only with
//   a discriminating reproduction: the same inputs fail again on another configuration (another adapter, the software
//   adapter, the other backend). A fault that only reproduces on the same adapter, or that was never reproduced,
//   stays undiscriminated; one that happened while memory was at or over a bound is reported as coinciding with that
//   pressure; one that a reproduction failed to repeat is reported as not reproduced. An injected fault is always
//   Injected, never a real one. Nothing here guesses hardware or OS faults either: it only refuses to call them ours.
//
// Contract: docs/design/runtime-diagnostics.md (DIAG.10c).

#include <crd/core/budget_pressure.hpp>
#include <crd/core/host_pressure.hpp>
#include <crd/core/types.hpp>

namespace crd::gpu
{

// Where a device memory reading came from.
// NOLINTNEXTLINE(performance-enum-size)
enum class DeviceMemorySource : crd::u8
{
    Unavailable,        // the backend or device cannot be asked; `reason` says why
    Dxgi,               // IDXGIAdapter3::QueryVideoMemoryInfo
    VulkanMemoryBudget, // VK_EXT_memory_budget through vkGetPhysicalDeviceMemoryProperties2
};
[[nodiscard]] const char* device_memory_source_name(DeviceMemorySource s) noexcept;

struct DeviceMemoryReading
{
    BudgetReading      local;     // memory on the device (DXGI local segment group; Vulkan device-local heaps)
    BudgetReading      non_local; // system memory the device uses (DXGI non-local; Vulkan heaps not device-local)
    DeviceMemorySource source = DeviceMemorySource::Unavailable;
    const char*        reason = "backend does not query device memory"; // static; empty once both were read
};

// The pressure a backend saw when it first recorded a device lost.
struct GpuLossPressure
{
    crd::u64            loss_sequence = 0U;    // the backend's sequence of the loss these readings belong to
    bool                taken         = false; // false until a loss was recorded
    HostMemoryReading   host;
    DeviceMemoryReading device;
};

// The kind of device failure, as both backends' completion seams classify it.
// NOLINTNEXTLINE(performance-enum-size)
enum class DeviceFailureKind : crd::u8
{
    None,         // nothing failed
    Observed,     // the runtime or driver reported the device lost or removed
    EngineForced, // a completion failed or timed out and the engine removed the device (in-flight memory stays safe)
    TimedOut,     // a bounded wait expired; the device is not known to be lost
    Failed,       // another error result from a submission or wait
    Simulated,    // injected through a fault seam or test hook: never a real fault
};

// Whether the failure was reproduced, and where.
// NOLINTNEXTLINE(performance-enum-size)
enum class Reproduction : crd::u8
{
    NotAttempted,       // nobody tried
    NotReproduced,      // the same inputs ran again and did not fail
    SameAdapterOnly,    // it failed again, but only on the adapter that failed first: hardware is not excluded
    OtherConfiguration, // it failed again on another adapter, the software adapter or the other backend
};

struct DeviceFaultEvidence
{
    DeviceFailureKind failure      = DeviceFailureKind::None;
    Reproduction      reproduction = Reproduction::NotAttempted;
    GpuLossPressure   pressure; // the readings at the failure (`taken` false when none were taken)
};

// What the failure may be blamed on, the strongest claim the evidence allows.
// NOLINTNEXTLINE(performance-enum-size)
enum class FaultAttribution : crd::u8
{
    NoFailure,          // nothing failed
    Injected,           // a simulated fault: it proves the failure path, never the hardware or the engine
    EngineDefect,       // reproduced on another configuration: the engine's inputs fail wherever they run
    PressureCoincident, // not discriminated, and host or device memory was at or over a bound when it failed
    NotReproduced,      // a reproduction with the same inputs did not fail: hardware, driver or OS suspected
    Undiscriminated,    // nothing tells the engine, the driver, the OS and the hardware apart
};
inline constexpr crd::usize kFaultAttributionCount = 6U;
[[nodiscard]] const char* fault_attribution_name(FaultAttribution a) noexcept;

// The strongest attribution `evidence` allows. Simulated is Injected whatever else is known; a failure reproduced on
// another configuration is an EngineDefect; otherwise a reading taken at the failure that is Full or Over (physical
// memory, an enforced commit limit, or the device's local or non-local budget) makes it PressureCoincident; otherwise
// NotReproduced or Undiscriminated by the reproduction.
[[nodiscard]] FaultAttribution attribute_device_fault(const DeviceFaultEvidence& evidence) noexcept;

// The worst level among the readings an attribution consults: physical, commit (only where enforced), and the
// device's local and non-local budgets, each judged on its in-use amount. Unread when none was read.
[[nodiscard]] PressureLevel worst_memory_level(const GpuLossPressure& pressure) noexcept;

// The host and the device readings of `device` taken together (the latter supplied by the backend).
[[nodiscard]] GpuLossPressure take_loss_pressure(crd::u64 loss_sequence, const DeviceMemoryReading& device) noexcept;

} // namespace crd::gpu
