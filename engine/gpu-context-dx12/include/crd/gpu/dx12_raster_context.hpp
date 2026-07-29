#pragma once

// crd-gpu-context-dx12 — the DIRECT3D 12 `IRasterContext` (ADR-0103 / D-008 C4): the DX12 mirror of the Vulkan raster
// context (C1). It draws IR-authored programs (HLSL VS+FS → DXIL, B3-d) into offscreen targets, so the DX12 half of every
// render gate stops being "unreachable". Raw D3D12 — a graphics command queue + committed render targets + readback; the
// backend-agnostic seam is `IRasterContext`, which consumers depend on, never on D3D12. WINDOWS-ONLY; self-skips
// elsewhere (the factory returns nullptr).
//
// C4-a (this slice): graphics device + queue + offscreen RGBA8 targets + a CLEAR with pixel readback (the render-target /
// RTV / readback plumbing, green). C4-b appends the shader DRAW (a graphics pipeline from a VS+FS DXIL pair + `DrawInstanced`).

#include <crd/gpu/raster_context.hpp>

#include <crd/memory/allocator.hpp>

#include <memory>

namespace crd::gpu
{

// Create a D3D12 raster context. Returns nullptr if D3D12 / a graphics device is unavailable (non-Windows, no adapter).
// The returned object is an `IRasterContext`; a consumer never sees D3D12.
[[nodiscard]] std::unique_ptr<IRasterContext>
create_dx12_raster_context(crd::memory::IAllocator* alloc = crd::memory::default_allocator());

// ⭐ REN-39-D2: the DX12 twin of `vulkan_present_*` — the handful of native handles a BACKEND-AWARE consumer
// (the ImGui render backend, and nothing else) needs, handed over as opaque pointers so no caller has to include
// d3d12.h. Same posture as the Vulkan accessors: narrow, named, and the only door out of the abstraction.
[[nodiscard]] void*    dx12_device_raw(IRasterContext& raster) noexcept;         // ID3D12Device*
[[nodiscard]] void*    dx12_graphics_queue_raw(IRasterContext& raster) noexcept; // ID3D12CommandQueue*
[[nodiscard]] crd::u32 dx12_present_image_count(const IPresentSurface& surface) noexcept;
[[nodiscard]] crd::u32 dx12_present_color_format_raw(const IPresentSurface& surface) noexcept; // DXGI_FORMAT

} // namespace crd::gpu
