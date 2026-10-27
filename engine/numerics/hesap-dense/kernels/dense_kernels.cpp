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

//
// The vector and matrix-vector kernels are written once over Vec4d and Vec8f, which every level backs with its own
// registers (one YMM from avx2, two XMM on sse2, arrays on portable); dense_kernels.hpp declares their reduction
// shapes. A level fixes only the accumulator and row counts (enough independent chains for its adder or FMA latency in
// its sixteen registers) and whether a step fuses.

#include <crd/hesap/dense/detail/gemm_block.hpp>
#include <crd/math/libm.hpp>
#include <crd/math/simd/backend.hpp>
#include <crd/math/simd/vec4d.hpp>
#include <crd/math/simd/vec8f.hpp>

namespace crd::hesap::dense::detail::inline CRD_MATH_LEVEL_NS
{
namespace
{

namespace simd = crd::math::simd;

#if CRD_SIMD_HAS_SSE2
CRD_FORCEINLINE void prefetch(const void* p) noexcept
{
    _mm_prefetch(static_cast<const char*>(p), _MM_HINT_T0);
}
#endif

#if CRD_SIMD_HAS_AVX2

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

// ---- the vector and matrix-vector kernels ----

// Independent chains for the adder (below avx2) or the FMA (from avx2), each four cycles deep on two ports: eight
// registers. Below avx2 a vector is two XMM registers, so half as many vectors.
#if CRD_SIMD_HAS_AVX2
constexpr crd::usize kDotAccumulators = 8;
constexpr crd::usize kGemvRows = 8;
#else
constexpr crd::usize kDotAccumulators = 4;
constexpr crd::usize kGemvRows = 4;
#endif

// gemv and symv prefetch the next rows into L2 once a row outgrows the hardware prefetcher's lead (at 64 elements the
// prefetches cost 6% and gain nothing).
constexpr crd::usize kPrefetchColumns = 512;

template <typename T> struct VectorOf
{
    using Type = simd::Vec4d;
    static constexpr crd::usize kWidth = 4;
};
template <> struct VectorOf<crd::f32>
{
    using Type = simd::Vec8f;
    static constexpr crd::usize kWidth = 8;
};

CRD_FORCEINLINE void prefetch_l2([[maybe_unused]] const void* p) noexcept
{
#if CRD_SIMD_HAS_SSE2
    _mm_prefetch(static_cast<const char*>(p), _MM_HINT_T1);
#endif
}

template <typename V> CRD_FORCEINLINE V madd(V a, V b, V c) noexcept
{
#if CRD_SIMD_HAS_AVX2
    return simd::fma(a, b, c);
#else
    return simd::mul_add(a, b, c);
#endif
}

template <crd::usize Q> struct Index
{
    static constexpr crd::usize kValue = Q;
};

// f(Index<Q>{}) for Q = 0 .. N - 1: every register array index is a constant, so the arrays stay in registers.
template <crd::usize N, crd::usize Q = 0, typename F> CRD_FORCEINLINE void unroll(F& f) noexcept
{
    if constexpr (Q < N)
    {
        f(Index<Q>{});
        unroll<N, Q + 1>(f);
    }
}

// The first `count` values (1 to w) of p as a vector, and the store of its first `count` lanes.
template <typename V, typename T> CRD_FORCEINLINE V load_n(const T* p, crd::usize count) noexcept
{
    return count == VectorOf<T>::kWidth ? V::load(p) : V::load_partial(p, count);
}

template <typename V, typename T> CRD_FORCEINLINE void store_n(V v, T* p, crd::usize count) noexcept
{
    if (count == VectorOf<T>::kWidth)
    {
        v.store(p);
    }
    else
    {
        v.store_partial(p, count);
    }
}

// f(k, count) for each whole vector of n elements (count w) and the partial one that ends them.
template <typename T, typename F> CRD_FORCEINLINE void for_each_vector(crd::usize n, F f) noexcept
{
    constexpr crd::usize w = VectorOf<T>::kWidth;
    crd::usize k = 0;
    for (; k + w <= n; k += w)
    {
        f(k, w);
    }
    if (k < n)
    {
        f(k, n - k);
    }
}

// y + alpha x as the level's step.
template <typename T> CRD_FORCEINLINE void axpy(T* y, const T* x, T alpha, crd::usize n) noexcept
{
    using V = typename VectorOf<T>::Type;
    // The lambdas capture scalars: a captured vector's alignment pads the closure (C4324).
    for_each_vector<T>(n, [y, x, alpha](crd::usize k, crd::usize count)
                       { store_n(madd(V(alpha), load_n<V>(x + k, count), load_n<V>(y + k, count)), y + k, count); });
}

// The dot product of a and x over n elements in the shape dense_kernels.hpp declares. on_vector(k, a_vector) and
// on_element(k) see every whole vector and every remaining element of a (symv's symmetric update).
template <typename T, typename OnVector, typename OnElement>
CRD_FORCEINLINE T dot_walk(const T* a, const T* x, crd::usize n, OnVector on_vector, OnElement on_element) noexcept
{
    using V = typename VectorOf<T>::Type;
    constexpr crd::usize w = VectorOf<T>::kWidth;
    V acc[kDotAccumulators];
    auto zero = [&](auto q)
    {
        acc[decltype(q)::kValue] = V::zero();
    };
    unroll<kDotAccumulators>(zero);
    crd::usize k = 0;
    for (; k + kDotAccumulators * w <= n; k += kDotAccumulators * w)
    {
        auto step = [&](auto q)
        {
            constexpr crd::usize at = decltype(q)::kValue * w;
            const V av = V::load(a + k + at);
            acc[decltype(q)::kValue] = madd(av, V::load(x + k + at), acc[decltype(q)::kValue]);
            on_vector(k + at, av);
        };
        unroll<kDotAccumulators>(step);
    }
    // Fewer than kDotAccumulators whole vectors remain; they continue the rotation from accumulator 0.
    bool more = true;
    auto rest = [&](auto q)
    {
        more = more && k + w <= n;
        if (more)
        {
            const V av = V::load(a + k);
            acc[decltype(q)::kValue] = madd(av, V::load(x + k), acc[decltype(q)::kValue]);
            on_vector(k, av);
            k += w;
        }
    };
    unroll<kDotAccumulators - 1>(rest);
#if CRD_SIMD_HAS_AVX2
    const V total = ((acc[0] + acc[1]) + (acc[2] + acc[3])) + ((acc[4] + acc[5]) + (acc[6] + acc[7]));
#else
    const V total = (acc[0] + acc[1]) + (acc[2] + acc[3]);
#endif
    T s = simd::horizontal_sum(total);
    for (; k < n; ++k)
    {
        s += a[k] * x[k];
        on_element(k);
    }
    return s;
}

template <typename T> CRD_FORCEINLINE T dot(const T* a, const T* x, crd::usize n) noexcept
{
    return dot_walk(a, x, n, [](crd::usize, typename VectorOf<T>::Type) {}, [](crd::usize) {});
}

template <typename T> T dot_kernel(const T* x, const T* y, crd::usize n) noexcept
{
    return dot(x, y, n);
}

// Rows [i, i + R) of a gemv, R independent accumulators sharing each x vector.
template <crd::usize R, typename T> CRD_FORCEINLINE void gemv_rows(const GemvRows<T>& g, crd::usize i) noexcept
{
    using V = typename VectorOf<T>::Type;
    constexpr crd::usize w = VectorOf<T>::kWidth;
    const T* a = g.a + i * g.lda;
    if (g.cols > kPrefetchColumns && i + 2U * R <= g.rows)
    {
        auto ahead = [&](auto q)
        {
            prefetch_l2(a + (R + decltype(q)::kValue) * g.lda);
        };
        unroll<R>(ahead);
    }
    V acc[R];
    auto zero = [&](auto q)
    {
        acc[decltype(q)::kValue] = V::zero();
    };
    unroll<R>(zero);
    crd::usize k = 0;
    for (; k + w <= g.cols; k += w)
    {
        const V xk = V::load(g.x + k);
        auto step = [&](auto q)
        {
            constexpr crd::usize r = decltype(q)::kValue;
            acc[r] = madd(V::load(a + r * g.lda + k), xk, acc[r]);
        };
        unroll<R>(step);
    }
    // The registers reduce first: a lambda that also ran the column tail stayed out of line under MSVC, which then
    // kept acc in memory and stored every accumulator on every step.
    T s[R];
    auto reduce = [&](auto q)
    {
        s[decltype(q)::kValue] = simd::horizontal_sum(acc[decltype(q)::kValue]);
    };
    unroll<R>(reduce);
    for (crd::usize r = 0; r < R; ++r)
    {
        const T* row = a + r * g.lda;
        for (crd::usize c = k; c < g.cols; ++c)
        {
            s[r] += row[c] * g.x[c];
        }
        g.y[i + r] = g.alpha * s[r] + g.beta * g.y[i + r];
    }
}

// The kernels copy their view: a store through y could otherwise change alpha and beta in the compiler's view and
// reload every field after each one.
template <typename T> void gemv_kernel(const GemvRows<T>& view) noexcept
{
    const GemvRows<T> g = view;
    crd::usize i = 0;
    for (; i + kGemvRows <= g.rows; i += kGemvRows)
    {
        gemv_rows<kGemvRows>(g, i);
    }
    for (; i < g.rows; ++i)
    {
        gemv_rows<1>(g, i);
    }
}

template <typename T> void symv_kernel(const SymvLower<T>& view) noexcept
{
    using V = typename VectorOf<T>::Type;
    const SymvLower<T> s = view;
    for (crd::usize i = 0; i < s.n; ++i)
    {
        s.y[i] = s.beta * s.y[i];
    }
    const bool ahead = s.n > kPrefetchColumns;
    for (crd::usize i = 0; i < s.n; ++i)
    {
        const T* row = s.a + i * s.lda;
        const T* next = row + s.lda;
        const bool has_next = ahead && i + 1U < s.n;
        if (has_next)
        {
            prefetch_l2(next);
            prefetch_l2(next + 8);
        }
        const T ax = s.alpha * s.x[i];
        T* y = s.y;
        // By value: MSVC keeps by-reference captures in memory. The scalar, not its broadcast: an aligned vector
        // capture pads the closure (C4324).
        const T d = dot_walk(
            row, s.x, i,
            [y, ax, next, has_next](crd::usize k, V av)
            {
                if (has_next && k % 32U == 0U)
                {
                    prefetch_l2(next + k);
                }
                madd(av, V(ax), V::load(y + k)).store(y + k);
            },
            [y, row, ax](crd::usize k) { y[k] = y[k] + row[k] * ax; });
        y[i] = y[i] + s.alpha * (d + row[i] * s.x[i]);
    }
}

template <typename T> void trsv_kernel(const TrsvRows<T>& t) noexcept
{
    if (t.lower)
    {
        for (crd::usize i = 0; i < t.n; ++i)
        {
            const T* row = t.a + i * t.lda;
            const T s = t.x[i] - dot(row, t.x, i);
            t.x[i] = t.unit ? s : s / row[i];
        }
        return;
    }
    for (crd::usize i = t.n; i-- > 0;)
    {
        const T* row = t.a + i * t.lda;
        const T s = t.x[i] - dot(row + i + 1, t.x + i + 1, t.n - i - 1);
        t.x[i] = t.unit ? s : s / row[i];
    }
}

template <typename T> crd::usize cholesky_kernel(T* a, crd::usize n, crd::usize ld) noexcept
{
    for (crd::usize j = 0; j < n; ++j)
    {
        T* row_j = a + j * ld;
        const T pivot = row_j[j] - dot(row_j, row_j, j);
        if (!(pivot > T(0)))
        {
            return j + 1;
        }
        const T diag = crd::math::libm::sqrt(pivot);
        row_j[j] = diag;
        const T inv_diag = T(1) / diag;
        for (crd::usize i = j + 1; i < n; ++i)
        {
            T* row_i = a + i * ld;
            row_i[j] = (row_i[j] - dot(row_i, row_j, j)) * inv_diag;
        }
    }
    return 0;
}

template <typename T>
void cholesky_trsm_kernel(T* a, crd::usize ld, crd::usize k, crd::usize nb, crd::usize first, crd::usize end) noexcept
{
    for (crd::usize i = first; i < end; ++i)
    {
        T* row_i = a + i * ld + k;
        for (crd::usize j = 0; j < nb; ++j)
        {
            const T* row_j = a + (k + j) * ld + k;
            row_i[j] = (row_i[j] - dot(row_i, row_j, j)) / row_j[j];
        }
    }
}

template <typename T> void axpy_kernel(T* y, const T* x, T alpha, crd::usize n) noexcept
{
    axpy(y, x, alpha, n);
}

// Column blocks of kDotAccumulators vectors stay in registers over every row; the remaining columns go one vector at a
// time. Each element takes the rows in order either way.
template <typename T> void gemvt_kernel(const GemvtRows<T>& view) noexcept
{
    using V = typename VectorOf<T>::Type;
    constexpr crd::usize w = VectorOf<T>::kWidth;
    constexpr crd::usize block = kDotAccumulators * w;
    const GemvtRows<T> g = view;
    crd::usize j = 0;
    for (; j + block <= g.cols; j += block)
    {
        V acc[kDotAccumulators];
        auto load = [&](auto q)
        {
            acc[decltype(q)::kValue] = V::load(g.y + j + decltype(q)::kValue * w);
        };
        unroll<kDotAccumulators>(load);
        for (crd::usize r = 0; r < g.rows; ++r)
        {
            const V xr(g.x[r * g.incx]);
            const T* row = g.a + r * g.lda + j;
            auto step = [&](auto q)
            {
                constexpr crd::usize at = decltype(q)::kValue * w;
                acc[decltype(q)::kValue] = madd(xr, V::load(row + at), acc[decltype(q)::kValue]);
            };
            unroll<kDotAccumulators>(step);
        }
        auto store = [&](auto q)
        {
            acc[decltype(q)::kValue].store(g.y + j + decltype(q)::kValue * w);
        };
        unroll<kDotAccumulators>(store);
    }
    for_each_vector<T>(g.cols - j,
                       [&g, j](crd::usize k, crd::usize count)
                       {
                           T* y = g.y + j + k;
                           V acc = load_n<V>(y, count);
                           for (crd::usize r = 0; r < g.rows; ++r)
                           {
                               acc = madd(V(g.x[r * g.incx]), load_n<V>(g.a + r * g.lda + j + k, count), acc);
                           }
                           store_n(acc, y, count);
                       });
}

template <typename T> void ger_kernel(const GerRows<T>& view) noexcept
{
    const GerRows<T> g = view;
    for (crd::usize r = 0; r < g.rows; ++r)
    {
        axpy(g.a + r * g.lda, g.y, g.alpha * g.x[r], g.cols);
    }
}

template <typename T> void rot_kernel(T* x, T* y, T c, T s, crd::usize n) noexcept
{
    using V = typename VectorOf<T>::Type;
    for_each_vector<T>(n,
                       [x, y, c, s](crd::usize k, crd::usize count)
                       {
                           const V xv = load_n<V>(x + k, count);
                           const V yv = load_n<V>(y + k, count);
                           store_n(V(c) * xv + V(s) * yv, x + k, count);
                           store_n(V(c) * yv - V(s) * xv, y + k, count);
                       });
}

template <typename T> void reflect_kernel(const ReflectorRows<T>& view) noexcept
{
    const ReflectorRows<T> h = view;
    for (crd::usize r = 0; r < h.count; ++r)
    {
        T* c = h.c + r * h.ldc;
        const T w = h.tau * (c[0] + dot(h.v, c + 1, h.len));
        c[0] = c[0] - w;
        axpy(c + 1, h.v, -w, h.len);
    }
}

// Both terms of a segment in one pass: each element takes the step with -f0 u0, then the step with -f1 u1.
template <typename T>
CRD_FORCEINLINE void ldlt_two_terms(T* seg, const T* u0, const T* u1, T f0, T f1, crd::usize len) noexcept
{
    using V = typename VectorOf<T>::Type;
    const T n0 = -f0;
    const T n1 = -f1;
    for_each_vector<T>(len,
                       [seg, u0, u1, n0, n1](crd::usize k, crd::usize count)
                       {
                           const V s = madd(V(n0), load_n<V>(u0 + k, count), load_n<V>(seg + k, count));
                           store_n(madd(V(n1), load_n<V>(u1 + k, count), s), seg + k, count);
                       });
}

template <typename T> void ldlt_update_kernel(const LdltUpdate<T>& view) noexcept
{
    const LdltUpdate<T> u = view;
    for (crd::usize s = 0; s < u.rows; ++s)
    {
        T* seg = u.a + s * u.lda + (u.trailing ? s : 0U);
        const crd::usize from = u.trailing ? s : 0U;
        const crd::usize len = u.trailing ? u.n - s : s + 1U;
        if (u.u1 == nullptr)
        {
            const T f0 = u.u0[s] * u.inv_d;
            if (f0 != T(0))
            {
                axpy(seg, u.u0 + from, -f0, len);
            }
            continue;
        }
        const T f0 = (u.d22 * u.u0[s] - u.d21 * u.u1[s]) * u.inv_d;
        const T f1 = (u.d11 * u.u1[s] - u.d21 * u.u0[s]) * u.inv_d;
        if (f0 != T(0) && f1 != T(0))
        {
            ldlt_two_terms(seg, u.u0 + from, u.u1 + from, f0, f1, len);
        }
        else if (f0 != T(0))
        {
            axpy(seg, u.u0 + from, -f0, len);
        }
        else if (f1 != T(0))
        {
            axpy(seg, u.u1 + from, -f1, len);
        }
    }
}

template <typename T> struct TileOf
{
    using Type = F64Tile;
};
template <> struct TileOf<crd::f32>
{
    using Type = F32Tile;
};

} // namespace

template <typename T> constexpr DenseTypeKernels<T> type_kernels() noexcept
{
    return {
        &gemm_block<typename TileOf<T>::Type, T>,
        &dot_kernel<T>,
        &axpy_kernel<T>,
        &gemv_kernel<T>,
        &gemvt_kernel<T>,
        &ger_kernel<T>,
        &symv_kernel<T>,
        &trsv_kernel<T>,
        &rot_kernel<T>,
        &reflect_kernel<T>,
        &ldlt_update_kernel<T>,
        &cholesky_kernel<T>,
        &cholesky_trsm_kernel<T>,
    };
}

} // namespace crd::hesap::dense::detail::inline CRD_MATH_LEVEL_NS

namespace crd::hesap::dense
{

namespace level = detail::CRD_MATH_LEVEL_NS;

const DenseKernels CRD_KERNEL_TABLE(kDenseKernels) = {
    level::type_kernels<crd::f64>(),
    level::type_kernels<crd::f32>(),
};

} // namespace crd::hesap::dense
