#include <crd/gpu/device_pressure.hpp>

namespace crd::gpu
{

const char* device_memory_source_name(DeviceMemorySource s) noexcept
{
    switch (s) // no default: every source is named
    {
    case DeviceMemorySource::Unavailable:        return "unavailable";
    case DeviceMemorySource::Dxgi:               return "dxgi";
    case DeviceMemorySource::VulkanMemoryBudget: return "vulkan-memory-budget";
    }
    return "unknown";
}

const char* fault_attribution_name(FaultAttribution a) noexcept
{
    switch (a) // no default: every attribution is named
    {
    case FaultAttribution::NoFailure:          return "no-failure";
    case FaultAttribution::Injected:           return "injected";
    case FaultAttribution::EngineDefect:       return "engine-defect";
    case FaultAttribution::PressureCoincident: return "pressure-coincident";
    case FaultAttribution::NotReproduced:      return "not-reproduced";
    case FaultAttribution::Undiscriminated:    return "undiscriminated";
    }
    return "unknown";
}

namespace
{
// Unread < Unbounded < Within < Near < Full < Over: the order the attribution needs ("at or over a bound").
[[nodiscard]] crd::u32 severity(PressureLevel l) noexcept
{
    switch (l) // no default: every level is ranked
    {
    case PressureLevel::Unread:    return 0U;
    case PressureLevel::Unbounded: return 1U;
    case PressureLevel::Within:    return 2U;
    case PressureLevel::Near:      return 3U;
    case PressureLevel::Full:      return 4U;
    case PressureLevel::Over:      return 5U;
    }
    return 0U;
}

void consider(PressureLevel& worst, const BudgetReading& r) noexcept
{
    const PressureLevel l = pressure_level(r, r.in_use);
    if (severity(l) > severity(worst))
    {
        worst = l;
    }
}
} // namespace

PressureLevel worst_memory_level(const GpuLossPressure& pressure) noexcept
{
    PressureLevel worst = PressureLevel::Unread;
    if (!pressure.taken)
    {
        return worst;
    }
    consider(worst, pressure.host.physical);
    if (pressure.host.commit_enforced)
    {
        consider(worst, pressure.host.commit);
    }
    consider(worst, pressure.device.local);
    consider(worst, pressure.device.non_local);
    return worst;
}

FaultAttribution attribute_device_fault(const DeviceFaultEvidence& evidence) noexcept
{
    if (evidence.failure == DeviceFailureKind::None)
    {
        return FaultAttribution::NoFailure;
    }
    if (evidence.failure == DeviceFailureKind::Simulated)
    {
        return FaultAttribution::Injected;
    }
    if (evidence.reproduction == Reproduction::OtherConfiguration)
    {
        return FaultAttribution::EngineDefect;
    }
    const PressureLevel worst = worst_memory_level(evidence.pressure);
    if (worst == PressureLevel::Full || worst == PressureLevel::Over)
    {
        return FaultAttribution::PressureCoincident;
    }
    if (evidence.reproduction == Reproduction::NotReproduced)
    {
        return FaultAttribution::NotReproduced;
    }
    return FaultAttribution::Undiscriminated;
}

GpuLossPressure take_loss_pressure(crd::u64 loss_sequence, const DeviceMemoryReading& device) noexcept
{
    GpuLossPressure out;
    out.loss_sequence = loss_sequence;
    out.taken         = true;
    out.host          = read_host_memory();
    out.device        = device;
    return out;
}

} // namespace crd::gpu
