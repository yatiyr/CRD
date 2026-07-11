#pragma once

// crd-gpu-context-vulkan — the Vulkan `IRasterContext` (ADR-0103 / D-008 C1). Draws through DYNAMIC RENDERING on a
// graphics queue drawn from a `VulkanGpuContext` (which now creates one). Offscreen targets + pixel readback; the
// shader-object DRAW path lands in C1-b. Raw Vulkan — NO dependency on crd-rhi (C2 absorbs rhi INTO here; the edge must
// not point back).

#include <crd/gpu/raster_context.hpp>
#include <crd/gpu/vulkan_context.hpp>

#include <crd/memory/allocator.hpp>

#include <memory>

namespace crd::gpu
{

// Create a Vulkan raster context over `ctx` (must be `graphics_capable()`). Returns nullptr otherwise.
[[nodiscard]] std::unique_ptr<IRasterContext>
create_vulkan_raster_context(VulkanGpuContext& ctx, crd::memory::IAllocator* alloc = crd::memory::default_allocator());

} // namespace crd::gpu
