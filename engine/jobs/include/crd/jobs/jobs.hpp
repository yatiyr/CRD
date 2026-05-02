#pragma once

#include <crd/jobs/detail/fiber_context.hpp>
#include <crd/jobs/job_decl.hpp>
#include <crd/core/types.hpp>

#include <span>

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
    crd::u32 num_threads              = 0u;      // 0 = hardware_concurrency()
    crd::u32 small_fiber_count        = 128u;
    crd::u32 medium_fiber_count       = 64u;
    crd::u32 large_fiber_count        = 16u;
    crd::u32 max_counters             = 512u;
    crd::u32 injection_queue_capacity = 4096u;
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

} // namespace crd::jobs
