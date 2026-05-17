#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-mesh-processing — v7a HalfEdgeMesh<T> substrate.
//
// The MUTABLE mesh data structure consumed by every v7 algorithm
// (QEM / Loop / remesh / hole-fill / manifoldness-repair / self-intersect
// / Taubin). This is the standard half-edge (DCEL-for-meshes) representation
// from Computational Geometry / Polygon Mesh Processing — Botsch et al.
// 2010 chapter 2, Mäntylä 1988, every production mesh-edit tool.
//
// **Half-edge convention (LOCKED):** each undirected edge is represented
// by TWO directed half-edges (twin pair). A half-edge points FROM `origin`
// TO `destination = m_he[twin(h)].origin`. The face on the LEFT of a half-
// edge (when walking from origin to destination, in 2D screen coords with
// +X right + +Y up) is `m_he[h].face`. Boundary half-edges have
// `m_he[h].face == k_null_face` and represent the OUTSIDE of an open mesh
// (boundary loops walked in the opposite direction from interior faces).
//
// **Per-half-edge fields** (16 B, `static_assert`-pinned for layout
// stability across MSVC / GCC / clang):
//   - `origin`  : u32 vertex index this half-edge starts FROM.
//   - `twin`    : u32 opposite half-edge index.
//   - `next`    : u32 next half-edge around the face (CCW).
//   - `face`    : u32 face index this half-edge bounds on its LEFT;
//                 `k_null_face` if this is a boundary half-edge.
//
// `prev` is NOT stored — it's recovered by walking `next` around the face
// (3 hops for a triangle). Storing prev would bloat each half-edge to 20 B
// and the recovery is fast enough that the memory savings dominate.
//
// **Per-vertex fields** (~24 B with `Vec3<T>` position + 8 B handle):
//   - `position` : `Vec3<T>` coordinate.
//   - `outgoing` : u32 any outgoing half-edge from this vertex (or `k_null`
//                  for isolated vertices). Used as the seed for one-ring
//                  walks via `for_each_outgoing_he`.
//   - `flags`    : u8 (alive bit + boundary-touching bit + reserved).
//
// **Per-face fields**: just `first_he` (any half-edge bounding this face)
// + `flags` (alive bit). Triangle topology is recovered by walking next.
//
// **Free-list pool design:** vertices, half-edges, faces all live in
// `crd::containers::Array` pools with separate `Array<u32>` free lists.
// Deletes mark `flags &= ~k_alive` + push onto the free list; allocates
// pop the free list first, fall back to `push_back`. Indices stay stable
// across the slot's lifetime; downstream code can hold handles across
// arbitrary edits as long as the slot itself stays alive.
//
// **Builder contract** (ADR-0076 §15 builder-reject):
//   - `build_from(positions, indices)` rejects non-finite vertex positions
//     (debug `CRD_ASSERT`) + degenerate triangles (i0==i1 etc.) + out-of-
//     bounds indices.
//   - Non-manifold edges (>2 incident faces) → status =
//     `BuildStatus::NonManifoldEdge`; partial result available for repair.
//
// **Atomic edit operations:**
//   - `collapse_edge(h, new_pos)` — merges destination into origin at
//     `new_pos`; eliminates the two faces incident on `h` and `h.twin`;
//     redirects all incoming half-edges of the destination to the origin.
//     **Link-condition check** (Edelsbrunner 2001): the collapse is valid
//     iff the link of edge(origin, dest) equals the intersection of the
//     vertex links — i.e. the two endpoints share EXACTLY the two opposite
//     vertices (`apex1` and `apex2` of the incident triangles), no more.
//     Violating this disconnects or creates non-manifold topology.
//
//   - `split_edge(h, new_pos)` — inserts a new vertex at `new_pos` on the
//     edge; each of the two incident triangles splits into two; total
//     deltas: +1 vertex, +3 half-edges (per side), +2 faces (the new
//     triangles next to each of the new sub-edges).
//
//   - `flip_edge(h)` — swaps the diagonal between the two faces sharing
//     `h`. Quad must be convex (Shewchuk-checked); rejects boundary
//     half-edges; preserves manifoldness.
//
// All three edit ops return `bool ok` (false = rejected; mesh unchanged)
// + update the affected slot indices. Stable handles for slots that
// survive the operation; deleted slots become free-list entries that
// later operations may recycle.
//
// **Determinism contract (ADR-0063 + ADR-0076 §4 pin #11):** twin pairing
// during `build_from` uses lex-tuple `(min(va, vb), max(va, vb), face_id,
// local_edge_idx)` sort. Free-list pop is LIFO (last-freed-first-reused)
// — caller-deterministic given deterministic input sequence. No
// transcendentals, no `std::sort` on FP keys.
//
// **Two-layer typed architecture (ADR-0078 §5 D34):** algorithm bodies
// stay raw `<MathScalar T>`; typed `Vec3<Length32>` consumers ride
// `half_edge_mesh_typed.hpp` strip-compute-retag wrappers at the API
// boundary (added at slice close if a typed-surface consumer pulls).
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/primitives/is_finite.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

#include <limits>

namespace crd::geometry::mesh_processing
{

constexpr crd::u32 k_null_vertex = std::numeric_limits<crd::u32>::max();
constexpr crd::u32 k_null_he     = std::numeric_limits<crd::u32>::max();
constexpr crd::u32 k_null_face   = std::numeric_limits<crd::u32>::max();

// Slot flag bits.
constexpr crd::u8 k_alive_bit            = 1U << 0;
constexpr crd::u8 k_vertex_on_boundary   = 1U << 1;

// ---- POD slot types --------------------------------------------------------

struct HalfEdgeSlot
{
    crd::u32 origin = k_null_vertex;
    crd::u32 twin   = k_null_he;
    crd::u32 next   = k_null_he;
    crd::u32 face   = k_null_face;
};

static_assert(sizeof(HalfEdgeSlot) == 16U,
              "HalfEdgeSlot must be 16 bytes (4 u32 fields). If you add fields, "
              "weigh memory cost vs the algorithm savings; per-mesh half-edge "
              "count is typically 6x triangle count — every byte multiplies.");

template <crd::math::MathScalar T>
struct VertexSlot
{
    crd::math::Vec3<T> position{};
    crd::u32           outgoing = k_null_he;
    crd::u8            flags    = 0U;
};

struct FaceSlot
{
    crd::u32 first_he = k_null_he;
    crd::u8  flags    = 0U;
};

// ---- Build / status -------------------------------------------------------

enum class BuildStatus : crd::u8
{
    Ok                  = 0,
    NonFiniteInput      = 1, // a vertex position was non-finite
    OutOfBoundsIndex    = 2, // triangle index >= positions.size()
    DegenerateTriangle  = 3, // i0==i1, i1==i2, or i2==i0 in a triangle
    NonManifoldEdge     = 4, // >2 incident faces on an edge (partial result built)
    IndicesNotMultipleOf3 = 5,
};

// ---- HalfEdgeMesh<T> ------------------------------------------------------

template <crd::math::MathScalar T>
class HalfEdgeMesh
{
public:
    explicit HalfEdgeMesh(crd::memory::IAllocator* alloc) noexcept
      : m_vertices(alloc), m_half_edges(alloc), m_faces(alloc),
        m_free_vertices(alloc), m_free_he(alloc), m_free_faces(alloc)
    {
    }

    // -- Builders ---------------------------------------------------------

    // Construct from a flat indexed triangle mesh. Returns Ok or a
    // diagnostic status; on hard errors (NonFiniteInput / OutOfBoundsIndex
    // / DegenerateTriangle / IndicesNotMultipleOf3) the mesh is left empty.
    // On `NonManifoldEdge` the mesh is built with non-manifold edges
    // flagged as boundaries (downstream caller can run v7f repair).
    [[nodiscard]] BuildStatus build_from(crd::containers::ConstSpan<crd::math::Vec3<T>> positions,
                                         crd::containers::ConstSpan<crd::u32> triangle_indices);

    // Extract back to flat indexed triangle mesh. Output arrays cleared
    // first. Alive-only — dead slots are skipped (and a side index map
    // can be requested via `out_old_to_new_vertex` if caller needs to
    // remap external attribute arrays).
    void to_indexed(crd::containers::Array<crd::math::Vec3<T>>& out_positions,
                    crd::containers::Array<crd::u32>&            out_indices,
                    crd::containers::Array<crd::u32>* out_old_to_new_vertex = nullptr) const;

    // -- Atomic edit operations (consumer-shared primitives) --------------

    // Collapse edge `h` (and its twin) — merges destination into origin at
    // `new_pos`. Returns false (mesh unchanged) if the link condition
    // fails or the collapse would create non-manifold topology.
    [[nodiscard]] bool collapse_edge(crd::u32 h, const crd::math::Vec3<T>& new_pos);

    // Split edge `h` at `new_pos`; subdivides the two incident faces.
    // Returns the new vertex's index, or `k_null_vertex` on failure.
    [[nodiscard]] crd::u32 split_edge(crd::u32 h, const crd::math::Vec3<T>& new_pos);

    // Flip the diagonal of the quad formed by the two faces sharing `h`.
    // Rejects boundary edges + non-convex quads. Returns false if invalid.
    [[nodiscard]] bool flip_edge(crd::u32 h);

    // -- Allocator handle (for v7b+ algorithms that need scratch arrays) --

    [[nodiscard]] crd::memory::IAllocator* allocator() const noexcept
    {
        return m_vertices.allocator();
    }

    // -- Mutators on stable handles --------------------------------------
    //
    // `set_vertex_position` lets v7d isotropic remeshing (and other
    // smoothing passes) apply Jacobi-style position updates without going
    // through `to_indexed → build_from` per iteration. Topology unchanged;
    // only the position coordinate moves. Slot id stays stable.
    void set_vertex_position(crd::u32 v, const crd::math::Vec3<T>& new_pos)
    {
        CRD_ASSERT(vertex_alive(v));
        m_vertices[v].position = new_pos;
    }

    // -- Topology queries -------------------------------------------------

    [[nodiscard]] crd::u32 vertex_count() const noexcept; // alive only
    [[nodiscard]] crd::u32 face_count() const noexcept;
    [[nodiscard]] crd::u32 edge_count() const noexcept;
    [[nodiscard]] crd::u32 boundary_loop_count() const;
    [[nodiscard]] bool     is_manifold() const noexcept;
    [[nodiscard]] bool     is_closed() const noexcept;
    [[nodiscard]] int      euler_characteristic() const noexcept; // V - E + F

    // Slot-level accessors (for v7b–v7h algorithm bodies).
    [[nodiscard]] crd::u32 he_pool_size() const noexcept   { return static_cast<crd::u32>(m_half_edges.size()); }
    [[nodiscard]] crd::u32 face_pool_size() const noexcept { return static_cast<crd::u32>(m_faces.size()); }
    [[nodiscard]] crd::u32 vertex_pool_size() const noexcept { return static_cast<crd::u32>(m_vertices.size()); }

    [[nodiscard]] const HalfEdgeSlot& he(crd::u32 i) const noexcept       { return m_half_edges[i]; }
    [[nodiscard]] const VertexSlot<T>& vertex(crd::u32 i) const noexcept  { return m_vertices[i]; }
    [[nodiscard]] const FaceSlot& face(crd::u32 i) const noexcept         { return m_faces[i]; }

    [[nodiscard]] bool he_alive(crd::u32 i) const noexcept
    {
        return i < m_half_edges.size() && m_half_edges[i].origin != k_null_vertex;
    }
    [[nodiscard]] bool vertex_alive(crd::u32 i) const noexcept
    {
        return i < m_vertices.size() && (m_vertices[i].flags & k_alive_bit) != 0U;
    }
    [[nodiscard]] bool face_alive(crd::u32 i) const noexcept
    {
        return i < m_faces.size() && (m_faces[i].flags & k_alive_bit) != 0U;
    }

    // -- Walk helpers -----------------------------------------------------

    // Invoke `fn(crd::u32 he)` for each OUTGOING half-edge from `v`. The
    // iteration order is by rotation around v (next-around-vertex =
    // twin(prev_in_face)); deterministic across runs.
    template <typename Fn>
    void for_each_outgoing_he(crd::u32 v, Fn&& fn) const;

    // Invoke `fn(crd::u32 he)` for each half-edge bounding face `f` (next
    // walk). For triangles this fires exactly 3 times.
    template <typename Fn>
    void for_each_face_he(crd::u32 f, Fn&& fn) const;

    // Convenience helpers (constexpr where useful).
    [[nodiscard]] crd::u32 he_dest(crd::u32 h) const noexcept
    {
        const crd::u32 t = m_half_edges[h].twin;
        return (t == k_null_he) ? k_null_vertex : m_half_edges[t].origin;
    }
    [[nodiscard]] crd::u32 he_prev(crd::u32 h) const noexcept
    {
        // For a triangle face, prev(h) is just next(next(h)) — exactly 2
        // hops. For arbitrary polygons we'd walk until next() points back
        // to h; since v7a stores ONLY triangle meshes, we hard-code 2 hops.
        // (When v8+ adds n-gon support we'll generalise.)
        const crd::u32 n1 = m_half_edges[h].next;
        if (n1 == k_null_he) { return k_null_he; }
        return m_half_edges[n1].next;
    }

    [[nodiscard]] bool he_is_boundary(crd::u32 h) const noexcept
    {
        return m_half_edges[h].face == k_null_face;
    }

private:
    // Slot allocators (free-list-first).
    [[nodiscard]] crd::u32 alloc_vertex() noexcept;
    [[nodiscard]] crd::u32 alloc_he() noexcept;
    [[nodiscard]] crd::u32 alloc_face() noexcept;
    void                   free_vertex(crd::u32 v) noexcept;
    void                   free_he(crd::u32 h) noexcept;
    void                   free_face(crd::u32 f) noexcept;

    crd::containers::Array<VertexSlot<T>> m_vertices;
    crd::containers::Array<HalfEdgeSlot>  m_half_edges;
    crd::containers::Array<FaceSlot>      m_faces;
    crd::containers::Array<crd::u32>      m_free_vertices;
    crd::containers::Array<crd::u32>      m_free_he;
    crd::containers::Array<crd::u32>      m_free_faces;
};

// ---- Templated inline implementations (walks must be header-only) ---------

template <crd::math::MathScalar T>
template <typename Fn>
void HalfEdgeMesh<T>::for_each_outgoing_he(crd::u32 v, Fn&& fn) const
{
    if (!vertex_alive(v)) { return; }
    const crd::u32 start = m_vertices[v].outgoing;
    if (start == k_null_he) { return; }
    // Uniform CW rotation: `next outgoing = cur.twin.next`. Works the same
    // way for interior and boundary HEs because:
    //   - interior cur: cur.twin is INCOMING to v (in a different face);
    //     cur.twin.next is the next outgoing in that face (the next CW
    //     outgoing slot at v).
    //   - boundary cur (face == k_null_face): cur.twin is interior INCOMING
    //     to v; cur.twin.next is the next interior outgoing at v.
    // For boundary vertices, build_from points `outgoing` at the boundary
    // HE, so the walk starts at the boundary, rotates through the interior
    // fan, and closes when the next CW outgoing equals `start` (which
    // happens when we walk all interior outgoings and the LAST interior's
    // twin.next points back to the starting boundary HE via the boundary
    // loop's next pointer).
    crd::u32       cur = start;
    const crd::u32 cap = static_cast<crd::u32>(m_half_edges.size()) + 4U;
    for (crd::u32 step = 0; step < cap; ++step)
    {
        fn(cur);
        const crd::u32 t = m_half_edges[cur].twin;
        if (t == k_null_he) { break; }
        const crd::u32 nxt = m_half_edges[t].next;
        if (nxt == k_null_he) { break; }
        if (nxt == start) { return; } // wrapped — full one-ring complete
        cur = nxt;
    }
}

template <crd::math::MathScalar T>
template <typename Fn>
void HalfEdgeMesh<T>::for_each_face_he(crd::u32 f, Fn&& fn) const
{
    if (!face_alive(f)) { return; }
    const crd::u32 start = m_faces[f].first_he;
    if (start == k_null_he) { return; }
    crd::u32       cur   = start;
    const crd::u32 cap   = 8U; // triangle = 3 hops; cap defensively at 8 in case of arbitrary polygon
    for (crd::u32 step = 0; step < cap; ++step)
    {
        fn(cur);
        cur = m_half_edges[cur].next;
        if (cur == k_null_he || cur == start) { break; }
    }
}

} // namespace crd::geometry::mesh_processing
