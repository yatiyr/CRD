#pragma once

#include "fiber.hpp"
#include <crd/jobs/detail/fiber_context.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>

#include <atomic>
#include <memory>

// MSVC C4324: structure padded due to alignment specifier (alignas(64) on Counter).
#if CRD_COMPILER_MSVC
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

namespace crd::jobs::detail
{

static constexpr crd::u32 kCounterNullIndex = 0xFFFF'FFFFU;

// Resolution of a published Waiter, decided by exactly one CAS so the parking
// machinery (counter_finish_park, on the scheduler thread) and counter_decrement
// (any thread) never both act on it:
//   Pending  — freshly published; nobody has claimed it.
//   Canceled — counter_finish_park's ABA re-check found the value already at
//              target and won the CAS: the fiber gets resumed by the scheduler,
//              not by a decrement; no decrement owes it anything.
//   Wakeup   — counter_decrement won the CAS: it has removed this Waiter from
//              the list and owes the fiber exactly one resume.
enum class WaiterClaim : crd::u8 { Pending, Canceled, Wakeup };

// A Waiter node represents one fiber parked on a Counter.
// Stack-allocated by the caller of counter_wait() (it lives in that fiber's
// stack frame); remains valid until counter_wait() returns — a Canceled node
// may linger on the list a little after that, which is harmless (see
// CounterPool::release / acquire). At most one Waiter per fiber (a fiber can
// only wait on one Counter at a time).
//
// Lifecycle: counter_wait() fills `fiber`/`target` and stashes the Waiter as a
// "park request"; the fiber then switches to the scheduler (which saves its
// context); the scheduler — via counter_finish_park() — publishes the Waiter
// onto counter->waiters. So by the time any other thread can observe this node
// the fiber is fully parked.
//
// Thread-safety: `claim` and `next` are atomic because counter_decrement()
// (any thread) may access them concurrently with counter_finish_park(). `fiber`
// and `target` are written before the publish (Treiber push), so they are
// read-only by the time any other thread sees this node.
struct Waiter
{
    Fiber*                   fiber          = nullptr;              // the parked fiber
    crd::u32                 target         = 0U;                  // wake when counter->value == target
    std::atomic<WaiterClaim> claim          {WaiterClaim::Pending};// single-CAS arbiter (see WaiterClaim)
    std::atomic<Waiter*>     next           {nullptr};             // Treiber stack link; nullptr = end
    // Handshake: counter_finish_park sets this true once it has finished touching
    // the counter and this Waiter; counter_wait waits for it before returning, so
    // the fiber cannot race ahead, return from jobs::wait(), release the counter
    // and unwind this Waiter's stack frame while counter_finish_park is still
    // mid-flight (which would corrupt a re-acquired counter / dangle this node).
    std::atomic<bool>        park_finalized {false};
};

// Pool-allocated job counter. One 64-byte cache line.
// All live modifications go through atomic members.
struct alignas(64) Counter
{
    std::atomic<crd::u32>    value      {0U};
    crd::u32                 _pad0      = 0U;       // explicit pad → align waiters to 8
    std::atomic<Waiter*>     waiters    {nullptr};  // Treiber stack of pending waiters
    crd::u32                 pool_index  = kCounterNullIndex;
    crd::u32                 next_free   = kCounterNullIndex;
    crd::u8                  _pad1[40]  = {};
};
static_assert(sizeof(Counter)  == 64U, "Counter must be exactly one cache line");
static_assert(alignof(Counter) == 64U, "Counter must be cache-line aligned");

// Fixed-capacity pool of Counter objects; no heap allocation on the hot path.
//
// Thread-safety: acquire() and release() are fully lock-free using a
// generation-tagged 64-bit Treiber head [gen:32 | idx:32], matching the
// pattern used by FiberPool. The generation counter prevents ABA on the
// free list without CMPXCHG16B.
class CounterPool
{
public:
    CounterPool()  = default;
    ~CounterPool() { shutdown(); }

    CounterPool(const CounterPool&)            = delete;
    CounterPool& operator=(const CounterPool&) = delete;
    CounterPool(CounterPool&&)                 = delete;
    CounterPool& operator=(CounterPool&&)      = delete;

    // Allocate the counter array and wire the free list. Returns true always
    // (allocation failure aborts via CRD_ASSERT). capacity must be >= 1.
    [[nodiscard]] bool init(crd::u32 capacity) noexcept;

    // Return all storage. Debug-asserts every counter has been released.
    void shutdown() noexcept;

    // Pop a counter from the free list; initialise its value to initial_value.
    // Debug-asserts (with Ignore semantics) if the pool is exhausted.
    [[nodiscard]] Counter* acquire(crd::u32 initial_value) noexcept;

    // Push a counter back. Debug-asserts no pending waiters remain on it.
    void release(Counter* counter) noexcept;

    [[nodiscard]] bool     is_initialized() const noexcept { return m_initialized; }
    [[nodiscard]] crd::u32 capacity()       const noexcept { return m_capacity; }
    [[nodiscard]] crd::u32 available()      const noexcept;

private:
    // Tagged-head helpers — identical to FiberPool.
    static constexpr crd::u64 pack_head(crd::u32 idx, crd::u32 gen) noexcept
    {
        return (crd::u64(gen) << 32U) | crd::u64(idx);
    }
    static constexpr crd::u32 head_idx(crd::u64 h) noexcept { return crd::u32(h); }
    static constexpr crd::u32 head_gen(crd::u64 h) noexcept { return crd::u32(h >> 32U); }

    std::unique_ptr<Counter[]>  m_counters;
    std::atomic<crd::u64>       m_free_head  {pack_head(kCounterNullIndex, 0U)};
    std::atomic<crd::u32>       m_acquired   {0U};
    crd::u32                    m_capacity   = 0U;
    bool                        m_initialized = false;
};

// ---------------------------------------------------------------------------
// Counter free functions
// ---------------------------------------------------------------------------

// Atomically subtract `amount` from counter->value.
//
// All waiters wait for the counter to reach 0 (see counter_wait), so the waiters
// list is touched at exactly one point — the decrement that hits 0 — and only
// ever drained, never partially rebuilt. When `new_value > 0` this returns
// nullptr without touching counter->waiters (waiters stay parked). When
// `new_value == 0` it steals the whole list and, for each Waiter: if already
// Canceled → discard; else CAS-claim Pending→Wakeup → on success prepend to the
// returned list (this call owes that fiber a resume), on failure (counter_finish_
// park canceled it first) → discard.
//
// The returned list is null-terminated via Waiter::next. The caller re-queues
// each fiber (Waiter::fiber) as a High-priority resume job. Thread-safe,
// lock-free, safe with many concurrent decrementers.
[[nodiscard]] Waiter* counter_decrement(Counter* counter, crd::u32 amount = 1U) noexcept;

// Stash a "park request" so the scheduler will publish the Waiter on the calling
// fiber's behalf once it has switched out (see worker_pool.hpp comment + counter_
// finish_park). Defined in worker_pool.cpp (where the thread-local lives). Called
// only by counter_wait, immediately before its fiber_switch.
void tl_set_pending_park(Counter* counter, Waiter* waiter) noexcept;

// Park the calling fiber until counter->value reaches 0.
//
// `target` must be 0 (the only value the job system waits for; counter_decrement
// drains the waiters list exactly at zero). The parameter is kept for clarity at
// call sites and is asserted.
//
// Runs ON the fiber. If the value is already 0 this returns immediately (fast
// path). Otherwise it fills `w` (fiber/target/claim=Pending), stashes a "park
// request" so the scheduler will publish the Waiter on this fiber's behalf (see
// tl_set_pending_park / counter_finish_park), and switches to the scheduler —
// which is what saves this fiber's context. The fiber therefore only becomes
// wakeable AFTER its context is saved. Returns when the fiber is resumed (by
// counter_decrement having claimed Wakeup, or by the scheduler if the value had
// already reached 0 by the time the Waiter was published).
//
// `w` must remain valid until counter_wait() returns (stack-allocate at call site).
// `scheduler_ctx`: the execution context to switch to.
//
// FiberState transitions (debug builds only):
//   Active → Waiting  (before the switch; published-then-resumed → Ready by the
//                      scheduler or counter_decrement)
//   Ready  → Active   (after the switch returns)
void counter_wait(Counter* counter, Waiter* w, Fiber* current_fiber,
                  FiberContext& scheduler_ctx, crd::u32 target = 0U) noexcept;

// Called by the scheduler (run_job_in_fiber) immediately after a fiber suspended
// inside counter_wait — i.e. once the fiber's context is saved. Publishes `w`
// onto counter->waiters, then ABA-re-checks counter->value:
//   - if it is still != target → leave `w` parked; some later counter_decrement
//     will wake the fiber. Returns false.
//   - if it has reached target → CAS w->claim Pending→Canceled. On success the
//     wait is already satisfied and no decrement owes the fiber a resume →
//     returns true (caller must mark the fiber Ready and enqueue it as a resume).
//     On failure a counter_decrement already claimed Wakeup and will resume the
//     fiber → returns false.
//
// `w->fiber` must already be set (counter_wait did that). Thread-safe, lock-free.
[[nodiscard]] bool counter_finish_park(Counter* counter, Waiter* w) noexcept;

} // namespace crd::jobs::detail

#if CRD_COMPILER_MSVC
#pragma warning(pop)
#endif
