#pragma once
// CEIR-11c part 2 — the crd-perf BRIDGE adapter for the compiled-plan profiling seam. ⛔ crd-ceir core CANNOT link
// crd-perf (I5 + it PUBLIC-links crd-jobs → the jobs-free invariant), so the core exposes a null-default `RunHooks` seam
// (plan.hpp) and THIS bridge — which MAY link crd-perf — owns the crd-perf calls (the §112 StepHook / crd/perf
// `jobs_adapter` inversion pattern). `profiled_run` wires the seam to a per-Op-class dispatch-count + parent-pause
// SELF-TIME profile inside a `CRD_PERF_SCOPE("ceir.plan.run")` region; `profiled_compile` wraps `compile()` in a
// `CRD_PERF_SCOPE("ceir.plan.compile")` region + publishes the plan-shape stats as counters.
//
// ⭐ The volume ruling (advisor): a crd-perf REGION per dispatched instr would overflow the 4096-slot ring at fuel
// 1<<24 (silent sample drops — a no-silent-caps violation). So the adapter AGGREGATES per Op class (counters + self-time,
// atomic slots — ring-safe); a region-driving consumer stays possible through the same seam (that is what
// "CRD_PERF_SCOPE-compatible" means).

#include <crd/ceir/plan.hpp>
#include <crd/core/types.hpp>

namespace crd::ceir::host
{
// A per-Op-class profile — dense-indexed by the `plan::Op` enum value. ⛔ A POD (no crd-perf / crd-time types leak here);
// filled by the seam hooks (via `crd::time::MonotonicClock`, always available — so the profile fills even on a perf-OFF
// build; only the crd-perf regions/counters are compiled out when profiling is off).
struct PlanProfile
{
    static constexpr crd::u32 kMaxOps = 32U;      // >= the plan::Op count (indexed by u8(op))
    crd::u64                  dispatch[kMaxOps] = {}; // dispatch count per Op class
    crd::f64                  self_s[kMaxOps]   = {}; // SELF-time (seconds; parent-pause — excludes children) per Op class
    crd::u64                  total_dispatch    = 0U; // == the seam's pre count on a successful run
    crd::u32                  max_depth         = 0U; // deepest control-flow / call nesting reached
    crd::u64                  depth_overflow    = 0U; // ⛔ witness: the parent-pause stack overflowed (NO silent cap)
};

// Run `plan` with the profiling seam wired to `out` (reset per call). A `CRD_PERF_SCOPE("ceir.plan.run")` region wraps it.
[[nodiscard]] plan::RunResult profiled_run(const plan::CompiledPlan& plan, containers::ConstSpan<crd::i64> args,
                                           memory::IAllocator* alloc, PlanProfile& out);

// Compile `entry` inside a `CRD_PERF_SCOPE("ceir.plan.compile")` region + publish the plan-shape stats as crd-perf counters.
[[nodiscard]] plan::CompileResult profiled_compile(Context& ctx, const Module& module, containers::StringView entry,
                                                   memory::IAllocator* alloc);
} // namespace crd::ceir::host
