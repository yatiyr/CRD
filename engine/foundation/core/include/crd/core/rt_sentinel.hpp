#pragma once

// Portable real-time sentinels: a declared real-time scope (an audio callback, a deadline-bound task) must not
// allocate or block. Wrapping such a region in an RtScope makes the crd allocators and blocking primitives
// REPORT a forbidden operation instead of silently permitting it -- a diagnostic, never a behaviour change.
//
// Layering: the scope flag and reporting live in crd::core, below both crd::memory (which raises the allocation
// violation from its allocation seam) and crd::jobs (which raises the blocking violation from wait()); neither
// of those depends on the other, so the shared state cannot live in either. There is deliberately NO default
// action -- with no handler installed nothing happens (mirrors the jobs watchdog); a handler decides what to do
// (log, break, count). The checks are CRD_ENABLE_ASSERTS-only, so a shipping build pays nothing; comprehensive
// release-time coverage is RTSan's job after tool qualification (see ADR-0133 and the runtime-diagnostics
// design doc's real-time sentinels section).

#include <crd/core/build_config.hpp>
#include <crd/core/types.hpp>

namespace crd
{

enum class RtViolationKind : crd::u8
{
    Allocation, // a memory allocation happened inside a declared real-time scope
    Blocking,   // a blocking wait happened inside a declared real-time scope
};

struct RtViolation
{
    RtViolationKind kind;
    crd::usize      bytes; // allocation size for Allocation; 0 for Blocking
};

using RtViolationHandler = void (*)(const RtViolation& violation, void* user);

// Install (or clear, with nullptr) the RT-violation handler. Stored atomically; safe to call at any time.
// Available in every build so caller code compiles unchanged; in a build without CRD_ENABLE_ASSERTS the checks
// never run, so the handler is simply never invoked.
void set_rt_violation_handler(RtViolationHandler handler, void* user) noexcept;

#if CRD_ENABLE_ASSERTS

// True while at least one RtScope is active on the calling thread.
[[nodiscard]] bool in_rt_scope() noexcept;

// Report a forbidden operation observed inside an RT scope. Delegates to the installed handler (if any) and is
// re-entrancy safe: a handler that itself allocates or blocks will not recurse back into another report.
void report_rt_violation(RtViolationKind kind, crd::usize bytes) noexcept;

// RAII marker for a declared real-time scope. Nesting is a thread-local depth; the checks fire while depth > 0.
//
// INVARIANT: an RT scope must not span a fiber park (jobs::wait). Such a park is itself the Blocking violation
// this class exists to catch AND the thread-local depth would not travel with a fiber that resumes on another
// OS thread. Keep RT scopes leaf-level (no jobs::wait inside). A fiber-local depth via a core hook is the
// follow-up if a legitimate case ever needs an RT scope to survive a park.
class RtScope
{
public:
    RtScope() noexcept;
    ~RtScope() noexcept;
    RtScope(const RtScope&)            = delete;
    RtScope& operator=(const RtScope&) = delete;
    RtScope(RtScope&&)                 = delete;
    RtScope& operator=(RtScope&&)      = delete;
};

#else // !CRD_ENABLE_ASSERTS -- zero-cost no-ops so RT-annotated code compiles identically in shipping builds.

[[nodiscard]] inline bool in_rt_scope() noexcept { return false; }
inline void               report_rt_violation(RtViolationKind /*kind*/, crd::usize /*bytes*/) noexcept {}

class RtScope
{
public:
    RtScope() noexcept                 = default;
    ~RtScope() noexcept                = default;
    RtScope(const RtScope&)            = delete;
    RtScope& operator=(const RtScope&) = delete;
    RtScope(RtScope&&)                 = delete;
    RtScope& operator=(RtScope&&)      = delete;
};

#endif // CRD_ENABLE_ASSERTS

} // namespace crd
