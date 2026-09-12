#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-primitives — SIMD batch intersection kernels (v0f).
//
// Eight-lane (`Vec8f`) kernels over the scalar cores, for the two BVH-traversal
// shapes:
//   * leaf-batch — one ray vs N primitives held as AoSoA columns: `ray_vs_8_aabb`
//     (Williams slab × 8 child boxes), `ray_vs_8_triangle` (Möller-Trumbore × 8
//     leaf triangles), `aabb8_vs_aabb`, `sphere8_vs_sphere`.
//   * packet — N coherent rays vs one box per node: `ray_packet8_vs_aabb`
//     (Wald-style 8-ray packet; each lane is an independent ray, the box is
//     scalar — broadcast).
//   plus `segment8_vs_segment_distsq` — eight segment-pair squared closest
//   distances (Ericson §5.1.9 in SIMD; eylem broadphase capsule-vs-N-capsule).
//
// Determinism (ADR-0076 §4 #8-#11): all comparisons return all-bits-set masks;
// `min`/`max` use the IEEE ordering so a ∞/NaN lane (zero-direction ray) drops
// that axis rather than poisoning the result; `1/x` is the correctly-rounded
// `_mm_div_ps`/`_mm256_div_ps`. When a "best lane" is wanted, feed the kernel's
// `Vec8f` outputs to `crd::math::simd::reduce_argmax_with_lex_tiebreak` (v0e).
// Lane masking for a partial tail (< 8 valid items) is the caller's job — these
// kernels always process all 8 lanes.
//
// f32-only (the `Vec8f` width); out-of-line in `geometry_primitives.cpp`'s
// sibling `simd_batch.cpp` (so `crd-simd-emission-check` has SIMD `.obj` to
// inspect). The scalar reference for ULP-conformance is the corresponding
// `intersect.hpp` / `closest_point.hpp` / `robust_ray_aabb.hpp` function.
// ---------------------------------------------------------------------------

#include <crd/geometry/primitives/primitives.hpp>
#include <crd/geometry/primitives/robust_ray_aabb.hpp>
#include <crd/math/simd/vec8f.hpp>

namespace crd::geometry::primitives
{
using crd::math::simd::Vec8f;

// ---- AoSoA column bundles -------------------------------------------------

struct Aabb8 // eight axis-aligned boxes, structure-of-arrays
{
    Vec8f min_x{}, min_y{}, min_z{};
    Vec8f max_x{}, max_y{}, max_z{};
};
struct Sphere8
{
    Vec8f center_x{}, center_y{}, center_z{};
    Vec8f radius{};
};
struct Triangle38 // eight triangles, SoA
{
    Vec8f ax{}, ay{}, az{};
    Vec8f bx{}, by{}, bz{};
    Vec8f cx{}, cy{}, cz{};
};
struct Segment38Pair // eight pairs of segments (segment1 from P1→Q1, segment2 from P2→Q2)
{
    Vec8f p1x{}, p1y{}, p1z{}, q1x{}, q1y{}, q1z{};
    Vec8f p2x{}, p2y{}, p2z{}, q2x{}, q2y{}, q2z{};
};

// ---- Results --------------------------------------------------------------

struct RayAabb8Result
{
    Vec8f t_enter{};  // per-lane entry parameter (clamped to t0); meaningless where hit==0
    Vec8f hit_mask{}; // per-lane all-bits-set on hit, all-zero on miss
};
struct RayTri8Result
{
    Vec8f t{};      // per-lane hit parameter
    Vec8f u{}, v{}; // per-lane barycentric weights of verts b, c (vertex a's weight = 1−u−v)
    Vec8f hit_mask{};
};

// ---- Packet (multi-ray) precompute ----------------------------------------

// Eight independent rays as a coherent packet — per-lane origin + 1/direction
// (∞ for a zero direction component; the slab's IEEE min/max drops that axis).
struct RayPacket8
{
    Vec8f origin_x{}, origin_y{}, origin_z{};
    Vec8f inv_dir_x{}, inv_dir_y{}, inv_dir_z{};
};

[[nodiscard]] inline RayPacket8 precompute_ray_packet8(const Vec8f& origin_x, const Vec8f& origin_y,
                                                       const Vec8f& origin_z, const Vec8f& dir_x, const Vec8f& dir_y,
                                                       const Vec8f& dir_z) noexcept
{
    return RayPacket8{origin_x, origin_y, origin_z, Vec8f::one() / dir_x, Vec8f::one() / dir_y, Vec8f::one() / dir_z};
}

// ---- Kernels (defined in simd_batch.cpp) ----------------------------------

// One ray (precomputed slab state) vs eight child AABBs. `t0`/`t1` = the
// traversal parameter window. `tmax` widened per Ize 2013 (conservative).
[[nodiscard]] RayAabb8Result ray_vs_8_aabb(const Ray3<crd::f32>& ray, const RayAABBPrecompute<crd::f32>& pre,
                                           const Aabb8& boxes, crd::f32 t0, crd::f32 t1) noexcept;

// Eight coherent rays vs one AABB (broadcast). `t0`/`t1` = the parameter window.
[[nodiscard]] RayAabb8Result ray_packet8_vs_aabb(const RayPacket8& packet, const AABB3<crd::f32>& box, crd::f32 t0,
                                                 crd::f32 t1) noexcept;

// One ray vs eight triangles — Möller-Trumbore × 8. `cull_back` ⇒ ignore
// back-facing hits. `tnear` = the near clip.
[[nodiscard]] RayTri8Result ray_vs_8_triangle(const Ray3<crd::f32>& ray, const Triangle38& tris, bool cull_back = false,
                                              crd::f32 tnear = 0.0F) noexcept;

// Eight AABBs vs one AABB — per-lane overlap mask.
[[nodiscard]] Vec8f aabb8_vs_aabb(const Aabb8& boxes, const AABB3<crd::f32>& box) noexcept;
// Eight spheres vs one sphere — per-lane overlap mask.
[[nodiscard]] Vec8f sphere8_vs_sphere(const Sphere8& spheres, const Sphere<crd::f32>& sphere) noexcept;

// Eight segment-pair squared closest distances (Ericson §5.1.9, robust on
// parallel / degenerate).
[[nodiscard]] Vec8f segment8_vs_segment_distsq(const Segment38Pair& pairs) noexcept;

} // namespace crd::geometry::primitives
