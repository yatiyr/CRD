#pragma once

#include <crd/core/assert.hpp>
#include <crd/core/platform.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/alignment.hpp>
#include <crd/memory/allocator.hpp>

#include <atomic>
#include <new>
#include <type_traits>
#include <utility>

// MSVC warns C4324 whenever alignas() pads a struct. For SpscQueue the padding
// is intentional — the head/tail atomics must sit on separate cache lines.
#if CRD_COMPILER_MSVC
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

namespace crd::containers
{

/// Lock-free single-producer / single-consumer FIFO queue.
///
/// Capacity must be a power of two. One slot is sacrificed to distinguish
/// full from empty, so `capacity()` returns `capacity_pow2 - 1`.
///
/// Thread-safety contract:
///   - Exactly one thread calls try_push / try_emplace at a time (producer).
///   - Exactly one thread calls try_pop at a time (consumer).
///   - Producer and consumer may run concurrently on different threads.
///   - size() / empty() / full() are diagnostic snapshots and are not
///     reliable for flow-control decisions across threads.
///
/// Move is intentionally deleted: moving a live queue while another thread
/// is accessing it is inherently unsafe.
template <typename T>
class SpscQueue
{
public:
    explicit SpscQueue(usize capacity_pow2, memory::IAllocator* alloc = memory::default_allocator())
        : m_alloc(alloc)
    {
        CRD_ASSERT(capacity_pow2 > 1U); // need at least 2 slots (1 usable + 1 sentinel)
        CRD_ASSERT(memory::is_pow2(capacity_pow2));
        m_capacity = capacity_pow2;
        m_mask = capacity_pow2 - 1U;
        m_data = static_cast<T*>(m_alloc->allocate(sizeof(T) * m_capacity, alignof(T)));
    }

    ~SpscQueue()
    {
        drain_remaining();
        if (m_data && m_alloc)
        {
            m_alloc->deallocate(m_data);
        }
        m_data = nullptr;
    }

    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;
    SpscQueue(SpscQueue&&) = delete;
    SpscQueue& operator=(SpscQueue&&) = delete;

    // ---- Producer side (call only from the single producer thread) -----

    [[nodiscard]] bool try_push(const T& v) noexcept(std::is_nothrow_copy_constructible_v<T>)
    {
        const usize write = m_write.load(std::memory_order_relaxed);
        const usize next  = (write + 1U) & m_mask;
        if (next == m_read.load(std::memory_order_acquire))
        {
            return false; // full
        }
        ::new (slot(write)) T(v);
        m_write.store(next, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool try_push(T&& v) noexcept(std::is_nothrow_move_constructible_v<T>)
    {
        const usize write = m_write.load(std::memory_order_relaxed);
        const usize next  = (write + 1U) & m_mask;
        if (next == m_read.load(std::memory_order_acquire))
        {
            return false;
        }
        ::new (slot(write)) T(std::move(v));
        m_write.store(next, std::memory_order_release);
        return true;
    }

    template <typename... Args>
    [[nodiscard]] bool try_emplace(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args...>)
    {
        const usize write = m_write.load(std::memory_order_relaxed);
        const usize next  = (write + 1U) & m_mask;
        if (next == m_read.load(std::memory_order_acquire))
        {
            return false;
        }
        ::new (slot(write)) T(std::forward<Args>(args)...);
        m_write.store(next, std::memory_order_release);
        return true;
    }

    // ---- Consumer side (call only from the single consumer thread) -----

    [[nodiscard]] bool try_pop(T& out) noexcept(std::is_nothrow_move_assignable_v<T>)
    {
        const usize read = m_read.load(std::memory_order_relaxed);
        if (read == m_write.load(std::memory_order_acquire))
        {
            return false; // empty
        }
        T* p = slot(read);
        out  = std::move(*p);
        if constexpr (!std::is_trivially_destructible_v<T>)
        {
            p->~T();
        }
        m_read.store((read + 1U) & m_mask, std::memory_order_release);
        return true;
    }

    // ---- Capacity / state (approximate — diagnostic use only) ----------

    /// Usable capacity: one slot is sacrificed to distinguish full from empty.
    [[nodiscard]] usize capacity() const noexcept { return m_capacity - 1U; }

    /// Snapshot element count. Races with concurrent push/pop — use for diagnostics only.
    [[nodiscard]] usize size() const noexcept
    {
        const usize w = m_write.load(std::memory_order_acquire);
        const usize r = m_read.load(std::memory_order_acquire);
        return (w + m_capacity - r) & m_mask;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return m_read.load(std::memory_order_acquire) == m_write.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool full() const noexcept
    {
        const usize write = m_write.load(std::memory_order_acquire);
        const usize next  = (write + 1U) & m_mask;
        return next == m_read.load(std::memory_order_acquire);
    }

    [[nodiscard]] memory::IAllocator* allocator() const noexcept { return m_alloc; }

private:
    T* slot(usize i) noexcept
    {
        return reinterpret_cast<T*>(reinterpret_cast<u8*>(m_data) + i * sizeof(T));
    }

    void drain_remaining() noexcept
    {
        if constexpr (!std::is_trivially_destructible_v<T>)
        {
            if (!m_data)
            {
                return;
            }
            usize r       = m_read.load(std::memory_order_relaxed);
            const usize w = m_write.load(std::memory_order_relaxed);
            while (r != w)
            {
                slot(r)->~T();
                r = (r + 1U) & m_mask;
            }
        }
    }

    memory::IAllocator* m_alloc   = nullptr;
    T*                  m_data    = nullptr;
    usize               m_capacity = 0;
    usize               m_mask     = 0;

    // Producer-owned write cursor — on its own cache line to avoid false sharing with m_read.
    alignas(64) std::atomic<usize> m_write{0};
    // Consumer-owned read cursor — on its own cache line to avoid false sharing with m_write.
    alignas(64) std::atomic<usize> m_read{0};
};

} // namespace crd::containers

#if CRD_COMPILER_MSVC
#pragma warning(pop)
#endif
