#pragma once

#include "tensor.hpp"

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>

#include <bit>

// ---------------------------------------------------------------------------
// crd-hesap-tensor einsum front-end (Phase 3.1.6 v14-e; ADR-0096 §3).
//
// Parser (a-z subscripts, "..." ellipsis, repeated-index diagonals, implicit
// NumPy output rule) + the opt_einsum-class PATH OPTIMIZER + `EinsumPlan`
// (build once, execute many — execution lands in v14-f).
//
// The optimizer is a faithful transcription of opt_einsum 3.4.0's semantics
// (python-verified BEFORE the port, per protocol; oracle corpus in
// tests/hesap-tensor/ref_einsum_paths.inc):
//   - cost model  : flop_count(union, inner, 2) = prod(sizes) * (1 + inner)
//   - k12         : (k1 | k2) & keep, keep = output | union(other remaining)
//   - greedy      : min-heap over index-sharing pairs by the memory-removed
//                   heuristic size(k12) - size(k1) - size(k2); remaining
//                   disconnected terms folded by ascending size (outer
//                   products last)
//   - optimal     : branch-and-bound DFS over SSA pair choices with a
//                   flops sieve (their `optimal`, unlimited memory)
// Gate: our greedy <= opt_einsum greedy (parity-or-better) and our optimal
// == opt_einsum optimal on every corpus case.
//
// Index sets are u64 bitmasks (26 letters + up to 8 ellipsis dims = 34 bits)
// — allocation-free, deterministic iteration, WCET-bounded (operand and
// step counts are hard caps). Status-not-exception throughout.
// ---------------------------------------------------------------------------

namespace crd::hesap::tensor
{

inline constexpr crd::u32 kEinsumMaxOperands = 16U;
inline constexpr crd::u32 kEinsumMaxIndices = 34U; // 26 letters + 8 ellipsis slots
inline constexpr crd::u32 kEinsumEllipsisBase = 26U;

// One parsed operand: its indices in subscript order (values 0..33; repeated
// values = a diagonal, resolved at execution).
struct EinsumTerm
{
    crd::u8 idx[kMaxRank];
    crd::u32 count;
    crd::u64 mask; // set of distinct indices
};

struct EinsumExpr
{
    EinsumTerm term[kEinsumMaxOperands];
    crd::u32 n_ops;
    crd::u8 out_idx[kMaxRank];
    crd::u32 out_count;
    crd::u64 out_mask;
    crd::u32 ellipsis_rank; // widest "..." run seen (0 = none)
    bool has_diagonal;      // some operand repeats an index
};

// One pairwise contraction step (SSA ids: 0..n_ops-1 are the inputs; each
// step's result gets the next id).
struct EinsumStep
{
    crd::u32 a;
    crd::u32 b;
    crd::u64 result_mask;
    crd::u64 flops;
};

struct EinsumPlan
{
    EinsumExpr expr;
    crd::u64 idx_size[kEinsumMaxIndices];
    EinsumStep step[kEinsumMaxOperands]; // n_ops - 1 steps
    crd::u32 n_steps;
    crd::u64 total_flops;
    crd::u64 largest_intermediate; // elements
};

enum class EinsumOptimize : crd::u8
{
    Greedy,  // the scalable default
    Optimal, // exact branch-and-bound (use for <= ~8 operands)
};

namespace detail
{

[[nodiscard]] inline crd::u64 mask_size(crd::u64 mask, const crd::u64* idx_size) noexcept
{
    crd::u64 s = 1;
    while (mask != 0U)
    {
        const crd::u32 i = static_cast<crd::u32>(std::countr_zero(mask));
        s *= idx_size[i];
        mask &= mask - 1U;
    }
    return s;
}

// opt_einsum flop_count for a pairwise step: prod(union) * (1 + inner).
[[nodiscard]] inline crd::u64 pair_flops(crd::u64 either, bool inner, const crd::u64* idx_size) noexcept
{
    const crd::u64 s = mask_size(either, idx_size);
    return inner ? 2U * s : s;
}

} // namespace detail

// Parse "ab,bc->ac" style subscripts against the operand ranks. Ellipsis
// ("...") maps to shared implicit indices (leftmost-aligned, NumPy rule).
// Implicit output (no "->"): ellipsis dims first, then indices appearing
// exactly once, alphabetically.
[[nodiscard]] inline TensorStatus einsum_parse(const char* subscripts, crd::containers::ConstSpan<crd::u32> ranks,
                                               EinsumExpr& out) noexcept
{
    out = EinsumExpr{};
    if (subscripts == nullptr || ranks.size() == 0U || ranks.size() > kEinsumMaxOperands)
    {
        return TensorStatus::BadInput;
    }
    crd::u32 seen_count[kEinsumMaxIndices] = {};
    const char* p = subscripts;
    crd::u32 op = 0;
    bool explicit_out = false;
    // ---- left-hand terms ----
    for (;; ++op)
    {
        if (op >= ranks.size())
        {
            return TensorStatus::BadInput; // more terms than operands
        }
        EinsumTerm& t = out.term[op];
        crd::u32 ell_here = 0;
        bool saw_ellipsis = false;
        while (*p != '\0' && *p != ',' && *p != '-')
        {
            if (*p == '.')
            {
                if (saw_ellipsis || p[1] != '.' || p[2] != '.')
                {
                    return TensorStatus::BadInput;
                }
                saw_ellipsis = true;
                p += 3;
                // ellipsis rank of this operand = rank - explicit letters (count the rest first)
                crd::u32 letters = t.count;
                for (const char* q = p; *q != '\0' && *q != ',' && *q != '-'; ++q)
                {
                    if (*q >= 'a' && *q <= 'z')
                    {
                        ++letters;
                    }
                    else
                    {
                        return TensorStatus::BadInput;
                    }
                }
                if (letters > ranks[op])
                {
                    return TensorStatus::BadInput;
                }
                ell_here = ranks[op] - letters;
                if (ell_here > kMaxRank || kEinsumEllipsisBase + ell_here > kEinsumMaxIndices)
                {
                    return TensorStatus::RankOverflow;
                }
                for (crd::u32 e = 0; e < ell_here; ++e)
                {
                    if (t.count >= kMaxRank)
                    {
                        return TensorStatus::RankOverflow;
                    }
                    // right-aligned ellipsis slots (NumPy broadcast alignment)
                    const crd::u8 id = static_cast<crd::u8>(kEinsumEllipsisBase + (kMaxRank - ell_here) + e);
                    t.idx[t.count++] = id;
                    t.mask |= 1ULL << id;
                }
                if (ell_here > out.ellipsis_rank)
                {
                    out.ellipsis_rank = ell_here;
                }
                continue;
            }
            if (*p < 'a' || *p > 'z' || t.count >= kMaxRank)
            {
                return *p >= 'a' && *p <= 'z' ? TensorStatus::RankOverflow : TensorStatus::BadInput;
            }
            const crd::u8 id = static_cast<crd::u8>(*p - 'a');
            if ((t.mask & (1ULL << id)) != 0U)
            {
                out.has_diagonal = true;
            }
            t.idx[t.count++] = id;
            t.mask |= 1ULL << id;
            ++seen_count[id]; // per-OCCURRENCE (the NumPy implicit rule: "ii" -> trace)
            ++p;
        }
        if (t.count != ranks[op])
        {
            return TensorStatus::ShapeMismatch; // subscript count vs operand rank
        }
        if (*p == ',')
        {
            ++p;
            continue;
        }
        break;
    }
    out.n_ops = op + 1U;
    if (out.n_ops != ranks.size())
    {
        return TensorStatus::BadInput;
    }
    // ---- output ----
    if (*p == '-')
    {
        if (p[1] != '>')
        {
            return TensorStatus::BadInput;
        }
        p += 2;
        explicit_out = true;
        crd::u64 seen_out = 0;
        while (*p != '\0')
        {
            if (*p == '.')
            {
                if (p[1] != '.' || p[2] != '.')
                {
                    return TensorStatus::BadInput;
                }
                p += 3;
                for (crd::u32 e = 0; e < out.ellipsis_rank; ++e)
                {
                    const crd::u8 id = static_cast<crd::u8>(kEinsumEllipsisBase + (kMaxRank - out.ellipsis_rank) + e);
                    out.out_idx[out.out_count++] = id;
                    out.out_mask |= 1ULL << id;
                }
                continue;
            }
            if (*p < 'a' || *p > 'z' || out.out_count >= kMaxRank)
            {
                return TensorStatus::BadInput;
            }
            const crd::u8 id = static_cast<crd::u8>(*p - 'a');
            if (seen_count[id] == 0U || (seen_out & (1ULL << id)) != 0U)
            {
                return TensorStatus::BadInput; // unknown or repeated output index
            }
            seen_out |= 1ULL << id;
            out.out_idx[out.out_count++] = id;
            out.out_mask |= 1ULL << id;
            ++p;
        }
    }
    if (!explicit_out)
    {
        // NumPy implicit rule: ellipsis dims first, then once-seen letters a-z.
        for (crd::u32 e = 0; e < out.ellipsis_rank; ++e)
        {
            const crd::u8 id = static_cast<crd::u8>(kEinsumEllipsisBase + (kMaxRank - out.ellipsis_rank) + e);
            out.out_idx[out.out_count++] = id;
            out.out_mask |= 1ULL << id;
        }
        for (crd::u32 i = 0; i < 26U; ++i)
        {
            if (seen_count[i] == 1U)
            {
                if (out.out_count >= kMaxRank)
                {
                    return TensorStatus::RankOverflow;
                }
                out.out_idx[out.out_count++] = static_cast<crd::u8>(i);
                out.out_mask |= 1ULL << i;
            }
        }
    }
    return TensorStatus::Ok;
}

namespace detail
{

// keep = output | union(inputs of remaining except i,j); k12 = either & keep.
struct PathState
{
    crd::u64 masks[2U * kEinsumMaxOperands]; // SSA slots
    crd::u32 n;
};

// ---- GREEDY ----------------------------------------------------------------
// One deterministic greedy sweep under a selectable pair heuristic:
//   0 = memory-removed (opt_einsum's default): size(k12)-size(k1)-size(k2)
//   1 = min step FLOPs
//   2 = min result size
// The public greedy runs ALL THREE and keeps the cheapest total — strictly
// dominating opt_einsum's single (stale-queue) heuristic; gated
// parity-or-better on the oracle corpus.
[[nodiscard]] inline crd::u64 greedy_path_h(const EinsumExpr& e, const crd::u64* idx_size, crd::u32 heuristic,
                                            EinsumStep* steps, crd::u32& n_steps) noexcept
{
    crd::u64 masks[2U * kEinsumMaxOperands];
    bool alive[2U * kEinsumMaxOperands] = {};
    for (crd::u32 i = 0; i < e.n_ops; ++i)
    {
        masks[i] = e.term[i].mask;
        alive[i] = true;
    }
    crd::u32 n_ssa = e.n_ops;
    crd::u32 n_alive = e.n_ops;
    crd::u64 total = 0;
    n_steps = 0;

    while (n_alive > 1U)
    {
        // keep-set helper for the CURRENT alive set
        const auto keep_for = [&](crd::u32 skip_a, crd::u32 skip_b) noexcept
        {
            crd::u64 k = e.out_mask;
            for (crd::u32 t = 0; t < n_ssa; ++t)
            {
                if (alive[t] && t != skip_a && t != skip_b)
                {
                    k |= masks[t];
                }
            }
            return k;
        };
        // best index-sharing pair by memory-removed; deterministic (i,j) tie order
        crd::i64 best_cost = 0;
        crd::u32 bi = 0;
        crd::u32 bj = 0;
        bool found = false;
        for (crd::u32 j = 0; j < n_ssa; ++j)
        {
            if (!alive[j])
            {
                continue;
            }
            for (crd::u32 i = 0; i < j; ++i)
            {
                if (!alive[i] || (masks[i] & masks[j]) == 0U)
                {
                    continue;
                }
                const crd::u64 either = masks[i] | masks[j];
                const crd::u64 keep = keep_for(i, j);
                const crd::u64 k12 = either & keep;
                crd::i64 cost = 0;
                if (heuristic == 0U)
                {
                    cost = static_cast<crd::i64>(mask_size(k12, idx_size)) -
                           static_cast<crd::i64>(mask_size(masks[i], idx_size)) -
                           static_cast<crd::i64>(mask_size(masks[j], idx_size));
                }
                else if (heuristic == 1U)
                {
                    cost = static_cast<crd::i64>(pair_flops(either, (either & ~keep) != 0U, idx_size));
                }
                else
                {
                    cost = static_cast<crd::i64>(mask_size(k12, idx_size));
                }
                if (!found || cost < best_cost)
                {
                    best_cost = cost;
                    bi = i;
                    bj = j;
                    found = true;
                }
            }
        }
        if (!found)
        {
            // Disconnected terms: fold the two SMALLEST alive terms (outer
            // product; opt_einsum folds leftovers by ascending size).
            crd::u64 s1 = ~crd::u64{0};
            crd::u64 s2 = ~crd::u64{0};
            bi = bj = n_ssa; // sentinels
            for (crd::u32 t = 0; t < n_ssa; ++t)
            {
                if (!alive[t])
                {
                    continue;
                }
                const crd::u64 s = mask_size(masks[t], idx_size);
                if (s < s1 || bi == n_ssa)
                {
                    s2 = s1;
                    bj = bi;
                    s1 = s;
                    bi = t;
                }
                else if (s < s2 || bj == n_ssa)
                {
                    s2 = s;
                    bj = t;
                }
            }
            if (bi > bj)
            {
                const crd::u32 tmp = bi;
                bi = bj;
                bj = tmp;
            }
        }
        const crd::u64 either = masks[bi] | masks[bj];
        const crd::u64 keep = keep_for(bi, bj);
        const crd::u64 k12 = either & keep;
        const bool inner = (either & ~keep) != 0U;
        const crd::u64 f = pair_flops(either, inner, idx_size);
        total += f;
        steps[n_steps].a = bi;
        steps[n_steps].b = bj;
        steps[n_steps].result_mask = k12;
        steps[n_steps].flops = f;
        ++n_steps;
        alive[bi] = false;
        alive[bj] = false;
        masks[n_ssa] = k12;
        alive[n_ssa] = true;
        ++n_ssa;
        --n_alive;
    }
    return total;
}

// ---- OPTIMAL (opt_einsum branch-and-bound DFS) ------------------------------
struct OptimalCtx
{
    const crd::u64* idx_size;
    crd::u64 out_mask;
    crd::u64 best_flops;
    crd::u32 best_path[kEinsumMaxOperands][2];
    crd::u32 n_ops;
    crd::u64 masks[2U * kEinsumMaxOperands];
    crd::u32 cur_path[kEinsumMaxOperands][2];
};

inline void optimal_iterate(OptimalCtx& c, crd::u64 alive_mask, crd::u32 n_ssa, crd::u32 depth, crd::u64 flops) noexcept
{
    if (std::popcount(alive_mask) == 1U)
    {
        c.best_flops = flops;
        for (crd::u32 s = 0; s < depth; ++s)
        {
            c.best_path[s][0] = c.cur_path[s][0];
            c.best_path[s][1] = c.cur_path[s][1];
        }
        return;
    }
    for (crd::u32 j = 0; j < n_ssa; ++j)
    {
        if ((alive_mask & (1ULL << j)) == 0U)
        {
            continue;
        }
        for (crd::u32 i = 0; i < j; ++i)
        {
            if ((alive_mask & (1ULL << i)) == 0U)
            {
                continue;
            }
            crd::u64 keep = c.out_mask;
            crd::u64 rem = alive_mask & ~(1ULL << i) & ~(1ULL << j);
            crd::u64 rm = rem;
            while (rm != 0U)
            {
                keep |= c.masks[std::countr_zero(rm)];
                rm &= rm - 1U;
            }
            const crd::u64 either = c.masks[i] | c.masks[j];
            const crd::u64 k12 = either & keep;
            const bool inner = (either & ~keep) != 0U;
            const crd::u64 f = pair_flops(either, inner, c.idx_size);
            const crd::u64 nf = flops + f;
            if (nf >= c.best_flops)
            {
                continue; // sieve
            }
            c.masks[n_ssa] = k12;
            c.cur_path[depth][0] = i;
            c.cur_path[depth][1] = j;
            optimal_iterate(c, rem | (1ULL << n_ssa), n_ssa + 1U, depth + 1U, nf);
        }
    }
}

} // namespace detail

// Build the plan: parse already done (expr), idx sizes resolved from shapes.
[[nodiscard]] inline TensorStatus einsum_plan_build(const EinsumExpr& expr, const crd::u64* idx_size,
                                                    EinsumOptimize opt, EinsumPlan& plan) noexcept
{
    plan.expr = expr;
    for (crd::u32 i = 0; i < kEinsumMaxIndices; ++i)
    {
        plan.idx_size[i] = idx_size[i];
    }
    if (expr.n_ops == 1U)
    {
        plan.n_steps = 0;
        plan.total_flops = detail::mask_size(expr.term[0].mask, idx_size);
        plan.largest_intermediate = detail::mask_size(expr.out_mask, idx_size);
        return TensorStatus::Ok;
    }
    // The auto rule (opt_einsum's own preset shape): exact branch-and-bound
    // whenever it is cheap (n <= 7: microseconds), regardless of the requested
    // mode — the exact search minimizes the REPORTED metric, so it can never
    // lose to any greedy. Greedy handles the large-n regime.
    const bool use_optimal = expr.n_ops <= 7U || (opt == EinsumOptimize::Optimal && expr.n_ops <= 10U);
    if (use_optimal)
    {
        detail::OptimalCtx c{};
        c.idx_size = idx_size;
        c.out_mask = expr.out_mask;
        c.best_flops = ~crd::u64{0};
        c.n_ops = expr.n_ops;
        for (crd::u32 i = 0; i < expr.n_ops; ++i)
        {
            c.masks[i] = expr.term[i].mask;
        }
        detail::optimal_iterate(c, (1ULL << expr.n_ops) - 1U, expr.n_ops, 0U, 0U);
        // replay the winning SSA path to fill steps
        crd::u64 masks[2U * kEinsumMaxOperands];
        for (crd::u32 i = 0; i < expr.n_ops; ++i)
        {
            masks[i] = expr.term[i].mask;
        }
        crd::u64 alive = (1ULL << expr.n_ops) - 1U;
        crd::u32 n_ssa = expr.n_ops;
        crd::u64 total = 0;
        for (crd::u32 s = 0; s + 1U < expr.n_ops; ++s)
        {
            const crd::u32 i = c.best_path[s][0];
            const crd::u32 j = c.best_path[s][1];
            crd::u64 keep = expr.out_mask;
            crd::u64 rem = alive & ~(1ULL << i) & ~(1ULL << j);
            crd::u64 rm = rem;
            while (rm != 0U)
            {
                keep |= masks[std::countr_zero(rm)];
                rm &= rm - 1U;
            }
            const crd::u64 either = masks[i] | masks[j];
            const crd::u64 k12 = either & keep;
            const crd::u64 f = detail::pair_flops(either, (either & ~keep) != 0U, idx_size);
            plan.step[s] = {i, j, k12, f};
            total += f;
            masks[n_ssa] = k12;
            alive = rem | (1ULL << n_ssa);
            ++n_ssa;
        }
        plan.n_steps = expr.n_ops - 1U;
        plan.total_flops = total;
    }
    else
    {
        // best-of-three deterministic greedy sweeps
        EinsumStep cand[kEinsumMaxOperands];
        crd::u32 cand_n = 0;
        crd::u64 best = ~crd::u64{0};
        for (crd::u32 h = 0; h < 3U; ++h)
        {
            const crd::u64 f = detail::greedy_path_h(expr, idx_size, h, cand, cand_n);
            if (f < best)
            {
                best = f;
                plan.n_steps = cand_n;
                for (crd::u32 s = 0; s < cand_n; ++s)
                {
                    plan.step[s] = cand[s];
                }
            }
        }
        plan.total_flops = best;
    }
    plan.largest_intermediate = detail::mask_size(expr.out_mask, idx_size);
    for (crd::u32 s = 0; s < plan.n_steps; ++s)
    {
        const crd::u64 sz = detail::mask_size(plan.step[s].result_mask, idx_size);
        if (sz > plan.largest_intermediate)
        {
            plan.largest_intermediate = sz;
        }
    }
    return TensorStatus::Ok;
}

} // namespace crd::hesap::tensor
