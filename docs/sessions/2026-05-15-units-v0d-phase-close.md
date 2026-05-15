# Session log — 2026-05-15 — units v0d adoption pass C + Phase 3.1.7.5 CLOSE

> Phase 3.1.7.5 `crd-units` closes. v0d-1 (Vec reductions widening) → v0d-2 (geometry-primitives API re-tag) → v0d-3 (renderer boundary) → v0d-4 (resources contract) → v0d-5 (Layer-6 format/parse + 11 disciplines) → v0d-6 (ImGui inspector) → v0d-close.

## Scope landed

| Sub-slice | Surface |
|---|---|
| v0d-1 | Vec<Quantity> reductions widened (length/dot/cross/distance/normalized/hadamard) |
| v0d-2 | `crd-geometry-primitives` struct widening + `queries_typed.hpp` boundary-wrapper layer |
| v0d-3 | `crd-renderer` raw-Mat4f boundary contract pinned in `FrameContext` |
| v0d-4 | `crd-resources` byte-buffer + cooker SI-meter contract pinned in `mesh_resource.hpp` |
| v0d-5 | Layer-6 `UnitPreferences` + format/parse + 11 discipline presets |
| v0d-6 | `crd-imgui` UnitPreferences inspector (header-only optional) |
| v0d-close | ADR-0078 §4 amendment (D26–D31) + Phase 3.1.7.5 CLOSE + this log + full sweep |

## v0d-1 — Vec reductions widening

`engine/math/include/crd/math/vec.hpp` adds typed reductions alongside the
existing MathScalar versions:

```cpp
template <typename D, typename T>
inline Quantity<D, T> length(const Vec3<Quantity<D, T>>& v) noexcept
{
    return Quantity<D, T>{std::sqrt(length_squared(v).value)};
}
```

Same pattern for `length_squared`, `dot` (cross-Dim returns `DimMul<D1,D2>`),
`cross` (returns `Vec3<DimMul<D,D>>`), `distance`, `distance_squared`,
`hadamard`, `normalized` (returns `Vec<T>` dimensionless unit). The earlier
"DimRoot needed" framing was overstated — `length(Vec<Q>) → Q` works through
`Q²` re-tagging because the return Dim is statically known from input.

Tests: `tests/units/test_vec_quantity_reductions.cpp` — 9 cases / 23 assertions,
including bit-identical determinism vs raw MathScalar path.

## v0d-2 — `crd-geometry-primitives` re-tag

Strategy after scope discovery (advisor flagged the 2-day floor):
**struct widening + boundary-wrapper layer**, not full algorithm widening.

1. **Struct widening:** swept `<MathScalar T>` → `<MathValue T>` in
   `primitives.hpp` (95 sites). `Sphere<Length32>` / `Box<Length32>` /
   `Capsule3<Length32>` / `Triangle3<Length32>` / `Plane<Length32>` / etc.
   exist as typed data.
2. **Algorithm headers REVERTED** to `<MathScalar T>`. `closest_point.hpp` /
   `intersect.hpp` / `barycentric.hpp` / `formulary.hpp` / `plucker.hpp` /
   `watertight_ray_tri.hpp` / `robust_ray_aabb.hpp` / `is_finite.hpp` /
   `constants.hpp` — algorithms stay raw because internal arithmetic
   (`std::numeric_limits<T>::min()`, `std::sqrt(T)`, `static_cast<T>(0)`,
   dimensionless cascades) doesn't compose for Quantity.
3. **NEW `queries_typed.hpp`** with strip helpers + typed-overload wrappers:
   - 8 `closest_point` typed overloads (Sphere / Capsule3 / Cylinder3 /
     AABB3 / OBB3 / Triangle3 / Plane / Segment3).
   - 5 `distance` typed overloads (Sphere / Capsule3 / AABB3 / Triangle3 / Plane).
   - 4 `distance_squared` typed overloads (return `Quantity<DimMul<D,D>>`).
   - Pattern: `closest_point(Sphere<Q>, Vec3<Q>) → strip Q at boundary →
     call raw `closest_point(Sphere<f32>, Vec3<f32>)` → re-tag result as Vec3<Q>`.
     Zero runtime overhead (`to_raw_vec` / `from_raw_vec` constexpr;
     `.value` accessors compile away).

Foundation widening in `scalar.hpp`: `abs` / `min` / `max` / `clamp` /
`is_finite` / `is_nan` / `default_epsilon<Q>` Quantity overloads + new
`sqrt_as<D, T>(Quantity<DimMul<D,D>, T>) → Quantity<D, T>` helper.

Fix: `Plane<T>::d` default `static_cast<T>(0)` → `T{}` (works for both raw
and Quantity zero-init).

Tests: `tests/geometry-primitives/test_queries_typed.cpp` — 8 cases /
23 assertions, including bit-identical determinism vs raw path.

**The "Option A = widen all 388 sites" scope was a misread.** The advisor
called it out mid-stream: foundation widening + per-algorithm `if
constexpr` branches would be 2+ days of work for the same typed-API
surface that the boundary-wrapper pattern delivers in ~150 LOC. Pattern is
documented as the canonical typed-query approach in ADR-0078 §4 D27.

## v0d-3 — `crd-renderer` boundary

Audit confirmed `crd-renderer` was ALREADY at the raw-Mat4f boundary:
`Renderable::transform = Transformf` (raw), `DrawItem::model = Mat4f`,
push constants take raw Mat4f. Dimensional types live one layer above in
`scene::Transform` (v0b-3). Bridging is done by `TransformPropagation`
via `from_trs(to_raw_vec(translation), rotation, scale)`.

v0d-3 is doc-only: added contract pin to `engine/renderer/include/crd/
renderer/renderer.hpp:FrameContext` documenting that the renderer never
imports `crd/units/*` and never includes `crd-scene::Transform`. SIMD
upload path + LTCG bit-identical to pre-units build.

## v0d-4 — Resources contract

`crd-resources` is byte-buffer based; geometric interpretation lives at
cooker (`.meta [cook] position_scale` per ADR-0078 §2 D18, v0b-4) and ECS
(`scene::Transform::translation = Vec3<Length32>`, v0b-3). v0d-4 is
doc-only: contract pin added to `mesh_resource.hpp` documenting both
boundaries. Re-tagging vertex bytes would defeat SIMD upload (D22).

## v0d-5 — Layer-6 format/parse + 11 disciplines

New `engine/units/include/crd/units/unit_preferences.hpp` +
`src/unit_preferences.cpp`:

- 13 `UnitChoice` enums for Length / Mass / Time / Angle / Velocity / Force
  / Pressure / Energy / Power / Voltage / Current / Frequency / Temperature.
- `UnitPreferences` struct holds the per-Dim choice + precision_digits +
  include_suffix + scientific_notation toggles.
- 11 discipline-preset factories: Game, CAD, Robotics (ROS REP 103),
  Aerospace, PCB/EDA, Audio, 3D Print, CAM, Cinematic, Imperial,
  SI Strict, Scientific.
- `format_length` / `format_mass` / `format_time` / `format_angle` /
  `format_velocity` / `format_force` / `format_pressure` / `format_energy`
  / `format_frequency` / `format_temperature` — SI Quantity in → String
  in display unit.
- `parse_length<T>` / `parse_mass<T>` / `parse_angle<T>` / `parse_velocity<T>`
  / `parse_force<T>` / `parse_pressure<T>` — typed Quantity out;
  `optional<>` on malformed input. `parse_temperature_to_kelvin` for
  affine temperature.
- `suffix_for(UnitChoice)` helper for UI labels.

Temperature is affine (K canonical; C / F / Rankine convert through
scale + offset). Format uses `std::snprintf("%.*g", precision_digits, ...)`;
sci-notation toggle uses `%.*e`. Parse is the inverse of v0b-2's
`crd-config` accessor suffix tables.

Tests: `tests/units/test_unit_preferences.cpp` — 14 cases / 42 assertions
covering preset distinctness, format/parse round-trip, malformed-input
handling, affine temperature, suffix toggles.

## v0d-6 — `crd-imgui` UnitPreferences inspector

New header `engine/imgui/include/crd/imgui/unit_preferences_inspector.hpp`
(header-only; pulls `<imgui.h>` directly so it's optional — consumers
include only when ImGui is in scope). Provides:

```cpp
bool draw_unit_preset_picker(UnitPreferences& prefs);              // dropdown of 12 presets
bool draw_unit_preferences_inspector(UnitPreferences& prefs);      // full per-Dim editor
```

Each function returns `true` if the prefs changed (consumer persists
via `crd-config`). UI: preset combo, per-Dim combos for Length / Mass /
Time / Angle / Temperature, precision_digits slider, suffix +
scientific_notation toggles. Each Dim choice gets a friendly label
("m (meter)", "mm (millimeter)") via `detail_units_ui::*_label` switch
tables.

`crd-imgui` adds `crd-units` to its PUBLIC dep list. No new test
(header-only ImGui code is GPU/window-only; compile-tested via the
crd-imgui library build under all 5 configs).

## v0d-close — Phase 3.1.7.5 CLOSE

- ADR-0078 §4 amendment (D26-D31): Vec reductions, geometry-primitives
  re-tag pattern, renderer boundary, resources contract, Layer-6
  UnitPreferences, Phase close marker.
- `docs/systems/units.md` status table updated (v0d-* all ✅).
- `docs/ROADMAP.md` Phase 3.1.7.5 row → CLOSED.
- `docs/decisions/README.md` entry tag updated.
- `context.md` current-focus + NEXT updated.
- MEMORY index + project_state synced.
- This session log.

## 5-config DoD per slice

| Slice | win-debug | win-asan | win-shipping | win-shipping-profile | win-tidy |
|---|---|---|---|---|---|
| v0d-1 | 1891/1891 | 1891/1891 | 1804/1804 | 1886/1886 | clean |
| v0d-2 | 1899/1899 | 1899/1899 | 1812/1812 | 1894/1894 | clean |
| v0d-3 | (doc-only; verified build) | — | — | — | — |
| v0d-4 | (doc-only; verified build) | — | — | — | — |
| v0d-5 | 1913/1913 | 1913/1913 | 1826/1826 | 1908/1908 | clean |
| v0d-6 | 1913/1913 | 1913/1913 | 1826/1826 | 1908/1908 | clean |

Net: +35 cases / +88 assertions on top of v0c (full project ctest
1882 → 1913 win-debug).

## Phase 3.1.7.5 close summary

**Calendar:** ~3 weeks (v0a-close 2026-05-15 → v0d-close 2026-05-15). Same
day chronology with detours D-006 / D-003 + adoption A/B/C all shipped.

**Net code:**
- `engine/units/` — 14 headers + 2 cpp (~3 KLOC).
- `engine/math/` (additions) — Vec<Quantity> reductions, cross-Dim Vec*Q overloads, MathValue concept, scalar Quantity overloads, sqrt_as helper (~400 LOC).
- `engine/eylem/` (typing) — RigidBody, PhysicsConfig, IPhysicsScene, ForceFieldComponent (~150 LOC delta).
- `engine/scene/` (typing) — Transform.translation (~50 LOC delta).
- `engine/config/` (Layer-1) — 13 unit-tagged TOML accessors (~400 LOC).
- `engine/renderer/` + `engine/resources/` — doc-only contract pins.
- `engine/geometry-primitives/` — struct widening + queries_typed.hpp (~500 LOC).
- `engine/imgui/` — UnitPreferences inspector header (~150 LOC).
- `tools/asset_cooker/` — glTF cooker `[cook] position_scale` (~100 LOC).

**Total: ~4.5 KLOC engine + ~2.3 KLOC tests across 19 sub-slices.**

**ADR-0078 amendments shipped:** §1 (v0a baseline, D1-D14) + §2 (v0b
adoption A, D15-D19) + §3 (v0c adoption B, D20-D25) + §4 (v0d adoption
C + Phase close, D26-D31). 31 locked design decisions; one Phase ADR
covering the complete dimensional adoption arc.

**Bug class eliminated:** Mars Climate Orbiter at the engine surface.
Every physical / scientific quantity at every API boundary carries a
compile-time dimension tag. The `crd-no-untagged-physical-numeric` CI
guard remains live; the type system is the durable enforcement.

## Open follow-ups

- **`DimRoot<>`** — fractional-exponent dimensions for unknown-Dim sqrt.
  Deferred until a real consumer surfaces; v0d showed length(Vec<Q>) →
  Q works without it.
- **`Mat<Quantity>`** — homogeneous-Dim matrix wrappers (ADR-0078 §2 D24).
  Deferred; no consumer yet.
- **dB / cents / RPM cross-paths** — when `2π · frequency` appears in
  audio / robotics, the bridge is `Angle{2π} * frequency → AngularVelocity`.
  Documented in v0c session log.
- **Tighten `crd-no-untagged-physical-numeric` regex** — current form is
  conservative (struct fields only); future slices can broaden to function
  parameters as more domain modules adopt typed surfaces.
- **`crd-eylem-aero` / scientific computing federated unit packs** — the
  Layer-5 federated extension pattern is in place from v0a; first consumer
  module adds its own units in its own namespace, ADL handles lookup.

## References

- ADR-0078 §1 + §2 + §3 + §4 (all four amendments).
- `docs/systems/units.md` — final status + boundary stencils + 11-preset table.
- `docs/phases/phase-3.1.7.5-units.md` — Phase plan (now CLOSED).
- `feedback_strategic_execution_plan_2026_05_15.md` — Pathway A (units-first) locked.
- `feedback_per_slice_run_ctest.md` — 5-config DoD protocol.
- `feedback_always_units.md` — the original "no opt-out" mandate that drove the phase.
- `feedback_full_sweep_required.md` — slice closure protocol.
- Sessions: `2026-05-15-units-v0a-substrate.md`, `2026-05-15-units-v0b-adoption-a.md`, `2026-05-15-units-v0c-adoption-b.md`, this log.
