// test_kan.cpp -- Phase 3.1.6 v16-k (part 2): Kolmogorov-Arnold network. Gates: (1) the B-spline basis is a partition
// of unity and its analytic derivative matches central FD; (2) the EFFICIENT layer forward is BIT-IDENTICAL to the
// naive per-edge form; (3) the layer reverse pass (kan_vjp) matches central FD for dwb/dws/dx; (4) a 2-layer KAN
// trains (fits a coupled target) and replays bit-for-bit.

#include <crd/hesap/autodiff/kan.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

namespace kan = crd::hesap::autodiff::kan;
using crd::f64;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr int kG   = 5;
constexpr int kP   = 3;
constexpr int kNb  = kG + kP; // 8 basis functions
} // namespace

TEST_CASE("v16-k: B-spline basis is a partition of unity + analytic derivative == FD", "[autodiff][kan]")
{
    f64 grid[kG + 2 * kP + 1];
    kan::make_grid(-2.0, 2.0, kG, kP, grid);
    for (const f64 x : {-1.3, -0.4, 0.2, 0.9, 1.4})
    {
        f64 b[kNb];
        f64 db[kNb];
        kan::bspline(x, grid, kNb, kP, b, db);
        f64 sum = 0.0;
        for (int g = 0; g < kNb; ++g) { sum += b[g]; }
        CHECK_THAT(sum, WithinAbs(1.0, 1e-12)); // partition of unity
        // derivative vs central FD, per basis
        const f64 h = 1e-6;
        f64       bp[kNb];
        f64       bm[kNb];
        kan::bspline(x + h, grid, kNb, kP, bp, nullptr);
        kan::bspline(x - h, grid, kNb, kP, bm, nullptr);
        for (int g = 0; g < kNb; ++g) { CHECK_THAT(db[g], WithinAbs((bp[g] - bm[g]) / (2.0 * h), 1e-5)); }
    }
}

TEST_CASE("v16-k: efficient KAN forward == naive per-edge (bit-identical) + kan_vjp == FD", "[autodiff][kan]")
{
    constexpr int din = 2;
    constexpr int dout = 3;
    f64           grid[kG + 2 * kP + 1];
    kan::make_grid(-2.0, 2.0, kG, kP, grid);
    f64 wb[dout * din];
    f64 ws[dout * din * kNb];
    for (int i = 0; i < dout * din; ++i) { wb[i] = 0.3 * std::sin(1.0 + i); }
    for (int i = 0; i < dout * din * kNb; ++i) { ws[i] = 0.2 * std::cos(0.5 + i); }
    const f64 x[din] = {0.4, -0.6};

    f64 ye[dout];
    f64 yn[dout];
    kan::kan_forward(din, dout, kNb, kP, wb, ws, grid, x, ye);
    kan::kan_forward_naive(din, dout, kNb, kP, wb, ws, grid, x, yn);
    for (int j = 0; j < dout; ++j) { CHECK(ye[j] == yn[j]); } // BIT-identical

    // kan_vjp: scalar loss L = Σ_j c_j y_j ; dL/dparam via vjp == central FD
    const f64 c[dout] = {1.0, -0.5, 0.7};
    f64       gwb[dout * din] = {};
    f64       gws[dout * din * kNb] = {};
    f64       gx[din] = {};
    kan::kan_vjp(din, dout, kNb, kP, wb, ws, grid, x, c, gwb, gws, gx);
    auto loss = [&](const f64* wbb, const f64* wss, const f64* xx) -> f64
    {
        f64 y[dout];
        kan::kan_forward(din, dout, kNb, kP, wbb, wss, grid, xx, y);
        f64 s = 0.0;
        for (int j = 0; j < dout; ++j) { s += c[j] * y[j]; }
        return s;
    };
    const f64 hh = 1e-6;
    for (int m = 0; m < dout * din; ++m)
    {
        f64 wp[dout * din];
        for (int i = 0; i < dout * din; ++i) { wp[i] = wb[i]; }
        wp[m] += hh;
        const f64 fp = loss(wp, ws, x);
        wp[m] -= 2 * hh;
        CHECK_THAT(gwb[m], WithinAbs((fp - loss(wp, ws, x)) / (2 * hh), 1e-6));
    }
    for (int m = 0; m < din; ++m)
    {
        f64 xp[din] = {x[0], x[1]};
        xp[m] += hh;
        const f64 fp = loss(wb, ws, xp);
        xp[m] -= 2 * hh;
        CHECK_THAT(gx[m], WithinAbs((fp - loss(wb, ws, xp)) / (2 * hh), 1e-6));
    }
    // spot-check a few ws components
    for (int m : {0, 17, 40})
    {
        f64 wp[dout * din * kNb];
        for (int i = 0; i < dout * din * kNb; ++i) { wp[i] = ws[i]; }
        wp[m] += hh;
        const f64 fp = loss(wb, wp, x);
        wp[m] -= 2 * hh;
        CHECK_THAT(gws[m], WithinAbs((fp - loss(wb, wp, x)) / (2 * hh), 1e-6));
    }
}

TEST_CASE("v16-k: 2-layer KAN fits a coupled target and replays bit-for-bit", "[autodiff][kan]")
{
    constexpr int nh = 4; // hidden width
    f64           grid[kG + 2 * kP + 1];
    kan::make_grid(-3.0, 3.0, kG, kP, grid);
    // data: f(x,y)=sin(2x+1.5y) on a 6x6 grid in [-1,1]^2 (non-additive => needs 2 layers)
    constexpr int npt = 36;
    f64           xs[npt * 2];
    f64           ts[npt];
    int           idx = 0;
    for (int a = 0; a < 6; ++a)
    {
        for (int b = 0; b < 6; ++b)
        {
            const f64 xv = -1.0 + 0.4 * a;
            const f64 yv = -1.0 + 0.4 * b;
            xs[idx * 2] = xv; xs[idx * 2 + 1] = yv; ts[idx] = std::sin(2.0 * xv + 1.5 * yv); ++idx;
        }
    }

    const auto run = [&](f64* wb1o, f64* ws1o, f64* wb2o, f64* ws2o, f64* loss_first, f64* loss_last)
    {
        f64 wb1[nh * 2];
        f64 ws1[nh * 2 * kNb];
        f64 wb2[1 * nh];
        f64 ws2[1 * nh * kNb];
        for (int i = 0; i < nh * 2; ++i) { wb1[i] = 0.1 * std::sin(0.3 + i); }
        for (int i = 0; i < nh * 2 * kNb; ++i) { ws1[i] = 0.1 * std::cos(0.2 + i); }
        for (int i = 0; i < nh; ++i) { wb2[i] = 0.1 * std::sin(0.7 + i); }
        for (int i = 0; i < nh * kNb; ++i) { ws2[i] = 0.1 * std::cos(0.4 + i); }
        for (int epoch = 0; epoch < 3000; ++epoch)
        {
            f64 gwb1[nh * 2] = {};
            f64 gws1[nh * 2 * kNb] = {};
            f64 gwb2[nh] = {};
            f64 gws2[nh * kNb] = {};
            f64 loss = 0.0;
            for (int k = 0; k < npt; ++k)
            {
                f64 z[nh];
                f64 y[1];
                kan::kan_forward(2, nh, kNb, kP, wb1, ws1, grid, xs + k * 2, z);
                kan::kan_forward(nh, 1, kNb, kP, wb2, ws2, grid, z, y);
                const f64 e   = y[0] - ts[k];
                loss += e * e;
                const f64 dy  = 2.0 * e;
                f64       gz[nh] = {};
                f64       gxdummy[2];
                kan::kan_vjp(nh, 1, kNb, kP, wb2, ws2, grid, z, &dy, gwb2, gws2, gz);
                kan::kan_vjp(2, nh, kNb, kP, wb1, ws1, grid, xs + k * 2, gz, gwb1, gws1, gxdummy);
            }
            if (epoch == 0) { *loss_first = loss; }
            *loss_last = loss;
            const f64 lr = 0.02 / static_cast<f64>(npt);
            for (int i = 0; i < nh * 2; ++i) { wb1[i] -= lr * gwb1[i]; }
            for (int i = 0; i < nh * 2 * kNb; ++i) { ws1[i] -= lr * gws1[i]; }
            for (int i = 0; i < nh; ++i) { wb2[i] -= lr * gwb2[i]; }
            for (int i = 0; i < nh * kNb; ++i) { ws2[i] -= lr * gws2[i]; }
        }
        for (int i = 0; i < nh * 2; ++i) { wb1o[i] = wb1[i]; }
        for (int i = 0; i < nh * 2 * kNb; ++i) { ws1o[i] = ws1[i]; }
        for (int i = 0; i < nh; ++i) { wb2o[i] = wb2[i]; }
        for (int i = 0; i < nh * kNb; ++i) { ws2o[i] = ws2[i]; }
    };

    f64 a1[nh * 2];
    f64 a2[nh * 2 * kNb];
    f64 a3[nh];
    f64 a4[nh * kNb];
    f64 lf1 = 0.0;
    f64 ll1 = 0.0;
    f64 b1[nh * 2];
    f64 b2[nh * 2 * kNb];
    f64 b3[nh];
    f64 b4[nh * kNb];
    f64 lf2 = 0.0;
    f64 ll2 = 0.0;
    run(a1, a2, a3, a4, &lf1, &ll1);
    run(b1, b2, b3, b4, &lf2, &ll2);
    CHECK(ll1 < 0.6 * lf1); // fit the coupled target (loss reduced >40%)
    CHECK(ll1 == ll2);       // bit-identical replay
    for (int i = 0; i < nh * 2 * kNb; ++i) { CHECK(a2[i] == b2[i]); }
}
