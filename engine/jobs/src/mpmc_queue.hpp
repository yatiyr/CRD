#pragma once

#include <crd/core/assert.hpp>
#include <crd/core/platform.hpp>
#include <crd/core/types.hpp>

#include <atomic>
#include <memory>
#include <type_traits>

// MSVC C4324: structure padded due to alignment specifier. Expected — m_enqueue_pos and
// m_dequeue_pos are intentionally placed on separate cache lines to prevent false sharing
// between producers and consumers.
#if CRD_COMPILER_MSVC
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

namespace crd::jobs::detail
{

// Vyukov bounded MPMC queue (Dmitry Vyukov, 1024cores.net).
//
// Thread-safety: enqueue() and dequeue() are fully lock-free and safe to call
// concurrently from any combination of producers and consumers.
//
// Capacity must be a power of two (>= 2). Fixed at construction time; zero dynamic
// allocation after construction (all storage is in the cells array allocated at init).
//
// enqueue() returns false (does not block) when the queue is full.
// dequeue() returns false (does not block) when the queue is empty.
//
// T must be trivially copyable. The intended maximum element type is the 64-byte JobDecl.
template<typename T>
class MpmcQueue
{
    static_assert(std::is_trivially_copyable_v<T>,
                  "MpmcQueue<T>: T must be trivially copyable");

public:
    explicit MpmcQueue(crd::u32 capacity = 256u);
    ~MpmcQueue() = default;

    MpmcQueue(const MpmcQueue&)            = delete;
    MpmcQueue& operator=(const MpmcQueue&) = delete;
    MpmcQueue(MpmcQueue&&)                 = delete;
    MpmcQueue& operator=(MpmcQueue&&)      = delete;

    // Enqueue one item. Returns false without blocking if the queue is full.
    [[nodiscard]] bool enqueue(T item) noexcept;

    // Dequeue one item into `out`. Returns false without blocking if the queue is empty.
    [[nodiscard]] bool dequeue(T& out) noexcept;

    // Approximate: loads enqueue and dequeue positions with relaxed ordering.
    // May return a transiently stale result under concurrent access.
    [[nodiscard]] bool     empty()    const noexcept;
    [[nodiscard]] crd::u32 capacity() const noexcept { return m_mask + 1u; }

private:
    // One slot in the ring buffer.
    //
    // sequence encodes the slot's lifecycle:
    //   sequence == slot_index          → slot is free (ready for a producer)
    //   sequence == slot_index + 1      → slot holds data (ready for a consumer)
    //   sequence == slot_index + capacity → slot has been freed by a consumer
    //                                       (ready for the next lap's producer)
    struct Cell
    {
        std::atomic<crd::u64> sequence{0};
        T                     data{};
    };

    // Producer and consumer positions on separate cache lines to prevent the
    // cross-core invalidation that would occur if both sides shared a line.
    alignas(64) std::atomic<crd::u64> m_enqueue_pos{0};
    alignas(64) std::atomic<crd::u64> m_dequeue_pos{0};

    crd::u32                m_mask{0u};
    std::unique_ptr<Cell[]> m_cells;
};

// ---------------------------------------------------------------------------
// Constructor
//
// sequence[i] = i means "slot i is free for the very first producer lap".
// After a consumer frees a slot it stores sequence = pos + capacity, which
// equals the index of the first enqueue that will next claim that slot.
// ---------------------------------------------------------------------------

template<typename T>
MpmcQueue<T>::MpmcQueue(crd::u32 capacity)
    : m_mask{capacity - 1u}
    , m_cells{std::make_unique<Cell[]>(capacity)}
{
    CRD_ASSERT_MSG(capacity >= 2u,
                   "MpmcQueue: capacity must be >= 2");
    CRD_ASSERT_MSG((capacity & (capacity - 1u)) == 0u,
                   "MpmcQueue: capacity must be a power of two");

    for (crd::u32 i = 0u; i < capacity; ++i)
    {
        m_cells[i].sequence.store(static_cast<crd::u64>(i), std::memory_order_relaxed);
    }
}

// ---------------------------------------------------------------------------
// enqueue (any thread — MPMC producer side)
//
// A producer claims slot `pos` when sequence == pos (slot is free for writing).
// After writing data it publishes by storing sequence = pos + 1, signalling
// consumers that the slot is ready to read.
//
// Memory ordering (Vyukov):
//   sequence.load  acquire : observe the consumer's release-store that freed the slot.
//   enqueue_pos CAS relaxed: position counter; synchronization carried by sequence.
//   sequence.store release : makes cell->data visible to the matching dequeue acquire.
// ---------------------------------------------------------------------------

template<typename T>
bool MpmcQueue<T>::enqueue(T item) noexcept
{
    crd::u64 pos = m_enqueue_pos.load(std::memory_order_relaxed);

    for (;;)
    {
        Cell&    cell = m_cells[pos & static_cast<crd::u64>(m_mask)];
        crd::u64 seq  = cell.sequence.load(std::memory_order_acquire);
        crd::i64 diff = static_cast<crd::i64>(seq) - static_cast<crd::i64>(pos);

        if (diff == 0)
        {
            // Slot free — try to claim it. compare_exchange_weak updates pos on failure.
            if (m_enqueue_pos.compare_exchange_weak(pos, pos + 1u,
                    std::memory_order_relaxed, std::memory_order_relaxed))
            {
                cell.data = item;
                cell.sequence.store(pos + 1u, std::memory_order_release);
                return true;
            }
            // CAS failed; pos is refreshed by compare_exchange_weak. Retry.
        }
        else if (diff < 0)
        {
            return false; // queue full: slot not freed by its consumer yet
        }
        else
        {
            // Another producer already claimed this pos; reload and retry.
            pos = m_enqueue_pos.load(std::memory_order_relaxed);
        }
    }
}

// ---------------------------------------------------------------------------
// dequeue (any thread — MPMC consumer side)
//
// A consumer claims slot `pos` when sequence == pos + 1 (data is ready to read).
// After reading data it frees the slot by storing sequence = pos + capacity,
// signalling producers that the slot is available for the next lap.
//
// Memory ordering (Vyukov):
//   sequence.load  acquire : observe the producer's release-store that filled the slot.
//   dequeue_pos CAS relaxed: same rationale as enqueue.
//   sequence.store release : makes the freed slot visible to future producers.
// ---------------------------------------------------------------------------

template<typename T>
bool MpmcQueue<T>::dequeue(T& out) noexcept
{
    crd::u64 pos = m_dequeue_pos.load(std::memory_order_relaxed);

    for (;;)
    {
        Cell&    cell = m_cells[pos & static_cast<crd::u64>(m_mask)];
        crd::u64 seq  = cell.sequence.load(std::memory_order_acquire);
        crd::i64 diff = static_cast<crd::i64>(seq) - static_cast<crd::i64>(pos + 1u);

        if (diff == 0)
        {
            // Data ready — try to claim the slot.
            if (m_dequeue_pos.compare_exchange_weak(pos, pos + 1u,
                    std::memory_order_relaxed, std::memory_order_relaxed))
            {
                out = cell.data;
                // Release this slot for the (pos + capacity)-th enqueue.
                cell.sequence.store(pos + static_cast<crd::u64>(m_mask) + 1u,
                                    std::memory_order_release);
                return true;
            }
            // CAS failed; pos is refreshed by compare_exchange_weak. Retry.
        }
        else if (diff < 0)
        {
            return false; // queue empty: producer has not filled this slot yet
        }
        else
        {
            // Another consumer already claimed this pos; reload and retry.
            pos = m_dequeue_pos.load(std::memory_order_relaxed);
        }
    }
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

template<typename T>
bool MpmcQueue<T>::empty() const noexcept
{
    // Relaxed loads; result is an approximation. Callers must not rely on
    // this being instantaneously accurate under concurrent access.
    const crd::u64 ep = m_enqueue_pos.load(std::memory_order_relaxed);
    const crd::u64 dp = m_dequeue_pos.load(std::memory_order_relaxed);
    return ep == dp;
}

} // namespace crd::jobs::detail

#if CRD_COMPILER_MSVC
#pragma warning(pop)
#endif
