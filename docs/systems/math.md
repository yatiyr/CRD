# crd-math

Math substrate for graphics, simulation, robotics, and later numerical work.

## Status

| Slice | Ships | Status |
| --- | --- | --- |
| v1a | scalar helpers, `Vec2/3/4`, `f32/f64` aliases | 🚧 |
| v1b | `Mat2/3/4` (column-major) | ⏳ |
| v1c | `Quat`, `Transform` | ⏳ |
| v1d | `Ray`, `Plane`, `AABB`, `Sphere`, `Triangle`, `Frustum` | ⏳ |

## Core decisions

- Column-major matrices
- Column vectors, so multiplication is `Mat * Vec`
- Radians-first API
- Public API supports both `float` and `double`
- Scalar-first implementation, SIMD-ready layout and benchmarks from day one
- Quaternion convention: Hamilton, stored as `xyzw`, identity = `(0, 0, 0, 1)`

## Long-term direction

`crd-math` is planned to grow beyond transform math:

- SIMD acceleration after scalar semantics are locked down
- dense numerical routines (factorisations, linear solves, least squares)
- sparse matrix assembly + iterative solvers
- robust computational geometry in later phases

That means the module is intentionally being built as engine-owned core math,
not as a thin adapter over any graphics or physics SDK.
