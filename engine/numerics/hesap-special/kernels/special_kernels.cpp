// The hesap-special kernels of crd/hesap/special/special_kernels.hpp, compiled once per CPU level by
// crd_add_cpu_kernels (../CMakeLists.txt, ADR-0134 §5). Everything here lives in the level's namespace
// (CRD_MATH_LEVEL_NS), the Lanczos core and the log and exp cores included. The portable bodies run the half-plane
// core inline and call the out-of-line scalar functions for the other inputs; the AVX2 bodies run four lanes of the
// core (crd_log4 and crd_exp4 reproduce crd_log1 and crd_exp1 lane for lane) and send the lanes outside the
// half-plane to the same scalar functions, so every level gives detail::lanczos_lgamma's and lanczos_tgamma's bits.

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

#if CRD_SIMD_HAS_AVX2
// lanczos_lgamma_half on four lanes, in its operation order.
__m256d lgamma_half4(__m256d x) noexcept
{
    const __m256d z = _mm256_sub_pd(x, _mm256_set1_pd(1.0));
    __m256d a = _mm256_set1_pd(detail::kLanczosC[0]);
    for (int k = 1; k <= 8; ++k)
    {
        const __m256d denom = _mm256_add_pd(z, _mm256_set1_pd(static_cast<double>(k)));
        a = _mm256_add_pd(a, _mm256_div_pd(_mm256_set1_pd(detail::kLanczosC[k]), denom));
    }
    const __m256d t = _mm256_add_pd(z, _mm256_set1_pd(detail::kLanczosGph));
    __m256d r = _mm256_mul_pd(_mm256_add_pd(z, _mm256_set1_pd(0.5)), crd::math::crd_log4(t));
    r = _mm256_add_pd(_mm256_set1_pd(detail::kHalfLn2Pi), r);
    r = _mm256_sub_pd(r, t);
    return _mm256_add_pd(r, crd::math::crd_log4(a));
}

// The lanes outside the half-plane (x < 0.5, NaN, +inf), as a movemask.
int outside_half4(__m256d x) noexcept
{
    const __m256d inside = _mm256_and_pd(_mm256_cmp_pd(x, _mm256_set1_pd(0.5), _CMP_GE_OQ),
                                         _mm256_cmp_pd(x, _mm256_set1_pd(1.7976931348623157e308), _CMP_LE_OQ));
    return _mm256_movemask_pd(inside) ^ 0xF;
}

// Four results at out, the lanes of mask replaced by scalar(in[lane]).
void store_with_scalar_lanes(crd::f64* out, const crd::f64* in, __m256d r, int mask,
                             crd::f64 (*scalar)(crd::f64) noexcept) noexcept
{
    _mm256_storeu_pd(out, r);
    for (int lane = 0; lane < 4; ++lane)
    {
        if (((mask >> lane) & 1) != 0)
        {
            out[lane] = scalar(in[lane]);
        }
    }
}

void lgamma_avx2(crd::f64* out, const crd::f64* in, crd::usize n) noexcept
{
    crd::usize i = 0;
    for (; i + 4U <= n; i += 4U)
    {
        const __m256d x = _mm256_loadu_pd(in + i);
        store_with_scalar_lanes(out + i, in + i, lgamma_half4(x), outside_half4(x), &detail::lanczos_lgamma);
    }
    lgamma_scalar(out + i, in + i, n - i);
}

void tgamma_avx2(crd::f64* out, const crd::f64* in, crd::usize n) noexcept
{
    crd::usize i = 0;
    for (; i + 4U <= n; i += 4U)
    {
        const __m256d x = _mm256_loadu_pd(in + i);
        store_with_scalar_lanes(out + i, in + i, crd::math::crd_exp4(lgamma_half4(x)), outside_half4(x),
                                &detail::lanczos_tgamma);
    }
    tgamma_scalar(out + i, in + i, n - i);
}
#endif

} // namespace

#if CRD_SIMD_HAS_AVX2
constexpr auto kLgamma = &lgamma_avx2;
constexpr auto kTgamma = &tgamma_avx2;
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
