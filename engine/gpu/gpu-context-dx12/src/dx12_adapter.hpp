#pragma once

// DIAG.9a, HW-RESILIENCE.2: the DX12 side of crd/gpu/adapter.hpp. Provider-private.

#include <crd/gpu/adapter.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>

namespace crd::gpu::detail
{
// The DXGI adapter `device` runs on: its description, PCI ids and user-mode driver version (the Windows driver
// version, "32.0.15.8157"). Unknown for a null device or when DXGI cannot name the adapter.
[[nodiscard]] GpuAdapter dx12_adapter(ID3D12Device* device) noexcept;
} // namespace crd::gpu::detail
