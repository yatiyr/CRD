# 2026-10-27 — SIMD-DISPATCH: the geometry kernels

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). Decision: [ADR-0134](../decisions/0134-runtime-cpu-dispatch.md).

## CI read

- No completed run has a new failed job. Complete run 40515770381 (`31e2b9ce`) is still running; its four failed jobs
  were fixed by `b306e2ea` and `14840123`. Push runs 40554714493 back to 40521536075 were cancelled with no failed job;
  40556024496 (`ce4c5148`) is pending. Nothing dispatched.
- Needs CI audit: [DIAG.11c](../ROADMAP.md#slice-diag.11c) and [DIAG.12](../ROADMAP.md#slice-diag.12) stay Needs CI;
  no complete run at or after `14840123` has finished.

## Per-site decisions

The rule is ADR-0134 §5: one table call per routine, never per microkernel, and a kernel unit compiles only
level-namespace inline code and plain data.

- **Batch intersection kernels** (`simd_batch.hpp`, no engine caller): the public bundles held `Vec8f`, a type of the
  level namespace, so no kernel unit could even take them. They are now plain columns (`primitive_kernels.hpp`: `Aabb8`,
  `Sphere8`, `Triangle38`, `Segment38Pair`, `RayPacket8`, `u32` masks, `Lanes8`), and each public function takes a
  span of bundles and makes one call (the packet kernel: one packet against N boxes).
- **Hull support scan** (`support_simd_f32`): already the whole scan; one call per support query, signature kept.
- **Mesh raycast** (`mesh_raycast_simd`): the routine is the whole traversal; one call per ray. The kernel reads the
  vertices and `BvhNode`s as bytes by `memcpy` at strides and offsets the wrapper's static asserts tie to the types,
  so no `Vec3`, `AABB3`, `BvhNode` or span code reaches the unit.
- **BVH4 traversal** (`bvh4_simd.*`): stays general code. `Vec4f` is one 128-bit register on every x64 level from SSE2
  up, so a per-level unit would compile the same operations VEX-encoded; the row-close board checks it.
- **Morton radix sort**: only an `_mm_prefetch` under `CRD_SIMD_HAS_SSE2`, inside the floor; nothing to do.

## The families

- `crd_add_cpu_kernels(crd-geometry-primitives NAME primitives LEVELS portable sse2 avx2)`: the six batch kernels and
  the hull scan; `crd_add_cpu_kernels(crd-geometry-mesh NAME mesh LEVELS portable sse2 avx2)`: the raycast.
- Shared level-namespace cores: `detail/batch8_core.hpp` (Moller-Trumbore on eight lanes, used by both units) and
  `detail/slab_core.hpp` (the robust slab test on plain components). `intersect_ray_aabb_robust` now calls the slab
  core, so general code and the mesh unit run one source.
- Declaration: both families are bit-exact across levels, portable the reference (two roundings a step, the IEEE
  min/max ordering, scalar lane decisions), NaN payloads aside.
- Deleted: the dead `force_link_geometry_primitives` anchor; the stale AVX2 and `crd-simd-emission-check` comments.
- Include budget: no entry. `primitives.hpp` reaches `<immintrin.h>` through `crd/math/mat.hpp`'s general SIMD
  `Mat4`, which every consumer already takes; the new data headers include only `crd/core/types.hpp`.

Verification: `test_primitive_kernels.cpp` and `test_mesh_kernels.cpp` ([kernels], with
`crd-geometry-{primitives,mesh}-kernels-at-<level>`) compare every level the host runs with portable bit for bit over
spans of 0 to 5 bundles with guard words, lanes mixing zeros, signed zeros, subnormals, infinities and NaN, the hull
scan with exact ties, and 2,000 rays (zero direction components, inside and outside, culled, a finite tmax) against a
420-triangle soup with repeated triangles. win-debug: the primitives, mesh, convex, bvh and spatial programs pass;
`llvm-nm` on the six kernel objects shows no code symbol outside the level namespace; the ctest guards pass.
Tests on hosted CI, boards in the row-close pass.
