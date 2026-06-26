#pragma once

// more_thuente_line_search.hpp — Phase 3.1.6 v7-c: the More-Thuente line search (Moré & Thuente 1994; MINPACK-2
// dcsrch/dcstep), ported verbatim. It finds a step satisfying the STRONG Wolfe conditions using cubic/quadratic
// interpolation guarded by a "modified function" ψ(α) = φ(α) − φ(0) − c1·α·φ'(0), giving far fewer objective
// evaluations than bisection-zoom near convergence. This is the default line search of liblbfgs and Ceres — the
// gold-standard the v7-d L-BFGS line-search-eval count is judged against (no standalone-line-search peer exists, so
// v7-c claims correctness only; the parity bench lives at v7-d). ADR-0090.
//
// COST CONTRACT (v7-a): evaluates ∇f at trial points (curvature test), so on success returns the accepted point in
// x_out and its gradient in g_out with grad_at_new_valid=true.
//
// PORT FIDELITY (advisor-pinned): three bug-loci guarded exactly as MINPACK — (1) the discriminant safeguard
// `gamma = s·sqrt(max(0, (θ/s)² − (dx/s)(dp/s)))` with the per-case gamma sign-flip; (2) the stage-1→unmodified
// switch `f ≤ ftest1 ∧ min(ftol,gtol)·φ'(0) ≤ dg` copied literally; (3) bracketing termination via the xtol
// interval-width test + stmin/stmax clamping + a max-eval cap.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/opt/line_search.hpp>
#include <crd/hesap/opt/objective.hpp>

#include <algorithm>
#include <crd/math/cmath.hpp>

namespace crd::hesap::opt
{
namespace detail
{
// dcstep (MINPACK-2): compute a safeguarded trial step `stp` from the interval [stx,sty] + the new trial (fp,dp),
// updating the interval endpoints and `brackt`. A verbatim port — the four cases mirror the subroutine exactly.
template <typename T>
inline void mt_dcstep(T& stx, T& fx, T& dx, T& sty, T& fy, T& dy, T& stp, T fp, T dp, bool& brackt, T stpmin,
                      T stpmax)
{
    const T zero = static_cast<T>(0);
    const T p66 = static_cast<T>(0.66);
    const T two = static_cast<T>(2);
    const T three = static_cast<T>(3);

    const T sgnd = dp * (dx / crd::math::fabs(dx));
    T stpf;

    if (fp > fx)
    {
        // Case 1: a higher function value ⇒ the minimizer is bracketed; cubic step (closer to stx is safer).
        const T theta = three * (fx - fp) / (stp - stx) + dx + dp;
        const T s = std::max({crd::math::fabs(theta), crd::math::fabs(dx), crd::math::fabs(dp)});
        T gamma = s * crd::math::sqrt(std::max(zero, (theta / s) * (theta / s) - (dx / s) * (dp / s)));
        if (stp < stx)
        {
            gamma = -gamma;
        }
        const T p = (gamma - dx) + theta;
        const T q = ((gamma - dx) + gamma) + dp;
        const T r = p / q;
        const T stpc = stx + r * (stp - stx);
        const T stpq = stx + ((dx / ((fx - fp) / (stp - stx) + dx)) / two) * (stp - stx);
        stpf = (crd::math::fabs(stpc - stx) < crd::math::fabs(stpq - stx)) ? stpc : stpc + (stpq - stpc) / two;
        brackt = true;
    }
    else if (sgnd < zero)
    {
        // Case 2: lower value, derivatives opposite sign ⇒ bracketed; cubic step (farther from stp is safer).
        const T theta = three * (fx - fp) / (stp - stx) + dx + dp;
        const T s = std::max({crd::math::fabs(theta), crd::math::fabs(dx), crd::math::fabs(dp)});
        T gamma = s * crd::math::sqrt(std::max(zero, (theta / s) * (theta / s) - (dx / s) * (dp / s)));
        if (stp > stx)
        {
            gamma = -gamma;
        }
        const T p = (gamma - dp) + theta;
        const T q = ((gamma - dp) + gamma) + dx;
        const T r = p / q;
        const T stpc = stp + r * (stx - stp);
        const T stpq = stp + (dp / (dp - dx)) * (stx - stp);
        stpf = (crd::math::fabs(stpc - stp) > crd::math::fabs(stpq - stp)) ? stpc : stpq;
        brackt = true;
    }
    else if (crd::math::fabs(dp) < crd::math::fabs(dx))
    {
        // Case 3: lower value, derivatives same sign, |dp| decreasing ⇒ cubic only if it tends toward the
        // minimizer; otherwise extrapolate to the step bound.
        const T theta = three * (fx - fp) / (stp - stx) + dx + dp;
        const T s = std::max({crd::math::fabs(theta), crd::math::fabs(dx), crd::math::fabs(dp)});
        T gamma = s * crd::math::sqrt(std::max(zero, (theta / s) * (theta / s) - (dx / s) * (dp / s)));
        if (stp > stx)
        {
            gamma = -gamma;
        }
        const T p = (gamma - dp) + theta;
        const T q = (gamma + (dx - dp)) + gamma;
        const T r = p / q;
        T stpc;
        if (r < zero && gamma != zero)
        {
            stpc = stp + r * (stx - stp);
        }
        else if (stp > stx)
        {
            stpc = stpmax;
        }
        else
        {
            stpc = stpmin;
        }
        const T stpq = stp + (dp / (dp - dx)) * (stx - stp);
        if (brackt)
        {
            stpf = (crd::math::fabs(stpc - stp) < crd::math::fabs(stpq - stp)) ? stpc : stpq;
            if (stp > stx)
            {
                stpf = std::min(stp + p66 * (sty - stp), stpf);
            }
            else
            {
                stpf = std::max(stp + p66 * (sty - stp), stpf);
            }
        }
        else
        {
            stpf = (crd::math::fabs(stpc - stp) > crd::math::fabs(stpq - stp)) ? stpc : stpq;
            stpf = std::min(stpmax, stpf);
            stpf = std::max(stpmin, stpf);
        }
    }
    else
    {
        // Case 4: lower value, derivatives same sign, |dp| not decreasing.
        if (brackt)
        {
            const T theta = three * (fp - fy) / (sty - stp) + dy + dp;
            const T s = std::max({crd::math::fabs(theta), crd::math::fabs(dy), crd::math::fabs(dp)});
            T gamma = s * crd::math::sqrt(std::max(zero, (theta / s) * (theta / s) - (dy / s) * (dp / s)));
            if (stp > sty)
            {
                gamma = -gamma;
            }
            const T p = (gamma - dp) + theta;
            const T q = ((gamma - dp) + gamma) + dy;
            const T r = p / q;
            stpf = stp + r * (sty - stp);
        }
        else if (stp > stx)
        {
            stpf = stpmax;
        }
        else
        {
            stpf = stpmin;
        }
    }

    // Update the interval that brackets a minimizer.
    if (fp > fx)
    {
        sty = stp;
        fy = fp;
        dy = dp;
    }
    else
    {
        if (sgnd < zero)
        {
            sty = stx;
            fy = fx;
            dy = dx;
        }
        stx = stp;
        fx = fp;
        dx = dp;
    }
    stp = stpf;
}
} // namespace detail

template <typename T>
class MoreThuenteLineSearch final : public LineSearch<T>
{
public:
    MoreThuenteLineSearch() = default;
    MoreThuenteLineSearch(T c1, T c2, crd::usize max_evals) noexcept : m_c1(c1), m_c2(c2), m_max_evals(max_evals) {}

    [[nodiscard]] LineSearchResult<T> search(const Objective<T>& obj, crd::containers::ConstSpan<T> x, T fx,
                                             crd::containers::ConstSpan<T> g, crd::containers::ConstSpan<T> p,
                                             T alpha0, crd::containers::Span<T> x_out,
                                             crd::containers::Span<T> g_out) const override
    {
        namespace dn = crd::hesap::dense;
        const crd::usize n = x.size();
        const T zero = static_cast<T>(0);

        const T dginit = dn::dot<T>(g, p); // φ'(0)
        LineSearchResult<T> r;
        if (!(dginit < zero))
        {
            r.fx_new = fx;
            r.ok = false; // not a descent direction
            return r;
        }

        auto phi = [&](T a) -> T
        {
            for (crd::usize i = 0; i < n; ++i)
            {
                x_out[i] = x[i] + a * p[i];
            }
            ++r.evals;
            return obj.value(x_out);
        };
        auto dphi = [&]() -> T // x_out already holds x + α·p
        {
            (void)obj.gradient(x_out, g_out);
            ++r.grad_evals;
            return dn::dot<T>(g_out, p);
        };

        const T finit = fx;
        const T dgtest = m_c1 * dginit;
        const T mintest = std::min(m_c1, m_c2) * dginit;
        bool    brackt = false;
        bool    stage1 = true;
        T       width = m_stpmax - m_stpmin;
        T       prev_width = static_cast<T>(2) * width;

        T stx = zero, fxv = finit, dgx = dginit; // best step so far (lowest modified value)
        T sty = zero, fyv = finit, dgy = dginit; // the other interval endpoint
        T stp = std::min(std::max(alpha0 > zero ? alpha0 : static_cast<T>(1), m_stpmin), m_stpmax);

        for (crd::usize it = 0; it < m_max_evals; ++it)
        {
            // Interval bounds for this trial.
            T stmin, stmax;
            if (brackt)
            {
                stmin = std::min(stx, sty);
                stmax = std::max(stx, sty);
            }
            else
            {
                stmin = stx;
                stmax = stp + static_cast<T>(4) * (stp - stx);
            }
            stp = std::min(std::max(stp, m_stpmin), m_stpmax);
            // If progress is no longer possible, fall back to the best point found.
            if ((brackt && (stp <= stmin || stp >= stmax)) || (it + 1 >= m_max_evals) ||
                (brackt && stmax - stmin <= m_xtol * stmax))
            {
                stp = stx;
            }

            const T f = phi(stp);
            const T dg = dphi(); // x_out/g_out now hold φ, ∇f at stp
            const T ftest1 = finit + stp * dgtest;

            // Strong-Wolfe convergence.
            if (std::isfinite(f) && f <= ftest1 && crd::math::fabs(dg) <= m_c2 * (-dginit))
            {
                r.alpha = stp;
                r.fx_new = f;
                r.ok = true;
                r.grad_at_new_valid = true;
                return r;
            }

            // Warnings: rounding / at-bound / xtol — stop with the best step (driver treats !ok as a fallback).
            if ((brackt && (stp <= stmin || stp >= stmax)) ||
                (stp >= m_stpmax && f <= ftest1 && dg <= dgtest) ||
                (stp <= m_stpmin && (f > ftest1 || dg >= dgtest)) || (brackt && stmax - stmin <= m_xtol * stmax))
            {
                break;
            }

            // Enter stage 2 once the modified function is near its minimum.
            if (stage1 && f <= ftest1 && mintest <= dg)
            {
                stage1 = false;
            }

            if (stage1 && f <= fxv && f > ftest1)
            {
                // Operate on the modified function ψ = φ − φ(0) − c1·α·φ'(0).
                T fm = f - stp * dgtest;
                T fxm = fxv - stx * dgtest;
                T fym = fyv - sty * dgtest;
                T dgm = dg - dgtest;
                T dgxm = dgx - dgtest;
                T dgym = dgy - dgtest;
                detail::mt_dcstep<T>(stx, fxm, dgxm, sty, fym, dgym, stp, fm, dgm, brackt, stmin, stmax);
                fxv = fxm + stx * dgtest;
                fyv = fym + sty * dgtest;
                dgx = dgxm + dgtest;
                dgy = dgym + dgtest;
            }
            else
            {
                detail::mt_dcstep<T>(stx, fxv, dgx, sty, fyv, dgy, stp, f, dg, brackt, stmin, stmax);
            }

            // Bisection safeguard on a slowly shrinking bracketed interval.
            if (brackt)
            {
                if (crd::math::fabs(sty - stx) >= static_cast<T>(0.66) * prev_width)
                {
                    stp = stx + static_cast<T>(0.5) * (sty - stx);
                }
                prev_width = width;
                width = crd::math::fabs(sty - stx);
            }
        }

        // Warning / budget exhausted: return the BEST point found (stx) with a status — usable by L-BFGS if it has
        // sufficient decrease (Armijo), per liblbfgs's policy (don't hard-abort an otherwise-progressing solve). The
        // L-BFGS curvature test (sᵀy>0) decides whether to take the update, so an Armijo-only step is still safe.
        if (stx > zero)
        {
            const T fstx = phi(stx); // re-eval at the best step so x_out/g_out hold it
            (void)dphi();
            r.alpha = stx;
            r.fx_new = fstx;
            r.ok = std::isfinite(fstx) && fstx <= finit + stx * dgtest;
            r.grad_at_new_valid = true;
        }
        else
        {
            r.alpha = stp;
            r.fx_new = fx;
            r.ok = false; // no progress from x at all
        }
        return r;
    }

private:
    T          m_c1 = static_cast<T>(1e-4);
    T          m_c2 = static_cast<T>(0.9);
    T          m_xtol = static_cast<T>(1e-10);
    T          m_stpmin = static_cast<T>(1e-20);
    T          m_stpmax = static_cast<T>(1e20);
    crd::usize m_max_evals = 64;
};

} // namespace crd::hesap::opt
