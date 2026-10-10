#pragma once

// crd-perf -- minimizing a fault plan that reproduces a failure (DIAG.9c).
//
// A plan that makes a run fail (a fault.arm plan, a recipe's plan, an authored fault-plan asset) usually covers more
// than the failure needs: rules that fire without consequence, wide hit ranges, a seeded share of hits. Given the
// caller's run of the reproduction, minimize_fault_plan returns a smaller plan that still reproduces it under the
// all-K-runs oracle of crd/core/minimize.hpp (a candidate counts only when every one of K runs fails the original way).
// It never changes what a fault does (its site and value); it only removes faults:
//
//   1. The input plan is judged first; a plan that does not reproduce is returned unchanged (NotReproduced).
//   2. Each rule's range is narrowed, in rule order. An unbounded rule (hit_limit 0) is first bounded at the hits its
//      site counted in the last reproducing run, and kept unbounded if that does not reproduce. Then its first hit is
//      raised and its last hit lowered by bisection, so moving either end one hit inward was judged not to reproduce.
//      Rule indices do not change in this step, so a seeded rule keeps firing on the same hits inside its range.
//   3. The plan is pinned: every hit a bounded rule fires on is enumerated with fault_plan_fires, and when there are at
//      most kFaultMaxPinnedHits and they fit in kFaultMaxRules rules, the plan made of exactly those hits (each rule
//      always firing, consecutive hits of one site and value merged, seed 0) replaces it if it reproduces. The seed
//      then no longer matters: the result names the faulting hits themselves.
//   4. Faults are removed by greedy single-element removal: single hits of a pinned plan (a candidate that needs more
//      than kFaultMaxRules rules is skipped, never run), otherwise whole rules. The result is 1-minimal over those
//      elements: dropping any one of them was judged not to reproduce.
//
// Hit ordinals count from the arm, so the caller's run must arm each candidate (FaultInjector::arm restarts every
// count) and start the reproduction from the same state each time. The candidates are deterministic functions of the
// plan; a reproduction that is not deterministic is what the all-K oracle is for.
//
// Pure apart from the caller's runs: it needs no fault seams, so it runs in every build (a shipping run cannot arm, so
// its reproduction answers NotReproduced). Contract: docs/design/runtime-diagnostics.md (DIAG.9c); ADR-0133.

#include <crd/perf/fault_injection.hpp>

#include <crd/core/minimize.hpp>
#include <crd/core/types.hpp>

namespace crd::perf
{

// The hits each site counted during one run (FaultInjector::hits after it).
struct FaultRunHits
{
    crd::u64 hits[kFaultSiteCount] = {};
};

// One run of the reproduction under `plan`: arm it, run, fill `hits` and answer true when the run failed the way the
// original did.
using FaultRunOnce = bool (*)(void* user, const FaultPlan& plan, FaultRunHits& hits);

inline constexpr crd::u32 kFaultMaxPinnedHits = 64U;      // fired hits a plan is pinned to at most
inline constexpr crd::u64 kFaultMaxPinScan    = 1U << 20U; // hits enumerated while pinning at most

struct FaultMinimizeResult
{
    minimize::Status status     = minimize::Status::NotReproduced;
    FaultPlanError   plan_error = FaultPlanError::None; // the input plan's validation error (nothing was run)
    FaultPlan        plan;      // the smallest plan judged to reproduce (the input plan when NotReproduced)
    minimize::Stats  stats;
    bool             pinned = false; // the plan names its fired hits, every rule always fires, the seed is 0
    crd::u32         hits   = 0U;    // pinned: the hits the plan fires on
};

// Minimize `plan` against `run`. `out.plan` always reproduces unless the status is NotReproduced, which also answers a
// reproduction that stopped reproducing while it was minimized (the plan the removal step started from was judged
// again and failed): `out.plan` is then the input plan.
void minimize_fault_plan(const FaultPlan& plan, FaultRunOnce run, void* user, const minimize::Budget& budget,
                         FaultMinimizeResult& out);

} // namespace crd::perf
