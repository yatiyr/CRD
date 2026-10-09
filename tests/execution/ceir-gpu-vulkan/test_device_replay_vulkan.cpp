// DIAG.9a -- device records on a real Vulkan device. The two-dispatch program of device_replay_fixture.hpp (an exact
// affine kernel, then exp times an input) is recorded on the device through execute_lowered_host with the compute
// context's own adapter. An exact record replays bit for bit from its file on the same adapter and is refused on the
// CPU reference before anything runs; an ulp record replays on the CPU reference within its declared bound, and one
// ULP less than the device's measured distance fails at @wave's dispatch; an edited kernel is named there on the
// device. Soft-skips without a Vulkan device. ASCII test names.

#include "device_replay_gate.hpp"

#include <crd/gpu/vulkan_compute_context.hpp>
#include <crd/gpu/vulkan_context.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
std::unique_ptr<crd::gpu::ComputePipeline> compile_vulkan(const crd::kir::KGraph& graph, const crd::kir::KEntry& entry,
                                                          int bindings, void* user, crd::memory::IAllocator* alloc)
{
    return static_cast<crd::gpu::VulkanComputeContext*>(user)->create_pipeline_from_ckir(graph, entry, bindings, 0U,
                                                                                         alloc);
}
} // namespace

TEST_CASE("diag 9a: a device record on Vulkan replays within its declared envelope",
          "[ceir][ceir-gpu][vulkan][gpu][diag]")
{
    crd::gpu::GpuContextConfig cfg{};
    cfg.backend  = crd::gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = crd::gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr)
    {
        WARN("no Vulkan device available; skipping");
        return;
    }
    auto* const vk = static_cast<crd::gpu::VulkanGpuContext*>(ctx.get());
    REQUIRE(vk->valid());
    crd::memory::GrowableTlsfAllocator alloc;
    crd::gpu::VulkanComputeContext     compute(*vk, &alloc);
    REQUIRE(compute.valid());
    crd::ceir_gpu_test::device_replay::device_replay_gate(compute, &compile_vulkan, &compute, "vulkan",
                                                          "diag9a_device_vulkan.crpl", &alloc);
}
