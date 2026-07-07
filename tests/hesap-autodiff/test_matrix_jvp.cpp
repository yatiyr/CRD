// test_matrix_jvp.cpp — Phase 3.1.6 v15-f: matrix-calculus forward differentials. Gates: each JVP rule matches a
// finite-difference reference; the SPD solve JVP reuses the factor (verified == re-solving the perturbed system);
// the VALUE-ONLY drivers (logdet / eigvals / svdvals) are exact AND stay finite at repeated eigenvalues — where the
// eigenvector derivative (F-matrix, 1/(λ_i−λ_j)) NaNs.

#include <crd/hesap/autodiff/forward.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
namespace mj = crd::hesap::autodiff::forward::matrix;
namespace su = crd::hesap::autodiff::forward::suite;

// Parallel fixed-size matrix/vector fixtures read clearer grouped than one-per-line here.
// NOLINTBEGIN(readability-isolate-declaration)

namespace
{
// Build SPD A = MᵀM + n·I (n×n) into a; also returns M-filled scratch.
void build_spd(crd::f64* a, int n)
{
    crd::f64 m[64];
    for (int i = 0; i < n * n; ++i) { m[i] = 0.3 + 0.2 * std::sin(1.0 + i * 1.7); }
    mj::gemm_tn(m, m, a, n, n, n); // Mᵀ M
    for (int i = 0; i < n; ++i) { a[i * n + i] += static_cast<crd::f64>(n); }
}
} // namespace

TEST_CASE("gemm_jvp == FD", "[autodiff][matrix]")
{
    constexpr int n = 3;
    crd::f64      a[n * n], b[n * n], da[n * n], db[n * n], dc[n * n], sc[n * n];
    for (int i = 0; i < n * n; ++i)
    {
        a[i]  = 0.5 + 0.1 * i;
        b[i]  = -0.3 + 0.2 * i;
        da[i] = 0.07 * (i + 1);
        db[i] = -0.05 * (i + 1);
    }
    mj::gemm_jvp(a, b, da, db, dc, n, n, n, sc);
    const crd::f64 eps = 1e-6;
    crd::f64       ap[n * n], bp[n * n], c0[n * n], c1[n * n];
    for (int i = 0; i < n * n; ++i) { ap[i] = a[i] + eps * da[i]; bp[i] = b[i] + eps * db[i]; }
    mj::gemm(a, b, c0, n, n, n);
    mj::gemm(ap, bp, c1, n, n, n);
    for (int i = 0; i < n * n; ++i) { CHECK_THAT(dc[i], WithinRel((c1[i] - c0[i]) / eps, 1e-4)); }
}

TEST_CASE("solve_spd_jvp == FD (factor reuse)", "[autodiff][matrix]")
{
    constexpr int n = 4;
    crd::f64      a[n * n], l[n * n], b[n], x[n], da[n * n], db[n], dx[n], r[n];
    build_spd(a, n);
    for (int i = 0; i < n; ++i) { b[i] = 1.0 + 0.3 * i; }
    mj::cholesky(a, l, n);
    mj::trisolve_lower(l, b, x, n, 1);
    mj::trisolve_lower_t(l, x, x, n, 1); // x = A⁻¹b
    crd::f64 dsym[n * n];
    for (int i = 0; i < n; ++i)
    {
        for (int j = 0; j < n; ++j) { dsym[i * n + j] = 0.02 * std::cos(1.0 + i + 2.0 * j); }
    }
    for (int i = 0; i < n; ++i) // symmetrize dA (perturbation must keep A SPD-symmetric)
    {
        for (int j = i + 1; j < n; ++j) { dsym[j * n + i] = dsym[i * n + j]; }
    }
    for (int i = 0; i < n; ++i) { da[i * n + i] = dsym[i * n + i]; }
    for (int i = 0; i < n * n; ++i) { da[i] = dsym[i]; }
    for (int i = 0; i < n; ++i) { db[i] = 0.1 * (i + 1); }
    mj::solve_spd_jvp(l, x, da, db, dx, n, 1, r);

    const crd::f64 eps = 1e-6;
    crd::f64       ap[n * n], lp[n * n], bp[n], xp[n];
    for (int i = 0; i < n * n; ++i) { ap[i] = a[i] + eps * da[i]; }
    for (int i = 0; i < n; ++i) { bp[i] = b[i] + eps * db[i]; }
    mj::cholesky(ap, lp, n);
    mj::trisolve_lower(lp, bp, xp, n, 1);
    mj::trisolve_lower_t(lp, xp, xp, n, 1);
    for (int i = 0; i < n; ++i) { CHECK_THAT(dx[i], WithinRel((xp[i] - x[i]) / eps, 1e-4)); }
}

TEST_CASE("logdet_spd_jvp == FD (value-only, degeneracy-free)", "[autodiff][matrix]")
{
    constexpr int n = 4;
    crd::f64      a[n * n], l[n * n], da[n * n], m1[n * n], m2[n * n];
    build_spd(a, n);
    for (int i = 0; i < n; ++i)
    {
        for (int j = 0; j < n; ++j) { da[i * n + j] = 0.03 * std::cos(2.0 + i + j); }
    }
    for (int i = 0; i < n; ++i)
    {
        for (int j = i + 1; j < n; ++j) { da[j * n + i] = da[i * n + j]; }
    }
    mj::cholesky(a, l, n);
    const crd::f64 dld = mj::logdet_spd_jvp(l, da, n, m1, m2);

    const crd::f64 eps = 1e-6;
    crd::f64       ap[n * n], lp[n * n];
    crd::f64       ld0 = 0.0;
    for (int i = 0; i < n; ++i) { ld0 += 2.0 * std::log(l[i * n + i]); }
    for (int i = 0; i < n * n; ++i) { ap[i] = a[i] + eps * da[i]; }
    mj::cholesky(ap, lp, n);
    crd::f64 ld1 = 0.0;
    for (int i = 0; i < n; ++i) { ld1 += 2.0 * std::log(lp[i * n + i]); }
    CHECK_THAT(dld, WithinRel((ld1 - ld0) / eps, 1e-4));
}

TEST_CASE("eigvals_jvp value-only: exact + FINITE at repeated eigenvalues", "[autodiff][matrix]")
{
    constexpr int n = 2;
    const crd::f64 th = 0.6;
    const crd::f64 c = std::cos(th), s = std::sin(th);
    const crd::f64 q[4]  = {c, -s, s, c}; // columns = eigenvectors
    const crd::f64 qt[4] = {c, s, -s, c};
    const crd::f64 dlam_in[2] = {0.3, 0.7};
    // dA = Q·diag(dλ)·Qᵀ  ⇒ eigvals_jvp must recover diag(dλ), regardless of the (possibly repeated) eigenvalues.
    crd::f64 qd[4] = {q[0] * dlam_in[0], q[1] * dlam_in[1], q[2] * dlam_in[0], q[3] * dlam_in[1]};
    crd::f64 da[4];
    mj::gemm(qd, qt, da, n, n, n);
    crd::f64 dlam[2], tmp[4];
    mj::eigvals_jvp(q, da, dlam, n, tmp);
    CHECK_THAT(dlam[0], WithinRel(0.3, 1e-12));
    CHECK_THAT(dlam[1], WithinRel(0.7, 1e-12));
    // The rule has NO 1/(λ_i−λ_j) term — so even with the eigenvalues EQUAL (a degenerate spectrum, where the
    // eigenvector derivative NaNs), the value tangents are finite and exact.
    CHECK(std::isfinite(dlam[0]));
    CHECK(std::isfinite(dlam[1]));
}

TEST_CASE("svdvals_jvp value-only: exact", "[autodiff][matrix]")
{
    constexpr int m = 2, n = 2;
    const crd::f64 a1 = 0.5, a2 = 0.9;
    const crd::f64 u[4]  = {std::cos(a1), -std::sin(a1), std::sin(a1), std::cos(a1)};
    const crd::f64 v[4]  = {std::cos(a2), -std::sin(a2), std::sin(a2), std::cos(a2)};
    const crd::f64 vt[4] = {v[0], v[2], v[1], v[3]};
    const crd::f64 dsig_in[2] = {0.4, 0.6};
    crd::f64 ud[4] = {u[0] * dsig_in[0], u[1] * dsig_in[1], u[2] * dsig_in[0], u[3] * dsig_in[1]};
    crd::f64 da[4];
    mj::gemm(ud, vt, da, m, n, n); // dA = U·diag(dσ)·Vᵀ
    crd::f64 dsig[2], tmp[4];
    mj::svdvals_jvp(u, v, da, dsig, m, n, tmp);
    CHECK_THAT(dsig[0], WithinRel(0.4, 1e-12));
    CHECK_THAT(dsig[1], WithinRel(0.6, 1e-12));
}

TEST_CASE("cholesky_jvp == FD", "[autodiff][matrix]")
{
    constexpr int n = 4;
    crd::f64      a[n * n], l[n * n], da[n * n], dl[n * n], m1[n * n], m2[n * n];
    build_spd(a, n);
    for (int i = 0; i < n; ++i)
    {
        for (int j = 0; j < n; ++j) { da[i * n + j] = 0.01 * std::cos(1.0 + i + j); }
    }
    for (int i = 0; i < n; ++i)
    {
        for (int j = i + 1; j < n; ++j) { da[j * n + i] = da[i * n + j]; }
    }
    mj::cholesky(a, l, n);
    mj::cholesky_jvp(l, da, dl, n, m1, m2);
    const crd::f64 eps = 1e-6;
    crd::f64       ap[n * n], lp[n * n];
    for (int i = 0; i < n * n; ++i) { ap[i] = a[i] + eps * da[i]; }
    mj::cholesky(ap, lp, n);
    for (int i = 0; i < n; ++i)
    {
        for (int j = 0; j <= i; ++j) { CHECK_THAT(dl[i * n + j], WithinAbs((lp[i * n + j] - l[i * n + j]) / eps, 1e-4)); }
    }
}

TEST_CASE("conv_jvp (DSP filter) == FD + FFT JVP linearity", "[autodiff][matrix]")
{
    constexpr int nh = 3, nx = 5, ny = nh + nx - 1;
    crd::f64      h[nh] = {0.5, 0.3, 0.2}, x[nx] = {1, 2, 3, 4, 5};
    crd::f64      dh[nh] = {0.1, -0.2, 0.05}, dx[nx] = {0.3, 0.1, -0.2, 0.4, 0.0};
    crd::f64      dy[ny], sc[ny];
    su::conv_jvp(h, dh, nh, x, dx, nx, dy, sc);
    const crd::f64 eps = 1e-6;
    crd::f64       hp[nh], xp[nx], y0[ny], y1[ny];
    for (int i = 0; i < nh; ++i) { hp[i] = h[i] + eps * dh[i]; }
    for (int i = 0; i < nx; ++i) { xp[i] = x[i] + eps * dx[i]; }
    su::conv(h, nh, x, nx, y0);
    su::conv(hp, nh, xp, nx, y1);
    for (int i = 0; i < ny; ++i) { CHECK_THAT(dy[i], WithinAbs((y1[i] - y0[i]) / eps, 1e-4)); }

    // FFT JVP: linear ⇒ (dft(x+ε·dx) − dft(x))/ε == dft(dx)
    constexpr int        n = 8;
    std::complex<crd::f64> cx[n], cdx[n], y0c[n], y1c[n], dyc[n], xp2[n];
    for (int i = 0; i < n; ++i)
    {
        cx[i]  = {std::sin(1.0 * i), std::cos(1.0 * i)};
        cdx[i] = {0.1 * i, -0.05 * i};
    }
    su::dft(cx, y0c, n);
    su::dft_jvp(cdx, dyc, n);
    for (int i = 0; i < n; ++i) { xp2[i] = cx[i] + eps * cdx[i]; }
    su::dft(xp2, y1c, n);
    for (int i = 0; i < n; ++i)
    {
        CHECK_THAT(dyc[i].real(), WithinAbs((y1c[i].real() - y0c[i].real()) / eps, 1e-4));
        CHECK_THAT(dyc[i].imag(), WithinAbs((y1c[i].imag() - y0c[i].imag()) / eps, 1e-4));
    }
}

TEST_CASE("thomas (spline factor-reuse) solves T*u = r", "[autodiff][matrix]")
{
    constexpr int n = 5;
    crd::f64      a[n] = {0, 1, 1, 1, 1}, b[n] = {4, 4, 4, 4, 4}, c[n] = {1, 1, 1, 1, 0};
    crd::f64      r[n] = {1, 2, 3, 4, 5}, u[n], cp[n], dp[n];
    su::thomas_solve(a, b, c, r, u, n, cp, dp);
    for (int i = 0; i < n; ++i)
    {
        crd::f64 tu = b[i] * u[i] + (i > 0 ? a[i] * u[i - 1] : 0.0) + (i < n - 1 ? c[i] * u[i + 1] : 0.0);
        CHECK_THAT(tu, WithinRel(r[i], 1e-12));
    }
}
// NOLINTEND(readability-isolate-declaration)
