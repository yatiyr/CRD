#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>

#include <crd/core/types.hpp>

namespace crd::gpu
{
enum class InnerCoverageRoute : u8; // B1-f: defined in raster_context.hpp
}

namespace crd::gpu::detail
{
// B1-f: decide the inner-coverage route for `device` from its conservative tier, OPTIONS3 barycentrics, highest shader
// model and the selected adapter's classification; honours the process-wide test override (dx12_context.hpp).
[[nodiscard]] InnerCoverageRoute dx12_inner_coverage_route(ID3D12Device* device) noexcept;
// True iff `device` can run `route` at all (Native: conservative Tier 3; Barycentric: Tier >= 1, barycentrics, SM 6.1).
[[nodiscard]] bool dx12_inner_coverage_route_runnable(ID3D12Device* device, InnerCoverageRoute route) noexcept;

// Shared command lifecycle for every provider queue. Callers latch failure into valid() and do not consume readback
// or recycle resources after a failed operation. Submission count means ExecuteCommandLists was actually called.
[[nodiscard]] HRESULT dx12_reset(ID3D12CommandAllocator* allocator, ID3D12GraphicsCommandList* list) noexcept;
[[nodiscard]] HRESULT dx12_signal(ID3D12Device* device, ID3D12CommandQueue* queue,
                                  ID3D12Fence* fence, UINT64& value) noexcept;
[[nodiscard]] HRESULT dx12_submit(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* list,
                                  ID3D12Fence* fence, UINT64& value, bool& submitted) noexcept;
[[nodiscard]] HRESULT dx12_wait(ID3D12Device* device, ID3D12Fence* fence, UINT64 value, HANDLE event,
                                DWORD timeout_ms = 30000U) noexcept;
void dx12_execution_failure(HRESULT result, const char* operation) noexcept;
} // namespace crd::gpu::detail
