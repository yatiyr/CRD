// ---------------------------------------------------------------------------
// crd-geometry-delaunay — v8c 3D Bowyer-Watson Delaunay implementation.
//
// See header for the algorithm contract. This TU owns:
//   - Input validation (TooFewPoints / NonFiniteInput / DuplicatePoint /
//     Coplanar).
//   - Lex-sort `(x, y, z, original_index)` insertion order.
//   - Augmented-points construction + super-tet seed (CCW verified).
//   - Per-point insert loop driving the shared Phase 1-5 core in
//     `delaunay_3d_internal.hpp`.
//   - Strip-time tet emission (filtering tets that touch super-tet vertices).
//
// Pinned design decisions D90-D94:
//
//   D90. **Internal `Tet` slot layout**: `{ u32 v[4]; u32 nbr[4]; u8 alive; }`,
//        ≤ 40 bytes pinned by `static_assert` in the internal header. `v[i]`
//        is vertex id (input index or N+0/1/2/3 for super-tet). `nbr[i]` is
//        the tet opposite v[i], or `k_null_tet`. Free-list LIFO pool.
//
//   D91. **Star-shape defensive check** at cavity boundary collection: every
//        boundary face must satisfy `orient3d(face_v0, face_v1, face_v2, q) > 0`.
//        With Stage D `insphere` (v8c-pre) this MUST hold for valid input;
//        a failure means input is degenerate beyond what predicates can
//        resolve. Return `DelaunayStatus3::InternalInvariant` rather than
//        ship a corrupt mesh.
//
//   D92. **Face vertex permutation table** in the internal header — for
//        each face index i, lists the 3 vertices of the canonical
//        outward-oriented face opposite v[i]. Verified via transposition
//        parity such that orient3d(face_v0, face_v1, face_v2, v[i]) > 0 for
//        every positively-oriented tet. Lets the new tet (face, q) inherit
//        positive orientation automatically.
//
//   D93. **Coplanar diagnostic**: if all N input points are coplanar, no 3D
//        tetrahedralisation exists. Detected at validation time by scanning
//        the first 4-tuple of lex-sorted points for non-zero `orient3d`; if
//        none found, fall back to checking up to all N points. If every
//        4-tuple is coplanar, report `DelaunayStatus3::Coplanar`. Bypasses
//        the algorithm entirely (saves super-tet construction on a known-
//        bad input).
//
//   D94. **Super-tet ordering** chosen explicitly to give positive
//        orient3d: 3-corner CCW base in the z = cz - scale plane + 4th
//        vertex above. Verified by `CRD_ASSERT` at init in debug builds.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/sort.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/delaunay/delaunay_3d.hpp>
#include <crd/geometry/primitives/predicates.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

#include "delaunay_3d_internal.hpp"

namespace crd::geometry::delaunay
{

namespace
{

// Detect "all coplanar". Scan the lex-sorted prefix; the first 4-tuple
// with non-zero orient3d proves the input has 3D extent. If we exhaust all
// 4-tuples and find none non-zero, all points are coplanar.
//
// For typical input the first 4 sorted points are non-coplanar so this is
// O(1). Worst case (truly coplanar input or input where the first many
// points happen to be coplanar by accident) is O(N) — we walk through the
// sorted prefix incrementally trying each new point against the first 3.
template <crd::math::MathScalar T>
bool all_points_coplanar(crd::containers::ConstSpan<crd::math::Vec3<T>> points,
                           const crd::containers::Array<crd::u32>&         order)
{
    if (points.size() < 4U) { return true; }
    const auto& p0 = points[order[0]];
    const auto& p1 = points[order[1]];
    const auto& p2 = points[order[2]];
    for (crd::usize i = 3; i < points.size(); ++i)
    {
        const T o = crd::geometry::primitives::orient3d(p0, p1, p2, points[order[i]]);
        if (o != static_cast<T>(0)) { return false; }
    }
    return true;
}

} // anonymous namespace

template <crd::math::MathScalar T>
DelaunayResult3<T>
delaunay_3d(crd::containers::ConstSpan<crd::math::Vec3<T>> points,
            crd::memory::IAllocator*                        alloc)
{
    using detail3d::k_null_tet;
    using detail3d::Tet;
    using detail3d::TetPool;

    DelaunayResult3<T> result{alloc};
    // N = point count per Bowyer-Watson notation.
    const crd::u32 N = static_cast<crd::u32>(points.size()); // NOLINT(readability-identifier-naming)

    if (N < 4U)
    {
        result.status = DelaunayStatus3::TooFewPoints;
        return result;
    }
    for (crd::u32 i = 0; i < N; ++i)
    {
        if (!detail3d::is_finite_vec(points[i]))
        {
            result.status = DelaunayStatus3::NonFiniteInput;
            return result;
        }
    }

    // Lex-sort `(x, y, z, original_index)`.
    crd::containers::Array<crd::u32> order(alloc);
    order.resize(N, crd::u32{0});
    for (crd::u32 i = 0; i < N; ++i) { order[i] = i; }
    crd::containers::sort(order.data(), order.data() + order.size(),
                          [&](crd::u32 a, crd::u32 b) noexcept {
                              const auto& pa = points[a];
                              const auto& pb = points[b];
                              if (pa.x != pb.x) { return pa.x < pb.x; }
                              if (pa.y != pb.y) { return pa.y < pb.y; }
                              if (pa.z != pb.z) { return pa.z < pb.z; }
                              return a < b;
                          });

    // Reject duplicate points (consecutive in sorted order with identical
    // (x, y, z)).
    for (crd::u32 i = 1; i < N; ++i)
    {
        const auto& pa = points[order[i - 1U]];
        const auto& pb = points[order[i]];
        if (pa.x == pb.x && pa.y == pb.y && pa.z == pb.z)
        {
            result.status = DelaunayStatus3::DuplicatePoint;
            return result;
        }
    }

    // Coplanar diagnostic (D93).
    if (all_points_coplanar(points, order))
    {
        result.status = DelaunayStatus3::Coplanar;
        return result;
    }

    // Augmented points array = input points ++ 4 super-tet vertices.
    crd::containers::Array<crd::math::Vec3<T>> aug_pts(alloc);
    aug_pts.reserve(static_cast<crd::usize>(N) + 4U);
    for (crd::u32 i = 0; i < N; ++i) { aug_pts.push_back(points[i]); }
    crd::math::Vec3<T> s0{};
    crd::math::Vec3<T> s1{};
    crd::math::Vec3<T> s2{};
    crd::math::Vec3<T> s3{};
    detail3d::build_super_tet(points, s0, s1, s2, s3);

    // D94: verify super-tet is positively oriented.
    {
        const T super_orient = crd::geometry::primitives::orient3d(s0, s1, s2, s3);
        CRD_ASSERT(super_orient > static_cast<T>(0));
        if (super_orient <= static_cast<T>(0))
        {
            result.status = DelaunayStatus3::InternalInvariant;
            return result;
        }
    }

    const crd::u32 sv0 = N;
    const crd::u32 sv1 = N + 1U;
    const crd::u32 sv2 = N + 2U;
    const crd::u32 sv3 = N + 3U;
    aug_pts.push_back(s0);
    aug_pts.push_back(s1);
    aug_pts.push_back(s2);
    aug_pts.push_back(s3);

    // Initial triangulation = single super-tet.
    TetPool        pool{alloc};
    const crd::u32 super_tet = pool.alloc_tet();
    pool[super_tet].v[0]   = sv0;
    pool[super_tet].v[1]   = sv1;
    pool[super_tet].v[2]   = sv2;
    pool[super_tet].v[3]   = sv3;
    pool[super_tet].nbr[0] = k_null_tet;
    pool[super_tet].nbr[1] = k_null_tet;
    pool[super_tet].nbr[2] = k_null_tet;
    pool[super_tet].nbr[3] = k_null_tet;

    // Insert each input point in lex-sorted order.
    crd::u32       hint     = super_tet;
    const crd::u32 max_walk = N * 8U + 32U;
    for (crd::u32 oi = 0; oi < N; ++oi)
    {
        const crd::u32 q_idx = order[oi];
        const auto&    q_pos = points[q_idx];
        const crd::u32 ct = detail3d::locate_tet(pool, aug_pts, hint, q_pos, max_walk);
        if (ct == k_null_tet)
        {
            result.status = DelaunayStatus3::InternalInvariant;
            return result;
        }
        bool invariant_failure = false;
        const crd::u32 seed = detail3d::insert_point_3d(pool, aug_pts, q_idx, q_pos, ct,
                                                          alloc, result.cavity_max_size,
                                                          invariant_failure);
        if (invariant_failure || seed == k_null_tet)
        {
            result.status = DelaunayStatus3::InternalInvariant;
            return result;
        }
        hint = seed;
    }

    // Strip super-tet vertices: emit any alive tet whose 4 vertices are all
    // < N (i.e., none is a super-tet vertex).
    result.tet_indices.reserve(static_cast<crd::usize>(N) * 28U); // ~7N tets * 4 indices
    for (crd::u32 ti = 0; ti < pool.pool_size(); ++ti)
    {
        if (!pool.alive(ti)) { continue; }
        const Tet& t = pool[ti];
        if (t.v[0] >= N || t.v[1] >= N || t.v[2] >= N || t.v[3] >= N)
        {
            ++result.super_tet_stripped;
            continue;
        }
        result.tet_indices.push_back(t.v[0]);
        result.tet_indices.push_back(t.v[1]);
        result.tet_indices.push_back(t.v[2]);
        result.tet_indices.push_back(t.v[3]);
        ++result.tet_count;
    }

    result.status = DelaunayStatus3::Ok;
    return result;
}

// Explicit instantiations.
template DelaunayResult3<crd::f32>
delaunay_3d<crd::f32>(crd::containers::ConstSpan<crd::math::Vec3<crd::f32>>,
                       crd::memory::IAllocator*);
template DelaunayResult3<crd::f64>
delaunay_3d<crd::f64>(crd::containers::ConstSpan<crd::math::Vec3<crd::f64>>,
                       crd::memory::IAllocator*);

} // namespace crd::geometry::delaunay
