#pragma once

// crd-core -- minimizing a failing reproduction (DIAG.9c).
//
// A reproduction is a candidate (a schedule script, a fault plan's hits, a recorded input) and a run that answers
// whether it failed the way the original did. These helpers shrink a candidate while it still reproduces:
//
//   minimize_sequence   drop single elements, order kept, until no single element can be dropped (1-minimal).
//   minimize_toward_zero shrink an integer toward 0 until 0 does not reproduce and neither does the value one step
//                        closer to 0.
//   bisect_toward       move a known-reproducing bound toward a known-or-assumed non-reproducing one, by halving,
//                        until the two are adjacent.
//
// The oracle is all K runs. A candidate reproduces only when every one of `Budget::runs_per_candidate` runs failed the
// original way; a candidate that failed in some runs and not in others is a race, counted in `Stats::flaky` and
// treated as not reproducing, so a lucky run never shrinks the result. A candidate is never judged on fewer than K
// runs: when the remaining run budget cannot hold K more runs the helper stops with Status::Budget, and the candidate
// it holds is the smallest one judged so far, which still reproduces but is not claimed minimal.
//
// Every helper first judges the input itself. An input that does not reproduce answers Status::NotReproduced and is
// left untouched: minimizing a passing input yields garbage.
//
// Greedy single-element removal, not ddmin. The removal pass drops each element in turn, keeps the drop when the
// candidate reproduces and restarts the pass after any drop; when a whole pass drops nothing the result is 1-minimal.
// ddmin's subset and complement steps form intermediate candidates that keep a later element without the earlier one
// it depends on. For a schedule script (DIAG.4b) such a candidate is a race (a point held by an unscripted predecessor
// and released early), so its verdict is skewed rather than deterministic, and the minimizer itself would stop being
// deterministic. Greedy removal visits only candidates with one element less than one already shown to reproduce.
//
// Bisection assumes the predicate changes once between the two bounds. When it does not, the result still reproduces
// and still has a judged non-reproducing neighbour; it is a local boundary, not the global one.
//
// Header-only and allocation-free apart from the candidates the caller's sequence type makes. `Seq` needs a copy
// constructor, size(), operator[] and erase(index) (crd::containers::Array has them). Contract:
// docs/design/runtime-diagnostics.md (DIAG.9c); ADR-0133.

#include <crd/core/types.hpp>

#include <utility>

namespace crd::minimize
{

enum class Status : u8
{
    Minimal       = 0U, // the result reproduces and no smaller step of the helper does
    NotReproduced = 1U, // the input did not reproduce in all K runs; it is left as given
    Budget        = 2U, // the run budget ran out; the result reproduces but is not claimed minimal
};

// "minimal", "not-reproduced", "budget".
[[nodiscard]] constexpr const char* status_name(Status s) noexcept
{
    switch (s)
    {
    case Status::Minimal:
        return "minimal";
    case Status::NotReproduced:
        return "not-reproduced";
    case Status::Budget:
        return "budget";
    }
    return "unknown";
}

inline constexpr u32 kMaxRunsPerCandidate = 64U;

struct Budget
{
    u32 runs_per_candidate = 3U;    // K, clamped to [1, kMaxRunsPerCandidate]
    u64 max_runs           = 4096U; // runs over the whole minimization, the input's own included
};

struct Stats
{
    u64 candidates = 0U; // candidates judged (each on K runs, or fewer when one run did not reproduce)
    u64 runs       = 0U; // runs made
    u64 reproduced = 0U; // candidates that reproduced in all K runs
    u64 flaky      = 0U; // candidates that reproduced in some runs and not in a later one
    u64 skipped    = 0U; // candidates the caller's admission rejected without running
};

enum class Verdict : u8
{
    Yes    = 0U,
    No     = 1U,
    Budget = 2U,
};

[[nodiscard]] constexpr u32 runs_per_candidate(const Budget& b) noexcept
{
    if (b.runs_per_candidate == 0U)
    {
        return 1U;
    }
    return b.runs_per_candidate > kMaxRunsPerCandidate ? kMaxRunsPerCandidate : b.runs_per_candidate;
}

// Judge one candidate: `run_once(candidate)` makes one run and answers true when it failed the original way.
template <typename Candidate, typename RunOnce>
[[nodiscard]] Verdict judge(const Candidate& candidate, RunOnce& run_once, const Budget& budget, Stats& stats)
{
    const u32 k = runs_per_candidate(budget);
    if (stats.runs > budget.max_runs || budget.max_runs - stats.runs < k)
    {
        return Verdict::Budget;
    }
    ++stats.candidates;
    for (u32 i = 0U; i < k; ++i)
    {
        ++stats.runs;
        if (!run_once(candidate))
        {
            if (i > 0U)
            {
                ++stats.flaky;
            }
            return Verdict::No;
        }
    }
    ++stats.reproduced;
    return Verdict::Yes;
}

// Every candidate is admissible.
struct AdmitAll
{
    template <typename Candidate>
    [[nodiscard]] constexpr bool operator()(const Candidate& /*candidate*/) const noexcept
    {
        return true;
    }
};

// Shrink `items` to a 1-minimal order-preserving subsequence that still reproduces. `admit(candidate)` rejects a
// candidate the caller cannot run (counted in Stats::skipped; it is never judged), so the result is 1-minimal among
// the admissible candidates. On NotReproduced `items` is unchanged; on Budget it holds the smallest candidate judged.
template <typename Seq, typename RunOnce, typename Admit>
[[nodiscard]] Status minimize_sequence(Seq& items, RunOnce run_once, Admit admit, const Budget& budget, Stats& stats)
{
    const Verdict first = judge(items, run_once, budget, stats);
    if (first == Verdict::No)
    {
        return Status::NotReproduced;
    }
    if (first == Verdict::Budget)
    {
        return Status::Budget;
    }
    bool changed = true;
    while (changed)
    {
        changed = false;
        for (usize i = 0U; i < items.size(); ++i)
        {
            Seq candidate(items);
            candidate.erase(i);
            if (!admit(candidate))
            {
                ++stats.skipped;
                continue;
            }
            const Verdict v = judge(candidate, run_once, budget, stats);
            if (v == Verdict::Budget)
            {
                return Status::Budget;
            }
            if (v == Verdict::Yes)
            {
                items   = std::move(candidate);
                changed = true;
                break; // the indices shifted: restart the pass
            }
        }
    }
    return Status::Minimal;
}

template <typename Seq, typename RunOnce>
[[nodiscard]] Status minimize_sequence(Seq& items, RunOnce run_once, const Budget& budget, Stats& stats)
{
    return minimize_sequence(items, std::move(run_once), AdmitAll{}, budget, stats);
}

// Move `yes` (a value already judged to reproduce) toward `no` (judged not to, or excluded and never judged) by
// halving the gap; `probe(value)` judges a value strictly between them and answers a Verdict. Ends with `yes` and
// `no` adjacent (Minimal) or when the budget runs out (Budget); `yes` reproduces either way.
template <typename Probe>
[[nodiscard]] Status bisect_toward(u64& yes, u64 no, Probe probe)
{
    while ((yes > no ? yes - no : no - yes) > 1U)
    {
        const u64     mid = yes > no ? no + (yes - no) / 2U : yes + (no - yes) / 2U;
        const Verdict v   = probe(mid);
        if (v == Verdict::Budget)
        {
            return Status::Budget;
        }
        if (v == Verdict::Yes)
        {
            yes = mid;
        }
        else
        {
            no = mid;
        }
    }
    return Status::Minimal;
}

// The signed value with sign `negative` and magnitude `m` (m <= 2^63 when negative, m < 2^63 otherwise).
[[nodiscard]] constexpr i64 signed_magnitude(bool negative, u64 m) noexcept
{
    return negative ? static_cast<i64>(0U - m) : static_cast<i64>(m);
}

// Shrink `value` toward 0 while `run_once(candidate_value)` still reproduces: 0 is tried first, then the magnitude is
// bisected, keeping the sign. Minimal: 0 reproduces, or the value one step closer to 0 was judged not to.
template <typename RunOnce>
[[nodiscard]] Status minimize_toward_zero(i64& value, RunOnce run_once, const Budget& budget, Stats& stats)
{
    const Verdict first = judge(value, run_once, budget, stats);
    if (first == Verdict::No)
    {
        return Status::NotReproduced;
    }
    if (first == Verdict::Budget)
    {
        return Status::Budget;
    }
    if (value == 0)
    {
        return Status::Minimal;
    }
    const i64     zero = 0;
    const Verdict at_0 = judge(zero, run_once, budget, stats);
    if (at_0 == Verdict::Budget)
    {
        return Status::Budget;
    }
    if (at_0 == Verdict::Yes)
    {
        value = 0;
        return Status::Minimal;
    }
    const bool negative = value < 0;
    u64        yes      = negative ? 0U - static_cast<u64>(value) : static_cast<u64>(value);
    const auto probe    = [&](u64 m)
    {
        const i64 candidate = signed_magnitude(negative, m);
        return judge(candidate, run_once, budget, stats);
    };
    const Status s = bisect_toward(yes, 0U, probe);
    value          = signed_magnitude(negative, yes);
    return s;
}

} // namespace crd::minimize
