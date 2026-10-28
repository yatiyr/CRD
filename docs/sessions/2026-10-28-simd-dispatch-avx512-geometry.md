# 2026-10-28 — SIMD-DISPATCH: the geometry zmm bodies

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). HEAD before: `4077a236`. CI read for this batch:
[tensor zmm bodies](2026-10-28-simd-dispatch-avx512-tensor.md#ci-first).

## The bodies

- **Primitives** (`primitive_kernels.cpp`): each batch kernel is written once over a register type. A run hoists its
  constants and steps one bundle of eight on Vec8f, or two on Vec16f at avx512 (bundle i in the low half, i + 1 in
  the high one, each column two 256-bit loads), with an odd last bundle on Vec8f. The packet test keeps the packet's
  eight rays in both halves against two boxes. The hull support scan takes sixteen vertices a step while they last,
  scanned in index order.
- **Moller-Trumbore** (`detail/batch8_core.hpp`): one template over the register type, shared by both families.
- **Mesh** (`mesh_kernels.cpp`): a leaf runs sixteen triangles a step at avx512 while more than eight are left, then
  eight; the lane decisions stay scalar, in triangle order. The default leaf holds at most four triangles
  (`max_leaf_prims`), so the wider step serves the larger leaves a build may choose.

Every lane is independent and every product and sum rounds on its own, so every level gives the portable bits.

## Tests

- The existing cases already run 0 to 5 bundles a call (odd and even counts, the Vec8f last bundle), padded hull
  scans of 8 to 40 vertices, and mesh leaves of one to many chunks of eight.
- win-debug `[kernels]`: primitives 68 assertions in 5 cases, mesh 25 in 3 (portable, sse2, avx2).
- SDE 10.13.1 `-skx`, `CRD_CPU_LEVEL=avx512`, `[cpu-level-applied]`: primitives 85 in 6, mesh 28 in 4, avx512 included.

## Board candidates (row close, on AVX-512 hardware)

- The two-bundle step against avx2's one, with the all-ones vector masks of the Vec16f surface.
- The sixteen-triangle leaf step at larger `max_leaf_prims`.
