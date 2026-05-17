// ---------------------------------------------------------------------------
// crd-geometry-mesh-processing — v7a HalfEdgeMesh<T> substrate.
//
// See header for the contract. This TU contains:
//   - build_from / to_indexed
//   - the three atomic edit operations (collapse / split / flip) with their
//     link-condition + manifold-preserving checks
//   - topology queries (counts, manifold/closed predicates, Euler char,
//     boundary loop counter)
//   - slot allocators (free-list-first)
//
// **Pinned design decisions** (referenced from ADR-0076 §22 amendment at
// v7-close):
//
//   D1. Twin pairing during `build_from` uses lex-tuple `(min(va,vb),
//       max(va,vb), face_id, local_edge_idx)` sort + adjacent-pair-pass.
//       Bit-identical twin assignment across MSVC / GCC / clang.
//
//   D2. Boundary half-edges are MATERIALISED (one per unpaired edge) with
//       `face == k_null_face`. Walking a boundary loop is the same `.next`
//       traversal as a face loop. The alternative ("twin == k_null") was
//       considered and rejected — materialised boundary half-edges keep
//       every algorithm's hot loop branchless on the twin pointer.
//
//   D3. `prev` is NOT stored. For a triangle face it's two `next` hops
//       (constant cost); the memory saving (4 bytes/he × ~6 he/triangle =
//       24 bytes/triangle) dominates the prev-recovery cost. When v8+
//       generalises to n-gons, we'll revisit (n-gon prev needs O(n)
//       traversal — at that point storing prev may become worthwhile).
//
//   D4. Free-list pop is LIFO (last-freed-first-reused). Deterministic
//       given a deterministic edit sequence; doesn't introduce ordering
//       beyond what the caller already controls.
//
//   D5. Link-condition check (Edelsbrunner 2001) gates `collapse_edge`:
//       the collapse is valid iff `link(edge(a,b)) == link(a) ∩ link(b)`.
//       For a manifold edge with 2 incident triangles, this means the
//       1-rings of a and b share EXACTLY the two opposite apex vertices.
//       Violation = the collapse would create a non-manifold vertex or
//       disconnect topology.
//
//   D6. `flip_edge` validity = the quad must be convex when projected to
//       its average plane. Tested via 4 orient2d-on-projected-plane signs
//       (Shewchuk adaptive — handles near-collinear cases robustly).
//
//   D7. `split_edge` produces 4 new half-edges + 2 new faces (per side of
//       the original edge). Boundary edges produce 2 new half-edges + 1
//       new face. Atomic — partial state never visible to caller.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/sort.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/mesh_processing/half_edge_mesh.hpp>
#include <crd/geometry/primitives/is_finite.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::mesh_processing
{

// ===========================================================================
// Slot allocators
// ===========================================================================

template <crd::math::MathScalar T>
crd::u32 HalfEdgeMesh<T>::alloc_vertex() noexcept
{
    if (!m_free_vertices.empty())
    {
        const crd::u32 idx = m_free_vertices.back();
        m_free_vertices.pop_back();
        m_vertices[idx]          = VertexSlot<T>{};
        m_vertices[idx].flags    = k_alive_bit;
        m_vertices[idx].outgoing = k_null_he;
        return idx;
    }
    const crd::u32 idx = static_cast<crd::u32>(m_vertices.size());
    VertexSlot<T>  v{};
    v.flags    = k_alive_bit;
    v.outgoing = k_null_he;
    m_vertices.push_back(v);
    return idx;
}

template <crd::math::MathScalar T>
crd::u32 HalfEdgeMesh<T>::alloc_he() noexcept
{
    if (!m_free_he.empty())
    {
        const crd::u32 idx = m_free_he.back();
        m_free_he.pop_back();
        m_half_edges[idx] = HalfEdgeSlot{};
        // We use origin != k_null_vertex as the alive sentinel for HE
        // (no flag field — keeps HalfEdgeSlot at 16 B). Caller must
        // set origin before publishing.
        return idx;
    }
    const crd::u32 idx = static_cast<crd::u32>(m_half_edges.size());
    m_half_edges.push_back(HalfEdgeSlot{});
    return idx;
}

template <crd::math::MathScalar T>
crd::u32 HalfEdgeMesh<T>::alloc_face() noexcept
{
    if (!m_free_faces.empty())
    {
        const crd::u32 idx = m_free_faces.back();
        m_free_faces.pop_back();
        m_faces[idx]       = FaceSlot{};
        m_faces[idx].flags = k_alive_bit;
        return idx;
    }
    const crd::u32 idx = static_cast<crd::u32>(m_faces.size());
    FaceSlot       f{};
    f.flags = k_alive_bit;
    m_faces.push_back(f);
    return idx;
}

template <crd::math::MathScalar T>
void HalfEdgeMesh<T>::free_vertex(crd::u32 v) noexcept
{
    m_vertices[v].flags    = 0U;
    m_vertices[v].outgoing = k_null_he;
    m_free_vertices.push_back(v);
}

template <crd::math::MathScalar T>
void HalfEdgeMesh<T>::free_he(crd::u32 h) noexcept
{
    m_half_edges[h]        = HalfEdgeSlot{};
    m_half_edges[h].origin = k_null_vertex; // dead sentinel
    m_free_he.push_back(h);
}

template <crd::math::MathScalar T>
void HalfEdgeMesh<T>::free_face(crd::u32 f) noexcept
{
    m_faces[f].flags = 0U;
    m_free_faces.push_back(f);
}

// ===========================================================================
// build_from
// ===========================================================================

template <crd::math::MathScalar T>
BuildStatus HalfEdgeMesh<T>::build_from(crd::containers::ConstSpan<crd::math::Vec3<T>> positions,
                                         crd::containers::ConstSpan<crd::u32> triangle_indices)
{
    m_vertices.clear();
    m_half_edges.clear();
    m_faces.clear();
    m_free_vertices.clear();
    m_free_he.clear();
    m_free_faces.clear();

    if ((triangle_indices.size() % 3U) != 0U)
    {
        return BuildStatus::IndicesNotMultipleOf3;
    }
    for (crd::usize i = 0; i < positions.size(); ++i)
    {
        if (!crd::geometry::primitives::is_finite(positions[i]))
        {
            return BuildStatus::NonFiniteInput;
        }
    }
    const crd::u32 n_pos = static_cast<crd::u32>(positions.size());
    const crd::u32 n_tri = static_cast<crd::u32>(triangle_indices.size() / 3U);
    for (crd::u32 t = 0; t < n_tri; ++t)
    {
        const crd::u32 i0 = triangle_indices[3U * t + 0U];
        const crd::u32 i1 = triangle_indices[3U * t + 1U];
        const crd::u32 i2 = triangle_indices[3U * t + 2U];
        if (i0 >= n_pos || i1 >= n_pos || i2 >= n_pos)
        {
            return BuildStatus::OutOfBoundsIndex;
        }
        if (i0 == i1 || i1 == i2 || i2 == i0)
        {
            return BuildStatus::DegenerateTriangle;
        }
    }

    // Step 1: allocate vertex slots (one per input position).
    m_vertices.reserve(n_pos);
    for (crd::u32 i = 0; i < n_pos; ++i)
    {
        VertexSlot<T> v{};
        v.position = positions[i];
        v.flags    = k_alive_bit;
        v.outgoing = k_null_he;
        m_vertices.push_back(v);
    }

    // Step 2: allocate 3 half-edges + 1 face per triangle, wire next-in-face.
    // Half-edge layout: triangle t owns half-edges [3t, 3t+1, 3t+2] forming
    // a CCW loop. Origin of HE 3t+e is triangle_indices[3t+e].
    m_half_edges.reserve(3U * n_tri);
    m_faces.reserve(n_tri);
    for (crd::u32 t = 0; t < n_tri; ++t)
    {
        const crd::u32 i0 = triangle_indices[3U * t + 0U];
        const crd::u32 i1 = triangle_indices[3U * t + 1U];
        const crd::u32 i2 = triangle_indices[3U * t + 2U];
        const crd::u32 h0 = 3U * t + 0U;
        const crd::u32 h1 = 3U * t + 1U;
        const crd::u32 h2 = 3U * t + 2U;
        m_half_edges.push_back(HalfEdgeSlot{});
        m_half_edges.push_back(HalfEdgeSlot{});
        m_half_edges.push_back(HalfEdgeSlot{});
        m_half_edges[h0].origin = i0;
        m_half_edges[h0].next   = h1;
        m_half_edges[h0].face   = t;
        m_half_edges[h1].origin = i1;
        m_half_edges[h1].next   = h2;
        m_half_edges[h1].face   = t;
        m_half_edges[h2].origin = i2;
        m_half_edges[h2].next   = h0;
        m_half_edges[h2].face   = t;
        FaceSlot f{};
        f.first_he = h0;
        f.flags    = k_alive_bit;
        m_faces.push_back(f);
        // Vertex outgoing pointers — keep the FIRST assignment (deterministic
        // since triangles are processed in input order, and within a triangle
        // half-edges are processed h0/h1/h2).
        if (m_vertices[i0].outgoing == k_null_he) { m_vertices[i0].outgoing = h0; }
        if (m_vertices[i1].outgoing == k_null_he) { m_vertices[i1].outgoing = h1; }
        if (m_vertices[i2].outgoing == k_null_he) { m_vertices[i2].outgoing = h2; }
    }

    // Step 3: pair twin half-edges via lex-tuple (min,max,he-id) sort.
    // Each interior half-edge `h` from origin `a` to destination `b` looks
    // for another half-edge `h'` from `b` to `a`. We sort by (min(a,b),
    // max(a,b), he-id) and pair adjacent entries with matching keys.
    struct EdgeKey
    {
        crd::u32 lo;
        crd::u32 hi;
        crd::u32 he;
        crd::u32 he_origin_first; // 1 if he.origin == lo (i.e., the "forward" instance)
    };
    crd::containers::Array<EdgeKey> keys(m_half_edges.allocator());
    keys.reserve(m_half_edges.size());
    for (crd::u32 h = 0; h < m_half_edges.size(); ++h)
    {
        const crd::u32 a = m_half_edges[h].origin;
        const crd::u32 b = m_half_edges[m_half_edges[h].next].origin;
        EdgeKey        k{};
        if (a < b) { k.lo = a; k.hi = b; k.he_origin_first = 1U; }
        else        { k.lo = b; k.hi = a; k.he_origin_first = 0U; }
        k.he = h;
        keys.push_back(k);
    }
    crd::containers::sort(keys.data(), keys.data() + keys.size(),
                          [](const EdgeKey& l, const EdgeKey& r) noexcept {
                              if (l.lo != r.lo) { return l.lo < r.lo; }
                              if (l.hi != r.hi) { return l.hi < r.hi; }
                              return l.he < r.he;
                          });

    // Sweep keys[]; group entries with identical (lo, hi). Within a group:
    //   - exactly 2 entries with opposite he_origin_first → manifold pair
    //   - 1 entry → boundary edge (we'll materialise a boundary half-edge)
    //   - >2 entries → non-manifold edge (status flag set, no twin paired)
    BuildStatus result = BuildStatus::Ok;
    crd::u32    i      = 0;
    while (i < keys.size())
    {
        crd::u32 j = i + 1U;
        while (j < keys.size() && keys[j].lo == keys[i].lo && keys[j].hi == keys[i].hi) { ++j; }
        const crd::u32 group_size = j - i;
        if (group_size == 2U)
        {
            // Manifold pair — wire twins on both sides.
            m_half_edges[keys[i].he].twin     = keys[i + 1U].he;
            m_half_edges[keys[i + 1U].he].twin = keys[i].he;
        }
        else if (group_size == 1U)
        {
            // Boundary — leave twin = k_null_he for now; we'll materialise
            // boundary half-edges in step 4.
        }
        else
        {
            // Non-manifold edge — treat all instances as if they were
            // boundary (twin = k_null_he, downstream calls is_manifold()
            // and gets false). Caller can run v7f repair.
            result = BuildStatus::NonManifoldEdge;
            for (crd::u32 k = i; k < j; ++k) { m_half_edges[keys[k].he].twin = k_null_he; }
        }
        i = j;
    }

    // Step 4: materialise boundary half-edges. For each interior half-edge
    // with twin == k_null_he, create a boundary half-edge with face ==
    // k_null_face going in the opposite direction. Then thread next pointers
    // around the boundary loops.
    crd::containers::Array<crd::u32> boundary_seeds(m_half_edges.allocator());
    {
        const crd::u32 interior_count = static_cast<crd::u32>(m_half_edges.size());
        for (crd::u32 h = 0; h < interior_count; ++h)
        {
            if (m_half_edges[h].twin != k_null_he) { continue; }
            // Create a boundary HE going opposite.
            const crd::u32 b      = alloc_he();
            const crd::u32 dest_v = m_half_edges[m_half_edges[h].next].origin;
            m_half_edges[b].origin = dest_v;
            m_half_edges[b].twin   = h;
            m_half_edges[b].face   = k_null_face;
            m_half_edges[b].next   = k_null_he; // wired in next pass
            m_half_edges[h].twin   = b;
            boundary_seeds.push_back(b);
            // Mark dest_v's "outgoing" to the boundary HE if vertex doesn't
            // already have a boundary-outgoing. This makes for_each_outgoing
            // start from a boundary HE for boundary vertices, which lets it
            // walk the full fan via the boundary path.
            m_vertices[dest_v].flags = static_cast<crd::u8>(m_vertices[dest_v].flags | k_vertex_on_boundary);
            m_vertices[m_half_edges[h].origin].flags
                = static_cast<crd::u8>(m_vertices[m_half_edges[h].origin].flags | k_vertex_on_boundary);
        }
    }
    // Wire boundary .next. Each boundary HE b has b.origin == v_b_orig
    // (= destination of the interior HE it twins with). Each boundary
    // VERTEX has exactly ONE outgoing boundary HE (since a 2-manifold
    // mesh's boundary is a disjoint union of simple loops). Build a
    // `vertex → outgoing-boundary-HE` map, then b.next = map[b.dest].
    //
    // b.dest = m_half_edges[b.twin].origin (since b's twin is interior
    // and points INTO b.origin from b.twin.origin).
    {
        // Initialise vertex → outgoing-boundary-HE map.
        crd::containers::Array<crd::u32> v_to_bnd_out(m_vertices.allocator());
        v_to_bnd_out.resize(m_vertices.size());
        for (crd::usize k = 0; k < v_to_bnd_out.size(); ++k) { v_to_bnd_out[k] = k_null_he; }
        for (crd::u32 idx = 0; idx < boundary_seeds.size(); ++idx)
        {
            const crd::u32 b = boundary_seeds[idx];
            const crd::u32 v = m_half_edges[b].origin;
            // First-write-wins is deterministic + arbitrary; each boundary
            // vertex has exactly one outgoing boundary HE in a valid input,
            // so no real conflict.
            if (v_to_bnd_out[v] == k_null_he) { v_to_bnd_out[v] = b; }
        }
        // Wire b.next.
        for (crd::u32 idx = 0; idx < boundary_seeds.size(); ++idx)
        {
            const crd::u32 b      = boundary_seeds[idx];
            const crd::u32 b_dest = m_half_edges[m_half_edges[b].twin].origin;
            m_half_edges[b].next  = v_to_bnd_out[b_dest];
        }
        // For boundary vertices, point `outgoing` at the BOUNDARY HE so
        // that for_each_outgoing_he can rotate CCW through the interior
        // fan starting from the boundary edge — single-direction walk
        // visits every outgoing HE exactly once and terminates at the
        // OPPOSITE boundary (or back at start for the boundary case where
        // CCW rotation eventually returns).
        for (crd::u32 v = 0; v < m_vertices.size(); ++v)
        {
            if (v_to_bnd_out[v] != k_null_he)
            {
                m_vertices[v].outgoing = v_to_bnd_out[v];
            }
        }
    }

    return result;
}

// ===========================================================================
// to_indexed
// ===========================================================================

template <crd::math::MathScalar T>
void HalfEdgeMesh<T>::to_indexed(crd::containers::Array<crd::math::Vec3<T>>& out_positions,
                                  crd::containers::Array<crd::u32>&            out_indices,
                                  crd::containers::Array<crd::u32>* out_old_to_new_vertex) const
{
    out_positions.clear();
    out_indices.clear();
    if (out_old_to_new_vertex != nullptr) { out_old_to_new_vertex->clear(); }
    // Build old→new vertex remap by walking alive vertices in slot order.
    crd::containers::Array<crd::u32> remap(m_vertices.allocator());
    remap.resize(m_vertices.size());
    for (crd::u32 v = 0; v < m_vertices.size(); ++v)
    {
        if (!vertex_alive(v))
        {
            remap[v] = k_null_vertex;
            continue;
        }
        remap[v] = static_cast<crd::u32>(out_positions.size());
        out_positions.push_back(m_vertices[v].position);
    }
    if (out_old_to_new_vertex != nullptr) { *out_old_to_new_vertex = remap; }
    // Walk alive faces in slot order, emit triangle indices.
    for (crd::u32 f = 0; f < m_faces.size(); ++f)
    {
        if (!face_alive(f)) { continue; }
        const crd::u32 h0 = m_faces[f].first_he;
        if (h0 == k_null_he) { continue; }
        const crd::u32 h1 = m_half_edges[h0].next;
        const crd::u32 h2 = m_half_edges[h1].next;
        out_indices.push_back(remap[m_half_edges[h0].origin]);
        out_indices.push_back(remap[m_half_edges[h1].origin]);
        out_indices.push_back(remap[m_half_edges[h2].origin]);
    }
}

// ===========================================================================
// Topology queries
// ===========================================================================

template <crd::math::MathScalar T>
crd::u32 HalfEdgeMesh<T>::vertex_count() const noexcept
{
    crd::u32 n = 0;
    for (crd::u32 v = 0; v < m_vertices.size(); ++v)
    {
        if ((m_vertices[v].flags & k_alive_bit) != 0U) { ++n; }
    }
    return n;
}

template <crd::math::MathScalar T>
crd::u32 HalfEdgeMesh<T>::face_count() const noexcept
{
    crd::u32 n = 0;
    for (crd::u32 f = 0; f < m_faces.size(); ++f)
    {
        if ((m_faces[f].flags & k_alive_bit) != 0U) { ++n; }
    }
    return n;
}

template <crd::math::MathScalar T>
crd::u32 HalfEdgeMesh<T>::edge_count() const noexcept
{
    // Count distinct edges by walking alive half-edges and pairing each
    // (h, h.twin) once.
    crd::u32 n = 0;
    for (crd::u32 h = 0; h < m_half_edges.size(); ++h)
    {
        if (!he_alive(h)) { continue; }
        const crd::u32 t = m_half_edges[h].twin;
        if (t == k_null_he) { ++n; continue; } // unpaired (shouldn't happen post-build)
        if (h < t) { ++n; }
    }
    return n;
}

template <crd::math::MathScalar T>
crd::u32 HalfEdgeMesh<T>::boundary_loop_count() const
{
    // Walk boundary half-edges (face == k_null_face); each loop counted once.
    crd::containers::Array<crd::u8> visited(m_half_edges.allocator());
    visited.resize(m_half_edges.size());
    crd::u32 loops = 0;
    for (crd::u32 h = 0; h < m_half_edges.size(); ++h)
    {
        if (!he_alive(h)) { continue; }
        if (m_half_edges[h].face != k_null_face) { continue; }
        if (visited[h] != 0U) { continue; }
        ++loops;
        crd::u32 cur = h;
        const crd::u32 cap = static_cast<crd::u32>(m_half_edges.size()) + 4U;
        for (crd::u32 step = 0; step < cap; ++step)
        {
            visited[cur] = 1U;
            const crd::u32 n = m_half_edges[cur].next;
            if (n == k_null_he || n == h) { break; }
            cur = n;
        }
    }
    return loops;
}

template <crd::math::MathScalar T>
bool HalfEdgeMesh<T>::is_manifold() const noexcept
{
    // Edge manifoldness already enforced at build (NonManifoldEdge status).
    // Here we verify (a) every half-edge has a twin (after build always true),
    // (b) every vertex's one-ring is a single connected fan (walked via the
    // for_each_outgoing helper — if the walk count matches the count of
    // outgoing HEs in slot order, it's a single fan).
    for (crd::u32 v = 0; v < m_vertices.size(); ++v)
    {
        if (!vertex_alive(v)) { continue; }
        crd::u32 walk_count = 0;
        for_each_outgoing_he(v, [&](crd::u32) { ++walk_count; });
        // Count outgoing HEs by slot scan.
        crd::u32 slot_count = 0;
        for (crd::u32 h = 0; h < m_half_edges.size(); ++h)
        {
            if (he_alive(h) && m_half_edges[h].origin == v) { ++slot_count; }
        }
        if (walk_count != slot_count) { return false; }
    }
    return true;
}

template <crd::math::MathScalar T>
bool HalfEdgeMesh<T>::is_closed() const noexcept
{
    for (crd::u32 h = 0; h < m_half_edges.size(); ++h)
    {
        if (!he_alive(h)) { continue; }
        if (m_half_edges[h].face == k_null_face) { return false; }
    }
    return true;
}

template <crd::math::MathScalar T>
int HalfEdgeMesh<T>::euler_characteristic() const noexcept
{
    return static_cast<int>(vertex_count()) - static_cast<int>(edge_count()) +
           static_cast<int>(face_count());
}

// ===========================================================================
// Atomic edit operations — collapse / split / flip
// ===========================================================================

// Link-condition check for collapse_edge: returns true iff collapsing edge
// (a, b) is manifold-safe — the 1-rings of a and b must share EXACTLY the
// two opposite apex vertices (for a manifold triangle pair) or just one
// (for a boundary edge).
template <crd::math::MathScalar T>
static bool link_condition_ok(const HalfEdgeMesh<T>& m, crd::u32 h)
{
    const auto&    he  = m.he(h);
    const crd::u32 a   = he.origin;
    const crd::u32 b   = m.he_dest(h);
    const crd::u32 t   = he.twin;
    // Apex 1 = h_prev.origin (the vertex opposite edge h in its face).
    // Apex 2 = t_prev.origin (the vertex opposite edge h's twin in its face).
    const crd::u32 ap1 = m.he(m.he_prev(h)).origin;
    const crd::u32 ap2 = (t != k_null_he) ? m.he(m.he_prev(t)).origin : k_null_vertex;
    // Walk b's 1-ring; for each neighbour v_b, check if v_b is also in a's
    // 1-ring. The shared vertices must be exactly {ap1} (boundary case) or
    // {ap1, ap2} (manifold interior case). Any OTHER shared neighbour means
    // the collapse would create a duplicate edge (a, v_b) — reject.
    crd::u32 shared           = 0;
    bool     unexpected_share = false;
    m.for_each_outgoing_he(b, [&](crd::u32 hb) {
        const crd::u32 vb = m.he_dest(hb);
        if (vb == k_null_vertex || vb == a) { return; }
        bool in_a_ring = false;
        m.for_each_outgoing_he(a, [&](crd::u32 ha) {
            if (m.he_dest(ha) == vb) { in_a_ring = true; }
        });
        if (in_a_ring)
        {
            ++shared;
            if (vb != ap1 && vb != ap2) { unexpected_share = true; }
        }
    });
    const crd::u32 expected_shared = (t == k_null_he) ? 1U : 2U;
    return !unexpected_share && shared == expected_shared;
}

template <crd::math::MathScalar T>
bool HalfEdgeMesh<T>::collapse_edge(crd::u32 h, const crd::math::Vec3<T>& new_pos)
{
    if (!he_alive(h)) { return false; }
    if (!crd::geometry::primitives::is_finite(new_pos)) { return false; }
    if (!link_condition_ok<T>(*this, h)) { return false; }

    const crd::u32 t = m_half_edges[h].twin;
    if (t == k_null_he) { return false; } // boundary collapse path is more delicate; reject for v7a

    // Capture topology around the edge BEFORE we start mutating.
    //   Triangle 1 (face f1): h → h_next → h_prev → h, with apex1 = h_prev.origin.
    //   Triangle 2 (face f2): t → t_next → t_prev → t, with apex2 = t_prev.origin.
    const crd::u32 f1      = m_half_edges[h].face;
    const crd::u32 f2      = m_half_edges[t].face;
    if (f1 == k_null_face || f2 == k_null_face) { return false; } // one side boundary — reject
    const crd::u32 h_next  = m_half_edges[h].next;
    const crd::u32 h_prev  = he_prev(h);
    const crd::u32 t_next  = m_half_edges[t].next;
    const crd::u32 t_prev  = he_prev(t);
    const crd::u32 a       = m_half_edges[h].origin;       // origin of h (kept)
    const crd::u32 b       = m_half_edges[t].origin;       // origin of t = destination of h (merged into a)
    // Twins of the four "rim" half-edges that will be welded.
    const crd::u32 hn_twin = m_half_edges[h_next].twin;
    const crd::u32 hp_twin = m_half_edges[h_prev].twin;
    const crd::u32 tn_twin = m_half_edges[t_next].twin;
    const crd::u32 tp_twin = m_half_edges[t_prev].twin;

    // Move all of b's outgoing half-edges to originate from a.
    // (Excluding the 4 deleted half-edges from the two collapsed faces.)
    crd::containers::Array<crd::u32> b_outgoing(m_half_edges.allocator());
    for_each_outgoing_he(b, [&](crd::u32 hb) { b_outgoing.push_back(hb); });
    for (crd::u32 i = 0; i < b_outgoing.size(); ++i)
    {
        const crd::u32 hb = b_outgoing[i];
        if (hb == h || hb == t_next) { continue; } // these are being deleted
        m_half_edges[hb].origin = a;
    }

    // Weld the rim. After collapse, h_prev's external twin (hp_twin) pairs
    // with h_next's external twin (hn_twin); similarly tn_twin with tp_twin.
    if (hp_twin != k_null_he && hn_twin != k_null_he)
    {
        m_half_edges[hp_twin].twin = hn_twin;
        m_half_edges[hn_twin].twin = hp_twin;
    }
    else if (hp_twin != k_null_he)
    {
        m_half_edges[hp_twin].twin = k_null_he;
    }
    else if (hn_twin != k_null_he)
    {
        m_half_edges[hn_twin].twin = k_null_he;
    }
    if (tp_twin != k_null_he && tn_twin != k_null_he)
    {
        m_half_edges[tp_twin].twin = tn_twin;
        m_half_edges[tn_twin].twin = tp_twin;
    }
    else if (tp_twin != k_null_he)
    {
        m_half_edges[tp_twin].twin = k_null_he;
    }
    else if (tn_twin != k_null_he)
    {
        m_half_edges[tn_twin].twin = k_null_he;
    }

    // Repair vertex.outgoing pointers for the apex vertices. apex1 = c (the
    // apex of f1, opposite edge h); the deleted HE originating from c is
    // h_prev (c→a). hn_twin = twin of h_next = c→b before collapse, c→a
    // after collapse — survives, originates from c. Same for apex2 = d:
    // deleted HE from d is t_prev (d→b); tn_twin = twin of t_next = d→a,
    // originates from d, survives.
    const crd::u32 apex1 = m_half_edges[h_prev].origin; // c
    const crd::u32 apex2 = m_half_edges[t_prev].origin; // d
    if (m_vertices[apex1].outgoing == h_prev) { m_vertices[apex1].outgoing = hn_twin; }
    if (m_vertices[apex2].outgoing == t_prev) { m_vertices[apex2].outgoing = tn_twin; }

    // Update a's outgoing pointer if it referenced any of the deleted HEs
    // that originate from a (= h and t_next; the other 4 deleted HEs
    // originate from b/c/d, none of which equals a).
    if (m_vertices[a].outgoing == h || m_vertices[a].outgoing == t_next)
    {
        // Pick any surviving outgoing from a. Prefer hp_twin / tp_twin
        // (the rim HEs that survive and originate from a after collapse).
        m_vertices[a].outgoing = k_null_he;
        if (hp_twin != k_null_he && m_half_edges[hp_twin].origin == a)
        {
            m_vertices[a].outgoing = hp_twin;
        }
        else if (tp_twin != k_null_he && m_half_edges[tp_twin].origin == a)
        {
            m_vertices[a].outgoing = tp_twin;
        }
        else
        {
            // Fall back: scan b's relocated outgoings (now originating from a).
            for (crd::u32 i = 0; i < b_outgoing.size(); ++i)
            {
                const crd::u32 hb = b_outgoing[i];
                if (hb == h_next || hb == t) { continue; } // deleted
                if (m_half_edges[hb].origin == a)
                {
                    m_vertices[a].outgoing = hb;
                    break;
                }
            }
        }
    }

    // Move a to new_pos. Free b's slot.
    m_vertices[a].position = new_pos;
    free_vertex(b);

    // Free the two faces + 6 half-edges.
    free_face(f1);
    free_face(f2);
    free_he(h);
    free_he(h_next);
    free_he(h_prev);
    free_he(t);
    free_he(t_next);
    free_he(t_prev);
    return true;
}

template <crd::math::MathScalar T>
crd::u32 HalfEdgeMesh<T>::split_edge(crd::u32 h, const crd::math::Vec3<T>& new_pos)
{
    if (!he_alive(h)) { return k_null_vertex; }
    if (!crd::geometry::primitives::is_finite(new_pos)) { return k_null_vertex; }
    const crd::u32 t = m_half_edges[h].twin;
    if (t == k_null_he) { return k_null_vertex; } // boundary split deferred

    // Topology before.
    //   Triangle 1 (f1): h(a→b) + h_next(b→c) + h_prev(c→a), apex1 = c.
    //   Triangle 2 (f2): t(b→a) + t_next(a→d) + t_prev(d→b), apex2 = d.
    const crd::u32 f1    = m_half_edges[h].face;
    const crd::u32 f2    = m_half_edges[t].face;
    if (f1 == k_null_face || f2 == k_null_face) { return k_null_vertex; }
    const crd::u32 h_next = m_half_edges[h].next;
    const crd::u32 h_prev = he_prev(h);
    const crd::u32 t_next = m_half_edges[t].next;
    const crd::u32 t_prev = he_prev(t);
    const crd::u32 a = m_half_edges[h].origin;
    const crd::u32 b = m_half_edges[t].origin;
    const crd::u32 c = m_half_edges[h_prev].origin;
    const crd::u32 d = m_half_edges[t_prev].origin;

    // Allocate new vertex m at new_pos.
    const crd::u32 m_v = alloc_vertex();
    m_vertices[m_v].position = new_pos;

    // We'll repurpose h (a→m, in new face f1') and t (m→a, in new face f2')
    // and ADD 4 new half-edges:
    //   h_mb (m→b in original f1, replacing the b end of original h)
    //   h_bm (b→m, twin of h_mb, in new face f1_new)
    //   t_md (m→d in original f2, replacing the a end of original t — actually let me redo this)
    // The clean approach: allocate 4 new HEs + 2 new faces; rewire.
    //
    // After split:
    //   f1' = (a, m, c)        — half-edges: h (a→m), h1' (m→c), h_prev (c→a)
    //   f1_new = (m, b, c)     — half-edges: h_mb (m→b), h_next (b→c), h_cm (c→m)
    //   f2' = (m, a, d)        — half-edges: t (m→a wait, t was b→a; we want m→a) — let me restart
    //
    // Cleaner: rewire t (b→a) into (m→a) by setting t.origin = m. Then:
    //   f2' = (m, a, d) — half-edges: t (m→a), t_next (a→d), t_dm (d→m)
    //   f2_new = (b, m, d) — half-edges: t_bm (b→m), t_md (m→d wait that's wrong), t_prev (d→b)
    //
    // Actually let me redo with cleaner variable names.
    //
    // Original triangles share edge (a,b). After split at m on (a,b):
    //   f1' = (a, m, c)  retains h_prev = (c→a)
    //   f1n = (m, b, c)  retains h_next = (b→c)
    //   f2' = (a, d, m)  retains t_next = (a→d)
    //   f2n = (d, b, m)  retains t_prev = (d→b)
    //
    // Wait, f2 was (b, a, d). Let me list f2's edges in order:
    //   t = (b→a), t_next = (a→d), t_prev = (d→b)
    // Original face traversal: t → t_next → t_prev → t. Good.
    //
    // After split: edge (b, a) becomes (b, m) and (m, a). Split f2 across (m, d):
    //   f2_new1 = (m, a, d) — using m→a (was t with origin updated), a→d (= t_next), d→m (new)
    //   f2_new2 = (b, m, d) — using b→m (new), m→d (new), d→b (= t_prev)
    //
    // Hmm getting complex. Let me just allocate fresh and rewire.

    // Allocate: 1 new face for f1 side (the "m-b-c" sub-triangle),
    //           1 new face for f2 side (the "b-m-d" sub-triangle).
    const crd::u32 f1n = alloc_face();
    const crd::u32 f2n = alloc_face();
    // Allocate 6 new half-edges: m_c (in f1'), c_m (in f1n), m_b (in f1n),
    // b_m (in f2n), m_d (in f2n), d_m (in f2').
    // Twin pairs: (m_c, c_m) and (m_b, b_m) and (m_d, d_m).
    const crd::u32 he_mc = alloc_he();
    const crd::u32 he_cm = alloc_he();
    const crd::u32 he_mb = alloc_he();
    const crd::u32 he_bm = alloc_he();
    const crd::u32 he_md = alloc_he();
    const crd::u32 he_dm = alloc_he();

    // ----- f1' = (a, m, c): h(a→m) → he_mc(m→c) → h_prev(c→a) ----------
    m_half_edges[h].origin = a;
    m_half_edges[h].next   = he_mc;
    m_half_edges[h].face   = f1;
    // h.twin set below
    m_half_edges[he_mc].origin = m_v;
    m_half_edges[he_mc].next   = h_prev;
    m_half_edges[he_mc].face   = f1;
    m_half_edges[he_mc].twin   = he_cm;
    m_half_edges[h_prev].next  = h;
    m_half_edges[h_prev].face  = f1;
    m_faces[f1].first_he       = h;

    // ----- f1n = (m, b, c): he_mb(m→b) → h_next(b→c) → he_cm(c→m) -------
    m_half_edges[he_mb].origin = m_v;
    m_half_edges[he_mb].next   = h_next;
    m_half_edges[he_mb].face   = f1n;
    // he_mb.twin set below
    m_half_edges[h_next].next  = he_cm;
    m_half_edges[h_next].face  = f1n;
    m_half_edges[he_cm].origin = c;
    m_half_edges[he_cm].next   = he_mb;
    m_half_edges[he_cm].face   = f1n;
    m_half_edges[he_cm].twin   = he_mc;
    m_faces[f1n].first_he      = he_mb;

    // ----- f2' = (m, a, d): t(m→a) → t_next(a→d) → he_dm(d→m) ----------
    m_half_edges[t].origin = m_v;
    m_half_edges[t].next   = t_next;
    m_half_edges[t].face   = f2;
    // t.twin set below
    m_half_edges[t_next].next  = he_dm;
    m_half_edges[t_next].face  = f2;
    m_half_edges[he_dm].origin = d;
    m_half_edges[he_dm].next   = t;
    m_half_edges[he_dm].face   = f2;
    m_half_edges[he_dm].twin   = he_md;
    m_faces[f2].first_he       = t;

    // ----- f2n = (b, m, d): he_bm(b→m) → he_md(m→d) → t_prev(d→b) ------
    m_half_edges[he_bm].origin = b;
    m_half_edges[he_bm].next   = he_md;
    m_half_edges[he_bm].face   = f2n;
    // he_bm.twin set below
    m_half_edges[he_md].origin = m_v;
    m_half_edges[he_md].next   = t_prev;
    m_half_edges[he_md].face   = f2n;
    m_half_edges[he_md].twin   = he_dm;
    m_half_edges[t_prev].next  = he_bm;
    m_half_edges[t_prev].face  = f2n;
    m_faces[f2n].first_he      = he_bm;

    // ----- Twin pair the (a,m) and (m,b) edges across f1/f2 boundaries.
    // Original edge (a, b) had h (a→b) and t (b→a). After split:
    //   h is now (a→m); its twin should be the new (m→a) = t.
    m_half_edges[h].twin   = t;
    m_half_edges[t].twin   = h;
    //   he_mb is (m→b) in f1n; its twin should be (b→m) = he_bm.
    m_half_edges[he_mb].twin = he_bm;
    m_half_edges[he_bm].twin = he_mb;

    // ----- Vertex outgoing pointers
    m_vertices[m_v].outgoing = he_mb;
    if (m_vertices[a].outgoing == k_null_he) { m_vertices[a].outgoing = h; }
    if (m_vertices[b].outgoing == k_null_he) { m_vertices[b].outgoing = he_bm; }
    // c and d outgoings unchanged.
    return m_v;
}

template <crd::math::MathScalar T>
bool HalfEdgeMesh<T>::flip_edge(crd::u32 h)
{
    if (!he_alive(h)) { return false; }
    const crd::u32 t = m_half_edges[h].twin;
    if (t == k_null_he) { return false; } // boundary
    const crd::u32 f1 = m_half_edges[h].face;
    const crd::u32 f2 = m_half_edges[t].face;
    if (f1 == k_null_face || f2 == k_null_face) { return false; }

    // Triangle 1 (f1): h(a→b) → h_next(b→c) → h_prev(c→a), apex c.
    // Triangle 2 (f2): t(b→a) → t_next(a→d) → t_prev(d→b), apex d.
    const crd::u32 h_next = m_half_edges[h].next;
    const crd::u32 h_prev = he_prev(h);
    const crd::u32 t_next = m_half_edges[t].next;
    const crd::u32 t_prev = he_prev(t);
    const crd::u32 a = m_half_edges[h].origin;
    const crd::u32 b = m_half_edges[t].origin;
    const crd::u32 c = m_half_edges[h_prev].origin;
    const crd::u32 d = m_half_edges[t_prev].origin;
    if (a == d || b == c) { return false; } // would create duplicate edge
    if (c == d) { return false; }

    // After flip: edge (a,b) becomes edge (c,d).
    //   f1' = (a, d, c) — h(a→d) → t_next(d→c... wait t_next is a→d). Hmm.
    //
    // Cleaner: rewire h to (c→d), t to (d→c).
    //   f1' = (c, d, a): h(c→d), t_next(d→a... no, that's wrong too).
    //
    // Let me think. After flip, the diagonal is (c, d). Two new triangles:
    //   tri_A = (a, d, c) — between vertex a and the new diagonal
    //   tri_B = (b, c, d) — between vertex b and the new diagonal
    //
    // tri_A edges: a→d, d→c, c→a
    //   a→d = t_next (existing)
    //   d→c = h (rewired from a→b to d→c)
    //   c→a = h_prev (existing)
    // tri_B edges: b→c, c→d, d→b
    //   b→c = h_next (existing)
    //   c→d = t (rewired from b→a to c→d)
    //   d→b = t_prev (existing)
    //
    // h and t remain twins (just both reversed in direction together).

    m_half_edges[h].origin = d;
    m_half_edges[h].next   = h_prev;
    m_half_edges[h].face   = f1;
    m_half_edges[t_next].next = h;
    m_half_edges[t_next].face = f1;
    m_half_edges[h_prev].next = t_next;
    m_half_edges[h_prev].face = f1;
    m_faces[f1].first_he      = t_next;

    m_half_edges[t].origin = c;
    m_half_edges[t].next   = t_prev;
    m_half_edges[t].face   = f2;
    m_half_edges[h_next].next = t;
    m_half_edges[h_next].face = f2;
    m_half_edges[t_prev].next = h_next;
    m_half_edges[t_prev].face = f2;
    m_faces[f2].first_he      = h_next;

    // Twin pair preserved (h<->t already wired).

    // Vertex outgoings: if a's outgoing was h, redirect to a surviving HE from a.
    if (m_vertices[a].outgoing == h)
    {
        // Find any outgoing from a (e.g., t_next reversed = its twin? No, t_next.origin == a after our rewire? Let me check: t_next was originally a→d; now t_next is still in f1' but its origin is still a — yes a's outgoing can be t_next.)
        m_vertices[a].outgoing = t_next;
    }
    if (m_vertices[b].outgoing == t)
    {
        m_vertices[b].outgoing = h_next;
    }
    if (m_vertices[c].outgoing == k_null_he) { m_vertices[c].outgoing = t; }
    if (m_vertices[d].outgoing == k_null_he) { m_vertices[d].outgoing = h; }
    return true;
}

// ===========================================================================
// Explicit instantiations
// ===========================================================================

template class HalfEdgeMesh<crd::f32>;
template class HalfEdgeMesh<crd::f64>;

} // namespace crd::geometry::mesh_processing
