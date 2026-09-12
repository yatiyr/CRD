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

// RET-4 pt 2 (ADR-0085 S6 absorbed): the pooled VkDeviceMemory block count behind `raster`'s image allocations —
// the suballocation diagnostic (a small count across many small images proves pooling; mirrors the retired rhi
// `vulkan_resident_block_count`). `raster` must be a context returned by create_vulkan_raster_context.
[[nodiscard]] crd::u32 vulkan_raster_block_count(const IRasterContext& raster) noexcept;

// RET-4 pt 3 (S7 compaction): release DRAINED pooled blocks back to the driver (a streaming system calls this on
// level unload / memory pressure). Live blocks never move — indices are stable. Returns the blocks released.
crd::u32 vulkan_raster_compact(IRasterContext& raster) noexcept;

// RET-4 pt 5 (S7 relocation, absorbed from ADR-0085): relocate every live storage buffer's device allocation to a
// fresh suballocation (recreate + GPU copy + swap — contents preserved), healing fragmentation before a compact().
// Idle-gated by the context's synchronous submission model. Returns the number of relocations performed.
crd::u32 vulkan_raster_defragment(IRasterContext& raster) noexcept;

// RET-5: the swapchain parameters an overlay backend (crd-imgui's gpu backend) initializes against. `surface` must
// be a Vulkan present surface (create_present_surface of a Vulkan raster context). The format is the raw VkFormat
// value as u32 (this header stays Vulkan-type-free at the API line).
[[nodiscard]] crd::u32 vulkan_present_image_count(const IPresentSurface& surface) noexcept;
[[nodiscard]] crd::u32 vulkan_present_color_format_raw(const IPresentSurface& surface) noexcept;

} // namespace crd::gpu
