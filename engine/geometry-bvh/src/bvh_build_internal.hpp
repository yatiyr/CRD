#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-bvh — internal binned-SAH build kernels shared by the serial
// builder (`bvh_build`) and the jobs-parallel builder (`bvh_build_parallel`).
// NOT a public header (lives under src/). `crd::geometry::bvh::detail`.
//
// The split is: the *centroid-bounds* pass and the *bin-histogram* pass are
// each an O(count) reduction over the index slice — those are the parts the
// parallel builder fans out over `crd::jobs::parallel_reduce` (per-chunk
// partials folded in fixed job order; the partials use min/max + integer adds,
// which are exact and commutative, so the parallel histograms are bit-identical
// to the serial ones → the parallel build produces a tree byte-for-byte equal
// to the serial one). The *sweep* (over the bins) and the *stable partition*
// are O(bins)/O(count) but stay serial in v1f.
// ---------------------------------------------------------------------------

#include "aabb_ops.hpp"

#include <crd/core/types.hpp>
#include <crd/geometry/primitives/constants.hpp>

#include <limits>

namespace crd::geometry::bvh::detail
{
inline constexpr crd::u32 k_max_bins = 64;
// SAH-cost ties below this collapse to X→Y→Z, lower-bin (ADR-0076 §5.2). Single
// source of truth = `crd-geometry-primitives`' epsilon policy (v1h, ADR-0076 §15).
inline constexpr crd::f32 k_sah_cost_epsilon = crd::geometry::primitives::k_sah_cost_epsilon<crd::f32>();

// Per-axis binned-SAH histogram: bin populations + per-bin primitive bounds.
struct BinHistogram
{
    crd::u32 count[3][k_max_bins];
    AABB3<crd::f32> bounds[3][k_max_bins];
};

// The winning split: prims whose centroid bins to `[0, bin]` on `axis` go left.
struct SplitChoice
{
    int axis{-1};
    crd::u32 bin{0};
    crd::f32 cost{std::numeric_limits<crd::f32>::infinity()};
};

inline void clear_histogram(BinHistogram& h, crd::u32 bins) noexcept
{
    for (int a = 0; a < 3; ++a)
    {
        for (crd::u32 b = 0; b < bins; ++b)
        {
            h.count[a][b] = 0;
            h.bounds[a][b] = aabb_empty();
        }
    }
}

inline void merge_histogram(BinHistogram& dst, const BinHistogram& src, crd::u32 bins) noexcept
{
    for (int a = 0; a < 3; ++a)
    {
        for (crd::u32 b = 0; b < bins; ++b)
        {
            dst.count[a][b] += src.count[a][b];
            aabb_merge(dst.bounds[a][b], src.bounds[a][b]);
        }
    }
}

// Map a centroid coordinate on `axis` to a bin index in `[0, bins)`.
[[nodiscard]] inline crd::u32 bin_index(crd::f32 coord, crd::f32 cmin_a, crd::f32 scale, crd::u32 bins) noexcept
{
    crd::i32 b = static_cast<crd::i32>((coord - cmin_a) * scale);
    if (b < 0)
    {
        b = 0;
    }
    if (b >= static_cast<crd::i32>(bins))
    {
        b = static_cast<crd::i32>(bins) - 1;
    }
    return static_cast<crd::u32>(b);
}

// Accumulate `idx[first, first+count)` into `h` (all three axes). `cbounds` =
// the centroid bounds over the slice (sets up the bin scale); on an axis with
// zero centroid extent, that axis's bins all collapse to bin 0 — the sweep then
// finds no usable split there. (`h` must already be `clear`ed.)
inline void accumulate_histogram(BinHistogram& h, const crd::u32* idx, crd::u32 first, crd::u32 count,
                                 const crd::math::Vec3<crd::f32>* cent, const AABB3<crd::f32>* prim, crd::u32 bins,
                                 const AABB3<crd::f32>& cbounds) noexcept
{
    const crd::math::Vec3<crd::f32> cextent(cbounds.max.x - cbounds.min.x, cbounds.max.y - cbounds.min.y,
                                            cbounds.max.z - cbounds.min.z);
    crd::f32 scale[3];
    crd::f32 cmin[3] = {cbounds.min.x, cbounds.min.y, cbounds.min.z};
    scale[0] = (cextent.x > 0.0F) ? static_cast<crd::f32>(bins) / cextent.x : 0.0F;
    scale[1] = (cextent.y > 0.0F) ? static_cast<crd::f32>(bins) / cextent.y : 0.0F;
    scale[2] = (cextent.z > 0.0F) ? static_cast<crd::f32>(bins) / cextent.z : 0.0F;
    for (crd::u32 i = first; i < first + count; ++i)
    {
        const crd::u32 p = idx[i];
        const crd::math::Vec3<crd::f32>& c = cent[p];
        const crd::f32 cc[3] = {c.x, c.y, c.z};
        for (int a = 0; a < 3; ++a)
        {
            const crd::u32 b = bin_index(cc[a], cmin[a], scale[a], bins);
            ++h.count[a][b];
            aabb_merge(h.bounds[a][b], prim[p]);
        }
    }
}

// Sweep a built histogram: for each axis (X→Y→Z) and each split bin, the SAH
// cost; `out` is replaced only on a *strictly* lower cost, so X→Y→Z + lower-bin
// is the tiebreak with no extra logic (ADR-0076 §5.2). An axis whose centroid
// extent was zero has all its prims in bin 0 ⇒ every split there has an empty
// side ⇒ skipped. `out.axis < 0` afterward ⇒ no spatial split exists.
inline void sweep_for_split(const BinHistogram& h, crd::u32 bins, SplitChoice& out) noexcept
{
    for (int a = 0; a < 3; ++a)
    {
        AABB3<crd::f32> right_bounds[k_max_bins];
        crd::u32 right_count[k_max_bins] = {};
        right_bounds[bins - 1] = aabb_empty();
        right_count[bins - 1] = 0;
        for (crd::i32 s = static_cast<crd::i32>(bins) - 2; s >= 0; --s)
        {
            const auto su = static_cast<crd::u32>(s);
            right_bounds[su] = right_bounds[su + 1];
            aabb_merge(right_bounds[su], h.bounds[a][su + 1]);
            right_count[su] = right_count[su + 1] + h.count[a][su + 1];
        }
        AABB3<crd::f32> left_bounds = aabb_empty();
        crd::u32 left_count = 0;
        for (crd::u32 s = 0; s + 1 < bins; ++s)
        {
            aabb_merge(left_bounds, h.bounds[a][s]);
            left_count += h.count[a][s];
            if (left_count == 0 || right_count[s] == 0)
            {
                continue; // everything on one side — not a usable split
            }
            const crd::f32 cost = static_cast<crd::f32>(left_count) * aabb_half_area(left_bounds) +
                                  static_cast<crd::f32>(right_count[s]) * aabb_half_area(right_bounds[s]);
            if (cost + k_sah_cost_epsilon < out.cost)
            {
                out.cost = cost;
                out.axis = a;
                out.bin = s;
            }
        }
    }
}

// Centroid bounds over `idx[first, first+count)` (serial — the parallel builder
// has its own fanned-out version).
[[nodiscard]] inline AABB3<crd::f32> centroid_bounds_of(const crd::u32* idx, crd::u32 first, crd::u32 count,
                                                        const crd::math::Vec3<crd::f32>* cent) noexcept
{
    AABB3<crd::f32> b = aabb_empty();
    for (crd::u32 i = first; i < first + count; ++i)
    {
        aabb_include_point(b, cent[idx[i]]);
    }
    return b;
}

// Stable two-pass partition of `idx[first, first+count)`: prims whose centroid
// bins to `[0, best_bin]` on `best_axis` go left, in source order; the rest go
// right, in source order. Uses `scratch[0, count)`. Returns the left count
// (`mid` ∈ (0, count) given a real split). Deterministic; no `std::sort`.
[[nodiscard]] inline crd::u32 stable_partition_by_bin(crd::u32* idx, crd::u32* scratch, crd::u32 first, crd::u32 count,
                                                      const crd::math::Vec3<crd::f32>* cent, int best_axis,
                                                      crd::u32 best_bin, const AABB3<crd::f32>& cbounds,
                                                      crd::u32 bins) noexcept
{
    const crd::f32 cextent_a = (best_axis == 0)   ? (cbounds.max.x - cbounds.min.x)
                               : (best_axis == 1) ? (cbounds.max.y - cbounds.min.y)
                                                  : (cbounds.max.z - cbounds.min.z);
    const crd::f32 cmin_a = (best_axis == 0) ? cbounds.min.x : (best_axis == 1 ? cbounds.min.y : cbounds.min.z);
    const crd::f32 scale = (cextent_a > 0.0F) ? static_cast<crd::f32>(bins) / cextent_a : 0.0F;
    const auto bin_of = [&](crd::u32 p) noexcept -> crd::u32
    {
        const crd::math::Vec3<crd::f32>& c = cent[p];
        const crd::f32 coord = (best_axis == 0) ? c.x : (best_axis == 1 ? c.y : c.z);
        return bin_index(coord, cmin_a, scale, bins);
    };
    crd::u32 w = 0;
    for (crd::u32 i = first; i < first + count; ++i)
    {
        if (bin_of(idx[i]) <= best_bin)
        {
            scratch[w++] = idx[i];
        }
    }
    const crd::u32 mid = w;
    for (crd::u32 i = first; i < first + count; ++i)
    {
        if (bin_of(idx[i]) > best_bin)
        {
            scratch[w++] = idx[i];
        }
    }
    for (crd::u32 i = 0; i < count; ++i)
    {
        idx[first + i] = scratch[i];
    }
    return mid;
}

} // namespace crd::geometry::bvh::detail
