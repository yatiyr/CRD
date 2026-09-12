// test_topopt.cpp -- Phase 3.1.6 v16-j: adjoint topology optimization (SIMP). The discrete-adjoint compliance
// sensitivity must pass the dolfin-adjoint-class TAYLOR-REMAINDER test (2nd-order convergence => the gradient is
// exact) and central FD; the full OC optimisation must reduce compliance, meet the volume constraint, and be
// bit-deterministic run-to-run.

#include <crd/hesap/autodiff/topopt.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

namespace topo = crd::hesap::autodiff::topopt;
using crd::f64;
using crd::u8;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace
{
// MBB beam: left edge x-fixed (symmetry), bottom-right corner y-fixed, unit downward load at the top-left node.
void mbb(const topo::Problem& p, u8* freem, f64* f)
{
    const int nd = p.ndof();
    for (int d = 0; d < nd; ++d) { freem[d] = 1; f[d] = 0.0; }
    for (int iy = 0; iy <= p.nely; ++iy) { freem[2 * (0 * (p.nely + 1) + iy)] = 0; }
    freem[2 * (p.nelx * (p.nely + 1) + p.nely) + 1] = 0;
    f[2 * (0 * (p.nely + 1) + 0) + 1]               = -1.0;
}
} // namespace

TEST_CASE("v16-j: adjoint compliance sensitivity passes Taylor-remainder (2nd order) + central FD", "[autodiff][topopt]")
{
    const topo::Problem p{8, 4, 3.0, 1e-9, 1.0, 0.3};
    const int           n = p.nel();
    f64                 ke[64];
    topo::q4_ke(p.nu, ke);
    u8  freem[720];
    f64 f[720];
    mbb(p, freem, f);
    f64 rho[320];
    f64 dc[320];
    f64 delta[320];
    for (int i = 0; i < n; ++i)
    {
        rho[i]   = 0.5 + 0.15 * std::sin(1.0 + i);
        delta[i] = std::cos(0.4 + i);
    }
    f64 u[720];
    f64 band[1400];
    f64 y[720];
    topo::solve(p, rho, ke, freem, f, u, band, y);
    const f64 c0 = topo::compliance(p, rho, ke, f, u, dc); // dc = the raw adjoint sensitivity
    f64       gd = 0.0;
    for (int i = 0; i < n; ++i) { gd += dc[i] * delta[i]; }

    const auto cval = [&](f64 eps) -> f64
    {
        f64 rp[320];
        for (int i = 0; i < n; ++i) { rp[i] = rho[i] + eps * delta[i]; }
        f64 uu[720];
        f64 bb[1400];
        f64 yy[720];
        topo::solve(p, rp, ke, freem, f, uu, bb, yy);
        return topo::compliance(p, rp, ke, f, uu, nullptr);
    };
    // (1) central FD of the directional derivative == the adjoint gd
    const f64 eps = 1e-6;
    CHECK_THAT(gd, WithinRel((cval(eps) - cval(-eps)) / (2.0 * eps), 1e-5));
    // (2) Taylor remainder is 2nd-order: r(e)=|c(rho+e*d) - c0 - e*gd| halves-> quarters => ratio ~ 4
    const f64 rem1 = std::abs(cval(2e-3) - c0 - 2e-3 * gd);
    const f64 rem2 = std::abs(cval(1e-3) - c0 - 1e-3 * gd);
    CHECK(rem1 / rem2 > 3.5);
    CHECK(rem1 / rem2 < 4.5);
}

TEST_CASE("v16-j: SIMP topopt reduces compliance, meets volume, bit-deterministic", "[autodiff][topopt]")
{
    const topo::Problem p{30, 10, 3.0, 1e-9, 1.0, 0.3};
    const int           n = p.nel();
    f64                 ke[64];
    topo::q4_ke(p.nu, ke);
    u8  freem[720];
    f64 f[720];
    mbb(p, freem, f);
    f64 rho[320];
    f64 u[720];
    f64 band[18000];
    f64 y[720];
    f64 dc[320];
    f64 dcf[320];
    f64 rn[320];
    const f64 c_final = topo::optimize(p, 0.5, 1.5, 80, ke, freem, f, rho, u, band, y, dc, dcf, rn);

    // initial (uniform volfrac) compliance
    f64 rho0[320];
    for (int i = 0; i < n; ++i) { rho0[i] = 0.5; }
    topo::solve(p, rho0, ke, freem, f, u, band, y);
    const f64 c_init = topo::compliance(p, rho0, ke, f, u, nullptr);
    CHECK(c_final < c_init); // optimization strictly reduced compliance

    f64 vol = 0.0;
    for (int i = 0; i < n; ++i) { vol += rho[i]; }
    CHECK_THAT(vol / n, WithinAbs(0.5, 0.02)); // volume constraint held

    // determinism: a second identical run yields bit-identical design + compliance
    f64 rho_a[320];
    for (int i = 0; i < n; ++i) { rho_a[i] = rho[i]; }
    const f64 c2 = topo::optimize(p, 0.5, 1.5, 80, ke, freem, f, rho, u, band, y, dc, dcf, rn);
    CHECK(c2 == c_final);
    for (int i = 0; i < n; ++i) { CHECK(rho[i] == rho_a[i]); }
}
