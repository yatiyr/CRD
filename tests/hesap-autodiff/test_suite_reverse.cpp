// test_suite_reverse.cpp — Phase 3.1.6 v16-d: the SUITE VJPs (FFT / DSP filtering / spline). Gate: the adjoint
// identity ⟨ȳ, JVP(v)⟩ == ⟨VJP(ȳ), v⟩ against the FD-gated v15-f suite JVPs (real inner product for the complex FFT),
// direct central FD, and the exact FFT round-trip Fᴴ·F = n·I (dft_vjp ∘ dft = n·x).

#include <crd/hesap/autodiff/suite_reverse.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <complex>

namespace sf = crd::hesap::autodiff::forward::suite;
namespace sr = crd::hesap::autodiff::reverse::suite;
using crd::f64;
using cf64 = std::complex<f64>;
using Catch::Matchers::WithinAbs;

namespace
{
f64 rdot(const cf64* u, const cf64* v, int n) // real inner product Re Σ conj(u) v
{
    f64 s = 0.0;
    for (int i = 0; i < n; ++i) { s += u[i].real() * v[i].real() + u[i].imag() * v[i].imag(); }
    return s;
}
f64 dot(const f64* u, const f64* v, int n)
{
    f64 s = 0.0;
    for (int i = 0; i < n; ++i) { s += u[i] * v[i]; }
    return s;
}
} // namespace

TEST_CASE("v16-d: FFT VJP = adjoint DFT (== transpose of the DFT JVP; F^H F = n I)", "[autodiff][reverse][suite]")
{
    constexpr int n = 8;
    cf64          x[n];
    cf64          dx[n];
    cf64          ybar[n];
    for (int i = 0; i < n; ++i)
    {
        x[i]    = {std::sin(0.3 + i), std::cos(0.7 + i)};
        dx[i]   = {std::sin(1.1 + 0.6 * i), std::cos(0.2 + 0.9 * i)};
        ybar[i] = {std::cos(0.5 + i), std::sin(0.4 + 0.8 * i)};
    }
    // transpose identity (real inner product): ⟨ȳ, DFT(dx)⟩ == ⟨dft_vjp(ȳ), dx⟩
    cf64 dy[n];
    cf64 xbar[n];
    sf::dft(dx, dy, n);
    sr::dft_vjp(ybar, xbar, n);
    CHECK_THAT(rdot(ybar, dy, n), WithinAbs(rdot(xbar, dx, n), 1e-10));
    // exact round-trip: dft_vjp(dft(x)) == n·x  (Fᴴ·F = n·I)
    cf64 y[n];
    cf64 back[n];
    sf::dft(x, y, n);
    sr::dft_vjp(y, back, n);
    for (int i = 0; i < n; ++i)
    {
        CHECK_THAT(back[i].real(), WithinAbs(n * x[i].real(), 1e-9));
        CHECK_THAT(back[i].imag(), WithinAbs(n * x[i].imag(), 1e-9));
    }
}

TEST_CASE("v16-d: DSP filtering VJP (correlation = convolution transpose) == transpose JVP / FD",
          "[autodiff][reverse][suite]")
{
    constexpr int nh = 4;
    constexpr int nx = 6;
    constexpr int ny = nh + nx - 1;
    f64           h[nh];
    f64           x[nx];
    f64           dh[nh];
    f64           dx[nx];
    f64           gy[ny];
    for (int i = 0; i < nh; ++i) { h[i] = 0.4 * std::sin(0.5 + i); dh[i] = std::cos(0.3 + i); }
    for (int i = 0; i < nx; ++i) { x[i] = 0.3 * std::cos(0.2 + i); dx[i] = std::sin(0.6 + i); }
    for (int i = 0; i < ny; ++i) { gy[i] = 0.2 * std::sin(1.0 + 0.7 * i); }
    f64 dy[ny];
    f64 scr[ny];
    sf::conv_jvp(h, dh, nh, x, dx, nx, dy, scr);
    f64 gh[nh];
    f64 gx[nx];
    sr::conv_vjp(h, nh, x, nx, gy, gh, gx);
    CHECK_THAT(dot(gy, dy, ny), WithinAbs(dot(gh, dh, nh) + dot(gx, dx, nx), 1e-11)); // ⟨ḡy,dy⟩==⟨ḡh,dh⟩+⟨ḡx,dx⟩
    // direct FD
    auto loss = [&]() -> f64 { f64 y[ny]; sf::conv(h, nh, x, nx, y); return dot(gy, y, ny); };
    const f64 hh = 1e-6;
    for (int e = 0; e < nh; ++e)
    {
        const f64 sv = h[e];
        h[e]         = sv + hh;
        const f64 fp = loss();
        h[e]         = sv - hh;
        const f64 fm = loss();
        h[e]         = sv;
        CHECK_THAT(gh[e], WithinAbs((fp - fm) / (2.0 * hh), 1e-8));
    }
    for (int e = 0; e < nx; ++e)
    {
        const f64 sv = x[e];
        x[e]         = sv + hh;
        const f64 fp = loss();
        x[e]         = sv - hh;
        const f64 fm = loss();
        x[e]         = sv;
        CHECK_THAT(gx[e], WithinAbs((fp - fm) / (2.0 * hh), 1e-8));
    }
}

TEST_CASE("v16-d: spline (tridiagonal Thomas) solve VJP = transposed back-solve == transpose JVP / FD",
          "[autodiff][reverse][suite]")
{
    constexpr int n = 6;
    f64           a[n];
    f64           b[n];
    f64           c[n];
    f64           r[n];
    f64           dr[n];
    f64           ubar[n];
    for (int i = 0; i < n; ++i)
    {
        a[i]    = (i >= 1) ? -1.0 : 0.0;
        c[i]    = (i <= n - 2) ? -1.0 : 0.0;
        b[i]    = 4.0; // diagonally dominant
        r[i]    = 0.5 * std::sin(0.4 + i);
        dr[i]   = std::cos(0.3 + i);
        ubar[i] = 0.3 * std::sin(0.8 + i);
    }
    f64 cp[n];
    f64 dp[n];
    f64 u[n];
    f64 du[n];
    sf::thomas_solve(a, b, c, r, u, n, cp, dp);
    sf::thomas_solve(a, b, c, dr, du, n, cp, dp); // du = T⁻¹ dr (T constant)
    f64 rbar[n];
    f64 at[n];
    f64 ct[n];
    sr::thomas_solve_vjp(a, b, c, ubar, rbar, n, at, ct, cp, dp);
    CHECK_THAT(dot(ubar, du, n), WithinAbs(dot(rbar, dr, n), 1e-11)); // ⟨ū, du⟩ == ⟨r̄, dr⟩
    // direct FD
    auto loss = [&]() -> f64 { f64 uu[n];
    f64 cq[n];
    f64 dq[n]; sf::thomas_solve(a, b, c, r, uu, n, cq, dq); return dot(ubar, uu, n); };
    const f64 h = 1e-6;
    for (int e = 0; e < n; ++e)
    {
        const f64 sv = r[e];
        r[e]         = sv + h;
        const f64 fp = loss();
        r[e]         = sv - h;
        const f64 fm = loss();
        r[e]         = sv;
        CHECK_THAT(rbar[e], WithinAbs((fp - fm) / (2.0 * h), 1e-8));
    }
}
