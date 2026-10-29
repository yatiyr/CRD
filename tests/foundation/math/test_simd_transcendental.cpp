// crd-math — f64 SIMD transcendentals (crd_log / crd_exp). Gates: (1) accuracy vs std; (2) scalar↔SIMD BIT-IDENTITY
// (the property that lets a parallel batch's SIMD body + scalar tail stay byte-for-byte equal); (3) edge lanes.

#include <crd/math/simd/transcendental.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace d = crd::math;
using crd::f64;

namespace
{
[[maybe_unused]] bool same_bits(f64 a, f64 b) // used only by the twin bit-identity checks (unused on scalar)
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
    // crd_log1 measured ≤0.52 ulp over its hard inputs, the C library's log under 1 ulp (UCRT x64 ≤0.51 on these
    // points against mpmath; glibc documents its bound), so the two doubles are at most one ulp apart.
    f64 max_ulp = 0.0;
    f64 worst = 0.0;
    std::uint64_t s = 0x1234567ULL;
    for (int i = 0; i < 2000000; ++i)
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        const f64 u = static_cast<f64>(s >> 11) * (1.0 / 9007199254740992.0);
        const f64 x = std::exp((u - 0.5) * 1400.0); // spans ~1e-304 .. 1e304
        const f64 ref = std::log(x);
        const f64 ulp = std::nextafter(std::abs(ref), std::numeric_limits<f64>::infinity()) - std::abs(ref);
        const f64 err = std::abs(d::crd_log1(x) - ref) / ulp;
        if (err > max_ulp)
        {
            max_ulp = err;
            worst = x;
        }
    }
    INFO("max_ulp=" << max_ulp << " worst_x=" << worst);
    CHECK(max_ulp <= 1.0);
}

TEST_CASE("simd transcendental: crd_log1 edge cases", "[math][simd][transcendental]")
{
    CHECK(d::crd_log1(1.0) == 0.0);
    CHECK((std::isinf(d::crd_log1(0.0)) && d::crd_log1(0.0) < 0.0));
    CHECK(std::isnan(d::crd_log1(-1.0)));
    CHECK(std::isinf(d::crd_log1(std::numeric_limits<f64>::infinity())));
    CHECK(std::isnan(d::crd_log1(std::numeric_limits<f64>::quiet_NaN())));
    CHECK(std::abs(d::crd_log1(5e-310) - std::log(5e-310)) <= 1e-15 * std::abs(std::log(5e-310)));
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

#if CRD_SIMD_HAS_SSE2
// General code's twins, on the x64 baseline (ADR-0134 §4): two lanes a call. The four- and eight-lane twins run at
// their own levels in the SIMD-type probe (kernels/simd_level_probe.cpp), each held to portable's bits.
namespace
{
void twin(bool exp, const f64* x, f64* r)
{
    for (int k = 0; k < 4; k += 2)
    {
        const __m128d v = _mm_loadu_pd(x + k);
        _mm_storeu_pd(r + k, exp ? d::crd_exp_x2(v) : d::crd_log_x2(v));
    }
}
} // namespace

TEST_CASE("simd transcendental: crd_exp_x2 BIT-IDENTICAL to crd_exp1", "[math][simd][transcendental][moat]")
{
    std::uint64_t s = 0x99CC33ULL;
    bool all_identical = true;
    for (int i = 0; i < 500000; ++i)
    {
        f64 x[4];
        for (double& xi : x)
        {
            s = s * 6364136223846793005ULL + 1442695040888963407ULL;
            xi = (static_cast<f64>(s >> 11) * (1.0 / 9007199254740992.0) - 0.5) * 1400.0;
        }
        f64 r[4];
        twin(true, x, r);
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

TEST_CASE("simd transcendental: crd_log_x2 BIT-IDENTICAL to crd_log1 (moat property)",
          "[math][simd][transcendental][moat]")
{
    std::uint64_t s = 0xABCDEFULL;
    bool all_identical = true;
    for (int i = 0; i < 500000; ++i)
    {
        f64 x[4];
        for (double& xi : x)
        {
            s = s * 6364136223846793005ULL + 1442695040888963407ULL;
            const f64 u = static_cast<f64>(s >> 11) * (1.0 / 9007199254740992.0);
            xi = std::exp((u - 0.5) * 1400.0);
        }
        f64 r[4];
        twin(false, x, r);
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

TEST_CASE("simd transcendental: crd_log_x2 edge lanes", "[math][simd][transcendental]")
{
    const f64 x[4] = {0.0, -2.0, std::numeric_limits<f64>::infinity(), 5e-310};
    f64 r[4];
    twin(false, x, r);
    CHECK((std::isinf(r[0]) && r[0] < 0.0));
    CHECK(std::isnan(r[1]));
    CHECK(std::isinf(r[2]));
    CHECK(same_bits(r[3], d::crd_log1(5e-310)));
}
#endif
