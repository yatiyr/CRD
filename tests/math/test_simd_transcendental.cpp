// crd-math — f64 SIMD transcendentals (crd_log / crd_exp). Gates: (1) accuracy vs std; (2) scalar↔SIMD BIT-IDENTITY
// (the property that lets a parallel batch's SIMD body + scalar tail stay byte-for-byte equal); (3) edge lanes.

#include <crd/math/simd/transcendental.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace d = crd::math;
using crd::f64;

namespace
{
bool same_bits(f64 a, f64 b)
{
    std::uint64_t ba;
    std::uint64_t bb;
    std::memcpy(&ba, &a, 8);
    std::memcpy(&bb, &b, 8);
    return ba == bb;
}
} // namespace

TEST_CASE("simd transcendental: crd_log1 accuracy vs std::log", "[math][simd][transcendental]")
{
    f64 max_abs = 0.0;
    f64 max_rel = 0.0;
    f64 worst = 0.0;
    std::uint64_t s = 0x1234567ULL;
    for (int i = 0; i < 2000000; ++i)
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        const f64 u = static_cast<f64>(s >> 11) * (1.0 / 9007199254740992.0);
        const f64 x = std::exp((u - 0.5) * 1400.0); // spans ~1e-304 .. 1e304
        const f64 got = d::crd_log1(x);
        const f64 ref = std::log(x);
        const f64 abs = std::abs(got - ref);
        const f64 rel = abs / (std::abs(ref) + 1e-300);
        if (rel > max_rel)
        {
            max_rel = rel;
            worst = x;
        }
        max_abs = std::max(max_abs, abs);
    }
    INFO("max_abs=" << max_abs << " max_rel=" << max_rel << " worst_x=" << worst);
    CHECK(max_rel < 1e-13);
    CHECK(max_abs < 1e-13);
}

TEST_CASE("simd transcendental: crd_log1 edge cases", "[math][simd][transcendental]")
{
    CHECK(d::crd_log1(1.0) == 0.0);
    CHECK((std::isinf(d::crd_log1(0.0)) && d::crd_log1(0.0) < 0.0));
    CHECK(std::isnan(d::crd_log1(-1.0)));
    CHECK(std::isinf(d::crd_log1(std::numeric_limits<f64>::infinity())));
    CHECK(std::isnan(d::crd_log1(std::numeric_limits<f64>::quiet_NaN())));
    CHECK(std::abs(d::crd_log1(5e-310) - std::log(5e-310)) < 1e-13 * std::abs(std::log(5e-310)));
}

TEST_CASE("simd transcendental: crd_exp1 accuracy vs std::exp + edges", "[math][simd][transcendental]")
{
    f64 max_rel = 0.0;
    f64 worst = 0.0;
    std::uint64_t s = 0x55AA55ULL;
    for (int i = 0; i < 2000000; ++i)
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        const f64 u = static_cast<f64>(s >> 11) * (1.0 / 9007199254740992.0);
        const f64 x = (u - 0.5) * 1400.0;
        const f64 got = d::crd_exp1(x);
        const f64 ref = std::exp(x);
        const f64 rel = std::abs(got - ref) / (std::abs(ref) + 1e-300);
        if (rel > max_rel)
        {
            max_rel = rel;
            worst = x;
        }
    }
    INFO("max_rel=" << max_rel << " worst_x=" << worst);
    CHECK(max_rel < 1e-13);
    CHECK(d::crd_exp1(0.0) == 1.0);
    CHECK(std::isinf(d::crd_exp1(720.0)));
    CHECK(d::crd_exp1(-750.0) == 0.0);
    CHECK(std::isnan(d::crd_exp1(std::numeric_limits<f64>::quiet_NaN())));
}

#if CRD_SIMD_BACKEND == CRD_SIMD_BACKEND_AVX2
TEST_CASE("simd transcendental: crd_exp4 BIT-IDENTICAL to crd_exp1", "[math][simd][transcendental][moat]")
{
    std::uint64_t s = 0x99CC33ULL;
    bool all_identical = true;
    for (int i = 0; i < 500000; ++i)
    {
        alignas(32) f64 x[4];
        for (double& xi : x)
        {
            s = s * 6364136223846793005ULL + 1442695040888963407ULL;
            xi = (static_cast<f64>(s >> 11) * (1.0 / 9007199254740992.0) - 0.5) * 1400.0;
        }
        alignas(32) f64 r[4];
        _mm256_store_pd(r, d::crd_exp4(_mm256_load_pd(x)));
        for (int k = 0; k < 4; ++k)
        {
            if (!same_bits(r[k], d::crd_exp1(x[k])))
            {
                all_identical = false;
            }
        }
    }
    CHECK(all_identical);
}

TEST_CASE("simd transcendental: crd_log4 BIT-IDENTICAL to crd_log1 (moat property)", "[math][simd][transcendental][moat]")
{
    std::uint64_t s = 0xABCDEFULL;
    bool all_identical = true;
    for (int i = 0; i < 500000; ++i)
    {
        alignas(32) f64 x[4];
        for (double& xi : x)
        {
            s = s * 6364136223846793005ULL + 1442695040888963407ULL;
            const f64 u = static_cast<f64>(s >> 11) * (1.0 / 9007199254740992.0);
            xi = std::exp((u - 0.5) * 1400.0);
        }
        const __m256d v = d::crd_log4(_mm256_load_pd(x));
        alignas(32) f64 r[4];
        _mm256_store_pd(r, v);
        for (int k = 0; k < 4; ++k)
        {
            if (!same_bits(r[k], d::crd_log1(x[k])))
            {
                all_identical = false;
            }
        }
    }
    CHECK(all_identical);
}

TEST_CASE("simd transcendental: crd_log4 edge lanes", "[math][simd][transcendental]")
{
    alignas(32) f64 x[4] = {0.0, -2.0, std::numeric_limits<f64>::infinity(), 5e-310};
    const __m256d v = d::crd_log4(_mm256_load_pd(x));
    alignas(32) f64 r[4];
    _mm256_store_pd(r, v);
    CHECK((std::isinf(r[0]) && r[0] < 0.0));
    CHECK(std::isnan(r[1]));
    CHECK(std::isinf(r[2]));
    CHECK(same_bits(r[3], d::crd_log1(5e-310)));
}
#endif
