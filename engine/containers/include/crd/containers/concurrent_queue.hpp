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

// MSVC C4324: structure padded due to alignment specifier. Intentional — the
// enqueue/dequeue position counters sit on separate cache lines so producers
// and consumers don't ping-pong a shared line.
#if CRD_COMPILER_MSVC
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

namespace crd::containers
{

/// Lock-free bounded multi-producer / multi-consumer FIFO queue (Dmitry Vyukov,
/// 1024cores.net "bounded MPMC queue"). The public, allocator-aware sibling of
/// SpscQueue — promoted from the crd-jobs scheduler's internal injection queue
/// (detour D-002 v3).
///
/// Capacity must be a power of two (>= 2), fixed at construction. Zero dynamic
/// allocation after construction (one cell array from the supplied allocator).
///
/// Thread-safety contract:
///   - try_push / try_emplace are safe to call concurrently from any number of
///     producer threads.
///   - try_pop is safe to call concurrently from any number of consumer threads.
///   - Producers and consumers run concurrently with each other.
///   - try_push returns false (no blocking) when the queue is full; try_pop
///     returns false when it is empty.
///   - size() / empty() are diagnostic snapshots — they race with concurrent
///     push/pop and must not drive flow-control decisions across threads.
///
/// T must be trivially copyable. A cell's payload is "alive" between a
/// producer's publish and a consumer's claim and is moved by plain assignment;
/// supporting non-trivially-copyable T in a lock-free cell would need a
/// placement-new / explicit-destroy dance that isn't worth it until a consumer
/// asks for it. (Same constraint the crd-jobs MpmcQueue carried.)
///
/// Move/copy are deleted: relocating a live queue while another thread touches
/// it is inherently unsafe (matches SpscQueue).
template <typename T> class ConcurrentQueue
{
    static_assert(std::is_trivially_copyable_v<T>, "ConcurrentQueue<T>: T must be trivially copyable");

public:
    explicit ConcurrentQueue(usize capacity_pow2, memory::IAllocator* alloc = memory::default_allocator())
        : m_alloc(alloc)
    {
        CRD_ASSERT_MSG(capacity_pow2 >= 2U, "ConcurrentQueue: capacity must be >= 2");
        CRD_ASSERT_MSG(memory::is_pow2(capacity_pow2), "ConcurrentQueue: capacity must be a power of two");
        CRD_ASSERT(m_alloc != nullptr);

        m_capacity = capacity_pow2;
        m_mask = capacity_pow2 - 1U;
        m_cells = static_cast<Cell*>(m_alloc->allocate(sizeof(Cell) * m_capacity, alignof(Cell)));

        // sequence[i] = i means "slot i is free for the very first producer lap".
        // After a consumer frees a slot it stores sequence = pos + capacity.
        for (usize i = 0; i < m_capacity; ++i)
        {
            ::new (static_cast<void*>(m_cells + i)) Cell{};
            m_cells[i].sequence.store(static_cast<u64>(i), std::memory_order_relaxed);
        }
        m_enqueue_pos.store(0, std::memory_order_relaxed);
        m_dequeue_pos.store(0, std::memory_order_relaxed);
    }

    ~ConcurrentQueue()
    {
        if (m_cells != nullptr)
        {
            // Cell is trivially destructible (atomic<u64> + trivially-copyable T),
            // but be explicit so the lifetime story holds if that ever changes.
            for (usize i = 0; i < m_capacity; ++i)
            {
                m_cells[i].~Cell();
            }
            if (m_alloc != nullptr)
            {
                m_alloc->deallocate(m_cells);
            }
            m_cells = nullptr;
        }
    }

    ConcurrentQueue(const ConcurrentQueue&) = delete;
    ConcurrentQueue& operator=(const ConcurrentQueue&) = delete;
    ConcurrentQueue(ConcurrentQueue&&) = delete;
    ConcurrentQueue& operator=(ConcurrentQueue&&) = delete;

    // ---- Producer side (any number of threads) -------------------------

    [[nodiscard]] bool try_push(const T& v) noexcept { return try_emplace(v); }
    [[nodiscard]] bool try_push(T&& v) noexcept { return try_emplace(std::move(v)); }

    template <typename... Args> [[nodiscard]] bool try_emplace(Args&&... args) noexcept
    {
        // Note: `cell.data = T(args...)` constructs a temporary then assigns. With the
        // trivially-copyable constraint that folds to a single copy. Lifting that
        // constraint later would mean a real placement-new (after explicitly destroying
        // the slot's previous T on the consumer side) — different shape, revisit then.
        u64 pos = m_enqueue_pos.load(std::memory_order_relaxed);
        for (;;)
        {
            Cell& cell = m_cells[pos & static_cast<u64>(m_mask)];
            const u64 seq = cell.sequence.load(std::memory_order_acquire);
            const i64 diff = static_cast<i64>(seq) - static_cast<i64>(pos);

            if (diff == 0)
            {
                // Slot free — try to claim it. compare_exchange_weak refreshes `pos` on failure.
                if (m_enqueue_pos.compare_exchange_weak(pos, pos + 1U, std::memory_order_relaxed,
                                                        std::memory_order_relaxed))
                {
                    cell.data = T(std::forward<Args>(args)...);
                    cell.sequence.store(pos + 1U, std::memory_order_release);
                    return true;
                }
            }
            else if (diff < 0)
            {
                return false; // full: this slot's consumer hasn't freed it yet
            }
            else
            {
                pos = m_enqueue_pos.load(std::memory_order_relaxed); // another producer raced ahead
            }
        }
    }

    // ---- Consumer side (any number of threads) -------------------------

    [[nodiscard]] bool try_pop(T& out) noexcept
    {
        u64 pos = m_dequeue_pos.load(std::memory_order_relaxed);
        for (;;)
        {
            Cell& cell = m_cells[pos & static_cast<u64>(m_mask)];
            const u64 seq = cell.sequence.load(std::memory_order_acquire);
            const i64 diff = static_cast<i64>(seq) - static_cast<i64>(pos + 1U);

            if (diff == 0)
            {
                if (m_dequeue_pos.compare_exchange_weak(pos, pos + 1U, std::memory_order_relaxed,
                                                        std::memory_order_relaxed))
                {
                    out = cell.data;
                    // Release this slot for the (pos + capacity)-th enqueue.
                    cell.sequence.store(pos + static_cast<u64>(m_mask) + 1U, std::memory_order_release);
                    return true;
                }
            }
            else if (diff < 0)
            {
                return false; // empty: producer hasn't filled this slot yet
            }
            else
            {
                pos = m_dequeue_pos.load(std::memory_order_relaxed); // another consumer raced ahead
            }
        }
    }

    // ---- Capacity / state (diagnostic — approximate under contention) --

    [[nodiscard]] usize capacity() const noexcept { return m_capacity; }

    /// Snapshot element count. Races with concurrent push/pop — diagnostics only.
    [[nodiscard]] usize size() const noexcept
    {
        const u64 ep = m_enqueue_pos.load(std::memory_order_acquire);
        const u64 dp = m_dequeue_pos.load(std::memory_order_acquire);
        return ep > dp ? static_cast<usize>(ep - dp) : 0U;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return m_enqueue_pos.load(std::memory_order_acquire) == m_dequeue_pos.load(std::memory_order_acquire);
    }

    [[nodiscard]] memory::IAllocator* allocator() const noexcept { return m_alloc; }

private:
    // One ring-buffer slot. `sequence` encodes the slot's lifecycle:
    //   sequence == slot_index            → free (ready for a producer)
    //   sequence == slot_index + 1         → holds data (ready for a consumer)
    //   sequence == slot_index + capacity  → freed by a consumer (next lap's producer)
    struct Cell
    {
        std::atomic<u64> sequence{0};
        T data{};
    };

    memory::IAllocator* m_alloc = nullptr;
    Cell* m_cells = nullptr;
    usize m_capacity = 0;
    usize m_mask = 0;

    // Producer and consumer position counters on separate cache lines.
    alignas(64) std::atomic<u64> m_enqueue_pos{0};
    alignas(64) std::atomic<u64> m_dequeue_pos{0};
};

} // namespace crd::containers

#if CRD_COMPILER_MSVC
#pragma warning(pop)
#endif
