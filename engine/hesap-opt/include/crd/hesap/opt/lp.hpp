#pragma once

// lp.hpp — Phase 3.1.6 v7-l: LINEAR PROGRAMMING —
//
//     min cᵀx   s.t.   l ≤ Ax ≤ u,   xlo ≤ x ≤ xup      (row + variable bounds; ±inf for absent sides)
//
//   • solve_lp_simplex — the **bounded-variable REVISED SIMPLEX**: slack columns turn the rows into
//     [A −I]·[x; s] = 0 with the bounds moved onto s, so the working form is Av = 0, lo ≤ v ≤ up. TWO-PHASE
//     (artificial basis with Phase-I cost Σa_i ⇒ certified infeasibility when min > 0), **Dantzig pricing
//     with the Bland anti-cycling fallback** after a degenerate-pivot streak, bounded ratio test with BOUND
//     FLIPS, explicit dense B⁻¹ maintained by eta updates + periodic refactorization (Gauss-Jordan) for
//     stability. Exact vertex solutions + exact active sets — the combinatorial member.
//   • solve_lp_mehrotra — the v7-k predictor-corrector interior point at P = 0 (LP is the QP special case;
//     variable bounds enter as identity rows). The smooth member; cross-adjudicates the simplex.
//
// Row duals are reported in the shared OSQP sign (so the v7-k certificate conventions carry over); variable-
// bound duals are the simplex reduced costs (not separately reported — the cross-adjudication checks
// objective + feasibility + complementary slackness). [gold: HiGHS, GLPK, scipy.linprog — the v7-z
// scoreboard]. DENSE scope, like v7-k. ADR-0090.
//
// DETERMINISM: fixed pricing tie-breaks (lowest index) ⇒ a deterministic pivot sequence; bit-identical runs.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/opt/qp.hpp>
#include <crd/memory/allocator.hpp>

#include <crd/math/cmath.hpp>
#include <limits>

namespace crd::hesap::opt
{

// min cᵀx s.t. l ≤ Ax ≤ u, xlo ≤ x ≤ xup. Spans row-major dense; ±infinity for absent bounds; xlo/xup may be
// EMPTY spans (⇒ free variables).
template <typename T> struct LpProblem
{
    crd::containers::ConstSpan<T> c;   // n
    crd::containers::ConstSpan<T> a;   // m×n
    crd::containers::ConstSpan<T> l;   // m
    crd::containers::ConstSpan<T> u;   // m
    crd::containers::ConstSpan<T> xlo; // n or empty
    crd::containers::ConstSpan<T> xup; // n or empty
    crd::usize n = 0;
    crd::usize m = 0;

    [[nodiscard]] bool valid() const noexcept
    {
        return c.size() == n && a.size() == m * n && l.size() == m && u.size() == m &&
               (xlo.size() == n || xlo.size() == 0) && (xup.size() == n || xup.size() == 0);
    }
};

template <typename T> struct LpResult
{
    crd::containers::Array<T> x;
    crd::containers::Array<T> y; // row duals (OSQP sign)
    T obj = static_cast<T>(0);
    QpStatus status = QpStatus::MaxIterations;
    crd::usize iterations = 0; // simplex pivots / IPM iterations

    explicit LpResult(crd::memory::IAllocator* alloc) noexcept : x(alloc), y(alloc) {}
};

// --------------------------------------------------------- the interior-point route (the v7-k IPM at P = 0)

template <typename T>
[[nodiscard]] LpResult<T> solve_lp_mehrotra(const LpProblem<T>& prob, crd::memory::IAllocator* alloc,
                                            const QpIpmOptions<T>& opts = {})
{
    CRD_ASSERT_MSG(prob.valid(), "solve_lp_mehrotra: inconsistent problem spans");
    const crd::usize n = prob.n;
    const bool has_lo = prob.xlo.size() == n;
    const bool has_up = prob.xup.size() == n;
    // Fold variable bounds in as identity rows (only those with at least one finite side).
    crd::usize extra = 0;
    for (crd::usize j = 0; j < n; ++j)
    {
        const T lo = has_lo ? prob.xlo[j] : -std::numeric_limits<T>::infinity();
        const T up = has_up ? prob.xup[j] : std::numeric_limits<T>::infinity();
        if (std::isfinite(lo) || std::isfinite(up))
        {
            ++extra;
        }
    }
    const crd::usize mq = prob.m + extra;
    crd::containers::Array<T> p0(alloc);
    crd::containers::Array<T> aq(alloc);
    crd::containers::Array<T> lq(alloc);
    crd::containers::Array<T> uq(alloc);
    p0.resize(n * n);
    aq.resize(mq * n);
    lq.resize(mq);
    uq.resize(mq);
    for (crd::usize k = 0; k < n * n; ++k)
    {
        p0[k] = static_cast<T>(0);
    }
    for (crd::usize k = 0; k < prob.m * n; ++k)
    {
        aq[k] = prob.a[k];
    }
    for (crd::usize i = 0; i < prob.m; ++i)
    {
        lq[i] = prob.l[i];
        uq[i] = prob.u[i];
    }
    {
        crd::usize w = prob.m;
        for (crd::usize j = 0; j < n; ++j)
        {
            const T lo = has_lo ? prob.xlo[j] : -std::numeric_limits<T>::infinity();
            const T up = has_up ? prob.xup[j] : std::numeric_limits<T>::infinity();
            if (std::isfinite(lo) || std::isfinite(up))
            {
                for (crd::usize k = 0; k < n; ++k)
                {
                    aq[w * n + k] = k == j ? static_cast<T>(1) : static_cast<T>(0);
                }
                lq[w] = lo;
                uq[w] = up;
                ++w;
            }
        }
    }
    const QpProblem<T> qp{{p0.data(), n * n}, prob.c, {aq.data(), mq * n}, {lq.data(), mq}, {uq.data(), mq}, n, mq};
    const QpResult<T> qr = solve_qp_mehrotra<T>(qp, opts, alloc);
    LpResult<T> result(alloc);
    result.x.resize(n);
    result.y.resize(prob.m);
    for (crd::usize j = 0; j < n; ++j)
    {
        result.x[j] = qr.x[j];
    }
    for (crd::usize i = 0; i < prob.m; ++i)
    {
        result.y[i] = qr.y[i];
    }
    result.obj = qr.obj;
    result.status = qr.status;
    result.iterations = qr.iterations;
    return result;
}

// ------------------------------------------------------------------ the bounded-variable revised simplex

template <typename T> struct LpSimplexOptions
{
    crd::usize max_pivots = 0;      // 0 ⇒ 200·(n+m)
    crd::usize refactor_every = 64; // explicit B⁻¹ refresh cadence
    crd::usize bland_after = 50;    // consecutive degenerate pivots before switching to Bland's rule
    T tol = static_cast<T>(1e-9);   // pricing / ratio / feasibility tolerance
};

template <typename T>
[[nodiscard]] LpResult<T> solve_lp_simplex(const LpProblem<T>& prob, crd::memory::IAllocator* alloc,
                                           const LpSimplexOptions<T>& opts = {})
{
    CRD_ASSERT_MSG(prob.valid(), "solve_lp_simplex: inconsistent problem spans");
    const crd::usize n = prob.n;
    const crd::usize m = prob.m;
    const T inf = std::numeric_limits<T>::infinity();
    const T tol = opts.tol;

    LpResult<T> result(alloc);
    result.x.resize(n);
    result.y.resize(m);
    for (crd::usize j = 0; j < n; ++j)
    {
        result.x[j] = static_cast<T>(0);
    }
    for (crd::usize i = 0; i < m; ++i)
    {
        result.y[i] = static_cast<T>(0);
    }
    if (n == 0)
    {
        result.status = QpStatus::Solved;
        return result;
    }

    // Working form: [A −I | ±I_artificial]·v = 0. Columns: 0..n-1 = x, n..n+m-1 = s (row slacks),
    // n+m..n+2m-1 = artificials (Phase I only; bounds pinched to [0,0] in Phase II).
    const crd::usize nv = n + m;     // real variables
    const crd::usize nt = n + 2 * m; // + artificials
    const bool has_lo = prob.xlo.size() == n;
    const bool has_up = prob.xup.size() == n;

    crd::containers::Array<T> lo(alloc);
    crd::containers::Array<T> up(alloc);
    crd::containers::Array<T> val(alloc);  // current value of every column (nonbasic at a bound; basic = computed)
    crd::containers::Array<T> cost(alloc); // phase-dependent objective
    lo.resize(nt);
    up.resize(nt);
    val.resize(nt);
    cost.resize(nt);
    for (crd::usize j = 0; j < n; ++j)
    {
        lo[j] = has_lo ? prob.xlo[j] : -inf;
        up[j] = has_up ? prob.xup[j] : inf;
    }
    for (crd::usize i = 0; i < m; ++i)
    {
        lo[n + i] = prob.l[i];
        up[n + i] = prob.u[i];
    }
    for (crd::usize i = 0; i < m; ++i) // artificials: [0, inf) in Phase I
    {
        lo[n + m + i] = static_cast<T>(0);
        up[n + m + i] = inf;
    }

    // Column accessor for the constraint matrix [A | −I | D_art] (D_art signs set after the residual is known).
    crd::containers::Array<T> art_sign(alloc);
    art_sign.resize(m);
    auto col_entry = [&](crd::usize row, crd::usize j) -> T
    {
        if (j < n)
        {
            return prob.a[row * n + j];
        }
        if (j < n + m)
        {
            return j - n == row ? static_cast<T>(-1) : static_cast<T>(0);
        }
        return j - n - m == row ? art_sign[j - n - m] : static_cast<T>(0);
    };

    // Nonbasic start: every real variable at its nearest finite bound (free ⇒ 0).
    for (crd::usize j = 0; j < nv; ++j)
    {
        if (std::isfinite(lo[j]))
        {
            val[j] = lo[j];
        }
        else if (std::isfinite(up[j]))
        {
            val[j] = up[j];
        }
        else
        {
            val[j] = static_cast<T>(0);
        }
    }
    // Row residuals r = −Σ_real cols (Av = 0 target) ⇒ artificial values |r| with sign-matched columns.
    crd::containers::Array<T> resid(alloc);
    resid.resize(m);
    for (crd::usize i = 0; i < m; ++i)
    {
        T r = static_cast<T>(0);
        for (crd::usize j = 0; j < nv; ++j)
        {
            r += col_entry(i, j) * val[j];
        }
        resid[i] = -r; // the artificial must contribute exactly −r
        art_sign[i] = resid[i] >= static_cast<T>(0) ? static_cast<T>(1) : static_cast<T>(-1);
        val[n + m + i] = crd::math::fabs(resid[i]);
    }

    // Basis = the artificials; explicit dense B⁻¹ (= diag(sign) initially, since B = diag(art_sign)).
    crd::containers::Array<crd::u32> basis(alloc);
    crd::containers::Array<bool> in_basis(alloc);
    crd::containers::Array<T> binv(alloc);
    basis.resize(m);
    in_basis.resize(nt);
    binv.resize(m * m);
    for (crd::usize j = 0; j < nt; ++j)
    {
        in_basis[j] = false;
    }
    for (crd::usize i = 0; i < m; ++i)
    {
        basis[i] = static_cast<crd::u32>(n + m + i);
        in_basis[n + m + i] = true;
        for (crd::usize k = 0; k < m; ++k)
        {
            binv[i * m + k] = i == k ? art_sign[i] : static_cast<T>(0); // (diag s)⁻¹ = diag s
        }
    }

    auto refactor = [&]() -> bool // recompute B⁻¹ from the basis columns by Gauss-Jordan (stability refresh)
    {
        crd::containers::Array<T> work(alloc);
        work.resize(m * 2 * m);
        for (crd::usize i = 0; i < m; ++i)
        {
            for (crd::usize k = 0; k < m; ++k)
            {
                work[i * 2 * m + k] = col_entry(i, basis[k]);
                work[i * 2 * m + m + k] = i == k ? static_cast<T>(1) : static_cast<T>(0);
            }
        }
        for (crd::usize col = 0; col < m; ++col)
        {
            crd::usize piv = col;
            T best = crd::math::fabs(work[col * 2 * m + col]);
            for (crd::usize r = col + 1; r < m; ++r)
            {
                const T a = crd::math::fabs(work[r * 2 * m + col]);
                if (a > best)
                {
                    best = a;
                    piv = r;
                }
            }
            if (!(best > static_cast<T>(0)))
            {
                return false; // singular basis (should not happen with a valid pivot sequence)
            }
            if (piv != col)
            {
                for (crd::usize k = 0; k < 2 * m; ++k)
                {
                    const T t = work[col * 2 * m + k];
                    work[col * 2 * m + k] = work[piv * 2 * m + k];
                    work[piv * 2 * m + k] = t;
                }
            }
            const T d = work[col * 2 * m + col];
            for (crd::usize k = 0; k < 2 * m; ++k)
            {
                work[col * 2 * m + k] /= d;
            }
            for (crd::usize r = 0; r < m; ++r)
            {
                if (r != col)
                {
                    const T f = work[r * 2 * m + col];
                    if (f != static_cast<T>(0))
                    {
                        for (crd::usize k = 0; k < 2 * m; ++k)
                        {
                            work[r * 2 * m + k] -= f * work[col * 2 * m + k];
                        }
                    }
                }
            }
        }
        for (crd::usize i = 0; i < m; ++i)
        {
            for (crd::usize k = 0; k < m; ++k)
            {
                binv[i * m + k] = work[i * 2 * m + m + k];
            }
        }
        return true;
    };
    auto compute_basics = [&]() // x_B = B⁻¹·(0 − N x_N)
    {
        crd::containers::Array<T> rhs(alloc);
        rhs.resize(m);
        for (crd::usize i = 0; i < m; ++i)
        {
            T r = static_cast<T>(0);
            for (crd::usize j = 0; j < nt; ++j)
            {
                if (!in_basis[j])
                {
                    r -= col_entry(i, j) * val[j];
                }
            }
            rhs[i] = r;
        }
        for (crd::usize i = 0; i < m; ++i)
        {
            T v = static_cast<T>(0);
            for (crd::usize k = 0; k < m; ++k)
            {
                v += binv[i * m + k] * rhs[k];
            }
            val[basis[i]] = v;
        }
    };

    const crd::usize max_pivots = opts.max_pivots > 0 ? opts.max_pivots : 200 * (n + m + 1);
    crd::usize total_pivots = 0;
    crd::usize degen_streak = 0;
    crd::usize since_refactor = 0;

    // One simplex phase over the given cost vector; returns Solved / DualInfeasible (= unbounded) / MaxIterations.
    auto run_phase = [&]() -> QpStatus
    {
        crd::containers::Array<T> ydual(alloc); // y = c_Bᵀ B⁻¹
        crd::containers::Array<T> wcol(alloc);  // B⁻¹·N_j
        ydual.resize(m);
        wcol.resize(m);
        while (total_pivots < max_pivots)
        {
            for (crd::usize i = 0; i < m; ++i)
            {
                T acc = static_cast<T>(0);
                for (crd::usize k = 0; k < m; ++k)
                {
                    acc += cost[basis[k]] * binv[k * m + i];
                }
                ydual[i] = acc;
            }
            // Pricing: Dantzig (most negative effective reduced cost), Bland (lowest index) when cycling.
            const bool bland = degen_streak >= opts.bland_after;
            crd::usize enter = nt;
            T best = -tol;
            bool enter_from_lower = true;
            for (crd::usize j = 0; j < nt; ++j)
            {
                if (in_basis[j] || lo[j] == up[j])
                {
                    continue; // fixed columns (e.g. pinched artificials) never re-enter
                }
                T d = cost[j];
                for (crd::usize i = 0; i < m; ++i)
                {
                    d -= ydual[i] * col_entry(i, j);
                }
                const bool at_lower = std::isfinite(lo[j]) ? val[j] <= lo[j] + tol : !std::isfinite(up[j]);
                // increase from lower needs d < 0; decrease from upper needs d > 0; free nonbasic: either.
                if ((at_lower || !std::isfinite(up[j])) && d < -tol)
                {
                    if (bland ? enter == nt : d < best)
                    {
                        best = d;
                        enter = j;
                        enter_from_lower = true;
                        if (bland)
                        {
                            break;
                        }
                    }
                }
                else if (!at_lower && d > tol)
                {
                    if (bland ? enter == nt : -d < best)
                    {
                        best = -d;
                        enter = j;
                        enter_from_lower = false;
                        if (bland)
                        {
                            break;
                        }
                    }
                }
            }
            if (enter == nt)
            {
                return QpStatus::Solved; // optimal for this phase
            }

            for (crd::usize i = 0; i < m; ++i) // w = B⁻¹·column(enter)
            {
                T acc = static_cast<T>(0);
                for (crd::usize k = 0; k < m; ++k)
                {
                    acc += binv[i * m + k] * col_entry(k, enter);
                }
                wcol[i] = acc;
            }
            // Bounded ratio test. Entering moves by t ≥ 0 in direction dir (+1 from lower, −1 from upper);
            // basic i changes by −dir·t·w_i.
            const T dir = enter_from_lower ? static_cast<T>(1) : static_cast<T>(-1);
            T t_max = up[enter] - lo[enter]; // the entering variable's own bound-flip distance
            crd::usize leave = m;            // m ⇒ bound flip
            T leave_to = static_cast<T>(0);
            for (crd::usize i = 0; i < m; ++i)
            {
                const T delta = -dir * wcol[i];
                const crd::usize bj = basis[i];
                if (delta > tol && std::isfinite(up[bj])) // basic increases toward its upper bound
                {
                    const T t = (up[bj] - val[bj]) / delta;
                    if (t < t_max - tol || (bland && t < t_max + tol && (leave == m || bj < basis[leave])))
                    {
                        t_max = t < static_cast<T>(0) ? static_cast<T>(0) : t;
                        leave = i;
                        leave_to = up[bj];
                    }
                }
                else if (delta < -tol && std::isfinite(lo[bj])) // basic decreases toward its lower bound
                {
                    const T t = (lo[bj] - val[bj]) / delta;
                    if (t < t_max - tol || (bland && t < t_max + tol && (leave == m || bj < basis[leave])))
                    {
                        t_max = t < static_cast<T>(0) ? static_cast<T>(0) : t;
                        leave = i;
                        leave_to = lo[bj];
                    }
                }
            }
            if (!std::isfinite(t_max))
            {
                return QpStatus::DualInfeasible; // unbounded ray
            }
            degen_streak = t_max <= tol ? degen_streak + 1 : 0;

            // Apply the step.
            val[enter] += dir * t_max;
            for (crd::usize i = 0; i < m; ++i)
            {
                val[basis[i]] -= dir * t_max * wcol[i];
            }
            ++total_pivots;
            if (leave == m)
            {
                continue; // bound flip — no basis change
            }
            val[basis[leave]] = leave_to; // pin the leaver exactly to its bound
            in_basis[basis[leave]] = false;
            in_basis[enter] = true;
            basis[leave] = static_cast<crd::u32>(enter);
            const T piv = wcol[leave]; // eta update of B⁻¹: row `leave` scales by 1/piv; others eliminate
            for (crd::usize k = 0; k < m; ++k)
            {
                binv[leave * m + k] /= piv;
            }
            for (crd::usize i = 0; i < m; ++i)
            {
                if (i != leave && wcol[i] != static_cast<T>(0))
                {
                    const T f = wcol[i];
                    for (crd::usize k = 0; k < m; ++k)
                    {
                        binv[i * m + k] -= f * binv[leave * m + k];
                    }
                }
            }
            if (++since_refactor >= opts.refactor_every)
            {
                since_refactor = 0;
                if (!refactor())
                {
                    return QpStatus::NumericalError;
                }
                compute_basics();
            }
        }
        return QpStatus::MaxIterations;
    };

    // ---- Phase I: minimize Σ artificials.
    for (crd::usize j = 0; j < nt; ++j)
    {
        cost[j] = j >= n + m ? static_cast<T>(1) : static_cast<T>(0);
    }
    QpStatus st = run_phase();
    if (st == QpStatus::MaxIterations || st == QpStatus::NumericalError)
    {
        result.status = st;
        result.iterations = total_pivots;
        return result;
    }
    T art_sum = static_cast<T>(0);
    for (crd::usize i = 0; i < m; ++i)
    {
        art_sum += val[n + m + i];
    }
    if (art_sum > static_cast<T>(100) * tol * (static_cast<T>(1) + static_cast<T>(m)))
    {
        result.status = QpStatus::PrimalInfeasible; // certified by the Phase-I optimum
        result.iterations = total_pivots;
        return result;
    }

    // ---- Phase II: the real cost; artificials pinched to [0, 0] so they can never re-enter.
    for (crd::usize i = 0; i < m; ++i)
    {
        lo[n + m + i] = static_cast<T>(0);
        up[n + m + i] = static_cast<T>(0);
    }
    for (crd::usize j = 0; j < nt; ++j)
    {
        cost[j] = j < n ? prob.c[j] : static_cast<T>(0);
    }
    degen_streak = 0;
    st = run_phase();

    // ---- Report. Row duals: y_simplex = c_Bᵀ B⁻¹; the OSQP-sign convention is y_report = −y_simplex
    //      (then c + Aᵀy_report = the variable reduced costs = the bound duals, 0 for unbounded x).
    for (crd::usize i = 0; i < m; ++i)
    {
        T acc = static_cast<T>(0);
        for (crd::usize k = 0; k < m; ++k)
        {
            acc += cost[basis[k]] * binv[k * m + i];
        }
        result.y[i] = -acc;
    }
    T obj = static_cast<T>(0);
    for (crd::usize j = 0; j < n; ++j)
    {
        result.x[j] = val[j];
        obj += prob.c[j] * val[j];
    }
    result.obj = obj;
    result.status = st;
    result.iterations = total_pivots;
    return result;
}

} // namespace crd::hesap::opt
