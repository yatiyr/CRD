// The hesap-tensor kernels of crd/hesap/tensor/tensor_kernels.hpp, compiled once per CPU level by crd_add_cpu_kernels
// (../CMakeLists.txt, ADR-0134 §5). Everything here lives in the level's namespace (CRD_MATH_LEVEL_NS), the tile loops
// and the binned operations included, so no function of this unit is shared with general code or with another level.
// The portable bodies are the scalar loops; the AVX2 bodies run the 8x8 in-register f32 transpose (with non-temporal
// stores for the multithreaded DRAM-sized permutes), twelve binned accumulators, the nn quantizers eight lanes at a
// time, the quantized dots on maddubs/madd integer lanes and softmax's exp four lanes at a time. Every body gives the
// same bits: the permute is element copies and IEEE multiplies, the binned representation is partition-independent,
// the quantizers are per-element IEEE operations, the dots are exact integers combined in one f32 order, and crd_exp4
// reproduces crd_exp1.

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

#if CRD_SIMD_HAS_AVX2
// In-register 8x8 f32 transpose (the classic unpack/shuffle/permute2f128 microkernel). Source column j is 8
// contiguous floats at src + j*src_stride (the a dim is stride-1 in the source); destination row i is 8 contiguous
// floats at dst + i*dst_stride (the b dim is stride-1 in the destination). Both memory streams stay unit-stride.
template <bool Scale, bool Stream>
void transpose_8x8_f32(const crd::f32* src, crd::i64 src_stride, crd::f32* dst, crd::i64 dst_stride,
                       crd::f32 alpha) noexcept
{
    if constexpr (Stream)
    {
        // Prefetch the next micro-block's 8 source columns (the ib+8 call), only when columns are near (<= 64 KB
        // apart): it hides strided-read latency the streamer cannot track in the multithreaded regime (2D 46.8 ->
        // 51.2 GB/s); on page-scattered columns the 8 extra TLB walks cost more than they hide.
        if (src_stride * static_cast<crd::i64>(sizeof(crd::f32)) <= 65536)
        {
            for (int k = 8; k < 16; ++k)
            {
                _mm_prefetch(reinterpret_cast<const char*>(src + k * src_stride), _MM_HINT_T0);
            }
        }
    }
    __m256 r0 = _mm256_loadu_ps(src + 0 * src_stride);
    __m256 r1 = _mm256_loadu_ps(src + 1 * src_stride);
    __m256 r2 = _mm256_loadu_ps(src + 2 * src_stride);
    __m256 r3 = _mm256_loadu_ps(src + 3 * src_stride);
    __m256 r4 = _mm256_loadu_ps(src + 4 * src_stride);
    __m256 r5 = _mm256_loadu_ps(src + 5 * src_stride);
    __m256 r6 = _mm256_loadu_ps(src + 6 * src_stride);
    __m256 r7 = _mm256_loadu_ps(src + 7 * src_stride);

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

    r0 = _mm256_permute2f128_ps(s0, s4, 0x20);
    r1 = _mm256_permute2f128_ps(s1, s5, 0x20);
    r2 = _mm256_permute2f128_ps(s2, s6, 0x20);
    r3 = _mm256_permute2f128_ps(s3, s7, 0x20);
    r4 = _mm256_permute2f128_ps(s0, s4, 0x31);
    r5 = _mm256_permute2f128_ps(s1, s5, 0x31);
    r6 = _mm256_permute2f128_ps(s2, s6, 0x31);
    r7 = _mm256_permute2f128_ps(s3, s7, 0x31);

    if constexpr (Scale)
    {
        const __m256 va = _mm256_set1_ps(alpha);
        r0 = _mm256_mul_ps(r0, va);
        r1 = _mm256_mul_ps(r1, va);
        r2 = _mm256_mul_ps(r2, va);
        r3 = _mm256_mul_ps(r3, va);
        r4 = _mm256_mul_ps(r4, va);
        r5 = _mm256_mul_ps(r5, va);
        r6 = _mm256_mul_ps(r6, va);
        r7 = _mm256_mul_ps(r7, va);
    }

    if constexpr (Stream)
    {
        // Non-temporal stores: no read-for-ownership, no cache pollution (the DRAM-bound permute; the write-combining
        // buffers pair consecutive ib tiles into full lines).
        _mm256_stream_ps(dst + 0 * dst_stride, r0);
        _mm256_stream_ps(dst + 1 * dst_stride, r1);
        _mm256_stream_ps(dst + 2 * dst_stride, r2);
        _mm256_stream_ps(dst + 3 * dst_stride, r3);
        _mm256_stream_ps(dst + 4 * dst_stride, r4);
        _mm256_stream_ps(dst + 5 * dst_stride, r5);
        _mm256_stream_ps(dst + 6 * dst_stride, r6);
        _mm256_stream_ps(dst + 7 * dst_stride, r7);
    }
    else
    {
        _mm256_storeu_ps(dst + 0 * dst_stride, r0);
        _mm256_storeu_ps(dst + 1 * dst_stride, r1);
        _mm256_storeu_ps(dst + 2 * dst_stride, r2);
        _mm256_storeu_ps(dst + 3 * dst_stride, r3);
        _mm256_storeu_ps(dst + 4 * dst_stride, r4);
        _mm256_storeu_ps(dst + 5 * dst_stride, r5);
        _mm256_storeu_ps(dst + 6 * dst_stride, r6);
        _mm256_storeu_ps(dst + 7 * dst_stride, r7);
    }
}

// One tile: with a unit source stride the 8x8 microkernel covers the full 8x8 grid and the right and bottom strips
// finish scalar; any other source stride is the scalar tile.
template <bool Scale, bool Stream>
void permute_tile_f32(crd::f32* dst, crd::i64 ra, const crd::f32* src, crd::i64 sa, crd::i64 sb, crd::u64 na,
                      crd::u64 nb, crd::f32 alpha) noexcept
{
    if (sa != 1)
    {
        detail::permute_tile_scalar<Scale>(dst, ra, src, sa, sb, na, nb, alpha);
        return;
    }
    const crd::u64 na8 = na & ~crd::u64{7};
    const crd::u64 nb8 = nb & ~crd::u64{7};
    for (crd::u64 ia = 0; ia < na8; ia += 8U)
    {
        for (crd::u64 ib = 0; ib < nb8; ib += 8U)
        {
            transpose_8x8_f32<Scale, Stream>(src + static_cast<crd::i64>(ia) + static_cast<crd::i64>(ib) * sb, sb,
                                             dst + static_cast<crd::i64>(ia) * ra + static_cast<crd::i64>(ib), ra,
                                             alpha);
        }
    }
    if (nb8 < nb) // right strip: rows [0, na8), cols [nb8, nb)
    {
        detail::permute_tile_scalar<Scale>(dst + static_cast<crd::i64>(nb8), ra, src + static_cast<crd::i64>(nb8) * sb,
                                           sa, sb, na8, nb - nb8, alpha);
    }
    if (na8 < na) // bottom strip: rows [na8, na), all cols
    {
        detail::permute_tile_scalar<Scale>(dst + static_cast<crd::i64>(na8) * ra, ra,
                                           src + static_cast<crd::i64>(na8) * sa, sa, sb, na - na8, nb, alpha);
    }
}
#else
template <bool Scale, bool Stream>
void permute_tile_f32(crd::f32* dst, crd::i64 ra, const crd::f32* src, crd::i64 sa, crd::i64 sb, crd::u64 na,
                      crd::u64 nb, crd::f32 alpha) noexcept
{
    detail::permute_tile_scalar<Scale>(dst, ra, src, sa, sb, na, nb, alpha);
}
#endif

template <bool Scale, bool Stream> void permute_region(const PermuteRegionF32& r) noexcept
{
    detail::permute_region_tiles(r.na, r.nb, r.tile_a, r.tile_b,
                                 [&r](crd::u64 ta, crd::u64 tb, crd::u64 ha, crd::u64 hb)
                                 {
                                     permute_tile_f32<Scale, Stream>(
                                         r.dst + static_cast<crd::i64>(ta) * r.ra + static_cast<crd::i64>(tb), r.ra,
                                         r.src + static_cast<crd::i64>(ta) * r.sa + static_cast<crd::i64>(tb) * r.sb,
                                         r.sa, r.sb, ha, hb, r.alpha);
                                 });
#if CRD_SIMD_HAS_AVX2
    if constexpr (Stream)
    {
        _mm_sfence(); // non-temporal stores are weakly ordered: publish them before returning
    }
#endif
}

void permute_region_f32(const PermuteRegionF32& r) noexcept
{
#if CRD_SIMD_HAS_AVX2
    if (r.stream)
    {
        if (r.scale)
        {
            permute_region<true, true>(r);
        }
        else
        {
            permute_region<false, true>(r);
        }
        return;
    }
#endif
    if (r.scale)
    {
        permute_region<true, false>(r);
    }
    else
    {
        permute_region<false, false>(r);
    }
}

#if CRD_SIMD_HAS_AVX2
// TWELVE independent binned accumulators: 3 streams (breaking the rung-ladder latency chain) x 4 lanes, merged at the
// end. Legal because the binned representation is partition-independent: the merged bits equal the scalar
// whole-array sum. The per-element ladder is the scalar deposit (sticky-or, two-sum) per lane.
using detail::kBinEndurance;
using detail::kBinFold;

constexpr crd::u64 kStreams = 3U;
constexpr crd::u64 kLanes = 4U;
constexpr crd::u64 kStep = kStreams * kLanes;

// The larger of the four lanes of v.
crd::f64 lane_max(__m256d v) noexcept
{
    alignas(32) crd::f64 mx[kLanes];
    _mm256_store_pd(mx, v);
    crd::f64 amax = mx[0];
    for (crd::u64 l = 1; l < kLanes; ++l)
    {
        amax = mx[l] > amax ? mx[l] : amax;
    }
    return amax;
}

// One super-block of deposits over the 12 accumulators. TrackMax fuses the |max| scan into the same pass (the
// speculative single-DRAM-pass mode) and returns the block's abs-max (0 when not tracking).
template <bool TrackMax>
crd::f64 deposit_superblock(BinnedRungs (&lanes)[kStep], const crd::f64* p, crd::u64 len) noexcept
{
    const __m256d sticky = _mm256_castsi256_pd(_mm256_set1_epi64x(1LL));
    const __m256d absmask = _mm256_castsi256_pd(_mm256_set1_epi64x(0x7FFFFFFFFFFFFFFFLL));
    __m256d pri[kStreams][kBinFold];
    for (crd::u64 s = 0; s < kStreams; ++s)
    {
        for (crd::i32 k = 0; k < kBinFold; ++k)
        {
            pri[s][k] = _mm256_set_pd(lanes[s * kLanes + 3U].pri[k], lanes[s * kLanes + 2U].pri[k],
                                      lanes[s * kLanes + 1U].pri[k], lanes[s * kLanes + 0U].pri[k]);
        }
    }
    __m256d vmax = _mm256_setzero_pd();
    for (crd::u64 k = 0; k < len; k += kStep)
    {
        for (crd::u64 s = 0; s < kStreams; ++s)
        {
            __m256d x = _mm256_loadu_pd(p + k + s * kLanes);
            if constexpr (TrackMax)
            {
                vmax = _mm256_max_pd(vmax, _mm256_and_pd(x, absmask));
            }
            for (crd::i32 r = 0; r < kBinFold - 1; ++r)
            {
                const __m256d m = pri[s][r];
                const __m256d snew = _mm256_add_pd(_mm256_or_pd(x, sticky), m);
                pri[s][r] = snew;
                x = _mm256_add_pd(x, _mm256_sub_pd(m, snew));
            }
            pri[s][kBinFold - 1] = _mm256_add_pd(pri[s][kBinFold - 1], _mm256_or_pd(x, sticky));
        }
    }
    for (crd::u64 s = 0; s < kStreams; ++s)
    {
        for (crd::i32 k = 0; k < kBinFold; ++k)
        {
            alignas(32) crd::f64 lv[kLanes];
            _mm256_store_pd(lv, pri[s][k]);
            for (crd::u64 l = 0; l < kLanes; ++l)
            {
                lanes[s * kLanes + l].pri[k] = lv[l];
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

void binned_sum_run_avx2(BinnedRungs& out, const crd::f64* p, crd::u64 n) noexcept
{
    BinnedRungs lanes[kStep] = {};
    crd::i32 cur_index = -1; // no window yet: the first block seeds it two-pass
    crd::u64 i = 0;
    while (i + kStep <= n)
    {
        const crd::u64 len0 = ((n - i) / kStep) * kStep;
        const crd::u64 len = len0 < kBinEndurance * kStep ? len0 : kBinEndurance * kStep;

        if (cur_index < 0)
        {
            // Seed pass: scan |max| first (reads once from DRAM, the deposit hits L2).
            __m256d vmax = _mm256_setzero_pd();
            const __m256d absmask = _mm256_castsi256_pd(_mm256_set1_epi64x(0x7FFFFFFFFFFFFFFFLL));
            for (crd::u64 k = 0; k < len; k += kLanes)
            {
                vmax = _mm256_max_pd(vmax, _mm256_and_pd(_mm256_loadu_pd(p + i + k), absmask));
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
            BinnedRungs snap[kStep];
            for (crd::u64 l = 0; l < kStep; ++l)
            {
                snap[l] = lanes[l];
            }
            const crd::f64 amax = deposit_superblock<true>(lanes, p + i, len);
            const crd::i32 amax_index = detail::bin_index_of(amax);
            if (amax_index == 0 || amax_index < cur_index)
            {
                for (crd::u64 l = 0; l < kStep; ++l)
                {
                    lanes[l] = snap[l];
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
#if CRD_SIMD_HAS_AVX2
    if (n >= 8U * kStep)
    {
        binned_sum_run_avx2(acc, p, n);
        return;
    }
#endif
    detail::binned_sum_run_scalar(acc, p, n);
}

// ---- nn: softmax (last dim, stable) -----------------------------------------
// exp rides the deterministic crd_exp1 core; the AVX2 4-lane widen path uses crd_exp4, which is bit-identical to
// crd_exp1 on every input, so the vector/tail split cannot perturb results.
void softmax_row(const crd::f32* x, crd::f32* y, crd::u64 d) noexcept
{
    crd::f32 m = x[0];
    for (crd::u64 j = 1; j < d; ++j)
    {
        m = x[j] > m ? x[j] : m;
    }
    crd::u64 j = 0;
#if CRD_SIMD_HAS_AVX2
    for (; j + 4U <= d; j += 4U)
    {
        const __m128 xf = _mm_sub_ps(_mm_loadu_ps(x + j), _mm_set1_ps(m));
        const __m128 ef = _mm256_cvtpd_ps(crd::math::crd_exp4(_mm256_cvtps_pd(xf)));
        _mm_storeu_ps(y + j, ef);
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

// ---- nn: the Q8_0 linear ----------------------------------------------------
// The lane-parallel Q8_0 kernel (pinned semantics, every body identical): each block's int8 x int8 products land in
// eight exact i32 lane partials (lane k = products 4k..4k+3, the maddubs/madd pair structure; |lane| <= 64516, so the
// f32 convert is exact too). The per-block f32 combine is one fma per lane (acc_k = fma(dx*dw, f32(lane_k), acc_k)),
// and the per-output horizontal sum is the fixed hadd tree ((l0+l1)+(l2+l3)) + ((l4+l5)+(l6+l7)). Everything is a
// fixed operation sequence, so the result is bit-exact across worker counts, runs and levels, and the integer stage
// across all hardware (the certification tier). The portable body mirrors the lane structure and the tree exactly.
constexpr crd::u32 kNnQ8OutTile = 8U; // outputs sharing one activation-block load (8 acc chains hide the fma latency)

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
#if !defined(__AVXVNNI__)
    const __m256i ones16 = _mm256_set1_epi16(1);
#endif
    for (crd::u64 bi = 0; bi < nb; ++bi)
    {
        const __m256i vx = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(xrow[bi].qs));
        const __m256i ax = _mm256_sign_epi8(vx, vx); // |x| (u8 <= 127), shared by the tile
        const __m256 vdx = _mm256_set1_ps(xsc[bi]);
        for (crd::u32 r = 0; r < t; ++r)
        {
            const BlockQ8Zero* wrow = wq + (o0 + r) * nb;
            const __m256i vw = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(wrow[bi].qs));
            const __m256i sw = _mm256_sign_epi8(vw, vx); // w * sign(x)
#if defined(__AVXVNNI__)
            // dpbusd accumulates the same 4-product i32 lanes exactly (no intermediate saturation is reachable at
            // |q| <= 127): the integer lanes are bit-identical to the maddubs/madd pair
            const __m256i p32 = _mm256_dpbusd_avx_epi32(_mm256_setzero_si256(), ax, sw);
#else
            const __m256i p16 = _mm256_maddubs_epi16(ax, sw);   // pairs -> i16 (no saturation reachable)
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
// trunc(t + copysign(0.5, t)) (-0 included), and a NaN lane quantizes as 0.
void quantize_q8_0(const crd::f32* x, BlockQ8Zero* out, crd::u64 nblocks) noexcept
{
#if CRD_SIMD_HAS_AVX2
    const __m256 abs_mask = _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF));
    const __m256 sign_mask = _mm256_castsi256_ps(_mm256_set1_epi32(static_cast<crd::i32>(0x80000000U)));
    const __m256 half = _mm256_set1_ps(0.5F);
    const __m256i nan_q = _mm256_set1_epi8(-128); // a NaN lane converts to INT32_MIN, which packs to -128
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
        // |t| <= 127 rounds into [-127, 127], so -128 is exactly the NaN lanes: they quantize as 0
        const __m256i q = _mm256_andnot_si256(_mm256_cmpeq_epi8(packed, nan_q), packed);
        const __m256i fixed = _mm256_permutevar8x32_epi32(q, _mm256_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(out[i].qs), fixed);
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
// trunc(t + copysign(0.5, t)) form lane for lane with a NaN lane at the zero point, and packus saturation == the
// scalar [0, 255] clamp. k must be a multiple of 32.
void quantize_u8_row(const crd::f32* x, crd::u64 k, crd::u8* out, crd::f32& scale, crd::i32& zero_point) noexcept
{
#if CRD_SIMD_HAS_AVX2
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
    const __m256 half = _mm256_set1_ps(0.5F);
    const auto away_zp = [&](__m256 t)
    {
        const __m256 ordered = _mm256_and_ps(t, _mm256_cmp_ps(t, t, _CMP_ORD_Q)); // a NaN lane becomes +0
        const __m256i q =
            _mm256_cvttps_epi32(_mm256_add_ps(ordered, _mm256_or_ps(_mm256_and_ps(ordered, sign_mask), half)));
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
#if !defined(__AVXVNNI__)
    const __m256i ones16 = _mm256_set1_epi16(1);
    const __m256i low7 = _mm256_set1_epi8(0x7F);
    const __m256i one8 = _mm256_set1_epi8(1);
#endif
    for (crd::u64 c = 0; c < k; c += 32U)
    {
        const __m256i vx = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(xrow + c)); // u8
#if !defined(__AVXVNNI__)
        // u8 x i8 without VNNI: split x = xl + 128*xh (xl <= 127, xh in {0,1}) so the maddubs i16 pair sums can
        // never saturate; exact i32 either way
        const __m256i xl = _mm256_and_si256(vx, low7);
        const __m256i xh = _mm256_and_si256(_mm256_srli_epi16(vx, 7), one8);
#endif
        for (crd::u32 r = 0; r < t; ++r)
        {
            const __m256i vw = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(wq + (o0 + r) * k + c));
#if defined(__AVXVNNI__)
            acc[r] = _mm256_dpbusd_avx_epi32(acc[r], vx, vw); // u8 x i8, exact i32 accumulate
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
#if !defined(__AVXVNNI__)
    const __m256i ones16 = _mm256_set1_epi16(1);
    const __m256i low7 = _mm256_set1_epi8(0x7F);
    const __m256i one8 = _mm256_set1_epi8(1);
#endif
    const __m256 zero = _mm256_setzero_ps();
    for (crd::u64 b = 0; b < n8; b += 8U)
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
#if defined(__AVXVNNI__)
                acc[r] = _mm256_dpbusd_avx_epi32(acc[r], va, vb); // u8 x i8, exact i32
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

const TensorKernels CRD_KERNEL_TABLE(kTensorKernels) = {
    &CRD_MATH_LEVEL_NS::permute_region_f32, &CRD_MATH_LEVEL_NS::binned_sum_f64,
    &CRD_MATH_LEVEL_NS::quantize_q8_0,      &CRD_MATH_LEVEL_NS::quantize_u8_row,
    &CRD_MATH_LEVEL_NS::nn_softmax_rows,    &CRD_MATH_LEVEL_NS::nn_linear_q8_rows,
    &CRD_MATH_LEVEL_NS::nn_linear_i8_rows,  &CRD_MATH_LEVEL_NS::nn_linear_i8_packed_rows,
};

} // namespace crd::hesap::tensor
