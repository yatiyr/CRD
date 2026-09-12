#include "worker_pool.hpp"
#include <crd/jobs/detail/fiber_context.hpp>
#include <crd/jobs/jobs.hpp> // performance_core_cpu_ids (ADR-0094 affinity)
#include <crd/jobs/observer.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/platform.hpp>
#include <crd/core/types.hpp>

#include <cstring>
#include <thread>

#if CRD_OS_WINDOWS
#include <windows.h>
#elif CRD_OS_LINUX
#include <pthread.h>
#include <sched.h>
#endif

namespace crd::jobs::detail
{

// ---------------------------------------------------------------------------
// CRD_JOBS_TLS_OPAQUE — load-bearing optimization barrier for the fiber runtime.
//
// A fiber may suspend on OS thread A inside a job and be resumed on OS thread B.
// The C++ abstract machine has no concept of that migration, so an optimizing
// compiler is entitled to materialise the TLS base (the %fs / TEB pointer) once
// and reuse it across an opaque call — and GCC at -O3 does exactly that: it
// hoists `lea tl_sched_ctx@tpoff(%fs:0)` out of job_fiber_trampoline's loop and
// keeps it in a callee-saved register across the job call. After a cross-thread
// resume that cached address points at the *wrong* thread's tl_sched_ctx, so the
// trailing fiber_switch jumps onto another live thread's scheduler stack — two
// threads, one stack — corrupting it and crashing later (the historical
// `linux-gcc-release` "transient SEGFAULT in jobs: run_and_wait from inside a
// worker fiber" flake; MSVC was masked by the /Od on this TU).
//
// Routing every post-resume thread-local access through a function that the
// optimizer may neither inline nor IPA-analyse forces the TLS base to be re-read
// from the live CPU register each time, which is correct after a migration.
// `noipa` (GCC) is required, not just `noinline`: without it IPA-PURE-CONST can
// still prove the accessor "const" and CSE two calls back together.
#if defined(__GNUC__) && !defined(__clang__)
#define CRD_JOBS_TLS_OPAQUE __attribute__((noipa))
#else
#define CRD_JOBS_TLS_OPAQUE CRD_NOINLINE
#endif

// ---------------------------------------------------------------------------
// Thread-local state
//
// Each OS thread in the pool maintains its own scheduler context, current fiber,
// pending job, thread index, and back-pointer to the owning pool.
//
// tl_sched_ctx  – the OS thread's own stack context. Jobs switch to this when
//                 they complete or suspend so the scheduler loop can continue.
// tl_fiber      – the job fiber currently running on this thread; nullptr when
//                 executing on the OS (scheduler) stack.
// tl_job        – the job to run; written by run_job_in_fiber before the first
//                 switch to a fresh fiber so the trampoline can read it.
// tl_idx        – this thread's position in the scheduler's thread_states array.
// tl_pool_ptr   – back-pointer used by the public v1h API and introspection helpers.
// ---------------------------------------------------------------------------

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
static thread_local FiberContext   tl_sched_ctx{};
static thread_local Fiber*         tl_fiber    {nullptr};
// Store fn/data separately to avoid thread_local alignas(64) issues on MSVC.
static thread_local void         (*tl_job_fn)(void*) {nullptr};
static thread_local void*          tl_job_data       {nullptr};
static thread_local crd::u32       tl_idx      {0U};
static thread_local WorkerPool*    tl_pool_ptr {nullptr};
static thread_local FrameArena*    tl_frame_arena {nullptr};
// Park request handed from counter_wait (on the fiber) to run_job_in_fiber (the
// scheduler) after the fiber switches out — same OS thread, so a plain TLS slot.
static thread_local PendingPark    tl_pending_park{};
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

// The first three are CRD_JOBS_TLS_OPAQUE because job_fiber_trampoline reads
// them on the far side of a fiber resume that may have migrated this fiber to a
// different OS thread — see the macro comment above. tl_thread_index /
// tl_frame_arena_ref / the pending-park slot are only ever touched on the same
// thread that set them.
CRD_JOBS_TLS_OPAQUE FiberContext& tl_scheduler_context() noexcept { return tl_sched_ctx; }
CRD_JOBS_TLS_OPAQUE Fiber*&       tl_current_fiber_ref() noexcept { return tl_fiber; }
CRD_JOBS_TLS_OPAQUE WorkerPool*   tl_worker_pool()       noexcept { return tl_pool_ptr; }
crd::u32      tl_thread_index()      noexcept { return tl_idx; }
FrameArena&   tl_frame_arena_ref()    noexcept
{
    CRD_ASSERT_MSG(tl_frame_arena != nullptr,
                   "tl_frame_arena_ref: frame arena not set — call jobs::init() first");
    return *tl_frame_arena;
}

void tl_set_pending_park(Counter* counter, Waiter* waiter) noexcept
{
    CRD_ASSERT_MSG(tl_pending_park.counter == nullptr,
                   "tl_set_pending_park: a previous park request was not consumed");
    tl_pending_park.counter = counter;
    tl_pending_park.waiter  = waiter;
}

PendingPark tl_take_pending_park() noexcept
{
    const PendingPark p = tl_pending_park;
    tl_pending_park = PendingPark{};
    return p;
}

// ---------------------------------------------------------------------------
// resume_fiber_fn — sentinel function pointer
//
// A synthetic JobDecl with fn == &resume_fiber_fn signals that data is a
// Fiber* to resume directly (not a new job to run in a fresh fiber).
// run_job_in_fiber detects this and switches straight to the waiting fiber's
// saved context rather than acquiring a new one from the pool.
// ---------------------------------------------------------------------------

static void resume_fiber_fn(void* /*data*/) noexcept
{
    CRD_FATAL("resume_fiber_fn: sentinel — must never be called directly");
}

// ---------------------------------------------------------------------------
// job_fiber_trampoline
//
// Entry point burned into every fiber stack at pool init (FiberPoolConfig::trampoline).
// On the first switch to a fresh fiber, reads tl_job (set by run_job_in_fiber
// immediately before the switch) and calls fn(data).
//
// After fn(data) returns:
//   1. Captures the current fiber pointer.
//   2. Clears tl_fiber (completion signal for the scheduler).
//   3. Switches back to the scheduler stack.
//
// The while(true) is a safety net; in normal operation WorkerPool calls
// fiber_init_stack before releasing the fiber to the pool, so re-acquired
// fibers always restart here rather than resuming mid-loop.
//
// THREAD-MIGRATION HAZARD: if the job suspended (jobs::wait from inside a fiber)
// it may resume — and therefore return to us here — on a *different* OS thread
// than the one that dispatched it. Every thread-local touched after that point
// (the scheduler context to switch back to, the current-fiber slot, the pool
// back-pointer) must be re-read through the CRD_JOBS_TLS_OPAQUE accessors, never
// the raw `tl_*` globals — otherwise the optimizer caches the pre-resume TLS
// base and we switch onto the wrong thread's stack. See CRD_JOBS_TLS_OPAQUE.
// ---------------------------------------------------------------------------

static void job_fiber_trampoline() noexcept
{
    while (true)
    {
        tl_job_fn(tl_job_data);
        // --- past this line we may be on a different OS thread (see above) ---

        // Decrement the fiber's associated counter (if any) and wake satisfied waiters.
        // Read the fiber pointer from this thread's slot via the opaque accessor;
        // the counter is read off the fiber struct so it survives suspension + resume.
        Fiber*& cur_fiber = tl_current_fiber_ref();
        Fiber* const done = cur_fiber;
        cur_fiber = nullptr;    // completion signal: run_job_in_fiber checks this
        Counter* const c = done->job_counter;
        done->job_counter = nullptr;
        if (c != nullptr)
        {
            WorkerPool* const pool = tl_worker_pool();
            Waiter* woken = counter_decrement(c);
            while (woken != nullptr)
            {
                Waiter* const next = woken->next.load(std::memory_order_relaxed);
                pool->enqueue_fiber_resume(woken->fiber);
                woken = next;
            }
        }

        fiber_switch(&done->context, &tl_scheduler_context());
    }
}

// ---------------------------------------------------------------------------
// WorkerPool — private helpers
// ---------------------------------------------------------------------------

FiberTier WorkerPool::stack_size_to_tier(crd::jobs::StackSize s) noexcept
{
    switch (s)
    {
    case crd::jobs::StackSize::Small:  return FiberTier::Small;
    case crd::jobs::StackSize::Medium: return FiberTier::Medium;
    case crd::jobs::StackSize::Large:  return FiberTier::Large;
    }
    CRD_FATAL("WorkerPool::stack_size_to_tier: unknown StackSize");
    return FiberTier::Small; // unreachable — silences compiler warning
}

// ---------------------------------------------------------------------------
// WorkerPool::run_job_in_fiber
//
// Routes a job through the fiber system. Two cases:
//
//   Resume job (job.fn == &resume_fiber_fn):
//     data is a suspended Fiber* — switch directly to its saved context without
//     acquiring a new fiber from the pool. FiberState Ready → Active is handled
//     by counter_wait (inside the resuming fiber) after the switch returns there.
//
//   New job:
//     Acquire a fiber from the pool, set tl_job (so the trampoline can read it),
//     set tl_fiber (so the trampoline can reference itself on completion), then
//     switch to the fiber.
//
// After fiber_switch returns:
//   tl_fiber == nullptr   → trampoline signalled completion; reset the fiber's
//                           context to initial_ctx and return it to the pool.
//   tl_fiber != nullptr   → fiber suspended in counter_wait; leave it active.
// ---------------------------------------------------------------------------

void WorkerPool::run_job_in_fiber(const crd::jobs::JobDecl& job)
{
    Fiber* target = nullptr;

    // Cache the observer pointer once for the entire dispatch. set_observer
    // may race with this read (well-defined, atomic-acquire load); we use
    // the snapshot consistently for the begin/end/yield/resume pair so we
    // don't fire a mismatched event sequence under concurrent replacement.
    const crd::jobs::JobObserver* const obs = crd::jobs::current_observer();
    const crd::u8 thread_idx = static_cast<crd::u8>(tl_thread_index());

    const bool is_resume = (job.fn == &resume_fiber_fn);

    if (is_resume)
    {
        target = static_cast<Fiber*>(job.data);
        CRD_ASSERT_MSG(target != nullptr,
                       "run_job_in_fiber: resume sentinel carries null fiber pointer");
        // job_counter stays in target->job_counter from when the fiber was first launched;
        // no update needed here — the fiber's counter survives suspension.
        if (obs != nullptr && obs->on_fiber_resume != nullptr)
        {
            obs->on_fiber_resume(static_cast<crd::jobs::FiberHandle>(target), thread_idx);
        }
    }
    else
    {
        target = m_fiber_pool.acquire(stack_size_to_tier(job.stack));
        if (!target)
            return; // pool exhausted: CRD_ASSERT already fired inside acquire()
        tl_job_fn = job.fn;
        Counter* cp = nullptr;
        std::memcpy(&cp, &job._pad[0], sizeof(cp));
        target->job_counter = cp;

        // SBO path: callable bytes were packed into data + _pad[9..41] by make_job<F>.
        // Copy them into the fiber's sbo_buf so they survive suspension + resume on any thread.
        if (job._pad[8] == crd::jobs::detail::kSboFlag)
        {
            std::memcpy(&target->sbo_buf[0], &job.data, 8U);
            std::memcpy(&target->sbo_buf[8], &job._pad[9], 33U);
            tl_job_data = target->sbo_buf;
        }
        else
        {
            tl_job_data = job.data;
        }

        if (obs != nullptr && obs->on_job_begin != nullptr)
        {
            obs->on_job_begin(static_cast<crd::jobs::FiberHandle>(target), thread_idx,
                              static_cast<crd::u8>(job.priority),
                              static_cast<crd::u8>(job.stack));
        }
    }

    tl_fiber = target;
    fiber_switch(&tl_sched_ctx, &target->context);

    // Back on the OS (scheduler) stack. By now `target`'s context is fully saved.
    //
    // Re-read the thread index post-switch: the OS thread that we now resume on
    // is the one that was waiting in worker_loop / pump; it may differ from
    // `thread_idx` captured before the switch (it doesn't differ today for the
    // immediate-return paths -- run_job_in_fiber returns to its caller on the
    // same OS thread -- but the OS thread that woke up to handle the next event
    // is the one whose index we should report to the observer).
    const crd::u8 post_thread_idx = static_cast<crd::u8>(tl_thread_index());

    if (tl_fiber == nullptr)
    {
        // Job completed — the trampoline already cleared target->job_counter
        // *and* tl_fiber. Rebuild the initial stack frame so the fiber is clean
        // on re-use.
        if (obs != nullptr && obs->on_job_end != nullptr)
        {
            obs->on_job_end(static_cast<crd::jobs::FiberHandle>(target), post_thread_idx);
        }
        fiber_init_stack(target->context, target->usable_base, target->usable_size, target->trampoline);
        m_fiber_pool.release(target);
    }
    else
    {
        // The fiber suspended inside counter_wait — it handed us a park request.
        if (obs != nullptr && obs->on_fiber_yield != nullptr)
        {
            obs->on_fiber_yield(static_cast<crd::jobs::FiberHandle>(target), post_thread_idx);
        }
        // Publish its Waiter now that its context is saved (so it only becomes
        // wakeable once it's safe to resume). counter_finish_park returns true iff
        // the ABA re-check found the value already at target and won the cancel
        // CAS, in which case nothing else owes the fiber a resume — we do it.
        const PendingPark park = tl_take_pending_park();
        CRD_ASSERT_MSG(park.counter != nullptr && park.waiter != nullptr,
                       "run_job_in_fiber: fiber suspended without a valid park request");
        if (counter_finish_park(park.counter, park.waiter))
        {
#if CRD_ENABLE_ASSERTS
            // counter_finish_park left it Waiting; promote like a normal wakeup
            // so counter_wait's post-switch assert (state == Ready) holds.
            CRD_ASSERT_MSG(target->state == FiberState::Waiting,
                           "run_job_in_fiber: parked fiber was not Waiting");
            target->state = FiberState::Ready;
#endif
            enqueue_fiber_resume(target);
        }
        // else: a counter_decrement claimed Wakeup and will (or already did)
        // enqueue the resume. Either way `target` stays parked until then.

        // Crucial: the suspended path leaves tl_fiber == target. Clear it — the
        // caller (worker_loop / pump) is back on the bare OS thread, NOT running
        // a fiber. A stale tl_fiber would make a later jobs::wait() on this
        // thread take the fiber-suspend path with a garbage "current fiber",
        // corrupting the runtime. (The completed path above is fine: the
        // trampoline already nulled tl_fiber.)
        tl_fiber = nullptr;
    }
}

// ---------------------------------------------------------------------------
// WorkerPool::worker_loop
//
// Per-thread function for background worker threads (indices 1..N-1). Sets up
// thread-local identity, then alternates between draining the scheduler and
// sleeping until m_stopping is set.
// ---------------------------------------------------------------------------

void WorkerPool::worker_loop(WorkerPool* self, crd::u32 thread_index)
{
    tl_idx         = thread_index;
    tl_pool_ptr    = self;
    tl_frame_arena = &self->m_frame_arenas[thread_index];

    while (!self->m_stopping.load(std::memory_order_acquire))
    {
        std::optional<crd::jobs::JobDecl> job = self->m_scheduler.try_pop(thread_index);
        if (job)
        {
            self->run_job_in_fiber(*job);
        }
        else
        {
            self->m_scheduler.wait_for_work(thread_index);
        }
    }
}

// ---------------------------------------------------------------------------
// WorkerPool — lifecycle
// ---------------------------------------------------------------------------

namespace
{
// ADR-0094 affinity. Build a P-cores-first CPU order and pin the main thread (index 0) + each worker (index i) to
// order[i]. No-op when topology is unknown (e.g. WSL) ⇒ unsupported hosts and the default path are unaffected.
void pin_pool_to_pcores([[maybe_unused]] std::vector<std::thread>& workers,
                        [[maybe_unused]] crd::u32 num_threads) noexcept
{
    crd::u32 pids[64];
    const crd::u32 k = crd::jobs::performance_core_cpu_ids(pids, 64U);
    if (k == 0U)
    {
        return; // unknown topology → leave placement to the OS
    }
    const auto hw = static_cast<crd::u32>(std::thread::hardware_concurrency());
    crd::u32 order[64];
    crd::u32 n = 0U;
    for (crd::u32 i = 0U; i < k && n < 64U; ++i)
    {
        order[n++] = pids[i];
    }
    for (crd::u32 c = 0U; c < hw && n < 64U; ++c)
    {
        bool seen = false;
        for (crd::u32 j = 0U; j < k; ++j)
        {
            if (pids[j] == c)
            {
                seen = true;
                break;
            }
        }
        if (!seen)
        {
            order[n++] = c;
        }
    }
    if (n == 0U)
    {
        return;
    }
#if CRD_OS_WINDOWS
    ::SetThreadAffinityMask(::GetCurrentThread(), static_cast<DWORD_PTR>(1) << order[0]);
    for (crd::u32 i = 1U; i < num_threads; ++i)
    {
        ::SetThreadAffinityMask(reinterpret_cast<HANDLE>(workers[i - 1U].native_handle()),
                                static_cast<DWORD_PTR>(1) << order[i < n ? i : i % n]);
    }
#elif CRD_OS_LINUX
    auto pin = [](pthread_t h, crd::u32 cpu)
    {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(static_cast<int>(cpu), &set);
        (void)pthread_setaffinity_np(h, sizeof(set), &set);
    };
    pin(pthread_self(), order[0]);
    for (crd::u32 i = 1U; i < num_threads; ++i)
    {
        pin(static_cast<pthread_t>(workers[i - 1U].native_handle()), order[i < n ? i : i % n]);
    }
#endif
}
} // namespace

bool WorkerPool::init(const WorkerConfig& cfg)
{
    CRD_ASSERT_MSG(!m_initialized, "WorkerPool::init called twice");

    m_num_threads = (cfg.num_threads == 0U)
        ? static_cast<crd::u32>(std::thread::hardware_concurrency())
        : cfg.num_threads;
    if (m_num_threads == 0U)
        m_num_threads = 1U; // hardware_concurrency() returned 0 on this platform

    m_pcore_routing = cfg.pcore_routing;

    SchedulerConfig sched_cfg;
    sched_cfg.num_threads        = m_num_threads;
    sched_cfg.deque_capacity     = cfg.deque_capacity;
    sched_cfg.injection_capacity = cfg.injection_capacity;
    sched_cfg.targeted_wake      = cfg.pcore_routing; // ADR-0094 opt-in; default false ⇒ shared-semaphore path

    if (!m_scheduler.init(sched_cfg))
        return false;

    FiberPoolConfig fiber_cfg;
    fiber_cfg.small_count  = cfg.small_fiber_count;
    fiber_cfg.medium_count = cfg.medium_fiber_count;
    fiber_cfg.large_count  = cfg.large_fiber_count;
    fiber_cfg.trampoline   = &job_fiber_trampoline;

    if (!m_fiber_pool.init(fiber_cfg))
    {
        m_scheduler.shutdown();
        return false;
    }

    if (!m_counter_pool.init(cfg.max_counters))
    {
        m_fiber_pool.shutdown();
        m_scheduler.shutdown();
        return false;
    }

    // Allocate per-thread frame arenas. Use unique_ptr<T[]> so the base address is
    // stable — setting tl_frame_arena_ptr into each element will never be invalidated
    // by a reallocation (unlike std::vector).
    m_frame_arenas       = std::make_unique<FrameArena[]>(m_num_threads);
    m_frame_arena_count  = m_num_threads;
    for (crd::u32 i = 0U; i < m_num_threads; ++i)
    {
        [[maybe_unused]] const bool ok = m_frame_arenas[i].init(cfg.frame_arena_bytes);
        CRD_ASSERT_MSG(ok, "WorkerPool::init: failed to allocate frame arena");
    }

    // Enroll the calling (main) thread as thread 0.
    tl_idx         = 0U;
    tl_pool_ptr    = this;
    tl_frame_arena = &m_frame_arenas[0];

    // Spawn N-1 background worker threads (thread indices 1..N-1).
    const crd::u32 worker_count = m_num_threads > 1U ? m_num_threads - 1U : 0U;
    m_threads.reserve(worker_count);
    for (crd::u32 i = 1U; i < m_num_threads; ++i)
        m_threads.emplace_back(&WorkerPool::worker_loop, this, i);

    if (m_pcore_routing)
    {
        pin_pool_to_pcores(m_threads, m_num_threads); // ADR-0094; no-op on unknown topology
    }

    m_initialized = true;
    return true;
}

void WorkerPool::shutdown() noexcept
{
    if (!m_initialized)
        return;

    m_stopping.store(true, std::memory_order_release);

    // Wake all sleeping workers so they can observe m_stopping and exit cleanly.
    if (!m_threads.empty())
        m_scheduler.wake_all(static_cast<crd::u32>(m_threads.size()));

    for (auto& t : m_threads)
        if (t.joinable()) t.join();
    m_threads.clear();

    m_counter_pool.shutdown();
    m_fiber_pool.shutdown();
    m_scheduler.shutdown();

    // Destroy all frame arenas (each ~FrameArena() calls free()).
    m_frame_arenas.reset();
    m_frame_arena_count = 0U;

    // Reset the worker count so num_workers() reflects reality after shutdown.
    // A stale positive count here is a landmine: gemm_parallel_auto (and any
    // num_workers()-driven dispatch) would take the parallel path and dispatch
    // parallel_for onto the now-dead scheduler -> SIGSEGV. Threads are already
    // joined above, so no worker can read this concurrently.
    m_num_threads = 0U;

    m_stopping.store(false, std::memory_order_relaxed);
    m_initialized = false;
}

// ---------------------------------------------------------------------------
// WorkerPool — public interface
// ---------------------------------------------------------------------------

void WorkerPool::reset_all_frame_arenas() noexcept
{
    for (crd::u32 i = 0U; i < m_frame_arena_count; ++i)
        m_frame_arenas[i].reset();
}

void WorkerPool::push(const crd::jobs::JobDecl& job)
{
    CRD_ASSERT_MSG(m_initialized, "WorkerPool::push called before init");
    m_scheduler.push(job);
}

void WorkerPool::enqueue_fiber_resume(Fiber* fiber)
{
    CRD_ASSERT_MSG(m_initialized, "WorkerPool::enqueue_fiber_resume called before init");
    CRD_ASSERT_MSG(fiber != nullptr, "WorkerPool::enqueue_fiber_resume: null fiber pointer");

    crd::jobs::JobDecl resume{};
    resume.fn       = &resume_fiber_fn;
    resume.data     = fiber;
    resume.priority = crd::jobs::Priority::High;
    m_scheduler.push(resume);
}

bool WorkerPool::pump()
{
    CRD_ASSERT_MSG(m_initialized, "WorkerPool::pump called before init");

    std::optional<crd::jobs::JobDecl> job = m_scheduler.try_pop(tl_idx);
    if (!job)
        return false;
    run_job_in_fiber(*job);
    return true;
}

} // namespace crd::jobs::detail
