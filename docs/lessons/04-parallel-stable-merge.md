# Lesson 04 — Parallel stable merge: the per-(chunk, bucket) offset table

> **The question that motivated this lesson:** "How do you make a parallel radix sort produce *exactly* the same output as the serial reference, across runs, across worker counts, byte-for-byte?"

This is the template for any "fan out work, then merge deterministically" pattern. Once you understand the offset-table trick, you can reach for it any time you have:
- A workload that fans out across N workers
- Output that should preserve some input-order invariant
- A requirement that the output be byte-identical regardless of how the workers raced

## TL;DR

The pattern, in three phases:

```
Phase 1 (parallel):   Each worker writes local results into ITS OWN per-worker bucket.
Phase 2 (serial):     Compute global offsets so each worker's buckets land in CHUNK-INDEX order in the final output.
Phase 3 (parallel):   Each worker writes its results into its assigned non-overlapping output range.
```

The key insight: **chunk index, not worker thread, is the stable identity.** Workers can race; chunk indexes are deterministic.

## Part 1 — The naive parallel attempt (and why it fails)

A first instinct for parallel radix:

```cpp
// WRONG — naive parallel radix
crd::jobs::parallel_for(num_jobs, num_jobs, [&](u32 i, u32){
    const usize begin = i * N / num_jobs;
    const usize end   = (i+1) * N / num_jobs;
    for (usize j = begin; j < end; ++j) {
        const usize bucket = (src[j].code >> shift) & 0xFF;
        // ATOMIC bump of a shared global histogram
        const usize pos = std::atomic_fetch_add(&shared_hist[bucket], 1);
        dst[pos] = src[j];
    }
});
```

This is correct *in the "everything ends up sorted" sense* — but it has three fatal problems:

**Problem 1 — Atomics serialize the hot path.** Every scatter does an atomic increment on `shared_hist[bucket]`. Cores fight for the same cache lines (the 256-bin histogram fits in 1 KB; every core wants it). Net: no parallelism extracted.

**Problem 2 — Non-deterministic across runs.** Two workers see the same bucket; whichever atomic fires first gets the lower output position. Same input → different output bytes → different LBVH tree topology run-to-run. **This breaks the deterministic-FP contract** (ADR-0063) and breaks the `bit_compare` against the v9a-b2 GPU oracle.

**Problem 3 — Non-deterministic across worker counts.** At num_jobs=4, worker 0 might fill bucket B with items 0..99 before worker 1 fills B with 100..199. At num_jobs=8, worker 4 (which now owns the same input range as worker 1 at nj=4) might race differently. Two different num_jobs values → two different byte outputs. **Tests can't even gate on "produces correct output" reliably.**

The fix: **don't race**.

## Part 2 — The 3-phase deterministic pattern

### Phase 1: per-worker local histograms (parallel, no contention)

Each worker owns its own histogram tile. No atomics, no sharing.

```cpp
// hist_tiles[chunk][bucket] — one row per chunk
parallel_for(num_jobs, num_jobs, [&](u32 i, u32){
    const usize begin = i * N / num_jobs;
    const usize end   = (i+1) * N / num_jobs;
    usize* my_hist = hist_tiles + i * 256;
    for (usize k = 0; k < 256; ++k) { my_hist[k] = 0; }
    for (usize j = begin; j < end; ++j) {
        const usize bucket = (src[j].code >> shift) & 0xFF;
        ++my_hist[bucket];     // private — no atomic needed
    }
});
wait();
```

Cost: each worker visits its slice once. Perfect linear scaling.

### Phase 2: build the per-(chunk, bucket) offset table (serial, tiny)

This is the load-bearing step. We compute, for each (chunk, bucket) pair, the **absolute starting position in `dst`** where that chunk's items in that bucket should land. The construction makes stability automatic.

```cpp
// Global per-bucket totals: sum across all chunks.
usize bucket_total[256] = {0};
for (u32 c = 0; c < num_jobs; ++c)
    for (usize k = 0; k < 256; ++k)
        bucket_total[k] += hist_tiles[c * 256 + k];

// Global bucket start: exclusive prefix over bucket_total.
usize bucket_start[256];
{
    usize sum = 0;
    for (usize k = 0; k < 256; ++k) {
        bucket_start[k] = sum;
        sum += bucket_total[k];
    }
}

// Per-(chunk, bucket) offset: chunk c writes to dst at
//   bucket_start[B] + sum over j < c of hist_tiles[j * 256 + B]
// I.e., each chunk's bucket-B region starts right after all PRIOR
// chunks' bucket-B contributions are placed.
usize running[256];
for (usize k = 0; k < 256; ++k) running[k] = bucket_start[k];
for (u32 c = 0; c < num_jobs; ++c) {
    for (usize k = 0; k < 256; ++k) {
        scatter_off[c * 256 + k] = running[k];
        running[k] += hist_tiles[c * 256 + k];
    }
}
```

After this loop, `scatter_off[c][B]` is the absolute position in `dst` where chunk `c`'s first item with bucket `B` will go. Chunk `c+1`'s first item with bucket `B` will go at `scatter_off[c+1][B] = scatter_off[c][B] + hist_tiles[c][B]` — **strictly after** all of chunk `c`'s bucket-`B` items.

Cost: O(num_jobs × 256). At num_jobs=8 that's 2048 add operations — about 1 µs. Negligible. The serial phase is essentially free.

### Phase 3: deterministic parallel scatter (parallel, disjoint output ranges)

Each chunk writes to its precomputed per-bucket offsets. No two chunks ever write to the same output index. **No atomics needed.**

```cpp
parallel_for(num_jobs, num_jobs, [&](u32 i, u32){
    const usize begin = i * N / num_jobs;
    const usize end   = (i+1) * N / num_jobs;
    usize* my_off = scatter_off + i * 256;
    for (usize j = begin; j < end; ++j) {
        const usize bucket = (src[j].code >> shift) & 0xFF;
        const usize pos = my_off[bucket]++;     // private — no atomic
        dst[pos] = src[j];
    }
});
wait();
```

Cost: same as the serial scatter, divided across workers. Bandwidth-bound but parallelizable.

## Part 3 — Why this is stable

The stability claim: for any two equal-key items at input indices `i1 < i2`, their output positions satisfy `out_pos(i1) < out_pos(i2)`.

**Case 1 — both items in the same chunk c.** Within chunk c, we visit input left-to-right. Each scatter bumps `my_off[bucket]` monotonically. So if both items have the same bucket B and i1 < i2 (both in chunk c), item i1 lands at `scatter_off[c][B]` and item i2 at `scatter_off[c][B] + 1`. **Stable within chunk.**

**Case 2 — items in different chunks c1 < c2.** Both items have the same bucket B. Item i1 (in chunk c1) lands somewhere in `[scatter_off[c1][B], scatter_off[c1][B] + hist_tiles[c1][B])`. Item i2 (in chunk c2 > c1) lands somewhere in `[scatter_off[c2][B], ...)`. Since `scatter_off[c2][B] = scatter_off[c1][B] + hist_tiles[c1][B] + ...` (all prior chunks' contributions added), `scatter_off[c2][B] > scatter_off[c1][B] + hist_tiles[c1][B] > out_pos(i1)`. **Stable across chunks.**

Combine cases 1 and 2: stability holds globally. The output is byte-identical to a serial single-thread radix.

## Part 4 — The discriminating tests

If your test corpus is just "10k random keys, check sorted," you can have a buggy implementation that **looks** correct but actually fails stability. The bugs we worried about:

1. **Within-chunk stable, across-chunk unstable.** (Naive parallel without the offset table.)
2. **Stable at num_jobs=1, unstable at num_jobs=8.** (Bug in the offset-table computation that only fires when chunks split.)
3. **Stable for unique keys, unstable for equal keys.** (Cropping the offset table at the wrong index.)

The tests that catch these:

```cpp
// TEST 1 — 4096 all-equal keys spanning ALL workers
//   All items have the same code. Bucket distribution is "everything in
//   one bucket". Output indices MUST be 0, 1, ..., 4095 monotonically
//   ACROSS all worker chunks, not just within each.

// TEST 2 — Two specific equal keys, placed in chunk 0 and chunk N-1
//   Catches "stable within worker, unstable across workers." Chunk 0's
//   item must land BEFORE chunk N-1's item at the same bucket.

// TEST 3 — num_jobs sensitivity: byte-identical at num_jobs ∈ {1, 2, 4, 8, 16}
//   Same input, vary the worker count. Output must be byte-for-byte
//   identical at every count. Catches subtle offset-computation bugs.
```

Our `test_morton_sort_parallel.cpp` ships all three. 400 025 assertions pass; the implementation is correct.

If you ever write a parallel-stable-merge for another module (eylem broadphase, parallel BVH refit, cooker bake), **copy these test patterns**. They're cheap to write, they catch real bugs, and they're the only way to be confident the output is deterministic.

## Part 5 — Where the pattern applies beyond radix

Anywhere you have:
- A workload with **N inputs** to fan out across workers
- Each input maps to a **bucket** (could be a hash, a bin, a spatial cell, a destination buffer)
- The output must preserve some **input-order property** within each bucket

The 3-phase pattern applies. Examples Cerid will encounter:

### Eylem broadphase — parallel pair list build
- Input: M dynamic bodies, AABB-overlapping
- Buckets: pair-list cells (e.g., one bucket per `(min_axis_quantized)`)
- Stability requirement: pairs reported in deterministic order (replay-hash contract from ADR-0063)
- Pattern: per-worker local pair lists → merge into deterministic global list via offset table

### Parallel mesh-cooker bake
- Input: triangles in a source mesh
- Buckets: cooker output sections (vertex buffer, index buffer, BVH subtrees)
- Stability: cooked artifact byte-identical across cooker runs (so `.crdr` hashes are stable)
- Pattern: per-worker partial writes → merge with per-(worker, section) offsets

### Histogram-style scene queries
- Input: visible primitives in a frame
- Buckets: render queues by material/shader/pipeline
- Stability: depth-sorted draws within each pipeline must be in deterministic depth order for fair comparisons
- Pattern: per-worker partial queues → merge via offset table after culling

### Voxelization (already shipped in V-HACD)
- Input: triangles
- Buckets: voxel cells
- The current V-HACD path uses `atomic_ref<u8>::fetch_or(Surface)` instead of an offset table, because the operation is **commutative** (OR is order-independent). When it isn't commutative, you fall back to the 3-phase pattern.

## Part 6 — When NOT to use this pattern

The 3-phase pattern adds overhead: per-worker tiles, the serial Phase 2, the wait barriers. It pays off only when:

- **N is large enough** that the per-chunk work amortizes the Phase 2 cost. Below ~64K items, single-threaded wins.
- **The output requires stability**. If the bucket-assignment operation is commutative (associative-and-commutative monoid), atomics are simpler and may be faster.
- **You have at least 4 cores actually available**. Below 4, the parallel overhead exceeds the parallel saving for bandwidth-bound problems.

If any of those fails, ship the serial path. Cerid's `sort_morton_pairs_parallel` falls back to serial at `N < kDefaultParallelSortThreshold = 65536` precisely for this reason.

## What to read next

- [Lesson 05 — CPU vs GPU performance tiers](05-cpu-vs-gpu-perf-tiers.md) — when parallel CPU still loses to GPU, and when it doesn't.
- [Lesson 06 — Substrate vs speculation](06-substrate-vs-speculation.md) — when to ship this pattern proactively (as we did for radix) vs when to wait for a real consumer.
