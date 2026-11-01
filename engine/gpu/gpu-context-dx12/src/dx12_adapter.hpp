#pragma once

// DIAG.9a, HW-RESILIENCE.2: the DX12 side of crd/gpu/adapter.hpp. Provider-private.

#include <crd/gpu/adapter.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
#include <dxgi.h>

namespace crd::gpu::detail
{
// The DXGI adapter `dxgi`: its description, PCI ids and user-mode driver version (the Windows driver version,
// "32.0.15.8157"). Unknown for a null adapter or when DXGI cannot describe it.
[[nodiscard]] GpuAdapter dx12_adapter(IDXGIAdapter1* dxgi) noexcept;

// The same for the adapter `device` runs on. Unknown for a null device or when DXGI cannot name the adapter.
[[nodiscard]] GpuAdapter dx12_adapter(ID3D12Device* device) noexcept;
} // namespace crd::gpu::detail
