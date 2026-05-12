#pragma once

#include <crd/core/assert.hpp>
#include <crd/core/platform.hpp>
#include <crd/core/types.hpp>

#include <atomic>
#include <memory>
#include <optional>
#include <type_traits>

// MSVC C4324: structure padded due to alignment specifier. Expected — we use
// alignas(64) on m_bottom and m_top deliberately to prevent false sharing.
#if CRD_COMPILER_MSVC
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

namespace crd::jobs::detail
{

// Chase-Lev work-stealing deque (Lê et al. 2013,
// "Correct and Efficient Work-Stealing for Weak Memory Models").
//
// Thread-safety contract:
//   push() and pop() MUST be called only by the owning thread.
//   steal() is safe to call concurrently from any number of other threads.
//
// Capacity is fixed at construction time and must be a power of two (>= 2).
// push() on a full deque asserts in debug and returns false in release — the
// scheduler is responsible for sizing deques so they never fill.
//
// T must be trivially copyable. The intended maximum element type is the
// 64-byte JobDecl.
template<typename T>
class WorkStealingDeque
{
    static_assert(std::is_trivially_copyable_v<T>,
                  "WorkStealingDeque<T>: T must be trivially copyable");

public:
    explicit WorkStealingDeque(crd::u32 capacity = 256U);
    ~WorkStealingDeque() = default;

    WorkStealingDeque(const WorkStealingDeque&)            = delete;
    WorkStealingDeque& operator=(const WorkStealingDeque&) = delete;
    WorkStealingDeque(WorkStealingDeque&&)                 = delete;
    WorkStealingDeque& operator=(WorkStealingDeque&&)      = delete;

    // Push item (owner only). Returns false and asserts in debug if full.
    // By const-ref: T can be over-aligned (JobDecl is alignas(64)), and passing
    // such a type by value triggers GCC's "ABI for 64-byte-aligned params has
    // changed" diagnostic — and copies 64 B onto the call stack for nothing.
    bool push(const T& item) noexcept;

    // Pop item LIFO (owner only). Returns nullopt if empty.
    [[nodiscard]] std::optional<T> pop() noexcept;

    // Steal item FIFO (any thread). Returns nullopt if empty or CAS lost.
    [[nodiscard]] std::optional<T> steal() noexcept;

    // Approximate element count. May be transiently <= 0 under concurrent access.
    [[nodiscard]] crd::i64 size() const noexcept;
    [[nodiscard]] bool     empty()    const noexcept;
    [[nodiscard]] crd::u32 capacity() const noexcept { return m_mask + 1U; }

private:
    // bottom (owner) and top (thieves) on separate cache lines.
    // The owner writes bottom frequently; thieves write top via CAS.
    // Sharing a cache line would cause cross-core invalidation on every push/steal.
    alignas(64) std::atomic<crd::i64> m_bottom{0};
    alignas(64) std::atomic<crd::i64> m_top{0};

    crd::u32             m_mask{0U};   // capacity - 1; capacity is always a power of two
    std::unique_ptr<T[]> m_buf;        // circular buffer; m_buf[i & m_mask]

    [[nodiscard]] std::size_t buf_index(crd::i64 i) const noexcept
    {
        // Cast through u64 before masking to avoid signed-overflow UB and
        // MSVC signed/unsigned-mismatch warnings. Negative i values produce
        // valid (wrapped) indices, but callers guard against accessing them.
        return static_cast<std::size_t>(static_cast<crd::u64>(i) & static_cast<crd::u64>(m_mask));
    }
};

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

template<typename T>
WorkStealingDeque<T>::WorkStealingDeque(crd::u32 capacity)
    : m_mask{capacity - 1U}
    , m_buf{std::make_unique<T[]>(capacity)}
{
    CRD_ASSERT_MSG(capacity >= 2U,
                   "WorkStealingDeque: capacity must be >= 2");
    CRD_ASSERT_MSG((capacity & (capacity - 1U)) == 0U,
                   "WorkStealingDeque: capacity must be a power of two");
}

// ---------------------------------------------------------------------------
// push (owner only)
//
// Memory ordering (Lê et al. 2013, §4):
//   - m_top loaded acquire : see the most recent steal increments for the
//     size check so we never push into an index that a thief might still hold.
//   - m_buf write is a plain store; it happens-before the bottom store.
//   - m_bottom stored release: makes the buf write visible to any subsequent
//     acquire load of m_bottom in steal().
// ---------------------------------------------------------------------------

template<typename T>
bool WorkStealingDeque<T>::push(const T& item) noexcept
{
    const crd::i64 b = m_bottom.load(std::memory_order_relaxed);
    const crd::i64 t = m_top.load(std::memory_order_acquire);

    if ((b - t) >= static_cast<crd::i64>(capacity())) [[unlikely]]
    {
        CRD_ASSERT_MSG(false, "WorkStealingDeque::push: deque is full");
        return false;
    }

    m_buf[buf_index(b)] = item;
    m_bottom.store(b + 1, std::memory_order_release);
    return true;
}

// ---------------------------------------------------------------------------
// pop (owner only, LIFO)
//
// Memory ordering (Lê et al. 2013, §4):
//   - m_bottom stored seq_cst before loading m_top: establishes a total order
//     with steal's seq_cst fence, so the thief sees the retracted bottom before
//     the owner decides whether a race exists.
//   - m_top loaded seq_cst: pairs with steal's seq_cst CAS success.
//   - CAS on m_top seq_cst/relaxed: picks a single winner for the last element.
// ---------------------------------------------------------------------------

template<typename T>
std::optional<T> WorkStealingDeque<T>::pop() noexcept
{
    const crd::i64 b = m_bottom.load(std::memory_order_relaxed) - 1;
    m_bottom.store(b, std::memory_order_seq_cst);

    crd::i64 t = m_top.load(std::memory_order_seq_cst);

    if (t <= b)
    {
        T item = m_buf[buf_index(b)];
        if (t < b)
            return item; // > 1 element — no race possible

        // Exactly 1 element: race with concurrent steal() for the last slot.
        if (!m_top.compare_exchange_strong(t, t + 1,
                std::memory_order_seq_cst,
                std::memory_order_relaxed))
        {
            m_bottom.store(b + 1, std::memory_order_relaxed);
            return std::nullopt; // thief won
        }
        m_bottom.store(b + 1, std::memory_order_relaxed);
        return item;
    }
    else
    {
        m_bottom.store(b + 1, std::memory_order_relaxed);
        return std::nullopt; // deque was empty
    }
}

// ---------------------------------------------------------------------------
// steal (any thread, FIFO)
//
// Memory ordering (Lê et al. 2013, §4):
//   - m_top loaded acquire  : see prior steal successes.
//   - fence(seq_cst)        : orders the top load and the bottom load on weak
//     memory models (ARM, Power). On x86 (TSO) this degrades to a compiler
//     fence. Required for proof of Theorem 1 in the paper.
//   - m_bottom loaded acquire: sync-with the owner's release store in push(),
//     ensuring m_buf[t] is visible once bottom has advanced past t.
//   - CAS on m_top seq_cst/relaxed: compete with other thieves and with pop().
// ---------------------------------------------------------------------------

template<typename T>
std::optional<T> WorkStealingDeque<T>::steal() noexcept
{
    crd::i64 t = m_top.load(std::memory_order_acquire);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    const crd::i64 b = m_bottom.load(std::memory_order_acquire);

    if (t < b)
    {
        T item = m_buf[buf_index(t)];
        if (!m_top.compare_exchange_strong(t, t + 1,
                std::memory_order_seq_cst,
                std::memory_order_relaxed))
        {
            return std::nullopt; // another thief or owner won
        }
        return item;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

template<typename T>
crd::i64 WorkStealingDeque<T>::size() const noexcept
{
    // Relaxed: size() is an approximation for scheduling hints, not a
    // correctness primitive. Callers must not rely on exact values.
    const crd::i64 b = m_bottom.load(std::memory_order_relaxed);
    const crd::i64 t = m_top.load(std::memory_order_relaxed);
    return b - t;
}

template<typename T>
bool WorkStealingDeque<T>::empty() const noexcept
{
    return size() <= 0;
}

} // namespace crd::jobs::detail

#if CRD_COMPILER_MSVC
#pragma warning(pop)
#endif
