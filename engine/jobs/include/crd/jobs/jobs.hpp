#pragma once

#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/jobs/detail/fiber_context.hpp>
#include <crd/jobs/job_decl.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <span>
#include <type_traits>

namespace crd::jobs
{

// Counter — opaque handle returned by run(). Pool-allocated; valid until wait() returns.
// Caller must eventually call wait() on every Counter* returned by run(); failing to do
// so leaks the counter slot from the internal pool.
namespace detail
{
struct Counter;
}
using Counter = detail::Counter;

// Init / shutdown — call from the main thread before any run() / wait() use.
struct Config
{
    crd::u32 num_threads = 0U; // 0 = hardware_concurrency()
    crd::u32 small_fiber_count = 128U;
    crd::u32 medium_fiber_count = 64U;
    crd::u32 large_fiber_count = 16U;
    crd::u32 max_counters = 512U;
    crd::u32 injection_queue_capacity = 4096U;
    crd::u32 frame_alloc_bytes = 1U << 20U; // 1 MB per thread

    // ADR-0094 — opt-in P-core routing. Default false ⇒ the historical shared-semaphore wake path runs verbatim
    // (no behavior/perf change for any existing system). When true, the pool uses per-worker targeted wake +
    // pins workers to cores (performance cores first) so MemoryBoundElementwise batches can route to P-cores.
    bool pcore_routing = false;
};

void init(const Config& cfg = {});
void shutdown();

// Submit one or more jobs. Returns a counter initialised to jobs.size().
// Each job decrements the counter by 1 on completion.
[[nodiscard]] Counter* run(std::span<const JobDecl> jobs);
[[nodiscard]] Counter* run(const JobDecl& job);

// Block until counter->value == target, then release the counter.
// Inside a fiber: suspends the fiber cooperatively; the thread remains available for other work.
// Outside a fiber on the main thread (thread 0): spins, calling pump() on each iteration so
// thread-0-pinned jobs can make progress. Safe even with a single-thread pool.
// Outside a fiber on any other unenrolled thread: spins with yield(); requires at least one
// background worker thread (num_threads >= 2) to decrement the counter, otherwise deadlocks.
void wait(Counter* counter, crd::u32 target = 0U);

// Convenience: submit + wait; counter released before returning.
void run_and_wait(std::span<const JobDecl> jobs);
void run_and_wait(const JobDecl& job);

// ---------------------------------------------------------------------------
// JobFence — RAII owner of a Counter* returned by run().
//
// In debug builds the destructor asserts if wait() was never called, catching
// accidental counter leaks before they reach the pool's shutdown assert.
// No auto-blocking: if you want fire-and-forget call release_without_wait().
// ---------------------------------------------------------------------------

class JobFence
{
public:
    JobFence() noexcept = default;
    explicit JobFence(Counter* c) noexcept : m_counter(c) {}

    ~JobFence() noexcept
    {
        CRD_ASSERT_MSG(m_counter == nullptr, "JobFence destroyed without wait() — counter pool slot leaked");
    }

    JobFence(const JobFence&) = delete;
    JobFence& operator=(const JobFence&) = delete;

    JobFence(JobFence&& o) noexcept : m_counter(o.m_counter) { o.m_counter = nullptr; }
    JobFence& operator=(JobFence&& o) noexcept
    {
        CRD_ASSERT_MSG(m_counter == nullptr, "JobFence: move-assigned over an uncompleted fence — counter leaked");
        m_counter = o.m_counter;
        o.m_counter = nullptr;
        return *this;
    }

    void wait(crd::u32 target = 0U)
    {
        CRD_ASSERT_MSG(m_counter != nullptr, "JobFence::wait called on an empty fence");
        crd::jobs::wait(m_counter, target);
        m_counter = nullptr;
    }

    [[nodiscard]] bool valid() const noexcept { return m_counter != nullptr; }

    // Suppress the destructor assert — caller explicitly accepts responsibility.
    // Note: the counter slot remains acquired; this leaks unless another call
    // to crd::jobs::wait() on the same pointer clears it externally.
    void release_without_wait() noexcept { m_counter = nullptr; }

private:
    Counter* m_counter = nullptr;
};

// ---------------------------------------------------------------------------
// Main-thread pump — drain one (or all) jobs from thread 0's queues.
//
// Must be called from the main thread (thread 0) after init(). Returns true
// if at least one job was found and executed, false if all queues were empty.
//
// pump_main_thread_once()    — drain-execute one job and return.
// pump_main_thread_until_idle() — loop until all queues are empty; returns
//                                 true if any work was done.
//
// Application::tick() calls pump_main_thread_once() automatically. Explicit
// calls are only needed in hand-rolled game loops or unit tests.
// ---------------------------------------------------------------------------

[[nodiscard]] bool pump_main_thread_once();
[[nodiscard]] bool pump_main_thread_until_idle();

// Introspection — valid after init().
[[nodiscard]] bool is_worker_fiber() noexcept;  // true when called from inside a job fiber
[[nodiscard]] crd::u32 worker_index() noexcept; // thread index of the calling thread
[[nodiscard]] crd::u32 num_workers() noexcept;  // total thread count (incl. thread 0)

// ---------------------------------------------------------------------------
// Worker-dispatch policy for parallel batches (ADR-0094).
//
// Bandwidth-bound elementwise work (e.g. an `out[i]=f(in[i])` sweep over millions of doubles) does NOT scale with
// one-job-per-logical-CPU on a hybrid part: a 14900K's 32 logical (8 P + 16 E + HT) THRASHES the memory subsystem
// (measured: ~0.19 ns/elem at 8 workers ≈ DDR5 peak, vs ~1.4 ns at 32). MemoryBoundElementwise asks the dispatcher
// to limit concurrency to the performance-core count so the memory floor is hit without the E-core/HT cliff.
// Compute-bound work should keep Default (it wants every core). A declarative request — callers must NOT hardcode
// worker counts. Default preserves all existing behaviour exactly.
// ---------------------------------------------------------------------------
enum class WorkerPreference : crd::u8
{
    Default,                // one job per worker (compute-bound / mixed; the historical behaviour)
    MemoryBoundElementwise, // limit to the performance-core count (bandwidth-bound elementwise)
};

// Detected performance (P) physical-core count. Intel-hybrid: cores at the top EfficiencyClass. Non-hybrid /
// sandboxed (e.g. WSL where topology is hidden): returns 0 ("unknown") and policy degrades to Default. Cached.
[[nodiscard]] crd::u32 performance_core_count() noexcept;

// Fill out_ids[0..return) with the logical-CPU indices of the performance cores (for affinity pinning, ADR-0094).
// Returns the count written (0 = unknown topology). Used by the pool when Config::pcore_routing is set.
[[nodiscard]] crd::u32 performance_core_cpu_ids(crd::u32* out_ids, crd::u32 max_ids) noexcept;

// Recommended job count for a `count`-element batch under `pref`, clamped to [1, min(num_workers(), count)].
// MemoryBoundElementwise resolves to: env `CRD_JOBS_MEMBOUND_WORKERS` if set, else performance_core_count(), else
// num_workers() (safe — never oversubscribes beyond the pool, never assumes hybrid).
[[nodiscard]] crd::u32 recommended_jobs(WorkerPreference pref, crd::u32 count) noexcept;

// Per-thread linear frame allocator. Each thread owns a private bump arena sized by
// Config::frame_alloc_bytes. Allocation is lock-free and O(1).
//
// frame_reset() resets All threads' arenas. It is NOT thread-safe relative to concurrent
// frame_alloc() calls — invoke it only at a frame boundary after all jobs of the previous
// frame have completed (e.g. after the last wait() / run_and_wait() of the frame).
[[nodiscard]] void* frame_alloc(crd::usize size, crd::usize alignment = alignof(std::max_align_t));
void frame_reset();

// Scoped frame-arena reclaim. frame_get_mark() returns the CURRENT thread's frame-arena offset;
// frame_set_mark() restores it, reclaiming only the allocations THIS thread made since the mark. Unlike
// frame_reset() (all threads, wiped to zero), this is nest-safe — it preserves a caller's earlier
// frame_alloc state below the mark. Used by parallel kernels (gemm_parallel) to reclaim their per-call
// JobDecl arrays in place. Same not-thread-safe contract as frame_alloc(): mark/restore on the owning
// thread, with no concurrent frame_alloc on that arena in flight (i.e. after the pass's wait()).
[[nodiscard]] crd::usize frame_get_mark();
void frame_set_mark(crd::usize mark);

// ---------------------------------------------------------------------------
// SBO helper — internal; referenced by make_job<F> below.
// ---------------------------------------------------------------------------

namespace detail
{
// Entry point stored in JobDecl::fn for SBO jobs. Receives a pointer to the callable
// (residing in Fiber::sbo_buf) and invokes it.
template <typename F> void sbo_trampoline(void* buf) noexcept
{
    (*static_cast<F*>(buf))();
}
} // namespace detail

// ---------------------------------------------------------------------------
// make_job — wraps a callable F into a JobDecl using 41-byte inline SBO.
//
// Requirements on F (enforced by concept):
//   sizeof(F)  <= 41 bytes  — fits in data field (8 B) + _pad[9..41] (33 B)
//   alignof(F) <= 8 bytes   — fits in Fiber::sbo_buf (alignas(8))
//   trivially copyable      — moved through queues via memcpy
//   trivially destructible  — no destructor needed when the fiber completes
//
// Callables that exceed these limits must use the raw fn/data form instead.
// ---------------------------------------------------------------------------

template <typename F>
    requires(sizeof(std::decay_t<F>) <= 41U && alignof(std::decay_t<F>) <= 8U &&
             std::is_trivially_copyable_v<std::decay_t<F>> && std::is_trivially_destructible_v<std::decay_t<F>>)
[[nodiscard]] JobDecl make_job(F&& f, StackSize stack = StackSize::Small, Priority priority = Priority::Normal)
{
    using FD = std::decay_t<F>;

    JobDecl j;
    j.fn = &detail::sbo_trampoline<FD>;
    j.stack = stack;
    j.priority = priority;
    j._pad[8] = detail::kSboFlag;

    // Construct F into a local, then pack its bytes into data + _pad[9..].
    // Both fields survive queue copies (the full 64-byte JobDecl is copied by value).
    // run_job_in_fiber will copy these bytes into Fiber::sbo_buf before the first switch.
    FD tmp(std::forward<F>(f));
    if constexpr (sizeof(FD) <= 8U)
    {
        std::memcpy(&j.data, &tmp, sizeof(FD));
    }
    else
    {
        std::memcpy(&j.data, &tmp, 8U);
        std::memcpy(&j._pad[9], reinterpret_cast<const crd::u8*>(&tmp) + 8U, sizeof(FD) - 8U);
    }
    return j;
}

// ---------------------------------------------------------------------------
// parallel_for — splits [0, count) into num_jobs ranges (clamped to count).
// F signature: void(crd::u32 begin, crd::u32 end)
//
// The Task struct {begin, end, F} must fit in 41-byte SBO (sizeof(Task) <= 41).
// For a typical pointer-capturing lambda (8 bytes), sizeof(Task) = 4+4+8 = 16.
//
// Returns a Counter* the caller must wait() on.
// Note: a std::vector is heap-allocated for the JobDecl array; the frame arena
// (v1j) will replace this allocation on the hot path.
// ---------------------------------------------------------------------------

template <typename F>
[[nodiscard]] Counter* parallel_for(crd::u32 count, crd::u32 num_jobs, F&& fn, StackSize stack = StackSize::Small,
                                    Priority priority = Priority::Normal)
{
    using FD = std::decay_t<F>;

    struct Task
    {
        crd::u32 begin;
        crd::u32 end;
        FD fn;
        void operator()() { fn(begin, end); }
    };

    static_assert(sizeof(Task) <= 41U, "parallel_for: sizeof({begin,end,F}) exceeds 41-byte SBO; use raw fn/data form");
    static_assert(alignof(Task) <= 8U, "parallel_for: alignment of Task exceeds 8 bytes");
    static_assert(std::is_trivially_copyable_v<Task>, "parallel_for: F must be trivially copyable");
    static_assert(std::is_trivially_destructible_v<Task>, "parallel_for: F must be trivially destructible");

    CRD_ASSERT_MSG(count > 0U, "parallel_for: count must be > 0");
    CRD_ASSERT_MSG(num_jobs > 0U, "parallel_for: num_jobs must be > 0");

    num_jobs = std::min(num_jobs, count); // can't have more jobs than items

    FD fn_copy(std::forward<F>(fn));

    // Allocate the JobDecl array from the per-thread frame arena — no heap allocation.
    // The array is valid until the next frame_reset(); run() copies each element into
    // the scheduler queue before returning so only the run() call must see valid memory.
    auto* const jobs = static_cast<JobDecl*>(frame_alloc(num_jobs * sizeof(JobDecl), alignof(JobDecl)));

    for (crd::u32 i = 0U; i < num_jobs; ++i)
    {
        const crd::u32 begin = i * count / num_jobs;
        const crd::u32 end = (i + 1U) * count / num_jobs;
        const JobDecl j = make_job(Task{begin, end, fn_copy}, stack, priority);
        std::memcpy(jobs + i, &j, sizeof(JobDecl));
    }
    return run(std::span<const JobDecl>(jobs, num_jobs));
}

// ---------------------------------------------------------------------------
// P-core routing (ADR-0094). Valid only on a pool created with Config::pcore_routing = true (else falls back to a
// plain parallel_for). is_pcore_routing() reports whether the pool opted in; pcore_worker_count() is how many
// workers a memory-bound batch should use (the performance-core count, clamped to the pool).
// ---------------------------------------------------------------------------
[[nodiscard]] bool is_pcore_routing() noexcept;
[[nodiscard]] crd::u32 pcore_worker_count() noexcept;

// parallel_for_pcores — like parallel_for, but pins each job to a specific worker (0..K-1, K = pcore_worker_count())
// via the pinned-job lane + the targeted-wake path, so the work lands on the performance-core-pinned workers and the
// remaining workers stay parked (no E-core/HT oversubscription). On a non-pcore_routing pool it is exactly
// parallel_for(count, num_workers, fn). Determinism is unaffected (fn ranges are disjoint).
template <typename F>
[[nodiscard]] Counter* parallel_for_pcores(crd::u32 count, F&& fn, StackSize stack = StackSize::Small,
                                           Priority priority = Priority::Normal)
{
    using FD = std::decay_t<F>;
    struct Task
    {
        crd::u32 begin;
        crd::u32 end;
        FD fn;
        void operator()() { fn(begin, end); }
    };
    static_assert(sizeof(Task) <= 41U, "parallel_for_pcores: Task exceeds 41-byte SBO");
    static_assert(std::is_trivially_copyable_v<Task>, "parallel_for_pcores: F must be trivially copyable");

    CRD_ASSERT_MSG(count > 0U, "parallel_for_pcores: count must be > 0");
    if (!is_pcore_routing())
    {
        return parallel_for(count, num_workers(), std::forward<F>(fn), stack, priority); // plain fallback
    }
    crd::u32 njobs = pcore_worker_count();
    njobs = njobs < count ? njobs : count;
    if (njobs == 0U)
    {
        njobs = 1U;
    }
    FD fn_copy(std::forward<F>(fn));
    auto* const jobs = static_cast<JobDecl*>(frame_alloc(njobs * sizeof(JobDecl), alignof(JobDecl)));
    for (crd::u32 i = 0U; i < njobs; ++i)
    {
        const crd::u32 begin = i * count / njobs;
        const crd::u32 end = (i + 1U) * count / njobs;
        JobDecl j = make_job(Task{begin, end, fn_copy}, stack, priority);
        j.pin_thread = static_cast<crd::i32>(i); // pin to worker i (affinity-pinned to a performance core at init)
        std::memcpy(jobs + i, &j, sizeof(JobDecl));
    }
    return run(std::span<const JobDecl>(jobs, njobs));
}

// ---------------------------------------------------------------------------
// Freezing an Array for the duration of a parallel pass is intentionally NOT a
// jobs function — that would force a crd-jobs -> crd-containers module edge for
// pure sugar. The freeze guard lives in crd-containers (Array::freeze /
// FrozenView, detour D-002 v2); combine them at the call site:
//
//     {
//         crd::containers::FrozenView fv(my_array);   // frozen for this scope
//         crd::jobs::Counter* c = crd::jobs::parallel_for(static_cast<crd::u32>(fv.size()), num_jobs,
//             [&fv](crd::u32 b, crd::u32 e){ for (crd::u32 i = b; i < e; ++i) fv[i] = compute(i); });
//         crd::jobs::wait(c);                         // FrozenView stays alive across the whole pass
//     }                                               // unfrozen here
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// parallel_reduce — split [0, count) into num_jobs ranges, map each range to a
// partial result, then fold the partials. Synchronous: submits, waits, folds,
// returns. The map function runs on worker fibers; the fold runs on the caller.
//
//   map    : R (crd::u32 begin, crd::u32 end)   — partial result over [begin,end)
//   reduce : R (R acc, R partial)               — combine; should be associative.
//                                                 Partials are folded LEFT-TO-RIGHT
//                                                 in job order starting from `init`,
//                                                 so the result is deterministic for
//                                                 a fixed num_jobs.
//   init   : R                                  — fold seed / identity.
//
// R must be trivially copyable (it lives in the per-thread frame arena and is
// written by a worker via assignment). MapFn must satisfy the same SBO
// constraints as parallel_for's F. ReduceFn runs only on the caller — no SBO
// constraint, may be any callable.
// ---------------------------------------------------------------------------
template <typename R, typename MapFn, typename ReduceFn>
    requires(std::is_trivially_copyable_v<R>)
[[nodiscard]] R parallel_reduce(crd::u32 count, crd::u32 num_jobs, R init, MapFn&& map, ReduceFn&& reduce,
                                StackSize stack = StackSize::Small, Priority priority = Priority::Normal)
{
    CRD_ASSERT_MSG(count > 0U, "parallel_reduce: count must be > 0");
    CRD_ASSERT_MSG(num_jobs > 0U, "parallel_reduce: num_jobs must be > 0");

    num_jobs = std::min(num_jobs, count);

    using MFD = std::decay_t<MapFn>;
    struct Task
    {
        crd::u32 begin;
        crd::u32 end;
        R* out;
        MFD map;
        void operator()() { *out = map(begin, end); }
    };

    static_assert(sizeof(Task) <= 41U,
                  "parallel_reduce: sizeof({begin,end,R*,MapFn}) exceeds 41-byte SBO; use raw fn/data form");
    static_assert(alignof(Task) <= 8U, "parallel_reduce: Task alignment exceeds 8 bytes");
    static_assert(std::is_trivially_copyable_v<Task>, "parallel_reduce: MapFn must be trivially copyable");
    static_assert(std::is_trivially_destructible_v<Task>, "parallel_reduce: MapFn must be trivially destructible");

    MFD map_copy(std::forward<MapFn>(map));

    // Partials + JobDecl array from the per-thread frame arena. JobDecls only
    // need to outlive run(); partials must outlive wait() + the fold below —
    // both live until the next frame_reset(), so we're safe.
    R* const partials = static_cast<R*>(frame_alloc(static_cast<crd::usize>(num_jobs) * sizeof(R), alignof(R)));
    auto* const jobs =
        static_cast<JobDecl*>(frame_alloc(static_cast<crd::usize>(num_jobs) * sizeof(JobDecl), alignof(JobDecl)));

    for (crd::u32 i = 0U; i < num_jobs; ++i)
    {
        const crd::u32 begin = i * count / num_jobs;
        const crd::u32 end = (i + 1U) * count / num_jobs;
        const JobDecl j = make_job(Task{begin, end, partials + i, map_copy}, stack, priority);
        std::memcpy(jobs + i, &j, sizeof(JobDecl));
    }

    Counter* const c = run(std::span<const JobDecl>(jobs, num_jobs));
    wait(c); // happens-before: the partial writes are visible after the wait

    R acc = init;
    for (crd::u32 i = 0U; i < num_jobs; ++i)
    {
        acc = reduce(acc, partials[i]);
    }
    return acc;
}

} // namespace crd::jobs
