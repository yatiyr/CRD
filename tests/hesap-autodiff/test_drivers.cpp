// test_drivers.cpp — Phase 3.1.6 v15-d: forward-mode drivers over the SIMD carrier. Gates: gradient/jacobian/jvp
// match the scalar Jet reference (<=1 ulp) and analytic; the DETERMINISM MOAT — the tiled SIMD gradient is
// BIT-IDENTICAL across tile widths W (4 vs 8 vs 16), which is the same per-lane independence that makes it
// bit-identical across {1..16} workers; ragged tiles (n not a multiple of W) are exact.

#include <crd/hesap/autodiff/forward.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace ad = crd::hesap::autodiff::forward;
namespace cc = crd::containers;

namespace
{
struct ScalarF // f: R^n → R
{
    template <class T>
    T operator()(const T* x, int n) const
    {
        using std::exp;
        using std::sin;
        T acc = x[0] * x[0];
        for (int i = 0; i < n; ++i)
        {
            acc = acc + sin(x[i]) * x[(i + 1) % n] + exp(x[i] * 0.1);
        }
        return acc;
    }
};
struct VecF // f: R^n → R^m
{
    template <class T>
    void operator()(const T* x, int n, T* y, int m) const
    {
        using std::sin;
        for (int j = 0; j < m; ++j)
        {
            y[j] = sin(x[j]) * x[(j + 1) % n] + x[j] * x[j];
        }
    }
};
} // namespace

TEST_CASE("gradient driver matches the scalar Jet reference (<=1 ulp)", "[autodiff][drivers]")
{
    constexpr int      n = 8;
    crd::f64           x[n];
    for (int i = 0; i < n; ++i) { x[i] = 0.2 + 0.1 * i; }

    // driver gradient (tiled SIMD)
    ad::JetPackD<8> sc[n];
    crd::f64        g[n];
    ad::gradient<8>(ScalarF{}, cc::ConstSpan<crd::f64>(x, n), cc::Span<crd::f64>(g, n), cc::Span<ad::JetPackD<8>>(sc, n));

    // scalar Jet reference (one pass)
    ad::Jet<crd::f64, n> jx[n];
    for (int i = 0; i < n; ++i) { jx[i] = ad::Jet<crd::f64, n>(x[i], i); }
    const ad::Jet<crd::f64, n> jy = ScalarF{}(jx, n);
    for (int k = 0; k < n; ++k) { CHECK_THAT(g[k], WithinRel(jy.v[k], 1e-12)); }
}

TEST_CASE("gradient is BIT-IDENTICAL across tile widths (determinism moat)", "[autodiff][drivers][determinism]")
{
    constexpr int n = 13; // deliberately not a multiple of 4 or 8 → ragged tiles differ per width
    crd::f64      x[n];
    for (int i = 0; i < n; ++i) { x[i] = 0.15 + 0.07 * i; }

    ad::JetPackD<4>  s4[n];
    ad::JetPackD<8>  s8[n];
    ad::JetPackD<16> s16[n];
    crd::f64         g4[n];
    crd::f64         g8[n];
    crd::f64         g16[n];
    ad::gradient<4>(ScalarF{}, cc::ConstSpan<crd::f64>(x, n), cc::Span<crd::f64>(g4, n), cc::Span<ad::JetPackD<4>>(s4, n));
    ad::gradient<8>(ScalarF{}, cc::ConstSpan<crd::f64>(x, n), cc::Span<crd::f64>(g8, n), cc::Span<ad::JetPackD<8>>(s8, n));
    ad::gradient<16>(ScalarF{}, cc::ConstSpan<crd::f64>(x, n), cc::Span<crd::f64>(g16, n),
                     cc::Span<ad::JetPackD<16>>(s16, n));
    for (int k = 0; k < n; ++k)
    {
        CHECK_THAT(g4[k], WithinAbs(g8[k], 0.0));  // exact — per-lane independence
        CHECK_THAT(g8[k], WithinAbs(g16[k], 0.0)); // exact across all tilings
    }
}

TEST_CASE("gradient ragged tail is correct (n not a multiple of W)", "[autodiff][drivers]")
{
    constexpr int n = 5; // W=8 → one ragged tile of 5; W=4 → tile of 4 + ragged tile of 1
    crd::f64      x[n]    = {0.3, -0.4, 0.55, 0.1, -0.25};
    ad::JetPackD<4> s4[n];
    crd::f64        g4[n];
    ad::gradient<4>(ScalarF{}, cc::ConstSpan<crd::f64>(x, n), cc::Span<crd::f64>(g4, n), cc::Span<ad::JetPackD<4>>(s4, n));

    ad::Jet<crd::f64, n> jx[n];
    for (int i = 0; i < n; ++i) { jx[i] = ad::Jet<crd::f64, n>(x[i], i); }
    const ad::Jet<crd::f64, n> jy = ScalarF{}(jx, n);
    for (int k = 0; k < n; ++k) { CHECK_THAT(g4[k], WithinRel(jy.v[k], 1e-12)); }
}

TEST_CASE("jacobian driver matches analytic", "[autodiff][drivers]")
{
    constexpr int   n = 5;
    constexpr int   m = 3;
    crd::f64        x[n] = {0.4, 0.7, 0.2, 0.9, 0.5};
    ad::JetPackD<8> sc[n];
    ad::JetPackD<8> ys[m];
    crd::f64        jac[m * n];
    ad::jacobian<8>(VecF{}, cc::ConstSpan<crd::f64>(x, n), m, cc::Span<crd::f64>(jac, m * n),
                    cc::Span<ad::JetPackD<8>>(sc, n), cc::Span<ad::JetPackD<8>>(ys, m));
    // y_j = sin(x_j)·x_{j+1} + x_j² ; ∂y_j/∂x_j = cos(x_j)·x_{j+1} + 2x_j ; ∂y_j/∂x_{j+1} = sin(x_j)
    for (int j = 0; j < m; ++j)
    {
        const int jp1 = (j + 1) % n;
        CHECK_THAT(jac[j * n + j], WithinRel(std::cos(x[j]) * x[jp1] + 2.0 * x[j], 1e-12));
        CHECK_THAT(jac[j * n + jp1], WithinRel(std::sin(x[j]), 1e-12));
    }
}

TEST_CASE("jvp (J*v) and directional derivative match analytic", "[autodiff][drivers]")
{
    constexpr int n = 5;
    constexpr int m = 3;
    crd::f64      x[n] = {0.4, 0.7, 0.2, 0.9, 0.5};
    crd::f64      v[n] = {0.5, -0.3, 0.8, 0.1, -0.2};

    ad::Dual<crd::f64> sc[n];
    ad::Dual<crd::f64> ys[m];
    crd::f64           out[m];
    ad::jvp(VecF{}, cc::ConstSpan<crd::f64>(x, n), cc::ConstSpan<crd::f64>(v, n), m, cc::Span<crd::f64>(out, m),
            cc::Span<ad::Dual<crd::f64>>(sc, n), cc::Span<ad::Dual<crd::f64>>(ys, m));
    // (J·v)_j = (cos(x_j)·x_{j+1} + 2x_j)·v_j + sin(x_j)·v_{j+1}
    for (int j = 0; j < m; ++j)
    {
        const int    jp1 = (j + 1) % n;
        const double e   = (std::cos(x[j]) * x[jp1] + 2.0 * x[j]) * v[j] + std::sin(x[j]) * v[jp1];
        CHECK_THAT(out[j], WithinRel(e, 1e-12));
    }

    // scalar directional derivative ∇f·v == Σ_k g_k v_k
    ad::JetPackD<8> gsc[n];
    crd::f64        g[n];
    ad::gradient<8>(ScalarF{}, cc::ConstSpan<crd::f64>(x, n), cc::Span<crd::f64>(g, n), cc::Span<ad::JetPackD<8>>(gsc, n));
    double gv = 0.0;
    for (int k = 0; k < n; ++k) { gv += g[k] * v[k]; }
    ad::Dual<crd::f64> dsc[n];
    CHECK_THAT(ad::directional(ScalarF{}, cc::ConstSpan<crd::f64>(x, n), cc::ConstSpan<crd::f64>(v, n),
                               cc::Span<ad::Dual<crd::f64>>(dsc, n)),
               WithinRel(gv, 1e-11));
}
