// ---------------------------------------------------------------------------
// crd-geometry-mesh-processing — v7d Botsch-Kobbelt 2004 isotropic remeshing.
//
// See header for the algorithm contract. This TU contains:
//   - 4-pass-per-iteration loop (split / collapse / flip / smooth).
//   - Per-pass canonical-HE snapshot (Botsch-Kobbelt iteration discipline).
//   - Jacobi-style smoothing with input-BVH surface projection.
//   - f32/f64 explicit instantiations (BVH stays f32; query boundary casts).
//
// **Pinned design decisions** (carried for ADR-0076 §22 amendment at
// v7-close):
//
//   D26. **Module dep extension.** v7d adds PUBLIC deps on
//        `crd-geometry-mesh` (TriangleMeshView + TriangleMeshBvh +
//        mesh_closest_point) and TRANSITIVELY on `crd-geometry-bvh`. The
//        mesh-processing module now has two distinct "geometry backends"
//        consumed: -primitives (v7a-v7c) + -mesh+-bvh (v7d+).
//
//   D27. **Input untouched + clone-via-indexed.** Same pattern as v7b/v7c:
//        extract input to (positions, indices), build_from on requested
//        allocator into a fresh `output`, mutate `output` through the
//        iteration loop, return `output`. Input mesh + caller's
//        IAllocator handle are read-only.
//
//   D28. **Operate IN-PLACE on output during iterations.** Unlike v7c
//        (which rebuilds the mesh per level), v7d mutates `output`
//        through HalfEdgeMesh::{split_edge, collapse_edge, flip_edge,
//        set_vertex_position}. Avoids O(V) rebuilds per iteration.
//        `set_vertex_position` added to HalfEdgeMesh in this slice.
//
//   D29. **Per-pass canonical-HE snapshot.** Each of the 4 passes starts
//        by collecting the current canonical-HE list (`min(h, h.twin)`
//        per edge). The pass iterates the SNAPSHOT — new HEs created by
//        edits within the pass are NOT re-processed in the same pass.
//        Convergence requires this: a freshly-split edge is at exactly
//        the target length, so re-checking it would be wasted work.
//
//   D30. **Collapse safety pre-check.** Botsch-Kobbelt §4: collapsing
//        edge (a,b) at midpoint is rejected if ANY resulting edge (from
//        merged_vertex to a's or b's surviving neighbours) would EXCEED
//        the split threshold (4/3·L). Without this guard, the next pass
//        immediately re-splits the over-long edge — convergence stalls.
//        Predict via squared-distance from midpoint to each neighbour.
//
//   D31. **Flip target valences:** 6 for interior vertex (a hexagonal
//        lattice is the optimal-fairness 2-manifold tessellation), 4 for
//        boundary vertex (Botsch-Kobbelt §4; matches v7c boundary mask).
//
//   D32. **Smoothing = Jacobi.** New positions for all alive vertices are
//        computed against OLD positions, then applied atomically via
//        `set_vertex_position`. Independent of slot-iteration order;
//        deterministic; matches the "simultaneous update" reading of
//        Botsch-Kobbelt §4 step 4.
//
//   D33. **Input-mesh BVH** is f32 (TriangleMeshBvh is f32-only in
//        crd-geometry-mesh). For T=f64, positions are cast to f32 at BVH
//        build time and at each closest-point query boundary. Precision
//        loss bounded by `f32` ulp × surface scale — negligible against
//        the smoothing offset for any realistic mesh.
//
//   D34. **Boundary fixed by default.** `keep_boundary_fixed = true` skips
//        boundary vertices in pass 4. The cubic-B-spline boundary mask
//        from v7c could be added; deferred to v7d follow-on if needed.
//
//   D35. **Boundary edges skipped in split/collapse.** v7a's
//        `split_edge` / `collapse_edge` reject boundary edges (return
//        k_null / false). Pass 1 + 2 honour this; boundary edges retain
//        their original length. Pass 3 (flip) also skips boundary.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/bvh/bvh_build.hpp>
#include <crd/geometry/mesh/mesh_bvh.hpp>
#include <crd/geometry/mesh/mesh_closest_point.hpp>
#include <crd/geometry/mesh/triangle_mesh.hpp>
#include <crd/geometry/mesh_processing/half_edge_mesh.hpp>
#include <crd/geometry/mesh_processing/isotropic_remesh.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::mesh_processing
{
namespace
{

inline crd::u32 canonical_he_id(crd::u32 h, crd::u32 twin) noexcept
{
    if (twin == k_null_he) { return h; }
    return (h < twin) ? h : twin;
}

template <crd::math::MathScalar T>
inline T edge_length_squared(const HalfEdgeMesh<T>& m, crd::u32 h) noexcept
{
    const crd::u32 va = m.he(h).origin;
    const crd::u32 vb = m.he_dest(h);
    if (vb == k_null_vertex) { return T{0}; }
    const auto& pa = m.vertex(va).position;
    const auto& pb = m.vertex(vb).position;
    const T dx = pb.x - pa.x;
    const T dy = pb.y - pa.y;
    const T dz = pb.z - pa.z;
    return dx * dx + dy * dy + dz * dz;
}

template <crd::math::MathScalar T>
crd::containers::Array<crd::u32> snapshot_canonical_edges(const HalfEdgeMesh<T>&    m,
                                                           crd::memory::IAllocator* alloc)
{
    crd::containers::Array<crd::u32> snap(alloc);
    snap.reserve(m.he_pool_size() / 2U);
    for (crd::u32 h = 0; h < m.he_pool_size(); ++h)
    {
        if (!m.he_alive(h)) { continue; }
        if (h != canonical_he_id(h, m.he(h).twin)) { continue; }
        snap.push_back(h);
    }
    return snap;
}

template <crd::math::MathScalar T>
bool is_boundary_vertex(const HalfEdgeMesh<T>& m, crd::u32 v) noexcept
{
    bool b = false;
    m.for_each_outgoing_he(v, [&](crd::u32 ho) {
        if (m.he_is_boundary(ho)) { b = true; return; }
        const crd::u32 t = m.he(ho).twin;
        if (t != k_null_he && m.he_is_boundary(t)) { b = true; }
    });
    return b;
}

template <crd::math::MathScalar T>
crd::u32 vertex_valence(const HalfEdgeMesh<T>& m, crd::u32 v) noexcept
{
    crd::u32 n = 0;
    m.for_each_outgoing_he(v, [&](crd::u32) { ++n; });
    return n;
}

template <crd::math::MathScalar T>
crd::u32 vertex_target_valence(const HalfEdgeMesh<T>& m, crd::u32 v) noexcept
{
    return is_boundary_vertex(m, v) ? 4U : 6U;
}

template <crd::math::MathScalar T>
crd::u32 split_long_edges(HalfEdgeMesh<T>& m, T length_high_sq)
{
    auto snap = snapshot_canonical_edges(m, m.allocator());
    crd::u32 count = 0;
    for (crd::u32 i = 0; i < snap.size(); ++i)
    {
        const crd::u32 h = snap[i];
        if (!m.he_alive(h)) { continue; }
        const crd::u32 t = m.he(h).twin;
        // v7a's split_edge rejects boundary edges (returns k_null_vertex).
        if (m.he_is_boundary(h) || t == k_null_he || m.he_is_boundary(t)) { continue; }
        const T len_sq = edge_length_squared(m, h);
        if (len_sq <= length_high_sq) { continue; }
        const auto& pa = m.vertex(m.he(h).origin).position;
        const auto& pb = m.vertex(m.he_dest(h)).position;
        const crd::math::Vec3<T> mid{
            (pa.x + pb.x) / T{2},
            (pa.y + pb.y) / T{2},
            (pa.z + pb.z) / T{2},
        };
        const crd::u32 new_v = m.split_edge(h, mid);
        if (new_v != k_null_vertex) { ++count; }
    }
    return count;
}

template <crd::math::MathScalar T>
crd::u32 collapse_short_edges(HalfEdgeMesh<T>& m, T length_low_sq, T length_high_sq)
{
    auto snap = snapshot_canonical_edges(m, m.allocator());
    crd::u32 count = 0;
    for (crd::u32 i = 0; i < snap.size(); ++i)
    {
        const crd::u32 h = snap[i];
        if (!m.he_alive(h)) { continue; }
        const crd::u32 t = m.he(h).twin;
        // v7a's collapse_edge rejects boundary-side collapses.
        if (m.he_is_boundary(h) || t == k_null_he || m.he_is_boundary(t)) { continue; }
        const T len_sq = edge_length_squared(m, h);
        if (len_sq >= length_low_sq) { continue; }

        const crd::u32 va = m.he(h).origin;
        const crd::u32 vb = m.he_dest(h);
        const auto&    pa = m.vertex(va).position;
        const auto&    pb = m.vertex(vb).position;
        const crd::math::Vec3<T> midpoint{
            (pa.x + pb.x) / T{2},
            (pa.y + pb.y) / T{2},
            (pa.z + pb.z) / T{2},
        };

        // Post-collapse safety pre-check: any neighbour of va or vb whose
        // post-collapse edge length would exceed split threshold → reject.
        bool safe = true;
        auto check_neighbours = [&](crd::u32 endpoint) {
            m.for_each_outgoing_he(endpoint, [&](crd::u32 ho) {
                if (!safe) { return; }
                const crd::u32 dest = m.he_dest(ho);
                if (dest == k_null_vertex || dest == va || dest == vb) { return; }
                const auto& pd = m.vertex(dest).position;
                const T ddx = pd.x - midpoint.x;
                const T ddy = pd.y - midpoint.y;
                const T ddz = pd.z - midpoint.z;
                if (ddx * ddx + ddy * ddy + ddz * ddz > length_high_sq) { safe = false; }
            });
        };
        check_neighbours(va);
        if (safe) { check_neighbours(vb); }
        if (!safe) { continue; }

        if (m.collapse_edge(h, midpoint)) { ++count; }
    }
    return count;
}

// Returns true if vertex `u` has an outgoing HE pointing at `w` (i.e., the
// undirected edge (u, w) already exists in the current mesh).
template <crd::math::MathScalar T>
bool vertices_connected(const HalfEdgeMesh<T>& m, crd::u32 u, crd::u32 w) noexcept
{
    bool found = false;
    m.for_each_outgoing_he(u, [&](crd::u32 ho) {
        if (found) { return; }
        if (m.he_dest(ho) == w) { found = true; }
    });
    return found;
}

template <crd::math::MathScalar T>
crd::u32 flip_to_equalize_valence(HalfEdgeMesh<T>& m)
{
    auto snap = snapshot_canonical_edges(m, m.allocator());
    crd::u32 count = 0;
    for (crd::u32 i = 0; i < snap.size(); ++i)
    {
        const crd::u32 h = snap[i];
        if (!m.he_alive(h)) { continue; }
        const crd::u32 t = m.he(h).twin;
        if (m.he_is_boundary(h) || t == k_null_he || m.he_is_boundary(t)) { continue; }

        const crd::u32 va = m.he(h).origin;
        const crd::u32 vb = m.he_dest(h);
        const crd::u32 vc = m.he(m.he_prev(h)).origin; // apex of h's face
        const crd::u32 vd = m.he(m.he_prev(t)).origin; // apex of t's face

        // Manifold-preservation gate: if (c, d) is ALREADY an edge in the
        // mesh, flipping creates a duplicate edge (c, d has 2 incident
        // edges, hence ≥4 incident faces post-flip → non-manifold). v7a's
        // flip_edge doesn't check this case; we must gate here.
        if (vertices_connected(m, vc, vd)) { continue; }

        const crd::u32 val_a = vertex_valence(m, va);
        const crd::u32 val_b = vertex_valence(m, vb);
        const crd::u32 val_c = vertex_valence(m, vc);
        const crd::u32 val_d = vertex_valence(m, vd);
        const crd::u32 tgt_a = vertex_target_valence(m, va);
        const crd::u32 tgt_b = vertex_target_valence(m, vb);
        const crd::u32 tgt_c = vertex_target_valence(m, vc);
        const crd::u32 tgt_d = vertex_target_valence(m, vd);

        auto absdiff = [](crd::u32 x, crd::u32 y) noexcept { return x > y ? x - y : y - x; };
        const crd::u32 dev_before = absdiff(val_a, tgt_a) + absdiff(val_b, tgt_b)
                                    + absdiff(val_c, tgt_c) + absdiff(val_d, tgt_d);
        // After flip: edge endpoints va, vb lose 1 valence each (the edge
        // they shared is gone); apex vertices vc, vd gain 1 each (they're
        // the endpoints of the new edge).
        if (val_a == 0 || val_b == 0) { continue; }
        const crd::u32 dev_after = absdiff(val_a - 1U, tgt_a) + absdiff(val_b - 1U, tgt_b)
                                   + absdiff(val_c + 1U, tgt_c) + absdiff(val_d + 1U, tgt_d);
        if (dev_after >= dev_before) { continue; }
        if (m.flip_edge(h)) { ++count; }
    }
    return count;
}

// Compute area-weighted vertex normal (sum of un-normalised face cross
// products = 2·face_area·face_normal; the sum gives an area-weighted
// average naturally). Returns the zero vector for degenerate cases.
template <crd::math::MathScalar T>
crd::math::Vec3<T> compute_vertex_normal(const HalfEdgeMesh<T>& m, crd::u32 v) noexcept
{
    crd::math::Vec3<T> sum{T{0}, T{0}, T{0}};
    m.for_each_outgoing_he(v, [&](crd::u32 ho) {
        const crd::u32 f = m.he(ho).face;
        if (f == k_null_face) { return; }
        const crd::u32 h0 = m.face(f).first_he;
        const crd::u32 h1 = m.he(h0).next;
        const crd::u32 h2 = m.he(h1).next;
        const auto&    p0 = m.vertex(m.he(h0).origin).position;
        const auto&    p1 = m.vertex(m.he(h1).origin).position;
        const auto&    p2 = m.vertex(m.he(h2).origin).position;
        const auto     n  = crd::math::cross(p1 - p0, p2 - p0);
        sum = sum + n;
    });
    const T len = crd::math::length(sum);
    if (len < static_cast<T>(1e-20)) { return crd::math::Vec3<T>{T{0}, T{0}, T{0}}; }
    return sum * (T{1} / len);
}

// Inversion check: returns true iff moving vertex v to new_p would NOT
// flip the orientation of any incident face. The smoothing pass uses this
// to reject moves that would introduce self-intersection (Botsch-Kobbelt
// 2004 §4 triangle-quality safeguard).
template <crd::math::MathScalar T>
bool smoothing_no_inversion(const HalfEdgeMesh<T>&    m,
                             crd::u32                  v,
                             const crd::math::Vec3<T>& new_p) noexcept
{
    bool ok = true;
    m.for_each_outgoing_he(v, [&](crd::u32 ho) {
        if (!ok) { return; }
        const crd::u32 f = m.he(ho).face;
        if (f == k_null_face) { return; }
        const crd::u32 h0 = m.face(f).first_he;
        const crd::u32 h1 = m.he(h0).next;
        const crd::u32 h2 = m.he(h1).next;
        const crd::u32 v0 = m.he(h0).origin;
        const crd::u32 v1 = m.he(h1).origin;
        const crd::u32 v2 = m.he(h2).origin;
        const auto&    p0 = m.vertex(v0).position;
        const auto&    p1 = m.vertex(v1).position;
        const auto&    p2 = m.vertex(v2).position;
        const auto     n_old = crd::math::cross(p1 - p0, p2 - p0);
        const auto p0_new = (v0 == v) ? new_p : p0;
        const auto p1_new = (v1 == v) ? new_p : p1;
        const auto p2_new = (v2 == v) ? new_p : p2;
        const auto n_new = crd::math::cross(p1_new - p0_new, p2_new - p0_new);
        if (crd::math::dot(n_old, n_new) <= T{0}) { ok = false; }
    });
    return ok;
}

template <crd::math::MathScalar T>
crd::u32 tangential_smooth_pass(HalfEdgeMesh<T>&                      m,
                                 const crd::geometry::mesh::TriangleMeshViewf& input_view,
                                 const crd::geometry::mesh::TriangleMeshBvh&    input_bvh,
                                 const IsotropicRemeshOptions<T>&      opts)
{
    // Jacobi: compute all new positions against OLD; apply atomically.
    crd::containers::Array<crd::math::Vec3<T>> new_positions(m.allocator());
    new_positions.resize(m.vertex_pool_size(), crd::math::Vec3<T>{T{0}, T{0}, T{0}});
    crd::containers::Array<crd::u8> updated(m.allocator());
    updated.resize(m.vertex_pool_size(), crd::u8{0});

    crd::u32 count = 0;
    for (crd::u32 v = 0; v < m.vertex_pool_size(); ++v)
    {
        if (!m.vertex_alive(v)) { continue; }
        const auto& p = m.vertex(v).position;

        if (opts.keep_boundary_fixed && is_boundary_vertex(m, v))
        {
            new_positions[v] = p;
            updated[v]       = 1U;
            continue;
        }

        // Compute one-ring centroid (arithmetic mean of neighbour positions).
        crd::math::Vec3<T> sum{T{0}, T{0}, T{0}};
        crd::u32           n = 0;
        m.for_each_outgoing_he(v, [&](crd::u32 ho) {
            const crd::u32 dest = m.he_dest(ho);
            if (dest == k_null_vertex) { return; }
            sum = sum + m.vertex(dest).position;
            ++n;
        });

        if (n == 0U)
        {
            new_positions[v] = p;
            updated[v]       = 1U;
            continue;
        }

        const T inv_n    = T{1} / static_cast<T>(n);
        const crd::math::Vec3<T> centroid{sum.x * inv_n, sum.y * inv_n, sum.z * inv_n};

        // Botsch-Kobbelt 2004 §4 TANGENTIAL smoothing: restrict displacement
        // to the tangent plane at v. The component along the vertex normal
        // is what would push v away from the original surface; the tangent
        // component slides v across the surface (which is what we want for
        // isotropy). The follow-on BVH projection then snaps any small
        // tangent-plane drift back onto the input surface.
        const auto displacement_full = centroid - p;
        const auto vertex_normal     = compute_vertex_normal(m, v);
        const T    d_dot_n = displacement_full.x * vertex_normal.x
                             + displacement_full.y * vertex_normal.y
                             + displacement_full.z * vertex_normal.z;
        const crd::math::Vec3<T> displacement_tan{
            displacement_full.x - vertex_normal.x * d_dot_n,
            displacement_full.y - vertex_normal.y * d_dot_n,
            displacement_full.z - vertex_normal.z * d_dot_n,
        };
        crd::math::Vec3<T> new_p{
            p.x + opts.smoothing_lambda * displacement_tan.x,
            p.y + opts.smoothing_lambda * displacement_tan.y,
            p.z + opts.smoothing_lambda * displacement_tan.z,
        };

        // Surface projection (Botsch-Kobbelt shape-preservation).
        if (opts.project_to_input && !input_bvh.is_empty())
        {
            const crd::math::Vec3<crd::f32> q{
                static_cast<crd::f32>(new_p.x),
                static_cast<crd::f32>(new_p.y),
                static_cast<crd::f32>(new_p.z),
            };
            const auto hit = crd::geometry::mesh::mesh_closest_point(input_view, input_bvh, q);
            if (hit)
            {
                new_p = crd::math::Vec3<T>{
                    static_cast<T>(hit->point.x),
                    static_cast<T>(hit->point.y),
                    static_cast<T>(hit->point.z),
                };
            }
        }

        // Inversion-rejection safeguard (Botsch-Kobbelt §4 triangle-quality
        // protection): if moving v to new_p would flip any incident face,
        // keep v fixed. This prevents the silent topology corruption that
        // pure-Laplacian smoothing produces in dense regions.
        if (!smoothing_no_inversion(m, v, new_p))
        {
            new_positions[v] = p;
            updated[v]       = 1U;
            continue;
        }

        new_positions[v] = new_p;
        updated[v]       = 1U;
        ++count;
    }

    // Apply all-at-once.
    for (crd::u32 v = 0; v < m.vertex_pool_size(); ++v)
    {
        if (updated[v] == 0U) { continue; }
        if (!m.vertex_alive(v)) { continue; }
        m.set_vertex_position(v, new_positions[v]);
    }
    return count;
}

} // anonymous namespace

template <crd::math::MathScalar T>
HalfEdgeMesh<T> isotropic_remesh(const HalfEdgeMesh<T>&             input,
                                  const IsotropicRemeshOptions<T>&  opts,
                                  IsotropicRemeshReport*            out_report)
{
    IsotropicRemeshReport report{};
    auto                  report_out = [&] {
        if (out_report != nullptr) { *out_report = report; }
    };

    crd::memory::IAllocator* alloc = opts.output_allocator != nullptr
                                          ? opts.output_allocator
                                          : input.allocator();
    CRD_ASSERT(alloc != nullptr);

    if (opts.target_edge_length <= T{0})
    {
        report.status = IsotropicRemeshStatus::InvalidTargetLength;
        report_out();
        return HalfEdgeMesh<T>{alloc};
    }
    if (input.face_count() == 0U)
    {
        report.status = IsotropicRemeshStatus::EmptyMesh;
        report_out();
        return HalfEdgeMesh<T>{alloc};
    }
    if (!input.is_manifold())
    {
        report.status = IsotropicRemeshStatus::NonManifoldInput;
        report_out();
        return HalfEdgeMesh<T>{alloc};
    }

    // Clone input into a fresh output mesh on the requested allocator.
    HalfEdgeMesh<T>                            output{alloc};
    crd::containers::Array<crd::math::Vec3<T>> input_pos(alloc);
    crd::containers::Array<crd::u32>           input_idx(alloc);
    input.to_indexed(input_pos, input_idx);
    const auto build_status = output.build_from(
        crd::containers::ConstSpan<crd::math::Vec3<T>>{input_pos.data(), input_pos.size()},
        crd::containers::ConstSpan<crd::u32>{input_idx.data(), input_idx.size()});
    (void)build_status;

    // Build input BVH (f32) for surface projection.
    crd::containers::Array<crd::math::Vec3<crd::f32>> input_pos_f32(alloc);
    crd::geometry::mesh::TriangleMeshBvh              input_bvh{alloc};
    crd::geometry::mesh::TriangleMeshViewf            input_view{};
    if (opts.project_to_input)
    {
        input_pos_f32.reserve(input_pos.size());
        for (crd::usize k = 0; k < input_pos.size(); ++k)
        {
            const auto& p = input_pos[k];
            input_pos_f32.push_back(crd::math::Vec3<crd::f32>{
                static_cast<crd::f32>(p.x),
                static_cast<crd::f32>(p.y),
                static_cast<crd::f32>(p.z),
            });
        }
        input_view = crd::geometry::mesh::TriangleMeshViewf{
            crd::containers::ConstSpan<crd::math::Vec3<crd::f32>>{
                input_pos_f32.data(), input_pos_f32.size()},
            crd::containers::ConstSpan<crd::u32>{input_idx.data(), input_idx.size()},
        };
        input_bvh = crd::geometry::mesh::build_triangle_mesh_bvh(input_view, alloc);
    }

    const T length_high    = opts.split_factor * opts.target_edge_length;
    const T length_low     = opts.collapse_factor * opts.target_edge_length;
    const T length_high_sq = length_high * length_high;
    const T length_low_sq  = length_low * length_low;

    for (crd::u32 iter = 0; iter < opts.n_iterations; ++iter)
    {
        report.splits_applied    += split_long_edges(output, length_high_sq);
        report.collapses_applied += collapse_short_edges(output, length_low_sq, length_high_sq);
        report.flips_applied     += flip_to_equalize_valence(output);
        report.vertices_smoothed += tangential_smooth_pass(output, input_view, input_bvh, opts);
        ++report.iterations_run;
    }

    report.output_vertices = output.vertex_count();
    report.output_faces    = output.face_count();
    report_out();
    return output;
}

// Explicit instantiations.
template HalfEdgeMesh<crd::f32> isotropic_remesh<crd::f32>(const HalfEdgeMesh<crd::f32>&,
                                                            const IsotropicRemeshOptions<crd::f32>&,
                                                            IsotropicRemeshReport*);
template HalfEdgeMesh<crd::f64> isotropic_remesh<crd::f64>(const HalfEdgeMesh<crd::f64>&,
                                                            const IsotropicRemeshOptions<crd::f64>&,
                                                            IsotropicRemeshReport*);

} // namespace crd::geometry::mesh_processing
