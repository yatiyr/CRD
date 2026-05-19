# Lesson 10 — API shape sets the performance floor

> **The question that motivated this lesson:** "Why are we 3× slower than KittenGpuLBVH? My GPU isn't worse than theirs."

The full answer is in two parts. The short answer is: we weren't slower — we were measuring different things. Once we matched the measurement assumptions of the reference (data already on the GPU), our number went from 7.4 ms / 1 M to **1.45 ms / 1 M** — matching KittenGpuLBVH's published 1.5 ms on RTX 3090, despite our RTX 4070 Ti SUPER having ~40 % LESS memory bandwidth (672 vs 936 GB/s).

**Same algorithm. Same hardware. 5× faster from an API change.**

## TL;DR

Three dispatch paths on the same LBVH pipeline:

| Path | 1 M / RTX 4070 Ti SUPER | Why it costs this much |
|---|---|---|
| `dispatch_build_lbvh` (CPU input, CPU output) | **~20 ms** | 2 ms staging upload + 5 ms GPU compute + 12 ms readback to CPU |
| `dispatch_build_lbvh_gpu_resident` (CPU input, GPU output) | **~7.4 ms** | 2 ms staging upload + 5 ms GPU compute, no readback |
| `dispatch_build_lbvh_from_gpu` (GPU input, GPU output) | **~1.45 ms** | Just the GPU compute + minor command-buffer overhead |

Pick the path where your consumer lives. The kernel is identical in all three; the cost is what crosses the PCIe bus.

## Part 1 — How we got to 1.45 ms (the chain)

Starting state (v9a-c original): **28.1 ms / 1 M**, compact 32 B `BvhNode` layout, canonical-reorder pass on CPU, full readback to CPU `BvhTree`.

| Step | Change | Result |
|---|---|---|
| Layout | 32 B BvhNode → 64 B fat-node (KittenGpu-style, both children's bounds inline) | 28.1 → 20.9 ms (kills the canonical-reorder CPU pass; siblings are no longer consecutive by design) |
| Output API | Add `GpuResidentTree` handle; skip 64 MB CPU readback | 20.9 → 7.4 ms |
| Input API | Add `GpuInputView`; skip CPU staging upload | 7.4 → **1.45 ms** 🎯 |
| Kernel tuning | Workgroup {32,64,128,256}, level-by-level, drop `coherent`, persistent-threads + warp-batched atomics | All within ±0.1 ms noise of baseline |
| Cmd-cache | Pre-allocate cmd buffer + fence in ctor, reset + re-record per call | ~5-7 % across all paths |

**The three big wins were structural (layout + output API + input API), not algorithmic.** The kernel didn't change. The kernel was already fast — we just kept hiding it behind upload/readback work.

## Part 2 — What `dispatch_build_lbvh_from_gpu` actually does differently

The GPU-inputs path looks like this:

```cpp
GpuResidentTree handle = pipeline.dispatch_build_lbvh_from_gpu({
    .sorted_pairs = morton_sort_output_gpu_buffer,   // already on GPU
    .leaf_aabbs   = scene_aabbs_gpu_buffer,           // already on GPU
    .n            = 1'000'000,
});
// handle.nodes and handle.prim_indices live in pipeline-owned GPU buffers.
// Consumer queries the tree on the GPU. No CPU intervention.
```

Internally:
1. Copy `done_gpu` ← pre-zeroed staging (4 MB GPU transfer, ~0.05 ms — tiny)
2. Copy `nodes_gpu[0].parent_idx` ← `0xFFFFFFFF` sentinel (4 bytes)
3. Build kernel dispatch — N-1 internal-node threads writing fat-node topology
4. Buffer barrier
5. Upsweep kernel dispatch — N leaf threads doing carry-register walks
6. Buffer barrier
7. `lbvh_fat_extract_prim_indices` kernel — N threads copying `sorted_pairs[2k+1]` → `prim_indices_gpu[k]`
8. Fence wait

No CPU `memcpy` to staging. No `vkCmdCopyBuffer` for pairs (~8 MB) or leaf AABBs (~24 MB). No final readback. The 32 MB of data that was being pushed and pulled across PCIe is just *not in the path anymore*.

What's left is the pure GPU compute (~1 ms) plus command-buffer recording (~0.4 ms) — that's the floor.

## Part 3 — Why this generalizes (the principle)

When you measure "how fast is this operation," you're measuring three things at once:

1. **Pure compute** — instruction throughput, memory bandwidth, atomic latency. Algorithm-bound.
2. **Data movement** — transfers between CPU and GPU memory. API-bound.
3. **Command-buffer overhead** — descriptor allocation, barrier emission, fence handling. Driver-bound.

For most "production" GPU kernels, **(2) dominates**. PCIe Gen4 x16 gives ~32 GB/s peak; small-transfer overhead drops it to ~10-15 GB/s effective. A 64 MB readback at 12 GB/s is 5 ms. That number doesn't care how clever your algorithm is.

The published research papers measure (1) alone, because they assume the consumer pipeline keeps data on the GPU. When you compare a real-consumer pipeline to a paper's number, you're not measuring the same thing unless you match the API shape.

**This is the same trap as Lesson 05** (CPU-vs-GPU radix tiers), just generalized. Lesson 05 said: the GPU is faster only when downstream stays on GPU. Lesson 10 says: the API surface IS the perf floor — if your API forces CPU data movement, no kernel optimization will save you.

## Part 4 — The hierarchy of perf wins (use in this order)

When chasing performance, walk the list from cheapest-to-implement to hardest:

1. **API shape: where does the data live?** Can the consumer accept GPU buffers instead of CPU memory? If yes, the win is multiplicative (we got 14× from one API change).
2. **Readback elimination.** Does the caller actually need the data on CPU? If not, return a GPU handle.
3. **Staging elimination.** Are CPU memcpys writing zero-initialized data? Move the init to the ctor or use a GPU-side fill kernel.
4. **Cmd-buffer caching.** Pre-allocate cmd buffers + fences, reset+re-record per call. Saves ~5-10 % across the board.
5. **Algorithmic restructuring.** Level-by-level vs carry-register, persistent threads, warp shuffles. Last resort — measure first to confirm the kernel is the bottleneck.

Most engineers reach for (5) first because it's the most fun. The wins are usually in (1)-(3).

## Part 5 — Empirical data > prediction (the persistent-threads non-result)

After hitting 1.45 ms with API changes, we tried `lbvh_fat_upsweep_persistent.comp` — a persistent-threads kernel with warp-batched atomic pulls. Tested 4 K / 16 K / 64 K thread counts. **Every variant was within ±0.1 ms of the regular flat dispatch.**

The data confirms: the kernel is **memory-bandwidth-bound** on a 64 MB working set. The scheduling pattern is irrelevant. The flat dispatch and the persistent-threads kernel both saturate the same bandwidth bottleneck.

We kept the persistent kernel compiled but unused — useful when:
- Different hardware shifts the bandwidth/SM-count balance
- Scene type changes (mesh triangles with spatial coherence let warp threads share parent paths → warp-shuffle aggregation could win)
- The Apetrei 2014 agglomerative algorithm gets implemented (uses persistent-threads natively)

The lesson: **the negative result is also data.** Predicting "persistent threads will win for irregular workloads" sounded plausible, but the actual measurement showed our scene + hardware was already at the bandwidth floor with a much simpler design. Knowing this empirically is more valuable than guessing.

## Part 6 — Action items for new GPU kernels in Cerid

When designing the next GPU pipeline in this codebase:

1. **Default to the GPU-inputs API shape** from day one. CPU-input variants are convenience wrappers that call the GPU-input core after a one-time staging copy. Don't bake CPU staging into the core dispatch.
2. **Return GPU handles, not CPU data.** Provide a separate explicit "readback" function for tests/debug; consumers opt in to the CPU cost.
3. **Pre-allocate everything in the pipeline ctor** that doesn't change between calls: cmd buffers, fences, staging buffers (pre-zeroed), descriptor allocators sized for the worst case.
4. **Measure with median-of-5 + printf in win-shipping** (per Lesson 03). The win-debug numbers lie by 2-3×; single-shot timing is contaminated by thermals.
5. **Only after the above, look at the kernel.** And when you do, measure before AND after — the kernel is usually already near the bandwidth floor.

## What to read next

- [Lesson 03 — Measuring performance correctly](03-measuring-performance-correctly.md) — how to actually know how fast something is.
- [Lesson 05 — CPU vs GPU performance tiers](05-cpu-vs-gpu-perf-tiers.md) — the original "where the data lives" lesson; Lesson 10 is its generalization to any GPU kernel.
- [Lesson 07 — Using radix + Morton in real consumers](07-using-radix-and-morton.md) — concrete examples of CPU-input vs GPU-input pipeline composition.
