// ---------------------------------------------------------------------------
// crd-geometry-delaunay — v8h 3D Delaunay quality refinement implementation.
//
// See tet_refine_3d.hpp for algorithm contract. **Scope honest**: this is
// dihedral-bounded refinement (3D-Ruppert analog), NOT true sliver
// exudation (Cheng-Dey-Edelsbrunner-Facello-Teng 2000). Sliver exudation
// is a v8h-exude follow-on.
//
// Pinned design decisions D119-D122 (carryover for ADR-0076 §23 at v8-close):
//
//   D119. **Scope honest** — dihedral-bounded refinement only. True sliver
//         exudation deferred to v8h-exude follow-on (weighted Delaunay
//         with per-vertex weight perturbations, different machinery).
//
//   D120. **Six dihedrals per tet** enumerated explicitly via edge table
//         `{(0,1,2,3), (0,2,1,3), (0,3,1,2), (1,2,0,3), (1,3,0,2),
//         (2,3,0,1)}` where the tuple is `(i, j, k, l)` with edge (vi, vj)
//         and off-edge vertices (vk, vm). Dihedral via face-normal dot
//         product: `cos = dot(n1, n2) / (|n1| |n2|)` where
//         `n1 = (vj-vi) × (vk-vi)`, `n2 = (vj-vi) × (vm-vi)`. Calibration:
//         regular tet returns arccos(1/3) ≈ 70.5288°.
//
//   D121. **Out-of-domain skip** — bbox-bounded circumcentre check. If a
//         bad tet's circumcentre is outside `[bbox_min - pad, bbox_max +
//         pad]` (10% padding), skip and continue to next bad tet. Don't
//         extend the mesh past the input domain.
//
//   D122. **Bbox-scaled near-duplicate eps**: `eps² = (bbox_diag * 1e-6)²`.
//         Auto-scales with input coord magnitude. Absolute 1e-12 (used in
//         v8g 2D) is too tight for FEA-scale 3D inputs.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/sort.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/delaunay/delaunay_3d.hpp>
#include <crd/geometry/delaunay/tet_refine_3d.hpp>
#include <crd/geometry/primitives/circumcenter.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace crd::geometry::delaunay
{

namespace
{

inline TetRefineStatus propagate_delaunay_status(DelaunayStatus3 s) noexcept
{
    switch (s)
    {
        case DelaunayStatus3::Ok:                return TetRefineStatus::Ok;
        case DelaunayStatus3::TooFewPoints:      return TetRefineStatus::TooFewPoints;
        case DelaunayStatus3::NonFiniteInput:    return TetRefineStatus::NonFiniteInput;
        case DelaunayStatus3::DuplicatePoint:    return TetRefineStatus::DuplicatePoint;
        case DelaunayStatus3::Coplanar:          return TetRefineStatus::Coplanar;
        case DelaunayStatus3::InternalInvariant: return TetRefineStatus::InternalInvariant;
    }
    return TetRefineStatus::InternalInvariant;
}

template <crd::math::MathScalar T>
inline bool is_finite_vec3(const crd::math::Vec3<T>& p) noexcept
{
    return std::isfinite(static_cast<double>(p.x))
        && std::isfinite(static_cast<double>(p.y))
        && std::isfinite(static_cast<double>(p.z));
}

// Six dihedral edge tuples (i, j, k, l). i,j = edge endpoints; k,l = off-
// edge vertices forming the two faces (vi, vj, vk) and (vi, vj, vm).
constexpr crd::u32 kEdgeTuples[6][4] = {
    {0U, 1U, 2U, 3U},
    {0U, 2U, 1U, 3U},
    {0U, 3U, 1U, 2U},
    {1U, 2U, 0U, 3U},
    {1U, 3U, 0U, 2U},
    {2U, 3U, 0U, 1U},
};

template <crd::math::MathScalar T>
inline crd::math::Vec3<T> cross3(const crd::math::Vec3<T>& a,
                                   const crd::math::Vec3<T>& b) noexcept
{
    return crd::math::Vec3<T>{
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

} // anonymous namespace

template <crd::math::MathScalar T>
T min_dihedral_of_tet_rad(const crd::math::Vec3<T>& v0,
                            const crd::math::Vec3<T>& v1,
                            const crd::math::Vec3<T>& v2,
                            const crd::math::Vec3<T>& v3) noexcept
{
    const crd::math::Vec3<T> verts[4] = {v0, v1, v2, v3};
    T min_rad = std::numeric_limits<T>::infinity();
    for (crd::u32 e = 0; e < 6U; ++e)
    {
        const auto& vi = verts[kEdgeTuples[e][0]];
        const auto& vj = verts[kEdgeTuples[e][1]];
        const auto& vk = verts[kEdgeTuples[e][2]];
        const auto& vm = verts[kEdgeTuples[e][3]];
        const crd::math::Vec3<T> ev{vj.x - vi.x, vj.y - vi.y, vj.z - vi.z};
        const crd::math::Vec3<T> ak{vk.x - vi.x, vk.y - vi.y, vk.z - vi.z};
        const crd::math::Vec3<T> am{vm.x - vi.x, vm.y - vi.y, vm.z - vi.z};
        const crd::math::Vec3<T> n1 = cross3(ev, ak);
        const crd::math::Vec3<T> n2 = cross3(ev, am);
        const T n1_len2 = n1.x * n1.x + n1.y * n1.y + n1.z * n1.z;
        const T n2_len2 = n2.x * n2.x + n2.y * n2.y + n2.z * n2.z;
        if (n1_len2 <= static_cast<T>(0) || n2_len2 <= static_cast<T>(0))
        {
            return static_cast<T>(0); // degenerate
        }
        T cos_d = (n1.x * n2.x + n1.y * n2.y + n1.z * n2.z)
                / std::sqrt(n1_len2 * n2_len2);
        if (cos_d > static_cast<T>(1))  { cos_d = static_cast<T>(1); }
        if (cos_d < static_cast<T>(-1)) { cos_d = static_cast<T>(-1); }
        const T d = std::acos(cos_d);
        if (d < min_rad) { min_rad = d; }
    }
    return min_rad;
}

template <crd::math::MathScalar T>
TetRefineResult<T>
tet_refine_3d(crd::containers::ConstSpan<crd::math::Vec3<T>> points,
               const TetRefineOptions<T>&                      opts,
               crd::memory::IAllocator*                        alloc)
{
    TetRefineResult<T> result{alloc};
    const crd::u32 n_in = static_cast<crd::u32>(points.size());

    if (n_in < 4U) { result.status = TetRefineStatus::TooFewPoints; return result; }
    for (crd::u32 i = 0; i < n_in; ++i)
    {
        if (!is_finite_vec3(points[i]))
        {
            result.status = TetRefineStatus::NonFiniteInput;
            return result;
        }
    }
    if (opts.min_dihedral_degrees <= static_cast<T>(0)
        || opts.min_dihedral_degrees > static_cast<T>(70.5))
    {
        result.status = TetRefineStatus::InvalidAngle;
        return result;
    }
    const T min_dihedral_rad = opts.min_dihedral_degrees * static_cast<T>(3.14159265358979323846 / 180.0);

    // Working vertex array. Initialise from input; Steiner points appended.
    crd::containers::Array<crd::math::Vec3<T>> vertices(alloc);
    vertices.reserve(n_in);
    for (crd::u32 i = 0; i < n_in; ++i) { vertices.push_back(points[i]); }

    // Compute bbox + padded bounds + bbox-scaled eps² (D121, D122).
    T xmin = vertices[0].x;
    T ymin = vertices[0].y;
    T zmin = vertices[0].z;
    T xmax = vertices[0].x;
    T ymax = vertices[0].y;
    T zmax = vertices[0].z;
    for (crd::u32 i = 1; i < n_in; ++i)
    {
        if (vertices[i].x < xmin) { xmin = vertices[i].x; }
        if (vertices[i].x > xmax) { xmax = vertices[i].x; }
        if (vertices[i].y < ymin) { ymin = vertices[i].y; }
        if (vertices[i].y > ymax) { ymax = vertices[i].y; }
        if (vertices[i].z < zmin) { zmin = vertices[i].z; }
        if (vertices[i].z > zmax) { zmax = vertices[i].z; }
    }
    const T dx = xmax - xmin;
    const T dy = ymax - ymin;
    const T dz = zmax - zmin;
    const T diag = std::sqrt(dx * dx + dy * dy + dz * dz);
    T pad = diag * static_cast<T>(0.1);
    if (pad <= static_cast<T>(0)) { pad = static_cast<T>(0.1); }
    const T xlo = xmin - pad;
    const T ylo = ymin - pad;
    const T zlo = zmin - pad;
    const T xhi = xmax + pad;
    const T yhi = ymax + pad;
    const T zhi = zmax + pad;
    T dup_eps = diag * static_cast<T>(1e-6);
    if (dup_eps <= static_cast<T>(0)) { dup_eps = static_cast<T>(1e-9); }
    const T eps_sq = dup_eps * dup_eps;

    // Initial Delaunay.
    auto del = delaunay_3d<T>(
        crd::containers::ConstSpan<crd::math::Vec3<T>>{vertices.data(), vertices.size()}, alloc);
    if (!del.ok())
    {
        result.status = propagate_delaunay_status(del.status);
        return result;
    }
    crd::containers::Array<crd::u32> tet_indices(alloc);
    tet_indices = std::move(del.tet_indices);
    crd::u32 tet_count = del.tet_count;

    // Refinement loop.
    for (crd::u32 iter = 0; iter < opts.max_iterations; ++iter)
    {
        result.iterations_run = iter + 1U;

        // Scan tets in order. For each bad tet, attempt circumcentre
        // insertion. First actionable one wins.
        bool inserted = false;
        bool any_bad = false;
        for (crd::u32 t = 0; t < tet_count; ++t)
        {
            const crd::u32 ia = tet_indices[4U * t + 0U];
            const crd::u32 ib = tet_indices[4U * t + 1U];
            const crd::u32 ic = tet_indices[4U * t + 2U];
            const crd::u32 id = tet_indices[4U * t + 3U];
            const T d = min_dihedral_of_tet_rad<T>(
                vertices[ia], vertices[ib], vertices[ic], vertices[id]);
            if (d >= min_dihedral_rad) { continue; }
            any_bad = true;

            // Compute circumcentre.
            const auto cc = crd::geometry::primitives::circumcenter_3d(
                vertices[ia], vertices[ib], vertices[ic], vertices[id]);

            // Domain check (D121): inside padded input bbox.
            if (!is_finite_vec3(cc))                  { continue; }
            if (cc.x < xlo || cc.x > xhi)             { continue; }
            if (cc.y < ylo || cc.y > yhi)             { continue; }
            if (cc.z < zlo || cc.z > zhi)             { continue; }

            // Near-duplicate check (D122).
            bool near_existing = false;
            for (const auto& v : vertices)
            {
                const T ddx = cc.x - v.x;
                const T ddy = cc.y - v.y;
                const T ddz = cc.z - v.z;
                if (ddx * ddx + ddy * ddy + ddz * ddz < eps_sq)
                {
                    near_existing = true;
                    break;
                }
            }
            if (near_existing) { continue; }

            // Actionable bad tet -- insert Steiner and re-Delaunay.
            vertices.push_back(cc);
            ++result.steiner_count;
            if (result.steiner_count > opts.max_steiner) { break; }

            auto del_it = delaunay_3d<T>(
                crd::containers::ConstSpan<crd::math::Vec3<T>>{vertices.data(), vertices.size()},
                alloc);
            if (!del_it.ok())
            {
                result.status = propagate_delaunay_status(del_it.status);
                return result;
            }
            tet_indices = std::move(del_it.tet_indices);
            tet_count   = del_it.tet_count;
            inserted    = true;
            break;
        }

        if (!any_bad)
        {
            result.converged = true;
            result.status    = TetRefineStatus::Ok;
            break;
        }
        if (!inserted)
        {
            // Bad tets exist but none actionable -- domain/duplicate guards
            // prevent progress. Halt as NotConverged.
            break;
        }
        if (result.steiner_count > opts.max_steiner) { break; }
    }

    if (!result.converged && result.status == TetRefineStatus::Ok)
    {
        result.status = TetRefineStatus::NotConverged;
    }
    result.vertices    = std::move(vertices);
    result.tet_indices = std::move(tet_indices);
    result.tet_count   = tet_count;
    return result;
}

// Explicit instantiations.
template crd::f32 min_dihedral_of_tet_rad<crd::f32>(const crd::math::Vec3<crd::f32>&,
                                                      const crd::math::Vec3<crd::f32>&,
                                                      const crd::math::Vec3<crd::f32>&,
                                                      const crd::math::Vec3<crd::f32>&) noexcept;
template crd::f64 min_dihedral_of_tet_rad<crd::f64>(const crd::math::Vec3<crd::f64>&,
                                                      const crd::math::Vec3<crd::f64>&,
                                                      const crd::math::Vec3<crd::f64>&,
                                                      const crd::math::Vec3<crd::f64>&) noexcept;
template TetRefineResult<crd::f32>
tet_refine_3d<crd::f32>(crd::containers::ConstSpan<crd::math::Vec3<crd::f32>>,
                          const TetRefineOptions<crd::f32>&, crd::memory::IAllocator*);
template TetRefineResult<crd::f64>
tet_refine_3d<crd::f64>(crd::containers::ConstSpan<crd::math::Vec3<crd::f64>>,
                          const TetRefineOptions<crd::f64>&, crd::memory::IAllocator*);

} // namespace crd::geometry::delaunay
