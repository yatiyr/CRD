#pragma once

// ForceFieldComponent — eylem's force-field substrate API surface.
// Phase 3.1 v1f-fields-a (planned). API frozen here at v1l (ADR-0062 §15);
// formula impls fill in inside the frozen surface across v1f-fields-a..i.
//
// This header DECLARES the public contract so:
//   - cookers (`.field.toml`, `.field.crdr`) can validate against the
//     enum surface without the impl module being built;
//   - tooling (Phase 7 editor property panel, sandbox demo) compiles
//     against the surface today;
//   - öbek prefab serialisation (ADR-0058) round-trips ForceFieldComponent
//     bytes from day one of v1f-fields-a;
//   - the crd-eylem-rigid3d impl module fills in `EylemFieldSystem` inside
//     this surface.
//
// Architecture: ADR-0067 (eylem force-field architecture).
// Determinism contract: ADR-0063.
// Industry survey + algorithm rationale: docs/research/cerid-eylem-fields.md.
//
// IMPL STATUS — every formula returns an unconditional zero force from
// `EylemFieldSystem` until its slice ships. Calling `register_component<
// ForceFieldComponent>()` is supported today (v1l API freeze); attaching
// the component to an entity simply stores the bytes and is observed by
// no system until v1f-fields-a registers `EylemFieldSystem`. This is the
// SAME pattern v1a IPhysicsScene uses (NullPhysicsScene impl ships day
// one; real impls land per slice).

#include <crd/core/types.hpp>
#include <crd/math/quat.hpp>
#include <crd/math/vec.hpp>
#include <crd/units/quantity_aliases.hpp>

namespace crd::eylem
{
// ---------------------------------------------------------------------------
// Closed enum surface — locked at v1l API freeze.
// ---------------------------------------------------------------------------

// FieldFormula — the 9 canonical formulas the substrate ships. Each is the
// final answer for a category of physical/authoring intent the industry
// survey identified; collapsing related formulas (Coulomb/Newton/Hooke
// → Radial parameterised) keeps the cooker grammar terse.
//
// Per ADR-0067 §3, the enum is closed — new formulas require a major-
// version bump. New formula candidates SHOULD prove themselves as
// `Script` first; promotion to a permanent slot is by ADR.
enum class FieldFormula : crd::u8
{
    Directional = 0, // f = direction · magnitude
    Radial      = 1, // f = polarity · magnitude / (r + radius_min)^falloff_p · r̂
    Vortex      = 2, // f = magnitude · (axis × (p − origin))
    Drag        = 3, // f = -magnitude · v · |v|^(falloff_p − 1)
    Noise       = 4, // f = magnitude · curl_noise(p, t, scale, octaves)
                     //   analytic-derivative Simplex per Bridson 2007.
                     //   DETERMINISM-CRITICAL: see ADR-0067 §7.
    Magnetic    = 5, // f = polarity · magnitude · (v × direction)
    Gradient    = 6, // f = polarity · magnitude · ∇φ(p) — sampled from a
                     //   crd-sdf SdfResource via `sdf` handle.
    GridSample  = 7, // f = magnitude · trilinear_sample(VectorGrid, p)
                     //   Tier 2 — consumes a cooked .field.crdr.
    Script      = 8, // f = ScriptComponent::eval_field(...) — Tier 3
                     //   (Phase 4 scripting prereq).
};

// FieldFalloff — magnitude attenuation across the field volume. Six
// values, locked at v1l. Polynomial covers the long-tail (cubic,
// near-exponential decay) without exploding the enum.
enum class FieldFalloff : crd::u8
{
    Constant      = 0, // f stays at full magnitude inside volume
    Linear        = 1, // f scales (1 − r / radius_max)
    InverseLinear = 2, // f scales 1 / (r + radius_min)
    InverseSquare = 3, // f scales 1 / (r + radius_min)^2 — true Newton/Coulomb
    Smoothstep    = 4, // f scales smoothstep(radius_max, radius_min, r) — C¹
    Polynomial    = 5, // f scales by `poly_coeffs` (cubic) — escape hatch
};

// FieldMassCoupling — how the formula's vector becomes a force. `Force`
// hits heavy bodies less; `Acceleration` hits all bodies the same;
// `GravityStyle` matches Earth-style gravity (mass-couple to cancel mass
// in the equation of motion). `Impulse` is for OnEnter / OnEnterOnce
// triggers. `VelocitySet` matches Maya nDynamics' Air field — replaces
// velocity component instead of accumulating force.
enum class FieldMassCoupling : crd::u8
{
    Force        = 0, // f applied as raw force (a = f / m)
    Acceleration = 1, // f applied as acceleration (mass-independent)
    GravityStyle = 2, // f scaled by m internally then applied as force
    Impulse      = 3, // ∫f dt accumulated; one-shot for triggers
    VelocitySet  = 4, // v_new = lerp(v_old, f, blend) — Air-style replacement
};

// FieldComposition — when N fields overlap a body, how do their
// contributions combine? Per-field setting (each field independently
// declares how IT enters the accumulator). Application order is
// id-stable (ascending FieldId) per ADR-0067 §6 / ADR-0063.
enum class FieldComposition : crd::u8
{
    Add      = 0, // accumulate (vector sum) — default
    Replace  = 1, // overwrite the accumulator with this field's contribution
    Multiply = 2, // multiply the accumulator (damping / scaling)
    Max      = 3, // componentwise max (clip-style)
    Min      = 4, // componentwise min
};

// FieldTrigger — when does the field fire?
enum class FieldTrigger : crd::u8
{
    Continuous   = 0, // every substep while body overlaps the volume — default
    OnEnter      = 1, // one-shot impulse when body enters
    OnExit       = 2, // one-shot impulse when body exits
    OnEnterOnce  = 3, // OnEnter, then auto-disables (fire-and-forget)
};

// ---------------------------------------------------------------------------
// ForceFieldComponent — the per-entity force-field declaration.
//
// One field per entity. Multi-formula compound fields (e.g., wind +
// turbulence in the same volume) are authored as multiple sibling
// entities under the same öbek root — each with its own
// ForceFieldComponent — letting each formula carry its own composition
// rule + parameters. Composition is at the SOLVER level, not at the
// component level. Cleaner serialisation, simpler bench surface, no
// "compound formula" enum dimension.
//
// Storage hint: SparseSet (low total count, high per-frame read rate).
// Serialisation: ComponentSerialize trait, FourCC 'EYFF', version 1.
// ---------------------------------------------------------------------------

// Forward-declared resource handles. Real definitions ship in their
// owning modules (`crd-resources` for the template; the concrete resource
// types ship in their substrate modules: `VectorGridResource` in
// `crd-eylem-rigid3d`, `SdfResource` in `crd-sdf`). Forward-declaring
// here keeps `crd-eylem` thin (no resource-system dep at the interface
// layer).
class VectorGridResource;
class SdfResource;
class ScriptComponentHandle; // Phase 4 scripting placeholder

struct ForceFieldComponent
{
    // ── Closed-enum surface ────────────────────────────────────────────
    FieldFormula      formula       = FieldFormula::Directional;
    FieldFalloff      falloff       = FieldFalloff::Constant;
    FieldMassCoupling mass_coupling = FieldMassCoupling::Force;
    FieldComposition  composition   = FieldComposition::Add;
    FieldTrigger      trigger       = FieldTrigger::Continuous;
    crd::u8           _pad0[3]      = {0, 0, 0};

    // ── Formula parameters ─────────────────────────────────────────────
    // Interpreted per `formula`. Fields that the formula doesn't read
    // are ignored (cooker zeroes them for clean serialisation hash).
    //
    // Typing rule (ADR-0078 §3 D21): GEOMETRIC parameters (positions /
    // distances / lengths) carry SI Length32; DIRECTIONS are unit vectors
    // (dimensionless f32); FORMULA COEFFICIENTS (magnitude / polarity /
    // falloff_p / poly_coeffs / noise_*) stay polymorphic raw f32 because
    // their dimension depends on `formula` + `mass_coupling`. The cooker
    // emits raw and EylemFieldSystem re-tags at use time per formula.
    crd::math::Vec3f                       direction  {0.0F, -1.0F, 0.0F}; // Directional / Magnetic (B vector) — dimensionless unit vector
    crd::math::Vec3f                       axis       {0.0F,  1.0F, 0.0F}; // Vortex axis / GravityStyle up — dimensionless unit vector
    crd::math::Vec3<crd::units::Length32>  origin     {};                   // Vortex centre / Radial centre (local-space)
    crd::f32                               magnitude      = 1.0F;          // formula-polymorphic
    crd::units::Length32                   radius_min     {0.01F};         // Radial / Magnetic — singularity guard
    crd::units::Length32                   radius_max     {1.0F};          // Linear / Smoothstep cutoff
    crd::f32                               falloff_p      = 2.0F;          // dimensionless exponent
    crd::f32                               polarity       = 1.0F;          // +1 / -1 sign
    crd::math::Vec4f                       poly_coeffs    {0.0F, 0.0F, 0.0F, 0.0F}; // Polynomial falloff coefficients (dimensionless)
    crd::units::Length32                   noise_scale    {1.0F};          // Noise spatial frequency (= 1/wavelength, but stored as wavelength in m)
    crd::f32                               noise_time     = 0.0F;          // Noise time advance (seconds — could be Duration32 but treated as raw at the noise() call site)
    crd::u32                               noise_octaves  = 4U;            // Noise octave count

    // ── Tier 2 / Tier 3 handles (only one is used, by formula) ─────────
    // Type-erased pointers — concrete types ship in their owning
    // modules. The crd-eylem interface layer carries opaque void* +
    // a formula tag so the ECS storage stays self-contained. Resolution
    // happens in EylemFieldSystem at the impl-module layer.
    void*             grid_handle   = nullptr; // GridSample (VectorGridResource*)
    void*             sdf_handle    = nullptr; // Gradient   (SdfResource*)
    void*             script_handle = nullptr; // Script     (ScriptComponentHandle*)

    // ── Determinism-stable identity ────────────────────────────────────
    // FNV-1a hash of (all enum + scalar fields above + entity öbek path).
    // Set at registration time by the cooker / scene loader; mutating
    // mid-run breaks the determinism contract (ADR-0063). 0 = unstamped.
    crd::u64          field_id      = 0ULL;

    // ── Composition DAG (rare) ─────────────────────────────────────────
    // Field ids this field reads after applying. Default empty = parallel
    // evaluation. Used when a velocity-dependent field (Drag, Magnetic)
    // must read the post-update velocity from earlier fields. Topologically
    // sorted in setup; applied in waves at substep.
    //
    // Fixed-size array of 8 dependency ids — plenty for any practical
    // compound field; keeps ForceFieldComponent an aggregate (designated
    // initialisers just work) and pins the byte layout for öbek
    // serialisation. If a field needs >8 dependencies, that's a pattern
    // smell — author it as multiple sibling fields with their own DAG.
    static constexpr crd::u32 kMaxApplyAfter = 8U;
    crd::u32                  apply_after_count            = 0U;
    crd::u64                  apply_after_ids[kMaxApplyAfter] = {};
};

// API surface freeze pin (ADR-0062 §15). The exact size will be
// re-asserted at v1l close once the alignment + Array layout settles
// across compilers; for now we lock the SHAPE (enum + scalar field
// surface) and defer the byte-precise pin.
//
// TODO (v1f-fields-a): bracket the size with a static_assert once the
// storage hits real consumers. Until then the unit tests verify
// per-field round-trip rather than total byte count.

} // namespace crd::eylem
