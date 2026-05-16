# Session log — 2026-05-16 — geometry v4d: `mesh_raycast_simd`

> Fourth slice of Phase 3.1.7 v4 `-mesh` cluster. AVX2 8-wide Möller-Trumbore
> batched ray-triangle test at BVH leaves, alongside the v4b Woop watertight
> default. SIMD-batched ALU + scalar lane-scan for masking.

## Scope landed

| Element | Path |
|---|---|
| SIMD raycast header | `engine/geometry-mesh/include/crd/geometry/mesh/mesh_raycast_simd.hpp` |
| SIMD raycast impl   | `engine/geometry-mesh/src/mesh_raycast_simd.cpp` |
| Umbrella header     | extended `mesh.hpp` |
| Tests               | `tests/geometry-mesh/test_mesh_raycast_simd.cpp` + CMakeLists |

## API

```cpp
[[nodiscard]] std::optional<MeshRayHit>
mesh_raycast_simd(const TriangleMeshViewf&, const TriangleMeshBvh&,
                  const Ray3<f32>& ray,
                  f32 tmax = inf, bool cull_back = false) noexcept;
```

Same `MeshRayHit{t, MeshHitPayload{tri, bary}}` return as v4b — drop-in
swap at call sites that want the SIMD fast path.

## Algorithm

Same BVH traversal as v4b — Williams/Ize precomputed slab + ordered
nearer-first descent + `best_t` pruning. Differs only at the leaf inner
loop:

1. **Chunk the leaf's triangles into groups of ≤ 8.** Pad-replicate the
   last valid tri to fill (the lane scan ignores padded lanes by
   `lane < valid_count`).
2. **Gather → SoA.** Per chunk: read 8 prim indices from `bvh.tree.prim_indices()`,
   look up v0/v1/v2 via `view.indices` → `view.vertices`, transpose into
   9 × `Vec8f` registers (v0x/y/z, v1x/y/z, v2x/y/z).
3. **SIMD MT.** Compute edge1/edge2/pvec/det/inv_det/tvec/u/qvec/v/t —
   ~30 FMAs across the 8 lanes. Output is 4 × `Vec8f` (det, u, v, t).
4. **Scalar lane scan.** For each lane: apply the masking decisions
   (cull_back, |det|>ε, 0≤u≤1, 0≤v, u+v≤1, 0≤t≤best_t). Update
   `(best_t, best_tri, best_bary)` on improvement; lowest-triangle-index
   tiebreak on equal t per ADR-0076 §4 pin #11.

## Lesson learned mid-implementation

**First draft used SIMD-mask AND via `min(mask_gt, mask_lt)`** —
combining `_CMP_*` result registers (all-bits NaN on true, zero on
false) with `_mm256_min_ps`. That's implementation-defined for NaN
inputs; the cull bit silently disappeared and `cull_back=true` failed
to cull. **Fix:** keep the heavy ALU in SIMD, do the masking decisions
in a scalar lane scan. Per-lane branching is trivially cheap compared
to the 30-FMA inner ALU, and avoids the NaN-encoded-mask hazard
entirely. Pattern recorded as `feedback_simd_mask_via_scalar_scan.md`
candidate (didn't write a memory; logged here).

## Determinism vs v4b

MT uses a strict-sign det test, not Woop's watertight exact-edge
predicate promotion. Consequence:

- **Rays passing EXACTLY through a shared edge** may hit neither
  triangle (MT) where Woop hits both. The lowest-tri-index tiebreak
  picks deterministically when either lane reports a hit.
- **On tessellated curved meshes (coarse sphere, cube edges)**, MT
  and Woop disagree on which-triangle at near-edge rays. They report
  different `tri` indices and possibly different `t` (when picking
  front vs back face of a thin polyhedron). Both hits are legitimate;
  the inherent algorithmic divergence is documented and the
  cross-validation test tolerates up to 6 divergent rays per 36-ray
  corpus.
- **On bulk-triangle interior hits** (the common case), MT and Woop
  agree on tri index and `t` within 1-2 ULPs.

## Test corpus

7 cases / 15 assertions:

1. Empty mesh → nullopt.
2. Unit cube +X-face hit — SIMD `t` matches Woop within 1e-4.
3. Single triangle hit at t=1.
4. Miss — ray parallel above the cube.
5. `tmax` cull — 4.0 misses, 5.0 hits.
6. `cull_back` semantics — inside-out ray returns hit without
   cull-back, nullopt with cull-back.
7. **Sphere corpus (36 rays)** — cross-validates MT vs Woop on a
   6-lats × 6-lons tessellated sphere (72 triangles). Both
   algorithms either hit or miss on ≥30 rays; up to 6 edge-case
   divergences accepted (MT vs Woop differ on which triangle the
   ray picks).

## Decisions locked

- **Möller-Trumbore at v4d, not SIMD-Woop.** Original plan called
  for MT; sticking with it. SIMD-Woop is theoretically watertight
  but the per-edge `double` recompute path is lane-divergent
  (defeats SIMD). MT is the textbook AVX2 fast path; Embree, PBRT
  use the same. Precision difference at exact edges is documented
  and the v4b Woop path remains the watertight reference.
- **Scalar lane scan, not SIMD mask AND.** See "Lesson learned"
  above — implementation-defined behaviour of `min`/`max` on
  NaN-encoded mask registers makes the SIMD-mask path fragile.
  Cost: 8 conditional branches per chunk vs ~30 FMAs of SIMD ALU
  — branches are negligible.
- **Gather-then-batch at query time, not BVH layout repack.**
  v4d-base uses 24 indirect AoS Vec3 reads per 8-tri chunk +
  manual transpose. v4d-fast (follow-on) could repack BVH leaves
  into SoA at build time — saves the gather + transpose, costs
  build time + memory. Defer until perf measurements justify.
- **Last-tri replication for partial chunks.** Tail chunk of e.g.
  3 triangles → pad lanes 3-7 with tri 0's data. Lane scan ignores
  them via `lane < valid_count`. Deterministic; no NaN-poisoning.
- **No 4-wide fallback for AVX-less platforms.** `Vec8f` shim already
  decomposes to 2× `Vec4f` on non-AVX2 paths; the algorithm runs
  correctly via the shim. Same code path everywhere.

## 5-config DoD

| Config | Build | CTest |
|---|---|---|
| win-debug | clean | **1942/1942** (+7 from v4d) |
| win-asan | clean | 1942/1942 |
| win-shipping | clean | 1855/1855 |
| win-shipping-profile | clean | 1937/1937 |
| win-tidy | clean | — |

Full project ctest 1935 (v4c close) → **1942** after v4d.

## Open follow-ups inside v4

- **v4d-fast: per-leaf SoA repack** at BVH build time. Save the
  gather + transpose cost per query at the price of build time +
  memory. Defer until perf benchmarks (v4-bench, post-v4-close).
- **v4-validate: formal mesh validation pipeline stage** —
  manifoldness, orientation, area-zero, vertex-duplication,
  edge-non-manifold.
- **v4-close: ADR-0076 §17 amendment + `docs/systems/geometry-mesh.md`
  + 18-config full sweep + ONE sandbox-viz session demonstrating
  closest_point + raycast(woop) + raycast(simd) + winding all on
  the same picked mesh.**

## References

- ADR-0076 §4 pin #11 — determinism tiebreak.
- ADR-0078 §5 D27/D32-D36 — two-layer typing; SIMD layer always raw.
- Tomas Möller, Ben Trumbore, "Fast, Minimum Storage Ray-Triangle
  Intersection" (Journal of Graphics Tools, 1997). The MT test.
- Ingo Wald et al., Embree — the canonical AVX2-batched MT reference.
- `engine/geometry-bvh/src/bvh4_simd.cpp` — the SoA `Vec4f` per-axis
  pattern we mirror for `Vec8f`.
- `engine/math/include/crd/math/simd/vec8f.hpp` — the SIMD substrate.
- Preceding session logs: v4a closest-point, v4b raycast (Woop), v4c winding.
