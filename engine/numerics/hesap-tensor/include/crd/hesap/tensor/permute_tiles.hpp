#pragma once

// The tile loops and the plane walk of the blocked 2-D transpose (permute.hpp, ADR-0096 §5): one (a, b) region of a
// permute, cut into tiles that keep both the read and the write stream inside L1, and the odometer over the planes of
// a tiled permute. General code (permute.hpp, every element type) and the per-level tensor kernels
// (../../../../kernels/tensor_kernels.cpp, the f32, f64 and complex bodies) both compile them, so they live in the
// unit's level namespace (CRD_MATH_LEVEL_NS, ADR-0134 §5).

#include <crd/core/types.hpp>
#include <crd/hesap/tensor/tensor_kernels.hpp>
#include <crd/math/kernel_level.hpp>

namespace crd::hesap::tensor::detail::inline CRD_MATH_LEVEL_NS
{

// Scalar (a, b)-plane tile of elements of Parts scalars T each: dst(ia, ib) at element ia*ra + ib (b is
// dst-contiguous), src(ia, ib) at element ia*sa + ib*sb, put(d, s) writing the element at d from the one at s. The
// inner loop over ib keeps the WRITE stream contiguous; the read stream jumps sb per step but stays inside the tile's
// L1 footprint.
template <crd::u64 Parts, typename T, typename Put>
inline void permute_tile_scalar(T* dst, crd::i64 ra, const T* src, crd::i64 sa, crd::i64 sb, crd::u64 na, crd::u64 nb,
                                const Put& put) noexcept
{
    constexpr crd::i64 parts = static_cast<crd::i64>(Parts);
    const crd::i64 step = sb * parts;
    for (crd::u64 ia = 0; ia < na; ++ia)
    {
        T* drow = dst + static_cast<crd::i64>(ia) * ra * parts;
        const T* scol = src + static_cast<crd::i64>(ia) * sa * parts;
        crd::i64 off = 0;
        for (crd::u64 ib = 0; ib < nb; ++ib, off += step)
        {
            put(drow + static_cast<crd::i64>(ib) * parts, scol + off);
        }
    }
}

// The region [0, na) x [0, nb) in tile_a x tile_b tiles, a-tiles outer: tile(ta, tb, ha, hb, next) copies the tile at
// (ta, tb) of extents (ha, hb), where next is the tile distance tiles after it (0: none), located from this tile's
// origin (src(ia, ib) at ia*sa + ib*sb, dst at ia*ra + ib): a later tile of the region or, past its last one, a tile
// of the next region (region_next, from the region's origin, cut in the same tiles), the HPTT macro-tile prefetch
// order at distance 1.
template <typename Tile>
inline void permute_region_tiles(crd::u64 na, crd::u64 nb, crd::u64 tile_a, crd::u64 tile_b, crd::i64 sa, crd::i64 sb,
                                 crd::i64 ra, PermuteNext region_next, crd::u64 distance, const Tile& tile) noexcept
{
    // The extent of the tile at t of [0, n) cut in edge_tile steps, and the tiles [0, n) cuts into.
    const auto edge = [](crd::u64 n, crd::u64 t, crd::u64 edge_tile)
    {
        return n - t < edge_tile ? n - t : edge_tile;
    };
    const auto tiles = [](crd::u64 n, crd::u64 edge_tile)
    {
        return (n + edge_tile - 1U) / edge_tile;
    };
    const crd::u64 ntb = tiles(nb, tile_b);
    const crd::u64 count = tiles(na, tile_a) * ntb;
    const bool has_region_next = region_next.na != 0U && region_next.nb != 0U;
    const crd::u64 next_ntb = has_region_next ? tiles(region_next.nb, tile_b) : 0U;
    const crd::u64 next_count = has_region_next ? tiles(region_next.na, tile_a) * next_ntb : 0U;
    crd::u64 q = 0; // this tile's index in the region's tile order
    for (crd::u64 ta = 0; ta < na; ta += tile_a)
    {
        const crd::u64 ha = edge(na, ta, tile_a);
        for (crd::u64 tb = 0; tb < nb; tb += tile_b, ++q)
        {
            PermuteNext next{0, 0, 0U, 0U};
            const crd::u64 target = q + distance;
            if (distance != 0U && target < count)
            {
                const crd::u64 na2 = target / ntb * tile_a;
                const crd::u64 nb2 = target % ntb * tile_b;
                const crd::i64 da = static_cast<crd::i64>(na2) - static_cast<crd::i64>(ta);
                const crd::i64 db = static_cast<crd::i64>(nb2) - static_cast<crd::i64>(tb);
                next = {da * sa + db * sb, da * ra + db, edge(na, na2, tile_a), edge(nb, nb2, tile_b)};
            }
            else if (distance != 0U && target - count < next_count)
            {
                const crd::u64 at = target - count;
                const crd::i64 na2 = static_cast<crd::i64>(at / next_ntb * tile_a);
                const crd::i64 nb2 = static_cast<crd::i64>(at % next_ntb * tile_b);
                const crd::i64 da = static_cast<crd::i64>(ta);
                const crd::i64 db = static_cast<crd::i64>(tb);
                next = {region_next.src + na2 * sa + nb2 * sb - (da * sa + db * sb),
                        region_next.dst + na2 * ra + nb2 - (da * ra + db),
                        edge(region_next.na, static_cast<crd::u64>(na2), tile_a),
                        edge(region_next.nb, static_cast<crd::u64>(nb2), tile_b)};
            }
            tile(ta, tb, ha, edge(nb, tb, tile_b), next);
        }
    }
}

// The outer odometer of the tiled transpose: every dim except a (the src-fastest dim) and b = m-1 (the dst-fastest
// dim), INNERMOST-first. PermuteOrder::Src sorts by ascending |src stride|, so consecutive plane visits move the
// scattered source stream by its smallest step (page/TLB locality on the stream that has none inside the tile; the dst
// side is contiguous per row regardless). PermuteOrder::Hptt sorts by HPTT's loop cost (springer13/hptt
// src/transpose.cpp loopCostHeuristic): a dim weighs 2^(m - rank of |its src stride|) + 1.01 * 2^(m - rank of its dst
// stride), stride rank 0 the smallest, and the heaviest dim runs innermost. Writes the dims to ord and returns their
// count and (through outer) the number of planes.
inline crd::u32 permute_outer_order(const crd::u64* shp, const crd::i64* s_in, crd::u32 m, crd::u32 a,
                                    PermuteOrder order, crd::u32* ord, crd::u64& outer) noexcept
{
    const auto mag = [s_in](crd::u32 d)
    {
        return s_in[d] < 0 ? -s_in[d] : s_in[d];
    };
    // The sort key, ascending innermost first: |src stride|, or minus HPTT's weight (100 : 101 for 1 : 1.01). The dst
    // is canonical row-major over shp, so dim d's dst stride rank is m - 1 - d.
    crd::i64 key[kPermuteMaxRank] = {};
    for (crd::u32 d = 0; d < m; ++d)
    {
        if (order == PermuteOrder::Src)
        {
            key[d] = mag(d);
            continue;
        }
        crd::u32 rank_src = 0;
        for (crd::u32 e = 0; e < m; ++e)
        {
            rank_src += mag(e) < mag(d) || (mag(e) == mag(d) && e < d) ? 1U : 0U;
        }
        const crd::u32 rank_dst = m - 1U - d;
        key[d] = -(100 * (crd::i64{1} << (m - rank_src)) + 101 * (crd::i64{1} << (m - rank_dst)));
    }
    crd::u32 nout = 0;
    for (crd::u32 d = 0; d < m; ++d)
    {
        if (d != a && d != m - 1U)
        {
            ord[nout++] = d;
        }
    }
    for (crd::u32 i = 1; i < nout; ++i) // insertion sort, ascending key = innermost first
    {
        const crd::u32 v = ord[i];
        const crd::i64 mv = key[v];
        crd::u32 j = i;
        while (j > 0U)
        {
            const crd::i64 mj = key[ord[j - 1U]];
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

// Every (a, m-1) plane of a tiled permute in permute_outer_order's order: plane(soff, doff, next) copies the plane
// whose origin is src + soff, dst + doff, and next is the following plane (0 x 0 after the last).
template <typename Plane>
inline void permute_walk_planes(const crd::u64* shp, const crd::i64* s_in, const crd::i64* s_out, crd::u32 m,
                                crd::u32 a, PermuteOrder order, const Plane& plane) noexcept
{
    crd::u32 ord[kPermuteMaxRank];
    crd::u64 outer = 1;
    const crd::u32 nout = permute_outer_order(shp, s_in, m, a, order, ord, outer);
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
