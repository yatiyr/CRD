/* DIAG.4b -- weak-memory model of the job-counter publication/reclamation handshake, for the GenMC
 * stateless model checker (https://github.com/MPI-SWS/genmc). This is a standalone C11 artifact, NOT
 * engine code: it links nothing from crd and is never compiled by the Windows/CI build (crd_collect_sources
 * globs only .cpp/.hpp/.h, and this directory is not registered as a test target). It is authored here
 * and QUALIFIED ON A PINNED LINUX GenMC LANE -- not run on the Windows dev box -- exactly as the TSan/LSan
 * specimens are qualified on their sanitizer lanes (DIAG.1b/3f) and report InstrumentAbsent elsewhere.
 *
 * REGIME (per the DIAG.4b acceptance, which requires the three be kept distinct):
 *   - THIS artifact = the WEAK-MEMORY MODEL result: GenMC exhaustively explores every thread interleaving
 *     AND every C11 memory-model reordering, for the bounded topology below. It is exhaustive for those
 *     bounds; it says nothing about real hardware.
 *   - The controlled-interleaving driver (test_sched_check.cpp) = LOGICAL SCHEDULE REPLAY: one scripted
 *     interleaving of the production code, no weak-memory exploration.
 *   - The DG05 multicore stress (test_counter.cpp / test_fiber_pool.cpp) = REAL HARDWARE execution, many
 *     uncontrolled interleavings. None of the three subsumes the others.
 *
 * WHAT IS MODELLED: one Counter "slot" and one Waiter, list length 1, four worker threads -- the smallest
 * topology that exposes the reclamation race. Every atomic ordering below is transcribed verbatim from
 * engine/foundation/jobs/src/counter.cpp (line numbers cited); a wrong ordering would either hide the bug
 * or manufacture a race the engine does not have, making the verdict meaningless.
 *
 * SCOPE LIMIT: this models the park/publish/reclaim handshake (park_finalized) only. It does NOT model the
 * separate zero-drain/release race -- where jobs::wait() could release a counter after counter_decrement's
 * fetch_sub hit zero but before its waiters.exchange, letting a concurrent acquire recycle the slot under
 * the stale drain (fixed by the Counter::drained gate). That race was surfaced by the perturbed multicore
 * stress in test_sched_check.cpp, not by this model, and is deliberately out of scope here: extending the
 * model to cover it is not done, since a GenMC extension that cannot be run on this dev box would be an
 * unverifiable artifact.
 *
 * TWO VARIANTS, selected by -DCRD_MODEL_BROKEN_HANDSHAKE:
 *   repaired (default): counter_wait spins on w->park_finalized before unwinding, and counter_finish_park
 *     stores it last. The store happens-before the free/reacquire, so the scheduler's last touch of the
 *     Waiter and of the slot precede reclamation. GenMC must find NO error.
 *   broken (-DCRD_MODEL_BROKEN_HANDSHAKE): the spin and the store are both removed. The Waiter can be freed
 *     and the slot re-acquired while counter_finish_park is still mid-flight, so:
 *       (1) a heap use-after-free: the scheduler's ABA claim CAS (counter.cpp:339) touches w->claim after
 *           the waiter thread free()d it -- GenMC's native malloc/free oracle catches this;
 *       (2) a stale-slot read on the re-acquire branch: the scheduler sees value==1 (the re-acquirer reset
 *           it), skips the CAS so there is no UAF, but the slot's generation has advanced -- the assert()
 *           on the snapshot (the same invariant the C++ gate-only detector reports) catches this.
 *     Both oracles are needed: without (2) the re-acquire interleaving would read as clean.
 *
 * TO QUALIFY ON THE GenMC LANE (both invocations; verdicts are lane-pending, like the TSan routes):
 *     genmc -- counter_publish_reclaim.c
 *         expected: "Number of complete executions explored: N" with no error (handshake verified).
 *     genmc -- -DCRD_MODEL_BROKEN_HANDSHAKE counter_publish_reclaim.c
 *         expected: an error report -- an "Attempt to access freed memory" (UAF) and/or a failed
 *         assertion "gen == gen_snapshot" -- with -print-error-trace showing the reclaim-under-park order.
 * The side-effect-free spin loops are cut by GenMC's spin-assume transformation; no -unroll is required.
 */

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>

enum
{
    CLAIM_PENDING  = 0, /* WaiterClaim::Pending  */
    CLAIM_WAKEUP   = 1, /* WaiterClaim::Wakeup   */
    CLAIM_CANCELED = 2, /* WaiterClaim::Canceled */
};

typedef struct Waiter
{
    _Atomic(struct Waiter*) next;
    atomic_int              claim;          /* Pending/Wakeup/Canceled */
    atomic_int              park_finalized; /* the handshake flag */
} Waiter;

/* The pooled Counter slot. It persists for the pool's lifetime (never freed); a re-acquire "recycles" it
 * by bumping the generation (task_id in the engine) and resetting value/waiters -- see counter.cpp
 * acquire() (:90-97). */
static atomic_int       g_value;   /* Counter::value   */
static _Atomic(Waiter*) g_waiters; /* Counter::waiters (Treiber head) */
static atomic_uint      g_gen;     /* stands in for the per-acquire task_id */

/* Cross-thread handoffs (modelling harness, not part of the algorithm under test). */
static _Atomic(Waiter*) g_w;         /* the Waiter counter_wait handed to the scheduler */
static atomic_int       g_slot_free; /* waiter -> reacquirer: jobs::wait() released the slot */

/* counter_finish_park -- runs on the scheduler thread (counter.cpp:314). */
static void* sched_finish_park(void* unused)
{
    (void)unused;
    Waiter* const      w        = atomic_load_explicit(&g_w, memory_order_acquire);
    const unsigned int snapshot = atomic_load_explicit(&g_gen, memory_order_relaxed); /* task_id at entry */

    /* Publish w onto the waiters list (Treiber push); CAS is release / relaxed (counter.cpp:321-327). */
    Waiter* head = atomic_load_explicit(&g_waiters, memory_order_relaxed);
    do
    {
        atomic_store_explicit(&w->next, head, memory_order_relaxed);
    } while (!atomic_compare_exchange_weak_explicit(&g_waiters, &head, w, memory_order_release,
                                                    memory_order_relaxed));

    /* ABA re-check: value.load acquire (:336). If the counter already hit target(0), race the decrement
     * for this Waiter with a claim CAS Pending->Canceled, acq_rel / acquire (:339). */
    if (atomic_load_explicit(&g_value, memory_order_acquire) == 0)
    {
        int expected = CLAIM_PENDING;
        (void)atomic_compare_exchange_strong_explicit(&w->claim, &expected, CLAIM_CANCELED,
                                                      memory_order_acq_rel, memory_order_acquire);
    }

    /* Owner-release invariant (the C++ gate-only detector's check): the slot must still be ours before our
     * last touch. Broken variant lets a re-acquire bump the generation before we get here. */
    assert(atomic_load_explicit(&g_gen, memory_order_relaxed) == snapshot);

#ifndef CRD_MODEL_BROKEN_HANDSHAKE
    /* Release the fiber; counter_wait spins on this before unwinding. Store is release (:347). */
    atomic_store_explicit(&w->park_finalized, 1, memory_order_release);
#endif
    return NULL;
}

/* counter_decrement -- the decrement that brings the counter to zero (counter.cpp:176). */
static void* dec_to_zero(void* unused)
{
    (void)unused;
    const int old = atomic_fetch_sub_explicit(&g_value, 1, memory_order_acq_rel); /* :181 */
    if (old - 1 != 0)
        return NULL;

    /* Drain the waiters list: exchange acq_rel (:193). */
    Waiter* list = atomic_exchange_explicit(&g_waiters, NULL, memory_order_acq_rel);
    while (list != NULL)
    {
        Waiter* const nxt = atomic_load_explicit(&list->next, memory_order_relaxed);
        if (atomic_load_explicit(&list->claim, memory_order_acquire) == CLAIM_CANCELED) /* :200 */
        {
            list = nxt;
            continue;
        }
        int expected = CLAIM_PENDING;
        /* Claim CAS Pending->Wakeup, acq_rel / acquire (:212). Winning it owns the fiber's resume. */
        (void)atomic_compare_exchange_strong_explicit(&list->claim, &expected, CLAIM_WAKEUP,
                                                      memory_order_acq_rel, memory_order_acquire);
        list = nxt;
    }
    return NULL;
}

/* counter_wait's tail -- runs on the waiting fiber after it is resumed (counter.cpp:284). */
static void* waiter_unwind(void* unused)
{
    (void)unused;
    Waiter* const w = atomic_load_explicit(&g_w, memory_order_acquire);

    /* Resumed when the claim leaves Pending: Wakeup (the decrement owns us) or Canceled (the scheduler's
     * ABA cancel; the scheduler re-queues the fiber). Side-effect-free spin -> GenMC spin-assume. */
    while (atomic_load_explicit(&w->claim, memory_order_acquire) == CLAIM_PENDING)
    {
    }

#ifndef CRD_MODEL_BROKEN_HANDSHAKE
    /* The handshake: do not unwind (free w, release the slot) until counter_finish_park finished touching
     * w. Load is acquire (:294). */
    while (atomic_load_explicit(&w->park_finalized, memory_order_acquire) == 0)
    {
    }
#endif

    free(w); /* the fiber frame unwinds; the stack-resident Waiter is gone */
    atomic_store_explicit(&g_slot_free, 1, memory_order_release); /* jobs::wait() released the counter */
    return NULL;
}

/* The pool re-acquiring the freed slot for another job -- counter.cpp acquire() (:69-99). */
static void* reacquire_slot(void* unused)
{
    (void)unused;
    while (atomic_load_explicit(&g_slot_free, memory_order_acquire) == 0)
    {
    }
    atomic_fetch_add_explicit(&g_gen, 1, memory_order_relaxed); /* new task_id */
    atomic_store_explicit(&g_value, 1, memory_order_relaxed);
    atomic_store_explicit(&g_waiters, NULL, memory_order_relaxed);
    return NULL;
}

int main(void)
{
    /* counter_wait's pre-switch prep (counter.cpp:266-276): fill the Waiter, value already 1. */
    Waiter* const w = (Waiter*)malloc(sizeof(Waiter));
    if (w == NULL)
        abort();
    atomic_init(&w->next, NULL);
    atomic_init(&w->claim, CLAIM_PENDING);
    atomic_init(&w->park_finalized, 0);

    atomic_init(&g_value, 1);
    atomic_init(&g_waiters, NULL);
    atomic_init(&g_gen, 0u);
    atomic_init(&g_slot_free, 0);
    atomic_init(&g_w, w);

    pthread_t t_sched;
    pthread_t t_dec;
    pthread_t t_waiter;
    pthread_t t_reacq;
    if (pthread_create(&t_sched, NULL, sched_finish_park, NULL))
        abort();
    if (pthread_create(&t_dec, NULL, dec_to_zero, NULL))
        abort();
    if (pthread_create(&t_waiter, NULL, waiter_unwind, NULL))
        abort();
    if (pthread_create(&t_reacq, NULL, reacquire_slot, NULL))
        abort();

    pthread_join(t_sched, NULL);
    pthread_join(t_dec, NULL);
    pthread_join(t_waiter, NULL);
    pthread_join(t_reacq, NULL);
    return 0;
}
