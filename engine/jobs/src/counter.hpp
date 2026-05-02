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

static constexpr crd::u32 kCounterNullIndex = 0xFFFF'FFFFu;

// A Waiter node represents one fiber suspended on a Counter.
// Stack-allocated by the caller of counter_wait(); remains valid until
// counter_wait() returns. At most one Waiter per fiber (a fiber can only
// wait on one Counter at a time).
//
// Thread-safety: `canceled` and `next` are atomic because counter_decrement()
// (any thread) may access them concurrently with counter_wait() (owning fiber).
// `fiber` and `target` are written once by the owning fiber before the first
// publish (Treiber push), so they are read-only by the time any other thread
// sees this node.
struct Waiter
{
    Fiber*               fiber    = nullptr;  // the suspended fiber
    crd::u32             target   = 0u;       // wake when counter->value == target
    std::atomic<bool>    canceled {false};    // set true if ABA double-check fires
    std::atomic<Waiter*> next     {nullptr};  // Treiber stack link; nullptr = end
};

// Pool-allocated job counter. One 64-byte cache line.
// All live modifications go through atomic members.
struct alignas(64) Counter
{
    std::atomic<crd::u32>    value      {0u};
    crd::u32                 _pad0      = 0u;       // explicit pad → align waiters to 8
    std::atomic<Waiter*>     waiters    {nullptr};  // Treiber stack of pending waiters
    crd::u32                 pool_index  = kCounterNullIndex;
    crd::u32                 next_free   = kCounterNullIndex;
    crd::u8                  _pad1[40]  = {};
};
static_assert(sizeof(Counter)  == 64u, "Counter must be exactly one cache line");
static_assert(alignof(Counter) == 64u, "Counter must be cache-line aligned");

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
        return (crd::u64(gen) << 32u) | crd::u64(idx);
    }
    static constexpr crd::u32 head_idx(crd::u64 h) noexcept { return crd::u32(h); }
    static constexpr crd::u32 head_gen(crd::u64 h) noexcept { return crd::u32(h >> 32u); }

    std::unique_ptr<Counter[]>  m_counters;
    std::atomic<crd::u64>       m_free_head  {pack_head(kCounterNullIndex, 0u)};
    std::atomic<crd::u32>       m_acquired   {0u};
    crd::u32                    m_capacity   = 0u;
    bool                        m_initialized = false;
};

// ---------------------------------------------------------------------------
// Counter free functions
// ---------------------------------------------------------------------------

// Atomically subtract `amount` from counter->value.
// After the subtraction, atomically drains counter->waiters (exchange with
// nullptr) and partitions the list:
//   - Waiters with .canceled == true          → silently discarded.
//   - Waiters with .target == new_value       → prepended to the returned list.
//   - Waiters with .target != new_value       → pushed back onto counter->waiters.
//
// The returned list is null-terminated via Waiter::next. The caller is
// responsible for re-queuing each fiber (Waiter::fiber) as a High-priority job.
// Thread-safe, lock-free.
[[nodiscard]] Waiter* counter_decrement(Counter* counter, crd::u32 amount = 1u) noexcept;

// ABA-safe wait: block until counter->value == target.
//
// Protocol:
//   1. Load value with acquire. If == target → return immediately (fast path).
//   2. Initialise w: fiber = current_fiber, target = target, canceled = false.
//   3. Push w onto counter->waiters via Treiber CAS (release).
//   4. Load value with acquire. If == target:
//        w->canceled.store(true, release) → return (ABA path — no suspension).
//   5. Suspend: fiber_switch(&current_fiber->context, &scheduler_ctx).
//   6. Resumes here when counter_decrement returns this fiber as woken.
//
// `w` must remain valid until counter_wait() returns (stack-allocate at call site).
// `scheduler_ctx`: the execution context to switch to when suspending.
//
// FiberState transitions (debug builds only):
//   Active → Waiting  (before fiber_switch)
//   Ready  → Active   (after fiber_switch returns)
void counter_wait(Counter* counter, Waiter* w, Fiber* current_fiber,
                  FiberContext& scheduler_ctx, crd::u32 target = 0u) noexcept;

} // namespace crd::jobs::detail

#if CRD_COMPILER_MSVC
#pragma warning(pop)
#endif
