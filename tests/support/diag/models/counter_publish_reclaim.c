/* DIAG.4b -- weak-memory model of the job-counter publication/reclamation handshakes, for the GenMC
 * stateless model checker (https://github.com/MPI-SWS/genmc). This is a standalone C11 artifact, NOT
 * engine code: it links nothing from crd and is never compiled by the Windows/CI build (crd_collect_sources
 * globs only .cpp/.hpp/.h, and this directory is not registered as a test target). It is checked on Linux
 * with the pinned GenMC (cmake/pins.json tools/genmc, built by scripts/install-genmc.py) by
 * scripts/check-genmc-models.py: in the complete CI tier on the linux-clang-tsan entry, and on the WSL
 * reference host (recorded run: docs/sessions/2026-10-10-diag-4b-genmc.md).
 *
 * REGIME (per the DIAG.4b acceptance, which requires the three be kept distinct):
 *   - THIS artifact = the WEAK-MEMORY MODEL result: GenMC exhaustively explores every thread interleaving
 *     AND every RC11 reordering, for the bounded topology below. It is exhaustive for those bounds; it says
 *     nothing about real hardware.
 *   - The controlled-interleaving driver (test_sched_check.cpp) = LOGICAL SCHEDULE REPLAY: one scripted
 *     interleaving of the production code, no weak-memory exploration.
 *   - The DG05 multicore stress (test_counter.cpp / test_fiber_pool.cpp / test_sched_check.cpp case 6) = REAL
 *     HARDWARE execution, many uncontrolled interleavings. None of the three subsumes the others.
 *
 * BOUNDS: one Counter slot, one Waiter, list length 1, four worker threads (scheduler, zero-decrement,
 * waiting fiber, re-acquirer) -- the smallest topology that exposes both reclamation races. Every atomic
 * ordering is transcribed from engine/foundation/jobs/src/counter.cpp (counter_finish_park, counter_decrement,
 * counter_wait) and jobs.cpp (jobs::wait); a wrong ordering would either hide a bug or manufacture one the
 * engine does not have. Spin loops are side-effect free and are cut by GenMC's spin-assume transformation; no
 * -unroll bound is used.
 *
 * ONE ABSTRACTION: the publish loop's compare_exchange_weak is modelled as a strong CAS. GenMC 0.19.0 cannot
 * explore this weak CAS loop (it warns that the weak CAS "could not be strengthened", diverges without -unroll
 * and crashes with exit 139 under -unroll=N). The abstraction is sound here: a spurious failure only repeats
 * the relaxed store of the same head into w->next before w is published, which no other thread can observe,
 * and then retries -- every other behaviour of the weak loop is a behaviour of the strong one.
 *
 * WHAT IS CHECKED. Both reclamation handshakes guard a Waiter that lives in jobs::wait()'s frame and a Counter
 * slot that jobs::wait() releases for re-acquisition:
 *   (a) park_finalized: counter_wait spins on it before unwinding, counter_finish_park stores it last, so the
 *       scheduler's last touch of the Waiter and of the slot precede reclamation.
 *   (b) drained: counter_decrement stores it after its drain loop -- its last touch of any stolen Waiter --
 *       and jobs::wait() spins on it before the Waiter's frame ends and before release(). The first GenMC run
 *       (2026-10-10) found the engine storing it right after the exchange instead: the decrement could still
 *       be inside the loop, holding a Waiter that the scheduler's ABA cancel had already handed back to its
 *       fiber, when that fiber unwound -- the decrement's claim CAS then touched a dead stack Waiter.
 * Oracles: GenMC's native malloc/free oracle (the Waiter is heap-allocated here so a touch after free() is an
 * "Attempt to access freed memory") and the assert() on the slot generation (the same owner-release
 * invariant the C++ gate-only detector reports).
 *
 * THREE VARIANTS (scripts/check-genmc-models.py runs all three; each is held to its expected verdict):
 *     genmc --disable-estimation -- counter_publish_reclaim.c
 *         repaired: "No errors were detected" with the explored execution count.
 *     genmc --disable-estimation -print-error-trace -- -DCRD_MODEL_BROKEN_HANDSHAKE counter_publish_reclaim.c
 *         seeded (a): the park_finalized spin and store are removed. Must report an access to freed memory or
 *         the failed generation assertion "... == snapshot".
 *     genmc --disable-estimation -print-error-trace -- -DCRD_MODEL_EARLY_DRAINED counter_publish_reclaim.c
 *         seeded (b): drained is stored right after the exchange (the pre-2026-10-10 engine). Must report an
 *         access to freed memory (the decrement's claim CAS on the unwound Waiter).
 */

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>

enum
{
    CLAIM_PENDING = 0,  /* WaiterClaim::Pending  */
    CLAIM_WAKEUP = 1,   /* WaiterClaim::Wakeup   */
    CLAIM_CANCELED = 2, /* WaiterClaim::Canceled */
};

typedef struct Waiter
{
    _Atomic(struct Waiter*) next;
    atomic_int claim;          /* Pending/Wakeup/Canceled */
    atomic_int park_finalized; /* handshake (a) */
} Waiter;

/* The pooled Counter slot. It persists for the pool's lifetime (never freed); a re-acquire "recycles" it
 * by bumping the generation (task_id in the engine) and resetting value/drained/waiters -- see
 * CounterPool::acquire(). */
static atomic_int g_value;         /* Counter::value   */
static atomic_int g_drained;       /* Counter::drained, handshake (b) */
static _Atomic(Waiter*) g_waiters; /* Counter::waiters (Treiber head) */
static atomic_uint g_gen;          /* stands in for the per-acquire task_id */

/* Cross-thread handoffs (modelling harness, not part of the algorithm under test). */
static _Atomic(Waiter*) g_w;   /* the Waiter counter_wait handed to the scheduler */
static atomic_int g_slot_free; /* waiter -> reacquirer: jobs::wait() released the slot */

/* counter_finish_park -- runs on the scheduler thread. */
static void* sched_finish_park(void* unused)
{
    (void)unused;
    Waiter* const w = atomic_load_explicit(&g_w, memory_order_acquire);
    const unsigned int snapshot = atomic_load_explicit(&g_gen, memory_order_relaxed); /* task_id at entry */

    /* Publish w onto the waiters list (Treiber push); CAS is release / relaxed. The engine's weak CAS is
     * modelled strong (see ONE ABSTRACTION above). */
    Waiter* head = atomic_load_explicit(&g_waiters, memory_order_relaxed);
    do
    {
        atomic_store_explicit(&w->next, head, memory_order_relaxed);
    } while (
        !atomic_compare_exchange_strong_explicit(&g_waiters, &head, w, memory_order_release, memory_order_relaxed));

    /* ABA re-check: value.load acquire. If the counter already hit target(0), race the decrement for this
     * Waiter with a claim CAS Pending->Canceled, acq_rel / acquire; winning it resumes the fiber. */
    if (atomic_load_explicit(&g_value, memory_order_acquire) == 0)
    {
        int expected = CLAIM_PENDING;
        (void)atomic_compare_exchange_strong_explicit(&w->claim, &expected, CLAIM_CANCELED, memory_order_acq_rel,
                                                      memory_order_acquire);
    }

    /* Owner-release invariant (the C++ gate-only detector's check): the slot must still be ours before our
     * last touch. Seeded (a) lets a re-acquire bump the generation before we get here. */
    assert(atomic_load_explicit(&g_gen, memory_order_relaxed) == snapshot);

#ifndef CRD_MODEL_BROKEN_HANDSHAKE
    /* Release the fiber; counter_wait spins on this before unwinding. Store is release. */
    atomic_store_explicit(&w->park_finalized, 1, memory_order_release);
#endif
    return NULL;
}

/* counter_decrement -- the decrement that brings the counter to zero. */
static void* dec_to_zero(void* unused)
{
    (void)unused;
    const int old = atomic_fetch_sub_explicit(&g_value, 1, memory_order_acq_rel);
    if (old - 1 != 0)
    {
        return NULL;
    }

    /* Drain the waiters list: exchange acq_rel. */
    Waiter* list = atomic_exchange_explicit(&g_waiters, NULL, memory_order_acq_rel);
#ifdef CRD_MODEL_EARLY_DRAINED
    /* Seeded (b): the pre-2026-10-10 placement, before the loop below still touches the stolen Waiters. */
    atomic_store_explicit(&g_drained, 1, memory_order_release);
#endif
    while (list != NULL)
    {
        Waiter* const nxt = atomic_load_explicit(&list->next, memory_order_relaxed);
        if (atomic_load_explicit(&list->claim, memory_order_acquire) == CLAIM_CANCELED)
        {
            list = nxt;
            continue;
        }
        int expected = CLAIM_PENDING;
        /* Claim CAS Pending->Wakeup, acq_rel / acquire. Winning it owns the fiber's resume. */
        (void)atomic_compare_exchange_strong_explicit(&list->claim, &expected, CLAIM_WAKEUP, memory_order_acq_rel,
                                                      memory_order_acquire);
        list = nxt;
    }
#ifndef CRD_MODEL_EARLY_DRAINED
    /* The drain's last touch of any stolen Waiter is behind us: the Waiters' frames and the slot may go. */
    atomic_store_explicit(&g_drained, 1, memory_order_release);
#endif
    return NULL;
}

/* counter_wait's tail and jobs::wait()'s release -- runs on the waiting fiber after it is resumed. */
static void* waiter_unwind(void* unused)
{
    (void)unused;
    Waiter* const w = atomic_load_explicit(&g_w, memory_order_acquire);

    /* Resumed when the claim leaves Pending: Wakeup (the decrement owns us) or Canceled (the scheduler's
     * ABA cancel; the scheduler re-queues the fiber). */
    while (atomic_load_explicit(&w->claim, memory_order_acquire) == CLAIM_PENDING)
    {
    }

#ifndef CRD_MODEL_BROKEN_HANDSHAKE
    /* Handshake (a): do not unwind until counter_finish_park finished touching w and the slot. Acquire. */
    while (atomic_load_explicit(&w->park_finalized, memory_order_acquire) == 0)
    {
    }
#endif

    /* Handshake (b): jobs::wait() spins on drained (acquire) while its Waiter is still in scope. */
    while (atomic_load_explicit(&g_drained, memory_order_acquire) == 0)
    {
    }

    free(w); /* jobs::wait() returns: the frame holding the Waiter is gone */
    atomic_store_explicit(&g_slot_free, 1, memory_order_release); /* release(): the slot may be recycled */
    return NULL;
}

/* The pool re-acquiring the freed slot for another job -- CounterPool::acquire(). */
static void* reacquire_slot(void* unused)
{
    (void)unused;
    while (atomic_load_explicit(&g_slot_free, memory_order_acquire) == 0)
    {
    }
    atomic_fetch_add_explicit(&g_gen, 1, memory_order_relaxed); /* new task_id */
    atomic_store_explicit(&g_value, 1, memory_order_relaxed);
    atomic_store_explicit(&g_drained, 0, memory_order_relaxed);
    atomic_store_explicit(&g_waiters, NULL, memory_order_relaxed);
    return NULL;
}

int main(void)
{
    /* counter_wait's pre-switch prep: fill the Waiter, value already 1. */
    Waiter* const w = (Waiter*)malloc(sizeof(Waiter));
    if (w == NULL)
    {
        abort();
    }
    atomic_init(&w->next, NULL);
    atomic_init(&w->claim, CLAIM_PENDING);
    atomic_init(&w->park_finalized, 0);

    atomic_init(&g_value, 1);
    atomic_init(&g_drained, 0);
    atomic_init(&g_waiters, NULL);
    atomic_init(&g_gen, 0u);
    atomic_init(&g_slot_free, 0);
    atomic_init(&g_w, w);

    pthread_t t_sched;
    pthread_t t_dec;
    pthread_t t_waiter;
    pthread_t t_reacq;
    if (pthread_create(&t_sched, NULL, sched_finish_park, NULL))
    {
        abort();
    }
    if (pthread_create(&t_dec, NULL, dec_to_zero, NULL))
    {
        abort();
    }
    if (pthread_create(&t_waiter, NULL, waiter_unwind, NULL))
    {
        abort();
    }
    if (pthread_create(&t_reacq, NULL, reacquire_slot, NULL))
    {
        abort();
    }

    pthread_join(t_sched, NULL);
    pthread_join(t_dec, NULL);
    pthread_join(t_waiter, NULL);
    pthread_join(t_reacq, NULL);
    return 0;
}
