// The hesap-dense kernels of crd/hesap/dense/dense_kernels.hpp, compiled once per CPU level by crd_add_cpu_kernels
// (../CMakeLists.txt, ADR-0134 §5). Everything here lives in the level's namespace (CRD_MATH_LEVEL_NS).
//
// Each level has its own tile shape and packs its own panels (GemmShape). The portable level runs the block on a 4 by 4
// PortableTile (two roundings, its accumulators in registers). The vector levels run one tile of two registers a row:
// two accumulators a row, two B registers and one A broadcast a step. sse2 and avx2 take 6 rows (15 of 16 vector
// registers), avx512 14 (31 of 32 ZMM, the transpose of BLIS's skx shape). sse2 rounds twice in 128-bit registers (f64
// 6 by 4, f32 6 by 8) and gives the portable level's bits; avx2 sums by single-rounded FMA (f64 6 by 8, about 98% of
// peak in L1; f32 6 by 16), as avx512 does (f64 14 by 16, f32 14 by 32), so those two give the same bits. The tile
// hands each finished row to a sink while its accumulators are still in registers: the product stores the tile, the
// fused path merges a full row-major tile straight into C with merge_element's expression. Each step prefetches the
// next B panel's row (one line, two at avx512), which hides its re-stream from L3 at each panel boundary.

//
// The vector and matrix-vector kernels are written once over the level's vectors (vector_steps.hpp: Vec4d and Vec8f,
// one YMM from avx2, two XMM on sse2, arrays on portable; Vec8d and Vec16f, one ZMM, at avx512); dense_kernels.hpp
// declares their reduction shapes. A level fixes only the accumulator and row counts (enough independent chains for
// its adder or FMA latency) and whether a step fuses.

#include <crd/hesap/dense/detail/gemm_block.hpp>
#include <crd/hesap/dense/detail/vector_steps.hpp>
#include <crd/math/libm.hpp>
#include <crd/math/simd/backend.hpp>

namespace crd::hesap::dense::detail::inline CRD_MATH_LEVEL_NS
{
namespace
{

#if CRD_SIMD_HAS_SSE2
// A hint to fetch the line of `at`, a pointer into the block's buffers.
CRD_FORCEINLINE void prefetch(const void* at) noexcept
{
    _mm_prefetch(static_cast<const char*>(at), _MM_HINT_T0);
}

// The registers of a tile: kWidth lanes of T, a step of acc + a b (fused from avx2, rounded twice on sse2) and the
// operations of a merge.
#if CRD_SIMD_HAS_AVX2
template <typename T> struct Reg
{
    using V = typename VectorOf<T>::Type;
    static constexpr crd::usize kWidth = VectorOf<T>::kWidth;
    static CRD_FORCEINLINE V zero() noexcept { return V(T(0)); }
    static CRD_FORCEINLINE V splat(T x) noexcept { return V(x); }
    static CRD_FORCEINLINE V load(const T* p) noexcept { return V::load(p); }
    static CRD_FORCEINLINE void store(T* p, V v) noexcept { v.store(p); }
    static CRD_FORCEINLINE V step(V acc, V a, V b) noexcept { return simd::fma(a, b, acc); }
    static CRD_FORCEINLINE V add(V x, V y) noexcept { return x + y; }
    static CRD_FORCEINLINE V sub(V x, V y) noexcept { return x - y; }
    static CRD_FORCEINLINE V mul(V x, V y) noexcept { return x * y; }
};
#else
template <typename T> struct Reg;
template <> struct Reg<crd::f64>
{
    using V = __m128d;
    static constexpr crd::usize kWidth = 2;
    static CRD_FORCEINLINE V zero() noexcept { return _mm_setzero_pd(); }
    static CRD_FORCEINLINE V splat(crd::f64 x) noexcept { return _mm_set1_pd(x); }
    static CRD_FORCEINLINE V load(const crd::f64* p) noexcept { return _mm_loadu_pd(p); }
    static CRD_FORCEINLINE void store(crd::f64* p, V v) noexcept { _mm_storeu_pd(p, v); }
    static CRD_FORCEINLINE V step(V acc, V a, V b) noexcept { return _mm_add_pd(acc, _mm_mul_pd(a, b)); }
    static CRD_FORCEINLINE V add(V x, V y) noexcept { return _mm_add_pd(x, y); }
    static CRD_FORCEINLINE V sub(V x, V y) noexcept { return _mm_sub_pd(x, y); }
    static CRD_FORCEINLINE V mul(V x, V y) noexcept { return _mm_mul_pd(x, y); }
};
template <> struct Reg<crd::f32>
{
    using V = __m128;
    static constexpr crd::usize kWidth = 4;
    static CRD_FORCEINLINE V zero() noexcept { return _mm_setzero_ps(); }
    static CRD_FORCEINLINE V splat(crd::f32 x) noexcept { return _mm_set1_ps(x); }
    static CRD_FORCEINLINE V load(const crd::f32* p) noexcept { return _mm_loadu_ps(p); }
    static CRD_FORCEINLINE void store(crd::f32* p, V v) noexcept { _mm_storeu_ps(p, v); }
    static CRD_FORCEINLINE V step(V acc, V a, V b) noexcept { return _mm_add_ps(acc, _mm_mul_ps(a, b)); }
    static CRD_FORCEINLINE V add(V x, V y) noexcept { return _mm_add_ps(x, y); }
    static CRD_FORCEINLINE V sub(V x, V y) noexcept { return _mm_sub_ps(x, y); }
    static CRD_FORCEINLINE V mul(V x, V y) noexcept { return _mm_mul_ps(x, y); }
};
#endif

// One register of a full tile merged into C with merge_element's expression (gemm_block.hpp).
template <GemmMerge M, typename R, typename T, typename V>
CRD_FORCEINLINE void merge_into(T* c, V p, V alpha, V beta) noexcept
{
    if constexpr (M == GemmMerge::Assign)
    {
        R::store(c, p);
    }
    else if constexpr (M == GemmMerge::Zero)
    {
        // +0 + x as x - (-0), the same IEEE value: MSVC 14.51 folds a vector add of a constant +0 to its other
        // operand, which keeps a -0 the expression turns into +0.
        R::store(c, R::sub(R::mul(alpha, p), R::splat(T(-0.0))));
    }
    else if constexpr (M == GemmMerge::Scale)
    {
        R::store(c, R::add(R::mul(beta, R::load(c)), R::mul(alpha, p)));
    }
    else if constexpr (M == GemmMerge::Add)
    {
        R::store(c, R::add(R::load(c), p));
    }
    else if constexpr (M == GemmMerge::Subtract)
    {
        R::store(c, R::sub(R::load(c), p));
    }
    else
    {
        R::store(c, R::add(R::load(c), R::mul(alpha, p)));
    }
}

// The tile's rows: two accumulators a row beside two B registers and one A broadcast fill 15 of the 16 vector registers
// below avx512 and 31 of the 32 at avx512.
#if CRD_SIMD_HAS_AVX512
constexpr crd::usize kTileRows = 14;
#else
constexpr crd::usize kTileRows = 6;
#endif

// The values of T in a cache line, the stride of the tile's prefetches.
template <typename T> constexpr crd::usize kLineValues = 64U / sizeof(T);

// The kTileRows by 2w tile of T on the level's w-lane registers: two accumulators a row, two B registers and one A
// broadcast reused row after row, the next tile's B panel (b_next) prefetched a line of each step's row at a time.
// sink(i, lo, hi) receives row i. Every register array index is a constant (unroll), so the arrays stay in registers.
template <typename T, typename Sink>
CRD_FORCEINLINE void row_tile(crd::usize depth, crd::usize lda, const T* a, const T* b, const T* b_next,
                              Sink sink) noexcept
{
    using R = Reg<T>;
    using V = typename R::V;
    constexpr crd::usize w = R::kWidth;
    constexpr crd::usize nr = 2U * w;
    V lo[kTileRows];
    V hi[kTileRows];
    auto zero = [&](auto q)
    {
        lo[decltype(q)::kValue] = R::zero();
        hi[decltype(q)::kValue] = R::zero();
    };
    unroll<kTileRows>(zero);
    for (crd::usize p = 0; p < depth; ++p)
    {
        const V b0 = R::load(b + p * nr);
        const V b1 = R::load(b + p * nr + w);
        for (crd::usize at = 0; at < nr; at += kLineValues<T>)
        {
            prefetch(b_next + p * nr + at);
        }
        auto row = [&](auto q)
        {
            constexpr crd::usize i = decltype(q)::kValue;
            const V ai = R::splat(a[i * lda + p]);
            lo[i] = R::step(lo[i], ai, b0);
            hi[i] = R::step(hi[i], ai, b1);
        };
        unroll<kTileRows>(row);
    }
    auto hand = [&](auto q)
    {
        sink(decltype(q)::kValue, lo[decltype(q)::kValue], hi[decltype(q)::kValue]);
    };
    unroll<kTileRows>(hand);
}

template <typename T> struct RowTile
{
    using R = Reg<T>;
    using V = typename R::V;
    static constexpr bool kFused = true;
    static constexpr crd::usize kMr = kTileRows;
    static constexpr crd::usize kNr = 2U * R::kWidth;

    static void product(crd::usize depth, crd::usize lda, const T* a, const T* b, const T* b_next, T* out) noexcept
    {
        row_tile(depth, lda, a, b, b_next,
                 [out](crd::usize i, V lo, V hi)
                 {
                     R::store(out + i * kNr, lo);
                     R::store(out + i * kNr + R::kWidth, hi);
                 });
    }

    template <GemmMerge M>
    static void fused(crd::usize depth, crd::usize lda, const T* a, const T* b, const T* b_next, T* c, crd::usize ldc,
                      T alpha, T beta) noexcept
    {
        for (crd::usize i = 0; i < kMr; ++i) // C is read after the depth loop: request its lines now
        {
            for (crd::usize at = 0; at < kNr; at += kLineValues<T>)
            {
                prefetch(c + i * ldc + at);
            }
            prefetch(c + i * ldc + kNr - 1U);
        }
        const V av = R::splat(alpha);
        const V bv = R::splat(beta);
        row_tile(depth, lda, a, b, b_next,
                 [c, ldc, av, bv](crd::usize i, V lo, V hi)
                 {
                     merge_into<M, R>(c + i * ldc, lo, av, bv);
                     merge_into<M, R>(c + i * ldc + R::kWidth, hi, av, bv);
                 });
    }
};

using F64Tile = RowTile<crd::f64>;
using F32Tile = RowTile<crd::f32>;
#else
// 4 by 4: sixteen accumulators, four A and four B values a step. On the one-thread board of 2026-10-30 it ran f64
// 1.18x to 1.28x and f32 1.02x to 1.03x faster than a 2 by 4 tile at every size from 64 to 1024 on x64 (its 16 scalar
// registers), with the same bits; ARM64 (32 registers) runs the dense kernels at this level.
using F64Tile = PortableTile<crd::f64, 4, 4, true>;
using F32Tile = PortableTile<crd::f32, 4, 4, true>;
#endif

// The packers of a tile's shape (GemmKernels) over a strided source.
template <typename T, crd::usize MR>
void pack_a_kernel(const GemmSource<T>& s, crd::usize rows, crd::usize depth, T* out) noexcept
{
    pack_a_source<MR>(s, rows, depth, out, [](T x) { return x; });
}

template <typename T, crd::usize NR>
void pack_b_kernel(const GemmSource<T>& s, crd::usize depth, crd::usize cols, T* out) noexcept
{
    pack_b_source<NR>(s, depth, cols, out, [](T x) { return x; });
}

// ---- the vector and matrix-vector kernels ----

// Independent chains for the adder (below avx2) or the FMA (from avx2), each four cycles deep on two ports: eight
// registers, YMM or ZMM. Below avx2 a vector is two XMM registers, so half as many vectors.
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

CRD_FORCEINLINE void prefetch_l2([[maybe_unused]] const void* p) noexcept
{
#if CRD_SIMD_HAS_SSE2
    _mm_prefetch(static_cast<const char*>(p), _MM_HINT_T1);
#endif
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

// ---- the split-complex kernels ----

// The accumulators of each real product of a complex dot: the four products share the registers with their loads, so
// two each from avx2 (eight FMA chains, YMM or ZMM) and one below (a vector is two XMM there: eight adder chains).
#if CRD_SIMD_HAS_AVX2
constexpr crd::usize kSplitAccumulators = 2;
#else
constexpr crd::usize kSplitAccumulators = 1;
#endif

// cgemvt keeps a column block of y in registers over every row: re and im accumulators for kSplitBlock vectors.
constexpr crd::usize kSplitBlock = kDotAccumulators / 2U;

template <typename T> struct SplitValue
{
    T re;
    T im;
};

template <typename V> CRD_FORCEINLINE V accumulator_sum(const V* acc) noexcept
{
    if constexpr (kSplitAccumulators == 2U)
    {
        return acc[0] + acc[1];
    }
    else
    {
        return acc[0];
    }
}

// The complex dot product u = sum a_k v_k over n elements in the shape dense_kernels.hpp declares. on_vector(k, a_re,
// a_im) and on_element(k) see every whole vector and every remaining element of a (chemv's column update).
template <typename T, typename OnVector, typename OnElement>
CRD_FORCEINLINE SplitValue<T> split_dot_walk(const T* ar, const T* ai, const T* vr, const T* vi, crd::usize n,
                                             OnVector on_vector, OnElement on_element) noexcept
{
    using V = typename VectorOf<T>::Type;
    constexpr crd::usize w = VectorOf<T>::kWidth;
    V rere[kSplitAccumulators];
    V imim[kSplitAccumulators];
    V reim[kSplitAccumulators];
    V imre[kSplitAccumulators];
    auto zero = [&](auto q)
    {
        constexpr crd::usize at = decltype(q)::kValue;
        rere[at] = V::zero();
        imim[at] = V::zero();
        reim[at] = V::zero();
        imre[at] = V::zero();
    };
    unroll<kSplitAccumulators>(zero);
    auto take = [&](auto q, crd::usize k)
    {
        constexpr crd::usize at = decltype(q)::kValue;
        const V a = V::load(ar + k);
        const V b = V::load(ai + k);
        const V c = V::load(vr + k);
        const V d = V::load(vi + k);
        rere[at] = madd(a, c, rere[at]);
        imim[at] = madd(b, d, imim[at]);
        reim[at] = madd(a, d, reim[at]);
        imre[at] = madd(b, c, imre[at]);
        on_vector(k, a, b);
    };
    crd::usize k = 0;
    for (; k + kSplitAccumulators * w <= n; k += kSplitAccumulators * w)
    {
        auto step = [&](auto q)
        {
            take(q, k + decltype(q)::kValue * w);
        };
        unroll<kSplitAccumulators>(step);
    }
    // Fewer than kSplitAccumulators whole vectors remain; they continue the rotation from accumulator 0.
    bool more = true;
    auto rest = [&](auto q)
    {
        more = more && k + w <= n;
        if (more)
        {
            take(q, k);
            k += w;
        }
    };
    unroll<kSplitAccumulators - 1U>(rest);
    T s_rere = simd::horizontal_sum(accumulator_sum(rere));
    T s_imim = simd::horizontal_sum(accumulator_sum(imim));
    T s_reim = simd::horizontal_sum(accumulator_sum(reim));
    T s_imre = simd::horizontal_sum(accumulator_sum(imre));
    for (; k < n; ++k)
    {
        s_rere += ar[k] * vr[k];
        s_imim += ai[k] * vi[k];
        s_reim += ar[k] * vi[k];
        s_imre += ai[k] * vr[k];
        on_element(k);
    }
    return {s_rere - s_imim, s_reim + s_imre};
}

// The two steps of each part of y + s x (Conj: y + s conj(x)) on one vector, the s_re term first.
template <bool Conj, typename V> CRD_FORCEINLINE void split_step(V& yr, V& yi, V xr, V xi, V sr, V si) noexcept
{
    yr = madd(sr, xr, yr);
    if constexpr (Conj)
    {
        yi = madd(-sr, xi, yi);
        yr = madd(si, xi, yr);
    }
    else
    {
        yi = madd(sr, xi, yi);
        yr = madd(-si, xi, yr);
    }
    yi = madd(si, xr, yi);
}

// y + s x (Conj: y + s conj(x)) over n elements, every element the level's steps.
template <bool Conj, typename T>
CRD_FORCEINLINE void split_axpy(T* yr, T* yi, const T* xr, const T* xi, T sr, T si, crd::usize n) noexcept
{
    using V = typename VectorOf<T>::Type;
    for_each_vector<T>(n,
                       [yr, yi, xr, xi, sr, si](crd::usize k, crd::usize count)
                       {
                           V re = load_n<V>(yr + k, count);
                           V im = load_n<V>(yi + k, count);
                           split_step<Conj>(re, im, load_n<V>(xr + k, count), load_n<V>(xi + k, count), V(sr), V(si));
                           store_n(re, yr + k, count);
                           store_n(im, yi + k, count);
                       });
}

template <typename T> CRD_FORCEINLINE SplitValue<T> complex_product(T are, T aim, T bre, T bim) noexcept
{
    return {are * bre - aim * bim, are * bim + aim * bre};
}

template <typename T> void creflect_kernel(const SplitReflectorRows<T>& view) noexcept
{
    const SplitReflectorRows<T> h = view;
    for (crd::usize r = 0; r < h.count; ++r)
    {
        T* re = h.re + r * h.ld;
        T* im = h.im + r * h.ld;
        const SplitValue<T> u = split_dot_walk(
            re, im, h.vr, h.vi, h.len, [](crd::usize, typename VectorOf<T>::Type, typename VectorOf<T>::Type) {},
            [](crd::usize) {});
        const SplitValue<T> s = complex_product(h.tau_re, h.tau_im, u.re, u.im);
        split_axpy<true>(re, im, h.vr, h.vi, -s.re, -s.im, h.len);
    }
}

template <typename T> void cgemvt_kernel(const SplitGemvtRows<T>& view) noexcept
{
    using V = typename VectorOf<T>::Type;
    constexpr crd::usize w = VectorOf<T>::kWidth;
    constexpr crd::usize block = kSplitBlock * w;
    const SplitGemvtRows<T> g = view;
    const T sign = g.conj_x ? T(-1) : T(1);
    crd::usize j = 0;
    for (; j + block <= g.cols; j += block)
    {
        V acc_re[kSplitBlock];
        V acc_im[kSplitBlock];
        auto load = [&](auto q)
        {
            constexpr crd::usize at = decltype(q)::kValue;
            acc_re[at] = V::load(g.yr + j + at * w);
            acc_im[at] = V::load(g.yi + j + at * w);
        };
        unroll<kSplitBlock>(load);
        for (crd::usize r = 0; r < g.rows; ++r)
        {
            const V sr(g.xr[r]);
            const V si(sign * g.xi[r]);
            const T* re = g.re + r * g.ld + j;
            const T* im = g.im + r * g.ld + j;
            auto step = [&](auto q)
            {
                constexpr crd::usize at = decltype(q)::kValue;
                split_step<false>(acc_re[at], acc_im[at], V::load(re + at * w), V::load(im + at * w), sr, si);
            };
            unroll<kSplitBlock>(step);
        }
        auto store = [&](auto q)
        {
            constexpr crd::usize at = decltype(q)::kValue;
            acc_re[at].store(g.yr + j + at * w);
            acc_im[at].store(g.yi + j + at * w);
        };
        unroll<kSplitBlock>(store);
    }
    for_each_vector<T>(g.cols - j,
                       [&g, j, sign](crd::usize k, crd::usize count)
                       {
                           T* yr = g.yr + j + k;
                           T* yi = g.yi + j + k;
                           V acc_re = load_n<V>(yr, count);
                           V acc_im = load_n<V>(yi, count);
                           for (crd::usize r = 0; r < g.rows; ++r)
                           {
                               const crd::usize at = r * g.ld + j + k;
                               split_step<false>(acc_re, acc_im, load_n<V>(g.re + at, count),
                                                 load_n<V>(g.im + at, count), V(g.xr[r]), V(sign * g.xi[r]));
                           }
                           store_n(acc_re, yr, count);
                           store_n(acc_im, yi, count);
                       });
}

template <typename T> void cger_kernel(const SplitGerRows<T>& view) noexcept
{
    const SplitGerRows<T> g = view;
    for (crd::usize r = 0; r < g.rows; ++r)
    {
        const SplitValue<T> s = complex_product(g.alpha_re, g.alpha_im, g.xr[r], g.xi[r]);
        split_axpy<false>(g.re + r * g.ld, g.im + r * g.ld, g.yr, g.yi, s.re, s.im, g.cols);
    }
}

template <typename T> void chemv_kernel(const SplitHemvLower<T>& view) noexcept
{
    using V = typename VectorOf<T>::Type;
    const SplitHemvLower<T> h = view;
    for (crd::usize i = 0; i < h.n; ++i)
    {
        h.xr[i] = T(0);
        h.xi[i] = T(0);
    }
    for (crd::usize i = 0; i < h.n; ++i)
    {
        const T* re = h.re + i * h.ld;
        const T* im = h.im + i * h.ld;
        const T sr = h.vr[i];
        const T si = h.vi[i];
        T* xr = h.xr;
        T* xi = h.xi;
        // By value, the scalars rather than their broadcasts (see symv_kernel).
        const SplitValue<T> u = split_dot_walk(
            re, im, h.vr, h.vi, i,
            [xr, xi, sr, si](crd::usize k, V are, V aim)
            {
                V yr = V::load(xr + k);
                V yi = V::load(xi + k);
                split_step<true>(yr, yi, are, aim, V(sr), V(si));
                yr.store(xr + k);
                yi.store(xi + k);
            },
            [xr, xi, re, im, sr, si](crd::usize k)
            {
                xr[k] = xr[k] + sr * re[k];
                xi[k] = xi[k] + -sr * im[k];
                xr[k] = xr[k] + si * im[k];
                xi[k] = xi[k] + si * re[k];
            });
        h.xr[i] = h.xr[i] + (u.re + re[i] * sr);
        h.xi[i] = h.xi[i] + (u.im + re[i] * si);
    }
}

template <typename T> void cher2_kernel(const SplitHer2Lower<T>& view) noexcept
{
    using V = typename VectorOf<T>::Type;
    const SplitHer2Lower<T> h = view;
    for (crd::usize i = 0; i < h.n; ++i)
    {
        T* re = h.re + i * h.ld;
        T* im = h.im + i * h.ld;
        const T vr = -h.vr[i];
        const T vi = -h.vi[i];
        const T wr = -h.wr[i];
        const T wi = -h.wi[i];
        const T* xvr = h.vr;
        const T* xvi = h.vi;
        const T* xwr = h.wr;
        const T* xwi = h.wi;
        for_each_vector<T>(
            i + 1U,
            [re, im, xvr, xvi, xwr, xwi, vr, vi, wr, wi](crd::usize k, crd::usize count)
            {
                V yr = load_n<V>(re + k, count);
                V yi = load_n<V>(im + k, count);
                split_step<true>(yr, yi, load_n<V>(xwr + k, count), load_n<V>(xwi + k, count), V(vr), V(vi));
                split_step<true>(yr, yi, load_n<V>(xvr + k, count), load_n<V>(xvi + k, count), V(wr), V(wi));
                store_n(yr, re + k, count);
                store_n(yi, im + k, count);
            });
        im[i] = T(0);
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

template <typename T, typename Tile> constexpr GemmKernels<T> gemm_kernels() noexcept
{
    return {
        .shape = {.mr = Tile::kMr, .nr = Tile::kNr},
        .block = &gemm_block<Tile, T>,
        .pack_a = &pack_a_kernel<T, Tile::kMr>,
        .pack_b = &pack_b_kernel<T, Tile::kNr>,
    };
}

template <typename T> constexpr DenseTypeKernels<T> type_kernels() noexcept
{
    return {
        .gemm = gemm_kernels<T, typename TileOf<T>::Type>(),
        .dot = &dot_kernel<T>,
        .axpy = &axpy_kernel<T>,
        .gemv = &gemv_kernel<T>,
        .gemvt = &gemvt_kernel<T>,
        .ger = &ger_kernel<T>,
        .symv = &symv_kernel<T>,
        .trsv = &trsv_kernel<T>,
        .rot = &rot_kernel<T>,
        .reflect = &reflect_kernel<T>,
        .ldlt_update = &ldlt_update_kernel<T>,
        .creflect = &creflect_kernel<T>,
        .cgemvt = &cgemvt_kernel<T>,
        .cger = &cger_kernel<T>,
        .chemv = &chemv_kernel<T>,
        .cher2 = &cher2_kernel<T>,
        .cholesky = &cholesky_kernel<T>,
        .cholesky_trsm = &cholesky_trsm_kernel<T>,
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
