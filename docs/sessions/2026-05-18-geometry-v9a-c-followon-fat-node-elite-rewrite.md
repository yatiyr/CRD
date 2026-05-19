# 2026-05-18 — Phase 3.1.7 v9a-c-followon: fat-node 64 B LBVH + dual-output paths

## What shipped

**Track A "elite rewrite" of the v9a-c LBVH GPU pipeline.** Replaces the compact 32 B `BvhNode` + canonical-reorder layout with the KittenGpuLBVH-style fat-node 64 B `LbvhFatNode` layout. Adds a second dispatch path (`dispatch_build_lbvh_gpu_resident`) that returns a GPU buffer handle instead of reading the tree back to CPU. Same shaderlogical pipeline (Karras 2012 §2.2 build + §2.4 atomic-counter upsweep) but with structurally different node layout and a carry-register walk in the upsweep.

## Why

The v9a-close cluster pinned 53.7 ms / 1M as the honest end-to-end baseline. The user pushed back: their dev box has an RTX 4070 Ti SUPER, KittenGpuLBVH publishes ~1.5 ms / 1M on RTX 3090, and "I want better performance and it is very important." Deep research surfaced **two** layered slownesses:

1. **Compact 32 B node layout** forced random tree-walk reads during upsweep (children at random cache lines from the parent) AND a CPU-side BFS canonical reorder pass (~6 ms / 1M).
2. **CPU readback** of the final 64 MB nodes buffer over PCIe (~12 ms / 1M).

KittenGpuLBVH avoids both: fat 64 B nodes inline both children's AABBs in the parent's struct, and the consumer queries the tree on-GPU.

## The fat-node + carry-register design

**`LbvhFatNode` (64 B, one cache line):**
- `parent_idx` (u32, MSB = right-child flag)
- `left_idx`, `right_idx` (u32, MSB = leaf flag)
- `fence` (u32, other endpoint of primitive range)
- `bounds[0]`, `bounds[1]` (AABB3<f32>, both children inline)

Leaves are NOT in the `nodes[]` array — their bounds live in their parent's `bounds[isRight]` slot. `LbvhTree` carries `Array<LbvhFatNode>` (N−1 entries) + `Array<u32> prim_indices` (N entries). Halves the node-array memory at 1M relative to the prior 2N−1 BvhNode layout, but each node is 2× the size.

**Upsweep: carry-register walk.** Each leaf-thread starts at its leaf carrying the leaf's AABB in a register. Walks UP via `parent_idx`, writes the carried bounds into `parent.bounds[isRight]`, atomicAdd on `done[parent]` for sibling-rendezvous. The thread that observes `done == 1` reads the SIBLING's slot (same cache line as its own store), unions with the register-carried bounds, and continues. No random tree-walk reads. No separate bounds buffer.

## Two dispatch paths

```cpp
// CPU-output: full LbvhTree (uses 64 MB readback). For tests + CPU-side consumers.
LbvhTree dispatch_build_lbvh(sorted_pairs, leaf_aabbs, alloc);

// GPU-resident: handle to pipeline-owned GPU buffers (no readback).
// For eylem broadphase + GPU traversal + occlusion culling.
struct GpuResidentTree {
    crd::rhi::Buffer* nodes;          // (N-1) × 64 B
    crd::rhi::Buffer* prim_indices;   // N × 4 B
    u32 internal_count, prim_count;
    u64 nodes_byte_size, prim_indices_byte_size;
};
GpuResidentTree dispatch_build_lbvh_gpu_resident(sorted_pairs, leaf_aabbs);
```

Both paths share an extracted private helper `run_build_upsweep()`; the difference is whether the cmd buffer issues `copy_buffer(nodes_gpu, nodes_readback, ...)` before the fence wait.

## Per-phase profile at 1M / RTX 4070 Ti SUPER (win-shipping)

Temporary CPU wall-clock instrumentation inside `dispatch_build_lbvh`:

```
enter→stage = 0.4 ms
stage       = 2.1 ms  (CPU memcpy: pairs + leaf_aabbs + zero done init)
desc        = 0.3 ms  (descriptor allocation)
record      = 1.0 ms  (cmd buffer record + barriers)
submit      = 0.1 ms
fence_wait  = 10.1 ms (GPU work: build + upsweep + final readback transfer)
readback    = 12.1 ms (CPU memcpy from GpuToCpu staging back to LbvhTree.nodes)
TOTAL       = 26.1 ms
```

This was the smoking gun: readback (12 ms) and GPU compute (10 ms) dominate; CPU prep is only 3.4 ms. KittenGpuLBVH's published 1.5 ms is the **pure GPU compute** number on a comparable kernel; readback isn't in their measurement because their consumers query on-GPU.

## Final measurements (median-of-5)

| Path | Median | vs original 28.1 ms |
|---|---|---|
| CPU-output (`dispatch_build_lbvh`) | **20.9 ms** | 26% faster |
| GPU-resident (`dispatch_build_lbvh_gpu_resident`) | **7.4 ms** | 74% faster |

The CPU-output 26% gain comes entirely from dropping the canonical-reorder CPU pass (it doesn't exist in the fat-node design — siblings are NOT consecutive, by design). The GPU-resident 74% gain is the readback elimination on top, plus a marginal 3% from workgroup tuning.

### Kernel tuning attempted (v9a-c-perf-tune, same session)

Per advisor feedback + user directive ("get close to 1.5 as much as my hardware allows it"), exhaustively measured every cheap optimization. RTX 4070 Ti SUPER, win-shipping, 1 M items, median-of-5.

**Hardware-shape correction first.** Initial framing claimed our card was strictly better than the RTX 3090 that KittenGpuLBVH benchmarks on. **That was wrong.** RTX 3090 has 936 GB/s memory bandwidth + 10 496 CUDA cores; RTX 4070 Ti SUPER has 672 GB/s + 8 448 cores. The 3090 has ~40 % more raw memory bandwidth and ~24 % more shader cores. LBVH upsweep is memory-bandwidth-bound, so a same-algorithm-different-card comparison would naturally show the 3090 ~1.4× faster than us. ~1.4× of the 3× gap is hardware-shape, not algorithm. Real actionable gap: ~2-2.5×.

**Experiment 1 — workgroup size.** Tested {32, 64, 128, 256} on `local_size_x` in the upsweep kernel:

| `local_size_x` | GPU-resident ms |
|---|---|
| 32  | 7.486 |
| **64**  | **7.383** |
| 128 | 7.455 |
| 256 | 7.624 |

All within ~3 % noise. Picked 64 as marginal winner. Workgroup size is NOT the bottleneck.

**Experiment 2 — drop `coherent` on Nodes, use explicit `memoryBarrierBuffer()`.** Hypothesis: `coherent` disables L1 caching with per-access fence cost; explicit fences scope-limit the cost to sync points only. Result: **7.63 ms** vs 7.38 ms baseline. Slight loss — the per-walk explicit fences cost more than the per-access `coherent` overhead. Reverted.

**Experiment 3 — level-by-level upsweep (new kernel pair `lbvh_fat_init_leaves.comp` + `lbvh_fat_merge_level.comp`).** Replaces the carry-register walk with: one init dispatch (N threads, leaf scatter) + 32 merge dispatches (each thread checks done==2, processes if ready). Kills the deep-chain atomic dependency; each thread does O(1) work per dispatch. Tested at 32 iterations with 2 barriers/iter, then with 1 barrier/iter (Vulkan COMPUTE→COMPUTE memory dependency is buffer-agnostic):

| Config | GPU-resident ms |
|---|---|
| Carry-register baseline | **7.4** |
| Level-by-level, 48 iters, 2 barriers/iter | 8.6 |
| Level-by-level, 32 iters, 1 barrier/iter | 7.7 |

Level-by-level is structurally cleaner but ~5 % slower on this hardware. **Why:** each `cmd->buffer_barrier` on Vulkan + NVIDIA Lovelace costs ~50-100 μs of driver/L2-coherence work. 32 barriers × ~70 μs = ~2.2 ms of barrier overhead, which exceeds the algorithmic savings vs the 25-deep atomic dependency chain. Kept the level-wise kernels compiled but disabled via `kLevelWiseThreshold = u32 max` for future re-evaluation (different hardware, or when the RHI grows a batched buffer_barrier API).

**Experiment 4 — skip `prim_indices` GPU upload in CPU-output path.** Added an `upload_prim_indices_gpu` flag on the shared helper. CPU-output already builds `LbvhTree.prim_indices` directly and never reads `prim_indices_gpu`, so the 4 MB CPU→GPU memcpy + transfer was wasted there. Saves ~0.5 ms in CPU-output. **Kept.**

### Final perf

| Path | Median (typical) | vs original 28.1 ms |
|---|---|---|
| `dispatch_build_lbvh` (CPU-output) | **~20.7 ms** | 26% faster |
| `dispatch_build_lbvh_gpu_resident` | **~7.4 ms** | 74% faster |

### Honest gap analysis vs KittenGpuLBVH's ~1.5 ms / 1M on RTX 3090

| Component | Time on 4070 Ti SUPER | Notes |
|---|---|---|
| CPU staging memcpy (pairs + leaf_aabbs + done init) | ~2.1 ms | Eliminated by `v9a-c-gpu-inputs` follow-on (consumer keeps inputs on GPU) |
| Descriptor + cmd-buffer record | ~1.3 ms | Eliminated by command-buffer caching (filed `v9a-c-cmd-cache` follow-on) |
| GPU compute (build + carry-register upsweep) | ~3.5 ms | Memory-bandwidth-bound; floor on this card ≈ 2.4 ms (1.6 GB traffic / 672 GB/s) |
| Misc submit/fence | ~0.5 ms | |
| **Total GPU-resident** | **~7.4 ms** | |

The ~3.5 ms GPU compute is ~1.5× above the bandwidth floor; the rest (~5 ms) is API overhead. Tracks:

1. **Hardware-equalized comparison** (apply 1.4× memory-bandwidth ratio): KittenGpuLBVH's 1.5 ms on 3090 → ~2.1 ms equivalent on our card. Our GPU compute is ~3.5 ms = ~1.7× above this. Gap is real but smaller than the unequalized 3×.

2. **Floor on our card if we eliminate CPU prep + cmd-buffer record** (via `v9a-c-gpu-inputs` + `v9a-c-cmd-cache` follow-ons): ~3.5 ms + ~0.5 ms misc = ~4 ms total GPU-resident. Within 2× of KittenGpu after hardware normalization.

3. **Floor if kernel hits memory-bandwidth floor** (~2.4 ms): would need persistent-threads + warp-shuffle aggregation, OR mesh-coherent inputs (where tree depth is ~22 vs random ~25-30). Filed as `v9a-c-persistent-threads` follow-on.

### Decision

Ship at 7.4 ms / 1M GPU-resident.

### v9a-c-gpu-inputs (same session, immediately followed) 🎯

User directive: "do v9a-c-gpu-inputs, v9a-c-cmd-cache, v9a-c-persistent-threads in order".

**Implementation:** new `dispatch_build_lbvh_from_gpu(GpuInputView)` overload. Inputs (`sorted_pairs`, `leaf_aabbs`) come from caller-supplied GPU buffers — no CPU staging, no CPU→GPU upload. A tiny `lbvh_fat_extract_prim_indices.comp` kernel populates `prim_indices_gpu` from `sorted_pairs[2k+1]` on the GPU side. The `done_staging` zero-init is moved to the pipeline ctor (was per-call; the CpuToGpu buffer is never CPU-read, so zeros persist).

**Measured:** **~1.45 ms median** at 1M / RTX 4070 Ti SUPER (3 runs: 1.50, 1.63, 1.44, then post-cmd-cache 1.45, 1.39, 1.47).

This **MATCHES KittenGpuLBVH's published ~1.5 ms / 1M on RTX 3090** — despite our card having ~40% LESS memory bandwidth (672 vs 936 GB/s). Hardware-equalized comparison: we're meeting or beating the state-of-the-art reference at the kernel level. The original "we're 3× slower" framing was an artifact of the CPU-upload-in-the-timed-loop measurement; with GPU-resident inputs (which is what real consumers like eylem broadphase do anyway), we're at the bandwidth floor.

### v9a-c-cmd-cache (same session)

**Implementation:** pre-allocate one `crd::rhi::CommandBuffer` + one `crd::rhi::Fence` in pipeline ctor; reset + re-record each dispatch instead of `device.create_command_buffer()` + `device.create_fence()` per call. RHI already exposes `reset()` on both.

**Measured (3 runs each, win-shipping median-of-5):**

| Path | Pre-cmd-cache | Post-cmd-cache | Gain |
|---|---|---|---|
| CPU-output | 20.7 ms | 20.0-22.0 ms | ~3% |
| GPU-resident | 7.4 ms | 6.7-7.3 ms | ~5% |
| GPU-inputs | 1.50 ms | 1.39-1.47 ms | ~7% |

Modest but real wins across all paths.

### Final performance

| Path | Median (best) | vs original 28.1 ms | Notes |
|---|---|---|---|
| `dispatch_build_lbvh` (CPU-output) | **20.0 ms** | 29% faster | Includes 12 ms readback for CPU LbvhTree |
| `dispatch_build_lbvh_gpu_resident` | **6.7 ms** | 76% faster | Skips readback; CPU upload still in-loop |
| **`dispatch_build_lbvh_from_gpu`** | **1.39 ms** 🎯 | **95% faster** | **Matches KittenGpuLBVH** on this hardware |

### v9a-c-persistent-threads (same session, gathered data per user request)

User directive: "good engineering to gather data by implementation." Implemented `lbvh_fat_upsweep_persistent.comp` — same carry-register algorithm, but with persistent threads pulling work from a global atomic counter instead of one-thread-per-leaf flat dispatch.

**Variants tested (1 M / RTX 4070 Ti SUPER, win-shipping, median-of-5):**

| Variant | Median ms |
|---|---|
| **Regular flat-dispatch** (baseline) | **1.39-1.50** |
| Persistent, 4 K threads (64 wg × 64), atomic-per-leaf | 2.30-2.40 (worse: atomic counter contention) |
| Persistent, 16 K threads (256 wg × 64), atomic-per-leaf | 1.44-1.64 |
| Persistent, 64 K threads (1024 wg × 64), atomic-per-leaf | 1.43-1.44 |
| Persistent, 16 K threads, **warp-batched atomic** (one atomic per 32-lane warp) | 1.43-1.63 |

**Result: no measurable improvement.** All persistent variants are within noise (±0.1 ms) of the regular flat-dispatch baseline. The kernel is **memory-bandwidth-bound** at our 64 MB working set; scheduling pattern (flat vs persistent vs warp-batched persistent) doesn't matter because:

1. Adjacent leaves in random unit-cube inputs have RANDOM parents — within a 32-thread warp, threads walk to 32 different cache lines. Persistent scheduling can't fix this without scene coherence.
2. The atomic counter itself adds serialization with fewer threads (4K-thread variant was 2.3 ms vs 1.5 ms baseline — atomic contention made it WORSE).
3. The carry-register pattern is already simple enough that "warming the SM" doesn't help — there's no significant launch/teardown overhead between leaves.

**Decision: revert to regular flat-dispatch.** Same perf, simpler code, fewer pipelines to maintain. The `lbvh_fat_upsweep_persistent.comp` kernel + the `upsweep_persistent` pipeline + the `work_queue_gpu` buffer all remain in the engine as compiled-but-unused infrastructure, useful when:
- Hardware changes (different bandwidth/SM-count ratio could shift the tradeoff)
- Scene type changes (mesh-coherent geometry where warp threads share paths)
- The Apetrei 2014 agglomerative algorithm gets implemented (which DOES use persistent-threads with warp-shuffle)

### Engineering takeaway

The user's instinct — "gather data by implementation" — proved right in confirming the architecturally correct decision. Without the data, we'd be guessing whether persistent-threads is worth the complexity. With the data, we KNOW the regular dispatch is optimal at this scene + hardware, and we have a reusable kernel ready for future hardware/scene experiments. The empirical answer is more valuable than the predicted one.

## Memory model + `coherent` qualifier

The upsweep is the highest-risk kernel for correctness. The atomic on `done_buf` provides acquire-release ordering on `done_buf` only; the cross-invocation bounds writes to `nodes_buf` require `coherent` on `nodes_buf` independently.

Considered: drop `coherent` on Nodes (keep on ChildrenDone), rely on the atomic to globally order non-atomic memory ops. **Rejected** — per Vulkan/GLSL memory model, an atomic with `coherent` orders writes on **its own buffer's storage class**, not cross-buffer. To avoid `coherent` on Nodes we'd need explicit `memoryBarrierBuffer(nodes_buf)` before each atomic, which is more dispatches' worth of cost. `coherent` on both is the canonical pattern (Lesson 09 captures this).

## D165 — pinned design decision

> Fat-node 64 B layout + dual output paths. `LbvhTree` carries `Array<LbvhFatNode>` NOT canonical 32 B `BvhNode`. Industry precedent: NVIDIA OptiX, AMD Radeon Rays, KittenGpuLBVH. `lbvh_to_bvh_tree()` conversion deferred; ship at consumer arrival.

Supersedes D156 (canonical-BvhNode-layout) + D158 (canonical reorder is part of this same function) + D161 (Auxiliary parent_indices buffer) + D163 (CPU-side post-build reorder).

## Test corpus

Test count: 12 → **13 cases** (+ 1 GPU-resident byte-identity verification, which manually issues a `copy_buffer(handle.nodes, my_readback)` from the test's own cmd buffer to compare against the CPU build's fat-node array byte-for-byte).

Assertion count: 379 021 → **398 279** (+19 K).

5-config DoD: PASS (debug + asan + shipping + release + tidy) in 40 s via `scripts/per-slice-check.ps1 -IncludeRelease -Parallel`.

## Files

**New:**
- `runtime/examples/shaders/lbvh_fat_build.comp` (Karras tree-build writing to fat-node format; fixes up children's parent_idx)
- `runtime/examples/shaders/lbvh_fat_upsweep.comp` (carry-register walk; `coherent` on Nodes + ChildrenDone)

**Rewritten:**
- `engine/geometry-bvh-gpu/include/crd/geometry/bvh_gpu/lbvh_tree.hpp` — new `LbvhFatNode` struct + `LbvhTree` carrying Array<LbvhFatNode>
- `engine/geometry-bvh-gpu/include/crd/geometry/bvh_gpu/lbvh.hpp` — adds `GpuResidentTree` + `dispatch_build_lbvh_gpu_resident`; superseded D156/D158/D161/D163 marked; new D165 pinned
- `engine/geometry-bvh-gpu/src/lbvh.cpp` — CPU reference rewritten for fat nodes (Karras phase A + carry-register phase B; leaf-parent + leaf-is-right lookup tables built between phases)
- `engine/geometry-bvh-gpu/src/lbvh_gpu.cpp` — pipeline now caches a `prim_indices_gpu` + `prim_indices_staging` pair; `dispatch_build_lbvh` calls shared `run_build_upsweep` helper + readback; `dispatch_build_lbvh_gpu_resident` calls helper + returns handle
- `tests/geometry-bvh-gpu/test_lbvh.cpp` — DFS walks via `nodes[i].left()/right()` with MSB-leaf-flag checks; new `[gpu-resident]` test does manual readback for byte-identity verification; perf test now measures both paths
- `tests/geometry-bvh-gpu/CMakeLists.txt` — swapped shader list to fat-node shaders

**Deleted:**
- `runtime/examples/shaders/lbvh_build.comp`, `lbvh_aabb_upsweep.comp`, `lbvh_finalize.comp` (obsolete compact-node shaders)

## Deferred follow-ons

- **`v9a-c-perf-tune`** — kernel-level tuning to drop the ~5 ms GPU compute toward ~2 ms. Workgroup tuning, persistent-threads upsweep, warp-shuffle reductions. Ship when a consumer demands <5 ms.
- **`v9a-c-gpu-inputs`** — accept `sorted_pairs` + `leaf_aabbs` as GPU buffers (eylem broadphase keeps them on GPU after morton + radix sort). Drops the 2 ms staging upload.
- **`v9a-c-bvh-tree-bridge`** — `lbvh_to_bvh_tree()` conversion for consumers needing compact-node BvhTree compatibility. Ship at consumer arrival.

## Lessons reinforced

- **The advisor was right.** "Profile first, surface the choice second, then either commit to GPU-resident or get explicit user buy-in." Per-phase profiling immediately revealed that readback (12 ms) is half the wall time, redirecting the slice toward the GPU-resident API rather than only kernel tuning. `feedback_use_crash_dumps_first.md` generalizes: profile before assuming. → Updates memory note `feedback_use_crash_dumps_first` doctrine ("profile before tuning").
- **API shape sets the perf floor.** A 64 MB readback isn't optimizable — it has to go away. The right move is a different API, not a faster transfer. → reinforces `feedback_elite_only_no_shortcuts`.
