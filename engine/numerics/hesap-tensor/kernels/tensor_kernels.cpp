// The hesap-tensor kernels of crd/hesap/tensor/tensor_kernels.hpp, compiled once per CPU level by crd_add_cpu_kernels
// (../CMakeLists.txt, ADR-0134 §5). Everything here lives in the level's namespace (CRD_MATH_LEVEL_NS), the tile loops
// and the binned operations included, so no function of this unit is shared with general code or with another level.
// The portable bodies are the scalar loops; the AVX2 bodies run the in-register permute transposes of f32, f64 and
// complex elements (8x8 f32, 4x4 64-bit and 2x2 128-bit squares; 4x4, 2x2 and single elements on SSE2; with next-tile
// prefetches, and non-temporal stores for the multithreaded DRAM-sized permutes), twelve binned accumulators (six on
// SSE2), the nn quantizers eight lanes at a time, the quantized dots on maddubs/madd integer lanes, softmax's exp four
// lanes at a time and the Philox block loop 24 blocks at a time (16 on SSE2 and SSE4.2). The SSE2 and SSE4.2 nn bodies
// run four lanes: the quantizers on SSE2 operations, the dots on even/odd madd lanes (SSE2) or maddubs (SSE4.2), every
// f32 combine through the exact fma. The AVX-512 bodies run the permute squares on zmm (16x16 f32, 8x8 64-bit, 4x4
// 128-bit), three binned streams of eight lanes, the quantizers sixteen lanes at a time, softmax's exp eight, the Q8_0
// dots two blocks a zmm in the integer stage (each block's f32 combine stays on its own eight lanes in block order),
// the int8 row dots 64 values a step and the packed dots two 8-output blocks a step. The avx2 and avx512 levels' VNNI
// variants (CRD_KERNEL_HAS_AVXVNNI, CRD_KERNEL_HAS_AVX512VNNI) run the dots on dpbusd. Every body gives the same bits:
// the permute is element copies and the IEEE operations of a real or complex product, the binned representation is
// partition-independent, the quantizers are per-element IEEE operations, the dots are exact integers combined in one
// f32 order, crd_exp8 and crd_exp4 reproduce crd_exp1, and the Philox rounds are integer operations.

#include "tensor_batched_kernels.hpp"
#include "tensor_ew_kernels.hpp"
#include "tensor_nn_kernels.hpp"
#include "tensor_sparse_kernels.hpp"

#include <crd/hesap/stats/philox_blocks.hpp>
#include <crd/hesap/tensor/binned_core.hpp>
#include <crd/hesap/tensor/permute_tiles.hpp>
#include <crd/hesap/tensor/quant_core.hpp>
#include <crd/hesap/tensor/tensor_kernels.hpp>
#include <crd/math/libm.hpp>
#include <crd/math/simd/backend.hpp>
#include <crd/math/simd/transcendental.hpp>

#include <cstring>

namespace crd::hesap::tensor::inline CRD_MATH_LEVEL_NS
{
namespace
{

using detail::BinnedRungs;

// A permute element: Parts scalars S, one real value or (Parts 2) the (re, im) pair of a Complex<S>
// (crd/hesap/complex.hpp, which this unit never includes: general code passes the pairs as scalars).
template <typename S, crd::u64 Parts> struct Elem
{
    using Scalar = S;
    static constexpr crd::u64 kParts = Parts;
    static constexpr crd::i64 kStep = static_cast<crd::i64>(Parts); // the scalars from one element to the next
    static constexpr crd::u64 kBytes = sizeof(S) * Parts;
    // The elements in a 64-byte line: a tile runs its micro transposes over blocks of this many rows of a, so each
    // source line is read in one visit.
    static constexpr crd::u64 kBlockRows = 64U / kBytes;
};

// One element: a bit copy, or alpha times it in the operations of Complex<S>'s operator* (re = ar*xr - ai*xi,
// im = ar*xi + ai*xr; ar*x for a real element).
template <typename E, bool Scale, typename S> inline void put_element(S* d, const S* s, S ar, S ai) noexcept
{
    if constexpr (!Scale)
    {
        (void)ar;
        (void)ai;
        for (crd::u64 p = 0; p < E::kParts; ++p)
        {
            d[p] = s[p];
        }
    }
    else if constexpr (E::kParts == 1U)
    {
        (void)ai;
        d[0] = ar * s[0];
    }
    else
    {
        const S re = s[0];
        const S im = s[1];
        d[0] = ar * re - ai * im;
        d[1] = ar * im + ai * re;
    }
}

// The scalar tile of put_element: the edge strips, the non-unit source strides and the portable level.
template <typename E, bool Scale, typename S>
void scalar_tile(S* dst, crd::i64 ra, const S* src, crd::i64 sa, crd::i64 sb, crd::u64 na, crd::u64 nb, S ar,
                 S ai) noexcept
{
    detail::permute_tile_scalar<E::kParts>(dst, ra, src, sa, sb, na, nb,
                                           [ar, ai](S* d, const S* s) { put_element<E, Scale>(d, s, ar, ai); });
}

#if CRD_SIMD_HAS_SSE2
// The in-register transposes: a register holds kRegBytes of elements, and a micro transpose moves a square of
// kRegBytes / E::kBytes elements on a side. Source column j is that many contiguous elements at src + j*src_stride
// (the a dim is stride-1 in the source); destination row i is that many contiguous elements at dst + i*dst_stride
// (the b dim is stride-1 in the destination), so both memory streams stay unit-stride. The shuffles move bits only;
// a scale is applied to each destination row register. Stream stores non-temporally (dst rows 32-byte aligned, the
// region contract of every level: a zmm row on a line boundary is one 64-byte store, otherwise two 32-byte ones).
#if CRD_SIMD_HAS_AVX512
using Reg = __m512d;
constexpr crd::u64 kRegBytes = 64U;

inline Reg load_reg(const crd::f32* p) noexcept
{
    return _mm512_castps_pd(_mm512_loadu_ps(p));
}

inline Reg load_reg(const crd::f64* p) noexcept
{
    return _mm512_loadu_pd(p);
}

template <bool Stream> inline void store_reg(crd::f64* p, Reg v) noexcept
{
    if constexpr (Stream)
    {
        if ((reinterpret_cast<crd::usize>(p) & 63U) == 0U)
        {
            _mm512_stream_pd(p, v);
        }
        else
        {
            _mm256_stream_pd(p, _mm512_castpd512_pd256(v));
            _mm256_stream_pd(p + 4, _mm512_extractf64x4_pd(v, 1));
        }
    }
    else
    {
        _mm512_storeu_pd(p, v);
    }
}

template <bool Stream> inline void store_reg(crd::f32* p, Reg v) noexcept
{
    const __m512 x = _mm512_castpd_ps(v);
    if constexpr (Stream)
    {
        if ((reinterpret_cast<crd::usize>(p) & 63U) == 0U)
        {
            _mm512_stream_ps(p, x);
        }
        else
        {
            _mm256_stream_ps(p, _mm512_castps512_ps256(x));
            _mm256_stream_ps(p + 8, _mm256_castpd_ps(_mm512_extractf64x4_pd(v, 1)));
        }
    }
    else
    {
        _mm512_storeu_ps(p, x);
    }
}

inline Reg broadcast(crd::f32 v) noexcept
{
    return _mm512_castps_pd(_mm512_set1_ps(v));
}

inline Reg broadcast(crd::f64 v) noexcept
{
    return _mm512_set1_pd(v);
}
#elif CRD_SIMD_HAS_AVX2
using Reg = __m256d;
constexpr crd::u64 kRegBytes = 32U;

inline Reg load_reg(const crd::f32* p) noexcept
{
    return _mm256_castps_pd(_mm256_loadu_ps(p));
}

inline Reg load_reg(const crd::f64* p) noexcept
{
    return _mm256_loadu_pd(p);
}

template <bool Stream> inline void store_reg(crd::f32* p, Reg v) noexcept
{
    if constexpr (Stream)
    {
        _mm256_stream_ps(p, _mm256_castpd_ps(v));
    }
    else
    {
        _mm256_storeu_ps(p, _mm256_castpd_ps(v));
    }
}

template <bool Stream> inline void store_reg(crd::f64* p, Reg v) noexcept
{
    if constexpr (Stream)
    {
        _mm256_stream_pd(p, v);
    }
    else
    {
        _mm256_storeu_pd(p, v);
    }
}

inline Reg broadcast(crd::f32 v) noexcept
{
    return _mm256_castps_pd(_mm256_set1_ps(v));
}

inline Reg broadcast(crd::f64 v) noexcept
{
    return _mm256_set1_pd(v);
}
#else
using Reg = __m128d;
constexpr crd::u64 kRegBytes = 16U;

inline Reg load_reg(const crd::f32* p) noexcept
{
    return _mm_castps_pd(_mm_loadu_ps(p));
}

inline Reg load_reg(const crd::f64* p) noexcept
{
    return _mm_loadu_pd(p);
}

template <bool Stream> inline void store_reg(crd::f32* p, Reg v) noexcept
{
    if constexpr (Stream)
    {
        _mm_stream_ps(p, _mm_castpd_ps(v));
    }
    else
    {
        _mm_storeu_ps(p, _mm_castpd_ps(v));
    }
}

template <bool Stream> inline void store_reg(crd::f64* p, Reg v) noexcept
{
    if constexpr (Stream)
    {
        _mm_stream_pd(p, v);
    }
    else
    {
        _mm_storeu_pd(p, v);
    }
}

inline Reg broadcast(crd::f32 v) noexcept
{
    return _mm_castps_pd(_mm_set1_ps(v));
}

inline Reg broadcast(crd::f64 v) noexcept
{
    return _mm_set1_pd(v);
}
#endif

// alpha's parts in every lane of their scalar type.
struct RegScale
{
    Reg re;
    Reg im;
};

// One line prefetched with a PermutePick hint (the hint is an immediate on every compiler, so each one is a case).
inline void prefetch_line(const void* p, PermuteHint hint) noexcept
{
    const char* const line = static_cast<const char*>(p);
    switch (hint)
    {
        case PermuteHint::T0:
            _mm_prefetch(line, _MM_HINT_T0);
            break;
        case PermuteHint::T1:
            _mm_prefetch(line, _MM_HINT_T1);
            break;
        case PermuteHint::T2:
            _mm_prefetch(line, _MM_HINT_T2);
            break;
        case PermuteHint::Nta:
            _mm_prefetch(line, _MM_HINT_NTA);
            break;
    }
}

// alpha * v per element, the operations of put_element lane by lane: the products in its operand order, then a true
// subtract on the re lanes and an add on the im lanes (addsub on AVX2, the add with a masked subtract on AVX-512, both
// and a select on SSE2), so a propagated NaN keeps its sign.
template <typename E> inline Reg scale_reg(Reg v, const RegScale& k) noexcept
{
    using S = typename E::Scalar;
    if constexpr (E::kParts == 1U && sizeof(S) == 4U)
    {
#if CRD_SIMD_HAS_AVX512
        return _mm512_castps_pd(_mm512_mul_ps(_mm512_castpd_ps(k.re), _mm512_castpd_ps(v)));
#elif CRD_SIMD_HAS_AVX2
        return _mm256_castps_pd(_mm256_mul_ps(_mm256_castpd_ps(k.re), _mm256_castpd_ps(v)));
#else
        return _mm_castps_pd(_mm_mul_ps(_mm_castpd_ps(k.re), _mm_castpd_ps(v)));
#endif
    }
    else if constexpr (E::kParts == 1U)
    {
#if CRD_SIMD_HAS_AVX512
        return _mm512_mul_pd(k.re, v);
#elif CRD_SIMD_HAS_AVX2
        return _mm256_mul_pd(k.re, v);
#else
        return _mm_mul_pd(k.re, v);
#endif
    }
    else if constexpr (sizeof(S) == 4U)
    {
#if CRD_SIMD_HAS_AVX512
        const __m512 x = _mm512_castpd_ps(v);
        const __m512 t1 = _mm512_mul_ps(_mm512_castpd_ps(k.re), x);
        const __m512 t2 = _mm512_mul_ps(_mm512_castpd_ps(k.im), _mm512_permute_ps(x, _MM_SHUFFLE(2, 3, 0, 1)));
        return _mm512_castps_pd(_mm512_mask_sub_ps(_mm512_add_ps(t1, t2), 0x5555, t1, t2));
#elif CRD_SIMD_HAS_AVX2
        const __m256 x = _mm256_castpd_ps(v);
        const __m256 t1 = _mm256_mul_ps(_mm256_castpd_ps(k.re), x);
        const __m256 t2 = _mm256_mul_ps(_mm256_castpd_ps(k.im), _mm256_permute_ps(x, _MM_SHUFFLE(2, 3, 0, 1)));
        return _mm256_castps_pd(_mm256_addsub_ps(t1, t2));
#else
        const __m128 x = _mm_castpd_ps(v);
        const __m128 t1 = _mm_mul_ps(_mm_castpd_ps(k.re), x);
        const __m128 t2 = _mm_mul_ps(_mm_castpd_ps(k.im), _mm_shuffle_ps(x, x, _MM_SHUFFLE(2, 3, 0, 1)));
        const __m128 re_lanes = _mm_castsi128_ps(_mm_set_epi32(0, -1, 0, -1));
        return _mm_castps_pd(
            _mm_or_ps(_mm_and_ps(re_lanes, _mm_sub_ps(t1, t2)), _mm_andnot_ps(re_lanes, _mm_add_ps(t1, t2))));
#endif
    }
    else
    {
#if CRD_SIMD_HAS_AVX512
        const __m512d t1 = _mm512_mul_pd(k.re, v);
        const __m512d t2 = _mm512_mul_pd(k.im, _mm512_permute_pd(v, 0x55));
        return _mm512_mask_sub_pd(_mm512_add_pd(t1, t2), 0x55, t1, t2);
#elif CRD_SIMD_HAS_AVX2
        const __m256d t1 = _mm256_mul_pd(k.re, v);
        const __m256d t2 = _mm256_mul_pd(k.im, _mm256_permute_pd(v, 0x5));
        return _mm256_addsub_pd(t1, t2);
#else
        const __m128d t1 = _mm_mul_pd(k.re, v);
        const __m128d t2 = _mm_mul_pd(k.im, _mm_shuffle_pd(v, v, 1));
        return _mm_move_sd(_mm_add_pd(t1, t2), _mm_sub_pd(t1, t2));
#endif
    }
}

// One square of the in-register transpose: 32-bit elements the unpack/shuffle/permute2f128 8x8 (the unpack/shuffle/
// shuffle_f32x4 16x16 on AVX-512, the unpack/movelh/movehl 4x4 on SSE2), 64-bit ones the unpack/permute2f128 4x4 (the
// unpack/shuffle_f64x2 8x8, the unpack 2x2), 128-bit ones the permute2f128 2x2 (the shuffle_f64x2 4x4, one element on
// SSE2). Strides are in scalars.
template <typename E, bool Scale, bool Stream>
void transpose_micro(const typename E::Scalar* src, crd::i64 src_stride, typename E::Scalar* dst, crd::i64 dst_stride,
                     const RegScale& k) noexcept
{
    (void)k;
    const auto put = [&](crd::i64 i, Reg v)
    {
        if constexpr (Scale)
        {
            v = scale_reg<E>(v, k);
        }
        store_reg<Stream>(dst + i * dst_stride, v);
    };
    if constexpr (E::kBytes == 4U)
    {
#if CRD_SIMD_HAS_AVX512
        // The 4x4 transposes inside each 128-bit lane of four rows (AVX2's unpack and shuffle stages): s[4g + c] holds,
        // in lane L, column 4L + c of rows 4g..4g+3. Output row 4L + c is lane L of s[c], s[4 + c], s[8 + c] and
        // s[12 + c]: a 4x4 transpose of 128-bit lanes in two shuffle_f32x4 stages.
        __m512 s[16];
        for (crd::i64 g = 0; g < 4; ++g)
        {
            const __m512 r0 = _mm512_loadu_ps(src + (4 * g + 0) * src_stride);
            const __m512 r1 = _mm512_loadu_ps(src + (4 * g + 1) * src_stride);
            const __m512 r2 = _mm512_loadu_ps(src + (4 * g + 2) * src_stride);
            const __m512 r3 = _mm512_loadu_ps(src + (4 * g + 3) * src_stride);
            const __m512 t0 = _mm512_unpacklo_ps(r0, r1);
            const __m512 t1 = _mm512_unpackhi_ps(r0, r1);
            const __m512 t2 = _mm512_unpacklo_ps(r2, r3);
            const __m512 t3 = _mm512_unpackhi_ps(r2, r3);
            s[4 * g + 0] = _mm512_shuffle_ps(t0, t2, _MM_SHUFFLE(1, 0, 1, 0));
            s[4 * g + 1] = _mm512_shuffle_ps(t0, t2, _MM_SHUFFLE(3, 2, 3, 2));
            s[4 * g + 2] = _mm512_shuffle_ps(t1, t3, _MM_SHUFFLE(1, 0, 1, 0));
            s[4 * g + 3] = _mm512_shuffle_ps(t1, t3, _MM_SHUFFLE(3, 2, 3, 2));
        }
        for (crd::i64 c = 0; c < 4; ++c)
        {
            const __m512 u0 = _mm512_shuffle_f32x4(s[c], s[4 + c], _MM_SHUFFLE(1, 0, 1, 0));
            const __m512 u1 = _mm512_shuffle_f32x4(s[c], s[4 + c], _MM_SHUFFLE(3, 2, 3, 2));
            const __m512 u2 = _mm512_shuffle_f32x4(s[8 + c], s[12 + c], _MM_SHUFFLE(1, 0, 1, 0));
            const __m512 u3 = _mm512_shuffle_f32x4(s[8 + c], s[12 + c], _MM_SHUFFLE(3, 2, 3, 2));
            put(0 + c, _mm512_castps_pd(_mm512_shuffle_f32x4(u0, u2, _MM_SHUFFLE(2, 0, 2, 0))));
            put(4 + c, _mm512_castps_pd(_mm512_shuffle_f32x4(u0, u2, _MM_SHUFFLE(3, 1, 3, 1))));
            put(8 + c, _mm512_castps_pd(_mm512_shuffle_f32x4(u1, u3, _MM_SHUFFLE(2, 0, 2, 0))));
            put(12 + c, _mm512_castps_pd(_mm512_shuffle_f32x4(u1, u3, _MM_SHUFFLE(3, 1, 3, 1))));
        }
#elif CRD_SIMD_HAS_AVX2
        const __m256 r0 = _mm256_loadu_ps(src + 0 * src_stride);
        const __m256 r1 = _mm256_loadu_ps(src + 1 * src_stride);
        const __m256 r2 = _mm256_loadu_ps(src + 2 * src_stride);
        const __m256 r3 = _mm256_loadu_ps(src + 3 * src_stride);
        const __m256 r4 = _mm256_loadu_ps(src + 4 * src_stride);
        const __m256 r5 = _mm256_loadu_ps(src + 5 * src_stride);
        const __m256 r6 = _mm256_loadu_ps(src + 6 * src_stride);
        const __m256 r7 = _mm256_loadu_ps(src + 7 * src_stride);

        const __m256 t0 = _mm256_unpacklo_ps(r0, r1);
        const __m256 t1 = _mm256_unpackhi_ps(r0, r1);
        const __m256 t2 = _mm256_unpacklo_ps(r2, r3);
        const __m256 t3 = _mm256_unpackhi_ps(r2, r3);
        const __m256 t4 = _mm256_unpacklo_ps(r4, r5);
        const __m256 t5 = _mm256_unpackhi_ps(r4, r5);
        const __m256 t6 = _mm256_unpacklo_ps(r6, r7);
        const __m256 t7 = _mm256_unpackhi_ps(r6, r7);

        const __m256 s0 = _mm256_shuffle_ps(t0, t2, _MM_SHUFFLE(1, 0, 1, 0));
        const __m256 s1 = _mm256_shuffle_ps(t0, t2, _MM_SHUFFLE(3, 2, 3, 2));
        const __m256 s2 = _mm256_shuffle_ps(t1, t3, _MM_SHUFFLE(1, 0, 1, 0));
        const __m256 s3 = _mm256_shuffle_ps(t1, t3, _MM_SHUFFLE(3, 2, 3, 2));
        const __m256 s4 = _mm256_shuffle_ps(t4, t6, _MM_SHUFFLE(1, 0, 1, 0));
        const __m256 s5 = _mm256_shuffle_ps(t4, t6, _MM_SHUFFLE(3, 2, 3, 2));
        const __m256 s6 = _mm256_shuffle_ps(t5, t7, _MM_SHUFFLE(1, 0, 1, 0));
        const __m256 s7 = _mm256_shuffle_ps(t5, t7, _MM_SHUFFLE(3, 2, 3, 2));

        put(0, _mm256_castps_pd(_mm256_permute2f128_ps(s0, s4, 0x20)));
        put(1, _mm256_castps_pd(_mm256_permute2f128_ps(s1, s5, 0x20)));
        put(2, _mm256_castps_pd(_mm256_permute2f128_ps(s2, s6, 0x20)));
        put(3, _mm256_castps_pd(_mm256_permute2f128_ps(s3, s7, 0x20)));
        put(4, _mm256_castps_pd(_mm256_permute2f128_ps(s0, s4, 0x31)));
        put(5, _mm256_castps_pd(_mm256_permute2f128_ps(s1, s5, 0x31)));
        put(6, _mm256_castps_pd(_mm256_permute2f128_ps(s2, s6, 0x31)));
        put(7, _mm256_castps_pd(_mm256_permute2f128_ps(s3, s7, 0x31)));
#else
        const __m128 r0 = _mm_loadu_ps(src + 0 * src_stride);
        const __m128 r1 = _mm_loadu_ps(src + 1 * src_stride);
        const __m128 r2 = _mm_loadu_ps(src + 2 * src_stride);
        const __m128 r3 = _mm_loadu_ps(src + 3 * src_stride);

        const __m128 t0 = _mm_unpacklo_ps(r0, r1);
        const __m128 t1 = _mm_unpackhi_ps(r0, r1);
        const __m128 t2 = _mm_unpacklo_ps(r2, r3);
        const __m128 t3 = _mm_unpackhi_ps(r2, r3);

        put(0, _mm_castps_pd(_mm_movelh_ps(t0, t2)));
        put(1, _mm_castps_pd(_mm_movehl_ps(t2, t0)));
        put(2, _mm_castps_pd(_mm_movelh_ps(t1, t3)));
        put(3, _mm_castps_pd(_mm_movehl_ps(t3, t1)));
#endif
    }
    else if constexpr (E::kBytes == 8U)
    {
#if CRD_SIMD_HAS_AVX512
        // t[2p + c] holds, in 128-bit lane L, column 2L + c of rows 2p and 2p + 1; output row 2L + c is lane L of t[c],
        // t[2 + c], t[4 + c] and t[6 + c] (the 128-bit lane transpose).
        __m512d t[8];
        for (crd::i64 p = 0; p < 4; ++p)
        {
            const Reg r0 = load_reg(src + (2 * p + 0) * src_stride);
            const Reg r1 = load_reg(src + (2 * p + 1) * src_stride);
            t[2 * p + 0] = _mm512_unpacklo_pd(r0, r1);
            t[2 * p + 1] = _mm512_unpackhi_pd(r0, r1);
        }
        for (crd::i64 c = 0; c < 2; ++c)
        {
            const __m512d u0 = _mm512_shuffle_f64x2(t[c], t[2 + c], _MM_SHUFFLE(1, 0, 1, 0));
            const __m512d u1 = _mm512_shuffle_f64x2(t[c], t[2 + c], _MM_SHUFFLE(3, 2, 3, 2));
            const __m512d u2 = _mm512_shuffle_f64x2(t[4 + c], t[6 + c], _MM_SHUFFLE(1, 0, 1, 0));
            const __m512d u3 = _mm512_shuffle_f64x2(t[4 + c], t[6 + c], _MM_SHUFFLE(3, 2, 3, 2));
            put(0 + c, _mm512_shuffle_f64x2(u0, u2, _MM_SHUFFLE(2, 0, 2, 0)));
            put(2 + c, _mm512_shuffle_f64x2(u0, u2, _MM_SHUFFLE(3, 1, 3, 1)));
            put(4 + c, _mm512_shuffle_f64x2(u1, u3, _MM_SHUFFLE(2, 0, 2, 0)));
            put(6 + c, _mm512_shuffle_f64x2(u1, u3, _MM_SHUFFLE(3, 1, 3, 1)));
        }
#elif CRD_SIMD_HAS_AVX2
        const Reg r0 = load_reg(src + 0 * src_stride);
        const Reg r1 = load_reg(src + 1 * src_stride);
        const Reg r2 = load_reg(src + 2 * src_stride);
        const Reg r3 = load_reg(src + 3 * src_stride);

        const __m256d t0 = _mm256_unpacklo_pd(r0, r1);
        const __m256d t1 = _mm256_unpackhi_pd(r0, r1);
        const __m256d t2 = _mm256_unpacklo_pd(r2, r3);
        const __m256d t3 = _mm256_unpackhi_pd(r2, r3);

        put(0, _mm256_permute2f128_pd(t0, t2, 0x20));
        put(1, _mm256_permute2f128_pd(t1, t3, 0x20));
        put(2, _mm256_permute2f128_pd(t0, t2, 0x31));
        put(3, _mm256_permute2f128_pd(t1, t3, 0x31));
#else
        const Reg r0 = load_reg(src + 0 * src_stride);
        const Reg r1 = load_reg(src + 1 * src_stride);

        put(0, _mm_unpacklo_pd(r0, r1));
        put(1, _mm_unpackhi_pd(r0, r1));
#endif
    }
    else
    {
        static_assert(E::kBytes == 16U, "the permute elements are 4, 8 or 16 bytes");
#if CRD_SIMD_HAS_AVX512
        const Reg r0 = load_reg(src + 0 * src_stride);
        const Reg r1 = load_reg(src + 1 * src_stride);
        const Reg r2 = load_reg(src + 2 * src_stride);
        const Reg r3 = load_reg(src + 3 * src_stride);
        const __m512d u0 = _mm512_shuffle_f64x2(r0, r1, _MM_SHUFFLE(1, 0, 1, 0));
        const __m512d u1 = _mm512_shuffle_f64x2(r0, r1, _MM_SHUFFLE(3, 2, 3, 2));
        const __m512d u2 = _mm512_shuffle_f64x2(r2, r3, _MM_SHUFFLE(1, 0, 1, 0));
        const __m512d u3 = _mm512_shuffle_f64x2(r2, r3, _MM_SHUFFLE(3, 2, 3, 2));
        put(0, _mm512_shuffle_f64x2(u0, u2, _MM_SHUFFLE(2, 0, 2, 0)));
        put(1, _mm512_shuffle_f64x2(u0, u2, _MM_SHUFFLE(3, 1, 3, 1)));
        put(2, _mm512_shuffle_f64x2(u1, u3, _MM_SHUFFLE(2, 0, 2, 0)));
        put(3, _mm512_shuffle_f64x2(u1, u3, _MM_SHUFFLE(3, 1, 3, 1)));
#elif CRD_SIMD_HAS_AVX2
        const Reg r0 = load_reg(src + 0 * src_stride);
        const Reg r1 = load_reg(src + 1 * src_stride);

        put(0, _mm256_permute2f128_pd(r0, r1, 0x20));
        put(1, _mm256_permute2f128_pd(r0, r1, 0x31));
#else
        (void)src_stride;
        put(0, load_reg(src));
#endif
    }
}

// One tile, strides and extents in elements. With a unit source stride the micro transposes cover the tile's grid in
// blocks of E::kBlockRows rows of a (ib outer within a block), and the right and bottom strips finish scalar; any
// other source stride, or pick.scalar, is the scalar tile. Given a next tile (pick.distance; the rules give none), each
// block step prefetches with pick.hint the same block step of the next tile (inside its extents only): one line of
// each source column and, unless the stores are non-temporal or pick.dst_prefetch is off, the destination rows once
// per line of them. On the streamed path with near columns (<= 64 KB apart) it also prefetches the next step's source
// columns into L1: it hides strided-read latency the streamer cannot track in the multithreaded regime (2D f32 46.8 ->
// 51.2 GB/s); on page-scattered columns those TLB walks cost more than they hide.
template <typename E, bool Scale, bool Stream>
void permute_tile(typename E::Scalar* dst, crd::i64 ra, const typename E::Scalar* src, crd::i64 sa, crd::i64 sb,
                  crd::u64 na, crd::u64 nb, typename E::Scalar ar, typename E::Scalar ai, PermuteNext next,
                  const PermutePick& pick) noexcept
{
    using S = typename E::Scalar;
    constexpr crd::i64 step = E::kStep;
    constexpr crd::u64 micro = kRegBytes / E::kBytes;
    if (sa != 1 || pick.scalar)
    {
        scalar_tile<E, Scale>(dst, ra, src, sa, sb, na, nb, ar, ai);
        return;
    }
    const RegScale k{broadcast(ar), broadcast(ai)};
    const crd::u64 nam = na & ~(micro - 1U);
    const crd::u64 nbm = nb & ~(micro - 1U);
    const bool has_next = next.na != 0U && next.nb != 0U;
    const S* next_src = has_next ? src + next.src * step : src;
    const S* next_dst = has_next ? dst + next.dst * step : dst;
    const bool near_cols = (sb < 0 ? -sb : sb) * static_cast<crd::i64>(E::kBytes) <= 65536;
    for (crd::u64 ia0 = 0; ia0 < nam; ia0 += E::kBlockRows)
    {
        const crd::u64 ia1 = ia0 + E::kBlockRows < nam ? ia0 + E::kBlockRows : nam;
        for (crd::u64 ib = 0; ib < nbm; ib += micro)
        {
            if (ia0 < next.na)
            {
                for (crd::u64 c = ib; c < ib + micro && c < next.nb; ++c)
                {
                    prefetch_line(next_src + (static_cast<crd::i64>(ia0) + static_cast<crd::i64>(c) * sb) * step,
                                  pick.hint);
                }
                if (!Stream && pick.dst_prefetch && ib % E::kBlockRows == 0U && ib < next.nb)
                {
                    for (crd::u64 i = ia0; i < ia1 && i < next.na; ++i)
                    {
                        prefetch_line(next_dst + (static_cast<crd::i64>(i) * ra + static_cast<crd::i64>(ib)) * step,
                                      pick.hint);
                    }
                }
            }
            if (Stream && near_cols && ib + micro < nbm)
            {
                for (crd::u64 c = ib + micro; c < ib + 2U * micro && c < nbm; ++c)
                {
                    _mm_prefetch(reinterpret_cast<const char*>(
                                     src + (static_cast<crd::i64>(ia0) + static_cast<crd::i64>(c) * sb) * step),
                                 _MM_HINT_T0);
                }
            }
            for (crd::u64 ia = ia0; ia < ia1; ia += micro)
            {
                transpose_micro<E, Scale, Stream>(
                    src + (static_cast<crd::i64>(ia) + static_cast<crd::i64>(ib) * sb) * step, sb * step,
                    dst + (static_cast<crd::i64>(ia) * ra + static_cast<crd::i64>(ib)) * step, ra * step, k);
            }
        }
    }
    if (nbm < nb) // right strip: rows [0, nam), cols [nbm, nb)
    {
        scalar_tile<E, Scale>(dst + static_cast<crd::i64>(nbm) * step, ra, src + static_cast<crd::i64>(nbm) * sb * step,
                              sa, sb, nam, nb - nbm, ar, ai);
    }
    if (nam < na) // bottom strip: rows [nam, na), all cols
    {
        scalar_tile<E, Scale>(dst + static_cast<crd::i64>(nam) * ra * step, ra,
                              src + static_cast<crd::i64>(nam) * sa * step, sa, sb, na - nam, nb, ar, ai);
    }
}
#else
template <typename E, bool Scale, bool Stream>
void permute_tile(typename E::Scalar* dst, crd::i64 ra, const typename E::Scalar* src, crd::i64 sa, crd::i64 sb,
                  crd::u64 na, crd::u64 nb, typename E::Scalar ar, typename E::Scalar ai, PermuteNext /*next*/,
                  const PermutePick& /*pick*/) noexcept
{
    scalar_tile<E, Scale>(dst, ra, src, sa, sb, na, nb, ar, ai);
}
#endif

// One tile as a staged strip (PermuteRegion::stage, tile_a * tile_b elements): the tile into the stage, whose rows are
// nb elements, then each stage row stored to its destination row in one sweep (non-temporally when Stream; the
// region's alignment contract makes every full register of a row aligned). With a unit source stride on a level with
// the register transposes, the micro column groups run outer, so each source column is read whole in one pass (the
// 1 MB-apart columns of the 3D 512^3 rotation as one sequential run each); prefetches as permute_tile's, of the next
// tile's source columns only. Otherwise the scalar tile fills the stage.
template <typename E, bool Scale, bool Stream>
void permute_tile_staged(typename E::Scalar* dst, crd::i64 ra, const typename E::Scalar* src, crd::i64 sa, crd::i64 sb,
                         crd::u64 na, crd::u64 nb, typename E::Scalar ar, typename E::Scalar ai, PermuteNext next,
                         const PermutePick& pick, typename E::Scalar* stage) noexcept
{
    using S = typename E::Scalar;
    constexpr crd::i64 step = E::kStep;
    const crd::i64 rs = static_cast<crd::i64>(nb); // the stage's row stride
#if CRD_SIMD_HAS_SSE2
    constexpr crd::u64 micro = kRegBytes / E::kBytes;
    if (sa == 1 && !pick.scalar)
    {
        const RegScale k{broadcast(ar), broadcast(ai)};
        const crd::u64 nam = na & ~(micro - 1U);
        const crd::u64 nbm = nb & ~(micro - 1U);
        const bool has_next = next.na != 0U && next.nb != 0U;
        const S* next_src = has_next ? src + next.src * step : src;
        for (crd::u64 ib = 0; ib < nbm; ib += micro)
        {
            for (crd::u64 ia = 0; ia < nam; ia += micro)
            {
                if (ia % E::kBlockRows == 0U && ia < next.na)
                {
                    for (crd::u64 c = ib; c < ib + micro && c < next.nb; ++c)
                    {
                        prefetch_line(next_src + (static_cast<crd::i64>(ia) + static_cast<crd::i64>(c) * sb) * step,
                                      pick.hint);
                    }
                }
                transpose_micro<E, Scale, false>(
                    src + (static_cast<crd::i64>(ia) + static_cast<crd::i64>(ib) * sb) * step, sb * step,
                    stage + (static_cast<crd::i64>(ia) * rs + static_cast<crd::i64>(ib)) * step, rs * step, k);
            }
        }
        if (nbm < nb) // right strip: rows [0, nam), cols [nbm, nb)
        {
            scalar_tile<E, Scale>(stage + static_cast<crd::i64>(nbm) * step, rs,
                                  src + static_cast<crd::i64>(nbm) * sb * step, sa, sb, nam, nb - nbm, ar, ai);
        }
        if (nam < na) // bottom strip: rows [nam, na), all cols
        {
            scalar_tile<E, Scale>(stage + static_cast<crd::i64>(nam) * rs * step, rs,
                                  src + static_cast<crd::i64>(nam) * sa * step, sa, sb, na - nam, nb, ar, ai);
        }
    }
    else
    {
        scalar_tile<E, Scale>(stage, rs, src, sa, sb, na, nb, ar, ai);
    }
    constexpr crd::u64 lanes = kRegBytes / sizeof(S); // scalars per register
    const crd::u64 row = nb * E::kParts;              // scalars per row
    const crd::u64 rowm = row & ~(lanes - 1U);
    for (crd::u64 ia = 0; ia < na; ++ia)
    {
        const S* const from = stage + static_cast<crd::i64>(ia) * rs * step;
        S* const to = dst + static_cast<crd::i64>(ia) * ra * step;
        for (crd::u64 j = 0; j < rowm; j += lanes)
        {
            store_reg<Stream>(to + j, load_reg(from + j));
        }
        for (crd::u64 j = rowm; j < row; ++j)
        {
            to[j] = from[j];
        }
    }
#else
    (void)next;
    (void)pick;
    scalar_tile<E, Scale>(stage, rs, src, sa, sb, na, nb, ar, ai);
    const crd::u64 row = nb * E::kParts;
    for (crd::u64 ia = 0; ia < na; ++ia)
    {
        const S* const from = stage + static_cast<crd::i64>(ia) * rs * step;
        S* const to = dst + static_cast<crd::i64>(ia) * ra * step;
        for (crd::u64 j = 0; j < row; ++j)
        {
            to[j] = from[j];
        }
    }
#endif
}

template <typename E, bool Scale, bool Stream> void permute_region(const PermuteRegion<typename E::Scalar>& r) noexcept
{
    detail::permute_region_tiles(
        r.na, r.nb, r.tile_a, r.tile_b, r.sa, r.sb, r.ra, r.next, r.pick.distance,
        [&r](crd::u64 ta, crd::u64 tb, crd::u64 ha, crd::u64 hb, PermuteNext next)
        {
            typename E::Scalar* const d =
                r.dst + (static_cast<crd::i64>(ta) * r.ra + static_cast<crd::i64>(tb)) * E::kStep;
            const typename E::Scalar* const s =
                r.src + (static_cast<crd::i64>(ta) * r.sa + static_cast<crd::i64>(tb) * r.sb) * E::kStep;
            if (r.stage != nullptr)
            {
                permute_tile_staged<E, Scale, Stream>(d, r.ra, s, r.sa, r.sb, ha, hb, r.alpha_re, r.alpha_im, next,
                                                      r.pick, r.stage);
            }
            else
            {
                permute_tile<E, Scale, Stream>(d, r.ra, s, r.sa, r.sb, ha, hb, r.alpha_re, r.alpha_im, next, r.pick);
            }
        });
#if CRD_SIMD_HAS_SSE2
    if constexpr (Stream)
    {
        _mm_sfence(); // non-temporal stores are weakly ordered: publish them before returning
    }
#endif
}

template <typename E> void permute_region_element(const PermuteRegion<typename E::Scalar>& r) noexcept
{
#if CRD_SIMD_HAS_SSE2
    if (r.stream)
    {
        if (r.scale)
        {
            permute_region<E, true, true>(r);
        }
        else
        {
            permute_region<E, false, true>(r);
        }
        return;
    }
#endif
    if (r.scale)
    {
        permute_region<E, true, false>(r);
    }
    else
    {
        permute_region<E, false, false>(r);
    }
}

void permute_region_f32(const PermuteRegion<crd::f32>& r) noexcept
{
    if (r.complex)
    {
        permute_region_element<Elem<crd::f32, 2U>>(r);
    }
    else
    {
        permute_region_element<Elem<crd::f32, 1U>>(r);
    }
}

void permute_region_f64(const PermuteRegion<crd::f64>& r) noexcept
{
    if (r.complex)
    {
        permute_region_element<Elem<crd::f64, 2U>>(r);
    }
    else
    {
        permute_region_element<Elem<crd::f64, 1U>>(r);
    }
}

// The serial walk: every plane one region, handed the next plane for a pick distance that reaches past it.
template <typename E, bool Scale, bool Stream> void permute_walk(const PermuteWalk<typename E::Scalar>& w) noexcept
{
    const crd::u32 b = w.m - 1U;
    detail::permute_walk_planes(w.shp, w.s_in, w.s_out, w.m, w.a, w.pick.order,
                                [&w, b](crd::i64 soff, crd::i64 doff, PermuteNext next)
                                {
                                    permute_region<E, Scale, Stream>(
                                        {w.dst + doff * E::kStep, w.s_out[w.a], w.src + soff * E::kStep, w.s_in[w.a],
                                         w.s_in[b], w.shp[w.a], w.shp[b], w.tile, w.tile, w.alpha_re, w.alpha_im,
                                         w.complex, Scale, Stream, next, w.pick});
                                });
}

template <typename E> void permute_walk_element(const PermuteWalk<typename E::Scalar>& w) noexcept
{
#if CRD_SIMD_HAS_SSE2
    if (w.stream)
    {
        if (w.scale)
        {
            permute_walk<E, true, true>(w);
        }
        else
        {
            permute_walk<E, false, true>(w);
        }
        return;
    }
#endif
    if (w.scale)
    {
        permute_walk<E, true, false>(w);
    }
    else
    {
        permute_walk<E, false, false>(w);
    }
}

void permute_walk_f32(const PermuteWalk<crd::f32>& w) noexcept
{
    if (w.complex)
    {
        permute_walk_element<Elem<crd::f32, 2U>>(w);
    }
    else
    {
        permute_walk_element<Elem<crd::f32, 1U>>(w);
    }
}

void permute_walk_f64(const PermuteWalk<crd::f64>& w) noexcept
{
    if (w.complex)
    {
        permute_walk_element<Elem<crd::f64, 2U>>(w);
    }
    else
    {
        permute_walk_element<Elem<crd::f64, 1U>>(w);
    }
}

#if CRD_SIMD_HAS_SSE2
// Independent binned accumulators: kStreams streams (breaking the rung-ladder latency chain) x kLanes lanes, merged at
// the end. Legal because the binned representation is partition-independent: the merged bits equal the scalar
// whole-array sum at any lane or stream count. The per-element ladder is the scalar deposit (sticky-or, two-sum) per
// lane. The level's register set: three streams of eight lanes on AVX-512, of four on AVX2, of two on SSE2 (nine ladder
// registers plus the constants and the deposit's temporaries fit every register file; each stream carries one add per
// rung from element to element, so three streams already keep the adders busy).
using detail::kBinEndurance;
using detail::kBinFold;

#if CRD_SIMD_HAS_AVX512
struct BinnedIsa
{
    using V = __m512d;
    static constexpr crd::u64 kLanes = 8U;
    static constexpr crd::u64 kStreams = 3U;
    static V load(const crd::f64* p) noexcept { return _mm512_loadu_pd(p); }
    static void store(crd::f64* p, V v) noexcept { _mm512_storeu_pd(p, v); }
    static V bits(crd::i64 b) noexcept { return _mm512_castsi512_pd(_mm512_set1_epi64(b)); }
    static V zero() noexcept { return _mm512_setzero_pd(); }
    static V add(V a, V b) noexcept { return _mm512_add_pd(a, b); }
    static V sub(V a, V b) noexcept { return _mm512_sub_pd(a, b); }
    static V bit_or(V a, V b) noexcept { return _mm512_or_pd(a, b); }
    static V bit_and(V a, V b) noexcept { return _mm512_and_pd(a, b); }
    static V max(V a, V b) noexcept { return _mm512_max_pd(a, b); }
};
#elif CRD_SIMD_HAS_AVX2
struct BinnedIsa
{
    using V = __m256d;
    static constexpr crd::u64 kLanes = 4U;
    static constexpr crd::u64 kStreams = 3U;
    static V load(const crd::f64* p) noexcept { return _mm256_loadu_pd(p); }
    static void store(crd::f64* p, V v) noexcept { _mm256_storeu_pd(p, v); }
    static V bits(crd::i64 b) noexcept { return _mm256_castsi256_pd(_mm256_set1_epi64x(b)); }
    static V zero() noexcept { return _mm256_setzero_pd(); }
    static V add(V a, V b) noexcept { return _mm256_add_pd(a, b); }
    static V sub(V a, V b) noexcept { return _mm256_sub_pd(a, b); }
    static V bit_or(V a, V b) noexcept { return _mm256_or_pd(a, b); }
    static V bit_and(V a, V b) noexcept { return _mm256_and_pd(a, b); }
    static V max(V a, V b) noexcept { return _mm256_max_pd(a, b); }
};
#else
struct BinnedIsa
{
    using V = __m128d;
    static constexpr crd::u64 kLanes = 2U;
    static constexpr crd::u64 kStreams = 3U;
    static V load(const crd::f64* p) noexcept { return _mm_loadu_pd(p); }
    static void store(crd::f64* p, V v) noexcept { _mm_storeu_pd(p, v); }
    static V bits(crd::i64 b) noexcept { return _mm_castsi128_pd(_mm_set1_epi64x(b)); }
    static V zero() noexcept { return _mm_setzero_pd(); }
    static V add(V a, V b) noexcept { return _mm_add_pd(a, b); }
    static V sub(V a, V b) noexcept { return _mm_sub_pd(a, b); }
    static V bit_or(V a, V b) noexcept { return _mm_or_pd(a, b); }
    static V bit_and(V a, V b) noexcept { return _mm_and_pd(a, b); }
    static V max(V a, V b) noexcept { return _mm_max_pd(a, b); }
};
#endif

constexpr crd::u64 kBinLanes = BinnedIsa::kLanes;
constexpr crd::u64 kBinStreams = BinnedIsa::kStreams;
constexpr crd::u64 kBinStep = kBinStreams * kBinLanes;

// The larger of the lanes of v.
crd::f64 lane_max(BinnedIsa::V v) noexcept
{
    crd::f64 mx[kBinLanes];
    BinnedIsa::store(mx, v);
    crd::f64 amax = mx[0];
    for (crd::u64 l = 1; l < kBinLanes; ++l)
    {
        amax = mx[l] > amax ? mx[l] : amax;
    }
    return amax;
}

// One super-block of deposits over the kBinStep accumulators. TrackMax fuses the |max| scan into the same pass (the
// speculative single-DRAM-pass mode) and returns the block's abs-max (0 when not tracking).
template <bool TrackMax>
crd::f64 deposit_superblock(BinnedRungs (&lanes)[kBinStep], const crd::f64* p, crd::u64 len) noexcept
{
    using I = BinnedIsa;
    const I::V sticky = I::bits(1LL);
    const I::V absmask = I::bits(0x7FFFFFFFFFFFFFFFLL);
    I::V pri[kBinStreams][kBinFold];
    for (crd::u64 s = 0; s < kBinStreams; ++s)
    {
        for (crd::i32 k = 0; k < kBinFold; ++k)
        {
            crd::f64 lv[kBinLanes];
            for (crd::u64 lane = 0; lane < kBinLanes; ++lane)
            {
                lv[lane] = lanes[s * kBinLanes + lane].pri[k];
            }
            pri[s][k] = I::load(lv);
        }
    }
    I::V vmax = I::zero();
    for (crd::u64 k = 0; k < len; k += kBinStep)
    {
        for (crd::u64 s = 0; s < kBinStreams; ++s)
        {
            I::V x = I::load(p + k + s * kBinLanes);
            if constexpr (TrackMax)
            {
                vmax = I::max(vmax, I::bit_and(x, absmask));
            }
            for (crd::i32 r = 0; r < kBinFold - 1; ++r)
            {
                const I::V m = pri[s][r];
                const I::V snew = I::add(I::bit_or(x, sticky), m);
                pri[s][r] = snew;
                x = I::add(x, I::sub(m, snew));
            }
            pri[s][kBinFold - 1] = I::add(pri[s][kBinFold - 1], I::bit_or(x, sticky));
        }
    }
    for (crd::u64 s = 0; s < kBinStreams; ++s)
    {
        for (crd::i32 k = 0; k < kBinFold; ++k)
        {
            crd::f64 lv[kBinLanes];
            I::store(lv, pri[s][k]);
            for (crd::u64 lane = 0; lane < kBinLanes; ++lane)
            {
                lanes[s * kBinLanes + lane].pri[k] = lv[lane];
            }
        }
    }
    if constexpr (TrackMax)
    {
        return lane_max(vmax);
    }
    else
    {
        return 0.0;
    }
}

void binned_sum_run_vector(BinnedRungs& out, const crd::f64* p, crd::u64 n) noexcept
{
    using I = BinnedIsa;
    BinnedRungs lanes[kBinStep] = {};
    crd::i32 cur_index = -1; // no window yet: the first block seeds it two-pass
    crd::u64 i = 0;
    while (i + kBinStep <= n)
    {
        const crd::u64 len0 = ((n - i) / kBinStep) * kBinStep;
        const crd::u64 len = len0 < kBinEndurance * kBinStep ? len0 : kBinEndurance * kBinStep;

        if (cur_index < 0)
        {
            // Seed pass: scan |max| first (reads once from DRAM, the deposit hits L2).
            I::V vmax = I::zero();
            const I::V absmask = I::bits(0x7FFFFFFFFFFFFFFFLL);
            for (crd::u64 k = 0; k < len; k += kBinLanes)
            {
                vmax = I::max(vmax, I::bit_and(I::load(p + i + k), absmask));
            }
            const crd::f64 amax = lane_max(vmax);
            if (detail::bin_index_of(amax) == 0) // near-DBL_MAX regime: the scalar run handles compression
            {
                break;
            }
            for (BinnedRungs& lane : lanes)
            {
                detail::binned_update(lane, amax);
            }
            cur_index = detail::bin_index_of_acc(lanes[0].pri[0]);
            (void)deposit_superblock<false>(lanes, p + i, len);
        }
        else
        {
            // SPECULATIVE single-DRAM-pass: deposit under the previous block's window while tracking |max| in the
            // same registers. On the rare violation (a bigger magnitude arrived: its index < the window's) restore
            // the snapshot, widen, and redo from L2. The result is bit-identical either way.
            BinnedRungs snap[kBinStep];
            for (crd::u64 lane = 0; lane < kBinStep; ++lane)
            {
                snap[lane] = lanes[lane];
            }
            const crd::f64 amax = deposit_superblock<true>(lanes, p + i, len);
            const crd::i32 amax_index = detail::bin_index_of(amax);
            if (amax_index == 0 || amax_index < cur_index)
            {
                for (crd::u64 lane = 0; lane < kBinStep; ++lane)
                {
                    lanes[lane] = snap[lane];
                }
            }
            if (amax_index == 0)
            {
                break; // the scalar run handles the compression regime
            }
            if (amax_index < cur_index)
            {
                for (BinnedRungs& lane : lanes)
                {
                    detail::binned_update(lane, amax);
                }
                cur_index = detail::bin_index_of_acc(lanes[0].pri[0]);
                (void)deposit_superblock<false>(lanes, p + i, len);
            }
        }
        for (BinnedRungs& lane : lanes)
        {
            detail::binned_renorm(lane);
        }
        i += len;
    }
    for (const BinnedRungs& lane : lanes)
    {
        detail::binned_add(out, lane);
    }
    detail::binned_sum_run_scalar(out, p + i, n - i);
}
#endif

void binned_sum_f64(BinnedRungs& acc, const crd::f64* p, crd::u64 n) noexcept
{
#if CRD_SIMD_HAS_SSE2
    if (n >= 8U * kBinStep)
    {
        binned_sum_run_vector(acc, p, n);
        return;
    }
#endif
    detail::binned_sum_run_scalar(acc, p, n);
}

// ---- nn: softmax (last dim, stable) -----------------------------------------
// exp rides the deterministic crd_exp1 core; the widen paths use crd_exp8 (eight lanes, AVX-512, then AVX2's step),
// crd_exp4 (AVX2) and two crd_exp_x2 (SSE2, SSE4.2), which are bit-identical to crd_exp1 on every input, so the
// vector/tail split cannot perturb results.
void softmax_row(const crd::f32* x, crd::f32* y, crd::u64 d) noexcept
{
    crd::f32 m = x[0];
    for (crd::u64 j = 1; j < d; ++j)
    {
        m = x[j] > m ? x[j] : m;
    }
    crd::u64 j = 0;
#if CRD_SIMD_HAS_AVX512
    for (; j + 8U <= d; j += 8U)
    {
        const __m256 xf = _mm256_sub_ps(_mm256_loadu_ps(x + j), _mm256_set1_ps(m));
        _mm256_storeu_ps(y + j, _mm512_cvtpd_ps(crd::math::crd_exp8(_mm512_cvtps_pd(xf))));
    }
#endif
#if CRD_SIMD_HAS_AVX2
    for (; j + 4U <= d; j += 4U)
    {
        const __m128 xf = _mm_sub_ps(_mm_loadu_ps(x + j), _mm_set1_ps(m));
        const __m128 ef = _mm256_cvtpd_ps(crd::math::crd_exp4(_mm256_cvtps_pd(xf)));
        _mm_storeu_ps(y + j, ef);
    }
#elif CRD_SIMD_HAS_SSE2
    for (; j + 4U <= d; j += 4U)
    {
        const __m128 xf = _mm_sub_ps(_mm_loadu_ps(x + j), _mm_set1_ps(m));
        const __m128 lo = _mm_cvtpd_ps(crd::math::crd_exp_x2(_mm_cvtps_pd(xf)));
        const __m128 hi = _mm_cvtpd_ps(crd::math::crd_exp_x2(_mm_cvtps_pd(_mm_movehl_ps(xf, xf))));
        _mm_storeu_ps(y + j, _mm_movelh_ps(lo, hi));
    }
#endif
    for (; j < d; ++j)
    {
        y[j] = static_cast<crd::f32>(crd::math::crd_exp1(static_cast<crd::f64>(x[j] - m)));
    }
    crd::f32 s = 0.0F;
    for (crd::u64 t = 0; t < d; ++t)
    {
        s += y[t];
    }
    for (crd::u64 t = 0; t < d; ++t)
    {
        y[t] /= s;
    }
}

void nn_softmax_rows(const NnSoftmaxF32& c, crd::u32 lo, crd::u32 hi) noexcept
{
    for (crd::u32 r = lo; r < hi; ++r)
    {
        softmax_row(c.x + static_cast<crd::u64>(r) * c.d, c.y + static_cast<crd::u64>(r) * c.d, c.d);
    }
}

#if CRD_SIMD_HAS_SSE2 && !CRD_SIMD_HAS_AVX2
// ---- nn: the 128-bit integer lanes of the SSE2 and SSE4.2 levels ----------------
// Lane k of a 16-byte step is the exact i32 sum of products 4k..4k+3, the lane structure of the AVX2 bodies. SSE4.2
// runs SSSE3's maddubs (u8 x i8 pair sums to i16, then madd by ones); SSE2 has no u8 x i8 multiply, so it widens the
// even and odd bytes of each operand to i16 lanes, and madd of the evens gives products 4k and 4k+2 in lane k, of the
// odds 4k+1 and 4k+3. Every f32 combine is fma_exact.hpp's single-rounded fma.
using crd::math::simd::detail::fma_ps;

#if CRD_SIMD_HAS_SSE42
// The Q8_0 lanes of x and w (both i8, |x| <= 127): psignb puts w's sign on x, and pabsb's |w| is the u8 magnitude even
// for w = -128 (0x80 reads as 128), so maddubs multiplies |w| by x*sign(w) exactly and no i16 pair sum saturates.
inline __m128i q8_lanes(__m128i x, __m128i w) noexcept
{
    return _mm_madd_epi16(_mm_maddubs_epi16(_mm_abs_epi8(w), _mm_sign_epi8(x, w)), _mm_set1_epi16(1));
}

// u8 activations split as x = xl + 128*xh (xl <= 127, xh in {0, 1}), so the maddubs pair sums never saturate.
struct U8Lhs
{
    __m128i low;
    __m128i high;
};

inline U8Lhs u8_lhs(__m128i x) noexcept
{
    return {_mm_and_si128(x, _mm_set1_epi8(0x7F)), _mm_and_si128(_mm_srli_epi16(x, 7), _mm_set1_epi8(1))};
}

struct I8Rhs
{
    __m128i w;
};

inline I8Rhs i8_rhs(__m128i w) noexcept
{
    return {w};
}

inline __m128i u8i8_lanes(const U8Lhs& x, const I8Rhs& w) noexcept
{
    const __m128i ones = _mm_set1_epi16(1);
    const __m128i pl = _mm_madd_epi16(_mm_maddubs_epi16(x.low, w.w), ones);
    const __m128i ph = _mm_madd_epi16(_mm_maddubs_epi16(x.high, w.w), ones);
    return _mm_add_epi32(pl, _mm_slli_epi32(ph, 7));
}
#else
// The even and odd bytes of i8 values, sign-extended to i16 lanes.
inline __m128i even_i8(__m128i v) noexcept
{
    return _mm_srai_epi16(_mm_slli_epi16(v, 8), 8);
}

inline __m128i odd_i8(__m128i v) noexcept
{
    return _mm_srai_epi16(v, 8);
}

inline __m128i q8_lanes(__m128i x, __m128i w) noexcept
{
    return _mm_add_epi32(_mm_madd_epi16(even_i8(x), even_i8(w)), _mm_madd_epi16(odd_i8(x), odd_i8(w)));
}

// The even and odd bytes of u8 values, zero-extended (255 * 128 * 2 fits every madd lane).
struct U8Lhs
{
    __m128i even;
    __m128i odd;
};

inline U8Lhs u8_lhs(__m128i x) noexcept
{
    return {_mm_and_si128(x, _mm_set1_epi16(0x00FF)), _mm_srli_epi16(x, 8)};
}

struct I8Rhs
{
    __m128i even;
    __m128i odd;
};

inline I8Rhs i8_rhs(__m128i w) noexcept
{
    return {even_i8(w), odd_i8(w)};
}

inline __m128i u8i8_lanes(const U8Lhs& x, const I8Rhs& w) noexcept
{
    return _mm_add_epi32(_mm_madd_epi16(x.even, w.even), _mm_madd_epi16(x.odd, w.odd));
}
#endif

// (l0 + l1) + (l2 + l3), the Q8_0 tree within one half of an output's eight lanes.
inline crd::f32 q8_half_sum(__m128 v) noexcept
{
    crd::f32 l[4];
    _mm_storeu_ps(l, v);
    return (l[0] + l[1]) + (l[2] + l[3]);
}

// The exact i32 total of four lanes (order-free).
inline crd::i32 lane_total(__m128i v) noexcept
{
    crd::i32 l[4];
    _mm_storeu_si128(reinterpret_cast<__m128i*>(l), v);
    return (l[0] + l[1]) + (l[2] + l[3]);
}

// y[0..count) = fma(scale, f32(dc), bias) lane for lane, ReLU'd as the ternary form (count <= 4).
inline void i8_store4(crd::f32* y, crd::u32 count, crd::f32 scale, const crd::i32* dc, const crd::f32* bias,
                      bool relu) noexcept
{
    crd::i32 d4[4] = {};
    crd::f32 b4[4] = {};
    for (crd::u32 r = 0; r < count; ++r)
    {
        d4[r] = dc[r];
        b4[r] = bias[r];
    }
    __m128 v = fma_ps(_mm_set1_ps(scale), _mm_cvtepi32_ps(_mm_loadu_si128(reinterpret_cast<const __m128i*>(d4))),
                      _mm_loadu_ps(b4));
    if (relu)
    {
        v = _mm_max_ps(v, _mm_setzero_ps()); // lane semantics == the ternary form
    }
    crd::f32 o[4];
    _mm_storeu_ps(o, v);
    for (crd::u32 r = 0; r < count; ++r)
    {
        y[r] = o[r];
    }
}
#endif

// ---- nn: the Q8_0 linear ----------------------------------------------------
// The lane-parallel Q8_0 kernel (pinned semantics, every body identical): each block's int8 x int8 products land in
// eight exact i32 lane partials (lane k = products 4k..4k+3, the maddubs/madd pair structure; |lane| <= 65024 for any
// i8 weight, so the f32 convert is exact too). The per-block f32 combine is one fma per lane (acc_k = fma(dx*dw,
// f32(lane_k), acc_k)), and the per-output horizontal sum is the fixed hadd tree ((l0+l1)+(l2+l3)) + ((l4+l5)+(l6+l7)).
// Everything is a fixed operation sequence, so the result is bit-exact across worker counts, runs and levels, and the
// integer stage across all hardware (the certification tier). The portable body mirrors the lane structure and the tree
// exactly.
constexpr crd::u32 kNnQ8OutTile = 8U; // outputs sharing one activation-block load (8 acc chains hide the fma latency)

// The VNNI variants (AVX-VNNI on avx2, AVX512-VNNI on avx512) accumulate the 4-product i32 lanes with dpbusd: no
// intermediate saturation is reachable at |x| <= 127 or with the u8 x i8 split, so the lanes are bit-identical to the
// maddubs/madd pair.
#define CRD_TENSOR_VNNI (CRD_KERNEL_HAS_AVXVNNI || CRD_KERNEL_HAS_AVX512VNNI) // NOLINT(cppcoreguidelines-macro-usage)

#if CRD_TENSOR_VNNI
inline __m256i dpbusd256(__m256i acc, __m256i u, __m256i s) noexcept
{
#if CRD_KERNEL_HAS_AVX512VNNI
    return _mm256_dpbusd_epi32(acc, u, s);
#else
    return _mm256_dpbusd_avx_epi32(acc, u, s);
#endif
}
#endif

#if CRD_SIMD_HAS_AVX512
// Two 32-byte rows in one zmm: a in the low half, b in the high one.
inline __m512i load_pair(const void* a, const void* b) noexcept
{
    return _mm512_inserti64x4(_mm512_castsi256_si512(_mm256_loadu_si256(static_cast<const __m256i*>(a))),
                              _mm256_loadu_si256(static_cast<const __m256i*>(b)), 1);
}

// The 4-product i32 lanes of u8 u times i8 s (|s| <= 127 when u reaches 128): dpbusd, or maddubs then madd.
inline __m512i dot_lanes512(__m512i u, __m512i s) noexcept
{
#if CRD_KERNEL_HAS_AVX512VNNI
    return _mm512_dpbusd_epi32(_mm512_setzero_si512(), u, s);
#else
    return _mm512_madd_epi16(_mm512_maddubs_epi16(u, s), _mm512_set1_epi16(1));
#endif
}
#endif

// y[o0..o0+T) over nb blocks. T is a compile-time tile size so the inner loop is fully unrolled straight-line code (a
// runtime bound costs 2-3 loop micro-ops per 3-instruction iteration, measured as the issue-width wall).
template <crd::u32 T>
void q8_dot_tile(const BlockQ8Zero* xrow, const crd::f32* xsc, const BlockQ8Zero* wq, const crd::f32* wscale,
                 crd::u64 nb, crd::u64 o0, const crd::f32* bias, crd::f32* yrow, bool relu) noexcept
{
    constexpr crd::u32 t = T;
#if CRD_SIMD_HAS_AVX2
    __m256 acc[kNnQ8OutTile];
    for (crd::u32 r = 0; r < kNnQ8OutTile; ++r)
    {
        acc[r] = _mm256_setzero_ps();
    }
#if !CRD_TENSOR_VNNI
    const __m256i ones16 = _mm256_set1_epi16(1);
#endif
    crd::u64 bi = 0;
#if CRD_SIMD_HAS_AVX512
    // Two blocks a step in the integer stage (block bi in the low half, bi + 1 in the high one), then each block's f32
    // combine on its own eight lanes in block order: the fma chain of one block a step. AVX-512 has no psignb: x
    // negated where w is negative is x * sign(w) wherever |w| is not 0.
    for (; bi + 2U <= nb; bi += 2U)
    {
        const __m512i vx = load_pair(xrow[bi].qs, xrow[bi + 1U].qs);
        const __m256 vdx0 = _mm256_set1_ps(xsc[bi]);
        const __m256 vdx1 = _mm256_set1_ps(xsc[bi + 1U]);
        for (crd::u32 r = 0; r < t; ++r)
        {
            const BlockQ8Zero* wrow = wq + (o0 + r) * nb;
            const __m512i vw = load_pair(wrow[bi].qs, wrow[bi + 1U].qs);
            const __m512i sx = _mm512_mask_sub_epi8(vx, _mm512_movepi8_mask(vw), _mm512_setzero_si512(), vx);
            const __m512 lanes = _mm512_cvtepi32_ps(dot_lanes512(_mm512_abs_epi8(vw), sx));
            const crd::f32* ws = wscale + (o0 + r) * nb + bi;
            const __m256 scale0 = _mm256_mul_ps(vdx0, _mm256_broadcast_ss(ws));
            const __m256 scale1 = _mm256_mul_ps(vdx1, _mm256_broadcast_ss(ws + 1));
            acc[r] = _mm256_fmadd_ps(scale0, _mm512_castps512_ps256(lanes), acc[r]);
            acc[r] = _mm256_fmadd_ps(scale1, _mm512_extractf32x8_ps(lanes, 1), acc[r]);
        }
    }
#endif
    for (; bi < nb; ++bi)
    {
        // the activations are quantize_q8_0's (|x| <= 127), the weights any i8: |w| (pabsb, 128 for -128) times
        // x * sign(w) (psignb, no wrap at |x| <= 127) is the product exactly
        const __m256i vx = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(xrow[bi].qs));
        const __m256 vdx = _mm256_set1_ps(xsc[bi]);
        for (crd::u32 r = 0; r < t; ++r)
        {
            const BlockQ8Zero* wrow = wq + (o0 + r) * nb;
            const __m256i vw = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(wrow[bi].qs));
            const __m256i aw = _mm256_abs_epi8(vw);
            const __m256i sx = _mm256_sign_epi8(vx, vw);
#if CRD_TENSOR_VNNI
            const __m256i p32 = dpbusd256(_mm256_setzero_si256(), aw, sx);
#else
            const __m256i p16 = _mm256_maddubs_epi16(aw, sx);   // pairs -> i16 (|pair| <= 32512, no saturation)
            const __m256i p32 = _mm256_madd_epi16(p16, ones16); // 4-product i32 lanes
#endif
            const __m256 scale = _mm256_mul_ps(vdx, _mm256_broadcast_ss(wscale + (o0 + r) * nb + bi));
            acc[r] = _mm256_fmadd_ps(scale, _mm256_cvtepi32_ps(p32), acc[r]);
        }
    }
    // 4-output horizontal reductions (the pinned hadd tree per output; outputs group by index mod 4)
    for (crd::u32 g = 0; g < t; g += 4U)
    {
        const crd::u32 tg = t - g < 4U ? t - g : 4U;
        const __m256 h0 = _mm256_hadd_ps(acc[g], acc[g + 1U]);
        const __m256 h1 = _mm256_hadd_ps(acc[g + 2U], acc[g + 3U]);
        const __m256 hh = _mm256_hadd_ps(h0, h1);
        const __m128 r4 = _mm_add_ps(_mm256_castps256_ps128(hh), _mm256_extractf128_ps(hh, 1));
        if (tg == 4U)
        {
            __m128 v = _mm_add_ps(r4, _mm_loadu_ps(bias + o0 + g));
            if (relu)
            {
                v = _mm_max_ps(v, _mm_setzero_ps()); // lane semantics == the ternary form
            }
            _mm_storeu_ps(yrow + o0 + g, v);
            continue;
        }
        alignas(16) crd::f32 d4[4];
        _mm_store_ps(d4, r4);
        for (crd::u32 r = 0; r < tg; ++r)
        {
            const crd::f32 v = d4[r] + bias[o0 + g + r];
            const crd::f32 rectified = v > 0.0F ? v : 0.0F; // ReLU (a NaN becomes 0)
            yrow[o0 + g + r] = relu ? rectified : v;
        }
    }
#elif CRD_SIMD_HAS_SSE2
    __m128 acc_lo[kNnQ8OutTile]; // lanes 0-3 of each output
    __m128 acc_hi[kNnQ8OutTile]; // lanes 4-7
    for (crd::u32 r = 0; r < kNnQ8OutTile; ++r)
    {
        acc_lo[r] = _mm_setzero_ps();
        acc_hi[r] = _mm_setzero_ps();
    }
    for (crd::u64 bi = 0; bi < nb; ++bi)
    {
        const __m128i x_lo = _mm_loadu_si128(reinterpret_cast<const __m128i*>(xrow[bi].qs));
        const __m128i x_hi = _mm_loadu_si128(reinterpret_cast<const __m128i*>(xrow[bi].qs + 16));
        const __m128 vdx = _mm_set1_ps(xsc[bi]);
        for (crd::u32 r = 0; r < t; ++r)
        {
            const crd::i8* w = wq[(o0 + r) * nb + bi].qs;
            const __m128i lanes_lo = q8_lanes(x_lo, _mm_loadu_si128(reinterpret_cast<const __m128i*>(w)));
            const __m128i lanes_hi = q8_lanes(x_hi, _mm_loadu_si128(reinterpret_cast<const __m128i*>(w + 16)));
            const __m128 scale = _mm_mul_ps(vdx, _mm_set1_ps(wscale[(o0 + r) * nb + bi]));
            acc_lo[r] = fma_ps(scale, _mm_cvtepi32_ps(lanes_lo), acc_lo[r]);
            acc_hi[r] = fma_ps(scale, _mm_cvtepi32_ps(lanes_hi), acc_hi[r]);
        }
    }
    for (crd::u32 r = 0; r < t; ++r)
    {
        const crd::f32 v = (q8_half_sum(acc_lo[r]) + q8_half_sum(acc_hi[r])) + bias[o0 + r];
        const crd::f32 rectified = v > 0.0F ? v : 0.0F; // ReLU (a NaN becomes 0)
        yrow[o0 + r] = relu ? rectified : v;
    }
#else
    for (crd::u32 r = 0; r < t; ++r)
    {
        crd::f32 accl[8] = {};
        const BlockQ8Zero* wrow = wq + (o0 + r) * nb;
        for (crd::u64 bi = 0; bi < nb; ++bi)
        {
            const crd::f32 scale = xsc[bi] * wscale[(o0 + r) * nb + bi];
            for (crd::u32 k = 0; k < 8U; ++k) // the exact i32 lane partials (maddubs/madd structure)
            {
                crd::i32 lane = 0;
                for (crd::u32 j = 0; j < 4U; ++j)
                {
                    lane +=
                        static_cast<crd::i32>(xrow[bi].qs[4U * k + j]) * static_cast<crd::i32>(wrow[bi].qs[4U * k + j]);
                }
                accl[k] = crd::math::libm::fma(scale, static_cast<crd::f32>(lane), accl[k]);
            }
        }
        // the fixed horizontal tree, the AVX2 hadd reduction's order
        const crd::f32 q0 = (accl[0] + accl[1]) + (accl[2] + accl[3]);
        const crd::f32 q1 = (accl[4] + accl[5]) + (accl[6] + accl[7]);
        const crd::f32 v = (q0 + q1) + bias[o0 + r];
        const crd::f32 rectified = v > 0.0F ? v : 0.0F; // ReLU (a NaN becomes 0)
        yrow[o0 + r] = relu ? rectified : v;
    }
#endif
}

void q8_dot_tile_any(crd::u32 t, const BlockQ8Zero* xrow, const crd::f32* xsc, const BlockQ8Zero* wq,
                     const crd::f32* wscale, crd::u64 nb, crd::u64 o0, const crd::f32* bias, crd::f32* yrow,
                     bool relu) noexcept
{
    switch (t)
    {
        case 8U:
            q8_dot_tile<8U>(xrow, xsc, wq, wscale, nb, o0, bias, yrow, relu);
            return;
        case 7U:
            q8_dot_tile<7U>(xrow, xsc, wq, wscale, nb, o0, bias, yrow, relu);
            return;
        case 6U:
            q8_dot_tile<6U>(xrow, xsc, wq, wscale, nb, o0, bias, yrow, relu);
            return;
        case 5U:
            q8_dot_tile<5U>(xrow, xsc, wq, wscale, nb, o0, bias, yrow, relu);
            return;
        case 4U:
            q8_dot_tile<4U>(xrow, xsc, wq, wscale, nb, o0, bias, yrow, relu);
            return;
        case 3U:
            q8_dot_tile<3U>(xrow, xsc, wq, wscale, nb, o0, bias, yrow, relu);
            return;
        case 2U:
            q8_dot_tile<2U>(xrow, xsc, wq, wscale, nb, o0, bias, yrow, relu);
            return;
        case 1U:
            q8_dot_tile<1U>(xrow, xsc, wq, wscale, nb, o0, bias, yrow, relu);
            return;
        default:
            return;
    }
}

// The Q8_0 quantizer, bit-identical to quantize_q8_0_blocks (quant_core.hpp) on every input: the lane maxima keep the
// scalar's a > amax ? a : amax (a NaN never wins), the per-lane f32 multiply matches, roundf's half-away form is
// trunc(t + copysign(0.5, t)) (-0 included), and a lane whose scaled value is not finite quantizes as 0.
void quantize_q8_0(const crd::f32* x, BlockQ8Zero* out, crd::u64 nblocks) noexcept
{
#if CRD_SIMD_HAS_AVX512
    const __m512 abs_mask = _mm512_castsi512_ps(_mm512_set1_epi32(0x7FFFFFFF));
    const __m512 sign_mask = _mm512_castsi512_ps(_mm512_set1_epi32(static_cast<crd::i32>(0x80000000U)));
    const __m512 half = _mm512_set1_ps(0.5F);
    const __m256i nan_q = _mm256_set1_epi8(-128); // a non-finite lane converts to INT32_MIN, which narrows to -128
    // vpmovsdb: each i32 saturated to i8 in element order, the packs_epi32 then packs_epi16 chain of AVX2
    const auto away = [&](__m512 t)
    {
        return _mm512_cvtsepi32_epi8(
            _mm512_cvttps_epi32(_mm512_add_ps(t, _mm512_or_ps(_mm512_and_ps(t, sign_mask), half))));
    };
    for (crd::u64 i = 0; i < nblocks; ++i)
    {
        const crd::f32* blk = x + i * kQuantBlock;
        const __m512 v0 = _mm512_loadu_ps(blk);
        const __m512 v1 = _mm512_loadu_ps(blk + 16);
        // maxps(a, b) is a > b ? a : b: with a NaN-free b on every step a NaN never wins
        const __m512 am =
            _mm512_max_ps(_mm512_and_ps(v1, abs_mask), _mm512_max_ps(_mm512_and_ps(v0, abs_mask), _mm512_setzero_ps()));
        const __m256 m8 = _mm256_max_ps(_mm512_castps512_ps256(am), _mm512_extractf32x8_ps(am, 1));
        __m128 m4 = _mm_max_ps(_mm256_castps256_ps128(m8), _mm256_extractf128_ps(m8, 1));
        m4 = _mm_max_ps(m4, _mm_shuffle_ps(m4, m4, 0x4E));
        m4 = _mm_max_ps(m4, _mm_shuffle_ps(m4, m4, 0xB1));
        const crd::f32 amax = _mm_cvtss_f32(m4);
        const crd::f32 d = amax / 127.0F;
        const crd::f32 id = d != 0.0F ? 1.0F / d : 0.0F;
        out[i].d = crd::math::f32_to_f16_bits(d);
        const __m512 vid = _mm512_set1_ps(id);
        const __m128i q0 = away(_mm512_mul_ps(v0, vid));
        const __m128i q1 = away(_mm512_mul_ps(v1, vid));
        const __m256i packed = _mm256_inserti128_si256(_mm256_castsi128_si256(q0), q1, 1);
        // -128 is exactly the non-finite lanes (a finite t rounds into [-127, 127]): they quantize as 0
        const __m256i q = _mm256_andnot_si256(_mm256_cmpeq_epi8(packed, nan_q), packed);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(out[i].qs), q);
    }
#elif CRD_SIMD_HAS_AVX2
    const __m256 abs_mask = _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF));
    const __m256 sign_mask = _mm256_castsi256_ps(_mm256_set1_epi32(static_cast<crd::i32>(0x80000000U)));
    const __m256 half = _mm256_set1_ps(0.5F);
    const __m256i nan_q = _mm256_set1_epi8(-128); // a non-finite lane converts to INT32_MIN, which packs to -128
    const auto away = [&](__m256 t)
    {
        return _mm256_cvttps_epi32(_mm256_add_ps(t, _mm256_or_ps(_mm256_and_ps(t, sign_mask), half)));
    };
    for (crd::u64 i = 0; i < nblocks; ++i)
    {
        const crd::f32* blk = x + i * kQuantBlock;
        const __m256 v0 = _mm256_loadu_ps(blk);
        const __m256 v1 = _mm256_loadu_ps(blk + 8);
        const __m256 v2 = _mm256_loadu_ps(blk + 16);
        const __m256 v3 = _mm256_loadu_ps(blk + 24);
        // maxps(a, b) is a > b ? a : b, which gives b when a is a NaN: with a NaN-free b on every step, a NaN never
        // wins (the scalar's comparison), and two chains of two keep the tree's depth
        const __m256 zero = _mm256_setzero_ps();
        const __m256 a01 = _mm256_max_ps(_mm256_and_ps(v1, abs_mask), _mm256_max_ps(_mm256_and_ps(v0, abs_mask), zero));
        const __m256 a23 = _mm256_max_ps(_mm256_and_ps(v3, abs_mask), _mm256_max_ps(_mm256_and_ps(v2, abs_mask), zero));
        const __m256 am = _mm256_max_ps(a01, a23);
        __m128 m4 = _mm_max_ps(_mm256_castps256_ps128(am), _mm256_extractf128_ps(am, 1));
        m4 = _mm_max_ps(m4, _mm_shuffle_ps(m4, m4, 0x4E));
        m4 = _mm_max_ps(m4, _mm_shuffle_ps(m4, m4, 0xB1));
        const crd::f32 amax = _mm_cvtss_f32(m4);
        const crd::f32 d = amax / 127.0F;
        const crd::f32 id = d != 0.0F ? 1.0F / d : 0.0F;
        out[i].d = crd::math::f32_to_f16_bits(d);
        const __m256 vid = _mm256_set1_ps(id);
        const __m256i q01 = _mm256_packs_epi32(away(_mm256_mul_ps(v0, vid)), away(_mm256_mul_ps(v1, vid)));
        const __m256i q23 = _mm256_packs_epi32(away(_mm256_mul_ps(v2, vid)), away(_mm256_mul_ps(v3, vid)));
        const __m256i packed = _mm256_packs_epi16(q01, q23); // i8, order 0,1|2,3 per 128-bit half
        // a finite t has |t| <= 127 and rounds into [-127, 127], so -128 is exactly the non-finite lanes (a NaN, or
        // +-inf when d is subnormal and id overflows): they quantize as 0
        const __m256i q = _mm256_andnot_si256(_mm256_cmpeq_epi8(packed, nan_q), packed);
        const __m256i fixed = _mm256_permutevar8x32_epi32(q, _mm256_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(out[i].qs), fixed);
    }
#elif CRD_SIMD_HAS_SSE2
    const __m128 abs_mask = _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF));
    const __m128 sign_mask = _mm_castsi128_ps(_mm_set1_epi32(static_cast<crd::i32>(0x80000000U)));
    const __m128 half = _mm_set1_ps(0.5F);
    const __m128i nan_q = _mm_set1_epi8(-128); // a non-finite lane converts to INT32_MIN, which packs to -128
    const auto away = [&](__m128 t)
    {
        return _mm_cvttps_epi32(_mm_add_ps(t, _mm_or_ps(_mm_and_ps(t, sign_mask), half)));
    };
    for (crd::u64 i = 0; i < nblocks; ++i)
    {
        const crd::f32* blk = x + i * kQuantBlock;
        __m128 v[8];
        // maxps(a, b) is a > b ? a : b: with the NaN-free running maximum as b, a NaN never wins (two chains)
        __m128 a0 = _mm_setzero_ps();
        __m128 a1 = _mm_setzero_ps();
        for (crd::u32 j = 0; j < 8U; j += 2U)
        {
            v[j] = _mm_loadu_ps(blk + 4U * j);
            v[j + 1U] = _mm_loadu_ps(blk + 4U * (j + 1U));
            a0 = _mm_max_ps(_mm_and_ps(v[j], abs_mask), a0);
            a1 = _mm_max_ps(_mm_and_ps(v[j + 1U], abs_mask), a1);
        }
        __m128 m4 = _mm_max_ps(a0, a1);
        m4 = _mm_max_ps(m4, _mm_shuffle_ps(m4, m4, 0x4E));
        m4 = _mm_max_ps(m4, _mm_shuffle_ps(m4, m4, 0xB1));
        const crd::f32 amax = _mm_cvtss_f32(m4);
        const crd::f32 d = amax / 127.0F;
        const crd::f32 id = d != 0.0F ? 1.0F / d : 0.0F;
        out[i].d = crd::math::f32_to_f16_bits(d);
        const __m128 vid = _mm_set1_ps(id);
        for (crd::u32 h = 0; h < 2U; ++h)
        {
            const __m128* vh = v + 4U * h;
            const __m128i q01 = _mm_packs_epi32(away(_mm_mul_ps(vh[0], vid)), away(_mm_mul_ps(vh[1], vid)));
            const __m128i q23 = _mm_packs_epi32(away(_mm_mul_ps(vh[2], vid)), away(_mm_mul_ps(vh[3], vid)));
            const __m128i packed = _mm_packs_epi16(q01, q23); // i8 in element order
            // -128 is exactly the non-finite lanes (a finite t rounds into [-127, 127]): they quantize as 0
            const __m128i q = _mm_andnot_si128(_mm_cmpeq_epi8(packed, nan_q), packed);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(out[i].qs + 16U * h), q);
        }
    }
#else
    detail::quantize_q8_0_blocks(x, out, nblocks);
#endif
}

void nn_linear_q8_rows(const NnLinearQ8& c, crd::u32 lo, crd::u32 hi) noexcept
{
    const crd::u64 nb = c.k / kQuantBlock;
    for (crd::u32 m = lo; m < hi; ++m)
    {
        BlockQ8Zero* xrow = c.xq + static_cast<crd::u64>(m) * nb;
        crd::f32* xsc = c.xscale + static_cast<crd::u64>(m) * nb;
        quantize_q8_0(c.x + static_cast<crd::u64>(m) * c.k, xrow, nb);
        for (crd::u64 bi = 0; bi < nb; ++bi) // widen the row scales once (never in the o loop)
        {
            xsc[bi] = crd::math::f16_bits_to_f32(xrow[bi].d);
        }
        crd::f32* yrow = c.y + static_cast<crd::u64>(m) * c.n;
        for (crd::u64 o0 = 0; o0 < c.n; o0 += kNnQ8OutTile)
        {
            const crd::u32 t = c.n - o0 < kNnQ8OutTile ? static_cast<crd::u32>(c.n - o0) : kNnQ8OutTile;
            q8_dot_tile_any(t, xrow, xsc, c.wq, c.wscale, nb, o0, c.bias, yrow, c.relu);
        }
    }
}

// ---- nn: the per-tensor int8 linear (the throughput tier) -------------------
// Scheme (the ort DynamicQuantizeLinear + MatMulInteger class): weights are per-tensor symmetric i8 (one f32 scale,
// quantized at load, with precomputed per-output column sums); activations are per-row asymmetric u8 (dynamic scale
// and zero point over the 0-extended row range, computed inside the row task: single pass and partition-local, so the
// worker moat holds by construction; u8 captures the full range of post-ReLU rows). The inner kernel is a pure
// u8 x i8 -> i32 dot over the whole row (one dpbusd per 32 values under AVX-VNNI: no sign fixup, no per-block scale),
// and the epilogue is the integer zero-point correction (idot - zp*colsum, exact i32) and one fma:
// y = fma(sx*sw, f32(idot - zp*colsum), bias). The integer stages are exact in any summation order on every body
// (vector, VNNI and scalar give identical integers by arithmetic identity); the f32 convert is exact for K <= 516 in
// the worst case and correctly rounded (still deterministic) above. Q8_0 block-32 stays the default tier.

// The per-row u8 quantizer, bit-identical to quantize_u8_asym_row (quant_core.hpp) on every input: the lane minima and
// maxima keep the scalar's comparisons (a NaN never enters the range), the same f32 operations, the half-away
// trunc(t + copysign(0.5, t)) form lane for lane with a non-finite lane at the zero point, and packus saturation == the
// scalar [0, 255] clamp. k must be a multiple of 32.
void quantize_u8_row(const crd::f32* x, crd::u64 k, crd::u8* out, crd::f32& scale, crd::i32& zero_point) noexcept
{
#if CRD_SIMD_HAS_AVX512
    __m512 vmin = _mm512_setzero_ps(); // the range is 0-extended (the ort convention)
    __m512 vmax = _mm512_setzero_ps();
    for (crd::u64 i = 0; i < k; i += 16U)
    {
        const __m512 v = _mm512_loadu_ps(x + i);
        vmin = _mm512_min_ps(v, vmin); // v < vmin ? v : vmin
        vmax = _mm512_max_ps(v, vmax); // v > vmax ? v : vmax
    }
    // the lanes hold +0 or values of one sign, so the reduction order does not matter
    const __m256 lo8 = _mm256_min_ps(_mm512_castps512_ps256(vmin), _mm512_extractf32x8_ps(vmin, 1));
    const __m256 hi8 = _mm256_max_ps(_mm512_castps512_ps256(vmax), _mm512_extractf32x8_ps(vmax, 1));
    __m128 lo4 = _mm_min_ps(_mm256_castps256_ps128(lo8), _mm256_extractf128_ps(lo8, 1));
    lo4 = _mm_min_ps(lo4, _mm_shuffle_ps(lo4, lo4, 0x4E));
    lo4 = _mm_min_ps(lo4, _mm_shuffle_ps(lo4, lo4, 0xB1));
    __m128 hi4 = _mm_max_ps(_mm256_castps256_ps128(hi8), _mm256_extractf128_ps(hi8, 1));
    hi4 = _mm_max_ps(hi4, _mm_shuffle_ps(hi4, hi4, 0x4E));
    hi4 = _mm_max_ps(hi4, _mm_shuffle_ps(hi4, hi4, 0xB1));
    const crd::f32 rmin = _mm_cvtss_f32(lo4);
    const crd::f32 rmax = _mm_cvtss_f32(hi4);
    const crd::f32 s = (rmax - rmin) / 255.0F;
    const crd::f32 inv = s != 0.0F ? 1.0F / s : 0.0F;
    const crd::i32 zp = detail::quantize_u8_zero_point(rmin, s);
    const __m512 vinv = _mm512_set1_ps(inv);
    const __m512i vzp = _mm512_set1_epi32(zp);
    const __m512 sign_mask = _mm512_castsi512_ps(_mm512_set1_epi32(static_cast<crd::i32>(0x80000000U)));
    const __m512 abs_mask = _mm512_castsi512_ps(_mm512_set1_epi32(0x7FFFFFFF));
    const __m512 inf = _mm512_castsi512_ps(_mm512_set1_epi32(0x7F800000));
    const __m512 half = _mm512_set1_ps(0.5F);
    for (crd::u64 i = 0; i < k; i += 16U)
    {
        const __m512 t = _mm512_mul_ps(_mm512_loadu_ps(x + i), vinv);
        const __m512 finite = _mm512_maskz_mov_ps(_mm512_cmp_ps_mask(_mm512_and_ps(t, abs_mask), inf, _CMP_LT_OQ), t);
        const __m512i q = _mm512_add_epi32(
            _mm512_cvttps_epi32(_mm512_add_ps(finite, _mm512_or_ps(_mm512_and_ps(finite, sign_mask), half))), vzp);
        // packus's [0, 255] clamp: the lanes raised to 0, then vpmovusdb's unsigned saturation (it reads a negative
        // lane as a large unsigned one, so the raise comes first)
        const __m128i bytes = _mm512_cvtusepi32_epi8(_mm512_max_epi32(q, _mm512_setzero_si512()));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out + i), bytes);
    }
    scale = s;
    zero_point = zp;
#elif CRD_SIMD_HAS_AVX2
    __m256 vmin = _mm256_setzero_ps(); // the range is 0-extended (the ort convention)
    __m256 vmax = _mm256_setzero_ps();
    for (crd::u64 i = 0; i < k; i += 8U)
    {
        const __m256 v = _mm256_loadu_ps(x + i);
        vmin = _mm256_min_ps(v, vmin); // v < vmin ? v : vmin
        vmax = _mm256_max_ps(v, vmax); // v > vmax ? v : vmax
    }
    __m128 lo4 = _mm_min_ps(_mm256_castps256_ps128(vmin), _mm256_extractf128_ps(vmin, 1));
    lo4 = _mm_min_ps(lo4, _mm_shuffle_ps(lo4, lo4, 0x4E));
    lo4 = _mm_min_ps(lo4, _mm_shuffle_ps(lo4, lo4, 0xB1));
    __m128 hi4 = _mm_max_ps(_mm256_castps256_ps128(vmax), _mm256_extractf128_ps(vmax, 1));
    hi4 = _mm_max_ps(hi4, _mm_shuffle_ps(hi4, hi4, 0x4E));
    hi4 = _mm_max_ps(hi4, _mm_shuffle_ps(hi4, hi4, 0xB1));
    const crd::f32 rmin = _mm_cvtss_f32(lo4);
    const crd::f32 rmax = _mm_cvtss_f32(hi4);
    const crd::f32 s = (rmax - rmin) / 255.0F;
    const crd::f32 inv = s != 0.0F ? 1.0F / s : 0.0F;
    const crd::i32 zp = detail::quantize_u8_zero_point(rmin, s);
    const __m256 vinv = _mm256_set1_ps(inv);
    const __m256i vzp = _mm256_set1_epi32(zp);
    const __m256 sign_mask = _mm256_castsi256_ps(_mm256_set1_epi32(static_cast<crd::i32>(0x80000000U)));
    const __m256 abs_mask = _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF));
    const __m256 inf = _mm256_castsi256_ps(_mm256_set1_epi32(0x7F800000));
    const __m256 half = _mm256_set1_ps(0.5F);
    const auto away_zp = [&](__m256 t)
    {
        const __m256 finite = _mm256_and_ps(t, _mm256_cmp_ps(_mm256_and_ps(t, abs_mask), inf, _CMP_LT_OQ)); // else +0
        const __m256i q =
            _mm256_cvttps_epi32(_mm256_add_ps(finite, _mm256_or_ps(_mm256_and_ps(finite, sign_mask), half)));
        return _mm256_add_epi32(q, vzp);
    };
    for (crd::u64 i = 0; i < k; i += 32U)
    {
        const __m256i q0 = away_zp(_mm256_mul_ps(_mm256_loadu_ps(x + i), vinv));
        const __m256i q1 = away_zp(_mm256_mul_ps(_mm256_loadu_ps(x + i + 8), vinv));
        const __m256i q2 = away_zp(_mm256_mul_ps(_mm256_loadu_ps(x + i + 16), vinv));
        const __m256i q3 = away_zp(_mm256_mul_ps(_mm256_loadu_ps(x + i + 24), vinv));
        const __m256i q01 = _mm256_packus_epi32(q0, q1); // unsigned saturate == clamp [0, 255]
        const __m256i q23 = _mm256_packus_epi32(q2, q3);
        const __m256i q = _mm256_packus_epi16(q01, q23);
        const __m256i fixed = _mm256_permutevar8x32_epi32(q, _mm256_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + i), fixed);
    }
    scale = s;
    zero_point = zp;
#elif CRD_SIMD_HAS_SSE2
    __m128 vmin = _mm_setzero_ps(); // the range is 0-extended (the ort convention)
    __m128 vmax = _mm_setzero_ps();
    for (crd::u64 i = 0; i < k; i += 4U)
    {
        const __m128 v = _mm_loadu_ps(x + i);
        vmin = _mm_min_ps(v, vmin); // v < vmin ? v : vmin
        vmax = _mm_max_ps(v, vmax); // v > vmax ? v : vmax
    }
    vmin = _mm_min_ps(vmin, _mm_shuffle_ps(vmin, vmin, 0x4E));
    vmin = _mm_min_ps(vmin, _mm_shuffle_ps(vmin, vmin, 0xB1));
    vmax = _mm_max_ps(vmax, _mm_shuffle_ps(vmax, vmax, 0x4E));
    vmax = _mm_max_ps(vmax, _mm_shuffle_ps(vmax, vmax, 0xB1));
    const crd::f32 rmin = _mm_cvtss_f32(vmin);
    const crd::f32 rmax = _mm_cvtss_f32(vmax);
    const crd::f32 s = (rmax - rmin) / 255.0F;
    const crd::f32 inv = s != 0.0F ? 1.0F / s : 0.0F;
    const crd::i32 zp = detail::quantize_u8_zero_point(rmin, s);
    const __m128 vinv = _mm_set1_ps(inv);
    const __m128i vzp = _mm_set1_epi32(zp);
    const __m128 sign_mask = _mm_castsi128_ps(_mm_set1_epi32(static_cast<crd::i32>(0x80000000U)));
    const __m128 abs_mask = _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF));
    const __m128 inf = _mm_castsi128_ps(_mm_set1_epi32(0x7F800000));
    const __m128 half = _mm_set1_ps(0.5F);
    const auto away_zp = [&](__m128 t)
    {
        const __m128 finite = _mm_and_ps(t, _mm_cmplt_ps(_mm_and_ps(t, abs_mask), inf)); // else +0
        const __m128i q = _mm_cvttps_epi32(_mm_add_ps(finite, _mm_or_ps(_mm_and_ps(finite, sign_mask), half)));
        return _mm_add_epi32(q, vzp);
    };
    for (crd::u64 i = 0; i < k; i += 16U)
    {
        const __m128i q0 = away_zp(_mm_mul_ps(_mm_loadu_ps(x + i), vinv));
        const __m128i q1 = away_zp(_mm_mul_ps(_mm_loadu_ps(x + i + 4), vinv));
        const __m128i q2 = away_zp(_mm_mul_ps(_mm_loadu_ps(x + i + 8), vinv));
        const __m128i q3 = away_zp(_mm_mul_ps(_mm_loadu_ps(x + i + 12), vinv));
        // packs_epi32 then packus_epi16 is the [0, 255] clamp: both saturations are monotone, and an i32 outside
        // [0, 255] saturates to an i16 outside it too
        const __m128i q = _mm_packus_epi16(_mm_packs_epi32(q0, q1), _mm_packs_epi32(q2, q3));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out + i), q);
    }
    scale = s;
    zero_point = zp;
#else
    detail::quantize_u8_asym_row(x, k, out, scale, zero_point);
#endif
}

// y[o0..o0+T) over the whole K in exact i32, the integer zero-point correction and one fma per output. 8 independent
// i32 accumulator chains hide the dpbusd/madd latency; T is compile-time so the inner loop is fully unrolled.
template <crd::u32 T>
void i8_dot_tile(const crd::u8* xrow, const crd::i8* wq, const crd::i32* colsum, crd::u64 k, crd::u64 o0,
                 crd::f32 scale_m, crd::i32 zp, const crd::f32* bias, crd::f32* yrow, bool relu) noexcept
{
    constexpr crd::u32 t = T;
#if CRD_SIMD_HAS_AVX2
    __m256i acc[8];
    for (crd::u32 r = 0; r < 8U; ++r)
    {
        acc[r] = _mm256_setzero_si256();
    }
#if !CRD_TENSOR_VNNI
    const __m256i ones16 = _mm256_set1_epi16(1);
    const __m256i low7 = _mm256_set1_epi8(0x7F);
    const __m256i one8 = _mm256_set1_epi8(1);
#endif
    crd::u64 c = 0;
#if CRD_SIMD_HAS_AVX512
    {
        // 64 values a step on zmm lanes, folded exactly into the ymm accumulators for a last 32 and the reduction
        __m512i wide[8];
        for (crd::u32 r = 0; r < 8U; ++r)
        {
            wide[r] = _mm512_setzero_si512();
        }
#if !CRD_TENSOR_VNNI
        const __m512i ones16z = _mm512_set1_epi16(1);
        const __m512i low7z = _mm512_set1_epi8(0x7F);
        const __m512i one8z = _mm512_set1_epi8(1);
#endif
        for (; c + 64U <= k; c += 64U)
        {
            const __m512i vx = _mm512_loadu_si512(xrow + c); // u8
#if !CRD_TENSOR_VNNI
            const __m512i xl = _mm512_and_si512(vx, low7z);
            const __m512i xh = _mm512_and_si512(_mm512_srli_epi16(vx, 7), one8z);
#endif
            for (crd::u32 r = 0; r < t; ++r)
            {
                const __m512i vw = _mm512_loadu_si512(wq + (o0 + r) * k + c);
#if CRD_TENSOR_VNNI
                wide[r] = _mm512_dpbusd_epi32(wide[r], vx, vw);
#else
                const __m512i pl = _mm512_madd_epi16(_mm512_maddubs_epi16(xl, vw), ones16z);
                const __m512i ph = _mm512_madd_epi16(_mm512_maddubs_epi16(xh, vw), ones16z);
                wide[r] = _mm512_add_epi32(wide[r], _mm512_add_epi32(pl, _mm512_slli_epi32(ph, 7)));
#endif
            }
        }
        for (crd::u32 r = 0; r < 8U; ++r)
        {
            acc[r] = _mm256_add_epi32(_mm512_castsi512_si256(wide[r]), _mm512_extracti64x4_epi64(wide[r], 1));
        }
    }
#endif
    for (; c < k; c += 32U)
    {
        const __m256i vx = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(xrow + c)); // u8
#if !CRD_TENSOR_VNNI
        // u8 x i8 without VNNI: split x = xl + 128*xh (xl <= 127, xh in {0,1}) so the maddubs i16 pair sums can
        // never saturate; exact i32 either way
        const __m256i xl = _mm256_and_si256(vx, low7);
        const __m256i xh = _mm256_and_si256(_mm256_srli_epi16(vx, 7), one8);
#endif
        for (crd::u32 r = 0; r < t; ++r)
        {
            const __m256i vw = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(wq + (o0 + r) * k + c));
#if CRD_TENSOR_VNNI
            acc[r] = dpbusd256(acc[r], vx, vw); // u8 x i8, exact i32 accumulate
#else
            const __m256i pl = _mm256_madd_epi16(_mm256_maddubs_epi16(xl, vw), ones16);
            const __m256i ph = _mm256_madd_epi16(_mm256_maddubs_epi16(xh, vw), ones16);
            acc[r] = _mm256_add_epi32(acc[r], _mm256_add_epi32(pl, _mm256_slli_epi32(ph, 7)));
#endif
        }
    }
    // 4-output integer horizontal reductions and the zero-point correction (exact)
    for (crd::u32 g = 0; g < t; g += 4U)
    {
        const crd::u32 tg = t - g < 4U ? t - g : 4U;
        const __m256i h0 = _mm256_hadd_epi32(acc[g], acc[g + 1U]);
        const __m256i h1 = _mm256_hadd_epi32(acc[g + 2U], acc[g + 3U]);
        const __m256i hh = _mm256_hadd_epi32(h0, h1);
        const __m128i r4 = _mm_add_epi32(_mm256_castsi256_si128(hh), _mm256_extracti128_si256(hh, 1));
        if (tg == 4U)
        {
            const __m128i corr =
                _mm_mullo_epi32(_mm_set1_epi32(zp), _mm_loadu_si128(reinterpret_cast<const __m128i*>(colsum + o0 + g)));
            __m128 v = _mm_fmadd_ps(_mm_set1_ps(scale_m), _mm_cvtepi32_ps(_mm_sub_epi32(r4, corr)),
                                    _mm_loadu_ps(bias + o0 + g));
            if (relu)
            {
                v = _mm_max_ps(v, _mm_setzero_ps()); // lane semantics == the ternary form
            }
            _mm_storeu_ps(yrow + o0 + g, v);
            continue;
        }
        alignas(16) crd::i32 d4[4]; // tail: scalar correction (no 4-wide colsum read past N)
        _mm_store_si128(reinterpret_cast<__m128i*>(d4), r4);
        for (crd::u32 r = 0; r < tg; ++r)
        {
            const crd::i32 dc = d4[r] - zp * colsum[o0 + g + r];
            const crd::f32 v = crd::math::libm::fma(scale_m, static_cast<crd::f32>(dc), bias[o0 + g + r]);
            const crd::f32 rectified = v > 0.0F ? v : 0.0F; // ReLU (a NaN becomes 0)
            yrow[o0 + g + r] = relu ? rectified : v;
        }
    }
#elif CRD_SIMD_HAS_SSE2
    __m128i acc[8];
    for (crd::u32 r = 0; r < 8U; ++r)
    {
        acc[r] = _mm_setzero_si128();
    }
    for (crd::u64 c = 0; c < k; c += 16U)
    {
        const U8Lhs vx = u8_lhs(_mm_loadu_si128(reinterpret_cast<const __m128i*>(xrow + c)));
        for (crd::u32 r = 0; r < t; ++r)
        {
            const __m128i vw = _mm_loadu_si128(reinterpret_cast<const __m128i*>(wq + (o0 + r) * k + c));
            acc[r] = _mm_add_epi32(acc[r], u8i8_lanes(vx, i8_rhs(vw)));
        }
    }
    for (crd::u32 g = 0; g < t; g += 4U)
    {
        const crd::u32 tg = t - g < 4U ? t - g : 4U;
        crd::i32 dc[4];
        for (crd::u32 r = 0; r < tg; ++r)
        {
            dc[r] = lane_total(acc[g + r]) - zp * colsum[o0 + g + r]; // exact zero-point correction
        }
        i8_store4(yrow + o0 + g, tg, scale_m, dc, bias + o0 + g, relu);
    }
#else
    for (crd::u32 r = 0; r < t; ++r)
    {
        crd::i32 idot = 0; // exact integer: order-free, identical to the vector path by arithmetic
        const crd::i8* wrow = wq + (o0 + r) * k;
        for (crd::u64 j = 0; j < k; ++j)
        {
            idot += static_cast<crd::i32>(xrow[j]) * static_cast<crd::i32>(wrow[j]);
        }
        idot -= zp * colsum[o0 + r]; // exact zero-point correction
        const crd::f32 v = crd::math::libm::fma(scale_m, static_cast<crd::f32>(idot), bias[o0 + r]);
        const crd::f32 rectified = v > 0.0F ? v : 0.0F; // ReLU (a NaN becomes 0)
        yrow[o0 + r] = relu ? rectified : v;
    }
#endif
}

void i8_dot_tile_any(crd::u32 t, const crd::u8* xrow, const crd::i8* wq, const crd::i32* colsum, crd::u64 k,
                     crd::u64 o0, crd::f32 scale_m, crd::i32 zp, const crd::f32* bias, crd::f32* yrow,
                     bool relu) noexcept
{
    switch (t)
    {
        case 8U:
            i8_dot_tile<8U>(xrow, wq, colsum, k, o0, scale_m, zp, bias, yrow, relu);
            return;
        case 7U:
            i8_dot_tile<7U>(xrow, wq, colsum, k, o0, scale_m, zp, bias, yrow, relu);
            return;
        case 6U:
            i8_dot_tile<6U>(xrow, wq, colsum, k, o0, scale_m, zp, bias, yrow, relu);
            return;
        case 5U:
            i8_dot_tile<5U>(xrow, wq, colsum, k, o0, scale_m, zp, bias, yrow, relu);
            return;
        case 4U:
            i8_dot_tile<4U>(xrow, wq, colsum, k, o0, scale_m, zp, bias, yrow, relu);
            return;
        case 3U:
            i8_dot_tile<3U>(xrow, wq, colsum, k, o0, scale_m, zp, bias, yrow, relu);
            return;
        case 2U:
            i8_dot_tile<2U>(xrow, wq, colsum, k, o0, scale_m, zp, bias, yrow, relu);
            return;
        case 1U:
            i8_dot_tile<1U>(xrow, wq, colsum, k, o0, scale_m, zp, bias, yrow, relu);
            return;
        default:
            return;
    }
}

// The row-major reference path (nn_linear_i8's unit surface; the NnSequential runner rides the packed kernel below,
// gated bit-identical against this and the scalar formula).
void nn_linear_i8_rows(const NnLinearI8& c, crd::u32 lo, crd::u32 hi) noexcept
{
    for (crd::u32 m = lo; m < hi; ++m)
    {
        crd::u8* xrow = c.xq + static_cast<crd::u64>(m) * c.k;
        quantize_u8_row(c.x + static_cast<crd::u64>(m) * c.k, c.k, xrow, c.xscale[m], c.xzp[m]);
        const crd::f32 scale_m = c.xscale[m] * c.wscale;
        crd::f32* yrow = c.y + static_cast<crd::u64>(m) * c.n;
        for (crd::u64 o0 = 0; o0 < c.n; o0 += 8U)
        {
            const crd::u32 t = c.n - o0 < 8U ? static_cast<crd::u32>(c.n - o0) : 8U;
            i8_dot_tile_any(t, xrow, c.wq, c.colsum, c.k, o0, scale_m, c.xzp[m], c.bias, yrow, c.relu);
        }
    }
}

// ---- nn: the packed runner kernel (the MLAS structure) ----------------------
// Weights are repacked at load (nn.hpp i8_pack_weights) to [ceil(N/8) blocks][K/4 slices][8 outputs x 4 k-bytes], so
// one dpbusd accumulates 4 k-steps for 8 outputs at once: the accumulator vector is 8 outputs (no horizontal
// reduction, no per-output accumulator chains), activations broadcast 4 bytes at a time, and R rows share every weight
// load. Integer results are the exact whole-row dots, identical to the row-major kernel and the scalar formula by
// arithmetic identity (gate-enforced).
#if CRD_SIMD_HAS_AVX2
alignas(32) constexpr crd::i32 kNnTailMask[16] = {-1, -1, -1, -1, -1, -1, -1, -1, 0, 0, 0, 0, 0, 0, 0, 0};
#endif

// R rows (R <= 4) against the packed weights; colsum and bias are n8-padded.
template <crd::u32 R>
void i8_packed_rows(const crd::u8* xrows, const crd::i8* wpack, const crd::i32* colsum, crd::u64 k, crd::u64 n,
                    const crd::f32* scale_m, const crd::i32* zp, const crd::f32* bias, crd::f32* yrows,
                    bool relu) noexcept
{
#if CRD_SIMD_HAS_AVX2
    const crd::u64 n8 = (n + 7U) & ~crd::u64{7};
#if !CRD_TENSOR_VNNI
    const __m256i ones16 = _mm256_set1_epi16(1);
    const __m256i low7 = _mm256_set1_epi8(0x7F);
    const __m256i one8 = _mm256_set1_epi8(1);
#endif
    const __m256 zero = _mm256_setzero_ps();
    crd::u64 b = 0;
#if CRD_SIMD_HAS_AVX512
    // Two blocks a step, 16 outputs: block b's slice in the low half, block b + 8's in the high one, one activation
    // broadcast for both; an odd last block takes the 8-output step below.
    {
#if !CRD_TENSOR_VNNI
        const __m512i ones16z = _mm512_set1_epi16(1);
        const __m512i low7z = _mm512_set1_epi8(0x7F);
        const __m512i one8z = _mm512_set1_epi8(1);
#endif
        for (; b + 16U <= n8; b += 16U)
        {
            __m512i acc[R];
            for (crd::u32 r = 0; r < R; ++r)
            {
                acc[r] = _mm512_setzero_si512();
            }
            const crd::i8* wp0 = wpack + b * k;
            const crd::i8* wp1 = wp0 + 8U * k;
            for (crd::u64 c = 0; c < k; c += 4U)
            {
                const __m512i vb = load_pair(wp0 + c * 8U, wp1 + c * 8U);
                for (crd::u32 r = 0; r < R; ++r)
                {
                    crd::i32 xw = 0;
                    std::memcpy(&xw, xrows + static_cast<crd::u64>(r) * k + c, 4U);
                    const __m512i va = _mm512_set1_epi32(xw);
#if CRD_TENSOR_VNNI
                    acc[r] = _mm512_dpbusd_epi32(acc[r], va, vb);
#else
                    const __m512i xl = _mm512_and_si512(va, low7z);
                    const __m512i xh = _mm512_and_si512(_mm512_srli_epi16(va, 7), one8z);
                    const __m512i pl = _mm512_madd_epi16(_mm512_maddubs_epi16(xl, vb), ones16z);
                    const __m512i ph = _mm512_madd_epi16(_mm512_maddubs_epi16(xh, vb), ones16z);
                    acc[r] = _mm512_add_epi32(acc[r], _mm512_add_epi32(pl, _mm512_slli_epi32(ph, 7)));
#endif
                }
            }
            const crd::u64 valid = n - b < 16U ? n - b : 16U; // more than 8: block b + 8 holds an output
            const __mmask16 keep = static_cast<__mmask16>((1U << valid) - 1U);
            const __m512i cs16 = _mm512_loadu_si512(colsum + b);
            const __m512 b16 = _mm512_loadu_ps(bias + b);
            for (crd::u32 r = 0; r < R; ++r)
            {
                const __m512i corr = _mm512_mullo_epi32(_mm512_set1_epi32(zp[r]), cs16);
                const __m512 dots = _mm512_cvtepi32_ps(_mm512_sub_epi32(acc[r], corr));
                __m512 v = _mm512_fmadd_ps(_mm512_set1_ps(scale_m[r]), dots, b16);
                if (relu)
                {
                    v = _mm512_max_ps(v, _mm512_setzero_ps()); // lane semantics == the ternary form
                }
                _mm512_mask_storeu_ps(yrows + static_cast<crd::u64>(r) * n + b, keep, v);
            }
        }
    }
#endif
    for (; b < n8; b += 8U)
    {
        __m256i acc[R];
        for (crd::u32 r = 0; r < R; ++r)
        {
            acc[r] = _mm256_setzero_si256();
        }
        const crd::i8* wp = wpack + b * k; // this block: k/4 slices of 8x4 bytes
        for (crd::u64 c = 0; c < k; c += 4U)
        {
            const __m256i vb = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(wp + c * 8U));
            for (crd::u32 r = 0; r < R; ++r)
            {
                crd::i32 xw = 0; // 4 u8 activation bytes, broadcast to every lane
                std::memcpy(&xw, xrows + static_cast<crd::u64>(r) * k + c, 4U);
                const __m256i va = _mm256_set1_epi32(xw);
#if CRD_TENSOR_VNNI
                acc[r] = dpbusd256(acc[r], va, vb); // u8 x i8, exact i32
#else
                // u8 x i8 without VNNI: split x = xl + 128*xh so the maddubs i16 pair sums can never saturate;
                // exact i32 either way
                const __m256i xl = _mm256_and_si256(va, low7);
                const __m256i xh = _mm256_and_si256(_mm256_srli_epi16(va, 7), one8);
                const __m256i pl = _mm256_madd_epi16(_mm256_maddubs_epi16(xl, vb), ones16);
                const __m256i ph = _mm256_madd_epi16(_mm256_maddubs_epi16(xh, vb), ones16);
                acc[r] = _mm256_add_epi32(acc[r], _mm256_add_epi32(pl, _mm256_slli_epi32(ph, 7)));
#endif
            }
        }
        const crd::u64 valid = n - b < 8U ? n - b : 8U;
        const __m256i cs8 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(colsum + b));
        const __m256 b8 = _mm256_loadu_ps(bias + b);
        for (crd::u32 r = 0; r < R; ++r)
        {
            const __m256i corr = _mm256_mullo_epi32(_mm256_set1_epi32(zp[r]), cs8);
            __m256 v =
                _mm256_fmadd_ps(_mm256_set1_ps(scale_m[r]), _mm256_cvtepi32_ps(_mm256_sub_epi32(acc[r], corr)), b8);
            if (relu)
            {
                v = _mm256_max_ps(v, zero); // lane semantics == the ternary form
            }
            crd::f32* dst = yrows + static_cast<crd::u64>(r) * n + b;
            if (valid == 8U)
            {
                _mm256_storeu_ps(dst, v);
            }
            else
            {
                const __m256i vmask = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(kNnTailMask + (8U - valid)));
                _mm256_maskstore_ps(dst, vmask, v);
            }
        }
    }
#elif CRD_SIMD_HAS_SSE2
    const crd::u64 n8 = (n + 7U) & ~crd::u64{7};
    for (crd::u64 b = 0; b < n8; b += 8U)
    {
        __m128i acc_lo[R]; // outputs b..b+3
        __m128i acc_hi[R]; // outputs b+4..b+7
        for (crd::u32 r = 0; r < R; ++r)
        {
            acc_lo[r] = _mm_setzero_si128();
            acc_hi[r] = _mm_setzero_si128();
        }
        const crd::i8* wp = wpack + b * k; // this block: k/4 slices of 8x4 bytes
        for (crd::u64 c = 0; c < k; c += 4U)
        {
            const I8Rhs w_lo = i8_rhs(_mm_loadu_si128(reinterpret_cast<const __m128i*>(wp + c * 8U)));
            const I8Rhs w_hi = i8_rhs(_mm_loadu_si128(reinterpret_cast<const __m128i*>(wp + c * 8U + 16U)));
            for (crd::u32 r = 0; r < R; ++r)
            {
                crd::i32 xw = 0; // 4 u8 activation bytes, broadcast to every lane
                std::memcpy(&xw, xrows + static_cast<crd::u64>(r) * k + c, 4U);
                const U8Lhs va = u8_lhs(_mm_set1_epi32(xw));
                acc_lo[r] = _mm_add_epi32(acc_lo[r], u8i8_lanes(va, w_lo));
                acc_hi[r] = _mm_add_epi32(acc_hi[r], u8i8_lanes(va, w_hi));
            }
        }
        const crd::u64 valid = n - b < 8U ? n - b : 8U;
        for (crd::u32 r = 0; r < R; ++r)
        {
            crd::i32 dc[8];
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dc), acc_lo[r]);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dc + 4), acc_hi[r]);
            for (crd::u32 o = 0; o < 8U; ++o)
            {
                dc[o] -= zp[r] * colsum[b + o]; // exact zero-point correction (colsum is n8-padded)
            }
            crd::f32* dst = yrows + static_cast<crd::u64>(r) * n + b;
            const crd::u32 first = valid < 4U ? static_cast<crd::u32>(valid) : 4U;
            i8_store4(dst, first, scale_m[r], dc, bias + b, relu);
            if (valid > 4U)
            {
                i8_store4(dst + 4, static_cast<crd::u32>(valid - 4U), scale_m[r], dc + 4, bias + b + 4, relu);
            }
        }
    }
#else
    for (crd::u32 r = 0; r < R; ++r)
    {
        for (crd::u64 o = 0; o < n; ++o)
        {
            crd::i32 idot = 0; // exact integer over the packed layout, order-free
            for (crd::u64 c = 0; c < k; c += 4U)
            {
                for (crd::u64 j = 0; j < 4U; ++j)
                {
                    const crd::i8 wv = wpack[(o / 8U) * (k * 8U) + (c / 4U) * 32U + (o % 8U) * 4U + j];
                    idot +=
                        static_cast<crd::i32>(xrows[static_cast<crd::u64>(r) * k + c + j]) * static_cast<crd::i32>(wv);
                }
            }
            idot -= zp[r] * colsum[o];
            const crd::f32 v = crd::math::libm::fma(scale_m[r], static_cast<crd::f32>(idot), bias[o]);
            const crd::f32 rectified = v > 0.0F ? v : 0.0F; // ReLU (a NaN becomes 0)
            yrows[static_cast<crd::u64>(r) * n + o] = relu ? rectified : v;
        }
    }
#endif
}

// 4-row groups amortize the weight-block loads; grouping cannot perturb results (each row's accumulator and math are
// its own, bit-identical for any grouping, so task partitions stay moat-safe).
void nn_linear_i8_packed_rows(const NnLinearI8& c, crd::u32 lo, crd::u32 hi) noexcept
{
    crd::u32 m0 = lo;
    while (m0 < hi)
    {
        const crd::u32 rr = hi - m0 < 4U ? hi - m0 : 4U;
        crd::f32 scale_m[4];
        crd::i32 zp[4];
        for (crd::u32 r = 0; r < rr; ++r)
        {
            const crd::u32 m = m0 + r;
            quantize_u8_row(c.x + static_cast<crd::u64>(m) * c.k, c.k, c.xq + static_cast<crd::u64>(m) * c.k,
                            c.xscale[m], c.xzp[m]);
            scale_m[r] = c.xscale[m] * c.wscale;
            zp[r] = c.xzp[m];
        }
        const crd::u8* xrows = c.xq + static_cast<crd::u64>(m0) * c.k;
        crd::f32* yrows = c.y + static_cast<crd::u64>(m0) * c.n;
        switch (rr)
        {
            case 4U:
                i8_packed_rows<4U>(xrows, c.wq, c.colsum, c.k, c.n, scale_m, zp, c.bias, yrows, c.relu);
                break;
            case 3U:
                i8_packed_rows<3U>(xrows, c.wq, c.colsum, c.k, c.n, scale_m, zp, c.bias, yrows, c.relu);
                break;
            case 2U:
                i8_packed_rows<2U>(xrows, c.wq, c.colsum, c.k, c.n, scale_m, zp, c.bias, yrows, c.relu);
                break;
            case 1U:
                i8_packed_rows<1U>(xrows, c.wq, c.colsum, c.k, c.n, scale_m, zp, c.bias, yrows, c.relu);
                break;
            default:
                break;
        }
        m0 += rr;
    }
}

} // namespace

} // namespace crd::hesap::tensor::inline CRD_MATH_LEVEL_NS

namespace crd::hesap::tensor
{

// The table's entry groups, in the level's namespace like every function of the unit.
inline namespace CRD_MATH_LEVEL_NS
{
namespace
{

template <typename T, ReduceOp Op> constexpr ReduceKernels<T> tier_d() noexcept
{
    using K = CRD_MATH_LEVEL_NS::TierDKernel<T, Op>;
    return {&K::block, &K::rows, &K::columns};
}

template <typename T, BinaryOp Op> constexpr auto ew_binary() noexcept
{
    return &CRD_MATH_LEVEL_NS::EwBinaryKernel<T, Op>::walk;
}

template <typename T> constexpr BatchedKernels<T> batched() noexcept
{
    using K = CRD_MATH_LEVEL_NS::BatchedKernel<T>;
    return {&K::gemm_tiny, &K::cholesky, &K::lu, &K::svd, &K::cholesky_solve, &K::lu_solve};
}

template <typename T> constexpr SparseTensorKernels<T> sparse() noexcept
{
    using K = CRD_MATH_LEVEL_NS::SparseKernel<T>;
    return {&K::ttm_scatter, &K::ttm_staged, &K::fold, &K::mttkrp_csf, &K::mttkrp_coo};
}

} // namespace
} // namespace CRD_MATH_LEVEL_NS

const TensorKernels CRD_KERNEL_TABLE(kTensorKernels) = {
    &CRD_MATH_LEVEL_NS::permute_region_f32,
    &CRD_MATH_LEVEL_NS::permute_region_f64,
    &CRD_MATH_LEVEL_NS::permute_walk_f32,
    &CRD_MATH_LEVEL_NS::permute_walk_f64,
    &CRD_MATH_LEVEL_NS::binned_sum_f64,
    &CRD_MATH_LEVEL_NS::quantize_q8_0,
    &CRD_MATH_LEVEL_NS::quantize_u8_row,
    &CRD_MATH_LEVEL_NS::nn_softmax_rows,
    &CRD_MATH_LEVEL_NS::nn_linear_q8_rows,
    &CRD_MATH_LEVEL_NS::nn_linear_i8_rows,
    &CRD_MATH_LEVEL_NS::nn_linear_i8_packed_rows,
    &crd::hesap::stats::CRD_MATH_LEVEL_NS::philox4x32_fill_blocks,
    {ew_binary<crd::f32, BinaryOp::Add>(), ew_binary<crd::f32, BinaryOp::Sub>(), ew_binary<crd::f32, BinaryOp::Mul>(),
     ew_binary<crd::f32, BinaryOp::Div>(), ew_binary<crd::f32, BinaryOp::Min>(), ew_binary<crd::f32, BinaryOp::Max>()},
    {ew_binary<crd::f64, BinaryOp::Add>(), ew_binary<crd::f64, BinaryOp::Sub>(), ew_binary<crd::f64, BinaryOp::Mul>(),
     ew_binary<crd::f64, BinaryOp::Div>(), ew_binary<crd::f64, BinaryOp::Min>(), ew_binary<crd::f64, BinaryOp::Max>()},
    {tier_d<crd::f32, ReduceOp::Sum>(), tier_d<crd::f32, ReduceOp::Prod>(), tier_d<crd::f32, ReduceOp::Min>(),
     tier_d<crd::f32, ReduceOp::Max>()},
    {tier_d<crd::f64, ReduceOp::Sum>(), tier_d<crd::f64, ReduceOp::Prod>(), tier_d<crd::f64, ReduceOp::Min>(),
     tier_d<crd::f64, ReduceOp::Max>()},
    &CRD_MATH_LEVEL_NS::EinsumKernel<crd::f32>::thin,
    &CRD_MATH_LEVEL_NS::EinsumKernel<crd::f64>::thin,
    &CRD_MATH_LEVEL_NS::EinsumKernel<crd::f32>::smallm,
    &CRD_MATH_LEVEL_NS::EinsumKernel<crd::f64>::smallm,
    batched<crd::f32>(),
    batched<crd::f64>(),
    sparse<crd::f32>(),
    sparse<crd::f64>(),
    &CRD_MATH_LEVEL_NS::NnKernel::pool2d_planes,
    &CRD_MATH_LEVEL_NS::NnKernel::conv2d_images,
};

} // namespace crd::hesap::tensor
