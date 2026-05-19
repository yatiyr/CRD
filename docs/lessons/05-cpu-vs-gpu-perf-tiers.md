# Lesson 05 — CPU vs GPU performance tiers

> **The question that motivated this lesson:** "How fast is the GPU radix sort right now? When do we use it vs the CPU one?"

The honest answer surprised the user (and me): the GPU radix is **slower end-to-end** than the parallel CPU radix at 1 M elements. Not because the GPU is bad, but because the question "how fast" only has a meaningful answer once you specify **where the data lives** and **where it goes next**.

## TL;DR

- **GPU radix sort end-to-end (1 M u32, win-shipping):** 7.14 ms — includes CPU→GPU upload + 25 dispatches + GPU→CPU readback + fence wait.
- **Pure GPU compute (estimated, no transfers):** ~1-2 ms.
- **Parallel CPU radix (8 workers, same workload):** 2.56 ms — no transfers, data stays on CPU.

The GPU is faster **only when the downstream pipeline stays on the GPU**. The moment a CPU consumer needs to read the result, transfers dominate.

Pick CPU when:
- The producer is on CPU (asset loader, scene cooker, host-side physics).
- The consumer is on CPU.
- N is below ~10 M (transfer overhead doesn't amortize).

Pick GPU when:
- The producer is on GPU (Morton codes computed by a compute shader on GPU-resident AABBs).
- The consumer is on GPU (LBVH tree builder, frustum culler, raytracer).
- The whole pipeline lives in GPU memory; you never touch the CPU.

## Part 1 — Where the GPU's 7.14 ms goes

The `MortonRadixGpuPipeline::dispatch_radix_sort` for 1 M u32 codes does:

1. **CPU→GPU upload** of 1 M × 4 B = 4 MB of input codes
2. **Init kernel dispatch** — packs `{code, gl_GlobalInvocationID.x}` pairs (one workgroup of 256 threads × ceil(1M/1024) = 1024 workgroups)
3. **8 sort passes**, each three dispatches: histogram + scan + scatter
4. **GPU→CPU readback** of 1 M × 8 B = 8 MB of sorted pairs
5. **Fence wait** on the queue, blocking the CPU until done

Rough breakdown by category (estimated from the dev box; integrated/low-end GPU):

| Phase | Time | Notes |
|---|---|---|
| Upload (4 MB) | ~0.2-0.5 ms | PCIe Gen4 effective ~10-20 GB/s for small transfers |
| Init dispatch | ~0.1 ms | One simple kernel, 1024 workgroups |
| 8 sort passes (24 dispatches) | ~1-2 ms | Each dispatch ~50-100 µs incl. command-buffer overhead |
| Readback (8 MB) | ~0.4-0.8 ms | Slower than upload (GPU→CPU is typically lower bandwidth) |
| Fence wait + queue submit overhead | ~3-5 ms | The wait for all commands to drain |

The **fence wait dominates** on this workload. The GPU finishes the actual compute quickly; the synchronous "wait for the queue to fully drain then map the readback buffer" eats most of the wall-clock time.

On a workstation GPU (RTX 3060+ class), expect:
- Upload + readback: similar (PCIe-bound, not GPU-bound)
- Sort dispatches: ~0.3-0.6 ms (vs 1-2 ms on integrated)
- Fence wait: ~1-2 ms (smaller queue, faster drain)

**Total e2e on workstation GPU: probably ~3-5 ms.** Still slower than the parallel CPU 2.56 ms for end-to-end, because transfers haven't gone away.

## Part 2 — Why this isn't a failure of the GPU radix

If you read the v9a-b2 GPU radix as "the GPU sort that beats CPU," you'd be disappointed. But that's not what it is.

It's "the sort that fits into a **pure-GPU pipeline** where the data never leaves the GPU." In that context, the cost is just the dispatch time (~1-2 ms), because:

- The input is already on the GPU (computed by `MortonGpuPipeline::dispatch_morton_codes`)
- The output is consumed on the GPU (by the upcoming v9a-c GPU LBVH tree builder)
- No upload, no readback, no fence wait — just submit-and-keep-going

**The 7.14 ms is for the test harness**, which has to validate the sort by reading the result back to the CPU. Production consumers won't pay that.

This is the same trap as "is your function fast?" → it depends on whether the caller has the data hot in registers, hot in L1, hot in RAM, or sitting on disk. Asking "how fast is the GPU radix" requires saying "in what pipeline."

## Part 3 — The three pipeline shapes and their winners

### Shape A — GPU-resident producer-consumer pipeline (GPU wins big)

```
GPU compute shader writes AABBs
   ↓
GPU MortonGpuPipeline writes morton codes
   ↓
GPU MortonRadixGpuPipeline writes sorted pairs        ← v9a-b2
   ↓
GPU LBVH builder writes BvhNodes                       ← v9a-c (planned)
   ↓
GPU compute shader does raycast / culling / etc.
```

Every step lives in GPU buffers. No upload, no readback between stages. The radix sort costs ~1-2 ms because that's the pure compute time. **GPU wins by 3-5×** over the parallel CPU path that would have to do the upload + sort + readback for each frame.

This is the v9a cluster's reason for existing. Once v9a-c lands, this pipeline will be the hot path for any "rebuild BVH per frame" workload (eylem dynamic-body broadphase at high entity counts, occlusion culling on visible-set, particle-cell sort for SPH/MPM physics).

### Shape B — CPU-resident producer-consumer pipeline (parallel CPU wins)

```
Asset loader builds AABBs on CPU
   ↓
CPU Morton-code generator (compute_morton_codes_cpu)
   ↓
sort_morton_pairs_parallel (parallel CPU radix)        ← v9a-b1-parallel
   ↓
CPU LBVH builder (bvh_build) writes BvhTree to host memory
   ↓
Renderer uploads BvhTree to GPU once, draws every frame
```

The BVH is built once per scene load, lives on the host, gets uploaded to GPU once. No per-frame round-trips. **Parallel CPU wins** at 2.56 ms because there's no GPU transfer cost at all. The GPU radix would add upload+readback for zero benefit — the BVH never lives on the GPU during the sort.

This is the eylem-v1c-broadphase path for "physics builds its broadphase BVH on CPU, syncs to GPU for collision visualization."

### Shape C — Mixed: GPU compute the AABBs, CPU consumes the tree

```
GPU compute writes per-particle AABBs (e.g., from SPH simulation)
   ↓
GPU Morton codes computed on the same buffer
   ↓
?  Sort happens... where?
   ↓
CPU reads the sorted result for collision callbacks / scripting hooks
```

This is the tricky case. The shortest pipeline is:
1. Sort on GPU (~2 ms compute) + readback (~1 ms) = ~3 ms
2. OR readback AABBs first (~1 ms) + sort on parallel CPU (~2.5 ms) = ~3.5 ms
3. OR readback Morton codes first (~0.5 ms) + parallel CPU sort (~2.5 ms) = ~3 ms

All three are within a millisecond. The right choice depends on **what the CPU side wants to do next.** If it wants the sorted *indices* into the original GPU buffer, option (1) wins because we only ship sorted indices back, not the data. If it wants the sorted *pairs*, options 1 and 3 are equivalent.

**General principle:** the sort happens where the *downstream consumer* lives. Moving data is more expensive than the sort itself once N is small enough that compute time is comparable to transfer time.

## Part 4 — Bandwidth math (back-of-envelope)

For 1 M u32 input (4 MB) + 1 M MortonPair<u32> output (8 MB):

| Bus | Bandwidth | Time for 12 MB |
|---|---|---|
| L1 cache (per core, both directions) | ~1 TB/s | 0.012 ms |
| L2 cache (per core) | ~500 GB/s | 0.024 ms |
| L3 cache | ~100-200 GB/s | 0.06-0.12 ms |
| DDR5 RAM | ~50-90 GB/s | 0.13-0.24 ms |
| PCIe Gen4 x16 (one direction) | ~32 GB/s | 0.38 ms |
| PCIe Gen3 x16 (one direction) | ~16 GB/s | 0.75 ms |
| GPU VRAM (RTX 3060 class) | ~360 GB/s | 0.033 ms |

The PCIe round-trip alone (upload 4 MB + readback 8 MB) costs ~0.4-1.0 ms even at peak bandwidth. In practice, the small-transfer overhead (driver buffer staging, command submission) adds another 0.5-2 ms. **The CPU↔GPU bus is the slowest link in any mixed pipeline.**

This is also why **streaming GPU workloads** (decoupled producer-consumer where multiple frames are in flight simultaneously) hide the transfer cost. The single-frame "submit and wait" pattern v9a-b2 uses today exposes it; an async-compute path (where the next frame's CPU work proceeds while the previous frame's GPU work drains) would change this picture.

## Part 5 — Decision rules

Use the **parallel CPU radix** (`sort_morton_pairs_parallel`) when:
- ✅ Producer is on CPU
- ✅ Consumer is on CPU
- ✅ N < ~10 M
- ✅ Sort runs on a per-frame or rarely-rebuild cadence (asset load, scene change)

Use the **GPU radix** (`MortonRadixGpuPipeline`) when:
- ✅ Data is **already on the GPU** when sort starts
- ✅ Consumer will use it **without leaving the GPU**
- ✅ N >= 100K (small N doesn't amortize dispatch overhead)
- ✅ Sort is part of a per-frame compute pipeline (GPU LBVH, occlusion cull, etc.)

Use the **serial scalar+prefetch CPU radix** (`sort_morton_pairs`) when:
- ✅ N < 64K (below the parallel threshold)
- ✅ `crd::jobs` isn't initialized (e.g., a test, a small tool)
- ✅ You need the smallest binary footprint (e.g., shipping a CLI that doesn't depend on the fiber job system)

The first two listed entries are **opt-in by the consumer**. The serial scalar+prefetch is the default (`sort_morton_pairs<KeyT>`). Consumers who know they're in the parallel-CPU or GPU regime call the explicit entry point.

## Part 6 — A practical example: eylem v1c broadphase (planned)

When eylem v1c broadphase lands, here's the decision logic it'll go through:

```cpp
// Pseudo-code for eylem broadphase initialization at scene load:
if (scene_has_gpu_pipeline) {
    // GPU compute writes per-body AABBs into a GPU buffer.
    morton_gpu.dispatch_morton_codes(body_aabbs_gpu, scene_bounds);
    radix_gpu.dispatch_radix_sort(morton_codes_gpu);
    lbvh_gpu.build_tree(sorted_pairs_gpu);
    // Whole BVH lives on GPU. Collision broadphase runs as GPU compute.
} else {
    // Asset-time: build BVH on CPU once, upload to GPU.
    const auto morton_codes = compute_morton_codes_cpu(body_aabbs, scene_bounds, alloc);
    const auto sorted_pairs = sort_morton_pairs_parallel<u32>(morton_codes, alloc);
    const auto tree         = bvh_build_from_lbvh(sorted_pairs, alloc);  // v9a-c CPU mirror, future
    gpu_upload(tree);
    // Per-frame: refit only (no sort, no rebuild).
}
```

Both code paths exist. The choice happens at scene-init time based on what the consumer (renderer? collision system? scripting layer?) needs. The substrate gives us both options; the consumer picks.

## What to read next

- [Lesson 01 — Morton codes, radix sort, and the LBVH pipeline](01-morton-codes-and-lbvh-pipeline.md) — the full pipeline these implementations belong to.
- [Lesson 07 — Using radix and Morton in real consumers](07-using-radix-and-morton.md) — concrete consumer recipes.
