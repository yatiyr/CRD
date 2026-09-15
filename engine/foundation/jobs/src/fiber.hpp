#pragma once

#include <crd/jobs/detail/fiber_context.hpp>
#include "sanitizer_fibers.hpp"
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>

#include <atomic>

namespace crd::jobs::detail
{

// Forward-declared here to avoid a circular include with counter.hpp.
struct Counter;

enum class FiberTier : crd::u8 { Small, Medium, Large };

// Sentinel value for the Treiber free-list link — means "no next fiber".
static constexpr crd::u32 kFiberNullIndex = 0xFFFF'FFFFU;

#if CRD_ENABLE_ASSERTS
// Explicit state machine. Only Idle↔Active transitions are enforced in v1b.
// Waiting and Ready are defined here so v1f (counter+wait) can assert transitions
// without changing this header.
enum class FiberState : crd::u8
{
    Idle,    // in the free pool; can be acquired
    Active,  // executing a job or in transit between job and scheduler
    Waiting, // suspended on a Counter waiting for a target value  (v1f)
    Ready,   // counter reached target; re-queued as High-priority  (v1f)
};
#endif

// Internal fiber descriptor.
// Lives in a dense array owned by FiberPool::Tier. Only the pool and scheduler
// should touch this struct directly.
struct Fiber
{
    // Saved execution context — must be first so callers can safely cast
    // &fiber to FiberContext* when needed by unit tests or the scheduler.
    FiberContext context{};

    void*      stack_alloc   = nullptr;         // platform_stack_alloc base (guard page start)
    crd::usize alloc_size    = 0;               // total reserved bytes: guard + usable
    void*      usable_base   = nullptr;         // first committed byte of the stack
    crd::usize usable_size   = 0;               // committed byte count (= alloc_size - guard)
    void     (*trampoline)() = nullptr;         // entry_fn passed to fiber_init_stack
    crd::u32   pool_index    = kFiberNullIndex; // stable index in the tier's fibers[] array
    // Treiber stack link; kFiberNullIndex = end-of-list. Atomic (relaxed) so the
    // pop-side read and push-side write of the link are not a data race under TSan
    // The real happens-before is carried by the tier's free_head, and ABA is
    // handled by the generation tag packed into free_head — see fiber_pool.cpp.
    std::atomic<crd::u32> next_free{kFiberNullIndex};
    FiberTier  tier          = FiberTier::Small;
    Counter*   job_counter   = nullptr;         // counter to decrement when this fiber's job completes
    // The counter this fiber is currently BLOCKED on (distinct from job_counter, which is its own job's
    // completion counter): set on the park path in counter_wait before the switch, cleared to nullptr on
    // resume. It is the wait-graph edge — a diagnostics snapshot walks the fiber pool and reports every fiber
    // with a non-null value. Atomic/relaxed so a concurrent snapshot reader is race-free; it is written only
    // by the owning fiber on the (cold) park/resume path, so relaxed is enough for that writer.
    std::atomic<Counter*> waiting_on{nullptr};

    // Opt-in task progress epoch for livelock detection. 0 = unmonitored (the default, and what a recycled
    // fiber is reset to in FiberPool::release_to). The running task bumps it via jobs::note_progress(); the
    // watchdog reports a monitored task whose epoch stays flat across several windows while it keeps a worker
    // executing (a livelock -- indistinguishable from a legitimate long job by aggregates alone, hence opt-in).
    // Atomic/relaxed: written only by the owning fiber, read racily by the diagnostics snapshot.
    std::atomic<crd::u32> progress_epoch{0U};

    // Sanitizer switch state (sanitizer_fibers.hpp): the fake stack this fiber left behind and the scheduler stack
    // it returns to, its TSan context, and the TSan context of the thread that dispatched or resumed it last.
    SanitizerSwitch sanitizer{};
    void*           tsan_fiber  = nullptr;
    void*           tsan_return = nullptr;

    // SBO callable storage — valid when the running job was created by make_job<F>.
    // Copied from JobDecl (data + _pad[9..41]) by run_job_in_fiber on initial dispatch.
    // Persists across fiber suspension + resume on any thread; outlives the JobDecl copy.
    alignas(8) crd::u8 sbo_buf[41] = {}; // NOLINT(modernize-avoid-c-arrays)

#if CRD_ENABLE_ASSERTS
    FiberState state = FiberState::Idle;
#endif
};

} // namespace crd::jobs::detail
