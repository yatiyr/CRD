# ADR-0069 — Eylem materials substrate

> Status: **Accepted** (2026-05-11)
> Companions: [ADR-0062](0062-eylem-physics-architecture.md) (eylem
> architecture; §5.5 reserves Material), [ADR-0063](0063-eylem-determinism-contract.md)
> (determinism contract), [ADR-0065](0065-hesap-numerical-substrate.md)
> §3 (reference-implementation posture), [ADR-0066](0066-draw-substrate-architecture.md)
> (`crd-draw`), [ADR-0067](0067-eylem-force-field-architecture.md) (force
> fields), [ADR-0068](0068-eylem-body-types-collision-filtering-callbacks.md)
> (body / filter / callback substrate), [ADR-0075](0075-eylem-testing-rigor.md)
> (testing rigor — conservation tests consume Material).
> Research dossier: [`docs/research/cerid-eylem-materials.md`](../research/cerid-eylem-materials.md).

## Context

The current v1a `Material` struct (`engine/eylem/include/crd/eylem/material.hpp`,
20 bytes) is a placeholder shipped to satisfy linkage at v1a freeze.
Coverage audit §3.1 marked it **P0 — must close before v1a interface
freeze** because the public `Material` shape gates the entire
downstream API:

- **Vehicles (v5)** need anisotropic friction at the API surface
  (tires have direction-dependent μ — direction-of-travel vs
  perpendicular slip is the foundation of any tire model from
  Pacejka onward).
- **Robotics manipulation** needs **LuGre friction** for stick-slip
  modelling at gripper contacts ([Canudas-de-Wit 1995](https://hal.science/hal-00394988/document)).
- **Cinematic actor falls** need **Hunt-Crossley compliant restitution**
  for soft-deformation contact (Drake's hydroelastic substrate is the
  reference; rigid-only constant CoR makes characters bounce like steel
  balls).
- **Scientific computing** wants `density` authoring + `Material`
  field overloads catalogued — the v9b replay-hash CI cannot accept
  a struct shape that grows after freeze.

The 14-engine industry survey (`cerid-eylem-materials.md`, 9,650
words) confirmed: every elite-tier physics engine — PhysX, Bullet,
Jolt, Box2D, Unity (classic + DOTS), Unreal Chaos, Godot, MuJoCo,
Drake, NVIDIA IsaacSim/Lab, Project Chrono, AGX Dynamics, Maya
nDynamics, Houdini Vellum, Vortex Studio, Blender — locks the
material struct shape **before** the rest of the contact-physics API.
Cerid follows the consensus.

## Decision

### 1. `Material` struct shape — locked at v1a freeze

Single 64-byte cache-line struct. Field overloading driven by
`FrictionModel` / `RestitutionModel` enum gates the slot
interpretation; struct does NOT grow as new models ship.

```cpp
struct Material
{
    // ----- Friction (24 bytes) -----
    FrictionModel    friction_model;       //  1B  Coulomb / Stribeck / LuGre / Karnopp / Anisotropic / FrictionTriple
    CombineMode      friction_combine;     //  1B  default GeometricMean
    crd::u8          _pad_friction[2];     //  2B
    crd::f32         friction_static;      //  4B  μ_s
    crd::f32         friction_dynamic;     //  4B  μ_d
    crd::math::Vec3f friction_anisotropy;  // 12B  material-local frame; reinterpreted by FrictionTriple

    // ----- Friction model parameters (8 bytes) -----
    crd::f32         stribeck_velocity;    //  4B  v_s (Stribeck) / σ_0 (LuGre)
    crd::f32         viscous_coefficient;  //  4B  α (Stribeck) / σ_2 (LuGre)

    // ----- Restitution (12 bytes) -----
    RestitutionModel restitution_model;    //  1B  Constant / Newton / HuntCrossley
    CombineMode      restitution_combine;  //  1B  default Max
    crd::u8          _pad_restitution[2];  //  2B
    crd::f32         restitution;          //  4B  e_0 (Constant, Newton); stiffness (HuntCrossley)
    crd::f32         restitution_decay;    //  4B  α (Newton); dissipation d (HuntCrossley)

    // ----- Surface (12 bytes) -----
    crd::math::Vec3f surface_velocity;     // 12B  material-local frame, m/s

    // ----- Mass derivation (4 bytes) -----
    crd::f32         density;              //  4B  kg/m³, default 1000 (water)

    // ----- Damage / fracture reservation (4 bytes; v1 ignores) -----
    crd::f32         yield_stress;         //  4B  Pa, used by post-v1 destruction
};

static_assert(sizeof(Material)  == 64, "Material must pack to 64 bytes (one cache line)");
static_assert(alignof(Material) == 4,  "Material alignment is 4");
```

**One cache line.** 4 bytes `yield_stress` reserved for the
post-v1 destruction substrate. Two parameter slots overloaded by
friction model (`v_s + α` for Stribeck → `σ_0 + σ_2` for LuGre — the
LuGre `σ_1` lives in the per-contact bristle cache, see §6); two
slots overloaded by restitution model. The `FrictionModel::FrictionTriple`
enum slot (reserved at v1a freeze; v5 impl) reinterprets
`friction_anisotropy` as `(sliding, torsional, rolling)` per the
MuJoCo §2.8 pattern — same 12 bytes, different reading. **Struct
does not grow; the enum gates slot interpretation.**

### 2. New / extended enums (additive, before v1a freeze)

```cpp
enum class FrictionModel : crd::u8
{
    Coulomb        = 0, // default; constant μ
    Stribeck       = 1, // velocity-dependent low-speed dip
    LuGre          = 2, // state-variable; per-contact bristle
    Karnopp        = 3, // dead-zone piecewise (vehicle ODE)
    Anisotropic    = 4, // Vec3f friction in material-local frame
    FrictionTriple = 5, // sliding/torsional/rolling triple (MuJoCo §2.8 pattern; v5 impl)
};

enum class RestitutionModel : crd::u8
{
    Constant     = 0, // default; e ∈ [0, 1]
    Newton       = 1, // velocity-dependent: e(v) = e_0 · exp(-α · |v|)
    HuntCrossley = 2, // compliant: F = k · δ^n · (1 + 1.5 · d · δ̇)
};

enum class CombineMode : crd::u8 // EXTENDED — adds GeometricMean slot 4
{
    Average       = 0, // (a + b) / 2
    Min           = 1, // min(a, b)
    Max           = 2, // max(a, b)
    Multiply      = 3, // a · b
    GeometricMean = 4, // sqrt(a · b)  -- new; Box2D v3 / Jolt / Unity DOTS / AGX consensus default
};
```

**`GeometricMean` is the locked default for `friction_combine`** (Box2D
v3 default, stacking-stable for rubber-on-steel-on-ice scenes). **`Max`
is the locked default for `restitution_combine`** (PhysX convention).

### 3. `MaterialId` + `MaterialPool` — content-addressed

```cpp
struct MaterialId
{
    crd::u32 raw = 0; // [generation:8 | index:24]
    [[nodiscard]] constexpr crd::u32 index() const noexcept;
    [[nodiscard]] constexpr crd::u32 generation() const noexcept;
    [[nodiscard]] constexpr bool is_null() const noexcept;
    [[nodiscard]] static constexpr MaterialId null() noexcept;
    [[nodiscard]] static constexpr MaterialId default_material() noexcept; // slot 1
    [[nodiscard]] static constexpr MaterialId make(u32 index, u32 generation);
    [[nodiscard]] constexpr bool operator==(const MaterialId&) const = default;
};
static_assert(sizeof(MaterialId) == 4);
```

Layout matches `BodyId` / `ColliderId`: `[generation:8 | index:24]`.
Scene owns `MaterialPool` (a `crd::containers::Array<Material>`).
`scene.create_material(material) → MaterialId`,
`scene.update_material(id, new)`,
`scene.material(id) → const Material&`. Slot 0 is null; slot 1 is the
shipped `default_material()`.

**Content-addressed via FNV-1a-64** over the material's serialized
parameters (öbek/cooker path). Identical material parameters produce
identical ids regardless of authoring order. The id is stable across
runs — replay-hash CI proves it. Same discipline as the `FieldId`
content-addressing in ADR-0067 §3.

### 4. Per-collider material assignment

Add to `Collider` struct (per `engine/eylem/include/crd/eylem/collider.hpp`):

```cpp
struct Collider
{
    ColliderShape    shape           = ColliderShape::Sphere;
    ColliderFlags    flags{};
    MaterialId       material        = MaterialId::default_material(); // NEW
    crd::math::Vec3f local_position{0.0F, 0.0F, 0.0F};
    crd::math::Quatf local_rotation{0.0F, 0.0F, 0.0F, 1.0F};
    union { /* shape-specific data */ };
};
```

Cost: 4 bytes per collider (shape's cache line still fits). The
`RigidBody` struct does NOT grow — bodies do not carry materials,
only colliders do. Per-collider granularity is the universal modern
choice (PhysX `PxShape::material`, Jolt `Shape::SubShapeMaterial`,
Box2D v3 `b2ShapeDef::material`); a compound character body can carry
rubber boots + leather gloves + bare-skin head with three materials,
and the contact resolver sees per-contact materials.

### 5. Combine-mode pinning rule

```
combine(a, b) := mode_priority_winner(a.combine_mode, b.combine_mode)
                 ( material[min(id_a, id_b)].param,
                   material[max(id_a, id_b)].param )
```

Mode priority: `Max > Min > Multiply > GeometricMean > Average`. The
`min` / `max` on ids guards against future asymmetric mode addition;
today the ordering has zero cost (commutative formulas).

### 6. LuGre per-contact bristle state

LuGre friction stores per-contact bristle deflection that integrates
across contact lifetime. Folded into the persistent contact warm-start
cache (the same cache ADR-0068 §8 keys by
`(body_a_id, body_b_id, feature_id)`):

```cpp
struct ContactWarmStartEntry
{
    BodyId    body_a;
    BodyId    body_b;
    crd::u32  feature_id;
    crd::f32  normal_impulse;
    crd::math::Vec2f friction_impulse;
    // LuGre payload (only allocated when material's friction_model == LuGre):
    crd::math::Vec2f bristle_z;       // bristle deflection per tangent direction
    crd::math::Vec2f bristle_dz_dt;   // bristle deflection rate
};
```

LuGre fields: 16 bytes per active LuGre contact, lazily allocated via
a per-pair tag bit. Cache key matches ADR-0068 §8.4 already.

**LuGre integration uses Tustin (implicit trapezoidal)
discretization.** A-stable, bit-exact across compilers — the
trapezoidal update reduces to `+ - *` plus one division by a per-pair
constant **precomputed in the cooker and stored in the cooked
Material; runtime never divides.** 4× sub-substep is an *accuracy*
(not stability) refinement. Explicit Euler is rejected — FP-deterministic
but step-size-dependent at LuGre's stiff regime
(`σ_0 · dt < 0.5` for stability).

### 7. Per-pair material (AGX-style) — explicitly REJECTED for v1

Storage cost is `O(N²)` in distinct-material count even sparse;
authoring cost ("declare every pair I care about") is significant;
robotics workflows that need it (granular DEM with cohesive soil)
land in v8d MPM substrate alongside their own material extension
(`SoilMaterialComponent` per the §2.15 Vortex pattern).

The v1 contract is "Material + commutative combine modes". Pairwise
override is a v9+ revisit if FEM / hydroelastic substrate (v7) needs
it; even then likely ships as a side-channel `MaterialPairOverridePool`
rather than expanding `Material` itself.

### 8. Density default + mass derivation

Default `density = 1000.0F` (water). Designer-friendly: a 1m³ box → 1000 kg.

Mass derivation contract: if `RigidBody::inv_mass == 0` (default,
"derive from density"), the scene computes
`mass = Σ (collider_volume · material.density)`. Otherwise the
authored `inv_mass` overrides. The summation runs in **ascending
`ColliderId` order** (stable across runs) — FP `+` is commutative but
not associative; pinning the order matches ADR-0063 §4's
"fixed-position write" protocol.

### 9. Determinism contract

Per ADR-0063 the substrate participates in the cross-platform
replay-hash CI. Six material-specific failure modes are blocked at
the architecture level:

1. **All friction-model evaluation must be deterministic FP.** Coulomb +
   Karnopp use only `+ - * /` and IEEE-754 `min`/`max` — bit-exact
   across compilers. Stribeck + LuGre call `exp` and require
   `crd::math::deterministic::exp`. Hunt-Crossley calls `pow` requiring
   `crd::math::deterministic::pow`. Existing `crd-no-std-math-check`
   CI guard catches violations.
2. **LuGre ODE integration uses Tustin discretization** with cooker-
   precomputed division-free constants (§6).
3. **Mass derivation summation order pinned by `ColliderId`** (§8).
4. **Combine-mode evaluation order pinned by `MaterialId`** (§5).
5. **`MaterialId` is content-addressed (FNV-1a-64)**, not sequential.
   Identical parameters → identical id regardless of authoring order
   (§3). Same discipline as `FieldId` in ADR-0067 §3.
6. **Surface-velocity application order**: contact bias =
   `surface_a + surface_b` (default Add combine) — order-independent.
   Optional Replace mode picks `min(material_id)`-wins (stable per §5).

The v9b CI matrix runs the v1j replay-hash test (10 seconds of "100
falling boxes with 4 different materials + 1 ragdoll with LuGre
gripper material + 1 character running on Stribeck-tire wheel") and
asserts snapshot hash matches across MSVC / clang / gcc × x64 / ARM
× Windows / Linux. Regression fails the build.

### 10. Cooker artifact + shipped MaterialLibrary

CRDR FourCC `'EMAT'` for individual materials, `'EMLB'` for material
libraries. `.physics-material.toml` cooker handler (lands with v1k
batch) parses:

```toml
[material]
friction_model    = "Coulomb"
friction_static   = 0.6
friction_dynamic  = 0.4
friction_combine  = "GeometricMean"
restitution_model = "Constant"
restitution       = 0.3
restitution_combine = "Max"
surface_velocity  = [0.0, 0.0, 0.0]
density           = 1000.0

[material.stribeck]                   # overlay; cooker validates per friction_model
v_s   = 0.01
alpha = 0.05
```

Cooker:
1. Validates parameter ranges (`density > 0`, `μ ≥ 0`, restitution ∈
   [0, 1] for Constant model, etc.)
2. For LuGre: precomputes Tustin-discretization constants from
   `(σ_0, σ_1, σ_2, dt_substep)` so runtime never divides
3. Computes `MaterialId` as FNV-1a-64 over canonical parameter bytes
4. Emits CRDR `'EMAT'` artifact

**8 shipped default materials** (CRDR `'EMLB'` pack with v1k):

| Material | μ_s | μ_d | e | density (kg/m³) | model | Notes |
|---|---|---|---|---|---|---|
| `Default` | 0.5 | 0.5 | 0.0 | 1000 | Coulomb | universal fallback |
| `Rubber` | 1.0 | 0.8 | 0.7 | 1100 | Coulomb | tire baseline |
| `Steel` | 0.5 | 0.4 | 0.4 | 7850 | Coulomb | structural metal |
| `Ice` | 0.05 | 0.02 | 0.1 | 920 | Anisotropic | low μ on x/y, lower along skate axis |
| `Wood` | 0.4 | 0.3 | 0.3 | 700 | Coulomb | mid-range |
| `Concrete` | 0.7 | 0.6 | 0.1 | 2400 | Coulomb | level geometry default |
| `Water` | 0.0 | 0.0 | 0.0 | 1000 | Coulomb | DAW / fluid demo |
| `Flesh` | 0.6 | 0.5 | 0.1 | 1050 | HuntCrossley | ragdoll / cinematic actor |

Designers mix-and-match per scene; öbek prefabs in v1k
(`StackingDemo`, `RagdollDemo`, `VehicleDemo`) reference the library
by name.

### 11. Slice plan (locked)

Slot into Phase 3.1 v1 as a 7-row critical-path cluster + 8 deferred
slices (deferred fill formula impls inside the already-frozen
`Material` shape — same discipline as ADR-0067's blocked sub-slices):

| Sub-slice | Scope | LOC | Tests |
|---|---|---|---|
| **v1a-material-a** | `Material` struct (64B) + enum surface (`FrictionModel`, `RestitutionModel`, extended `CombineMode`) + `MaterialId` strong type + `default_material()`. Static-asserts pin layout. | ~150 | ~5 |
| **v1a-material-b** | `MaterialPool` on scene; `create_material` / `update_material` / `material(id)` API; öbek/cooker round-trip stub. | ~200 | ~4 |
| **v1a-material-c** | Per-collider `material` field on `Collider`; `set_material` API; default-fallback validation; per-collider compound test (a body with 3 colliders, 3 materials). | ~100 | ~3 |
| **v1a-material-d** | Mass derivation: `Σ collider_volume · material.density`; ColliderId-stable summation order. | ~250 | ~5 |
| **v1e-material** | Coulomb friction integration into SI solver; friction-pyramid linearization consumes `friction_static/_dynamic`; combine-mode evaluation per §5. | ~300 | ~6 |
| **v1k-material-cooker** | `.physics-material.toml` handler + CRDR `'EMAT'` + `MaterialLibrary` (`'EMLB'`) + 8 shipped materials. | ~400 | ~5 |
| **v1k-material-bench** | `bench_materials.cpp` per §12 budget table; CI assertions. | ~100 | bench |

**Deferred sub-slices** (ship at their natural slot inside the
already-frozen v1a API surface):

| Sub-slice | Scope | When | LOC |
|---|---|---|---|
| v5-material-stribeck | Stribeck friction in solver | with vehicles v5 | ~150 |
| v5-material-lugre | LuGre integration: per-contact bristle in warm-start cache + Tustin sub-substep + cooker preprocessing | with vehicles v5 | ~400 |
| v5-material-karnopp | Karnopp friction in solver (vehicle ODE path) | with vehicles v5 | ~100 |
| v5-material-anisotropic | Anisotropic friction in SI solver: Vec3f frame-projected μ | with vehicles v5 | ~150 |
| v5-material-friction-triple | Sliding/torsional/rolling triple (MuJoCo §2.8) | with v5 | ~200 |
| v8d-material-newton | Newton restitution in SI solver | with granular DEM v8d MPM | ~100 |
| v7-material-huntcrossley | Hunt-Crossley compliant contact in FEM solver | with FEM v7 | ~250 |
| post-v1-material-fracture | Fracture parameters consumed by destruction substrate | with `GeometryCollectionComponent` | ~150 |

The v1a-material-{a,b,c,d}, v1e-material, v1k-material-cooker, and
v1k-material-bench cluster is the **v1a freeze critical path** —
must close before v1a interface freeze.

### 12. Performance budgets (CI-asserted in v1k-material-bench)

| Workload | Budget |
|---|---|
| Coulomb friction eval (per contact) | ≤ 10 ns |
| Stribeck friction eval (per contact, with `crd::math::deterministic::exp`) | ≤ 25 ns |
| LuGre friction eval (per contact, Tustin step) | ≤ 50 ns |
| Hunt-Crossley restitution eval (per contact) | ≤ 30 ns |
| Anisotropic friction projection (per contact) | ≤ 15 ns |
| Combine-mode evaluation (per pair) | ≤ 5 ns |
| Mass derivation per body (5 colliders) | ≤ 200 ns |
| MaterialPool lookup `material(id)` | ≤ 3 ns (cache hit) |

Regressions fail the build, same model the Phase 2.5 jobs benchmarks
use. Measured on Zen 4 / Raptor Lake; NEON 4-lane M-series scales to
~2× the budget.

## Rationale

### Why one cache line (64 bytes), not arbitrary growth

Solver inner loop reads `Material` once per contact pair (broadphase
+ narrow phase rejected pairs never touch it). Cache-line-aligned
struct = single memory transaction per material lookup; the alternative
(80-byte or 128-byte struct) doubles cache pressure for friction +
restitution evaluation. The v9b replay-hash CI also benefits — fixed
size means snapshot diff is byte-comparable per pool slot.

### Why `MaterialId` handle, not inline material per collider

Inline busts the 64-byte collider union budget. Raw pointer breaks
determinism + öbek serialisation. Handle is the universal modern
choice (PhysX, Jolt, Box2D v3 all converged here).

### Why `GeometricMean` as friction default

Box2D v3 / Jolt / Unity DOTS / AGX all default to geometric-mean
friction combine because it produces stacking-stable behaviour
("rubber on ice" combines to a smooth low μ rather than averaging
or maximizing). The dossier §6 has the citation cluster. Cerid
adopting the same default reduces designer surprise when porting
scenes between engines.

### Why content-addressed `MaterialId` (FNV-1a-64)

Sequential ids would permute across runs as material add/remove
order varies (which it does — async load, scene-graph order, editor
undo/redo). Content-addressing guarantees the same material set
produces the same iteration order regardless of insertion sequence —
which is the determinism contract that ADR-0063 demands.

This is the discipline `FieldId` already follows in ADR-0067 §3.

### Why LuGre with Tustin, not Euler

Stiff regime — LuGre's `σ_0` parameter (bristle stiffness) typically
sits at 10⁵ to 10⁶ for rubber-on-steel; explicit Euler requires
`σ_0 · dt < 0.5` for stability, which forces `dt < 5 × 10⁻⁶ s` —
unworkable. Tustin (implicit trapezoidal) is unconditionally stable
and reduces to `+ - *` plus one division by a per-pair constant.
Cooker precomputes the constant; runtime never divides; bit-exact
across compilers.

### Why 8 shipped materials, not 0 or 100

Zero forces every project to rebuild the canonical material catalogue
(reinventing rubber's μ for the Nth time). 100 is overwhelming +
likely wrong (every scene needs a different "metal"). Eight covers
the demo + sandbox + tutorial space without forcing scenes into a
particular vocabulary. Designers extend the library per project; the
shipped 8 are the universal references.

## Consequences

**Positive:**
- One material substrate covers all 5 mandate domains (games / robotics
  / aerospace / cinematic / scientific computing).
- Designer-authorable for the 90% case via 8 default materials + per-
  collider override + 5 combine modes.
- LuGre / Stribeck / Hunt-Crossley enable the manipulation /
  tire / soft-contact use cases without API growth.
- Determinism baked in via content-addressed ids + Tustin LuGre +
  pinned summation order.
- 64-byte cache-line struct is solver-loop friendly.
- `Material` API surface freezes at v1l alongside the rest of
  `crd-eylem`; deferred slices (v5 vehicles, v7 FEM, v8d MPM) fill
  formula impls inside the frozen surface.

**Negative:**
- Field overloading by enum (e.g., `stribeck_velocity` reinterprets as
  `σ_0` for LuGre) is more error-prone than discriminated-union
  storage. Mitigation: cooker validates per-model parameter ranges +
  emits canonical bytes; runtime trusts the cooker.
- 8 deferred sub-slices means the full friction/restitution catalogue
  isn't usable until v5 / v7 / v8d ship. Mitigation: Coulomb +
  Constant restitution (the 90% case) ships in v1a/v1e; the deferred
  models matter for specialized domains that have their own slice
  schedule anyway.
- LuGre per-contact bristle storage adds 16 bytes per active LuGre
  contact to the warm-start cache. Mitigation: lazy allocation via
  per-pair tag bit; non-LuGre cache entries unchanged.

**Neutral:**
- Substrate adds ~1500 LOC + ~25 tests + 1 bench across the v1a /
  v1e / v1k critical path. Material but proportional to the value
  (a substrate every solver consumes).
- 4 bytes `yield_stress` reservation absorbs post-v1 destruction
  parameters without struct growth.

## References

**Research:** [`docs/research/cerid-eylem-materials.md`](../research/cerid-eylem-materials.md)
— full industry survey across PhysX 5, Bullet 3, Jolt, Box2D v3,
Unity classic + DOTS, Unreal Chaos, Godot 4, MuJoCo, Drake, NVIDIA
IsaacSim/Lab, Project Chrono, AGX Dynamics, Maya nDynamics, Houdini
Vellum, Vortex Studio, Blender; algorithm references (Coulomb,
Stribeck, LuGre Canudas-de-Wit 1995, Karnopp, Hunt-Crossley, Newton);
determinism failure modes; performance benchmarks.

**Companion ADRs:**
- [ADR-0050](0050-scene-storage-backends.md) — sparse-set storage rationale (MaterialPool)
- [ADR-0058](0058-obek-system.md) — öbek prefab system (round-trips Material via 'EMAT')
- [ADR-0062](0062-eylem-physics-architecture.md) §5.5 — material reservation
- [ADR-0063](0063-eylem-determinism-contract.md) — determinism contract
- [ADR-0065](0065-hesap-numerical-substrate.md) §3 — reference-implementation posture
- [ADR-0066](0066-draw-substrate-architecture.md) — VisualizerRegistry pattern (used by eylem-viz to render per-material contact response)
- [ADR-0067](0067-eylem-force-field-architecture.md) — `FieldId` content-addressing pattern (mirrored by `MaterialId`)
- [ADR-0068](0068-eylem-body-types-collision-filtering-callbacks.md) §8 — contact warm-start cache (LuGre bristle state piggybacks)
- [ADR-0075](0075-eylem-testing-rigor.md) — testing rigor (conservation tests consume Material)

**Algorithms (selected):**
- [Canudas-de-Wit et al. 1995 — LuGre friction model](https://hal.science/hal-00394988/document)
- [Drake compliant contact (Hunt-Crossley)](https://drake.mit.edu/doxygen_cxx/group__compliant__contact.html)
- [PhysX 5 PxMaterial](https://nvidia-omniverse.github.io/PhysX/physx/5.4.0/_api_build/class_px_material.html)
- [Box2D v3 b2ShapeDef](https://box2d.org/documentation/group__shape.html)
- [Jolt PhysicsMaterial](https://jrouwe.github.io/JoltPhysics/class_physics_material.html)
- [MuJoCo contact friction model](https://mujoco.readthedocs.io/en/stable/computation/index.html#friction)
