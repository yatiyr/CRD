#include "counter.hpp"
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>

namespace crd::jobs::detail
{

// ---------------------------------------------------------------------------
// CounterPool — lifecycle
// ---------------------------------------------------------------------------

bool CounterPool::init(crd::u32 capacity) noexcept
{
    CRD_ASSERT_MSG(!m_initialized, "CounterPool::init called twice");
    CRD_ASSERT_MSG(capacity >= 1U, "CounterPool: capacity must be >= 1");

    m_capacity = capacity;
    m_counters = std::make_unique<Counter[]>(capacity);
    m_acquired.store(0U, std::memory_order_relaxed);

    // Wire the singly-linked free list: 0 → 1 → … → (capacity-1) → nil.
    for (crd::u32 i = 0U; i < capacity; ++i)
    {
        m_counters[i].pool_index = i;
        m_counters[i].next_free  = (i + 1U < capacity) ? (i + 1U) : kCounterNullIndex;
    }

    m_free_head.store(pack_head(0U, 0U), std::memory_order_release);
    m_initialized = true;
    return true;
}

void CounterPool::shutdown() noexcept
{
    if (!m_initialized)
        return;
    CRD_ASSERT_MSG(m_acquired.load(std::memory_order_relaxed) == 0U,
                   "CounterPool::shutdown: counters still acquired — call release() first");
    m_counters.reset();
    m_capacity = 0U;
    m_free_head.store(pack_head(kCounterNullIndex, 0U), std::memory_order_relaxed);
    m_initialized = false;
}

// ---------------------------------------------------------------------------
// CounterPool — acquire (Treiber pop)
//
// Memory ordering: mirrors FiberPool::acquire_from — acquire on load, acq_rel
// on successful CAS so the next_free link read between them is safe.
// ---------------------------------------------------------------------------

Counter* CounterPool::acquire(crd::u32 initial_value) noexcept
{
    CRD_ASSERT_MSG(m_initialized, "CounterPool::acquire called before init");

    crd::u64 head = m_free_head.load(std::memory_order_acquire);
    while (true)
    {
        const crd::u32 idx = head_idx(head);
        if (idx == kCounterNullIndex)
        {
            CRD_ASSERT_MSG(false, "CounterPool exhausted — raise max_counters in jobs::Config");
            return nullptr;
        }

        const crd::u32 next    = m_counters[idx].next_free; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        const crd::u64 desired = pack_head(next, head_gen(head) + 1U);

        if (m_free_head.compare_exchange_weak(head, desired,
                std::memory_order_acq_rel, std::memory_order_acquire))
        {
            Counter* c    = &m_counters[idx]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            c->value.store(initial_value,  std::memory_order_relaxed);
            c->waiters.store(nullptr,       std::memory_order_relaxed);
            c->next_free = kCounterNullIndex;
            m_acquired.fetch_add(1U, std::memory_order_relaxed);
            return c;
        }
    }
}

// ---------------------------------------------------------------------------
// CounterPool — release (Treiber push)
//
// Memory ordering: release on successful CAS so next_free is visible to
// subsequent acquire() calls that read it with acquire.
// ---------------------------------------------------------------------------

void CounterPool::release(Counter* counter) noexcept
{
    CRD_ASSERT_MSG(m_initialized, "CounterPool::release called before init");
    CRD_ASSERT_MSG(counter != nullptr, "CounterPool::release: null pointer");
    CRD_ASSERT_MSG(counter->waiters.load(std::memory_order_relaxed) == nullptr,
                   "CounterPool::release: counter still has pending waiters");

    const crd::u32 idx = counter->pool_index;
    crd::u64 head      = m_free_head.load(std::memory_order_relaxed);
    crd::u64 desired;
    do
    {
        counter->next_free = head_idx(head);
        desired            = pack_head(idx, head_gen(head));
    } while (!m_free_head.compare_exchange_weak(head, desired,
                  std::memory_order_release, std::memory_order_relaxed));

    m_acquired.fetch_sub(1U, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// CounterPool — available
// ---------------------------------------------------------------------------

crd::u32 CounterPool::available() const noexcept
{
    const crd::u32 acq = m_acquired.load(std::memory_order_relaxed);
    return (acq <= m_capacity) ? (m_capacity - acq) : 0U;
}

// ---------------------------------------------------------------------------
// counter_decrement
//
// Memory ordering:
//   fetch_sub(acq_rel): the acquire half synchronises with the release in the
//   Treiber push inside counter_wait(), making every Waiter field written
//   before that push visible here. The release half makes the new value visible
//   to any concurrent counter_wait() that loads it with acquire.
//
//   exchange(nullptr, acq_rel) on waiters: atomically steals the entire waiter
//   list. Any concurrent counter_wait() that loses its CAS will retry and see
//   the empty list, then its double-check (step 4) will observe the new value
//   and set canceled=true — so the fiber never suspends.
//
//   Re-push of unsatisfied waiters uses release CAS so the next_free chain
//   is visible to a subsequent acquire on the same counter.
// ---------------------------------------------------------------------------

Waiter* counter_decrement(Counter* counter, crd::u32 amount) noexcept
{
    CRD_ASSERT_MSG(counter != nullptr, "counter_decrement: null counter");
    CRD_ASSERT_MSG(amount  >= 1U,      "counter_decrement: amount must be >= 1");

    const crd::u32 old_val = counter->value.fetch_sub(amount, std::memory_order_acq_rel);
    const crd::u32 new_val = old_val - amount;

    // Atomically steal the entire waiter list.
    Waiter* list = counter->waiters.exchange(nullptr, std::memory_order_acq_rel);

    Waiter* woken    = nullptr;  // fibers whose target matches new_val
    Waiter* put_back = nullptr;  // fibers whose target does not yet match

    while (list != nullptr)
    {
        Waiter* nxt = list->next.load(std::memory_order_relaxed);

        if (list->canceled.load(std::memory_order_acquire))
        {
            // ABA path: the waiter already returned from counter_wait without
            // suspending. Discard — there is no fiber to resume.
            list = nxt;
            continue;
        }

        if (list->target == new_val)
        {
            // Satisfied: transition fiber to Ready (debug builds only).
#if CRD_ENABLE_ASSERTS
            CRD_ASSERT_MSG(list->fiber != nullptr,
                           "counter_decrement: woken Waiter has null fiber");
            CRD_ASSERT_MSG(list->fiber->state == FiberState::Waiting,
                           "counter_decrement: woken fiber was not Waiting");
            list->fiber->state = FiberState::Ready;
#endif
            list->next.store(woken, std::memory_order_relaxed);
            woken = list;
        }
        else
        {
            // Not yet satisfied — push back for a future decrement.
            list->next.store(put_back, std::memory_order_relaxed);
            put_back = list;
        }
        list = nxt;
    }

    // Re-push unsatisfied waiters back onto counter->waiters (Treiber push).
    while (put_back != nullptr)
    {
        Waiter* nxt  = put_back->next.load(std::memory_order_relaxed);
        Waiter* head = counter->waiters.load(std::memory_order_relaxed);
        do
        {
            put_back->next.store(head, std::memory_order_relaxed);
        } while (!counter->waiters.compare_exchange_weak(
                     head, put_back,
                     std::memory_order_release, std::memory_order_relaxed));
        put_back = nxt;
    }

    return woken;
}

// ---------------------------------------------------------------------------
// counter_wait
//
// ABA-safe protocol — see header comment for the full step-by-step.
//
// FiberState note (debug builds):
//   The Waiting → Ready transition for the suspended fiber is performed by
//   counter_decrement() on whichever thread runs it. FiberState is not an
//   atomic type; the transition is safe because:
//     - The fiber_switch call (step 5) provides a full memory fence on x86-64
//       via the RSP store in the asm stub.
//     - counter_decrement's fetch_sub(acq_rel) synchronises with the push CAS
//       (release) that published this Waiter, ordering the state write before
//       the state read.
//   This is a debug-only best-effort check, not a hard guarantee.
// ---------------------------------------------------------------------------

void counter_wait(Counter* counter, Waiter* w, Fiber* current_fiber,
                  FiberContext& scheduler_ctx, crd::u32 target) noexcept
{
    CRD_ASSERT_MSG(counter       != nullptr, "counter_wait: null counter");
    CRD_ASSERT_MSG(w             != nullptr, "counter_wait: null Waiter");
    CRD_ASSERT_MSG(current_fiber != nullptr, "counter_wait: null current_fiber");

    // Fast path: value is already at target.
    if (counter->value.load(std::memory_order_acquire) == target)
        return;

    // Set up the Waiter node.
    w->fiber  = current_fiber;
    w->target = target;
    w->canceled.store(false, std::memory_order_relaxed);

    // Push w onto counter->waiters (Treiber push — release so all writes to
    // w->fiber / w->target / w->canceled are visible to counter_decrement).
    Waiter* head = counter->waiters.load(std::memory_order_relaxed);
    do
    {
        w->next.store(head, std::memory_order_relaxed);
    } while (!counter->waiters.compare_exchange_weak(
                 head, w,
                 std::memory_order_release, std::memory_order_relaxed));

    // ABA double-check: did the counter reach target while we were pushing?
    if (counter->value.load(std::memory_order_acquire) == target)
    {
        // Signal to counter_decrement that this waiter must not cause a resume.
        // counter_decrement may or may not have already drained the list; either
        // way it will see canceled == true and discard this node.
        w->canceled.store(true, std::memory_order_release);
        return;
    }

    // Suspend — switch to the scheduler fiber.
#if CRD_ENABLE_ASSERTS
    CRD_ASSERT_MSG(current_fiber->state == FiberState::Active,
                   "counter_wait: fiber must be Active before suspending");
    current_fiber->state = FiberState::Waiting;
#endif

    fiber_switch(&current_fiber->context, &scheduler_ctx);

    // --- Resumed by counter_decrement --- //

#if CRD_ENABLE_ASSERTS
    CRD_ASSERT_MSG(current_fiber->state == FiberState::Ready,
                   "counter_wait: fiber should be in Ready state on resume");
    current_fiber->state = FiberState::Active;
#endif
}

} // namespace crd::jobs::detail
