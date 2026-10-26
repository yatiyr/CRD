#include "scheduler.hpp"
#include <crd/jobs/job_decl.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>

#include <bit>
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
    if (cfg.pinned_capacity < 2U || (cfg.pinned_capacity & (cfg.pinned_capacity - 1U)) != 0U)
    {
        CRD_FATAL("Scheduler: pinned_capacity must be a power of two >= 2");
        return false;
    }

    m_config = cfg;
    m_targeted_wake = cfg.targeted_wake;
    CRD_ASSERT_MSG(!m_targeted_wake || cfg.num_threads <= 64U,
                   "Scheduler: targeted_wake supports up to 64 workers (idle mask is 64-bit)");
    m_idle_mask.store(0U, std::memory_order_relaxed);
    for (std::atomic<crd::u64>& r : m_injection_refusals)
    {
        r.store(0U, std::memory_order_relaxed);
    }
    m_pinned_refusals.store(0U, std::memory_order_relaxed);

    m_high_injection   = std::make_unique<JobInjectionQueue>(cfg.injection_capacity);
    m_normal_injection = std::make_unique<JobInjectionQueue>(cfg.injection_capacity);
    m_low_injection    = std::make_unique<JobInjectionQueue>(cfg.injection_capacity);

    m_thread_states.reserve(cfg.num_threads);
    for (crd::u32 i = 0U; i < cfg.num_threads; ++i)
    {
        m_thread_states.push_back(std::make_unique<ThreadState>(cfg.deque_capacity, cfg.pinned_capacity, i));
    }

    m_initialized = true;
    return true;
}

void Scheduler::shutdown() noexcept
{
    if (!m_initialized)
    {
        return;
    }

    m_thread_states.clear();
    m_low_injection.reset();
    m_normal_injection.reset();
    m_high_injection.reset();
    m_initialized = false;
}

// ---------------------------------------------------------------------------
// push
//
// Pinned jobs bypass the injection queues and land in the target thread's
// bounded pinned lane. A full lane or injection queue is counted, then an
// always-on fatal: the job is never dropped (DIAG.10c).
// ---------------------------------------------------------------------------

void Scheduler::push(const crd::jobs::JobDecl& job)
{
    if (push_unwoken(job))
    {
        wake_for(1U);
    }
}

bool Scheduler::push_unwoken(const crd::jobs::JobDecl& job)
{
    CRD_ASSERT_MSG(m_initialized, "Scheduler::push called before init");

    if (job.pin_thread >= 0)
    {
        const auto pin = static_cast<crd::u32>(job.pin_thread);
        CRD_ASSERT_MSG(pin < m_config.num_threads,
                       "Scheduler::push: pin_thread out of range");
        ThreadState& ts = *m_thread_states[pin]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        if (!ts.pinned.try_push(job))
        {
            m_pinned_refusals.fetch_add(1U, std::memory_order_relaxed);
            CRD_FATAL("Scheduler::push: pinned lane full — raise pinned_queue_capacity in jobs::Config");
            return false;
        }
        const auto held = static_cast<crd::u32>(ts.pinned.size());
        crd::u32   peak = ts.pinned_peak.load(std::memory_order_relaxed);
        while (held > peak && !ts.pinned_peak.compare_exchange_weak(peak, held, std::memory_order_relaxed))
        {
        }
        if (m_targeted_wake)
        {
            wake_worker(pin); // wake the SPECIFIC target (the shared semaphore couldn't)
        }
        else
        {
            // DIAG.10c: wake every sleeper, so the target is among them. A release(1) woke one arbitrary sleeper; when
            // that was another worker it took the token, found nothing it may run and slept again, and the target
            // slept on with the job in its lane. The fence pairs with the one in wait_for_work: a target that missed
            // this push in its last look at the lane has announced it is sleeping before anyone who takes this token
            // looks (wake_pinned_sleeper), so the token's taker wakes it instead of sleeping.
            std::atomic_thread_fence(std::memory_order_seq_cst);
            m_semaphore.release_broadcast();
        }
        return false;
    }

    const auto lane = static_cast<crd::u32>(job.priority); // 0 = High, 1 = Normal, 2 = Low
    if (lane > 2U)
    {
        CRD_FATAL("Scheduler::push: unknown job priority");
        return false;
    }
    JobInjectionQueue* const lanes[3] = {m_high_injection.get(), m_normal_injection.get(), m_low_injection.get()};
    if (!lanes[lane]->try_push(job))
    {
        m_injection_refusals[lane].fetch_add(1U, std::memory_order_relaxed);
        CRD_FATAL("Scheduler::push: injection queue full — raise injection_queue_capacity in jobs::Config");
        return false;
    }
    return true;
}

void Scheduler::wake_for(crd::u32 jobs) noexcept
{
    const crd::u32 tokens = jobs < m_config.num_threads ? jobs : m_config.num_threads;
    if (tokens == 0U)
    {
        return;
    }
    if (m_targeted_wake)
    {
        for (crd::u32 i = 0U; i < tokens; ++i)
        {
            wake_one_idle();
        }
        return;
    }
    m_semaphore.release(tokens); // one wake: a single token wakes one sleeper, more wake every sleeper
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
    bool ok = false;
    switch (job.priority)
    {
    case crd::jobs::Priority::High:
        ok = ts.high.push(job);
        break;
    case crd::jobs::Priority::Normal:
        ok = ts.normal.push(job);
        break;
    case crd::jobs::Priority::Low:
        ok = ts.low.push(job);
        break;
    }
    if (!ok)
    {
        CRD_FATAL("Scheduler::push_local: local deque full — raise deque_capacity");
        return;
    }
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

    // 1. Pinned lane — highest priority override.
    {
        crd::jobs::JobDecl job{};
        if (me.pinned.try_pop(job))
        {
            run_job(job);
            return true;
        }
    }

    // 2. High priority: injection → local → steal.
    {
        crd::jobs::JobDecl job{};
        if (m_high_injection->try_pop(job))
        {
            run_job(job);
            return true;
        }
        if (auto opt = me.high.pop())
        {
            run_job(*opt);
            return true;
        }
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
        if (m_normal_injection->try_pop(job))
        {
            run_job(job);
            return true;
        }
        if (auto opt = me.normal.pop())
        {
            run_job(*opt);
            return true;
        }
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
        if (m_low_injection->try_pop(job))
        {
            run_job(job);
            return true;
        }
        if (auto opt = me.low.pop())
        {
            run_job(*opt);
            return true;
        }
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

    // 1. Pinned lane.
    {
        crd::jobs::JobDecl job{};
        if (me.pinned.try_pop(job))
        {
            return job;
        }
    }

    // 2. High priority: injection → local → steal.
    {
        crd::jobs::JobDecl job{};
        if (m_high_injection->try_pop(job))
        {
            return job;
        }
        if (auto opt = me.high.pop())
        {
            return *opt;
        }
        for (crd::u32 i = 1U; i < m_config.num_threads; ++i)
        {
            const crd::u32 peer = (thread_idx + i) % m_config.num_threads;
            if (auto opt = m_thread_states[peer]->high.steal()) // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            {
                return *opt;
            }
        }
    }

    // 3. Normal priority: injection → local → steal.
    {
        crd::jobs::JobDecl job{};
        if (m_normal_injection->try_pop(job))
        {
            return job;
        }
        if (auto opt = me.normal.pop())
        {
            return *opt;
        }
        for (crd::u32 i = 1U; i < m_config.num_threads; ++i)
        {
            const crd::u32 peer = (thread_idx + i) % m_config.num_threads;
            if (auto opt = m_thread_states[peer]->normal.steal()) // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            {
                return *opt;
            }
        }
    }

    // 4. Low priority: injection → local → steal.
    {
        crd::jobs::JobDecl job{};
        if (m_low_injection->try_pop(job))
        {
            return job;
        }
        if (auto opt = me.low.pop())
        {
            return *opt;
        }
        for (crd::u32 i = 1U; i < m_config.num_threads; ++i)
        {
            const crd::u32 peer = (thread_idx + i) % m_config.num_threads;
            if (auto opt = m_thread_states[peer]->low.steal()) // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            {
                return *opt;
            }
        }
    }

    return std::nullopt;
}

// ---------------------------------------------------------------------------
// wait_for_work
// ---------------------------------------------------------------------------

void Scheduler::wait_for_work(crd::u32 thread_index, SleepHook before_sleep, void* user, crd::u32 spin_rounds)
{
    CRD_ASSERT_MSG(m_initialized, "Scheduler::wait_for_work called before init");
    if (!m_targeted_wake)
    {
        // The spin only reads the count; the protocol below runs after it unchanged, and takes the token it saw.
        if (spin_rounds != 0U)
        {
            (void)m_semaphore.spin_for_token(spin_rounds);
        }
        // DEFAULT PATH. Return to the worker loop after every wake, not only after taking a token: a broadcast
        // (wake_all, e.g. a worker_snapshot request) must bring EVERY sleeper back to its loop-top ack, and with
        // acquire() one worker could drain all the broadcast's tokens while the rest re-slept inside it unacknowledged
        // (hosted linux-gcc-debug-sse2, 2026-10-04: an idle 2-worker snapshot saw 1 response). A wake without a token
        // costs one extra loop turn (the ack store and an empty try_pop) before the worker sleeps again.
        //
        // Pinned work (DIAG.10c). Tokens are shared, so the token a pinned push posts for this worker can be taken by
        // another one first. So a worker announces it is sleeping, then takes a last look at its own pinned lane; and
        // before sleeping it looks for another announced sleeper whose lane holds a job, and if it finds one it wakes
        // every sleeper and returns instead of sleeping. With the push's fence, either this worker's last look sees
        // the job, or whoever takes the push's token sees this worker's announcement and its job.
        ThreadState& me = *m_thread_states[thread_index]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        me.sleeping.store(1U, std::memory_order_seq_cst);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (!me.pinned.empty() || wake_pinned_sleeper(thread_index))
        {
            me.sleeping.store(0U, std::memory_order_relaxed);
            return;
        }
        m_semaphore.acquire_or_wake(before_sleep, user);
        me.sleeping.store(0U, std::memory_order_relaxed);
        return;
    }
    // TARGETED PATH (opt-in): mark this worker idle, then park on its own semaphore with a short timeout. The
    // timeout is a deadlock-proof backstop — a missed wake self-heals within 1 ms (the worker loops back to
    // execute_one). In the common case wake_worker/wake_one_idle posts this semaphore and the wait returns at once.
    const crd::u64 bit = crd::u64{1} << thread_index;
    m_idle_mask.fetch_or(bit, std::memory_order_seq_cst);
    // Marked idle first, so wake_one_idle can pick this worker and post the token the spin looks for.
    if (spin_rounds != 0U)
    {
        (void)m_thread_states[thread_index]->wake.spin_for_token(spin_rounds);
    }
    (void)m_thread_states[thread_index]->wake.try_acquire_for_ms(1U, before_sleep, user);
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
    {
        m_semaphore.release(count);
    }
}

void Scheduler::wake_sleepers() noexcept
{
    if (!m_initialized)
    {
        return;
    }
    if (m_targeted_wake)
    {
        // The targeted path sleeps at most 1 ms per turn and so cannot sleep through a request; wake its parked
        // workers anyway so they answer at once.
        for (auto& ts : m_thread_states)
        {
            ts->wake.wake_sleepers();
        }
        return;
    }
    m_semaphore.wake_sleepers();
}

bool Scheduler::wake_pinned_sleeper(crd::u32 self) noexcept
{
    // Thread 0 never sleeps here (it pumps its own lane), so only workers 1..N-1 are looked at.
    for (crd::u32 i = 1U; i < m_config.num_threads; ++i)
    {
        if (i == self)
        {
            continue;
        }
        const ThreadState& ts = *m_thread_states[i]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        if (ts.sleeping.load(std::memory_order_seq_cst) != 0U && !ts.pinned.empty())
        {
            m_semaphore.release_broadcast();
            return true;
        }
    }
    return false;
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

void Scheduler::injection_diagnostics(crd::u32 backlog[3], crd::u64 pops[3]) const noexcept
{
    const JobInjectionQueue* const lanes[3] = {m_high_injection.get(), m_normal_injection.get(),
                                               m_low_injection.get()};
    for (crd::u32 i = 0U; i < 3U; ++i)
    {
        backlog[i] = (lanes[i] != nullptr) ? static_cast<crd::u32>(lanes[i]->size()) : 0U;
        pops[i]    = (lanes[i] != nullptr) ? lanes[i]->dequeued() : 0U;
    }
}

void Scheduler::injection_pressure(crd::BudgetReading out[3]) const noexcept
{
    const JobInjectionQueue* const lanes[3] = {m_high_injection.get(), m_normal_injection.get(), m_low_injection.get()};
    for (crd::u32 i = 0U; i < 3U; ++i)
    {
        crd::BudgetReading r;
        if (lanes[i] != nullptr)
        {
            r.read     = true;
            r.bounded  = true;
            r.capacity = lanes[i]->capacity();
            r.in_use   = lanes[i]->size();
            r.refusals = m_injection_refusals[i].load(std::memory_order_relaxed);
        }
        out[i] = r;
    }
}

crd::BudgetReading Scheduler::pinned_pressure() const noexcept
{
    crd::BudgetReading r;
    if (!m_initialized)
    {
        return r;
    }
    r.read               = true;
    r.bounded            = true;
    r.high_water_tracked = true;
    r.capacity           = m_config.pinned_capacity;
    r.refusals           = m_pinned_refusals.load(std::memory_order_relaxed);
    for (const std::unique_ptr<ThreadState>& ts : m_thread_states)
    {
        const crd::u64 held = ts->pinned.size();
        const crd::u64 peak = ts->pinned_peak.load(std::memory_order_relaxed);
        r.in_use            = held > r.in_use ? held : r.in_use;
        r.high_water        = peak > r.high_water ? peak : r.high_water;
    }
    // A push raises the peak just after the lane grew, so a lane read between the two is above it.
    r.high_water = r.in_use > r.high_water ? r.in_use : r.high_water;
    return r;
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
