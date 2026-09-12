#pragma once

// crd-hesap-quadrature v13-h — the Gauss-Kronrod rule: the foundation of the adaptive engine. A (2n+1)-point Kronrod
// extension of the n-point Gauss rule gives BOTH a high-order integral estimate AND, via the difference from the
// embedded n-point Gauss value, a local error estimate — at no extra f-evaluations (the Gauss nodes are a subset of
// the Kronrod nodes). This is GK21 (10-point Gauss embedded in 21-point Kronrod), scipy.integrate.quad's default.
//
// The nodes/weights are the QUADPACK dqk21 constants (1983) — the same ones scipy hard-codes; tabulation is the
// standard (computing the Kronrod extension is Laurie's intricate algorithm). VERIFIED: degree-31 (=3n+1) polynomial
// exactness, the value bit-matches scipy.integrate.quad, and the QUADPACK error estimate (with the roundoff floor)
// matches scipy's reported error to ratio 1.0000 (`build/gk_verify.py`).
//
// Determinism by construction (pillar 1): fixed FP summation order over the constant nodes, crd::math, no
// data-dependent branches in the arithmetic. Allocation-free (the fv scratch is a stack array).

#include <crd/core/types.hpp>
#include <crd/math/cmath.hpp>

#include <limits>

namespace crd::hesap::quadrature
{

// The result of one Gauss-Kronrod panel: the integral estimate + the QUADPACK local error estimate + the magnitude
// integrals (resabs = ∫|f|, resasc = ∫|f − mean|) used by the adaptive driver's roundoff/divergence detection.
template <typename T> struct GkResult
{
    T value = T{0};
    T abserr = T{0};
    T resabs = T{0};
    T resasc = T{0};
};

namespace detail
{
// QUADPACK dqk21 abscissae (positive half, largest→smallest), 21-point Kronrod weights (10 pairs + centre), and the
// embedded 10-point Gauss weights (the Gauss nodes are the odd-indexed abscissae xgk[1,3,5,7,9]).
template <typename T> struct Gk21
{
    static constexpr T kXgk[10] = {
        static_cast<T>(0.995657163025808080735527280689003), static_cast<T>(0.973906528517171720077964012084452),
        static_cast<T>(0.930157491355708226001207180059508), static_cast<T>(0.865063366688984510732096688423493),
        static_cast<T>(0.780817726586416897063717578345042), static_cast<T>(0.679409568299024406234327365114874),
        static_cast<T>(0.562757134668604683339000099272694), static_cast<T>(0.433395394129247190799265943165784),
        static_cast<T>(0.294392862701460198131126603103866), static_cast<T>(0.148874338981631210884826001129720)};
    static constexpr T kWgk[11] = {
        static_cast<T>(0.011694638867371874278064396062192), static_cast<T>(0.032558162307964727478818972459390),
        static_cast<T>(0.054755896574351996031381300244580), static_cast<T>(0.075039674810919952767043140916190),
        static_cast<T>(0.093125454583697605535065465083366), static_cast<T>(0.109387158802297641899210590325805),
        static_cast<T>(0.123491976262065851077958109831074), static_cast<T>(0.134709217311473325928054001771707),
        static_cast<T>(0.142775938577060080797094273138717), static_cast<T>(0.147739104901338491374841515972068),
        static_cast<T>(0.149445554002916905664936468389821)};
    static constexpr T kWg[5] = {
        static_cast<T>(0.066671344308688137593568809893332), static_cast<T>(0.149451349150580593145776339657697),
        static_cast<T>(0.219086362515982043995534934228163), static_cast<T>(0.269266719309996355091226921569469),
        static_cast<T>(0.295524224714752870173892994651338)};
};

template <typename T> [[nodiscard]] constexpr T qmin(T a, T b) noexcept
{
    return a < b ? a : b;
}
template <typename T> [[nodiscard]] constexpr T qmax(T a, T b) noexcept
{
    return a > b ? a : b;
}
} // namespace detail

// Evaluate the 21-point Gauss-Kronrod rule on [a,b] (transcription of QUADPACK dqk21). Returns the integral estimate
// + the local error estimate + resabs/resasc. 21 f-evaluations. f: callable T→T.
template <typename T, typename F> [[nodiscard]] GkResult<T> gauss_kronrod_21(F&& f, T a, T b)
{
    using K = detail::Gk21<T>;
    const T centr = (a + b) / T{2};
    const T hlgth = (b - a) / T{2};
    const T dhlgth = crd::math::fabs(hlgth);
    const T fc = f(centr);
    T resk = K::kWgk[10] * fc;
    T resg = T{0};
    T resabs = crd::math::fabs(resk);
    T fv1[10];
    T fv2[10];
    for (int j = 0; j < 5; ++j) // the 5 Gauss pairs: odd-indexed abscissae xgk[1,3,5,7,9]
    {
        const int idx = 2 * j + 1;
        const T absc = hlgth * K::kXgk[idx];
        const T f1 = f(centr - absc);
        const T f2 = f(centr + absc);
        fv1[idx] = f1;
        fv2[idx] = f2;
        const T fsum = f1 + f2;
        resg += K::kWg[j] * fsum;
        resk += K::kWgk[idx] * fsum;
        resabs += K::kWgk[idx] * (crd::math::fabs(f1) + crd::math::fabs(f2));
    }
    for (int j = 0; j < 5; ++j) // the 5 Kronrod-only pairs: even-indexed abscissae xgk[0,2,4,6,8]
    {
        const int idx = 2 * j;
        const T absc = hlgth * K::kXgk[idx];
        const T f1 = f(centr - absc);
        const T f2 = f(centr + absc);
        fv1[idx] = f1;
        fv2[idx] = f2;
        const T fsum = f1 + f2;
        resk += K::kWgk[idx] * fsum;
        resabs += K::kWgk[idx] * (crd::math::fabs(f1) + crd::math::fabs(f2));
    }
    const T reskh = resk * static_cast<T>(0.5);
    T resasc = K::kWgk[10] * crd::math::fabs(fc - reskh);
    for (int j = 0; j < 10; ++j)
    {
        resasc += K::kWgk[j] * (crd::math::fabs(fv1[j] - reskh) + crd::math::fabs(fv2[j] - reskh));
    }
    GkResult<T> r;
    r.value = resk * hlgth;
    r.resabs = resabs * dhlgth;
    r.resasc = resasc * dhlgth;
    T abserr = crd::math::fabs((resk - resg) * hlgth);
    if (r.resasc != T{0} && abserr != T{0})
    {
        // QUADPACK's (200·abserr/resasc)^1.5 — but x^1.5 = x·√x: one mul + one correctly-rounded hardware sqrt,
        // vs the engine's heavy double-double pow. Faster, deterministic, exact for this exponent.
        const T rat = static_cast<T>(200) * abserr / r.resasc;
        abserr = r.resasc * detail::qmin<T>(T{1}, rat * crd::math::sqrt(rat));
    }
    const T uflow = std::numeric_limits<T>::min();
    const T epmach = std::numeric_limits<T>::epsilon();
    if (r.resabs > uflow / (static_cast<T>(50) * epmach))
    {
        abserr = detail::qmax<T>(epmach * static_cast<T>(50) * r.resabs, abserr);
    }
    r.abserr = abserr;
    return r;
}

} // namespace crd::hesap::quadrature
