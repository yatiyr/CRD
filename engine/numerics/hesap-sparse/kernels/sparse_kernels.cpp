// The hesap-sparse kernels of crd/hesap/sparse/sparse_kernels.hpp, compiled once per CPU level by crd_add_cpu_kernels
// (../CMakeLists.txt, ADR-0134 §5). Everything here lives in the level's namespace (CRD_MATH_LEVEL_NS).
//
// Every body runs wide slices two at a time, so two independent sets of lane sums are in flight, and each lane adds
// vals * x[col] to its row's sum column by column with two roundings (D(sparse)-3: never a fused multiply-add). The
// portable bodies keep the lane sums in arrays; the SSE2 bodies in 128-bit registers, gathering x by scalar loads; the
// AVX2 bodies in 256-bit registers, gathering f64 and complex x by scalar loads and f32 x with vgatherdps (each the
// faster form on the 2026-10-27 board). A complex product is (vr xr - vi xi, vr xi + vi xr), SSE2 adding the negated
// vi xi as it has no addsub. The columns every row of a slice stores run unmasked; past the shortest row a padded
// lane multiplies its padding 0 by a zero instead of x[0], so an infinite or NaN x[0] reaches only the rows that
// store column 0, and adding that +0 leaves the sum's bits (a sum from +0 is never -0). The sums are written out by
// one scalar expression on every level, so every level gives the CSR spmv's bits.

#include <crd/hesap/sparse/sparse_kernels.hpp>
#include <crd/math/simd/backend.hpp>

namespace crd::hesap::sparse::inline CRD_MATH_LEVEL_NS
{
namespace
{

// What a padded lane reads in place of x: one real or complex zero.
template <typename T> constexpr T kZeros[2] = {};

// x at element `at` for a lane at column w of a row of `len` values; a padded lane (Masked, w >= len) reads zeros.
template <bool Masked, typename T>
const T* x_of(const T* x, crd::usize at, [[maybe_unused]] crd::u32 w, [[maybe_unused]] crd::u32 len) noexcept
{
    if constexpr (Masked)
    {
        return w < len ? x + at : kZeros<T>;
    }
    else
    {
        return x + at;
    }
}

// y[r] of a real row from its sum: alpha * sum, plus beta * y unless beta is zero (y is then never read).
template <typename T> struct RealWrite
{
    static constexpr crd::u32 kParts = 1;
    T alpha;
    T beta;
    bool beta_zero;
    void operator()(T* y, crd::usize r, const T* sum) const noexcept
    {
        y[r] = beta_zero ? (alpha * sum[0]) : (alpha * sum[0] + beta * y[r]);
    }
};

// The same for a complex row, its sum and y as (re, im) pairs, in the operators of Complex<T>.
template <typename T> struct ComplexWrite
{
    static constexpr crd::u32 kParts = 2;
    SellComplexScales<T> s;
    bool beta_zero;
    void operator()(T* y, crd::usize r, const T* sum) const noexcept
    {
        T re = s.alpha_re * sum[0] - s.alpha_im * sum[1];
        T im = s.alpha_re * sum[1] + s.alpha_im * sum[0];
        if (!beta_zero)
        {
            const T yr = y[2U * r];
            const T yi = y[2U * r + 1U];
            re = re + (s.beta_re * yr - s.beta_im * yi);
            im = im + (s.beta_re * yi + s.beta_im * yr);
        }
        y[2U * r] = re;
        y[2U * r + 1U] = im;
    }
};

// y of the C rows of slice s from their sums (Write::kParts values a row).
template <crd::u32 C, typename T, typename Write>
void write_slice(const SellSlices<T>& a, const Write& write, crd::u32 s, const T* sums, T* y) noexcept
{
    const crd::u32 first = s * C;
    const crd::u32 lanes = (first + C < a.rows) ? C : (a.rows - first);
    for (crd::u32 lane = 0; lane < lanes; ++lane)
    {
        const crd::u32 r = a.perm == nullptr ? first + lane : a.perm[first + lane];
        write(y, r, sums + lane * Write::kParts);
    }
}

// The columns every row of a slice stores: the shortest of its C rows.
template <crd::u32 C> crd::u32 full_columns(const crd::u32* lens) noexcept
{
    crd::u32 full = lens[0];
    for (crd::u32 lane = 1; lane < C; ++lane)
    {
        full = lens[lane] < full ? lens[lane] : full;
    }
    return full;
}

// Columns [from, width) of slice s added to sums: unmasked up to `full`, masked after it.
template <typename Body, typename T>
typename Body::Sums finish(typename Body::Sums sums, const SellSlices<T>& a, const T* x, crd::u32 s, crd::u32 from,
                           crd::u32 full) noexcept
{
    constexpr crd::u32 height = Body::kC;
    const crd::u32 base = a.slice_ptr[s];
    const crd::u32 width = a.slice_width[s];
    const crd::u32* lens = a.row_len + static_cast<crd::usize>(s) * height;
    for (crd::u32 w = from; w < full; ++w)
    {
        sums = Body::template step<false>(sums, a, x, base + w * height, w, lens);
    }
    for (crd::u32 w = full; w < width; ++w)
    {
        sums = Body::template step<true>(sums, a, x, base + w * height, w, lens);
    }
    return sums;
}

// Two slices both at least this wide run as a pair, two independent sets of sums in flight; narrower ones run alone,
// where a pair lost 4% to 15% on DRAM-resident matrices and gained nothing in L2 (2026-10-27 board).
inline constexpr crd::u32 kPairWidth = 8;

// The driver. Body: the lane sums of one slice (Sums), zero(), step<Masked>(sums, a, x, offset, w, lens) for column w
// of the C lanes at element offset, and store(sums, out) of its kOut values (C rows of Write::kParts).
template <typename Body, typename T, typename Write>
void sell_spmv(const SellSlices<T>& a, const Write& write, const T* x, T* y, crd::u32 s_begin, crd::u32 s_end) noexcept
{
    constexpr crd::u32 height = Body::kC;
    static_assert(Body::kOut == height * Write::kParts, "a body stores one sum per part of each row");
    T out0[Body::kOut];
    T out1[Body::kOut];
    crd::u32 s = s_begin;
    while (s < s_end)
    {
        const crd::u32 full0 = full_columns<height>(a.row_len + static_cast<crd::usize>(s) * height);
        const crd::u32 w0 = a.slice_width[s];
        const crd::u32 w1 = s + 1U < s_end ? a.slice_width[s + 1U] : 0U;
        if (w0 < kPairWidth || w1 < kPairWidth)
        {
            Body::store(finish<Body>(Body::zero(), a, x, s, 0, full0), out0);
            write_slice<height>(a, write, s, out0, y);
            s += 1U;
            continue;
        }
        const crd::u32 full1 = full_columns<height>(a.row_len + static_cast<crd::usize>(s + 1U) * height);
        const crd::u32 b0 = a.slice_ptr[s];
        const crd::u32 b1 = a.slice_ptr[s + 1U];
        const crd::u32* lens0 = a.row_len + static_cast<crd::usize>(s) * height;
        const crd::u32* lens1 = lens0 + height;
        const crd::u32 both = full0 < full1 ? full0 : full1;
        typename Body::Sums sum0 = Body::zero();
        typename Body::Sums sum1 = Body::zero();
        for (crd::u32 w = 0; w < both; ++w)
        {
            sum0 = Body::template step<false>(sum0, a, x, b0 + w * height, w, lens0);
            sum1 = Body::template step<false>(sum1, a, x, b1 + w * height, w, lens1);
        }
        Body::store(finish<Body>(sum0, a, x, s, both, full0), out0);
        Body::store(finish<Body>(sum1, a, x, s + 1U, both, full1), out1);
        write_slice<height>(a, write, s, out0, y);
        write_slice<height>(a, write, s + 1U, out1, y);
        s += 2U;
    }
}

template <typename T, crd::u32 C> struct ScalarBody
{
    static constexpr crd::u32 kC = C;
    static constexpr crd::u32 kOut = C;
    struct Sums
    {
        T lane[C];
    };
    static Sums zero() noexcept { return Sums{}; }
    template <bool Masked>
    static Sums step(Sums sums, const SellSlices<T>& a, const T* x, crd::u32 off, crd::u32 w,
                     const crd::u32* lens) noexcept
    {
        for (crd::u32 lane = 0; lane < C; ++lane)
        {
            const T xv = *x_of<Masked>(x, a.col_idx[off + lane], w, lens[lane]);
            sums.lane[lane] = sums.lane[lane] + a.vals[off + lane] * xv;
        }
        return sums;
    }
    static void store(const Sums& sums, T (&out)[kOut]) noexcept
    {
        for (crd::u32 lane = 0; lane < C; ++lane)
        {
            out[lane] = sums.lane[lane];
        }
    }
};

template <typename T> struct ScalarComplexBody
{
    static constexpr crd::u32 kC = kSellSliceComplex;
    static constexpr crd::u32 kOut = 2U * kC;
    struct Sums
    {
        T re[kC];
        T im[kC];
    };
    static Sums zero() noexcept { return Sums{}; }
    template <bool Masked>
    static Sums step(Sums sums, const SellSlices<T>& a, const T* x, crd::u32 off, crd::u32 w,
                     const crd::u32* lens) noexcept
    {
        for (crd::u32 lane = 0; lane < kC; ++lane)
        {
            const T* v = a.vals + 2U * static_cast<crd::usize>(off + lane);
            const T* xv = x_of<Masked>(x, 2U * static_cast<crd::usize>(a.col_idx[off + lane]), w, lens[lane]);
            sums.re[lane] = sums.re[lane] + (v[0] * xv[0] - v[1] * xv[1]);
            sums.im[lane] = sums.im[lane] + (v[0] * xv[1] + v[1] * xv[0]);
        }
        return sums;
    }
    static void store(const Sums& sums, T (&out)[kOut]) noexcept
    {
        for (crd::u32 lane = 0; lane < kC; ++lane)
        {
            out[2U * lane] = sums.re[lane];
            out[2U * lane + 1U] = sums.im[lane];
        }
    }
};

#if CRD_SIMD_HAS_SSE2
// The (re, im) pairs of x for complex lanes `lane` and lane + 1 of an f32 slice column, in one 128-bit register.
template <bool Masked>
__m128 c32_x_pair(const crd::f32* x, const crd::u32* c, crd::u32 w, const crd::u32* lens, crd::u32 lane) noexcept
{
    const auto* lo =
        reinterpret_cast<const __m64*>(x_of<Masked>(x, 2U * static_cast<crd::usize>(c[lane]), w, lens[lane]));
    const auto* hi =
        reinterpret_cast<const __m64*>(x_of<Masked>(x, 2U * static_cast<crd::usize>(c[lane + 1U]), w, lens[lane + 1U]));
    return _mm_loadh_pi(_mm_loadl_pi(_mm_setzero_ps(), lo), hi);
}
#endif

#if CRD_SIMD_HAS_AVX2
struct Avx2F64Body
{
    static constexpr crd::u32 kC = kSellSliceF64;
    static constexpr crd::u32 kOut = kC;
    using Sums = __m256d;
    static Sums zero() noexcept { return _mm256_setzero_pd(); }
    template <bool Masked>
    static Sums step(Sums sums, const SellSlices<crd::f64>& a, const crd::f64* x, crd::u32 off, crd::u32 w,
                     const crd::u32* lens) noexcept
    {
        // Scalar loads: 6% to 20% ahead of vgatherdpd on L2-resident matrices and level on DRAM-resident ones.
        const crd::u32* c = a.col_idx + off;
        const __m256d xv = _mm256_setr_pd(*x_of<Masked>(x, c[0], w, lens[0]), *x_of<Masked>(x, c[1], w, lens[1]),
                                          *x_of<Masked>(x, c[2], w, lens[2]), *x_of<Masked>(x, c[3], w, lens[3]));
        return _mm256_add_pd(sums, _mm256_mul_pd(_mm256_loadu_pd(a.vals + off), xv));
    }
    static void store(Sums sums, crd::f64 (&out)[kOut]) noexcept { _mm256_storeu_pd(out, sums); }
};

struct Avx2F32Body
{
    static constexpr crd::u32 kC = kSellSliceF32;
    static constexpr crd::u32 kOut = kC;
    using Sums = __m256;
    static Sums zero() noexcept { return _mm256_setzero_ps(); }
    template <bool Masked>
    static Sums step(Sums sums, const SellSlices<crd::f32>& a, const crd::f32* x, crd::u32 off, crd::u32 w,
                     const crd::u32* lens) noexcept
    {
        const __m256i cols = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a.col_idx + off));
        __m256 xv;
        if constexpr (Masked)
        {
            // A lane is live where len >= w + 1 (unsigned); a padded lane gathers nothing and keeps the zero.
            const __m256i len = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(lens));
            const __m256i live =
                _mm256_cmpeq_epi32(_mm256_max_epu32(len, _mm256_set1_epi32(static_cast<int>(w + 1U))), len);
            xv = _mm256_mask_i32gather_ps(_mm256_setzero_ps(), x, cols, _mm256_castsi256_ps(live), 4);
        }
        else
        {
            xv = _mm256_i32gather_ps(x, cols, 4);
        }
        return _mm256_add_ps(sums, _mm256_mul_ps(_mm256_loadu_ps(a.vals + off), xv));
    }
    static void store(Sums sums, crd::f32 (&out)[kOut]) noexcept { _mm256_storeu_ps(out, sums); }
};

// Two complex lanes a register: (vr, vr) x (xr, xi) addsub (vi, vi) x (xi, xr).
struct Avx2C64Body
{
    static constexpr crd::u32 kC = kSellSliceComplex;
    static constexpr crd::u32 kOut = 2U * kC;
    struct Sums
    {
        __m256d lo; // lanes 0 and 1
        __m256d hi; // lanes 2 and 3
    };
    static Sums zero() noexcept { return Sums{_mm256_setzero_pd(), _mm256_setzero_pd()}; }
    static __m256d mul(__m256d v, __m256d xv) noexcept
    {
        return _mm256_addsub_pd(_mm256_mul_pd(_mm256_movedup_pd(v), xv),
                                _mm256_mul_pd(_mm256_permute_pd(v, 0xF), _mm256_permute_pd(xv, 0x5)));
    }
    template <bool Masked>
    static __m256d x_pair(const crd::f64* x, const crd::u32* c, crd::u32 w, const crd::u32* lens,
                          crd::u32 lane) noexcept
    {
        const __m128d lo = _mm_loadu_pd(x_of<Masked>(x, 2U * static_cast<crd::usize>(c[lane]), w, lens[lane]));
        const __m128d hi =
            _mm_loadu_pd(x_of<Masked>(x, 2U * static_cast<crd::usize>(c[lane + 1U]), w, lens[lane + 1U]));
        return _mm256_insertf128_pd(_mm256_castpd128_pd256(lo), hi, 1);
    }
    template <bool Masked>
    static Sums step(Sums sums, const SellSlices<crd::f64>& a, const crd::f64* x, crd::u32 off, crd::u32 w,
                     const crd::u32* lens) noexcept
    {
        const crd::u32* c = a.col_idx + off;
        const crd::f64* v = a.vals + 2U * static_cast<crd::usize>(off);
        sums.lo = _mm256_add_pd(sums.lo, mul(_mm256_loadu_pd(v), x_pair<Masked>(x, c, w, lens, 0)));
        sums.hi = _mm256_add_pd(sums.hi, mul(_mm256_loadu_pd(v + 4), x_pair<Masked>(x, c, w, lens, 2)));
        return sums;
    }
    static void store(const Sums& sums, crd::f64 (&out)[kOut]) noexcept
    {
        _mm256_storeu_pd(out, sums.lo);
        _mm256_storeu_pd(out + 4, sums.hi);
    }
};

// Four complex lanes a register, as Avx2C64Body.
struct Avx2C32Body
{
    static constexpr crd::u32 kC = kSellSliceComplex;
    static constexpr crd::u32 kOut = 2U * kC;
    using Sums = __m256;
    static Sums zero() noexcept { return _mm256_setzero_ps(); }
    template <bool Masked>
    static Sums step(Sums sums, const SellSlices<crd::f32>& a, const crd::f32* x, crd::u32 off, crd::u32 w,
                     const crd::u32* lens) noexcept
    {
        const crd::u32* c = a.col_idx + off;
        const __m256 v = _mm256_loadu_ps(a.vals + 2U * static_cast<crd::usize>(off));
        const __m256 xv = _mm256_insertf128_ps(_mm256_castps128_ps256(c32_x_pair<Masked>(x, c, w, lens, 0)),
                                               c32_x_pair<Masked>(x, c, w, lens, 2), 1);
        const __m256 p = _mm256_addsub_ps(_mm256_mul_ps(_mm256_moveldup_ps(v), xv),
                                          _mm256_mul_ps(_mm256_movehdup_ps(v), _mm256_permute_ps(xv, 0xB1)));
        return _mm256_add_ps(sums, p);
    }
    static void store(Sums sums, crd::f32 (&out)[kOut]) noexcept { _mm256_storeu_ps(out, sums); }
};

using F64Body = Avx2F64Body;
using F32Body = Avx2F32Body;
using C64Body = Avx2C64Body;
using C32Body = Avx2C32Body;
#elif CRD_SIMD_HAS_SSE2
struct Sse2F64Body
{
    static constexpr crd::u32 kC = kSellSliceF64;
    static constexpr crd::u32 kOut = kC;
    struct Sums
    {
        __m128d lo;
        __m128d hi;
    };
    static Sums zero() noexcept { return Sums{_mm_setzero_pd(), _mm_setzero_pd()}; }
    template <bool Masked>
    static Sums step(Sums sums, const SellSlices<crd::f64>& a, const crd::f64* x, crd::u32 off, crd::u32 w,
                     const crd::u32* lens) noexcept
    {
        const crd::u32* c = a.col_idx + off;
        const __m128d x_lo =
            _mm_loadh_pd(_mm_load_sd(x_of<Masked>(x, c[0], w, lens[0])), x_of<Masked>(x, c[1], w, lens[1]));
        const __m128d x_hi =
            _mm_loadh_pd(_mm_load_sd(x_of<Masked>(x, c[2], w, lens[2])), x_of<Masked>(x, c[3], w, lens[3]));
        sums.lo = _mm_add_pd(sums.lo, _mm_mul_pd(_mm_loadu_pd(a.vals + off), x_lo));
        sums.hi = _mm_add_pd(sums.hi, _mm_mul_pd(_mm_loadu_pd(a.vals + off + 2U), x_hi));
        return sums;
    }
    static void store(const Sums& sums, crd::f64 (&out)[kOut]) noexcept
    {
        _mm_storeu_pd(out, sums.lo);
        _mm_storeu_pd(out + 2, sums.hi);
    }
};

struct Sse2F32Body
{
    static constexpr crd::u32 kC = kSellSliceF32;
    static constexpr crd::u32 kOut = kC;
    struct Sums
    {
        __m128 lo;
        __m128 hi;
    };
    static Sums zero() noexcept { return Sums{_mm_setzero_ps(), _mm_setzero_ps()}; }
    template <bool Masked>
    static Sums step(Sums sums, const SellSlices<crd::f32>& a, const crd::f32* x, crd::u32 off, crd::u32 w,
                     const crd::u32* lens) noexcept
    {
        const crd::u32* c = a.col_idx + off;
        alignas(16) crd::f32 xg[kC]; // scalar loads, then two vector loads: 2% ahead of four-lane shuffles in L2
        for (crd::u32 lane = 0; lane < kC; ++lane)
        {
            xg[lane] = *x_of<Masked>(x, c[lane], w, lens[lane]);
        }
        sums.lo = _mm_add_ps(sums.lo, _mm_mul_ps(_mm_loadu_ps(a.vals + off), _mm_load_ps(xg)));
        sums.hi = _mm_add_ps(sums.hi, _mm_mul_ps(_mm_loadu_ps(a.vals + off + 4U), _mm_load_ps(xg + 4)));
        return sums;
    }
    static void store(const Sums& sums, crd::f32 (&out)[kOut]) noexcept
    {
        _mm_storeu_ps(out, sums.lo);
        _mm_storeu_ps(out + 4, sums.hi);
    }
};

// One complex lane a register: (vr, vr) x (xr, xi) plus (-(vi xi), vi xr).
struct Sse2C64Body
{
    static constexpr crd::u32 kC = kSellSliceComplex;
    static constexpr crd::u32 kOut = 2U * kC;
    struct Sums
    {
        __m128d lane[kC];
    };
    static Sums zero() noexcept
    {
        return Sums{{_mm_setzero_pd(), _mm_setzero_pd(), _mm_setzero_pd(), _mm_setzero_pd()}};
    }
    template <bool Masked>
    static Sums step(Sums sums, const SellSlices<crd::f64>& a, const crd::f64* x, crd::u32 off, crd::u32 w,
                     const crd::u32* lens) noexcept
    {
        const __m128d negate_re = _mm_set_pd(0.0, -0.0);
        for (crd::u32 lane = 0; lane < kC; ++lane)
        {
            const __m128d v = _mm_loadu_pd(a.vals + 2U * static_cast<crd::usize>(off + lane));
            const __m128d xv =
                _mm_loadu_pd(x_of<Masked>(x, 2U * static_cast<crd::usize>(a.col_idx[off + lane]), w, lens[lane]));
            const __m128d re_im = _mm_mul_pd(_mm_unpacklo_pd(v, v), xv);
            const __m128d im_re = _mm_mul_pd(_mm_unpackhi_pd(v, v), _mm_shuffle_pd(xv, xv, 1));
            sums.lane[lane] = _mm_add_pd(sums.lane[lane], _mm_add_pd(re_im, _mm_xor_pd(im_re, negate_re)));
        }
        return sums;
    }
    static void store(const Sums& sums, crd::f64 (&out)[kOut]) noexcept
    {
        for (crd::u32 lane = 0; lane < kC; ++lane)
        {
            _mm_storeu_pd(out + 2U * lane, sums.lane[lane]);
        }
    }
};

// Two complex lanes a register, as Sse2C64Body.
struct Sse2C32Body
{
    static constexpr crd::u32 kC = kSellSliceComplex;
    static constexpr crd::u32 kOut = 2U * kC;
    struct Sums
    {
        __m128 lo; // lanes 0 and 1
        __m128 hi; // lanes 2 and 3
    };
    static Sums zero() noexcept { return Sums{_mm_setzero_ps(), _mm_setzero_ps()}; }
    static __m128 mul(__m128 v, __m128 xv) noexcept
    {
        const __m128 re_im = _mm_mul_ps(_mm_shuffle_ps(v, v, _MM_SHUFFLE(2, 2, 0, 0)), xv);
        const __m128 im_re =
            _mm_mul_ps(_mm_shuffle_ps(v, v, _MM_SHUFFLE(3, 3, 1, 1)), _mm_shuffle_ps(xv, xv, _MM_SHUFFLE(2, 3, 0, 1)));
        return _mm_add_ps(re_im, _mm_xor_ps(im_re, _mm_setr_ps(-0.0F, 0.0F, -0.0F, 0.0F)));
    }
    template <bool Masked>
    static Sums step(Sums sums, const SellSlices<crd::f32>& a, const crd::f32* x, crd::u32 off, crd::u32 w,
                     const crd::u32* lens) noexcept
    {
        const crd::u32* c = a.col_idx + off;
        const crd::f32* v = a.vals + 2U * static_cast<crd::usize>(off);
        sums.lo = _mm_add_ps(sums.lo, mul(_mm_loadu_ps(v), c32_x_pair<Masked>(x, c, w, lens, 0)));
        sums.hi = _mm_add_ps(sums.hi, mul(_mm_loadu_ps(v + 4), c32_x_pair<Masked>(x, c, w, lens, 2)));
        return sums;
    }
    static void store(const Sums& sums, crd::f32 (&out)[kOut]) noexcept
    {
        _mm_storeu_ps(out, sums.lo);
        _mm_storeu_ps(out + 4, sums.hi);
    }
};

using F64Body = Sse2F64Body;
using F32Body = Sse2F32Body;
using C64Body = Sse2C64Body;
using C32Body = Sse2C32Body;
#else
using F64Body = ScalarBody<crd::f64, kSellSliceF64>;
using F32Body = ScalarBody<crd::f32, kSellSliceF32>;
using C64Body = ScalarComplexBody<crd::f64>;
using C32Body = ScalarComplexBody<crd::f32>;
#endif

template <typename Body, typename T>
void sell_spmv_real(const SellSlices<T>& a, T alpha, T beta, const T* x, T* y, crd::u32 s_begin,
                    crd::u32 s_end) noexcept
{
    sell_spmv<Body>(a, RealWrite<T>{alpha, beta, beta == T(0)}, x, y, s_begin, s_end);
}

template <typename Body, typename T>
void sell_spmv_complex(const SellSlices<T>& a, const SellComplexScales<T>& scales, const T* x, T* y, crd::u32 s_begin,
                       crd::u32 s_end) noexcept
{
    const bool beta_zero = scales.beta_re == T(0) && scales.beta_im == T(0);
    sell_spmv<Body>(a, ComplexWrite<T>{scales, beta_zero}, x, y, s_begin, s_end);
}

} // namespace

constexpr auto kSellSpmvF64 = &sell_spmv_real<F64Body, crd::f64>;
constexpr auto kSellSpmvF32 = &sell_spmv_real<F32Body, crd::f32>;
constexpr auto kSellSpmvC64 = &sell_spmv_complex<C64Body, crd::f64>;
constexpr auto kSellSpmvC32 = &sell_spmv_complex<C32Body, crd::f32>;

} // namespace crd::hesap::sparse::inline CRD_MATH_LEVEL_NS

namespace crd::hesap::sparse
{

const SparseKernels CRD_KERNEL_TABLE(kSparseKernels) = {
    CRD_MATH_LEVEL_NS::kSellSpmvF64,
    CRD_MATH_LEVEL_NS::kSellSpmvF32,
    CRD_MATH_LEVEL_NS::kSellSpmvC64,
    CRD_MATH_LEVEL_NS::kSellSpmvC32,
};

} // namespace crd::hesap::sparse
