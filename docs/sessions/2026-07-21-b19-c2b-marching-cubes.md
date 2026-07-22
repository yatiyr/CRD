# 2026-07-21 — B19-c2b: marching cubes (the mesh bridge is complete)

**Detour:** D-007 GPU-program-system · **Slice:** B19 (3D Gaussian Splatting) · **Sub-slice:** B19-c2b
**Directive:** "let's go, no debts, no defers, full gold standard."

## What shipped

The second half of the mesh bridge: **marching cubes** (Lorensen & Cline 1987) extracts the SDF=0 isosurface of the
fused TSDF field as a watertight triangle mesh, entirely on-device. With B19-c2a's TSDF fusion, the full chain now
runs: **2DGS surfel render → surface depth → TSDF → marching cubes → triangle mesh**, bridging captured radiance
fields into the B1 mesh/material pipeline.

New/extended files:
- `engine/kir/include/crd/kir/mc_tables.hpp` — the canonical Lorensen/Bourke tables (corner offsets, edge
  endpoints, the 256×16 triangle table). Data only.
- `engine/kir/include/crd/kir/ckir_mesh.hpp` (extended): `build_tsdf_finalize_kernel` (tsdf_sum/w_sum → field,
  unobserved=+1), `build_mc_count_kernel` (per cell → cubeindex from the 8 corner signs → triangle count),
  `build_mc_emit_kernel` (per cell → each valid triangle's 3 edge-crossing vertices + outward face normal), plus
  the `cube_index`/`edge_vertex` helpers.

The variable per-cell output (0–5 triangles) uses the **same count → scan → emit compaction as the B19-a4 splat
bin**: count triangles, exclusive-scan (`ckir_scan`) to per-cell offsets + total, emit at the offset. Only the
256×16 triTable is a runtime buffer (I32, indexed by the per-cell cubeindex); the small corner/edge tables are baked
into the kernels at authoring time. Vertex on edge (a,b) = `P[a] + (f[a]/(f[a]−f[b]))·(P[b]−P[a])`.

## Gates

| gate | result |
|---|---|
| CPU oracle: per-cell triangle count == host MC reference (sphere, 4096 cells) | exact (0 mismatches) |
| CPU oracle: sphere mesh vertices on-surface | worst |r−R| = 0.0104 |
| CPU oracle: total area vs 4πR² | 2.90 vs 3.14 (coarse MC) |
| CPU oracle: **outward face normals** | 140/140 |
| CPU oracle: **vertex-for-vertex == host MC reference** | 0 mismatches |
| `crd-kir-tests [mesh]` (4 cases) | 39 assertions — PASS |
| **real Vulkan** `[mesh]`: full count→scan→emit MC == oracle mesh | **bit-exact, worst 5.96e-08** (140 tris) |

## Traps hit

- **The canonical triTable winds INWARD.** For the `inside = field<iso` convention, `cross(v1−v0,v2−v0)` on the
  standard table points toward the interior — the sphere came out **0/140 outward** until the winding was reversed
  (emit v0,v2,v1) and the normal negated. Crucially, the **vertex-for-vertex host comparison did NOT catch this**
  (both host and kernel use the same table order): vertex *positions* are independent of winding. The independent
  geometric checks — total area ≈ 4πR² and every face normal · centroid > 0 — are what exposed it. Lesson recorded:
  gate connectivity with a surface metric, not a self-comparison.
- **Typed GPU buffers.** The kernel reads triTable as I32 and edge/corner tables as U32; the GPU buffer is raw
  bytes, so `dispatch_kernel_1wg` (float uploads) would corrupt them — the Vulkan gate uses a manual typed dispatch
  (integer bytes for integer buffers). The CPU oracle is forgiving (f64 + declared dtype); only the real backend is
  strict.
- **Scan single-pass sizing.** `build_scan` is single-pass only when `n ≤ threads·8` with `threads | n` — chose
  `ncells = 8³ = 512` with threads 256 so the offset scan is one dispatch.
- The emit kernel (an `If` per triangle + the edge-interpolation helper + cross-product normal) emitted to SPIR-V
  first try — the If-shared-temp discipline (materialise the shared u32 index bases, never the Bool guards) held.

## State

B19-c2b DONE — the mesh bridge is complete. `ckir_mesh.hpp`, `mc_tables.hpp`, `test_ckir_mesh.cpp`, and the Vulkan
`[mesh]` gates tidy-clean (pinned LLVM 20.1.8). New recipe
`docs/recipes/2026-07-21-mesh-extraction-tsdf-marching-cubes.md`; context.md + detour + the 2DGS recipe updated.

**Next (remaining B19):** B19-d compression (Self-Organizing Gaussians / HAC, 20–40×) · StopThePop per-pixel resort
· B19-e relightable/ray-traced (on C3/B9) · B19-f differentiable training (v16 AD) + LoD · the cross-backend
DX12/HLSL pass (B19 is Vulkan-only so far).
