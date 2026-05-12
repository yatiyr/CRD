# crd-geometry-primitives

> First sub-module of **crd-geometry** (ADR-0076 §1). Primitive shape types +
> closest-point / intersection / containment helpers — the leaf substrate every
> later geometry sub-module (`-bvh`, `-convex`, `-mesh`, …) and every consumer
> (eylem, sdf, renderer, scene) builds on.

## Status

| Slice | What | State |
|---|---|---|
| v0a | Module skeleton + v0 type catalogue (`Line`/`Segment`/`Ray`/`Plane`/`AABB`/`OBB`/`Sphere`/`Capsule`/`Triangle3`/`Frustum`) + **the `crd::math::geometry` move-and-delete** (ADR-0076 §13): the old `Ray`/`Plane`/`Sphere`/`AABB`/`Triangle`/`Frustum` + ~16 helpers + their formatters migrated here as `crd::geometry::primitives::*`; `crd/math/geometry.hpp` deleted; the 9 consumers (`crd-scene` query/world, `crd-math` umbrella + format, `tests/math` + `tests/bench` + `tests/scene`, `runtime/examples/smoke_math.cpp`) repointed; `crd-math` thereafter ships only Vec/Mat/Quat/Transform/SIMD/`deterministic`. | ✅ 2026-05-12 |
| v0b | closest-point catalogue (point → line/segment/ray/plane/triangle/AABB/OBB/sphere/capsule) — incl. Ericson Voronoi-region closest-point-on-`Triangle3` | ⏳ next |
| v0c | intersection tests, everything-vs-everything (ray-{plane,triangle,AABB-slab,OBB,sphere,capsule}, AABB-triangle 13-axis SAT, OBB-OBB 15-axis SAT, tri-tri, sphere-X, capsule-capsule, frustum-X) | ⏳ |
| v0d | barycentric / tetrahedron utilities (Ericson §3.4) | ⏳ |
| v0e | iq smin/domain-op formulary substrate + the shader-helpers cooker-generator skeleton + `crd::math::simd::reduce_argmax_with_lex_tiebreak` | ⏳ |
| v0f | cutting-edge / branchless / SIMD intersection corpus (watertight ray-tri Woop 2013, Baldwin-Weber 2016, branchless NaN-safe slab ray-AABB, Ize 2013 `RayPacket`, Plücker edge classification, `Vec4f`/`Vec8f` batch kernels) — `intersect_ray_triangle` here becomes its cross-check reference | ⏳ |

Then v1+ are the other crd-geometry sub-modules (`-bvh`, `-convex`, …) — separate libraries, see `docs/phases/phase-3.1.7-geometry.md`.

## What you get today (v0a)

`#include <crd/geometry/primitives/primitives.hpp>` — all types + the migrated
helpers. `#include <crd/geometry/primitives/format.hpp>` — `std::format` support.
Namespace `crd::geometry::primitives`. Link `crd-geometry-primitives` (it pulls
`crd-core` + `crd-math` PUBLIC).

Types (all 3D, templated on `T : crd::math::MathScalar`):
`Line`, `Segment`, `Ray` (linear primitives), `Plane`, `AABB`, `OBB`, `Sphere`,
`Capsule`, `Triangle3`, `Frustum` — plus `Point3<T>` (= `Vec3<T>`, for call-site
readability) and `Linef`/`Rayf`/`AABBf`/`Triangle3f`/… aliases.

Helpers migrated from `crd::math::geometry` (the v0/v0b basics; v0b–v0f extend):
`point_at`, `try_normalize`/`normalized`/`plane_from_point_normal`/`signed_distance`/`closest_point(Plane)`,
`center`/`extents`/`contains(AABB)`/`closest_point(AABB)`/`intersects(AABB,AABB)`/`positive_vertex`,
`contains(Sphere)`/`intersects(Sphere,Sphere)`/`intersects(AABB,Sphere)`,
`centroid`/`normal`/`barycentric`/`contains(Triangle3)`,
`intersect_ray_plane`/`intersect_ray_sphere`/`intersect_ray_triangle` (Möller-Trumbore),
`frustum_from_view_projection`/`contains(Frustum)`/`intersects(Frustum,Sphere|AABB)`.

```cpp
using namespace crd::geometry::primitives;
const Triangle3f tri(Vec3f(-1,-1,0), Vec3f(1,-1,0), Vec3f(0,1,0));
float t = 0.0F; Vec3f bary{};
if (intersect_ray_triangle(Rayf(Vec3f(0,0,-5), Vec3f(0,0,1)), tri, t, bary)) { /* ... */ }
```

## Naming rule (pin — read before adding a type)

Primitives here are **3D** and carry **no dimension suffix unless a 2D peer is
planned**. `Triangle3` carries the `3` because `crd-geometry-polygon` (v6) will
add `Triangle2`; `Ray`/`Line`/`Segment`/`Plane`/`AABB`/`OBB`/`Sphere`/`Capsule`/
`Frustum` have no 2D peer on the roadmap, so no suffix. If a 2D `Ray`/`AABB`/etc.
ever lands, revisit the whole set holistically rather than spot-renaming. (This
asymmetry is intentional and matches ADR-0076 §1's literal type list.)

## API layers

Two-layer per ADR-0076 §5 (mirrors `crd-hesap`):
- **Typed C++ "Eigen-class" layer** — what v0a ships: zero-overhead inlined
  templates, data-oriented (consumers pass `ConstSpan` of vertex/index data,
  never `Mesh*` objects; functional form `bvh_build(...)` not `BvhTree::build`).
  This is what eylem / sdf / renderer / scene call.
- **Opt-in cooker/editor handle-based façade** — reserved for later sub-slices;
  nothing in v0a forbids it.

## Determinism

Inherits the ADR-0063 contract (ADR-0076 §4): no `std::sin/cos/tan/exp/log/pow`
in this module (the `crd-no-std-math-check` CI guard now scopes
`engine/geometry-primitives`); `std::sqrt` is allowed (IEEE-754 mandates a
correctly-rounded single-rounding sqrt everywhere). Algorithm-specific tiebreaks
(GJK simplex Ericson-not-vandenBergen, SAH-split X-then-Y-then-Z, Quickhull lex
order, watertight ray-tri axis selection, Plücker sign-zero) are pinned in
ADR-0076 §4 and land with their algorithms in v0c–v0f / v2+.

## References

- `docs/decisions/0076-geometry-substrate-architecture.md` — the architecture (§1 sub-modules, §3/§5 API, §4 determinism, §13 the move-and-delete + the v0f corpus)
- `docs/phases/phase-3.1.7-geometry.md` — the 30-slice phase plan
- `docs/research/cerid-geometry.md` + `docs/research/cerid-geometry-supplement.md` — the research dossiers
