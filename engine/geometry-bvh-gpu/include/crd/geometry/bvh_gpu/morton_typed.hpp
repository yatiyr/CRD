#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-bvh-gpu — typed Quantity-overload Morton entry. Phase 3.1.7
// v9a-a-typed (ADR-0078 §5 D34 — two-layer typed architecture).
//
// The raw `compute_morton_codes_cpu` / `MortonGpuPipeline::dispatch_morton_codes`
// operate on raw `f32` AABBs per the §5 D34 lower-layer rule. Consumers that
// carry `Vec3<Length32>` AABBs (eylem broadphase, future cad surfaces, scene
// queries) reach for these wrappers: strip the Dim tag at the boundary, call
// the raw entry, return the same dimensionless `Array<u32>` morton codes
// (codes are bit-indices, not lengths — they have no Dim).
//
// Zero runtime overhead — `to_raw_vec` / `from_raw_vec` are constexpr;
// `.value` accessors compile away. Strip-compute-retag round-trip exactness
// is asserted by the test corpus.
//
// **Why ship now (substrate, not consumer-specific) — 2026-05-18 v9a-a
// follow-on payment.** See [[ship-at-consumer-template-from-day-one]]
// memory entry refined this day. Substrate work ships proactively when
// tests are cheap; the typed wrapper is ~50 LOC + a round-trip test, and
// establishes the pattern for every subsequent v9a-b/c/d slice to follow.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/bvh_gpu/dispatch.hpp>
#include <crd/geometry/bvh_gpu/morton.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/units/quantity.hpp>
#include <crd/units/quantity_aliases.hpp>

namespace crd::geometry::bvh_gpu
{

// Typed AABB: min/max carry a Length dim.
template <typename D, typename T> struct AABB3T
{
    crd::math::Vec3<crd::units::Quantity<D, T>> min{};
    crd::math::Vec3<crd::units::Quantity<D, T>> max{};
};

namespace detail
{

// Strip a typed AABB to its raw form for the lower-layer kernel. Layout
// parity (ADR-0078 §2 D2) guarantees `sizeof(Vec3<Q>) == sizeof(Vec3<T>)`,
// so the strip is a per-component value extraction without storage churn.
template <typename D, typename T>
[[nodiscard]] constexpr crd::geometry::primitives::AABB3<T>
strip_aabb(const AABB3T<D, T>& typed) noexcept
{
    return {
        crd::math::to_raw_vec(typed.min),
        crd::math::to_raw_vec(typed.max),
    };
}

} // namespace detail

// --- CPU typed entries ----------------------------------------------------

// Typed CPU Morton oracle. Strips each AABB to raw, computes the union
// scene AABB internally (same semantics as the raw one-arg overload),
// returns dimensionless u32 morton codes.
template <typename D, typename T>
[[nodiscard]] inline crd::containers::Array<crd::u32>
compute_morton_codes_cpu_typed(crd::containers::ConstSpan<AABB3T<D, T>> typed_aabbs,
                                 crd::memory::IAllocator* alloc) noexcept
{
    static_assert(std::is_same_v<T, crd::f32>,
                   "v9a-a-typed currently bridges f32 only — matches raw entry support");

    // Strip into a contiguous raw buffer; raw entry expects ConstSpan<AABB3<T>>.
    crd::containers::Array<crd::geometry::primitives::AABB3<T>> raw(alloc);
    raw.reserve(typed_aabbs.size());
    for (const auto& a : typed_aabbs)
    {
        raw.push_back(detail::strip_aabb(a));
    }
    return compute_morton_codes_cpu(
        crd::containers::ConstSpan<crd::geometry::primitives::AABB3<T>>(raw.data(), raw.size()),
        alloc);
}

// Typed CPU Morton with caller-supplied typed scene AABB.
template <typename D, typename T>
[[nodiscard]] inline crd::containers::Array<crd::u32>
compute_morton_codes_cpu_typed(crd::containers::ConstSpan<AABB3T<D, T>> typed_aabbs,
                                 const AABB3T<D, T>& typed_scene,
                                 crd::memory::IAllocator* alloc) noexcept
{
    static_assert(std::is_same_v<T, crd::f32>,
                   "v9a-a-typed currently bridges f32 only — matches raw entry support");

    crd::containers::Array<crd::geometry::primitives::AABB3<T>> raw(alloc);
    raw.reserve(typed_aabbs.size());
    for (const auto& a : typed_aabbs)
    {
        raw.push_back(detail::strip_aabb(a));
    }
    return compute_morton_codes_cpu(
        crd::containers::ConstSpan<crd::geometry::primitives::AABB3<T>>(raw.data(), raw.size()),
        detail::strip_aabb(typed_scene),
        alloc);
}

// --- GPU typed entry ------------------------------------------------------

// Typed GPU Morton dispatch. Same strip-compute-retag pattern as the CPU
// entries; routes through the existing `MortonGpuPipeline::dispatch_morton_codes`
// after stripping the typed AABBs to raw.
template <typename D, typename T>
[[nodiscard]] inline crd::containers::Array<crd::u32>
dispatch_morton_codes_typed(MortonGpuPipeline& pipeline,
                              crd::containers::ConstSpan<AABB3T<D, T>> typed_aabbs,
                              const AABB3T<D, T>& typed_scene,
                              crd::memory::IAllocator* alloc) noexcept
{
    static_assert(std::is_same_v<T, crd::f32>,
                   "v9a-a-typed currently bridges f32 only — matches raw entry support");

    crd::containers::Array<crd::geometry::primitives::AABB3<T>> raw(alloc);
    raw.reserve(typed_aabbs.size());
    for (const auto& a : typed_aabbs)
    {
        raw.push_back(detail::strip_aabb(a));
    }
    return pipeline.dispatch_morton_codes(
        crd::containers::ConstSpan<crd::geometry::primitives::AABB3<T>>(raw.data(), raw.size()),
        detail::strip_aabb(typed_scene),
        alloc);
}

} // namespace crd::geometry::bvh_gpu
