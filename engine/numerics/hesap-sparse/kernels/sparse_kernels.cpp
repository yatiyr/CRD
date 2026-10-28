// The hesap-sparse kernels of crd/hesap/sparse/sparse_kernels.hpp, compiled once per CPU level by crd_add_cpu_kernels
// (../CMakeLists.txt, ADR-0134 §5). Everything here lives in the level's namespace (CRD_MATH_LEVEL_NS).
//
// Each level and type picks per sweep (SellSweep) whether wide slices run two at a time, so two independent sets of
// lane sums are in flight, and how x is gathered; each lane adds vals * x[col] to its row's sum column by column with
// two roundings (D(sparse)-3: never a fused multiply-add). The portable bodies keep the lane sums in arrays; the SSE2
// bodies in 128-bit registers, gathering x by scalar loads; the AVX2 bodies in 256-bit registers, gathering complex x
// by scalar loads, f32 x with vgatherdps and f64 x by scalar loads in L2 and vgatherdpd beyond it (each pick the
// faster form on the 2026-10-27 board; a gather's signed index needs cols <= 2^31, scalar loads serve wider x). The
// AVX-512 bodies run a pair's shared full columns in one ZMM (slice s low, s + 1 high, gathering both slices' x in one
// instruction), then finish each slice in the 256-bit body, which masks its gathers with an opmask; a complex f64
// slice fills one ZMM alone. A complex product is (vr xr - vi xi, vr xi + vi xr), SSE2 adding the negated
// vi xi as it has no addsub. The columns every row of a slice stores run unmasked; past the shortest row a padded
// lane multiplies its padding 0 by a zero instead of x[0], so an infinite or NaN x[0] reaches only the rows that
// store column 0, and adding that +0 leaves the sum's bits (a sum from +0 is never -0). A Delta16 slice walks each
// lane's column by its 16-bit gaps (scalar adds; the gathering AVX2 bodies widen eight or four gaps in one add) and a
// pair runs two slices of one column form, so the column form changes only how the columns are read. The sums are
// written out by one scalar expression on every level, so every level gives the CSR spmv's bits.

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

template <typename Body> constexpr bool gathers() noexcept
{
    if constexpr (requires { Body::kGathers; })
    {
        return Body::kGathers;
    }
    else
    {
        return false;
    }
}

// The columns of a slice, one column w after another: next() gives the C lanes' columns of the next w.
// A U32 slice: its stored columns.
template <crd::u32 C> struct U32Columns
{
    const crd::u32* at;
    const crd::u32* next() noexcept
    {
        const crd::u32* c = at;
        at += C;
        return c;
    }
};

// A Delta16 slice: each lane's running column, which next() moves by the lane's gap (a load and an add a lane).
template <crd::u32 C> struct Delta16Columns
{
    alignas(32) crd::u32 col[C];
    const crd::u16* gap;
    const crd::u32* next() noexcept
    {
        for (crd::u32 lane = 0; lane < C; ++lane)
        {
            col[lane] += gap[lane];
        }
        gap += C;
        return col;
    }
};

#if CRD_SIMD_HAS_AVX2
template <bool First, typename A, typename B> struct Pick
{
    using Type = A;
};
template <typename A, typename B> struct Pick<false, A, B>
{
    using Type = B;
};

// The same in one widening add, for the bodies that gather x with the columns as one vector index.
template <crd::u32 C> struct VectorDelta16Columns
{
    static_assert(C == 4U || C == 8U, "a vector index of four or eight lanes");
    alignas(32) crd::u32 col[C];
    const crd::u16* gap;
    const crd::u32* next() noexcept
    {
        if constexpr (C == 4U)
        {
            const __m128i g = _mm_cvtepu16_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(gap)));
            auto* run = reinterpret_cast<__m128i*>(col);
            _mm_store_si128(run, _mm_add_epi32(_mm_load_si128(run), g));
        }
        else
        {
            const __m256i g = _mm256_cvtepu16_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(gap)));
            auto* run = reinterpret_cast<__m256i*>(col);
            _mm256_store_si256(run, _mm256_add_epi32(_mm256_load_si256(run), g));
        }
        gap += C;
        return col;
    }
};
#endif

// Whether slice s stores its columns as Delta16.
template <typename T> bool delta16(const SellSlices<T>& a, crd::u32 s) noexcept
{
    return a.delta_ptr[s + 1U] != a.delta_ptr[s];
}

// The column walk of slice s for Body: Delta16 or U32.
template <typename Body, bool Delta, typename T> auto slice_columns(const SellSlices<T>& a, crd::u32 s) noexcept
{
    constexpr crd::u32 height = Body::kC;
    if constexpr (!Delta)
    {
        return U32Columns<height>{a.col_idx + (a.slice_ptr[s] - a.delta_ptr[s])};
    }
    else
    {
#if CRD_SIMD_HAS_AVX2
        using Walk = typename Pick<gathers<Body>(), VectorDelta16Columns<height>, Delta16Columns<height>>::Type;
#else
        using Walk = Delta16Columns<height>;
#endif
        Walk walk;
        const crd::u32* first = a.col_first + static_cast<crd::usize>(s) * height;
        for (crd::u32 lane = 0; lane < height; ++lane)
        {
            walk.col[lane] = first[lane];
        }
        walk.gap = a.col_delta + a.delta_ptr[s];
        return walk;
    }
}

// Columns [from, width) of slice s added to sums, the walk at column from: unmasked up to `full`, masked after it.
template <typename Body, typename T, typename Walk>
typename Body::Sums finish(typename Body::Sums sums, const SellSlices<T>& a, const T* x, crd::u32 s, crd::u32 from,
                           crd::u32 full, Walk& walk) noexcept
{
    constexpr crd::u32 height = Body::kC;
    const crd::u32 base = a.slice_ptr[s];
    const crd::u32 width = a.slice_width[s];
    const crd::u32* lens = a.row_len + static_cast<crd::usize>(s) * height;
    for (crd::u32 w = from; w < full; ++w)
    {
        sums = Body::template step<false>(sums, a, x, base + w * height, walk.next(), w, lens);
    }
    for (crd::u32 w = full; w < width; ++w)
    {
        sums = Body::template step<true>(sums, a, x, base + w * height, walk.next(), w, lens);
    }
    return sums;
}

// Slice s alone, its sums stored to out.
template <typename Body, bool Delta, typename T>
void run_slice(const SellSlices<T>& a, const T* x, crd::u32 s, T (&out)[Body::kOut]) noexcept
{
    constexpr crd::u32 height = Body::kC;
    auto walk = slice_columns<Body, Delta>(a, s);
    const crd::u32 full = full_columns<height>(a.row_len + static_cast<crd::usize>(s) * height);
    Body::store(finish<Body>(Body::zero(), a, x, s, 0, full, walk), out);
}

// Slices s and s + 1 of one column form as a pair, two independent sets of sums in flight.
template <typename Body, bool Delta, typename T>
void run_pair(const SellSlices<T>& a, const T* x, crd::u32 s, T (&out0)[Body::kOut], T (&out1)[Body::kOut]) noexcept
{
    constexpr crd::u32 height = Body::kC;
    auto walk0 = slice_columns<Body, Delta>(a, s);
    auto walk1 = slice_columns<Body, Delta>(a, s + 1U);
    const crd::u32* lens0 = a.row_len + static_cast<crd::usize>(s) * height;
    const crd::u32* lens1 = lens0 + height;
    const crd::u32 full0 = full_columns<height>(lens0);
    const crd::u32 full1 = full_columns<height>(lens1);
    const crd::u32 b0 = a.slice_ptr[s];
    const crd::u32 b1 = a.slice_ptr[s + 1U];
    const crd::u32 both = full0 < full1 ? full0 : full1;
    typename Body::Sums sum0 = Body::zero();
    typename Body::Sums sum1 = Body::zero();
    if constexpr (requires { typename Body::Pair; })
    {
        // Both slices' lanes in one register over the columns both store, then each slice's own sums.
        using Pair = typename Body::Pair;
        typename Pair::Sums sums = Pair::zero();
        for (crd::u32 w = 0; w < both; ++w)
        {
            const crd::u32* c0 = walk0.next();
            sums = Pair::step(sums, a, x, b0 + w * height, c0, b1 + w * height, walk1.next());
        }
        Pair::split(sums, sum0, sum1);
    }
    else
    {
        for (crd::u32 w = 0; w < both; ++w)
        {
            sum0 = Body::template step<false>(sum0, a, x, b0 + w * height, walk0.next(), w, lens0);
            sum1 = Body::template step<false>(sum1, a, x, b1 + w * height, walk1.next(), w, lens1);
        }
    }
    Body::store(finish<Body>(sum0, a, x, s, both, full0, walk0), out0);
    Body::store(finish<Body>(sum1, a, x, s + 1U, both, full1, walk1), out1);
}

// Two slices of one column form both at least this wide run as a pair; narrower ones run alone, where a pair lost 4%
// to 15% on DRAM-resident matrices and gained nothing in L2 (2026-10-27 board). kNoPairs: every slice runs alone.
inline constexpr crd::u32 kPairWidth = 8;
inline constexpr crd::u32 kNoPairs = 0xFFFFFFFFU;

// The driver. Body: the lane sums of one slice (Sums), zero(), step<Masked>(sums, a, x, offset, c, w, lens) for column
// w of the C lanes at element offset, their columns at c, and store(sums, out) of its kOut values (C rows of
// Write::kParts).
template <typename Body, crd::u32 PairWidth, typename T, typename Write>
void sell_spmv(const SellSlices<T>& a, const Write& write, const T* x, T* y, crd::u32 s_begin, crd::u32 s_end) noexcept
{
    constexpr crd::u32 height = Body::kC;
    static_assert(Body::kOut == height * Write::kParts, "a body stores one sum per part of each row");
    T out0[Body::kOut];
    T out1[Body::kOut];
    crd::u32 s = s_begin;
    while (s < s_end)
    {
        const bool delta = delta16(a, s);
        const bool pair = PairWidth != kNoPairs && s + 1U < s_end && a.slice_width[s] >= PairWidth &&
                          a.slice_width[s + 1U] >= PairWidth && delta16(a, s + 1U) == delta;
        if (!pair)
        {
            if (delta)
            {
                run_slice<Body, true>(a, x, s, out0);
            }
            else
            {
                run_slice<Body, false>(a, x, s, out0);
            }
            write_slice<height>(a, write, s, out0, y);
            s += 1U;
            continue;
        }
        if (delta)
        {
            run_pair<Body, true>(a, x, s, out0, out1);
        }
        else
        {
            run_pair<Body, false>(a, x, s, out0, out1);
        }
        write_slice<height>(a, write, s, out0, y);
        write_slice<height>(a, write, s + 1U, out1, y);
        s += 2U;
    }
}

// A vector gather's signed 32-bit index reaches the first 2^31 columns of x; a wider x takes the body's scalar loads.
inline constexpr crd::u32 kGatherColumns = 0x80000000U;

// One sweep's pick: Body at PairWidth, its scalar-load form where x is too wide for a gather.
template <typename Body, crd::u32 PairWidth, typename T, typename Write>
void run_sweep(const SellSlices<T>& a, const Write& write, const T* x, T* y, crd::u32 s_begin, crd::u32 s_end) noexcept
{
    if constexpr (gathers<Body>())
    {
        if (a.cols > kGatherColumns)
        {
            sell_spmv<typename Body::Loads, PairWidth>(a, write, x, y, s_begin, s_end);
            return;
        }
    }
    sell_spmv<Body, PairWidth>(a, write, x, y, s_begin, s_end);
}

// A level's pick for one type: the body and pair width of each sweep.
template <typename ResidentBody, crd::u32 ResidentPairs, typename StreamingBody, crd::u32 StreamingPairs>
struct SellPolicy
{
    template <typename T, typename Write>
    static void run(SellSweep sweep, const SellSlices<T>& a, const Write& write, const T* x, T* y, crd::u32 s_begin,
                    crd::u32 s_end) noexcept
    {
        if (sweep == SellSweep::Resident)
        {
            run_sweep<ResidentBody, ResidentPairs>(a, write, x, y, s_begin, s_end);
        }
        else
        {
            run_sweep<StreamingBody, StreamingPairs>(a, write, x, y, s_begin, s_end);
        }
    }
};

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
    static Sums step(Sums sums, const SellSlices<T>& a, const T* x, crd::u32 off, const crd::u32* c, crd::u32 w,
                     const crd::u32* lens) noexcept
    {
        for (crd::u32 lane = 0; lane < C; ++lane)
        {
            const T xv = *x_of<Masked>(x, c[lane], w, lens[lane]);
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
    static Sums step(Sums sums, const SellSlices<T>& a, const T* x, crd::u32 off, const crd::u32* c, crd::u32 w,
                     const crd::u32* lens) noexcept
    {
        for (crd::u32 lane = 0; lane < kC; ++lane)
        {
            const T* v = a.vals + 2U * static_cast<crd::usize>(off + lane);
            const T* xv = x_of<Masked>(x, 2U * static_cast<crd::usize>(c[lane]), w, lens[lane]);
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
// Two (re, im) pairs of f32 x in one 128-bit register: the pair at lo, then the pair at hi.
__m128 c32_two(const crd::f32* lo, const crd::f32* hi) noexcept
{
    return _mm_loadh_pi(_mm_loadl_pi(_mm_setzero_ps(), reinterpret_cast<const __m64*>(lo)),
                        reinterpret_cast<const __m64*>(hi));
}

// The (re, im) pairs of x for complex lanes `lane` and lane + 1 of an f32 slice column, in one 128-bit register.
template <bool Masked>
__m128 c32_x_pair(const crd::f32* x, const crd::u32* c, crd::u32 w, const crd::u32* lens, crd::u32 lane) noexcept
{
    return c32_two(x_of<Masked>(x, 2U * static_cast<crd::usize>(c[lane]), w, lens[lane]),
                   x_of<Masked>(x, 2U * static_cast<crd::usize>(c[lane + 1U]), w, lens[lane + 1U]));
}
#endif

#if CRD_SIMD_HAS_AVX512
// The lanes of a masked column that store a value, len > w (unsigned), as an opmask over four or eight lanes.
__mmask8 live_mask4(const crd::u32* lens, crd::u32 w) noexcept
{
    return _mm_cmpgt_epu32_mask(_mm_loadu_si128(reinterpret_cast<const __m128i*>(lens)),
                                _mm_set1_epi32(static_cast<int>(w)));
}
__mmask8 live_mask8(const crd::u32* lens, crd::u32 w) noexcept
{
    return _mm256_cmpgt_epu32_mask(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(lens)),
                                   _mm256_set1_epi32(static_cast<int>(w)));
}
#elif CRD_SIMD_HAS_AVX2
// The lanes of a masked column that store a value: len >= w + 1 (unsigned), all ones; a padded lane, zero.
__m128i live_lanes(const crd::u32* lens, crd::u32 w) noexcept
{
    const __m128i len = _mm_loadu_si128(reinterpret_cast<const __m128i*>(lens));
    return _mm_cmpeq_epi32(_mm_max_epu32(len, _mm_set1_epi32(static_cast<int>(w + 1U))), len);
}
#endif

#if CRD_SIMD_HAS_AVX2

// Gather: vgatherdpd (a padded lane gathers nothing and keeps the zero); else scalar loads.
template <bool Gather> struct Avx2F64Body
{
    static constexpr crd::u32 kC = kSellSliceF64;
    static constexpr crd::u32 kOut = kC;
    static constexpr bool kGathers = Gather;
    using Loads = Avx2F64Body<false>;
    using Sums = __m256d;
    static Sums zero() noexcept { return _mm256_setzero_pd(); }
    template <bool Masked>
    static Sums step(Sums sums, const SellSlices<crd::f64>& a, const crd::f64* x, crd::u32 off, const crd::u32* c,
                     crd::u32 w, const crd::u32* lens) noexcept
    {
        __m256d xv;
        if constexpr (!Gather)
        {
            xv = _mm256_setr_pd(*x_of<Masked>(x, c[0], w, lens[0]), *x_of<Masked>(x, c[1], w, lens[1]),
                                *x_of<Masked>(x, c[2], w, lens[2]), *x_of<Masked>(x, c[3], w, lens[3]));
        }
        else if constexpr (Masked)
        {
            const __m128i index = _mm_loadu_si128(reinterpret_cast<const __m128i*>(c));
#if CRD_SIMD_HAS_AVX512
            xv = _mm256_mmask_i32gather_pd(_mm256_setzero_pd(), live_mask4(lens, w), index, x, 8);
#else
            const __m256d live = _mm256_castsi256_pd(_mm256_cvtepi32_epi64(live_lanes(lens, w)));
            xv = _mm256_mask_i32gather_pd(_mm256_setzero_pd(), x, index, live, 8);
#endif
        }
        else
        {
            xv = _mm256_i32gather_pd(x, _mm_loadu_si128(reinterpret_cast<const __m128i*>(c)), 8);
        }
        return _mm256_add_pd(sums, _mm256_mul_pd(_mm256_loadu_pd(a.vals + off), xv));
    }
    static void store(Sums sums, crd::f64 (&out)[kOut]) noexcept { _mm256_storeu_pd(out, sums); }
};

// Gather: vgatherdps, as Avx2F64Body; else scalar loads.
template <bool Gather> struct Avx2F32Body
{
    static constexpr crd::u32 kC = kSellSliceF32;
    static constexpr crd::u32 kOut = kC;
    static constexpr bool kGathers = Gather;
    using Loads = Avx2F32Body<false>;
    using Sums = __m256;
    static Sums zero() noexcept { return _mm256_setzero_ps(); }
    template <bool Masked>
    static Sums step(Sums sums, const SellSlices<crd::f32>& a, const crd::f32* x, crd::u32 off, const crd::u32* c,
                     crd::u32 w, const crd::u32* lens) noexcept
    {
        __m256 xv;
        if constexpr (!Gather)
        {
            alignas(32) crd::f32 xg[kC];
            for (crd::u32 lane = 0; lane < kC; ++lane)
            {
                xg[lane] = *x_of<Masked>(x, c[lane], w, lens[lane]);
            }
            xv = _mm256_load_ps(xg);
        }
        else if constexpr (Masked)
        {
            const __m256i index = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(c));
#if CRD_SIMD_HAS_AVX512
            xv = _mm256_mmask_i32gather_ps(_mm256_setzero_ps(), live_mask8(lens, w), index, x, 4);
#else
            const __m256i len = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(lens));
            const __m256i live =
                _mm256_cmpeq_epi32(_mm256_max_epu32(len, _mm256_set1_epi32(static_cast<int>(w + 1U))), len);
            xv = _mm256_mask_i32gather_ps(_mm256_setzero_ps(), x, index, _mm256_castsi256_ps(live), 4);
#endif
        }
        else
        {
            xv = _mm256_i32gather_ps(x, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(c)), 4);
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
    static Sums step(Sums sums, const SellSlices<crd::f64>& a, const crd::f64* x, crd::u32 off, const crd::u32* c,
                     crd::u32 w, const crd::u32* lens) noexcept
    {
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
    static Sums step(Sums sums, const SellSlices<crd::f32>& a, const crd::f32* x, crd::u32 off, const crd::u32* c,
                     crd::u32 w, const crd::u32* lens) noexcept
    {
        const __m256 v = _mm256_loadu_ps(a.vals + 2U * static_cast<crd::usize>(off));
        const __m256 xv = _mm256_insertf128_ps(_mm256_castps128_ps256(c32_x_pair<Masked>(x, c, w, lens, 0)),
                                               c32_x_pair<Masked>(x, c, w, lens, 2), 1);
        const __m256 p = _mm256_addsub_ps(_mm256_mul_ps(_mm256_moveldup_ps(v), xv),
                                          _mm256_mul_ps(_mm256_movehdup_ps(v), _mm256_permute_ps(xv, 0xB1)));
        return _mm256_add_ps(sums, p);
    }
    static void store(Sums sums, crd::f32 (&out)[kOut]) noexcept { _mm256_storeu_ps(out, sums); }
};

#if CRD_SIMD_HAS_AVX512
// addsub on 512 bits: a - b in the even lanes (each complex value's real part), a + b in the odd ones.
__m512d addsub(__m512d a, __m512d b) noexcept
{
    return _mm512_mask_sub_pd(_mm512_add_pd(a, b), 0x55, a, b);
}
__m512 addsub(__m512 a, __m512 b) noexcept
{
    return _mm512_mask_sub_ps(_mm512_add_ps(a, b), 0x5555, a, b);
}

// A pair's step in one ZMM: slice s's lanes low, slice s + 1's high, each lane the 256-bit body's chain. The pair runs
// only the columns both slices store in full, so no lane is masked; the 256-bit body finishes each slice.
template <bool Gather> struct Avx512F64Body : Avx2F64Body<Gather>
{
    using Loads = Avx512F64Body<false>;
    struct Pair
    {
        using Sums = __m512d;
        static Sums zero() noexcept { return _mm512_setzero_pd(); }
        static Sums step(Sums sums, const SellSlices<crd::f64>& a, const crd::f64* x, crd::u32 off0, const crd::u32* c0,
                         crd::u32 off1, const crd::u32* c1) noexcept
        {
            __m512d xv;
            if constexpr (Gather)
            {
                const __m256i index = _mm256_inserti128_si256(
                    _mm256_castsi128_si256(_mm_loadu_si128(reinterpret_cast<const __m128i*>(c0))),
                    _mm_loadu_si128(reinterpret_cast<const __m128i*>(c1)), 1);
                xv = _mm512_i32gather_pd(index, x, 8);
            }
            else
            {
                xv = _mm512_setr_pd(x[c0[0]], x[c0[1]], x[c0[2]], x[c0[3]], x[c1[0]], x[c1[1]], x[c1[2]], x[c1[3]]);
            }
            const __m512d v = _mm512_insertf64x4(_mm512_castpd256_pd512(_mm256_loadu_pd(a.vals + off0)),
                                                 _mm256_loadu_pd(a.vals + off1), 1);
            return _mm512_add_pd(sums, _mm512_mul_pd(v, xv));
        }
        static void split(Sums sums, __m256d& lo, __m256d& hi) noexcept
        {
            lo = _mm512_castpd512_pd256(sums);
            hi = _mm512_extractf64x4_pd(sums, 1);
        }
    };
};

// The same for f32: sixteen lanes, one vgatherdps of both slices' columns.
template <bool Gather> struct Avx512F32Body : Avx2F32Body<Gather>
{
    using Loads = Avx512F32Body<false>;
    struct Pair
    {
        using Sums = __m512;
        static Sums zero() noexcept { return _mm512_setzero_ps(); }
        static Sums step(Sums sums, const SellSlices<crd::f32>& a, const crd::f32* x, crd::u32 off0, const crd::u32* c0,
                         crd::u32 off1, const crd::u32* c1) noexcept
        {
            __m512 xv;
            if constexpr (Gather)
            {
                const __m512i index =
                    _mm512_inserti64x4(_mm512_castsi256_si512(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(c0))),
                                       _mm256_loadu_si256(reinterpret_cast<const __m256i*>(c1)), 1);
                xv = _mm512_i32gather_ps(index, x, 4);
            }
            else
            {
                alignas(64) crd::f32 xg[2U * kSellSliceF32];
                for (crd::u32 lane = 0; lane < kSellSliceF32; ++lane)
                {
                    xg[lane] = x[c0[lane]];
                    xg[kSellSliceF32 + lane] = x[c1[lane]];
                }
                xv = _mm512_load_ps(xg);
            }
            const __m512 v = _mm512_insertf32x8(_mm512_castps256_ps512(_mm256_loadu_ps(a.vals + off0)),
                                                _mm256_loadu_ps(a.vals + off1), 1);
            return _mm512_add_ps(sums, _mm512_mul_ps(v, xv));
        }
        static void split(Sums sums, __m256& lo, __m256& hi) noexcept
        {
            lo = _mm512_castps512_ps256(sums);
            hi = _mm512_extractf32x8_ps(sums, 1);
        }
    };
};

// The four complex f64 lanes of a slice in one register, as Avx2C64Body.
struct Avx512C64Body
{
    static constexpr crd::u32 kC = kSellSliceComplex;
    static constexpr crd::u32 kOut = 2U * kC;
    using Sums = __m512d;
    static Sums zero() noexcept { return _mm512_setzero_pd(); }
    template <bool Masked>
    static Sums step(Sums sums, const SellSlices<crd::f64>& a, const crd::f64* x, crd::u32 off, const crd::u32* c,
                     crd::u32 w, const crd::u32* lens) noexcept
    {
        const __m512d v = _mm512_loadu_pd(a.vals + 2U * static_cast<crd::usize>(off));
        const __m512d xv = _mm512_insertf64x4(_mm512_castpd256_pd512(Avx2C64Body::x_pair<Masked>(x, c, w, lens, 0)),
                                              Avx2C64Body::x_pair<Masked>(x, c, w, lens, 2), 1);
        const __m512d p = addsub(_mm512_mul_pd(_mm512_movedup_pd(v), xv),
                                 _mm512_mul_pd(_mm512_permute_pd(v, 0xFF), _mm512_permute_pd(xv, 0x55)));
        return _mm512_add_pd(sums, p);
    }
    static void store(Sums sums, crd::f64 (&out)[kOut]) noexcept { _mm512_storeu_pd(out, sums); }
};

// A pair of complex f32 slices in one register, as Avx512F64Body.
struct Avx512C32Body : Avx2C32Body
{
    struct Pair
    {
        using Sums = __m512;
        static Sums zero() noexcept { return _mm512_setzero_ps(); }
        // The x pairs of a slice column's four complex lanes.
        static __m256 x_lanes(const crd::f32* x, const crd::u32* c) noexcept
        {
            const auto at = [x, c](crd::u32 lane)
            {
                return x + 2U * static_cast<crd::usize>(c[lane]);
            };
            return _mm256_insertf128_ps(_mm256_castps128_ps256(c32_two(at(0), at(1))), c32_two(at(2), at(3)), 1);
        }
        static Sums step(Sums sums, const SellSlices<crd::f32>& a, const crd::f32* x, crd::u32 off0, const crd::u32* c0,
                         crd::u32 off1, const crd::u32* c1) noexcept
        {
            const __m512 v =
                _mm512_insertf32x8(_mm512_castps256_ps512(_mm256_loadu_ps(a.vals + 2U * static_cast<crd::usize>(off0))),
                                   _mm256_loadu_ps(a.vals + 2U * static_cast<crd::usize>(off1)), 1);
            const __m512 xv = _mm512_insertf32x8(_mm512_castps256_ps512(x_lanes(x, c0)), x_lanes(x, c1), 1);
            const __m512 p = addsub(_mm512_mul_ps(_mm512_moveldup_ps(v), xv),
                                    _mm512_mul_ps(_mm512_movehdup_ps(v), _mm512_permute_ps(xv, 0xB1)));
            return _mm512_add_ps(sums, p);
        }
        static void split(Sums sums, __m256& lo, __m256& hi) noexcept
        {
            lo = _mm512_castps512_ps256(sums);
            hi = _mm512_extractf32x8_ps(sums, 1);
        }
    };
};

using F64Policy = SellPolicy<Avx512F64Body<false>, kPairWidth, Avx512F64Body<true>, kPairWidth>;
using F32Policy = SellPolicy<Avx512F32Body<true>, kPairWidth, Avx512F32Body<true>, kPairWidth>;
using C64Policy = SellPolicy<Avx512C64Body, kPairWidth, Avx512C64Body, kPairWidth>;
using C32Policy = SellPolicy<Avx512C32Body, kPairWidth, Avx512C32Body, kPairWidth>;
#else
// f64: scalar loads 6% to 20% ahead of vgatherdpd in L2, vgatherdpd ahead from DRAM (power-law 1M 12.49 against 13.52
// to 13.67 ms).
using F64Policy = SellPolicy<Avx2F64Body<false>, kPairWidth, Avx2F64Body<true>, kPairWidth>;
using F32Policy = SellPolicy<Avx2F32Body<true>, kPairWidth, Avx2F32Body<true>, kPairWidth>;
using C64Policy = SellPolicy<Avx2C64Body, kPairWidth, Avx2C64Body, kPairWidth>;
using C32Policy = SellPolicy<Avx2C32Body, kPairWidth, Avx2C32Body, kPairWidth>;
#endif
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
    static Sums step(Sums sums, const SellSlices<crd::f64>& a, const crd::f64* x, crd::u32 off, const crd::u32* c,
                     crd::u32 w, const crd::u32* lens) noexcept
    {
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
    static Sums step(Sums sums, const SellSlices<crd::f32>& a, const crd::f32* x, crd::u32 off, const crd::u32* c,
                     crd::u32 w, const crd::u32* lens) noexcept
    {
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
    static Sums step(Sums sums, const SellSlices<crd::f64>& a, const crd::f64* x, crd::u32 off, const crd::u32* c,
                     crd::u32 w, const crd::u32* lens) noexcept
    {
        const __m128d negate_re = _mm_set_pd(0.0, -0.0);
        for (crd::u32 lane = 0; lane < kC; ++lane)
        {
            const __m128d v = _mm_loadu_pd(a.vals + 2U * static_cast<crd::usize>(off + lane));
            const __m128d xv = _mm_loadu_pd(x_of<Masked>(x, 2U * static_cast<crd::usize>(c[lane]), w, lens[lane]));
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
    static Sums step(Sums sums, const SellSlices<crd::f32>& a, const crd::f32* x, crd::u32 off, const crd::u32* c,
                     crd::u32 w, const crd::u32* lens) noexcept
    {
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

using F64Policy = SellPolicy<Sse2F64Body, kPairWidth, Sse2F64Body, kPairWidth>;
// f32 from DRAM runs every slice alone: pairs cost the power-law rows 9% (0.91x of HEAD against 1.02x and 1.03x).
using F32Policy = SellPolicy<Sse2F32Body, kPairWidth, Sse2F32Body, kNoPairs>;
using C64Policy = SellPolicy<Sse2C64Body, kPairWidth, Sse2C64Body, kPairWidth>;
using C32Policy = SellPolicy<Sse2C32Body, kPairWidth, Sse2C32Body, kPairWidth>;
#else
using F64Policy =
    SellPolicy<ScalarBody<crd::f64, kSellSliceF64>, kPairWidth, ScalarBody<crd::f64, kSellSliceF64>, kPairWidth>;
using F32Policy =
    SellPolicy<ScalarBody<crd::f32, kSellSliceF32>, kPairWidth, ScalarBody<crd::f32, kSellSliceF32>, kPairWidth>;
using C64Policy = SellPolicy<ScalarComplexBody<crd::f64>, kPairWidth, ScalarComplexBody<crd::f64>, kPairWidth>;
using C32Policy = SellPolicy<ScalarComplexBody<crd::f32>, kPairWidth, ScalarComplexBody<crd::f32>, kPairWidth>;
#endif

template <typename Policy, typename T>
void sell_spmv_real(const SellSlices<T>& a, T alpha, T beta, const T* x, T* y, crd::u32 s_begin, crd::u32 s_end,
                    SellSweep sweep) noexcept
{
    Policy::run(sweep, a, RealWrite<T>{alpha, beta, beta == T(0)}, x, y, s_begin, s_end);
}

template <typename Policy, typename T>
void sell_spmv_complex(const SellSlices<T>& a, const SellComplexScales<T>& scales, const T* x, T* y, crd::u32 s_begin,
                       crd::u32 s_end, SellSweep sweep) noexcept
{
    const bool beta_zero = scales.beta_re == T(0) && scales.beta_im == T(0);
    Policy::run(sweep, a, ComplexWrite<T>{scales, beta_zero}, x, y, s_begin, s_end);
}

} // namespace

constexpr auto kSellSpmvF64 = &sell_spmv_real<F64Policy, crd::f64>;
constexpr auto kSellSpmvF32 = &sell_spmv_real<F32Policy, crd::f32>;
constexpr auto kSellSpmvC64 = &sell_spmv_complex<C64Policy, crd::f64>;
constexpr auto kSellSpmvC32 = &sell_spmv_complex<C32Policy, crd::f32>;

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
