#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-convex — 3D convex hull via Quickhull (Phase 3.1.7 v3c;
// ADR-0076 §4 pin #11, §18).
//
// Computes the 3D convex hull of a point set per Barber, Dobkin & Huhdanpaa
// 1996 ("The Quickhull Algorithm for Convex Hulls"). Output is a
// `QuickhullResult<T>` that carries owned arrays of:
//   - hull vertices (a subset of the input, in input order — but only the
//     vertices that survive on the hull boundary),
//   - outward face planes (CCW vertex order from outside),
//   - face_vertex_indices + face_vertex_offsets (prefix-sum), matching the
//     v1h `ConvexHullView<T>` layout for direct binding to v2 GJK/EPA.
//
// A free helper `convex_hull_view_of(const QuickhullResult&) → ConvexHullView`
// builds the non-owning view inline (the v2 GJK/EPA consumers reach for this).
//
// **Algorithm (Barber-Dobkin-Huhdanpaa 1996)**:
//
//   1. **Initial tetrahedron** — pick 4 spread-extremal input points and
//      verify they form a non-degenerate (orient3d != 0) tetrahedron. Use
//      v3a `orient3d` with full Stage D for the sign check, so the
//      degeneracy decision is bit-exact.
//
//   2. **Conflict-list initialisation** — assign each remaining input point
//      to the face above which it lies (orient3d > 0 — Shewchuk convention
//      "below = positive" applied to outward face normals means a point
//      "above" the outward-normal direction is OUTSIDE the face).
//
//   3. **Iteration loop**:
//      a. Pick any face with a non-empty outside-set. Within that set, find
//         the eye point: the point with the largest distance to the face's
//         plane (lowest input-index tiebreak — ADR-0076 §4 pin #11).
//      b. Identify all faces visible from the eye (DFS through face
//         adjacency, in increasing face_index order — deterministic).
//      c. Identify the horizon: edges of the visible region where visible
//         meets non-visible.
//      d. Build new faces by connecting each horizon edge to the eye point
//         (CCW from outside).
//      e. Redistribute the conflict lists of the removed faces to the new
//         faces (each "orphaned" point assigned to its outside-of new face,
//         lowest-face-index on ties).
//      f. Repeat until no face has a non-empty outside-set.
//
//   4. **Result construction** — extract the surviving faces into
//      `QuickhullResult`'s arrays. Vertex deduplication preserves input
//      index order (lowest input index per unique hull vertex).
//
// **Degenerate inputs** (each is a distinct code path):
//   - 0 points → empty `QuickhullResult` (vertices.empty(), faces.empty()).
//   - 1 point → 1 vertex, no faces.
//   - 2 distinct points → 2 vertices, no faces (a segment).
//   - 3 collinear points → 2 vertices (lex-extremes), no faces.
//   - 3 non-collinear → 1 triangle face (degenerate flat 3D hull as 2
//     copies front + back — v2 GJK still operates on this via the support
//     function).
//   - 4+ all-coplanar → fall back to v3b `convex_hull_2d` on the dominant
//     plane, then form a flat 3D hull (front + back polygon copies). The
//     `QuickhullResult::is_coplanar` flag records this.
//   - All-coincident → 1 vertex, no faces.
//
// **Determinism (ADR-0076 §4 pin #11, §18)**:
//   - All sign tests use v3a `orient3d` with full Stage D (bit-exact across
//     compilers / SIMD widths / OSes).
//   - Face traversal in increasing face_index order.
//   - Outside-set processing in increasing input-index order.
//   - "Furthest point" / "highest face_index" / "best-vertex" all break
//     ties by lowest input index.
//   - Result vertex order: in increasing original-input-index order
//     (a stable sort over the surviving hull vertex set).
//
// **Builder-reject contract (ADR-0076 §15)**: `CRD_ASSERT(all_finite(points))`
// at function entry. Release path falls through to the adaptive predicate's
// queries-tolerate behaviour (non-finite inputs → degenerate hull, no UB).
//
// **API surface**:
//   - `quickhull(points, alloc, opts)` → `QuickhullResult<T>`.
//   - `convex_hull_view_of(QuickhullResult)` → `ConvexHullView<T>` (non-
//     owning view).
//   - `enrich_for_gjk(QuickhullResult&)` — mutator that appends v2g vertex
//     adjacency + v2h SoA SIMD arrays. Optional; caller decides whether to
//     pay the build cost (~O(V log V)).
//
// **Templated on `T ∈ {f32, f64}`**. f32 callers go through f64-adaptive
// `orient3d` internally; output type matches input.
//
// **Consumers**: V-HACD v9c (decomposition), eylem `Collider::ConvexHull`
// (cooker pipeline), editor (interactive hull authoring), CAD test-data
// generation (Phase 3.1.8 `crd-brep` consumer).
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>

namespace crd::geometry::convex
{
using crd::math::MathScalar;
using crd::math::Vec2;
using crd::math::Vec3;
using crd::geometry::primitives::ConvexHullView;
using crd::geometry::primitives::Plane;

// ===========================================================================
// Result type
// ===========================================================================

// Owned-arrays form of a 3D convex hull built by Quickhull. Carries all the
// data needed to construct a `ConvexHullView<T>` (which is the non-owning
// span shape v2 GJK/EPA consume).
//
// Lifetimes: arrays own their storage; allocator passed at construction.
// Moveable but not copyable (consistent with Cerid's owning-form pattern).
template <MathScalar T> struct QuickhullResult
{
    // Hull vertices, in increasing original-input-index order (a subset of
    // the input). For a successful 3D hull: at least 4 vertices.
    crd::containers::Array<Vec3<T>> vertices;

    // Outward face planes (one per face). `plane.normal` is unit; `plane.d`
    // satisfies `dot(plane.normal, vertices[face_vertex_indices[..]]) +
    // plane.d == 0` (within Stage D arithmetic precision).
    crd::containers::Array<Plane<T>> faces;

    // CCW-from-outside vertex indices, flat array; face f owns slots
    // [face_vertex_offsets[f], face_vertex_offsets[f+1]). Indices reference
    // `vertices` (NOT the original input).
    crd::containers::Array<crd::u32> face_vertex_indices;

    // Prefix-sum offsets; size = faces.size() + 1.
    crd::containers::Array<crd::u32> face_vertex_offsets;

    // OPTIONAL — populated by `enrich_for_gjk` for v2g hill-climb support
    // and v2h SoA SIMD support paths. Empty by default; caller opts in.
    crd::containers::Array<crd::u32> vertex_adjacency_indices;
    crd::containers::Array<crd::u32> vertex_adjacency_offsets;
    crd::containers::Array<T> vx_soa;
    crd::containers::Array<T> vy_soa;
    crd::containers::Array<T> vz_soa;

    // Degeneracy markers — set when the input collapsed to a lower-dimensional
    // hull. The `ConvexHullView<T>` built from this still works with v2
    // GJK/EPA (the support function handles degenerate cases), but consumers
    // that need volumetric properties should branch on these.
    bool is_coplanar = false;  // all input points lay on a plane (flat hull)
    bool is_colinear = false;  // all input points lay on a line (degenerate)
    bool is_coincident = false; // all input points are the same point

    // Constructor: takes the allocator that owns all member arrays.
    explicit QuickhullResult(crd::memory::IAllocator* alloc) noexcept
        : vertices(alloc), faces(alloc), face_vertex_indices(alloc), face_vertex_offsets(alloc),
          vertex_adjacency_indices(alloc), vertex_adjacency_offsets(alloc), vx_soa(alloc),
          vy_soa(alloc), vz_soa(alloc)
    {
    }

    QuickhullResult(const QuickhullResult&) = delete;
    QuickhullResult& operator=(const QuickhullResult&) = delete;
    QuickhullResult(QuickhullResult&&) noexcept = default;
    QuickhullResult& operator=(QuickhullResult&&) noexcept = default;
    ~QuickhullResult() = default;

    [[nodiscard]] bool empty() const noexcept { return vertices.empty(); }
};

// ===========================================================================
// Options
// ===========================================================================

template <MathScalar T> struct QuickhullOptions
{
    // Maximum number of iterations of the main loop. The algorithm is
    // O(n log n) average and rarely exceeds n^2 / 4 iterations; this is a
    // safety bound. Default 100,000 is enough for hulls of millions of
    // input points.
    crd::u32 max_iterations = 100'000U;

    // Squared-distance tolerance below which a candidate fourth point of
    // the initial tetrahedron is considered "coplanar" with the first three
    // (triggers the v3b 2D-hull fallback). Default: f32 1e-8, f64 1e-16.
    T degenerate_tetrahedron_eps = static_cast<T>(1e-8);
};

template <> struct QuickhullOptions<crd::f64>
{
    crd::u32 max_iterations = 100'000U;
    crd::f64 degenerate_tetrahedron_eps = 1e-16;
};

// ===========================================================================
// Public API (forward declarations — implementations in `quickhull.cpp`)
// ===========================================================================

// Compute the 3D convex hull of `points`. Output is built into a
// `QuickhullResult<T>` with arrays owned by `alloc`. Empty / degenerate
// input → degenerate `QuickhullResult` with `is_coplanar` / `is_colinear` /
// `is_coincident` flag set as appropriate.
//
// `alloc` outlives the returned `QuickhullResult` (the result's arrays bind
// to it). On move, the new owner takes over.
template <MathScalar T>
[[nodiscard]] QuickhullResult<T> quickhull(crd::containers::ConstSpan<Vec3<T>> points,
                                             crd::memory::IAllocator* alloc,
                                             const QuickhullOptions<T>& opts = {}) noexcept;

// Build a non-owning `ConvexHullView<T>` from a `QuickhullResult<T>`. The
// view references the result's backing arrays; lifetime is tied to the
// result. For v2 GJK/EPA binding.
template <MathScalar T>
[[nodiscard]] inline ConvexHullView<T> convex_hull_view_of(const QuickhullResult<T>& r) noexcept
{
    ConvexHullView<T> v;
    v.vertices = crd::containers::ConstSpan<Vec3<T>>(r.vertices.data(), r.vertices.size());
    v.faces = crd::containers::ConstSpan<Plane<T>>(r.faces.data(), r.faces.size());
    v.face_vertex_indices = crd::containers::ConstSpan<crd::u32>(r.face_vertex_indices.data(),
                                                                   r.face_vertex_indices.size());
    v.face_vertex_offsets = crd::containers::ConstSpan<crd::u32>(r.face_vertex_offsets.data(),
                                                                   r.face_vertex_offsets.size());
    // v2g adjacency + v2h SoA — only set if `enrich_for_gjk` populated them.
    if (!r.vertex_adjacency_indices.empty())
    {
        v.vertex_adjacency_indices = crd::containers::ConstSpan<crd::u32>(
            r.vertex_adjacency_indices.data(), r.vertex_adjacency_indices.size());
        v.vertex_adjacency_offsets = crd::containers::ConstSpan<crd::u32>(
            r.vertex_adjacency_offsets.data(), r.vertex_adjacency_offsets.size());
    }
    // SoA fields exist only on f32 specialization of ConvexHullView; the
    // non-f32 path leaves them as default-constructed (empty spans).
    if constexpr (std::is_same_v<T, crd::f32>)
    {
        if (!r.vx_soa.empty())
        {
            v.vx_soa = crd::containers::ConstSpan<crd::f32>(r.vx_soa.data(), r.vx_soa.size());
            v.vy_soa = crd::containers::ConstSpan<crd::f32>(r.vy_soa.data(), r.vy_soa.size());
            v.vz_soa = crd::containers::ConstSpan<crd::f32>(r.vz_soa.data(), r.vz_soa.size());
        }
    }
    return v;
}

// Enrich a Quickhull result with v2g vertex adjacency + v2h SoA SIMD arrays.
// Mutates `r` in place. Cost: O(V log V) for adjacency, O(V) for SoA. Caller
// opts in by calling this; the default `quickhull(...)` result has these
// arrays empty.
template <MathScalar T> void enrich_for_gjk(QuickhullResult<T>& r) noexcept;

} // namespace crd::geometry::convex
