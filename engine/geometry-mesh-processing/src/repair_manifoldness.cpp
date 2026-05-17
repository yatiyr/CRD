// ---------------------------------------------------------------------------
// crd-geometry-mesh-processing — v7f manifoldness repair implementation.
//
// See header for the algorithm contract. This TU contains:
//   - Phase A: non-manifold edge detection + orientation-pair repair via
//     vertex duplication.
//   - Phase B: bowtie vertex detection via temp-HE-mesh fan walk +
//     fan-component-BFS repair via vertex duplication.
//   - to_indexed → repair → build_from output assembly.
//
// **Pinned design decisions** (carried for ADR-0076 §22 amendment at
// v7-close):
//
//   D52. **Phase A operates at INDEX LEVEL.** Detect non-manifold edges
//        by building an `Array<Array<u32>>` edge-key → directed-HE-index
//        list (one entry per (3·tri + corner)). Edge key = lex-sorted
//        `(min(u, v), max(u, v))` flattened to a row-major
//        `vertex_count` × `vertex_count` map — for dense small meshes;
//        for large sparse meshes a hash would scale better, but at v7f's
//        N ≤ ~100k vertices the array overhead is acceptable. To keep
//        memory linear we use a `crd::containers::HashMap<u64, Array<u32>>`
//        keyed on the packed `u64 (lo << 32) | hi`.
//
//   D53. **Phase A pairing**: within a non-manifold edge group, split
//        HEs into `forwards` (origin < dest, i.e., HE goes `u→v` with
//        u = min endpoint) and `backwards` (origin > dest, HE goes `v→u`).
//        The FIRST forward and FIRST backward (if both exist) keep the
//        ORIGINAL edge endpoints. Each subsequent (forward, backward)
//        pair gets a SINGLE duplicated `min(u, v)` vertex shared between
//        both triangles in the pair — they now share a new manifold edge
//        `(u_new, v)`. Leftover unpaired triangles (orientation imbalance)
//        each get their own duplicated `min(u, v)`, becoming boundary
//        edges in the output.
//
//   D54. **Vertex-corner finder**: to replace `u` with `u_new` in a
//        triangle, find which of the three corner positions (3·tri+0,
//        3·tri+1, 3·tri+2) currently holds `u` and update that index.
//        For non-manifold edge repair this is well-defined: the HE
//        index in the directed list IS the corner of `u` for forward
//        HEs and the corner of `u` AFTER the next corner for backward
//        HEs (= origin of the directed HE in the canonical `(u, v)`
//        sense, regardless of forward/backward).
//
//   D55. **Phase B operates via TEMP HALF-EDGE MESH.** Build a temp
//        HalfEdgeMesh from the (possibly Phase-A-repaired) indices to
//        get a clean topology view. For each alive vertex `v`:
//          * walk_count = number of outgoing HEs visited via
//            `for_each_outgoing_he` (the v7a CW fan walk that closes
//            on the first fan and stops).
//          * slot_count = number of alive HEs whose `origin == v`
//            (= total outgoing count = sum over fans).
//          * If `walk_count != slot_count`, `v` is a bowtie.
//
//   D56. **Fan-component BFS** identifies the disjoint fans at a bowtie.
//        Collect all triangles incident to `v` (linear scan of the index
//        buffer); build adjacency where two triangles `T_a`, `T_b` are
//        adjacent iff they share an edge through `v` (= they share another
//        vertex besides `v`); union-find / BFS gives the connected
//        components. Each component = one fan.
//
//   D57. **Bowtie repair**: for each fan AFTER the first (= fan with
//        the lowest representative-triangle index), duplicate `v` to
//        `v_new` (push to positions array, same coordinates); rewrite
//        all triangles in that fan to use `v_new` wherever they
//        currently reference `v`.
//
//   D58. **Phase A runs BEFORE Phase B.** Phase B's detection requires
//        building a temp HalfEdgeMesh, which itself requires the input
//        triangulation to be manifold-EDGE (each undirected edge has
//        ≤ 2 incident triangles). Phase A enforces this precondition;
//        Phase B then handles whatever bowties remain (plus any introduced
//        by Phase A's duplications, which is rare but possible).
//
//   D59. **Status assignment**: if Phase A detected ≥1 non-manifold edge
//        OR Phase B detected ≥1 bowtie → `Ok` (repaired). Else (input
//        was already manifold) → `AlreadyManifold` and output is a
//        clone-by-rebuild of input.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/hash_map.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/mesh_processing/half_edge_mesh.hpp>
#include <crd/geometry/mesh_processing/repair_manifoldness.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::mesh_processing
{
namespace
{

// Pack (u, v) into a u64 key with the lower 32 bits = min(u,v) and
// upper 32 bits = max(u,v) — direction-agnostic edge key.
inline crd::u64 edge_key(crd::u32 u, crd::u32 v) noexcept
{
    const crd::u32 lo = u < v ? u : v;
    const crd::u32 hi = u < v ? v : u;
    return (static_cast<crd::u64>(hi) << 32) | static_cast<crd::u64>(lo);
}

// Index helpers for the indexed-form triangle representation.
inline crd::u32 he_to_triangle(crd::u32 he_idx) noexcept { return he_idx / 3U; }
inline crd::u32 he_to_corner(crd::u32 he_idx) noexcept   { return he_idx % 3U; }

inline crd::u32 he_origin(const crd::containers::Array<crd::u32>& idx, crd::u32 he_idx) noexcept
{
    return idx[he_idx];
}
inline crd::u32 he_dest(const crd::containers::Array<crd::u32>& idx, crd::u32 he_idx) noexcept
{
    const crd::u32 tri    = he_to_triangle(he_idx);
    const crd::u32 corner = he_to_corner(he_idx);
    return idx[3U * tri + ((corner + 1U) % 3U)];
}

// Replace vertex value `old_v` with `new_v` at the corner of triangle
// `tri` where it currently appears. Asserts the value was found.
void replace_vertex_in_triangle(crd::containers::Array<crd::u32>& idx,
                                  crd::u32                         tri,
                                  crd::u32                         old_v,
                                  crd::u32                         new_v)
{
    for (crd::u32 c = 0; c < 3U; ++c)
    {
        if (idx[3U * tri + c] == old_v)
        {
            idx[3U * tri + c] = new_v;
            return;
        }
    }
    CRD_ASSERT(false); // unreachable: `old_v` was supposed to be in `tri`
}

// Phase A: detect non-manifold edges; repair by duplicating min-endpoint
// for the extra-pair triangles.
template <crd::math::MathScalar T>
void repair_non_manifold_edges_phase(crd::containers::Array<crd::math::Vec3<T>>& positions,
                                       crd::containers::Array<crd::u32>&            indices,
                                       RepairManifoldnessReport&                    report,
                                       crd::memory::IAllocator*                     alloc)
{
    // Build edge → HE-list map.
    crd::containers::HashMap<crd::u64, crd::containers::Array<crd::u32>> edge_to_hes(alloc);
    const crd::u32 he_count = static_cast<crd::u32>(indices.size());
    for (crd::u32 h = 0; h < he_count; ++h)
    {
        const crd::u32 u = he_origin(indices, h);
        const crd::u32 v = he_dest(indices, h);
        if (u == v) { continue; } // skip degenerate
        const crd::u64 key = edge_key(u, v);
        auto* slot = edge_to_hes.find(key);
        if (slot == nullptr)
        {
            crd::containers::Array<crd::u32> lst(alloc);
            lst.push_back(h);
            edge_to_hes.insert(key, std::move(lst));
        }
        else
        {
            slot->push_back(h);
        }
    }

    // Process each non-manifold edge (group with > 2 HEs).
    // HashMap iterator yields const value(); collect keys first, then
    // re-look-up via non-const `find` for mutation.
    crd::containers::Array<crd::u64> non_manifold_keys(alloc);
    for (auto it = edge_to_hes.begin(); it != edge_to_hes.end(); ++it)
    {
        if (it.value().size() > 2U) { non_manifold_keys.push_back(it.key()); }
    }
    for (crd::u32 ki = 0; ki < non_manifold_keys.size(); ++ki)
    {
        auto* hes_ptr = edge_to_hes.find(non_manifold_keys[ki]);
        CRD_ASSERT(hes_ptr != nullptr);
        auto& hes = *hes_ptr;
        if (hes.size() <= 2U) { continue; }

        ++report.non_manifold_edges_detected;

        // Pair into forwards (origin < dest) and backwards.
        crd::containers::Array<crd::u32> forwards(alloc);
        crd::containers::Array<crd::u32> backwards(alloc);
        for (crd::u32 i = 0; i < hes.size(); ++i)
        {
            const crd::u32 h = hes[i];
            const crd::u32 u = he_origin(indices, h);
            const crd::u32 v = he_dest(indices, h);
            if (u < v) { forwards.push_back(h); }
            else        { backwards.push_back(h); }
        }

        const crd::u32 pair_count = forwards.size() < backwards.size()
                                         ? static_cast<crd::u32>(forwards.size())
                                         : static_cast<crd::u32>(backwards.size());

        // Determine the original edge endpoints (the min/max of the key).
        // For pair index 0: keep both triangles on the original edge.
        // For pair index >= 1: duplicate min_endpoint; reassign both
        // triangles in pair to use the duplicate.
        // For singletons (extras beyond pair_count): each gets its own
        // duplicated min_endpoint.
        // We only need the `min` endpoint (= duplicate-target). `max`
        // is implicit in the edge-key and not used in the duplication
        // step.
        crd::u32 orig_u = 0;
        if (!forwards.empty())
        {
            orig_u = he_origin(indices, forwards[0]);
        }
        else
        {
            orig_u = he_dest(indices, backwards[0]); // backward dir: dest = min
        }

        // Pairs beyond the first.
        for (crd::u32 p = 1U; p < pair_count; ++p)
        {
            const crd::u32 new_v = static_cast<crd::u32>(positions.size());
            positions.push_back(positions[orig_u]);
            ++report.duplicated_vertices_added;
            const crd::u32 he_fwd = forwards[p];
            const crd::u32 he_bwd = backwards[p];
            replace_vertex_in_triangle(indices, he_to_triangle(he_fwd), orig_u, new_v);
            replace_vertex_in_triangle(indices, he_to_triangle(he_bwd), orig_u, new_v);
        }

        // Singletons (unpaired extras): one duplicate per triangle.
        for (crd::u32 i = pair_count; i < forwards.size(); ++i)
        {
            const crd::u32 new_v = static_cast<crd::u32>(positions.size());
            positions.push_back(positions[orig_u]);
            ++report.duplicated_vertices_added;
            replace_vertex_in_triangle(indices, he_to_triangle(forwards[i]), orig_u, new_v);
        }
        for (crd::u32 i = pair_count; i < backwards.size(); ++i)
        {
            const crd::u32 new_v = static_cast<crd::u32>(positions.size());
            positions.push_back(positions[orig_u]);
            ++report.duplicated_vertices_added;
            replace_vertex_in_triangle(indices, he_to_triangle(backwards[i]), orig_u, new_v);
        }

        ++report.non_manifold_edges_repaired;
    }
}

// Phase B helper: find ALL triangles incident to vertex `v` by linear
// scan of the index buffer.
void collect_triangles_at_vertex(const crd::containers::Array<crd::u32>& indices,
                                   crd::u32                                v,
                                   crd::containers::Array<crd::u32>&       out_tris)
{
    const crd::u32 tri_count = static_cast<crd::u32>(indices.size() / 3U);
    for (crd::u32 t = 0; t < tri_count; ++t)
    {
        if (indices[3U * t + 0] == v || indices[3U * t + 1] == v
            || indices[3U * t + 2] == v)
        {
            out_tris.push_back(t);
        }
    }
}

// Phase B helper: returns true iff triangles `ta` and `tb` share an edge
// THROUGH vertex `v` (= they share another vertex besides `v`).
bool share_edge_through_vertex(const crd::containers::Array<crd::u32>& indices,
                                 crd::u32                                ta,
                                 crd::u32                                tb,
                                 crd::u32                                v) noexcept
{
    crd::u32 ta_v0 = indices[3U * ta + 0];
    crd::u32 ta_v1 = indices[3U * ta + 1];
    crd::u32 ta_v2 = indices[3U * ta + 2];
    crd::u32 tb_v0 = indices[3U * tb + 0];
    crd::u32 tb_v1 = indices[3U * tb + 1];
    crd::u32 tb_v2 = indices[3U * tb + 2];
    // For each vertex of tb other than v: does ta contain it?
    for (crd::u32 c = 0; c < 3U; ++c)
    {
        const crd::u32 tb_v = (c == 0 ? tb_v0 : (c == 1 ? tb_v1 : tb_v2));
        if (tb_v == v) { continue; }
        if (tb_v == ta_v0 || tb_v == ta_v1 || tb_v == ta_v2) { return true; }
    }
    return false;
}

// Phase B: detect + repair bowtie vertices.
template <crd::math::MathScalar T>
void repair_bowtie_vertices_phase(crd::containers::Array<crd::math::Vec3<T>>& positions,
                                    crd::containers::Array<crd::u32>&            indices,
                                    RepairManifoldnessReport&                    report,
                                    crd::memory::IAllocator*                     alloc)
{
    // Build temp HE mesh for the v7a CW fan-walk topology check.
    HalfEdgeMesh<T> temp{alloc};
    const auto bs = temp.build_from(
        crd::containers::ConstSpan<crd::math::Vec3<T>>{positions.data(), positions.size()},
        crd::containers::ConstSpan<crd::u32>{indices.data(), indices.size()});
    if (bs != BuildStatus::Ok && bs != BuildStatus::NonManifoldEdge)
    {
        // Hard build failure (non-finite / degenerate / OOB) — we cannot
        // detect bowties without a valid topology view. Abort phase B.
        return;
    }

    const crd::u32 vert_pool = temp.vertex_pool_size();
    crd::containers::Array<crd::u8> processed(alloc);
    processed.resize(positions.size(), crd::u8{0});

    for (crd::u32 v = 0; v < vert_pool; ++v)
    {
        if (!temp.vertex_alive(v)) { continue; }
        if (processed[v] != 0U) { continue; }
        processed[v] = 1U;

        // Walk-count = #outgoings visited by the CW fan walk (closes on
        // one fan).
        crd::u32 walk_count = 0;
        temp.for_each_outgoing_he(v, [&](crd::u32) { ++walk_count; });

        // Slot-count = total alive HEs originating at v.
        crd::u32 slot_count = 0;
        for (crd::u32 h = 0; h < temp.he_pool_size(); ++h)
        {
            if (temp.he_alive(h) && temp.he(h).origin == v) { ++slot_count; }
        }

        if (walk_count == slot_count) { continue; }
        ++report.bowtie_vertices_detected;

        // Bowtie detected. Identify fans by BFS over triangle-to-triangle
        // adjacency at v.
        crd::containers::Array<crd::u32> tris_at_v(alloc);
        collect_triangles_at_vertex(indices, v, tris_at_v);
        if (tris_at_v.size() < 2U) { continue; }

        // Disjoint-set / union-find via parent array.
        crd::containers::Array<crd::u32> parent(alloc);
        parent.resize(tris_at_v.size(), crd::u32{0});
        for (crd::u32 i = 0; i < parent.size(); ++i) { parent[i] = i; }
        auto find_root = [&](crd::u32 x) {
            while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
            return x;
        };
        auto unite = [&](crd::u32 a, crd::u32 b) {
            const crd::u32 ra = find_root(a);
            const crd::u32 rb = find_root(b);
            if (ra != rb)
            {
                if (ra < rb) { parent[rb] = ra; }
                else          { parent[ra] = rb; }
            }
        };

        for (crd::u32 i = 0; i < tris_at_v.size(); ++i)
        {
            for (crd::u32 j = i + 1U; j < tris_at_v.size(); ++j)
            {
                if (share_edge_through_vertex(indices, tris_at_v[i], tris_at_v[j], v))
                {
                    unite(i, j);
                }
            }
        }

        // Group by root. Avoid HashMap (whose iterator yields const
        // value); use parallel Array<root> + Array<Array<tri>> indexed
        // by the order roots first appear (which is itself slot-order
        // deterministic since we BFS-walk tris_at_v in slot order).
        crd::containers::Array<crd::u32>                 fan_roots(alloc);
        crd::containers::Array<crd::containers::Array<crd::u32>> fan_tris(alloc);
        auto find_or_make_fan_index = [&](crd::u32 r) -> crd::u32 {
            for (crd::u32 fi = 0; fi < fan_roots.size(); ++fi)
            {
                if (fan_roots[fi] == r) { return fi; }
            }
            fan_roots.push_back(r);
            crd::containers::Array<crd::u32> lst(alloc);
            fan_tris.push_back(std::move(lst));
            return static_cast<crd::u32>(fan_roots.size() - 1U);
        };
        for (crd::u32 i = 0; i < tris_at_v.size(); ++i)
        {
            const crd::u32 r = find_root(i);
            const crd::u32 fi = find_or_make_fan_index(r);
            fan_tris[fi].push_back(tris_at_v[i]);
        }

        if (fan_roots.size() < 2U) { continue; } // false positive — single fan

        // First fan (= the fan containing the lowest-indexed triangle)
        // keeps `v`. Sort fan indices by their MIN triangle id so we
        // deterministically pick that fan.
        crd::containers::Array<crd::u32> fan_order(alloc);
        fan_order.resize(fan_roots.size(), crd::u32{0});
        for (crd::u32 i = 0; i < fan_roots.size(); ++i) { fan_order[i] = i; }
        // Insertion sort by min triangle id.
        for (crd::u32 i = 1; i < fan_order.size(); ++i)
        {
            const crd::u32 ki = fan_order[i];
            const crd::u32 mi = fan_tris[ki][0]; // any triangle suffices; lst is in BFS order
            crd::u32 j = i;
            while (j > 0)
            {
                const crd::u32 kj = fan_order[j - 1];
                const crd::u32 mj = fan_tris[kj][0];
                if (mj <= mi) { break; }
                fan_order[j] = fan_order[j - 1];
                --j;
            }
            fan_order[j] = ki;
        }

        for (crd::u32 f = 1U; f < fan_order.size(); ++f)
        {
            const crd::u32 new_v = static_cast<crd::u32>(positions.size());
            positions.push_back(positions[v]);
            ++report.duplicated_vertices_added;
            auto& this_fan = fan_tris[fan_order[f]];
            for (crd::u32 i = 0; i < this_fan.size(); ++i)
            {
                replace_vertex_in_triangle(indices, this_fan[i], v, new_v);
            }
        }

        ++report.bowtie_vertices_repaired;
    }
}

} // anonymous namespace

template <crd::math::MathScalar T>
HalfEdgeMesh<T> repair_manifoldness(const HalfEdgeMesh<T>&               input,
                                     const RepairManifoldnessOptions&     opts,
                                     RepairManifoldnessReport*            out_report)
{
    RepairManifoldnessReport report{};
    auto                      report_out = [&] {
        if (out_report != nullptr) { *out_report = report; }
    };

    crd::memory::IAllocator* alloc = opts.output_allocator != nullptr
                                          ? opts.output_allocator
                                          : input.allocator();
    CRD_ASSERT(alloc != nullptr);

    if (input.face_count() == 0U)
    {
        report.status = RepairManifoldnessStatus::EmptyMesh;
        report_out();
        return HalfEdgeMesh<T>{alloc};
    }

    crd::containers::Array<crd::math::Vec3<T>> positions(alloc);
    crd::containers::Array<crd::u32>           indices(alloc);
    input.to_indexed(positions, indices);

    if (opts.repair_non_manifold_edges)
    {
        repair_non_manifold_edges_phase(positions, indices, report, alloc);
    }
    if (opts.repair_bowtie_vertices)
    {
        repair_bowtie_vertices_phase(positions, indices, report, alloc);
    }

    HalfEdgeMesh<T> output{alloc};
    const auto bs = output.build_from(
        crd::containers::ConstSpan<crd::math::Vec3<T>>{positions.data(), positions.size()},
        crd::containers::ConstSpan<crd::u32>{indices.data(), indices.size()});
    (void)bs;

    if (report.non_manifold_edges_detected == 0U && report.bowtie_vertices_detected == 0U)
    {
        report.status = RepairManifoldnessStatus::AlreadyManifold;
    }
    report.output_vertices = output.vertex_count();
    report.output_faces    = output.face_count();
    report_out();
    return output;
}

template HalfEdgeMesh<crd::f32> repair_manifoldness<crd::f32>(const HalfEdgeMesh<crd::f32>&,
                                                                const RepairManifoldnessOptions&,
                                                                RepairManifoldnessReport*);
template HalfEdgeMesh<crd::f64> repair_manifoldness<crd::f64>(const HalfEdgeMesh<crd::f64>&,
                                                                const RepairManifoldnessOptions&,
                                                                RepairManifoldnessReport*);

} // namespace crd::geometry::mesh_processing
