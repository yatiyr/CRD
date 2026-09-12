#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>

namespace crd::gpu::detail
{
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
