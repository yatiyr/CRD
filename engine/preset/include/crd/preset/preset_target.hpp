#pragma once

#include <crd/preset/camera_preset.hpp>
#include <crd/preset/quality_preset.hpp>

namespace crd::preset
{
// IPresetTarget — base interface for any system that consumes preset values
// (renderer, camera, audio device, physics, input map).
//
// Concrete `apply(const SomePreset&)` overloads are added to this base as
// new preset types ship. Targets override only the overloads they care
// about; the default-empty bodies make that mechanical (no override of
// overloads they don't consume).
//
// Per-type overloads landing in Phase 3.0:
//   v1n1 — base interface only.
//   v1n2 — apply(const QualityPreset&)
//   v1n3 — apply(const CameraPreset&)          ← THIS SLICE.
//
// Phase 4+ overloads add audio/physics/input variants in their consumer
// modules; the per-overload pattern keeps the registration grammar closed
// (no string lookup at apply time).
//
// Resolution semantics (ADR-0059 §2 — five-layer stack):
//   The PresetRegistry resolves the active value across L0 (schema default),
//   L1 (extends chain), L2 (active preset, ADR-0060), L3 (per-instance),
//   and L4 (runtime override) BEFORE calling apply(). Targets receive the
//   final fully-resolved value and cache it; per-frame resolution cost is
//   zero.
//
// **Partial-override convention** — when a derived class overrides only
// some of the apply() overloads (e.g. a render path that only consumes
// QualityPreset), it MUST import the unhidden overloads with
// `using IPresetTarget::apply;` to avoid C++ name-hiding rules silently
// dropping the inherited defaults. GCC enforces this with
// `-Woverloaded-virtual` (treated as -Werror in the project's Linux
// configs). Pattern:
//
//     class MyTarget : public crd::preset::IPresetTarget
//     {
//     public:
//         using IPresetTarget::apply;          // import unhidden overloads
//         void apply(const QualityPreset& q) override { /* ... */ }
//     };
class IPresetTarget
{
public:
    virtual ~IPresetTarget() = default;

    IPresetTarget(const IPresetTarget&)            = delete;
    IPresetTarget& operator=(const IPresetTarget&) = delete;
    IPresetTarget(IPresetTarget&&)                 = delete;
    IPresetTarget& operator=(IPresetTarget&&)      = delete;

    // Per-type apply overloads. Default bodies are intentionally empty so
    // targets that don't consume a given preset type don't need to define
    // a stub — the override is purely opt-in.
    virtual void apply(const QualityPreset& /*preset*/) {}
    virtual void apply(const CameraPreset&  /*preset*/) {}

protected:
    IPresetTarget() noexcept = default;
};

} // namespace crd::preset
