# 2026-05-18 — Phase 3.1.7 v9a-b1-parallel CLOSED: 3-phase parallel radix via crd-jobs

**Slice:** v9a-b1-parallel, the parallel CPU radix follow-on filed at v9a-b1-simd close.

**Status:** ✅ SHIPPED same day. **1.86× measured speedup** (4.77 ms → 2.56 ms median-of-5 on 1 M u32 win-shipping, 8 workers). 8 conformance tests / 400 025 byte-identical assertions across the worker-spanning adversarial corpus. **5-config DoD PASS in 46 s.**

> User directive (paraphrased): "Let's build v9a-b1-parallel, but honestly tell me if it is going to help us. I think it will in the future."
>
> Honest answer delivered up front: substrate value is real (jobs-bandwidth-stress + parallel-stable-merge template), expected outcome 2-3 ms NOT 1 ms (1 ms is a GPU number; v9a-b2 already owns it). User confirmed scope before code.

---

## Architectural rationale (why this is substrate, not speculation)

Per the refined [[ship-at-consumer-template-from-day-one]] rule: substrate ships proactively; speculative consumer-specific paths defer. v9a-b1-parallel qualifies as substrate on four counts:

1. **Exercises `crd-jobs` in a bandwidth-bound regime never previously tested.** crd-jobs was validated under fiber-stress + ECS-iteration profiles; pure-bandwidth radix scatter is a distinct stress class. The 1 M u32 / 4-pass / 8-worker scatter pattern surfaced no latent jobs-system issues, but having a test that *would* catch them protects future consumers (eylem v1c broadphase, parallel BVH refit).
2. **Establishes the per-(chunk, bucket) deterministic-stable-merge template.** Future consumers — eylem v1c broadphase, parallel BVH refit, parallel mesh-cooker bake — need exactly this pattern: per-worker output + offset table computed serially in a tiny Phase 2 + disjoint Phase 3 scatter. Building it once here means future consumers wire it up, not reinvent it. The implementation is in `engine/geometry-bvh-gpu/src/morton_sort.cpp` and serves as the reference.
3. **Faster CI test oracle.** The v9a-b2 GPU radix conformance test runs the CPU radix as the byte-identical oracle on every test invocation. Median-of-5 baseline was 4.99 ms (post-prefetch); parallel hits 2.56 ms — that's nearly halved oracle time across many test runs in CI.
4. **API stability preserved.** `sort_morton_pairs<KeyT>(span, alloc)` stays the canonical serial entry point with zero dependency on jobs being init'd. `sort_morton_pairs_parallel<KeyT>(span, alloc, num_jobs, threshold)` is the opt-in parallel sibling. Consumers who explicitly want parallelism (and have crd-jobs init'd) opt in by name. This is what makes it true substrate rather than speculative auto-dispatch.

The 1.86× speedup itself is the *third* most important outcome. The substrate template + jobs-stress validation + faster oracle are #1.

---

## What shipped

### Files

| File | LOC | Purpose |
|---|---|---|
| `engine/geometry-bvh-gpu/include/crd/geometry/bvh_gpu/morton_sort.hpp` | +60 | New `sort_morton_pairs_parallel<KeyT>` declaration + extern templates + `kDefaultParallelSortThreshold = 65536` constant + algorithm doc-block |
| `engine/geometry-bvh-gpu/src/morton_sort.cpp` | +160 | 3-phase parallel radix impl + `parallel_radix_pass<KeyT>` private helper + fallback dispatch |
| `tests/geometry-bvh-gpu/test_morton_sort_parallel.cpp` | +250 | 8 conformance cases / 400 025 assertions including the worker-spanning stability discriminators |

### Edits

- `engine/geometry-bvh-gpu/CMakeLists.txt` — added `crd-jobs` as PUBLIC dependency for the parallel sort entry point.
- `tests/geometry-bvh-gpu/CMakeLists.txt` — added `crd-jobs` link for the jobs-listener pattern.

### API

```cpp
namespace crd::geometry::bvh_gpu {

inline constexpr crd::u32 kDefaultParallelSortThreshold = 65536U;

// Opt-in parallel sort. Falls back transparently to sort_morton_pairs<KeyT>
// (the serial scalar+prefetch path from v9a-b1-simd) when:
//   - crd::jobs not initialised (num_workers() == 0)
//   - num_jobs <= 1
//   - codes.size() < parallel_threshold
//
// Output is BYTE-IDENTICAL to sort_morton_pairs<KeyT> for any input, any
// num_jobs >= 1.
template <typename KeyT>
[[nodiscard]] Array<MortonPair<KeyT>>
sort_morton_pairs_parallel(ConstSpan<KeyT> codes,
                           IAllocator* alloc,
                           u32 num_jobs = 0U,
                           u32 parallel_threshold = kDefaultParallelSortThreshold) noexcept;
} // namespace crd::geometry::bvh_gpu
```

### Algorithm (one radix pass; 4 passes for u32, 8 for u64)

```
Phase 1 (parallel, num_jobs workers):
  each chunk i scans input[i*N/nj, (i+1)*N/nj)
  writes hist_tiles[i][bucket] = local count

Phase 2 (serial, num_jobs * 256 ops):
  bucket_total[B] = sum over chunks of hist_tiles[chunk][B]
  bucket_start[B] = exclusive_prefix(bucket_total)        // global
  for each chunk c, bucket B:
    scatter_off[c][B] = bucket_start[B] + sum_{j < c} hist_tiles[j][B]
  // Chunk c writes to scatter_off[c][B] ..< scatter_off[c+1][B] in bucket B.

Phase 3 (parallel, same chunking):
  each chunk i scatters input[i*N/nj, (i+1)*N/nj)
  using its own scatter_off[i][...] slice (disjoint output range)
```

Stability invariant — the load-bearing piece:
1. Within chunk i: src visited left-to-right, off[bucket] bumped monotonically → equal-key items in the same chunk preserve input order.
2. Across chunks: `scatter_off[i+1][B] = scatter_off[i][B] + hist_tiles[i][B]` strictly → chunk i+1's items in bucket B land AFTER chunk i's items. Combined with (1) → global monotonic-input-index stability holds.

---

## Pinned design decisions for ADR-0076 §25 amendment

- **D152** — Parallelism dispatched via `crd::jobs::parallel_for(num_jobs, num_jobs, fn)` where each fn invocation gets its chunk index as `i = begin`. This is the same pattern as `bvh_build_parallel`; the chunk-index → chunk-range mapping (`i * N / num_jobs` .. `(i+1) * N / num_jobs`) is deterministic across runs even though *which worker thread* runs which chunk is work-stealing (non-deterministic). Our merge keys on chunk index, not worker ID.
- **D153** — Stability across worker boundaries delivered via the per-(chunk, bucket) scatter offset table in Phase 2. The serial Phase 2 cost is O(num_jobs × 256) — negligible vs the Phase 1+3 parallel work. The discriminating tests are 4096-equal-keys-across-all-workers + cross-chunk-equal-keys + byte-identical at num_jobs ∈ {1,2,4,8,16}.
- **D154** — Threshold = 65 536 elements. Below this, per-pass parallel overhead (2 barriers + Phase-2 serial + worker-wakeup cost) exceeds the saving; serial+prefetch wins. Above this, the per-element scatter cost dominates and parallelism converts directly to speedup.
- **D155** — Realistic expected speedup is **bandwidth-bound, not core-bound**. On an 8-core dev box the measured ratio is 1.86× (not 8×) because all cores share L3/RAM. This is the correct number for the algorithm + hardware combination. Future >2× single-machine speedups need either AVX-512 vpscatterdd (different scatter regime) or GPU (already shipped at v9a-b2).

---

## Test corpus (8 cases / 400 025 assertions)

| Section | Cases | Notes |
|---|---|---|
| **CALIBRATION** | 1 | N=16 hand-rolled u32 byte-identical to serial reference. |
| **STABILITY worker-spanning** | 1 | 200 000 all-equal keys spanning all 8 workers → output indices 0..199999 strictly monotonic. |
| **STABILITY cross-chunk** | 1 | Two equal keys placed in chunk 0 + chunk 7; chunk 7's item must land AFTER chunk 0's at the same bucket. |
| **num_jobs-sensitivity** | 1 | 200K random; output byte-identical at num_jobs ∈ {1, 2, 4, 8, 16}. Catches "stable within worker, unstable across workers" silent failures. |
| **Bullet-proof oracle** | 2 | 10 000 u32 + 100 000 u64 random; `bit_compare<MortonPair<KeyT>>` byte-identical to serial reference. |
| **Large oracle** | 1 | 1 048 576 u32 random; byte-identical to serial. The perf-budget workload. |
| **Threshold fallback** | 1 | N=1000 (< 65 536); parallel falls back to serial via the documented path; output identical. |

All cases exercise the parallel path (or document the fallback when below threshold). 400 025 byte-identical assertions across the suite means the deterministic stable merge is correct under every adversarial pattern the v9a-b2 oracle would care about.

---

## Performance (median-of-5, 1 M u32 win-shipping)

| Implementation | Time | vs serial+prefetch | vs original v9a-b1 (5.32 ms) |
|---|---|---|---|
| Original v9a-b1 (no prefetch) | 5.32 ms | 0.94× | 1.00× |
| Serial scalar + prefetch (v9a-b1-simd) | 4.77 ms | 1.00× | 1.12× |
| **Parallel 8 workers (this slice)** | **2.56 ms** | **1.86×** | **2.08×** |

Budget headroom on 1 M u32: **2.56 ms measured vs 20 ms asserted NDEBUG budget = 7.8×** (was 4.0× at v9a-b1-simd close).

The 1 ms target from Karras 2012 is a GPU number, not a CPU target. v9a-b2 GPU radix already hits sub-ms on the same workload. We are not pursuing 1 ms on CPU.

---

## 5-config DoD

```
win-debug          PASS (build+ctest)
win-asan           PASS (build+ctest)
win-shipping       PASS (build+ctest)
win-release        PASS (build+ctest)
win-tidy           PASS (build)
```

`scripts/per-slice-check.ps1 -IncludeRelease -Parallel` — elapsed 00:46.

Full geometry-bvh-gpu binary post-slice: **59 cases / 448 590 assertions** (was 51 cases / 48 565 at v9a-b2 close → +8 cases / +400 025 assertions for v9a-b1-parallel).

---

## What this unlocks

When eylem v1c broadphase, parallel BVH refit, or the cooker LBVH bake hit the wall, they can:

1. Read the v9a-b1-parallel implementation as the reference for 3-phase deterministic-parallel radix.
2. Reuse the per-(chunk, bucket) offset table pattern with their own per-worker output ranges.
3. Reuse the discriminating worker-spanning + num_jobs-sensitivity test patterns to assert stability.

No more "implement it for radix, then implement it for X, then debug why X drifts" — the template is set.

---

## Commit message proposed

```
feat(geometry-bvh-gpu): v9a-b1-parallel — 3-phase parallel radix via crd-jobs

* New `sort_morton_pairs_parallel<KeyT>(span, alloc, num_jobs, threshold)`
  opt-in parallel path. Fallback-clean: returns serial scalar+prefetch
  output when crd::jobs not init'd OR num_jobs<=1 OR N<threshold.
  Default threshold = kDefaultParallelSortThreshold = 65536.
* Algorithm: per-pass 3-phase fan-out.
    Phase 1 (parallel): per-chunk local histogram into hist_tiles[chunk][bucket]
    Phase 2 (serial):   per-(chunk,bucket) scatter offset table — chunk c
                        writes scatter_off[c][B]..<scatter_off[c+1][B]
    Phase 3 (parallel): per-chunk scatter into disjoint output ranges
  Stability across worker boundaries delivered by the Phase 2 offset
  table: chunk i+1's items in bucket B start at scatter_off[i][B] +
  hist_tiles[i][B] > scatter_off[i][B] -> equal-key items from earlier
  chunks land before later chunks (D153).
* num_jobs queried at runtime via crd::jobs::num_workers() when caller
  passes 0; matches bvh_build_parallel pattern.
* Test corpus (8 cases / 400 025 assertions):
    - calibration N=16 byte-identical to serial reference
    - STABILITY 200K all-equal across all 8 workers (indices 0..N-1)
    - STABILITY cross-chunk equal keys preserve input-index monotonicity
    - num_jobs ∈ {1,2,4,8,16} byte-identity (worker-count insensitivity)
    - 10K u32 + 100K u64 oracle byte-identical to serial
    - 1M u32 oracle byte-identical to serial
    - sub-threshold fallback identical to serial
* Measured 1.86x speedup on 1M u32 win-shipping median-of-5:
    serial+prefetch  : 4.77 ms (v9a-b1-simd)
    parallel 8w      : 2.56 ms (this slice)
  Budget headroom: 7.8x (was 4x at v9a-b1-simd close).
* Architectural value: jobs-bandwidth-stress validation + parallel-
  stable-merge template for future consumers (eylem v1c broadphase,
  parallel BVH refit, cooker LBVH bake).
* Pinned D152 (chunk-index dispatch via parallel_for(nj,nj,fn) pattern),
  D153 (per-(chunk,bucket) offset table is the stability invariant),
  D154 (threshold 65536), D155 (1.86x is correct number — bandwidth-bound
  not core-bound) for ADR-0076 §25 amendment.
* 5-config DoD PASS in 46s.
* Catch2 jobs listener pattern reused from test_bvh_parallel.cpp.
* Session log: docs/sessions/2026-05-18-geometry-v9a-b1-parallel-close.md.
```
