#pragma once

// ---------------------------------------------------------------------------
// crd-perf -- crd-jobs adapter (Detour D-003 v0c).
//
// Installs a `crd::jobs::JobObserver` whose callbacks turn job lifecycle
// events into Sample push/pop on the per-thread profiler ring. Once
// installed, every job that runs through the scheduler produces a
// Category::Job region in the timeline -- automatically, without any
// CRD_PERF_SCOPE on the call site. **The "profile every job" pin.**
//
// Wire format:
//   - on_job_begin  -> push_region(name="job", Category::Job); fiber id is
//                      the Fiber* lower 32 bits. The BeginToken is parked
//                      in a side table keyed by FiberHandle so a cross-thread
//                      pop_region (after fiber migration) can retrieve it.
//   - on_job_end    -> pop_region(parked-token). Migration shows up as
//                      Sample.begin_thread != Sample.end_thread.
//   - on_fiber_yield  -> records a fiber-yield event (consumed by the v0g UI
//                        timeline to show suspend gaps; v0c just records).
//   - on_fiber_resume -> set_current_fiber_id(fiber); the next push_region on
//                        this thread will tag samples with this fiber.
//
// Usage:
//   crd::perf::init();
//   crd::perf::install_jobs_adapter();   // before jobs::init() ideally
//   crd::jobs::init();
//   // ... run workload ...
//   crd::jobs::shutdown();
//   crd::perf::uninstall_jobs_adapter();
//   crd::perf::shutdown();
//
// install_jobs_adapter() is idempotent and safe to call before
// crd::perf::init() (callbacks fire but no-op until the profiler is live).
// ---------------------------------------------------------------------------

#include <crd/perf/config.hpp>

namespace crd::perf
{

// Install the JobObserver. Subsequent crd::jobs scheduler activity will be
// captured in the profiler rings. Calling twice is a no-op.
void install_jobs_adapter() noexcept;

// Remove the JobObserver. Pairs with install_jobs_adapter(). After this
// call, jobs scheduler activity is no longer captured.
void uninstall_jobs_adapter() noexcept;

// True iff the adapter is currently installed.
[[nodiscard]] bool jobs_adapter_installed() noexcept;

// Statistics surface (for tests + v0g UI).
struct JobsAdapterStats
{
    crd::u64 jobs_begun     = 0;
    crd::u64 jobs_ended     = 0;
    crd::u64 fibers_yielded = 0;
    crd::u64 fibers_resumed = 0;
    crd::u64 missing_tokens = 0; // pop without a matching begin (shouldn't happen)
};

[[nodiscard]] JobsAdapterStats jobs_adapter_stats() noexcept;

} // namespace crd::perf
