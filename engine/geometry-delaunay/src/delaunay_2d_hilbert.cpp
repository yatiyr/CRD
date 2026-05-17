// ---------------------------------------------------------------------------
// crd-geometry-delaunay — v8b 2D Hilbert-sorted Bowyer-Watson Delaunay.
//
// Shares the Bowyer-Watson core with v8a via `delaunay_2d_internal.hpp`.
// This TU contributes ONLY the Hilbert-sort insertion strategy + the entry
// `delaunay_2d_hilbert<T>(points, alloc)`.
//
// **Pinned decisions** (carried for ADR-0076 §23 amendment at v8-close;
// continuation of D73-D80 in v8a):
//
//   D81. **Hilbert grid resolution = 2^16 (65 536) cells per axis**.
//        16-bit indices fit comfortably in `u32` Hilbert codes (32 bits used
//        when interleaved). Sub-cell resolution is irrelevant for sort
//        ordering — ties broken by `original_index`. CGAL `spatial_sort`
//        and Mapbox delaunator both use this resolution.
//
//   D82. **Hilbert mapping = Skilling 2004 iterative xy2d**. Standard
//        bit-by-bit Lam-Shapiro 1994 form, but reformulated by Skilling for
//        constant work per bit. 16 iterations per point → 16 × N bit ops.
//
//   D83. **Sort key = (hilbert_index, original_index)**. The original-index
//        tiebreak resolves coincident-grid-cell points deterministically.
//        Together with super-tri stripping in triangle-id order, this gives
//        a fully deterministic algorithm — same input → same output byte-
//        for-byte. (Output WILL differ from v8a — different insertion order
//        produces different triangle-id allocation order — but is internally
//        deterministic.)
//
//   D84. **Bbox padding factor = 1.0 (no pad)**. Bbox is computed from
//        input points directly; degenerate (all-coincident) bbox falls back
//        to a unit extent for sort purposes (Hilbert sort then degenerates
//        to original-index tiebreak — exactly what we want).
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/sort.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/delaunay/delaunay_2d_hilbert.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

#include "delaunay_2d_internal.hpp"

namespace crd::geometry::delaunay
{
namespace
{

constexpr crd::u32 kHilbertOrder = 16U;             // 2^16 = 65536 cells/axis
constexpr crd::u32 kHilbertN     = 1U << kHilbertOrder;

// Skilling 2004 iterative xy2d. Convert (x, y) in [0, n) integer grid to a
// 1D Hilbert index in [0, n*n). `n` MUST be a power of two.
inline crd::u32 hilbert_xy2d(crd::u32 n, crd::u32 x, crd::u32 y) noexcept
{
    crd::u32 d = 0;
    for (crd::u32 s = n >> 1U; s > 0U; s >>= 1U)
    {
        const crd::u32 rx = (x & s) > 0U ? 1U : 0U;
        const crd::u32 ry = (y & s) > 0U ? 1U : 0U;
        d += s * s * ((3U * rx) ^ ry);
        // Rotate quadrant appropriately.
        if (ry == 0U)
        {
            if (rx == 1U)
            {
                x = s - 1U - x;
                y = s - 1U - y;
            }
            const crd::u32 tmp = x;
            x = y;
            y = tmp;
        }
    }
    return d;
}

template <crd::math::MathScalar T>
inline crd::u32 hilbert_index_for(T px, T py, T xmin, T ymin, T inv_extent) noexcept
{
    // Map [xmin, xmin + extent] → [0, kHilbertN - 1].
    T fx = (px - xmin) * inv_extent;
    T fy = (py - ymin) * inv_extent;
    if (fx < static_cast<T>(0)) { fx = static_cast<T>(0); }
    if (fy < static_cast<T>(0)) { fy = static_cast<T>(0); }
    const T n_minus_1 = static_cast<T>(kHilbertN - 1U);
    if (fx > static_cast<T>(1)) { fx = static_cast<T>(1); }
    if (fy > static_cast<T>(1)) { fy = static_cast<T>(1); }
    const crd::u32 ix = static_cast<crd::u32>(fx * n_minus_1);
    const crd::u32 iy = static_cast<crd::u32>(fy * n_minus_1);
    return hilbert_xy2d(kHilbertN, ix, iy);
}

} // anonymous namespace

template <crd::math::MathScalar T>
DelaunayResult2<T>
delaunay_2d_hilbert(crd::containers::ConstSpan<crd::math::Vec2<T>> points,
                    crd::memory::IAllocator*                        alloc)
{
    using detail::k_null_tri;
    using detail::Tri;
    using detail::TriPool;

    DelaunayResult2<T> result{alloc};
    // N = point count per Bowyer-Watson notation.
    const crd::u32 N = static_cast<crd::u32>(points.size()); // NOLINT(readability-identifier-naming)

    if (N < 3U)
    {
        result.status = DelaunayStatus::TooFewPoints;
        return result;
    }
    for (crd::u32 i = 0; i < N; ++i)
    {
        if (!detail::is_finite_vec(points[i]))
        {
            result.status = DelaunayStatus::NonFiniteInput;
            return result;
        }
    }

    // Bbox for Hilbert mapping.
    T xmin = points[0].x;
    T xmax = points[0].x;
    T ymin = points[0].y;
    T ymax = points[0].y;
    for (crd::u32 i = 1; i < N; ++i)
    {
        if (points[i].x < xmin) { xmin = points[i].x; }
        if (points[i].x > xmax) { xmax = points[i].x; }
        if (points[i].y < ymin) { ymin = points[i].y; }
        if (points[i].y > ymax) { ymax = points[i].y; }
    }
    const T dx = xmax - xmin;
    const T dy = ymax - ymin;
    T       extent = dx > dy ? dx : dy;
    if (extent <= static_cast<T>(0)) { extent = static_cast<T>(1); }
    const T inv_extent = static_cast<T>(1) / extent;

    // Compute Hilbert indices, then sort `(hilbert, original_index)`.
    struct HilbertKey
    {
        crd::u32 hilbert;
        crd::u32 orig_idx;
    };
    crd::containers::Array<HilbertKey> keys(alloc);
    keys.resize(N, HilbertKey{0U, 0U});
    for (crd::u32 i = 0; i < N; ++i)
    {
        keys[i].hilbert  = hilbert_index_for<T>(points[i].x, points[i].y, xmin, ymin, inv_extent);
        keys[i].orig_idx = i;
    }
    crd::containers::sort(keys.data(), keys.data() + keys.size(),
                          [](const HilbertKey& a, const HilbertKey& b) noexcept {
                              if (a.hilbert != b.hilbert) { return a.hilbert < b.hilbert; }
                              return a.orig_idx < b.orig_idx;
                          });

    // Reject duplicates: after Hilbert sort, true duplicates may not be
    // adjacent (different Hilbert keys with collision is rare but possible
    // if the grid quantises them differently). Do a separate exact-coord
    // pass — O(N log N) by lex-sort of indices, then adjacent compare.
    crd::containers::Array<crd::u32> dup_check(alloc);
    dup_check.resize(N, crd::u32{0});
    for (crd::u32 i = 0; i < N; ++i) { dup_check[i] = i; }
    crd::containers::sort(dup_check.data(), dup_check.data() + dup_check.size(),
                          [&](crd::u32 a, crd::u32 b) noexcept {
                              const auto& pa = points[a];
                              const auto& pb = points[b];
                              if (pa.x != pb.x) { return pa.x < pb.x; }
                              if (pa.y != pb.y) { return pa.y < pb.y; }
                              return a < b;
                          });
    for (crd::u32 i = 1; i < N; ++i)
    {
        const auto& pa = points[dup_check[i - 1U]];
        const auto& pb = points[dup_check[i]];
        if (pa.x == pb.x && pa.y == pb.y)
        {
            result.status = DelaunayStatus::DuplicatePoint;
            return result;
        }
    }

    // Augmented points = input ++ 3 super-tri vertices.
    crd::containers::Array<crd::math::Vec2<T>> aug_pts(alloc);
    aug_pts.reserve(static_cast<crd::usize>(N) + 3U);
    for (crd::u32 i = 0; i < N; ++i) { aug_pts.push_back(points[i]); }
    crd::math::Vec2<T> s0{};
    crd::math::Vec2<T> s1{};
    crd::math::Vec2<T> s2{};
    detail::build_super_triangle(points, s0, s1, s2);
    const crd::u32 sv0 = N;
    const crd::u32 sv1 = N + 1U;
    const crd::u32 sv2 = N + 2U;
    aug_pts.push_back(s0);
    aug_pts.push_back(s1);
    aug_pts.push_back(s2);

    // Initial triangulation = single super-triangle.
    TriPool        pool{alloc};
    const crd::u32 super_tri = pool.alloc_tri();
    pool[super_tri].v[0]   = sv0;
    pool[super_tri].v[1]   = sv1;
    pool[super_tri].v[2]   = sv2;
    pool[super_tri].nbr[0] = k_null_tri;
    pool[super_tri].nbr[1] = k_null_tri;
    pool[super_tri].nbr[2] = k_null_tri;

    // Insert each input point in Hilbert order. The hint propagates from
    // one new triangle to the next; spatial locality of Hilbert order means
    // the jump-walk is O(1) average steps.
    crd::u32       hint     = super_tri;
    const crd::u32 max_walk = N * 4U + 16U;
    for (crd::u32 oi = 0; oi < N; ++oi)
    {
        const crd::u32 q_idx = keys[oi].orig_idx;
        const auto&    q_pos = points[q_idx];
        const crd::u32 ct = detail::locate_triangle(pool, aug_pts, hint, q_pos, max_walk);
        if (ct == k_null_tri)
        {
            result.status = DelaunayStatus::InternalInvariant;
            return result;
        }
        const crd::u32 seed = detail::insert_point(pool, aug_pts, q_idx, q_pos, ct, alloc);
        if (seed == k_null_tri)
        {
            result.status = DelaunayStatus::InternalInvariant;
            return result;
        }
        hint = seed;
    }

    // Strip super-triangle vertices.
    for (crd::u32 ti = 0; ti < pool.pool_size(); ++ti)
    {
        if (!pool.alive(ti)) { continue; }
        const Tri& t = pool[ti];
        if (t.v[0] >= N || t.v[1] >= N || t.v[2] >= N) { continue; }
        result.triangle_indices.push_back(t.v[0]);
        result.triangle_indices.push_back(t.v[1]);
        result.triangle_indices.push_back(t.v[2]);
        ++result.triangle_count;
    }

    result.status = DelaunayStatus::Ok;
    return result;
}

// Explicit instantiations.
template DelaunayResult2<crd::f32>
delaunay_2d_hilbert<crd::f32>(crd::containers::ConstSpan<crd::math::Vec2<crd::f32>>,
                              crd::memory::IAllocator*);
template DelaunayResult2<crd::f64>
delaunay_2d_hilbert<crd::f64>(crd::containers::ConstSpan<crd::math::Vec2<crd::f64>>,
                              crd::memory::IAllocator*);

} // namespace crd::geometry::delaunay
