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
    bool kernel_render = false;
    u32 dxgi_vendor = 0;
    u32 dxgi_device = 0;
};

// Backend-private identity query; explicit software adapters can exercise the same production mechanism.
[[nodiscard]] Dx12AdapterKind query_dx12_adapter_kind(u32 luid_low, i32 luid_high) noexcept;

[[nodiscard]] constexpr Dx12AdapterKind classify_dx12_adapter(const Dx12AdapterEvidence& evidence) noexcept
{
    // Microsoft's documented BasicRender identity covers the primary display-backed variant as well as WARP.
    // That primary variant can omit BOTH software flags; a display-capable kernel adapter is not a hardware renderer.
    // See docs/recipes/2026-09-12-dx12-adapter-classification.md. Neither a vendor alone nor a name is a match.
    if (evidence.dxgi_available && evidence.dxgi_vendor == 0x1414U && evidence.dxgi_device == 0x008cU)
    {
        return Dx12AdapterKind::Software;
    }
    if (evidence.kernel_available)
    {
        if (evidence.kernel_software) { return Dx12AdapterKind::Software; }
        if (evidence.kernel_render)
        {
            if (evidence.dxgi_available && evidence.dxgi_software) { return Dx12AdapterKind::Unknown; }
            return Dx12AdapterKind::Hardware;
        }
    }
    if (evidence.dxgi_available && evidence.dxgi_software) { return Dx12AdapterKind::Software; }
    return Dx12AdapterKind::Unknown;
}

} // namespace crd::gpu::detail
