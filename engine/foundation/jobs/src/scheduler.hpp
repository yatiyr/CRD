#pragma once

#include "semaphore.hpp"
#include "work_stealing_deque.hpp"
#include <crd/containers/concurrent_queue.hpp>
#include <crd/jobs/job_decl.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/budget_pressure.hpp>
#include <crd/core/platform.hpp>
#include <crd/core/types.hpp>

#include <atomic>
#include <memory>
#include <optional>
#include <crd/containers/array.hpp>

// MSVC C4324: structure padded due to alignment specifier. Expected — alignas(64) on
// ThreadState::pinned_peak is intentional (the producers' tally stays off the deques' lines).
#if CRD_COMPILER_MSVC
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

namespace crd::jobs::detail
{

// Per-worker local state. Each ThreadState is individually heap-allocated so that
// concurrent deque operations by different workers never share a cache line.
//
// Thread-safety contract:
//   high / normal / low  — push() and pop() only from the owning thread;
//                          steal() from any thread.
//   pinned               — pushed by any thread (producers of pinned jobs);
//                          popped by the owning thread only.
struct ThreadState
{
    explicit ThreadState(crd::u32 deque_cap, crd::u32 pinned_cap, crd::u32 idx)
        : high(deque_cap)
        , normal(deque_cap)
        , low(deque_cap)
        , thread_index(idx)
        , pinned(pinned_cap)
    {}

    ThreadState(const ThreadState&)            = delete;
    ThreadState& operator=(const ThreadState&) = delete;
    ThreadState(ThreadState&&)                 = delete;
    ThreadState& operator=(ThreadState&&)      = delete;
    ~ThreadState()                             = default;

    WorkStealingDeque<crd::jobs::JobDecl> high;
    WorkStealingDeque<crd::jobs::JobDecl> normal;
    WorkStealingDeque<crd::jobs::JobDecl> low;
    crd::u32 thread_index;

    // Pinned-job lane: a bounded MPMC queue of SchedulerConfig::pinned_capacity jobs, drained by the owning thread
    // before anything else. It was a single slot (DIAG.10c): a second job pinned to a thread before that thread
    // drained the first asserted where asserts are on and overwrote the first elsewhere, losing it, and concurrent
    // producers raced on the plain slot. A full lane is now a counted, always-on fatal in Scheduler::push.
    crd::containers::ConcurrentQueue<crd::jobs::JobDecl> pinned;

    // The most jobs this lane has held at once since init(): raised by producers after a push (a rare path).
    alignas(64) std::atomic<crd::u32> pinned_peak{0U};

    // Default wake path only: 1 while this worker is going to sleep on the shared semaphore (set before its last look
    // at its pinned lane, cleared once it is awake again). Another worker that finds a sleeper with pinned work wakes
    // every sleeper instead of sleeping itself (Scheduler::wake_pinned_sleeper, DIAG.10c).
    alignas(64) std::atomic<crd::u32> sleeping{0U};

    // Per-worker wake semaphore — used ONLY on the opt-in targeted-wake path (ADR-0094 P-core routing). The default
    // path uses the shared Scheduler::m_semaphore and never touches this. It lets a SPECIFIC worker be woken (e.g.
    // for a pinned P-core job), which the single shared semaphore cannot do.
    Semaphore wake{};
};

// Configuration for Scheduler::init().
// Defined outside Scheduler to avoid a clang-cl diagnostic (DR: default member
// initializers in nested classes needed outside member functions).
struct SchedulerConfig
{
    crd::u32 num_threads        = 1;
    crd::u32 deque_capacity     = 256;
    crd::u32 injection_capacity = 4096;
    crd::u32 pinned_capacity    = 64;    // per-thread pinned lane (DIAG.10c); a power of two >= 2
    bool     targeted_wake      = false; // opt-in per-worker wake (ADR-0094); default = shared-semaphore path
};

// Priority scheduler: three global injection queues, per-thread local Chase-Lev
// deques, and per-thread pinned-job lanes.
//
// Drain order per execute_one() call (highest to lowest):
//   1. Pinned lane of the calling thread (if not empty)
//   2. High injection queue  → High local deque → steal High from peers
//   3. Normal injection queue→ Normal local deque→ steal Normal from peers
//   4. Low injection queue   → Low local deque  → steal Low from peers
//   5. Return false — no work found this iteration
//
// Steal direction: round-robin starting at (thread_index + 1) % num_threads,
// giving deterministic peer ordering (important for unit tests).
//
// Sleeping: workers call wait_for_work() after execute_one() returns false.
// push() and push_local() each post the semaphore once to wake one sleeper.
class Scheduler
{
public:
    Scheduler()  = default;
    ~Scheduler() { shutdown(); }

    Scheduler(const Scheduler&)            = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    Scheduler(Scheduler&&)                 = delete;
    Scheduler& operator=(Scheduler&&)      = delete;

    // Allocate per-thread states and injection queues.
    // Asserts in debug if called twice. Always returns true (allocation errors abort).
    [[nodiscard]] bool init(const SchedulerConfig& cfg = SchedulerConfig{});

    // Tear down all state. Safe to call even if init() was never called (idempotent).
    void shutdown() noexcept;

    // Inject a job. If job.pin_thread >= 0, queued in that thread's pinned lane.
    // Otherwise enqueued into the global injection queue for the job's priority.
    // Posts the semaphore once to wake one sleeping worker.
    // Thread-safe; lock-free. A full lane or queue is counted and is an always-on fatal (CRD_FATAL): a job is never
    // dropped (DIAG.10c; in builds without asserts a full queue used to lose the job and hang its waiter).
    void push(const crd::jobs::JobDecl& job);

    // Push directly to a thread's local deque (must be called by the owning thread).
    // Avoids injection-queue contention for child jobs spawned by a running job.
    // Posts the semaphore once. A full deque is an always-on fatal, like push().
    void push_local(crd::u32 thread_index, const crd::jobs::JobDecl& job);

    // Try to execute one job according to the drain order for thread_index.
    // Returns true if a job was found and executed; false if all queues were empty.
    // thread_index: identifies the calling thread for deque ownership and steal skip.
    [[nodiscard]] bool execute_one(crd::u32 thread_index);

    // Pop one job without executing it. Same drain order as execute_one().
    // Returns std::nullopt if all queues are empty.
    // Used by WorkerPool to run jobs through the fiber context switch.
    [[nodiscard]] std::optional<crd::jobs::JobDecl> try_pop(crd::u32 thread_index);

    // Block until push() or push_local() posts a wake.
    // Call after execute_one() / try_pop() returns false/nullopt to avoid spinning.
    // Default path: acquire the shared semaphore (unchanged). Targeted path: park on this worker's own semaphore
    // with a short timeout safety-net (a missed wake self-heals within the timeout ⇒ no deadlock possible).
    // `before_sleep(user)` runs on the calling thread just before it would block (no token was left for it); a wait
    // that returns at once, with a token or a job in its pinned lane, does not call it.
    void wait_for_work(crd::u32 thread_index, SleepHook before_sleep = nullptr, void* user = nullptr);

    // Release count units on the semaphore to wake up to count sleeping workers.
    // Called by WorkerPool::shutdown() to unblock workers that are in wait_for_work().
    void wake_all(crd::u32 count);

    // Wake every sleeping worker without posting a token: each returns to its worker loop's safe point once and
    // sleeps again if there is no work. A broadcast can miss a worker that was about to sleep (semaphore.hpp), so
    // worker_snapshot() repeats this while it waits for acknowledgements.
    void wake_sleepers() noexcept;

    [[nodiscard]] crd::u32 num_threads()    const noexcept;
    [[nodiscard]] bool     is_initialized() const noexcept { return m_initialized; }

    // Per-lane injection-queue diagnostics for starvation detection, indexed by Priority (0=High,1=Normal,2=Low):
    //   backlog[i] = jobs waiting in that lane's injection queue (a racy snapshot depth);
    //   pops[i]    = monotonic successful dequeues from that lane's injection queue since init.
    // Reads only the queues' own diagnostic accessors (size()/dequeued()); a best-effort dump, never an oracle.
    // Local Chase-Lev deques and stolen work are not counted -- public run() jobs land in the injection queues,
    // which is exactly the lane a starved job waits in. Both arrays are caller-provided (no allocation).
    void injection_diagnostics(crd::u32 backlog[3], crd::u64 pops[3]) const noexcept;

    // DIAG.10c queue pressure. Each injection queue by Priority: its capacity, its racy depth now and the pushes it
    // refused; its high water is not kept (it would cost every push a read of the consumers' position). The pinned
    // lanes as one budget: the per-thread capacity, the fullest lane now, the highest lane peak and the refusals of
    // all lanes. Relaxed reads -- a diagnostic sample, not an oracle.
    void injection_pressure(crd::BudgetReading out[3]) const noexcept;
    [[nodiscard]] crd::BudgetReading pinned_pressure() const noexcept;

private:
    static void run_job(const crd::jobs::JobDecl& job);

    // Targeted-wake helpers (no-ops conceptually unless m_targeted_wake). wake_worker posts one worker's semaphore;
    // wake_one_idle picks a parked worker from m_idle_mask (CAS) and posts it (falls back to nothing if none idle —
    // a busy worker will drain the job via execute_one, and the parked workers' timeout is the backstop).
    void wake_worker(crd::u32 thread_index) noexcept;
    void wake_one_idle() noexcept;

    // Default path (DIAG.10c): when a worker other than `self` has announced it is sleeping and its pinned lane holds a
    // job, wake every sleeper and return true (the caller then does not sleep); otherwise false.
    bool wake_pinned_sleeper(crd::u32 self) noexcept;

    // Injection queues — MPMC, shared across all threads. Allocated in init().
    // crd::containers::ConcurrentQueue is the promoted (allocator-aware) form of
    // what used to be jobs/src/mpmc_queue.hpp — same Vyukov algorithm (D-002 v3).
    using JobInjectionQueue = crd::containers::ConcurrentQueue<crd::jobs::JobDecl>;
    std::unique_ptr<JobInjectionQueue> m_high_injection;
    std::unique_ptr<JobInjectionQueue> m_normal_injection;
    std::unique_ptr<JobInjectionQueue> m_low_injection;

    // Per-thread state, indexed by thread_index.
    crd::containers::Array<std::unique_ptr<ThreadState>> m_thread_states;

    // Counting semaphore: posted once per push/push_local; workers acquire() to sleep. (Default-path wake.)
    // Cerid-owned futex/WaitOnAddress primitive — std::counting_semaphore lost wakes on the CI's libstdc++
    // (GCC 13.3), hanging determinism-moat tests at shutdown. See semaphore.hpp for the full mechanism.
    Semaphore m_semaphore{};

    // DIAG.10c refusal tallies, bumped just before the always-on fatal so a handler intercepting it can read them.
    std::atomic<crd::u64> m_injection_refusals[3]{};
    std::atomic<crd::u64> m_pinned_refusals{0U};

    // Targeted-wake state (opt-in; untouched on the default path). idle bit i set ⇒ worker i is parked on its own
    // ThreadState::wake. ≤ 64 workers (asserted in init when targeted_wake is requested).
    bool                  m_targeted_wake = false;
    std::atomic<crd::u64> m_idle_mask{0};

    SchedulerConfig m_config;
    bool   m_initialized = false;
};

} // namespace crd::jobs::detail

#if CRD_COMPILER_MSVC
#pragma warning(pop)
#endif
