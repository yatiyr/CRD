// The hesap-special kernels of crd/hesap/special/special_kernels.hpp, compiled once per CPU level by
// crd_add_cpu_kernels (../CMakeLists.txt, ADR-0134 §5). Everything here lives in the level's namespace
// (CRD_MATH_LEVEL_NS), the Lanczos core and the log and exp cores included. The portable bodies run the half-plane
// core inline and call the out-of-line scalar functions for the other inputs; the vector bodies run the core on four
// lanes (AVX2: crd_log4, crd_exp4) or two (SSE2, SSE4.2: crd_log_x2, crd_exp_x2), which reproduce crd_log1 and
// crd_exp1 lane for lane, and send the lanes outside the half-plane to the same scalar functions, so every level gives
// detail::lanczos_lgamma's and lanczos_tgamma's bits.

#include <crd/hesap/special/detail/lanczos_core.hpp>
#include <crd/hesap/special/special_kernels.hpp>
#include <crd/math/simd/transcendental.hpp>

namespace crd::hesap::special::inline CRD_MATH_LEVEL_NS
{
namespace
{

using detail::lanczos_in_half;
using detail::lanczos_lgamma_half;

void lgamma_scalar(crd::f64* out, const crd::f64* in, crd::usize n) noexcept
{
    for (crd::usize i = 0; i < n; ++i)
    {
        const crd::f64 x = in[i];
        out[i] = lanczos_in_half(x) ? lanczos_lgamma_half(x) : detail::lanczos_lgamma(x);
    }
}

void tgamma_scalar(crd::f64* out, const crd::f64* in, crd::usize n) noexcept
{
    for (crd::usize i = 0; i < n; ++i)
    {
        const crd::f64 x = in[i];
        out[i] = lanczos_in_half(x) ? crd::math::crd_exp1(lanczos_lgamma_half(x)) : detail::lanczos_tgamma(x);
    }
}

#if CRD_SIMD_HAS_SSE2
// The registers of this level's bodies: four lanes on AVX2 (crd_log4, crd_exp4), two on SSE2 and SSE4.2 (crd_log_x2,
// crd_exp_x2).
#if CRD_SIMD_HAS_AVX2
using V = __m256d;
constexpr int kLanes = 4;
V set1(crd::f64 x) noexcept
{
    return _mm256_set1_pd(x);
}
V add(V a, V b) noexcept
{
    return _mm256_add_pd(a, b);
}
V sub(V a, V b) noexcept
{
    return _mm256_sub_pd(a, b);
}
V mul(V a, V b) noexcept
{
    return _mm256_mul_pd(a, b);
}
V divide(V a, V b) noexcept
{
    return _mm256_div_pd(a, b);
}
V load(const crd::f64* p) noexcept
{
    return _mm256_loadu_pd(p);
}
void store(crd::f64* p, V v) noexcept
{
    _mm256_storeu_pd(p, v);
}
V log_lanes(V x) noexcept
{
    return crd::math::crd_log4(x);
}
V exp_lanes(V x) noexcept
{
    return crd::math::crd_exp4(x);
}
// The lanes inside the half-plane (0.5 <= x <= max finite), as a movemask.
int inside_half(V x) noexcept
{
    return _mm256_movemask_pd(_mm256_and_pd(_mm256_cmp_pd(x, set1(0.5), _CMP_GE_OQ),
                                            _mm256_cmp_pd(x, set1(1.7976931348623157e308), _CMP_LE_OQ)));
}
#else
using V = __m128d;
constexpr int kLanes = 2;
V set1(crd::f64 x) noexcept
{
    return _mm_set1_pd(x);
}
V add(V a, V b) noexcept
{
    return _mm_add_pd(a, b);
}
V sub(V a, V b) noexcept
{
    return _mm_sub_pd(a, b);
}
V mul(V a, V b) noexcept
{
    return _mm_mul_pd(a, b);
}
V divide(V a, V b) noexcept
{
    return _mm_div_pd(a, b);
}
V load(const crd::f64* p) noexcept
{
    return _mm_loadu_pd(p);
}
void store(crd::f64* p, V v) noexcept
{
    _mm_storeu_pd(p, v);
}
V log_lanes(V x) noexcept
{
    return crd::math::crd_log_x2(x);
}
V exp_lanes(V x) noexcept
{
    return crd::math::crd_exp_x2(x);
}
int inside_half(V x) noexcept
{
    return _mm_movemask_pd(_mm_and_pd(_mm_cmpge_pd(x, set1(0.5)), _mm_cmple_pd(x, set1(1.7976931348623157e308))));
}
#endif

// lanczos_lgamma_half on every lane, in its operation order.
V lgamma_half(V x) noexcept
{
    const V z = sub(x, set1(1.0));
    V a = set1(detail::kLanczosC[0]);
    for (int k = 1; k <= 8; ++k)
    {
        a = add(a, divide(set1(detail::kLanczosC[k]), add(z, set1(static_cast<double>(k)))));
    }
    const V t = add(z, set1(detail::kLanczosGph));
    V r = mul(add(z, set1(0.5)), log_lanes(t));
    r = add(set1(detail::kHalfLn2Pi), r);
    r = sub(r, t);
    return add(r, log_lanes(a));
}

// The lanes' results at out, the lanes outside the half-plane (x < 0.5, NaN, +inf) replaced by scalar(in[lane]).
void store_with_scalar_lanes(crd::f64* out, const crd::f64* in, V r, int inside,
                             crd::f64 (*scalar)(crd::f64) noexcept) noexcept
{
    store(out, r);
    for (int lane = 0; lane < kLanes; ++lane)
    {
        if (((inside >> lane) & 1) == 0)
        {
            out[lane] = scalar(in[lane]);
        }
    }
}

void lgamma_vector(crd::f64* out, const crd::f64* in, crd::usize n) noexcept
{
    crd::usize i = 0;
    for (; i + kLanes <= n; i += kLanes)
    {
        const V x = load(in + i);
        store_with_scalar_lanes(out + i, in + i, lgamma_half(x), inside_half(x), &detail::lanczos_lgamma);
    }
    lgamma_scalar(out + i, in + i, n - i);
}

void tgamma_vector(crd::f64* out, const crd::f64* in, crd::usize n) noexcept
{
    crd::usize i = 0;
    for (; i + kLanes <= n; i += kLanes)
    {
        const V x = load(in + i);
        store_with_scalar_lanes(out + i, in + i, exp_lanes(lgamma_half(x)), inside_half(x), &detail::lanczos_tgamma);
    }
    tgamma_scalar(out + i, in + i, n - i);
}
#endif

} // namespace

#if CRD_SIMD_HAS_SSE2
constexpr auto kLgamma = &lgamma_vector;
constexpr auto kTgamma = &tgamma_vector;
#else
constexpr auto kLgamma = &lgamma_scalar;
constexpr auto kTgamma = &tgamma_scalar;
#endif

} // namespace crd::hesap::special::inline CRD_MATH_LEVEL_NS

namespace crd::hesap::special
{

const SpecialKernels CRD_KERNEL_TABLE(kSpecialKernels) = {
    CRD_MATH_LEVEL_NS::kLgamma,
    CRD_MATH_LEVEL_NS::kTgamma,
};

} // namespace crd::hesap::special
