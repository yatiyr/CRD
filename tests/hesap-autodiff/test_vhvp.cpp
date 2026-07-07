// test_vhvp.cpp — Phase 3.1.6 v16-e (vectorized addendum): the VECTORIZED forward-over-reverse HVP (a tape of vector
// ops, SIMD-vectorizable) must agree EXACTLY with the scalar HVP (hvp.hpp, already gated vs the v15-c hyper-dual) on
// the same functor, with the v15-c exact Hessian H·v and central FD, and be bit-identical run-to-run.

#include <crd/hesap/autodiff/hvp.hpp>       // scalar HVP (reference)
#include <crd/hesap/autodiff/hyperdual.hpp> // exact-Hessian oracle
#include <crd/hesap/autodiff/vhvp.hpp>      // the vectorized HVP under test

#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

namespace rev = crd::hesap::autodiff::reverse;
namespace vec = crd::hesap::autodiff::reverse::vec;
namespace fwd = crd::hesap::autodiff::forward;
using crd::f64;
using Catch::Matchers::WithinAbs;
using D = fwd::Dual<f64>;

namespace
{
// the SAME function, scalar-generic (for the scalar HVP + hyper-dual): f(x) = Σ x_i·x_{(i+1)%n} + Σ sin(x_i)
struct RingNoExp
{
    template <class S>
    S operator()(const S* x, int n) const
    {
        using crd::math::sin;
        S acc = x[0] * x[1 % n];
        for (int i = 1; i < n; ++i) { acc = acc + x[i] * x[(i + 1) % n]; }
        for (int i = 0; i < n; ++i) { acc = acc + sin(x[i]); }
        return acc;
    }
};
} // namespace

TEST_CASE("v16-e: VECTORIZED HVP == scalar HVP == hyper-dual H*v == FD, deterministic", "[autodiff][reverse][vhvp]")
{
    constexpr int              n = 6;
    crd::memory::TlsfAllocator alloc(8 << 20);
    f64                        x[n];
    f64                        v[n];
    for (int i = 0; i < n; ++i) { x[i] = 0.3 + 0.2 * std::sin(1.0 + i); v[i] = 0.5 * std::cos(0.4 + i); }

    // vectorized HVP: express f as vector ops — sum(x ⊙ roll(x,−1)) + sum(sin(x))
    auto build = [&](vec::VTape& t) -> vec::VVar
    {
        const vec::VVar xg = vec::input(t, x, v);
        return vec::sum(xg * vec::roll(xg, -1)) + vec::sum(vec::sin(xg));
    };
    vec::VTape vt(&alloc, n, 16);
    f64        vg[n];
    f64        vh[n];
    f64        vg2[n];
    f64        vh2[n];
    vec::vhvp(vt, build, n, vg, vh);
    vec::vhvp(vt, build, n, vg2, vh2);
    for (int i = 0; i < n; ++i) // determinism
    {
        CHECK(vg[i] == vg2[i]);
        CHECK(vh[i] == vh2[i]);
    }

    // scalar HVP (reference) on the same functor
    rev::RTape<D> st(&alloc);
    rev::RVar<D>  scr[n];
    f64           sg[n];
    f64           sh[n];
    rev::hvp(RingNoExp{}, {x, n}, {v, n}, {sg, n}, {sh, n}, st, {scr, n});
    for (int i = 0; i < n; ++i)
    {
        CHECK_THAT(vg[i], WithinAbs(sg[i], 1e-10));
        CHECK_THAT(vh[i], WithinAbs(sh[i], 1e-10));
    }

    // v15-c hyper-dual exact Hessian: H·v
    f64 hess[n * n];
    fwd::hessian<n>(RingNoExp{}, x, hess);
    for (int i = 0; i < n; ++i)
    {
        f64 s = 0.0;
        for (int j = 0; j < n; ++j) { s += hess[i * n + j] * v[j]; }
        CHECK_THAT(vh[i], WithinAbs(s, 1e-9));
    }

    // grad == central FD of f
    const f64 h = 1e-6;
    for (int i = 0; i < n; ++i)
    {
        const f64 sv = x[i];
        x[i]         = sv + h;
        const f64 fp = RingNoExp{}(x, n);
        x[i]         = sv - h;
        const f64 fm = RingNoExp{}(x, n);
        x[i]         = sv;
        CHECK_THAT(vg[i], WithinAbs((fp - fm) / (2.0 * h), 1e-6));
    }
}
