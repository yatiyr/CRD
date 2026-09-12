#pragma once

// Strong-type identifiers + closed enum tags for the eylem public surface.
// Layout pinned via static_assert; see ADR-0062.

#include <crd/core/types.hpp>

namespace crd::eylem
{
// ---------------------------------------------------------------------------
// Strong-type identifiers
//
// 32-bit handles into per-type pools. Layout [generation:8 | index:24] —
// 16M live bodies/colliders/joints with 256 generation values per slot.
// Generation guards against use-after-free across slot recycling.
//
// Slot index 0 is reserved as null. Default-init = null. Strong typing
// prevents passing a BodyId where a ColliderId was expected.
// ---------------------------------------------------------------------------

struct BodyId
{
    crd::u32 raw = 0;

    [[nodiscard]] constexpr crd::u32 index() const noexcept { return raw & 0x00FF'FFFFu; }

    [[nodiscard]] constexpr crd::u32 generation() const noexcept { return raw >> 24; }

    [[nodiscard]] constexpr bool is_null() const noexcept { return raw == 0; }

    [[nodiscard]] static constexpr BodyId null() noexcept { return BodyId{0}; }

    [[nodiscard]] static constexpr BodyId make(crd::u32 index, crd::u32 generation) noexcept
    {
        return BodyId{((generation & 0xFFu) << 24) | (index & 0x00FF'FFFFu)};
    }

    [[nodiscard]] constexpr bool operator==(const BodyId& other) const noexcept = default;
};

struct ColliderId
{
    crd::u32 raw = 0;

    [[nodiscard]] constexpr crd::u32 index() const noexcept { return raw & 0x00FF'FFFFu; }

    [[nodiscard]] constexpr crd::u32 generation() const noexcept { return raw >> 24; }

    [[nodiscard]] constexpr bool is_null() const noexcept { return raw == 0; }

    [[nodiscard]] static constexpr ColliderId null() noexcept { return ColliderId{0}; }

    [[nodiscard]] static constexpr ColliderId make(crd::u32 index, crd::u32 generation) noexcept
    {
        return ColliderId{((generation & 0xFFu) << 24) | (index & 0x00FF'FFFFu)};
    }

    [[nodiscard]] constexpr bool operator==(const ColliderId& other) const noexcept = default;
};

struct JointId
{
    crd::u32 raw = 0;

    [[nodiscard]] constexpr crd::u32 index() const noexcept { return raw & 0x00FF'FFFFu; }

    [[nodiscard]] constexpr crd::u32 generation() const noexcept { return raw >> 24; }

    [[nodiscard]] constexpr bool is_null() const noexcept { return raw == 0; }

    [[nodiscard]] static constexpr JointId null() noexcept { return JointId{0}; }

    [[nodiscard]] static constexpr JointId make(crd::u32 index, crd::u32 generation) noexcept
    {
        return JointId{((generation & 0xFFu) << 24) | (index & 0x00FF'FFFFu)};
    }

    [[nodiscard]] constexpr bool operator==(const JointId& other) const noexcept = default;
};

// MaterialId — handle into the scene-owned MaterialPool. Per ADR-0069 §3.
//
// Layout matches BodyId / ColliderId / JointId: [generation:8 | index:24].
// Slot 0 reserved as null sentinel; slot 1 is the shipped `default_material()`
// (always allocated at scene construction). The id is content-addressed via
// FNV-1a-64 over the canonical material parameter bytes by the cooker /
// öbek path; identical material parameters produce identical ids regardless
// of authoring order — the same discipline FieldId already follows in
// ADR-0067 §3 for cross-platform replay-hash determinism.
struct MaterialId
{
    crd::u32 raw = 0;

    [[nodiscard]] constexpr crd::u32 index() const noexcept { return raw & 0x00FF'FFFFu; }

    [[nodiscard]] constexpr crd::u32 generation() const noexcept { return raw >> 24; }

    [[nodiscard]] constexpr bool is_null() const noexcept { return raw == 0; }

    [[nodiscard]] static constexpr MaterialId null() noexcept { return MaterialId{0}; }

    // Slot 1 is the shipped `Default` material (universal fallback per
    // ADR-0069 §10). Generation 1 = initial allocation. Always allocated at
    // scene construction; never invalidated.
    [[nodiscard]] static constexpr MaterialId default_material() noexcept
    {
        return MaterialId::make(/*index=*/1U, /*generation=*/1U);
    }

    [[nodiscard]] static constexpr MaterialId make(crd::u32 index, crd::u32 generation) noexcept
    {
        return MaterialId{((generation & 0xFFu) << 24) | (index & 0x00FF'FFFFu)};
    }

    [[nodiscard]] constexpr bool operator==(const MaterialId& other) const noexcept = default;
};

// API surface freeze pins (ADR-0062 §15).
static_assert(sizeof(BodyId)     == 4, "BodyId must pack to 4 bytes");
static_assert(sizeof(ColliderId) == 4, "ColliderId must pack to 4 bytes");
static_assert(sizeof(JointId)    == 4, "JointId must pack to 4 bytes");
static_assert(sizeof(MaterialId) == 4, "MaterialId must pack to 4 bytes");

// ---------------------------------------------------------------------------
// Closed enum tags
// ---------------------------------------------------------------------------

// Determinism contract per ADR-0063. The mode the scene runs under decides
// which guards the implementation enforces.
enum class DeterminismMode : crd::u8
{
    // Same-machine same-compiler bit-exact replay. Default.
    Default = 0,
    // Bit-exact across MSVC/clang/gcc x86/ARM. Requires the v0c
    // crd::math::deterministic substrate + commutative cross-thread merges
    // (eylem v9b 9-config replay-hash CI).
    CrossPlatform = 1,
    // Reserved for serialise-stable across engine versions (post-v9).
    BackwardCompat = 2
};

// Continuous collision detection mode (v6+). v1 always uses Discrete.
enum class CCDMode : crd::u8
{
    Discrete   = 0,
    Linear     = 1,   // sweep test against translation only
    Full       = 2    // sweep against full motion (linear + rotational)
};

// How two material properties combine when bodies collide. Matches the
// PhysX / Bullet convention.
enum class CombineMode : crd::u8
{
    Average       = 0, // (a + b) * 0.5
    Min           = 1, // min(a, b)
    Max           = 2, // max(a, b)
    Multiply      = 3, // a * b
    GeometricMean = 4, // sqrt(a * b)  -- ADR-0069 §2 (Box2D v3 / Jolt / Unity DOTS / AGX
                       //                 consensus default for friction; stacking-stable)
};

// FrictionModel — per-material friction law selector. Per ADR-0069 §2.
//
// Coulomb is the universal default (constant μ; bit-exact across compilers).
// Stribeck adds velocity-dependent low-speed dip (vehicle tires + robotics
// manipulation); LuGre adds state-variable bristle dynamics (industrial
// manipulation, Canudas-de-Wit 1995); Karnopp is the piecewise stick-slip
// numerical-stability fallback when Stribeck integration is too stiff
// (vehicle ODE solvers); Anisotropic is direction-dependent μ (tires,
// ice, conveyors). FrictionTriple reserves the slot for v5 vehicles'
// MuJoCo-style sliding/torsional/rolling triple — same `friction_anisotropy`
// Vec3f, different reading; struct does NOT grow.
//
// Coulomb is shipped in v1e; the other five fill formula impls in their
// natural domain slices (v5 vehicles / v8d MPM) inside the v1l-frozen
// surface — same blocked-sub-slice discipline as ADR-0067's Gradient/Script.
enum class FrictionModel : crd::u8
{
    Coulomb        = 0, // default; constant μ
    Stribeck       = 1, // velocity-dependent low-speed dip (v5)
    LuGre          = 2, // state-variable; per-contact bristle (v5)
    Karnopp        = 3, // dead-zone piecewise (vehicle ODE) (v5)
    Anisotropic    = 4, // Vec3f friction in material-local frame (v5)
    FrictionTriple = 5, // sliding/torsional/rolling triple (MuJoCo §2.8) (v5)
};

// RestitutionModel — per-material restitution law selector. Per ADR-0069 §2.
//
// Constant is the universal default (Newtonian CoR; e ∈ [0, 1]). Newton
// adds velocity-dependent decay (real bouncing bodies attenuate with speed).
// HuntCrossley is the compliant-contact constitutive law (Drake's
// hydroelastic substrate; cinematic actor falls; medical soft tissue) —
// `restitution` reinterpreted as compliance stiffness, `restitution_decay`
// as dissipation coefficient.
//
// Constant is shipped in v1e; Newton ships in v8d MPM; HuntCrossley
// ships in v7 FEM — all inside the v1l-frozen Material surface.
enum class RestitutionModel : crd::u8
{
    Constant     = 0, // default; e ∈ [0, 1]
    Newton       = 1, // velocity-dependent: e(v) = e_0 · exp(-α · |v|) (v8d)
    HuntCrossley = 2, // compliant: F = k · δ^n · (1 + 1.5 · d · δ̇) (v7 FEM)
};

} // namespace crd::eylem
