#include "dx12_frame_descriptors.hpp"

#include <memory>

namespace crd::gpu::detail
{
Dx12FrameDescriptors::Dx12FrameDescriptors(ID3D12Device* device, crd::memory::IAllocator* allocator,
                                           Dx12DescriptorLimits limits) noexcept
    : m_device(device), m_allocator(allocator), m_limits(limits)
{
    if (device != nullptr)
    {
        m_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
}

Dx12FrameDescriptors::~Dx12FrameDescriptors()
{
    while (m_head != nullptr)
    {
        Page* page = m_head;
        m_head = page->next;
        std::destroy_at(page);
        m_allocator->deallocate(page);
    }
}

HRESULT Dx12FrameDescriptors::reserve(UINT count, Dx12DescriptorRange& out) noexcept
{
    out = {};
    if (m_device == nullptr || m_allocator == nullptr || m_increment == 0U || count == 0U
        || m_limits.initial_page == 0U || m_limits.initial_page > m_limits.maximum_page
        || m_limits.maximum_page > D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_1
        || m_limits.initial_page > m_limits.total_slots)
    {
        return E_INVALIDARG;
    }
    if (count > m_limits.maximum_page || count > m_limits.total_slots - m_used) { return E_OUTOFMEMORY; }

    Page* page = m_current;
    if (page == nullptr || count > page->capacity - page->used)
    {
        // Append only to unwritten tails. Switching back to an older page is safe; its published slots stay intact.
        page = m_head;
        while (page != nullptr && count > page->capacity - page->used) { page = page->next; }
        if (page == nullptr)
        {
            const UINT available = m_limits.total_slots - m_capacity;
            if (count > available) { return E_OUTOFMEMORY; }
            UINT capacity = m_current != nullptr ? m_current->capacity : m_limits.initial_page;
            if (m_current != nullptr && capacity < m_limits.maximum_page)
            {
                capacity = capacity <= m_limits.maximum_page / 2U ? capacity * 2U : m_limits.maximum_page;
            }
            while (capacity < count)
            {
                capacity = capacity <= m_limits.maximum_page / 2U ? capacity * 2U : m_limits.maximum_page;
            }
            if (capacity > available) { capacity = available; }
            void* storage = m_allocator->try_allocate(sizeof(Page), alignof(Page));
            if (storage == nullptr) { return E_OUTOFMEMORY; }
            page = std::construct_at(static_cast<Page*>(storage));
            D3D12_DESCRIPTOR_HEAP_DESC desc{};
            desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            desc.NumDescriptors = capacity;
            desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            const HRESULT result = m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&page->heap));
            if (FAILED(result))
            {
                std::destroy_at(page);
                m_allocator->deallocate(page);
                return result;
            }
            page->capacity = capacity;
            page->next = m_head;
            m_head = page;
            m_capacity += capacity;
            ++m_pages;
        }
        m_current = page;
    }
    out.heap = page->heap.Get();
    out.cpu = page->heap->GetCPUDescriptorHandleForHeapStart();
    out.gpu = page->heap->GetGPUDescriptorHandleForHeapStart();
    out.cpu.ptr += static_cast<SIZE_T>(page->used) * m_increment;
    out.gpu.ptr += static_cast<UINT64>(page->used) * m_increment;
    out.count = count;
    page->used += count;
    m_used += count;
    return S_OK;
}

void Dx12FrameDescriptors::reset_after_retirement() noexcept
{
    // Warm frames start at the largest existing page, minimizing heap switches without rewriting queued ranges.
    m_current = nullptr;
    for (Page* page = m_head; page != nullptr; page = page->next)
    {
        page->used = 0U;
        if (m_current == nullptr || page->capacity > m_current->capacity) { m_current = page; }
    }
    m_used = 0U;
}
} // namespace crd::gpu::detail
