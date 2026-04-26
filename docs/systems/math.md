# crd-math

Math substrate for graphics, simulation, robotics, and later numerical work.

## Status

| Slice | Ships | Status |
| --- | --- | --- |
| v1a | scalar helpers, `Vec2/3/4`, `f32/f64` aliases | ✅ |
| v1b | `Mat2/3/4` (column-major) | ✅ |
| v1c | `Quat`, `Transform` | ✅ |
| v1d | `Ray`, `Plane`, `AABB`, `Sphere`, `Triangle`, `Frustum` | ✅ |

## Core decisions

- Column-major matrices
- Column vectors, so multiplication is `Mat * Vec`
- Radians-first API
- Public API supports both `float` and `double`
- Scalar-first implementation, SIMD-ready layout and benchmarks from day one
- Quaternion convention: Hamilton, stored as `xyzw`, identity = `(0, 0, 0, 1)`
- Matrix storage contract: columns are stored directly, so `m[0]` is the first
  column and `transpose(m)` materialises the row/column swap explicitly

## What ships today

- scalar constants and helpers (`pi`, radians/degrees, finite/NaN, approximate compares)
- ergonomic float/double aliases for the core angle constants
- `Vec2/3/4<T>` plus `Vec*f` / `Vec*d`
- `Mat2/3/4<T>` plus `Mat*f` / `Mat*d`
- `Mat * Vec`, `Mat * Mat`, transpose, identity/zero constructors
- `Quat<T>` plus `Quatf` / `Quatd`
- rigid `Transform<T>` plus `Transformf` / `Transformd`
- quaternion axis-angle, vector rotation, matrix conversion, inverse,
  `nlerp` / `slerp`
- `Ray`, `Plane`, `AABB`, `Sphere`, `Triangle`, `Frustum`
- ray/plane, ray/sphere, ray/triangle intersection helpers
- barycentric coordinates, triangle normal / centroid, frustum extraction
- `std::format` / log-friendly formatting for vectors, matrices, quaternions,
  transforms, and primitive geometry
- Visual Studio Natvis views for the same core math types

## Current transform policy

`Transform<T>` currently means **rigid transform**:

- translation: `Vec3<T>`
- rotation: `Quat<T>`
- no embedded non-uniform scale yet

This is deliberate. It keeps composition and inversion unambiguous for the
robotics/simulation path while still giving graphics a clean bridge through
matrix conversion.

## Primitive geometry policy

The current primitive layer is deliberately **fast and foundational**, not yet a
full computational-geometry module.

- intersection helpers are branch-light and future SIMD-friendly
- triangle ray tests use Moller-Trumbore
- frustum extraction follows the standard clip-plane extraction path from a
  column-major view-projection matrix
- barycentric coordinates are present now because later geometry algorithms will
  build on them directly

## Long-term direction

`crd-math` is planned to grow beyond transform math:

- SIMD acceleration after scalar semantics are locked down
- dense numerical routines (factorisations, linear solves, least squares)
- sparse matrix assembly + iterative solvers
- robust computational geometry in later phases

That means the module is intentionally being built as engine-owned core math,
not as a thin adapter over any graphics or physics SDK.
