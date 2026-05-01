#pragma once

#include <crd/core/types.hpp>

namespace crd::jobs::detail
{

// Opaque fiber execution context.
// Stores only the saved stack pointer; all callee-saved registers live
// on the fiber's own stack, pushed/popped by fiber_switch.
struct FiberContext
{
    void* rsp = nullptr;
};

// Low-level fiber context switch (hand-rolled assembly, one file per ABI).
//
// Saves the calling context into *from and resumes *to.
// On the FIRST switch to a fresh fiber, execution continues at the entry
// function passed to fiber_init_stack. On subsequent switches, execution
// continues at the point immediately after the last fiber_switch call in
// that fiber.
//
// Precondition: to->rsp must be non-null (fiber_init_stack or prior
// fiber_switch has initialised it).
extern "C" void fiber_switch(FiberContext* from, FiberContext* to) noexcept;

// Set up the initial stack frame for a new fiber.
//
// After this call, fiber_switch(_, &ctx) will begin execution at entry_fn.
// entry_fn MUST call fiber_switch before returning; if it returns without
// switching, the runtime will abort via CRD_FATAL.
//
// stack_base: beginning of the raw memory region used as the stack.
// stack_size: total byte count of the region (minimum 1 KB; ≥ 64 KB recommended).
// entry_fn:   fiber entry point with no arguments and no return.
void fiber_init_stack(FiberContext& ctx, void* stack_base, crd::usize stack_size,
                      void (*entry_fn)()) noexcept;

} // namespace crd::jobs::detail
