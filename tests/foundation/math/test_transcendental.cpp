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
#include <limits>

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
    // |got−ref| in units of ulp(ref); a subnormal's ulp is the smallest subnormal
    return std::fabs(got - ref) / std::max(std::ldexp(1.0, e - 53), std::numeric_limits<double>::denorm_min());
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
    TX_GATE(cbrt, 2.0); // bit-trick seed + Halley steps
    TX_GATE(rsqrt, 1.0);
}

TEST_CASE("crd::math sin, cos and tan keep their bound from the reduction seam to the largest double",
          "[math][transcendental][tx]")
{
    namespace r = crd::math::txref;
    const double sin_ulp =
        max_ulp([](double x) { return crd::math::sin(x); }, r::ref_sin_large_x, r::ref_sin_large_y, r::ref_sin_large_n);
    const double cos_ulp =
        max_ulp([](double x) { return crd::math::cos(x); }, r::ref_cos_large_x, r::ref_cos_large_y, r::ref_cos_large_n);
    const double tan_ulp =
        max_ulp([](double x) { return crd::math::tan(x); }, r::ref_tan_large_x, r::ref_tan_large_y, r::ref_tan_large_n);
    INFO("max ulp: sin " << sin_ulp << ", cos " << cos_ulp << ", tan " << tan_ulp);
    CHECK(sin_ulp <= 1.0);
    CHECK(cos_ulp <= 1.0);
    CHECK(tan_ulp <= 2.0); // the division, as in the Cody-Waite range
    for (int w = 0; w < 37; ++w)
    {
        INFO("2/pi word " << w);
        CHECK(crd::math::trig_detail::kTwoOverPiBits[w] == crd::math::txref::ref_two_over_pi_bits[w]);
    }
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
    std::uint64_t large = 0;
    for (int i = 0; i < crd::math::txref::ref_sin_large_n; ++i)
    {
        const double x = crd::math::txref::ref_sin_large_x[i];
        large = fold(fold(fold(large, crd::math::sin(x)), crd::math::cos(x)), crd::math::tan(x));
    }
#undef TX_FOLD
    std::uint64_t power = 0;
    for (int i = 0; i < crd::math::txref::ref_pow_n; ++i)
    {
        power = fold(power, crd::math::pow(crd::math::txref::ref_pow_x[i], crd::math::txref::ref_pow_p[i]));
    }
    INFO("golden checksum = 0x" << std::hex << h << ", Payne-Hanek range 0x" << large << ", pow 0x" << power);
    CHECK(h == 0x9179c815fca024a6ULL); // canonical bits; every platform and level must reproduce
    CHECK(large == 0x6caf0b0834f6ea14ULL);
    CHECK(power == 0x45d1f044b54f2901ULL);
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

}

TEST_CASE("crd::math pow keeps its bound over the whole exponent range and the IEEE cases",
          "[math][transcendental][tx]")
{
    namespace m = crd::math;
    namespace r = crd::math::txref;
    double pu = 0.0;
    int worst = 0;
    for (int i = 0; i < r::ref_pow_n; ++i)
    {
        const double e = ulp_err(m::pow(r::ref_pow_x[i], r::ref_pow_p[i]), r::ref_pow_y[i]);
        if (e > pu)
        {
            pu = e;
            worst = i;
        }
    }
    INFO("pow max ulp vs mpmath = " << pu << " at (" << r::ref_pow_x[worst] << ", " << r::ref_pow_p[worst] << ")");
    CHECK(pu <= 1.0);
    // The pair constants, bit for bit against mpmath's.
    const double pairs[] = {m::kLn2PairHi, m::kLn2PairLo, m::kLog2eHi, m::kLog2eLo,
                            m::kThirdHi,   m::kThirdLo,   m::kFifthHi, m::kFifthLo};
    for (int i = 0; i < 8; ++i)
    {
        INFO("pair constant " << i);
        CHECK(pairs[i] == r::ref_pair_constants[i]);
    }
    // Exact results: a correctly rounded value within 0.58 ulp of an exact double is that double.
    CHECK(m::pow(2.0, 10.0) == 1024.0);
    CHECK(m::pow(-2.0, 3.0) == -8.0);
    CHECK(m::pow(4.0, 0.5) == 2.0);
    CHECK(m::pow(10.0, 2.0) == 100.0);
    CHECK(m::pow(3.0, 0.0) == 1.0);
    for (const double x : {0.3, 1.7, 123456.789, 1e-300, 1e300})
    {
        CHECK(m::pow(x, 1.0) == x);
    }
    for (const double x : {0.25, 8.0, 0x1p-1000, 0x1p1000})
    {
        CHECK(m::pow(x, -1.0) == 1.0 / x); // an exact reciprocal (pow(1e-300, -1) is 0.52 ulp off, as allowed)
    }
    // A subnormal base takes the same path: its log is exact at a power of two, and an odd power keeps the sign.
    constexpr double min_sub = std::numeric_limits<double>::denorm_min();
    CHECK(m::pow(min_sub, 1.0) == min_sub);
    const double neg_cube = m::pow(-min_sub, 3.0);
    CHECK((neg_cube == 0.0 && std::signbit(neg_cube)));
    // y·log2 x past the exponent range: the head decides, so a product that overflows gives ±inf or ±0, never a
    // NaN or the wrong sign.
    constexpr double inf = std::numeric_limits<double>::infinity();
    CHECK(m::pow(10.0, 1e308) == inf);
    CHECK(m::pow(-10.0, 1e308) == inf); // 1e308 is an even integer
    CHECK(m::pow(0.5, -1e308) == inf);
    CHECK(m::pow(10.0, 1e299) == inf);
    const double tiny = m::pow(0.1, 1e308);
    CHECK((tiny == 0.0 && !std::signbit(tiny)));
    const double tiny2 = m::pow(10.0, -1e299);
    CHECK((tiny2 == 0.0 && !std::signbit(tiny2)));
    CHECK(m::pow(-3.0, 1001.0) == -inf);
    const double neg_tiny = m::pow(-3.0, -1001.0);
    CHECK((neg_tiny == 0.0 && std::signbit(neg_tiny)));
    // C99 Annex F: ±0 with odd and even y and with -inf, ±1, NaN and the |x| against ±inf rules.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    CHECK(m::pow(0.0, 3.0) == 0.0);
    CHECK(std::signbit(m::pow(-0.0, 3.0)));
    CHECK(!std::signbit(m::pow(-0.0, 4.0)));
    CHECK(m::pow(0.0, -3.0) == inf);
    CHECK(m::pow(-0.0, -3.0) == -inf);
    CHECK(m::pow(-0.0, -4.0) == inf);
    CHECK(m::pow(0.0, -inf) == inf);
    CHECK(m::pow(-1.0, inf) == 1.0);
    CHECK(m::pow(-1.0, -inf) == 1.0);
    CHECK(m::pow(1.0, nan) == 1.0);
    CHECK(m::pow(nan, 0.0) == 1.0);
    CHECK(std::isnan(m::pow(nan, 1.0)));
    CHECK(std::isnan(m::pow(-2.0, 0.5)));
    CHECK(m::pow(0.5, inf) == 0.0);
    CHECK(m::pow(0.5, -inf) == inf);
    CHECK(m::pow(2.0, inf) == inf);
    CHECK(m::pow(2.0, -inf) == 0.0);
    CHECK(m::pow(-inf, 3.0) == -inf);
    CHECK(m::pow(-inf, 4.0) == inf);
    CHECK(std::signbit(m::pow(-inf, -3.0)));
    CHECK(m::pow(inf, -2.0) == 0.0);
    // f32 rounds the f64 result once.
    CHECK(m::pow(2.0F, 10.0F) == 1024.0F);
    CHECK(m::pow(10.0F, 50.0F) == std::numeric_limits<float>::infinity());
}

TEST_CASE("crd::math expm1 and the trig quadrant stay defined to the ends of the finite range",
          "[math][transcendental][tx]")
{
    namespace m = crd::math;
    constexpr double max_value = std::numeric_limits<double>::max();
    // Past 1023.5·ln2 the reduction's 2^k is 2^1024, which overflows alone; the result does not.
    for (const double x : {709.437, 709.5, 709.7, 709.78})
    {
        INFO("expm1(" << x << ")");
        CHECK(std::isfinite(m::expm1(x)));
        CHECK(ulp_err(m::expm1(x), std::expm1(x)) <= 2.0); // both ≤1 ulp
    }
    for (const double x : {709.79, 710.0, 1e300, max_value})
    {
        CHECK(m::expm1(x) == std::numeric_limits<double>::infinity());
    }
    for (const double x : {-40.5, -745.2, -1e300, -max_value})
    {
        CHECK(m::expm1(x) == -1.0);
    }
    // A huge argument's k = round(x·2/π) fits no integer type; its quadrant k mod 4 is still exact (mpmath's).
    for (int i = 0; i < m::txref::ref_quadrant_n; ++i)
    {
        double r = 0.0;
        double rt = 0.0;
        INFO("x = " << m::txref::ref_quadrant_x[i]);
        CHECK(m::trig_detail::reduce_pio2(m::txref::ref_quadrant_x[i], r, rt) == m::txref::ref_quadrant_q[i]);
        CHECK(std::fabs(r) <= m::kQuarterPi);
    }
}
