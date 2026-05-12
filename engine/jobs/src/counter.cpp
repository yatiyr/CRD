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

    // The waiters list may legitimately contain Canceled nodes: counter_finish_
    // park's ABA re-check found the value already at target and claimed Canceled,
    // and counter_decrement hasn't drained the list yet. Harmless — acquire()
    // resets waiters=nullptr so the next user sees a clean list. A Pending node
    // here, though, means a fiber is parked awaiting a decrement that won't come
    // (a leak/deadlock bug); a Wakeup node is never on the list (counter_decrement
    // removes it the instant it claims it).
#if CRD_ENABLE_ASSERTS
    {
        const Waiter* w = counter->waiters.load(std::memory_order_acquire);
        while (w != nullptr)
        {
            CRD_ASSERT_MSG(w->claim.load(std::memory_order_acquire) == WaiterClaim::Canceled,
                           "CounterPool::release: counter has an unresolved waiter (fiber still parked)");
            w = w->next.load(std::memory_order_relaxed);
        }
    }
#endif

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
// The job-counter wait protocol only ever waits for the counter to reach 0 (see
// counter_wait), and a counter only ever decreases. So the waiters list is
// touched at exactly one point — the decrement that brings the value to 0 — and
// only ever drained, never partially rebuilt. That is what makes this safe with
// many concurrent decrementers: there is no "steal the list, put some back"
// step that a later decrement could race against (an earlier design had that and
// it could lose a wakeup: A steals + is about to re-push a not-yet-satisfied
// waiter while C — the decrement to 0 — runs its exchange in the gap, sees an
// empty list, and then A re-pushes onto a list nobody will ever drain again).
//
// Memory ordering:
//   fetch_sub(acq_rel): the acquire half synchronises with the release in
//   counter_finish_park()'s publish CAS, making every Waiter field (and the
//   parked fiber's Waiting state, written before that publish) visible here. The
//   release half makes the new value visible to a concurrent counter_finish_park
//   that loads it with acquire.
//   exchange(nullptr, acq_rel) on waiters: steals the list to drain it.
//   Per-waiter claim CAS (Pending→Wakeup): arbitrates against counter_finish_
//   park()'s ABA re-check (Pending→Canceled). RMWs on one atomic form a total
//   modification order, so exactly one of the two wins; the loser backs off.
// ---------------------------------------------------------------------------

Waiter* counter_decrement(Counter* counter, crd::u32 amount) noexcept
{
    CRD_ASSERT_MSG(counter != nullptr, "counter_decrement: null counter");
    CRD_ASSERT_MSG(amount  >= 1U,      "counter_decrement: amount must be >= 1");

    const crd::u32 old_val = counter->value.fetch_sub(amount, std::memory_order_acq_rel);
    CRD_ASSERT_MSG(amount <= old_val, "counter_decrement: underflow — decremented past zero");
    const crd::u32 new_val = old_val - amount;

    // Waiters only ever wait for 0 (see counter_wait's assert). Until we hit it,
    // leave the list alone — they stay parked.
    if (new_val != 0U)
        return nullptr;

    // We are the decrement that satisfies every waiter. Drain the list.
    Waiter* list = counter->waiters.exchange(nullptr, std::memory_order_acq_rel);

    Waiter* woken = nullptr;  // fibers this call owes a resume
    while (list != nullptr)
    {
        Waiter* nxt = list->next.load(std::memory_order_relaxed);

        if (list->claim.load(std::memory_order_acquire) == WaiterClaim::Canceled)
        {
            // Leftover from counter_finish_park's ABA cancel — the fiber is being
            // resumed by the scheduler, not by us. Discard.
            list = nxt;
            continue;
        }
        // Otherwise it is Pending (Wakeup is only set here, on a node we remove).

        WaiterClaim expected = WaiterClaim::Pending;
        if (list->claim.compare_exchange_strong(expected, WaiterClaim::Wakeup,
                std::memory_order_acq_rel, std::memory_order_acquire))
        {
            // We own the resume for this fiber.
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
        // else: counter_finish_park canceled it between our load and CAS — drop it.
        list = nxt;
    }

    return woken;
}

// ---------------------------------------------------------------------------
// counter_wait — runs on the waiting fiber
//
// Switch-then-publish: the fiber does NOT put its Waiter on the list itself.
// Becoming wakeable must happen *after* the fiber's context is saved, and the
// only thing that saves it is the fiber_switch to the scheduler. So the fiber
// fills in its Waiter, stashes a park request, and switches; the scheduler —
// see worker_pool.cpp's run_job_in_fiber → counter_finish_park — publishes the
// Waiter once we're safely parked.
//
// FiberState (debug builds): the fiber sets Active → Waiting before the switch;
// whoever resumes it (counter_decrement, or the scheduler on an ABA-cancel) sets
// Waiting → Ready before queueing the resume; this function then sets Ready →
// Active after the switch returns. Each transition is performed by exactly one
// party (serialised by the Waiter::claim CAS), so there is no data race despite
// FiberState being a plain (non-atomic) enum.
// ---------------------------------------------------------------------------

void counter_wait(Counter* counter, Waiter* w, Fiber* current_fiber,
                  FiberContext& scheduler_ctx, crd::u32 target) noexcept
{
    CRD_ASSERT_MSG(counter       != nullptr, "counter_wait: null counter");
    CRD_ASSERT_MSG(w             != nullptr, "counter_wait: null Waiter");
    CRD_ASSERT_MSG(current_fiber != nullptr, "counter_wait: null current_fiber");
    CRD_ASSERT_MSG(target == 0U,
                   "counter_wait: only target == 0 is supported — counter_decrement "
                   "drains the waiters list exactly at zero (see counter_decrement)");

    // Fast path: value is already at target — no need to park at all.
    if (counter->value.load(std::memory_order_acquire) == target)
        return;

    // Prepare the Waiter and hand parking off to the scheduler.
    w->fiber  = current_fiber;
    w->target = target;
    w->claim.store(WaiterClaim::Pending, std::memory_order_relaxed);
    w->next.store(nullptr,               std::memory_order_relaxed);
    w->park_finalized.store(false,       std::memory_order_relaxed);

#if CRD_ENABLE_ASSERTS
    CRD_ASSERT_MSG(current_fiber->state == FiberState::Active,
                   "counter_wait: fiber must be Active before parking");
    current_fiber->state = FiberState::Waiting;
#endif

    tl_set_pending_park(counter, w);
    fiber_switch(&current_fiber->context, &scheduler_ctx);

    // --- Resumed (by counter_decrement having claimed Wakeup, or by the
    //     scheduler if the value had already reached target at publish time) --- //

    // Don't unwind this frame (which would free `w` and let jobs::wait() release
    // the counter) until counter_finish_park has finished touching both. The
    // resume chain (decrement → enqueue → scheduler pop → fiber_switch) is far
    // longer than counter_finish_park's tail, so this almost never actually
    // spins; it's strictly defensive against the tight interleaving.
    while (!w->park_finalized.load(std::memory_order_acquire))
    { /* counter_finish_park is a handful of instructions away */ }

#if CRD_ENABLE_ASSERTS
    CRD_ASSERT_MSG(current_fiber->state == FiberState::Ready,
                   "counter_wait: fiber should be in Ready state on resume");
    current_fiber->state = FiberState::Active;
#endif
}

// ---------------------------------------------------------------------------
// counter_finish_park — runs on the scheduler thread, after the fiber switched out
//
// Memory ordering: the publish CAS is release; counter_decrement reads the
// Waiter after its exchange(acq_rel), so it sees w->fiber / w->target / claim
// (and, transitively, the fiber's Waiting state, which counter_wait wrote before
// the switch — sequenced-before this publish on the same OS thread). The ABA
// re-check load is acquire; the claim CAS is acq_rel / acquire.
// ---------------------------------------------------------------------------

bool counter_finish_park(Counter* counter, Waiter* w) noexcept
{
    CRD_ASSERT_MSG(counter != nullptr, "counter_finish_park: null counter");
    CRD_ASSERT_MSG(w       != nullptr, "counter_finish_park: null Waiter");
    CRD_ASSERT_MSG(w->target == 0U,    "counter_finish_park: only target == 0 is supported");

    // Publish w onto counter->waiters (Treiber push).
    Waiter* head = counter->waiters.load(std::memory_order_relaxed);
    do
    {
        w->next.store(head, std::memory_order_relaxed);
    } while (!counter->waiters.compare_exchange_weak(
                 head, w,
                 std::memory_order_release, std::memory_order_relaxed));

    // ABA re-check: did the counter reach target while the fiber was switching
    // out / we were publishing? If yes, race counter_decrement for this Waiter:
    // if we win the cancel CAS, no decrement owes the fiber a resume — the caller
    // does it.
    bool resumed_by_scheduler = false;
    if (counter->value.load(std::memory_order_acquire) == w->target)
    {
        WaiterClaim expected = WaiterClaim::Pending;
        resumed_by_scheduler = w->claim.compare_exchange_strong(expected, WaiterClaim::Canceled,
                                   std::memory_order_acq_rel, std::memory_order_acquire);
    }

    // Done touching `counter` and `w`. Release the fiber to complete jobs::wait()
    // — counter_wait spins on this before returning.
    w->park_finalized.store(true, std::memory_order_release);
    return resumed_by_scheduler;
}

} // namespace crd::jobs::detail
