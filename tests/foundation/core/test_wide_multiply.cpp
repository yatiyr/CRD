#include <catch2/catch_test_macros.hpp>
#include <crd/core/wide_multiply.hpp>

// The exact 128-bit product and its quotient (crd/core/wide_multiply.hpp): known values at the edges, and on random
// operands the quotient's defining property, q * d <= a * b < (q + 1) * d, held by the host's native division and by
// the restoring division the hosts without one run.

namespace
{
using crd::u64;
using crd::WideProduct;

constexpr u64 kMax = ~u64{0};

bool less_equal(WideProduct a, WideProduct b)
{
    return a.hi < b.hi || (a.hi == b.hi && a.lo <= b.lo);
}

// a * b - q * d, when it is below 2^64 (it must be, below d, for q to be the floor).
bool is_floor(u64 a, u64 b, u64 d, u64 q)
{
    const WideProduct product = crd::multiply_wide(a, b);
    const WideProduct taken = crd::multiply_wide(q, d);
    if (!less_equal(taken, product))
    {
        return false;
    }
    const u64 borrow = (product.lo < taken.lo) ? 1U : 0U;
    const u64 hi = product.hi - taken.hi - borrow;
    const u64 lo = product.lo - taken.lo;
    return hi == 0U && lo < d;
}

u64 next(u64& state)
{
    state ^= state << 13U;
    state ^= state >> 7U;
    state ^= state << 17U;
    return state;
}

} // namespace

TEST_CASE("core wide multiply: the exact product at the edges", "[core][wide-multiply]")
{
    WideProduct p = crd::multiply_wide(kMax, kMax);
    CHECK(p.hi == kMax - 1U);
    CHECK(p.lo == 1U);
    p = crd::multiply_wide(u64{1} << 32U, u64{1} << 32U);
    CHECK(p.hi == 1U);
    CHECK(p.lo == 0U);
    p = crd::multiply_wide(0U, kMax);
    CHECK(p.hi == 0U);
    CHECK(p.lo == 0U);
}

TEST_CASE("core multiply_divide: exact quotients, and ~0 past 64 bits or for a zero divisor", "[core][wide-multiply]")
{
    CHECK(crd::multiply_divide(1000U, 1000000000U, 3U) == 333333333333U);
    CHECK(crd::multiply_divide(kMax, kMax, kMax) == kMax);
    CHECK(crd::multiply_divide(kMax, 2U, 4U) == kMax / 2U);
    CHECK(crd::multiply_divide(u64{1} << 40U, u64{1} << 40U, u64{1} << 30U) == u64{1} << 50U);
    CHECK(crd::multiply_divide(7U, 0U, 5U) == 0U);
    CHECK(crd::multiply_divide(7U, 5U, 0U) == kMax);
    CHECK(crd::multiply_divide(kMax, kMax, kMax - 1U) == kMax); // the quotient passes 64 bits
    CHECK(crd::multiply_divide(u64{1} << 63U, 4U, 2U) == kMax);
    CHECK(crd::multiply_divide(u64{1} << 63U, 4U, 3U) == 12297829382473034410U); // floor(2^65 / 3)
    CHECK(crd::multiply_divide(u64{1} << 63U, 2U, 3U) == 6148914691236517205U); // floor(2^64 / 3)
}

TEST_CASE("core multiply_divide: native and restoring division agree with the floor on random operands",
          "[core][wide-multiply]")
{
    u64 state = 0x9E3779B97F4A7C15U;
    for (int i = 0; i < 20000; ++i)
    {
        const unsigned shift = static_cast<unsigned>(next(state) % 64U);
        const u64 a = next(state) >> (next(state) % 64U);
        const u64 b = next(state) >> (next(state) % 64U);
        const u64 d = (next(state) >> shift) | 1U;
        const WideProduct p = crd::multiply_wide(a, b);
        const u64 q = crd::multiply_divide(a, b, d);
        if (p.hi >= d)
        {
            CHECK(q == kMax);
            continue;
        }
        CHECK(is_floor(a, b, d, q));
        CHECK(crd::detail::divide_wide_restoring(p, d) == q);
    }
}
