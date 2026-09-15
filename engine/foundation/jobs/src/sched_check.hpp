#pragma once

// Test-only controlled-interleaving hooks. A scheduler YIELD POINT: gate off (the default, and always
// in production) it expands to nothing; gate on with an oracle installed, sched_point() calls into the
// oracle, which may run free (return immediately) or block the calling thread until a driver releases
// it -- letting a test serialize threads through the concurrency-sensitive steps in any order, i.e.
// controlled interleavings. The primitive is a yield point, NOT a choice-among-n: the target races
// (publication/reclamation) are about which thread's atomic step runs next -- preemption, not selection.
//
// Zero production cost: with CRD_JOBS_SCHED_CHECK == 0 the macro is ((void)0) and this file declares
// nothing. The gate is a build option (see the jobs CMakeLists); the #ifndef below is the safe default
// for any translation unit compiled without that definition.

#ifndef CRD_JOBS_SCHED_CHECK
#define CRD_JOBS_SCHED_CHECK 0
#endif

#if CRD_JOBS_SCHED_CHECK

#include <crd/core/types.hpp>

namespace crd::jobs::detail
{

// Function-pointer table (like JobObserver) invoked at every yield point -- no virtuals, no allocation.
struct SchedOracle
{
    void (*on_point)(void* user, const char* tag) noexcept = nullptr;
    void*  user                                            = nullptr;
};

// Install / read the process-wide oracle (atomic acquire/release, like set_observer). nullptr disables.
void                             set_sched_oracle(const SchedOracle* oracle) noexcept;
[[nodiscard]] const SchedOracle* current_sched_oracle() noexcept;

// A yield point named `tag`: load the oracle, null-check, call on_point. No-op when no oracle is set.
void sched_point(const char* tag) noexcept;

// Violation channel. A gate-only invariant detector on the publication/reclamation path (see
// counter_finish_park) reports a broken interleaving HERE rather than faulting. Rationale: the symptom
// of the broken variant is a counter slot recycled out from under a half-finished park -- a
// use-after-read of recycled memory, which does not fault in a debug build; and an in-process CRD_FATAL
// cannot be asserted on by the very test that provoked it. So "found" is a counter the test reads back,
// not a crash. The repaired algorithm never reports (the park_finalized handshake forbids the recycle);
// only a deliberately broken variant, driven into the vulnerable window, does.
void                   sched_check_note_violation(const char* tag) noexcept;
[[nodiscard]] crd::u64 sched_check_violations() noexcept;
void                   sched_check_reset_violations() noexcept;

// Deliberately-broken publication/reclamation variant, in the SAME binary (no per-variant rebuild). When
// set, counter_wait skips the park_finalized spin AND counter_finish_park skips the park_finalized store
// (so no stale write lands on an unwound fiber stack): the handshake that forbids a parked slot being
// recycled mid-park is removed, and the detector above reports the resulting reclamation. Default false =
// the repaired algorithm. Gate-only; the production build has no such switch. A test flips it on around a
// forced interleaving, then off (use an RAII guard — Catch2 randomizes case order).
void               sched_check_break_handshake(bool broken) noexcept;
[[nodiscard]] bool sched_check_handshake_broken() noexcept;

} // namespace crd::jobs::detail

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): a compile-time gate that must vanish when off cannot be a function.
#define CRD_JOBS_SCHED_POINT(tag) ::crd::jobs::detail::sched_point(tag)

#else

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): the gate-off form must expand to nothing at all sites.
#define CRD_JOBS_SCHED_POINT(tag) ((void)0)

#endif
