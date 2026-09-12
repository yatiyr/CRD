// test_reverse.cpp — Phase 3.1.6 v16-a: the deterministic reverse-mode tape. Gates: one backward pass yields the
// WHOLE gradient (≡ analytic ≡ central FD); the backward is bit-deterministic run-to-run (fixed order, no atomics);
// the data-parallel batched gradient is BIT-IDENTICAL across worker counts {1,2,4} (the {1..16} moat — folded in
// fixed sample order, per-sample tapes, no shared adjoints).

#include <crd/hesap/autodiff/reverse.hpp>

#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

namespace rev = crd::hesap::autodiff::reverse;
using crd::f64;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace
{
// f(x) = exp(x0) + Σ_{i=1}^{n-1} x_{i-1}·x_i  — scalar-generic (Var for reverse, double for the FD oracle).
struct GradF
{
    template <class T>
    T operator()(const T* x, int n) const
    {
        using crd::math::exp;
        T acc = exp(x[0]);
        for (int i = 1; i < n; ++i) { acc = acc + x[i - 1] * x[i]; }
        return acc;
    }
};
// vector f: R³→R²  y0 = x0·x1 + x2 ; y1 = sin(x0) − x1·x2
struct VecF
{
    void operator()(const rev::Var* x, int /*n*/, rev::Var* y, int /*m*/) const
    {
        y[0] = x[0] * x[1] + x[2];
        y[1] = rev::sin(x[0]) - x[1] * x[2];
    }
};
// per-sample least-squares loss: (θ·a_s − b_s)²
struct LossF
{
    const f64* a; // S×n features (row-major)
    const f64* b; // S targets
    rev::Var operator()(const rev::Var* theta, int n, int s) const
    {
        rev::Var pred = theta[0] * a[static_cast<crd::usize>(s) * n + 0];
        for (int i = 1; i < n; ++i) { pred = pred + theta[i] * a[static_cast<crd::usize>(s) * n + i]; }
        rev::Var r = pred - b[s];
        return r * r;
    }
};
} // namespace

TEST_CASE("reverse gradient == analytic == FD in one backward pass", "[autodiff][reverse]")
{
    crd::memory::TlsfAllocator alloc(4 << 20);
    rev::Tape                  tape(&alloc);
    constexpr int              n = 4;
    const f64                  x[n]    = {0.5, 1.0, 1.5, 2.0};
    f64                        g[n]    = {};
    rev::Var                   scr[n]  = {};
    rev::gradient(GradF{}, {x, n}, {g, n}, tape, {scr, n});

    // analytic: ∂/∂x0 = e^{x0}+x1 ; interior ∂/∂xi = x_{i-1}+x_{i+1} ; ∂/∂x_{n-1} = x_{n-2}
    f64 an[n];
    an[0] = std::exp(x[0]) + x[1];
    for (int i = 1; i < n - 1; ++i) { an[i] = x[i - 1] + x[i + 1]; }
    an[n - 1] = x[n - 2];
    for (int i = 0; i < n; ++i) { CHECK_THAT(g[i], WithinRel(an[i], 1e-12)); }

    // central FD oracle
    const f64 h = 1e-6;
    for (int i = 0; i < n; ++i)
    {
        f64 xp[n];
        f64 xm[n];
        for (int k = 0; k < n; ++k) { xp[k] = x[k]; xm[k] = x[k]; }
        xp[i] += h;
        xm[i] -= h;
        const f64 fd = (GradF{}(xp, n) - GradF{}(xm, n)) / (2 * h);
        CHECK_THAT(g[i], WithinAbs(fd, 1e-5));
    }
}

TEST_CASE("reverse Jacobian (build graph once, backward per row)", "[autodiff][reverse]")
{
    crd::memory::TlsfAllocator alloc(4 << 20);
    rev::Tape                  tape(&alloc);
    constexpr int              n = 3;
    constexpr int              m = 2;
    const f64                  x[n] = {0.7, 1.3, -0.4};
    f64                        jac[m * n] = {};
    rev::Var                   xs[n] = {};
    rev::Var                   ys[m] = {};
    rev::jacobian(VecF{}, {x, n}, m, {jac, m * n}, tape, {xs, n}, {ys, m});
    // y0 = x0 x1 + x2 → [x1, x0, 1]
    CHECK_THAT(jac[0], WithinRel(x[1], 1e-12));
    CHECK_THAT(jac[1], WithinRel(x[0], 1e-12));
    CHECK_THAT(jac[2], WithinRel(1.0, 1e-12));
    // y1 = sin x0 − x1 x2 → [cos x0, −x2, −x1]
    CHECK_THAT(jac[3], WithinRel(std::cos(x[0]), 1e-12));
    CHECK_THAT(jac[4], WithinRel(-x[2], 1e-12));
    CHECK_THAT(jac[5], WithinRel(-x[1], 1e-12));
}

TEST_CASE("reverse backward is bit-deterministic run-to-run", "[autodiff][reverse]")
{
    crd::memory::TlsfAllocator alloc(4 << 20);
    rev::Tape                  tape(&alloc);
    constexpr int              n = 4;
    const f64                  x[n]   = {0.3, -1.1, 2.2, 0.9};
    f64                        g1[n]  = {};
    f64                        g2[n] = {};
    rev::Var                   scr[n] = {};
    rev::gradient(GradF{}, {x, n}, {g1, n}, tape, {scr, n});
    rev::gradient(GradF{}, {x, n}, {g2, n}, tape, {scr, n});
    for (int i = 0; i < n; ++i) { CHECK(g1[i] == g2[i]); } // exact bit-identity
}

TEST_CASE("MOAT: batched gradient is BIT-IDENTICAL across {1,2,4} workers", "[autodiff][reverse][moat]")
{
    crd::jobs::init(crd::jobs::Config{4U});
    {
        crd::memory::TlsfAllocator a0(2 << 20);
        crd::memory::TlsfAllocator a1(2 << 20);
        crd::memory::TlsfAllocator a2(2 << 20);
        crd::memory::TlsfAllocator a3(2 << 20);
        rev::Tape                  t0(&a0);
        rev::Tape                  t1(&a1);
        rev::Tape                  t2(&a2);
        rev::Tape                  t3(&a3);
        rev::Tape*                 tapes[4] = {&t0, &t1, &t2, &t3};

        constexpr int n = 3;
        constexpr int nsamp = 16;
        f64           amat[nsamp * n];
        f64           b[nsamp];
        for (int s = 0; s < nsamp; ++s)
        {
            for (int i = 0; i < n; ++i) { amat[s * n + i] = 0.2 + 0.1 * std::sin(1.0 + s + 2.0 * i); }
            b[s] = 0.5 + 0.05 * s;
        }
        const LossF  loss{amat, b};
        const f64    theta[n] = {0.4, -0.3, 0.8};
        f64          gbuf[nsamp * n];

        f64 g1[n];
        f64 g2[n];
        f64 g4[n];
        rev::batch_gradient(loss, {theta, n}, nsamp, {g1, n}, {tapes, 1}, {gbuf, nsamp * n}, 1U);
        rev::batch_gradient(loss, {theta, n}, nsamp, {g2, n}, {tapes, 2}, {gbuf, nsamp * n}, 2U);
        rev::batch_gradient(loss, {theta, n}, nsamp, {g4, n}, {tapes, 4}, {gbuf, nsamp * n}, 4U);

        // ★ bit-identical across worker counts (exact ==)
        for (int i = 0; i < n; ++i)
        {
            CHECK(g1[i] == g2[i]);
            CHECK(g1[i] == g4[i]);
        }
        // analytic: ∇ Σ_s (θ·a_s − b_s)² = Σ_s 2(θ·a_s − b_s) a_s
        f64 an[n] = {};
        for (int s = 0; s < nsamp; ++s)
        {
            f64 pred = 0.0;
            for (int i = 0; i < n; ++i) { pred += theta[i] * amat[s * n + i]; }
            const f64 r = pred - b[s];
            for (int i = 0; i < n; ++i) { an[i] += 2.0 * r * amat[s * n + i]; }
        }
        for (int i = 0; i < n; ++i) { CHECK_THAT(g1[i], WithinRel(an[i], 1e-10)); }
    }
    crd::jobs::shutdown();
}
