# Session log — 2026-05-15 — units v0c adoption pass B

> Phase 3.1.7.5 `crd-units` adoption B. v0c-1 (RigidBody + PhysicsConfig dimensional) → v0c-2 (integrator typed-math) → v0c-3 (ForceFieldComponent geometric params) → v0c-close. v0c-4 (geometry-primitives surface re-tag) deferred to post-DimRoot per scope discovery.

## Scope landed

| Sub-slice | Surface | Tests |
|---|---|---|
| v0c-1 | `RigidBody` + `PhysicsConfig` + `IPhysicsScene` dimensional surface | existing eylem-tests migrated |
| v0c-2 | Integrator + interpolation typed-math end-to-end | included in eylem-tests |
| v0c-3 | `ForceFieldComponent` geometric params typed | +6 assertions in `test_v1a_interface.cpp` |
| v0c-4 | DEFERRED to post-DimRoot | — |
| v0c-close | ADR-0078 §3 amendment (D20–D25) + session log + full sweep + docs | — |

## v0c-1 — `RigidBody` + `PhysicsConfig` dimensional

### Types changed

`engine/eylem/include/crd/eylem/rigid_body.hpp`:

```cpp
struct RigidBody {
    Vec3<Length32>                    position{};
    Quatf                              rotation{0.0F, 0.0F, 0.0F, 1.0F};
    Vec3<Velocity32>                  linear_velocity{};
    Vec3<AngularVelocity32>           angular_velocity{};
    InverseMass32                     inv_mass{0.0F};         // = Quantity<DimInv<Mass>, f32>
    Vec3<InverseMomentOfInertia32>    inv_inertia{};
    f32                                linear_damping  = 0.05F;
    f32                                angular_damping = 0.05F;
    RigidBodyFlags                     flags{};
};
static_assert(sizeof(RigidBody) == 80, ...);  // API freeze pin preserved
```

`engine/eylem/include/crd/eylem/physics_config.hpp`:

```cpp
struct PhysicsConfig {
    Vec3<Acceleration32>   gravity{...};
    Duration32             fixed_dt{1.0F / 60.0F};
    ...
    Length32               contact_offset{0.02F};
    Length32               contact_breaking_threshold{0.02F};
    Velocity32             sleep_linear_threshold{0.01F};
    AngularVelocity32      sleep_angular_threshold{0.01F};
    Duration32             sleep_time_threshold{0.5F};
};
```

`engine/eylem/include/crd/eylem/physics_scene.hpp`:

```cpp
class IPhysicsScene {
    virtual void set_gravity(Vec3<Acceleration32>) noexcept = 0;
    virtual Vec3<Acceleration32> gravity() const noexcept   = 0;
    virtual void apply_force(BodyId, Vec3<Force32>)         = 0;
    virtual void apply_torque(BodyId, Vec3<Torque32>)       = 0;
    virtual void apply_impulse(BodyId, Vec3<Momentum32>, Vec3<Length32>) = 0;
    virtual void step(Duration32 dt) = 0;
    virtual std::optional<RaycastHit> raycast(Vec3<Length32>, Vec3f, Length32) const = 0;
};

struct RaycastHit {
    BodyId      body;
    ColliderId  collider;
    Vec3<Length32>  point{};
    Vec3f           normal{0.0F, 1.0F, 0.0F};  // dimensionless unit vector
    Length32        distance{0.0F};
};
```

### New aliases

`engine/units/include/crd/units/dim_aliases.hpp` + `quantity_aliases.hpp`:

```cpp
using InverseMass             = DimInv<Mass>;
using InverseMomentOfInertia  = DimInv<MomentOfInertia>;

template <typename T = f32> using InverseMass            = Quantity<dim::InverseMass, T>;
template <typename T = f32> using InverseMomentOfInertia = Quantity<dim::InverseMomentOfInertia, T>;
// Concrete: InverseMass32 / InverseMomentOfInertia32 / Momentum32 / MomentOfInertia32 / AngularAccel32 / Duration32
```

### Cross-Dim `Vec * Quantity` overloads

`engine/math/include/crd/math/vec.hpp` gains six overloads. The integrator
pattern `v += a * dt` and `p += v * dt` now type-checks end-to-end without
`.value` escapes:

```cpp
template <typename D1, typename D2, typename T>
constexpr Vec3<Quantity<DimMul<D1, D2>, T>>
operator*(const Vec3<Quantity<D1, T>>& v, Quantity<D2, T> q) noexcept;
```

Same pattern for Vec2/Vec4 and the symmetric `Q * Vec` direction + the `/` form.
Per the advisor's D21 commentary: same-Dim `Vec<T> * S` remains the primary
overload; cross-Dim only fires when the same-result constraint fails.

### `BodyPool` AoSoA storage stays raw

The `BodyChunk` SIMD columns (`pos_x`/`pos_y`/`pos_z`/...) remain raw
`Vec8f` / `Vec4f`. Per D22 (SIMD-boundary pin), the type tag lives at the
AoS read/write boundary in `read()` / `store_lane()` / `read_prev()`,
constructing typed Quantity at the boundary:

```cpp
body.position.x = Length32{tile.pos_x.lane(lane)};
...
put_lane(tile.pos_x, lane, body.position.x.value);
```

### Ripple in tests

~30 test sites bridged: `body.position = {x, y, z}` →
`body.position = from_raw_vec<dim::Length>(Vec3f{x, y, z})`; equality
comparisons added `.value`. Quat / scalar flag fields stay raw.

## v0c-2 — Integrator typed-math

`engine/eylem-rigid3d/src/eylem_system.cpp`:

```cpp
const Duration32                    dt      = m_config.fixed_dt;
const Vec3<Acceleration32>          gravity = m_config.gravity;
...
body.linear_velocity += gravity * dt;            // Acceleration * Time -> Velocity
const f32 lin_damp_factor = 1.0F - body.linear_damping * dt.value;
body.linear_velocity = body.linear_velocity * lin_damp_factor;
body.position += body.linear_velocity * dt;      // Velocity * Time -> Length
```

The quaternion derivative `q̇ = 0.5·ω·q` bridges raw at the
quat-construction site (Quat is unit-norm, dimensionless;
`body.angular_velocity.{x,y,z}.value` escapes into the dimensionless
quaternion arithmetic).

`engine/eylem-rigid3d/src/interpolation_system.cpp`:
`crd::math::lerp` is `MathScalar`-only by D2 — bridge typed `Vec3<Length32>`
through `to_raw_vec` → `lerp` → re-tag for the World setter. Documented
in the call site.

## v0c-3 — `ForceFieldComponent` geometric params typed

`engine/eylem/include/crd/eylem/force_field.hpp`:

```cpp
struct ForceFieldComponent {
    Vec3f                   direction{};       // unit vector, dimensionless
    Vec3f                   axis{};            // unit vector, dimensionless
    Vec3<Length32>          origin{};          // local-space position
    f32                     magnitude;         // formula-polymorphic
    Length32                radius_min{0.01F}; // singularity guard
    Length32                radius_max{1.0F};  // cutoff
    f32                     falloff_p;         // dimensionless exponent
    f32                     polarity;          // ±1
    Vec4f                   poly_coeffs;       // dimensionless cubic
    Length32                noise_scale{1.0F}; // wavelength in metres
    f32                     noise_time;        // seconds (raw scalar at noise() call site)
    u32                     noise_octaves;
    // ... enum / void* handle / DAG fields unchanged
};
```

Typing rule from ADR-0078 §3 D21: GEOMETRIC params (position / distance /
length) carry SI `Length32`; DIRECTIONS are unit vectors (raw `f32`);
FORMULA COEFFICIENTS (`magnitude` / `polarity` / `falloff_p` /
`poly_coeffs` / `noise_*`) stay raw because their dimension depends on
`formula` + `mass_coupling` — set at use time by `EylemFieldSystem`.

`EylemFieldSystem` stays as the v1f stub (zero-force) per ADR-0067 §3 —
typing the parameters now means when the real impl ships, every
geometric coordinate already carries the SI tag.

## v0c-4 — DEFERRED

`crd-geometry-primitives` is templated on `MathScalar T` (raw `f32`).
Re-templating to `MathValue` requires fractional-exponent dimensions
(`DimRoot<Area> -> Length`) for `length()` reductions on `Vec<Length>`,
which is explicitly deferred per ADR-0078 §2 D2.

**Scope-discovery moment captured 2026-05-15** during v0c-4 prep — the
template parameter contracts with internal scalar reductions made
naive re-typing dead-end at the first `length(Vec3<Length>)` call site.
ADR-0078 §3 D24 codifies the deferral; v0c-4 reappears as a follow-on
slice once `DimRoot<>` lands.

In the interim, primitives carry the SI interpretation by
**documentation contract** (D24): `Sphere<f32>::center` is a position
in metres, `Sphere<f32>::radius` is a length in metres. Callers bridge
at the boundary with `from_raw_vec<dim::Length>` when handing the
primitive to a typed surface (e.g. `eylem::RaycastHit::distance`).

## v0c-close — 5-config DoD + full sweep

Per-slice (v0c-1 + v0c-2 + v0c-3):
- win-debug: 1882/1882
- win-asan: 1882/1882
- win-shipping: 1795/1795
- win-shipping-profile: 1877/1877
- win-tidy: build-clean (no new warnings — pre-existing tidy debt unchanged)

Full sweep at v0c-close: 11-config Windows sweep planned next.

## Decisions locked (ADR-0078 §3 amendment)

- **D20** — `RigidBody` + `PhysicsConfig` + `IPhysicsScene` dimensional surface; 80-byte freeze pin preserved.
- **D21** — `Vec<Q1> * Q2 -> Vec<DimMul<Q1, Q2>>` cross-Dim overloads added.
- **D22** — SIMD-boundary pin: `Vec4f`/`Vec8f` batch kernels stay raw; typed surface one layer up.
- **D23** — `signed_distance.hpp` is a `MathScalar` reduction per D2 — stays raw.
- **D24** — `crd-geometry-primitives` API re-tag DEFERRED until `DimRoot<>` ships.
- **D25** — 5-config per-slice DoD carried forward.

## Open follow-ups

- **v0d adoption C** (next): `crd-renderer` uniform-upload boundary; `crd-resources` cookers; ImGui inspector; Layer-6 full format/parse/`UnitPreferences`; `DimRoot<>` design for fractional-exponent reductions.
- **v0c-4 (deferred)**: `crd-geometry-primitives` API re-tag once `DimRoot<>` lands.
- **`crd-eylem-aero` / RPM-Hz cross-paths**: when implementing rotational frequency conversion (`omega = 2π · frequency`), expect compile rejection since `Frequency = DimInv<Time>` vs `AngularVelocity = DimDiv<Angle, Time>` are distinct per Cerid's tagged-Angle (D17 / 8th base). Resolution: `Angle{2π} * frequency` is the canonical bridge.

## References

- ADR-0078 §3 amendment (D20–D25).
- `docs/systems/units.md` — status table updated.
- `docs/phases/phase-3.1.7.5-units.md` — phase plan (next: v0d).
- `feedback_per_slice_run_ctest.md` — 5-config DoD protocol.
- `docs/sessions/2026-05-15-units-v0b-adoption-a.md` — preceding sub-slices.
