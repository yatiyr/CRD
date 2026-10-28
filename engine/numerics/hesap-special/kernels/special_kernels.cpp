// The hesap-special kernels of crd/hesap/special/special_kernels.hpp, compiled once per CPU level by
// crd_add_cpu_kernels (../CMakeLists.txt, ADR-0134 §5). Everything here lives in the level's namespace
// (CRD_MATH_LEVEL_NS), the Lanczos core and the log and exp cores included. The portable bodies run the half-plane
// core inline and call the out-of-line scalar functions for the other inputs; the vector bodies run the core on eight
// lanes (AVX-512: crd_log8, crd_exp8, then one four-lane register for the rest), four (AVX2: crd_log4, crd_exp4) or
// two (SSE2, SSE4.2: crd_log_x2, crd_exp_x2), which reproduce crd_log1 and crd_exp1 lane for lane, and send the lanes
// outside the half-plane to the same scalar functions, so every level gives detail::lanczos_lgamma's and
// lanczos_tgamma's bits.

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
// One register width's lanes: eight on AVX-512 (crd_log8, crd_exp8), four on AVX2 (crd_log4, crd_exp4), two on SSE2
// and SSE4.2 (crd_log_x2, crd_exp_x2).
#if CRD_SIMD_HAS_AVX512
struct Lanes8
{
    using V = __m512d;
    static constexpr int kLanes = 8;
    static V set1(crd::f64 x) noexcept { return _mm512_set1_pd(x); }
    static V add(V a, V b) noexcept { return _mm512_add_pd(a, b); }
    static V sub(V a, V b) noexcept { return _mm512_sub_pd(a, b); }
    static V mul(V a, V b) noexcept { return _mm512_mul_pd(a, b); }
    static V divide(V a, V b) noexcept { return _mm512_div_pd(a, b); }
    static V load(const crd::f64* p) noexcept { return _mm512_loadu_pd(p); }
    static void store(crd::f64* p, V v) noexcept { _mm512_storeu_pd(p, v); }
    static V log_lanes(V x) noexcept { return crd::math::crd_log8(x); }
    static V exp_lanes(V x) noexcept { return crd::math::crd_exp8(x); }
    // The lanes inside the half-plane (0.5 <= x <= max finite), one bit a lane.
    static int inside_half(V x) noexcept
    {
        return static_cast<int>(_mm512_cmp_pd_mask(x, set1(0.5), _CMP_GE_OQ) &
                                _mm512_cmp_pd_mask(x, set1(1.7976931348623157e308), _CMP_LE_OQ));
    }
};
#endif

#if CRD_SIMD_HAS_AVX2
struct Lanes4
{
    using V = __m256d;
    static constexpr int kLanes = 4;
    static V set1(crd::f64 x) noexcept { return _mm256_set1_pd(x); }
    static V add(V a, V b) noexcept { return _mm256_add_pd(a, b); }
    static V sub(V a, V b) noexcept { return _mm256_sub_pd(a, b); }
    static V mul(V a, V b) noexcept { return _mm256_mul_pd(a, b); }
    static V divide(V a, V b) noexcept { return _mm256_div_pd(a, b); }
    static V load(const crd::f64* p) noexcept { return _mm256_loadu_pd(p); }
    static void store(crd::f64* p, V v) noexcept { _mm256_storeu_pd(p, v); }
    static V log_lanes(V x) noexcept { return crd::math::crd_log4(x); }
    static V exp_lanes(V x) noexcept { return crd::math::crd_exp4(x); }
    static int inside_half(V x) noexcept
    {
        return _mm256_movemask_pd(_mm256_and_pd(_mm256_cmp_pd(x, set1(0.5), _CMP_GE_OQ),
                                                _mm256_cmp_pd(x, set1(1.7976931348623157e308), _CMP_LE_OQ)));
    }
};
using Narrow = Lanes4;
#else
struct Lanes2
{
    using V = __m128d;
    static constexpr int kLanes = 2;
    static V set1(crd::f64 x) noexcept { return _mm_set1_pd(x); }
    static V add(V a, V b) noexcept { return _mm_add_pd(a, b); }
    static V sub(V a, V b) noexcept { return _mm_sub_pd(a, b); }
    static V mul(V a, V b) noexcept { return _mm_mul_pd(a, b); }
    static V divide(V a, V b) noexcept { return _mm_div_pd(a, b); }
    static V load(const crd::f64* p) noexcept { return _mm_loadu_pd(p); }
    static void store(crd::f64* p, V v) noexcept { _mm_storeu_pd(p, v); }
    static V log_lanes(V x) noexcept { return crd::math::crd_log_x2(x); }
    static V exp_lanes(V x) noexcept { return crd::math::crd_exp_x2(x); }
    static int inside_half(V x) noexcept
    {
        return _mm_movemask_pd(_mm_and_pd(_mm_cmpge_pd(x, set1(0.5)), _mm_cmple_pd(x, set1(1.7976931348623157e308))));
    }
};
using Narrow = Lanes2;
#endif

// lanczos_lgamma_half on every lane, in its operation order.
template <class L> typename L::V lgamma_half(typename L::V x) noexcept
{
    using V = typename L::V;
    const V z = L::sub(x, L::set1(1.0));
    V a = L::set1(detail::kLanczosC[0]);
    for (int k = 1; k <= 8; ++k)
    {
        a = L::add(a, L::divide(L::set1(detail::kLanczosC[k]), L::add(z, L::set1(static_cast<double>(k)))));
    }
    const V t = L::add(z, L::set1(detail::kLanczosGph));
    V r = L::mul(L::add(z, L::set1(0.5)), L::log_lanes(t));
    r = L::add(L::set1(detail::kHalfLn2Pi), r);
    r = L::sub(r, t);
    return L::add(r, L::log_lanes(a));
}

// The lanes' results at out, the lanes outside the half-plane (x < 0.5, NaN, +inf) replaced by scalar(in[lane]).
template <class L>
void store_with_scalar_lanes(crd::f64* out, const crd::f64* in, typename L::V r, int inside,
                             crd::f64 (*scalar)(crd::f64) noexcept) noexcept
{
    L::store(out, r);
    for (int lane = 0; lane < L::kLanes; ++lane)
    {
        if (((inside >> lane) & 1) == 0)
        {
            out[lane] = scalar(in[lane]);
        }
    }
}

// Whole registers of L from element i on: lgamma, or tgamma (exp of it) when Exp. Returns the first element left.
template <class L, bool Exp>
crd::usize gamma_lanes(crd::f64* out, const crd::f64* in, crd::usize i, crd::usize n) noexcept
{
    for (; i + L::kLanes <= n; i += L::kLanes)
    {
        const typename L::V x = L::load(in + i);
        if constexpr (Exp)
        {
            store_with_scalar_lanes<L>(out + i, in + i, L::exp_lanes(lgamma_half<L>(x)), L::inside_half(x),
                                       &detail::lanczos_tgamma);
        }
        else
        {
            store_with_scalar_lanes<L>(out + i, in + i, lgamma_half<L>(x), L::inside_half(x), &detail::lanczos_lgamma);
        }
    }
    return i;
}

// The widest registers first, then the narrow one for what is left (one register at most), then the scalar loop.
template <bool Exp> crd::usize gamma_vector(crd::f64* out, const crd::f64* in, crd::usize n) noexcept
{
    crd::usize i = 0;
#if CRD_SIMD_HAS_AVX512
    i = gamma_lanes<Lanes8, Exp>(out, in, i, n);
#endif
    return gamma_lanes<Narrow, Exp>(out, in, i, n);
}

void lgamma_vector(crd::f64* out, const crd::f64* in, crd::usize n) noexcept
{
    const crd::usize i = gamma_vector<false>(out, in, n);
    lgamma_scalar(out + i, in + i, n - i);
}

void tgamma_vector(crd::f64* out, const crd::f64* in, crd::usize n) noexcept
{
    const crd::usize i = gamma_vector<true>(out, in, n);
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
