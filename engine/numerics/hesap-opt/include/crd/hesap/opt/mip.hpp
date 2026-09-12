#pragma once

// mip.hpp — Phase 3.1.6 v7-r: MIXED-INTEGER LINEAR PROGRAMMING — BRANCH AND BOUND over the v7-l
// bounded-variable revised simplex (the natural pairing: bound tightening IS branching in that form):
//
//     min cᵀx   s.t.   l ≤ Ax ≤ u,  xlo ≤ x ≤ xup,  x_j ∈ ℤ for j ∈ I
//
//   • LP relaxations via `solve_lp_simplex` (exact vertex solutions + certified infeasibility per node);
//   • BEST-BOUND node selection with lowest-index tie-breaks (deterministic tree order);
//   • MOST-FRACTIONAL branching (deterministic tie-break), floor/ceil child bounds;
//   • pruning by bound (against the incumbent, with the integrality tolerance), by infeasibility, and by
//     integrality (incumbent updates);
//   • exhausted tree ⇒ PROVEN optimum (Solved); the node cap ⇒ the best incumbent with MaxIterations.
//
// HONEST SCOPE (named): pure B&B — Gomory cutting planes need simplex-tableau access (`solve_lp_simplex`
// does not expose its basis inverse) and presolve/heuristics are accelerators, not correctness pieces; both
// are future levers, and the v7-z scoreboard against HiGHS/GLPK states this plainly. DENSE small-instance
// scope, like v7-l. ADR-0090.
//
// DETERMINISM: the simplex is deterministic and the node/branch orders are fixed ⇒ bit-identical runs.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/opt/lp.hpp>
#include <crd/memory/allocator.hpp>

#include <crd/math/cmath.hpp>
#include <limits>

namespace crd::hesap::opt
{

template <typename T> struct MipOptions
{
    crd::usize max_nodes = 100000;
    T int_tol = static_cast<T>(1e-6); // |x_j − round(x_j)| ≤ tol counts as integral
    T gap_abs = static_cast<T>(1e-9); // prune when node bound ≥ incumbent − gap
    LpSimplexOptions<T> lp{};         // the per-node relaxation solver
};

template <typename T> struct MipResult
{
    crd::containers::Array<T> x;
    T obj = static_cast<T>(0);
    QpStatus status = QpStatus::MaxIterations;
    crd::usize nodes = 0; // B&B nodes solved
    bool has_incumbent = false;

    explicit MipResult(crd::memory::IAllocator* alloc) noexcept : x(alloc) {}
};

// `integer_mask[j]` ⇒ x_j must be integral. The problem's xlo/xup may be empty (free continuous vars are
// fine; integer vars without finite bounds are legal but can make the tree infinite — bound your integers).
template <typename T>
[[nodiscard]] MipResult<T> solve_mip_branch_and_bound(const LpProblem<T>& prob,
                                                      crd::containers::ConstSpan<bool> integer_mask,
                                                      crd::memory::IAllocator* alloc, const MipOptions<T>& mo = {})
{
    CRD_ASSERT_MSG(prob.valid(), "solve_mip_branch_and_bound: inconsistent problem spans");
    CRD_ASSERT_MSG(integer_mask.size() == prob.n, "solve_mip_branch_and_bound: integer mask size mismatch");
    const crd::usize n = prob.n;
    const T inf = std::numeric_limits<T>::infinity();

    MipResult<T> result(alloc);
    result.x.resize(n);
    for (crd::usize j = 0; j < n; ++j)
    {
        result.x[j] = static_cast<T>(0);
    }
    if (n == 0)
    {
        result.status = QpStatus::Solved;
        result.has_incumbent = true;
        return result;
    }

    // The node store: per node, the variable-bound overrides + the parent's LP bound (for best-bound order).
    // `state`: 0 = open, 1 = closed.
    crd::containers::Array<T> node_lo(alloc);
    crd::containers::Array<T> node_up(alloc);
    crd::containers::Array<T> node_bound(alloc);
    crd::containers::Array<crd::u8> node_state(alloc);
    auto push_node = [&](const T* lo, const T* up, T bound)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            node_lo.push_back(lo[j]);
            node_up.push_back(up[j]);
        }
        node_bound.push_back(bound);
        node_state.push_back(0);
    };

    // The root takes the problem's own variable bounds (±inf when absent).
    {
        crd::containers::Array<T> lo0(alloc);
        crd::containers::Array<T> up0(alloc);
        lo0.resize(n);
        up0.resize(n);
        for (crd::usize j = 0; j < n; ++j)
        {
            lo0[j] = prob.xlo.size() == n ? prob.xlo[j] : -inf;
            up0[j] = prob.xup.size() == n ? prob.xup[j] : inf;
        }
        push_node(lo0.data(), up0.data(), -inf);
    }

    T incumbent = inf;
    bool tree_truncated = false;
    crd::usize solved_nodes = 0;

    while (solved_nodes < mo.max_nodes)
    {
        // Best-bound node selection (lowest index breaks ties — deterministic).
        crd::usize pick = node_state.size();
        T best_bound = inf;
        for (crd::usize k = 0; k < node_state.size(); ++k)
        {
            if (node_state[k] == 0 && node_bound[k] < best_bound)
            {
                pick = k;
                best_bound = node_bound[k];
            }
        }
        if (pick == node_state.size())
        {
            break; // tree exhausted
        }
        node_state[pick] = 1;
        if (best_bound >= incumbent - mo.gap_abs)
        {
            continue; // pruned by bound (the parent's relaxation already dominates)
        }

        // Solve the node's LP relaxation.
        const T* lo = node_lo.data() + pick * n;
        const T* up = node_up.data() + pick * n;
        LpProblem<T> node = prob;
        node.xlo = {lo, n};
        node.xup = {up, n};
        const LpResult<T> rel = solve_lp_simplex<T>(node, alloc, mo.lp);
        ++solved_nodes;
        if (rel.status == QpStatus::PrimalInfeasible)
        {
            continue; // pruned by infeasibility
        }
        if (rel.status != QpStatus::Solved)
        {
            tree_truncated = true; // an unbounded/failed relaxation poisons the proof
            continue;
        }
        if (rel.obj >= incumbent - mo.gap_abs)
        {
            continue; // pruned by bound
        }

        // Most-fractional integer variable (deterministic tie-break: strictly-greater keeps the lowest index).
        crd::usize branch = n;
        T best_frac = mo.int_tol;
        for (crd::usize j = 0; j < n; ++j)
        {
            if (!integer_mask[j])
            {
                continue;
            }
            const T r = crd::math::floor(rel.x[j] + static_cast<T>(0.5));
            const T dist = crd::math::fabs(rel.x[j] - r);
            if (dist > best_frac)
            {
                best_frac = dist;
                branch = j;
            }
        }
        if (branch == n)
        {
            // Integral ⇒ a new incumbent (round the integer coordinates exactly).
            incumbent = rel.obj;
            result.has_incumbent = true;
            for (crd::usize j = 0; j < n; ++j)
            {
                result.x[j] = integer_mask[j] ? crd::math::floor(rel.x[j] + static_cast<T>(0.5)) : rel.x[j];
            }
            continue;
        }

        // Branch: x_branch ≤ floor(v) | x_branch ≥ ceil(v). ⚠ `lo`/`up` point INTO node_lo/node_up and
        // push_node reallocates them — snapshot EVERYTHING needed before the first push (this dangling read
        // was a real bug: the second child got a garbage bound and the optimum was "pruned"; caught by the
        // exhaustive-enumeration oracle scan).
        const T v = rel.x[branch];
        crd::containers::Array<T> lo_child(alloc);
        crd::containers::Array<T> up_child(alloc);
        lo_child.resize(n);
        up_child.resize(n);
        for (crd::usize j = 0; j < n; ++j)
        {
            lo_child[j] = lo[j];
            up_child[j] = up[j];
        }
        const T parent_up_branch = up[branch];
        const T fl = crd::math::floor(v);
        up_child[branch] = fl;
        if (lo_child[branch] <= up_child[branch])
        {
            push_node(lo_child.data(), up_child.data(), rel.obj);
        }
        up_child[branch] = parent_up_branch;
        lo_child[branch] = fl + static_cast<T>(1);
        if (lo_child[branch] <= up_child[branch])
        {
            push_node(lo_child.data(), up_child.data(), rel.obj);
        }
    }

    // Any open node left means the cap truncated the proof.
    for (crd::usize k = 0; k < node_state.size(); ++k)
    {
        if (node_state[k] == 0 && node_bound[k] < incumbent - mo.gap_abs)
        {
            tree_truncated = true;
            break;
        }
    }

    result.obj = incumbent;
    result.nodes = solved_nodes;
    if (result.has_incumbent)
    {
        result.status = tree_truncated ? QpStatus::MaxIterations : QpStatus::Solved;
    }
    else
    {
        result.status = tree_truncated ? QpStatus::MaxIterations : QpStatus::PrimalInfeasible;
    }
    return result;
}

} // namespace crd::hesap::opt
