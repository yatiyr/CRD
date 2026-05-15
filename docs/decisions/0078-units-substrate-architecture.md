# ADR-0078 — `crd-units` substrate architecture

**Status:** Accepted 2026-05-15 (v0a-close).

**Context.** Phase 3.1.7.5 `crd-units` ships as a peer-substrate module
(deps: `crd-core` only). Per the Strategic Execution Plan locked 2026-05-15
+ memory `feedback_always_units.md` + memory `feedback_strategic_execution_plan_2026_05_15.md`,
every physical and scientific quantity in Cerid carries a compile-time
dimensional type via `crd-units`. There is no opt-out path.

This ADR captures the locked design decisions from Sprint 1 (v0a-1 / v0a-2 /
v0a-3 sub-slices, all green on win-debug + win-asan + win-shipping + win-tidy
per the per-slice-check protocol).

## Decisions

### D1. 8-exponent dimension tag with Angle as tagged 8th base

`Dim<L, M, T, I, Th, N, J, A>` carries 8 signed-integer exponents on the 7 SI
base dimensions + Angle. Strict SI has radians as dimensionless (m/m), but
Cerid tags Angle as a distinct 8th base so `Length + Angle` is a compile
error. This is the mp-units (P1935) pragmatic choice; we follow it.

**Future amendment trigger:** if Cerid grows a 9th dimension (e.g. data
storage, radioactivity), extend the template parameter list. Existing code
recompiles unchanged (added exponent defaults to 0).

### D2. `Quantity<D, T>` zero-overhead wrapper, layout pins

Single `.value` member of type `T` (typically `f32` or `f64`). `Quantity`
is `is_standard_layout_v` + `is_trivially_copyable_v`. `sizeof(Quantity<D, T>) == sizeof(T)`.
Compile-time `static_assert` enforces.

`.value` is publicly accessible — no getter encapsulation overhead. SIMD/GPU
upload paths reach the raw scalar via `.value`; the type safety lives at
the API surface, not inside the inner loop.

### D3. Six-layer conversion system

1. **Layer 1 — `LinearUnit<Dim, std::ratio>`.** Most units. SI prefix +
   standardised imperial conversions are exact rational arithmetic at
   compile time.
2. **Layer 2 — `AffineUnit<Dim, ScaleRatio, OffsetRatio>`.** Temperature
   (Celsius, Fahrenheit, Rankine) in v0a. Reserved for future Pressure /
   Voltage / Datetime if a consumer needs the absolute-vs-delta distinction.
3. **Layer 3 — `NonLinearUnit` with explicit `to_si` / `from_si` functions.**
   dB family (SPL / V / W / dBm) + cents + semitones in v0a. Stellar
   magnitude / pH / Richter reserved for when a consumer surfaces.
4. **Layer 4 — `UnitMul` / `UnitDiv` / `UnitPow` compound auto-derivation.**
   Adding one base unit unlocks N compound units automatically via
   `std::ratio_multiply` / `std::ratio_divide` at compile time.
5. **Layer 5 — Federated domain registration.** Future domain modules
   (`crd-eylem-aero` / `crd-eda` / `crd-cam` / future `crd-material`) declare
   their own units in their own `units` sub-namespace with their own UDLs.
   ADL handles lookup. No central registry, no plugin system.
6. **Layer 6 — Format / parse / `UnitPreferences`.** Skeleton at v0a; full
   format/parse + 11-discipline-preset table lands in v0d adoption pass C.

### D4. Absolute vs Delta types

`Temperature<T>` = `AbsoluteQuantity<dim::Temperature, T>` — a Quantity with
an origin (absolute zero at 0 K). Cannot be added to another Temperature.
Cannot be scaled by a scalar. Cannot be negated.

`TemperatureDelta<T>` = `Quantity<dim::Temperature, T>` — a regular
relative-delta. Supports full arithmetic.

Operator rules:
- `Temperature - Temperature -> TemperatureDelta`
- `Temperature + TemperatureDelta -> Temperature`
- `Temperature - TemperatureDelta -> Temperature`
- `TemperatureDelta +/- TemperatureDelta -> TemperatureDelta`
- `Temperature + Temperature` — **compile error**

The same pattern is reserved (not v0a) for `Pressure` / `PressureDelta`,
`Datetime` / `Duration`, possibly `Voltage` / `VoltageDelta`.

### D5. Non-linear units are an I/O concern only

dB / cents / semitones do not support arithmetic at the type level. The
underlying linear `Quantity` does (in SI). Convert dB → linear, add, convert
linear → dB at the boundary.

There is no distinct "DbValue" type; dB lives as a unit-name-tag on the
linear `Quantity` only at conversion sites (`quantity_from_nonlinear<U>(v)`
and `value_in_nonlinear<U>(q)`).

This makes the arithmetic semantics correct (`80 dB + 80 dB == 86 dB SPL`
for incoherent summation, not `160 dB`) and keeps the type system clean.

### D6. `std::ratio` for Layer 1 factors

Conversion factors are `std::ratio<num, den>` at compile time. SI prefix
conversions (`m ↔ mm ↔ km`) and standardised imperial conversions (1 in
= 0.0254 m EXACT per 1959 international agreement, 1 mile = 1609344 mm
EXACT, 1 lb-mass = 0.45359237 kg EXACT) are bit-exact rational round-trips
at the type level.

Irrational factors (Degree = π/180, Grad = π/200, Revolution = 2π,
PoundForce ≈ 4.4482216152605 N) use the best big-integer rational
approximation to f64 precision. Round-trip at the rational layer is exact;
f64 evaluation rounds at the last step (documented 1 ULP tolerance).

### D7. Ambiguous-literal disallowance

`_lb` and `_oz` UDLs are deliberately NOT defined. `1.0_lb` produces a
compile error. Users explicitly pick:
- `_lb_mass` (pound-mass, kg)
- `_lbf` (pound-force, N)
- `_oz_mass` (ounce-mass, kg)
- `_oz_troy` (ounce-troy, kg)
- `_oz_fluid_us` / `_oz_fluid_imp` (fluid ounce, m³) — when shipped

This eliminates cross-cultural mass-vs-force / mass-vs-volume bugs at the
literal site.

### D8. `crd-no-untagged-physical-numeric` CI guard

Registered as a ctest test in `tests/math/CMakeLists.txt` (alongside
`crd-no-non-ascii-test-names`, `crd-simd-emission-check`, `crd-no-std-math-check`,
`crd-no-std-sort-check`). Flags struct/class field declarations of bare
`f32` / `f64` / `float` / `double` whose name contains a physical-quantity
substring (`length`, `mass`, `force`, `velocity`, `acceleration`, `pressure`,
`energy`, `power`, `temperature`, `voltage`, `current`, `frequency`,
`resistance`, `capacitance`, `inductance`).

Initial regex is conservative (only matches lines ending with `;` and with
no `(` / `)` — excludes function-parameter list members). Will tighten
through v0b/c/d adoption as modules opt into typed quantities.

Suppression marker: `crd-lint-allow-untagged-physical` on the same line.

### D9. `crd-math` stays raw

SIMD kernels (Vec / Mat ops, the SIMD substrate from ADR-0033) operate on
raw `f32` / `f64`. The dimensional layer is *around* `crd-math`, not inside
it. `Vec<Quantity>` / `Mat<Quantity>` wrappers (deferred from v0a) live in
the unit-side adoption layer (v0b/c) where `crd-math` types are visible.

### D10. GPU / RHI stays raw

`vkCmdPushConstants` / uniform-buffer / SSBO writes consume raw `f32`/`f64`.
Conversion happens once at the upload-call site (`.value` accessor); shaders
see bare floats.

### D11. Federated domain extensibility

Domain modules add their own units in their own namespace without touching
`crd-units` core. No central registry. ADL handles lookup. Adding a new
base unit is one `LinearUnit<>` declaration + optional UDL.

The compound `UnitMul` / `UnitDiv` framework means adding one base unit
unlocks N new compound units automatically — the extensibility multiplier.

### D12. Precision tier is orthogonal to dimension

`Length<f32>` for games / runtime; `Length<f64>` for aerospace large-world
/ CAD micrometer / scientific. Same dimensional type system; scalar precision
varies per consumer.

Explicit cross-precision conversion only (`Length<f64>{l32.value_in<Meter>()}`).
No implicit cross-precision arithmetic.

### D13. Determinism preserved

ADR-0063 deterministic-by-construction contract intact across `crd-units`
adoption. `Quantity` arithmetic uses the same f32 / f64 operations as bare-
scalar; bit-exact reproducibility across compilers / SIMD widths preserved.

### D14. Frame transforms are NOT unit conversions

Coordinate-system / reference-frame transforms (ENU / NED / ECEF in geodetic
/ robotics; body vs inertial frame in physics; world / local / view / clip
in rendering) are geometric transforms — dimension stays `Length`, only the
basis changes. They live in `crd-math::Transform` + `crd-geometry::transform_aabb`,
**not** in `crd-units`.

## Consequences

### Positive

1. **Mars Climate Orbiter class of bugs becomes a compile error.** Every
   physical quantity carries its unit at the type level.
2. **Cross-domain integration is unit-safe.** Games + robotics + CAD + PCB
   + aerospace + CFD + FEA + audio all use the same `Quantity<D, T>` type
   system internally. No per-domain unit-system divergence.
3. **Zero runtime cost.** `Quantity` arithmetic produces identical codegen
   to bare-scalar.
4. **Bit-exact SI-prefix + standardised-imperial round-trips.** `std::ratio`
   arithmetic at compile time.
5. **Extensibility multiplier.** Adding one new base unit unlocks N compound
   units. Adding a new domain pack = one header file in the domain module's
   namespace, no `crd-units` changes.

### Negative / cost

1. **Adoption cost across `crd-config` / `crd-scene` / `crd-eylem` /
   `crd-renderer` / `crd-resources` / `crd-imgui`** — paid in v0b/c/d (~3
   weeks calendar after v0a close).
2. **Slight template-syntax noise in math-heavy hot paths** — mitigated by
   `.value` accessor at the boundary.
3. **`std::ratio` overflow risk** for big factors — pre-reduce by gcd
   manually (e.g., ElectronVolt). Documented in v0a-3 session log.
4. **Affine arithmetic has 1-3 ULP error in f64** — tests use `near_eq`
   tolerance for Fahrenheit conversions, bit-exact for Celsius (scale=1
   degenerates the multiply).
5. **Non-linear arithmetic semantics surprise users initially** (`80 dB +
   80 dB ≠ 160 dB`). Documented + the type system makes it impossible to
   write `dB + dB` directly (convert to linear, add, convert back).

## Alternatives considered + rejected

- **Boost.Units.** Battle-tested for 18 years but C++03-era + heavy
  compile-time cost. Wraps every operation. We picked a leaner Cerid-owned
  implementation matching mp-units' (P1935) shape so a future mp-units
  migration is mechanical.
- **mp-units directly.** Targets C++26 + still in flux + heavyweight. A
  focused ~1.5 KLOC Cerid implementation is sufficient for our 8-domain
  scope.
- **A stated convention + code review enforcement.** Rejected: conventions
  drift. The type system is the only durable enforcement.
- **Strict SI dimensionless Angle.** Rejected: silently lets `Angle + Length`
  compile. Tagging as 8th base costs nothing and catches a real bug class.
- **`Vec3<Quantity>` in `crd-units`.** Rejected: would force `crd-units →
  crd-math` dependency. Deferred to v0b adoption layer where `crd-math`
  types are visible.

## §2 Amendment — v0b adoption pass A (Accepted 2026-05-15)

Adoption sub-slices v0b-1 / v0b-2 / v0b-3 / v0b-4 shipped 2026-05-15
all green across the 5-config DoD (win-debug + win-asan + win-shipping +
win-shipping-profile + win-tidy). Decisions locked beyond the v0a core:

### D15. `Vec<Quantity>` allowed; reductions remain on `MathScalar`

`Vec2`/`Vec3`/`Vec4` templates widen from `MathScalar` (`f32`/`f64`) to
a new `MathValue` concept (`MathScalar || IsQuantity`). Element-wise
ops (`==`, `+`, `-`, scalar `*`, scalar `/`) now type-check for
`Vec3<Length<f32>>` etc. Member defaults use `T{}` instead of
`static_cast<T>(0)` so they survive Quantity's explicit-ctor.

Reductions (`dot`, `cross`, `length`, `length_squared`, `distance`,
`distance_squared`, `hadamard`, `try_normalize`, `normalize`) stay
gated on strict `MathScalar`. Fractional-exponent dimensions (e.g.
`sqrt(Area)` -> `Length`) are out of scope for v0b; introducing them
requires either `DimRoot<>` machinery or a separate fixed-norm type.
Tracked as a future amendment trigger.

Boundary helpers `to_raw_vec<D, T>(Vec3<Quantity<D, T>>) -> Vec3<T>`
and `from_raw_vec<D>(Vec3<T>) -> Vec3<Quantity<dim::D, T>>` cross the
SIMD / GPU / Mat4 boundary. `from_trs` gains an overload accepting a
`Vec3<Quantity<D, T>>` translation that strips via `to_raw_vec` so
`scene::Transform` callers compose without rewrite.

### D16. `crd-config` ships unit-tagged TOML accessors

`engine/config/include/crd/config/unit_accessor.hpp` exposes 13 typed
accessors: `get_length`, `get_mass`, `get_time`, `get_angle`,
`get_velocity`, `get_force`, `get_pressure`, `get_energy`,
`get_power`, `get_voltage`, `get_current`, `get_frequency`,
`get_temperature`. Suffix tables map authoring strings (`_m`, `_mm`,
`_in`, `_ft`, `_kg`, `_lb_mass`, `_Pa`, `_kPa`, `_psi`, `_kelvin`,
`_celsius`, `_fahrenheit`, ...) to SI at the boundary; raw numeric
keys (no suffix) are interpreted as already-SI.

Missing-key returns `fallback` (not 0). The implementation explicitly
instantiates each accessor for `f32` and `f64` so `Length32` /
`Length64` consumers get the same API surface.

### D17. `scene::Transform::translation` is `Vec3<Length<f32>>`

The 8-layer ECS storage now carries the dimensional tag on
translation. `rotation` (`Quatf`) and `scale` (`Vec3f`) stay raw — they
are dimensionless under D14 (frame-transform geometry, not unit
conversion). `Transform::local()` calls the new `from_trs` overload;
`world` stays `Mat4f` for GPU upload compatibility (D10).

Adoption ripple: `World::set_local` / `World::set_translation`,
`eylem` integrator-to-transform sync, glTF scene cooker, eylem-viz
visualizers, ~30 test sites all converted to use `from_raw_vec` at
the boundary and `.value` for component access in assertions. No
runtime overhead — `sizeof(Length32) == sizeof(f32)` per D2.

### D18. glTF cooker SI normalization — `.meta [cook] position_scale`

glTF 2.0 §3.5.2.1 mandates POSITION accessors carry SI meters; no
`KHR_unit` extension exists. Real-world exporters violate the spec
(Blender "Apply Transform" toggle, SolidWorks cm exports, etc.).

The cooker now reads an optional `[cook] position_scale = 0.01` key
from the asset's `.meta` and multiplies every position attribute at
cook time. Default `1.0F` preserves bit-exact pass-through for
conformant assets. The cooker logs an SI-sanity warning when any
final position magnitude exceeds `1e6 m` (1000 km — well above any
reasonable engine asset), pointing the user at the `[cook]
position_scale` knob.

Parser is pure string processing (`parse_mesh_cook_options(StringView)`),
testable without filesystem access; 12 cases / 15 assertions in
`tests/cooker/test_mesh_cook_options.cpp`. Non-positive / non-finite
values fall back to `1.0F` with a warning.

### D19. Per-slice DoD = 5 configs (carried forward from D-003)

v0b inherits the 5-config DoD codified in
[[feedback_per_slice_run_ctest]] + ADR-0079 D7: every v0b sub-slice
verified across win-debug + win-asan + win-shipping +
win-shipping-profile + win-tidy. v0b-1/-2/-3/-4 all closed under
this protocol; the new typed surfaces ship under LTCG +
profiling-on as well as debug + ASan.

## References

- `docs/phases/phase-3.1.7.5-units.md` — full phase plan + 6-layer conversion system spec.
- `docs/systems/units.md` — system overview shipped at v0a close (this slice).
- `docs/sessions/2026-05-15-units-v0a-substrate.md` — session log.
- `docs/ROADMAP.md` § Strategic Execution Plan — sequencing context.
- `docs/PRINCIPLES.md` — "every physical and scientific quantity carries a unit" pin.
- `feedback_always_units.md` + `feedback_strategic_execution_plan_2026_05_15.md` (memory) — pins.
- mp-units (Mateusz Pusz, P1935R8) — the C++26-targeted dimensional units proposal we track for future inter-op.
- Boost.Units (Matthias Schabel, 2007) — the canonical hand-rolled approach.
- NASA Mars Climate Orbiter loss report (1999) — the canonical case study for unit-conversion bugs.
- ROS REP 103 — Standard Units of Measure and Coordinate Conventions; the robotics SI-everywhere policy.
- IEC 80000-1 — Quantities and units, general principles.
