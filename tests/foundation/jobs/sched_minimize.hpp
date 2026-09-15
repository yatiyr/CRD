#pragma once

// Delta-minimization of a failing yield-point script for DIAG.4b. Given a script that reproduces a
// violation and an oracle reproduces(script) -> bool, shrink it to a 1-MINIMAL subsequence that still
// reproduces: no single remaining tag can be dropped without losing reproduction. Order-preserving.
//
// Strategy: greedy single-tag removal (repeatedly drop each remaining tag, keep the drop iff the script
// still reproduces, restart the pass after any drop) rather than ddmin's subset/complement split. This is
// deliberate and load-bearing for THIS failure: the reclamation is exposed only while the scheduler is
// HELD at fp.finalizing until the test re-acquires the slot, and what holds it is an earlier scripted tag
// (test.reacquired). ddmin's complement step would form intermediate subsets like [wait.resumed,
// fp.finalizing] -- fp.finalizing held by an unscripted predecessor, then released the instant
// wait.resumed is consumed, i.e. BEFORE the test re-acquires -- whose reproduction is a race (skewed to
// not-found), which would make the minimizer itself nondeterministic. Greedy removes tags left-to-right,
// so by the time it tries dropping test.reacquired the earlier wait.resumed is already gone: every subset
// it visits is deterministically found or not-found. Same 1-minimal guarantee, deterministic oracle.
//
// Test-only; gated with the scheduler hooks. Empty when the gate is off.

#include "../../../engine/foundation/jobs/src/sched_check.hpp"

#if CRD_JOBS_SCHED_CHECK

#include <cstddef>
#include <utility>
#include <vector>

namespace crd::jobs::test
{

// Precondition: reproduces(script) is true (assert it at the call site -- minimizing a non-failing input
// yields garbage). `reproduces` should be an all-K-runs-must-find oracle so a race never reads as a
// reproduction. Returns a 1-minimal order-preserving subsequence.
template <typename Reproduces>
std::vector<const char*> minimize_failure(std::vector<const char*> script, Reproduces reproduces)
{
    bool changed = true;
    while (changed)
    {
        changed = false;
        for (std::size_t i = 0U; i < script.size(); ++i)
        {
            std::vector<const char*> candidate;
            candidate.reserve(script.size());
            for (std::size_t j = 0U; j < script.size(); ++j)
            {
                if (j != i)
                    candidate.push_back(script[j]);
            }
            if (reproduces(candidate))
            {
                script  = std::move(candidate);
                changed = true;
                break; // indices shifted; restart the pass
            }
        }
    }
    return script;
}

} // namespace crd::jobs::test

#endif
