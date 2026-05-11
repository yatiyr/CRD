#pragma once

// Material — per-collider friction / restitution / surface-velocity / density.
// Phase 3.1 v1a-material-a (ADR-0069). API surface frozen at v1a interface
// freeze; sizeof(Material) == 64 (one cache line) is the locked pin.
//
// Per ADR-0069 §1, the struct shape carries the FULL multi-domain catalogue
// in 64 bytes via enum-gated slot interpretation:
//
//   - friction_model selects how the friction parameters are read
//     (Coulomb / Stribeck / LuGre / Karnopp / Anisotropic / FrictionTriple)
//   - restitution_model selects how the restitution parameters are read
//     (Constant / Newton / HuntCrossley)
//   - friction_anisotropy is reinterpreted as (sliding/torsional/rolling)
//     when friction_model == FrictionTriple (MuJoCo §2.8 pattern)
//
// Coulomb + Constant ship in v1e (the v1 critical path); the other models
// fill formula impls inside this frozen surface in their natural slices
// (v5 vehicles for Stribeck/LuGre/Karnopp/Anisotropic/FrictionTriple;
// v7 FEM for HuntCrossley; v8d MPM for Newton; post-v1 destruction for
// `yield_stress`). Same blocked-sub-slice discipline as ADR-0067's
// Gradient/Script.
//
// Materials are referenced via `MaterialId` (declared in types.hpp,
// matching BodyId/ColliderId/JointId pattern). Scene owns the
// `MaterialPool`; colliders carry `MaterialId` handles. Per-collider
// granularity is the universal modern choice (PhysX `PxShape::material`,
// Jolt `Shape::SubShapeMaterial`, Box2D v3 `b2ShapeDef::material`) — a
// compound character body carries rubber boots + leather gloves + bare
// skin with three materials, contact resolver sees per-contact materials.

#include <crd/core/types.hpp>
#include <crd/eylem/types.hpp>
#include <crd/math/vec.hpp>

namespace crd::eylem
{
struct Material
{
    // ── Friction (24 bytes) ─────────────────────────────────────────────
    // friction_model selects how the parameters below are interpreted:
    //   Coulomb     — friction_static + friction_dynamic; anisotropy ignored
    //   Stribeck    — adds stribeck_velocity (v_s) + viscous_coefficient (α)
    //   LuGre       — overloads {stribeck_velocity → σ_0, viscous_coefficient → σ_2};
    //                 σ_1 lives in per-contact bristle cache (ADR-0069 §6)
    //   Karnopp     — friction_static + friction_dynamic + dead-zone from
    //                 stribeck_velocity (used as v_thresh)
    //   Anisotropic — friction_anisotropy is per-axis μ in material-local frame
    //   FrictionTriple — friction_anisotropy reinterpreted as
    //                    (sliding, torsional, rolling) per MuJoCo §2.8
    FrictionModel    friction_model       = FrictionModel::Coulomb;
    CombineMode      friction_combine     = CombineMode::GeometricMean;  // ADR-0069 §2 default
    crd::u8          _pad_friction[2]     = {0, 0};                       // alignment to f32
    crd::f32         friction_static      = 0.5F;                         // μ_s
    crd::f32         friction_dynamic     = 0.5F;                         // μ_d
    crd::math::Vec3f friction_anisotropy  {1.0F, 1.0F, 1.0F};             // material-local frame

    // ── Friction-model parameters (8 bytes) ─────────────────────────────
    // Slot interpretation gated by `friction_model`:
    //   Stribeck:  v_s (m/s) + α (viscous coeff, unitless)
    //   LuGre:     σ_0 (bristle stiffness, N/m) + σ_2 (viscous, N·s/m)
    //              — σ_1 (bristle damping) lives in the per-contact warm-start
    //                cache (ADR-0069 §6); cooker precomputes Tustin constants
    //   Karnopp:   v_thresh (dead-zone half-width, m/s) + viscous coeff
    //   Coulomb / Anisotropic / FrictionTriple: ignored
    crd::f32         stribeck_velocity    = 0.01F;
    crd::f32         viscous_coefficient  = 0.0F;

    // ── Restitution (12 bytes) ──────────────────────────────────────────
    // restitution_model selects how the parameters are interpreted:
    //   Constant     — restitution = e ∈ [0, 1]; restitution_decay ignored
    //   Newton       — restitution = e_0; restitution_decay = α decay rate
    //   HuntCrossley — restitution = compliance stiffness k (N/m^n);
    //                  restitution_decay = dissipation d (s/m)
    RestitutionModel restitution_model    = RestitutionModel::Constant;
    CombineMode      restitution_combine  = CombineMode::Max;             // ADR-0069 §2 default (PhysX convention)
    crd::u8          _pad_restitution[2]  = {0, 0};
    crd::f32         restitution          = 0.0F;
    crd::f32         restitution_decay    = 0.0F;

    // ── Surface (12 bytes) ──────────────────────────────────────────────
    // Material-local-frame velocity added to the contact's relative velocity
    // at solver time. Drives conveyor belts, rolling tires, escalators,
    // treadmills, water currents on solid colliders. Per ADR-0069 §5; cleaner
    // than the after-the-fact ContactModify hook (ADR-0068 §10.6) because
    // designers author it on the material, not the collider.
    crd::math::Vec3f surface_velocity     {0.0F, 0.0F, 0.0F};

    // ── Mass derivation (4 bytes) ───────────────────────────────────────
    // kg/m^3. Used to derive body mass from Σ(collider_volume · density)
    // when RigidBody::inv_mass == 0 (the default — "derive"). Otherwise the
    // authored inv_mass overrides. Default 1000.0 = water; designer-friendly
    // (a 1m³ box → 1000 kg). Summation runs in ascending ColliderId order
    // for FP-determinism (ADR-0063 §4 fixed-position-write protocol).
    crd::f32         density              = 1000.0F;

    // ── Damage / fracture reservation (4 bytes; v1 ignores) ─────────────
    // Pa. Reserved for the post-v1 destruction substrate
    // (`GeometryCollectionComponent` per ADR-0068 §10.8). v1 reads but does
    // not act on it; reserving the bytes here prevents a struct-growth
    // post-v1l-freeze that would cascade through cooker artifact format,
    // snapshot artifact format, and every shipped öbek prefab.
    crd::f32         yield_stress         = 0.0F;
};

// API surface freeze pin (ADR-0069 §1, ADR-0062 §15).
//   Friction (24) + friction-params (8) + Restitution (12) + Surface (12)
//   + Density (4) + Yield (4) = 64 bytes (one cache line).
static_assert(sizeof(Material)  == 64, "Material must pack to 64 bytes (one cache line)");
static_assert(alignof(Material) ==  4, "Material alignment is 4 (largest member f32)");

// Default material returned by MaterialId::default_material(). Equivalent
// to a fresh Material{} via the locked defaults — the scene allocates this
// at slot 1 (slot 0 = null sentinel) at construction time so unset
// `Collider::material` handles always resolve cleanly.
[[nodiscard]] constexpr Material default_material_value() noexcept
{
    return Material{};
}

} // namespace crd::eylem
