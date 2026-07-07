#pragma once

// revolve.hpp — Phase 3.1.6 v16-f: REVOLVE / treeverse checkpointing (Griewank-Walther, ACM TOMS Alg. 799). To
// reverse a T-step computation (the reverse/adjoint pass of a time integration), storing every intermediate state is
// O(T) memory; revolve keeps only `snaps` checkpoints and RECOMPUTES forward segments on demand, trading a provably
// minimal number of extra forward steps for O(snaps) = O(log T) memory. The schedule is STATIC + WCET-analyzable (the
// certification pillar): the recompute count is bounded by the binomial `beta(snaps,r)` and is exactly optimal here.
//
// The split point is chosen by a memoized DYNAMIC PROGRAM over the treeverse cost — cost(len,s) = min_d [ d +
// cost(len−d, s−1) + cost(d, s) ] — which IS the definition of the optimal offline checkpointing, so `revolve` here is
// GW-optimal by construction (no reliance on the closed-form mid formula). Deterministic, allocation-light (the DP
// table is caller-owned). ADR-0097.
//
// Driver contract — `revolve(T, snaps, advance, store, restore, reverse)` reverses steps T−1..0 assuming the working
// state starts at step 0. Callbacks:
//   advance(from, to)  : advance the working state from step `from` to step `to` (to > from), forward.
//   store(slot)        : save the working state (at its current step) into checkpoint `slot` (0 ≤ slot < snaps).
//   restore(slot)      : load the working state from checkpoint `slot`.
//   reverse(step)      : do the ADJOINT of step `step` (the working state must be AT `step`, i.e. its input).
// The working state is left at step 0 on return; every step's `reverse` is called exactly once, in decreasing order.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>

namespace crd::hesap::autodiff::reverse
{

// Optimal treeverse cost + split, memoized in caller-owned tables cost/arg (each (T+1)*(snaps+1), row-major).
// cost(len,s) = extra forward-step advances to reverse `len` steps with `s` checkpoints; arg(len,s) = the optimal split.
class RevolvePlan
{
public:
    // tables: cost[(T+1)*(S+1)], arg[(T+1)*(S+1)] (caller-owned). Fills them for all len≤T, s≤S.
    RevolvePlan(int t_max, int snaps, crd::i64* cost, int* arg) noexcept
        : m_cost(cost), m_arg(arg), m_tmax(t_max), m_snaps(snaps)
    {
        const int stride = snaps + 1;
        for (int len = 0; len <= t_max; ++len)
        {
            for (int s = 0; s <= snaps; ++s)
            {
                constexpr crd::i64 inf_cost = static_cast<crd::i64>(1) << 60;
                crd::i64           c    = 0;
                int                d    = 0;
                if (len <= 1) { c = 0; d = 0; }
                else if (s == 0)
                {
                    // INFEASIBLE: with no checkpoint, once we advance past `lo` its state is lost — a len>1 range can
                    // never be reversed. The DP therefore forbids splitting into an s=0 sub-range of length > 1.
                    c = inf_cost;
                    d = len - 1;
                }
                else
                {
                    crd::i64 best = inf_cost;
                    for (int dd = 1; dd < len; ++dd) // advance dd, right=[dd..len) with s−1, left=[0..dd) with s
                    {
                        const crd::i64 right = m_cost[(len - dd) * stride + (s - 1)];
                        if (right >= inf_cost) { continue; } // right sub-range not reversible with s−1 snaps
                        const crd::i64 cc = static_cast<crd::i64>(dd) + right + m_cost[dd * stride + s];
                        if (cc < best) { best = cc; d = dd; } // dd=len−1 (right length 1) is always feasible
                    }
                    c = best;
                }
                m_cost[len * stride + s] = c;
                m_arg[len * stride + s]  = d;
            }
        }
    }
    [[nodiscard]] int      split(int len, int s) const noexcept { return m_arg[len * (m_snaps + 1) + s]; }
    [[nodiscard]] crd::i64 recompute(int len, int s) const noexcept { return m_cost[len * (m_snaps + 1) + s]; }

private:
    crd::i64* m_cost;
    int*      m_arg;
    int       m_tmax;
    int       m_snaps;
};

namespace detail
{
template <class Adv, class Store, class Restore, class Rev>
inline void treeverse(const RevolvePlan& plan, int lo, int hi, int s, int slot, const Adv& advance,
                      const Store& store, const Restore& restore, const Rev& reverse) noexcept
{
    const int len = hi - lo;
    if (len == 1)
    {
        reverse(lo); // working state is at lo (its input) — do the adjoint of step lo
        return;
    }
    // s >= 1 here: the DP's split guarantees any s=0 sub-range has length 1 (handled by the base case above), so a
    // live checkpoint always exists for the range's input.
    const int off = plan.split(len, s);
    const int mid = lo + off;
    store(slot);          // checkpoint the state at lo
    advance(lo, mid);     // advance working state lo → mid
    treeverse(plan, mid, hi, s - 1, slot + 1, advance, store, restore, reverse); // reverse [mid,hi)
    restore(slot);        // working state back to lo
    treeverse(plan, lo, mid, s, slot, advance, store, restore, reverse);         // reverse [lo,mid)
}
} // namespace detail

// Reverse steps T−1..0 with `snaps` checkpoints, GW-optimal. `plan` built for (T, snaps).
template <class Adv, class Store, class Restore, class Rev>
inline void revolve(const RevolvePlan& plan, int T, int snaps, const Adv& advance, const Store& store,
                    const Restore& restore, const Rev& reverse) noexcept
{
    if (T <= 0) { return; }
    detail::treeverse(plan, 0, T, snaps, 0, advance, store, restore, reverse);
}

} // namespace crd::hesap::autodiff::reverse
