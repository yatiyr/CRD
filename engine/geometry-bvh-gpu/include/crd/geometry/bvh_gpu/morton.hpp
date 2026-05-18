#pragma once

// ---------------------------------------------------------------------------
// 30-bit Morton-code generation for AABB centroids. Phase 3.1.7 v9a-a.
//
// First stage of the GPU LBVH (Karras 2012) pipeline. Given a span of
// world-space AABBs, compute one 30-bit Morton code per AABB centroid,
// normalised into the **scene AABB** so each axis maps into [0, 1023].
// 10 bits per axis interleaved produces a 30-bit u32 Morton code.
//
// The CPU implementation `compute_morton_codes_cpu` IS the algorithm
// definition. The GPU implementation (`compute_morton_codes_gpu`,
// defined in dispatch.hpp; v9a-a) is a mechanical translation of the
// CPU kernel body into GLSL — same bit-twiddle, same float→u32
// quantisation, same scene-AABB normalisation. Test:
// `bit_compare(cpu, gpu)` must be byte-identical for any finite input.
//
// Scope honesty (D-): 30-bit Morton at 1024³ resolution per axis means
// primitives smaller than `(scene_extent / 1024)` along any axis collide
// into the same Morton bin. At a 100 m scene that's ~10 cm; at a 1 km
// scene that's ~1 m. Acceptable for game / sim / CAD scenes at ≤ 100 m
// extent. Eylem-aero and CAM at km-scale need the planned `v9a-60bit`
// follow-on slice (u64 60-bit Morton, 20 bits per axis ≈ 1M³ resolution).
// Filed at v9a-a slice start; the 30-bit lock is documented divergence
// from "one size fits all", not silent default-on-broken-input.
//
// Determinism: 30-bit Morton is a pure deterministic function of the
// input AABB and the scene AABB. No FP non-determinism (the quantisation
// is rounding-toward-zero `u32(clamp(x, 0, 1023))`).
//
// Tiebreak: callers that need a deterministic sort over Morton codes
// (every LBVH consumer does) should sort `(morton_code, original_index)`
// pairs — see v9a-b1 `radix_sort_cpu`. Equal Morton codes ⇒ lower input
// index wins ⇒ deterministic LBVH topology even on cocircular Morton
// codes.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::bvh_gpu
{

// --- Bit-interleave kernel -------------------------------------------------

// Insert two 0 bits between each of the low 10 bits of `x`. The classic
// Karras / Lauterbach 2009 "spread bits" function: bit `i` of input lands
// at position `3*i` of output. Output therefore has bits at positions
// `{0, 3, 6, 9, 12, 15, 18, 21, 24, 27}`.
//
// Usage: `morton_x = spread_bits_30(x); morton_y = spread_bits_30(y) << 1;
//         morton_z = spread_bits_30(z) << 2; morton = morton_x | morton_y |
//         morton_z;` → bit pattern z2 y2 x2 z1 y1 x1 ... z0 y0 x0.
//
// Pure function. No floating point. Identical CPU + GPU (the GLSL
// translation is a verbatim copy of this body).
[[nodiscard]] constexpr crd::u32 spread_bits_30(crd::u32 x) noexcept
{
    x &= 0x000003FFU;                       // keep low 10 bits
    x = (x | (x << 16U)) & 0x030000FFU;     // bits at positions {0..7, 16..17}
    x = (x | (x << 8U))  & 0x0300F00FU;     // bits at positions {0..3, 8..11, 16..17, 24..25}
    x = (x | (x << 4U))  & 0x030C30C3U;     // bits at positions {0..1, 4..5, 8..9, 12..13, 16..17, 20..21, 24..25}
    x = (x | (x << 2U))  & 0x09249249U;     // every 3rd bit set across [0, 27]
    return x;
}

// 30-bit Morton code from three pre-quantised 10-bit ints (each in
// [0, 1023]). The "low-level" entry; the float-overload below quantises
// world-space coordinates into this form using the scene AABB.
[[nodiscard]] constexpr crd::u32 morton3_30bit_from_ints(crd::u32 ix, crd::u32 iy, crd::u32 iz) noexcept
{
    return spread_bits_30(ix) | (spread_bits_30(iy) << 1U) | (spread_bits_30(iz) << 2U);
}

// --- World-space → Morton --------------------------------------------------

// Map a world-space coordinate component into the [0, 1023] integer
// grid defined by the scene AABB. Out-of-AABB inputs clamp to the
// nearest cell (defensive — caller should pass centroids that lie
// inside `scene_aabb`, but a numerically-marginal centroid shouldn't
// crash). NaN/Inf-clean: input AABB is assumed finite by builder reject
// (ADR-0076 §16); centroid finiteness is the caller's contract.
[[nodiscard]] constexpr crd::u32
quantize_to_morton_grid(crd::f32 v, crd::f32 lo, crd::f32 inv_extent) noexcept
{
    const crd::f32 normalized = (v - lo) * inv_extent;          // [0, 1] iff in AABB
    const crd::f32 scaled     = normalized * 1024.0F;            // [0, 1024]
    // Truncate-toward-zero then clamp into [0, 1023]. Cast through i32
    // first to handle small-negative normalised values without UB.
    const crd::i32 truncated  = static_cast<crd::i32>(scaled);
    const crd::i32 clamped    = truncated < 0       ? 0
                                : truncated > 1023  ? 1023
                                : truncated;
    return static_cast<crd::u32>(clamped);
}

// 30-bit Morton code for an AABB centroid normalised into the scene AABB.
// THE ALGORITHM DEFINITION — the GLSL kernel is a mechanical copy of
// this body. `scene_extent` is `scene_aabb.max - scene_aabb.min`; any
// zero/near-zero axis is handled by the inverse-extent guard (zero
// inv_extent ⇒ all centroids map to bin 0 along that axis, which is
// correct — a degenerate flat scene has no Morton structure on the
// degenerate axis).
[[nodiscard]] constexpr crd::u32
morton3_30bit_for_centroid(const crd::math::Vec3<crd::f32>& centroid,
                            const crd::math::Vec3<crd::f32>& scene_min,
                            const crd::math::Vec3<crd::f32>& inv_scene_extent) noexcept
{
    const crd::u32 ix = quantize_to_morton_grid(centroid.x, scene_min.x, inv_scene_extent.x);
    const crd::u32 iy = quantize_to_morton_grid(centroid.y, scene_min.y, inv_scene_extent.y);
    const crd::u32 iz = quantize_to_morton_grid(centroid.z, scene_min.z, inv_scene_extent.z);
    return morton3_30bit_from_ints(ix, iy, iz);
}

// --- Batch CPU oracle ------------------------------------------------------

// Compute one 30-bit Morton code per input AABB centroid, normalised
// into the union of all input AABBs (the scene AABB). Output array is
// sized to match `aabbs.size()`; same allocator binds. This is the
// CPU REFERENCE — the GPU dispatch in dispatch.hpp must produce
// byte-identical output for any input.
//
// Empty input ⇒ empty output (no scene AABB to normalise into).
[[nodiscard]] crd::containers::Array<crd::u32>
compute_morton_codes_cpu(crd::containers::ConstSpan<crd::geometry::primitives::AABB3<crd::f32>> aabbs,
                          crd::memory::IAllocator* alloc) noexcept;

// Same as above but the scene AABB is passed in (skip the union pass).
// Useful when the caller already knows the scene bounds (e.g. has them
// cached from a previous frame). For the GPU dispatch this is also the
// path that matches one-pass GPU execution where the scene AABB is
// derived on host and uploaded as push-constants.
[[nodiscard]] crd::containers::Array<crd::u32>
compute_morton_codes_cpu(crd::containers::ConstSpan<crd::geometry::primitives::AABB3<crd::f32>> aabbs,
                          const crd::geometry::primitives::AABB3<crd::f32>& scene_aabb,
                          crd::memory::IAllocator* alloc) noexcept;

// Compute the union AABB of an input span. Empty span ⇒ empty AABB
// (min = +inf, max = -inf, per the existing aabb_empty convention).
[[nodiscard]] crd::geometry::primitives::AABB3<crd::f32>
union_aabb_of(crd::containers::ConstSpan<crd::geometry::primitives::AABB3<crd::f32>> aabbs) noexcept;

} // namespace crd::geometry::bvh_gpu
