#pragma once

#include <crd/jobs/detail/fiber_context.hpp>
#include <crd/jobs/job_decl.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>

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
namespace detail { struct Counter; }
using Counter = detail::Counter;

// Init / shutdown — call from the main thread before any run() / wait() use.
struct Config
{
    crd::u32 num_threads              = 0u;       // 0 = hardware_concurrency()
    crd::u32 small_fiber_count        = 128u;
    crd::u32 medium_fiber_count       = 64u;
    crd::u32 large_fiber_count        = 16u;
    crd::u32 max_counters             = 512u;
    crd::u32 injection_queue_capacity = 4096u;
    crd::u32 frame_alloc_bytes        = 1u << 20u; // 1 MB per thread
};

void init(const Config& cfg = {});
void shutdown();

// Submit one or more jobs. Returns a counter initialised to jobs.size().
// Each job decrements the counter by 1 on completion.
[[nodiscard]] Counter* run(std::span<const JobDecl> jobs);
[[nodiscard]] Counter* run(const JobDecl& job);

// Block until counter->value == target, then release the counter.
// Inside a fiber: suspends the fiber cooperatively; the thread remains available for other work.
// Outside a fiber (e.g. main thread): spins with yield() — requires at least one background
// worker thread to be present (num_threads >= 2), otherwise deadlocks.
void wait(Counter* counter, crd::u32 target = 0u);

// Convenience: submit + wait; counter released before returning.
void run_and_wait(std::span<const JobDecl> jobs);
void run_and_wait(const JobDecl& job);

// Introspection — valid after init().
[[nodiscard]] bool      is_worker_fiber() noexcept; // true when called from inside a job fiber
[[nodiscard]] crd::u32  worker_index()    noexcept; // thread index of the calling thread
[[nodiscard]] crd::u32  num_workers()     noexcept; // total thread count (incl. thread 0)

// Per-thread linear frame allocator. Each thread owns a private bump arena sized by
// Config::frame_alloc_bytes. Allocation is lock-free and O(1).
//
// frame_reset() resets ALL threads' arenas. It is NOT thread-safe relative to concurrent
// frame_alloc() calls — invoke it only at a frame boundary after all jobs of the previous
// frame have completed (e.g. after the last wait() / run_and_wait() of the frame).
[[nodiscard]] void* frame_alloc(crd::usize size,
                                crd::usize alignment = alignof(std::max_align_t));
void frame_reset();

// ---------------------------------------------------------------------------
// SBO helper — internal; referenced by make_job<F> below.
// ---------------------------------------------------------------------------

namespace detail
{
// Entry point stored in JobDecl::fn for SBO jobs. Receives a pointer to the callable
// (residing in Fiber::sbo_buf) and invokes it.
template<typename F>
void sbo_trampoline(void* buf) noexcept
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

template<typename F>
requires (sizeof(std::decay_t<F>)  <= 41u &&
          alignof(std::decay_t<F>) <= 8u  &&
          std::is_trivially_copyable_v<std::decay_t<F>> &&
          std::is_trivially_destructible_v<std::decay_t<F>>)
[[nodiscard]] JobDecl make_job(F&& f,
                               StackSize stack    = StackSize::Small,
                               Priority  priority = Priority::Normal)
{
    using FD = std::decay_t<F>;

    JobDecl j;
    j.fn       = &detail::sbo_trampoline<FD>;
    j.stack    = stack;
    j.priority = priority;
    j._pad[8]  = detail::kSboFlag;

    // Construct F into a local, then pack its bytes into data + _pad[9..].
    // Both fields survive queue copies (the full 64-byte JobDecl is copied by value).
    // run_job_in_fiber will copy these bytes into Fiber::sbo_buf before the first switch.
    FD tmp(std::forward<F>(f));
    if constexpr (sizeof(FD) <= 8u)
    {
        std::memcpy(&j.data, &tmp, sizeof(FD));
    }
    else
    {
        std::memcpy(&j.data, &tmp, 8u);
        std::memcpy(&j._pad[9],
                    reinterpret_cast<const crd::u8*>(&tmp) + 8u,
                    sizeof(FD) - 8u);
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

template<typename F>
[[nodiscard]] Counter* parallel_for(crd::u32 count, crd::u32 num_jobs, F&& fn,
                                     StackSize stack    = StackSize::Small,
                                     Priority  priority = Priority::Normal)
{
    using FD = std::decay_t<F>;

    struct Task
    {
        crd::u32 begin;
        crd::u32 end;
        FD       fn;
        void operator()() { fn(begin, end); }
    };

    static_assert(sizeof(Task)  <= 41u,
        "parallel_for: sizeof({begin,end,F}) exceeds 41-byte SBO; use raw fn/data form");
    static_assert(alignof(Task) <= 8u,
        "parallel_for: alignment of Task exceeds 8 bytes");
    static_assert(std::is_trivially_copyable_v<Task>,
        "parallel_for: F must be trivially copyable");
    static_assert(std::is_trivially_destructible_v<Task>,
        "parallel_for: F must be trivially destructible");

    CRD_ASSERT_MSG(count    > 0u, "parallel_for: count must be > 0");
    CRD_ASSERT_MSG(num_jobs > 0u, "parallel_for: num_jobs must be > 0");

    num_jobs = std::min(num_jobs, count); // can't have more jobs than items

    FD fn_copy(std::forward<F>(fn));

    // Allocate the JobDecl array from the per-thread frame arena — no heap allocation.
    // The array is valid until the next frame_reset(); run() copies each element into
    // the scheduler queue before returning so only the run() call must see valid memory.
    auto* const jobs = static_cast<JobDecl*>(
        frame_alloc(num_jobs * sizeof(JobDecl), alignof(JobDecl)));

    for (crd::u32 i = 0u; i < num_jobs; ++i)
    {
        const crd::u32 begin = i * count / num_jobs;
        const crd::u32 end   = (i + 1u) * count / num_jobs;
        const JobDecl  j     = make_job(Task{begin, end, fn_copy}, stack, priority);
        std::memcpy(jobs + i, &j, sizeof(JobDecl));
    }
    return run(std::span<const JobDecl>(jobs, num_jobs));
}

} // namespace crd::jobs
