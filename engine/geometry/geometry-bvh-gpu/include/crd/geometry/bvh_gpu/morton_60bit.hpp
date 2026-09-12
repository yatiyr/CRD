#pragma once

// ---------------------------------------------------------------------------
// 60-bit Morton-code generation for AABB centroids. Phase 3.1.7
// v9a-60bit-cpu (sibling of v9a-a's 30-bit path).
//
// 60-bit Morton at 20 bits per axis = ~1,048,576³ resolution per axis:
//
//   Scene extent          Resolution per axis
//   --------------        --------------------
//   100 m                 ~95 µm     (10000× finer than 30-bit)
//   1 km                  ~1 mm
//   1 Mm  (planetary)     ~1 m
//   orbital scale         ~km
//
// Consumer list: CAM swept-volume pre-process, eylem-aero (km-scale
// orbital primitives), CAD precision modelling. These are stated
// first-class consumers per Phase 3.1.7 + the user's "engine substrate
// must serve everything" framing.
//
// The CPU implementation IS the algorithm definition (same discipline
// as v9a-a 30-bit; D134). The GPU 60-bit shader (v9a-60bit-gpu) is a
// mechanical translation of the per-element kernel body below into
// GLSL with `shaderInt64` enabled. Any divergence in GPU output vs CPU
// output is a bug, asserted by `bit_compare<u64>`.
//
// Determinism: same as 30-bit. Pure deterministic function of input.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

#include <cstdint>

namespace crd::geometry::bvh_gpu
{

// --- Bit-interleave kernel (20 bits per axis) -----------------------------

// Insert two 0 bits between each of the low 20 bits of `x` (u64).
// Bit `i` of input lands at position `3*i` of output. Output has bits
// at positions `{0, 3, 6, ..., 57}` (low 60 bits of u64).
//
// The 5-step magic-mask cascade is the standard u64 spread; constants
// derived by the same "interleave two zeros at each level" pattern as
// `spread_bits_30`, scaled to 20-bit input width.
[[nodiscard]] constexpr std::uint64_t spread_bits_60(std::uint64_t x) noexcept
{
    x &= 0x00000000000FFFFFULL;                            // keep low 20 bits
    x = (x | (x << 32ULL)) & 0x001F00000000FFFFULL;       // groups of 16/16 spread to 16/32
    x = (x | (x << 16ULL)) & 0x001F0000FF0000FFULL;       // groups of 8 spread
    x = (x | (x << 8ULL))  & 0x100F00F00F00F00FULL;       // groups of 4 spread
    x = (x | (x << 4ULL))  & 0x10C30C30C30C30C3ULL;       // groups of 2 spread
    x = (x | (x << 2ULL))  & 0x1249249249249249ULL;       // every 3rd bit in [0, 57]
    return x;
}

// 60-bit Morton code from three pre-quantised 20-bit ints (each in
// [0, 1048575]).
[[nodiscard]] constexpr std::uint64_t
morton3_60bit_from_ints(std::uint64_t ix, std::uint64_t iy, std::uint64_t iz) noexcept
{
    return spread_bits_60(ix) | (spread_bits_60(iy) << 1ULL) | (spread_bits_60(iz) << 2ULL);
}

// --- World-space → 20-bit grid --------------------------------------------

// Quantise a world-space coordinate component into the [0, 1048575]
// integer grid defined by the scene AABB. Identical structure to
// `quantize_to_morton_grid` (v9a-a 30-bit) but with the 20-bit
// resolution constant 1048576.0F.
[[nodiscard]] constexpr std::uint64_t
quantize_to_morton_grid_20bit(crd::f32 v, crd::f32 lo, crd::f32 inv_extent) noexcept
{
    const crd::f32 normalized = (v - lo) * inv_extent;          // [0, 1] iff in AABB
    const crd::f32 scaled     = normalized * 1048576.0F;         // [0, 1048576]
    // Truncate-toward-zero then clamp into [0, 1048575].
    const crd::i64 truncated  = static_cast<crd::i64>(scaled);
    const crd::i64 clamped    = truncated < 0       ? crd::i64{0}
                                : truncated > 1048575 ? crd::i64{1048575}
                                : truncated;
    return static_cast<std::uint64_t>(clamped);
}

// 60-bit Morton code for an AABB centroid normalised into the scene AABB.
// THE ALGORITHM DEFINITION — the GLSL 60-bit kernel (v9a-60bit-gpu) is
// a mechanical copy of this body.
[[nodiscard]] constexpr std::uint64_t
morton3_60bit_for_centroid(const crd::math::Vec3<crd::f32>& centroid,
                            const crd::math::Vec3<crd::f32>& scene_min,
                            const crd::math::Vec3<crd::f32>& inv_scene_extent) noexcept
{
    const std::uint64_t ix = quantize_to_morton_grid_20bit(centroid.x, scene_min.x, inv_scene_extent.x);
    const std::uint64_t iy = quantize_to_morton_grid_20bit(centroid.y, scene_min.y, inv_scene_extent.y);
    const std::uint64_t iz = quantize_to_morton_grid_20bit(centroid.z, scene_min.z, inv_scene_extent.z);
    return morton3_60bit_from_ints(ix, iy, iz);
}

// --- Batch CPU oracle ------------------------------------------------------

// Compute one 60-bit Morton code per input AABB centroid, normalised
// into the union of all input AABBs. CPU REFERENCE for the GPU 60-bit
// dispatch (v9a-60bit-gpu).
[[nodiscard]] crd::containers::Array<std::uint64_t>
compute_morton_codes_cpu_60bit(crd::containers::ConstSpan<crd::geometry::primitives::AABB3<crd::f32>> aabbs,
                                 crd::memory::IAllocator* alloc) noexcept;

// Same with caller-supplied scene AABB.
[[nodiscard]] crd::containers::Array<std::uint64_t>
compute_morton_codes_cpu_60bit(crd::containers::ConstSpan<crd::geometry::primitives::AABB3<crd::f32>> aabbs,
                                 const crd::geometry::primitives::AABB3<crd::f32>& scene_aabb,
                                 crd::memory::IAllocator* alloc) noexcept;

} // namespace crd::geometry::bvh_gpu
