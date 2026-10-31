#pragma once

// crd-sandbox -- DIAG.9a: the sandbox's GPU device executor for the replay commands' device records.
//
// A compute context on the sandbox's backend, crd-ceir-gpu's run_device_block (crd/ceir/gpu/device_run.hpp) over that
// context's CKIR kernel pipelines (create_pipeline_from_ckir) and the context's own adapter, as a cook::DeviceExecutor.
// Vulkan: a VulkanComputeContext over the sandbox's device, which submits on the device's compute queue (the graphics
// queue the frame loop submits to when the device has no dedicated compute family). DX12: a Dx12ComputeContext, which
// opens its own D3D12 device and compute queue on the default adapter. A run uses the context on the calling thread;
// the sandbox hands every run to its frame-loop thread (FrameThreadDevice, frame_thread_calls.hpp) on both backends.
// Contract: docs/design/runtime-diagnostics.md (DIAG.9a).

#include <crd/ceir/cook/device_replay.hpp>
#include <crd/ceir/gpu/device_run.hpp>
#include <crd/core/types.hpp>
#include <crd/gpu/compute.hpp>
#include <crd/memory/allocator.hpp>

#include <memory>

namespace crd::gpu
{
class VulkanGpuContext;
} // namespace crd::gpu

namespace crd::sandbox
{
class GpuDeviceExecutor
{
public:
    // A device executor over a compute context on `device`. nullptr when the context is not valid or reports no
    // adapter. `alloc` backs each run's lowering, kernel graphs and pipelines; `device` and `alloc` must outlive it.
    [[nodiscard]] static std::unique_ptr<GpuDeviceExecutor> vulkan(gpu::VulkanGpuContext& device,
                                                                   memory::IAllocator*    alloc);
#ifdef _WIN32
    // A device executor over a Dx12ComputeContext (its own device on the default adapter). nullptr as above.
    [[nodiscard]] static std::unique_ptr<GpuDeviceExecutor> dx12(memory::IAllocator* alloc);
#endif

    GpuDeviceExecutor(const GpuDeviceExecutor&)            = delete;
    GpuDeviceExecutor& operator=(const GpuDeviceExecutor&) = delete;
    GpuDeviceExecutor(GpuDeviceExecutor&&)                 = delete;
    GpuDeviceExecutor& operator=(GpuDeviceExecutor&&)      = delete;
    ~GpuDeviceExecutor();

    // The executor; its adapter views point into this object.
    [[nodiscard]] const ceir::cook::DeviceExecutor& executor() const noexcept { return m_executor; }
    [[nodiscard]] const gpu::GpuAdapter&            adapter() const noexcept { return m_adapter; }
    // Runs started on the device (evidence).
    [[nodiscard]] crd::u32 runs() const noexcept { return m_rig.runs; }

private:
    GpuDeviceExecutor(std::unique_ptr<gpu::IComputeContext> compute, ceir::gpu::KernelCompileFn compile,
                      memory::IAllocator* alloc);

    [[nodiscard]] static bool run(ceir::Context& ctx, const ceir::Module& module,
                                  containers::ConstSpan<ceir::cook::DeviceKernelSource> kernels,
                                  containers::Span<ceir::cook::DeviceBufferView> buffers,
                                  ceir::cook::DeviceRunOutcome& out, containers::String& reason, void* user);

    std::unique_ptr<gpu::IComputeContext> m_compute;
    ceir::gpu::DeviceRunRig               m_rig;
    gpu::GpuAdapter                       m_adapter;
    ceir::cook::DeviceExecutor            m_executor;
};
} // namespace crd::sandbox
