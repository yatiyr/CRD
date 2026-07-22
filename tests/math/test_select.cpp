// tests/math/test_select.cpp — the native cmath gap-fills in crd::math (select.hpp) must be BIT-EXACT vs std for every
// IEEE-exact case. They replace std with our OWN logic (bit-ops + composition of the exact primitives), so "correct" means
// identical bits, including ties-to-even, ±0, subnormals, and NaN handling. Includes select.hpp directly (not math.hpp) so
// scalar.hpp's parallel min/max don't collide (the two umbrellas are never co-included by design).
#include <crd/math/select.hpp>

#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>

TEST_CASE("crd::math native gap-fills are bit-exact vs std", "[math][select][scalar]")
{
    const double dv[] = {0.0, -0.0, 0.5, -0.5, 1.5, -1.5, 2.5, -2.5, 24.5, -24.5, 2.3, -2.3, 2.7, -2.7, 100.0, -100.0,
                         1e10, -1e10, 0.1, -0.1, 123456.789, -123456.789, 3.0, -3.0, 1e-308,
                         std::numeric_limits<double>::denorm_min(), std::numeric_limits<double>::min()};
    const auto db = [](double x) { return std::bit_cast<crd::u64>(x); };
    for (double x : dv)
    {
        CHECK(crd::math::signbit(x) == std::signbit(x));
        CHECK(db(crd::math::rint(x)) == db(std::rint(x))); // ties-to-even, exact bits (catches ±0)
        CHECK(db(crd::math::nearbyint(x)) == db(std::nearbyint(x)));
        double cip = 0.0;
        double sip = 0.0;
        CHECK(db(crd::math::modf(x, &cip)) == db(std::modf(x, &sip)));
        CHECK(db(cip) == db(sip));
        int          ce = 0;
        int          se = 0;
        const double cm = crd::math::frexp(x, &ce);
        const double sm = std::frexp(x, &se);
        CHECK(ce == se);
        CHECK(db(cm) == db(sm));
    }
    for (double x : dv)
    {
        for (double y : dv)
        {
            CHECK(crd::math::fmax(x, y) == std::fmax(x, y));
            CHECK(crd::math::fmin(x, y) == std::fmin(x, y));
            CHECK(crd::math::fdim(x, y) == std::fdim(x, y));
            if (y != 0.0) { CHECK(db(crd::math::remainder(x, y)) == db(std::remainder(x, y))); } // bit-exact incl. ties-to-even + −0
        }
    }
    // NaN handling: fmax/fmin drop a NaN operand (IEEE maxNum/minNum); remainder edge cases.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    CHECK(crd::math::fmax(nan, 1.0) == 1.0);
    CHECK(crd::math::fmax(1.0, nan) == 1.0);
    CHECK(crd::math::fmin(nan, 1.0) == 1.0);
    CHECK(crd::math::fmin(1.0, nan) == 1.0);
    CHECK(std::isnan(crd::math::remainder(1.0, 0.0)));   // y==0 ⇒ NaN
    CHECK(std::isnan(crd::math::remainder(inf, 3.0)));   // |x|==inf ⇒ NaN
    CHECK(crd::math::remainder(3.0, inf) == 3.0);        // |y|==inf ⇒ x
    CHECK(db(crd::math::remainder(5.5, 2.0)) == db(std::remainder(5.5, 2.0))); // tie: 5.5/2=2.75→rem -0.5
    CHECK(db(crd::math::remainder(-7.0, 2.0)) == db(std::remainder(-7.0, 2.0)));

    // float overloads — same bit-exactness.
    const float fv[] = {0.0F, -0.0F, 0.5F, -0.5F, 2.5F, -2.5F, 24.5F, -24.5F, 2.3F, -2.7F, 100.0F, -1e7F, 0.1F, 3.0F,
                        std::numeric_limits<float>::denorm_min(), std::numeric_limits<float>::min()};
    const auto  fb = [](float x) { return std::bit_cast<crd::u32>(x); };
    for (float x : fv)
    {
        CHECK(crd::math::signbit(x) == std::signbit(x));
        CHECK(fb(crd::math::rint(x)) == fb(std::rint(x)));
        int         ce = 0;
        int         se = 0;
        const float cm = crd::math::frexp(x, &ce);
        const float sm = std::frexp(x, &se);
        CHECK(ce == se);
        CHECK(fb(cm) == fb(sm));
    }
    for (float x : fv)
    {
        for (float y : fv) { CHECK(crd::math::fmax(x, y) == std::fmax(x, y)); }
    }
}
