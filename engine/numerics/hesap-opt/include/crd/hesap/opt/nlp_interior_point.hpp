#pragma once

// nlp_interior_point.hpp — Phase 3.1.6 v7-n-2 ⭐: the IPOPT-class **filter line-search interior-point** method
// (Wächter-Biegler 2006) — the third member of the NLP trio:
//
//     min f(x) s.t. c_E(x) = 0, c_I(x) ≥ 0   →   slack form: c_I(x) − s = 0, s ≥ 0, barrier −μ·Σ ln s_i.
//
// The primal-dual iteration (variables x, s; multipliers y_E and z = the s-duals ≈ μ/s):
//   • Eliminate (Δs, z⁺): the Newton system reduces to the saddle [W + J_IᵀΣJ_I, J_Eᵀ; J_E, 0] with
//     Σ = diag(z/s) — solved by the v7-j **inertia-corrected Bunch-Kaufman** (`solve_kkt_dense`): the
//     (n+, m−, 0) inertia test + δ·I ladder IS IPOPT's inertia-correction mechanism, already built. W = the
//     exact Lagrangian Hessian (∇²f + the `add_lagrangian_hessian` constraint curvature, evaluated at (y_E, z)).
//   • FRACTION-TO-BOUNDARY on s and z (τ = max(0.99, 1−μ)), then the **FILTER line search**: a trial point is
//     acceptable iff vs the current point AND every filter entry it improves the constraint violation θ or the
//     barrier objective φ_μ by the W-B margins (γ_θ = γ_φ = 1e-5); f-type steps (the switching condition
//     δ/s_θ/s_φ constants from the paper) must instead satisfy Armijo on φ_μ; θ-type acceptances AUGMENT the
//     filter. The filter resets per barrier problem.
//   • MONOTONE Fiacco-McCormick μ: the barrier problem is solved to E_μ ≤ κ_ε·μ, then
//     μ ← max(tol/10, min(κ_μ·μ, μ^{θ_μ})) (κ_ε=10, κ_μ=0.2, θ_μ=1.5 — the IPOPT defaults). Stop when the
//     ORIGINAL NLP error E_0 ≤ opts.grad_tol. z is κ_Σ-clipped into [μ/(κ_Σ s), κ_Σ μ/s] (the IPOPT safeguard).
//
// HONEST SCOPE (named): no RESTORATION PHASE — when the backtracking floors out the solver reports
// LineSearchFailed instead of entering feasibility restoration; no second-order correction inside the IPM
// (the SQP carries SOC); unscaled E (s_d = s_c = 1 — fine for well-scaled problems, scaling is v7-z polish).
// Requires the exact-Hessian capability (obj.has_hessian; constraint curvature via the v7-j hook when
// provided) — the Hessian-free members of the trio are the SQP (damped BFGS) and auglag (L-BFGS).
// [gold: IPOPT — the v7-z scoreboard; the install probe is part of this sub-slice]. ADR-0090; W-B 2006.
//
// DETERMINISM: serial fixed-order arithmetic throughout ⇒ bit-identical runs/worker counts.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/opt/constraints.hpp>
#include <crd/hesap/opt/kkt.hpp>
#include <crd/hesap/opt/objective.hpp>
#include <crd/hesap/opt/opt_types.hpp>
#include <crd/memory/allocator.hpp>

#include <crd/math/cmath.hpp>
#include <limits>

namespace crd::hesap::opt
{

template <typename T> struct IpmOptions
{
    T mu0 = static_cast<T>(0.1);
    T kappa_eps = static_cast<T>(10); // barrier-subproblem tolerance: E_μ ≤ κ_ε·μ
    T kappa_mu = static_cast<T>(0.2); // linear μ factor
    T theta_mu = static_cast<T>(1.5); // superlinear μ exponent
    T tau_min = static_cast<T>(0.99); // fraction-to-boundary floor
    T gamma_theta = static_cast<T>(1e-5);
    T gamma_phi = static_cast<T>(1e-5);
    T eta_phi = static_cast<T>(1e-8); // Armijo constant for f-type steps
    T s_theta = static_cast<T>(1.1);  // switching-condition exponents (W-B)
    T s_phi = static_cast<T>(2.3);
    T delta = static_cast<T>(1);
    T alpha_min = static_cast<T>(1e-11); // backtracking floor (no restoration phase — named scope)
};

// Minimize `obj` s.t. cons by the Wächter-Biegler filter interior-point method. Requires obj.has_gradient()
// + obj.has_hessian() + cons.has_jacobians(). Convergence: the NLP error E_0 ≤ opts.grad_tol.
template <typename T>
[[nodiscard]] OptResult<T> minimize_interior_point(const Objective<T>& obj, const Constraints<T>& cons,
                                                   crd::containers::ConstSpan<T> x0, const OptOptions<T>& opts,
                                                   crd::memory::IAllocator* alloc, const IpmOptions<T>& iopts = {})
{
    CRD_ASSERT_MSG(obj.has_gradient() && obj.has_hessian(),
                   "minimize_interior_point needs gradient + dense hessian (use SQP/auglag for Hessian-free)");
    CRD_ASSERT_MSG(cons.has_jacobians(), "minimize_interior_point needs the constraint Jacobians");
    CRD_ASSERT_MSG(cons.n() == obj.n(), "minimize_interior_point: objective/constraints dimension mismatch");
    const crd::usize n = obj.n();
    const crd::usize me = cons.num_eq();
    const crd::usize mi = cons.num_ineq();

    OptResult<T> result(alloc);
    result.x.resize(n);
    result.multipliers.resize(me + mi);
    for (crd::usize i = 0; i < n; ++i)
    {
        result.x[i] = x0[i];
    }
    for (crd::usize i = 0; i < me + mi; ++i)
    {
        result.multipliers[i] = static_cast<T>(0);
    }
    if (n == 0)
    {
        result.status = OptStatus::Success;
        result.converged = true;
        return result;
    }

    [[maybe_unused]] auto inf_nrm = [](crd::containers::ConstSpan<T> v) -> T
    {
        T mx = static_cast<T>(0);
        for (crd::usize i = 0; i < v.size(); ++i)
        {
            const T a = crd::math::fabs(v[i]);
            mx = a > mx ? a : mx;
        }
        return mx;
    };

    crd::containers::Array<T> g(alloc);
    crd::containers::Array<T> ce(alloc);
    crd::containers::Array<T> ci(alloc);
    crd::containers::Array<T> je(alloc);
    crd::containers::Array<T> ji(alloc);
    crd::containers::Array<T> w(alloc);
    crd::containers::Array<T> s(alloc);
    crd::containers::Array<T> z(alloc);
    crd::containers::Array<T> dx(alloc);
    crd::containers::Array<T> ds(alloc);
    crd::containers::Array<T> znew(alloc);
    crd::containers::Array<T> ye_new(alloc);
    crd::containers::Array<T> gmod(alloc);
    crd::containers::Array<T> x_t(alloc);
    crd::containers::Array<T> s_t(alloc);
    crd::containers::Array<T> ce_t(alloc);
    crd::containers::Array<T> ci_t(alloc);
    g.resize(n);
    ce.resize(me);
    ci.resize(mi);
    je.resize(me * n);
    ji.resize(mi * n);
    w.resize(n * n);
    s.resize(mi);
    z.resize(mi);
    dx.resize(n);
    ds.resize(mi);
    znew.resize(mi);
    ye_new.resize(me);
    gmod.resize(n);
    x_t.resize(n);
    s_t.resize(mi);
    ce_t.resize(me);
    ci_t.resize(mi);

    T* x = result.x.data();
    T* ye = result.multipliers.data();      // y_E
    T* zr = result.multipliers.data() + me; // z reported as the c_I duals (≥ 0)

    // ---- Initialization: s from c_I (pushed strictly positive), z = μ/s, y_E = LS estimate.
    T mu = iopts.mu0;
    cons.eval({x, n}, {ce.data(), me}, {ci.data(), mi});
    for (crd::usize i = 0; i < mi; ++i)
    {
        s[i] = ci[i] > static_cast<T>(1e-2) ? ci[i] : static_cast<T>(1e-2);
        z[i] = mu / s[i];
    }
    if (me > 0)
    {
        estimate_eq_multipliers<T>(obj, cons, {x, n}, {ye, me}, alloc);
    }

    auto theta_of = [&](crd::containers::ConstSpan<T> cev, crd::containers::ConstSpan<T> civ,
                        crd::containers::ConstSpan<T> sv) -> T // θ = Σ|c_E| + Σ|c_I − s| (1-norm)
    {
        T t = static_cast<T>(0);
        for (crd::usize i = 0; i < me; ++i)
        {
            t += crd::math::fabs(cev[i]);
        }
        for (crd::usize i = 0; i < mi; ++i)
        {
            t += crd::math::fabs(civ[i] - sv[i]);
        }
        return t;
    };
    auto phi_of = [&](T fval, crd::containers::ConstSpan<T> sv) -> T // φ_μ = f − μΣ ln s
    {
        T p = fval;
        for (crd::usize i = 0; i < mi; ++i)
        {
            p -= mu * crd::math::log(sv[i]);
        }
        return p;
    };

    // The filter: (θ, φ) pairs, reset per barrier problem; initialized with the θ-cap row.
    crd::containers::Array<T> filt_theta(alloc);
    crd::containers::Array<T> filt_phi(alloc);
    crd::usize filt_n = 0;
    filt_theta.resize(opts.max_iters + 2);
    filt_phi.resize(opts.max_iters + 2);
    T fx = obj.value({x, n});
    ++result.fn_evals;
    const T theta0_init = theta_of({ce.data(), me}, {ci.data(), mi}, {s.data(), mi});
    const T theta_max = static_cast<T>(1e4) * (theta0_init > static_cast<T>(1) ? theta0_init : static_cast<T>(1));
    auto filter_reset = [&]()
    {
        filt_n = 0;
        filt_theta[filt_n] = theta_max; // the cap row: any θ ≥ θ_max is rejected outright
        filt_phi[filt_n] = -std::numeric_limits<T>::infinity();
        ++filt_n;
    };
    filter_reset();

    OptStatus status = OptStatus::MaxIterations;
    crd::usize it = 0;
    for (; it < opts.max_iters; ++it)
    {
        if (opts.record_history)
        {
            result.history.push_back(fx);
        }
        (void)obj.gradient({x, n}, {g.data(), n});
        ++result.grad_evals;
        cons.eval({x, n}, {ce.data(), me}, {ci.data(), mi});
        (void)cons.jacobians({x, n}, {je.data(), me * n}, {ji.data(), mi * n});

        // ---- E_μ: max(stationarity, ‖c_E‖∞, ‖c_I − s‖∞, ‖s∘z − μ‖∞). E_0 = the same with μ = 0.
        auto error_at = [&](T mu_val) -> T
        {
            T e = static_cast<T>(0);
            for (crd::usize j = 0; j < n; ++j)
            {
                T acc = g[j];
                for (crd::usize i = 0; i < me; ++i)
                {
                    acc -= ye[i] * je[i * n + j];
                }
                for (crd::usize i = 0; i < mi; ++i)
                {
                    acc -= z[i] * ji[i * n + j];
                }
                const T a = crd::math::fabs(acc);
                e = a > e ? a : e;
            }
            for (crd::usize i = 0; i < me; ++i)
            {
                const T a = crd::math::fabs(ce[i]);
                e = a > e ? a : e;
            }
            for (crd::usize i = 0; i < mi; ++i)
            {
                const T a = crd::math::fabs(ci[i] - s[i]);
                e = a > e ? a : e;
                const T c = crd::math::fabs(s[i] * z[i] - mu_val);
                e = c > e ? c : e;
            }
            return e;
        };
        const T e0 = error_at(static_cast<T>(0));
        if (e0 <= opts.grad_tol)
        {
            status = OptStatus::Success;
            break;
        }
        if (mi > 0 && error_at(mu) <= iopts.kappa_eps * mu && mu > opts.grad_tol / static_cast<T>(10))
        {
            // The barrier problem is solved to its tolerance ⇒ shrink μ (monotone Fiacco-McCormick) + reset.
            const T lin = iopts.kappa_mu * mu;
            const T sup = crd::math::pow(mu, iopts.theta_mu);
            T next = lin < sup ? lin : sup;
            const T floor_mu = opts.grad_tol / static_cast<T>(10);
            mu = next > floor_mu ? next : floor_mu;
            filter_reset();
            continue;
        }

        // ---- The reduced primal-dual Newton system through the v7-j saddle.
        (void)obj.hessian({x, n}, {w.data(), n * n});
        ++result.hess_evals;
        if (cons.has_lagrangian_hessian())
        {
            (void)cons.add_lagrangian_hessian({x, n}, {ye, me}, {z.data(), mi}, {w.data(), n * n});
        }
        for (crd::usize i = 0; i < mi; ++i) // W += J_IᵀΣJ_I (Σ = z/s)
        {
            const T sig = z[i] / s[i];
            for (crd::usize r = 0; r < n; ++r)
            {
                const T jr = ji[i * n + r];
                if (jr != static_cast<T>(0))
                {
                    for (crd::usize c2 = 0; c2 < n; ++c2)
                    {
                        w[r * n + c2] += sig * jr * ji[i * n + c2];
                    }
                }
            }
        }
        // g_mod = ∇f − J_Iᵀ(μ/s − Σ·r_I), with r_I = c_I − s (the eliminated-block contribution).
        for (crd::usize j = 0; j < n; ++j)
        {
            gmod[j] = g[j];
        }
        for (crd::usize i = 0; i < mi; ++i)
        {
            const T ri = ci[i] - s[i];
            const T wterm = mu / s[i] - (z[i] / s[i]) * ri;
            for (crd::usize j = 0; j < n; ++j)
            {
                gmod[j] -= wterm * ji[i * n + j];
            }
        }
        const auto ks = solve_kkt_dense<T>(alloc, {w.data(), n * n}, {je.data(), me * n}, {gmod.data(), n},
                                           {ce.data(), me}, {dx.data(), n}, {ye_new.data(), me});
        if (!ks.solved)
        {
            status = OptStatus::LineSearchFailed; // the regularization ladder ran out
            break;
        }
        for (crd::usize i = 0; i < mi; ++i) // Δs = J_IΔx + r_I ; z⁺ = μ/s − Σ(J_IΔx + r_I)
        {
            T jdx = static_cast<T>(0);
            for (crd::usize j = 0; j < n; ++j)
            {
                jdx += ji[i * n + j] * dx[j];
            }
            const T ri = ci[i] - s[i];
            ds[i] = jdx + ri;
            znew[i] = mu / s[i] - (z[i] / s[i]) * (jdx + ri);
        }

        // ---- Fraction-to-boundary.
        const T tau = iopts.tau_min > (static_cast<T>(1) - mu) ? iopts.tau_min : (static_cast<T>(1) - mu);
        T alpha_max = static_cast<T>(1);
        for (crd::usize i = 0; i < mi; ++i)
        {
            if (ds[i] < static_cast<T>(0))
            {
                const T a = -tau * s[i] / ds[i];
                alpha_max = a < alpha_max ? a : alpha_max;
            }
        }
        T alpha_z = static_cast<T>(1);
        for (crd::usize i = 0; i < mi; ++i)
        {
            const T dz = znew[i] - z[i];
            if (dz < static_cast<T>(0))
            {
                const T a = -tau * z[i] / dz;
                alpha_z = a < alpha_z ? a : alpha_z;
            }
        }

        // ---- The filter line search.
        const T theta0 = theta_of({ce.data(), me}, {ci.data(), mi}, {s.data(), mi});
        const T phi0 = phi_of(fx, {s.data(), mi});
        T dphi = static_cast<T>(0); // ∇φᵀd = gᵀΔx − μ Σ Δs_i/s_i
        for (crd::usize j = 0; j < n; ++j)
        {
            dphi += g[j] * dx[j];
        }
        for (crd::usize i = 0; i < mi; ++i)
        {
            dphi -= mu * ds[i] / s[i];
        }

        T alpha = alpha_max;
        T fx_t = fx;
        T theta_t = theta0;
        bool accepted = false;
        bool f_type_step = false;
        while (alpha >= iopts.alpha_min)
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                x_t[j] = x[j] + alpha * dx[j];
            }
            for (crd::usize i = 0; i < mi; ++i)
            {
                s_t[i] = s[i] + alpha * ds[i];
            }
            fx_t = obj.value({x_t.data(), n});
            ++result.fn_evals;
            cons.eval({x_t.data(), n}, {ce_t.data(), me}, {ci_t.data(), mi});
            theta_t = theta_of({ce_t.data(), me}, {ci_t.data(), mi}, {s_t.data(), mi});
            const T phi_t = phi_of(fx_t, {s_t.data(), mi});
            if (std::isfinite(phi_t) && std::isfinite(theta_t))
            {
                // Acceptable to the filter AND to the current point (the W-B margins).
                bool ok = theta_t <= (static_cast<T>(1) - iopts.gamma_theta) * theta0 ||
                          phi_t <= phi0 - iopts.gamma_phi * theta0;
                for (crd::usize k = 0; k < filt_n && ok; ++k)
                {
                    ok = theta_t <= (static_cast<T>(1) - iopts.gamma_theta) * filt_theta[k] ||
                         phi_t <= filt_phi[k] - iopts.gamma_phi * filt_theta[k];
                }
                if (ok)
                {
                    // Switching condition ⇒ f-type: additionally require Armijo on φ.
                    const T m = alpha * dphi;
                    const bool switching = dphi < static_cast<T>(0) &&
                                           crd::math::pow(-m, iopts.s_phi) > iopts.delta * crd::math::pow(theta0, iopts.s_theta);
                    if (switching)
                    {
                        if (phi_t <= phi0 + iopts.eta_phi * alpha * dphi)
                        {
                            accepted = true;
                            f_type_step = true;
                            break;
                        }
                    }
                    else
                    {
                        accepted = true; // θ-type acceptance (the filter is augmented below)
                        break;
                    }
                }
            }
            alpha *= static_cast<T>(0.5);
        }
        if (!accepted)
        {
            status = OptStatus::LineSearchFailed; // no restoration phase — named scope
            break;
        }
        if (!f_type_step) // θ-type ⇒ augment the filter with the CURRENT point's margins
        {
            filt_theta[filt_n] = (static_cast<T>(1) - iopts.gamma_theta) * theta0;
            filt_phi[filt_n] = phi0 - iopts.gamma_phi * theta0;
            ++filt_n;
        }

        // ---- Accept + dual updates (+ the κ_Σ clip).
        for (crd::usize j = 0; j < n; ++j)
        {
            x[j] = x_t[j];
        }
        for (crd::usize i = 0; i < mi; ++i)
        {
            s[i] = s_t[i];
            z[i] = z[i] + alpha_z * (znew[i] - z[i]);
            const T lo = mu / (static_cast<T>(1e10) * s[i]);
            const T hi = static_cast<T>(1e10) * mu / s[i];
            z[i] = z[i] < lo ? lo : (z[i] > hi ? hi : z[i]);
        }
        for (crd::usize i = 0; i < me; ++i)
        {
            ye[i] = ye_new[i];
        }
        fx = fx_t;
    }

    for (crd::usize i = 0; i < mi; ++i)
    {
        zr[i] = z[i];
    }
    // The exit bookkeeping: recompute the NLP error pieces at the final point.
    cons.eval({x, n}, {ce.data(), me}, {ci.data(), mi});
    (void)cons.jacobians({x, n}, {je.data(), me * n}, {ji.data(), mi * n});
    (void)obj.gradient({x, n}, {g.data(), n});
    T st = static_cast<T>(0);
    for (crd::usize j = 0; j < n; ++j)
    {
        T acc = g[j];
        for (crd::usize i = 0; i < me; ++i)
        {
            acc -= ye[i] * je[i * n + j];
        }
        for (crd::usize i = 0; i < mi; ++i)
        {
            acc -= z[i] * ji[i * n + j];
        }
        const T a = crd::math::fabs(acc);
        st = a > st ? a : st;
    }
    T worst = st;
    for (crd::usize i = 0; i < me; ++i)
    {
        const T a = crd::math::fabs(ce[i]);
        worst = a > worst ? a : worst;
    }
    for (crd::usize i = 0; i < mi; ++i)
    {
        const T viol = ci[i] < static_cast<T>(0) ? -ci[i] : static_cast<T>(0);
        worst = viol > worst ? viol : worst;
        const T comp = crd::math::fabs(z[i] * ci[i]);
        worst = comp > worst ? comp : worst;
    }
    result.fx = fx;
    result.grad_norm = st;
    result.kkt_residual = worst;
    result.iterations = it;
    result.status = status;
    result.converged = (status == OptStatus::Success);
    return result;
}

} // namespace crd::hesap::opt
