#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-convex — GJK distance + boolean overlap (Phase 3.1.7 v2a).
//
// "Given two convex shapes A, B with rigid transforms xform_a, xform_b, what
// is the squared distance between them and (if separated) the world-space
// closest-point witnesses on each?" When they overlap, GJK detects it with
// the same machinery — `result.overlapping == true` — and EPA (v2c) takes
// over from the terminating simplex to recover the penetration vector.
//
// Algorithm (Gilbert / Johnson / Keerthi 1988, with the Box2D / Ericson
// modernisations layered in):
//
//   1. Drive in shape A's local frame. Compute `T_BA = inv(xform_a) *
//      xform_b` ONCE; all support / direction math then happens in A's
//      local frame, with B's support points pulled across by `T_BA`.
//      ~40% faster on OBB pairs than world-frame driving (the per-iteration
//      transform of every support point was the hot inner cost).
//
//   2. Each iteration:
//        sa     = support(a, d)                          // A-local frame
//        d_b    = rotate(inv(T_BA.rotation), -d)         // -d in B-local
//        sb_b   = support(b, d_b)                        // B-local frame
//        sb_a   = transform_point(T_BA, sb_b.point)      // B-support in A
//        w      = sa.point - sb_a                        // Minkowski diff
//
//      Termination (in priority order):
//        (a) **Index-match** (Box2D pattern, ADR-0076 §4 pin #14): both
//            indices valid AND the new pair `(sa.vidx, sb_b.vidx)` is
//            already a simplex entry ⇒ converged.  No epsilon. Triggers
//            on the iteration *after* the optimal pair is found — the
//            simplex would oscillate without it. The deterministic
//            choice that makes GJK bit-exact across platforms.
//        (b) **Geometric** (fallback for analytic shapes — sphere /
//            capsule with `k_invalid_vertex`): `|d|² + d·w ≤ ε²`. Note
//            the **plus**: with `d = -v` (search direction is the
//            negated closest-point), `|v|² − v·w = |d|² + d·w` — the
//            standard "did we improve in direction d?" test.
//        (c) **Iteration cap** (32 — Bullet/Box2D match): hit ⇒ flag
//            `converged = false` and return the current best. This is
//            never reached on well-formed inputs; catches degenerate
//            cases (coincident hulls with degenerate vertex enumeration
//            etc.) without infinite looping.
//
//   3. Add `w` to the simplex. Run `sub_distance_N` (Ericson §9.5,
//      `detail/sub_distance.hpp`). If the surviving mask is `0b1111`
//      (origin inside the tetrahedron) → overlap; return with
//      `overlapping=true` and the full terminating simplex preserved
//      for EPA (v2c).
//
//   4. Otherwise, repack the surviving slots into the first
//      `popcount(mask)` positions of the simplex arrays, set
//      `d = -closest`, iterate.
//
// What `GjkResult` carries:
//   - `witness_a_world` / `witness_b_world`: the closest-point pair on the
//     two surfaces, in world space. Equal when overlapping (origin = the
//     Minkowski-difference closest point); `distance_squared = 0`.
//   - `distance_squared`: the squared distance between witnesses. Zero on
//     overlap; squared throughout so the caller `sqrt`s only when needed.
//   - `overlapping`: GJK terminated by detecting origin in the simplex hull.
//     v2b exposes this as the dedicated `overlap` entry point; here it's a
//     by-product of `gjk_distance`.
//   - `simplex_*_local`: the **terminating simplex's A-supports (in A's
//     local frame) and B-supports (in B's local frame)** plus their
//     `vertex_idx`es. EPA (v2c) runs in A's local frame and the vertex_idx
//     pair drives the polytope-vertex de-duplication; storing local-frame
//     means EPA doesn't pay the inverse-transform tax to enter its
//     working frame. The Minkowski-difference simplex `w_i = sa_i - T_BA·
//     sb_i` is reconstructible from these.
//   - `iteration_count` / `converged`: diagnostics for tests and
//     instrumentation.
//
// Determinism contract (inherits ADR-0063, applies here per §4 pin #14):
//   - All four `support()` overloads in `support.hpp` use strict-greater-
//     wins argmax with deterministic ties — fixed canonical corner / axis
//     replies on zero or grazing inputs.
//   - Sub-distance order is Ericson §9.5 (this header's `detail/`),
//     NOT van den Bergen / Johnson distance algorithm.
//   - Termination prefers the index-match check; the geometric check is
//     the analytic-shape fallback only.
//   - 32-iteration cap is fixed (matches Box2D / Bullet).
//   - No `std::sort`, no `<cmath>` outside the existing whitelisted set
//     (`sqrt`/`abs`); `crd-no-std-math-check` scopes this module.
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/geometry/convex/detail/sub_distance.hpp>
#include <crd/geometry/convex/support.hpp>
#include <crd/geometry/primitives/constants.hpp>
#include <crd/math/quat.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/transform.hpp>
#include <crd/math/vec.hpp>

#include <cmath>

namespace crd::geometry::convex
{
using crd::math::MathScalar;
using crd::math::Transform;
using crd::math::Vec3;

// GJK terminating-simplex storage. Parallel arrays — slot `i` holds the
// `i`-th simplex vertex's components. Slots `[0, size)` are valid; slots
// beyond are unread (do not assume zero).
template <MathScalar T> struct GjkSimplex
{
    Vec3<T> w_a_local[4]{};      // A-supports in A's local frame
    Vec3<T> w_b_local[4]{};      // B-supports in B's local frame
    crd::u32 vidx_a[4]{k_invalid_vertex, k_invalid_vertex, k_invalid_vertex, k_invalid_vertex};
    crd::u32 vidx_b[4]{k_invalid_vertex, k_invalid_vertex, k_invalid_vertex, k_invalid_vertex};
    crd::u8 size = 0;
};

// The result of `gjk_distance(a, xa, b, xb)`. Field layout frozen — EPA
// (v2c) reads `simplex` and the `vidx_*` arrays by member name.
template <MathScalar T> struct GjkResult
{
    Vec3<T> witness_a_world{};
    Vec3<T> witness_b_world{};
    T distance_squared{static_cast<T>(0)};
    bool overlapping{false};

    GjkSimplex<T> simplex{};

    crd::u8 iteration_count{0};
    // Default `false`: a default-constructed result represents "no call made
    // yet" — `gjk_distance` sets this to `true` on natural exit (index-match,
    // geometric, or overlap detected). A `false` value after the call means
    // the iteration cap was hit OR the simplex got stuck at 4 vertices with
    // no reduction — the witnesses are still the best the driver found, but
    // distance may be sub-optimal. Well-formed inputs should always produce
    // `converged == true`; a `false` here is a signal worth investigating.
    bool converged{false};
};

// ---- Driver ---------------------------------------------------------------
//
// `T` first per the concept-bind rule (ADR-0076 §16 pin #1 / the advisor's
// fix-once guidance). The shape types `A`, `B` are constrained by `ConvexShape
// <_, T>` so the same shape can participate at multiple scalars (v2i ships
// `gjk_distance<f64>` against the same hull types).
template <MathScalar T, typename A, typename B>
    requires ConvexShape<A, T> && ConvexShape<B, T>
[[nodiscard]] inline GjkResult<T> gjk_distance(const A& a, const Transform<T>& xform_a, const B& b,
                                               const Transform<T>& xform_b) noexcept
{
    GjkResult<T> result{};
    GjkSimplex<T>& simplex = result.simplex;

    // One-shot relative transform: B-local → A-local.
    const Transform<T> T_BA = crd::math::inversed(xform_a) * xform_b;
    const crd::math::Quat<T> T_BA_rot_inv = crd::math::inversed(T_BA.rotation);

    // Initial search direction in A-local: toward the origin from B's
    // center. A zero `T_BA.translation` (concentric or identity case) falls
    // back to `+X` — the canonical zero-direction reply of `normalize_safe`.
    Vec3<T> d = -T_BA.translation;
    if (!(crd::math::dot(d, d) > std::numeric_limits<T>::min()))
    {
        d = Vec3<T>(static_cast<T>(1), static_cast<T>(0), static_cast<T>(0));
    }

    // Iteration cap matches Box2D / Bullet — well-formed pairs converge in
    // 6-12 iterations; the cap catches pathological degenerate inputs.
    constexpr crd::u8 k_max_iter = 32;
    // ε² for the geometric (analytic-shape) termination test, scaled by the
    // value-class scale of the inputs at the call site. `k_distance_epsilon`
    // is `1e-6F` / `1e-12` per `crd-geometry-primitives::constants.hpp`.
    const T eps = crd::geometry::primitives::k_distance_epsilon<T>();
    const T eps_sq = eps * eps;

    // v2g warm-start cache: previous iter's argmax `vertex_idx` per shape,
    // threaded into `support_with_hint` as the start vertex for hill-climb
    // search on `ConvexHullView` with adjacency. Initialised to
    // `k_invalid_vertex` so the first iter falls back to the no-hint
    // linear scan. Non-hull shapes (Sphere/OBB/Capsule) and non-adjacency
    // hulls ignore the hint transparently via the generic
    // `support_with_hint` fallback template.
    crd::u32 last_vidx_a = k_invalid_vertex;
    crd::u32 last_vidx_b = k_invalid_vertex;

    // Convergence semantics: `converged` defaults to `false`. Every natural
    // exit (index-match OR geometric test OR overlap) explicitly flips it to
    // `true` before `break;`. If the loop falls off the end after
    // `k_max_iter` rounds, `converged` stays false — the cap was hit, the
    // result is the best simplex we found but caller knows it's suspect.
    // (Earlier form `if (iteration_count >= k_max_iter) converged = false;`
    // mis-fired at iter 31: the loop sets `iteration_count = iter + 1 = 32`
    // at the top, then a natural break leaves the post-loop check seeing
    // `32 >= 32` and falsely flagging convergence as failed.)
    result.converged = false;
    bool natural_exit = false;

    for (crd::u8 iter = 0; iter < k_max_iter; ++iter)
    {
        result.iteration_count = static_cast<crd::u8>(iter + 1);

        // Support in direction `d`: A-side in A-local, B-side in B-local then
        // transported into A-local. The Minkowski-difference vertex is the
        // difference in A's frame.
        const SupportPoint<T> sa = crd::geometry::primitives::support_with_hint<T>(a, d, last_vidx_a);
        const Vec3<T> d_in_b = crd::math::rotate_vector(T_BA_rot_inv, -d);
        const SupportPoint<T> sb_b = crd::geometry::primitives::support_with_hint<T>(b, d_in_b, last_vidx_b);
        last_vidx_a = sa.vertex_idx;
        last_vidx_b = sb_b.vertex_idx;
        const Vec3<T> sb_in_a = crd::math::transform_point(T_BA, sb_b.point);
        const Vec3<T> w = sa.point - sb_in_a;

        // ---- Termination (index-match, primary) ----
        // Cross-platform bit-exact when both indices are valid; epsilon-free.
        if (sa.vertex_idx != k_invalid_vertex && sb_b.vertex_idx != k_invalid_vertex)
        {
            bool already_in_simplex = false;
            for (crd::u8 s = 0; s < simplex.size; ++s)
            {
                if (simplex.vidx_a[s] == sa.vertex_idx && simplex.vidx_b[s] == sb_b.vertex_idx)
                {
                    already_in_simplex = true;
                    break;
                }
            }
            if (already_in_simplex)
            {
                natural_exit = true;
                break;
            }
        }
        // ---- Termination (geometric, fallback) ----
        // Standard GJK progress test: with `d = -v` (search direction is the
        // negated closest-point), the supporting hyperplane through `v` is
        // `{x : x·v = |v|²}`; origin satisfies `0 < |v|²` (separated), and
        // progress requires `w·v < |v|²`. Convergence is `|v|² − v·w ≤ ε²`
        // ⇔ `|d|² + d·w ≤ ε²`. Skip on iteration 0 (no `v` yet).
        if (simplex.size > 0)
        {
            const T d_sq = crd::math::dot(d, d);
            const T d_dot_w = crd::math::dot(d, w);
            if (d_sq + d_dot_w <= eps_sq)
            {
                natural_exit = true;
                break;
            }
        }

        // ---- Add `w` to the simplex ----
        // Defensive: shouldn't happen on well-formed inputs (index-match or
        // geometric test would catch a repeat), but if a degenerate hull
        // hands back the same Minkowski-diff vertex twice, we still need to
        // not corrupt the simplex. `natural_exit` stays `false` — this is
        // genuinely a non-converged exit.
        if (simplex.size >= 4)
        {
            break;
        }
        const crd::u8 ns = simplex.size;
        simplex.w_a_local[ns] = sa.point;
        simplex.w_b_local[ns] = sb_b.point;
        simplex.vidx_a[ns] = sa.vertex_idx;
        simplex.vidx_b[ns] = sb_b.vertex_idx;
        simplex.size = static_cast<crd::u8>(ns + 1);

        // ---- Sub-distance reduction ----
        // Build the Minkowski-diff array on the stack — sub_distance_N
        // operates on those.
        Vec3<T> w_arr[4];
        for (crd::u8 s = 0; s < simplex.size; ++s)
        {
            w_arr[s] = simplex.w_a_local[s] -
                       crd::math::transform_point(T_BA, simplex.w_b_local[s]);
        }
        detail::SubDistanceResult<T> sd;
        switch (simplex.size)
        {
        case 1:
            sd = detail::sub_distance_1(w_arr[0]);
            break;
        case 2:
            sd = detail::sub_distance_2(w_arr[0], w_arr[1]);
            break;
        case 3:
            sd = detail::sub_distance_3(w_arr[0], w_arr[1], w_arr[2]);
            break;
        case 4:
        default:
            sd = detail::sub_distance_4(w_arr[0], w_arr[1], w_arr[2], w_arr[3]);
            break;
        }

        // ---- Overlap detection ----
        // Full tetra surviving ⇒ origin in conv(simplex) ⇒ Minkowski
        // difference contains origin ⇒ shapes overlap.
        if (sd.mask == 0b1111)
        {
            result.overlapping = true;
            result.distance_squared = static_cast<T>(0);
            // Witnesses coincide at the common contact point; combine A-
            // supports and B-supports (in their own local frames) with the
            // barycentric weights.
            Vec3<T> wa_local(static_cast<T>(0), static_cast<T>(0), static_cast<T>(0));
            Vec3<T> wb_local(static_cast<T>(0), static_cast<T>(0), static_cast<T>(0));
            for (crd::u8 s = 0; s < 4; ++s)
            {
                wa_local.x += simplex.w_a_local[s].x * sd.weights[s];
                wa_local.y += simplex.w_a_local[s].y * sd.weights[s];
                wa_local.z += simplex.w_a_local[s].z * sd.weights[s];
                wb_local.x += simplex.w_b_local[s].x * sd.weights[s];
                wb_local.y += simplex.w_b_local[s].y * sd.weights[s];
                wb_local.z += simplex.w_b_local[s].z * sd.weights[s];
            }
            result.witness_a_world = crd::math::transform_point(xform_a, wa_local);
            result.witness_b_world = crd::math::transform_point(xform_b, wb_local);
            // Origin inside the Minkowski-difference simplex is a real,
            // bounded answer (deepest interior point detected) — not a
            // failure of GJK to find one. Flag as converged.
            result.converged = true;
            return result;
        }

        // ---- Repack surviving slots into [0, popcount(mask)) ----
        // Walk slots in increasing order; surviving slots compact toward
        // index 0; weights track the same compaction (for the witness
        // reconstruction after the loop).
        GjkSimplex<T> packed;
        T packed_weights[4]{};
        Vec3<T> packed_w_arr[4];
        crd::u8 ns_new = 0;
        for (crd::u8 s = 0; s < 4; ++s)
        {
            if (sd.mask & (1U << s))
            {
                packed.w_a_local[ns_new] = simplex.w_a_local[s];
                packed.w_b_local[ns_new] = simplex.w_b_local[s];
                packed.vidx_a[ns_new] = simplex.vidx_a[s];
                packed.vidx_b[ns_new] = simplex.vidx_b[s];
                packed_weights[ns_new] = sd.weights[s];
                packed_w_arr[ns_new] = w_arr[s];
                ++ns_new;
            }
        }
        packed.size = ns_new;
        simplex = packed;

        // ---- New search direction ----
        // `closest` is the closest point on conv(simplex) to origin in A-
        // local; the new search direction points toward the origin from
        // there: `d = -closest`.
        d = Vec3<T>(-sd.closest.x, -sd.closest.y, -sd.closest.z);

        // ---- Witnesses for the *current* (post-reduction) state ----
        // The same barycentric weights apply to the reduced A-supports /
        // B-supports. This becomes the result on normal termination.
        Vec3<T> wa_local(static_cast<T>(0), static_cast<T>(0), static_cast<T>(0));
        Vec3<T> wb_local(static_cast<T>(0), static_cast<T>(0), static_cast<T>(0));
        for (crd::u8 s = 0; s < simplex.size; ++s)
        {
            wa_local.x += packed.w_a_local[s].x * packed_weights[s];
            wa_local.y += packed.w_a_local[s].y * packed_weights[s];
            wa_local.z += packed.w_a_local[s].z * packed_weights[s];
            wb_local.x += packed.w_b_local[s].x * packed_weights[s];
            wb_local.y += packed.w_b_local[s].y * packed_weights[s];
            wb_local.z += packed.w_b_local[s].z * packed_weights[s];
        }
        result.witness_a_world = crd::math::transform_point(xform_a, wa_local);
        result.witness_b_world = crd::math::transform_point(xform_b, wb_local);
        result.distance_squared = crd::math::dot(sd.closest, sd.closest);

        // ---- Origin already on the simplex? ----
        // `closest == origin` AND not full tetra means origin lies on a
        // face / edge / vertex of the simplex — still a contact, treat as
        // overlap (boundary case). Use the same eps² as the geometric
        // test so we don't flap.
        if (result.distance_squared <= eps_sq)
        {
            result.overlapping = true;
            result.distance_squared = static_cast<T>(0);
            // Witnesses already set above. Origin-on-simplex is a natural
            // convergence — boundary contact is still a real answer.
            result.converged = true;
            return result;
        }
    }

    result.converged = natural_exit;
    return result;
}

// ---- gjk_overlap — boolean fast-out driver (Phase 3.1.7 v2b) ---------------
//
// Specialised "do these shapes overlap" predicate. Same simplex / sub-distance
// / termination logic as `gjk_distance`, with two surgical cuts:
//
//   (1) **No witness reconstruction.** `gjk_distance` builds
//       `wa_local`/`wb_local` barycentric combinations at the end of every
//       iteration (so the most-recent iter's witnesses are returned on
//       natural-exit `break`). The overlap path doesn't care about
//       witnesses — it only needs the boolean "is origin inside the
//       Minkowski-diff simplex".  Drops ~30 lines + 24 multiplies per iter.
//
//   (2) **No `result` struct.** No `GjkResult` allocation, no per-iter
//       writes to it. Returns `bool` directly from the loop.
//
// Behavioural equivalence with `gjk_distance(...).overlapping`:
//   - Both detect overlap when `sub_distance` returns `mask == 0b1111`
//     (origin in the 4-simplex hull).
//   - Both detect "origin on a face / edge / vertex of the simplex" as
//     overlap via `dot(closest, closest) <= eps_sq` (boundary contact).
//   - Both terminate to "separated" on index-match without progress OR
//     `|d|² + d·w <= eps_sq` (the analytic-shape geometric test).
//   - Both cap at 32 iterations.
//
// The v2b agreement test cross-checks `gjk_overlap(...) == gjk_distance
// (...).overlapping` across a randomized rigid-transform corpus (including
// non-identity rotations from the v2a polish pass) — if any code path
// diverges, the test catches it. Performance: expected ~15-20% faster on
// overlapping pairs (the hot iters when GJK is "closing in" — the saved
// witness work matters most when iter counts get into the 10-15 range).
// `[!benchmark]` measurement in `tests/bench/test_bench_gjk.cpp` for v2-close.
//
// Touching-boundary convention (PINNED in v2b):
//
// - **Analytic-vs-analytic** (sphere-vs-sphere, sphere-vs-capsule, capsule-
//   vs-capsule, sphere-vs-plane) at exact contact: reports `overlap = true`.
//   The Minkowski difference has a smooth boundary; GJK lands on origin
//   within `eps²` reliably at f32.
// - **Polyhedral-vs-polyhedral** (cube-vs-cube, hull-vs-hull, OBB-vs-OBB)
//   at exact face/edge/vertex contact: reports `overlap = true`. The
//   Minkowski-diff 4-simplex contains origin via `mask == 0b1111`.
// - **Smooth-vs-polyhedral** (sphere-vs-cube, capsule-vs-hull) at exact
//   distance 0: **AMBIGUOUS at f32 precision**. The Minkowski-diff has a
//   rounded face that GJK approaches but doesn't nail to `eps²` accuracy
//   in f32 (the support-direction `normalize_safe` and corner selection
//   together introduce ~1e-3 rounding); the geometric termination then
//   fires "no further progress" and the caller may see `overlap = false`.
//   For physics applications, post-filter via
//   `gjk_distance(...).distance_squared <= contact_margin²` with a
//   positive `contact_margin` (Box2D / Bullet use ~1e-3). v2i instantiates
//   `gjk_distance<f64>` for orbital-precision needs; until then,
//   exact-touching for smooth-vs-polyhedral is f32-precision-bound.
//
// `gjk_overlap` and `gjk_distance(...).overlapping` agree on every case —
// the v2b agreement test enforces that contract on a randomized rigid
// corpus (translations + rotations + mixed shape types). The convention
// applies to both paths equally.
template <MathScalar T, typename A, typename B>
    requires ConvexShape<A, T> && ConvexShape<B, T>
[[nodiscard]] inline bool gjk_overlap(const A& a, const Transform<T>& xform_a, const B& b,
                                      const Transform<T>& xform_b) noexcept
{
    GjkSimplex<T> simplex;

    const Transform<T> T_BA = crd::math::inversed(xform_a) * xform_b;
    const crd::math::Quat<T> T_BA_rot_inv = crd::math::inversed(T_BA.rotation);

    Vec3<T> d = -T_BA.translation;
    if (!(crd::math::dot(d, d) > std::numeric_limits<T>::min()))
    {
        // Concentric / identity case: shapes are sharing the same center.
        // Both `gjk_distance` and intuition say this is overlap. Fast-return.
        return true;
    }

    constexpr crd::u8 k_max_iter = 32;
    const T eps = crd::geometry::primitives::k_distance_epsilon<T>();
    const T eps_sq = eps * eps;

    // v2g warm-start cache (see `gjk_distance` for the rationale).
    crd::u32 last_vidx_a = k_invalid_vertex;
    crd::u32 last_vidx_b = k_invalid_vertex;

    for (crd::u8 iter = 0; iter < k_max_iter; ++iter)
    {
        const SupportPoint<T> sa = crd::geometry::primitives::support_with_hint<T>(a, d, last_vidx_a);
        const Vec3<T> d_in_b = crd::math::rotate_vector(T_BA_rot_inv, -d);
        const SupportPoint<T> sb_b = crd::geometry::primitives::support_with_hint<T>(b, d_in_b, last_vidx_b);
        last_vidx_a = sa.vertex_idx;
        last_vidx_b = sb_b.vertex_idx;
        const Vec3<T> sb_in_a = crd::math::transform_point(T_BA, sb_b.point);
        const Vec3<T> w = sa.point - sb_in_a;

        // ---- Termination (index-match, primary) ----
        // A repeat means support refused to introduce a new Minkowski-diff
        // vertex past the current closest hyperplane → no further progress
        // toward the origin → SEPARATED.
        if (sa.vertex_idx != k_invalid_vertex && sb_b.vertex_idx != k_invalid_vertex)
        {
            for (crd::u8 s = 0; s < simplex.size; ++s)
            {
                if (simplex.vidx_a[s] == sa.vertex_idx && simplex.vidx_b[s] == sb_b.vertex_idx)
                {
                    return false;
                }
            }
        }
        // ---- Termination (geometric, fallback) ----
        // Same `|d|² + d·w ≤ ε²` test as `gjk_distance` — no progress past
        // the supporting hyperplane through the current closest → SEPARATED.
        if (simplex.size > 0)
        {
            const T d_sq = crd::math::dot(d, d);
            const T d_dot_w = crd::math::dot(d, w);
            if (d_sq + d_dot_w <= eps_sq)
            {
                return false;
            }
        }

        // ---- Add `w`; defensive bail on a full simplex without convergence
        // (matches `gjk_distance`'s same path — pathological degenerate input).
        if (simplex.size >= 4)
        {
            return false;
        }
        const crd::u8 ns = simplex.size;
        simplex.w_a_local[ns] = sa.point;
        simplex.w_b_local[ns] = sb_b.point;
        simplex.vidx_a[ns] = sa.vertex_idx;
        simplex.vidx_b[ns] = sb_b.vertex_idx;
        simplex.size = static_cast<crd::u8>(ns + 1);

        Vec3<T> w_arr[4];
        for (crd::u8 s = 0; s < simplex.size; ++s)
        {
            w_arr[s] = simplex.w_a_local[s] - crd::math::transform_point(T_BA, simplex.w_b_local[s]);
        }
        detail::SubDistanceResult<T> sd;
        switch (simplex.size)
        {
        case 1:
            sd = detail::sub_distance_1(w_arr[0]);
            break;
        case 2:
            sd = detail::sub_distance_2(w_arr[0], w_arr[1]);
            break;
        case 3:
            sd = detail::sub_distance_3(w_arr[0], w_arr[1], w_arr[2]);
            break;
        case 4:
        default:
            sd = detail::sub_distance_4(w_arr[0], w_arr[1], w_arr[2], w_arr[3]);
            break;
        }

        // ---- OVERLAP detected: origin inside the 4-simplex ----
        if (sd.mask == 0b1111)
        {
            return true;
        }

        // ---- Repack surviving slots into [0, popcount(mask)) ----
        GjkSimplex<T> packed;
        crd::u8 ns_new = 0;
        for (crd::u8 s = 0; s < 4; ++s)
        {
            if (sd.mask & (1U << s))
            {
                packed.w_a_local[ns_new] = simplex.w_a_local[s];
                packed.w_b_local[ns_new] = simplex.w_b_local[s];
                packed.vidx_a[ns_new] = simplex.vidx_a[s];
                packed.vidx_b[ns_new] = simplex.vidx_b[s];
                ++ns_new;
            }
        }
        packed.size = ns_new;
        simplex = packed;

        d = Vec3<T>(-sd.closest.x, -sd.closest.y, -sd.closest.z);

        // ---- Boundary contact: origin on the (reduced) simplex hull ----
        // Same `<= eps_sq` test as `gjk_distance`; treats touching as overlap.
        if (crd::math::dot(sd.closest, sd.closest) <= eps_sq)
        {
            return true;
        }
    }

    // Iteration cap reached without a definitive overlap signal — caller
    // gets `false` (the conservative default). This matches `gjk_distance`,
    // whose `result.overlapping` defaults to `false` and is only flipped
    // by a positive overlap detection.
    return false;
}

} // namespace crd::geometry::convex

// ---- Unified queries facade overload set (ADR-0076 §16 pin #1) ------------
//
// `crd::geometry::overlap(convex_a, xform_a, convex_b, xform_b) -> bool` —
// the public boolean-overlap entry point in the unified `crd::geometry`
// namespace. Found by ADL alongside the existing BVH-side `overlap(...)`
// overloads in `<crd/geometry/queries.hpp>`. New shape types light up
// automatically: write a `support()` overload (matching `ConvexShape<S,T>`)
// in the shape's namespace, and this facade overload picks them up.
//
// Why declare it here rather than in `queries.hpp`: `crd-geometry-bvh`
// (which owns `queries.hpp`) does not depend on `crd-geometry-convex`,
// and adding a convex include there would invert the dep graph. Instead,
// the convex module contributes its own facade overload set; users who
// include `<crd/geometry/convex/convex.hpp>` get them, users who only
// need BVH queries don't pay any compile cost for convex code.

namespace crd::geometry
{
template <crd::math::MathScalar T, typename A, typename B>
    requires convex::ConvexShape<A, T> && convex::ConvexShape<B, T>
[[nodiscard]] inline bool overlap(const A& a, const crd::math::Transform<T>& xform_a, const B& b,
                                  const crd::math::Transform<T>& xform_b) noexcept
{
    return convex::gjk_overlap<T>(a, xform_a, b, xform_b);
}
} // namespace crd::geometry
