#pragma once

#include <crd/hesap/tensor/tensor.hpp>
#include <crd/jobs/jobs.hpp>

#include <cstring>
#include <type_traits>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

// ---------------------------------------------------------------------------
// crd-hesap-tensor — HPTT-class tensor transpose/permute (Phase 3.1.6 v14-d,
// increment 1: serial + SIMD-blocked; the deterministic multithreaded pass
// lands in increment 2. ADR-0096 §5).
//
// permute_copy materializes src.permute(order) into contiguous canonical
// row-major dst storage, with an optional fused scale (HPTT's alpha). The
// kernel pipeline is the standard TTC/HPTT structure:
//
//   1. DIMENSION COLLAPSE — express the copy in DESTINATION order (dst dim d
//      reads src dim order[d]), then merge adjacent dst dims that are
//      contiguous in BOTH streams. The dst side is canonical row-major, so
//      adjacency there is free; the merge condition is purely on the source:
//      stride[d] == stride[d+1] * shape[d+1]. Size-1 dims are dropped. This
//      turns e.g. a rank-5 permutation with a contiguous tail into the 2-D
//      transpose it really is, and an identity permutation into one memcpy.
//   2. KERNEL DISPATCH on the collapsed form:
//      - rank 0/1 -> scalar / single strided row (memcpy when stride 1).
//      - src-fastest dim == dst-fastest dim -> row loop: every dst row is one
//        contiguous strided-read copy (memcpy rows when the source row is
//        contiguous — the "no transpose at the inner level" case).
//      - otherwise the true transpose case: dst-fastest dim b reads a
//        non-fastest src dim, so a naive loop must stride one of the two
//        streams by a full row. We square-TILE the (a, b) plane — a = the
//        src-fastest dim, b = the dst-fastest dim — in kPermuteTile edges so
//        both the read and the write stream stay inside L1 for the duration
//        of a tile; all remaining dims are an outer odometer around the tiled
//        2-D kernel. Inside a tile, f32 with a unit source stride runs the
//        AVX2 8x8 in-register transpose microkernel (unpack/shuffle/
//        permute2f128); everything else runs the scalar tile loop with the
//        write stream contiguous.
//      - tiny inner extents fall back to the simple strided row loop (tile
//        machinery is pure overhead below kPermuteTinyDim).
//
// Determinism: serial, fixed traversal order, pure element copies (plus one
// IEEE multiply per element when alpha != 1) -> bit-identical by
// construction; alpha == 1 is a pure bit copy (no arithmetic touches the
// payload, NaNs included).
//
// Contract (the v13 pillars, ADR-0095 §2): status-not-exception, noexcept
// kernels, zero heap besides dst.resize(). src must NOT alias dst — resize
// reallocates dst's storage before the copy runs.
// ---------------------------------------------------------------------------

namespace crd::hesap::tensor
{

namespace detail
{

// Tile edge (elements) for the blocked 2-D transpose. A 32x32 tile touches
// 32*32*sizeof(T) bytes per stream (4 KB f32 / 8 KB f64, 2 streams) — well
// inside L1/L2 with room left for the outer odometer's streams — and is a
// multiple of the 8x8 AVX2 microkernel grid. (64: measured better than 32 on
// the HPTT board cases — fewer boundary crossings + TLB churn; 16 KB/stream.)
inline constexpr crd::u64 kPermuteTile = 64U;

// Below this extent on either transpose dim the tile machinery is pure
// overhead; use the simple strided row loop instead.
inline constexpr crd::u64 kPermuteTinyDim = 8U;

// Streaming-store gate. MEASURED 2026-07-02 (WSL2/Raptor Lake, 1T): NT stores
// REGRESS the single-thread big-transpose cases (4096^2 0.598->0.718 ns/elem;
// 512^3 0.795->0.897) — one core cannot fill the NT write-combining path
// faster than the regular-store + LLC write-back pipeline here. The machinery
// stays (Stream template) for the MULTITHREADED increment, where aggregate NT
// traffic is the classic win — re-measure there. Disabled by default:
inline constexpr crd::u64 kPermuteStreamBytes = ~crd::u64{0};

// One contiguous dst row of `count` elements from a strided source (stride
// may be negative = flipped, or 0 = broadcast). alpha == 1 (Scale=false) is
// a bit copy — memcpy when the source row is contiguous.
template <bool Scale, typename T>
inline void permute_row(T* dst, const T* src, crd::u64 count, crd::i64 s_in, T alpha) noexcept
{
    if constexpr (!Scale)
    {
        if (s_in == 1)
        {
            std::memcpy(dst, src, count * sizeof(T));
            return;
        }
    }
    crd::i64 off = 0;
    for (crd::u64 i = 0; i < count; ++i, off += s_in)
    {
        if constexpr (Scale)
        {
            dst[i] = alpha * src[off];
        }
        else
        {
            dst[i] = src[off];
        }
    }
}

#if defined(__AVX2__)
// In-register 8x8 f32 transpose (the classic unpack/shuffle/permute2f128
// microkernel). Preconditions: source column j is 8 contiguous floats at
// src + j*src_stride (the a dim is stride-1 in the source); destination row
// i is 8 contiguous floats at dst + i*dst_stride (the b dim is stride-1 in
// the destination). Loads 8 source columns, transposes in registers, stores
// 8 destination rows — both memory streams stay unit-stride.
template <bool Scale, bool Stream = false>
inline void transpose_8x8_f32(const crd::f32* src, crd::i64 src_stride, crd::f32* dst, crd::i64 dst_stride,
                              crd::f32 alpha) noexcept
{
    if constexpr (Stream)
    {
        // Prefetch the next micro-block's 8 source columns (the ib+8 call).
        // ONLY when columns are near (<= 64 KB apart): hides strided-read
        // latency the streamer cannot track in the MT regime (measured: 2D
        // 46.8 -> 51.2 GB/s); on page-scattered columns the 8 extra TLB walks
        // cost more than they hide (4D/3D regressed — gated off there).
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
        // Non-temporal stores: no read-for-ownership, no cache pollution — the
        // DRAM-bound big-permute path (32B-aligned by the dispatch gate; the
        // write-combining buffers pair consecutive ib tiles into full lines).
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
#endif // __AVX2__

// Scalar (a, b)-plane tile: dst(ia, ib) at ia*ra + ib (b is dst-contiguous),
// src(ia, ib) at ia*sa + ib*sb. Inner loop over ib keeps the WRITE stream
// contiguous; the read stream jumps sb per step but stays inside the tile's
// L1 footprint.
template <bool Scale, typename T>
inline void permute_tile_scalar(T* dst, crd::i64 ra, const T* src, crd::i64 sa, crd::i64 sb, crd::u64 na, crd::u64 nb,
                                T alpha) noexcept
{
    for (crd::u64 ia = 0; ia < na; ++ia)
    {
        T* drow = dst + static_cast<crd::i64>(ia) * ra;
        const T* scol = src + static_cast<crd::i64>(ia) * sa;
        crd::i64 off = 0;
        for (crd::u64 ib = 0; ib < nb; ++ib, off += sb)
        {
            if constexpr (Scale)
            {
                drow[ib] = alpha * scol[off];
            }
            else
            {
                drow[ib] = scol[off];
            }
        }
    }
}

// One (a, b) tile, extents (na, nb) <= kPermuteTile. f32 with a unit source
// stride runs the 8x8 AVX2 microkernel over the full 8x8 grid, then finishes
// the right/bottom edge strips scalar; every other case is the scalar tile.
template <bool Scale, bool Stream, typename T>
inline void permute_tile(T* dst, crd::i64 ra, const T* src, crd::i64 sa, crd::i64 sb, crd::u64 na, crd::u64 nb,
                         T alpha) noexcept
{
#if defined(__AVX2__)
    if constexpr (std::is_same_v<T, crd::f32>)
    {
        if (sa == 1)
        {
            const crd::u64 na8 = na & ~crd::u64{7};
            const crd::u64 nb8 = nb & ~crd::u64{7};
            for (crd::u64 ia = 0; ia < na8; ia += 8U)
            {
                for (crd::u64 ib = 0; ib < nb8; ib += 8U)
                {
                    transpose_8x8_f32<Scale, Stream>(
                        src + static_cast<crd::i64>(ia) + static_cast<crd::i64>(ib) * sb, sb,
                        dst + static_cast<crd::i64>(ia) * ra + static_cast<crd::i64>(ib), ra, alpha);
                }
            }
            if (nb8 < nb) // right strip: rows [0, na8), cols [nb8, nb)
            {
                permute_tile_scalar<Scale>(dst + static_cast<crd::i64>(nb8), ra, src + static_cast<crd::i64>(nb8) * sb,
                                           sa, sb, na8, nb - nb8, alpha);
            }
            if (na8 < na) // bottom strip: rows [na8, na), all cols
            {
                permute_tile_scalar<Scale>(dst + static_cast<crd::i64>(na8) * ra, ra,
                                           src + static_cast<crd::i64>(na8) * sa, sa, sb, na - na8, nb, alpha);
            }
            return;
        }
    }
#endif
    permute_tile_scalar<Scale>(dst, ra, src, sa, sb, na, nb, alpha);
}

// Row-loop kernel: odometer over dims [0, m-1); every step emits one
// contiguous dst row read from the (possibly strided) source. Covers both
// the "src-fastest == dst-fastest" case and the tiny-inner-dims fallback.
template <bool Scale, typename T>
inline void permute_rows(T* dst, const T* src, const crd::u64* shp, const crd::i64* s_in, crd::u32 m, T alpha) noexcept
{
    const crd::u64 nb = shp[m - 1U];
    crd::u64 rows = 1;
    for (crd::u32 d = 0; d + 1U < m; ++d)
    {
        rows *= shp[d];
    }
    crd::u64 idx[kMaxRank] = {};
    crd::i64 soff = 0;
    for (crd::u64 row = 0; row < rows; ++row)
    {
        permute_row<Scale>(dst, src + soff, nb, s_in[m - 1U], alpha);
        dst += nb; // dst is canonical row-major: rows are consecutive
        for (crd::u32 d = m - 1U; d-- > 0U;)
        {
            soff += s_in[d];
            if (++idx[d] < shp[d])
            {
                break;
            }
            soff -= s_in[d] * static_cast<crd::i64>(shp[d]);
            idx[d] = 0;
        }
    }
}

// Tiled-transpose kernel: outer odometer over every dim except a (the
// src-fastest dim) and b = m-1 (the dst-fastest dim); each step runs the
// square-tiled 2-D transpose over the (a, b) plane.
template <bool Scale, bool Stream, typename T>
inline void permute_tiled(T* dst, const T* src, const crd::u64* shp, const crd::i64* s_in, const crd::i64* s_out,
                          crd::u32 m, crd::u32 a, T alpha) noexcept
{
    const crd::u32 b = m - 1U;
    const crd::u64 na = shp[a];
    const crd::u64 nb = shp[b];
    const crd::i64 sa_in = s_in[a];
    const crd::i64 sb_in = s_in[b];
    const crd::i64 ra = s_out[a]; // dst stride of a (dst stride of b is 1)
    // Stride-aware tile edge (measured on the HPTT board): 64 keeps both
    // streams hot when the scattered-stream step is small (fewer boundary
    // crossings); when each tile column lands on a fresh page region
    // (|sb|*sizeof(T) > 64 KB), 64 columns thrash the dTLB — drop to 32.
    const crd::i64 sb_mag = sb_in < 0 ? -sb_in : sb_in;
    // …unless the whole plane is a single 64-tile (nb <= 64): then there is no
    // boundary overhead to pay, and the src-locality-ordered odometer reuses
    // the same column pages across consecutive planes (measured: 64^4 reversal
    // 0.44 ns/elem at 64 vs 0.51 at 32).
    const crd::u64 tile =
        (static_cast<crd::u64>(sb_mag) * sizeof(T) <= 65536U || nb <= kPermuteTile) ? kPermuteTile : 32U;

    // Outer odometer over every dim except (a, b), iterated INNERMOST-first by
    // ascending |src stride|: consecutive plane visits then move the scattered
    // source stream by its smallest step (page/TLB locality on the stream that
    // has none inside the tile). The dst side is contiguous per row regardless.
    crd::u32 ord[kMaxRank];
    crd::u32 nout = 0;
    for (crd::u32 d = 0; d < m; ++d)
    {
        if (d != a && d != b)
        {
            ord[nout++] = d;
        }
    }
    for (crd::u32 i = 1; i < nout; ++i) // insertion sort, ascending |s_in| = innermost first
    {
        const crd::u32 v = ord[i];
        const crd::i64 mv = s_in[v] < 0 ? -s_in[v] : s_in[v];
        crd::u32 j = i;
        while (j > 0U)
        {
            const crd::i64 mj = s_in[ord[j - 1U]] < 0 ? -s_in[ord[j - 1U]] : s_in[ord[j - 1U]];
            if (mj <= mv)
            {
                break;
            }
            ord[j] = ord[j - 1U];
            --j;
        }
        ord[j] = v;
    }
    crd::u64 outer = 1;
    for (crd::u32 i = 0; i < nout; ++i)
    {
        outer *= shp[ord[i]];
    }
    crd::u64 idx[kMaxRank] = {};
    crd::i64 soff = 0;
    crd::i64 doff = 0;
    for (crd::u64 it = 0; it < outer; ++it)
    {
        for (crd::u64 ta = 0; ta < na; ta += tile)
        {
            const crd::u64 ha = na - ta < tile ? na - ta : tile;
            for (crd::u64 tb = 0; tb < nb; tb += tile)
            {
                const crd::u64 hb = nb - tb < tile ? nb - tb : tile;
                permute_tile<Scale, Stream>(dst + doff + static_cast<crd::i64>(ta) * ra + static_cast<crd::i64>(tb), ra,
                                            src + soff + static_cast<crd::i64>(ta) * sa_in +
                                                static_cast<crd::i64>(tb) * sb_in,
                                            sa_in, sb_in, ha, hb, alpha);
            }
        }
        for (crd::u32 i = 0; i < nout; ++i) // ord[0] = the innermost (smallest src step)
        {
            const crd::u32 d = ord[i];
            soff += s_in[d];
            doff += s_out[d];
            if (++idx[d] < shp[d])
            {
                break;
            }
            soff -= s_in[d] * static_cast<crd::i64>(shp[d]);
            doff -= s_out[d] * static_cast<crd::i64>(shp[d]);
            idx[d] = 0;
        }
    }
}

// MULTITHREADED tiled transpose (v14-d increment 2). The macro-task space is
// (outer odometer position × ta tile row) — every task writes a DISJOINT dst
// region (the permutation is a bijection), so the result bytes are identical
// for ANY worker count / any scheduling BY CONSTRUCTION (gated anyway).
// Tasks decompose their outer index into odometer digits along the same
// src-locality order the serial path walks. Streaming (NT) stores are the
// aggregate-bandwidth lever in this regime — each task fences before it
// completes (the jobs counter's release ordering does not cover NT stores).
#if defined(__AVX2__)
// Staged scattered-column tile (f32, MT+Stream): reads run ib-OUTER — 8
// sequential column streams instead of 64 interleaved ones (DRAM page
// locality) — transposing through an L1-resident stage, which is then
// streamed to dst linearly (full-line NT). Resolves the loop-order tension
// measured on the 512^3 8T row. Tile dims must be multiples of 8.
template <bool Scale>
inline void permute_tile_staged_f32(crd::f32* dst, crd::i64 ra, const crd::f32* src, crd::i64 sb, crd::u64 na,
                                    crd::u64 nb, crd::f32 alpha) noexcept
{
    alignas(64) crd::f32 stage[512U * 32U];  // 64 KB exactly; worker threads carry MB stacks
    for (crd::u64 ib = 0; ib < nb; ib += 8U) // column-group OUTER: 8 read streams
    {
        for (crd::u64 ia = 0; ia < na; ia += 8U)
        {
            transpose_8x8_f32<Scale, false>(src + static_cast<crd::i64>(ia) + static_cast<crd::i64>(ib) * sb, sb,
                                            stage + ia * nb + ib, static_cast<crd::i64>(nb), alpha);
        }
    }
    for (crd::u64 ia = 0; ia < na; ++ia)
    {
        const crd::f32* srow = stage + ia * nb;
        crd::f32* drow = dst + static_cast<crd::i64>(ia) * ra;
        for (crd::u64 j = 0; j < nb; j += 8U)
        {
            _mm256_stream_ps(drow + j, _mm256_load_ps(srow + j));
        }
    }
}
#endif // __AVX2__

inline constexpr crd::u64 kPermuteMtBytes = 4U * 1024U * 1024U;
inline constexpr crd::u64 kPermuteMtStreamBytes = 8U * 1024U * 1024U;

template <bool Scale, bool Stream, typename T>
inline void permute_tiled_mt(T* dst, const T* src, const crd::u64* shp, const crd::i64* s_in, const crd::i64* s_out,
                             crd::u32 m, crd::u32 a, T alpha) noexcept
{
    const crd::u32 b = m - 1U;
    // MT tile rule (measured; INVERTED from the 1T rule): near columns -> 32
    // (the per-tile read-stream count stays under the L2 streamer's tracker
    // with 8 cores in flight); page-scattered columns -> 64 (the prefetcher
    // cannot follow anyway; fewer tile boundaries wins).
    // RECTANGULAR MT tiles: a (the src stride-1 dim) is where sequential DRAM
    // bursts live — on page-scattered columns a TALL tile (256×64) reads 1 KB
    // per column visit instead of 256 B and quarters the cold-miss rounds
    // (the 3D 512³ lever). Near columns keep the square 32 rule.
    const crd::i64 sb_mag = s_in[b] < 0 ? -s_in[b] : s_in[b];
    const bool near_cols = static_cast<crd::u64>(sb_mag) * sizeof(T) <= 65536U;
    const crd::u64 tile_a = near_cols ? 32U : (shp[a] < 512U ? shp[a] : 512U);
    const crd::u64 tile_b = near_cols ? 32U : 32U;

    // src-locality outer order (identical to the serial path's).
    crd::u32 ord[kMaxRank];
    crd::u32 nout = 0;
    for (crd::u32 d = 0; d < m; ++d)
    {
        if (d != a && d != b)
        {
            ord[nout++] = d;
        }
    }
    for (crd::u32 i = 1; i < nout; ++i)
    {
        const crd::u32 v = ord[i];
        const crd::i64 mv = s_in[v] < 0 ? -s_in[v] : s_in[v];
        crd::u32 j = i;
        while (j > 0U)
        {
            const crd::i64 mj = s_in[ord[j - 1U]] < 0 ? -s_in[ord[j - 1U]] : s_in[ord[j - 1U]];
            if (mj <= mv)
            {
                break;
            }
            ord[j] = ord[j - 1U];
            --j;
        }
        ord[j] = v;
    }
    crd::u64 outer = 1;
    for (crd::u32 i = 0; i < nout; ++i)
    {
        outer *= shp[ord[i]];
    }

    struct Params
    {
        T* dst;
        const T* src;
        const crd::u64* shp;
        const crd::i64* s_in;
        const crd::i64* s_out;
        const crd::u32* ord;
        crd::u32 nout;
        crd::u32 a;
        crd::u32 b;
        crd::u64 tile_a;
        crd::u64 tile_b;
        crd::u64 sup_a;
        crd::u64 sup_b;
        crd::u64 n_sup; // supers per (a,b) plane
        T alpha;
    };
    // Super-block task shape: 8x8 tiles (512^2 at tile 64). Partitioning BOTH
    // transpose dims keeps each task's scattered-stream reads inside a small
    // address span (DRAM row-buffer locality with 8 workers in flight) —
    // measured lever: the 2D 4096^2 8T case 39.8 -> ~55 GB/s.
    // (A fused-outer plane-group variant — sequential column reads across
    // consecutive planes — was built and MEASURED NO-GAIN 2026-07-02: the
    // microkernel's NT-write-friendly ia-outer order re-fragments the reads
    // inside each tile; inverting it fragments the NT writes instead. The
    // in-tile loop-order tension is the named lever for the next 3D pass.)
    const crd::u64 sup_a = tile_a * (near_cols ? 16U : 2U); // 512 elements either way
    const crd::u64 sup_b = tile_b * 8U;
    const crd::u64 n_ta = (shp[a] + sup_a - 1U) / sup_a;
    const crd::u64 n_tb = (shp[b] + sup_b - 1U) / sup_b;
    const Params prm{dst, src, shp, s_in, s_out, ord, nout, a, b, tile_a, tile_b, sup_a, sup_b, n_ta * n_tb, alpha};
    const crd::u64 tasks = outer * n_ta * n_tb;

    crd::jobs::Counter* c = crd::jobs::parallel_for(
        static_cast<crd::u32>(tasks), crd::jobs::num_workers(),
        [&prm](crd::u32 t0, crd::u32 t1)
        {
            const crd::u64 na = prm.shp[prm.a];
            const crd::u64 nb = prm.shp[prm.b];
            const crd::i64 sa_in = prm.s_in[prm.a];
            const crd::i64 sb_in = prm.s_in[prm.b];
            const crd::i64 ra = prm.s_out[prm.a];
            const crd::u64 n_tb_sup = (nb + prm.sup_b - 1U) / prm.sup_b;
            for (crd::u32 t = t0; t < t1; ++t)
            {
                crd::u64 outer_it = t / prm.n_sup;
                const crd::u64 sup = t % prm.n_sup;
                const crd::u64 ta0 = (sup / n_tb_sup) * prm.sup_a;
                const crd::u64 tb0 = (sup % n_tb_sup) * prm.sup_b;
                crd::i64 soff = 0;
                crd::i64 doff = 0;
                for (crd::u32 i = 0; i < prm.nout; ++i) // ord[0] = fastest digit (serial-order match)
                {
                    const crd::u32 d = prm.ord[i];
                    const crd::u64 digit = outer_it % prm.shp[d];
                    outer_it /= prm.shp[d];
                    soff += static_cast<crd::i64>(digit) * prm.s_in[d];
                    doff += static_cast<crd::i64>(digit) * prm.s_out[d];
                }
                const crd::u64 ta1 = ta0 + prm.sup_a < na ? ta0 + prm.sup_a : na;
                const crd::u64 tb1 = tb0 + prm.sup_b < nb ? tb0 + prm.sup_b : nb;
                for (crd::u64 ta = ta0; ta < ta1; ta += prm.tile_a)
                {
                    const crd::u64 ha = ta1 - ta < prm.tile_a ? ta1 - ta : prm.tile_a;
                    for (crd::u64 tb = tb0; tb < tb1; tb += prm.tile_b)
                    {
                        const crd::u64 hb = tb1 - tb < prm.tile_b ? tb1 - tb : prm.tile_b;
                        permute_tile<Scale, Stream>(
                            prm.dst + doff + static_cast<crd::i64>(ta) * ra + static_cast<crd::i64>(tb), ra,
                            prm.src + soff + static_cast<crd::i64>(ta) * sa_in + static_cast<crd::i64>(tb) * sb_in,
                            sa_in, sb_in, ha, hb, prm.alpha);
                    }
                }
            }
#if defined(__AVX2__)
            if constexpr (Stream)
            {
                _mm_sfence(); // publish NT stores before this task completes
            }
#endif
        });
    crd::jobs::wait(c);
    crd::jobs::frame_reset();
}

// Collapse + dispatch. oshape/istride are the dst-ordered (permuted) source
// dims: dst dim d has extent oshape[d] and reads the source with stride
// istride[d]; the dst itself is canonical row-major over oshape.
template <bool Scale, typename T>
inline void permute_impl(T* dst, const T* src, const crd::u64* oshape, const crd::i64* istride, crd::u32 r,
                         T alpha) noexcept
{
    // 1. Dimension collapse (the TTC/HPTT lever). The dst side is canonical
    //    row-major, so adjacent dst dims are ALWAYS contiguous there; merging
    //    d-1 (outer) with d (inner) only needs the source to walk contiguously
    //    across the pair: stride[d-1] == stride[d] * shape[d]. Size-1 dims
    //    carry no traversal information and are dropped outright (this also
    //    merges 0-stride broadcast runs: 0 == 0 * n).
    crd::u64 shp[kMaxRank] = {};
    crd::i64 s_in[kMaxRank] = {};
    crd::u32 m = 0;
    for (crd::u32 d = 0; d < r; ++d)
    {
        if (oshape[d] == 1U)
        {
            continue;
        }
        if (m > 0U && s_in[m - 1U] == istride[d] * static_cast<crd::i64>(oshape[d]))
        {
            shp[m - 1U] *= oshape[d];
            s_in[m - 1U] = istride[d];
        }
        else
        {
            shp[m] = oshape[d];
            s_in[m] = istride[d];
            ++m;
        }
    }

    if (m == 0U) // every dim has size 1 (or rank 0): a single element
    {
        if constexpr (Scale)
        {
            *dst = alpha * *src;
        }
        else
        {
            *dst = *src;
        }
        return;
    }
    if (m == 1U)
    {
        permute_row<Scale>(dst, src, shp[0], s_in[0], alpha);
        return;
    }

    // Canonical row-major dst strides over the collapsed shape.
    crd::i64 s_out[kMaxRank] = {};
    s_out[m - 1U] = 1;
    for (crd::u32 d = m - 1U; d-- > 0U;)
    {
        s_out[d] = s_out[d + 1U] * static_cast<crd::i64>(shp[d + 1U]);
    }

    // 2. Kernel dispatch: a = the src-fastest dim (smallest |stride|; ties
    //    prefer the dst-fastest dim b = m-1 so the cheap row path wins).
    crd::u32 a = m - 1U;
    crd::i64 best = s_in[m - 1U] < 0 ? -s_in[m - 1U] : s_in[m - 1U];
    for (crd::u32 d = 0; d + 1U < m; ++d)
    {
        const crd::i64 mag = s_in[d] < 0 ? -s_in[d] : s_in[d];
        if (mag < best)
        {
            best = mag;
            a = d;
        }
    }

    if (a == m - 1U || shp[a] < kPermuteTinyDim || shp[m - 1U] < kPermuteTinyDim)
    {
        permute_rows<Scale>(dst, src, shp, s_in, m, alpha);
        return;
    }
    // Streaming gate: DRAM-sized dst + the alignment the NT stores require
    // (64B-aligned base from Tensor::resize; every microkernel row start is
    // 32B-aligned iff the dst a-stride is a multiple of 8 f32).
    crd::u64 total = 1;
    for (crd::u32 d = 0; d < m; ++d)
    {
        total *= shp[d];
    }
    bool stream = false;
#if defined(__AVX2__)
    if constexpr (std::is_same_v<T, crd::f32>)
    {
        stream = total * sizeof(T) >= kPermuteStreamBytes && (reinterpret_cast<crd::usize>(dst) & 31U) == 0U &&
                 (s_out[a] & 7) == 0;
    }
#endif
    // MT dispatch: a live jobs pool + enough bytes + >= 2 macro tasks. The MT
    // regime is where aggregate NT stores win (re-measured; serial 1T NT was
    // refuted and stays off via kPermuteStreamBytes).
    const crd::u64 outer_est = total / (shp[a] * shp[m - 1U]);
    const crd::u64 tasks_min = outer_est * ((shp[a] + kPermuteTile - 1U) / kPermuteTile);
    const bool mt = crd::jobs::num_workers() > 1U && total * sizeof(T) >= kPermuteMtBytes && tasks_min >= 2U &&
                    tasks_min <= 0xFFFFFFFFULL;
    if (mt)
    {
        bool mt_stream = false;
#if defined(__AVX2__)
        if constexpr (std::is_same_v<T, crd::f32>)
        {
            mt_stream = total * sizeof(T) >= kPermuteMtStreamBytes && (reinterpret_cast<crd::usize>(dst) & 31U) == 0U &&
                        (s_out[a] & 7) == 0;
        }
#endif
        if (mt_stream)
        {
            permute_tiled_mt<Scale, true>(dst, src, shp, s_in, s_out, m, a, alpha);
        }
        else
        {
            permute_tiled_mt<Scale, false>(dst, src, shp, s_in, s_out, m, a, alpha);
        }
        return;
    }
    if (stream)
    {
        permute_tiled<Scale, true>(dst, src, shp, s_in, s_out, m, a, alpha);
#if defined(__AVX2__)
        _mm_sfence(); // NT stores are weakly ordered — publish before returning
#endif
    }
    else
    {
        permute_tiled<Scale, false>(dst, src, shp, s_in, s_out, m, a, alpha);
    }
}

} // namespace detail

// -----------------------------------------------------------------------
// permute_copy — materialize src.permute(order) into dst's contiguous
// canonical row-major storage (NumPy np.transpose(src, order).copy()),
// optionally scaling every element by alpha (HPTT's fused alpha; alpha == 1
// is a pure bit copy). dst is resized to the permuted shape.
//
// order must be a permutation of [0, src.rank()) — validated as a runtime
// status (BadInput), never an assert: permute orders are data-driven inputs
// on the einsum path (ADR-0096 §3). src must NOT alias dst — resize()
// reallocates dst's storage before the copy runs.
// -----------------------------------------------------------------------
template <typename T>
[[nodiscard]] TensorStatus permute_copy(TensorView<const T> src, crd::containers::ConstSpan<crd::u32> order,
                                        Tensor<T>& dst, T alpha = T{1}) noexcept
{
    const crd::u32 r = src.rank();
    if (order.size() != r)
    {
        return TensorStatus::BadInput;
    }
    crd::u64 oshape[kMaxRank] = {};
    crd::i64 istride[kMaxRank] = {};
    crd::u32 seen = 0; // bitmask — each source dim used exactly once (rank <= 8)
    for (crd::u32 d = 0; d < r; ++d)
    {
        const crd::u32 s = order[d];
        if (s >= r || (seen & (1U << s)) != 0U)
        {
            return TensorStatus::BadInput;
        }
        seen |= 1U << s;
        oshape[d] = src.shape(s);
        istride[d] = src.stride(s);
    }

    const TensorStatus st = dst.resize({oshape, r});
    if (st != TensorStatus::Ok)
    {
        return st;
    }
    if (dst.size() == 0U)
    {
        return TensorStatus::Ok; // zero-size: nothing to copy
    }

    if (alpha == T{1})
    {
        detail::permute_impl<false>(dst.data(), src.data(), oshape, istride, r, alpha);
    }
    else
    {
        detail::permute_impl<true>(dst.data(), src.data(), oshape, istride, r, alpha);
    }
    return TensorStatus::Ok;
}

} // namespace crd::hesap::tensor
