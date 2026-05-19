# 2026-05-18 — Phase 3.1.7 v9b: GPU BVH refit at sub-1ms / 1M

## What shipped

`LbvhGpuPipeline::dispatch_refit_lbvh(RefitInputs)` — per-frame refit API for dynamic-body broadphases. Given the existing fat-node BVH (topology preserved from a prior `dispatch_build_lbvh_from_gpu` call) and a new GPU-resident `leaf_aabbs` buffer, recompute all internal-node bounds in-place. Returns the same `GpuResidentTree` handle with `nodes_gpu` bounds updated.

## Perf

| Path | Median (1M / RTX 4070 Ti SUPER, win-shipping) |
|---|---|
| `dispatch_build_lbvh_from_gpu` | ~1.45 ms |
| **`dispatch_refit_lbvh`** | **0.98-1.09 ms** (5 runs: 1.024 / 1.093 / 0.996 / 0.990 / 0.979) |

Refit is ~30% faster than a full GPU-inputs build. The saved work is the build kernel (Karras tree construction, ~0.3-0.5 ms) and the prim_indices extract kernel (~0.05 ms). Remaining ~1 ms = `done_gpu` reset + upsweep kernel + cmd-buffer overhead.

**At 60 FPS, refitting 1M dynamic bodies costs ~6% of the frame budget** — eylem-broadphase-ready.

## The algorithmic trick

The existing `lbvh_fat_upsweep.comp` kernel is *literally reusable* for refit. Why:

1. Each leaf-thread reads the new leaf AABB from `leaf_aabbs_gpu` (the only thing that changed between build and refit).
2. The carry-register walk's `store_slot()` writes are **unconditional** (not unions) — stale bounds get clobbered.
3. The second-arriver `load_slot()` reads the *first arriver's just-written* bounds, never stale ones.
4. Topology (`parent_idx`, `left_idx`, `right_idx`, `leaf_parents`) is untouched by the upsweep — only the `bounds[0/1]` slots get rewritten.

So refit is: reset `done_gpu` to zero, bind the upsweep kernel with the new `leaf_aabbs`, dispatch. No new shaders needed.

## API

```cpp
struct RefitInputs {
    rhi::Buffer* sorted_pairs;  // SAME buffer + contents as the prior build
    rhi::Buffer* leaf_aabbs;    // NEW values, original prim-index order
    u32 n;                       // MUST match prior build's n
};

GpuResidentTree dispatch_refit_lbvh(const RefitInputs& inputs) noexcept;
```

Consumer's contract: keep `sorted_pairs` alive across the build → refit cycle as part of their scene representation. The pipeline preserves `nodes_gpu` + `leaf_parents_gpu` + cached cmd-buffer/fence/descriptor-allocator internally.

## Test corpus

Two cases / ~24 assertions:

1. **Byte-identical-to-fresh-build verification** — 4096 random AABBs, build via GPU-inputs path, perturb each AABB by ±0.05 + 0.5-1.0× size scale, refit, readback nodes, `memcmp` against a fresh CPU `build_lbvh_cpu` with the new AABBs. **Must be byte-identical** (same `sorted_pairs` + same leaf_aabbs through the same upsweep kernel → same output).
2. **1M perf budget** — `CRD_PERF_BUDGET_LE 60 ms NDEBUG` (3× parallel-DoD inflation over the sub-1ms target).

ValidationCapture silent on every dispatch. Full LBVH test corpus now **16 cases / 402K assertions** (was 14/402K — added 2 refit cases).

## 5-config DoD

PASS in 39 seconds via `scripts/per-slice-check.ps1 -IncludeRelease -Parallel` (debug + asan + shipping + release + tidy).

## Files

- `engine/geometry-bvh-gpu/include/crd/geometry/bvh_gpu/lbvh.hpp` — added `RefitInputs` struct + `dispatch_refit_lbvh` declaration
- `engine/geometry-bvh-gpu/src/lbvh_gpu.cpp` — added `dispatch_refit_lbvh` implementation (~80 LOC; pure helper reuse)
- `tests/geometry-bvh-gpu/test_lbvh.cpp` — added 2 `[refit]` test cases

No new shaders. No new pipelines. Pure API + dispatch addition over the existing v9a-c-gpu-inputs infrastructure.

## Engineering lesson reinforced

The "reference-implementations-are-the-floor" rule (saved this session to memory `feedback_reference_implementations_are_the_floor.md`) compounds with Lesson 10 ("API shape sets the perf floor"): once we got the API shape right (GPU-resident inputs), the **same kernel** that runs at 1.45 ms for build runs at ~1.0 ms for refit. No magic, no algorithm tweaks — just stripping the work the build does that refit doesn't need (Karras tree-build + prim-indices extract). Bandwidth-bound substrate stays bandwidth-bound; we cash out what we don't have to do.
