// The hesap-tensor kernels of crd/hesap/tensor/tensor_kernels.hpp, compiled once per CPU level by crd_add_cpu_kernels
// (../CMakeLists.txt, ADR-0134 §5). Everything here lives in the level's namespace (CRD_MATH_LEVEL_NS), the tile loops
// and the binned operations included, so no function of this unit is shared with general code or with another level.
// The portable bodies are the scalar loops; the AVX2 bodies run the 8x8 in-register f32 transpose (with non-temporal
// stores for the multithreaded DRAM-sized permutes) and twelve binned accumulators. Every body gives the same bits: the
// permute is element copies and IEEE multiplies, and the binned representation is partition-independent.

#include <crd/hesap/tensor/binned_core.hpp>
#include <crd/hesap/tensor/permute_tiles.hpp>
#include <crd/hesap/tensor/tensor_kernels.hpp>
#include <crd/math/simd/backend.hpp>

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

} // namespace

} // namespace crd::hesap::tensor::inline CRD_MATH_LEVEL_NS

namespace crd::hesap::tensor
{

const TensorKernels CRD_KERNEL_TABLE(kTensorKernels) = {
    &CRD_MATH_LEVEL_NS::permute_region_f32,
    &CRD_MATH_LEVEL_NS::binned_sum_f64,
};

} // namespace crd::hesap::tensor
