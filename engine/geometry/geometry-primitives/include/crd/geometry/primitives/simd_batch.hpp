#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-primitives — batch intersection kernels (v0f; per CPU level since SIMD-DISPATCH).
//
// Each call runs a whole span of bundles of eight (the plain columns of crd/geometry/primitives/primitive_kernels.hpp)
// through one per-level kernel (ADR-0134 §5), for the two BVH-traversal shapes:
//   * leaf-batch — one ray against N primitives in columns: `ray_vs_8_aabb` (Williams slab against eight child
//     boxes), `ray_vs_8_triangle` (Moller-Trumbore against eight leaf triangles), `aabb8_vs_aabb`,
//     `sphere8_vs_sphere`.
//   * packet — eight coherent rays against one box per node: `ray_packet8_vs_aabb` (Wald-style; each lane an
//     independent ray, each box scalar).
//   plus `segment8_vs_segment_distsq`, eight segment-pair squared closest distances (Ericson §5.1.9; eylem
//   broadphase capsule against N capsules).
//
// Determinism (ADR-0076 §4 #8-#11, ADR-0134 §7): masks are all ones or zero per lane; min and max use the IEEE
// ordering, so an infinite or NaN lane (a zero-direction ray) drops that axis instead of poisoning the result; 1/x is
// the correctly rounded divide; every level gives the same bits. A partial tail (fewer than eight valid items) is the
// caller's to mask: every lane is computed. The scalar references are the `intersect.hpp`, `closest_point.hpp` and
// `robust_ray_aabb.hpp` functions.
// ---------------------------------------------------------------------------

#include <crd/containers/span.hpp>
#include <crd/geometry/primitives/primitive_kernels.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/geometry/primitives/robust_ray_aabb.hpp>

namespace crd::geometry::primitives
{

// The packet of eight rays: origins and 1/direction (infinite for a zero component).
[[nodiscard]] RayPacket8 precompute_ray_packet8(const Ray3<crd::f32> (&rays)[kBatchLanes]) noexcept;

// out[i]: one ray (with its slab precompute) against the eight boxes of boxes[i] in [t0, t1]; tmax widened per
// Ize 2013 (conservative). out.size() == boxes.size().
void ray_vs_8_aabb(const Ray3<crd::f32>& ray, const RayAABBPrecompute<crd::f32>& pre,
                   crd::containers::ConstSpan<Aabb8> boxes, crd::f32 t0, crd::f32 t1,
                   crd::containers::Span<RayAabb8Result> out) noexcept;

// out[i]: the eight rays of the packet against boxes[i] in [t0, t1].
void ray_packet8_vs_aabb(const RayPacket8& packet, crd::containers::ConstSpan<AABB3<crd::f32>> boxes, crd::f32 t0,
                         crd::f32 t1, crd::containers::Span<RayAabb8Result> out) noexcept;

// out[i]: one ray against the eight triangles of tris[i] (Moller-Trumbore); `cull_back` ignores back-facing hits,
// `tnear` is the near clip.
void ray_vs_8_triangle(const Ray3<crd::f32>& ray, crd::containers::ConstSpan<Triangle38> tris,
                       crd::containers::Span<RayTri8Result> out, bool cull_back = false,
                       crd::f32 tnear = 0.0F) noexcept;

// out[i]: which of the eight boxes of boxes[i] overlap `box`.
void aabb8_vs_aabb(crd::containers::ConstSpan<Aabb8> boxes, const AABB3<crd::f32>& box,
                   crd::containers::Span<Mask8> out) noexcept;

// out[i]: which of the eight spheres of spheres[i] overlap `sphere`.
void sphere8_vs_sphere(crd::containers::ConstSpan<Sphere8> spheres, const Sphere<crd::f32>& sphere,
                       crd::containers::Span<Mask8> out) noexcept;

// out[i]: the squared closest distances of the eight segment pairs of pairs[i] (robust on parallel and degenerate
// segments).
void segment8_vs_segment_distsq(crd::containers::ConstSpan<Segment38Pair> pairs,
                                crd::containers::Span<Lanes8> out) noexcept;

} // namespace crd::geometry::primitives
