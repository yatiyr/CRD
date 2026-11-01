#pragma once

// HW-RESILIENCE.3: the DX12 side of crd/gpu/device_check.hpp. Provider-private.
//
// Every DX12 device created with no named adapter (Dx12DeviceScope::create, which every context and default-device
// query uses) opens the adapter dx12_checked_adapter chooses: the adapters in order of preference, hardware first
// (DXGI's high-performance order within a kind), then unclassified, then software, and WARP last when the list does
// not already hold it, each checked once per process against the known-answer kernel on a probe device of its own.
// An adapter that fails is passed over; when none passes there is no device, which consumers take as their CPU path.
// A named adapter is opened as named and is not checked: that is the probe's own route.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <dxgi.h>
#include <wrl/client.h>

namespace crd::gpu::detail
{
// The adapter a probe context opens (Dx12ComputeContext's probe constructor).
struct Dx12ProbeAdapter
{
    IDXGIAdapter1* adapter = nullptr;
};

// The preferred adapter that passed its device check (or runs unchecked), its selection noted in
// crd::gpu::device_checks(); null when no adapter passed or DXGI lists none.
[[nodiscard]] Microsoft::WRL::ComPtr<IDXGIAdapter1> dx12_checked_adapter();
} // namespace crd::gpu::detail
