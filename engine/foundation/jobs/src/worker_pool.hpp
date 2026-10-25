#pragma once

#include "counter.hpp"
#include "fiber_pool.hpp"
#include "frame_arena.hpp"
#include "scheduler.hpp"
#include <crd/jobs/job_decl.hpp>
#include <crd/core/types.hpp>

#include <atomic>
#include <memory>
#include <thread>
#include <crd/containers/array.hpp>

namespace crd::jobs::detail
{

// Configuration for WorkerPool::init().
struct WorkerConfig
{
    crd::u32 num_threads        = 0U;       // 0 = hardware_concurrency()
    crd::u32 deque_capacity     = 256U;
    crd::u32 injection_capacity = 4096U;
    crd::u32 pinned_capacity    = 64U;      // per-thread pinned lane (DIAG.10c)
    crd::u32 small_fiber_count  = 128U;
    crd::u32 medium_fiber_count = 64U;
    crd::u32 large_fiber_count  = 16U;
    crd::u32 max_counters       = 512U;
    crd::u32 frame_arena_bytes  = 1U << 20U; // 1 MB per thread
    bool     pcore_routing      = false;     // ADR-0094 opt-in (targeted wake + P-core affinity)
};

// Per-worker liveness signal for the hang watchdog (DIAG diagnostics). One slot per thread, each on its own
// cache line so a worker touching its own slot never invalidates a peer's. `completions` counts jobs this
// worker finished (a relaxed bump in the trampoline); `executing` is 1 while this worker is inside
// run_job_in_fiber and 0 otherwise. A watchdog samples the aggregate: no completion progress AND no worker
// executing while work is outstanding is the deadlock shape; any worker executing means a long job is
// progressing, not hung.
#if CRD_COMPILER_MSVC
#pragma warning(push)
#pragma warning(disable : 4324) // structure padded due to alignment specifier -- intentional (own cache line)
#endif
struct alignas(64) WorkerProgress
{
    std::atomic<crd::u64> completions{0U};
    std::atomic<crd::u8>  executing{0U};
    // Cooperative-snapshot state. current_task_id: the task this worker is dispatching (0 when idle); set/cleared
    // around run_job_in_fiber, mirroring `executing`. ack_gen: the latest snapshot generation this worker has
    // observed at its loop-top safe point -- a worker stuck in one job never updates it, which is exactly the
    // "unresponsive" signal for worker_snapshot().
    std::atomic<crd::u64> current_task_id{0U};
    std::atomic<crd::u32> ack_gen{0U};
};

// A thread's kept fiber (JOBS-HARD.3): the fiber of the last job it completed, still acquired from the FiberPool,
// rebuilt and reused for the thread's next new job of the same tier, so a run of tiny jobs on one worker touches
// the pool's shared free list once instead of twice a job. At most one per thread, read and written only by the
// thread whose index it has (and by shutdown() after the workers are joined), so it is a plain pointer. It is
// returned to the pool at every other transition: before a resume, before a job of another tier, before the thread
// sleeps or leaves its dispatch loop, and when the main thread's wait() or pump returns. A kept fiber stays counted
// as acquired, so the pool's readings are exact: no other thread can take it, and a refusal still needs every
// other fiber running, parked or kept, which is what the pool's sizing (parked + threads) already covers.
struct alignas(64) KeptFiber
{
    Fiber* fiber = nullptr;
};
#if CRD_COMPILER_MSVC
#pragma warning(pop)
#endif

// Owns the Scheduler, FiberPool, and CounterPool. Spawns N-1 OS worker threads;
// thread 0 is the calling (main) thread, driven externally via pump().
//
// Job execution model:
//   Every job runs inside a pool fiber rather than directly on the OS thread stack.
//   The OS thread ("scheduler stack") acquires a fiber, writes tl_pending_job (thread-
//   local), switches to the fiber. The trampoline reads tl_pending_job and calls fn(data).
//   After the job returns, the trampoline clears tl_current_fiber (completion signal) and
//   switches back. The scheduler detects completion, resets the fiber's context to its
//   initial state, and keeps it for the thread's next new job of the same tier (KeptFiber),
//   or returns it to the pool when the thread already keeps one.
//
// Suspension (counter_wait):
//   If fn(data) calls counter_wait, the fiber switches back to the scheduler with
//   tl_current_fiber still set (non-null). The scheduler leaves the fiber alive.
//   When the counter is satisfied, counter_decrement wakes the fiber; the caller
//   passes the woken Fiber* to enqueue_fiber_resume(), which pushes a synthetic
//   High-priority resume job. The scheduler picks it up and switches directly back
//   to the suspended fiber's saved context.
//
// Thread safety:
//   push() is fully thread-safe (delegates to Scheduler::push).
//   pump() must only be called from thread 0.
class WorkerPool
{
public:
    WorkerPool()  = default;
    ~WorkerPool() { shutdown(); }

    WorkerPool(const WorkerPool&)            = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;
    WorkerPool(WorkerPool&&)                 = delete;
    WorkerPool& operator=(WorkerPool&&)      = delete;

    // Initialise subsystems and spawn N-1 background worker threads.
    // Must be called from thread 0 (the main thread). Returns true on success.
    [[nodiscard]] bool init(const WorkerConfig& cfg = {});

    // Signal all workers to stop, join threads, and tear down subsystems.
    void shutdown() noexcept;

    // Inject a job into the priority scheduler (thread-safe, lock-free).
    void push(const crd::jobs::JobDecl& job);

    // Re-queue a suspended fiber as a High-priority job.
    // Called from the counter_decrement wakeup path after a counter reaches its target.
    void enqueue_fiber_resume(Fiber* fiber);

    // Run one drain-execute iteration on the calling thread (thread 0 only).
    // Returns true if a job was found and executed; false if all queues were empty.
    // The thread keeps no fiber after it returns.
    [[nodiscard]] bool pump();

    // pump() for a loop of iterations (wait(), pump_main_thread_until_idle()): the fiber of a completed job stays
    // kept for the next iteration, and is returned to the pool when an iteration finds no job. A caller that stops
    // iterating before that returns it with release_kept_fiber().
    [[nodiscard]] bool pump_keeping();

    // Return the calling thread's kept fiber, if any, to the pool. `thread_index` is the calling thread's index.
    void release_kept_fiber(crd::u32 thread_index) noexcept;

    // Reset all per-thread frame arenas to cursor 0. Must only be called when no
    // concurrent frame_alloc() is in flight (i.e. after all jobs of the frame have
    // completed). Maps to the public frame_reset() call.
    void reset_all_frame_arenas() noexcept;

    [[nodiscard]] Scheduler&   scheduler()      noexcept { return m_scheduler; }
    [[nodiscard]] FiberPool&   fiber_pool()     noexcept { return m_fiber_pool; }
    [[nodiscard]] CounterPool& counter_pool()   noexcept { return m_counter_pool; }
    [[nodiscard]] crd::u32     num_threads()    const noexcept { return m_num_threads; }
    [[nodiscard]] bool         is_initialized() const noexcept { return m_initialized; }
    [[nodiscard]] bool         is_pcore_routing() const noexcept { return m_pcore_routing; }

    // Hang-watchdog progress signals. note_job_completed bumps the calling worker's completion count (called
    // from the trampoline, which is a free function and reaches the pool via tl_worker_pool()). progress_counts
    // sums completions across workers and counts how many are currently executing a job — a racy aggregate for
    // a diagnostic sample, never an oracle.
    void note_job_completed(crd::u32 thread_index) noexcept
    {
        m_progress[thread_index].completions.fetch_add(1U, std::memory_order_relaxed); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    }
    void progress_counts(crd::u64& completions_out, crd::u32& executing_out) const noexcept;

    // Cooperative worker snapshot (DIAG safe stop/snapshot). request_snapshot() bumps the generation and wakes
    // every sleeper so idle workers revisit their loop-top safe point and acknowledge; a worker stuck inside one
    // job never revisits it, so its ack stays behind -- the honest "unresponsive" signal. The reads are racy
    // point-in-time diagnostics, never oracles. Building the public WorkerNode/result lives in jobs.cpp so this
    // internal header stays free of the public API types.
    void       request_snapshot() noexcept;
    // Repeated while the snapshot polls: a worker that was about to sleep when the request's broadcast landed can
    // sleep through it (semaphore.hpp), and this tokenless wake brings it back to its safe point.
    void       wake_idle_workers() noexcept { m_scheduler.wake_sleepers(); }
    [[nodiscard]] crd::u32 snapshot_gen()             const noexcept { return m_snapshot_gen.load(std::memory_order_acquire); }
    [[nodiscard]] crd::u32 worker_ack_gen(crd::u32 i) const noexcept { return m_progress[i].ack_gen.load(std::memory_order_acquire); }        // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    [[nodiscard]] crd::u64 worker_task(crd::u32 i)    const noexcept { return m_progress[i].current_task_id.load(std::memory_order_relaxed); } // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    [[nodiscard]] bool     worker_executing(crd::u32 i) const noexcept { return m_progress[i].executing.load(std::memory_order_relaxed) != 0U; } // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)

private:
    static void worker_loop(WorkerPool* self, crd::u32 thread_index);
    void        run_job_in_fiber(const crd::jobs::JobDecl& job);

    static FiberTier stack_size_to_tier(crd::jobs::StackSize s) noexcept;

    // The kept fiber of `thread_index` when it is of `tier` (the slot is emptied); a kept fiber of another tier is
    // returned to the pool and nullptr is the answer.
    [[nodiscard]] Fiber* take_kept_fiber(crd::u32 thread_index, FiberTier tier) noexcept;
    // Keep `fiber` (its job completed, its stack rebuilt) for the thread's next job, or return it to the pool when
    // the thread already keeps one.
    void keep_fiber(crd::u32 thread_index, Fiber* fiber) noexcept;

    Scheduler    m_scheduler;
    FiberPool    m_fiber_pool;
    CounterPool  m_counter_pool;

    // Per-thread frame arenas. unique_ptr<T[]> avoids vector relocation, which would
    // silently invalidate the tl_frame_arena_ptr thread-locals already set by each thread.
    std::unique_ptr<FrameArena[]> m_frame_arenas;
    crd::u32                      m_frame_arena_count = 0U;

    // Per-worker hang-watchdog progress slots, indexed by thread_index (sized to m_num_threads in init()).
    std::unique_ptr<WorkerProgress[]> m_progress;

    // Per-thread kept fibers, indexed by thread_index (sized to m_num_threads in init()).
    std::unique_ptr<KeptFiber[]> m_kept;

    crd::containers::Array<std::thread> m_threads;
    std::atomic<bool>        m_stopping{false};
    std::atomic<crd::u32>    m_snapshot_gen{0U}; // bumped by request_snapshot(); workers echo it into ack_gen
    crd::u32                 m_num_threads = 0U;
    bool                     m_initialized = false;
    bool                     m_pcore_routing = false; // ADR-0094 opt-in (Config::pcore_routing)
};

// ---------------------------------------------------------------------------
// Thread-local accessors
// Used by counter_wait, the v1h public wait() API, and debug introspection.
// Valid only from thread contexts that called WorkerPool::init() or that are
// running inside the worker_loop (i.e., all threads in the pool).
// ---------------------------------------------------------------------------

[[nodiscard]] FiberContext& tl_scheduler_context() noexcept;
[[nodiscard]] Fiber*&       tl_current_fiber_ref() noexcept;
[[nodiscard]] crd::u32      tl_thread_index()      noexcept;
[[nodiscard]] WorkerPool*   tl_worker_pool()        noexcept;
[[nodiscard]] FrameArena&   tl_frame_arena_ref()    noexcept;

// ---------------------------------------------------------------------------
// Park-request handoff (scheduler side)
//
// counter_wait() (in counter.cpp) runs on the waiting fiber; it cannot publish
// its Waiter itself — the fiber isn't parked until it has switched to the
// scheduler, which is what saves its context. So it stashes the Waiter via
// tl_set_pending_park (declared in counter.hpp) and switches; the scheduler
// (run_job_in_fiber), running on the *same* OS thread, picks it up after the
// switch returns and calls counter_finish_park(). One thread only — a plain
// thread_local suffices.
// ---------------------------------------------------------------------------

struct PendingPark
{
    Counter* counter = nullptr;
    Waiter*  waiter  = nullptr;
};

[[nodiscard]] PendingPark tl_take_pending_park() noexcept; // scheduler consumes (clears the slot)

} // namespace crd::jobs::detail
