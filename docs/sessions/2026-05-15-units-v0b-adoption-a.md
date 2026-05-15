# Session log — 2026-05-15 — units v0b adoption pass A

> Phase 3.1.7.5 `crd-units` adoption A. Sequential v0b-1 -> v0b-2 -> v0b-3 -> v0b-4 -> v0b-close, all shipped same session under 5-config DoD.

## Scope

v0b lights up the `crd-units` substrate across the rest of the engine.
v0a shipped the dimensional types + CI guard; v0b makes them load-bearing.

| Sub-slice | Surface |
|---|---|
| v0b-1 | `Vec<Quantity>` enablement + precision-suffix aliases |
| v0b-2 | `crd-config` unit-tagged TOML accessors |
| v0b-3 | `scene::Transform::translation` -> `Vec3<Length<f32>>` |
| v0b-4 | glTF cooker `.meta` `[cook] position_scale` (SI normalization) |
| v0b-close | docs + ADR §2 amendment + 5-config full sweep + session log |

## v0b-1 — `Vec<Quantity>` enablement

**Decision:** widen `Vec2`/`Vec3`/`Vec4` from `MathScalar` (`f32`/`f64`)
to a new `MathValue` concept (`MathScalar || IsQuantity`). Element-wise
ops (`==`, `+`, `-`, scalar `*`, scalar `/`) gain a `requires` clause
that gates on actual `T op S` capability. Reductions (`dot`, `cross`,
`length`, `length_squared`, `distance`, `distance_squared`, `hadamard`,
`try_normalize`, `normalize`) stay on strict `MathScalar` — `sqrt(Area)
-> Length` needs `DimRoot<>` machinery that is out of scope for v0b
(ADR-0078 D15).

Member defaults change from `T x = static_cast<T>(0)` to `T x{}`
(Quantity's explicit ctor rejects the cast). The class-template
operator bodies are not instantiated until they are called, so the
in-class `operator*= T` etc. survive even when not valid for
`Vec3<Quantity>` — verified via grep that no caller exercises
those paths.

Aliases: `Length32` / `Length64` / `Mass32` / `Mass64` / `Time32` /
`Time64` / `Force32` / `Voltage32` / ... live in a new
`engine/units/include/crd/units/quantity_aliases.hpp`. They are
non-Temperature; the affine `Temperature<T>` stays in
`units_affine.hpp`.

Boundary helpers in `engine/math/include/crd/math/vec.hpp`:

```cpp
template <typename D, typename T>
constexpr Vec3<T> to_raw_vec(const Vec3<Quantity<D, T>>& v) noexcept;

template <typename D, typename T>
constexpr Vec3<Quantity<D, T>> from_raw_vec(const Vec3<T>& v) noexcept;
```

`engine/math/include/crd/math/quat.hpp` gains a `from_trs` overload
accepting a `Vec3<Quantity<D, T>>` translation; it strips via
`to_raw_vec` so `scene::Transform::local()` composes without a
rewrite of `from_trs`.

`engine/math/CMakeLists.txt` adds `crd-units` to PUBLIC link.
`engine/math/include/crd/math/scalar.hpp` adds the `MathValue` concept.

**Tests:** `tests/units/test_vec_quantity.cpp` (13 cases / 40
assertions). Layout pin, default-zero, element-wise +/-/scale/divide/
negation/equality, to/from raw round-trip, byte-identical pass-
through, type-distinction (`Vec3<Length>` and `Vec3<Force>` don't
mix), f64 tier, determinism contract.

## v0b-2 — `crd-config` unit-tagged TOML accessors

13 typed accessors in `engine/config/include/crd/config/unit_accessor.hpp`:
`get_length`, `get_mass`, `get_time`, `get_angle`, `get_velocity`,
`get_force`, `get_pressure`, `get_energy`, `get_power`,
`get_voltage`, `get_current`, `get_frequency`, `get_temperature`.
Implementation in `engine/config/src/unit_accessor.cpp` uses
`SuffixEntry` + `AffineEntry` tables keyed by authoring strings
(`_m _mm _cm _km _um _in _ft _yd _mi`, `_kg _g _mg _t _lb_mass
_oz_mass`, `_rad _deg _turn`, `_mps _kmph _mph _knots`, `_N _kN
_lbf`, `_Pa _kPa _MPa _bar _psi _atm`, `_kelvin _celsius
_fahrenheit`, ...). Each accessor is explicitly instantiated for
`f32` and `f64` via the `CRD_CONFIG_INSTANTIATE_UNIT_ACCESSOR`
macro.

Critical fix: `resolve_linear` and `resolve_affine` check
`cfg.contains(key)` FIRST before suffix matching — otherwise a
missing key would match the `_mm` suffix on an empty string and
return `0`.

**Tests:** `tests/config/test_unit_accessor.cpp` (13 cases / 57
assertions). Every dimension covered + fallback semantics.

## v0b-3 — `scene::Transform::translation`

```cpp
struct Transform {
    crd::math::Vec3<crd::units::Length32> translation{};   // <- typed
    crd::math::Quatf                       rotation = ...;  // unchanged (D14: frame, not unit)
    crd::math::Vec3f                       scale{1,1,1};   // unchanged
    crd::math::Mat4f                       world = ...;    // unchanged (GPU upload, D10)

    Mat4f local() const noexcept
    { return from_trs(to_raw_vec(translation), rotation, scale); }
};
```

Ripple paid at all assignment / read sites:

- `engine/scene/src/world.cpp`: `set_local` (line 1037), `set_translation` (line 953) wrap
  raw input with `from_raw_vec<dim::Length>(...)`.
- `engine/eylem-rigid3d/src/eylem_system.cpp` line 128: same bridge on
  the integrator-to-transform sync.
- `tools/asset_cooker/src/cook_handlers/scene.cpp` line 206: scene
  cooker likewise.
- `sandbox/src/sandbox_layer.cpp` lines 449 / 1346: ImGui inspector
  reads `.x.value` and renders with ` m` suffix.
- ~20 test sites in `tests/scene/test_transform.cpp`,
  `tests/scene/test_obek.cpp`, `tests/scene_cooker/*`,
  `tests/eylem-rigid3d/test_eylem_system.cpp`,
  `tests/eylem-rigid3d/test_interpolation_system.cpp`,
  `tests/eylem-viz/test_visualizers.cpp` — bulk-patched
  `translation.{x,y,z},` -> `translation.{x,y,z}.value,` and
  `translation = {a, b, c};` -> `from_raw_vec<dim::Length>(...)`.

## v0b-4 — glTF cooker `.meta` `[cook] position_scale`

glTF 2.0 §3.5.2.1 mandates SI meters for POSITION. There is no
`KHR_unit` extension. Real-world exporters (Blender's "Apply
Transform" toggle, SolidWorks cm exports, Unity 3DS imports)
violate the spec; v0b-4 adds a per-asset opt-in conversion at
cook time so the runtime always sees SI regardless of source.

New files:
- `tools/asset_cooker/include/crd/cooker/mesh_cook_options.hpp` — `MeshCookOptions { position_scale }` + `parse_mesh_cook_options(StringView)` + `kSiPositionSanityMeters = 1e6F`.
- `tools/asset_cooker/src/cook_handlers/mesh_cook_options.cpp` — line-oriented `.meta` parser, ignores invalid / non-positive values with a warning.
- `tests/cooker/test_mesh_cook_options.cpp` + `tests/cooker/CMakeLists.txt` — 12 cases / 15 assertions covering empty / no-section / `0.01` / `1e-3` / CRLF / negative / zero / wrong-section / inline-comment / duplicate / section-toggling / SI-invariant pass-through.

Wiring in `tools/asset_cooker/src/cook_handlers/mesh.cpp`:
- `gltf_handler` reads `ctx.meta_path` once at handler entry, parses the optional `[cook]` section, threads `MeshCookOptions cook_options` through `build_mesh_artifact` and `cook_primitive`.
- `cook_primitive` multiplies every position attribute by `options.position_scale` before normals / UVs / tangents are processed. MikkTSpace sees the scaled positions so tangent generation is consistent with the cooked vertex buffer.
- After scaling, `cook_primitive` tracks `max_abs` magnitude and emits an SI-sanity warning to stderr when it exceeds `1e6 m` (1000 km), pointing the user at the `[cook] position_scale` knob.

Default `position_scale = 1.0F` preserves bit-exact pass-through for
conformant assets (the SI-invariant test pins this — `1.25F * 1.0F ==
1.25F` exactly).

## v0b-close — 5-config DoD sweep

Per `feedback_per_slice_run_ctest.md` + ADR-0079 D7, the 5 configs
verified across every sub-slice and again at close:

| Config | Build | CTest |
|---|---|---|
| win-debug | clean | 1882/1882 |
| win-asan | clean | 1882/1882 |
| win-shipping | clean | 1795/1795 |
| win-shipping-profile | clean | 1877/1877 |
| win-tidy | clean | — (build-only) |

Per-slice trend:
- v0b-1: +13 cases / +40 assertions
- v0b-2: +13 cases / +57 assertions
- v0b-3: 0 new cases (existing test sites migrated)
- v0b-4: +12 cases / +15 assertions
- **Phase total: +38 cases / +112 assertions on top of v0a's 138 cases / 464 assertions.**

## Decisions locked

- ADR-0078 §2 amendment: D15 `Vec<Quantity>` element-wise only; D16 config accessors; D17 scene::Transform; D18 glTF `.meta` `[cook] position_scale`; D19 5-config DoD carried forward.
- `Mat<Quantity>` deferred (no homogeneous-dimension consumer yet).
- Fractional-exponent dimensions (`sqrt(Area)`) deferred — reductions stay on `MathScalar`.

## Open follow-ups

- **v0c adoption B** (next): `crd-eylem` `RigidBody` dimensional; integrator typed-math; force-field substrate; `crd-geometry-primitives` API surface re-tag.
- **v0d adoption C**: `crd-renderer` uniform-upload boundary; `crd-resources` cookers; ImGui inspector; Layer-6 full format/parse/`UnitPreferences`; cross-engine readers (STEP, IGES, FBX, IFC, Gerber).
- **Tighten `crd-no-untagged-physical-numeric`** regex to function parameters as v0c lands.

## References

- ADR-0078 §2 amendment (D15-D19).
- `docs/systems/units.md` — status table + boundary stencils added.
- `docs/phases/phase-3.1.7.5-units.md` — phase plan (next: v0c).
- `feedback_per_slice_run_ctest.md` — 5-config DoD protocol.
- `feedback_strategic_execution_plan_2026_05_15.md` — Pathway A units-first.
