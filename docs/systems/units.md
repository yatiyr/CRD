# crd-units — Phase 3.1.7.5 v0a

> Compile-time-dimensional units substrate. Leaf module (deps: `crd-core` only).
> Provides `Quantity<D, T>` zero-overhead wrapper + `Dim<L, M, T, I, Th, N, J, A>`
> 8-exponent compile-time dimension tag + 6-layer conversion system.

## Status

- v0a-1 ✅ shipped — `Dim` + `Quantity` core + layout pins.
- v0a-2 ✅ shipped — Layer 1 `LinearUnit` + ~120 named units + Layer 4 `UnitMul`/`UnitDiv` compound auto-derive + `value_in<TargetUnit>` boundary accessor.
- v0a-3 ✅ shipped — Layer 2 `AffineUnit` + `Temperature`/`TemperatureDelta` + Layer 3 `NonLinearUnit` + dB family + cents/semitones + 80+ UDLs + `crd-no-untagged-physical-numeric` CI guard.
- v0a-close ✅ — units.md + session log + ADR-0078 (mint).
- **v0b-1 ✅ 2026-05-15** — `Vec<Quantity>` enablement: `MathValue` concept widens `Vec2`/`Vec3`/`Vec4` to accept `Quantity<D, T>` for element-wise ops; reductions stay on `MathScalar`; precision-suffix aliases (`Length32`/`Length64`/`Mass32`/...); `to_raw_vec`/`from_raw_vec` boundary helpers; `from_trs(Vec3<Quantity>...)` overload. 13 cases / 40 assertions in `tests/units/test_vec_quantity.cpp`.
- **v0b-2 ✅ 2026-05-15** — `crd-config` unit-tagged TOML accessors (`get_length`, `get_mass`, `get_time`, `get_angle`, `get_velocity`, `get_force`, `get_pressure`, `get_energy`, `get_power`, `get_voltage`, `get_current`, `get_frequency`, `get_temperature`); suffix parsing maps authoring strings to SI at the boundary; missing-key returns fallback; 13 cases / 57 assertions in `tests/config/test_unit_accessor.cpp`.
- **v0b-3 ✅ 2026-05-15** — `scene::Transform::translation` retyped to `Vec3<Length<f32>>`; ~30 call sites across `World::set_local`/`set_translation`, eylem integrator sync, glTF scene cooker, eylem-viz visualizers, and tests bridged with `from_raw_vec` / `.value`.
- **v0b-4 ✅ 2026-05-15** — glTF cooker `[cook] position_scale` `.meta` key; multiplies positions at cook time so the runtime always sees SI meters regardless of source authoring units; SI sanity-warn at `> 1e6 m`; 12 cases / 15 assertions in `tests/cooker/test_mesh_cook_options.cpp`.
- **v0b-close ✅ 2026-05-15** — this doc + session log + ADR-0078 §2 amendment (D15–D19) + 5-config full sweep + roadmap/context/MEMORY sync.
- **v0c-1 ✅ 2026-05-15** — `crd-eylem RigidBody` dimensional (`position: Vec3<Length32>`, `linear_velocity: Vec3<Velocity32>`, `angular_velocity: Vec3<AngularVelocity32>`, `inv_mass: InverseMass32 = Quantity<DimInv<Mass>, f32>`, `inv_inertia: Vec3<InverseMomentOfInertia32>`). `PhysicsConfig` typed end-to-end (`gravity: Vec3<Acceleration32>`, `fixed_dt: Duration32`, sleep thresholds, contact offsets). `IPhysicsScene` public surface typed (`set_gravity`, `apply_force`, `apply_torque`, `apply_impulse`, `step`, `raycast`, `RaycastHit`). 80-byte `RigidBody` freeze pin preserved. New `InverseMass`/`InverseMomentOfInertia` aliases; new `Momentum32`/`MomentOfInertia32`/`Duration32` concrete aliases.
- **v0c-2 ✅ 2026-05-15** — Integrator typed-math: `v += a * dt` and `p += v * dt` now Quantity-aware end-to-end via new cross-Dim `Vec<Q1> * Q2 -> Vec<DimMul<Q1, Q2>>` overloads (6 in vec.hpp). Damping factor (dimensionless rate) bridges raw at compute site only. `crd::math::lerp` stays `MathScalar`-only per D2; interpolation bridges through `to_raw_vec`.
- **v0c-3 ✅ 2026-05-15** — `ForceFieldComponent` geometric params typed: `origin: Vec3<Length32>`, `radius_min`/`radius_max`/`noise_scale: Length32`. Formula coefficients (`magnitude`/`polarity`/`falloff_p`/`poly_coeffs`/`noise_time`/`noise_octaves`) stay raw — formula-polymorphic per `FieldFormula` + `FieldMassCoupling`.
- **v0c-4 → v0d-2** — geometry-primitives re-tag was DEFERRED in v0c on the (later-overstated) framing that `length(Vec<Length>) → Length` needs `DimRoot<>`. v0d-1 demonstrated the DimMul-then-retag pattern works without DimRoot, unblocking v0c-4 inside v0d-2.
- **v0c-close ✅ 2026-05-15** — this doc + session log + ADR-0078 §3 amendment (D20–D25) + 5-config sweep + roadmap/context/MEMORY sync.
- **v0d-1 ✅ 2026-05-15** — `Vec<Quantity>` reductions widened (`length`/`length_squared`/`dot`/`cross`/`distance`/`distance_squared`/`hadamard`/`normalized`) via DimMul-then-retag pattern. No `DimRoot<>` needed — return Dim known a priori from input. 9 cases / 23 assertions.
- **v0d-2 ✅ 2026-05-15** — `crd-geometry-primitives` API re-tag: struct widening (95 sites in primitives.hpp) + `queries_typed.hpp` boundary-wrapper layer (8 `closest_point` + 5 `distance` + 4 `distance_squared` typed overloads). Algorithms stay `<MathScalar T>`; typed surface lives one layer above with strip-compute-retag at the boundary. Foundation widening in scalar.hpp for Quantity (abs/min/max/clamp/is_finite/is_nan/default_epsilon + `sqrt_as<D,T>` helper). 8 cases / 23 assertions.
- **v0d-3 ✅ 2026-05-15** — `crd-renderer` raw-Mat4f boundary contract pinned in `FrameContext`. Renderer never imports `crd/units/*`. SIMD upload bit-identical.
- **v0d-4 ✅ 2026-05-15** — `crd-resources` byte-buffer contract pinned in `mesh_resource.hpp`. Typed boundary lives at cooker (`.meta [cook] position_scale`) + ECS (`scene::Transform::translation`).
- **v0d-5 ✅ 2026-05-15** — Layer-6 `UnitPreferences` + `format_*` / `parse_*` for 13 Dims + 11 discipline-preset factories (game / CAD / robotics-REP-103 / aerospace / PCB / audio / 3D-print / CAM / cinematic / imperial / SI-strict / scientific). Affine temperature (K↔C↔F↔Ra). 14 cases / 42 assertions.
- **v0d-6 ✅ 2026-05-15** — `crd-imgui` `unit_preferences_inspector.hpp` header-only — discipline-preset picker + per-Dim combos + precision/suffix/sci-notation toggles.
- **v0d-close ✅ 2026-05-15 — Phase 3.1.7.5 CLOSED** — ADR-0078 §4 amendment (D26–D31) + session log `docs/sessions/2026-05-15-units-v0d-phase-close.md` + 11-config Windows full sweep PASS + ROADMAP/context/MEMORY sync.

🎉 **Phase 3.1.7.5 `crd-units` CLOSED 2026-05-15.** 19 sub-slices / ~4.5 KLOC engine / ~2.3 KLOC tests / 31 locked design decisions in ADR-0078 (§1-§4). Full project ctest 1546 → **1913 win-debug** across the phase. Every physical / scientific quantity at every API boundary carries a compile-time dimension tag. **Mars Climate Orbiter at the engine surface is now a compile error.**

**v0d adoption pass C** is next: `crd-renderer` uniform-upload boundary; `crd-resources` cookers; ImGui inspector; Layer-6 full format/parse/`UnitPreferences` with 11-discipline-preset table; `DimRoot<>` design for fractional-exponent reductions; v0c-4 follow-on after DimRoot lands; 17-config sweep close.

## API at a glance

```cpp
#include <crd/units/units.hpp>

using namespace crd::units;
using namespace crd::units::literals;

// Construction via UDLs
auto length     = 25.4_mm;       // Quantity<dim::Length, f64> = 0.0254 m
auto mass       = 5.0_kg;        // Quantity<dim::Mass, f64> = 5.0 kg
auto velocity   = 60.0_mph;      // Quantity<dim::Velocity, f64> = 26.8224 m/s
auto force      = 100.0_N;       // Quantity<dim::Force, f64>
auto pressure   = 1.0_atm;       // = 101325 Pa
auto energy     = 1.0_kWh;       // = 3.6e6 J
auto frequency  = 60.0_Hz;
auto voltage    = 3.3_V;
auto temp       = 25.0_celsius;  // Temperature<f64> = 298.15 K (AbsoluteQuantity)
auto db         = 80.0_dB_spl;   // Quantity<dim::Pressure, f64> = 0.2 Pa

// Cross-dimension arithmetic (Newton's law type-checks)
Quantity<dim::Mass, f32>         m{5.0f};
Quantity<dim::Acceleration, f32> a{9.81f};
auto F = m * a;  // Quantity<dim::Force, f32> = Force; compile-error if you swap.

// Integration step (units throughout)
Quantity<dim::Velocity, f32>     v{10.0f};
Quantity<dim::Acceleration, f32> grav{-9.81f};
Quantity<dim::Time, f32>         dt{0.016f};
v += grav * dt;   // Velocity += Acceleration*Time -> Velocity. Check.

// Boundary egress
f64 in_mph    = value_in<MilePerHour>(velocity);          // = 60.0
f64 in_kPa    = value_in<Kilopascal>(pressure);            // = 101.325
f64 in_deg    = value_in_temperature<Fahrenheit>(temp);    // = 77.0
f64 sum_db    = value_in_nonlinear<DecibelSPL>(db + db);   // = 86.02 dB

// Temperature arithmetic (absolute vs delta)
Temperature<f64>      a_temp = 100.0_celsius;   // 373.15 K
Temperature<f64>      b_temp = 25.0_celsius;    // 298.15 K
TemperatureDelta<f64> diff   = a_temp - b_temp; // 75 K delta (subtraction strips offset)
Temperature<f64>      sum    = b_temp + diff;   // 373.15 K (abs + delta = abs)
// Temperature<f64> bad = a_temp + b_temp;  // COMPILE ERROR (no abs + abs)
```

## Architecture overview

`Quantity<D, T>` is a thin compile-time wrapper around a scalar T (typically
`f32` or `f64`) carrying a dimension tag D. The dimension is a compile-time
type — zero runtime cost.

### Six-layer conversion system

1. **Layer 1 — `LinearUnit<Dim, FactorRatio>`.** Most units. SI prefix +
   standardised imperial conversions are exact `std::ratio` arithmetic at
   compile time. ~120 named units shipped in v0a-2.

2. **Layer 2 — `AffineUnit<Dim, ScaleRatio, OffsetRatio>`.** Temperature only
   in v0a-3 (`Celsius`, `Fahrenheit`, `Rankine`). Distinct `Temperature` vs
   `TemperatureDelta` types close the absolute-vs-delta trap at the type level.

3. **Layer 3 — `NonLinearUnit` with explicit `to_si`/`from_si` functions.**
   dB family + cents + semitones in v0a-3. Non-linear arithmetic doesn't
   exist at the type level: convert to linear SI, add, convert back.

4. **Layer 4 — `UnitMul`/`UnitDiv`/`UnitPow` compound auto-derivation.** Adding
   one base unit unlocks N compound units automatically via `std::ratio_multiply`/
   `std::ratio_divide` at compile time. The extensibility multiplier.

5. **Layer 5 — Federated domain registration.** Future domain modules
   (`crd-eylem-aero`, `crd-eda`, `crd-cam`, …) declare their own units in
   their own namespace with their own UDLs. ADL handles lookup. No central
   registry. **Not yet exercised at v0a close** — first consumer is `crd-eylem-aero`
   (Phase 3.1 v6).

6. **Layer 6 — Format / parse / `UnitPreferences`.** Skeleton at v0a; full
   format/parse + 11-discipline-preset table lands in v0d adoption pass C.

### Dimension exponents

`Dim<L, M, T, I, Th, N, J, A>` carries 8 signed integer exponents:

| Letter | Base dimension |
|---|---|
| L  | Length (meters) |
| M  | Mass (kilograms) |
| T  | Time (seconds) |
| I  | Electric current (amperes) |
| Th | Thermodynamic temperature (kelvin) |
| N  | Amount of substance (moles) |
| J  | Luminous intensity (candelas) |
| A  | Angle (radians — TAGGED in Cerid even though strict SI says dimensionless) |

The 8th tagged-dimension Angle is the Cerid pragmatic choice: strict SI has
radians as dimensionless (m/m), but tagging it as a distinct base prevents
silent `Length + Angle` arithmetic bugs at compile time. mp-units (P1935)
takes the same pragmatic approach.

## Two-layer typed architecture (ADR-0078 §5 — locked 2026-05-16)

Cerid runs a **two-layer typed system**. The dimensional check happens
at the API surface, then disappears for the inner loop. Same machine
code as raw `f32`; full Mars-Climate-Orbiter compile-time safety where
it matters.

```
UPPER LAYER (TYPED) — Quantity<D, T> everywhere
  • Public APIs, ECS components, configs, cookers, UI display
  • eylem::RigidBody, scene::Transform, IPhysicsScene,
    geometry::Sphere<Length32>, get_length(), UnitPreferences

                    ── API SURFACE — boundary ──
                    Bridges: .value, to_raw_vec,
                    from_raw_vec, strip-compute-retag wrappers.
                    Each crossing one line; one-line comment
                    naming the ADR clause.

LOWER LAYER (RAW) — raw f32/f64 inside
  • SIMD kernels (Vec4f/Vec8f, AVX2 intrinsics, bvh4_simd)
  • Math primitives (Vec/Mat/Quat inner ops, dot/cross/length)
  • Geometry algorithm bodies (closest_point.hpp / intersect.hpp / ...)
  • Numerical kernels (future BLAS / LAPACK / SVD / GMRES / FFT)
  • GPU command-buffer writes, file/wire byte buffers
```

### Why two layers, not one

1. **Compile-time safety where it pays.** Composing physics formulas,
   integrating cross-module data, authoring scenes — those are where
   unit-mix bugs happen. The upper layer catches them.
2. **Zero overhead where speed pays.** SIMD intrinsics cannot carry a
   compile-time tag through a lane shuffle. Numerical kernels want raw
   `f64`. The lower layer stays bit-identical to a no-units build.
3. **Algorithms stay portable.** A future numerical kernel or a new
   SIMD path doesn't pay a typing tax to integrate. The raw shape is
   the lingua franca of the lower layer.
4. **Domain modules grow independently.** Each domain (eylem, hesap,
   geometry, future cad/cfd/control) builds its own typed upper layer
   over the shared raw substrate. No central registry to update.

### Boundary patterns (idiomatic forms)

**1. Strip-compute-retag wrapper** — for query APIs that have a typed
public surface and a raw algorithm body:

```cpp
template <typename D, typename T>
Vec3<Quantity<D, T>> closest_point(const Sphere<Quantity<D, T>>& s,
                                    const Vec3<Quantity<D, T>>& p) noexcept
{
    return from_raw_vec<D>(closest_point(strip(s), to_raw_vec(p)));
}
```

**2. `.value` egress at the hot-path boundary** — SIMD column store,
push-constant write, file write:

```cpp
// raw egress — SIMD column store per ADR-0078 §3 D22
put_lane(tile.pos_x, lane, body.position.x.value);

// GPU push constant — raw Mat4f boundary per ADR-0078 §4 D28
cmd.push_constants(layout, &draw_item.model, sizeof(Mat4f));
```

**3. Tagging at file / cooker load** — re-introduce the dim tag when
data crosses upward:

```cpp
// glTF spec mandates SI metres; tag at the ECS lift boundary
const Vec3f raw_pos = read_vertex_bytes(buf, offset);
transform.translation = from_raw_vec<dim::Length>(raw_pos);
```

### Where the layers are TODAY (post-Phase 3.1.7.5 close)

| Module                      | Layer | Notes |
|---|---|---|
| `crd-units`                 | upper | the substrate |
| `crd-config` get_*           | upper | TOML → typed Quantity at the boundary |
| `crd-scene::Transform`       | upper | `translation = Vec3<Length32>` |
| `crd-eylem::RigidBody`       | upper | position / velocity / inv_mass all typed; 80-byte freeze pin preserved |
| `crd-eylem::IPhysicsScene`   | upper | apply_force / apply_torque / step(Duration32) typed |
| `crd-geometry-primitives` structs | upper data | Sphere<T> / Box<T> / Capsule3<T> widened to MathValue T |
| `crd-geometry-primitives` algorithms | **lower** | closest_point.hpp / intersect.hpp / signed_distance.hpp stay `<MathScalar T>` |
| `queries_typed.hpp`          | upper wrapper | strip-compute-retag boundary for closest_point / distance / distance_squared |
| `crd-math` Vec/Mat ops       | **lower** | inner ops stay MathScalar; Vec<Quantity> overloads bridge at the API surface |
| `crd-math` Vec reductions    | upper bridge | length(Vec<Q>) / dot(Vec<Q1>, Vec<Q2>) / cross — return typed result via DimMul re-tag |
| `crd-math::simd::Soa` columns | **lower** | `Vec8f` AoSoA columns — raw, never typed |
| `crd-renderer` Renderable / DrawItem | **lower** | raw Mat4f boundary; never imports crd/units/* |
| `crd-rhi-vulkan` push constants | **lower** | byte writes, raw |
| `crd-resources::MeshResource` | **lower** | byte buffers; SI interpretation by cooker contract |
| `tools/asset_cooker` glTF .meta | upper boundary | `[cook] position_scale` converts to SI at cook time |
| `crd-imgui` UnitPreferences inspector | upper | typed display layer for the UI |
| **Future `crd-hesap-dense`** | **lower** | numerical kernels operate on raw f64 matrices; callers wrap typed in/out |

### Code-review checklist

When reviewing a new slice, ask:

1. **Is this surface a public API?** (Anything that crosses module
   boundaries, gets cooked, gets serialised, gets exposed to ECS, or
   appears in a UI.) → typed.
2. **Is this code an inner kernel?** (SIMD intrinsic, BLAS routine,
   raycast/Möller-Trumbore body, signed-distance evaluator, GPU
   command-buffer write, byte serialiser.) → raw.
3. **If you see a `.value` or `to_raw_vec` in the middle of business
   logic** (not at an obvious boundary), it's a smell. Push it down to
   the boundary or up to the type.
4. **If you see a bare `f32 my_length;` in a struct field**, it's a
   build failure waiting to happen — the `crd-no-untagged-physical-numeric`
   CI guard catches it.



1. **SI is the only canonical internal unit.** Every `Quantity::value` is in
   the SI base for its dimension. No exceptions.

2. **Precision tier (f32 vs f64) is orthogonal to dimension.** Same dimensional
   type system; scalar precision per-domain. f32 for games / runtime; f64
   for aerospace large-world / CAD micrometer / scientific.

3. **Boundary discipline.** Asset / file / UI / network layers carry unit
   tags and convert at the boundary; runtime never sees non-SI.

4. **Zero overhead.** `Quantity<D, T>` is bit-equal to T for SIMD/GPU upload.
   `static_assert` layout pins.

5. **No untagged-physical numeric crosses a module boundary.**
   `crd-no-untagged-physical-numeric` CI guard enforces (shipped v0a-3).
   Dimensionless quantities (restitution, friction, indices, RGBA components)
   are allowed bare; physical quantities (length, mass, time, force, …) are
   typed.

6. **`crd-math` stays raw.** SIMD kernels operate on raw f32/f64. The dimensional
   layer is *around* `crd-math`, not inside it.

7. **GPU / RHI stays raw.** Vulkan command-buffer / uniform-buffer / SSBO
   writes consume raw scalars. Conversion happens once at the upload-call
   site (`.value` accessor); shaders see bare floats.

8. **Angle is a distinct base dimension**, not strict-SI dimensionless. Cerid
   tags it as the 8th exponent so `Angle + Length` is a compile error.

9. **Determinism preserved.** ADR-0063 contract intact across `crd-units`
   adoption.

10. **`.value` is publicly accessible.** No getter overhead. Type safety lives
    at the API surface; inside SIMD/GPU hot paths, consumers reach `.value`
    raw.

11. **Frame transforms are NOT unit conversions.** Coordinate-system / reference-
    frame transforms (ENU/NED/ECEF, body-vs-inertial, world-vs-local) are
    geometric transforms; the dimension stays `Length`, only the basis changes.
    They live in `crd-math::Transform` + `crd-geometry::transform_aabb`, NOT
    in `crd-units`.

## File layout

```
engine/units/
  CMakeLists.txt
  include/crd/units/
    dim.hpp                 Dim<8 exponents> + DimMul/DimDiv/DimInv/DimPow
    dim_aliases.hpp         Named base + derived dimensions (Length / Mass / Velocity / ...)
    quantity.hpp            Quantity<D, T> zero-overhead wrapper + arithmetic
    units_si.hpp            Layer 1 LinearUnit + ~120 named units
    units_compound.hpp      Layer 4 UnitMul/UnitDiv/UnitPow + ~30 compound units
    units_affine.hpp        Layer 2 AffineUnit + Temperature/TemperatureDelta
    units_nonlinear.hpp     Layer 3 NonLinearUnit + dB family + cents/semitones
    literals.hpp            80+ UDLs
    value_in.hpp            Boundary accessors (value_in<U>, quantity_from<U>)
    units.hpp               Umbrella include
  src/units.cpp             Translation-unit anchor (header-only otherwise)

tests/units/
  test_dim.cpp              Dim<> arithmetic
  test_quantity.cpp         Quantity layout + arithmetic
  test_linear_units.cpp     Layer 1 factor correctness + round-trips
  test_compound_units.cpp   Layer 4 auto-derive
  test_value_in.cpp         Boundary egress
  test_affine_units.cpp     Layer 2 + Temperature/TemperatureDelta
  test_nonlinear_units.cpp  Layer 3 dB family + cents/semitones
  test_literals.cpp         UDL conversions

scripts/check_no_untagged_physical_numeric.{ps1,sh}   CI guard
```

## Test corpus

| File | Cases | Assertions |
|---|---|---|
| test_dim.cpp | 16 | ~75 |
| test_quantity.cpp | 20 | ~70 |
| test_linear_units.cpp | 13 | ~80 |
| test_compound_units.cpp | 17 | ~50 |
| test_value_in.cpp | 24 | ~50 |
| test_affine_units.cpp | 16 | ~30 |
| test_nonlinear_units.cpp | 17 | ~25 |
| test_literals.cpp | 15 | ~80 |
| **Total** | **138** | **464** |

All test cases pass on **win-debug + win-asan + win-shipping + win-tidy** at
v0a-close.

## Adding a new unit (federated extension pattern)

Domain modules add their own units in their own namespace without touching
`crd-units` core:

```cpp
// engine/eylem-aero/include/crd/eylem_aero/units.hpp
namespace crd::eylem_aero::units
{
using namespace crd::units;  // brings in dim::, LinearUnit, UnitDiv, etc.

using AstronomicalUnit = LinearUnit<dim::Length, std::ratio<14'959'787'07, 1>>; // 1.49597870700e11 m
using LightYear        = LinearUnit<dim::Length, std::ratio<9'460'730'472'580'800, 1>>;  // EXACT
using StandardG        = LinearUnit<dim::Acceleration, std::ratio<980'665, 100'000>>;

inline namespace literals
{
    [[nodiscard]] constexpr Quantity<dim::Length, crd::f64>
    operator"" _au(long double v) noexcept
    { return quantity_from<AstronomicalUnit>(static_cast<crd::f64>(v)); }
}
} // namespace crd::eylem_aero::units
```

That's it — `crd-units` itself doesn't change. ADL picks up the literal at
use sites. Compound `UnitDiv<AstronomicalUnit, JulianYear>` auto-derives a
velocity unit with the right factor.

## Open follow-ups

Tracked as v0c/d adoption-pass work + future amendments:

- **v0c adoption B** (next) — `crd-eylem` `RigidBody` dimensional; integrator typed-math; force-field substrate; `crd-geometry-primitives` API surface re-tag.
- **v0d adoption C** — `crd-renderer` uniform-upload boundary; `crd-resources` cookers; ImGui inspector; Layer-6 full format/parse/`UnitPreferences` with 11 discipline presets; cross-engine format readers (STEP, IGES, FBX, IFC, Gerber).
- **`Mat<Quantity>` wrappers** — `Vec<Quantity>` shipped in v0b-1; matrix templates remain raw because rotation/projection matrices mix dimensional rows (e.g. perspective matrix has a Length / Length entry). Defer until a consumer surfaces a homogeneous-dimension matrix use case.
- **Fractional-exponent dimensions for `sqrt(Area)` -> `Length`** — required to widen `length()`/`distance()` reductions to `Vec<Quantity>`. Needs `DimRoot<>` machinery or a separate fixed-norm type. Tracked for v0c onward; reductions currently stay on strict `MathScalar` (ADR-0078 D15).
- **"Kind" tag for same-Dim distinct quantities** — Energy vs Torque (both kg·m²/s²); LuminousFlux vs LuminousI (both candelas in strict SI minus the dimensionless steradian). Add when a consumer disambiguates.
- **`PressureDelta` (gauge pressure)** — same absolute-vs-delta pattern as Temperature. Add when CFD / weather / aerospace consumer needs it.
- **Bytes / binary prefixes** (`_KiB` / `_MiB` / `_GiB`) — dimensionless `dim::Data` category. Add when `crd-memory` budgets / `crd-resources` file sizes need them.
- **Tighten `crd-no-untagged-physical-numeric` guard** as adoption proceeds. Current v0a-3 regex is conservative (struct-field-only); v0c/d will tighten to include function parameters once those modules are adopted.

## v0b authoring contracts (boundary stencils)

### TOML config — `crd-config`

```toml
# runtime/configs/example.toml
[player]
height       = "1.85_m"   # parsed via get_length(cfg, "player.height", fallback);
weight       = "82_kg"    # parsed via get_mass
top_speed    = "65_mph"   # parsed via get_velocity (SI = 29.0576 m/s)
hot_drink    = "85_celsius"  # parsed via get_temperature -> Kelvin internally

[physics]
gravity      = 9.81       # bare numeric = already-SI (m/s² here)
```

### glTF cook options — `.meta`

```toml
# blender_export.glb.meta — file is in Blender cm export, not SI.
[id]
uuid = "11111111-2222-3333-4444-555555555555"
[cook]
position_scale = 0.01     # cm -> m. Default = 1.0F if omitted.
```

### `scene::Transform` boundary

```cpp
crd::scene::Transform tr{};
// Constructing from raw camera data: bridge at the boundary.
tr.translation = crd::math::from_raw_vec<crd::units::dim::Length>(
                     crd::math::Vec3f{1.0F, 2.0F, 3.0F});
// Reading a typed component back at the SIMD/GPU upload boundary:
crd::math::Vec3f raw = crd::math::to_raw_vec(tr.translation);
// Or per-axis:
crd::f32 height = tr.translation.y.value;   // .value escapes to raw f32
```

## References

- `docs/phases/phase-3.1.7.5-units.md` — full phase plan + 6-layer conversion system.
- `docs/ROADMAP.md` § Strategic Execution Plan — sequencing context.
- `docs/PRINCIPLES.md` — "Every physical and scientific quantity carries a unit" pin.
- `feedback_always_units.md` (memory) — the rule + the why + how-to-apply.
- ADR-0078 candidate — minted at v0a-close (this slice).
