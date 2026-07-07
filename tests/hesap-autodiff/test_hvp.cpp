// test_hvp.cpp — Phase 3.1.6 v16-e: forward-over-reverse HESSIAN-VECTOR products + Hessian-free Newton-CG. Gate: the
// exact HVP (grad + ∇²f·v from ONE forward build + ONE backward over Dual) matches (a) the v15-c hyper-dual exact
// Hessian `H·v` and curvature `vᵀHv`, (b) central FD of the gradient, and the grad-part matches central FD of f;
// bit-identical run-to-run. Newton-CG (matrix-vector products = HVPs, never forms H) solves a quadratic exactly in
// one step and drives a convex problem to a vanishing gradient.

#include <crd/hesap/autodiff/hvp.hpp>
#include <crd/hesap/autodiff/hyperdual.hpp> // the v15-c exact-Hessian oracle

#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

namespace rev = crd::hesap::autodiff::reverse;
namespace fwd = crd::hesap::autodiff::forward;
using crd::f64;
using Catch::Matchers::WithinAbs;
using D = fwd::Dual<f64>;

namespace
{
// scalar-generic (works on RVar<Dual> for HVP, HyperDual for the oracle, f64 for FD): exp + ring products + Σ sin
struct F1
{
    template <class S>
    S operator()(const S* x, int n) const
    {
        using crd::math::exp;
        using crd::math::sin;
        S acc = exp(x[0]);
        for (int i = 1; i < n; ++i) { acc = acc + x[i - 1] * x[i]; }
        for (int i = 0; i < n; ++i) { acc = acc + sin(x[i]); }
        return acc;
    }
};
f64 g_a[8], g_b[8], g_t[8];
// separable quadratic  Σ ½ a_i x_i² + b_i x_i  (min at x_i = −b_i/a_i)
struct FQuad
{
    template <class S>
    S operator()(const S* x, int n) const
    {
        S acc = 0.5 * g_a[0] * x[0] * x[0] + g_b[0] * x[0];
        for (int i = 1; i < n; ++i) { acc = acc + 0.5 * g_a[i] * x[i] * x[i] + g_b[i] * x[i]; }
        return acc;
    }
};
// strictly convex non-quadratic  Σ (x_i − t_i)² + 0.1 x_i⁴
struct FConv
{
    template <class S>
    S operator()(const S* x, int n) const
    {
        S d0  = x[0] - g_t[0];
        S acc = d0 * d0 + 0.1 * x[0] * x[0] * x[0] * x[0];
        for (int i = 1; i < n; ++i) { S d = x[i] - g_t[i]; acc = acc + d * d + 0.1 * x[i] * x[i] * x[i] * x[i]; }
        return acc;
    }
};
} // namespace

TEST_CASE("v16-e: forward-over-reverse HVP == hyper-dual H*v == FD, deterministic", "[autodiff][reverse][hvp]")
{
    constexpr int              n = 6;
    crd::memory::TlsfAllocator alloc(4 << 20);
    rev::RTape<D>              tape(&alloc);
    rev::RVar<D>               scr[n];
    f64                        x[n];
    f64                        v[n];
    for (int i = 0; i < n; ++i) { x[i] = 0.3 + 0.2 * std::sin(1.0 + i); v[i] = 0.5 * std::cos(0.4 + i); }

    f64 grad[n];
    f64 hv[n];
    f64 grad2[n];
    f64 hv2[n];
    rev::hvp(F1{}, {x, n}, {v, n}, {grad, n}, {hv, n}, tape, {scr, n});
    rev::hvp(F1{}, {x, n}, {v, n}, {grad2, n}, {hv2, n}, tape, {scr, n});
    for (int i = 0; i < n; ++i) // determinism (bit-identical)
    {
        CHECK(hv[i] == hv2[i]);
        CHECK(grad[i] == grad2[i]);
    }

    // ★ hyper-dual oracle (v15-c): exact Hessian, H·v
    f64 hess[n * n];
    fwd::hessian<n>(F1{}, x, hess);
    for (int i = 0; i < n; ++i)
    {
        f64 s = 0.0;
        for (int j = 0; j < n; ++j) { s += hess[i * n + j] * v[j]; }
        CHECK_THAT(hv[i], WithinAbs(s, 1e-9));
    }
    // curvature vᵀHv in one pass
    const f64 curv = fwd::curvature<n>(F1{}, x, v);
    f64       v_hv  = 0.0;
    for (int i = 0; i < n; ++i) { v_hv += v[i] * hv[i]; }
    CHECK_THAT(v_hv, WithinAbs(curv, 1e-9));

    // grad-part == central FD of f
    const f64 h = 1e-6;
    for (int i = 0; i < n; ++i)
    {
        const f64 sv = x[i];
        x[i]         = sv + h;
        const f64 fp = F1{}(x, n);
        x[i]         = sv - h;
        const f64 fm = F1{}(x, n);
        x[i]         = sv;
        CHECK_THAT(grad[i], WithinAbs((fp - fm) / (2.0 * h), 1e-6));
    }

    // Hv == central FD of the GRADIENT (independent of the hyper-dual): (∇f(x+εv) − ∇f(x−εv))/2ε
    f64       zeros[n] = {};
    f64       gp[n];
    f64       gm[n];
    f64       dummy[n];
    f64       xp[n];
    f64       xm[n];
    const f64 e = 1e-6;
    for (int i = 0; i < n; ++i) { xp[i] = x[i] + e * v[i]; xm[i] = x[i] - e * v[i]; }
    rev::hvp(F1{}, {xp, n}, {zeros, n}, {gp, n}, {dummy, n}, tape, {scr, n});
    rev::hvp(F1{}, {xm, n}, {zeros, n}, {gm, n}, {dummy, n}, tape, {scr, n});
    for (int i = 0; i < n; ++i) { CHECK_THAT(hv[i], WithinAbs((gp[i] - gm[i]) / (2.0 * e), 1e-5)); }
}

TEST_CASE("v16-e: Newton-CG (HVP-based, Hessian-free) solves a quadratic EXACTLY in one step", "[autodiff][reverse][hvp]")
{
    constexpr int n = 5;
    for (int i = 0; i < n; ++i) { g_a[i] = 1.0 + 0.5 * i; g_b[i] = -0.3 + 0.2 * i; }
    crd::memory::TlsfAllocator alloc(4 << 20);
    rev::RTape<D>              tape(&alloc);
    rev::RVar<D>               scr[n];
    f64                        x[n];
    f64                        g[n];
    f64                        hv[n];
    f64                        r[n];
    f64                        d[n];
    f64                        p[n];
    for (int i = 0; i < n; ++i) { x[i] = 2.0 + i; }
    const f64 gn = rev::newton_cg_step(FQuad{}, {x, n}, n + 2, 1e-14, tape, {scr, n}, {g, n}, {hv, n}, {r, n},
                                       {d, n}, {p, n});
    CHECK(gn < 1e-16); // exact Newton on a quadratic: gradient vanishes in ONE step
    for (int i = 0; i < n; ++i) { CHECK_THAT(x[i], WithinAbs(-g_b[i] / g_a[i], 1e-9)); } // reached the minimiser
}

TEST_CASE("v16-e: Newton-CG drives a convex non-quadratic to a vanishing gradient", "[autodiff][reverse][hvp]")
{
    constexpr int n = 4;
    for (int i = 0; i < n; ++i) { g_t[i] = 0.5 + 0.3 * i; }
    crd::memory::TlsfAllocator alloc(4 << 20);
    rev::RTape<D>              tape(&alloc);
    rev::RVar<D>               scr[n];
    f64                        x[n];
    f64                        g[n];
    f64                        hv[n];
    f64                        r[n];
    f64                        d[n];
    f64                        p[n];
    for (int i = 0; i < n; ++i) { x[i] = g_t[i] + 1.0; }
    f64 gn = 1.0;
    for (int step = 0; step < 12; ++step)
    {
        gn = rev::newton_cg_step(FConv{}, {x, n}, n + 4, 1e-13, tape, {scr, n}, {g, n}, {hv, n}, {r, n}, {d, n},
                                 {p, n});
    }
    CHECK(gn < 1e-14); // converged (Newton is quadratically convergent with the exact HVP)
}
