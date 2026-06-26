#include "scheduler.hpp"
#include <crd/jobs/job_decl.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>

#include <bit>
#include <chrono>
#include <optional>

namespace crd::jobs::detail
{

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool Scheduler::init(const SchedulerConfig& cfg)
{
    CRD_ASSERT_MSG(!m_initialized, "Scheduler::init called twice");
    CRD_ASSERT_MSG(cfg.num_threads >= 1U,
                   "Scheduler: num_threads must be >= 1");
    CRD_ASSERT_MSG(cfg.deque_capacity >= 2U,
                   "Scheduler: deque_capacity must be >= 2");
    CRD_ASSERT_MSG((cfg.deque_capacity & (cfg.deque_capacity - 1U)) == 0U,
                   "Scheduler: deque_capacity must be a power of two");
    CRD_ASSERT_MSG(cfg.injection_capacity >= 2U,
                   "Scheduler: injection_capacity must be >= 2");
    CRD_ASSERT_MSG((cfg.injection_capacity & (cfg.injection_capacity - 1U)) == 0U,
                   "Scheduler: injection_capacity must be a power of two");

    m_config = cfg;
    m_targeted_wake = cfg.targeted_wake;
    CRD_ASSERT_MSG(!m_targeted_wake || cfg.num_threads <= 64U,
                   "Scheduler: targeted_wake supports up to 64 workers (idle mask is 64-bit)");
    m_idle_mask.store(0U, std::memory_order_relaxed);

    m_high_injection   = std::make_unique<JobInjectionQueue>(cfg.injection_capacity);
    m_normal_injection = std::make_unique<JobInjectionQueue>(cfg.injection_capacity);
    m_low_injection    = std::make_unique<JobInjectionQueue>(cfg.injection_capacity);

    m_thread_states.reserve(cfg.num_threads);
    for (crd::u32 i = 0U; i < cfg.num_threads; ++i)
    {
        m_thread_states.push_back(
            std::make_unique<ThreadState>(cfg.deque_capacity, i));
    }

    m_initialized = true;
    return true;
}

void Scheduler::shutdown() noexcept
{
    if (!m_initialized)
        return;

    m_thread_states.clear();
    m_low_injection.reset();
    m_normal_injection.reset();
    m_high_injection.reset();
    m_initialized = false;
}

// ---------------------------------------------------------------------------
// push
//
// Pinned jobs bypass the injection queues and land directly in the target
// thread's single-slot pinned lane. The slot must be empty at the time of
// push (asserted in debug; design invariant — pinned jobs are infrequent).
// ---------------------------------------------------------------------------

void Scheduler::push(const crd::jobs::JobDecl& job)
{
    CRD_ASSERT_MSG(m_initialized, "Scheduler::push called before init");

    if (job.pin_thread >= 0)
    {
        const auto pin = static_cast<crd::u32>(job.pin_thread);
        CRD_ASSERT_MSG(pin < m_config.num_threads,
                       "Scheduler::push: pin_thread out of range");
        ThreadState& ts = *m_thread_states[pin]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        CRD_ASSERT_MSG(!ts.pinned_available.load(std::memory_order_acquire),
                       "Scheduler::push: pinned slot already occupied");
        ts.pinned_storage = job;
        ts.pinned_available.store(true, std::memory_order_release);
        if (m_targeted_wake)
        {
            wake_worker(pin); // wake the SPECIFIC target (the shared semaphore couldn't)
        }
        else
        {
            m_semaphore.release(1);
        }
        return;
    }

    [[maybe_unused]] bool ok = false;
    switch (job.priority)
    {
    case crd::jobs::Priority::High:
        ok = m_high_injection->try_push(job);
        break;
    case crd::jobs::Priority::Normal:
        ok = m_normal_injection->try_push(job);
        break;
    case crd::jobs::Priority::Low:
        ok = m_low_injection->try_push(job);
        break;
    }
    CRD_ASSERT_MSG(ok, "Scheduler::push: injection queue full — raise injection_capacity");
    if (m_targeted_wake)
    {
        wake_one_idle();
    }
    else
    {
        m_semaphore.release(1);
    }
}

// ---------------------------------------------------------------------------
// push_local
//
// Places a job directly onto the calling thread's local deque, bypassing the
// shared injection queues. Intended for child jobs spawned by a running job.
// ---------------------------------------------------------------------------

void Scheduler::push_local(crd::u32 thread_index, const crd::jobs::JobDecl& job)
{
    CRD_ASSERT_MSG(m_initialized, "Scheduler::push_local called before init");
    CRD_ASSERT_MSG(thread_index < m_config.num_threads,
                   "Scheduler::push_local: thread_index out of range");

    ThreadState& ts = *m_thread_states[thread_index]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    [[maybe_unused]] bool ok = false;
    switch (job.priority)
    {
    case crd::jobs::Priority::High:   ok = ts.high.push(job);   break;
    case crd::jobs::Priority::Normal: ok = ts.normal.push(job); break;
    case crd::jobs::Priority::Low:    ok = ts.low.push(job);    break;
    }
    CRD_ASSERT_MSG(ok, "Scheduler::push_local: local deque full — raise deque_capacity");
    if (m_targeted_wake)
    {
        wake_one_idle();
    }
    else
    {
        m_semaphore.release(1);
    }
}

// ---------------------------------------------------------------------------
// execute_one
//
// Drain order per call:
//   1. Pinned slot (if occupied for this thread)
//   2. High: injection → local → steal from peers
//   3. Normal: injection → local → steal from peers
//   4. Low: injection → local → steal from peers
//   5. Return false — no work found
//
// Peer steal direction: round-robin starting from (thread_idx + 1) % num_threads.
// Deterministic ordering is intentional — it simplifies test verification.
// ---------------------------------------------------------------------------

bool Scheduler::execute_one(crd::u32 thread_idx)
{
    CRD_ASSERT_MSG(m_initialized, "Scheduler::execute_one called before init");
    CRD_ASSERT_MSG(thread_idx < m_config.num_threads,
                   "Scheduler::execute_one: thread_idx out of range");

    ThreadState& me = *m_thread_states[thread_idx]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)

    // 1. Pinned slot — highest priority override.
    if (me.pinned_available.load(std::memory_order_acquire))
    {
        const crd::jobs::JobDecl job = me.pinned_storage;
        me.pinned_available.store(false, std::memory_order_release);
        run_job(job);
        return true;
    }

    // 2. High priority: injection → local → steal.
    {
        crd::jobs::JobDecl job{};
        if (m_high_injection->try_pop(job))  { run_job(job); return true; }
        if (auto opt = me.high.pop())        { run_job(*opt); return true; }
        for (crd::u32 i = 1U; i < m_config.num_threads; ++i)
        {
            const crd::u32 peer = (thread_idx + i) % m_config.num_threads;
            if (auto opt = m_thread_states[peer]->high.steal()) // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            {
                run_job(*opt);
                return true;
            }
        }
    }

    // 3. Normal priority: injection → local → steal.
    {
        crd::jobs::JobDecl job{};
        if (m_normal_injection->try_pop(job)) { run_job(job); return true; }
        if (auto opt = me.normal.pop())       { run_job(*opt); return true; }
        for (crd::u32 i = 1U; i < m_config.num_threads; ++i)
        {
            const crd::u32 peer = (thread_idx + i) % m_config.num_threads;
            if (auto opt = m_thread_states[peer]->normal.steal()) // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            {
                run_job(*opt);
                return true;
            }
        }
    }

    // 4. Low priority: injection → local → steal.
    {
        crd::jobs::JobDecl job{};
        if (m_low_injection->try_pop(job))  { run_job(job); return true; }
        if (auto opt = me.low.pop())        { run_job(*opt); return true; }
        for (crd::u32 i = 1U; i < m_config.num_threads; ++i)
        {
            const crd::u32 peer = (thread_idx + i) % m_config.num_threads;
            if (auto opt = m_thread_states[peer]->low.steal()) // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            {
                run_job(*opt);
                return true;
            }
        }
    }

    return false;
}

// ---------------------------------------------------------------------------
// try_pop
//
// Same drain order as execute_one() but returns the job instead of running it.
// Used by WorkerPool so jobs run inside fiber context switches rather than
// directly on the OS thread stack.
// ---------------------------------------------------------------------------

std::optional<crd::jobs::JobDecl> Scheduler::try_pop(crd::u32 thread_idx)
{
    CRD_ASSERT_MSG(m_initialized, "Scheduler::try_pop called before init");
    CRD_ASSERT_MSG(thread_idx < m_config.num_threads,
                   "Scheduler::try_pop: thread_idx out of range");

    ThreadState& me = *m_thread_states[thread_idx]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)

    // 1. Pinned slot.
    if (me.pinned_available.load(std::memory_order_acquire))
    {
        const crd::jobs::JobDecl job = me.pinned_storage;
        me.pinned_available.store(false, std::memory_order_release);
        return job;
    }

    // 2. High priority: injection → local → steal.
    {
        crd::jobs::JobDecl job{};
        if (m_high_injection->try_pop(job)) return job;
        if (auto opt = me.high.pop()) return *opt;
        for (crd::u32 i = 1U; i < m_config.num_threads; ++i)
        {
            const crd::u32 peer = (thread_idx + i) % m_config.num_threads;
            if (auto opt = m_thread_states[peer]->high.steal()) // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
                return *opt;
        }
    }

    // 3. Normal priority: injection → local → steal.
    {
        crd::jobs::JobDecl job{};
        if (m_normal_injection->try_pop(job)) return job;
        if (auto opt = me.normal.pop()) return *opt;
        for (crd::u32 i = 1U; i < m_config.num_threads; ++i)
        {
            const crd::u32 peer = (thread_idx + i) % m_config.num_threads;
            if (auto opt = m_thread_states[peer]->normal.steal()) // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
                return *opt;
        }
    }

    // 4. Low priority: injection → local → steal.
    {
        crd::jobs::JobDecl job{};
        if (m_low_injection->try_pop(job)) return job;
        if (auto opt = me.low.pop()) return *opt;
        for (crd::u32 i = 1U; i < m_config.num_threads; ++i)
        {
            const crd::u32 peer = (thread_idx + i) % m_config.num_threads;
            if (auto opt = m_thread_states[peer]->low.steal()) // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
                return *opt;
        }
    }

    return std::nullopt;
}

// ---------------------------------------------------------------------------
// wait_for_work
// ---------------------------------------------------------------------------

void Scheduler::wait_for_work(crd::u32 thread_index)
{
    CRD_ASSERT_MSG(m_initialized, "Scheduler::wait_for_work called before init");
    if (!m_targeted_wake)
    {
        m_semaphore.acquire(); // DEFAULT PATH — unchanged
        return;
    }
    // TARGETED PATH (opt-in): mark this worker idle, then park on its own semaphore with a short timeout. The
    // timeout is a deadlock-proof backstop — a missed wake self-heals within 1 ms (the worker loops back to
    // execute_one). In the common case wake_worker/wake_one_idle posts this semaphore and the wait returns at once.
    const crd::u64 bit = crd::u64{1} << thread_index;
    m_idle_mask.fetch_or(bit, std::memory_order_seq_cst);
    (void)m_thread_states[thread_index]->wake.try_acquire_for(std::chrono::milliseconds(1));
    m_idle_mask.fetch_and(~bit, std::memory_order_seq_cst);
}

// ---------------------------------------------------------------------------
// wake_all / wake_worker / wake_one_idle
// ---------------------------------------------------------------------------

void Scheduler::wake_all(crd::u32 count)
{
    CRD_ASSERT_MSG(m_initialized, "Scheduler::wake_all called before init");
    if (m_targeted_wake)
    {
        for (auto& ts : m_thread_states) // wake every per-worker semaphore (shutdown/broadcast)
        {
            ts->wake.release(1);
        }
        return;
    }
    if (count > 0U)
        m_semaphore.release(static_cast<std::ptrdiff_t>(count));
}

void Scheduler::wake_worker(crd::u32 thread_index) noexcept
{
    m_thread_states[thread_index]->wake.release(1);
}

void Scheduler::wake_one_idle() noexcept
{
    crd::u64 m = m_idle_mask.load(std::memory_order_acquire);
    while (m != 0U)
    {
        const auto i = static_cast<crd::u32>(std::countr_zero(m));
        const crd::u64 bit = crd::u64{1} << i;
        if (m_idle_mask.compare_exchange_weak(m, m & ~bit, std::memory_order_acq_rel))
        {
            m_thread_states[i]->wake.release(1);
            return;
        }
        // CAS failed → `m` reloaded with the current mask; retry.
    }
    // No parked worker → a busy worker drains the job via execute_one; parked workers' 1 ms timeout is the backstop.
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

crd::u32 Scheduler::num_threads() const noexcept
{
    return m_config.num_threads;
}

// ---------------------------------------------------------------------------
// run_job
//
// In v1e, jobs are executed synchronously by calling fn(data).
// v1g replaces this with a fiber context switch.
// ---------------------------------------------------------------------------

void Scheduler::run_job(const crd::jobs::JobDecl& job)
{
    CRD_ASSERT_MSG(job.fn != nullptr, "Scheduler::run_job: job.fn is nullptr");
    job.fn(job.data);
}

} // namespace crd::jobs::detail
