#include <crd/jobs/jobs.hpp>
#include "worker_pool.hpp"
#include <crd/core/assert.hpp>

#include <cstring>
#include <thread>

namespace crd::jobs
{

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
static detail::WorkerPool g_pool;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

// ---------------------------------------------------------------------------
// init / shutdown
// ---------------------------------------------------------------------------

void init(const Config& cfg)
{
    detail::WorkerConfig wc;
    wc.num_threads        = cfg.num_threads;
    wc.small_fiber_count  = cfg.small_fiber_count;
    wc.medium_fiber_count = cfg.medium_fiber_count;
    wc.large_fiber_count  = cfg.large_fiber_count;
    wc.max_counters       = cfg.max_counters;
    wc.injection_capacity = cfg.injection_queue_capacity;

    [[maybe_unused]] const bool ok = g_pool.init(wc);
    CRD_ASSERT_MSG(ok, "crd::jobs::init: WorkerPool::init failed");
}

void shutdown()
{
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
    CRD_ASSERT_MSG(c != nullptr, "crd::jobs::run: counter pool exhausted");

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
    return submit_jobs({&job, 1u});
}

// ---------------------------------------------------------------------------
// wait
// ---------------------------------------------------------------------------

void wait(Counter* counter, crd::u32 target)
{
    CRD_ASSERT_MSG(counter != nullptr, "crd::jobs::wait: null counter");

    detail::Fiber* current_fiber = detail::tl_current_fiber_ref();

    if (current_fiber == nullptr)
    {
        // Non-fiber path (e.g. main thread): spin until counter reaches target.
        // Background worker threads must be present to decrement the counter;
        // calling wait() from a non-fiber context with num_threads == 1 deadlocks.
        while (counter->value.load(std::memory_order_acquire) != target)
            std::this_thread::yield();
    }
    else
    {
        detail::FiberContext& sched_ctx = detail::tl_scheduler_context();
        detail::Waiter w;
        detail::counter_wait(counter, &w, current_fiber, sched_ctx, target);
    }

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
// Introspection
// ---------------------------------------------------------------------------

bool is_worker_fiber() noexcept
{
    return detail::tl_current_fiber_ref() != nullptr;
}

crd::u32 worker_index() noexcept
{
    return detail::tl_thread_index();
}

crd::u32 num_workers() noexcept
{
    return g_pool.num_threads();
}

} // namespace crd::jobs
