// crd-geometry-mesh — mesh validation impl (v4-validate).

#include <crd/geometry/mesh/mesh_validate.hpp>

#include <crd/core/assert.hpp>
#include <crd/math/vec.hpp>

#include <algorithm>

namespace crd::geometry::mesh
{

using crd::math::Vec3;

namespace
{

// Edge record for the edge map. Canonical key is (min_v, max_v); we keep
// the original `dir` (0 = appears as (lo→hi) in triangle, 1 = (hi→lo))
// so the orientation check can spot two triangles using the edge in the
// SAME direction.
struct EdgeRec
{
    crd::u32 v_lo;
    crd::u32 v_hi;
    crd::u32 tri;
    crd::u8  dir; // 0 = (v_lo, v_hi) ordering in the triangle, 1 = (v_hi, v_lo)
};

inline bool edge_less(const EdgeRec& a, const EdgeRec& b) noexcept
{
    if (a.v_lo != b.v_lo) return a.v_lo < b.v_lo;
    if (a.v_hi != b.v_hi) return a.v_hi < b.v_hi;
    return a.tri < b.tri;
}

inline crd::f32 area_sq2(const Vec3<crd::f32>& v0, const Vec3<crd::f32>& v1,
                         const Vec3<crd::f32>& v2) noexcept
{
    // (2 * area)^2 = |edge1 × edge2|^2
    const Vec3<crd::f32> e1 = v1 - v0;
    const Vec3<crd::f32> e2 = v2 - v0;
    const Vec3<crd::f32> n{e1.y * e2.z - e1.z * e2.y,
                            e1.z * e2.x - e1.x * e2.z,
                            e1.x * e2.y - e1.y * e2.x};
    return n.x * n.x + n.y * n.y + n.z * n.z;
}

} // namespace

MeshValidationReport
validate_triangle_mesh(const TriangleMeshViewf&        view,
                        crd::memory::IAllocator*        alloc,
                        const MeshValidationOptions&    opts)
{
    CRD_ASSERT(alloc != nullptr);
    MeshValidationReport report{alloc};
    report.triangle_count = view.triangle_count();
    report.vertex_count   = static_cast<crd::u32>(view.vertices.size());

    bool critical_defect = false;

    // ── Pass 1: triangle-level checks (bounds, degenerate, area-zero). ──
    // (2·area)^2 threshold = (2·area_eps)^2 = 4·area_eps^2 — cheaper to
    // compare squared values + drop the sqrt.
    const crd::f32 area_thresh_sq =
        4.0F * opts.area_epsilon.value * opts.area_epsilon.value;
    const crd::u32 vcount = report.vertex_count;
    for (crd::u32 ti = 0U; ti < report.triangle_count; ++ti)
    {
        const crd::u32 i0 = view.indices[ti * 3U + 0U];
        const crd::u32 i1 = view.indices[ti * 3U + 1U];
        const crd::u32 i2 = view.indices[ti * 3U + 2U];
        if (i0 >= vcount || i1 >= vcount || i2 >= vcount)
        {
            crd::u32 bad = i2;
            if (i0 >= vcount)      { bad = i0; }
            else if (i1 >= vcount) { bad = i1; }
            report.defects.push_back(MeshDefect{MeshDefectKind::OutOfBoundsIndex, ti, bad});
            critical_defect = true;
            continue; // skip the rest of this tri's checks — its vertex reads are unsafe
        }
        if (i0 == i1 || i1 == i2 || i2 == i0)
        {
            report.defects.push_back(MeshDefect{MeshDefectKind::DegenerateTriangle, ti, 0xFFFFFFFFU});
            critical_defect = true;
            continue;
        }
        if (area_sq2(view.vertices[i0], view.vertices[i1], view.vertices[i2]) < area_thresh_sq)
        {
            report.defects.push_back(MeshDefect{MeshDefectKind::ZeroAreaTriangle, ti, 0xFFFFFFFFU});
            // Area-zero is an authoring smell, not a critical-defect.
        }
    }

    if (!opts.check_edges)
    {
        report.well_formed = !critical_defect;
        report.watertight  = false; // unknown without the edge pass
        return report;
    }

    // ── Pass 2: build the edge table, sorted by canonical (v_lo, v_hi). ──
    crd::containers::Array<EdgeRec> edges{alloc};
    edges.reserve(static_cast<crd::usize>(report.triangle_count) * 3U);
    for (crd::u32 ti = 0U; ti < report.triangle_count; ++ti)
    {
        const crd::u32 i0 = view.indices[ti * 3U + 0U];
        const crd::u32 i1 = view.indices[ti * 3U + 1U];
        const crd::u32 i2 = view.indices[ti * 3U + 2U];
        if (i0 >= vcount || i1 >= vcount || i2 >= vcount) { continue; }
        if (i0 == i1 || i1 == i2 || i2 == i0)            { continue; }
        const crd::u32 e[3][2] = {{i0, i1}, {i1, i2}, {i2, i0}};
        for (int k = 0; k < 3; ++k)
        {
            const crd::u32 a = e[k][0];
            const crd::u32 b = e[k][1];
            const crd::u32 lo = a < b ? a : b;
            const crd::u32 hi = a < b ? b : a;
            const crd::u8  dir = static_cast<crd::u8>(a < b ? 0U : 1U);
            edges.push_back(EdgeRec{lo, hi, ti, dir});
        }
    }
    std::sort(edges.begin(), edges.end(), edge_less);

    // ── Pass 3: scan the sorted edge list — classify each canonical edge. ──
    // Runs of identical (v_lo, v_hi) represent all triangles sharing that
    // undirected edge.
    crd::usize i = 0;
    while (i < edges.size())
    {
        crd::usize j = i + 1;
        while (j < edges.size() && edges[j].v_lo == edges[i].v_lo && edges[j].v_hi == edges[i].v_hi)
        {
            ++j;
        }
        const crd::usize count = j - i;
        if (count == 1)
        {
            ++report.boundary_edge_count;
            if (opts.report_boundary_edges)
            {
                report.defects.push_back(MeshDefect{
                    MeshDefectKind::BoundaryEdge, edges[i].v_lo, edges[i].v_hi});
            }
        }
        else if (count == 2)
        {
            ++report.manifold_edge_count;
            if (opts.check_orientation)
            {
                // The two adjacent triangles must traverse the edge in
                // OPPOSITE directions — i.e. dir values differ. If they
                // match, orientation is inconsistent.
                if (edges[i].dir == edges[i + 1].dir)
                {
                    const crd::u32 t_lo = edges[i].tri < edges[i + 1].tri
                                              ? edges[i].tri : edges[i + 1].tri;
                    const crd::u32 t_hi = edges[i].tri < edges[i + 1].tri
                                              ? edges[i + 1].tri : edges[i].tri;
                    report.defects.push_back(MeshDefect{
                        MeshDefectKind::InconsistentOrientation, t_lo, t_hi});
                    critical_defect = true;
                }
            }
        }
        else
        {
            // count ≥ 3 — non-manifold edge ("fin"). Emit once per edge.
            ++report.non_manifold_edge_count;
            report.defects.push_back(MeshDefect{
                MeshDefectKind::NonManifoldEdge, edges[i].v_lo, edges[i].v_hi});
            critical_defect = true;
        }
        i = j;
    }

    report.well_formed = !critical_defect;
    // An empty mesh is vacuously well-formed but NOT a closed surface — watertight
    // requires at least one triangle. (Caller's "is this mesh suitable for CSG /
    // SDF flood-fill" decision should reject zero-triangle inputs anyway.)
    report.watertight  = report.well_formed
                          && report.boundary_edge_count == 0U
                          && report.triangle_count > 0U;
    return report;
}

} // namespace crd::geometry::mesh
