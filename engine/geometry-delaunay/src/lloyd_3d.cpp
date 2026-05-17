// ---------------------------------------------------------------------------
// crd-geometry-delaunay — v8e 3D Lloyd's CVT relaxation implementation.
//
// See lloyd_3d.hpp for the algorithm contract. This TU owns:
//   - Validation pass (TooFewPoints / NonFiniteInput / DuplicatePoint /
//     Coplanar / BboxInvalid / BboxClipNotSupported3D).
//   - Lloyd iteration loop: voronoi_3d -> per-cell centroid -> displacement
//     check.
//   - Polyhedron centroid via tet decomposition (sum of tet-centroid *
//     signed-volume / total-signed-volume).
//
// Pinned design decisions D107-D108 (carryover for ADR-0076 §23 at v8-close):
//
//   D107. **3D polyhedron centroid = sum of tet centroids weighted by
//         tet signed volume.** Decompose cell into tets `(ref, face_v[0],
//         face_v[i], face_v[i+1])` for each face's triangle fan, where
//         `ref` = cell's first Voronoi vertex (any consistent point
//         works; subtractive cancellation preserves signed-volume sum
//         independent of ref choice). Centroid = sum(C_i * V_i) /
//         sum(V_i).
//
//   D108. **3D ClipToBbox returns BboxClipNotSupported3D**. 3D halfspace
//         polyhedron clipping is a substantial subsystem (~200 LOC for
//         the half-plane clip + cell traversal). Deferred to a v8e-3d-clip
//         follow-on. `Fix` is the documented production-ready mode for
//         v8e. Users requiring closed-domain 3D CVT can pad with sentinel
//         sites at the bbox corners; Voronoi naturally extends the
//         interior cells inward.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/sort.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/delaunay/lloyd_3d.hpp>
#include <crd/geometry/delaunay/voronoi_3d.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>
#include <limits>

namespace crd::geometry::delaunay
{

namespace
{

template <crd::math::MathScalar T>
inline bool is_finite_vec3(const crd::math::Vec3<T>& p) noexcept
{
    return std::isfinite(static_cast<double>(p.x))
        && std::isfinite(static_cast<double>(p.y))
        && std::isfinite(static_cast<double>(p.z));
}

inline LloydStatus3 propagate_voronoi3_status(VoronoiStatus3 s) noexcept
{
    switch (s)
    {
        case VoronoiStatus3::Ok:                return LloydStatus3::Ok;
        case VoronoiStatus3::TooFewPoints:      return LloydStatus3::TooFewPoints;
        case VoronoiStatus3::NonFiniteInput:    return LloydStatus3::NonFiniteInput;
        case VoronoiStatus3::DuplicatePoint:    return LloydStatus3::DuplicatePoint;
        case VoronoiStatus3::Coplanar:          return LloydStatus3::Coplanar;
        case VoronoiStatus3::InternalInvariant: return LloydStatus3::InternalInvariant;
    }
    return LloydStatus3::InternalInvariant;
}

// Compute volume-weighted centroid of a bounded Voronoi cell via tet
// decomposition. ref = cell's first Voronoi vertex. For each face, build
// a triangle fan and decompose each triangle into a tet w/ ref. Sum each
// tet's centroid * signed volume. Final centroid = weighted-sum / total-vol.
template <crd::math::MathScalar T>
crd::math::Vec3<T> polyhedron_centroid_via_tets(const VoronoiResult3<T>& vor,
                                                  const VoronoiCell3<T>&  cell) noexcept
{
    if (cell.faces.size() == 0U)
    {
        return crd::math::Vec3<T>{}; // defensive
    }
    // Reference point: first Voronoi vertex of the first face. Any consistent
    // ref works since the algebraic identity sum(C_tet * V_tet) / sum(V_tet)
    // is ref-invariant for a closed polyhedron.
    const auto& ref = vor.voronoi_vertices[cell.faces[0].vertex_indices[0]];

    crd::math::Vec3<T> weighted_centroid_sum{};
    T total_signed_volume = static_cast<T>(0);

    for (const auto& face : cell.faces)
    {
        const crd::u32 nf = static_cast<crd::u32>(face.vertex_indices.size());
        if (nf < 3U) { continue; }
        const auto& v0 = vor.voronoi_vertices[face.vertex_indices[0]];
        for (crd::u32 i = 1U; i + 1U < nf; ++i)
        {
            const auto& vi = vor.voronoi_vertices[face.vertex_indices[i]];
            const auto& vj = vor.voronoi_vertices[face.vertex_indices[i + 1U]];
            // Tet = (ref, v0, vi, vj). Signed volume = (1/6) * det([v0-ref, vi-ref, vj-ref]).
            const T ax = v0.x - ref.x;
            const T ay = v0.y - ref.y;
            const T az = v0.z - ref.z;
            const T bx = vi.x - ref.x;
            const T by = vi.y - ref.y;
            const T bz = vi.z - ref.z;
            const T cx = vj.x - ref.x;
            const T cy = vj.y - ref.y;
            const T cz = vj.z - ref.z;
            const T det = ax * (by * cz - bz * cy)
                        - ay * (bx * cz - bz * cx)
                        + az * (bx * cy - by * cx);
            const T signed_vol = det / static_cast<T>(6);
            // Tet centroid = (ref + v0 + vi + vj) / 4.
            const T cnx = (ref.x + v0.x + vi.x + vj.x) * static_cast<T>(0.25);
            const T cny = (ref.y + v0.y + vi.y + vj.y) * static_cast<T>(0.25);
            const T cnz = (ref.z + v0.z + vi.z + vj.z) * static_cast<T>(0.25);
            weighted_centroid_sum.x += cnx * signed_vol;
            weighted_centroid_sum.y += cny * signed_vol;
            weighted_centroid_sum.z += cnz * signed_vol;
            total_signed_volume += signed_vol;
        }
    }

    if (total_signed_volume == static_cast<T>(0))
    {
        // Degenerate cell — fall back to ref point.
        return ref;
    }
    const T inv = static_cast<T>(1) / total_signed_volume;
    return crd::math::Vec3<T>{weighted_centroid_sum.x * inv,
                               weighted_centroid_sum.y * inv,
                               weighted_centroid_sum.z * inv};
}

} // anonymous namespace

template <crd::math::MathScalar T>
LloydResult3<T>
lloyd_relax_3d(crd::containers::ConstSpan<crd::math::Vec3<T>> sites,
                const LloydOptions3<T>&                         opts,
                crd::memory::IAllocator*                        alloc)
{
    LloydResult3<T> result{alloc};
    const crd::u32 n = static_cast<crd::u32>(sites.size());

    // Validate.
    if (n < 4U)
    {
        result.status = LloydStatus3::TooFewPoints;
        return result;
    }
    for (crd::u32 i = 0; i < n; ++i)
    {
        if (!is_finite_vec3(sites[i]))
        {
            result.status = LloydStatus3::NonFiniteInput;
            return result;
        }
    }
    // Duplicate detection (lex sort).
    {
        crd::containers::Array<crd::u32> order(alloc);
        order.resize(n, crd::u32{0});
        for (crd::u32 i = 0; i < n; ++i) { order[i] = i; }
        crd::containers::sort(order.data(), order.data() + order.size(),
                               [&](crd::u32 a, crd::u32 b) noexcept {
                                   const auto& pa = sites[a];
                                   const auto& pb = sites[b];
                                   if (pa.x != pb.x) { return pa.x < pb.x; }
                                   if (pa.y != pb.y) { return pa.y < pb.y; }
                                   if (pa.z != pb.z) { return pa.z < pb.z; }
                                   return a < b;
                               });
        for (crd::u32 i = 1; i < n; ++i)
        {
            const auto& pa = sites[order[i - 1U]];
            const auto& pb = sites[order[i]];
            if (pa.x == pb.x && pa.y == pb.y && pa.z == pb.z)
            {
                result.status = LloydStatus3::DuplicatePoint;
                return result;
            }
        }
    }

    if (opts.hull_policy == HullPolicy3::ClipToBbox)
    {
        // Bbox sanity check, then bail with NotSupported. (Validate before
        // bailing so a malformed bbox+request returns BboxInvalid not
        // NotSupported.)
        if (opts.bbox_set)
        {
            const auto& mn = opts.bbox_min;
            const auto& mx = opts.bbox_max;
            if (!(mn.x < mx.x) || !(mn.y < mx.y) || !(mn.z < mx.z))
            {
                result.status = LloydStatus3::BboxInvalid;
                return result;
            }
        }
        result.status = LloydStatus3::BboxClipNotSupported3D;
        return result;
    }

    // Initialise relaxed_sites from input.
    result.relaxed_sites.reserve(n);
    for (crd::u32 i = 0; i < n; ++i) { result.relaxed_sites.push_back(sites[i]); }

    // Iteration loop.
    crd::containers::Array<crd::math::Vec3<T>> new_sites(alloc);
    for (crd::u32 iter = 0; iter < opts.max_iterations; ++iter)
    {
        auto vor = voronoi_3d<T>(
            crd::containers::ConstSpan<crd::math::Vec3<T>>{
                result.relaxed_sites.data(), result.relaxed_sites.size()},
            alloc);
        if (!vor.ok())
        {
            result.status = propagate_voronoi3_status(vor.status);
            return result;
        }

        new_sites.clear();
        new_sites.reserve(n);
        T max_disp2 = static_cast<T>(0);
        for (crd::u32 s = 0; s < n; ++s)
        {
            const auto& cell = vor.cells[s];
            crd::math::Vec3<T> new_pos = result.relaxed_sites[s];
            if (cell.is_bounded)
            {
                new_pos = polyhedron_centroid_via_tets(vor, cell);
            }
            // Else (unbounded under Fix policy): keep site put.
            new_sites.push_back(new_pos);
            const T dx = new_pos.x - result.relaxed_sites[s].x;
            const T dy = new_pos.y - result.relaxed_sites[s].y;
            const T dz = new_pos.z - result.relaxed_sites[s].z;
            const T d2 = dx * dx + dy * dy + dz * dz;
            if (d2 > max_disp2) { max_disp2 = d2; }
        }

        for (crd::u32 i = 0; i < n; ++i)
        {
            result.relaxed_sites[i] = new_sites[i];
        }
        result.iterations_run = iter + 1U;
        result.final_max_displacement = std::sqrt(max_disp2);
        if (result.final_max_displacement < opts.tolerance)
        {
            result.converged = true;
            result.status = LloydStatus3::Ok;
            return result;
        }
    }

    result.status = LloydStatus3::NotConverged;
    return result;
}

// Explicit instantiations.
template LloydResult3<crd::f32>
lloyd_relax_3d<crd::f32>(crd::containers::ConstSpan<crd::math::Vec3<crd::f32>>,
                          const LloydOptions3<crd::f32>&, crd::memory::IAllocator*);
template LloydResult3<crd::f64>
lloyd_relax_3d<crd::f64>(crd::containers::ConstSpan<crd::math::Vec3<crd::f64>>,
                          const LloydOptions3<crd::f64>&, crd::memory::IAllocator*);

} // namespace crd::geometry::delaunay
