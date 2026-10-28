// The table of the geometry-primitives kernels (SIMD-DISPATCH, ADR-0134 §5): one per CPU level the kernel unit is
// built for (engine/geometry/geometry-primitives/kernels/primitive_kernels.cpp), resolved by primitive_kernels() in
// crd/geometry/primitives/primitive_dispatch.hpp, and the plain bundles of eight the batch kernels read and write
// (crd/geometry/primitives/simd_batch.hpp). The header holds only data declarations, so a kernel unit compiles nothing
// from it. Every routine runs one algorithm on eight lanes with two roundings a step and is bit-exact across levels
// (ADR-0063, ADR-0134 §7), NaN payloads aside.

#pragma once

#include <crd/core/types.hpp>

namespace crd::geometry::primitives
{

// The lanes of a bundle.
inline constexpr crd::usize kBatchLanes = 8;

// Eight axis-aligned boxes, one column per coordinate.
struct Aabb8
{
    crd::f32 min_x[kBatchLanes];
    crd::f32 min_y[kBatchLanes];
    crd::f32 min_z[kBatchLanes];
    crd::f32 max_x[kBatchLanes];
    crd::f32 max_y[kBatchLanes];
    crd::f32 max_z[kBatchLanes];
};

struct Sphere8
{
    crd::f32 center_x[kBatchLanes];
    crd::f32 center_y[kBatchLanes];
    crd::f32 center_z[kBatchLanes];
    crd::f32 radius[kBatchLanes];
};

// Eight triangles (a, b, c).
struct Triangle38
{
    crd::f32 ax[kBatchLanes];
    crd::f32 ay[kBatchLanes];
    crd::f32 az[kBatchLanes];
    crd::f32 bx[kBatchLanes];
    crd::f32 by[kBatchLanes];
    crd::f32 bz[kBatchLanes];
    crd::f32 cx[kBatchLanes];
    crd::f32 cy[kBatchLanes];
    crd::f32 cz[kBatchLanes];
};

// Eight pairs of segments, segment 1 from p1 to q1 and segment 2 from p2 to q2.
struct Segment38Pair
{
    crd::f32 p1x[kBatchLanes];
    crd::f32 p1y[kBatchLanes];
    crd::f32 p1z[kBatchLanes];
    crd::f32 q1x[kBatchLanes];
    crd::f32 q1y[kBatchLanes];
    crd::f32 q1z[kBatchLanes];
    crd::f32 p2x[kBatchLanes];
    crd::f32 p2y[kBatchLanes];
    crd::f32 p2z[kBatchLanes];
    crd::f32 q2x[kBatchLanes];
    crd::f32 q2y[kBatchLanes];
    crd::f32 q2z[kBatchLanes];
};

// Eight independent rays: origin and 1/direction per lane (infinite for a zero component, whose axis the slab's
// min/max then drops).
struct RayPacket8
{
    crd::f32 origin_x[kBatchLanes];
    crd::f32 origin_y[kBatchLanes];
    crd::f32 origin_z[kBatchLanes];
    crd::f32 inv_dir_x[kBatchLanes];
    crd::f32 inv_dir_y[kBatchLanes];
    crd::f32 inv_dir_z[kBatchLanes];
};

// A lane mask is all ones where the test holds and zero where it does not.
struct Mask8
{
    crd::u32 lane[kBatchLanes];
};

struct Lanes8
{
    crd::f32 lane[kBatchLanes];
};

struct RayAabb8Result
{
    crd::f32 t_enter[kBatchLanes]; // entry parameter, clamped to t0; meaningless where hit_mask is zero
    crd::u32 hit_mask[kBatchLanes];
};

struct RayTri8Result
{
    crd::f32 t[kBatchLanes];
    crd::f32 u[kBatchLanes]; // barycentric weight of b
    crd::f32 v[kBatchLanes]; // barycentric weight of c (a's is 1 - u - v)
    crd::u32 hit_mask[kBatchLanes];
};

// A ray and its slab precompute (crd/geometry/primitives/robust_ray_aabb.hpp): 1/direction, the plane a slab enters
// by on each axis (1: the max plane, where the inverse direction is negative) and the Ize 2013 widening of tmax.
struct RaySlab
{
    crd::f32 origin[3];
    crd::f32 direction[3];
    crd::f32 inv_dir[3];
    crd::u32 sign[3];
    crd::f32 pad;
};

// An axis-aligned box as AABB3<f32> lays it out.
struct Box3
{
    crd::f32 min[3];
    crd::f32 max[3];
};

struct PrimitiveKernels
{
    // out[i]: the ray against the eight boxes of boxes[i] in [t0, t1] (Williams 2005 slab, near plane by sign).
    void (*ray_vs_aabb8)(const RaySlab& ray, const Aabb8* boxes, crd::usize n, crd::f32 t0, crd::f32 t1,
                         RayAabb8Result* out) noexcept;
    // out[i]: the eight rays of the packet against boxes[i] in [t0, t1] (the min/max form, no sign bits).
    void (*packet8_vs_aabb)(const RayPacket8& packet, const Box3* boxes, crd::usize n, crd::f32 pad, crd::f32 t0,
                            crd::f32 t1, RayAabb8Result* out) noexcept;
    // out[i]: the ray against the eight triangles of tris[i] (Moller-Trumbore); a hit needs |det| > eps, t >= tnear
    // and, with cull_back, det > 0.
    void (*ray_vs_triangle8)(const RaySlab& ray, const Triangle38* tris, crd::usize n, bool cull_back, crd::f32 tnear,
                             crd::f32 eps, RayTri8Result* out) noexcept;
    // out[i]: which of the eight boxes of boxes[i] overlap box (closed intervals).
    void (*aabb8_vs_aabb)(const Aabb8* boxes, crd::usize n, const Box3& box, Mask8* out) noexcept;
    // out[i]: which of the eight spheres of spheres[i] overlap the sphere (center, radius).
    void (*sphere8_vs_sphere)(const Sphere8* spheres, crd::usize n, const crd::f32 (&center)[3], crd::f32 radius,
                              Mask8* out) noexcept;
    // out[i]: the squared closest distance of each pair of pairs[i] (Ericson §5.1.9); tiny guards the degenerate
    // and parallel cases.
    void (*segment8_distsq)(const Segment38Pair* pairs, crd::usize n, crd::f32 tiny, Lanes8* out) noexcept;
    // The index of the vertex with the greatest dot product with (dx, dy, dz), the lowest index on a tie, over the
    // padded coordinate columns vx, vy, vz (padded a multiple of eight long).
    crd::u32 (*hull_support)(const crd::f32* vx, const crd::f32* vy, const crd::f32* vz, crd::usize padded, crd::f32 dx,
                             crd::f32 dy, crd::f32 dz) noexcept;
};

// The levels with a body; the dispatcher refuses a build that compiles any other.
extern const PrimitiveKernels kPrimitiveKernelsPortable;
extern const PrimitiveKernels kPrimitiveKernelsSse2;
extern const PrimitiveKernels kPrimitiveKernelsAvx2;
extern const PrimitiveKernels kPrimitiveKernelsAvx512;

} // namespace crd::geometry::primitives
