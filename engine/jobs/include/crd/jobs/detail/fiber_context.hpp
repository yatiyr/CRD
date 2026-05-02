#pragma once

#include <crd/core/platform.hpp>
#include <crd/core/types.hpp>

namespace crd::jobs::detail
{

// Opaque fiber execution context.
// Stores the saved stack pointer; all callee-saved registers live on the fiber's
// own stack, pushed/popped by fiber_switch.
//
// On Windows x64, also stores TIB stack bounds (GS:[8] StackBase, GS:[16] StackLimit)
// so fiber_switch can restore them atomically with the RSP swap. Without this,
// __chkstk and the guard-page mechanism see an out-of-bounds RSP on threads whose
// TIB still points to the OS thread stack, causing an access violation.
struct FiberContext
{
    void* rsp = nullptr;
#if CRD_OS_WINDOWS
    void* tib_stack_base  = nullptr; // GS:[0x08] — high end of the valid stack
    void* tib_stack_limit = nullptr; // GS:[0x10] — low end of committed stack pages
#endif
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
