#pragma once

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/alignment.hpp>
#include <crd/memory/allocator.hpp>

#include <new> // placement new
#include <type_traits>
#include <utility>

namespace crd::containers
{
// -----------------------------------------------------------------------
// RingBuffer<T> — fixed-capacity, single-threaded FIFO queue.
//
// Layout: a circular buffer of `capacity_pow2` slots (capacity must be a
// power of two so we can mask instead of mod). `head` is where the next
// push lands; `tail` is where the next pop comes from. `m_size` tracks
// live element count for cheap empty/full queries.
//
// try_push_*  returns false when full (no overwrite — that's a wrapper's
//             job, see log's RingBufferSink in v1d).
// try_pop     returns false when empty.
// snapshot    appends a chronological copy into an Array<T>; useful for
//             debug overlays.
//
// Move-only. Single-threaded use only. v1: trade-off chosen for
// simplicity; an SPSC lock-free version comes when job system lands.
// -----------------------------------------------------------------------
template <typename T> class RingBuffer
{
public:
    explicit RingBuffer(usize capacity_pow2, memory::IAllocator* alloc = memory::default_allocator()) : m_alloc(alloc)
    {
        CRD_ASSERT(capacity_pow2 > 0);
        CRD_ASSERT(memory::is_pow2(capacity_pow2));
        m_capacity = capacity_pow2;
        m_mask = capacity_pow2 - 1;
        m_data = static_cast<T*>(m_alloc->allocate(sizeof(T) * m_capacity, alignof(T)));
    }

    ~RingBuffer()
    {
        destroy_all();
        free_buffer();
    }

    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    RingBuffer(RingBuffer&& other) noexcept
        : m_alloc(other.m_alloc), m_data(other.m_data), m_capacity(other.m_capacity), m_mask(other.m_mask),
          m_head(other.m_head), m_tail(other.m_tail), m_size(other.m_size)
    {
        other.m_data = nullptr;
        other.m_capacity = 0;
        other.m_mask = 0;
        other.m_head = 0;
        other.m_tail = 0;
        other.m_size = 0;
    }

    RingBuffer& operator=(RingBuffer&& other) noexcept
    {
        if (this == &other)
        {
            return *this;
        }
        destroy_all();
        free_buffer();
        m_alloc = other.m_alloc;
        m_data = other.m_data;
        m_capacity = other.m_capacity;
        m_mask = other.m_mask;
        m_head = other.m_head;
        m_tail = other.m_tail;
        m_size = other.m_size;
        other.m_data = nullptr;
        other.m_capacity = 0;
        other.m_mask = 0;
        other.m_head = 0;
        other.m_tail = 0;
        other.m_size = 0;
        return *this;
    }

    // ---- Producer side --------------------------------------------

    [[nodiscard]] bool try_push(const T& v)
    {
        if (m_size == m_capacity)
        {
            return false;
        }
        ::new (slot(m_head)) T(v);
        m_head = (m_head + 1) & m_mask;
        ++m_size;
        return true;
    }

    [[nodiscard]] bool try_push(T&& v)
    {
        if (m_size == m_capacity)
        {
            return false;
        }
        ::new (slot(m_head)) T(std::move(v));
        m_head = (m_head + 1) & m_mask;
        ++m_size;
        return true;
    }

    template <typename... Args> [[nodiscard]] bool try_emplace(Args&&... args)
    {
        if (m_size == m_capacity)
        {
            return false;
        }
        ::new (slot(m_head)) T(std::forward<Args>(args)...);
        m_head = (m_head + 1) & m_mask;
        ++m_size;
        return true;
    }

    // ---- Consumer side --------------------------------------------

    [[nodiscard]] bool try_pop(T& out) noexcept
    {
        if (m_size == 0)
        {
            return false;
        }
        T* p = slot(m_tail);
        out = std::move(*p);
        if constexpr (!std::is_trivially_destructible_v<T>)
        {
            p->~T();
        }
        m_tail = (m_tail + 1) & m_mask;
        --m_size;
        return true;
    }

    // ---- Inspection -----------------------------------------------

    // Append a chronological copy into `out`. Useful for debug overlays
    // that want to render the current ring contents without pulling them
    // out of the buffer.
    void snapshot(Array<T>& out) const
    {
        out.reserve(out.size() + m_size);
        usize idx = m_tail;
        for (usize i = 0; i < m_size; ++i)
        {
            out.push_back(m_data[idx]);
            idx = (idx + 1) & m_mask;
        }
    }

    // ---- Capacity / state -----------------------------------------

    usize size() const noexcept { return m_size; }
    usize capacity() const noexcept { return m_capacity; }
    bool empty() const noexcept { return m_size == 0; }
    bool full() const noexcept { return m_size == m_capacity; }

    void clear() noexcept
    {
        destroy_all();
        m_head = 0;
        m_tail = 0;
        m_size = 0;
    }

    memory::IAllocator* allocator() const noexcept { return m_alloc; }

private:
    T* slot(usize i) noexcept { return reinterpret_cast<T*>(reinterpret_cast<u8*>(m_data) + i * sizeof(T)); }

    const T* slot(usize i) const noexcept
    {
        return reinterpret_cast<const T*>(reinterpret_cast<const u8*>(m_data) + i * sizeof(T));
    }

    void destroy_all() noexcept
    {
        if constexpr (!std::is_trivially_destructible_v<T>)
        {
            if (m_data == nullptr)
            {
                return;
            }
            usize idx = m_tail;
            for (usize i = 0; i < m_size; ++i)
            {
                m_data[idx].~T();
                idx = (idx + 1) & m_mask;
            }
        }
    }

    void free_buffer() noexcept
    {
        if (m_data && m_alloc)
        {
            m_alloc->deallocate(m_data);
        }
        m_data = nullptr;
        m_capacity = 0;
        m_mask = 0;
        m_head = 0;
        m_tail = 0;
        m_size = 0;
    }

    memory::IAllocator* m_alloc = nullptr;
    T* m_data = nullptr;
    usize m_capacity = 0;
    usize m_mask = 0;
    usize m_head = 0;
    usize m_tail = 0;
    usize m_size = 0;
};
} // namespace crd::containers
