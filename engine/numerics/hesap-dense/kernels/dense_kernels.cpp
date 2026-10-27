// The hesap-dense kernels of crd/hesap/dense/dense_kernels.hpp, compiled once per CPU level by crd_add_cpu_kernels
// (../CMakeLists.txt, ADR-0134 §5). Everything here lives in the level's namespace (CRD_MATH_LEVEL_NS).
//
// The portable level runs the block on the two-rounding PortableTile. The sse2 level sums with the same two roundings
// in 128-bit registers, the f64 tile as two 6 by 4 halves (twelve accumulators, two B loads and one A broadcast a step:
// 15 of 16 XMM) and the f32 tile as two 4 by 8 halves (eight accumulators); it gives the portable level's bits. The
// avx2 level sums by single-rounded FMA: f64 6 by 8 in twelve Vec4d accumulators (15 of 16 YMM, about 98% of peak in
// L1), f32 8 by 8 in eight Vec8f. Every tile hands each finished row to a sink while its accumulators are still in
// registers: the product stores the tile, the fused path merges a full row-major tile straight into C with
// merge_element's expression. The sse2 and avx2 tiles prefetch the next B panel one line a step, which hides its
// re-stream from L3 at each panel boundary.

#include <crd/hesap/dense/detail/gemm_block.hpp>
#include <crd/math/simd/backend.hpp>

#if CRD_SIMD_HAS_AVX2
#include <crd/math/simd/vec4d.hpp>
#include <crd/math/simd/vec8f.hpp>
#endif

namespace crd::hesap::dense::detail::inline CRD_MATH_LEVEL_NS
{
namespace
{

#if CRD_SIMD_HAS_SSE2
CRD_FORCEINLINE void prefetch(const void* p) noexcept
{
    _mm_prefetch(static_cast<const char*>(p), _MM_HINT_T0);
}
#endif

#if CRD_SIMD_HAS_AVX2
namespace simd = crd::math::simd;

// One register of a full tile merged into C with merge_element's expression (gemm_block.hpp).
template <GemmMerge M, typename V, typename T> CRD_FORCEINLINE void merge_into(T* c, V p, V alpha) noexcept
{
    if constexpr (M == GemmMerge::Assign)
    {
        p.store(c);
    }
    else if constexpr (M == GemmMerge::Add)
    {
        (V::load(c) + p).store(c);
    }
    else if constexpr (M == GemmMerge::Subtract)
    {
        (V::load(c) - p).store(c);
    }
    else
    {
        (V::load(c) + alpha * p).store(c);
    }
}

// The 6 by 8 f64 tile: one A broadcast register reused per row keeps the live set at 12 accumulators, 2 B halves and
// 1 A. sink(i, lo, hi) receives row i.
template <typename Sink>
CRD_FORCEINLINE void f64_tile(crd::usize depth, crd::usize lda, const crd::f64* a, const crd::f64* b,
                              Sink sink) noexcept
{
    simd::Vec4d c00(0.0);
    simd::Vec4d c01(0.0);
    simd::Vec4d c10(0.0);
    simd::Vec4d c11(0.0);
    simd::Vec4d c20(0.0);
    simd::Vec4d c21(0.0);
    simd::Vec4d c30(0.0);
    simd::Vec4d c31(0.0);
    simd::Vec4d c40(0.0);
    simd::Vec4d c41(0.0);
    simd::Vec4d c50(0.0);
    simd::Vec4d c51(0.0);
    for (crd::usize p = 0; p < depth; ++p)
    {
        const simd::Vec4d b0 = simd::Vec4d::load(b + p * 8U);
        const simd::Vec4d b1 = simd::Vec4d::load(b + p * 8U + 4U);
        prefetch(b + (depth + p) * 8U); // line p of the next B panel; past the last panel it is a no-op hint
        simd::Vec4d ai(a[0 * lda + p]);
        c00 = simd::fma(ai, b0, c00);
        c01 = simd::fma(ai, b1, c01);
        ai = simd::Vec4d(a[1 * lda + p]);
        c10 = simd::fma(ai, b0, c10);
        c11 = simd::fma(ai, b1, c11);
        ai = simd::Vec4d(a[2 * lda + p]);
        c20 = simd::fma(ai, b0, c20);
        c21 = simd::fma(ai, b1, c21);
        ai = simd::Vec4d(a[3 * lda + p]);
        c30 = simd::fma(ai, b0, c30);
        c31 = simd::fma(ai, b1, c31);
        ai = simd::Vec4d(a[4 * lda + p]);
        c40 = simd::fma(ai, b0, c40);
        c41 = simd::fma(ai, b1, c41);
        ai = simd::Vec4d(a[5 * lda + p]);
        c50 = simd::fma(ai, b0, c50);
        c51 = simd::fma(ai, b1, c51);
    }
    sink(0, c00, c01);
    sink(1, c10, c11);
    sink(2, c20, c21);
    sink(3, c30, c31);
    sink(4, c40, c41);
    sink(5, c50, c51);
}

struct Avx2F64Tile
{
    static constexpr bool kFused = true;

    static void product(crd::usize depth, crd::usize lda, const crd::f64* a, const crd::f64* b, crd::f64* out) noexcept
    {
        f64_tile(depth, lda, a, b,
                 [out](crd::usize i, simd::Vec4d lo, simd::Vec4d hi)
                 {
                     lo.store(out + i * 8U);
                     hi.store(out + i * 8U + 4U);
                 });
    }

    template <GemmMerge M>
    static void fused(crd::usize depth, crd::usize lda, const crd::f64* a, const crd::f64* b, crd::f64* c,
                      crd::usize ldc, crd::f64 alpha) noexcept
    {
        for (crd::usize i = 0; i < 6U; ++i) // C is read after the depth loop: request its lines now
        {
            prefetch(c + i * ldc);
            prefetch(c + i * ldc + 4U);
        }
        const simd::Vec4d av(alpha);
        f64_tile(depth, lda, a, b,
                 [c, ldc, av](crd::usize i, simd::Vec4d lo, simd::Vec4d hi)
                 {
                     merge_into<M>(c + i * ldc, lo, av);
                     merge_into<M>(c + i * ldc + 4U, hi, av);
                 });
    }
};

// The 8 by 8 f32 tile, one Vec8f accumulator a row. sink(i, row) receives row i.
template <typename Sink>
CRD_FORCEINLINE void f32_tile(crd::usize depth, crd::usize lda, const crd::f32* a, const crd::f32* b,
                              Sink sink) noexcept
{
    simd::Vec8f c0(0.0F);
    simd::Vec8f c1(0.0F);
    simd::Vec8f c2(0.0F);
    simd::Vec8f c3(0.0F);
    simd::Vec8f c4(0.0F);
    simd::Vec8f c5(0.0F);
    simd::Vec8f c6(0.0F);
    simd::Vec8f c7(0.0F);
    for (crd::usize p = 0; p < depth; ++p)
    {
        const simd::Vec8f bp = simd::Vec8f::load(b + p * 8U);
        prefetch(b + (depth + p) * 8U);
        c0 = simd::fma(simd::Vec8f(a[0 * lda + p]), bp, c0);
        c1 = simd::fma(simd::Vec8f(a[1 * lda + p]), bp, c1);
        c2 = simd::fma(simd::Vec8f(a[2 * lda + p]), bp, c2);
        c3 = simd::fma(simd::Vec8f(a[3 * lda + p]), bp, c3);
        c4 = simd::fma(simd::Vec8f(a[4 * lda + p]), bp, c4);
        c5 = simd::fma(simd::Vec8f(a[5 * lda + p]), bp, c5);
        c6 = simd::fma(simd::Vec8f(a[6 * lda + p]), bp, c6);
        c7 = simd::fma(simd::Vec8f(a[7 * lda + p]), bp, c7);
    }
    sink(0, c0);
    sink(1, c1);
    sink(2, c2);
    sink(3, c3);
    sink(4, c4);
    sink(5, c5);
    sink(6, c6);
    sink(7, c7);
}

struct Avx2F32Tile
{
    static constexpr bool kFused = true;

    static void product(crd::usize depth, crd::usize lda, const crd::f32* a, const crd::f32* b, crd::f32* out) noexcept
    {
        f32_tile(depth, lda, a, b, [out](crd::usize i, simd::Vec8f row) { row.store(out + i * 8U); });
    }

    template <GemmMerge M>
    static void fused(crd::usize depth, crd::usize lda, const crd::f32* a, const crd::f32* b, crd::f32* c,
                      crd::usize ldc, crd::f32 alpha) noexcept
    {
        for (crd::usize i = 0; i < 8U; ++i)
        {
            prefetch(c + i * ldc);
        }
        const simd::Vec8f av(alpha);
        f32_tile(depth, lda, a, b,
                 [c, ldc, av](crd::usize i, simd::Vec8f row) { merge_into<M>(c + i * ldc, row, av); });
    }
};

using F64Tile = Avx2F64Tile;
using F32Tile = Avx2F32Tile;
#elif CRD_SIMD_HAS_SSE2
// One register of a full tile merged into C with merge_element's expression (gemm_block.hpp).
template <GemmMerge M> CRD_FORCEINLINE void merge_into(crd::f64* c, __m128d p, __m128d alpha) noexcept
{
    if constexpr (M == GemmMerge::Assign)
    {
        _mm_storeu_pd(c, p);
    }
    else if constexpr (M == GemmMerge::Add)
    {
        _mm_storeu_pd(c, _mm_add_pd(_mm_loadu_pd(c), p));
    }
    else if constexpr (M == GemmMerge::Subtract)
    {
        _mm_storeu_pd(c, _mm_sub_pd(_mm_loadu_pd(c), p));
    }
    else
    {
        _mm_storeu_pd(c, _mm_add_pd(_mm_loadu_pd(c), _mm_mul_pd(alpha, p)));
    }
}

template <GemmMerge M> CRD_FORCEINLINE void merge_into(crd::f32* c, __m128 p, __m128 alpha) noexcept
{
    if constexpr (M == GemmMerge::Assign)
    {
        _mm_storeu_ps(c, p);
    }
    else if constexpr (M == GemmMerge::Add)
    {
        _mm_storeu_ps(c, _mm_add_ps(_mm_loadu_ps(c), p));
    }
    else if constexpr (M == GemmMerge::Subtract)
    {
        _mm_storeu_ps(c, _mm_sub_ps(_mm_loadu_ps(c), p));
    }
    else
    {
        _mm_storeu_ps(c, _mm_add_ps(_mm_loadu_ps(c), _mm_mul_ps(alpha, p)));
    }
}

// acc + a * b, rounded twice, as PortableTile sums.
CRD_FORCEINLINE __m128d step(__m128d acc, __m128d a, __m128d b) noexcept
{
    return _mm_add_pd(acc, _mm_mul_pd(a, b));
}

CRD_FORCEINLINE __m128 step(__m128 acc, __m128 a, __m128 b) noexcept
{
    return _mm_add_ps(acc, _mm_mul_ps(a, b));
}

// Columns [4 half, 4 half + 4) of the 6 by 8 f64 tile. sink(i, lo, hi) receives row i of the half.
template <typename Sink>
CRD_FORCEINLINE void f64_half(crd::usize depth, crd::usize lda, const crd::f64* a, const crd::f64* b, crd::usize half,
                              Sink sink) noexcept
{
    __m128d c00 = _mm_setzero_pd();
    __m128d c01 = _mm_setzero_pd();
    __m128d c10 = _mm_setzero_pd();
    __m128d c11 = _mm_setzero_pd();
    __m128d c20 = _mm_setzero_pd();
    __m128d c21 = _mm_setzero_pd();
    __m128d c30 = _mm_setzero_pd();
    __m128d c31 = _mm_setzero_pd();
    __m128d c40 = _mm_setzero_pd();
    __m128d c41 = _mm_setzero_pd();
    __m128d c50 = _mm_setzero_pd();
    __m128d c51 = _mm_setzero_pd();
    const crd::f64* bh = b + half * 4U;
    for (crd::usize p = 0; p < depth; ++p)
    {
        const __m128d b0 = _mm_loadu_pd(bh + p * 8U);
        const __m128d b1 = _mm_loadu_pd(bh + p * 8U + 2U);
        prefetch(b + (depth + p) * 8U); // line p of the next B panel
        __m128d ai = _mm_set1_pd(a[0 * lda + p]);
        c00 = step(c00, ai, b0);
        c01 = step(c01, ai, b1);
        ai = _mm_set1_pd(a[1 * lda + p]);
        c10 = step(c10, ai, b0);
        c11 = step(c11, ai, b1);
        ai = _mm_set1_pd(a[2 * lda + p]);
        c20 = step(c20, ai, b0);
        c21 = step(c21, ai, b1);
        ai = _mm_set1_pd(a[3 * lda + p]);
        c30 = step(c30, ai, b0);
        c31 = step(c31, ai, b1);
        ai = _mm_set1_pd(a[4 * lda + p]);
        c40 = step(c40, ai, b0);
        c41 = step(c41, ai, b1);
        ai = _mm_set1_pd(a[5 * lda + p]);
        c50 = step(c50, ai, b0);
        c51 = step(c51, ai, b1);
    }
    sink(0, c00, c01);
    sink(1, c10, c11);
    sink(2, c20, c21);
    sink(3, c30, c31);
    sink(4, c40, c41);
    sink(5, c50, c51);
}

struct Sse2F64Tile
{
    static constexpr bool kFused = true;

    static void product(crd::usize depth, crd::usize lda, const crd::f64* a, const crd::f64* b, crd::f64* out) noexcept
    {
        for (crd::usize half = 0; half < 2U; ++half)
        {
            crd::f64* o = out + half * 4U;
            f64_half(depth, lda, a, b, half,
                     [o](crd::usize i, __m128d lo, __m128d hi)
                     {
                         _mm_storeu_pd(o + i * 8U, lo);
                         _mm_storeu_pd(o + i * 8U + 2U, hi);
                     });
        }
    }

    template <GemmMerge M>
    static void fused(crd::usize depth, crd::usize lda, const crd::f64* a, const crd::f64* b, crd::f64* c,
                      crd::usize ldc, crd::f64 alpha) noexcept
    {
        const __m128d av = _mm_set1_pd(alpha);
        for (crd::usize half = 0; half < 2U; ++half)
        {
            crd::f64* ch = c + half * 4U;
            f64_half(depth, lda, a, b, half,
                     [ch, ldc, av](crd::usize i, __m128d lo, __m128d hi)
                     {
                         merge_into<M>(ch + i * ldc, lo, av);
                         merge_into<M>(ch + i * ldc + 2U, hi, av);
                     });
        }
    }
};

// Rows [4 half, 4 half + 4) of the 8 by 8 f32 tile. sink(i, lo, hi) receives row i of the half.
template <typename Sink>
CRD_FORCEINLINE void f32_half(crd::usize depth, crd::usize lda, const crd::f32* a, const crd::f32* b, crd::usize half,
                              Sink sink) noexcept
{
    __m128 c00 = _mm_setzero_ps();
    __m128 c01 = _mm_setzero_ps();
    __m128 c10 = _mm_setzero_ps();
    __m128 c11 = _mm_setzero_ps();
    __m128 c20 = _mm_setzero_ps();
    __m128 c21 = _mm_setzero_ps();
    __m128 c30 = _mm_setzero_ps();
    __m128 c31 = _mm_setzero_ps();
    const crd::f32* ah = a + half * 4U * lda;
    for (crd::usize p = 0; p < depth; ++p)
    {
        const __m128 b0 = _mm_loadu_ps(b + p * 8U);
        const __m128 b1 = _mm_loadu_ps(b + p * 8U + 4U);
        prefetch(b + (depth + p) * 8U);
        __m128 ai = _mm_set1_ps(ah[0 * lda + p]);
        c00 = step(c00, ai, b0);
        c01 = step(c01, ai, b1);
        ai = _mm_set1_ps(ah[1 * lda + p]);
        c10 = step(c10, ai, b0);
        c11 = step(c11, ai, b1);
        ai = _mm_set1_ps(ah[2 * lda + p]);
        c20 = step(c20, ai, b0);
        c21 = step(c21, ai, b1);
        ai = _mm_set1_ps(ah[3 * lda + p]);
        c30 = step(c30, ai, b0);
        c31 = step(c31, ai, b1);
    }
    sink(0, c00, c01);
    sink(1, c10, c11);
    sink(2, c20, c21);
    sink(3, c30, c31);
}

struct Sse2F32Tile
{
    static constexpr bool kFused = true;

    static void product(crd::usize depth, crd::usize lda, const crd::f32* a, const crd::f32* b, crd::f32* out) noexcept
    {
        for (crd::usize half = 0; half < 2U; ++half)
        {
            crd::f32* o = out + half * 4U * 8U;
            f32_half(depth, lda, a, b, half,
                     [o](crd::usize i, __m128 lo, __m128 hi)
                     {
                         _mm_storeu_ps(o + i * 8U, lo);
                         _mm_storeu_ps(o + i * 8U + 4U, hi);
                     });
        }
    }

    template <GemmMerge M>
    static void fused(crd::usize depth, crd::usize lda, const crd::f32* a, const crd::f32* b, crd::f32* c,
                      crd::usize ldc, crd::f32 alpha) noexcept
    {
        const __m128 av = _mm_set1_ps(alpha);
        for (crd::usize half = 0; half < 2U; ++half)
        {
            crd::f32* ch = c + half * 4U * ldc;
            f32_half(depth, lda, a, b, half,
                     [ch, ldc, av](crd::usize i, __m128 lo, __m128 hi)
                     {
                         merge_into<M>(ch + i * ldc, lo, av);
                         merge_into<M>(ch + i * ldc + 4U, hi, av);
                     });
        }
    }
};

using F64Tile = Sse2F64Tile;
using F32Tile = Sse2F32Tile;
#else
using F64Tile = PortableTile<crd::f64>;
using F32Tile = PortableTile<crd::f32>;
#endif

} // namespace

constexpr auto kGemmBlockF64 = &gemm_block<F64Tile, crd::f64>;
constexpr auto kGemmBlockF32 = &gemm_block<F32Tile, crd::f32>;

} // namespace crd::hesap::dense::detail::inline CRD_MATH_LEVEL_NS

namespace crd::hesap::dense
{

const DenseKernels CRD_KERNEL_TABLE(kDenseKernels) = {
    detail::CRD_MATH_LEVEL_NS::kGemmBlockF64,
    detail::CRD_MATH_LEVEL_NS::kGemmBlockF32,
};

} // namespace crd::hesap::dense
