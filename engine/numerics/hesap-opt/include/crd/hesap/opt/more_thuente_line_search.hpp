#pragma once

// more_thuente_line_search.hpp — Phase 3.1.6 v7-c: the More-Thuente line search, written from the published
// mathematics of Moré & Thuente 1994 (ACM TOMS 20(3), "Line search algorithms with guaranteed sufficient
// decrease"): the two-stage search over an interval of uncertainty and its four-case safeguarded trial-value
// choice. It finds a step satisfying the STRONG Wolfe conditions using cubic/quadratic interpolation guarded by
// a "modified function" ψ(α) = φ(α) − φ(0) − c1·α·φ'(0), giving far fewer objective evaluations than
// bisection-zoom near convergence. This is the default line search of liblbfgs and Ceres — the gold-standard the
// v7-d L-BFGS line-search-eval count is judged against (no standalone-line-search peer exists, so v7-c claims
// correctness only; the parity bench lives at v7-d). ADR-0090.
//
// COST CONTRACT (v7-a): evaluates ∇f at trial points (curvature test), so on success returns the accepted point in
// x_out and its gradient in g_out with grad_at_new_valid=true.
//
// GUARDS (advisor-pinned), each against one failure of the paper's method: (1) the cubic's discriminant root is
// formed with the largest of |θ|, |dl|, |dt| divided out and its radicand clamped at zero, so rounding can neither
// overflow the square nor take the root of a tiny negative number, and its sign follows the direction of travel
// per case; (2) the switch from the modified function ψ to φ happens once the trial is below the sufficient-decrease
// line with a slope no steeper than min(c1,c2)·φ'(0); (3) a bracket terminates through the relative-width test
// (xtol), the clamp of every trial into the interval and the step range, and a cap on the evaluation count.

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
// The cubic through (a1, f1, d1) and (a2, f2, d2) has its stationary points at a1 + (a2 − a1)·r, with r built from
// θ = 3(f1 − f2)/(a2 − a1) + d1 + d2 and the discriminant root γ = sqrt(θ² − d1·d2) (Moré & Thuente 1994, trial-value
// section). `da`/`db` are the slopes in the order the case adds them; the addition order is part of the rounding.
template <typename T>
inline T mt_cubic_theta(T f1, T f2, T a1, T a2, T da, T db)
{
    return static_cast<T>(3) * (f1 - f2) / (a2 - a1) + da + db;
}

// γ = sqrt(θ² − da·db) with the scale s = max(|θ|, |da|, |db|) divided out, and a radicand clamped at zero.
template <typename T>
inline T mt_cubic_root(T theta, T da, T db)
{
    const T zero = static_cast<T>(0);
    const T scale = std::max({crd::math::fabs(theta), crd::math::fabs(da), crd::math::fabs(db)});
    return scale * crd::math::sqrt(std::max(zero, (theta / scale) * (theta / scale) - (da / scale) * (db / scale)));
}

// The root γ carrying the sign the case needs: negated when `reversed` (the trial lies on the side the case walks
// from).
template <typename T>
inline T mt_signed_root(T theta, T da, T db, bool reversed)
{
    T root = mt_cubic_root<T>(theta, da, db);
    if (reversed)
    {
        root = -root;
    }
    return root;
}

// True when the slopes at the trial (dt) and at the best endpoint (dl) have opposite signs, i.e. the trial lies across
// the minimizer. The unit-sign form keeps NaN (dl zero or infinite) in the "same side" reading, and a zero dt at zero.
template <typename T>
inline bool mt_slopes_cross(T dt, T dl)
{
    return dt * (dl / crd::math::fabs(dl)) < static_cast<T>(0);
}

// One safeguarded trial-value choice: from the interval endpoints a_l (lowest value so far) and a_u, and the new
// trial a_t = `stp` with value f_t and slope d_t, pick the next trial, move the endpoints and set `brackt` once a
// minimizer is enclosed. The paper's four cases are told apart by the trial's value and slope relative to a_l; the
// first three interpolate between a_l and a_t, the fourth between a_t and a_u.
template <typename T>
inline void mt_dcstep(T& a_l, T& f_l, T& d_l, T& a_u, T& f_u, T& d_u, T& stp, T f_t, T d_t, bool& brackt, T stpmin,
                      T stpmax)
{
    const T zero = static_cast<T>(0);
    const T two = static_cast<T>(2);
    const T a_t = stp;

    // The case: 1 a higher value, 2 slopes of opposite sign, 3 slopes of shrinking size, else 4.
    const bool higher = f_t > f_l;

    T    next; // the new trial, before the interval is updated
    bool crossed = false; // case 2: the slopes straddle a minimizer
    if (!higher)
    {
        crossed = mt_slopes_cross<T>(d_t, d_l);
        if (crossed)
        {
            // Slopes of opposite sign: bracketed. Take the cubic if it lies farther from a_t than the secant.
            const T theta = mt_cubic_theta<T>(f_l, f_t, a_l, a_t, d_l, d_t);
            const T gamma = mt_signed_root<T>(theta, d_l, d_t, a_t > a_l);
            const T secant = a_t + (d_t / (d_t - d_l)) * (a_l - a_t);
            const T cubic = a_t + (((gamma - d_t) + theta) / (((gamma - d_t) + gamma) + d_l)) * (a_l - a_t);
            next = (crd::math::fabs(cubic - a_t) > crd::math::fabs(secant - a_t)) ? cubic : secant;
            brackt = true;
        }
        else if (crd::math::fabs(d_t) < crd::math::fabs(d_l))
        {
            // Lower value, same-sign slopes of shrinking size: the cubic only counts when it heads toward the
            // minimizer (r < 0 with a real root); otherwise the step bound on that side of a_t is the cubic.
            const T theta = mt_cubic_theta<T>(f_l, f_t, a_l, a_t, d_l, d_t);
            const T gamma = mt_signed_root<T>(theta, d_l, d_t, a_t > a_l);
            const T r = ((gamma - d_t) + theta) / ((gamma + (d_l - d_t)) + gamma);
            T       cubic;
            if (r < zero && gamma != zero)
            {
                cubic = a_t + r * (a_l - a_t);
            }
            else
            {
                cubic = (a_t > a_l) ? stpmax : stpmin;
            }
            const T secant = a_t + (d_t / (d_t - d_l)) * (a_l - a_t);
            if (brackt)
            {
                // Inside a bracket: nearer of the two, but never more than 66% of the way to a_u.
                next = (crd::math::fabs(cubic - a_t) < crd::math::fabs(secant - a_t)) ? cubic : secant;
                const T cap = a_t + static_cast<T>(0.66) * (a_u - a_t);
                next = (a_t > a_l) ? std::min(cap, next) : std::max(cap, next);
            }
            else
            {
                // No bracket yet: farther of the two, held within the step range.
                next = (crd::math::fabs(cubic - a_t) > crd::math::fabs(secant - a_t)) ? cubic : secant;
                next = std::min(stpmax, next);
                next = std::max(stpmin, next);
            }
        }
        else if (brackt)
        {
            // Lower value, same-sign slopes not shrinking, bracket known: cubic between a_t and a_u.
            const T theta = mt_cubic_theta<T>(f_t, f_u, a_t, a_u, d_u, d_t);
            const T gamma = mt_signed_root<T>(theta, d_u, d_t, a_t > a_u);
            next = a_t + (((gamma - d_t) + theta) / (((gamma - d_t) + gamma) + d_u)) * (a_u - a_t);
        }
        else
        {
            // The same case without a bracket: extrapolate to the step bound on the side of travel.
            next = (a_t > a_l) ? stpmax : stpmin;
        }
    }
    else
    {
        // The minimizer lies between a_l and a_t. Take the cubic if it is nearer a_l, else the midpoint of cubic and
        // quadratic minimizers (the quadratic uses f_l, d_l and f_t).
        const T theta = mt_cubic_theta<T>(f_l, f_t, a_l, a_t, d_l, d_t);
        const T gamma = mt_signed_root<T>(theta, d_l, d_t, a_t < a_l);
        const T cubic = a_l + (((gamma - d_l) + theta) / (((gamma - d_l) + gamma) + d_t)) * (a_t - a_l);
        const T quad = a_l + ((d_l / ((f_l - f_t) / (a_t - a_l) + d_l)) / two) * (a_t - a_l);
        next = (crd::math::fabs(cubic - a_l) < crd::math::fabs(quad - a_l)) ? cubic : cubic + (quad - cubic) / two;
        brackt = true;
    }

    // Move the endpoints: a higher value replaces a_u; otherwise a_t becomes a_l, handing the old a_l to a_u when
    // the slopes crossed.
    if (higher)
    {
        a_u = a_t;
        f_u = f_t;
        d_u = d_t;
    }
    else
    {
        if (crossed)
        {
            a_u = a_l;
            f_u = f_l;
            d_u = d_l;
        }
        a_l = a_t;
        f_l = f_t;
        d_l = d_t;
    }
    stp = next;
}

// ψ(α) = φ(α) − α·slope is the modified function of the search: its value at a is φ's less a·slope and its slope is
// φ's less `slope`. These move one endpoint (value f, slope d, at abscissa a) into ψ and back out again.
template <typename T>
inline void mt_into_psi(T& f, T& d, T a, T slope)
{
    f -= a * slope;
    d -= slope;
}

template <typename T>
inline void mt_out_of_psi(T& f, T& d, T a, T slope)
{
    f += a * slope;
    d += slope;
}

// Run mt_dcstep on ψ instead of φ: the endpoints are moved into ψ in place, the trial (f, d) is shifted as it is
// passed, and the endpoints are moved back at the abscissae the step left them on.
template <typename T>
inline void mt_dcstep_on_psi(T slope, T& a_l, T& f_l, T& d_l, T& a_u, T& f_u, T& d_u, T& stp, T f, T d, bool& brackt,
                             T stpmin, T stpmax)
{
    const T trial_f = f - stp * slope;
    const T trial_d = d - slope;
    mt_into_psi<T>(f_l, d_l, a_l, slope);
    mt_into_psi<T>(f_u, d_u, a_u, slope);
    mt_dcstep<T>(a_l, f_l, d_l, a_u, f_u, d_u, stp, trial_f, trial_d, brackt, stpmin, stpmax);
    mt_out_of_psi<T>(f_l, d_l, a_l, slope);
    mt_out_of_psi<T>(f_u, d_u, a_u, slope);
}

// Bracket-shrinkage guard: if the interval of uncertainty has not narrowed to under 66% of its width two rounds ago,
// the next trial is its midpoint. The last two bracket widths are rolled forward through the two width arguments.
template <typename T>
inline void mt_guard_shrinkage(T a_l, T a_u, T& stp, T& newest_width, T& older_width)
{
    const T width = crd::math::fabs(a_u - a_l);
    if (width >= static_cast<T>(0.66) * older_width)
    {
        stp = a_l + (a_u - a_l) / static_cast<T>(2);
    }
    older_width = newest_width;
    newest_width = width;
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

        const T dphi0 = dn::dot<T>(g, p); // φ'(0)
        LineSearchResult<T> r;
        if (!(dphi0 < zero))
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

        const T phi0 = fx;
        const T armijo_slope = m_c1 * dphi0;               // slope of the sufficient-decrease line
        const T flat_slope = std::min(m_c1, m_c2) * dphi0; // slope at which stage 1 hands over to stage 2

        bool bracketed = false;
        bool on_psi = true; // stage 1: steps are chosen on ψ until the trial is near its minimum
        T    width = m_stpmax - m_stpmin; // bracket width after the latest round
        T    older_width = static_cast<T>(2) * width;

        T a_l = zero, f_l = phi0, d_l = dphi0; // endpoint with the lowest value so far
        T a_u = zero, f_u = phi0, d_u = dphi0; // the other endpoint of the interval of uncertainty
        T step = std::min(std::max(alpha0 > zero ? alpha0 : static_cast<T>(1), m_stpmin), m_stpmax);

        for (crd::usize it = 0; it < m_max_evals; ++it)
        {
            // Window the trial may move in: the bracket itself, or from the best step to 4·(step − a_l) past the trial.
            const T lo = bracketed ? std::min(a_l, a_u) : a_l;
            const T hi = bracketed ? std::max(a_l, a_u) : step + static_cast<T>(4) * (step - a_l);
            step = std::min(std::max(step, m_stpmin), m_stpmax);
            // If progress is no longer possible, fall back to the best point found.
            if ((bracketed && (step <= lo || step >= hi)) || (it + 1 >= m_max_evals) ||
                (bracketed && hi - lo <= m_xtol * hi))
            {
                step = a_l;
            }

            const T f = phi(step);
            const T dg = dphi(); // x_out/g_out now hold φ, ∇f at step
            const T line = phi0 + step * armijo_slope;

            // Strong-Wolfe convergence.
            if (std::isfinite(f) && f <= line && crd::math::fabs(dg) <= m_c2 * (-dphi0))
            {
                r.alpha = step;
                r.fx_new = f;
                r.ok = true;
                r.grad_at_new_valid = true;
                return r;
            }

            // Warnings: rounding / at-bound / xtol — stop with the best step (driver treats !ok as a fallback).
            const bool pinned_in_bracket = bracketed && (step <= lo || step >= hi);
            const bool at_upper_end = step >= m_stpmax && f <= line && dg <= armijo_slope;
            const bool at_lower_end = step <= m_stpmin && (f > line || dg >= armijo_slope);
            const bool bracket_tiny = bracketed && hi - lo <= m_xtol * hi;
            if (pinned_in_bracket || at_upper_end || at_lower_end || bracket_tiny)
            {
                break;
            }

            // Enter stage 2 once the modified function is near its minimum.
            if (on_psi && f <= line && flat_slope <= dg)
            {
                on_psi = false;
            }

            if (on_psi && f <= f_l && f > line)
            {
                detail::mt_dcstep_on_psi<T>(armijo_slope, a_l, f_l, d_l, a_u, f_u, d_u, step, f, dg, bracketed, lo, hi);
            }
            else
            {
                detail::mt_dcstep<T>(a_l, f_l, d_l, a_u, f_u, d_u, step, f, dg, bracketed, lo, hi);
            }

            if (bracketed)
            {
                detail::mt_guard_shrinkage<T>(a_l, a_u, step, width, older_width);
            }
        }

        // Warning / budget exhausted: return the BEST point found (a_l) with a status — usable by L-BFGS if it has
        // sufficient decrease (Armijo), per liblbfgs's policy (don't hard-abort an otherwise-progressing solve). The
        // L-BFGS curvature test (sᵀy>0) decides whether to take the update, so an Armijo-only step is still safe.
        if (a_l > zero)
        {
            const T f_best = phi(a_l); // re-eval at the best step so x_out/g_out hold it
            (void)dphi();
            r.alpha = a_l;
            r.fx_new = f_best;
            r.ok = std::isfinite(f_best) && f_best <= phi0 + a_l * armijo_slope;
            r.grad_at_new_valid = true;
        }
        else
        {
            r.alpha = step;
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
