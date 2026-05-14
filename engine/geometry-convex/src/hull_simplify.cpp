// ---------------------------------------------------------------------------
// crd-geometry-convex — convex hull simplification (Phase 3.1.7 v3d).
//
// See `hull_simplify.hpp` for the algorithm + invariants. Quickhull output
// is always triangulated (every face has 3 vertices); v3d operates only on
// the non-degenerate triangulated case. Coplanar / colinear / coincident
// sources are returned unchanged (already minimal).
// ---------------------------------------------------------------------------

#include <crd/geometry/convex/hull_simplify.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/primitives/constants.hpp>
#include <crd/geometry/primitives/predicates.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>
#include <limits>

namespace crd::geometry::convex
{
namespace hull_simplify_detail
{
using crd::math::MathScalar;
using crd::math::Vec3;
using crd::geometry::primitives::Plane;

// Constant: a u32 sentinel for "no such index". The hull will never have
// 2^32 - 1 vertices.
inline constexpr crd::u32 kInvalidIndex = ~crd::u32{0};

// =========================================================================
// Geometric helpers
// =========================================================================

template <MathScalar T>
[[nodiscard]] Vec3<T> tri_outward_normal_unit(const Vec3<T>& a, const Vec3<T>& b, const Vec3<T>& c,
                                               const Vec3<T>& outward_hint) noexcept
{
    // Compute (b - a) × (c - a); orient so dot(normal, outward_hint) >= 0.
    const Vec3<T> ab = b - a;
    const Vec3<T> ac = c - a;
    Vec3<T> n;
    n.x = ab.y * ac.z - ab.z * ac.y;
    n.y = ab.z * ac.x - ab.x * ac.z;
    n.z = ab.x * ac.y - ab.y * ac.x;
    const T len_sq = n.x * n.x + n.y * n.y + n.z * n.z;
    if (len_sq <= static_cast<T>(0))
    {
        // Degenerate triangle — fall back to outward_hint.
        return outward_hint;
    }
    const T inv_len = static_cast<T>(1) / std::sqrt(len_sq);
    n.x *= inv_len;
    n.y *= inv_len;
    n.z *= inv_len;
    if (n.x * outward_hint.x + n.y * outward_hint.y + n.z * outward_hint.z < static_cast<T>(0))
    {
        n.x = -n.x;
        n.y = -n.y;
        n.z = -n.z;
    }
    return n;
}

template <MathScalar T>
[[nodiscard]] T signed_distance_to_plane(const Vec3<T>& point, const Vec3<T>& a, const Vec3<T>& b,
                                          const Vec3<T>& c, const Vec3<T>& outward_hint) noexcept
{
    const Vec3<T> n = tri_outward_normal_unit(a, b, c, outward_hint);
    return n.x * (point.x - a.x) + n.y * (point.y - a.y) + n.z * (point.z - a.z);
}

// =========================================================================
// Working state for a single simplification run
// =========================================================================

template <MathScalar T> struct SimplifyState
{
    // Source data (immutable after construction).
    crd::containers::ConstSpan<Vec3<T>> src_vertices;

    // Live state — flat-of-arrays per Cerid convention.
    crd::containers::Array<crd::u8> vertex_alive;
    crd::containers::Array<crd::u8> vertex_locked;
    crd::containers::Array<crd::u32> vertex_incident_count; // for fast pruning of zero-incident strays

    // Per-vertex incident-face list (variable length); stored as flat array
    // with prefix-sum offsets that get rebuilt after each removal. For our
    // scale (~5k vertices) a simple Array-per-vertex is the cleaner form.
    crd::containers::Array<crd::containers::Array<crd::u32>> vertex_faces;

    // Faces — alive flag + triangle vertex indices (always exactly 3 per
    // Quickhull triangulated output).
    crd::containers::Array<crd::u8> face_alive;
    crd::containers::Array<crd::u32> face_v0;
    crd::containers::Array<crd::u32> face_v1;
    crd::containers::Array<crd::u32> face_v2;

    crd::memory::IAllocator* alloc{nullptr};

    explicit SimplifyState(crd::memory::IAllocator* a) noexcept
        : vertex_alive(a), vertex_locked(a), vertex_incident_count(a), vertex_faces(a),
          face_alive(a), face_v0(a), face_v1(a), face_v2(a), alloc(a)
    {
    }
};

// Find position (0/1/2) of vertex `v` in face triangle (v0, v1, v2). Returns
// 0xFF if not found (algorithmic bug — guarded by assert).
[[nodiscard]] inline crd::u8 face_index_of(crd::u32 v, crd::u32 v0, crd::u32 v1, crd::u32 v2) noexcept
{
    if (v == v0)
    {
        return 0U;
    }
    if (v == v1)
    {
        return 1U;
    }
    if (v == v2)
    {
        return 2U;
    }
    return 0xFFU;
}

[[nodiscard]] inline crd::u32 third_vertex(crd::u32 v0, crd::u32 v1, crd::u32 v2, crd::u32 a,
                                            crd::u32 b) noexcept
{
    // Return the vertex of (v0, v1, v2) that is neither `a` nor `b`.
    if (v0 != a && v0 != b)
    {
        return v0;
    }
    if (v1 != a && v1 != b)
    {
        return v1;
    }
    return v2;
}

// Build the ring of vertex `v` as an ordered CCW-from-outside cycle. Returns
// false if the topology is malformed (non-manifold edge, non-cycle), in
// which case `v` is treated as not removable. The ordering: walking the ring
// such that consecutive (ring[i], ring[i+1]) are connected by a face triangle
// (v, ring[i], ring[i+1]) in CCW-from-outside orientation.
template <MathScalar T>
[[nodiscard]] bool build_ring(const SimplifyState<T>& st, crd::u32 v,
                                crd::containers::Array<crd::u32>& out_ring) noexcept
{
    out_ring.clear();
    const auto& inc = st.vertex_faces[v];
    if (inc.size() < 3U)
    {
        return false;
    }

    // Start with the first incident face. Find v's position k in it; the
    // walk-start ring vertex is at (k+2)%3 — the vertex that comes BEFORE
    // v in the CCW face ordering, i.e. the one we "return to" v via. The
    // next face shares the edge (v, r_start) with F0 and lies CCW-around-v
    // from F0 — which is the direction we want to walk so the resulting
    // ring is CCW around v looking from OUTSIDE (matches the outward
    // normal direction of the cap).
    const crd::u32 first_face = inc[0];
    const crd::u32 fv0 = st.face_v0[first_face];
    const crd::u32 fv1 = st.face_v1[first_face];
    const crd::u32 fv2 = st.face_v2[first_face];
    const crd::u8 k = face_index_of(v, fv0, fv1, fv2);
    if (k == 0xFFU)
    {
        return false;
    }
    const crd::u32 verts[3] = {fv0, fv1, fv2};
    const crd::u32 r_start = verts[(k + 2U) % 3U];

    out_ring.push_back(r_start);
    crd::u32 prev_face = first_face;
    crd::u32 r_curr = r_start;

    // Cap on iterations: a sane convex-hull vertex has < a few hundred
    // incident faces. Use 4× incident count as a hard cap to detect bugs.
    const crd::usize cap = inc.size() * 4U + 8U;
    for (crd::usize step = 0; step < cap; ++step)
    {
        // Find the OTHER incident face that contains edge (v, r_curr).
        crd::u32 next_face = kInvalidIndex;
        for (crd::usize i = 0; i < inc.size(); ++i)
        {
            const crd::u32 f = inc[i];
            if (f == prev_face)
            {
                continue;
            }
            const crd::u32 a = st.face_v0[f];
            const crd::u32 b = st.face_v1[f];
            const crd::u32 c = st.face_v2[f];
            if ((a == r_curr || b == r_curr || c == r_curr))
            {
                next_face = f;
                break;
            }
        }
        if (next_face == kInvalidIndex)
        {
            return false; // hit a boundary edge — non-closed ring
        }
        const crd::u32 r_next = third_vertex(st.face_v0[next_face], st.face_v1[next_face],
                                              st.face_v2[next_face], v, r_curr);
        if (r_next == r_start)
        {
            // Closed the cycle — we're done.
            return true;
        }
        // Defensive: detect duplicate visit (malformed topology).
        for (crd::usize j = 0; j < out_ring.size(); ++j)
        {
            if (out_ring[j] == r_next)
            {
                return false;
            }
        }
        out_ring.push_back(r_next);
        prev_face = next_face;
        r_curr = r_next;
    }
    return false; // ran the cap — malformed
}

// Compute "outward" direction at vertex `v`: average of incident face
// normals. Each face normal computed via cross of two edges with the
// "outward hint" disambiguator — but we need to bootstrap. The
// QuickhullResult faces have outward-CCW vertex order, so for face
// (a, b, c), normal = (b - a) × (c - a) is outward.
template <MathScalar T>
[[nodiscard]] Vec3<T> compute_outward(const SimplifyState<T>& st,
                                       crd::containers::ConstSpan<Vec3<T>> verts, crd::u32 v) noexcept
{
    Vec3<T> sum{static_cast<T>(0), static_cast<T>(0), static_cast<T>(0)};
    const auto& inc = st.vertex_faces[v];
    for (crd::usize i = 0; i < inc.size(); ++i)
    {
        const crd::u32 f = inc[i];
        const Vec3<T>& a = verts[st.face_v0[f]];
        const Vec3<T>& b = verts[st.face_v1[f]];
        const Vec3<T>& c = verts[st.face_v2[f]];
        const Vec3<T> ab = b - a;
        const Vec3<T> ac = c - a;
        Vec3<T> n;
        n.x = ab.y * ac.z - ab.z * ac.y;
        n.y = ab.z * ac.x - ab.x * ac.z;
        n.z = ab.x * ac.y - ab.y * ac.x;
        sum.x += n.x;
        sum.y += n.y;
        sum.z += n.z;
    }
    // Normalize if non-zero.
    const T len_sq = sum.x * sum.x + sum.y * sum.y + sum.z * sum.z;
    if (len_sq > static_cast<T>(0))
    {
        const T inv = static_cast<T>(1) / std::sqrt(len_sq);
        sum.x *= inv;
        sum.y *= inv;
        sum.z *= inv;
    }
    return sum;
}

// Compute shrinkage cost for vertex `v`: max signed distance from v to any
// fan-triangle plane (CCW from outside via outward_hint). Pivot is the
// lowest-index ring vertex (deterministic per ADR-0076 §4 pin #11).
// Returns +inf if the ring cannot be built (topology issue → not removable).
template <MathScalar T>
[[nodiscard]] T compute_cost(const SimplifyState<T>& st,
                              crd::containers::ConstSpan<Vec3<T>> verts, crd::u32 v) noexcept
{
    crd::containers::Array<crd::u32> ring(st.alloc);
    if (!build_ring<T>(st, v, ring) || ring.size() < 3U)
    {
        return std::numeric_limits<T>::infinity();
    }

    // Find pivot = lowest-index ring vertex.
    crd::usize pivot_pos = 0;
    for (crd::usize i = 1; i < ring.size(); ++i)
    {
        if (ring[i] < ring[pivot_pos])
        {
            pivot_pos = i;
        }
    }
    const crd::u32 pivot = ring[pivot_pos];
    const Vec3<T> outward = compute_outward<T>(st, verts, v);

    // Fan triangles: (pivot, ring[i], ring[i+1]) for i+1 in [pivot_pos+2, .., pivot_pos+ring.size()-1]
    // (modulo ring.size()) — i.e., skip the two triangles incident to pivot's two ring neighbours.
    const crd::usize n = ring.size();
    T max_dist = static_cast<T>(0);
    for (crd::usize step = 1; step + 1 < n; ++step)
    {
        const crd::usize i = (pivot_pos + step) % n;
        const crd::usize j = (pivot_pos + step + 1U) % n;
        if (i == pivot_pos || j == pivot_pos)
        {
            continue;
        }
        const Vec3<T>& a = verts[pivot];
        const Vec3<T>& b = verts[ring[i]];
        const Vec3<T>& c = verts[ring[j]];
        const T d = std::abs(signed_distance_to_plane<T>(verts[v], a, b, c, outward));
        if (d > max_dist)
        {
            max_dist = d;
        }
    }
    return max_dist;
}

// Check that removing v with the fan triangulation (pivot-rooted, ring
// order) yields a still-convex hull. For each new fan triangle, every
// OTHER live vertex must lie on the inside side (orient3d ≥ 0 per
// Shewchuk "below = positive" with CCW-from-outside vertex order).
template <MathScalar T>
[[nodiscard]] bool check_convexity(const SimplifyState<T>& st,
                                     crd::containers::ConstSpan<Vec3<T>> verts, crd::u32 v,
                                     const crd::containers::Array<crd::u32>& ring) noexcept
{
    // Find pivot = lowest-index ring vertex.
    crd::usize pivot_pos = 0;
    for (crd::usize i = 1; i < ring.size(); ++i)
    {
        if (ring[i] < ring[pivot_pos])
        {
            pivot_pos = i;
        }
    }
    const Vec3<T> outward = compute_outward<T>(st, verts, v);

    const crd::usize n = ring.size();
    const crd::usize vcount = verts.size();
    const T eps = crd::geometry::primitives::k_distance_epsilon<T>();

    for (crd::usize step = 1; step + 1 < n; ++step)
    {
        const crd::usize i = (pivot_pos + step) % n;
        const crd::usize j = (pivot_pos + step + 1U) % n;
        if (i == pivot_pos || j == pivot_pos)
        {
            continue;
        }
        const crd::u32 ia = ring[pivot_pos];
        const crd::u32 ib = ring[i];
        const crd::u32 ic = ring[j];

        // For each other live vertex, signed distance to fan plane must be
        // <= eps (i.e., inside or on the plane). The fan plane's outward
        // normal is the one aligned with `outward`.
        for (crd::u32 q = 0; q < vcount; ++q)
        {
            if (q == v || q == ia || q == ib || q == ic)
            {
                continue;
            }
            if (!st.vertex_alive[q])
            {
                continue;
            }
            const T sd = signed_distance_to_plane<T>(verts[q], verts[ia], verts[ib], verts[ic], outward);
            if (sd > eps)
            {
                return false;
            }
        }
    }
    return true;
}

// Apply removal of vertex `v`: drop incident faces, append fan-triangulation
// faces, update vertex_faces adjacency. `ring` is the pre-built ring (cycle
// order); `pivot_pos` is computed inside (= lowest-index position).
template <MathScalar T>
void apply_removal(SimplifyState<T>& st, crd::u32 v, const crd::containers::Array<crd::u32>& ring) noexcept
{
    crd::usize pivot_pos = 0;
    for (crd::usize i = 1; i < ring.size(); ++i)
    {
        if (ring[i] < ring[pivot_pos])
        {
            pivot_pos = i;
        }
    }
    const crd::u32 pivot = ring[pivot_pos];

    // Drop incident faces.
    for (crd::usize fi = 0; fi < st.vertex_faces[v].size(); ++fi)
    {
        const crd::u32 f = st.vertex_faces[v][fi];
        if (!st.face_alive[f])
        {
            continue;
        }
        st.face_alive[f] = 0U;
        // Decrement incident counts for the other two vertices.
        const crd::u32 v0 = st.face_v0[f];
        const crd::u32 v1 = st.face_v1[f];
        const crd::u32 v2 = st.face_v2[f];
        const crd::u32 others[2] = {v == v0 ? v1 : v0, v == v2 ? v1 : v2};
        for (crd::u32 ov : others)
        {
            // Remove `f` from ov's incident list.
            auto& list = st.vertex_faces[ov];
            for (crd::usize k = 0; k < list.size(); ++k)
            {
                if (list[k] == f)
                {
                    list[k] = list[list.size() - 1U];
                    list.pop_back();
                    break;
                }
            }
        }
    }
    st.vertex_faces[v].clear();
    st.vertex_alive[v] = 0U;

    // Append new fan triangles (pivot, ring[i], ring[i+1]).
    const crd::usize n = ring.size();
    for (crd::usize step = 1; step + 1 < n; ++step)
    {
        const crd::usize i = (pivot_pos + step) % n;
        const crd::usize j = (pivot_pos + step + 1U) % n;
        if (i == pivot_pos || j == pivot_pos)
        {
            continue;
        }
        const crd::u32 ia = pivot;
        const crd::u32 ib = ring[i];
        const crd::u32 ic = ring[j];

        const crd::u32 new_f = static_cast<crd::u32>(st.face_alive.size());
        st.face_alive.push_back(1U);
        st.face_v0.push_back(ia);
        st.face_v1.push_back(ib);
        st.face_v2.push_back(ic);
        st.vertex_faces[ia].push_back(new_f);
        st.vertex_faces[ib].push_back(new_f);
        st.vertex_faces[ic].push_back(new_f);
    }
}

// =========================================================================
// Main entry point
// =========================================================================

template <MathScalar T>
QuickhullResult<T> simplify_hull_impl(const QuickhullResult<T>& source,
                                       crd::memory::IAllocator* alloc,
                                       const HullSimplifyOptions<T>& opts) noexcept
{
    // Identity copy helper.
    auto identity_copy = [&]() -> QuickhullResult<T> {
        QuickhullResult<T> out(alloc);
        out.vertices.reserve(source.vertices.size());
        for (crd::usize i = 0; i < source.vertices.size(); ++i)
        {
            out.vertices.push_back(source.vertices[i]);
        }
        out.faces.reserve(source.faces.size());
        for (crd::usize i = 0; i < source.faces.size(); ++i)
        {
            out.faces.push_back(source.faces[i]);
        }
        out.face_vertex_indices.reserve(source.face_vertex_indices.size());
        for (crd::usize i = 0; i < source.face_vertex_indices.size(); ++i)
        {
            out.face_vertex_indices.push_back(source.face_vertex_indices[i]);
        }
        out.face_vertex_offsets.reserve(source.face_vertex_offsets.size());
        for (crd::usize i = 0; i < source.face_vertex_offsets.size(); ++i)
        {
            out.face_vertex_offsets.push_back(source.face_vertex_offsets[i]);
        }
        out.is_coplanar = source.is_coplanar;
        out.is_colinear = source.is_colinear;
        out.is_coincident = source.is_coincident;
        return out;
    };

    // No-op cases.
    if (source.vertices.size() < 4U || source.is_coplanar || source.is_colinear ||
        source.is_coincident)
    {
        return identity_copy();
    }
    if (opts.target_vertex_count == 0U && opts.max_error_threshold == static_cast<T>(0))
    {
        return identity_copy();
    }
    if (opts.target_vertex_count >= static_cast<crd::u32>(source.vertices.size()) &&
        opts.max_error_threshold == static_cast<T>(0))
    {
        return identity_copy();
    }

    // Bounds-check keep_vertex_indices.
    for (crd::usize i = 0; i < opts.keep_vertex_indices.size(); ++i)
    {
        CRD_ASSERT(opts.keep_vertex_indices[i] < source.vertices.size());
    }

    // Build working state.
    SimplifyState<T> st(alloc);
    const crd::usize n_verts = source.vertices.size();
    st.vertex_alive.resize(n_verts);
    st.vertex_locked.resize(n_verts);
    st.vertex_incident_count.resize(n_verts);
    st.vertex_faces.reserve(n_verts);
    for (crd::usize i = 0; i < n_verts; ++i)
    {
        st.vertex_alive[i] = 1U;
        st.vertex_locked[i] = 0U;
        st.vertex_incident_count[i] = 0U;
        st.vertex_faces.emplace_back(alloc);
    }
    for (crd::usize i = 0; i < opts.keep_vertex_indices.size(); ++i)
    {
        st.vertex_locked[opts.keep_vertex_indices[i]] = 1U;
    }

    // Quickhull triangulated output: every face has 3 vertices. The
    // face_vertex_offsets are 0, 3, 6, ... — assert that and load.
    const crd::usize n_faces = source.faces.size();
    st.face_alive.resize(n_faces);
    st.face_v0.resize(n_faces);
    st.face_v1.resize(n_faces);
    st.face_v2.resize(n_faces);
    for (crd::usize f = 0; f < n_faces; ++f)
    {
        const crd::u32 off = source.face_vertex_offsets[f];
        const crd::u32 end = source.face_vertex_offsets[f + 1U];
        if (end - off != 3U)
        {
            // Non-triangle face — only happens for coplanar-flat hulls,
            // which are excluded above. Defensive: return identity.
            return identity_copy();
        }
        const crd::u32 v0 = source.face_vertex_indices[off];
        const crd::u32 v1 = source.face_vertex_indices[off + 1U];
        const crd::u32 v2 = source.face_vertex_indices[off + 2U];
        st.face_alive[f] = 1U;
        st.face_v0[f] = v0;
        st.face_v1[f] = v1;
        st.face_v2[f] = v2;
        st.vertex_faces[v0].push_back(static_cast<crd::u32>(f));
        st.vertex_faces[v1].push_back(static_cast<crd::u32>(f));
        st.vertex_faces[v2].push_back(static_cast<crd::u32>(f));
        st.vertex_incident_count[v0]++;
        st.vertex_incident_count[v1]++;
        st.vertex_incident_count[v2]++;
    }

    crd::containers::ConstSpan<Vec3<T>> verts(source.vertices.data(), source.vertices.size());

    // Iteration loop: while we can remove a vertex within thresholds.
    crd::u32 live_count = static_cast<crd::u32>(n_verts);
    crd::containers::Array<crd::u32> ring(alloc);

    // Tombstone tracker: vertices whose convexity-check failed in the
    // current iteration — try them again only after another removal
    // (topology changed).
    crd::containers::Array<crd::u8> tombstoned(alloc);
    tombstoned.resize(n_verts);
    for (crd::usize i = 0; i < n_verts; ++i)
    {
        tombstoned[i] = 0U;
    }
    auto clear_tombstones = [&]() noexcept {
        for (crd::usize i = 0; i < n_verts; ++i)
        {
            tombstoned[i] = 0U;
        }
    };

    while (true)
    {
        if (opts.target_vertex_count != 0U && live_count <= opts.target_vertex_count)
        {
            break;
        }

        // Find lowest-cost removable candidate (linear scan — fine for our
        // working sizes; trivially upgradable to a heap if a perf consumer
        // surfaces).
        crd::u32 best_v = kInvalidIndex;
        T best_cost = std::numeric_limits<T>::infinity();
        for (crd::u32 v = 0; v < static_cast<crd::u32>(n_verts); ++v)
        {
            if (!st.vertex_alive[v] || st.vertex_locked[v] || tombstoned[v])
            {
                continue;
            }
            const T c = compute_cost<T>(st, verts, v);
            if (c < best_cost || (c == best_cost && (best_v == kInvalidIndex || v < best_v)))
            {
                best_cost = c;
                best_v = v;
            }
        }
        if (best_v == kInvalidIndex)
        {
            // No admissible removal — done.
            break;
        }
        if (opts.max_error_threshold > static_cast<T>(0) && best_cost > opts.max_error_threshold)
        {
            // Cheapest candidate exceeds threshold — done.
            break;
        }
        if (!std::isfinite(best_cost))
        {
            // No removable vertex has finite cost — done.
            break;
        }

        // Build ring + check convexity.
        if (!build_ring<T>(st, best_v, ring) || ring.size() < 3U)
        {
            tombstoned[best_v] = 1U;
            continue;
        }
        if (!check_convexity<T>(st, verts, best_v, ring))
        {
            tombstoned[best_v] = 1U;
            continue;
        }

        // Admit removal.
        apply_removal<T>(st, best_v, ring);
        --live_count;
        clear_tombstones();
    }

    // =====================================================================
    // Compact + emit fresh QuickhullResult.
    // =====================================================================
    QuickhullResult<T> out(alloc);

    // Old-vertex-index → new-vertex-index map (live vertices in ascending
    // input-index order — same convention as v3c).
    crd::containers::Array<crd::u32> old_to_new(alloc);
    old_to_new.resize(n_verts);
    crd::u32 new_idx = 0U;
    for (crd::usize i = 0; i < n_verts; ++i)
    {
        if (st.vertex_alive[i])
        {
            old_to_new[i] = new_idx++;
            out.vertices.push_back(verts[i]);
        }
        else
        {
            old_to_new[i] = kInvalidIndex;
        }
    }

    // Emit live faces: re-derive outward plane from current vertex positions
    // (the v3c face planes are stale after our removals; rebuild).
    crd::u32 emitted_offset = 0U;
    out.face_vertex_offsets.push_back(0U);
    for (crd::usize f = 0; f < st.face_alive.size(); ++f)
    {
        if (!st.face_alive[f])
        {
            continue;
        }
        const crd::u32 a = st.face_v0[f];
        const crd::u32 b = st.face_v1[f];
        const crd::u32 c = st.face_v2[f];
        const Vec3<T>& pa = verts[a];
        const Vec3<T>& pb = verts[b];
        const Vec3<T>& pc = verts[c];

        // The face's vertex order is CCW from outside — compute the normal
        // by cross product; the original v3c convention has (b-a)×(c-a)
        // pointing outward.
        const Vec3<T> ab = pb - pa;
        const Vec3<T> ac = pc - pa;
        Vec3<T> n;
        n.x = ab.y * ac.z - ab.z * ac.y;
        n.y = ab.z * ac.x - ab.x * ac.z;
        n.z = ab.x * ac.y - ab.y * ac.x;
        const T len_sq = n.x * n.x + n.y * n.y + n.z * n.z;
        if (len_sq > static_cast<T>(0))
        {
            const T inv = static_cast<T>(1) / std::sqrt(len_sq);
            n.x *= inv;
            n.y *= inv;
            n.z *= inv;
        }
        Plane<T> p;
        p.normal = n;
        p.d = -(n.x * pa.x + n.y * pa.y + n.z * pa.z);
        out.faces.push_back(p);

        out.face_vertex_indices.push_back(old_to_new[a]);
        out.face_vertex_indices.push_back(old_to_new[b]);
        out.face_vertex_indices.push_back(old_to_new[c]);
        emitted_offset += 3U;
        out.face_vertex_offsets.push_back(emitted_offset);
    }

    out.is_coplanar = false;
    out.is_colinear = false;
    out.is_coincident = false;
    return out;
}

} // namespace hull_simplify_detail

// =========================================================================
// Public API — explicit instantiations
// =========================================================================

template <crd::math::MathScalar T>
QuickhullResult<T> simplify_hull(const QuickhullResult<T>& source, crd::memory::IAllocator* alloc,
                                   const HullSimplifyOptions<T>& opts) noexcept
{
    return hull_simplify_detail::simplify_hull_impl<T>(source, alloc, opts);
}

template QuickhullResult<crd::f32> simplify_hull<crd::f32>(const QuickhullResult<crd::f32>&,
                                                            crd::memory::IAllocator*,
                                                            const HullSimplifyOptions<crd::f32>&) noexcept;
template QuickhullResult<crd::f64> simplify_hull<crd::f64>(const QuickhullResult<crd::f64>&,
                                                            crd::memory::IAllocator*,
                                                            const HullSimplifyOptions<crd::f64>&) noexcept;

} // namespace crd::geometry::convex
