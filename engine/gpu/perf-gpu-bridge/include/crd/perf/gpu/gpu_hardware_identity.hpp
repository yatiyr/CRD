#pragma once

// crd-perf-gpu-bridge -- the GPUs of the machine's hardware identity (HW-RESILIENCE.2).
//
// crd-perf's hardware identity (crd/perf/hardware_identity.hpp) names the GPUs a crash or failure ran beside, but
// crd-perf may not see a GPU API and crd-gpu-context may not link crd-perf; this bridge, the one module that names
// both, carries each adapter crd::gpu::opened_adapters() logs (every device a backend creates: Vulkan, DX12, CUDA)
// into crd::perf::add_host_gpu. The adapter was read when its device was created, so nothing here asks a driver.

#include <crd/gpu/adapter.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/perf/hardware_identity.hpp>

namespace crd::perf::gpu
{
// `adapter` as a hardware identity entry: its PCI ids, its name, and as the driver the backend and the version its API
// reports ("dx12 32.0.15.8157", "vulkan 581.57", "cuda 13.0"), since one GPU seen through two APIs reports two
// versions of one driver.
[[nodiscard]] crd::perf::GpuIdentity gpu_identity(const crd::gpu::GpuAdapter& adapter,
                                                  crd::memory::IAllocator* alloc = crd::memory::default_allocator());

// From now on every adapter the process opens a device on, and every one it opened before, joins
// crd::perf::host_hardware_identity(), so every later flight bundle and native crash names it. A host calls it once,
// where it starts its diagnostics (beside attach_hardware_identity_to_crashes).
void attach_gpus_to_hardware_identity() noexcept;
} // namespace crd::perf::gpu
