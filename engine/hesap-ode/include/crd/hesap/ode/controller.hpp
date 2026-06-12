#pragma once

// controller.hpp — Phase 3.1.6 v9-a: the step-size control substrate. Pure deterministic FP functions of the
// error estimate — no wall-clock, no thread-dependent state — so every adaptive driver (v9-b+) is
// bit-reproducible by construction. Two controllers ship, each matching its reference EXACTLY (they differ
// on accept-at-1.0 and clamping idiom, so they are separate types, not one parametrization):
//   • ElementaryController — scipy `RungeKutta._step_impl` semantics (the v9-b trajectory-exact gates).
//   • PiController         — the Hairer DOPRI5/DOP853 Gustafsson form (the v9-b Fortran-oracle gates).
// ADR-0091.

#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>

#include <cmath>

namespace crd::hesap::ode
{

// The WRMS error norm (Hairer II.4 (4.11) == scipy's `norm(error / scale)`):
//   scale_i = atol_i + rtol·max(|y0_i|, |y1_i|),  err = sqrt( (1/n) Σ (e_i / scale_i)² ).
// `atol_vec` non-empty ⇒ per-component atol (size n); empty ⇒ the scalar `atol`. Deterministic: fixed-order
// serial accumulation (n is the system dimension; huge-n MOL systems get a deterministic blocked variant at
// v9-j if profiles demand it — the call site is the seam). n == 0 ⇒ 0.
template <typename T>
[[nodiscard]] T error_norm_wrms(crd::containers::ConstSpan<T> e, crd::containers::ConstSpan<T> y0,
                                crd::containers::ConstSpan<T> y1, T rtol, T atol,
                                crd::containers::ConstSpan<T> atol_vec = {})
{
    const crd::usize n = e.size();
    CRD_ASSERT(y0.size() == n && y1.size() == n);
    CRD_ASSERT(atol_vec.empty() || atol_vec.size() == n);
    if (n == 0)
    {
        return static_cast<T>(0);
    }
    T sum = static_cast<T>(0);
    for (crd::usize i = 0; i < n; ++i)
    {
        const T a = atol_vec.empty() ? atol : atol_vec[i];
        const T y_mag = std::abs(y0[i]) > std::abs(y1[i]) ? std::abs(y0[i]) : std::abs(y1[i]);
        const T scale = a + rtol * y_mag;
        const T q = e[i] / scale;
        sum += q * q;
    }
    return std::sqrt(sum / static_cast<T>(n));
}

// scipy's elementary (integral) controller, exact semantics (scipy/integrate/_ivp/rk.py):
//   accept ⇔ err < 1
//   accept: factor = max_factor if err == 0 else min(max_factor, safety·err^exponent);
//           capped at 1 if the PREVIOUS attempt was rejected (no growth straight after a rejection)
//   reject: factor = max(min_factor, safety·err^exponent)
// exponent = -1/(q+1) with q = the ERROR-ESTIMATOR order (e.g. 4 for RK45's embedded 4th-order estimate).
// Constants are scipy's: SAFETY 0.9, MIN_FACTOR 0.2, MAX_FACTOR 10.
template <typename T> struct ElementaryController
{
    T safety = static_cast<T>(0.9);
    T min_factor = static_cast<T>(0.2);
    T max_factor = static_cast<T>(10);
    T exponent = static_cast<T>(-0.2); // -1/(q+1); set per method (q = 4 default → -1/5)
    bool rejected_last = false;

    // Returns the factor to scale h by; sets `accept`. Mutates only `rejected_last` (deterministic state).
    [[nodiscard]] T update(T err, bool& accept) noexcept
    {
        accept = err < static_cast<T>(1);
        if (accept)
        {
            T factor = max_factor;
            if (err != static_cast<T>(0))
            {
                const T raw = safety * std::pow(err, exponent);
                factor = raw < max_factor ? raw : max_factor;
            }
            if (rejected_last && factor > static_cast<T>(1))
            {
                factor = static_cast<T>(1);
            }
            rejected_last = false;
            return factor;
        }
        const T raw = safety * std::pow(err, exponent);
        rejected_last = true;
        return raw > min_factor ? raw : min_factor;
    }
};

// The Hairer/Gustafsson PI controller (dopri5.f / dop853.f form):
//   accept ⇔ err ≤ 1 (Hairer's convention, unlike scipy's strict <)
//   factor = safety · err^(-beta1) · err_prev^(beta2), clamped to [min_factor, max_factor];
//   on accept: err_prev ← max(err, err_floor) (Hairer's facold floor, 1e-4). On reject: growth capped at 1.
// beta1/beta2 are per-method (DOPRI5: beta1 = 1/5 − 0.75·beta2 form with beta2 = 0.04). Exact per-method
// constant sets are pinned at v9-b against the Fortran oracles; this type carries the mechanism.
template <typename T> struct PiController
{
    T safety = static_cast<T>(0.9);
    T min_factor = static_cast<T>(0.2);
    T max_factor = static_cast<T>(10);
    T beta1 = static_cast<T>(0.17);     // ~1/(q+1) − 0.75·beta2; set per method
    T beta2 = static_cast<T>(0.04);     // the Gustafsson history gain; set per method
    T err_floor = static_cast<T>(1e-4); // Hairer's facold floor
    T err_prev = static_cast<T>(1);     // history state (deterministic)

    [[nodiscard]] T update(T err, bool& accept) noexcept
    {
        accept = err <= static_cast<T>(1);
        const T e = err > err_floor ? err : err_floor; // also guards err == 0 in the pow
        T factor = safety * std::pow(e, -beta1) * std::pow(err_prev, beta2);
        if (factor < min_factor)
        {
            factor = min_factor;
        }
        if (factor > max_factor)
        {
            factor = max_factor;
        }
        if (accept)
        {
            err_prev = e;
        }
        else if (factor > static_cast<T>(1))
        {
            factor = static_cast<T>(1); // never grow on a rejection
        }
        return factor;
    }
};

} // namespace crd::hesap::ode
