#pragma once

#include <crd/jobs/detail/fiber_context.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>

namespace crd::jobs::detail
{

enum class FiberTier : crd::u8 { Small, Medium, Large };

// Sentinel value for the Treiber free-list link — means "no next fiber".
static constexpr crd::u32 kFiberNullIndex = 0xFFFF'FFFFu;

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

    void*      stack_alloc = nullptr;         // platform_stack_alloc base (guard page start)
    crd::usize alloc_size  = 0;               // total reserved bytes: guard + usable
    crd::u32   pool_index  = kFiberNullIndex; // stable index in the tier's fibers[] array
    crd::u32   next_free   = kFiberNullIndex; // Treiber stack link; kFiberNullIndex = end-of-list
    FiberTier  tier        = FiberTier::Small;

#if CRD_ENABLE_ASSERTS
    FiberState state = FiberState::Idle;
#endif
};

} // namespace crd::jobs::detail
