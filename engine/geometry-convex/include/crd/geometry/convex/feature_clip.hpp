#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-convex — Sutherland-Hodgman polygon clipping + face/edge
// feature enumeration (Phase 3.1.7 v2j; ADR-0076 §4 pin #14).
//
// The *manifold-builder* substrate. v2c/v2d ship single-point contacts
// (EPA witnesses, SAT witnesses); eylem v1d-manifold turns those into
// 4-point PhysX/Jolt-grade rectangles by:
//
//   1. `closest_face_index(shape_a, contact_normal_a_local)` — pick the
//       reference face on A (the face whose outward normal best aligns
//       with the contact normal).
//   2. `closest_face_index(shape_b, -contact_normal_b_local)` — pick the
//       incident face on B (the face whose outward normal best opposes
//       the contact normal).
//   3. Build the incident face's polygon vertex-list (in world space).
//   4. `clip_against_convex_volume(incident, side_planes_of_reference_face)`
//       — clip the incident polygon by the reference face's *side* planes
//       (4 planes per face for an OBB), producing the manifold polygon.
//   5. Keep only vertices behind the reference plane and reduce to ≤4
//       contact points (eylem `manifold_reduce`).
//
// **Determinism contract** (inherits ADR-0063 / ADR-0076 §4 pin #14):
//
//   - **Face ordering** (OBB): `+X, -X, +Y, -Y, +Z, -Z` ⇒ indices 0..5.
//   - **Face vertex ordering** (OBB, CCW from outside): pinned to the
//     existing `tests/geometry-convex/test_hill_climb.cpp` convention —
//     +X = (4, 5, 7, 6), -X = (0, 2, 3, 1), +Y = (2, 6, 7, 3), etc. A
//     static_assert in the implementation cross-checks against the same
//     pattern hand-built by the hull-test fixture.
//   - **Edge ordering** (OBB, 12 edges): sorted by (v0, v1) ascending,
//     v0 < v1 within each edge. Axis-X edges (vary bit 2) → axis-Y
//     (vary bit 1) → axis-Z (vary bit 0).
//   - **Face-pick tiebreak**: `closest_face_index` picks the face
//     maximising `dot(face_normal, direction)`. Ties within
//     `k_parallel_epsilon<T>()` break to the *lowest* face_index.
//   - **Sutherland-Hodgman bit-determinism**: the lerp form is pinned to
//     `t = sd_i / (sd_i - sd_{i+1})`, `out = v_i + t * (v_{i+1} - v_i)`
//     — NOT `v_i*(1-t) + v_{i+1}*t`. The two forms differ by a rounding
//     step that breaks bit-equality at the seam between adjacent clipping
//     planes (a polygon vertex emitted by plane k's exit becomes an input
//     to plane k+1 — if both planes intersect there, both must compute
//     the same vertex bit-for-bit).
//
// **Plane convention** — same as `ConvexHullView::faces`:
// `dot(normal, x) + d ≤ 0` is INSIDE; `> 0` is OUTSIDE. Vertices exactly
// on the plane (signed_distance == 0) are treated as inside (the half-
// space is closed).
//
// **`is_smooth(Shape)` semantic** — "should I face-clip?". Sphere and
// Capsule3 return `true` (no face features to clip against); OBB3 and
// ConvexHullView return `false`. Manifold builders bypass the face-clip
// path when either input shape `is_smooth` and instead emit a 1-point
// manifold from the EPA/SAT witnesses. The capsule's spine is reached
// separately via `enumerate_spine(Capsule3)` which returns a `Segment3`.
//
// **All output features are in the shape's *host frame*** (the frame in
// which `center`/`orientation` of an OBB, or `vertices` of a hull, are
// expressed). The caller transforms to world space via the shape's
// `Transform`. v1d-manifold composes the transform once per shape pair;
// this keeps the substrate frame-policy-free.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/static_array.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/primitives/constants.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>

namespace crd::geometry::convex
{
using crd::math::MathScalar;
using crd::math::Vec3;
using crd::geometry::primitives::Capsule3;
using crd::geometry::primitives::ConvexHullView;
using crd::geometry::primitives::OBB3;
using crd::geometry::primitives::Plane;
using crd::geometry::primitives::Segment3;
using crd::geometry::primitives::Sphere;

// ----- Sentinels -----------------------------------------------------------

inline constexpr crd::u8 k_invalid_face_u8 = static_cast<crd::u8>(0xFFU);
inline constexpr crd::u32 k_invalid_face_u32 = ~crd::u32{0};

// ----- Feature types -------------------------------------------------------

// OBB face — 4 vertices in OBB host frame (center + orientation already
// applied). `plane.normal` is the outward unit normal; `plane.d` satisfies
// `dot(normal, vertices[k]) + d == 0` for every k.
template <MathScalar T> struct ObbFaceFeature
{
    Plane<T> plane;
    crd::containers::StaticArray<Vec3<T>, 4> vertices;
    crd::u8 face_index = k_invalid_face_u8;
};

// ConvexHullView face — a non-owning slice of `hull.face_vertex_indices`
// CCW from outside (the cooker writes them this way; v1h documents).
// `plane` is the cooked outward plane (= `hull.faces[face_index]`).
template <MathScalar T> struct HullFaceFeature
{
    Plane<T> plane;
    crd::containers::ConstSpan<crd::u32> vertex_indices;
    crd::u32 face_index = k_invalid_face_u32;
};

// Edge feature — two vertex indices + two adjacent face indices. For OBB:
// indices are into the 8 cube corners (the same packing as `support()` —
// `idx = (sx<<2)|(sy<<1)|sz` with sign bit set for "+"). For hull:
// indices are into `hull.vertices` and `hull.faces`.
struct EdgeFeature
{
    crd::u32 v0 = ~crd::u32{0};
    crd::u32 v1 = ~crd::u32{0};
    crd::u32 face_a = k_invalid_face_u32;
    crd::u32 face_b = k_invalid_face_u32;
};

// ----- is_smooth -----------------------------------------------------------

template <MathScalar T> [[nodiscard]] constexpr bool is_smooth(const Sphere<T>&) noexcept
{
    return true;
}

template <MathScalar T> [[nodiscard]] constexpr bool is_smooth(const OBB3<T>&) noexcept
{
    return false;
}

template <MathScalar T> [[nodiscard]] constexpr bool is_smooth(const Capsule3<T>&) noexcept
{
    return true;
}

template <MathScalar T> [[nodiscard]] constexpr bool is_smooth(const ConvexHullView<T>&) noexcept
{
    return false;
}

// ----- OBB feature enumeration ---------------------------------------------

namespace feature_detail
{
// OBB face → 4 cube-corner indices, CCW from outside. Pinned to the
// `test_hill_climb.cpp` `CubeHullWithAdjacency` convention.
inline constexpr crd::u32 k_obb_face_corner_table[6][4] = {
    {4U, 5U, 7U, 6U}, // 0: +X
    {0U, 2U, 3U, 1U}, // 1: -X
    {2U, 6U, 7U, 3U}, // 2: +Y
    {0U, 1U, 5U, 4U}, // 3: -Y
    {1U, 3U, 7U, 5U}, // 4: +Z
    {0U, 4U, 6U, 2U}  // 5: -Z
};

// OBB edge → {v0, v1, face_a, face_b}. Sorted by (v0, v1) ascending,
// with v0 < v1 in each row. Axis-X edges (4) → Y (4) → Z (4).
inline constexpr crd::u32 k_obb_edge_table[12][4] = {
    // Axis-X edges (vary bit 2, share bits 1 & 0).
    {0U, 4U, 3U, 5U}, // (-Y, -Z)
    {1U, 5U, 3U, 4U}, // (-Y, +Z)
    {2U, 6U, 2U, 5U}, // (+Y, -Z)
    {3U, 7U, 2U, 4U}, // (+Y, +Z)
    // Axis-Y edges (vary bit 1, share bits 2 & 0).
    {0U, 2U, 1U, 5U}, // (-X, -Z)
    {1U, 3U, 1U, 4U}, // (-X, +Z)
    {4U, 6U, 0U, 5U}, // (+X, -Z)
    {5U, 7U, 0U, 4U}, // (+X, +Z)
    // Axis-Z edges (vary bit 0, share bits 2 & 1).
    {0U, 1U, 1U, 3U}, // (-X, -Y)
    {2U, 3U, 1U, 2U}, // (-X, +Y)
    {4U, 5U, 0U, 3U}, // (+X, -Y)
    {6U, 7U, 0U, 2U}  // (+X, +Y)
};

// Compute the world-frame position of cube corner `i` of the OBB
// (using OBB's own center+orientation; no extra transform applied).
template <MathScalar T> [[nodiscard]] inline Vec3<T> obb_corner(const OBB3<T>& obb, crd::u32 i) noexcept
{
    const T sx = (i & 4U) ? obb.half_extents.x : -obb.half_extents.x;
    const T sy = (i & 2U) ? obb.half_extents.y : -obb.half_extents.y;
    const T sz = (i & 1U) ? obb.half_extents.z : -obb.half_extents.z;
    return Vec3<T>(
        obb.center.x + obb.orientation.c0.x * sx + obb.orientation.c1.x * sy + obb.orientation.c2.x * sz,
        obb.center.y + obb.orientation.c0.y * sx + obb.orientation.c1.y * sy + obb.orientation.c2.y * sz,
        obb.center.z + obb.orientation.c0.z * sx + obb.orientation.c1.z * sy + obb.orientation.c2.z * sz);
}

// OBB face_index → outward normal + plane offset, in OBB host frame.
template <MathScalar T> [[nodiscard]] inline Plane<T> obb_face_plane(const OBB3<T>& obb, crd::u8 face) noexcept
{
    Vec3<T> n;
    T h_along_n;
    switch (face)
    {
    case 0: n = obb.orientation.c0;  h_along_n = obb.half_extents.x; break;
    case 1: n = -obb.orientation.c0; h_along_n = obb.half_extents.x; break;
    case 2: n = obb.orientation.c1;  h_along_n = obb.half_extents.y; break;
    case 3: n = -obb.orientation.c1; h_along_n = obb.half_extents.y; break;
    case 4: n = obb.orientation.c2;  h_along_n = obb.half_extents.z; break;
    default: n = -obb.orientation.c2; h_along_n = obb.half_extents.z; break;
    }
    // A point on the face: obb.center + h_along_n * n. Plane offset d
    // satisfies dot(n, p) + d == 0, so d = -dot(n, center) - h_along_n.
    return Plane<T>(n, -crd::math::dot(n, obb.center) - h_along_n);
}

} // namespace feature_detail

template <MathScalar T>
[[nodiscard]] inline crd::containers::StaticArray<ObbFaceFeature<T>, 6>
enumerate_faces(const OBB3<T>& obb) noexcept
{
    crd::containers::StaticArray<ObbFaceFeature<T>, 6> out{};
    for (crd::u8 f = 0; f < 6U; ++f)
    {
        ObbFaceFeature<T>& face = out[f];
        face.face_index = f;
        face.plane = feature_detail::obb_face_plane(obb, f);
        for (crd::u32 k = 0U; k < 4U; ++k)
        {
            face.vertices[k] = feature_detail::obb_corner(obb, feature_detail::k_obb_face_corner_table[f][k]);
        }
    }
    return out;
}

[[nodiscard]] inline crd::containers::StaticArray<EdgeFeature, 12> enumerate_edges_obb() noexcept
{
    crd::containers::StaticArray<EdgeFeature, 12> out{};
    for (crd::u32 e = 0U; e < 12U; ++e)
    {
        out[e].v0 = feature_detail::k_obb_edge_table[e][0];
        out[e].v1 = feature_detail::k_obb_edge_table[e][1];
        out[e].face_a = feature_detail::k_obb_edge_table[e][2];
        out[e].face_b = feature_detail::k_obb_edge_table[e][3];
    }
    return out;
}

// ----- Capsule spine -------------------------------------------------------

template <MathScalar T>
[[nodiscard]] constexpr Segment3<T> enumerate_spine(const Capsule3<T>& cap) noexcept
{
    return Segment3<T>(cap.a, cap.b);
}

// ----- ConvexHullView feature enumeration ----------------------------------

template <MathScalar T>
inline void enumerate_faces(const ConvexHullView<T>& hull,
                            crd::containers::Array<HullFaceFeature<T>>& out) noexcept
{
    out.clear();
    const crd::usize face_count = hull.faces.size();
    out.reserve(face_count);
    for (crd::u32 f = 0; f < static_cast<crd::u32>(face_count); ++f)
    {
        HullFaceFeature<T> hf{};
        hf.face_index = f;
        hf.plane = hull.faces[f];
        const crd::u32 begin = hull.face_vertex_offsets[f];
        const crd::u32 end = hull.face_vertex_offsets[f + 1U];
        hf.vertex_indices = crd::containers::ConstSpan<crd::u32>(
            hull.face_vertex_indices.data() + begin, static_cast<crd::usize>(end - begin));
        out.push_back(hf);
    }
}

template <MathScalar T>
inline void enumerate_edges(const ConvexHullView<T>& hull, crd::containers::Array<EdgeFeature>& out) noexcept
{
    // Walk all face vertex-pairs as directed edges. Each undirected edge
    // appears exactly twice in a closed manifold polytope: (v0→v1) in face
    // F_a and (v1→v0) in face F_b. Match them by reversed-direction pair,
    // and emit one EdgeFeature with v0 < v1.
    //
    // O(E²) — fine for hulls up to ~100 vertices / ~300 edges; eylem
    // colliders cap below this. v3 cooker (`crd-convex` Quickhull) can
    // emit edges directly when this matters.
    out.clear();

    struct Dir
    {
        crd::u32 v0;
        crd::u32 v1;
        crd::u32 face;
    };
    crd::containers::Array<Dir> directed(out.allocator());

    const crd::u32 face_count = static_cast<crd::u32>(hull.faces.size());
    for (crd::u32 f = 0U; f < face_count; ++f)
    {
        const crd::u32 begin = hull.face_vertex_offsets[f];
        const crd::u32 end = hull.face_vertex_offsets[f + 1U];
        const crd::u32 n = end - begin;
        for (crd::u32 k = 0U; k < n; ++k)
        {
            Dir d;
            d.v0 = hull.face_vertex_indices[begin + k];
            d.v1 = hull.face_vertex_indices[begin + (k + 1U) % n];
            d.face = f;
            directed.push_back(d);
        }
    }

    out.reserve(directed.size() / 2U);
    const crd::usize dn = directed.size();
    for (crd::usize i = 0U; i < dn; ++i)
    {
        if (directed[i].face == k_invalid_face_u32)
        {
            continue; // already matched
        }
        // Search for the reverse-direction pair anywhere in `directed` (the
        // match may be at j < i, since we walk faces in order and the two
        // halves of an edge live in different faces). Scan all positions
        // except `i`.
        crd::usize match = ~crd::usize{0};
        for (crd::usize j = 0U; j < dn; ++j)
        {
            if (j == i)
            {
                continue;
            }
            if (directed[j].face == k_invalid_face_u32)
            {
                continue;
            }
            if (directed[j].v0 == directed[i].v1 && directed[j].v1 == directed[i].v0)
            {
                match = j;
                break;
            }
        }
        // Non-manifold hull: edge appears in only one face. Assert in debug;
        // skip silently in release.
        CRD_ASSERT(match != ~crd::usize{0});
        if (match == ~crd::usize{0})
        {
            directed[i].face = k_invalid_face_u32;
            continue;
        }
        // Emit with v0 < v1 (deterministic canonical form). Assign
        // face_a/face_b to match the (v0->v1) and (v1->v0) directions
        // respectively.
        EdgeFeature ef;
        if (directed[i].v0 < directed[i].v1)
        {
            ef.v0 = directed[i].v0;
            ef.v1 = directed[i].v1;
            ef.face_a = directed[i].face;
            ef.face_b = directed[match].face;
        }
        else
        {
            ef.v0 = directed[i].v1;
            ef.v1 = directed[i].v0;
            ef.face_a = directed[match].face;
            ef.face_b = directed[i].face;
        }
        out.push_back(ef);
        directed[i].face = k_invalid_face_u32;
        directed[match].face = k_invalid_face_u32;
    }
}

// ----- closest_face_index --------------------------------------------------

// Picks the face whose outward normal best aligns with `direction_local`
// (i.e., maximises `dot(face_normal, direction_local)`). The argument
// frame matches the shape's host frame — for an OBB, `direction_local`
// is in the same frame as `obb.orientation`'s columns; for a hull,
// in the same frame as `hull.faces[i].normal`.
//
// Tiebreak: lowest `face_index` wins when two dot products are within
// `k_parallel_epsilon<T>()` of each other.
template <MathScalar T>
[[nodiscard]] inline crd::u8 closest_face_index(const OBB3<T>& obb, const Vec3<T>& direction_local) noexcept
{
    crd::u8 best = 0U;
    T best_dot = crd::math::dot(obb.orientation.c0, direction_local);
    const T axis_y = crd::math::dot(obb.orientation.c1, direction_local);
    const T axis_z = crd::math::dot(obb.orientation.c2, direction_local);
    const T candidates[6] = {best_dot, -best_dot, axis_y, -axis_y, axis_z, -axis_z};
    best_dot = candidates[0];
    for (crd::u8 i = 1U; i < 6U; ++i)
    {
        // Strict > with epsilon — equal-within-eps keeps the earlier index
        // (lower face_index wins on ties).
        if (candidates[i] > best_dot + crd::geometry::primitives::k_parallel_epsilon<T>())
        {
            best = i;
            best_dot = candidates[i];
        }
    }
    return best;
}

template <MathScalar T>
[[nodiscard]] inline crd::u32 closest_face_index(const ConvexHullView<T>& hull, const Vec3<T>& direction) noexcept
{
    const crd::u32 face_count = static_cast<crd::u32>(hull.faces.size());
    CRD_ASSERT(face_count > 0U);
    crd::u32 best = 0U;
    T best_dot = crd::math::dot(hull.faces[0].normal, direction);
    for (crd::u32 i = 1U; i < face_count; ++i)
    {
        const T d = crd::math::dot(hull.faces[i].normal, direction);
        if (d > best_dot + crd::geometry::primitives::k_parallel_epsilon<T>())
        {
            best = i;
            best_dot = d;
        }
    }
    return best;
}

// ----- Sutherland-Hodgman convex polygon clipping --------------------------

// Clip `input` (a convex CCW polygon in 3D) against the half-space
// `dot(plane.normal, x) + plane.d ≤ 0`. Output is CCW, possibly empty,
// possibly with one extra vertex (an N-vertex polygon clipped by one
// plane can produce at most N+1 vertices, though for convex inputs the
// usual bound is `min(N + 1, N_after_clip)`). `output` is cleared first;
// caller's allocator is used.
//
// Bit-determinism: the intersection lerp form is pinned to
// `t = sd_i / (sd_i - sd_{i+1})`, `out = v_i + t * (v_{i+1} - v_i)`. Do
// not switch to `(1-t)*v_i + t*v_{i+1}` — different rounding breaks
// vertex equality across adjacent clipping planes (a vertex emitted as
// "exit" by plane k must equal the vertex emitted as "entry" by plane
// k+1 if both planes cross at the same point).
template <MathScalar T>
inline void clip_convex_polygon(crd::containers::ConstSpan<Vec3<T>> input,
                                const Plane<T>& clipping_plane,
                                crd::containers::Array<Vec3<T>>& output) noexcept
{
    output.clear();
    const crd::usize n = input.size();
    if (n == 0U)
    {
        return;
    }

    Vec3<T> prev = input[n - 1U];
    T sd_prev = crd::math::dot(clipping_plane.normal, prev) + clipping_plane.d;

    for (crd::usize i = 0U; i < n; ++i)
    {
        const Vec3<T> curr = input[i];
        const T sd_curr = crd::math::dot(clipping_plane.normal, curr) + clipping_plane.d;

        const bool prev_inside = sd_prev <= static_cast<T>(0);
        const bool curr_inside = sd_curr <= static_cast<T>(0);

        if (curr_inside)
        {
            if (!prev_inside)
            {
                // Crossing into the half-space — emit intersection then curr.
                const T t = sd_prev / (sd_prev - sd_curr);
                output.push_back(prev + (curr - prev) * t);
            }
            output.push_back(curr);
        }
        else if (prev_inside)
        {
            // Crossing out of the half-space — emit intersection only.
            const T t = sd_prev / (sd_prev - sd_curr);
            output.push_back(prev + (curr - prev) * t);
        }
        // else: both outside — emit nothing.

        prev = curr;
        sd_prev = sd_curr;
    }
}

// Multi-plane intersection (clip against a convex volume). Caller
// supplies both buffers — function ping-pongs between them, leaving the
// final result in `output`. Both buffers are cleared before use; no
// hidden allocation. Empty result short-circuits remaining planes.
template <MathScalar T>
inline void clip_against_convex_volume(crd::containers::ConstSpan<Vec3<T>> input,
                                       crd::containers::ConstSpan<Plane<T>> planes,
                                       crd::containers::Array<Vec3<T>>& output,
                                       crd::containers::Array<Vec3<T>>& scratch) noexcept
{
    output.clear();
    scratch.clear();

    if (input.size() == 0U)
    {
        return;
    }
    if (planes.size() == 0U)
    {
        // No clipping planes — output = input verbatim.
        for (crd::usize i = 0U; i < input.size(); ++i)
        {
            output.push_back(input[i]);
        }
        return;
    }

    crd::containers::Array<Vec3<T>>* curr = &output;
    crd::containers::Array<Vec3<T>>* next = &scratch;

    // Seed curr with input.
    for (crd::usize i = 0U; i < input.size(); ++i)
    {
        curr->push_back(input[i]);
    }

    for (crd::usize p = 0U; p < planes.size(); ++p)
    {
        if (curr->empty())
        {
            break;
        }
        clip_convex_polygon<T>(crd::containers::ConstSpan<Vec3<T>>(curr->data(), curr->size()), planes[p], *next);
        crd::containers::Array<Vec3<T>>* tmp = curr;
        curr = next;
        next = tmp;
        next->clear();
    }

    // If the final result is in `scratch`, copy it back into `output`.
    if (curr != &output)
    {
        output.clear();
        for (crd::usize i = 0U; i < curr->size(); ++i)
        {
            output.push_back((*curr)[i]);
        }
    }
}

} // namespace crd::geometry::convex
