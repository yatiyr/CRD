#pragma once

#include "mpmc_queue.hpp"
#include "work_stealing_deque.hpp"
#include <crd/jobs/job_decl.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/platform.hpp>
#include <crd/core/types.hpp>

#include <atomic>
#include <memory>
#include <semaphore>
#include <vector>

// MSVC C4324: structure padded due to alignment specifier. Expected — alignas(64) on
// ThreadState::pinned_available is intentional (separate cache line for the pinned slot).
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
//   pinned_available     — written by any thread (producer of pinned jobs);
//                          read and cleared by the owning thread only.
struct ThreadState
{
    explicit ThreadState(crd::u32 deque_cap, crd::u32 idx)
        : high(deque_cap)
        , normal(deque_cap)
        , low(deque_cap)
        , thread_index(idx)
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

    // Single-slot pinned-job lane. Stores exactly one pinned job at a time.
    //
    // Producer protocol (any thread):
    //   1. Write pinned_storage.
    //   2. pinned_available.store(true, release).
    //
    // Consumer protocol (owning thread only):
    //   1. pinned_available.load(acquire) — if false, skip.
    //   2. Copy job from pinned_storage.
    //   3. pinned_available.store(false, release).
    //   4. Execute the job.
    //
    // The release/acquire pair establishes happens-before: step 1 of producer
    // is visible to the consumer after step 1 of consumer succeeds.
    alignas(64) std::atomic<bool>  pinned_available{false};
    crd::jobs::JobDecl             pinned_storage{};
};

// Configuration for Scheduler::init().
// Defined outside Scheduler to avoid a clang-cl diagnostic (DR: default member
// initializers in nested classes needed outside member functions).
struct SchedulerConfig
{
    crd::u32 num_threads        = 1;
    crd::u32 deque_capacity     = 256;
    crd::u32 injection_capacity = 4096;
};

// Priority scheduler: three global injection queues, per-thread local Chase-Lev
// deques, and per-thread pinned-job slots.
//
// Drain order per execute_one() call (highest to lowest):
//   1. Pinned slot for the calling thread (if occupied)
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

    // Inject a job. If job.pin_thread >= 0, stored in that thread's pinned slot.
    // Otherwise enqueued into the global injection queue for the job's priority.
    // Posts the semaphore once to wake one sleeping worker.
    // Thread-safe; lock-free.
    void push(const crd::jobs::JobDecl& job);

    // Push directly to a thread's local deque (must be called by the owning thread).
    // Avoids injection-queue contention for child jobs spawned by a running job.
    // Posts the semaphore once.
    void push_local(crd::u32 thread_index, const crd::jobs::JobDecl& job);

    // Try to execute one job according to the drain order for thread_index.
    // Returns true if a job was found and executed; false if all queues were empty.
    // thread_index: identifies the calling thread for deque ownership and steal skip.
    [[nodiscard]] bool execute_one(crd::u32 thread_index);

    // Block until push() or push_local() posts the semaphore.
    // Call after execute_one() returns false to avoid spinning.
    void wait_for_work();

    [[nodiscard]] crd::u32 num_threads()    const noexcept;
    [[nodiscard]] bool     is_initialized() const noexcept { return m_initialized; }

private:
    static void run_job(const crd::jobs::JobDecl& job);

    // Injection queues — MPMC, shared across all threads. Allocated in init().
    std::unique_ptr<MpmcQueue<crd::jobs::JobDecl>> m_high_injection;
    std::unique_ptr<MpmcQueue<crd::jobs::JobDecl>> m_normal_injection;
    std::unique_ptr<MpmcQueue<crd::jobs::JobDecl>> m_low_injection;

    // Per-thread state, indexed by thread_index.
    std::vector<std::unique_ptr<ThreadState>> m_thread_states;

    // Counting semaphore: posted once per push/push_local; workers acquire() to sleep.
    std::counting_semaphore<> m_semaphore{0};

    SchedulerConfig m_config;
    bool   m_initialized = false;
};

} // namespace crd::jobs::detail

#if CRD_COMPILER_MSVC
#pragma warning(pop)
#endif
