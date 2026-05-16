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

## §3 Amendment — v0c adoption pass B (Accepted 2026-05-15)

Adoption sub-slices v0c-1 / v0c-2 / v0c-3 shipped 2026-05-15 all green
across the 5-config DoD (win-debug + win-asan + win-shipping +
win-shipping-profile + win-tidy). v0c-4 (`crd-geometry-primitives`
API re-tag) is DEFERRED to a successor slice once fractional-exponent
dimensions (`DimRoot<>`) ship — see D24.

### D20. `crd-eylem RigidBody` is dimensional end-to-end

`RigidBody` fields gain compile-time dimension tags via `Vec<Quantity>`
+ `Quantity<DimInv<Mass>>`:

```cpp
struct RigidBody {
    Vec3<Length32>                    position;
    Quatf                              rotation;             // unit, dimensionless
    Vec3<Velocity32>                  linear_velocity;
    Vec3<AngularVelocity32>           angular_velocity;
    InverseMass32                     inv_mass;             // = Quantity<DimInv<Mass>, f32>
    Vec3<InverseMomentOfInertia32>    inv_inertia;
    f32                                linear_damping;       // dimensionless rate
    f32                                angular_damping;
    RigidBodyFlags                     flags;
};
static_assert(sizeof(RigidBody) == 80, ...);
```

**Layout is preserved bit-for-bit** because precision tier is
orthogonal to dimension (D12). The 80-byte API freeze pin (ADR-0062
§15) holds. The integrator now type-checks:
`Force * InverseMass -> Acceleration`, `Acceleration * Time -> Velocity`,
`Velocity * Time -> Length` close at compile time.

`PhysicsConfig` typed in the same pass: `gravity = Vec3<Acceleration32>`,
`fixed_dt = Duration32`, `sleep_linear_threshold = Velocity32`,
`sleep_angular_threshold = AngularVelocity32`,
`sleep_time_threshold = Duration32`,
`contact_offset/breaking_threshold = Length32`.

`IPhysicsScene` typed at the public surface:
`set_gravity(Vec3<Acceleration32>)`,
`apply_force(BodyId, Vec3<Force32>)`,
`apply_torque(BodyId, Vec3<Torque32>)`,
`apply_impulse(BodyId, Vec3<Momentum32>, Vec3<Length32>)`,
`step(Duration32)`,
`raycast(Vec3<Length32> origin, Vec3f direction, Length32 max_distance)`,
`RaycastHit { Vec3<Length32> point; Vec3f normal; Length32 distance; }`.

### D21. Cross-Dim `Vec<Q1> * Q2 -> Vec<DimMul<Q1, Q2>>` overloads

`engine/math/include/crd/math/vec.hpp` gains six overloads
(Vec2/Vec3/Vec4 × {mul, div}) for cross-Dim element-wise scalar product.
Previously `Vec3<Velocity> * Time` failed the same-result-type constraint;
the new overload returns `Vec3<Length>`. Integrator math now composes
end-to-end without `.value` escapes in the hot path.

Same-Dim `Vec<T> * S -> Vec<T>` (raw scalar multiply, dimensionless
rate) remains the primary overload; cross-Dim only fires when the
constraint `t * s -> T` would fail — orthogonal overload resolution.

### D22. SIMD-boundary pin — `Vec4f` / `Vec8f` batch kernels stay raw

`bvh4_simd.cpp`, watertight ray-tri, Plücker SIMD, Ize robust slab
test, every AVX2 lane-shuffle kernel — these consume raw `f32` /
`Vec4f` / `Vec8f`. The Dim tag cannot ride through an `_mm256_*`
intrinsic; pretending otherwise dead-ends in the first SIMD load.

Callers bridge at the function boundary with `to_raw_vec` /
`from_raw_vec` / column transpose. The SIMD kernel signature stays
dimensionless; the typed surface lives one layer up.

### D23. `signed_distance.hpp` is a `MathScalar` reduction

The 20 iq analytic SDFs (`sd_sphere`, `sd_box`, `sd_capsule`, ...)
take raw `f32` and return raw `f32`. Same status as `dot` / `cross` /
`length` per D2 — mathematical primitives, dimensionless contract.
Re-tagging propagates through every SDF caller (renderer DFAO/DFGI,
mesh-baking, ray marching) for no semantic gain. They stay raw.

### D24. `crd-geometry-primitives` API re-tag DEFERRED to post-DimRoot

`Sphere<T>` / `Box<T>` / `Capsule3<T>` / etc. are templated on
`MathScalar T`. Re-templating to `MathValue` requires:

- `length(Vec3<Length>) -> Length` (square-root reduction).
- `dot(Vec3<Length>, Vec3<Length>) -> Area` (multiply reduction).
- `cross(Vec3<Length>, Vec3<Length>) -> Vec3<Area>`.

These need fractional-exponent dimensions (`DimRoot<Area> -> Length`)
or a separate fixed-norm type. Out of scope for v0c per D2.

**Defer plan:** v0c-4 lands once `DimRoot<>` ships (v0d adoption C or
a follow-on slice). Until then, geometry-primitives carry the SI
interpretation by *documentation contract* — `Sphere<f32>::center` is
a position in metres, `Sphere<f32>::radius` is a length in metres.
Callers bridge at the boundary with `from_raw_vec<dim::Length>` /
`to_raw_vec` when they hand the primitive to a typed surface
(`scene::Transform`, `eylem::RigidBody`).

### D25. Per-slice DoD = 5 configs (carried from v0b)

v0c sub-slices inherit the 5-config protocol from
[[feedback_per_slice_run_ctest]] + §2 D19 + ADR-0079 D7:
win-debug + win-asan + win-shipping + win-shipping-profile + win-tidy.
v0c-1 + v0c-2 + v0c-3 all closed under this protocol; final
full-sweep at v0c-close per `feedback_full_sweep_required.md`.

## §4 Amendment — v0d adoption pass C + Phase 3.1.7.5 CLOSE (Accepted 2026-05-15)

Adoption sub-slices v0d-1 / v0d-2 / v0d-3 / v0d-4 / v0d-5 / v0d-6 shipped
2026-05-15 all green across the 5-config DoD. Phase 3.1.7.5 `crd-units`
closes after this slice. Six decisions locked.

### D26. `Vec<Quantity>` reductions widened (no DimRoot needed)

`crd::math::length` / `length_squared` / `dot` / `cross` / `distance` /
`distance_squared` / `hadamard` / `normalized` add Quantity overloads:

| Operation              | Input              | Return                              |
|---|---|---|
| `length(Vec<Q>)`        | Vec3<Q>             | Q (via `sqrt(dot(v,v).value)` re-tag) |
| `length_squared(Vec<Q>)` | Vec3<Q>             | Quantity<DimMul<D,D>, T> (Q²)        |
| `dot(Vec<Q1>, Vec<Q2>)` | Vec3<Q1>, Vec3<Q2>  | Quantity<DimMul<D1,D2>, T>           |
| `cross(Vec<Q>, Vec<Q>)` | Vec3<Q>, Vec3<Q>    | Vec3<Quantity<DimMul<D,D>, T>>       |
| `distance(Vec<Q>, Vec<Q>)` | Vec3<Q>, Vec3<Q> | Q                                    |
| `normalized(Vec<Q>)`    | Vec3<Q>             | Vec3<T> (unit, dimensionless)        |

The earlier ADR-0078 §3 D24 framing — "geometry-primitives re-tag needs
`DimRoot<>`" — was overstated. `length(Vec<Q>) → Q` works through
`DimMul<D,D>` (Q²) then re-tagging `sqrt(Q².value)` back as Q. Each typed
reduction has the result Dim known a priori from the input Dim; no
fractional-exponent machinery required. `DimRoot<>` is deferred to a
future slice only if an unknown-Dim sqrt actually appears (none in v0a/b/c/d).

`try_normalize(Vec<Q>&)` is intentionally NOT widened — the result type
(Vec<f32>) differs from the input (Vec<Q>), so the in-place mutation
signature doesn't compose. Callers use `normalized(...)` by value.

### D27. `crd-geometry-primitives` API re-tag — struct widening + typed-overload wrappers

Pattern: keep the algorithm bodies templated on `<MathScalar T>` (raw
arithmetic — `static_cast<T>(0)`, `std::numeric_limits<T>::min()`,
`std::sqrt(T)`, all the dimensionless internals that would dead-end for
Quantity). Widen ONLY the leaf struct templates from `<MathScalar T>`
to `<MathValue T>` so `Sphere<Length32>` / `Box<Length32>` / etc. exist
as typed data. Add a separate header `queries_typed.hpp` with
Quantity-overload wrappers (`closest_point`, `distance`, `distance_squared`)
that strip-compute-retag at the call boundary.

Files touched:
- `engine/geometry-primitives/include/crd/geometry/primitives/primitives.hpp` (95 sites widened).
- `engine/geometry-primitives/include/crd/geometry/primitives/{closest_point,intersect,barycentric,formulary,plucker,watertight_ray_tri,robust_ray_aabb,is_finite,constants}.hpp` reverted to MathScalar (algorithms stay raw).
- `engine/geometry-primitives/include/crd/geometry/primitives/queries_typed.hpp` (NEW — strip helpers + 8 closest_point + 5 distance + 4 distance_squared typed-overload wrappers).
- `engine/math/include/crd/math/scalar.hpp` — Quantity overloads for `abs`/`min`/`max`/`clamp`/`is_finite`/`is_nan`/`default_epsilon<Q>` + new `sqrt_as<D, T>(Quantity<DimMul<D,D>, T>) → Quantity<D, T>` helper.
- `engine/geometry-primitives/include/crd/geometry/primitives/primitives.hpp` — `Plane::d` default `static_cast<T>(0)` → `T{}` (works for both raw and Quantity).

Cost-benefit pin: the alternative (widen every algorithm template +
sqrt/numeric_limits/cast patterns through closest_point.hpp + intersect.hpp
+ ...) is the 2-day scope the advisor flagged. The strip-compute-retag
boundary pattern delivers the same typed-query API surface with zero
algorithm-body rewrites and zero runtime overhead (`to_raw_vec` /
`from_raw_vec` are `constexpr`, `.value` accessors compile away).

### D28. `crd-renderer` is the raw-Mat4f boundary

Renderer consumes `Mat4f` / `Vec3f` exclusively. Dimensional types live
one layer above in `crd-scene` (`Transform::translation = Vec3<Length32>`,
v0b-3) — bridging happens in `TransformPropagation` via
`from_trs(to_raw_vec(translation), ...)`. The renderer NEVER imports
`crd/units/*` and NEVER includes `crd-scene::Transform`. Documented as
a contract pin in `engine/renderer/include/crd/renderer/renderer.hpp:FrameContext`.

This keeps the SIMD upload path, push-constant writes, and the existing
LTCG path bit-identical to the pre-units build. Adoption Mars-Climate-
Orbiter safety lives upstream where typed values are constructed and
combined; by the time data reaches `cmd.push_constants`, it is raw
`Mat4f` with semantic interpretation pinned by ADR documentation.

### D29. Resources layer is byte-based — typed contract lives at cooker + ECS boundaries

`crd-resources` types (`MeshResource`, `TextureResource`, `MaterialResource`,
`SceneResource`) hold raw byte buffers. The dimensional contract lives:

1. At the COOKER — glTF cooker reads `.meta [cook] position_scale` per
   ADR-0078 §2 D18 and multiplies vertex positions to SI metres at cook
   time. The bytes stored on disk are always SI.
2. At the ECS LIFT — when a `Renderable` parents under `scene::Transform`,
   the typed `translation = Vec3<Length32>` carries the dim tag at the
   ECS layer (ADR-0078 §3 D17, v0b-3).

Re-tagging on every vertex read would defeat the SIMD upload path (D22).
Documented as a contract pin in `mesh_resource.hpp`.

### D30. Layer-6 `UnitPreferences` + format/parse + 11-discipline preset table

`engine/units/include/crd/units/unit_preferences.hpp` ships the runtime-
selectable display-unit-per-Dim layer:

```cpp
UnitPreferences prefs = make_cad_prefs();
Length32 height = Length32{1.85F};
auto s = format_length(height, prefs);              // "1850_mm"
auto q = parse_length<f32>("39.37_in", prefs);      // optional<Length32{1.0}>
```

13 typed `UnitChoice` enums (length / mass / time / angle / velocity /
force / pressure / energy / power / voltage / current / frequency /
temperature). 11 discipline-preset factories shipped:

| Preset | Length | Mass | Angle | Temp | Velocity | Frequency | Pressure |
|---|---|---|---|---|---|---|---|
| Game        | m     | kg | deg  | C  | m/s   | Hz  | Pa  |
| CAD         | mm    | kg | deg  | K  | m/s   | Hz  | MPa |
| Robotics    | m     | kg | rad  | K  | m/s   | Hz  | Pa  | (ROS REP 103)
| Aerospace   | m     | kg | deg  | K  | knots | Hz  | kPa |
| PCB / EDA   | mil   | g  | rad  | K  | m/s   | MHz | Pa  |
| Audio       | m     | kg | rad  | K  | m/s   | Hz  | Pa  |
| 3D Print    | mm    | g  | rad  | C  | m/s   | Hz  | Pa  |
| CAM         | in    | lb | rad  | K  | m/s   | RPM | Pa  |
| Cinematic   | cm    | kg | deg  | K  | m/s   | Hz  | Pa  |
| Imperial    | in    | lb | deg  | F  | mph   | Hz  | psi |
| SI Strict   | m     | kg | rad  | K  | m/s   | Hz  | Pa  |
| Scientific  | m     | kg | rad  | K  | m/s   | Hz  | Pa  | (9-digit + sci notation)

Format uses `std::snprintf("%.*g", prec_digits, ...)` for predictable
behaviour. Parse is the inverse of v0b-2's `crd-config` TOML
unit-suffix accessors — same suffix tables; returns
`std::optional<Q>` on success / `nullopt` on malformed input. Temperature
is affine (Kelvin canonical SI; Celsius / Fahrenheit / Rankine convert
through scale + offset).

`crd-imgui` ships an optional inspector header
`crd/imgui/unit_preferences_inspector.hpp` (header-only; pulls `<imgui.h>`)
with a discipline-preset picker + per-Dim combo boxes + precision /
suffix / scientific-notation toggles. Caller drives persistence to
`crd-config`.

### D31. Phase 3.1.7.5 `crd-units` CLOSES after v0d

The v0a substrate + v0b adoption A + v0c adoption B + v0d adoption C
ship the full 6-layer conversion system + every consumer adopted. Phase
3.1.7.5 → CLOSED 2026-05-15. Net: ~4.5 KLOC engine + ~2.3 KLOC tests
across 19 sub-slices. Full project ctest grew from 1546 (Phase 3.0 close)
through 1844 (D-006 + D-003 end of day) to **1913 win-debug at Phase
3.1.7.5 close**.

**Bug class eliminated:** Mars Climate Orbiter at the engine surface.
Eylem `RigidBody`, `PhysicsConfig`, `IPhysicsScene`, `ForceFieldComponent`
all carry dimensional tags. The new typed Vec reductions + geometry
typed-query overloads mean geometry consumers (eylem narrow-phase, scene
spatial queries, future SDF) cross the typed boundary without
information loss. Adoption-by-doc-contract (D29) covers byte-buffer
resources where typed access would defeat SIMD.

**Open follow-ups (not blockers):**
- `DimRoot<>` for unknown-Dim sqrt (deferred until a real consumer surfaces).
- Layer-6 dB / cents / RPM cross-paths (a sandbox might trip `2π · frequency` and need an `Angle{2π}` bridge — documented in v0c session log).
- `Mat<Quantity>` wrappers (deferred per ADR-0078 §2 D24; no homogeneous-Dim matrix consumer yet).

## §5 Amendment — Two-layer typed architecture (locked 2026-05-16, post-Phase-close)

**Status:** Accepted 2026-05-16. Codifies the implicit pattern that emerged
across §3 D22 (SIMD-boundary raw-f32 pin) + §3 D23 (signed_distance stays
raw) + §4 D27 (geometry-primitives algorithms stay raw, typed wrappers
above) + §4 D28 (renderer raw-Mat4f) + §4 D29 (resources byte-buffer +
typed at cooker / ECS boundaries). One-line rule: **units at the API
surface, raw scalars in the inner loop.**

### D32. Cerid is a two-layer typed system

```
┌─────────────────────────────────────────────────────────────────┐
│  UPPER LAYER — TYPED                                            │
│                                                                 │
│  Every public API, ECS field, config key, cooker output, UI     │
│  display path, cross-module signature uses Quantity<D, T>.      │
│                                                                 │
│  - eylem::RigidBody, PhysicsConfig, IPhysicsScene               │
│  - scene::Transform::translation                                │
│  - geometry::Sphere<Length32>, queries_typed.hpp wrappers       │
│  - crd-config get_length / get_mass / get_*                     │
│  - UnitPreferences format/parse                                 │
│  - future cookers, future domain modules                        │
└─────────────────────────────────────────────────────────────────┘
                              │
            ╔═════════════════╧═════════════════╗
            ║  API SURFACE — the boundary       ║
            ║                                   ║
            ║  Bridges: .value, to_raw_vec,     ║
            ║  from_raw_vec, strip-compute-     ║
            ║  retag wrappers. Each crossing    ║
            ║  is one line with a one-line      ║
            ║  comment naming the boundary.     ║
            ╚═════════════════╤═════════════════╝
                              │
┌─────────────────────────────────────────────────────────────────┐
│  LOWER LAYER — RAW                                              │
│                                                                 │
│  - SIMD kernels (Vec4f/Vec8f, _mm256_*, bvh4_simd, watertight)  │
│  - Math primitives (Vec/Mat/Quat inner ops; dot/cross/length    │
│    internals; signed_distance.hpp iq SDFs)                      │
│  - Geometry algorithm bodies (closest_point.hpp / intersect.hpp │
│    / barycentric.hpp / formulary.hpp / plucker.hpp / ...)       │
│  - Numerical kernels (future BLAS / LAPACK / SVD / Lanczos /    │
│    L-BFGS / GMRES / FFT / autodiff tape)                        │
│  - GPU command-buffer writes (vkCmdPushConstants, SSBO writes)  │
│  - File / wire byte buffers (CRDR pack, MeshResource.vertices,  │
│    network protocols)                                           │
│  - On-disk cooked payloads                                      │
└─────────────────────────────────────────────────────────────────┘
```

### D33. The boundary is exactly one layer thick

A typed function (`closest_point(Sphere<Length32>, Vec3<Length32>)`) calls a
raw function (`closest_point(Sphere<f32>, Vec3f)`) with a bridge at the
boundary. The bridge is ALWAYS exactly one line per stripped argument
(`to_raw_vec(...)`) and one line per re-tagged return (`from_raw_vec<D>(...)`).

Three idiomatic boundary patterns:

1. **Strip-compute-retag wrapper** (preferred for query APIs):
   ```cpp
   template <typename D, typename T>
   Vec3<Quantity<D, T>> closest_point(const Sphere<Quantity<D, T>>& s, const Vec3<Quantity<D, T>>& p) {
       return from_raw_vec<D>(closest_point(strip(s), to_raw_vec(p)));
   }
   ```
2. **`.value` egress at GPU upload / SIMD load**:
   ```cpp
   cmd.push_constants(layout, &transform.world, sizeof(Mat4f));  // already raw Mat4f
   put_lane(tile.pos_x, lane, body.position.x.value);            // typed → raw at column store
   ```
3. **Tagging at file / cooker load**:
   ```cpp
   Vec3f raw = read_vertex_bytes();
   Vec3<Length32> typed = from_raw_vec<dim::Length>(raw);  // contract: glTF spec SI metres
   ```

### D34. The lower layer NEVER carries a Dim tag

This is not a temporary compromise. It is the design.

- SIMD intrinsics are dimensionless by physics — `_mm256_load_ps` cannot
  carry a compile-time tag through a lane shuffle.
- Numerical algorithms (BLAS / LAPACK / sorting / hashing) are
  precision-tier-concerned, not dimension-concerned. They want bare `f64`.
- GPU shaders operate on raw `float` vectors; the typed surface ends at
  the upload site.
- On-disk payloads are byte buffers. Re-tagging on every vertex read
  would defeat the SIMD upload path and add zero semantic safety
  (the SI interpretation is fixed by the format spec).

Pretending otherwise dead-ends in the first SIMD intrinsic, the first
LAPACK call, or the first `vkCmdPushConstants`. The two-layer split is
the honest design.

### D35. Domain modules grow domain-specific typed surfaces

Future modules (`crd-eylem-aero`, `crd-eda`, `crd-cam`, `crd-cfd`,
`crd-control`, `crd-fea`) add their own dimensional types in their own
`units` sub-namespaces (Layer-5 federated extension from §1 D11). They
sit ABOVE the lower layer — same pattern. The numerical / SIMD / GPU
substrate stays raw and shared; each domain wraps the raw kernels in
its own typed surface.

Example: `crd-hesap-dense` (next major substrate per Strategic
Execution Plan) operates on raw `f64` matrices internally — that's a
numerical-computing layer. An `eylem v7 FEM` consumer wraps hesap calls
with its own typed in/out shape: solid mechanics tensor (typed Force / Stress
/ Strain at the API surface), raw `f64` matrix entries inside the LU
factorisation.

### D36. Documentation contract — every boundary comment is one line, ADR-tagged

When a `.value` or `to_raw_vec` appears outside its idiomatic place
(strip-compute-retag wrapper, ECS read/write boundary, push-constant
write), add a one-line comment:

```cpp
// raw egress — SIMD column store per ADR-0078 §3 D22
put_lane(tile.pos_x, lane, body.position.x.value);
```

Future readers see the ADR tag and know they're at a sanctioned boundary,
not an accidental escape.

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
