// ---------------------------------------------------------------------------
// crd-geometry-mesh-processing — v7b QEM decimation implementation.
//
// See header for the algorithm contract. This TU contains:
//   - Per-vertex quadric accumulation (interior + Garland 1998 boundary).
//   - Edge-cost evaluation with locked-vertex constraint.
//   - Greedy lazy-invalidating min-heap loop.
//   - Inversion-prevention check (predict-then-collapse).
//
// **Pinned design decisions** (carried for ADR-0076 §22 amendment at
// v7-close):
//
//   D9.  Per-vertex quadric `Q_v = Σ_{f ∋ v} K_f` where each
//        `K_f = p_f * p_f^T` for the unit-normal face plane `p_f`.
//        Degenerate faces (`|cross| < 1e-20`) contribute zero.
//
//   D10. Boundary preservation (Garland 1998 §3.1): for each boundary HE,
//        add `boundary_weight * p_b * p_b^T` to both endpoints where
//        `n_b = normalize(edge × n_f)` is perpendicular to face f and
//        contains the edge. Default weight = 1000.
//
//   D11. Per-edge cost: solve closed-form `v_opt` for combined quadric;
//        fall back to midpoint if singular. Locked endpoint constrains
//        v_opt to its position; both-locked rejected.
//
//   D12. Min-heap entries `{cost, canonical_he, generation}` sorted by
//        lex `(cost, canonical_he)` — byte-identical across compilers.
//        Canonical HE = min(h, h.twin) per undirected edge.
//
//   D13. Lazy invalidation: a per-canonical-HE `generation` counter is
//        bumped on every push; pop checks generation match + alive +
//        non-boundary. No periodic prune (heap waste bounded by
//        O(initial_edges × avg_valence) — acceptable for typical meshes).
//
//   D14. Inversion prevention: before collapse, for each face incident
//        to a or b (excluding f1/f2 — the two collapse-deleted faces),
//        check that substituting v_opt for the merged endpoint does not
//        flip the face normal. Rejects v_opt placements that would
//        self-intersect.
//
//   D15. Output mesh is constructed via `input.to_indexed → build_from`
//        on the requested allocator. Input is never mutated. Locked
//        vertex indices reference the INPUT slot space; the output
//        preserves index identity for any vertex that survives.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/sort.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/mesh_processing/half_edge_mesh.hpp>
#include <crd/geometry/mesh_processing/qem_decimate.hpp>
#include <crd/geometry/mesh_processing/attribute_quadric.hpp>
#include <crd/geometry/mesh_processing/quadric.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

#include <array>
#include <cmath>
#include <functional>
#include <optional>

namespace crd::geometry::mesh_processing
{
namespace
{

template <crd::math::MathScalar T>
struct HeapEntry
{
    T        cost;
    crd::u32 he_canonical;
    crd::u32 generation;
};

// Min-heap comparator. crd::containers::push_heap/pop_heap use std::less
// semantics (a max-heap); we invert to get a min-heap. Lex tiebreak by
// canonical HE id for byte-identical ordering across compilers.
template <crd::math::MathScalar T>
struct HeapMinCmp
{
    bool operator()(const HeapEntry<T>& l, const HeapEntry<T>& r) const noexcept
    {
        if (l.cost != r.cost) { return l.cost > r.cost; }
        return l.he_canonical > r.he_canonical;
    }
};

template <crd::math::MathScalar T>
inline crd::u32 canonical_he(const HalfEdgeMesh<T>& mesh, crd::u32 h) noexcept
{
    const crd::u32 t = mesh.he(h).twin;
    if (t == k_null_he) { return h; }
    return (h < t) ? h : t;
}

// Compute plane (a, b, c, d) for face f. Returns nullopt for degenerate
// faces (|cross| < 1e-20).
template <crd::math::MathScalar T>
std::optional<std::array<T, 4>> face_plane(const HalfEdgeMesh<T>& mesh, crd::u32 f) noexcept
{
    const crd::u32 h0  = mesh.face(f).first_he;
    const crd::u32 h1  = mesh.he(h0).next;
    const crd::u32 h2  = mesh.he(h1).next;
    const auto&    p0  = mesh.vertex(mesh.he(h0).origin).position;
    const auto&    p1  = mesh.vertex(mesh.he(h1).origin).position;
    const auto&    p2  = mesh.vertex(mesh.he(h2).origin).position;
    const auto     e1  = p1 - p0;
    const auto     e2  = p2 - p0;
    const auto     n   = crd::math::cross(e1, e2);
    const T        len = crd::math::length(n);
    if (len < static_cast<T>(1e-20)) { return std::nullopt; }
    const T inv_len = T{1} / len;
    const T a       = n.x * inv_len;
    const T b       = n.y * inv_len;
    const T c       = n.z * inv_len;
    const T d       = -(a * p0.x + b * p0.y + c * p0.z);
    return std::array<T, 4>{a, b, c, d};
}

// ⭐⭐ REN-40-C1: the quadric is now an ATTRIBUTE quadric with `M` channels, and
// `M = 0` is the historical path BIT FOR BIT (see attribute_quadric.hpp — the
// reduction is gated, not assumed). `attrs` is `mesh.vertex_pool_size() * M`
// values in OUTPUT slot space, or null when M == 0.
//
// ⛔ THE ATTRIBUTE HAS TO BE IN THE ERROR, NOT FIXED UP AFTER IT. A position-only
// metric will pick the collapse that leaves the surface where it was and drags
// the texture across it — the silhouette stays right and the texture swims as the
// LOD changes, which is the one artefact an LOD chain must not have.
template <crd::math::MathScalar T, crd::u32 M>
void compute_initial_quadrics(const HalfEdgeMesh<T>&                          mesh,
                               crd::containers::Array<AttributeQuadric<T, M>>& vertex_q,
                               T                                               boundary_weight,
                               const T*                                        attrs)
{
    vertex_q.resize(mesh.vertex_pool_size(), AttributeQuadric<T, M>::zero());

    // Pass 1: interior face quadrics (+ the per-channel linear models).
    for (crd::u32 f = 0; f < mesh.face_pool_size(); ++f)
    {
        if (!mesh.face_alive(f)) { continue; }
        const auto plane = face_plane(mesh, f);
        if (!plane) { continue; }
        AttributeGradient<T> grad[M > 0U ? M : 1U]{};
        crd::u32             corner[3]  = {k_null_vertex, k_null_vertex, k_null_vertex};
        crd::u32             n_corner   = 0U;
        mesh.for_each_face_he(f, [&](crd::u32 h) {
            if (n_corner < 3U) { corner[n_corner] = mesh.he(h).origin; }
            ++n_corner;
        });
        if constexpr (M > 0U)
        {
            // a non-triangle (or a face we could not read three corners from) gets
            // the geometric term only — a wrong linear model is worse than none
            if (n_corner == 3U && attrs != nullptr)
            {
                const auto& p1 = mesh.vertex(corner[0]).position;
                const auto& p2 = mesh.vertex(corner[1]).position;
                const auto& p3 = mesh.vertex(corner[2]).position;
                for (crd::u32 j = 0; j < M; ++j)
                {
                    grad[j] = plane_gradient(p1, p2, p3, attrs[(corner[0] * M) + j], attrs[(corner[1] * M) + j],
                                             attrs[(corner[2] * M) + j]);
                }
            }
        }
        mesh.for_each_face_he(f, [&](crd::u32 h) {
            accumulate_face<T, M>(vertex_q[mesh.he(h).origin], (*plane)[0], (*plane)[1], (*plane)[2], (*plane)[3],
                                  grad, T{1});
        });
    }

    // Pass 2: Garland 1998 boundary-preservation quadrics.
    if (boundary_weight <= T{0}) { return; }
    for (crd::u32 h = 0; h < mesh.he_pool_size(); ++h)
    {
        if (!mesh.he_alive(h)) { continue; }
        if (!mesh.he_is_boundary(h)) { continue; }
        // The interior twin of the boundary HE bounds the only incident
        // face. We need that face's plane to build the perpendicular
        // boundary plane.
        const crd::u32 t = mesh.he(h).twin;
        if (t == k_null_he) { continue; }
        const crd::u32 f = mesh.he(t).face;
        if (f == k_null_face) { continue; }
        const auto plane_f = face_plane(mesh, f);
        if (!plane_f) { continue; }
        const crd::math::Vec3<T> n_f{(*plane_f)[0], (*plane_f)[1], (*plane_f)[2]};
        const crd::u32           v_a     = mesh.he(h).origin;
        const crd::u32           v_b     = mesh.he(mesh.he(h).next).origin;
        const auto&              pa      = mesh.vertex(v_a).position;
        const auto&              pb      = mesh.vertex(v_b).position;
        const auto               edge    = pb - pa;
        const auto               n_b_raw = crd::math::cross(edge, n_f);
        const T                  len     = crd::math::length(n_b_raw);
        if (len < static_cast<T>(1e-20)) { continue; }
        const T inv_len = T{1} / len;
        const T a       = n_b_raw.x * inv_len;
        const T b       = n_b_raw.y * inv_len;
        const T c       = n_b_raw.z * inv_len;
        const T d       = -(a * pa.x + b * pa.y + c * pa.z);
        // ⛔ GEOMETRIC HALF ONLY, and no weight bump. A boundary plane constrains
        // WHERE the silhouette may go; it says nothing about the attribute field,
        // and counting it in `weight` would dilute the channel normalisation with
        // a face that carries no attribute model.
        const auto boundary_q = Quadric<T>::from_plane(a, b, c, d) * boundary_weight;
        vertex_q[v_a].geom += boundary_q;
        vertex_q[v_b].geom += boundary_q;
    }
}

// Inversion check: returns true iff substituting v_opt for vertex `merged`
// (one of a / b) does NOT flip any incident face's orientation. The two
// faces being deleted by the collapse (f_skip_1, f_skip_2) are skipped.
template <crd::math::MathScalar T>
bool no_inversion(const HalfEdgeMesh<T>&    mesh,
                   crd::u32                  merged,
                   const crd::math::Vec3<T>& v_opt,
                   crd::u32                  other_endpoint,
                   crd::u32                  f_skip_1,
                   crd::u32                  f_skip_2) noexcept
{
    bool ok = true;
    mesh.for_each_outgoing_he(merged, [&](crd::u32 ho) {
        if (!ok) { return; }
        const crd::u32 f = mesh.he(ho).face;
        if (f == k_null_face || f == f_skip_1 || f == f_skip_2) { return; }
        // Get the three vertices of face f and compute old normal.
        const crd::u32 ha = ho;
        const crd::u32 hb = mesh.he(ha).next;
        const crd::u32 hc = mesh.he(hb).next;
        const crd::u32 va = mesh.he(ha).origin;
        const crd::u32 vb = mesh.he(hb).origin;
        const crd::u32 vc = mesh.he(hc).origin;
        const auto&    pa = mesh.vertex(va).position;
        const auto&    pb = mesh.vertex(vb).position;
        const auto&    pc = mesh.vertex(vc).position;
        const auto     n_old = crd::math::cross(pb - pa, pc - pa);
        // Build new positions by substituting v_opt for the merged vertex.
        // The OTHER endpoint will be removed entirely by collapse_edge —
        // any face that still references it (= a face in OTHER's 1-ring
        // not deleted by collapse) effectively has it remapped to merged
        // too. So both `merged` and `other_endpoint` map to v_opt here.
        const auto pa_new = (va == merged || va == other_endpoint) ? v_opt : pa;
        const auto pb_new = (vb == merged || vb == other_endpoint) ? v_opt : pb;
        const auto pc_new = (vc == merged || vc == other_endpoint) ? v_opt : pc;
        const auto n_new  = crd::math::cross(pb_new - pa_new, pc_new - pa_new);
        if (crd::math::dot(n_old, n_new) <= T{0}) { ok = false; }
    });
    if (!ok) { return false; }
    // Also walk other_endpoint's faces (the ones not deleted by collapse —
    // they migrate to merged after collapse).
    mesh.for_each_outgoing_he(other_endpoint, [&](crd::u32 ho) {
        if (!ok) { return; }
        const crd::u32 f = mesh.he(ho).face;
        if (f == k_null_face || f == f_skip_1 || f == f_skip_2) { return; }
        const crd::u32 ha = ho;
        const crd::u32 hb = mesh.he(ha).next;
        const crd::u32 hc = mesh.he(hb).next;
        const crd::u32 va = mesh.he(ha).origin;
        const crd::u32 vb = mesh.he(hb).origin;
        const crd::u32 vc = mesh.he(hc).origin;
        const auto&    pa = mesh.vertex(va).position;
        const auto&    pb = mesh.vertex(vb).position;
        const auto&    pc = mesh.vertex(vc).position;
        const auto     n_old = crd::math::cross(pb - pa, pc - pa);
        const auto pa_new = (va == merged || va == other_endpoint) ? v_opt : pa;
        const auto pb_new = (vb == merged || vb == other_endpoint) ? v_opt : pb;
        const auto pc_new = (vc == merged || vc == other_endpoint) ? v_opt : pc;
        const auto n_new  = crd::math::cross(pb_new - pa_new, pc_new - pa_new);
        if (crd::math::dot(n_old, n_new) <= T{0}) { ok = false; }
    });
    return ok;
}

// Result of evaluating a candidate edge collapse. nullopt = collapse
// rejected at evaluation time (both endpoints locked); the algorithm
// continues without entering the candidate into the heap.
template <crd::math::MathScalar T>
struct EdgeCost
{
    crd::math::Vec3<T> v_opt;
    T                  cost;
    bool               singular_fallback; // optimal_position returned nullopt
};

template <crd::math::MathScalar T, crd::u32 M>
std::optional<EdgeCost<T>> evaluate_edge(const HalfEdgeMesh<T>&                               mesh,
                                          const crd::containers::Array<AttributeQuadric<T, M>>& vertex_q,
                                          const crd::containers::Array<crd::u8>&               is_locked,
                                          crd::u32                                             h,
                                          T                                                    singular_det_epsilon)
{
    const crd::u32 a = mesh.he(h).origin;
    const crd::u32 b = mesh.he_dest(h);
    if (b == k_null_vertex) { return std::nullopt; }
    const bool la = (a < is_locked.size()) && (is_locked[a] != 0U);
    const bool lb = (b < is_locked.size()) && (is_locked[b] != 0U);
    if (la && lb) { return std::nullopt; }

    const auto combined_q = vertex_q[a] + vertex_q[b];
    // ⭐ the FOLD is what keeps the rest of this function unchanged: eliminating
    // the attribute unknowns analytically leaves an ordinary 4x4 quadric, so the
    // 3x3 solve, the singular fallback and the determinism contract all carry over.
    const Quadric<T> folded = fold(combined_q);
    EdgeCost<T> ec{};
    if (la)
    {
        ec.v_opt             = mesh.vertex(a).position;
        ec.singular_fallback = false;
    }
    else if (lb)
    {
        ec.v_opt             = mesh.vertex(b).position;
        ec.singular_fallback = false;
    }
    else
    {
        const auto opt = optimal_position(folded, singular_det_epsilon);
        if (opt)
        {
            ec.v_opt             = *opt;
            ec.singular_fallback = false;
        }
        else
        {
            const auto& pa = mesh.vertex(a).position;
            const auto& pb = mesh.vertex(b).position;
            ec.v_opt = crd::math::Vec3<T>{(pa.x + pb.x) / T{2},
                                          (pa.y + pb.y) / T{2},
                                          (pa.z + pb.z) / T{2}};
            ec.singular_fallback = true;
        }
    }
    ec.cost = evaluate(folded, ec.v_opt);
    return ec;
}

// Push a new heap entry, bumping the canonical edge's generation so any
// pre-existing entries for the same canonical become stale.
template <crd::math::MathScalar T>
void push_edge(crd::containers::Array<HeapEntry<T>>& heap,
                crd::containers::Array<crd::u32>&     edge_generation,
                T                                     cost,
                crd::u32                              canonical)
{
    if (canonical >= edge_generation.size())
    {
        edge_generation.resize(canonical + 1U, crd::u32{0});
    }
    ++edge_generation[canonical];
    heap.push_back(HeapEntry<T>{cost, canonical, edge_generation[canonical]});
    crd::containers::push_heap(heap.data(), heap.data() + heap.size(), HeapMinCmp<T>{});
}

} // anonymous namespace

// The one implementation. `attrs_in` is `M` values per INPUT vertex (null when
// M == 0); `attrs_out`, when non-null, receives `M` values per OUTPUT vertex in
// the mesh's own `to_indexed` order - the same order its positions come out in,
// so a caller never has to guess a correspondence.
template <crd::math::MathScalar T, crd::u32 M>
HalfEdgeMesh<T> qem_decimate_impl(const HalfEdgeMesh<T>&            input,
                                   const QemDecimateOptions<T>&      opts,
                                   const T*                          attrs_in,
                                   crd::containers::Array<T>*        attrs_out,
                                   QemDecimateReport*                out_report)
{
    QemDecimateReport report{};
    auto              report_out = [&] {
        if (out_report != nullptr) { *out_report = report; }
    };

    crd::memory::IAllocator* out_alloc = opts.output_allocator != nullptr
                                              ? opts.output_allocator
                                              : input.allocator();
    CRD_ASSERT(out_alloc != nullptr);

    // Clone the input via to_indexed → build_from on the requested allocator.
    HalfEdgeMesh<T> output{out_alloc};
    {
        crd::containers::Array<crd::math::Vec3<T>> pos(out_alloc);
        crd::containers::Array<crd::u32>           idx(out_alloc);
        input.to_indexed(pos, idx);
        if (pos.empty() || idx.empty())
        {
            report.status = QemDecimateStatus::EmptyMesh;
            report_out();
            return output;
        }
        const auto bs = output.build_from(
            crd::containers::ConstSpan<crd::math::Vec3<T>>{pos.data(), pos.size()},
            crd::containers::ConstSpan<crd::u32>{idx.data(), idx.size()});
        (void)bs; // input was already validated by its own build_from; clone path mirrors it.
    }

    // Validate stop conditions.
    const bool has_target = (opts.target_face_count > 0U);
    const bool has_error  = std::isfinite(opts.max_error_threshold);
    if (!has_target && !has_error)
    {
        report.status = QemDecimateStatus::NoStopCondition;
        report_out();
        return output;
    }

    const crd::u32 initial_faces = output.face_count();
    if (initial_faces == 0U)
    {
        report.status = QemDecimateStatus::EmptyMesh;
        report_out();
        return output;
    }

    if (!output.is_manifold())
    {
        report.status = QemDecimateStatus::NonManifoldInput;
        report_out();
        return output;
    }

    // The attribute array must be seeded in OUTPUT slot space. `build_from`
    // renumbered the vertices, so feeding the caller's INPUT-ordered array
    // straight in would attach every vertex's attributes to a DIFFERENT vertex -
    // silently, and the result would still be a valid-looking mesh.
    crd::containers::Array<T> attrs(out_alloc);
    if constexpr (M > 0U)
    {
        attrs.resize(static_cast<crd::usize>(output.vertex_pool_size()) * M, T{0});
        if (attrs_in != nullptr)
        {
            crd::containers::Array<crd::math::Vec3<T>> pos_r(out_alloc);
            crd::containers::Array<crd::u32>           idx_r(out_alloc);
            crd::containers::Array<crd::u32>           remap_in(out_alloc);
            input.to_indexed(pos_r, idx_r, &remap_in);
            for (crd::u32 v_in = 0; v_in < remap_in.size(); ++v_in)
            {
                const crd::u32 v_out = remap_in[v_in];
                if (v_out == k_null_vertex || v_out >= output.vertex_pool_size()) { continue; }
                for (crd::u32 j = 0; j < M; ++j) { attrs[(v_out * M) + j] = attrs_in[(v_in * M) + j]; }
            }
        }
    }

    // Per-vertex quadrics.
    crd::containers::Array<AttributeQuadric<T, M>> vertex_q(out_alloc);
    compute_initial_quadrics<T, M>(output, vertex_q, opts.boundary_weight,
                                   attrs.empty() ? nullptr : attrs.data());

    // Per-vertex locked flag (in input slot space, which == output slot
    // space because to_indexed/build_from preserve vertex IDs by slot
    // walk order — alive vertices walked in slot order get fresh IDs 0..n,
    // BUT only if there are no dead slots in the input. For freshly-built
    // input meshes this holds. For inputs that have undergone prior edits,
    // locked indices must reference the OUTPUT slot space. To keep the
    // contract simple we use the OUTPUT slot space directly via the
    // old→new remap from to_indexed.).
    //
    // Re-extract the remap so locked indices in INPUT space map correctly:
    crd::containers::Array<crd::u8> is_locked(out_alloc);
    is_locked.resize(output.vertex_pool_size(), crd::u8{0});
    {
        crd::containers::Array<crd::math::Vec3<T>> pos2(out_alloc);
        crd::containers::Array<crd::u32>           idx2(out_alloc);
        crd::containers::Array<crd::u32>           remap(out_alloc);
        input.to_indexed(pos2, idx2, &remap);
        for (crd::u32 i = 0; i < opts.locked_vertices.size(); ++i)
        {
            const crd::u32 v_in = opts.locked_vertices[i];
            if (v_in >= remap.size()) { continue; }
            const crd::u32 v_out = remap[v_in];
            if (v_out == k_null_vertex) { continue; }
            if (v_out < is_locked.size()) { is_locked[v_out] = 1U; }
        }
    }

    // Per-canonical-HE generation counter.
    crd::containers::Array<crd::u32> edge_generation(out_alloc);
    edge_generation.resize(output.he_pool_size(), crd::u32{0});

    // Build initial heap.
    crd::containers::Array<HeapEntry<T>> heap(out_alloc);
    heap.reserve(output.he_pool_size() / 2U);
    for (crd::u32 h = 0; h < output.he_pool_size(); ++h)
    {
        if (!output.he_alive(h)) { continue; }
        if (output.he_is_boundary(h)) { continue; }
        if (h != canonical_he(output, h)) { continue; }
        const auto cand = evaluate_edge<T, M>(output, vertex_q, is_locked, h, opts.singular_det_epsilon);
        if (!cand) { continue; }
        if (cand->singular_fallback) { ++report.singular_fallbacks; }
        push_edge(heap, edge_generation, cand->cost, h);
    }

    // Greedy collapse loop.
    crd::u32 current_faces  = initial_faces;
    bool     reached_target = false;
    while (!heap.empty())
    {
        if (has_target && current_faces <= opts.target_face_count)
        {
            reached_target = true;
            break;
        }

        // Pop cheapest.
        crd::containers::pop_heap(heap.data(), heap.data() + heap.size(), HeapMinCmp<T>{});
        const HeapEntry<T> entry = heap.back();
        heap.pop_back();

        // Validate entry.
        if (entry.he_canonical >= edge_generation.size()) { continue; }
        if (edge_generation[entry.he_canonical] != entry.generation) { continue; }
        if (!output.he_alive(entry.he_canonical)) { continue; }
        if (output.he_is_boundary(entry.he_canonical)) { continue; }

        // Cost-threshold check (after popping = min cost).
        if (has_error && entry.cost > opts.max_error_threshold) { break; }

        const crd::u32 h = entry.he_canonical;

        // Re-evaluate (quadrics or locks may have shifted since push).
        const auto cand = evaluate_edge<T, M>(output, vertex_q, is_locked, h, opts.singular_det_epsilon);
        if (!cand) { continue; }

        // Capture topology around the collapse BEFORE applying.
        const crd::u32 a   = output.he(h).origin;
        const crd::u32 b   = output.he_dest(h);
        const crd::u32 t   = output.he(h).twin;
        const crd::u32 f1  = output.he(h).face;
        const crd::u32 f2  = (t != k_null_he) ? output.he(t).face : k_null_face;

        // Inversion-prevention check.
        if (!no_inversion(output, a, cand->v_opt, b, f1, f2))
        {
            ++report.collapses_rejected_flip;
            continue;
        }

        // Apply collapse.
        if (!output.collapse_edge(h, cand->v_opt))
        {
            ++report.collapses_rejected_link;
            continue;
        }

        // Update quadric for kept vertex.
        vertex_q[a] = vertex_q[a] + vertex_q[b];
        // ...and its attributes, from the SAME minimisation that produced the
        // position - not a lerp, not a copy of one endpoint. The stationary value
        // of the merged quadric is exactly what the metric just paid for.
        if constexpr (M > 0U)
        {
            T merged[M]{};
            attributes_at(vertex_q[a], cand->v_opt, merged);
            for (crd::u32 j = 0; j < M; ++j) { attrs[(a * M) + j] = merged[j]; }
        }

        // Re-evaluate every edge in a's new 1-ring.
        output.for_each_outgoing_he(a, [&](crd::u32 ho) {
            if (output.he_is_boundary(ho)) { return; }
            const crd::u32 c = canonical_he(output, ho);
            const auto     new_cand = evaluate_edge<T, M>(output, vertex_q, is_locked, c, opts.singular_det_epsilon);
            if (!new_cand) { return; }
            if (new_cand->singular_fallback) { ++report.singular_fallbacks; }
            push_edge(heap, edge_generation, new_cand->cost, c);
        });

        current_faces -= 2U;
        ++report.collapses_applied;
    }

    if (has_target && !reached_target && current_faces > opts.target_face_count)
    {
        report.status = QemDecimateStatus::TargetUnreachable;
    }
    // Hand back the surviving attributes in the SAME order `to_indexed` gives the
    // positions, so the caller reads one consistent vertex stream.
    if constexpr (M > 0U)
    {
        if (attrs_out != nullptr)
        {
            crd::containers::Array<crd::math::Vec3<T>> pos_f(out_alloc);
            crd::containers::Array<crd::u32>           idx_f(out_alloc);
            crd::containers::Array<crd::u32>           remap_out(out_alloc);
            output.to_indexed(pos_f, idx_f, &remap_out);
            attrs_out->clear();
            attrs_out->resize(pos_f.size() * M, T{0});
            for (crd::u32 v_slot = 0; v_slot < remap_out.size(); ++v_slot)
            {
                const crd::u32 v_new = remap_out[v_slot];
                if (v_new == k_null_vertex || v_new >= pos_f.size()) { continue; }
                for (crd::u32 j = 0; j < M; ++j) { (*attrs_out)[(v_new * M) + j] = attrs[(v_slot * M) + j]; }
            }
        }
    }
    report_out();
    return output;
}

// -- the public entry points ------------------------------------------------
// The historical, position-only form. `M = 0` is the attribute path with zero
// channels and is BIT-IDENTICAL to what this function did before attributes
// existed (attribute_quadric.hpp's reduction gate is what makes that a fact
// rather than an intention).
template <crd::math::MathScalar T>
HalfEdgeMesh<T> qem_decimate(const HalfEdgeMesh<T>& input, const QemDecimateOptions<T>& opts,
                              QemDecimateReport* out_report)
{
    return qem_decimate_impl<T, 0U>(input, opts, nullptr, nullptr, out_report);
}

template <crd::math::MathScalar T, crd::u32 M>
HalfEdgeMesh<T> qem_decimate_attr(const HalfEdgeMesh<T>& input, const T* attrs_in,
                                   const QemDecimateOptions<T>& opts, crd::containers::Array<T>* attrs_out,
                                   QemDecimateReport* out_report)
{
    return qem_decimate_impl<T, M>(input, opts, attrs_in, attrs_out, out_report);
}

// ---------------------------------------------------------------------------
// Explicit instantiations.
// ---------------------------------------------------------------------------

template HalfEdgeMesh<crd::f32> qem_decimate<crd::f32>(const HalfEdgeMesh<crd::f32>&,
                                                       const QemDecimateOptions<crd::f32>&,
                                                       QemDecimateReport*);
template HalfEdgeMesh<crd::f64> qem_decimate<crd::f64>(const HalfEdgeMesh<crd::f64>&,
                                                       const QemDecimateOptions<crd::f64>&,
                                                       QemDecimateReport*);
// REN-40-C1: M = 2 is the UV pair - the channels an LOD chain must not distort.
// Normals and tangents are RE-DERIVED from the decimated surface rather than
// carried, because an interpolated normal of a simplified surface is the normal
// of a surface that no longer exists.
template HalfEdgeMesh<crd::f32> qem_decimate_attr<crd::f32, 2U>(const HalfEdgeMesh<crd::f32>&, const crd::f32*,
                                                                const QemDecimateOptions<crd::f32>&,
                                                                crd::containers::Array<crd::f32>*,
                                                                QemDecimateReport*);
template HalfEdgeMesh<crd::f64> qem_decimate_attr<crd::f64, 2U>(const HalfEdgeMesh<crd::f64>&, const crd::f64*,
                                                                const QemDecimateOptions<crd::f64>&,
                                                                crd::containers::Array<crd::f64>*,
                                                                QemDecimateReport*);

} // namespace crd::geometry::mesh_processing
