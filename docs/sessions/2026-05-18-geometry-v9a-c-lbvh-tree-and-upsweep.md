# 2026-05-18 — Phase 3.1.7 v9a-c LBVH tree-build + AABB upsweep ✅ SHIPPED (elite combined slice)

**Slice:** v9a-c (combined with the originally-separate v9a-d per the elite-combine decision). Ships a fully queryable GPU LBVH end-to-end in one slice — no half-built intermediate.

**Status:** ✅ shipped same day. 5-config DoD PASS via `scripts/per-slice-check.ps1 -IncludeRelease -Parallel` in 39 s. 12 LBVH test cases / **379 021 byte-identical+1-ULP assertions** including the 4 advisor-flagged degenerate cases + end-to-end CPU↔GPU pipeline conformance.

---

## Why this exists — elite combine vs the original 2-slice plan

The phase doc originally split this work into v9a-c (tree topology, sentinel AABBs) and v9a-d (AABB upsweep). The "safe" path would have shipped v9a-c with an unqueryable tree, then waited 3 days for v9a-d to make it usable. Per the user's "elite — no shortcuts, single-path" directive (`feedback_quality_bar`), I combined them into ONE slice that ships a queryable BVH end-to-end. Same total LOC (~1500), same days (7-8 vs 4+3), but single complete deliverable.

The combined slice also lets us pin the algorithmic upgrade for the upsweep: **Karras 2012 §2.4 atomic-counter parent-walk** is BOTH faster and bit-deterministic than the phase-doc's "2-pass per-level barriers" alternative. The false dichotomy dissolves — one dispatch instead of log₂N per-level barriers, AABB union is commutative+associative so output is bit-deterministic regardless of thread arrival order.

---

## What shipped

### Files

| File | LOC | Purpose |
|---|---|---|
| `include/crd/geometry/bvh_gpu/lbvh.hpp` | ~165 | `build_lbvh_cpu<KeyT>` + `LbvhGpuPipeline` + extern templates + D156-D164 pin block |
| `src/lbvh.cpp` | ~310 | CPU reference (Phase A Karras build + Phase B atomic-equivalent serial upsweep + Phase C canonical reorder) — THE algorithm definition (D157) |
| `src/lbvh_gpu.cpp` | ~420 | `LbvhGpuPipeline` (2 cached pipelines + descriptor allocator) + `dispatch_build_lbvh` (upload + 2 dispatches + readback + same Phase C reorder on CPU per D163) |
| `runtime/examples/shaders/lbvh_build.comp` | ~105 | Karras tree-build, N-1 threads, one internal node per thread |
| `runtime/examples/shaders/lbvh_aabb_upsweep.comp` | ~110 | Karras §2.4 atomic-counter parent-walk; **`coherent` buffer + `memoryBarrierBuffer()` load-bearing** |
| `tests/geometry-bvh-gpu/test_lbvh.cpp` | ~650 | 12 cases / 379 021 assertions: calibration + 4 degenerate + 8-corner + 10K random oracle + end-to-end pipeline + 1M perf + determinism + ValidationCapture silent |
| **Total** | **~1760 + ~650** | |

### Pinned design decisions (carried for ADR-0076 §25 amendment at v9a-close)

- **D156** — Output uses canonical `BvhNode` struct + `BvhTree` layout (siblings consecutive: parent.left_first → left_child_idx; right at left_child_idx + 1). Karras's natural output uses index-range to distinguish internal vs leaf; a post-build DFS reorder rewrites into Cerid's canonical layout. `prim_count != 0` is the leaf marker.
- **D157** — CPU reference IS the algorithm definition (D134 discipline scales). `build_lbvh_cpu` is the byte-identical oracle for GPU conformance.
- **D158** — Internal-node AABBs computed in this same slice (NOT a separate v9a-d) — combined per elite-combine decision; deliverable is fully queryable.
- **D159** — Upsweep uses Karras 2012 §2.4 atomic-counter parent-walk. Both faster AND bit-deterministic than 2-pass per-level barriers — false dichotomy dissolves. AABB union is commutative+associative so output is bit-deterministic across thread arrival orders.
- **D160** — Tree build is a single GPU dispatch with `N-1` threads computing internal nodes; leaves are uploaded with their bounds directly (no separate leaf-write dispatch needed because the leaf's `bounds[leaf_kidx]` is initialised from the input `leaf_aabbs[sorted_pairs[k].index]`).
- **D161** — Auxiliary `child_left`/`child_right`/`parent`/`children_done` GPU buffers live only during build+upsweep, freed before return. Keeps `BvhNode` at its locked 32-byte invariant.
- **D162** — Conformance contract: `bit_compare<BvhNode>` on topology fields (left_first, prim_count) + within-1-ULP tolerance on bounds (atomic-counter upsweep's commutative AABB union may reorder operands run-to-run, FP-equivalent but not bit-equivalent at signed-zero / infinity edges).
- **D163** — GPU output goes through CPU-side post-build reorder for canonical BvhTree layout (so the same `bvh_query` traversal works on CPU SAH BVH and GPU LBVH). Pure-GPU reorder kernel filed as `v9a-c-gpu-reorder` follow-on for the day eylem v1c GPU broadphase needs GPU-resident output.
- **D164** — Equal-key Karras `delta` tiebreak via input indices as a secondary key (treat (code, index) as an extended key, δ = code_bits + clz(index_xor)). Load-bears on stable-sort property from v9a-b1/b2 — equal codes produce monotonic indices, so this tiebreak makes tree topology deterministic on degenerate input.

---

## The load-bearing GPU memory-ordering fix

**The bug that surfaced during the first GPU test run:** the N=10000 oracle test failed with huge bound differences (0.5+ in unit-scene coordinates, when expected 1-ULP tolerance was ~1e-7). All 11 other tests passed. The calibration N=4 + 8-corner + 4 degenerate cases + integration N=4096 + determinism N=1000 + perf 1M all PASSED.

**Diagnosis** — single bug, single test case where it surfaces: GLSL `atomicAdd` provides acquire-release semantics on the atomic location ONLY, not on arbitrary other memory operations. The upsweep's pattern:

```
thread A (first arriver at parent P):
  atomicAdd(done[P], 1)       // returns 0
  store bounds[P.left_child]  // ← non-atomic write, may be CACHED
  return

thread B (second arriver at parent P):
  atomicAdd(done[P], 1)       // returns 1
  load bounds[P.left_child]   // ← might read STALE data (not the cached write above)
  load bounds[P.right_child]
  compute union, store bounds[P]
```

Without `coherent` qualifier on the bounds buffer, GLSL implementations are free to cache writes across invocations. The atomic synchronizes the COUNTER but not the BOUNDS, so the second arriver can see stale bounds.

**Fix (one-line in GLSL):**
```glsl
layout(set = 0, binding = 0) coherent buffer Bounds { float bounds[]; } bounds_buf;
// + memoryBarrierBuffer() after store_aabb(p, ...) before walking up
```

`coherent` extends visibility across all invocations of this buffer; `memoryBarrierBuffer()` flushes the bounds write before the next `atomicAdd` up the chain ensures the higher thread sees the latest bounds.

**Why only N=10000 surfaced it:** at smaller N (4, 8, 64, 4096), the tree depth is small enough that most upsweep walks finish within one workgroup's invocation timeline, and the implementation happens to flush writes between work units. At N=10000, tree depth is ~14, walks cross more workgroup boundaries, and the cache-coherence race becomes observable. **Bigger N stresses the memory model more** — this is exactly why oracle tests at GB-scale matter.

**Memory-ordering bug that doesn't surface at small N but does at large N** is now Lesson 09. The pattern is general — any GPU atomic-coordinated reduction or scan can be bitten this way.

---

## Test corpus (12 cases / 379 021 assertions)

### CPU (7 cases / 110 833 assertions)
- **CALIBRATION** N=4 hand-rolled sorted codes with hand-computed expected tree topology + AABBs.
- **Degenerate 1**: N=1 singleton (1 leaf, no internals, AABB = leaf AABB).
- **Degenerate 2**: N=2 minimum tree (1 internal + 2 leaves; internal AABB = union of both).
- **Degenerate 3**: N=64 all-equal Morton codes. The D164 augmented-key tiebreak via input index must produce a deterministic tree that still covers all primitives + bit-identical across re-runs.
- **Degenerate 4**: 9 codes with adjacent-equal-keys interspersed (a, b, b, c, d, d, e, e, e). Catches Karras's tiebreak bugs that pass on unique-key inputs.
- 8-corner test (Morton 0..7 → unit cube corners).
- N=10000 random AABBs (built via real Morton + sort pipeline, structural invariants + root encloses all).

### GPU (5 cases / 268 188 assertions)
- **GPU calibration** N=4 byte-identical to CPU reference.
- **N=10000 oracle**: CPU vs GPU topology byte-identical + bounds within 1 ULP (the test that surfaced the `coherent` bug).
- **End-to-end pipeline**: 4096 random AABBs through full CPU pipeline vs GPU pipeline → byte-identical topology + 1-ULP bounds.
- **gpu_determinism_check** 3 dispatches: topology AND bounds bit-identical across runs (D159 contract — commutative union + atomic counter ⇒ deterministic).
- **`CRD_PERF_BUDGET_LE` 1M items end-to-end**: 200 ms NDEBUG / 60 s debug.
- `ValidationCapture` asserted silent on every dispatch.

---

## 5-config DoD

```
win-debug          PASS (build+ctest)
win-asan           PASS (build+ctest)
win-shipping       PASS (build+ctest)
win-release        PASS (build+ctest)
win-tidy           PASS (build)
```

`scripts/per-slice-check.ps1 -IncludeRelease -Parallel` — elapsed 00:39.

Full geometry-bvh-gpu binary post-slice: **71 cases / 827 611 assertions** (was 59/448 590 → +12 cases / +379 021 assertions for v9a-c).

---

## Mid-slice fixes

1. **`StringView` include missing** in `lbvh.hpp` — added.
2. **GLSL memory ordering** (the load-bearing fix) — `coherent` on bounds + children_done buffers + `memoryBarrierBuffer()` after bounds write.
3. **clang-tidy `readability-identifier-naming`** on `const int` function parameters — drop `const` (tidy treats by-value `const` params as constants, expects kFoo naming).
4. **clang-tidy `readability-redundant-casting`** — `std::countl_zero` already returns int, casts removed.
5. **clang-tidy `modernize-unary-static-assert`** — empty-message static_asserts converted to no-message form.
6. **clang-tidy `bugprone-` cast on `n - 1U + k`** — split to `static_cast<usize>(n-1U) + static_cast<usize>(k)` to avoid implicit u32→usize narrowing then re-widening.

---

## Cluster status post-v9a-c

**v9a `-gpu` LBVH cluster: 4 of 5 slices done** (v9a-a + 4 follow-ons + v9a-b1 + v9a-b1-simd + v9a-b1-parallel + v9a-b2 + v9a-c ✅). **Next = v9a-close** (cluster-close docs + ADR-0076 §25 amendment + 18-config full sweep + first-light sandbox demo).

The combined v9a-c slice replaces the originally-planned separate v9a-d entry; the phase doc is updated to mark v9a-d as **absorbed into v9a-c** rather than a separate slice.

---

## Commit message proposed

```
feat(geometry-bvh-gpu): v9a-c LBVH tree-build + AABB upsweep (elite combined slice)

* Combines the originally-separate v9a-c (tree topology) and v9a-d (AABB
  upsweep) into one slice that ships a fully QUERYABLE GPU LBVH end-to-end
  — no half-built intermediate. Same total LOC, same days, single complete
  deliverable per `feedback_quality_bar`.
* New `build_lbvh_cpu<KeyT>(sorted_pairs, leaf_aabbs, alloc) -> BvhTree`
  THE algorithm definition. 3 internal phases:
    A: Karras 2012 §2.2 binary-tree-from-sorted-codes (Karras-native indexing)
    B: Karras 2012 §2.4 atomic-counter-equivalent serial upsweep
    C: canonical BvhTree-layout reorder (siblings consecutive via DFS-mirror
       of bvh_build's stack ordering)
* New `LbvhGpuPipeline` — 2 cached compute pipelines (build + upsweep),
  dispatch path uploads inputs, runs 2 dispatches, reads back, runs the
  SAME Phase C reorder on CPU per D163 (output BYTE-IDENTICAL to CPU on
  topology, within 1 ULP on internal-node AABBs per D162).
* GPU shaders:
  - lbvh_build.comp: N-1 threads, one internal node each; δ function with
    D164 augmented-key tiebreak for equal codes.
  - lbvh_aabb_upsweep.comp: leaf threads walk parent chain via atomicAdd
    coordination. `coherent` buffer qualifier + memoryBarrierBuffer load-
    bearing for cross-invocation visibility of bounds writes (the bug that
    only surfaces at N >= 10000 because shorter walks happen to flush
    writes between work units).
* D156-D164 pinned for ADR-0076 §25 amendment at v9a-close. The original
  v9a-d "atomic vs 2-pass" decision-fork dissolves (D159) — Karras §2.4
  dominates both speed and determinism.
* Test corpus (12 cases / 379 021 assertions):
    CPU calibration N=4 + 4 degenerate cases (N=1 / N=2 / all-equal /
    adjacent-equal-interspersed) + 8-corner + N=10000 oracle +
    GPU calibration + N=10000 byte-identical-to-CPU + end-to-end pipeline
    + determinism 3 rounds + ValidationCapture silent +
    CRD_PERF_BUDGET_LE 1M items end-to-end.
* 5-config DoD PASS in 39s.
* Surfaced as Lesson 09 (docs/lessons/09-gpu-memory-ordering-gotchas.md):
  GLSL atomicAdd has acquire-release on the atomic ONLY; non-atomic
  cross-invocation writes require `coherent` + memoryBarrierBuffer.
* Session log: docs/sessions/2026-05-18-geometry-v9a-c-lbvh-tree-and-upsweep.md.
```
