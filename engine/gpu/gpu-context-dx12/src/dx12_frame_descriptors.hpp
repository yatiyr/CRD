#pragma once

#include "dx12_execution.hpp"

#include <crd/memory/allocator.hpp>
#include <wrl/client.h>

namespace crd::gpu::detail
{
// Native recording storage, not an authored resource table (RAH-2). A complete command reserves one contiguous
// range before binding anything. Published ranges stay immutable until the graph's submission has retired.
struct Dx12DescriptorLimits
{
    UINT initial_page = 256U;
    UINT maximum_page = 65536U;
    UINT total_slots = 262144U;
};

struct Dx12DescriptorRange
{
    ID3D12DescriptorHeap* heap = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
    UINT count = 0U;
};

class Dx12FrameDescriptors final
{
public:
    Dx12FrameDescriptors(ID3D12Device* device, crd::memory::IAllocator* allocator,
                         Dx12DescriptorLimits limits = {}) noexcept;
    ~Dx12FrameDescriptors();
    Dx12FrameDescriptors(const Dx12FrameDescriptors&) = delete;
    Dx12FrameDescriptors& operator=(const Dx12FrameDescriptors&) = delete;
    Dx12FrameDescriptors(Dx12FrameDescriptors&&) = delete;
    Dx12FrameDescriptors& operator=(Dx12FrameDescriptors&&) = delete;

    [[nodiscard]] HRESULT reserve(UINT count, Dx12DescriptorRange& out) noexcept;
    // Caller must have retired ALL commands referencing these pages. Reset does not wait or submit for the caller.
    void reset_after_retirement() noexcept;
    [[nodiscard]] UINT increment() const noexcept { return m_increment; }
    [[nodiscard]] UINT capacity() const noexcept { return m_capacity; }
    [[nodiscard]] UINT used() const noexcept { return m_used; }
    [[nodiscard]] UINT pages() const noexcept { return m_pages; }

private:
    struct Page
    {
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
        Page* next = nullptr;
        UINT capacity = 0U;
        UINT used = 0U;
    };
    ID3D12Device* m_device = nullptr;
    crd::memory::IAllocator* m_allocator = nullptr;
    Dx12DescriptorLimits m_limits{};
    Page* m_head = nullptr;
    Page* m_current = nullptr;
    UINT m_increment = 0U;
    UINT m_capacity = 0U;
    UINT m_used = 0U;
    UINT m_pages = 0U;
};
} // namespace crd::gpu::detail
