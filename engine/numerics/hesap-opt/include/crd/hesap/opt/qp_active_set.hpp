#pragma once

// qp_active_set.hpp — Phase 3.1.6 v7-k ⭐: the **Goldfarb-Idnani dual active-set** QP solver (Goldfarb &
// Idnani 1983, "A numerically stable dual method for solving strictly convex quadratic programs" — the
// algorithm behind R's quadprog / quadprog++ / eiquadprog). Requires P ≻ 0 (STRICTLY convex). Starts at the
// unconstrained minimum (dual feasible by construction), adds the most-violated constraint per outer step, and
// walks primal/dual step pairs — dropping blocking actives — until primal feasibility: FINITE termination, no
// feasible starting point needed, and exact active-set identification (the property the first-order ADMM
// lacks). Machinery: G = LLᵀ, J = L⁻ᵀ kept dense, and the active-set factor R updated/downdated with GIVENS
// rotations applied to (d, J) pairs — the numerically-stable core of the paper.
//
// Canonical-form adapter (qp.hpp): l_i == u_i rows enter as EQUALITIES (added first, never dropped); finite
// l_i ⇒ the ≥ row (+a_i, −l_i); finite u_i ⇒ (−a_i, +u_i). Duals are reported in the shared OSQP sign
// (Px + q + Aᵀy = 0). Dense, serial, deterministic (fixed tie-breaking: the most-violated, lowest index).
// Cross-adjudicated against ADMM + Mehrotra in test_qp.cpp (three independent algorithms agreeing on
// strictly convex instances). ADR-0090. [gold: quadprog, qpOASES]

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

// Goldfarb-Idnani on the canonical form. P must be SPD; status PrimalInfeasible when the dual is unbounded.
template <typename T>
[[nodiscard]] QpResult<T> solve_qp_goldfarb_idnani(const QpProblem<T>& prob, crd::memory::IAllocator* alloc)
{
    CRD_ASSERT_MSG(prob.valid(), "solve_qp_goldfarb_idnani: inconsistent problem spans");
    const crd::usize n = prob.n;

    QpResult<T> result(alloc);
    result.x.resize(n);
    result.y.resize(prob.m);
    for (crd::usize i = 0; i < prob.m; ++i)
    {
        result.y[i] = static_cast<T>(0);
    }
    if (n == 0)
    {
        result.status = QpStatus::Solved;
        return result;
    }

    // ---- Build the GI constraint list: equalities first (meq), then the ≥ rows. Normals are COLUMNS n_k
    //      with the constraint n_kᵀx + c0_k ≥ 0 (== 0 for the first meq). src/sgn map duals back to rows.
    crd::usize meq = 0;
    crd::usize mtot = 0;
    for (crd::usize i = 0; i < prob.m; ++i)
    {
        if (prob.l[i] == prob.u[i])
        {
            ++meq;
            ++mtot;
        }
        else
        {
            if (std::isfinite(prob.l[i]))
            {
                ++mtot;
            }
            if (std::isfinite(prob.u[i]))
            {
                ++mtot;
            }
        }
    }
    crd::containers::Array<T> cn(alloc); // normals, mtot × n row-major (row k = n_kᵀ)
    crd::containers::Array<T> c0(alloc);
    crd::containers::Array<crd::u32> src(alloc);
    crd::containers::Array<T> sgn(alloc); // y_row += sgn · u_k at the end
    cn.resize(mtot * n);
    c0.resize(mtot);
    src.resize(mtot);
    sgn.resize(mtot);
    {
        crd::usize w = 0;
        for (crd::usize i = 0; i < prob.m; ++i) // equalities first: n = a_i, c0 = −l_i; y = −u (lower-row sign)
        {
            if (prob.l[i] == prob.u[i])
            {
                for (crd::usize j = 0; j < n; ++j)
                {
                    cn[w * n + j] = prob.a[i * n + j];
                }
                c0[w] = -prob.l[i];
                src[w] = static_cast<crd::u32>(i);
                sgn[w] = static_cast<T>(-1);
                ++w;
            }
        }
        for (crd::usize i = 0; i < prob.m; ++i)
        {
            if (prob.l[i] == prob.u[i])
            {
                continue;
            }
            if (std::isfinite(prob.l[i])) // a_iᵀx ≥ l_i: n = a_i, c0 = −l_i; stationarity ⇒ y = −u
            {
                for (crd::usize j = 0; j < n; ++j)
                {
                    cn[w * n + j] = prob.a[i * n + j];
                }
                c0[w] = -prob.l[i];
                src[w] = static_cast<crd::u32>(i);
                sgn[w] = static_cast<T>(-1);
                ++w;
            }
            if (std::isfinite(prob.u[i])) // −a_iᵀx ≥ −u_i: n = −a_i, c0 = u_i; y = +u
            {
                for (crd::usize j = 0; j < n; ++j)
                {
                    cn[w * n + j] = -prob.a[i * n + j];
                }
                c0[w] = prob.u[i];
                src[w] = static_cast<crd::u32>(i);
                sgn[w] = static_cast<T>(1);
                ++w;
            }
        }
    }

    // ---- G = LLᵀ (lower, in place) → x = −G⁻¹q and J = L⁻ᵀ.
    crd::containers::Array<T> lmat(alloc);
    lmat.resize(n * n);
    for (crd::usize k = 0; k < n * n; ++k)
    {
        lmat[k] = prob.p[k];
    }
    for (crd::usize j = 0; j < n; ++j)
    {
        T d = lmat[j * n + j];
        for (crd::usize k = 0; k < j; ++k)
        {
            d -= lmat[j * n + k] * lmat[j * n + k];
        }
        if (!(d > static_cast<T>(0)))
        {
            result.status = QpStatus::NumericalError; // GI needs strict convexity
            for (crd::usize i = 0; i < n; ++i)
            {
                result.x[i] = static_cast<T>(0);
            }
            return result;
        }
        d = crd::math::sqrt(d);
        lmat[j * n + j] = d;
        for (crd::usize i = j + 1; i < n; ++i)
        {
            T s = lmat[i * n + j];
            for (crd::usize k = 0; k < j; ++k)
            {
                s -= lmat[i * n + k] * lmat[j * n + k];
            }
            lmat[i * n + j] = s / d;
        }
    }
    T* x = result.x.data();
    for (crd::usize i = 0; i < n; ++i) // L w = −q (forward)
    {
        T s = -prob.q[i];
        for (crd::usize k = 0; k < i; ++k)
        {
            s -= lmat[i * n + k] * x[k];
        }
        x[i] = s / lmat[i * n + i];
    }
    for (crd::usize ii = n; ii > 0; --ii) // Lᵀ x = w (backward)
    {
        const crd::usize i = ii - 1;
        T s = x[i];
        for (crd::usize k = i + 1; k < n; ++k)
        {
            s -= lmat[k * n + i] * x[k];
        }
        x[i] = s / lmat[i * n + i];
    }
    crd::containers::Array<T> jmat(alloc); // J = L⁻ᵀ: column k of J = solve Lᵀ e_k... build as J = (L⁻¹)ᵀ
    jmat.resize(n * n);
    for (crd::usize col = 0; col < n; ++col) // solve L y = e_col (forward); then J[col][i] = y[i] (J = Yᵀ)
    {
        for (crd::usize i = 0; i < n; ++i)
        {
            T s = i == col ? static_cast<T>(1) : static_cast<T>(0);
            for (crd::usize k = 0; k < i; ++k)
            {
                s -= lmat[i * n + k] * jmat[col * n + k]; // reuse row `col` of jmat as the y scratch
            }
            jmat[col * n + i] = s / lmat[i * n + i];
        }
    }
    // jmat[c*n + r] = (L⁻¹)[r][c] (column c of L⁻¹ stored as row c). J = L⁻ᵀ ⇒ J[i][k] = (L⁻¹)[k][i]
    // = jmat[i*n + k]. ⚠ The transposed accessor was a REAL bug caught by the cross-adjudication: with
    // J̃ = L⁻¹ the step direction lives in the (LᵀL)⁻¹ metric instead of P⁻¹ — it still lands exactly
    // FEASIBLE (t2 normalizes along the constraint) but NOT optimal; invisible on diagonal P (L = Lᵀ).
    auto jat = [&](crd::usize i, crd::usize k) -> T&
    {
        return jmat[i * n + k];
    };

    // ---- Active-set state.
    crd::containers::Array<T> rfac(alloc);       // R, n×n upper (column iq built from d)
    crd::containers::Array<crd::u32> act(alloc); // active constraint indices (into the GI list)
    crd::containers::Array<T> uact(alloc);       // duals of the actives (+ the incoming slot)
    crd::containers::Array<T> d(alloc);
    crd::containers::Array<T> z(alloc);
    crd::containers::Array<T> r(alloc);
    rfac.resize(n * n);
    act.resize(n + 1);
    uact.resize(n + 1);
    d.resize(n);
    z.resize(n);
    r.resize(n + 1);
    crd::usize iq = 0;

    const T inf = std::numeric_limits<T>::infinity();
    const T feas_tol = static_cast<T>(100) * std::numeric_limits<T>::epsilon();

    auto compute_d = [&](crd::usize p)
    {
        for (crd::usize k = 0; k < n; ++k) // d = Jᵀ n_p  (d_k = Σ_i J[i][k] n_p[i])
        {
            T s = static_cast<T>(0);
            for (crd::usize i = 0; i < n; ++i)
            {
                s += jat(i, k) * cn[p * n + i];
            }
            d[k] = s;
        }
    };
    auto compute_z = [&]()
    {
        for (crd::usize i = 0; i < n; ++i) // z = J₂ d₂ = Σ_{k≥iq} J[:,k] d_k
        {
            T s = static_cast<T>(0);
            for (crd::usize k = iq; k < n; ++k)
            {
                s += jat(i, k) * d[k];
            }
            z[i] = s;
        }
    };
    auto compute_r = [&]()
    {
        for (crd::usize jj = iq; jj > 0; --jj) // R r = d[0..iq) backward
        {
            const crd::usize j = jj - 1;
            T s = d[j];
            for (crd::usize k = j + 1; k < iq; ++k)
            {
                s -= rfac[j * n + k] * r[k];
            }
            r[j] = s / rfac[j * n + j];
        }
    };
    auto add_constraint = [&]() -> bool
    {
        for (crd::usize k = n - 1; k > iq; --k) // Givens to zero d[k] into d[k−1], applied to J columns
        {
            const T cc = d[k - 1];
            const T ss = d[k];
            const T h = crd::math::sqrt(cc * cc + ss * ss);
            if (h == static_cast<T>(0))
            {
                continue;
            }
            const T c1 = cc / h;
            const T s1 = ss / h;
            d[k - 1] = h;
            d[k] = static_cast<T>(0);
            for (crd::usize i = 0; i < n; ++i)
            {
                const T t1 = jat(i, k - 1);
                const T t2 = jat(i, k);
                jat(i, k - 1) = c1 * t1 + s1 * t2;
                jat(i, k) = -s1 * t1 + c1 * t2;
            }
        }
        if (crd::math::fabs(d[iq]) < feas_tol * (static_cast<T>(1) + crd::math::fabs(d[0])))
        {
            return false; // the new normal is linearly dependent on the actives
        }
        for (crd::usize i = 0; i <= iq; ++i)
        {
            rfac[i * n + iq] = d[i];
        }
        ++iq;
        return true;
    };
    auto delete_constraint = [&](crd::usize qq) { // drop the active at position qq (an inequality)
        for (crd::usize i = qq; i + 1 < iq; ++i)
        {
            act[i] = act[i + 1];
            uact[i] = uact[i + 1];
            for (crd::usize row = 0; row < n; ++row)
            {
                rfac[row * n + i] = rfac[row * n + i + 1];
            }
        }
        uact[iq - 1] = uact[iq]; // the INCOMING constraint's accumulated dual moves down a slot (quadprog)
        --iq;
        for (crd::usize j = qq; j < iq; ++j) // restore the triangularity of R; rotate J columns alongside
        {
            const T cc = rfac[j * n + j];
            const T ss = rfac[(j + 1) * n + j];
            const T h = crd::math::sqrt(cc * cc + ss * ss);
            if (h == static_cast<T>(0))
            {
                continue;
            }
            const T c1 = cc / h;
            const T s1 = ss / h;
            rfac[j * n + j] = h;
            rfac[(j + 1) * n + j] = static_cast<T>(0);
            for (crd::usize col = j + 1; col < iq; ++col)
            {
                const T t1 = rfac[j * n + col];
                const T t2 = rfac[(j + 1) * n + col];
                rfac[j * n + col] = c1 * t1 + s1 * t2;
                rfac[(j + 1) * n + col] = -s1 * t1 + c1 * t2;
            }
            for (crd::usize i = 0; i < n; ++i)
            {
                const T t1 = jat(i, j);
                const T t2 = jat(i, j + 1);
                jat(i, j) = c1 * t1 + s1 * t2;
                jat(i, j + 1) = -s1 * t1 + c1 * t2;
            }
        }
    };
    auto slack = [&](crd::usize p) -> T
    {
        T s = c0[p];
        for (crd::usize j = 0; j < n; ++j)
        {
            s += cn[p * n + j] * x[j];
        }
        return s;
    };

    // ---- Phase 1: add every equality (full steps; never dropped).
    for (crd::usize p = 0; p < meq; ++p)
    {
        compute_d(p);
        compute_z();
        compute_r();
        T zn = static_cast<T>(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            zn += z[i] * cn[p * n + i];
        }
        const T sp = slack(p);
        if (crd::math::fabs(zn) < feas_tol)
        {
            if (crd::math::fabs(sp) > static_cast<T>(1e3) * feas_tol * (static_cast<T>(1) + crd::math::fabs(c0[p])))
            {
                result.status = QpStatus::PrimalInfeasible; // dependent + inconsistent equality
                return result;
            }
            continue; // dependent but consistent — skip
        }
        const T t2 = -sp / zn;
        for (crd::usize i = 0; i < n; ++i)
        {
            x[i] += t2 * z[i];
        }
        for (crd::usize k = 0; k < iq; ++k)
        {
            uact[k] -= t2 * r[k];
        }
        uact[iq] = t2;
        act[iq] = static_cast<crd::u32>(p);
        if (!add_constraint())
        {
            result.status = QpStatus::NumericalError;
            return result;
        }
    }

    // ---- Phase 2: the dual iteration over inequalities.
    const crd::usize max_steps = 50 * (mtot + n + 1);
    QpStatus status = QpStatus::Solved;
    for (crd::usize step = 0; step < max_steps; ++step)
    {
        // Most violated inactive inequality (lowest index on ties — deterministic).
        crd::usize p = mtot;
        T worst = -feas_tol;
        for (crd::usize k = meq; k < mtot; ++k)
        {
            bool active = false;
            for (crd::usize a2 = 0; a2 < iq && !active; ++a2)
            {
                active = act[a2] == k;
            }
            if (active)
            {
                continue;
            }
            const T s = slack(k);
            if (s < worst)
            {
                worst = s;
                p = k;
            }
        }
        if (p == mtot)
        {
            break; // primal feasible ⇒ optimal (dual feasibility is invariant)
        }

        T sp = worst;
        uact[iq] = static_cast<T>(0);
        bool resolved = false;
        for (crd::usize inner = 0; inner <= mtot + n + 1 && !resolved; ++inner)
        {
            compute_d(p);
            compute_z();
            compute_r();

            // t1: the blocking dual step over ACTIVE INEQUALITIES (k ≥ meq) with r_k > 0.
            T t1 = inf;
            crd::usize lidx = iq;
            for (crd::usize k = 0; k < iq; ++k)
            {
                if (act[k] >= meq && r[k] > static_cast<T>(0))
                {
                    const T cand = uact[k] / r[k];
                    if (cand < t1)
                    {
                        t1 = cand;
                        lidx = k;
                    }
                }
            }
            T zn = static_cast<T>(0);
            T znorm = static_cast<T>(0);
            for (crd::usize i = 0; i < n; ++i)
            {
                zn += z[i] * cn[p * n + i];
                znorm += z[i] * z[i];
            }
            const bool z_zero = crd::math::sqrt(znorm) < feas_tol;
            const T t2 = z_zero ? inf : -sp / zn;
            const T t = t1 < t2 ? t1 : t2;

            if (t >= inf)
            {
                status = QpStatus::PrimalInfeasible; // both steps unbounded ⇒ no feasible point
                resolved = true;
                break;
            }
            if (t2 >= inf) // dual-only step: drop the blocker, retry the same p
            {
                for (crd::usize k = 0; k < iq; ++k)
                {
                    uact[k] -= t * r[k];
                }
                uact[iq] += t;
                delete_constraint(lidx);
                continue;
            }
            for (crd::usize i = 0; i < n; ++i)
            {
                x[i] += t * z[i];
            }
            for (crd::usize k = 0; k < iq; ++k)
            {
                uact[k] -= t * r[k];
            }
            uact[iq] += t;
            if (t == t2) // full step: p becomes active
            {
                act[iq] = static_cast<crd::u32>(p);
                if (!add_constraint())
                {
                    status = QpStatus::NumericalError;
                    resolved = true;
                    break;
                }
                resolved = true;
            }
            else // partial step: drop the blocker, recompute the slack, continue with p
            {
                delete_constraint(lidx);
                sp = slack(p);
            }
        }
        if (status != QpStatus::Solved)
        {
            break;
        }
        if (!resolved)
        {
            status = QpStatus::MaxIterations;
            break;
        }
    }

    // Map duals back to the canonical rows (OSQP sign): y_row += sgn_k · u_k for each active k.
    for (crd::usize k = 0; k < iq; ++k)
    {
        const crd::usize gi = act[k];
        result.y[src[gi]] += sgn[gi] * uact[k];
    }
    result.iterations = iq;
    result.status = status;
    detail::qp_finalize<T>(prob, result, alloc);
    return result;
}

} // namespace crd::hesap::opt
