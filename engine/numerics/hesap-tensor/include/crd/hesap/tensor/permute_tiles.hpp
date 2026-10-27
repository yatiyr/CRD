#pragma once

// The tile loops and the plane walk of the blocked 2-D transpose (permute.hpp, ADR-0096 §5): one (a, b) region of a
// permute, cut into tiles that keep both the read and the write stream inside L1, and the odometer over the planes of
// a tiled permute. General code (permute.hpp, every element type) and the per-level tensor kernels
// (../../../../kernels/tensor_kernels.cpp, the f32 bodies) both compile them, so they live in the unit's level
// namespace (CRD_MATH_LEVEL_NS, ADR-0134 §5).

#include <crd/core/types.hpp>
#include <crd/hesap/tensor/tensor_kernels.hpp>
#include <crd/math/kernel_level.hpp>

namespace crd::hesap::tensor::detail::inline CRD_MATH_LEVEL_NS
{

// Scalar (a, b)-plane tile: dst(ia, ib) at ia*ra + ib (b is dst-contiguous), src(ia, ib) at ia*sa + ib*sb. The inner
// loop over ib keeps the WRITE stream contiguous; the read stream jumps sb per step but stays inside the tile's L1
// footprint. Scale = false is a bit copy (NaN payloads included); Scale = true is one IEEE multiply per element.
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

// The region [0, na) x [0, nb) in tile_a x tile_b tiles, a-tiles outer: tile(ta, tb, ha, hb, next) copies the tile at
// (ta, tb) of extents (ha, hb), where next is the tile after it, located from this tile's origin (src(ia, ib) at
// ia*sa + ib*sb, dst at ia*ra + ib): the next tile of the region, or after the last one the first tile of the next
// region (region_next, from the region's origin), the HPTT macro-tile prefetch order.
template <typename Tile>
inline void permute_region_tiles(crd::u64 na, crd::u64 nb, crd::u64 tile_a, crd::u64 tile_b, crd::i64 sa, crd::i64 sb,
                                 crd::i64 ra, PermuteNext region_next, const Tile& tile) noexcept
{
    // The extent of the tile at t of [0, n) cut in edge_tile steps.
    const auto edge = [](crd::u64 n, crd::u64 t, crd::u64 edge_tile)
    {
        return n - t < edge_tile ? n - t : edge_tile;
    };
    for (crd::u64 ta = 0; ta < na; ta += tile_a)
    {
        const crd::u64 ha = edge(na, ta, tile_a);
        for (crd::u64 tb = 0; tb < nb; tb += tile_b)
        {
            // The step from this tile's origin to the next tile's (da rows of a, db columns of b).
            crd::i64 da = 0;
            crd::i64 db = 0;
            PermuteNext next{0, 0, 0U, 0U};
            if (tb + tile_b < nb)
            {
                db = static_cast<crd::i64>(tile_b);
                next = {0, 0, ha, edge(nb, tb + tile_b, tile_b)};
            }
            else if (ta + tile_a < na)
            {
                da = static_cast<crd::i64>(tile_a);
                db = -static_cast<crd::i64>(tb);
                next = {0, 0, edge(na, ta + tile_a, tile_a), edge(nb, 0U, tile_b)};
            }
            else if (region_next.na != 0U && region_next.nb != 0U)
            {
                da = -static_cast<crd::i64>(ta);
                db = -static_cast<crd::i64>(tb);
                next = {region_next.src, region_next.dst, edge(region_next.na, 0U, tile_a),
                        edge(region_next.nb, 0U, tile_b)};
            }
            next.src += da * sa + db * sb;
            next.dst += da * ra + db;
            tile(ta, tb, ha, edge(nb, tb, tile_b), next);
        }
    }
}

// The outer odometer of the tiled transpose: every dim except a (the src-fastest dim) and b = m-1 (the dst-fastest
// dim), INNERMOST-first by ascending |src stride|, so consecutive plane visits move the scattered source stream by its
// smallest step (page/TLB locality on the stream that has none inside the tile; the dst side is contiguous per row
// regardless). Writes the dims to ord and returns their count and (through outer) the number of planes.
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

// Every (a, m-1) plane of a tiled permute in permute_outer_order: plane(soff, doff, next) copies the plane whose
// origin is src + soff, dst + doff, and next is the following plane (0 x 0 after the last).
template <typename Plane>
inline void permute_walk_planes(const crd::u64* shp, const crd::i64* s_in, const crd::i64* s_out, crd::u32 m,
                                crd::u32 a, const Plane& plane) noexcept
{
    crd::u32 ord[kPermuteMaxRank];
    crd::u64 outer = 1;
    const crd::u32 nout = permute_outer_order(shp, s_in, m, a, ord, outer);
    crd::u64 idx[kPermuteMaxRank] = {};
    crd::i64 soff = 0;
    crd::i64 doff = 0;
    for (crd::u64 it = 0; it < outer; ++it)
    {
        const crd::i64 plane_soff = soff;
        const crd::i64 plane_doff = doff;
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
        const bool last = it + 1U == outer;
        plane(plane_soff, plane_doff,
              PermuteNext{soff - plane_soff, doff - plane_doff, last ? 0U : shp[a], last ? 0U : shp[m - 1U]});
    }
}

} // namespace crd::hesap::tensor::detail::inline CRD_MATH_LEVEL_NS
