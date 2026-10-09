// crd-sandbox -- DIAG.9a: the sandbox's GPU device executor. Declared in gpu_device_executor.hpp.

#include "gpu_device_executor.hpp"

#include <crd/ceir/gpu/execute.hpp>
#include <crd/containers/array.hpp>
#include <crd/gpu/vulkan_compute_context.hpp>
#include <crd/gpu/vulkan_context.hpp>
#ifdef _WIN32
#include <crd/gpu/dx12_compute_context.hpp>
#endif

namespace crd::sandbox
{
namespace
{
namespace ceg = ceir::gpu;

std::unique_ptr<gpu::ComputePipeline> compile_vulkan(const kir::KGraph& graph, const kir::KEntry& entry, int bindings,
                                                     void* user, memory::IAllocator* alloc)
{
    return static_cast<gpu::VulkanComputeContext*>(user)->create_pipeline_from_ckir(graph, entry, bindings, 0U, alloc);
}

#ifdef _WIN32
std::unique_ptr<gpu::ComputePipeline> compile_dx12(const kir::KGraph& graph, const kir::KEntry& entry, int bindings,
                                                   void* user, memory::IAllocator* alloc)
{
    return static_cast<gpu::Dx12ComputeContext*>(user)->create_pipeline_from_ckir(graph, entry, bindings, 0U, alloc);
}
#endif
} // namespace

GpuDeviceExecutor::GpuDeviceExecutor(std::unique_ptr<gpu::IComputeContext> compute, ceg::KernelCompileFn compile,
                                     memory::IAllocator* alloc)
    : m_compute(std::move(compute))
{
    m_rig.device       = m_compute.get();
    m_rig.compile      = compile;
    m_rig.compile_user = m_compute.get();
    m_rig.alloc        = alloc;
    m_adapter          = m_compute->adapter();
    m_executor.run     = &GpuDeviceExecutor::run;
    m_executor.user    = this;
    m_executor.adapter.backend = containers::StringView{m_adapter.backend};
    m_executor.adapter.name    = containers::StringView{m_adapter.name};
    m_executor.adapter.vendor  = m_adapter.vendor;
    m_executor.adapter.device  = m_adapter.device;
    m_executor.adapter.driver  = m_adapter.driver;
    m_executor.adapter.api     = m_adapter.api;
}

GpuDeviceExecutor::~GpuDeviceExecutor() = default;

std::unique_ptr<GpuDeviceExecutor> GpuDeviceExecutor::vulkan(gpu::VulkanGpuContext& device, memory::IAllocator* alloc)
{
    auto compute = std::make_unique<gpu::VulkanComputeContext>(device, alloc);
    if (!compute->valid())
    {
        return nullptr;
    }
    std::unique_ptr<GpuDeviceExecutor> e(new GpuDeviceExecutor(std::move(compute), &compile_vulkan, alloc));
    if (!e->m_adapter.known)
    {
        return nullptr;
    }
    return e;
}

#ifdef _WIN32
std::unique_ptr<GpuDeviceExecutor> GpuDeviceExecutor::dx12(memory::IAllocator* alloc)
{
    auto compute = std::make_unique<gpu::Dx12ComputeContext>(alloc);
    if (!compute->valid())
    {
        return nullptr;
    }
    std::unique_ptr<GpuDeviceExecutor> e(new GpuDeviceExecutor(std::move(compute), &compile_dx12, alloc));
    if (!e->m_adapter.known)
    {
        return nullptr;
    }
    return e;
}
#endif

bool GpuDeviceExecutor::run(ceir::Context& ctx, const ceir::Module& module,
                            containers::ConstSpan<ceir::cook::DeviceKernelSource> kernels,
                            containers::Span<ceir::cook::DeviceBufferView> buffers, ceir::cook::DeviceRunOutcome& out,
                            containers::String& reason, void* user)
{
    auto* const self = static_cast<GpuDeviceExecutor*>(user);
    if (buffers.size() > ceg::kMaxHostBufferBindings)
    {
        reason.append("the program declares more buffers than a device run binds");
        return false;
    }
    containers::Array<ceg::DeviceKernelText> texts(self->m_rig.alloc);
    for (const ceir::cook::DeviceKernelSource& k : kernels)
    {
        texts.push_back(ceg::DeviceKernelText{k.symbol, k.ckir});
    }
    ceg::HostBufferBinding host[ceg::kMaxHostBufferBindings];
    for (usize i = 0U; i < buffers.size(); ++i)
    {
        host[i] = ceg::HostBufferBinding{buffers[i].resource, buffers[i].words.data(),
                                         static_cast<crd::u64>(buffers[i].words.size()) * sizeof(crd::u32),
                                         buffers[i].written};
    }
    ceg::DeviceRunResult result;
    if (!ceg::run_device_block(ctx, *module.body()->first_block(),
                               containers::ConstSpan<ceg::DeviceKernelText>(texts.data(), texts.size()),
                               containers::ConstSpan<ceg::HostBufferBinding>(host, buffers.size()), self->m_rig, result,
                               reason))
    {
        return false;
    }
    out.error    = static_cast<crd::u8>(result.error);
    out.fault_op = result.fault_op;
    return true;
}
} // namespace crd::sandbox
