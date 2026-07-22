# 2026-07-21 — B19-c2a: TSDF fusion (the first half of the mesh bridge)

**Detour:** D-007 GPU-program-system · **Slice:** B19 (3D Gaussian Splatting) · **Sub-slice:** B19-c2a
**Directive:** "let's continue" (→ the mesh bridge: turn the 2DGS depth+normal G-buffer into geometry).

## What shipped

The mesh bridge's gold-standard route (the one the 2DGS paper uses) is **TSDF fusion → marching cubes**. This
sub-slice lands the first half: fuse posed depth maps into a Truncated Signed Distance Field whose zero-level set is
the surface. New file `engine/kir/include/crd/kir/ckir_mesh.hpp` (namespace `crd::kir::mesh`, **source-agnostic** —
the depth map can come from the 2DGS render, an RGBD sensor, any depth pass):

1. **`build_surface_depth_kernel`** — the splat render's 8-float G-buffer `[R G B T · depthSum · N]` → the surface
   depth `depthSum/(1−T)` per pixel (0 where `T≈1`, i.e. no surface ⇒ the TSDF skips it).
2. **`build_tsdf_integrate_kernel`** — one thread per voxel; integrate ONE posed depth map into `(tsdf_sum, w_sum)`.
   Project the voxel centre into the view, `sdf = d_obs − voxel_view_z`, truncate to `[−1,1]` by μ, accumulate a
   running weighted average via RMW. Skips out-of-image / no-surface / behind-by-more-than-μ voxels. Called once per
   view; multi-view fusion is repeated dispatches into the same accumulators.

**Sign convention:** `sdf = d_obs − depth(voxel)` — a voxel in free space (closer than the surface) is positive, one
behind (occluded, within μ) is negative, the surface is the zero crossing. Voxels farther behind than μ are
unobserved by that view (weight 0).

## Gates

| gate | result |
|---|---|
| CPU oracle: plane depth map → fused field == exact truncated SDF ramp | <1e-4 per voxel |
| CPU oracle: **zero crossing at the surface** | <1e-3 |
| CPU oracle: unobserved voxels (behind > μ) have weight 0; multi-view accumulate | exact |
| CPU oracle: **end-to-end 2DGS render → surface depth → TSDF crosses zero at the surfel plane** (world z=0) | <2e-2 |
| `crd-kir-tests [mesh]` (2 cases) | 31 assertions — PASS |
| **real Vulkan** `[mesh]`: GPU TSDF == oracle | **bit-exact, worst 0.000e+00**, 768 observed voxels |

## Notes / traps

- **RMW accumulators, per-voxel ownership.** Each voxel is one thread and owns its `(tsdf_sum, w_sum)` slots, so the
  running average via read-old → add → write is race-free without atomics; the loads are materialised before the
  stores (the inline-buffer-load read-after-write discipline). Multi-view = repeated dispatches over the same buffers.
- **Divide-safe / clamped indexing.** The projected pixel index is clamped for a safe inline load and masked out when
  out of image; μ is floored to avoid a zero divide. No NaN reaches the accumulators.
- The TSDF kernel emitted to SPIR-V first try (straight-line, no `If`/`For`) — GPU bit-exact vs the oracle.

## State

B19-c2a DONE. `ckir_mesh.hpp` + `test_ckir_mesh.cpp` + the Vulkan `[mesh]` gate tidy-clean (pinned LLVM 20.1.8).
context.md + detour updated; the 2DGS recipe's mesh-bridge section notes TSDF landed.

**Next:** B19-c2b — **marching cubes**: the fused SDF grid → a triangle mesh (the 256-case edge/triangle tables,
per-cell triangle count → scan → emit, reusing the B19-a4 count/scan/compaction pattern), producing the actual mesh
that enters the B1 material pipeline. Then StopThePop per-pixel resort, and the cross-backend DX12/HLSL pass.
