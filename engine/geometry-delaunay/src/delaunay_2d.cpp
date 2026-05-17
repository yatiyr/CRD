// ---------------------------------------------------------------------------
// crd-geometry-delaunay — v8a 2D Bowyer-Watson Delaunay implementation.
//
// **v8b refactor (2026-05-17)**: the Bowyer-Watson core (TriPool, Tri,
// build_super_triangle, locate_triangle, insert_point) moved into
// `delaunay_2d_internal.hpp` so v8b `delaunay_2d_hilbert.cpp` shares the
// same body. This TU now owns only the v8a-specific:
//   - Input validation (`TooFewPoints` / `NonFiniteInput` / `DuplicatePoint`).
//   - **Lex-sort `(x, y, original_index)`** insertion order (D75).
//   - Augmented-points construction + super-tri seed.
//   - Per-point insert loop driving the shared core.
//   - Strip-time triangle emission (D80).
//
// Pinned decisions D73-D80 documented at `delaunay_2d_internal.hpp` head.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/sort.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/delaunay/delaunay_2d.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

#include "delaunay_2d_internal.hpp"

namespace crd::geometry::delaunay
{

template <crd::math::MathScalar T>
DelaunayResult2<T>
delaunay_2d(crd::containers::ConstSpan<crd::math::Vec2<T>> points,
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

    // Build sort order: lex (x, y, original_index).
    crd::containers::Array<crd::u32> order(alloc);
    order.resize(N, crd::u32{0});
    for (crd::u32 i = 0; i < N; ++i) { order[i] = i; }
    crd::containers::sort(order.data(), order.data() + order.size(),
                          [&](crd::u32 a, crd::u32 b) noexcept {
                              const auto& pa = points[a];
                              const auto& pb = points[b];
                              if (pa.x != pb.x) { return pa.x < pb.x; }
                              if (pa.y != pb.y) { return pa.y < pb.y; }
                              return a < b;
                          });

    // Reject duplicate points (consecutive in sorted order with identical
    // (x, y)).
    for (crd::u32 i = 1; i < N; ++i)
    {
        const auto& pa = points[order[i - 1U]];
        const auto& pb = points[order[i]];
        if (pa.x == pb.x && pa.y == pb.y)
        {
            result.status = DelaunayStatus::DuplicatePoint;
            return result;
        }
    }

    // Augmented points array = input points ++ 3 super-triangle vertices.
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

    // Insert each input point in lex-sorted order.
    crd::u32       hint     = super_tri;
    const crd::u32 max_walk = N * 4U + 16U; // safety cap
    for (crd::u32 oi = 0; oi < N; ++oi)
    {
        const crd::u32 q_idx = order[oi];
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

    // Strip super-triangle vertices: emit any alive triangle whose 3 vertices
    // are all < N (i.e., none is a super-triangle vertex).
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
delaunay_2d<crd::f32>(crd::containers::ConstSpan<crd::math::Vec2<crd::f32>>,
                       crd::memory::IAllocator*);
template DelaunayResult2<crd::f64>
delaunay_2d<crd::f64>(crd::containers::ConstSpan<crd::math::Vec2<crd::f64>>,
                       crd::memory::IAllocator*);

} // namespace crd::geometry::delaunay
