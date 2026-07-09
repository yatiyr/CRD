// test_vulkan_context.cpp — Phase 3.1.6 v17-i-a (ADR-0099): the headless Vulkan compute context stands up on its own,
// with a compute queue + (on capable adapters) the coopmat2 tensor feature — no rendering RHI, no swapchain. This is
// the foundation kir-vulkan migrates onto in v17-i-b.

#include <crd/gpu/vulkan_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <memory>

namespace gpu = crd::gpu;

TEST_CASE("v17-i-a: headless Vulkan compute context via the GpuContextManager", "[gpu-context][vulkan][gpu]")
{
    gpu::GpuContextManager mgr;
    gpu::GpuContextConfig  cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;

    auto ctx = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr)
    {
        WARN("no Vulkan device available; skipping");
        return;
    }

    gpu::IGpuContext* held = mgr.add(std::move(ctx)); // manager takes ownership
    REQUIRE(held != nullptr);
    CHECK(held->valid());
    CHECK(held->backend() == gpu::GpuBackend::Vulkan);
    CHECK(mgr.get(gpu::GpuBackend::Vulkan) == held); // the manager serves it back
    CHECK(mgr.count() == 1);

    auto* vk = static_cast<gpu::VulkanGpuContext*>(held); // backend()==Vulkan ⇒ safe downcast
    CHECK(vk->vk_instance() != VK_NULL_HANDLE);
    CHECK(vk->vk_device() != VK_NULL_HANDLE);
    CHECK(vk->compute_queue() != VK_NULL_HANDLE); // a real compute queue, not borrowed from a graphics-only path
    std::printf("[gpu-context-vulkan] adapter=%s  coopmat2=%s  compute_family=%u\n",
                vk->adapter_name(), vk->cooperative_matrix2() ? "YES" : "no", vk->compute_family());
    // coopmat2 is the tensor lever (present on the RTX 4070 Ti Super); a soft note so the test stays portable.
    if (!vk->cooperative_matrix2()) { WARN("adapter has no VK_NV_cooperative_matrix2 — tensor tier will be unavailable"); }
}
