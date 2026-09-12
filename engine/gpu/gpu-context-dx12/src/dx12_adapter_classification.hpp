#pragma once

#include <crd/gpu/dx12_context.hpp>

namespace crd::gpu::detail
{

struct Dx12AdapterEvidence
{
    bool dxgi_available = false;
    bool dxgi_software = false;
    bool kernel_available = false;
    bool kernel_software = false;
};

// Backend-private identity query; explicit software adapters can exercise the same production mechanism.
[[nodiscard]] Dx12AdapterKind query_dx12_adapter_kind(u32 luid_low, i32 luid_high) noexcept;

[[nodiscard]] constexpr Dx12AdapterKind classify_dx12_adapter(const Dx12AdapterEvidence& evidence) noexcept
{
    if (evidence.kernel_available)
    {
        if (evidence.kernel_software) { return Dx12AdapterKind::Software; }
        if (evidence.dxgi_available && evidence.dxgi_software) { return Dx12AdapterKind::Unknown; }
        return Dx12AdapterKind::Hardware;
    }
    if (evidence.dxgi_available && evidence.dxgi_software) { return Dx12AdapterKind::Software; }
    return Dx12AdapterKind::Unknown;
}

} // namespace crd::gpu::detail
