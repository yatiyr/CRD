// ---------------------------------------------------------------------------
// crd-geometry-mesh-processing — v7c Loop subdivision implementation.
//
// See header for the algorithm contract. This TU contains:
//   - Per-vertex update position (interior Loop mask + boundary B-spline mask)
//   - Per-edge midpoint position (interior 3/8-1/8 mask + boundary midpoint)
//   - One-level subdivision pass operating on indexed-form scratch buffers
//   - Multi-level loop with per-level half-edge mesh reconstruction
//
// **Pinned design decisions** (carried for ADR-0076 §22 amendment at
// v7-close):
//
//   D17. **Indexed-form pipeline.** Each level: extract input HalfEdgeMesh
//        to (positions, indices) → build temp half-edge mesh for topology
//        queries → emit new (positions, indices) → repeat. Final build_from
//        creates the output mesh. Avoids HalfEdgeMesh move-semantics
//        questions and keeps the work serial-deterministic.
//
//   D18. **Vertex numbering.** Output = [n_old old vertices remapped via
//        slot order] ++ [n_edges new midpoint vertices in canonical-HE
//        order]. Canonical HE = min(h, h.twin) per undirected edge.
//
//   D19. **Sub-triangle CCW emission.** Each face (v₀,v₁,v₂) → four
//        sub-triangles: corner@v₀ (v₀,m₀₁,m₂₀); corner@v₁ (v₁,m₁₂,m₀₁);
//        corner@v₂ (v₂,m₂₀,m₁₂); central (m₀₁,m₁₂,m₂₀). All CCW so
//        orientation propagates.
//
//   D20. **Interior edge midpoint** = 3/8·A + 3/8·B + 1/8·C + 1/8·D
//        (Loop 1987 §2.2; C, D = apex of the two incident faces).
//
//   D21. **Boundary edge midpoint** = (A + B)/2 (limit position on the
//        cubic-B-spline subdivision curve; Loop 1987 §2.3).
//
//   D22. **Interior vertex update** = (1 - n·β)·V + β·Σneighbours, with
//        β = (1/n)·(5/8 - (3/8 + 1/4·cos(2π/n))²). Uses
//        `crd::math::deterministic::cos` for bit-identical FP across
//        compilers and architectures (ADR-0063).
//
//   D23. **Boundary vertex update** = 3/4·V + 1/8·u_left + 1/8·u_right
//        (Loop 1987 §2.3; cubic-B-spline boundary curve mask).
//
//   D24. **Multi-level** = repeated application of single-level
//        subdivision. `n_levels = 0` returns a fresh clone of input
//        (via to_indexed → build_from on the requested allocator).
//
//   D25. **Boundary-neighbour detection.** For each outgoing HE at v,
//        the edge (v, dest) is boundary iff (he_is_boundary(ho) OR
//        he_is_boundary(twin(ho))). The CW fan walk from v7a guarantees
//        exactly two such hits for a 2-manifold boundary vertex.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/mesh_processing/half_edge_mesh.hpp>
#include <crd/geometry/mesh_processing/loop_subdivide.hpp>
#include <crd/math/deterministic.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::mesh_processing
{
namespace
{

template <crd::math::MathScalar T>
inline crd::math::Vec3<T> add(const crd::math::Vec3<T>& a, const crd::math::Vec3<T>& b) noexcept
{
    return crd::math::Vec3<T>{a.x + b.x, a.y + b.y, a.z + b.z};
}

template <crd::math::MathScalar T>
inline crd::math::Vec3<T> scale(const crd::math::Vec3<T>& a, T s) noexcept
{
    return crd::math::Vec3<T>{a.x * s, a.y * s, a.z * s};
}

inline crd::u32 canonical_he(crd::u32 h, crd::u32 twin) noexcept
{
    if (twin == k_null_he) { return h; }
    return (h < twin) ? h : twin;
}

template <crd::math::MathScalar T>
crd::math::Vec3<T> compute_edge_midpoint(const HalfEdgeMesh<T>& m, crd::u32 h)
{
    const crd::u32 va = m.he(h).origin;
    const crd::u32 vb = m.he_dest(h);
    const auto&    pa = m.vertex(va).position;
    const auto&    pb = m.vertex(vb).position;

    const bool     h_b = m.he_is_boundary(h);
    const crd::u32 t   = m.he(h).twin;
    const bool     t_b = (t == k_null_he) || m.he_is_boundary(t);
    if (h_b || t_b)
    {
        return scale(add(pa, pb), T{0.5});
    }

    // Interior: 3/8 A + 3/8 B + 1/8 C + 1/8 D.
    // C is the apex of h's face = h_prev(h).origin (triangle topology).
    // D is the apex of t's face = h_prev(t).origin.
    const crd::u32 vc = m.he(m.he_prev(h)).origin;
    const crd::u32 vd = m.he(m.he_prev(t)).origin;
    const auto&    pc = m.vertex(vc).position;
    const auto&    pd = m.vertex(vd).position;
    return add(scale(add(pa, pb), T{0.375}), scale(add(pc, pd), T{0.125}));
}

template <crd::math::MathScalar T>
crd::math::Vec3<T> compute_updated_vertex_position(const HalfEdgeMesh<T>& m, crd::u32 v)
{
    const auto& p = m.vertex(v).position;

    auto     sum_all      = crd::math::Vec3<T>{T{0}, T{0}, T{0}};
    auto     sum_boundary = crd::math::Vec3<T>{T{0}, T{0}, T{0}};
    crd::u32 valence              = 0;
    crd::u32 boundary_count       = 0;
    bool     is_boundary          = false;

    m.for_each_outgoing_he(v, [&](crd::u32 ho) {
        const crd::u32 dest = m.he_dest(ho);
        if (dest == k_null_vertex) { return; }
        const auto& pd = m.vertex(dest).position;
        sum_all = add(sum_all, pd);
        ++valence;

        const bool     ho_is_b = m.he_is_boundary(ho);
        const crd::u32 t       = m.he(ho).twin;
        const bool     t_is_b  = (t != k_null_he) && m.he_is_boundary(t);
        if (ho_is_b || t_is_b)
        {
            is_boundary = true;
            sum_boundary = add(sum_boundary, pd);
            ++boundary_count;
        }
    });

    if (is_boundary)
    {
        // Cubic-B-spline boundary curve mask: V' = 3/4 V + 1/8 sum.
        // For a well-formed 2-manifold boundary, boundary_count == 2.
        if (boundary_count == 2U)
        {
            return add(scale(p, T{0.75}), scale(sum_boundary, T{0.125}));
        }
        // Degenerate fallback: hold the vertex in place.
        return p;
    }

    if (valence == 0U) { return p; }

    // Loop interior weight.
    constexpr T two_pi = static_cast<T>(6.28318530717958647692);
    const T     n        = static_cast<T>(valence);
    const T     two_pi_n = two_pi / n;
    const T     cos_val     = crd::math::deterministic::cos(two_pi_n);
    const T     inner       = T{0.375} + T{0.25} * cos_val;     // 3/8 + 1/4·cos(2π/n)
    const T     beta        = (T{0.625} - inner * inner) / n;   // (5/8 - inner²)/n
    return add(scale(p, T{1} - n * beta), scale(sum_all, beta));
}

template <crd::math::MathScalar T>
void subdivide_one_level(const HalfEdgeMesh<T>&                       m,
                          crd::containers::Array<crd::math::Vec3<T>>& out_pos,
                          crd::containers::Array<crd::u32>&            out_idx)
{
    // Build slot → new-index map for old vertices.
    crd::containers::Array<crd::u32> slot_to_new(out_pos.allocator());
    slot_to_new.resize(m.vertex_pool_size(), k_null_vertex);
    crd::u32 next_idx = 0;
    for (crd::u32 v = 0; v < m.vertex_pool_size(); ++v)
    {
        if (!m.vertex_alive(v)) { continue; }
        slot_to_new[v] = next_idx++;
    }
    const crd::u32 n_old_verts = next_idx;

    // Emit updated old-vertex positions in slot order.
    out_pos.clear();
    out_pos.reserve(n_old_verts + m.he_pool_size() / 2U);
    for (crd::u32 v = 0; v < m.vertex_pool_size(); ++v)
    {
        if (!m.vertex_alive(v)) { continue; }
        out_pos.push_back(compute_updated_vertex_position(m, v));
    }

    // Emit midpoint vertices per CANONICAL edge in canonical-HE-id order.
    crd::containers::Array<crd::u32> canonical_to_mid(out_pos.allocator());
    canonical_to_mid.resize(m.he_pool_size(), k_null_vertex);
    for (crd::u32 h = 0; h < m.he_pool_size(); ++h)
    {
        if (!m.he_alive(h)) { continue; }
        const crd::u32 t = m.he(h).twin;
        const crd::u32 c = canonical_he(h, t);
        if (c != h) { continue; }              // process each canonical exactly once
        if (canonical_to_mid[c] != k_null_vertex) { continue; }
        const auto mid = compute_edge_midpoint(m, h);
        const crd::u32 mid_idx = static_cast<crd::u32>(out_pos.size());
        out_pos.push_back(mid);
        canonical_to_mid[c] = mid_idx;
    }

    // Emit 4 sub-triangles per face, preserving CCW.
    out_idx.clear();
    out_idx.reserve(m.face_count() * 12U);
    for (crd::u32 f = 0; f < m.face_pool_size(); ++f)
    {
        if (!m.face_alive(f)) { continue; }
        const crd::u32 h0 = m.face(f).first_he;
        const crd::u32 h1 = m.he(h0).next;
        const crd::u32 h2 = m.he(h1).next;
        const crd::u32 v0 = slot_to_new[m.he(h0).origin];
        const crd::u32 v1 = slot_to_new[m.he(h1).origin];
        const crd::u32 v2 = slot_to_new[m.he(h2).origin];

        const crd::u32 c01 = canonical_he(h0, m.he(h0).twin);
        const crd::u32 c12 = canonical_he(h1, m.he(h1).twin);
        const crd::u32 c20 = canonical_he(h2, m.he(h2).twin);
        const crd::u32 m01 = canonical_to_mid[c01];
        const crd::u32 m12 = canonical_to_mid[c12];
        const crd::u32 m20 = canonical_to_mid[c20];

        // Corner @ v₀: (v₀, m₀₁, m₂₀)
        out_idx.push_back(v0);  out_idx.push_back(m01); out_idx.push_back(m20);
        // Corner @ v₁: (v₁, m₁₂, m₀₁)
        out_idx.push_back(v1);  out_idx.push_back(m12); out_idx.push_back(m01);
        // Corner @ v₂: (v₂, m₂₀, m₁₂)
        out_idx.push_back(v2);  out_idx.push_back(m20); out_idx.push_back(m12);
        // Central: (m₀₁, m₁₂, m₂₀)
        out_idx.push_back(m01); out_idx.push_back(m12); out_idx.push_back(m20);
    }
}

} // anonymous namespace

template <crd::math::MathScalar T>
HalfEdgeMesh<T> loop_subdivide(const HalfEdgeMesh<T>&        input,
                                const LoopSubdivideOptions&  opts,
                                LoopSubdivideReport*         out_report)
{
    LoopSubdivideReport report{};
    auto                report_out = [&] {
        if (out_report != nullptr) { *out_report = report; }
    };

    crd::memory::IAllocator* alloc = opts.output_allocator != nullptr
                                          ? opts.output_allocator
                                          : input.allocator();
    CRD_ASSERT(alloc != nullptr);

    // Extract input to indexed form on the target allocator.
    crd::containers::Array<crd::math::Vec3<T>> pos(alloc);
    crd::containers::Array<crd::u32>           idx(alloc);
    input.to_indexed(pos, idx);

    if (pos.empty() || idx.empty())
    {
        report.status = LoopSubdivideStatus::EmptyMesh;
        HalfEdgeMesh<T> empty{alloc};
        report.output_vertices = 0;
        report.output_faces    = 0;
        report_out();
        return empty;
    }

    for (crd::u32 level = 0; level < opts.n_levels; ++level)
    {
        // Build temp half-edge mesh for topology queries.
        HalfEdgeMesh<T> tmp{alloc};
        const auto bs = tmp.build_from(
            crd::containers::ConstSpan<crd::math::Vec3<T>>{pos.data(), pos.size()},
            crd::containers::ConstSpan<crd::u32>{idx.data(), idx.size()});
        (void)bs; // build_from already validated input topology at level 0.

        if (!tmp.is_manifold())
        {
            report.status = LoopSubdivideStatus::NonManifoldInput;
            HalfEdgeMesh<T> empty{alloc};
            report.output_vertices = 0;
            report.output_faces    = 0;
            report_out();
            return empty;
        }

        crd::containers::Array<crd::math::Vec3<T>> new_pos(alloc);
        crd::containers::Array<crd::u32>           new_idx(alloc);
        subdivide_one_level(tmp, new_pos, new_idx);

        pos = std::move(new_pos);
        idx = std::move(new_idx);
        ++report.levels_applied;
    }

    HalfEdgeMesh<T> result{alloc};
    const auto bs = result.build_from(
        crd::containers::ConstSpan<crd::math::Vec3<T>>{pos.data(), pos.size()},
        crd::containers::ConstSpan<crd::u32>{idx.data(), idx.size()});
    (void)bs;
    report.output_vertices = result.vertex_count();
    report.output_faces    = result.face_count();
    report_out();
    return result;
}

// Explicit instantiations.
template HalfEdgeMesh<crd::f32> loop_subdivide<crd::f32>(const HalfEdgeMesh<crd::f32>&,
                                                          const LoopSubdivideOptions&,
                                                          LoopSubdivideReport*);
template HalfEdgeMesh<crd::f64> loop_subdivide<crd::f64>(const HalfEdgeMesh<crd::f64>&,
                                                          const LoopSubdivideOptions&,
                                                          LoopSubdivideReport*);

} // namespace crd::geometry::mesh_processing
