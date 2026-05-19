# Lesson 01 — Morton codes, radix sort, and the LBVH pipeline

> **The question that motivated this lesson:** "Why do we need radix sort and morton sort? Where are we going to use them and for what?"

## TL;DR

There is no such thing as "Morton sort." There are two distinct pieces that **compose** into a single pipeline:

```
3D AABBs  →  Morton CODES  →  SORTED Morton codes  →  LBVH tree topology
            (geometric →     (radix sort —          (Karras 2012
             1D mapping)      pure integer sort)     binary-tree-from-sorted)
```

The radix sort doesn't know or care about 3D space. The Morton encoding doesn't know or care about sorting. They are useful **together** because the LBVH algorithm needs spatially-adjacent primitives to also be index-adjacent — and that's exactly what `sort(morton_codes)` produces.

## Part 1 — What is a Morton code?

A **Morton code** (a.k.a. Z-order curve) is a single integer that encodes a 3D position by **bit-interleaving** the X, Y, Z coordinates. For 10-bit per axis:

```
x = x9 x8 x7 ... x0     (10 bits)
y = y9 y8 y7 ... y0
z = z9 z8 z7 ... z0

morton = x9 y9 z9 x8 y8 z8 ... x0 y0 z0    (30 bits total)
```

The killer property: **points close in 3D space have Morton codes that are close in linear value.** If two AABBs share their top 9 bits, their boxes are in the same 1/512 of the scene. If they share the top 6 bits, they're in the same 1/64. The code is a 1D address that approximates 3D locality.

(There are *anomalies* — the Z-curve has discontinuities at certain bit boundaries — but for the LBVH purpose those don't matter. The sort doesn't need a perfect 1D mapping; it just needs spatial neighbors to mostly cluster together.)

**Cerid's Morton implementation** (`engine/geometry-bvh-gpu/include/crd/geometry/bvh_gpu/morton.hpp`):

- **30-bit codes** (10 bits per axis) — `compute_morton_codes_cpu(aabbs, scene_bounds, alloc) -> Array<u32>`
- **60-bit codes** (20 bits per axis) — for km-scale scenes where 1m bins collide. Output is `Array<u64>`.
- **GPU path** — `MortonGpuPipeline::dispatch_morton_codes(aabbs, scene_bounds)` reads AABBs from a GPU buffer, writes codes back, mechanical translation of the CPU body (D134).

Each AABB's centroid is quantized to the scene's bounding cube, the 10 bits per axis are interleaved, and the result is one `u32` per AABB.

## Part 2 — Why sort the codes?

The Karras 2012 LBVH algorithm wants to build a binary BVH where:
- Leaves are the input primitives (one primitive per leaf).
- The tree shape mirrors the spatial layout — neighboring leaves are spatially close.

If we sort the primitives by their Morton codes, **adjacency in the sorted array IS adjacency in 3D space** (approximately). The tree builder then becomes: "find the bit position where each pair of sorted Morton codes first disagrees, use that as the split level for the internal node." This is fully parallel — each internal node's split is determined locally from just two adjacent Morton codes — no global SAH evaluation, no recursion overhead.

So the **sort is the bridge** between "input order is arbitrary" and "input order encodes spatial locality." Without the sort, the LBVH wouldn't work.

**Why radix sort specifically?**

- Comparison sorts (merge, quicksort, std::sort) are O(N log N). For 1 M elements, log₂ = 20 → 20 M comparisons + branch mispredicts. On Cerid we measured: `crd::containers::sort` ~50-100 ms on 1 M pairs (slower than the asserted 20 ms budget).
- Radix sort is O(N k) where k = bytes-of-key. For 32-bit keys with 8-bit digits, k = 4. That's 4 M iterations. ~5 ms on the dev box.
- Radix is **stable** by construction (equal keys preserve input order) — important because the LBVH algorithm needs a deterministic tiebreak for equal Morton codes. Equal codes mean coincident centroids (multiple primitives at the same spot); we want the tree topology to be reproducible.

So: **Morton codes for spatial locality + radix sort because it's fast and stable.** They're inseparable for the LBVH pipeline; either alone is useless for this purpose.

## Part 3 — Cerid's sort implementations (current state)

We ship **three** implementations of sorting Morton-code pairs, all producing **byte-identical output** for the same input. Which one to use depends on where your data lives and what your downstream pipeline looks like.

| Implementation | Entry point | Measured 1 M u32 | When to use |
|---|---|---|---|
| Scalar + prefetch (canonical CPU reference) | `sort_morton_pairs<KeyT>(span, alloc)` | **4.77 ms** | CPU-side oracle, small inputs, fallback path |
| Parallel 8-worker (CPU, opt-in) | `sort_morton_pairs_parallel<KeyT>(span, alloc, num_jobs)` | **2.56 ms** | CPU LBVH builds, parallel BVH refit, cooker bakes |
| GPU end-to-end (Vulkan compute) | `MortonRadixGpuPipeline::dispatch_radix_sort(span, alloc)` | **7.14 ms** | GPU LBVH builds where downstream stays on GPU |

The GPU end-to-end number being *slower* than the parallel CPU is initially surprising. See [Lesson 05 — CPU vs GPU performance tiers](05-cpu-vs-gpu-perf-tiers.md) for the full explanation; the short version is that the 7.14 ms includes **CPU→GPU upload + GPU→CPU readback**, and those round-trips dominate when N is small enough that the GPU compute itself is fast.

The pure GPU-compute portion of v9a-b2 is roughly **1-2 ms** on the dev box (the rest is staging + readback). For a "10 GB of geometry already on the GPU" workload, the GPU radix wins by 5-10×. For a "load AABBs from disk, sort once, hand to CPU physics" workload, the parallel CPU is faster end-to-end.

## Part 4 — The pipeline composed: how LBVH actually uses these

The full LBVH build is a 4-stage pipeline. Each stage produces input for the next:

```
Stage 1: Morton-code generation
  input:  Array<AABB3<f32>>  primitives
          AABB3<f32>          scene_bounds
  output: Array<u32>          morton_codes  (or Array<u64> at 60-bit)

Stage 2: Stable radix sort
  input:  Array<u32>                morton_codes (raw codes)
  output: Array<MortonPair<u32>>    sorted pairs {code, original_index}

Stage 3: Karras tree-from-sorted-codes (v9a-c — NEXT slice)
  input:  Array<MortonPair<u32>>    sorted pairs
  output: Array<BvhNode>            internal nodes (split per prefix-length-comparison)

Stage 4: Bottom-up AABB upsweep (v9a-d)
  input:  Array<BvhNode>            tree topology
          Array<AABB3<f32>>         leaf AABBs (via prim_index lookup from sorted pairs)
  output: Array<BvhNode>            with cached AABBs propagated up
```

Each stage has both a **CPU reference** and a **GPU implementation**. The CPU is the algorithm definition (D134); the GPU is a mechanical translation conformance-tested via `bit_compare` (or `ulp_compare` for FP outputs). Stages can be mixed: you can do Morton on GPU, sort on CPU, tree on GPU, upsweep on CPU. Or all on one device. The contract that makes this work is the byte-identical `MortonPair<u32>` layout (D149 — sizeof 8, alignof 4, static_asserted).

## Part 5 — Who consumes this pipeline (planned)

Today, only the test suite consumes the sort. Here are the planned consumers:

### Consumer 1: GPU LBVH builder (`v9a-c`, ~4 days from now)
Pure GPU path. AABBs already in GPU storage → Morton codes → sort → tree → upsweep, no host round-trip. This is **the** consumer the v9a cluster exists for. Estimated build time: ~3-5 ms for 1 M primitives, dominated by the sort.

### Consumer 2: Eylem physics broadphase (Phase 3.1 v1c)
Dynamic-body simulation needs a BVH over rigid-body AABBs, rebuilt or refitted every frame. Two patterns:
- **Full rebuild** at scene-load: parallel CPU radix → CPU LBVH build → upload to GPU. Per-load, not per-frame. The 2.56 ms parallel CPU sort fits a 60 Hz frame budget easily.
- **Incremental refit** per-frame: NO sort, just update existing-tree AABBs. The sort is a one-time cost; refit is the per-frame hot path.

### Consumer 3: Cooker LBVH bake (offline pipeline, post-Phase-3)
Asset cooker writes BVH baked into `.bvh` artifact at build time. Sort happens once per asset; runtime loads the prebuilt tree directly. Even the scalar 4.77 ms is fast enough for an asset that bakes once and ships forever.

### Consumer 4: Mesh-collider bake (Phase 3.1 v1d-mesh)
Triangle-mesh colliders need a BVH over their triangles for raycast / closest-point queries. Same pattern as consumer 3: cooker-side bake, runtime load.

### Consumer 5: GPU-driven rendering occlusion culling (Phase 3.5+)
Per-frame BVH over visible objects, sorted by screen-space tile or depth, used by an indirect-draw compute pass to skip occluded clusters. Pure GPU consumer; uses v9a-b2 GPU radix path.

### Consumer 6: V-HACD output (Phase 3.1.7 v9c — already shipped)
Convex decomposition produces per-part convex hulls; the convex-collider conditioning step needs a BVH over the parts for collision-broadphase queries. Cooker-side; uses serial CPU sort.

## What this composes into, in one sentence

> Morton codes turn a 3D location into a 1D address that approximates spatial locality; sorting those addresses produces an index sequence where neighbors are spatially neighbors; the Karras LBVH builds a balanced binary tree from that sequence in fully-parallel fashion. **All three pieces are necessary; no two are sufficient.**

## What to read next

- [Lesson 04 — Parallel stable merge](04-parallel-stable-merge.md) — the template that makes `sort_morton_pairs_parallel` deterministic across worker boundaries.
- [Lesson 05 — CPU vs GPU performance tiers](05-cpu-vs-gpu-perf-tiers.md) — why the GPU radix is "slow" end-to-end but "fast" in a GPU-resident pipeline.
- [Lesson 07 — Using radix and Morton in real consumers](07-using-radix-and-morton.md) — concrete recipes per consumer.
