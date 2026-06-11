#pragma once

// nlp_auglag.hpp — Phase 3.1.6 v7-n: the AUGMENTED LAGRANGIAN method (LANCELOT-class; Powell-Hestenes-
// Rockafellar for the inequalities) — the robust, matrix-free member of the NLP trio (no QP subproblem, no
// KKT factorization: each outer iteration is one UNCONSTRAINED minimize of
//
//   L_A(x; λ, μ, ρ) = f − Σλ_i c_E,i + (ρ/2)Σc_E,i² + (1/2ρ)Σ[max(0, μ_i − ρ c_I,i)² − μ_i²]
//
// by the v7-d L-BFGS, followed by the first-order multiplier updates λ ← λ − ρc_E, μ ← [μ − ρc_I]₊ and the
// classical Bertsekas/LANCELOT (η, ω) schedule: constraint progress ⇒ tighten the inner tolerance + update
// multipliers; stall ⇒ ρ ← 10ρ. Scales to large n through L-BFGS; converges under weaker assumptions than
// SQP line search. Stop on the same 4-part KKT certificate. [gold: LANCELOT, scipy's outer ALM uses — v7-z]
// ADR-0090; Nocedal & Wright §17.3-17.4; Birgin-Martínez (ALGENCAN) for the PHR form.
//
// DETERMINISM: the outer loop + the serial L-BFGS inner ⇒ bit-identical runs/worker counts (evals may be
// parallel-but-bit-exact).

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/opt/constraints.hpp>
#include <crd/hesap/opt/lbfgs.hpp>
#include <crd/hesap/opt/objective.hpp>
#include <crd/hesap/opt/opt_types.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>

namespace crd::hesap::opt
{

namespace detail
{

// The augmented Lagrangian as an Objective<T> (the L-BFGS inner-solve adapter). Holds the CURRENT (λ, μ, ρ).
template <typename T> class AuglagObjective final : public Objective<T>
{
public:
    AuglagObjective(const Objective<T>& obj, const Constraints<T>& cons, crd::memory::IAllocator* alloc)
        : Objective<T>(/*has_gradient=*/true, false), m_obj(&obj), m_cons(&cons), m_ce(alloc), m_ci(alloc), m_je(alloc),
          m_ji(alloc), m_lam(alloc), m_mu(alloc)
    {
        const crd::usize n = obj.n();
        m_ce.resize(cons.num_eq());
        m_ci.resize(cons.num_ineq());
        m_je.resize(cons.num_eq() * n);
        m_ji.resize(cons.num_ineq() * n);
        m_lam.resize(cons.num_eq());
        m_mu.resize(cons.num_ineq());
        for (crd::usize i = 0; i < m_lam.size(); ++i)
        {
            m_lam[i] = static_cast<T>(0);
        }
        for (crd::usize i = 0; i < m_mu.size(); ++i)
        {
            m_mu[i] = static_cast<T>(0);
        }
    }

    [[nodiscard]] T value(crd::containers::ConstSpan<T> x) const override
    {
        const crd::usize me = m_ce.size();
        const crd::usize mi = m_ci.size();
        m_cons->eval(x, {m_ce.data(), me}, {m_ci.data(), mi});
        T acc = m_obj->value(x);
        for (crd::usize i = 0; i < me; ++i)
        {
            acc += -m_lam[i] * m_ce[i] + static_cast<T>(0.5) * m_rho * m_ce[i] * m_ce[i];
        }
        for (crd::usize i = 0; i < mi; ++i)
        {
            const T t = m_mu[i] - m_rho * m_ci[i];
            const T tp = t > static_cast<T>(0) ? t : static_cast<T>(0);
            acc += (tp * tp - m_mu[i] * m_mu[i]) / (static_cast<T>(2) * m_rho);
        }
        return acc;
    }

    [[nodiscard]] crd::usize n() const noexcept override { return m_obj->n(); }

    [[nodiscard]] bool gradient(crd::containers::ConstSpan<T> x, crd::containers::Span<T> g) const override
    {
        const crd::usize nn = m_obj->n();
        const crd::usize me = m_ce.size();
        const crd::usize mi = m_ci.size();
        (void)m_obj->gradient(x, g);
        m_cons->eval(x, {m_ce.data(), me}, {m_ci.data(), mi});
        (void)m_cons->jacobians(x, {m_je.data(), me * nn}, {m_ji.data(), mi * nn});
        for (crd::usize i = 0; i < me; ++i)
        {
            const T w = -m_lam[i] + m_rho * m_ce[i]; // d/dc of the equality terms
            for (crd::usize j = 0; j < nn; ++j)
            {
                g[j] += w * m_je[i * nn + j];
            }
        }
        for (crd::usize i = 0; i < mi; ++i)
        {
            const T t = m_mu[i] - m_rho * m_ci[i];
            if (t > static_cast<T>(0)) // the PHR max() branch
            {
                for (crd::usize j = 0; j < nn; ++j)
                {
                    g[j] -= t * m_ji[i * nn + j];
                }
            }
        }
        return true;
    }

    // The outer loop's knobs + read access to the latest constraint evaluation.
    void set_rho(T rho) noexcept { m_rho = rho; }
    [[nodiscard]] crd::containers::Span<T> lambdas() noexcept { return {m_lam.data(), m_lam.size()}; }
    [[nodiscard]] crd::containers::Span<T> mus() noexcept { return {m_mu.data(), m_mu.size()}; }
    [[nodiscard]] T rho() const noexcept { return m_rho; }

private:
    const Objective<T>* m_obj;
    const Constraints<T>* m_cons;
    mutable crd::containers::Array<T> m_ce;
    mutable crd::containers::Array<T> m_ci;
    mutable crd::containers::Array<T> m_je;
    mutable crd::containers::Array<T> m_ji;
    crd::containers::Array<T> m_lam;
    crd::containers::Array<T> m_mu;
    T m_rho = static_cast<T>(10);
};

} // namespace detail

template <typename T> struct AuglagOptions
{
    T rho0 = static_cast<T>(10); // initial penalty
    T rho_growth = static_cast<T>(10);
    crd::usize max_outer = 50;
    crd::usize max_inner = 500; // L-BFGS iterations per subproblem
};

// Minimize `obj` s.t. cons by the PHR augmented Lagrangian with L-BFGS inner solves. Requires
// obj.has_gradient() + cons.has_jacobians(). Convergence: KKT residual ≤ opts.grad_tol.
template <typename T>
[[nodiscard]] OptResult<T> minimize_auglag(const Objective<T>& obj, const Constraints<T>& cons,
                                           crd::containers::ConstSpan<T> x0, const OptOptions<T>& opts,
                                           crd::memory::IAllocator* alloc, const AuglagOptions<T>& aopts = {})
{
    CRD_ASSERT_MSG(obj.has_gradient(), "minimize_auglag needs an analytic gradient");
    CRD_ASSERT_MSG(cons.has_jacobians(), "minimize_auglag needs the constraint Jacobians");
    CRD_ASSERT_MSG(cons.n() == obj.n(), "minimize_auglag: objective/constraints dimension mismatch");
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

    detail::AuglagObjective<T> la(obj, cons, alloc);
    la.set_rho(aopts.rho0);

    crd::containers::Array<T> g(alloc);
    crd::containers::Array<T> ce(alloc);
    crd::containers::Array<T> ci(alloc);
    crd::containers::Array<T> je(alloc);
    crd::containers::Array<T> ji(alloc);
    g.resize(n);
    ce.resize(me);
    ci.resize(mi);
    je.resize(me * n);
    ji.resize(mi * n);

    T* x = result.x.data();
    T* lam = result.multipliers.data();
    T* mu = result.multipliers.data() + me;

    auto violation = [&]() -> T // ‖c_E‖∞ together with the PHR inequality measure ‖min(c_I, μ/ρ)‖∞
    {
        T v = static_cast<T>(0);
        for (crd::usize i = 0; i < me; ++i)
        {
            const T a = std::fabs(ce[i]);
            v = a > v ? a : v;
        }
        for (crd::usize i = 0; i < mi; ++i)
        {
            const T cap = la.mus()[i] / la.rho();
            const T m = ci[i] < cap ? ci[i] : cap;
            const T a = std::fabs(m);
            v = a > v ? a : v;
        }
        return v;
    };
    auto kkt_max = [&](T& stationarity_out) -> T
    {
        cons.eval({x, n}, {ce.data(), me}, {ci.data(), mi});
        (void)cons.jacobians({x, n}, {je.data(), me * n}, {ji.data(), mi * n});
        (void)obj.gradient({x, n}, {g.data(), n});
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
            const T a = std::fabs(acc);
            st = a > st ? a : st;
        }
        stationarity_out = st;
        T worst = st;
        for (crd::usize i = 0; i < me; ++i)
        {
            const T a = std::fabs(ce[i]);
            worst = a > worst ? a : worst;
        }
        for (crd::usize i = 0; i < mi; ++i)
        {
            const T viol = ci[i] < static_cast<T>(0) ? -ci[i] : static_cast<T>(0);
            worst = viol > worst ? viol : worst;
            const T comp = std::fabs(mu[i] * ci[i]);
            worst = comp > worst ? comp : worst;
        }
        return worst;
    };

    // The classical (η, ω) schedule (N&W Framework 17.4 / LANCELOT).
    T rho = aopts.rho0;
    T omega = static_cast<T>(1) / rho;                              // inner tolerance
    T eta = static_cast<T>(1) / std::pow(rho, static_cast<T>(0.1)); // required violation progress

    T stationarity = static_cast<T>(0);
    T kkt = std::numeric_limits<T>::max();
    OptStatus status = OptStatus::MaxIterations;
    crd::usize outer = 0;
    for (; outer < aopts.max_outer; ++outer)
    {
        // Inner: minimize L_A from the current x.
        OptOptions<T> inner_opts;
        inner_opts.grad_tol = omega > opts.grad_tol ? omega : opts.grad_tol;
        inner_opts.max_iters = aopts.max_inner;
        OptResult<T> inner = minimize_lbfgs<T>(la, {x, n}, inner_opts, alloc);
        result.fn_evals += inner.fn_evals;
        result.grad_evals += inner.grad_evals;
        for (crd::usize i = 0; i < n; ++i)
        {
            x[i] = inner.x[i];
        }

        cons.eval({x, n}, {ce.data(), me}, {ci.data(), mi});
        const T v = violation();
        if (v <= eta) // progress ⇒ first-order multiplier updates + tighten
        {
            for (crd::usize i = 0; i < me; ++i)
            {
                la.lambdas()[i] -= rho * ce[i];
                lam[i] = la.lambdas()[i];
            }
            for (crd::usize i = 0; i < mi; ++i)
            {
                const T m = la.mus()[i] - rho * ci[i];
                la.mus()[i] = m > static_cast<T>(0) ? m : static_cast<T>(0);
                mu[i] = la.mus()[i];
            }
            kkt = kkt_max(stationarity);
            if (kkt <= opts.grad_tol)
            {
                status = OptStatus::Success;
                ++outer;
                break;
            }
            omega = omega / rho;
            eta = eta / std::pow(rho, static_cast<T>(0.9));
        }
        else // stall ⇒ raise the penalty
        {
            rho *= aopts.rho_growth;
            la.set_rho(rho);
            omega = static_cast<T>(1) / rho;
            eta = static_cast<T>(1) / std::pow(rho, static_cast<T>(0.1));
        }
        if (opts.record_history)
        {
            result.history.push_back(obj.value({x, n}));
            ++result.fn_evals;
        }
    }

    result.fx = obj.value({x, n});
    ++result.fn_evals;
    result.grad_norm = stationarity;
    result.kkt_residual = kkt;
    result.iterations = outer;
    result.status = status;
    result.converged = (status == OptStatus::Success);
    return result;
}

} // namespace crd::hesap::opt
