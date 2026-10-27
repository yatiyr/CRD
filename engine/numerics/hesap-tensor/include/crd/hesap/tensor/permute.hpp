#pragma once

#include <crd/hesap/tensor/permute_tiles.hpp>
#include <crd/hesap/tensor/tensor.hpp>
#include <crd/hesap/tensor/tensor_dispatch.hpp>
#include <crd/jobs/jobs.hpp>

#include <cstring>
#include <type_traits>

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
//        2-D kernel. f32 regions run the per-level tensor kernels
//        (tensor_kernels.hpp, SIMD-DISPATCH): on AVX2, tiles with a unit
//        source stride run the 8x8 in-register transpose microkernel
//        (unpack/shuffle/permute2f128); everything else runs the scalar tile
//        loop with the write stream contiguous.
//      - tiny inner extents fall back to the simple strided row loop (tile
//        machinery is pure overhead below kPermuteTinyDim).
//
// Determinism: serial, fixed traversal order, pure element copies (plus one
// IEEE multiply per element when alpha != 1) -> bit-identical by
// construction, on every kernel level and worker count; alpha == 1 is a pure bit copy (no arithmetic touches the
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

// One (a, b) region [0, na) x [0, nb) in tile_a x tile_b tiles: dst(ia, ib)
// at ia*ra + ib, src(ia, ib) at ia*sa + ib*sb. f32 runs the per-level tensor
// kernel (the 8x8 in-register transpose on AVX2, non-temporal stores when
// stream is set; one call per region); every other type runs the scalar tiles.
template <bool Scale, typename T>
inline void permute_region(T* dst, crd::i64 ra, const T* src, crd::i64 sa, crd::i64 sb, crd::u64 na, crd::u64 nb,
                           crd::u64 tile_a, crd::u64 tile_b, T alpha, bool stream) noexcept
{
    if constexpr (std::is_same_v<T, crd::f32>)
    {
        tensor_kernels().table->permute_region_f32(
            {dst, ra, src, sa, sb, na, nb, tile_a, tile_b, alpha, Scale, stream});
    }
    else
    {
        (void)stream;
        permute_region_tiles(na, nb, tile_a, tile_b,
                             [&](crd::u64 ta, crd::u64 tb, crd::u64 ha, crd::u64 hb)
                             {
                                 permute_tile_scalar<Scale>(
                                     dst + static_cast<crd::i64>(ta) * ra + static_cast<crd::i64>(tb), ra,
                                     src + static_cast<crd::i64>(ta) * sa + static_cast<crd::i64>(tb) * sb, sa, sb, ha,
                                     hb, alpha);
                             });
    }
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

// The outer odometer of the tiled transpose: every dim except a (the
// src-fastest dim) and b = m-1 (the dst-fastest dim), INNERMOST-first by
// ascending |src stride|, so consecutive plane visits move the scattered
// source stream by its smallest step (page/TLB locality on the stream that
// has none inside the tile; the dst side is contiguous per row regardless).
// Writes the dims to ord and returns their count and (through outer) the
// number of planes.
inline crd::u32 permute_outer_order(const crd::u64* shp, const crd::i64* s_in, crd::u32 m, crd::u32 a, crd::u32* ord,
                                    crd::u64& outer) noexcept
{
    crd::u32 nout = 0;
    for (crd::u32 d = 0; d < m; ++d)
    {
        if (d != a && d != m - 1U)
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
    outer = 1;
    for (crd::u32 i = 0; i < nout; ++i)
    {
        outer *= shp[ord[i]];
    }
    return nout;
}

// Tiled-transpose kernel: the outer odometer around one square-tiled 2-D
// transpose of the (a, b) plane per step.
template <bool Scale, typename T>
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

    crd::u32 ord[kMaxRank];
    crd::u64 outer = 1;
    const crd::u32 nout = permute_outer_order(shp, s_in, m, a, ord, outer);
    crd::u64 idx[kMaxRank] = {};
    crd::i64 soff = 0;
    crd::i64 doff = 0;
    for (crd::u64 it = 0; it < outer; ++it)
    {
        permute_region<Scale>(dst + doff, ra, src + soff, sa_in, sb_in, na, nb, tile, tile, alpha, false);
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
// (outer odometer position × (a, b) super-block) — every task writes a
// DISJOINT dst region (the permutation is a bijection), so the result bytes
// are identical for ANY worker count / any scheduling BY CONSTRUCTION (gated
// anyway). Tasks decompose their outer index into odometer digits along the
// same src-locality order the serial path walks. Streaming (NT) stores are
// the aggregate-bandwidth lever in this regime; the kernel publishes them
// before each region returns (the jobs counter's release ordering does not
// cover NT stores).
inline constexpr crd::u64 kPermuteMtBytes = 4U * 1024U * 1024U;
inline constexpr crd::u64 kPermuteMtStreamBytes = 8U * 1024U * 1024U;

template <bool Scale, typename T>
inline void permute_tiled_mt(T* dst, const T* src, const crd::u64* shp, const crd::i64* s_in, const crd::i64* s_out,
                             crd::u32 m, crd::u32 a, T alpha, bool stream) noexcept
{
    const crd::u32 b = m - 1U;
    // MT tiles (measured): near columns -> 32x32 (the per-tile read-stream
    // count stays under the L2 streamer's tracker with 8 cores in flight);
    // page-scattered columns -> TALL tiles along a, the src stride-1 dim
    // where sequential DRAM bursts live (up to 512x32: each column visit
    // reads 2 KB instead of 128 B, the 3D 512³ lever).
    const crd::i64 sb_mag = s_in[b] < 0 ? -s_in[b] : s_in[b];
    const bool near_cols = static_cast<crd::u64>(sb_mag) * sizeof(T) <= 65536U;
    const crd::u64 tall = shp[a] < 512U ? shp[a] : 512U;
    const crd::u64 tile_a = near_cols ? 32U : tall;
    const crd::u64 tile_b = 32U;

    crd::u32 ord[kMaxRank];
    crd::u64 outer = 1;
    const crd::u32 nout = permute_outer_order(shp, s_in, m, a, ord, outer);

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
        bool stream;
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
    const Params prm{dst, src,    shp,    s_in,  s_out, ord,         nout,  a,
                     b,   tile_a, tile_b, sup_a, sup_b, n_ta * n_tb, alpha, stream};
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
                permute_region<Scale>(
                    prm.dst + doff + static_cast<crd::i64>(ta0) * ra + static_cast<crd::i64>(tb0), ra,
                    prm.src + soff + static_cast<crd::i64>(ta0) * sa_in + static_cast<crd::i64>(tb0) * sb_in, sa_in,
                    sb_in, ta1 - ta0, tb1 - tb0, prm.tile_a, prm.tile_b, prm.alpha, prm.stream);
            }
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
    crd::u64 total = 1;
    for (crd::u32 d = 0; d < m; ++d)
    {
        total *= shp[d];
    }
    // MT dispatch: a live jobs pool + enough bytes + >= 2 macro tasks. The MT
    // regime is where aggregate NT stores win; the single-thread path keeps
    // regular stores (NT stores measured slower there).
    const crd::u64 outer_est = total / (shp[a] * shp[m - 1U]);
    const crd::u64 tasks_min = outer_est * ((shp[a] + kPermuteTile - 1U) / kPermuteTile);
    const bool mt = crd::jobs::num_workers() > 1U && total * sizeof(T) >= kPermuteMtBytes && tasks_min >= 2U &&
                    tasks_min <= 0xFFFFFFFFULL;
    if (!mt)
    {
        permute_tiled<Scale>(dst, src, shp, s_in, s_out, m, a, alpha);
        return;
    }
    // Streaming gate: DRAM-sized f32 dst + the alignment the NT stores require.
    // The base is 64B-aligned (Tensor::resize); every region origin and
    // microkernel row start is 32B-aligned iff the dst-fastest extent is a
    // multiple of 8 f32, since every other dst stride is a multiple of it
    // (the a-stride alone is not enough: an outer dim between a and b steps
    // the origin by its own stride).
    const bool stream = std::is_same_v<T, crd::f32> && total * sizeof(T) >= kPermuteMtStreamBytes &&
                        (reinterpret_cast<crd::usize>(dst) & 31U) == 0U && (shp[m - 1U] & 7U) == 0U;
    permute_tiled_mt<Scale>(dst, src, shp, s_in, s_out, m, a, alpha, stream);
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
