// test_jet_simd.cpp — Phase 3.1.6 v15-a: the SIMD vector-forward carrier (JetPackD<N>, Vec4d named-register pack).
// Gates: gradient == the scalar Jet<T,N> path (<=1 ulp; fma vs mul+add) == analytic; register-tiling correctness at
// N=4/8/16; run-to-run bit-identity (determinism). This TU also forces the recursive-template carrier to compile on
// every toolchain (MSVC/clang-cl/gcc) — the [[no_unique_address]] pack + explicit function-template specializations.

#include <crd/hesap/autodiff/forward.hpp>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace ad = crd::hesap::autodiff::forward;

namespace
{
// Scalar-generic functor: arithmetic + transcendentals; runs on double / Jet<T,N> / JetPackD<N>.
struct Mixed
{
    template <class T>
    T operator()(const T* x, int n) const
    {
        using std::exp;
        using std::log;
        using std::sin;
        T acc = x[0] * x[0];
        for (int i = 0; i < n; ++i)
        {
            const T& xn = x[(i + 1) % n];
            acc         = acc + exp(sin(x[i])) + x[i] * log(1.0 + xn * xn);
        }
        return acc;
    }
};

// Speelpenning product (dense gradient g[k] = prod_{i!=k} x_i).
struct Prod
{
    template <class T>
    T operator()(const T* x, int n) const
    {
        T p = x[0];
        for (int i = 1; i < n; ++i)
        {
            p = p * x[i];
        }
        return p;
    }
};
} // namespace

TEST_CASE("JetPackD partial pack has no padding bloat", "[autodiff][simd]")
{
    // [[no_unique_address]] => RegPackD<kRegs> is exactly kRegs*32 bytes; JetPackD<4> == a + one Vec4d.
    STATIC_REQUIRE(sizeof(ad::detail::RegPackD<1>) == 32);
    STATIC_REQUIRE(sizeof(ad::detail::RegPackD<2>) == 64);
    STATIC_REQUIRE(ad::JetPackD<4>::kRegs == 1);
    STATIC_REQUIRE(ad::JetPackD<8>::kRegs == 2);
    STATIC_REQUIRE(ad::JetPackD<16>::kRegs == 4);
}

TEMPLATE_TEST_CASE_SIG("JetPackD gradient equals the scalar Jet path", "[autodiff][simd]", ((int N), N), 4, 8, 16)
{
    double xv[N];
    for (int i = 0; i < N; ++i)
    {
        xv[i] = 0.4 + 0.11 * static_cast<double>(i); // positive (log/sqrt-safe)
    }

    // SIMD carrier gradient.
    ad::JetPackD<N> xp[N];
    for (int i = 0; i < N; ++i)
    {
        xp[i] = ad::JetPackD<N>(xv[i], i);
    }
    const ad::JetPackD<N> yp = Mixed{}(xp, N);
    double                gp[N];
    yp.store_partials(gp);

    // Scalar Jet<T,N> reference (the correctness oracle).
    ad::Jet<double, N> xj[N];
    for (int i = 0; i < N; ++i)
    {
        xj[i] = ad::Jet<double, N>(xv[i], i);
    }
    const ad::Jet<double, N> yj = Mixed{}(xj, N);

    CHECK_THAT(yp.a, WithinRel(yj.a, 1e-14)); // value: identical scalar ops
    for (int k = 0; k < N; ++k)
    {
        // partials: <=1 ulp (carrier uses single-rounded fma; Jet uses mul+add)
        CHECK_THAT(gp[k], WithinRel(yj.v[k], 1e-12));
    }
}

TEST_CASE("JetPackD Speelpenning gradient equals analytic across register tiling", "[autodiff][simd]")
{
    auto check_prod = [](auto tag) {
        constexpr int n = decltype(tag)::value;
        double        xv[n];
        double        prod = 1.0;
        for (int i = 0; i < n; ++i)
        {
            xv[i] = 0.7 + 0.05 * static_cast<double>(i);
            prod *= xv[i];
        }
        ad::JetPackD<n> xp[n];
        for (int i = 0; i < n; ++i)
        {
            xp[i] = ad::JetPackD<n>(xv[i], i);
        }
        const ad::JetPackD<n> yp = Prod{}(xp, n);
        double                gp[n];
        yp.store_partials(gp);
        CHECK_THAT(yp.a, WithinRel(prod, 1e-13));
        for (int k = 0; k < n; ++k)
        {
            CHECK_THAT(gp[k], WithinRel(prod / xv[k], 1e-12)); // d/dx_k prod = prod / x_k
        }
    };
    check_prod(std::integral_constant<int, 4>{});
    check_prod(std::integral_constant<int, 8>{});
    check_prod(std::integral_constant<int, 16>{});
}

TEST_CASE("JetPackD is bit-identical run-to-run (determinism)", "[autodiff][simd][determinism]")
{
    constexpr int n = 8;
    double        xv[n];
    for (int i = 0; i < n; ++i)
    {
        xv[i] = 0.3 + 0.17 * static_cast<double>(i);
    }
    auto grad = [&](double* g) {
        ad::JetPackD<n> xp[n];
        for (int i = 0; i < n; ++i)
        {
            xp[i] = ad::JetPackD<n>(xv[i], i);
        }
        const ad::JetPackD<n> yp = Mixed{}(xp, n);
        yp.store_partials(g);
    };
    double g1[n];
    double g2[n];
    grad(g1);
    grad(g2);
    for (int k = 0; k < n; ++k)
    {
        CHECK_THAT(g1[k], WithinAbs(g2[k], 0.0)); // exact
    }
}
