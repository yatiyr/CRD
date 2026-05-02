#include "worker_pool.hpp"
#include <crd/jobs/detail/fiber_context.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>

#include <cstring>
#include <thread>

namespace crd::jobs::detail
{

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
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

FiberContext& tl_scheduler_context() noexcept { return tl_sched_ctx; }
Fiber*&       tl_current_fiber_ref() noexcept { return tl_fiber; }
crd::u32      tl_thread_index()      noexcept { return tl_idx; }
WorkerPool*   tl_worker_pool()        noexcept { return tl_pool_ptr; }
FrameArena&   tl_frame_arena_ref()    noexcept
{
    CRD_ASSERT_MSG(tl_frame_arena != nullptr,
                   "tl_frame_arena_ref: frame arena not set — call jobs::init() first");
    return *tl_frame_arena;
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
// ---------------------------------------------------------------------------

static void job_fiber_trampoline() noexcept
{
    while (true)
    {
        tl_job_fn(tl_job_data);

        // Decrement the fiber's associated counter (if any) and wake satisfied waiters.
        // Read from the fiber struct so the counter survives fiber suspension + resume.
        Fiber* const done = tl_fiber;
        tl_fiber = nullptr;    // completion signal: run_job_in_fiber checks this
        Counter* const c = done->job_counter;
        done->job_counter = nullptr;
        if (c != nullptr)
        {
            Waiter* woken = counter_decrement(c);
            while (woken != nullptr)
            {
                Waiter* const next = woken->next.load(std::memory_order_relaxed);
                tl_pool_ptr->enqueue_fiber_resume(woken->fiber);
                woken = next;
            }
        }

        fiber_switch(&done->context, &tl_sched_ctx);
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

    if (job.fn == &resume_fiber_fn)
    {
        target = static_cast<Fiber*>(job.data);
        CRD_ASSERT_MSG(target != nullptr,
                       "run_job_in_fiber: resume sentinel carries null fiber pointer");
        // job_counter stays in target->job_counter from when the fiber was first launched;
        // no update needed here — the fiber's counter survives suspension.
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
    }

    tl_fiber = target;
    fiber_switch(&tl_sched_ctx, &target->context);

    // Back on the OS (scheduler) stack.
    if (tl_fiber == nullptr)
    {
        // Job completed — the trampoline already cleared target->job_counter.
        // Rebuild the initial stack frame so the fiber is clean on re-use.
        fiber_init_stack(target->context, target->usable_base, target->usable_size, target->trampoline);
        m_fiber_pool.release(target);
    }
    // else: fiber suspended on a counter; it remains active until enqueue_fiber_resume().
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
            self->m_scheduler.wait_for_work();
        }
    }
}

// ---------------------------------------------------------------------------
// WorkerPool — lifecycle
// ---------------------------------------------------------------------------

bool WorkerPool::init(const WorkerConfig& cfg)
{
    CRD_ASSERT_MSG(!m_initialized, "WorkerPool::init called twice");

    m_num_threads = (cfg.num_threads == 0U)
        ? static_cast<crd::u32>(std::thread::hardware_concurrency())
        : cfg.num_threads;
    if (m_num_threads == 0U)
        m_num_threads = 1U; // hardware_concurrency() returned 0 on this platform

    SchedulerConfig sched_cfg;
    sched_cfg.num_threads        = m_num_threads;
    sched_cfg.deque_capacity     = cfg.deque_capacity;
    sched_cfg.injection_capacity = cfg.injection_capacity;

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
