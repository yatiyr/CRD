#include <crd/jobs/jobs.hpp>
#include "hang_watchdog.hpp" // HangVerdict / hang_verdict / HangDetector (pure classifier)
#include "worker_pool.hpp"
#include <crd/core/assert.hpp>
#include <crd/core/rt_sentinel.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

namespace crd::jobs
{

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
static detail::WorkerPool g_pool;

// --- Hang watchdog (opt-in via Config::hang_watchdog_period_ms). A thread OUTSIDE the pool samples progress
//     each window and, after several stale windows, calls the installed handler once per stall episode. It
//     uses only the public snapshot APIs (progress_snapshot / wait_graph_snapshot / is_quiescent), touches no
//     per-worker/enrollment state, and is started after the pool is up and joined before the pool tears down. ---
static std::atomic<HangHandler> g_hang_handler{nullptr};
static std::atomic<void*>       g_hang_handler_user{nullptr};

// --- Priority-starvation handler (same opt-in / atomic-pair pattern as the hang handler). Checked each window
//     alongside the hang verdict but independently: a starved system still completes other work. ---
static std::atomic<StarvationHandler> g_starvation_handler{nullptr};
static std::atomic<void*>             g_starvation_handler_user{nullptr};

// --- Livelock handler (same opt-in / atomic-pair pattern). Checked each window over the monitored (opted-in
//     via note_progress) tasks; independent of the hang and starvation checks. ---
static std::atomic<LivelockHandler> g_livelock_handler{nullptr};
static std::atomic<void*>           g_livelock_handler_user{nullptr};

static std::mutex               g_watchdog_mutex;
static std::condition_variable  g_watchdog_cv;
static bool                     g_watchdog_stop = false; // guarded by g_watchdog_mutex
static std::thread              g_watchdog_thread;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

namespace
{
void watchdog_loop(crd::u32 period_ms) noexcept
{
    constexpr crd::u32                        kK = 3U; // consecutive stale windows before a report
    detail::HangDetector                      detector{kK};
    detail::StarvationDetector                starv_detector{kK};
    detail::LivelockTracker                   livelock_tracker{kK};
    ProgressSample                            before = progress_snapshot();
    LaneSample                                before_lanes = lane_snapshot();
    std::array<WaitGraphNode, 64>             parked{};         // fixed buffer -- the watchdog never allocates
    std::array<ProgressNode, 64>              monitored{};      // monitored-task snapshot buffer
    std::array<ProgressNode, 64>              livelock_fired{}; // tasks that fire livelock this window

    while (true)
    {
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        {
            std::unique_lock<std::mutex> lock(g_watchdog_mutex);
            g_watchdog_cv.wait_for(lock, std::chrono::milliseconds(period_ms),
                                   [] { return g_watchdog_stop; });
            if (g_watchdog_stop)
                return;
        }
        const auto     t1        = std::chrono::steady_clock::now();
        const crd::u32 actual_ms = static_cast<crd::u32>(
            std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());

        const ProgressSample after       = progress_snapshot();
        const LaneSample      after_lanes = lane_snapshot();
        const detail::HangVerdict v = detail::hang_verdict(before, after, period_ms, actual_ms);
        if (detector.feed(v))
        {
            // Capture parked-fiber evidence and hand a report to the installed handler (if any). The handler
            // may call the snapshot APIs itself, so we hold no lock across the call.
            const crd::usize total = wait_graph_snapshot(parked);
            const crd::usize n     = total < parked.size() ? total : parked.size();

            HangHandler    handler = g_hang_handler.load(std::memory_order_acquire);
            void* const    user    = g_hang_handler_user.load(std::memory_order_relaxed);
            if (handler != nullptr)
            {
                HangReport report{};
                report.stale_windows = detector.stale_windows();
                report.completions   = after.completions;
                report.executing     = after.executing;
                report.outstanding   = after.outstanding;
                report.quiescent     = after.quiescent;
                report.parked        = std::span<const WaitGraphNode>(parked.data(), n);
                report.parked_total  = total;
                report.kind          = detail::classify_hang(report.parked, after.outstanding, after.executing);
                report.exhaustions   = after.exhaustions;
                handler(report, user);
            }
        }

        // Priority starvation -- an independent check: a starved system still completes other work, so the hang
        // verdict above never flags it. Fire the per-lane handler once per episode for each lane that has stayed
        // backlogged-without-progress for kK windows while the rest of the system advanced.
        const crd::u8 starved = detail::starvation_verdict(before_lanes, after_lanes, before.completions,
                                                           after.completions);
        if (const crd::u8 fired = starv_detector.feed(starved); fired != 0U)
        {
            StarvationHandler shandler = g_starvation_handler.load(std::memory_order_acquire);
            void* const       suser    = g_starvation_handler_user.load(std::memory_order_relaxed);
            if (shandler != nullptr)
            {
                for (crd::u32 lane = 0U; lane < 3U; ++lane)
                {
                    if ((fired & static_cast<crd::u8>(1U << lane)) == 0U)
                        continue;
                    StarvationReport sreport{};
                    sreport.lane          = static_cast<crd::u8>(lane);
                    sreport.backlog       = after_lanes.backlog[lane];
                    sreport.stale_windows = starv_detector.stale_windows(lane);
                    sreport.completions   = after.completions;
                    shandler(sreport, suser);
                }
            }
        }

        // Livelock -- another independent check, over the tasks that opted in via note_progress(). Skip the
        // feed when the window read as Paused: a frozen process (debugger/suspend) leaves every epoch flat,
        // which is exactly the false-positive shape. A monitored task that keeps a worker executing but stops
        // ticking for kK windows is spinning without progress.
        if (v != detail::HangVerdict::Paused)
        {
            const crd::usize mon_total = monitored_snapshot(monitored);
            const crd::usize mon_n     = mon_total < monitored.size() ? mon_total : monitored.size();
            const crd::usize fired     = livelock_tracker.feed(
                std::span<const ProgressNode>(monitored.data(), mon_n), livelock_fired);
            if (fired != 0U)
            {
                LivelockHandler lhandler = g_livelock_handler.load(std::memory_order_acquire);
                void* const     luser    = g_livelock_handler_user.load(std::memory_order_relaxed);
                if (lhandler != nullptr)
                {
                    const crd::usize emit = fired < livelock_fired.size() ? fired : livelock_fired.size();
                    for (crd::usize i = 0U; i < emit; ++i)
                    {
                        const ProgressNode& node = livelock_fired[i];
                        LivelockReport      lr{};
                        lr.fiber_index     = node.fiber_index;
                        lr.tier            = node.tier;
                        lr.task_id         = node.task_id;
                        lr.parent_task_id  = node.parent_task_id;
                        lr.progress_epoch  = node.progress_epoch;
                        lr.stale_windows   = livelock_tracker.stale_windows(node.task_id);
                        lr.completions     = after.completions;
                        lr.executing       = after.executing;
                        lr.monitored_total = mon_total;
                        lhandler(lr, luser);
                    }
                }
            }
        }

        before       = after;
        before_lanes = after_lanes;
    }
}
} // namespace

// ---------------------------------------------------------------------------
// init / shutdown
// ---------------------------------------------------------------------------

void set_hang_handler(HangHandler handler, void* user) noexcept
{
    g_hang_handler_user.store(user, std::memory_order_relaxed);
    g_hang_handler.store(handler, std::memory_order_release);
}

void set_starvation_handler(StarvationHandler handler, void* user) noexcept
{
    g_starvation_handler_user.store(user, std::memory_order_relaxed);
    g_starvation_handler.store(handler, std::memory_order_release);
}

void set_livelock_handler(LivelockHandler handler, void* user) noexcept
{
    g_livelock_handler_user.store(user, std::memory_order_relaxed);
    g_livelock_handler.store(handler, std::memory_order_release);
}

void init(const Config& cfg)
{
    detail::WorkerConfig wc;
    wc.num_threads        = cfg.num_threads;
    wc.small_fiber_count  = cfg.small_fiber_count;
    wc.medium_fiber_count = cfg.medium_fiber_count;
    wc.large_fiber_count  = cfg.large_fiber_count;
    wc.max_counters       = cfg.max_counters;
    wc.injection_capacity = cfg.injection_queue_capacity;
    wc.frame_arena_bytes  = cfg.frame_alloc_bytes;
    wc.pcore_routing      = cfg.pcore_routing;

    [[maybe_unused]] const bool ok = g_pool.init(wc);
    CRD_ASSERT_MSG(ok, "crd::jobs::init: WorkerPool::init failed");

    // Start the hang watchdog only after the pool is fully up (it samples pool state). Opt-in: period 0 = off.
    if (cfg.hang_watchdog_period_ms > 0U)
    {
        {
            std::lock_guard<std::mutex> lock(g_watchdog_mutex);
            g_watchdog_stop = false;
        }
        g_watchdog_thread = std::thread(&watchdog_loop, cfg.hang_watchdog_period_ms);
    }
}

void shutdown()
{
    // Stop and JOIN the watchdog before tearing the pool down: it may be mid-snapshot walking the fiber pool,
    // and WorkerPool::is_initialized only flips false at the very end of its shutdown, so the snapshot's own
    // guard would not protect against a concurrent teardown. Join first, always.
    if (g_watchdog_thread.joinable())
    {
        {
            std::lock_guard<std::mutex> lock(g_watchdog_mutex);
            g_watchdog_stop = true;
        }
        g_watchdog_cv.notify_all();
        g_watchdog_thread.join();
    }

    g_pool.shutdown();
}

// ---------------------------------------------------------------------------
// run
// ---------------------------------------------------------------------------

static detail::Counter* submit_jobs(std::span<const JobDecl> jobs)
{
    CRD_ASSERT_MSG(!jobs.empty(), "crd::jobs::run: empty job span");
    CRD_ASSERT_MSG(g_pool.is_initialized(), "crd::jobs::run: call init() first");

    detail::Counter* c = g_pool.counter_pool().acquire(static_cast<crd::u32>(jobs.size()));
    // Always-on guard (CRD_ASSERT_MSG is a no-op in Release): acquire() already fatals on exhaustion, but never
    // pack a null counter into jobs -- that would crash later on completion. Defense in depth for the fail path.
    if (c == nullptr)
        CRD_FATAL("crd::jobs::run: counter pool exhausted — raise max_counters in jobs::Config");

    for (const JobDecl& src : jobs)
    {
        JobDecl j = src;
        detail::Counter* cp = c;
        std::memcpy(&j._pad[0], &cp, sizeof(cp));
        g_pool.push(j);
    }

    return c;
}

Counter* run(std::span<const JobDecl> jobs)
{
    return submit_jobs(jobs);
}

Counter* run(const JobDecl& job)
{
    return submit_jobs({&job, 1U});
}

// ---------------------------------------------------------------------------
// wait
// ---------------------------------------------------------------------------

void wait(Counter* counter, crd::u32 target)
{
    CRD_ASSERT_MSG(counter != nullptr, "crd::jobs::wait: null counter");

#if CRD_ENABLE_ASSERTS
    // Real-time sentinel: blocking inside a declared RtScope is forbidden -- report it (then proceed; a sentinel
    // never changes behaviour). Reported at the entry, before the fast path, so even a wait that returns
    // immediately still records the forbidden call. Zero-cost in shipping builds.
    if (crd::in_rt_scope())
        crd::report_rt_violation(crd::RtViolationKind::Blocking, 0U);
#endif

    detail::Fiber* current_fiber = detail::tl_current_fiber_ref();

    if (current_fiber == nullptr)
    {
        // Non-fiber spin path.
        // If we are on thread 0 (main/enrolled), call pump() each iteration so
        // thread-0-pinned jobs can make progress even with a single-thread pool.
        // For any other unenrolled thread just yield — background workers must exist.
        const bool is_main = (detail::tl_worker_pool() == &g_pool)
                              && (detail::tl_thread_index() == 0U);
        // No-blocking-context contract: an unenrolled thread (not thread 0, not a worker) waiting on a
        // single-thread pool is a guaranteed deadlock — only thread 0 could run the job, and it is not
        // pumping. Fail fast instead of hanging. (num_threads >= 2 has a background worker to decrement;
        // a wait on a counter whose jobs are all thread-0-pinned still deadlocks, but that affinity is
        // not visible here — it is the caller's guarantee, documented on wait().)
        if (!is_main && detail::tl_worker_pool() == nullptr && g_pool.num_threads() == 1U)
        {
            CRD_FATAL("crd::jobs::wait: an unenrolled thread cannot wait on a single-thread pool "
                      "(num_threads == 1) -- nothing can decrement the counter, so this would deadlock. "
                      "Wait from thread 0, enroll the thread, or configure num_threads >= 2.");
        }
        while (counter->value.load(std::memory_order_acquire) != target)
        {
            if (is_main && g_pool.pump())
                continue;
            std::this_thread::yield();
        }
    }
    else
    {
        detail::FiberContext& sched_ctx = detail::tl_scheduler_context();
        detail::Waiter w;
        detail::counter_wait(counter, &w, current_fiber, sched_ctx, target);
    }

    // Gate release on the zero-decrement having finished draining the waiters list. value == target only means
    // the fetch_sub that hit zero landed; the same decrement's waiters.exchange (its last touch of this
    // Counter) may still be pending. Releasing here would recycle the slot under that exchange, which would
    // then steal the next generation's waiters. counter_decrement sets `drained` right after the exchange, a
    // few instructions away, so this almost never actually spins (mirrors counter_wait's park_finalized spin).
    while (!counter->drained.load(std::memory_order_acquire))
    { /* counter_decrement's exchange is a handful of instructions away */ }

    g_pool.counter_pool().release(counter);
}

// ---------------------------------------------------------------------------
// run_and_wait
// ---------------------------------------------------------------------------

void run_and_wait(std::span<const JobDecl> jobs)
{
    wait(run(jobs));
}

void run_and_wait(const JobDecl& job)
{
    wait(run(job));
}

// ---------------------------------------------------------------------------
// Main-thread pump
// ---------------------------------------------------------------------------

bool pump_main_thread_once()
{
    CRD_ASSERT_MSG(g_pool.is_initialized(),
                   "crd::jobs::pump_main_thread_once: call init() first");
    CRD_ASSERT_MSG((detail::tl_worker_pool() == &g_pool) && (detail::tl_thread_index() == 0U),
                   "pump_main_thread_once: must be called from the enrolled main thread (thread 0)");
    return g_pool.pump();
}

bool pump_main_thread_until_idle()
{
    CRD_ASSERT_MSG(g_pool.is_initialized(),
                   "crd::jobs::pump_main_thread_until_idle: call init() first");
    CRD_ASSERT_MSG((detail::tl_worker_pool() == &g_pool) && (detail::tl_thread_index() == 0U),
                   "pump_main_thread_until_idle: must be called from the enrolled main thread (thread 0)");
    bool did_work = false;
    while (g_pool.pump())
        did_work = true;
    return did_work;
}

// ---------------------------------------------------------------------------
// Frame allocator
// ---------------------------------------------------------------------------

void* frame_alloc(crd::usize size, crd::usize alignment)
{
    CRD_ASSERT_MSG(g_pool.is_initialized(), "crd::jobs::frame_alloc: call init() first");
    return detail::tl_frame_arena_ref().alloc(size, alignment);
}

void frame_reset()
{
    CRD_ASSERT_MSG(g_pool.is_initialized(), "crd::jobs::frame_reset: call init() first");
    g_pool.reset_all_frame_arenas();
}

crd::usize frame_get_mark()
{
    CRD_ASSERT_MSG(g_pool.is_initialized(), "crd::jobs::frame_get_mark: call init() first");
    return detail::tl_frame_arena_ref().cursor();
}

void frame_set_mark(crd::usize mark)
{
    CRD_ASSERT_MSG(g_pool.is_initialized(), "crd::jobs::frame_set_mark: call init() first");
    detail::tl_frame_arena_ref().set_cursor(mark);
}

// ---------------------------------------------------------------------------
// Introspection
// ---------------------------------------------------------------------------

bool is_worker_fiber() noexcept
{
    return detail::tl_current_fiber_ref() != nullptr;
}

crd::u64 current_task_id() noexcept
{
    // The running fiber carries its job's Counter (fiber->job_counter), which survives suspension and
    // resume on any OS thread — so reading the id off the current fiber is migration-safe and needs no
    // separate thread-local. A nested job runs on its own fiber with its own counter; when the parent
    // fiber resumes, tl_current_fiber_ref() points back to it, so the parent's id reappears for free.
    const detail::Fiber* const f = detail::tl_current_fiber_ref();
    if (f == nullptr)
        return 0U; // off-fiber caller (main thread, or outside any job)
    const detail::Counter* const c = f->job_counter;
    return (c != nullptr) ? c->task_id : 0U;
}

crd::u64 parent_task_id() noexcept
{
    const detail::Fiber* const f = detail::tl_current_fiber_ref();
    if (f == nullptr)
        return 0U; // off-fiber caller (main thread, or outside any job)
    const detail::Counter* const c = f->job_counter;
    return (c != nullptr) ? c->parent_task_id : 0U;
}

void note_progress() noexcept
{
    detail::Fiber* const f = detail::tl_current_fiber_ref();
    if (f == nullptr)
        return; // off-fiber caller: nothing to instrument
    // Opt in (first call moves the epoch off 0) and record forward progress. Relaxed: only this fiber writes it,
    // and the watchdog reads it racily as a diagnostic sample.
    f->progress_epoch.fetch_add(1U, std::memory_order_relaxed);
}

bool is_quiescent() noexcept
{
    // available() is capacity() - acquired counters (a cheap counter read, not a free-list walk).
    // Before init() the pool reports uninitialised, which is trivially quiescent.
    auto& cp = g_pool.counter_pool();
    return !cp.is_initialized() || (cp.available() == cp.capacity());
}

crd::usize wait_graph_snapshot(std::span<WaitGraphNode> out) noexcept
{
    if (!g_pool.is_initialized())
        return 0U;

    crd::usize total = 0U;
    g_pool.fiber_pool().for_each_fiber(
        [&](const detail::Fiber& f, detail::FiberTier tier) noexcept
        {
            // Relaxed racy read: a diagnostics dump, not an oracle (see the header contract).
            const detail::Counter* const c = f.waiting_on.load(std::memory_order_relaxed);
            if (c == nullptr)
                return; // not parked

            const crd::usize idx = total++;
            if (idx < out.size())
            {
                // The fiber's OWN job counter (the edge source). Plain read of a non-atomic pointer, matching
                // current_task_id()'s established racy-dump pattern -- a parked fiber does not rewrite it; 0 if
                // this fiber carries no job counter (an unresolved chain end for cycle detection).
                const detail::Counter* const own = f.job_counter;

                WaitGraphNode& n            = out[idx];
                n.fiber_index               = f.pool_index;
                n.tier                      = static_cast<crd::u8>(tier);
                n.waiting_on_task_id        = c->task_id;
                n.waiting_on_parent_task_id = c->parent_task_id;
                n.waiting_on_remaining      = c->value.load(std::memory_order_relaxed);
                n.own_task_id               = (own != nullptr) ? own->task_id : 0U;
            }
        });
    return total;
}

crd::usize monitored_snapshot(std::span<ProgressNode> out) noexcept
{
    if (!g_pool.is_initialized())
        return 0U;

    crd::usize total = 0U;
    g_pool.fiber_pool().for_each_fiber(
        [&](const detail::Fiber& f, detail::FiberTier tier) noexcept
        {
            // Monitored == opted in (epoch > 0). A parked monitored task is the hang detector's domain, not
            // livelock, so exclude waiting_on != nullptr. Relaxed racy reads: a diagnostics dump, not an oracle.
            if (f.progress_epoch.load(std::memory_order_relaxed) == 0U)
                return;
            if (f.waiting_on.load(std::memory_order_relaxed) != nullptr)
                return;

            const crd::usize idx = total++;
            if (idx < out.size())
            {
                const detail::Counter* const c = f.job_counter;
                ProgressNode&                n = out[idx];
                n.fiber_index                  = f.pool_index;
                n.tier                         = static_cast<crd::u8>(tier);
                n.task_id                      = (c != nullptr) ? c->task_id : 0U;
                n.parent_task_id               = (c != nullptr) ? c->parent_task_id : 0U;
                n.progress_epoch               = f.progress_epoch.load(std::memory_order_relaxed);
            }
        });
    return total;
}

WorkerSnapshotResult worker_snapshot(std::span<WorkerNode> out, crd::u32 timeout_ms) noexcept
{
    WorkerSnapshotResult r{0U, 0U, 0U, true};
    if (!g_pool.is_initialized())
        return r; // no workers -> vacuously complete

    const crd::u32 n = g_pool.num_threads();
    // If a background worker itself calls this, it must not be polled for its own ack (it is busy here, not at
    // its loop top). The main/pump thread (index 0) and any external caller are not loop workers either.
    const crd::u32 self =
        (detail::tl_worker_pool() == &g_pool) ? detail::tl_thread_index() : 0xFFFFFFFFU;

    g_pool.request_snapshot();
    const crd::u32 gen = g_pool.snapshot_gen();

    crd::usize expected = 0U;
    for (crd::u32 i = 1U; i < n; ++i)
        if (i != self)
            ++expected;

    // Bounded poll: return as soon as every expected worker has acked, or when the timeout elapses.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (true)
    {
        crd::usize acked = 0U;
        for (crd::u32 i = 1U; i < n; ++i)
            if (i != self && g_pool.worker_ack_gen(i) == gen)
                ++acked;
        if (acked >= expected || std::chrono::steady_clock::now() >= deadline)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    crd::usize responded = 0U;
    for (crd::u32 i = 0U; i < n; ++i)
    {
        // Thread 0 and the calling worker are not polled loop workers -> reported responsive from the dump.
        const bool responsive = (i == 0U || i == self) ? true : (g_pool.worker_ack_gen(i) == gen);
        if (i >= 1U && i != self && responsive)
            ++responded;
        if (i < out.size())
        {
            WorkerNode& node    = out[i];
            node.thread_index   = i;
            node.current_task_id = g_pool.worker_task(i);
            node.executing      = g_pool.worker_executing(i);
            node.responsive     = responsive;
        }
    }

    r.total     = n;
    r.expected  = expected;
    r.responded = responded;
    r.complete  = (responded == expected);
    return r;
}

ProgressSample progress_snapshot() noexcept
{
    ProgressSample s{0U, 0U, 0U, true};
    if (!g_pool.is_initialized())
        return s;
    g_pool.progress_counts(s.completions, s.executing);
    // outstanding = counters with value > 0 (jobs still to finish). A completed-but-unwaited counter sits at 0
    // and is correctly NOT counted, so this is the true "work in flight" signal (unlike quiescent, which stays
    // false until the counter is released). Relaxed racy walk on the watchdog's period, not the hot path.
    crd::u32 outstanding = 0U;
    g_pool.counter_pool().for_each_counter(
        [&outstanding](const detail::Counter& c) noexcept
        {
            if (c.value.load(std::memory_order_relaxed) > 0U)
                ++outstanding;
        });
    s.outstanding = outstanding;
    s.quiescent   = is_quiescent();
    s.exhaustions = g_pool.counter_pool().exhaustions() + g_pool.fiber_pool().exhaustions();
    return s;
}

LaneSample lane_snapshot() noexcept
{
    LaneSample s{};
    if (!g_pool.is_initialized())
        return s;
    g_pool.scheduler().injection_diagnostics(s.backlog, s.pops);
    return s;
}

crd::u32 worker_index() noexcept
{
    return detail::tl_thread_index();
}

crd::u32 num_workers() noexcept
{
    return g_pool.num_threads();
}

bool is_pcore_routing() noexcept
{
    return g_pool.is_pcore_routing();
}

} // namespace crd::jobs
