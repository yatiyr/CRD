#pragma once
// Sanitizer awareness of the fiber switches (REPO.DEV.9; docs/design/test-instruments.md).
//
// AddressSanitizer tracks one stack per thread; a hand-rolled fiber switch moves the stack pointer to memory ASan
// has never seen as a stack, so without annotations its stack bounds, fake stacks (detect_stack_use_after_return)
// and the poisoning left by frames that never returned belong to the wrong stack. ThreadSanitizer likewise keeps
// its shadow stack and happens-before clocks per thread; a fiber is a logical thread that must be created,
// switched to and destroyed through its interface. Both interfaces are declared by every supported toolchain
// (MSVC, GCC and clang ship <sanitizer/common_interface_defs.h>; GCC and clang <sanitizer/tsan_interface.h>).
//
// Model. A worker's scheduler stack dispatches a fiber (run_job_in_fiber), the fiber runs on that thread until it
// parks (counter_wait) or completes (job_fiber_trampoline) and then switches back to the scheduler stack of the
// thread it is running on, which may not be the thread that dispatched it. So the scheduler side keeps its switch
// state in the dispatching frame, and the fiber keeps its own in the Fiber struct: the fake stack it left behind
// and the bounds of the scheduler stack it will return to, learned each time it is entered or resumed. Every switch
// is a synchronization point for TSan on the switching thread (flags 0); cross-thread ordering is the work queues'
// acquire/release, which the switch composes with, so a race TSan reports through this model is a real one.
//
// Nothing here changes an uninstrumented build: every helper is empty unless the compiler defines the sanitizer.
#include <crd/core/types.hpp>

// The audit switch: -DCRD_JOBS_SANITIZER_FIBERS=0 reproduces the unannotated model under a sanitizer so the
// before/after of the instrument can be measured; no preset sets it.
#ifndef CRD_JOBS_SANITIZER_FIBERS
#define CRD_JOBS_SANITIZER_FIBERS 1
#endif

#if CRD_JOBS_SANITIZER_FIBERS && defined(__SANITIZE_ADDRESS__)
#define CRD_JOBS_ASAN 1
#elif CRD_JOBS_SANITIZER_FIBERS && defined(__has_feature)
#if __has_feature(address_sanitizer)
#define CRD_JOBS_ASAN 1
#endif
#endif
#ifndef CRD_JOBS_ASAN
#define CRD_JOBS_ASAN 0
#endif

#if CRD_JOBS_SANITIZER_FIBERS && defined(__SANITIZE_THREAD__)
#define CRD_JOBS_TSAN 1
#elif CRD_JOBS_SANITIZER_FIBERS && defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define CRD_JOBS_TSAN 1
#endif
#endif
#ifndef CRD_JOBS_TSAN
#define CRD_JOBS_TSAN 0
#endif

#if CRD_JOBS_ASAN
#include <sanitizer/common_interface_defs.h>
#endif
#if CRD_JOBS_TSAN
#include <sanitizer/tsan_interface.h>
#endif

namespace crd::jobs::detail
{

// The switch state of one stack: the ASan fake stack saved when it switched away and the bounds of the stack that
// was running before it was (re)entered, which is the stack it returns to.
struct SanitizerSwitch
{
    void*       fake_stack    = nullptr;
    const void* return_bottom = nullptr;
    crd::usize  return_size   = 0U;
};

// Immediately before fiber_switch(): the target stack's bounds and TSan context.
inline void sanitizer_switch_begin(SanitizerSwitch& state, const void* target_bottom, crd::usize target_size,
                                   void* target_tsan_fiber) noexcept
{
#if CRD_JOBS_ASAN
    __sanitizer_start_switch_fiber(&state.fake_stack, target_bottom, target_size);
#else
    (void)target_bottom;
    (void)target_size;
#endif
#if CRD_JOBS_TSAN
    __tsan_switch_to_fiber(target_tsan_fiber, 0U);
#else
    (void)target_tsan_fiber;
#endif
    (void)state;
}

// Immediately after fiber_switch() returned, back on the stack that called sanitizer_switch_begin(): restores the
// fake stack and records the bounds of the stack that switched here (the one this stack will return to next).
inline void sanitizer_switch_end(SanitizerSwitch& state) noexcept
{
#if CRD_JOBS_ASAN
    __sanitizer_finish_switch_fiber(state.fake_stack, &state.return_bottom, &state.return_size);
#else
    (void)state;
#endif
}

// The first instruction of a fresh fiber stack: no fake stack to restore, but the dispatching stack's bounds.
inline void sanitizer_fiber_entered(SanitizerSwitch& state) noexcept
{
#if CRD_JOBS_ASAN
    __sanitizer_finish_switch_fiber(nullptr, &state.return_bottom, &state.return_size);
#else
    (void)state;
#endif
}

// TSan contexts: one per fiber stack for its lifetime; the current thread's own for the scheduler side.
[[nodiscard]] inline void* sanitizer_create_fiber() noexcept
{
#if CRD_JOBS_TSAN
    return __tsan_create_fiber(0U);
#else
    return nullptr;
#endif
}

inline void sanitizer_destroy_fiber(void* fiber) noexcept
{
#if CRD_JOBS_TSAN
    if (fiber != nullptr)
    {
        __tsan_destroy_fiber(fiber);
    }
#else
    (void)fiber;
#endif
}

[[nodiscard]] inline void* sanitizer_current_fiber() noexcept
{
#if CRD_JOBS_TSAN
    return __tsan_get_current_fiber();
#else
    return nullptr;
#endif
}

} // namespace crd::jobs::detail
