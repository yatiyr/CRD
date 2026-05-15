# crd-units — Phase 3.1.7.5 v0a

> Compile-time-dimensional units substrate. Leaf module (deps: `crd-core` only).
> Provides `Quantity<D, T>` zero-overhead wrapper + `Dim<L, M, T, I, Th, N, J, A>`
> 8-exponent compile-time dimension tag + 6-layer conversion system.

## Status

- v0a-1 ✅ shipped — `Dim` + `Quantity` core + layout pins.
- v0a-2 ✅ shipped — Layer 1 `LinearUnit` + ~120 named units + Layer 4 `UnitMul`/`UnitDiv` compound auto-derive + `value_in<TargetUnit>` boundary accessor.
- v0a-3 ✅ shipped — Layer 2 `AffineUnit` + `Temperature`/`TemperatureDelta` + Layer 3 `NonLinearUnit` + dB family + cents/semitones + 80+ UDLs + `crd-no-untagged-physical-numeric` CI guard.
- v0a-close — this doc + session log + ADR-0078 (mint).

**v0b adoption pass A** is the next slice (per `docs/phases/phase-3.1.7.5-units.md` § Slice list):
`crd-config` unit-tagged TOML readers + `crd-scene Transform` dimensional + glTF cooker SI normalisation.

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

## Architectural pins

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

Tracked as v0b/c/d adoption-pass work + future amendments:

- **v0b adoption A** — `crd-config` unit-tagged TOML readers; `crd-scene Transform` dimensional; glTF cooker SI normalisation. Lights up the CI guard on `crd-scene`'s struct fields.
- **v0c adoption B** — `crd-eylem` `RigidBody` dimensional; integrator typed-math; force-field substrate; `crd-geometry-primitives` API surface re-tag.
- **v0d adoption C** — `crd-renderer` uniform-upload boundary; `crd-resources` cookers; ImGui inspector; Layer-6 full format/parse/`UnitPreferences` with 11 discipline presets; cross-engine format readers (glTF `KHR_unit`, STEP, IGES, FBX, IFC, Gerber).
- **`Vec<Quantity>` / `Mat<Quantity>` wrappers** — deferred to v0b (lives where `crd-math` types are accessible).
- **"Kind" tag for same-Dim distinct quantities** — Energy vs Torque (both kg·m²/s²); LuminousFlux vs LuminousI (both candelas in strict SI minus the dimensionless steradian). Add when a consumer disambiguates.
- **`PressureDelta` (gauge pressure)** — same absolute-vs-delta pattern as Temperature. Add when CFD / weather / aerospace consumer needs it.
- **Bytes / binary prefixes** (`_KiB` / `_MiB` / `_GiB`) — dimensionless `dim::Data` category. Add when `crd-memory` budgets / `crd-resources` file sizes need them.
- **Tighten `crd-no-untagged-physical-numeric` guard** as adoption proceeds. Current v0a-3 regex is conservative (struct-field-only); v0b/c/d will tighten to include function parameters once those modules are adopted.

## References

- `docs/phases/phase-3.1.7.5-units.md` — full phase plan + 6-layer conversion system.
- `docs/ROADMAP.md` § Strategic Execution Plan — sequencing context.
- `docs/PRINCIPLES.md` — "Every physical and scientific quantity carries a unit" pin.
- `feedback_always_units.md` (memory) — the rule + the why + how-to-apply.
- ADR-0078 candidate — minted at v0a-close (this slice).
