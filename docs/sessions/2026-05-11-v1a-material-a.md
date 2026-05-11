# Session — 2026-05-11 — v1a-material-a — Material API surface

## Goal

Lock the public `Material` API surface per ADR-0069 §1-§3 + §11 v1a-
critical-path. P0 — must close before v1a-b interface freeze because
`Material` gates the entire downstream API (vehicles need anisotropic
friction, robotics need LuGre, cinematic need Hunt-Crossley, scientific
computing wants the full enum catalogue locked at v1l).

## What we built / changed

- **`engine/eylem/include/crd/eylem/types.hpp`** — added:
  - **`MaterialId`** strong type, `[generation:8 | index:24]` layout
    matching `BodyId` / `ColliderId` / `JointId`. `null()` +
    `default_material()` (slot 1, generation 1) + `make()` + `==` operator.
  - **`FrictionModel` enum** — 6 values: `Coulomb` / `Stribeck` / `LuGre` /
    `Karnopp` / `Anisotropic` / `FrictionTriple`. Each documented with
    which slice ships its impl (Coulomb v1e; Stribeck/LuGre/Karnopp/
    Anisotropic/FrictionTriple v5).
  - **`RestitutionModel` enum** — 3 values: `Constant` / `Newton` /
    `HuntCrossley`. Slice mapping documented (Constant v1e;
    HuntCrossley v7 FEM; Newton v8d MPM).
  - **`CombineMode` extended** with `GeometricMean = 4` (additive — Box2D
    v3 / Jolt / Unity DOTS / AGX consensus default for friction).

- **`engine/eylem/include/crd/eylem/material.hpp`** — replaced the 20-byte
  v1a stub with the locked **64-byte `Material` struct**:
  - 24-byte friction block: `friction_model` + `friction_combine`
    (default `GeometricMean`) + 2-byte pad + `friction_static` +
    `friction_dynamic` + `friction_anisotropy` Vec3f
  - 8-byte friction-model parameters (`stribeck_velocity` /
    `viscous_coefficient`) — slot interpretation gated by `friction_model`
    per ADR-0069 §1
  - 12-byte restitution block: `restitution_model` + `restitution_combine`
    (default `Max` per PhysX convention) + 2-byte pad + `restitution` +
    `restitution_decay`
  - 12-byte `surface_velocity` Vec3f (conveyors, rolling tires)
  - 4-byte `density` (default 1000.0 = water)
  - 4-byte `yield_stress` reservation for post-v1 destruction
  - `static_assert(sizeof(Material) == 64)` + `static_assert(alignof(Material) == 4)`
  - `default_material_value()` constexpr helper

- **`tests/eylem/test_v1a_interface.cpp`** — 4 new freeze test cases (44
  new assertions): Material defaults pinned, FrictionModel enum values
  pinned, RestitutionModel enum values pinned, CombineMode extended-to-5
  pinned, MaterialId layout matches Body/Collider/Joint pattern,
  `default_material_value()` matches `Material{}`.

## Plain-English explanation

`Material` is what every contact in the simulator reads: friction
coefficients, bounce, surface velocity (conveyors), density (for mass
derivation). Every solver touches it; every collider points at one via
a `MaterialId` handle.

The struct is sized at exactly 64 bytes — one cache line. It's locked
at the v1a interface freeze, so it cannot grow. To support friction
models from Coulomb (universal default) all the way to LuGre (industrial
manipulation; bristle-deflection state) without struct growth, the
parameter slots are **reinterpreted by enum**: e.g., `stribeck_velocity`
is `v_s` for Stribeck friction, `σ_0` for LuGre, `v_thresh` for Karnopp.
The cooker validates the parameter ranges per friction model so the
runtime never sees a malformed slot.

`MaterialId` is the handle into the scene's `MaterialPool` (lands in
v1a-material-b). It uses the same `[generation:8 | index:24]` layout
as the other strong-type IDs (BodyId / ColliderId / JointId) — uniform
addressing across the eylem surface.

Only `Coulomb` friction + `Constant` restitution ship in v1e (the v1
critical path). The other 5 friction models + 2 restitution models are
"deferred sub-slices" that fill formula impls inside the already-frozen
Material surface — same blocked-sub-slice discipline as ADR-0067's
`Gradient` / `Script` field formulas. The struct doesn't change; only
the impl behind the enum gates fills in.

## Decisions made

- **`MaterialId` layout matches `BodyId` / `ColliderId` / `JointId`** —
  uniform `[gen:8|idx:24]` pattern across the eylem strong-type ID
  family. Designers / tooling learn one address pattern.
- **`GeometricMean` is additive (slot 4)** — preserves the existing
  `CombineMode` enum for any code reading values 0-3.
- **`default_material_value()` is `constexpr`** — usable in static
  initialisation for cooker-side default material registration without
  runtime-init order concerns.
- **Field-overloading discipline documented in-source per
  `friction_model` enum** — Stribeck reuses `stribeck_velocity` /
  `viscous_coefficient`; LuGre overloads them as `σ_0` / `σ_2`; Karnopp
  uses `stribeck_velocity` as `v_thresh`. Cooker validates per model
  before the runtime sees the bytes.
- **`FrictionTriple` enum slot reserved at v1a freeze** — even though
  v5 vehicles ships the impl, the enum slot is locked NOW so the cooker
  artifact format / snapshot artifact format / öbek prefab serialisation
  don't need a major-version bump when v5 lands. Reinterprets
  `friction_anisotropy` Vec3f as `(sliding, torsional, rolling)` per
  the MuJoCo §2.8 pattern — same 12 bytes, different reading.

## Files touched

- `engine/eylem/include/crd/eylem/types.hpp` — added MaterialId + FrictionModel + RestitutionModel + extended CombineMode
- `engine/eylem/include/crd/eylem/material.hpp` — replaced 20-byte stub with locked 64-byte struct
- `tests/eylem/test_v1a_interface.cpp` — 4 new freeze cases / 44 new assertions

## Tests / verification

- Built? ✅ Whole engine clean (Material change rippled through
  `NullPhysicsScene` + `ColliderPool` + downstream consumers without
  regression)
- `crd-eylem-tests`: **161 assertions across 19 test cases pass** (was 117/15)
- Full `ctest --preset win-debug`: **1029/1029 pass** (was 1025; +4 from new Material surface cases)
- Sweep cadence: win-debug only this slice; full sweep batched at end
  of v1a-material cluster (a/b/c/d) per project policy.

## Next session starts with

- **v1a-material-b**: `MaterialPool` on scene; `create_material` /
  `update_material` / `material(id)` API; öbek/cooker round-trip stub.
  Adds the impl for the `MaterialId` handle pattern declared today.
  ~200 LOC + ~4 tests.
