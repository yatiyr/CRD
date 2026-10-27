#pragma once

// The tile loops of the blocked 2-D transpose (permute.hpp, ADR-0096 §5): one (a, b) region of a permute, cut into
// tiles that keep both the read and the write stream inside L1. General code (permute.hpp, every element type) and
// the per-level tensor kernels (../../../../kernels/tensor_kernels.cpp, the f32 bodies) both compile them, so they
// live in the unit's level namespace (CRD_MATH_LEVEL_NS, ADR-0134 §5).

#include <crd/core/types.hpp>
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

// The region [0, na) x [0, nb) in tile_a x tile_b tiles, a-tiles outer: tile(ta, tb, ha, hb) copies the tile at
// (ta, tb) of extents (ha, hb).
template <typename Tile>
inline void permute_region_tiles(crd::u64 na, crd::u64 nb, crd::u64 tile_a, crd::u64 tile_b, const Tile& tile) noexcept
{
    for (crd::u64 ta = 0; ta < na; ta += tile_a)
    {
        const crd::u64 ha = na - ta < tile_a ? na - ta : tile_a;
        for (crd::u64 tb = 0; tb < nb; tb += tile_b)
        {
            tile(ta, tb, ha, nb - tb < tile_b ? nb - tb : tile_b);
        }
    }
}

} // namespace crd::hesap::tensor::detail::inline CRD_MATH_LEVEL_NS
