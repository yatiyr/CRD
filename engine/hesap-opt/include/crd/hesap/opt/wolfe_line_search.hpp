#pragma once

// wolfe_line_search.hpp — Phase 3.1.6 v7-c: a line search satisfying the (strong) Wolfe conditions via the
// Nocedal & Wright bracketing + zoom (Algorithm 3.5 / 3.6). This is the textbook-robust quasi-Newton line search:
// it enforces both Armijo sufficient decrease AND the curvature condition, so the step it returns is the kind
// L-BFGS / nonlinear-CG (v7-d/f) require for a stable Hessian update. ADR-0090.
//
//   Armijo:    φ(α) ≤ φ(0) + c1·α·φ'(0)
//   curvature: strong  |φ'(α)| ≤ c2·|φ'(0)|     (default — needed for BFGS positive-definiteness)
//              weak     φ'(α) ≥ c2·φ'(0)         (one-sided; cheaper, fine for CG)
// with φ(α) = f(x + α·p), φ'(α) = ∇f(x + α·p)·p, 0 < c1 < c2 < 1.
//
// COST CONTRACT (v7-a): this search evaluates ∇f at trial points (the curvature test needs φ'(α)), so on success
// it returns the accepted point in x_out AND its gradient in g_out with grad_at_new_valid=true — the optimizer
// then skips its own gradient eval. Zoom uses bisection (deterministic, robust); the cubic-interpolation
// gold-standard is More-Thuente (more_thuente_line_search.hpp).

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/opt/line_search.hpp>
#include <crd/hesap/opt/objective.hpp>

#include <cmath>

namespace crd::hesap::opt
{

template <typename T>
class WolfeLineSearch final : public LineSearch<T>
{
public:
    WolfeLineSearch() = default;
    WolfeLineSearch(T c1, T c2, bool strong, crd::usize max_iters) noexcept
        : m_c1(c1), m_c2(c2), m_strong(strong), m_max_iters(max_iters)
    {
    }

    [[nodiscard]] LineSearchResult<T> search(const Objective<T>& obj, crd::containers::ConstSpan<T> x, T fx,
                                             crd::containers::ConstSpan<T> g, crd::containers::ConstSpan<T> p,
                                             T alpha0, crd::containers::Span<T> x_out,
                                             crd::containers::Span<T> g_out) const override
    {
        namespace dn = crd::hesap::dense;
        const crd::usize n = x.size();
        const T phi0 = fx;
        const T dphi0 = dn::dot<T>(g, p); // φ'(0)

        LineSearchResult<T> r;
        if (!(dphi0 < static_cast<T>(0)))
        {
            r.fx_new = fx;
            r.ok = false; // not a descent direction
            return r;
        }

        // φ(α): writes x_out = x + α·p, returns f there.
        auto phi = [&](T a) -> T
        {
            for (crd::usize i = 0; i < n; ++i)
            {
                x_out[i] = x[i] + a * p[i];
            }
            ++r.evals;
            return obj.value(x_out);
        };
        // φ'(α): assumes x_out already holds x + α·p (called right after phi(a)); fills g_out, returns ∇f·p.
        auto dphi = [&]() -> T
        {
            (void)obj.gradient(x_out, g_out);
            ++r.grad_evals;
            return dn::dot<T>(g_out, p);
        };

        const T curv_rhs = m_c2 * (m_strong ? -dphi0 : dphi0); // strong: c2·|φ'(0)|; weak: c2·φ'(0)
        auto curvature_ok = [&](T dphi_a) -> bool
        { return m_strong ? (std::fabs(dphi_a) <= -m_c2 * dphi0) : (dphi_a >= curv_rhs); };

        // ---- zoom(α_lo, α_hi): the interval brackets a Wolfe point (φ(α_lo) is the lowest acceptable-Armijo end).
        auto zoom = [&](T a_lo, T phi_lo, T a_hi) -> LineSearchResult<T>
        {
            for (crd::usize it = 0; it < m_max_iters; ++it)
            {
                const T a_j = static_cast<T>(0.5) * (a_lo + a_hi); // bisection (deterministic, safeguarded)
                const T phi_j = phi(a_j);
                if (!std::isfinite(phi_j) || phi_j > phi0 + m_c1 * a_j * dphi0 || phi_j >= phi_lo)
                {
                    a_hi = a_j;
                }
                else
                {
                    const T dphi_j = dphi(); // x_out holds α_j's point
                    if (curvature_ok(dphi_j))
                    {
                        LineSearchResult<T> rr = r;
                        rr.alpha = a_j;
                        rr.fx_new = phi_j;
                        rr.ok = true;
                        rr.grad_at_new_valid = true;
                        return rr;
                    }
                    if (dphi_j * (a_hi - a_lo) >= static_cast<T>(0))
                    {
                        a_hi = a_lo;
                    }
                    a_lo = a_j;
                    phi_lo = phi_j;
                }
            }
            LineSearchResult<T> rr = r;
            rr.alpha = a_lo;
            rr.fx_new = phi_lo;
            rr.ok = false; // ran out of zoom iterations
            return rr;
        };

        // ---- bracketing (N&W Alg 3.5): grow α until it brackets a Wolfe point, then zoom.
        T a_prev = static_cast<T>(0);
        T phi_prev = phi0;
        T a_cur = alpha0 > static_cast<T>(0) ? alpha0 : static_cast<T>(1);
        for (crd::usize it = 0; it < m_max_iters; ++it)
        {
            const T phi_cur = phi(a_cur);
            if (!std::isfinite(phi_cur) || phi_cur > phi0 + m_c1 * a_cur * dphi0 || (it > 0 && phi_cur >= phi_prev))
            {
                return zoom(a_prev, phi_prev, a_cur);
            }
            const T dphi_cur = dphi(); // x_out holds α_cur's point
            if (curvature_ok(dphi_cur))
            {
                r.alpha = a_cur;
                r.fx_new = phi_cur;
                r.ok = true;
                r.grad_at_new_valid = true;
                return r;
            }
            if (dphi_cur >= static_cast<T>(0))
            {
                return zoom(a_cur, phi_cur, a_prev);
            }
            a_prev = a_cur;
            phi_prev = phi_cur;
            a_cur *= static_cast<T>(2); // expand toward α_max
        }

        r.alpha = a_cur;
        r.fx_new = phi_prev;
        r.ok = false;
        return r;
    }

private:
    T          m_c1 = static_cast<T>(1e-4);
    T          m_c2 = static_cast<T>(0.9); // quasi-Newton default; use ~0.1 for nonlinear-CG
    bool       m_strong = true;
    crd::usize m_max_iters = 50;
};

} // namespace crd::hesap::opt
