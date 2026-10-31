// crd-perf-gpu-bridge -- the GPUs of the machine's hardware identity (HW-RESILIENCE.2). See gpu_hardware_identity.hpp.

#include <crd/perf/gpu/gpu_hardware_identity.hpp>

namespace crd::perf::gpu
{
namespace
{
void add_opened_adapter(const crd::gpu::GpuAdapter& adapter)
{
    (void)crd::perf::add_host_gpu(gpu_identity(adapter));
}
} // namespace

crd::perf::GpuIdentity gpu_identity(const crd::gpu::GpuAdapter& adapter, crd::memory::IAllocator* alloc)
{
    crd::perf::GpuIdentity g(alloc);
    g.vendor_id = adapter.vendor;
    g.device_id = adapter.device;
    g.name.append(adapter.name);
    g.driver.append(adapter.backend);
    if (adapter.driver_version[0] != '\0')
    {
        g.driver.push_back(' ');
        g.driver.append(adapter.driver_version);
    }
    return g;
}

void attach_gpus_to_hardware_identity() noexcept
{
    crd::gpu::opened_adapters().listen(&add_opened_adapter);
}
} // namespace crd::perf::gpu
