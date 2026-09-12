#pragma once

// sqp_equality.hpp — Phase 3.1.6 v7-j: EQUALITY-CONSTRAINED SQP — the substrate PROVER (the v7-a
// gradient-descent analog for the constrained substrate): it exercises every v7-j piece end-to-end —
// Constraints + Jacobians → the Bunch-Kaufman KKT solve with the inertia correction → the ℓ1 merit line
// search → multipliers + the KKT-residual certificate in OptResult. Full SQP with INEQUALITIES (working
// sets / QP subproblems) is v7-n on the v7-k QP; this driver requires num_ineq() == 0.
//
// The iteration (N&W Algorithm 18.3, line-search SQP):
//   1. W = ∇²f(x) [+ the constraint-curvature contribution when has_lagrangian_hessian()] — without the
//      contribution on NONLINEAR constraints this is the standard Hessian-of-objective approximation.
//   2. Solve [W J_Eᵀ; J_E 0]·[p; z] = [−∇f; −c]   ⇒ step p, multipliers λ⁺ = −z (kkt.hpp, inertia-corrected).
//   3. Keep the ℓ1 penalty exact: ν ≥ ‖λ⁺‖∞ + margin (N&W 18.32-18.36).
//   4. Armijo backtracking on φ(x+αp; ν) against the directional derivative D(φ; p) (merit.hpp) — with the
//      SECOND-ORDER CORRECTION (N&W §15.6, Alg 18.3): when the FULL step is rejected, p̂ = Jᵀ(JJᵀ)⁻¹(−c(x+p))
//      restores feasibility to third order and x+p+p̂ is retried — the standard cure for the Maratos effect /
//      ℓ1-merit creep on curved constraints (HS6 creeps at α~1e-4 without it — measured).
//   5. x ← x + αp (or x + p + p̂), λ ← λ⁺. Stop when the KKT residual max ≤ opts.grad_tol.
// CONSTRAINED REPORTING SEMANTICS: OptResult::grad_norm = the STATIONARITY part ‖∇L‖∞ (not ‖∇f‖∞);
// kkt_residual = the full certificate max; multipliers = λ. ADR-0090; N&W §18.1-18.3.
//
// DETERMINISM MOAT: serial scalar loops + the serial Bunch-Kaufman; only the objective/constraint evals may
// be parallel-but-bit-exact ⇒ the trajectory is bit-identical across worker counts.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/opt/constraints.hpp>
#include <crd/hesap/opt/kkt.hpp>
#include <crd/hesap/opt/merit.hpp>
#include <crd/hesap/opt/objective.hpp>
#include <crd/hesap/opt/opt_types.hpp>
#include <crd/memory/allocator.hpp>

#include <crd/math/cmath.hpp>

namespace crd::hesap::opt
{

// Minimize `obj` s.t. cons.c_E(x) = 0 from `x0` by line-search SQP. Requires obj.has_gradient() +
// obj.has_hessian() + cons.has_jacobians() + cons.num_ineq() == 0. Convergence: KKT residual ≤ opts.grad_tol.
template <typename T>
[[nodiscard]] OptResult<T> minimize_sqp_equality(const Objective<T>& obj, const Constraints<T>& cons,
                                                 crd::containers::ConstSpan<T> x0, const OptOptions<T>& opts,
                                                 crd::memory::IAllocator* alloc)
{
    CRD_ASSERT_MSG(obj.has_gradient() && obj.has_hessian(), "minimize_sqp_equality needs gradient + dense hessian");
    CRD_ASSERT_MSG(cons.has_jacobians(), "minimize_sqp_equality needs the constraint Jacobians");
    CRD_ASSERT_MSG(cons.num_ineq() == 0, "minimize_sqp_equality is equality-only (inequalities land at v7-n)");
    CRD_ASSERT_MSG(cons.n() == obj.n(), "minimize_sqp_equality: objective/constraints dimension mismatch");
    const crd::usize n = obj.n();
    const crd::usize m = cons.num_eq();

    OptResult<T> result(alloc);
    result.x.resize(n);
    result.multipliers.resize(m);
    for (crd::usize i = 0; i < n; ++i)
    {
        result.x[i] = x0[i];
    }
    for (crd::usize i = 0; i < m; ++i)
    {
        result.multipliers[i] = static_cast<T>(0);
    }
    if (n == 0)
    {
        result.status = OptStatus::Success;
        result.converged = true;
        return result;
    }

    auto inf_nrm = [](crd::containers::ConstSpan<T> v) -> T
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
    crd::containers::Array<T> ce_new(alloc);
    crd::containers::Array<T> je(alloc);
    crd::containers::Array<T> w(alloc);
    crd::containers::Array<T> p(alloc);
    crd::containers::Array<T> lam_new(alloc);
    crd::containers::Array<T> x_new(alloc);
    crd::containers::Array<T> empty_i(alloc); // zero-length inequality blocks for eval/merit calls
    g.resize(n);
    ce.resize(m);
    ce_new.resize(m);
    je.resize(m * n);
    w.resize(n * n);
    p.resize(n);
    lam_new.resize(m);
    x_new.resize(n);

    T* x = result.x.data();
    T* lam = result.multipliers.data();

    T fx = obj.value({x, n});
    ++result.fn_evals;
    (void)obj.gradient({x, n}, {g.data(), n});
    ++result.grad_evals;
    cons.eval({x, n}, {ce.data(), m}, {empty_i.data(), 0});
    (void)cons.jacobians({x, n}, {je.data(), m * n}, {empty_i.data(), 0});

    // Initialize λ by the least-squares estimate (kkt.hpp) — the standard cold-start.
    if (m > 0)
    {
        estimate_eq_multipliers<T>(obj, cons, {x, n}, {lam, m}, alloc);
    }

    // Stationarity ‖∇f − J_Eᵀλ‖∞ + primal ‖c_E‖∞ from the CURRENT (g, je, ce, λ).
    auto kkt_max = [&](T& stationarity_out) -> T
    {
        T st = static_cast<T>(0);
        for (crd::usize j = 0; j < n; ++j)
        {
            T acc = g[j];
            for (crd::usize i = 0; i < m; ++i)
            {
                acc -= lam[i] * je[i * n + j];
            }
            const T a = crd::math::fabs(acc);
            st = a > st ? a : st;
        }
        stationarity_out = st;
        const T pr = inf_nrm({ce.data(), m});
        return st > pr ? st : pr;
    };

    T nu = static_cast<T>(1); // ℓ1 penalty weight (raised above ‖λ⁺‖∞ each iteration)
    T stationarity = static_cast<T>(0);
    T kkt = kkt_max(stationarity);
    OptStatus status = OptStatus::MaxIterations;
    crd::usize it = 0;
    for (; it < opts.max_iters; ++it)
    {
        if (opts.record_history)
        {
            result.history.push_back(fx);
        }
        if (kkt <= opts.grad_tol)
        {
            status = OptStatus::Success;
            break;
        }

        // W = ∇²f (+ the constraint curvature −Σλ∇²c when provided).
        (void)obj.hessian({x, n}, {w.data(), n * n});
        ++result.hess_evals;
        if (cons.has_lagrangian_hessian())
        {
            (void)cons.add_lagrangian_hessian({x, n}, {lam, m}, {empty_i.data(), 0}, {w.data(), n * n});
        }

        const auto ks = solve_kkt_dense<T>(alloc, {w.data(), n * n}, {je.data(), m * n}, {g.data(), n}, {ce.data(), m},
                                           {p.data(), n}, {lam_new.data(), m});
        if (!ks.solved)
        {
            status = OptStatus::LineSearchFailed; // the regularization ladder ran out
            break;
        }

        // Keep the ℓ1 penalty exact: ν ≥ ‖λ⁺‖∞ + margin (N&W 18.32-18.36).
        const T lam_inf = inf_nrm({lam_new.data(), m});
        if (nu < lam_inf + static_cast<T>(1))
        {
            nu = static_cast<T>(1.5) * (lam_inf + static_cast<T>(1));
        }

        // Armijo backtracking on the ℓ1 merit (D(φ; p) from merit.hpp must be negative).
        const T phi0 = l1_merit_value<T>(fx, {ce.data(), m}, {empty_i.data(), 0}, nu);
        const T dphi = l1_merit_directional<T>({g.data(), n}, {p.data(), n}, {ce.data(), m}, {empty_i.data(), 0},
                                               {je.data(), m * n}, {empty_i.data(), 0}, nu);
        if (!(dphi < static_cast<T>(0)))
        {
            status = OptStatus::SmallStep; // no merit descent at this scale (already at a stationary kink)
            break;
        }
        T alpha = static_cast<T>(1);
        T fx_new = fx;
        bool accepted = false;
        for (int ls = 0; ls < 50; ++ls)
        {
            for (crd::usize i = 0; i < n; ++i)
            {
                x_new[i] = x[i] + alpha * p[i];
            }
            fx_new = obj.value({x_new.data(), n});
            ++result.fn_evals;
            cons.eval({x_new.data(), n}, {ce_new.data(), m}, {empty_i.data(), 0});
            const T phi = l1_merit_value<T>(fx_new, {ce_new.data(), m}, {empty_i.data(), 0}, nu);
            if (std::isfinite(phi) && phi <= phi0 + static_cast<T>(1e-4) * alpha * dphi)
            {
                accepted = true;
                break;
            }
            // SECOND-ORDER CORRECTION on the rejected FULL step (N&W §15.6): the rejection is typically the
            // constraint CURVATURE c(x+p) (the linearized step is feasible to first order only). The minimum-norm
            // feasibility restoration p̂ = Jᵀ(JJᵀ)⁻¹(−c(x+p)) cancels it to third order; retry x + p + p̂ against
            // the SAME Armijo bound. Without this the merit creeps at α ≪ 1 on curved constraints (HS6, measured).
            if (ls == 0 && m > 0)
            {
                crd::containers::Array<T> jjt(alloc); // J·Jᵀ (m×m, SPD for full-rank J)
                crd::containers::Array<T> y(alloc);
                jjt.resize(m * m);
                y.resize(m);
                for (crd::usize i = 0; i < m; ++i)
                {
                    for (crd::usize k = 0; k <= i; ++k)
                    {
                        T acc = static_cast<T>(0);
                        for (crd::usize j = 0; j < n; ++j)
                        {
                            acc += je[i * n + j] * je[k * n + j];
                        }
                        jjt[i * m + k] = acc;
                        jjt[k * m + i] = acc;
                    }
                    y[i] = -ce_new[i];
                }
                if (detail::chol_solve<T>(jjt.data(), m, y.data()))
                {
                    for (crd::usize i = 0; i < n; ++i) // x_soc = x + p + Jᵀy
                    {
                        T corr = static_cast<T>(0);
                        for (crd::usize k = 0; k < m; ++k)
                        {
                            corr += je[k * n + i] * y[k];
                        }
                        x_new[i] = x[i] + p[i] + corr;
                    }
                    fx_new = obj.value({x_new.data(), n});
                    ++result.fn_evals;
                    cons.eval({x_new.data(), n}, {ce_new.data(), m}, {empty_i.data(), 0});
                    const T phi_soc = l1_merit_value<T>(fx_new, {ce_new.data(), m}, {empty_i.data(), 0}, nu);
                    if (std::isfinite(phi_soc) && phi_soc <= phi0 + static_cast<T>(1e-4) * dphi)
                    {
                        accepted = true;
                        break;
                    }
                }
            }
            alpha *= static_cast<T>(0.5);
        }
        if (!accepted)
        {
            status = OptStatus::LineSearchFailed;
            break;
        }

        T step_norm_sq = static_cast<T>(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            const T d = x_new[i] - x[i]; // covers both the α·p and the SOC-corrected step
            step_norm_sq += d * d;
            x[i] = x_new[i];
        }
        for (crd::usize i = 0; i < m; ++i)
        {
            ce[i] = ce_new[i];
            lam[i] = lam_new[i]; // the QP multipliers become the new iterate (N&W 18.3)
        }
        fx = fx_new;
        (void)obj.gradient({x, n}, {g.data(), n});
        ++result.grad_evals;
        (void)cons.jacobians({x, n}, {je.data(), m * n}, {empty_i.data(), 0});
        kkt = kkt_max(stationarity);

        if (opts.step_tol > static_cast<T>(0) &&
            crd::math::sqrt(step_norm_sq) <= opts.step_tol * (static_cast<T>(1) + inf_nrm({x, n})))
        {
            status = OptStatus::SmallStep;
            break;
        }
    }

    result.fx = fx;
    result.grad_norm = stationarity; // CONSTRAINED semantics: ‖∇L‖∞ (the stationarity part), not ‖∇f‖∞
    result.kkt_residual = kkt;
    result.iterations = it;
    result.status = status;
    result.converged = (status == OptStatus::Success);
    return result;
}

} // namespace crd::hesap::opt

