// DIAG.9a -- device records on a real D3D12 device: the DX12 twin of test_device_replay_vulkan.cpp. The fixture
// program is recorded on the device through execute_lowered_host with the compute context's own DXGI adapter. An exact
// record replays bit for bit from its file on the same adapter and is refused on the CPU reference before anything
// runs; an ulp record replays on the CPU reference within its declared bound, and one ULP less than the device's
// measured distance fails at @wave's dispatch; an edited kernel is named there on the device. Soft-skips without a
// D3D12 device. ASCII test names.

#include "../ceir-gpu-vulkan/device_replay_gate.hpp"

#include <crd/gpu/dx12_compute_context.hpp>
#include <crd/kir/ckir_hlsl.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
std::unique_ptr<crd::gpu::ComputePipeline> compile_dx12(const crd::kir::KGraph& graph, const crd::kir::KEntry& entry,
                                                        int bindings, void* user, crd::memory::IAllocator* alloc)
{
    auto* const          compute = static_cast<crd::gpu::Dx12ComputeContext*>(user);
    crd::kir::GlslKernel kernel(alloc);
    if (!crd::kir::emit_compute_kernel_hlsl(graph, entry, alloc, kernel))
    {
        return nullptr;
    }
    return compute->create_pipeline_from_hlsl(crd::containers::to_view(kernel.source), bindings, 0U);
}
} // namespace

TEST_CASE("diag 9a: a device record on DX12 replays within its declared envelope",
          "[ceir][ceir-gpu][dx12][gpu][diag]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    crd::gpu::Dx12ComputeContext       compute(&alloc);
    if (!compute.valid())
    {
        WARN("no D3D12 device available; skipping");
        return;
    }
    crd::ceir_gpu_test::device_replay::device_replay_gate(compute, &compile_dx12, &compute, "dx12",
                                                          "diag9a_device_dx12.crpl", &alloc);
}
