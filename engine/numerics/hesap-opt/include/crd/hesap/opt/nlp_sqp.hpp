#pragma once

// nlp_sqp.hpp — Phase 3.1.6 v7-n ⭐: SEQUENTIAL QUADRATIC PROGRAMMING — the smooth-constrained workhorse
// (N&W Algorithm 18.3, line-search SQP), INEQUALITY-capable:
//
//     min f(x)   s.t.   c_E(x) = 0,  c_I(x) ≥ 0     (Objective<T> + Constraints<T>, the v7-j interfaces)
//
// The iteration:
//   1. QP SUBPROBLEM (the v7-k consumer edge): min ½pᵀBp + ∇fᵀp s.t. J_E p = −c_E, J_I p ≥ −c_I, solved by
//      the Goldfarb-Idnani dual active-set (finite, exact duals, no feasible start needed — and B ≻ 0 always
//      holds, see 2). The QP duals become the new multiplier estimates.
//   2. **DAMPED BFGS** approximation of ∇²L (N&W Procedure 18.2): θ-damped update keeps B ≻ 0 — THE device
//      the v7-j prover lacked (exact-∇²L line-search SQP creeps on λ*≈0 problems like HS6 — measured there;
//      N&W prescribes damped BFGS for exactly this reason). No Hessian capability required.
//   3. ℓ1-merit Armijo backtracking with ν ≥ ‖multipliers‖∞ + margin (N&W 18.32-18.36) and the
//      **second-order correction** on a rejected full step (§15.6) — the restoration uses the equality rows
//      PLUS the inequalities violated at the trial point.
//   4. Stop on the 4-part KKT certificate (kkt.hpp semantics) ≤ opts.grad_tol.
// CONSTRAINED REPORTING: OptResult::multipliers = [λ (eq) ; μ (ineq, ≥ 0)]; kkt_residual = certificate max;
// grad_norm = the stationarity part. HONEST scope: no elastic mode — an infeasible QP linearization reports
// LineSearchFailed (IPOPT/SNOPT elasticity is future work, named). [gold: NLopt SLSQP, scipy SLSQP — v7-z]
// ADR-0090; N&W §18.1-18.3.
//
// DETERMINISM MOAT: serial scalar loops + the serial GI subproblem ⇒ trajectories bit-identical across runs
// and worker counts (the objective/constraint evals may be parallel-but-bit-exact).

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/opt/constraints.hpp>
#include <crd/hesap/opt/levenberg_marquardt.hpp> // detail::chol_solve (the SOC restoration solve)
#include <crd/hesap/opt/merit.hpp>
#include <crd/hesap/opt/objective.hpp>
#include <crd/hesap/opt/opt_types.hpp>
#include <crd/hesap/opt/qp.hpp>
#include <crd/hesap/opt/qp_active_set.hpp>
#include <crd/memory/allocator.hpp>

#include <crd/math/cmath.hpp>
#include <limits>

namespace crd::hesap::opt
{

// Minimize `obj` s.t. cons (equalities + inequalities) from `x0` by damped-BFGS line-search SQP.
// Requires obj.has_gradient() + cons.has_jacobians(). Convergence: KKT residual ≤ opts.grad_tol.
template <typename T>
[[nodiscard]] OptResult<T> minimize_sqp(const Objective<T>& obj, const Constraints<T>& cons,
                                        crd::containers::ConstSpan<T> x0, const OptOptions<T>& opts,
                                        crd::memory::IAllocator* alloc)
{
    CRD_ASSERT_MSG(obj.has_gradient(), "minimize_sqp needs an analytic gradient");
    CRD_ASSERT_MSG(cons.has_jacobians(), "minimize_sqp needs the constraint Jacobians");
    CRD_ASSERT_MSG(cons.n() == obj.n(), "minimize_sqp: objective/constraints dimension mismatch");
    const crd::usize n = obj.n();
    const crd::usize me = cons.num_eq();
    const crd::usize mi = cons.num_ineq();
    const crd::usize mall = me + mi;

    OptResult<T> result(alloc);
    result.x.resize(n);
    result.multipliers.resize(mall);
    for (crd::usize i = 0; i < n; ++i)
    {
        result.x[i] = x0[i];
    }
    for (crd::usize i = 0; i < mall; ++i)
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
    crd::containers::Array<T> ci(alloc);
    crd::containers::Array<T> ce_new(alloc);
    crd::containers::Array<T> ci_new(alloc);
    crd::containers::Array<T> je(alloc);
    crd::containers::Array<T> ji(alloc);
    crd::containers::Array<T> bmat(alloc); // the damped-BFGS approximation of ∇²L
    crd::containers::Array<T> x_new(alloc);
    crd::containers::Array<T> g_new(alloc);
    crd::containers::Array<T> gl_old(alloc); // ∇L at x (with the NEW multipliers — N&W 18.13)
    crd::containers::Array<T> qa(alloc);     // QP constraint matrix (mall × n)
    crd::containers::Array<T> ql(alloc);
    crd::containers::Array<T> qu(alloc);
    g.resize(n);
    ce.resize(me);
    ci.resize(mi);
    ce_new.resize(me);
    ci_new.resize(mi);
    je.resize(me * n);
    ji.resize(mi * n);
    bmat.resize(n * n);
    x_new.resize(n);
    g_new.resize(n);
    gl_old.resize(n);
    qa.resize(mall * n);
    ql.resize(mall);
    qu.resize(mall);
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            bmat[i * n + j] = i == j ? static_cast<T>(1) : static_cast<T>(0); // B₀ = I
        }
    }

    T* x = result.x.data();
    T* lam = result.multipliers.data();     // λ (length me)
    T* mu = result.multipliers.data() + me; // μ (length mi)

    T fx = obj.value({x, n});
    ++result.fn_evals;
    (void)obj.gradient({x, n}, {g.data(), n});
    ++result.grad_evals;
    cons.eval({x, n}, {ce.data(), me}, {ci.data(), mi});
    (void)cons.jacobians({x, n}, {je.data(), me * n}, {ji.data(), mi * n});

    // The 4-part KKT certificate from the CURRENT pieces.
    auto kkt_max = [&](T& stationarity_out) -> T
    {
        T st = static_cast<T>(0);
        for (crd::usize j = 0; j < n; ++j)
        {
            T acc = g[j];
            for (crd::usize i = 0; i < me; ++i)
            {
                acc -= lam[i] * je[i * n + j];
            }
            for (crd::usize i = 0; i < mi; ++i)
            {
                acc -= mu[i] * ji[i * n + j];
            }
            const T a = crd::math::fabs(acc);
            st = a > st ? a : st;
        }
        stationarity_out = st;
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
            const T dviol = mu[i] < static_cast<T>(0) ? -mu[i] : static_cast<T>(0);
            worst = dviol > worst ? dviol : worst;
            const T comp = crd::math::fabs(mu[i] * ci[i]);
            worst = comp > worst ? comp : worst;
        }
        return worst;
    };

    const T inf = std::numeric_limits<T>::infinity();
    T nu = static_cast<T>(1);
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

        // ---- The QP subproblem (v7-k GI): min ½pᵀBp + gᵀp s.t. J_E p = −c_E (eq rows), J_I p ≥ −c_I.
        for (crd::usize i = 0; i < me; ++i)
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                qa[i * n + j] = je[i * n + j];
            }
            ql[i] = -ce[i];
            qu[i] = -ce[i];
        }
        for (crd::usize i = 0; i < mi; ++i)
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                qa[(me + i) * n + j] = ji[i * n + j];
            }
            ql[me + i] = -ci[i];
            qu[me + i] = inf;
        }
        const QpProblem<T> qp{
            {bmat.data(), n * n}, {g.data(), n}, {qa.data(), mall * n}, {ql.data(), mall}, {qu.data(), mall}, n, mall};
        const QpResult<T> sub = solve_qp_goldfarb_idnani<T>(qp, alloc);
        if (sub.status != QpStatus::Solved)
        {
            status = OptStatus::LineSearchFailed; // infeasible linearization (no elastic mode — named scope)
            break;
        }
        const T* p = sub.x.data();
        // New multipliers from the QP duals (OSQP sign y ⇒ our convention λ = −y, μ = −y ≥ 0 on the ≥ rows).
        crd::containers::Array<T> lam_new(alloc);
        crd::containers::Array<T> mu_new(alloc);
        lam_new.resize(me);
        mu_new.resize(mi);
        for (crd::usize i = 0; i < me; ++i)
        {
            lam_new[i] = -sub.y[i];
        }
        for (crd::usize i = 0; i < mi; ++i)
        {
            const T m = -sub.y[me + i];
            mu_new[i] = m > static_cast<T>(0) ? m : static_cast<T>(0);
        }

        // ---- The QP's own KKT certificate: adopt the subproblem multipliers and re-test BEFORE the merit
        // machinery. When the QP returns p ≈ 0, its duals ARE the NLP multipliers and the merit has no descent
        // direction left — that is CONVERGENCE, not a stall (the textbook SQP stopping point). Without this,
        // an iterate that lands exactly on the solution (e.g. a polytope vertex reached in one step) reports
        // SmallStep with stale multipliers — caught by the v7-o modeling-layer gate.
        for (crd::usize i = 0; i < me; ++i)
        {
            lam[i] = lam_new[i];
        }
        for (crd::usize i = 0; i < mi; ++i)
        {
            mu[i] = mu_new[i];
        }
        kkt = kkt_max(stationarity);
        if (kkt <= opts.grad_tol)
        {
            status = OptStatus::Success;
            ++it;
            break;
        }

        // ---- Exact-penalty weight: ν ≥ ‖(λ⁺, μ⁺)‖∞ + margin.
        T dual_inf = inf_nrm({lam_new.data(), me});
        const T mu_inf = inf_nrm({mu_new.data(), mi});
        dual_inf = mu_inf > dual_inf ? mu_inf : dual_inf;
        if (nu < dual_inf + static_cast<T>(1))
        {
            nu = static_cast<T>(1.5) * (dual_inf + static_cast<T>(1));
        }

        // ---- ℓ1-merit Armijo with the second-order correction.
        const T phi0 = l1_merit_value<T>(fx, {ce.data(), me}, {ci.data(), mi}, nu);
        const T dphi = l1_merit_directional<T>({g.data(), n}, {p, n}, {ce.data(), me}, {ci.data(), mi},
                                               {je.data(), me * n}, {ji.data(), mi * n}, nu);
        if (!(dphi < static_cast<T>(0)))
        {
            status = OptStatus::SmallStep; // no merit descent at this scale
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
            cons.eval({x_new.data(), n}, {ce_new.data(), me}, {ci_new.data(), mi});
            const T phi = l1_merit_value<T>(fx_new, {ce_new.data(), me}, {ci_new.data(), mi}, nu);
            if (std::isfinite(phi) && phi <= phi0 + static_cast<T>(1e-4) * alpha * dphi)
            {
                accepted = true;
                break;
            }
            // Second-order correction on the rejected FULL step: restore feasibility through the equality
            // rows + the inequalities VIOLATED at the trial point (minimum-norm: p̂ = J_aᵀ(J_a J_aᵀ)⁻¹(−viol)).
            if (ls == 0 && mall > 0)
            {
                crd::containers::Array<crd::u32> arow(alloc); // active restoration rows (into [je; ji])
                crd::containers::Array<T> aviol(alloc);
                arow.resize(mall);
                aviol.resize(mall);
                crd::usize na = 0;
                for (crd::usize i = 0; i < me; ++i)
                {
                    arow[na] = static_cast<crd::u32>(i);
                    aviol[na] = ce_new[i];
                    ++na;
                }
                for (crd::usize i = 0; i < mi; ++i)
                {
                    if (ci_new[i] < static_cast<T>(0))
                    {
                        arow[na] = static_cast<crd::u32>(me + i);
                        aviol[na] = ci_new[i]; // negative violation; restoration drives it to 0
                        ++na;
                    }
                }
                if (na > 0 && na <= n)
                {
                    crd::containers::Array<T> jjt(alloc);
                    crd::containers::Array<T> yv(alloc);
                    jjt.resize(na * na);
                    yv.resize(na);
                    auto jrow = [&](crd::usize k) -> const T*
                    {
                        const crd::usize r = arow[k];
                        return r < me ? je.data() + r * n : ji.data() + (r - me) * n;
                    };
                    for (crd::usize i = 0; i < na; ++i)
                    {
                        for (crd::usize k = 0; k <= i; ++k)
                        {
                            T acc = static_cast<T>(0);
                            for (crd::usize j = 0; j < n; ++j)
                            {
                                acc += jrow(i)[j] * jrow(k)[j];
                            }
                            jjt[i * na + k] = acc;
                            jjt[k * na + i] = acc;
                        }
                        yv[i] = -aviol[i];
                    }
                    if (detail::chol_solve<T>(jjt.data(), na, yv.data()))
                    {
                        for (crd::usize j = 0; j < n; ++j)
                        {
                            T corr = static_cast<T>(0);
                            for (crd::usize k = 0; k < na; ++k)
                            {
                                corr += jrow(k)[j] * yv[k];
                            }
                            x_new[j] = x[j] + p[j] + corr;
                        }
                        fx_new = obj.value({x_new.data(), n});
                        ++result.fn_evals;
                        cons.eval({x_new.data(), n}, {ce_new.data(), me}, {ci_new.data(), mi});
                        const T phi_soc = l1_merit_value<T>(fx_new, {ce_new.data(), me}, {ci_new.data(), mi}, nu);
                        if (std::isfinite(phi_soc) && phi_soc <= phi0 + static_cast<T>(1e-4) * dphi)
                        {
                            accepted = true;
                            break;
                        }
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

        // ---- Damped-BFGS update (N&W Procedure 18.2) with y_L = ∇L(x⁺, duals⁺) − ∇L(x, duals⁺).
        for (crd::usize j = 0; j < n; ++j) // ∇L at the OLD x with the NEW multipliers
        {
            T acc = g[j];
            for (crd::usize i = 0; i < me; ++i)
            {
                acc -= lam_new[i] * je[i * n + j];
            }
            for (crd::usize i = 0; i < mi; ++i)
            {
                acc -= mu_new[i] * ji[i * n + j];
            }
            gl_old[j] = acc;
        }
        (void)obj.gradient({x_new.data(), n}, {g_new.data(), n});
        ++result.grad_evals;
        (void)cons.jacobians({x_new.data(), n}, {je.data(), me * n}, {ji.data(), mi * n}); // J at x⁺
        crd::containers::Array<T> svec(alloc);
        crd::containers::Array<T> yvec(alloc);
        crd::containers::Array<T> bs(alloc);
        svec.resize(n);
        yvec.resize(n);
        bs.resize(n);
        T step_norm_sq = static_cast<T>(0);
        for (crd::usize j = 0; j < n; ++j)
        {
            svec[j] = x_new[j] - x[j];
            step_norm_sq += svec[j] * svec[j];
            T acc = g_new[j];
            for (crd::usize i = 0; i < me; ++i)
            {
                acc -= lam_new[i] * je[i * n + j];
            }
            for (crd::usize i = 0; i < mi; ++i)
            {
                acc -= mu_new[i] * ji[i * n + j];
            }
            yvec[j] = acc - gl_old[j];
        }
        T sy = static_cast<T>(0);
        T sbs = static_cast<T>(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            T acc = static_cast<T>(0);
            for (crd::usize j = 0; j < n; ++j)
            {
                acc += bmat[i * n + j] * svec[j];
            }
            bs[i] = acc;
            sy += svec[i] * yvec[i];
            sbs += svec[i] * acc;
        }
        if (sbs > static_cast<T>(0) && step_norm_sq > static_cast<T>(0))
        {
            const T theta = sy >= static_cast<T>(0.2) * sbs
                                ? static_cast<T>(1)
                                : static_cast<T>(0.8) * sbs / (sbs - sy); // damping (sbs − sy > 0 here)
            T sr = static_cast<T>(0);
            for (crd::usize i = 0; i < n; ++i)
            {
                yvec[i] = theta * yvec[i] + (static_cast<T>(1) - theta) * bs[i]; // r = θy + (1−θ)Bs
                sr += svec[i] * yvec[i];
            }
            if (sr > static_cast<T>(0))
            {
                for (crd::usize i = 0; i < n; ++i)
                {
                    for (crd::usize j = 0; j < n; ++j)
                    {
                        bmat[i * n + j] += yvec[i] * yvec[j] / sr - bs[i] * bs[j] / sbs;
                    }
                }
            }
        }

        // ---- Accept.
        for (crd::usize i = 0; i < n; ++i)
        {
            x[i] = x_new[i];
            g[i] = g_new[i];
        }
        for (crd::usize i = 0; i < me; ++i)
        {
            ce[i] = ce_new[i];
            lam[i] = lam_new[i];
        }
        for (crd::usize i = 0; i < mi; ++i)
        {
            ci[i] = ci_new[i];
            mu[i] = mu_new[i];
        }
        fx = fx_new;
        kkt = kkt_max(stationarity);

        if (opts.step_tol > static_cast<T>(0) &&
            crd::math::sqrt(step_norm_sq) <= opts.step_tol * (static_cast<T>(1) + inf_nrm({x, n})))
        {
            status = OptStatus::SmallStep;
            break;
        }
    }

    result.fx = fx;
    result.grad_norm = stationarity;
    result.kkt_residual = kkt;
    result.iterations = it;
    result.status = status;
    result.converged = (status == OptStatus::Success);
    return result;
}

} // namespace crd::hesap::opt
