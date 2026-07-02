#pragma once

// crd-hesap-quadrature v13-i — non-Gauss quadrature: Clenshaw-Curtis / Fejér (Chebyshev-point rules with
// FFT-cheap, always-positive weights — competitive with Gauss for smooth integrands but with NESTED points) and
// Romberg (Richardson extrapolation of the trapezoidal rule). The Clenshaw-Curtis weights are the Trefethen
// "clencurt" form; verified by degree-n exactness (`build/cc_proto.py`). Determinism by construction.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/quadrature/gauss_kronrod.hpp> // detail::qmax/qmin
#include <crd/hesap/quadrature/integrate.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::quadrature
{
namespace detail
{
template <typename T> [[nodiscard]] constexpr T cc_pi() noexcept
{
    return static_cast<T>(3.14159265358979323846264338327950288);
}
} // namespace detail

// A precomputed Chebyshev-point rule on [−1,1]: n+1 (Clenshaw-Curtis) or n (Fejér) nodes + always-positive weights.
template <typename T> struct ChebyshevRule
{
    crd::containers::Array<T> x;
    crd::containers::Array<T> w;
    int count = 0;

    explicit ChebyshevRule(crd::memory::IAllocator* alloc) : x(alloc), w(alloc) {}
};

// Clenshaw-Curtis: n+1 nodes xₖ = cos(kπ/n), exact for polynomials of degree ≤ n, weights always positive.
template <typename T> [[nodiscard]] ChebyshevRule<T> build_clenshaw_curtis_rule(crd::memory::IAllocator* alloc, int n)
{
    ChebyshevRule<T> r(alloc);
    r.count = n + 1;
    r.x.resize(static_cast<crd::usize>(n + 1));
    r.w.resize(static_cast<crd::usize>(n + 1));
    const T pi = detail::cc_pi<T>();
    for (int k = 0; k <= n; ++k)
    {
        r.x[static_cast<crd::usize>(k)] = crd::math::cos(pi * static_cast<T>(k) / static_cast<T>(n));
    }
    const bool even = (n % 2 == 0);
    r.w[0] = even ? T{1} / static_cast<T>(n * n - 1) : T{1} / static_cast<T>(n * n);
    r.w[static_cast<crd::usize>(n)] = r.w[0];
    for (int k = 1; k < n; ++k)
    {
        const T theta = pi * static_cast<T>(k) / static_cast<T>(n);
        T v = T{1};
        if (even)
        {
            for (int j = 1; j < n / 2; ++j)
            {
                v -= T{2} * crd::math::cos(static_cast<T>(2 * j) * theta) / static_cast<T>(4 * j * j - 1);
            }
            v -= crd::math::cos(static_cast<T>(n) * theta) / static_cast<T>(n * n - 1);
        }
        else
        {
            for (int j = 1; j <= (n - 1) / 2; ++j)
            {
                v -= T{2} * crd::math::cos(static_cast<T>(2 * j) * theta) / static_cast<T>(4 * j * j - 1);
            }
        }
        r.w[static_cast<crd::usize>(k)] = T{2} * v / static_cast<T>(n);
    }
    return r;
}

// Fejér's first rule: n nodes xₖ = cos((2k−1)π/2n) (interior Chebyshev points), exact for degree ≤ n−1.
template <typename T> [[nodiscard]] ChebyshevRule<T> build_fejer_rule(crd::memory::IAllocator* alloc, int n)
{
    ChebyshevRule<T> r(alloc);
    r.count = n;
    r.x.resize(static_cast<crd::usize>(n));
    r.w.resize(static_cast<crd::usize>(n));
    const T pi = detail::cc_pi<T>();
    for (int j = 0; j < n; ++j)
    {
        const T ang = static_cast<T>(2 * (j + 1) - 1) * pi / static_cast<T>(2 * n);
        r.x[static_cast<crd::usize>(j)] = crd::math::cos(ang);
        T s = T{1};
        for (int m = 1; m <= n / 2; ++m)
        {
            s -= T{2} * crd::math::cos(static_cast<T>(2 * m) * ang) / static_cast<T>(4 * m * m - 1);
        }
        r.w[static_cast<crd::usize>(j)] = T{2} * s / static_cast<T>(n);
    }
    return r;
}

// Apply a precomputed Chebyshev-point rule to f over [a,b] (affine map). f: callable T→T.
template <typename T, typename F>
[[nodiscard]] QuadResult<T> integrate_chebyshev(const ChebyshevRule<T>& rule, F&& f, T a, T b)
{
    if (!detail::quad_finite(a) || !detail::quad_finite(b) || rule.count < 1)
    {
        return QuadResult<T>{T{0}, T{0}, 0, 0, QuadStatus::BadInput, false};
    }
    const T h = (b - a) / T{2};
    const T m = (a + b) / T{2};
    T s = T{0};
    for (int i = 0; i < rule.count; ++i)
    {
        s += rule.w[static_cast<crd::usize>(i)] * f(h * rule.x[static_cast<crd::usize>(i)] + m);
    }
    QuadResult<T> r;
    r.value = h * s;
    r.eval_count = static_cast<crd::u32>(rule.count);
    return r;
}

// Precomputed nested Clenshaw-Curtis rule: the always-positive CC weights for each doubling level (N = n0·2ᴸ), built
// ONCE (all the cos transcendentals) and reused. nmax = the finest order.
template <typename T> struct CcAdaptiveRule
{
    crd::containers::Array<T> w;           // per-level weights, flat (level l: [woff[l], woff[l]+nlev[l]])
    crd::containers::Array<crd::u32> woff; // weight offset per level
    crd::containers::Array<int> nlev;      // n_L
    int maxlevel = 0;
    int nmax = 0;

    explicit CcAdaptiveRule(crd::memory::IAllocator* alloc) : w(alloc), woff(alloc), nlev(alloc) {}
};

template <typename T>
[[nodiscard]] CcAdaptiveRule<T> build_cc_adaptive_rule(crd::memory::IAllocator* alloc, int n0 = 8, int maxlevel = 7)
{
    CcAdaptiveRule<T> r(alloc);
    r.maxlevel = maxlevel;
    r.nmax = n0 << (maxlevel - 1);
    const T pi = detail::cc_pi<T>();
    for (int l = 0; l < maxlevel; ++l)
    {
        const int n = n0 << l;
        r.nlev.push_back(n);
        r.woff.push_back(static_cast<crd::u32>(r.w.size()));
        const bool even = (n % 2 == 0);
        r.w.push_back(even ? T{1} / static_cast<T>(n * n - 1) : T{1} / static_cast<T>(n * n)); // w[0]
        for (int k = 1; k < n; ++k)
        {
            const T theta = pi * static_cast<T>(k) / static_cast<T>(n);
            T v = T{1};
            if (even)
            {
                for (int j = 1; j < n / 2; ++j)
                {
                    v -= T{2} * crd::math::cos(static_cast<T>(2 * j) * theta) / static_cast<T>(4 * j * j - 1);
                }
                v -= crd::math::cos(static_cast<T>(n) * theta) / static_cast<T>(n * n - 1);
            }
            else
            {
                for (int j = 1; j <= (n - 1) / 2; ++j)
                {
                    v -= T{2} * crd::math::cos(static_cast<T>(2 * j) * theta) / static_cast<T>(4 * j * j - 1);
                }
            }
            r.w.push_back(T{2} * v / static_cast<T>(n));
        }
        r.w.push_back(even ? T{1} / static_cast<T>(n * n - 1) : T{1} / static_cast<T>(n * n)); // w[n]
    }
    return r;
}

// Doubly-adaptive Clenshaw-Curtis with the PRECOMPUTED rule: nested doubling (N → 2N reuses the prior N+1
// evaluations), stopping when consecutive levels agree. The peer to GSL cquad. Allocates the eval cache once per call.
template <typename T, typename F>
[[nodiscard]] QuadResult<T> integrate_clenshaw_curtis(const CcAdaptiveRule<T>& rule, crd::memory::IAllocator* alloc,
                                                      F&& f, T a, T b, T epsabs, T epsrel)
{
    if (!detail::quad_finite(a) || !detail::quad_finite(b) || rule.maxlevel < 2)
    {
        return QuadResult<T>{T{0}, T{0}, 0, 0, QuadStatus::BadInput, false};
    }
    const int nmax = rule.nmax;
    const T h = (b - a) / T{2};
    const T mid = (a + b) / T{2};
    const T pi = detail::cc_pi<T>();

    crd::containers::Array<T> fcache(alloc);
    crd::containers::Array<unsigned char> filled(alloc);
    fcache.resize(static_cast<crd::usize>(nmax + 1));
    filled.resize(static_cast<crd::usize>(nmax + 1));
    for (int i = 0; i <= nmax; ++i)
    {
        filled[static_cast<crd::usize>(i)] = 0;
    }
    auto fval = [&](int jfine) -> T
    {
        const crd::usize idx = static_cast<crd::usize>(jfine);
        if (!filled[idx])
        {
            fcache[idx] = f(h * crd::math::cos(pi * static_cast<T>(jfine) / static_cast<T>(nmax)) + mid);
            filled[idx] = 1;
        }
        return fcache[idx];
    };

    T s_est = T{0};
    T err = T{0};
    crd::u32 nev = 0;
    bool done = false;
    int lvl = 0;
    for (int l = 0; l < rule.maxlevel; ++l)
    {
        const int n = rule.nlev[static_cast<crd::usize>(l)];
        const int stride = nmax / n;
        const crd::u32 woff = rule.woff[static_cast<crd::usize>(l)];
        T s = T{0};
        for (int k = 0; k <= n; ++k)
        {
            s += rule.w[woff + static_cast<crd::u32>(k)] * fval(k * stride);
        }
        const T s_prev = s_est;
        s_est = h * s;
        nev = static_cast<crd::u32>(n + 1);
        lvl = l;
        if (l >= 1)
        {
            err = crd::math::fabs(s_est - s_prev);
            if (err <= detail::qmax<T>(epsabs, epsrel * crd::math::fabs(s_est)))
            {
                done = true;
                break;
            }
        }
    }
    QuadResult<T> out;
    out.value = s_est;
    out.error_estimate = err;
    out.eval_count = nev;
    out.subdiv_count = static_cast<crd::u32>(lvl + 1);
    out.tolerance_met = done;
    out.status = done ? QuadStatus::Ok : QuadStatus::MaxSubdivisions;
    return out;
}

// Convenience overload — builds the rule then integrates (for one-shot use; pass a rule to amortize across calls).
template <typename T, typename F>
[[nodiscard]] QuadResult<T> integrate_clenshaw_curtis(crd::memory::IAllocator* alloc, F&& f, T a, T b, T epsabs,
                                                      T epsrel, int n0 = 8, int maxlevel = 7)
{
    const CcAdaptiveRule<T> rule = build_cc_adaptive_rule<T>(alloc, n0, maxlevel);
    return integrate_clenshaw_curtis<T>(rule, alloc, static_cast<F&&>(f), a, b, epsabs, epsrel);
}

// Romberg integration: Richardson extrapolation (4^j) of the composite trapezoidal rule at halving step. f over
// [a,b], in-place tableau (one row). The function-evaluation peer to gsl_integration_romberg.
template <typename T, typename F>
[[nodiscard]] QuadResult<T> integrate_romberg(F&& f, T a, T b, T epsabs, T epsrel, int maxk = 18)
{
    if (!detail::quad_finite(a) || !detail::quad_finite(b) || maxk < 2 || maxk > 24 ||
        (epsabs <= T{0} && epsrel <= T{0}))
    {
        return QuadResult<T>{T{0}, T{0}, 0, 0, QuadStatus::BadInput, false};
    }
    T r[24];
    T h = b - a;
    r[0] = (f(a) + f(b)) * h / T{2};
    T diag = r[0];
    T err = crd::math::fabs(r[0]);
    crd::u32 nev = 2;
    bool done = false;
    int kk = 0;
    for (int k = 1; k < maxk; ++k)
    {
        h /= T{2};
        T s = T{0};
        const int npts = 1 << (k - 1);
        for (int i = 1; i <= npts; ++i)
        {
            s += f(a + static_cast<T>(2 * i - 1) * h);
        }
        nev += static_cast<crd::u32>(npts);
        T prev = r[0];
        r[0] = r[0] * static_cast<T>(0.5) + h * s; // r[k][0]
        T pow4 = T{4};
        for (int j = 1; j <= k; ++j)
        {
            const T save = r[static_cast<crd::usize>(j)]; // r[k-1][j]
            r[static_cast<crd::usize>(j)] =
                r[static_cast<crd::usize>(j - 1)] + (r[static_cast<crd::usize>(j - 1)] - prev) / (pow4 - T{1});
            prev = save;
            pow4 *= T{4};
        }
        const T newdiag = r[static_cast<crd::usize>(k)];
        kk = k;
        if (k >= 2)
        {
            err = crd::math::fabs(newdiag - diag);
            if (err <= detail::qmax<T>(epsabs, epsrel * crd::math::fabs(newdiag)))
            {
                diag = newdiag;
                done = true;
                break;
            }
        }
        diag = newdiag;
    }
    QuadResult<T> out;
    out.value = diag;
    out.error_estimate = err;
    out.eval_count = nev;
    out.subdiv_count = static_cast<crd::u32>(kk + 1);
    out.tolerance_met = done;
    out.status = done ? QuadStatus::Ok : QuadStatus::MaxSubdivisions;
    return out;
}

// Romberg on 2^k+1 UNIFORMLY-spaced samples (no function calls). scipy.integrate.romb.
template <typename T> [[nodiscard]] T romberg_samples(crd::containers::ConstSpan<T> y, T dx)
{
    const crd::usize npts = y.size();
    if (npts < 2)
    {
        return T{0};
    }
    int n = 0; // n = log2(npts-1); require npts = 2^n + 1
    for (crd::usize p = npts - 1; p > 1; p >>= 1)
    {
        ++n;
    }
    if ((static_cast<crd::usize>(1u) << n) + 1 != npts)
    {
        return T{0}; // not 2^k+1
    }
    T r[24];
    T h = dx * static_cast<T>(npts - 1);
    r[0] = (y[0] + y[npts - 1]) * h / T{2};
    for (int k = 1; k <= n; ++k)
    {
        h /= T{2};
        const crd::usize step = static_cast<crd::usize>(1u) << static_cast<unsigned>(n - k);
        T s = T{0};
        for (crd::usize i = step; i < npts - 1; i += 2 * step)
        {
            s += y[i];
        }
        T prev = r[0];
        r[0] = r[0] * static_cast<T>(0.5) + h * s;
        T pow4 = T{4};
        for (int j = 1; j <= k; ++j)
        {
            const T save = r[static_cast<crd::usize>(j)];
            r[static_cast<crd::usize>(j)] =
                r[static_cast<crd::usize>(j - 1)] + (r[static_cast<crd::usize>(j - 1)] - prev) / (pow4 - T{1});
            prev = save;
            pow4 *= T{4};
        }
    }
    return r[static_cast<crd::usize>(n)];
}

} // namespace crd::hesap::quadrature
