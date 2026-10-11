#pragma once

// DIAG.10c: the DX12 side of device and host pressure evidence (crd/gpu/device_pressure.hpp). Provider-private.

#include <crd/gpu/device_pressure.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>

namespace crd::gpu::detail
{
// The adapter `device` runs on, as DXGI reports it for this process: the local and non-local segment groups of
// IDXGIAdapter3::QueryVideoMemoryInfo (node 0). Both are per process and per adapter, so every device of the process
// on that adapter counts. Safe after the device is removed (the query goes to the adapter, not the device).
[[nodiscard]] DeviceMemoryReading dx12_device_memory(ID3D12Device* device) noexcept;

// The pressure taken the last time a removal was recorded (dx12_record_removal); its loss_sequence is that record's
// sequence. `taken` is false before any removal.
[[nodiscard]] GpuLossPressure dx12_last_loss_pressure() noexcept;
} // namespace crd::gpu::detail
