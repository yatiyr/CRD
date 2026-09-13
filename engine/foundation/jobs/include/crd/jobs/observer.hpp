#pragma once

// ---------------------------------------------------------------------------
// crd-jobs -- per-job observer hook (added for D-003 v0c profiler integration).
//
// A `JobObserver` is a set of optional function-pointer callbacks invoked
// by the scheduler at well-defined points in a job's lifetime:
//
//   on_job_begin    -- the job's callable is about to run on this OS thread
//   on_job_end      -- the callable returned, before the job's counter releases
//                      any waiter (fiber will be recycled afterwards)
//   on_fiber_yield  -- the fiber is leaving this OS thread (counter_wait suspend)
//   on_fiber_resume -- the fiber is re-entering an OS thread after yield
//
// At most one observer is installed at any time (single subscriber). The
// profiler (crd-perf) is the intended consumer; tests / debug tooling may
// also temporarily install one. The observer is set via `set_observer(*)`
// before scheduler init or while the scheduler is quiescent; setting it
// during a running scheduler is permitted, but a job that began under the
// previous observer delivers its `on_job_end` to the observer current when
// the callable returns (the trampoline re-reads it there), so a swap in
// flight yields an end without a matching begin (crd-perf counts that as a
// missing token; its uninstall drains parked tokens).
//
// Cost when no observer is installed: one nullptr load + branch-not-taken
// per scheduler entry. `run_job_in_fiber` caches the observer pointer for
// begin/yield/resume so the compiler can hoist the null check; the
// trampoline loads it once more for `on_job_end`.
//
// FiberHandle is an opaque `void*` (the underlying Fiber* in the scheduler).
// Use it as an identity key only -- the observer must not dereference it.
// Pointer-as-id is stable for the fiber's lifetime within one process.
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>

namespace crd::jobs
{

// Opaque fiber handle. Use as identity key only.
using FiberHandle = void*;

struct JobObserver
{
    // Called immediately before the job callable runs on `thread_index`.
    // `fiber` is the OS-stack-allocated context the callable will execute
    // on. `priority` and `stack_tier` mirror the JobDecl fields.
    void (*on_job_begin)(FiberHandle fiber, crd::u8 thread_index,
                         crd::u8 priority, crd::u8 stack_tier) noexcept = nullptr;

    // Called after the job callable returns and BEFORE the job's counter is
    // decremented, on the OS thread that finished the callable (still on the
    // fiber's stack). A waiter that `wait` releases for this job therefore
    // observes everything the observer recorded here.
    void (*on_job_end)(FiberHandle fiber, crd::u8 thread_index) noexcept = nullptr;

    // Called when the fiber is about to suspend (counter_wait park). The OS
    // thread will pick up other work; this fiber may later resume on a
    // different thread.
    void (*on_fiber_yield)(FiberHandle fiber, crd::u8 thread_index) noexcept = nullptr;

    // Called when a previously-suspended fiber is about to resume on
    // `thread_index` (which may differ from the yielding thread).
    void (*on_fiber_resume)(FiberHandle fiber, crd::u8 thread_index) noexcept = nullptr;
};

// Install / replace / clear the observer. Pass nullptr to clear.
void set_observer(const JobObserver* observer) noexcept;

// Returns the currently installed observer, or nullptr if none.
[[nodiscard]] const JobObserver* current_observer() noexcept;

} // namespace crd::jobs
