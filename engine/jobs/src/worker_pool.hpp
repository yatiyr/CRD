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
#include <vector>

namespace crd::jobs::detail
{

// Configuration for WorkerPool::init().
struct WorkerConfig
{
    crd::u32 num_threads        = 0u;       // 0 = hardware_concurrency()
    crd::u32 deque_capacity     = 256u;
    crd::u32 injection_capacity = 4096u;
    crd::u32 small_fiber_count  = 128u;
    crd::u32 medium_fiber_count = 64u;
    crd::u32 large_fiber_count  = 16u;
    crd::u32 max_counters       = 512u;
    crd::u32 frame_arena_bytes  = 1u << 20u; // 1 MB per thread
};

// Owns the Scheduler, FiberPool, and CounterPool. Spawns N-1 OS worker threads;
// thread 0 is the calling (main) thread, driven externally via pump().
//
// Job execution model:
//   Every job runs inside a pool fiber rather than directly on the OS thread stack.
//   The OS thread ("scheduler stack") acquires a fiber, writes tl_pending_job (thread-
//   local), switches to the fiber. The trampoline reads tl_pending_job and calls fn(data).
//   After the job returns, the trampoline clears tl_current_fiber (completion signal) and
//   switches back. The scheduler detects completion, resets the fiber's context to its
//   initial state, and returns it to the pool.
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
    [[nodiscard]] bool pump();

    // Reset all per-thread frame arenas to cursor 0. Must only be called when no
    // concurrent frame_alloc() is in flight (i.e. after all jobs of the frame have
    // completed). Maps to the public frame_reset() call.
    void reset_all_frame_arenas() noexcept;

    [[nodiscard]] Scheduler&   scheduler()      noexcept { return m_scheduler; }
    [[nodiscard]] FiberPool&   fiber_pool()     noexcept { return m_fiber_pool; }
    [[nodiscard]] CounterPool& counter_pool()   noexcept { return m_counter_pool; }
    [[nodiscard]] crd::u32     num_threads()    const noexcept { return m_num_threads; }
    [[nodiscard]] bool         is_initialized() const noexcept { return m_initialized; }

private:
    static void worker_loop(WorkerPool* self, crd::u32 thread_index);
    void        run_job_in_fiber(const crd::jobs::JobDecl& job);

    static FiberTier stack_size_to_tier(crd::jobs::StackSize s) noexcept;

    Scheduler    m_scheduler;
    FiberPool    m_fiber_pool;
    CounterPool  m_counter_pool;

    // Per-thread frame arenas. unique_ptr<T[]> avoids vector relocation, which would
    // silently invalidate the tl_frame_arena_ptr thread-locals already set by each thread.
    std::unique_ptr<FrameArena[]> m_frame_arenas;
    crd::u32                      m_frame_arena_count = 0u;

    std::vector<std::thread> m_threads;
    std::atomic<bool>        m_stopping{false};
    crd::u32                 m_num_threads = 0u;
    bool                     m_initialized = false;
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

} // namespace crd::jobs::detail
