# Session — 2026-05-12 — Phase 3.1.7 v0a: `crd-geometry-primitives` + the `crd::math::geometry` move-and-delete

## Goal

Open Phase 3.1.7 (`crd-geometry`) with v0a: stand up the `crd-geometry-primitives` module
and its v0 primitive-type catalogue, and execute the ADR-0076 §13 move-and-delete that
relocates the pre-existing `crd::math::geometry` into it (so `crd-math` stays lean).

## What we built / changed

- New module `engine/geometry-primitives/` — target `crd-geometry-primitives`, namespace
  `crd::geometry::primitives`, headers under `crd/geometry/primitives/`, force-link
  `geometry_primitives.cpp`. System overview: `docs/systems/geometry-primitives.md`.
- v0 type catalogue in `primitives.hpp` + `format.hpp`: `Point3` (alias), `Line`/`Segment`/`Ray`,
  `Plane`, `AABB`, `OBB`, `Sphere`, `Capsule`, `Triangle3`, `Frustum` + their `std::formatter`s.
- **ADR-0076 §13 move-and-delete:** `crd::math::geometry`'s `Ray`/`Plane`/`Sphere`/`AABB`/
  `Triangle`(→`Triangle3`)/`Frustum` + ~16 helpers + their formatters migrated to
  `crd::geometry::primitives::*`; `engine/math/include/crd/math/geometry.hpp` **deleted**;
  9 consumers repointed (`crd-scene` `query.hpp`/`world.hpp` — new PUBLIC edge
  `crd-scene → crd-geometry-primitives`; `crd-math` umbrella + `format.hpp`; `tests/math`
  geometry tests moved to new `tests/geometry-primitives/test_primitives.cpp`; `tests/bench`;
  `tests/scene`; `runtime/examples/smoke_math.cpp`).
- `crd-math` now ships only Vec/Mat/Quat/Transform/SIMD/`deterministic`. `/wd4723` MSVC
  suppression moved from `crd-math` to `crd-geometry-primitives`. `crd-no-std-math-check`
  CI guard's scoped-dir list gained `engine/geometry-primitives`.

## Plain-English explanation

`crd-geometry` is the new computational-geometry substrate (BVH, GJK/EPA, mesh processing,
Delaunay, …) that several Cerid domains will consume (eylem physics, sdf, renderer cull,
scene spatial index, navmesh, editor). v0a is just the foundation: the basic shape types and
the housekeeping move that pulls the old `crd::math::geometry` (which had quietly grown a
`Ray`/`Plane`/`AABB`/`Triangle`/`Frustum` set) out of `crd-math` and into the new module's
proper namespace, so `crd-math` goes back to being only the linear-algebra core. Nothing
"new" geometry-wise yet — the algorithms start landing in v0b onward.

## Decisions made

- The `crd::math::geometry` types belong in `crd-geometry-primitives`, not `crd-math` —
  `crd-math` is linear algebra only. (ADR-0076 §13.)
- New module edge `crd-scene → crd-geometry-primitives` (PUBLIC) — the scene query DSL and
  `World` already used the geometry types; the dependency was implicit, now explicit.

## Files touched

- `engine/geometry-primitives/**` — new module (CMake, `primitives.hpp`, `format.hpp`,
  `geometry_primitives.cpp`)
- `engine/math/include/crd/math/geometry.hpp` — **deleted**
- `engine/math/include/crd/math/{math,format}.hpp` — umbrella + formatter includes repointed
- `engine/scene/include/crd/scene/{query,world}.hpp` — use `crd::geometry::primitives::*`;
  `engine/scene/CMakeLists.txt` — links `crd-geometry-primitives` PUBLIC
- `tests/geometry-primitives/{CMakeLists.txt,test_primitives.cpp}` — new (the migrated math
  geometry tests)
- `tests/{bench,scene}/...`, `runtime/examples/smoke_math.cpp` — repointed includes
- `CMakeLists.txt` — `add_subdirectory(engine/geometry-primitives)`
- `scripts/check_no_std_math.{ps1,sh}` — scoped-dir list gained `engine/geometry-primitives`

## Tests / verification

- Built? ✅ (win-debug / win-asan / win-clang-cl / linux-gcc-debug)
- Tests pass? geometry-primitives-tests 6 cases; math-tests 140; scene-tests 271; smoke_math
- `crd-no-std-math-check` guard green
- (Full 14-config sweep deferred to v0 close — see the 2026-05-13 session.)

## Next session starts with

- Phase 3.1.7 v0b — the 2D peer set + the closest-point catalogue (`closest_point.hpp`).
