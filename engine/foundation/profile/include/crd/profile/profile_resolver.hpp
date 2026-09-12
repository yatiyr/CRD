#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/profile/profile_context.hpp>
#include <crd/profile/profile_predicate.hpp>
#include <crd/profile/profile_resource.hpp>
#include <crd/resources/resource_id.hpp>

namespace crd::profile
{
// Phase 3.0 v1n6 — runtime resolver (ADR-0060 §3, §4, §7).
//
// Closes the Profile substrate by adding:
//   - evaluate_predicate(record, ctx) — per-field per-op comparison.
//   - ProfileResolver::resolve(ctx, out) — match → priority-sort → concat.
//   - detect_os() / detect_cpu_cores() helpers built from crd-core's
//     compile-time CRD_OS_* macros and <thread>.
//
// Composition semantics (ADR-0060 §3):
//   1. Match every profile whose every predicate evaluates true.
//   2. Sort matched profiles by priority ascending.
//   3. Concatenate their apply_bundle ResourceIds in priority order.
//   4. Same preset id appearing in multiple profiles' bundles is preserved
//      in order — duplicate apply is idempotent at the IPresetTarget
//      level, and the LAST application wins per field (which is the
//      "highest-priority wins" of ADR §3 step 4).
//
// Determinism (ADR-0060 §7): predicate evaluation order is the FRLE byte
// order; tie-break for equal-priority profiles falls back to that file
// order via stable sort. No floating-point in predicates.

// Predicate primitive — pure function. Returns true if `record` matches
// the corresponding axis of `ctx`. v1n6 evaluates Equal / GreaterEq /
// LessEq / InMask against the closed predicate-field schema. Integer and
// enum fields are compared via canonical signed-i64 widening so a
// negative i32 comparand round-trips through PredicateRecord::value
// (which is u32-bitcast).
[[nodiscard]] bool evaluate_predicate(const PredicateRecord& record,
                                      const ProfileContext&  ctx) noexcept;

// ProfileResolver — borrows a loaded ProfileResource and resolves bundles
// against runtime contexts. The resolver is move-only and not thread-safe;
// callers should hold one per worker if cross-thread resolution is needed.
class ProfileResolver
{
public:
    explicit ProfileResolver(crd::memory::IAllocator* alloc) noexcept;

    ProfileResolver(const ProfileResolver&)            = delete;
    ProfileResolver& operator=(const ProfileResolver&) = delete;
    ProfileResolver(ProfileResolver&&) noexcept        = default;
    ProfileResolver& operator=(ProfileResolver&&) noexcept = default;
    ~ProfileResolver()                                 = default;

    // Bind / rebind a loaded resource. Pointer ownership stays with the
    // ResourceManager; the resolver just borrows. Passing nullptr clears.
    void set_resource(const ProfileResource* resource) noexcept;

    [[nodiscard]] const ProfileResource* resource() const noexcept { return m_resource; }

    // Evaluate every profile against `ctx`, sort matches by priority
    // ascending (stable; tie-break on profile index), concatenate their
    // apply_bundle into `out`. Clears `out` first.
    //
    // Returns the number of matching profiles. The caller owns `out`'s
    // backing storage (typically a per-frame-arena Array).
    crd::u32 resolve(const ProfileContext&                                ctx,
                     crd::containers::Array<crd::resources::ResourceId>&  out) const;

private:
    crd::memory::IAllocator* m_alloc;
    const ProfileResource*   m_resource = nullptr;
};

// Compile-time OS detection via CRD_OS_* macros. Returns OperatingSystem::Unknown
// on platforms outside the closed enum (no current target hits this).
[[nodiscard]] OperatingSystem detect_os() noexcept;

// std::thread::hardware_concurrency. Returns 1 if the runtime fails to detect
// the count (rare; the enum's defaults pin a sensible floor).
[[nodiscard]] crd::i32 detect_cpu_cores() noexcept;

} // namespace crd::profile
