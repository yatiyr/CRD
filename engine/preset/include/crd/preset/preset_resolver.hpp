#pragma once

#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/preset/preset_resource.hpp>
#include <crd/preset/preset_target.hpp>

#include <cstring>

namespace crd::preset
{
// Phase 3.0 v1n4 — runtime resolver for the five-layer stack (ADR-0059 §2).
//
//   Highest precedence
//     L4 ── Runtime override   (CVar / ImGui slider / debug toggle) — ephemeral
//     L3 ── Per-instance       (per-entity in scene)                — reserved (Phase 4+)
//     L2 ── Active preset      (resolved by Profile, ADR-0060)      — PDAT bytes
//     L1 ── extends chain      (deepest extends wins per field)     — pre-resolved at cook time
//     L0 ── Schema default     (compile-time)                       — `T{}`
//   Lowest precedence
//
// At runtime the resolver only sees the COOKED resource. The cooker has
// already flattened L0 (schema defaults) + L1 (extends chain) + L2 (active
// preset's user-set values) into the PDAT bytes via deepest-first walk
// (shared Öbek resolver per ADR-0058). The loaded `PresetResource` therefore
// represents the L0+L1+L2 result; this function adds optional L4 on top.
//
// Resolution happens at APPLY TIME, not query time — the applied target
// caches the resolved value until the next apply, so per-frame cost is zero.
//
// L3 (per-instance) is intentionally left out. ADR-0059 §"Open questions"
// reserves it for Phase 4+ when a real consumer surfaces (rare case: one
// camera ignores the active QualityPreset).
//
// The schema struct contract (T must satisfy):
//   - static constexpr crd::u32 T::fourcc
//   - static constexpr crd::u32 T::version
//   - T is a trivially-copyable POD aggregate (memcpy is the merge primitive)
//
// Compile-time-checked via static_assert; the FourCC / size match are
// runtime-checked via CRD_ASSERT (tripping a debug assert on mismatch is
// the right behaviour — it indicates a type/loader registration bug).
template <typename T>
[[nodiscard]] T resolve_preset(const PresetResource* resource,
                               const T*              runtime_override = nullptr) noexcept
{
    static_assert(static_cast<crd::u32>(T::fourcc) != 0U,
                  "Preset schema struct must declare `static constexpr crd::u32 fourcc`");
    static_assert(static_cast<crd::u32>(T::version) >= 1U,
                  "Preset schema version must be >= 1");

    T value{}; // L0 — schema default

    if (resource != nullptr)
    {
        CRD_ASSERT_MSG(resource->fourcc() == T::fourcc,
                       "resolve_preset<T>: PresetResource fourcc does not match T::fourcc");
        CRD_ASSERT_MSG(resource->bytes().size() == sizeof(T),
                       "resolve_preset<T>: PresetResource payload size does not match sizeof(T)");
        // L1 + L2 — cooker pre-resolved deepest-first extends chain into PDAT.
        std::memcpy(&value, resource->bytes().data(), sizeof(T));
    }

    // L3 — per-instance reserved (Phase 4+).

    if (runtime_override != nullptr)
    {
        // L4 — caller-supplied full-T override. Field-mask / partial-field
        // override is reserved for v1o3+ when a real consumer (sandbox
        // quality slider, ImGui debug toggle) drives the API shape.
        value = *runtime_override;
    }

    return value;
}

// Resolve + dispatch in one call. Equivalent to:
//   target.apply(resolve_preset<T>(resource, runtime_override));
// but reads at the call site as a single intent ("apply this preset to
// this target") and matches the ADR-0059 §"reserved API surface" sketch.
template <typename T>
void apply_preset(IPresetTarget&        target,
                  const PresetResource* resource,
                  const T*              runtime_override = nullptr) noexcept
{
    target.apply(resolve_preset<T>(resource, runtime_override));
}

} // namespace crd::preset
