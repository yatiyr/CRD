// crd-math transcendental family — accuracy gate (≤1 ulp vs mpmath ground truth) for the unified crd::math::*
// facade. The family pattern (docs/phases/crd-math-transcendental.md): every function gated here against
// gen_transcendental_refs.py output. std:: in the ulp HELPER is test infrastructure, not engine math.

#include <catch2/catch_test_macros.hpp>

#include <crd/math/hyperbolic.hpp>
#include <crd/math/power.hpp>
#include <crd/math/select.hpp>
#include <crd/math/transcendental.hpp>

#include "transcendental_refs.inc" // crd::math::txref::ref_<fn>_{x,y,n}

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace
{
[[nodiscard]] double ulp_err(double got, double ref) noexcept
{
    if (got == ref)
    {
        return 0.0;
    }
    if (!std::isfinite(got) || !std::isfinite(ref))
    {
        return 1e18; // finite-vs-nonfinite mismatch
    }
    const double a = std::fabs(ref);
    int e = 0;
    std::frexp(a == 0.0 ? 1.0 : a, &e);
    return std::fabs(got - ref) / std::ldexp(1.0, e - 53); // |got−ref| in units of ulp(ref)
}

template <class F>
[[nodiscard]] double max_ulp(F f, const double* xs, const double* ys, int n) noexcept
{
    double m = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double e = ulp_err(f(xs[i]), ys[i]);
        if (e > m)
        {
            m = e;
        }
    }
    return m;
}
} // namespace

#define TX_GATE(fn, thr)                                                                                                \
    do                                                                                                                  \
    {                                                                                                                   \
        const double mu = max_ulp([](double x) { return crd::math::fn(x); }, crd::math::txref::ref_##fn##_x,            \
                                  crd::math::txref::ref_##fn##_y, crd::math::txref::ref_##fn##_n);                       \
        INFO(#fn << " max ulp = " << mu);                                                                               \
        CHECK(mu <= (thr));                                                                                             \
    } while (0)

TEST_CASE("crd::math transcendental family accuracy vs mpmath", "[math][transcendental][tx]")
{
    TX_GATE(exp, 1.0);
    TX_GATE(exp2, 1.0);
    TX_GATE(exp10, 1.0);
    TX_GATE(expm1, 1.0);
    TX_GATE(log, 1.0);
    TX_GATE(log2, 1.0);
    TX_GATE(log10, 1.0);
    TX_GATE(log1p, 1.0);
    TX_GATE(sin, 1.0);
    TX_GATE(cos, 1.0);
    TX_GATE(tan, 2.0); // tan = sin/cos ⇒ ≤2 ulp (the division); sin/cos themselves are ≤1 ulp
    TX_GATE(sinh, 2.0); // hyperbolics compose expm1/log1p ⇒ ≤2 ulp
    TX_GATE(cosh, 2.0);
    TX_GATE(tanh, 2.0);
    TX_GATE(asinh, 2.0);
    TX_GATE(acosh, 2.0);
    TX_GATE(atanh, 3.0); // composes two log1p (≤1 ulp each) ⇒ ≤3 ulp
    TX_GATE(atan, 2.0);
    TX_GATE(asin, 3.0); // = atan(x/√(1−x²)) ⇒ composed
    TX_GATE(acos, 3.0); // = 2·atan(√((1−x)/(1+x))) ⇒ composed
    TX_GATE(cbrt, 2.0); // exp2/log2 seed + 1 Halley
    TX_GATE(rsqrt, 1.0);
}

// Cross-platform determinism (the moat): the kernel is -ffp-contract=off + explicit fma ⇒ EVERY platform/compiler
// (gcc/clang/MSVC, x64/ARM/WASM) must produce bit-identical output. We fold every facade result over the ref grids
// into one checksum; the committed value is the canonical bits. A mismatch on any platform = a determinism break.
namespace
{
[[nodiscard]] std::uint64_t fold(std::uint64_t h, double v) noexcept
{
    std::uint64_t b = 0;
    std::memcpy(&b, &v, sizeof(b));
    return h ^ (b + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2));
}
} // namespace

TEST_CASE("crd::math transcendental cross-platform determinism golden checksum", "[math][transcendental][tx][moat]")
{
    std::uint64_t h = 0;
#define TX_FOLD(fn)                                                                                                     \
    for (int i = 0; i < crd::math::txref::ref_##fn##_n; ++i)                                                            \
    h = fold(h, crd::math::fn(crd::math::txref::ref_##fn##_x[i]))
    TX_FOLD(exp);
    TX_FOLD(exp2);
    TX_FOLD(exp10);
    TX_FOLD(expm1);
    TX_FOLD(log);
    TX_FOLD(log2);
    TX_FOLD(log10);
    TX_FOLD(log1p);
    TX_FOLD(sin);
    TX_FOLD(cos);
    TX_FOLD(tan);
    TX_FOLD(sinh);
    TX_FOLD(cosh);
    TX_FOLD(tanh);
    TX_FOLD(asinh);
    TX_FOLD(acosh);
    TX_FOLD(atanh);
    TX_FOLD(atan);
    TX_FOLD(asin);
    TX_FOLD(acos);
    TX_FOLD(cbrt);
    TX_FOLD(rsqrt);
#undef TX_FOLD
    INFO("golden checksum = 0x" << std::hex << h);
    CHECK(h == 0x3e379d52f6f98672ULL); // canonical bits (gcc/-ffp-contract=off); every platform must reproduce
}

TEST_CASE("crd::math select/round tier -- exactness", "[math][transcendental][tx]")
{
    namespace m = crd::math;
    CHECK(m::min(2.0, 3.0) == 2.0);
    CHECK(m::max(2.0, 3.0) == 3.0);
    CHECK(m::clamp(5.0, 0.0, 1.0) == 1.0);
    CHECK(m::clamp(-5.0, 0.0, 1.0) == 0.0);
    CHECK(m::abs(-2.5) == 2.5);
    CHECK(m::floor(2.7) == 2.0);
    CHECK(m::ceil(2.1) == 3.0);
    CHECK(m::round(2.5) == 3.0);
    CHECK(m::trunc(-2.7) == -2.0);
    CHECK(m::sqrt(16.0) == 4.0); // IEEE-exact
    CHECK(m::copysign(3.0, -1.0) == -3.0);
    CHECK(m::sign(-7.0) == -1.0);
    CHECK(m::sign(7.0) == 1.0);
    CHECK(m::saturate(1.5) == 1.0);
    CHECK(m::lerp(2.0, 4.0, 0.5) == 3.0);
    CHECK(m::fma(2.0, 3.0, 1.0) == 7.0);
    CHECK(m::min(2.0F, 3.0F) == 2.0F);
    CHECK(m::floor(2.7F) == 2.0F);
}

TEST_CASE("crd::math atan2 quadrants + determinism", "[math][transcendental][tx][moat]")
{
    // atan2 vs std::atan2 (≤1 ulp itself) over all 4 quadrants + the axes; ≤4 ulp ⇒ crd atan2 ~≤3 ulp vs true.
    double maxu = 0.0;
    std::uint64_t h = 0;
    const double vs[] = {0.0, 0.3, 1.0, 2.5, 7.0, -0.3, -1.0, -2.5, -7.0, 1e3, -1e3};
    for (double y : vs)
    {
        for (double x : vs)
        {
            if (x == 0.0 && y == 0.0)
            {
                continue;
            }
            const double got = crd::math::atan2(y, x);
            maxu = std::max(maxu, ulp_err(got, std::atan2(y, x)));
            h = fold(h, got);
        }
    }
    INFO("atan2 max ulp vs std = " << maxu << ", checksum = 0x" << std::hex << h);
    CHECK(maxu <= 4.0);
    CHECK(h == 0x252e50abe05c113bULL); // determinism anchor (gcc canonical; every platform must reproduce)

    // hypot vs std::hypot (≤1 ulp): scaled √(x²+y²), no spurious overflow/underflow.
    double hu = 0.0;
    const double hv[] = {0.0, 0.5, 3.0, 4.0, 1e300, 1e-300, -3.0, 7.5, 1e-200};
    for (double a : hv)
    {
        for (double b : hv)
        {
            hu = std::max(hu, ulp_err(crd::math::hypot(a, b), std::hypot(a, b)));
        }
    }
    INFO("hypot max ulp vs std = " << hu);
    CHECK(hu <= 2.0);
    CHECK(crd::math::hypot(3.0, 4.0) == 5.0); // exact Pythagorean triple

    // pow vs std::pow over moderate exponents (2^{y·log2 x}); large |y·log2 x| widens (documented).
    double pu = 0.0;
    const double px[] = {0.1, 0.5, 1.5, 2.0, 3.0, 7.0, 10.0};
    const double py[] = {-4.0, -2.5, -1.0, 0.0, 0.5, 1.0, 2.0, 3.5, 6.0};
    for (double a : px)
    {
        for (double b : py)
        {
            pu = std::max(pu, ulp_err(crd::math::pow(a, b), std::pow(a, b)));
        }
    }
    INFO("pow max ulp vs std = " << pu);
    CHECK(pu <= 2.0); // 2^{y·log2 x} with a double-double exponent ⇒ ≤2 ulp
    CHECK(crd::math::pow(2.0, 10.0) == 1024.0);   // exact integer power
    CHECK(crd::math::pow(-2.0, 3.0) == -8.0);     // negative base, odd integer exponent
    CHECK(crd::math::pow(3.0, 0.0) == 1.0);
}
