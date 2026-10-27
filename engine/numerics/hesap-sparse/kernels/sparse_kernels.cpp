// The hesap-sparse kernels of crd/hesap/sparse/sparse_kernels.hpp, compiled once per CPU level by crd_add_cpu_kernels
// (../CMakeLists.txt, ADR-0134 §5). Everything here lives in the level's namespace (CRD_MATH_LEVEL_NS).
//
// Every body runs wide slices two at a time, so two independent sets of lane sums are in flight, and each lane adds
// vals * x[col] to its row's sum column by column with two roundings (D(sparse)-3: never a fused multiply-add). The
// portable bodies keep the lane sums in arrays; the SSE2 bodies in two 128-bit registers a slice, gathering x by
// scalar loads; the AVX2 bodies in one 256-bit register a slice, gathering f64 x by scalar loads and f32 x with
// vgatherdps (each the faster form on the 2026-10-27 board). The sums are written out by one scalar expression on
// every level, so every level gives the CSR spmv's bits.

#include <crd/hesap/sparse/sparse_kernels.hpp>
#include <crd/math/simd/backend.hpp>

namespace crd::hesap::sparse::inline CRD_MATH_LEVEL_NS
{
namespace
{

// y of the C rows of slice s from their sums: alpha * sum, plus beta * y unless beta is zero (y is then never read).
template <typename T, crd::u32 C>
void write_slice(const SellSlices<T>& a, T alpha, T beta, bool beta_zero, crd::u32 s, const T (&sum)[C], T* y) noexcept
{
    const crd::u32 first = s * C;
    const crd::u32 lanes = (first + C < a.rows) ? C : (a.rows - first);
    for (crd::u32 lane = 0; lane < lanes; ++lane)
    {
        const crd::u32 r = a.perm == nullptr ? first + lane : a.perm[first + lane];
        y[r] = beta_zero ? (alpha * sum[lane]) : (alpha * sum[lane] + beta * y[r]);
    }
}

// Columns [from, to) of the slice at element offset base added to sums.
template <typename Body, typename T>
typename Body::Sums add_columns(typename Body::Sums sums, const SellSlices<T>& a, const T* x, crd::u32 base,
                                crd::u32 from, crd::u32 to) noexcept
{
    for (crd::u32 w = from; w < to; ++w)
    {
        sums = Body::step(sums, a, x, base + w * Body::kC);
    }
    return sums;
}

// Two slices both at least this wide run as a pair, two independent sets of sums in flight; narrower ones run alone,
// where a pair lost 4% to 15% on DRAM-resident matrices and gained nothing in L2 (2026-10-27 board).
inline constexpr crd::u32 kPairWidth = 8;

// The driver. Body: the lane sums of one slice (Sums), zero(), step(sums, a, x, offset) for one column of C lanes at
// element offset, and store(sums, out).
template <typename Body, typename T>
void sell_spmv(const SellSlices<T>& a, T alpha, T beta, const T* x, T* y, crd::u32 s_begin, crd::u32 s_end) noexcept
{
    constexpr crd::u32 height = Body::kC;
    const bool beta_zero = beta == T(0);
    T out0[height];
    T out1[height];
    crd::u32 s = s_begin;
    while (s < s_end)
    {
        const crd::u32 b0 = a.slice_ptr[s];
        const crd::u32 w0 = a.slice_width[s];
        const crd::u32 w1 = s + 1U < s_end ? a.slice_width[s + 1U] : 0U;
        if (w0 < kPairWidth || w1 < kPairWidth)
        {
            Body::store(add_columns<Body>(Body::zero(), a, x, b0, 0, w0), out0);
            write_slice<T, height>(a, alpha, beta, beta_zero, s, out0, y);
            s += 1U;
            continue;
        }
        const crd::u32 b1 = a.slice_ptr[s + 1U];
        const crd::u32 both = w0 < w1 ? w0 : w1;
        typename Body::Sums sum0 = Body::zero();
        typename Body::Sums sum1 = Body::zero();
        for (crd::u32 w = 0; w < both; ++w)
        {
            sum0 = Body::step(sum0, a, x, b0 + w * height);
            sum1 = Body::step(sum1, a, x, b1 + w * height);
        }
        Body::store(add_columns<Body>(sum0, a, x, b0, both, w0), out0);
        Body::store(add_columns<Body>(sum1, a, x, b1, both, w1), out1);
        write_slice<T, height>(a, alpha, beta, beta_zero, s, out0, y);
        write_slice<T, height>(a, alpha, beta, beta_zero, s + 1U, out1, y);
        s += 2U;
    }
}

template <typename T, crd::u32 C> struct ScalarBody
{
    static constexpr crd::u32 kC = C;
    struct Sums
    {
        T lane[C];
    };
    static Sums zero() noexcept { return Sums{}; }
    static Sums step(Sums sums, const SellSlices<T>& a, const T* x, crd::u32 off) noexcept
    {
        for (crd::u32 lane = 0; lane < C; ++lane)
        {
            sums.lane[lane] = sums.lane[lane] + a.vals[off + lane] * x[a.col_idx[off + lane]];
        }
        return sums;
    }
    static void store(const Sums& sums, T (&out)[C]) noexcept
    {
        for (crd::u32 lane = 0; lane < C; ++lane)
        {
            out[lane] = sums.lane[lane];
        }
    }
};

#if CRD_SIMD_HAS_AVX2
struct Avx2F64Body
{
    static constexpr crd::u32 kC = kSellSliceF64;
    using Sums = __m256d;
    static Sums zero() noexcept { return _mm256_setzero_pd(); }
    static Sums step(Sums sums, const SellSlices<crd::f64>& a, const crd::f64* x, crd::u32 off) noexcept
    {
        // Scalar loads: 6% to 20% ahead of vgatherdpd on L2-resident matrices and level on DRAM-resident ones.
        const crd::u32* c = a.col_idx + off;
        const __m256d xv = _mm256_setr_pd(x[c[0]], x[c[1]], x[c[2]], x[c[3]]);
        return _mm256_add_pd(sums, _mm256_mul_pd(_mm256_loadu_pd(a.vals + off), xv));
    }
    static void store(Sums sums, crd::f64 (&out)[kC]) noexcept { _mm256_storeu_pd(out, sums); }
};

struct Avx2F32Body
{
    static constexpr crd::u32 kC = kSellSliceF32;
    using Sums = __m256;
    static Sums zero() noexcept { return _mm256_setzero_ps(); }
    static Sums step(Sums sums, const SellSlices<crd::f32>& a, const crd::f32* x, crd::u32 off) noexcept
    {
        const __m256i cols = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a.col_idx + off));
        return _mm256_add_ps(sums, _mm256_mul_ps(_mm256_loadu_ps(a.vals + off), _mm256_i32gather_ps(x, cols, 4)));
    }
    static void store(Sums sums, crd::f32 (&out)[kC]) noexcept { _mm256_storeu_ps(out, sums); }
};

using F64Body = Avx2F64Body;
using F32Body = Avx2F32Body;
#elif CRD_SIMD_HAS_SSE2
struct Sse2F64Body
{
    static constexpr crd::u32 kC = kSellSliceF64;
    struct Sums
    {
        __m128d lo;
        __m128d hi;
    };
    static Sums zero() noexcept { return Sums{_mm_setzero_pd(), _mm_setzero_pd()}; }
    static Sums step(Sums sums, const SellSlices<crd::f64>& a, const crd::f64* x, crd::u32 off) noexcept
    {
        const crd::u32* c = a.col_idx + off;
        const __m128d x_lo = _mm_loadh_pd(_mm_load_sd(x + c[0]), x + c[1]);
        const __m128d x_hi = _mm_loadh_pd(_mm_load_sd(x + c[2]), x + c[3]);
        sums.lo = _mm_add_pd(sums.lo, _mm_mul_pd(_mm_loadu_pd(a.vals + off), x_lo));
        sums.hi = _mm_add_pd(sums.hi, _mm_mul_pd(_mm_loadu_pd(a.vals + off + 2U), x_hi));
        return sums;
    }
    static void store(const Sums& sums, crd::f64 (&out)[kC]) noexcept
    {
        _mm_storeu_pd(out, sums.lo);
        _mm_storeu_pd(out + 2, sums.hi);
    }
};

struct Sse2F32Body
{
    static constexpr crd::u32 kC = kSellSliceF32;
    struct Sums
    {
        __m128 lo;
        __m128 hi;
    };
    static Sums zero() noexcept { return Sums{_mm_setzero_ps(), _mm_setzero_ps()}; }
    static Sums step(Sums sums, const SellSlices<crd::f32>& a, const crd::f32* x, crd::u32 off) noexcept
    {
        const crd::u32* c = a.col_idx + off;
        alignas(16) crd::f32 xg[kC]; // scalar loads, then two vector loads: 2% ahead of four-lane shuffles in L2
        for (crd::u32 lane = 0; lane < kC; ++lane)
        {
            xg[lane] = x[c[lane]];
        }
        sums.lo = _mm_add_ps(sums.lo, _mm_mul_ps(_mm_loadu_ps(a.vals + off), _mm_load_ps(xg)));
        sums.hi = _mm_add_ps(sums.hi, _mm_mul_ps(_mm_loadu_ps(a.vals + off + 4U), _mm_load_ps(xg + 4)));
        return sums;
    }
    static void store(const Sums& sums, crd::f32 (&out)[kC]) noexcept
    {
        _mm_storeu_ps(out, sums.lo);
        _mm_storeu_ps(out + 4, sums.hi);
    }
};

using F64Body = Sse2F64Body;
using F32Body = Sse2F32Body;
#else
using F64Body = ScalarBody<crd::f64, kSellSliceF64>;
using F32Body = ScalarBody<crd::f32, kSellSliceF32>;
#endif

} // namespace

constexpr auto kSellSpmvF64 = &sell_spmv<F64Body, crd::f64>;
constexpr auto kSellSpmvF32 = &sell_spmv<F32Body, crd::f32>;

} // namespace crd::hesap::sparse::inline CRD_MATH_LEVEL_NS

namespace crd::hesap::sparse
{

const SparseKernels CRD_KERNEL_TABLE(kSparseKernels) = {
    CRD_MATH_LEVEL_NS::kSellSpmvF64,
    CRD_MATH_LEVEL_NS::kSellSpmvF32,
};

} // namespace crd::hesap::sparse
