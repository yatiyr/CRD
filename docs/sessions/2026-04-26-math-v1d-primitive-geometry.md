# Session — 2026-04-26 — crd-math v1d primitive geometry

## Goal

Finish the Phase 1 math substrate with a clean primitive-geometry layer:

1. `Ray`, `Plane`, `AABB`, `Sphere`, `Triangle`, `Frustum`
2. fast foundational intersection/distance helpers
3. good edge-case coverage
4. benchmarks, smoke example, and docs closure

## What we built / changed

- added `engine/math/include/crd/math/geometry.hpp`
- shipped primitive types:
  - `Ray<T>`
  - `Plane<T>`
  - `Sphere<T>`
  - `AABB<T>`
  - `Triangle<T>`
  - `Frustum<T>`
- shipped helpers:
  - `point_at(ray, t)`
  - plane construction, normalization, signed distance, closest-point
  - AABB center/extents/contains/closest-point/intersection
  - sphere contains/intersection
  - triangle normal/centroid/barycentric/contains
  - `intersect_ray_plane`
  - `intersect_ray_sphere`
  - `intersect_ray_triangle` (Moller-Trumbore)
  - frustum extraction from view-projection matrix
  - frustum point / sphere / AABB tests
- extended formatting and Natvis support to the new primitive types
- extended smoke math example with primitive-geometry use
- extended benchmarks with geometry workloads

## Design notes

- This slice intentionally stops at **fast foundational geometry**, not full
  computational geometry.
- The current helpers are the substrate future geometry algorithms will build on:
  especially barycentric coordinates, triangle tests, and frustum extraction.
- The code stays scalar-first and POD-like so future SIMD work can layer on top
  without fighting the type model.

## Files touched

- `engine/math/include/crd/math/geometry.hpp`
- `engine/math/include/crd/math/math.hpp`
- `engine/math/include/crd/math/format.hpp`
- `tests/math/test_math.cpp`
- `tests/bench/test_bench.cpp`
- `runtime/examples/smoke_math.cpp`
- `.natvis`
- `docs/bench/baseline_2026-04.md`
- `docs/systems/math.md`
- `docs/ROADMAP.md`
- `CONTEXT.md`

## Tests / verification

- Debug: `152/152`
- Release: `151/151`
- ASan: `152/152`
- direct release math executable: `32` test cases, `380` assertions, all green
- direct debug geometry-tagged subset: `4` test cases, `51` assertions, all green

## Benchmark additions

- `Rayf` plane intersection: `1.14232 ns`
- `Rayf` triangle intersection: `4.41779 ns`
- `Frustumf` AABB test: `5.3504 ns`

## One implementation wrinkle captured

MSVC Release + LTCG was overly conservative about guarded reciprocals inside
`intersect_ray_triangle`. We scoped a `/wd4723` suppression to `crd-math`
because the runtime checks already guarantee the divisor stays non-zero.

## Next session starts with

`crd-platform` v1a.
