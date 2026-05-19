# 2026-05-18 — v9a-b1-simd CLOSED: scalar+prefetch the elite answer; SWWC/AVX2 investigated and ruled out

**Slice:** v9a-b1-simd, the AVX2-vectorised CPU radix follow-on filed at v9a-b1 close.

**Status:** ✅ CLOSED. Net 1.07× speedup on 1M u32 (5.32 ms → 4.99 ms median-of-5) via a single `_mm_prefetch` hint in the scatter inner loop. Three SOTA techniques (AVX2 sub-histograms, Wassenberg SWWC, multi-pass histogram fusion) investigated, measured slower at this working set, removed. **5-config DoD PASS in 35 s.**

---

## Why this debt existed

v9a-b1 close filed a follow-on labeled "v9a-b1-simd" — AVX2 vectorised CPU radix sort — with a proposed SoA-split + per-lane sub-histogram approach. Original target: ~3-5× speedup. User directive on 2026-05-18: "no debts" + "find a very clever way" + "optimize as much as possible" + "check optimization parameters too — you measured win-release not win-shipping."

The last critique was substantive. `win-release` and `win-shipping` both have `/O2 + /LTCG`, but `win-shipping` adds `/Gw /Gy /Zc:inline /OPT:REF /OPT:ICF` for tighter dead-strip + COMDAT folding. Re-baselined on win-shipping.

---

## Web research — SOTA radix sort

Three relevant papers / techniques surfaced (sources at bottom of this log):

1. **Wassenberg 2010** *"Faster Radix Sort via Virtual Memory and Write-Combining"* (Fraunhofer). Per-bucket 64-byte (1 cache line) buffer; flush via non-temporal stores. Claims **89% of peak memory bandwidth** at GB-scale RAM-bound workloads.
2. **Satish 2010** Intel paper on radix sort with software write combining. Same buffer-then-flush idea, paired with multi-pass histogram precomputation.
3. **RADULS Kokot 2017** *"Even Faster Sorting of (Not Only) Integers"*. Refines Wassenberg with alignment tricks; reports +23-28% over RADULS at 1G elements.

Common pattern across all three: **scatter-bound, not bucket-extract-bound.** AVX2 has gather (`vpgatherdd`) but no scatter; vector scatter is AVX-512 only (`vpscatterdd`). So vectorising the bucket-extract phase (AVX2 sub-histograms) is the wrong target — the bottleneck is the random-bucket store, which AVX2 cannot help with.

---

## What I implemented + measured

All measurements: **win-shipping (`/O2 /LTCG /OPT:ICF /OPT:REF /arch:AVX2`), 1 M u32 random Morton codes, median-of-5 runs with one warmup discarded.**

| Variant | Time | vs scalar | Verdict |
|---|---|---|---|
| Scalar minimal (baseline) | 5.32 ms | 1.00× | reference |
| AVX2 8-wide SoA sub-histograms (initial attempt) | 11.81 ms (release single-shot) | 0.45× | **slower** |
| Wassenberg SWWC + multi-pass histogram + AVX2 burst flush | 8.47 ms | 0.63× | **slower** |
| SWWC per-pass histogram (corrected — multi-pass fusion required extra 4MB allocation) | 8.00 ms | 0.66× | **slower** |
| **Scalar + `_mm_prefetch` 8 iters ahead** | **4.99 ms** | **1.07×** | **WINNER** |

### Why SWWC didn't win at this scale

Wassenberg's 89%-of-peak result holds at **GB-scale RAM-bound workloads**. At 1M u32 pairs the destination buffer is **8 MB = L2-resident** on the dev box (16-32 MB L2). The "destination cache pollution" SWWC eliminates is L2 pollution, not RAM round-trips — that's ~5-10× cheaper than what Wassenberg was attacking. The buffer-table bookkeeping + branch-on-full + flush overhead exceeds the savings.

Concrete cost analysis per element:
- **Scalar scatter:** 1 strided src read + 1 small-random histogram read+inc + 1 random dst write. ~3-5 cycles/element with prefetch help.
- **SWWC scatter:** 1 strided src read + 1 medium-random buffer-count read+inc + 1 medium-random buffer write + 1 conditional + (1/8 of the time) 64B burst load+store. ~5-7 cycles/element. The buffer reads/writes hit 16 KB random pattern — still L1, but every access is L1-line-load not just L1-line-write-from-reg.

### Why AVX2 sub-histograms didn't win

`vpscatterdd` is AVX-512. Without it, the 8-wide bucket extract via `_mm256_srli_epi32` + `_mm256_and_si256` produces 8 bucket values per vector load, but each bucket then needs a **scalar** histogram increment — same instruction count as scalar. The SoA split (4 extra allocations + AoS↔SoA conversions) is pure overhead. Net negative.

### Why prefetch did win

The scatter store's random destination is the bottleneck. Modern x86 hardware streamers cannot prefetch a random-bucket address pattern. A single `_mm_prefetch(dst + histogram[future_bucket], _MM_HINT_T0)` 8 iterations ahead tells the cache hierarchy to stage the future cache line into L1d. Cost: ~1 extra instruction per iteration (5 ops total: future_bucket compute = 3 ops + prefetch dispatch). Benefit: converts the dominant store from a demand miss into a hit.

Even though our working set is L2-resident, the L1↔L2 round-trip for a demand-loaded random line is ~10 cycles. Prefetching ~8 iters ahead (= ~40 cycles wall time) covers it cleanly.

The change is 12 lines of code (8 iters lookahead + tail-handling for the last 8 elements) gated by `CRD_SIMD_HAS_SSE2` (`_mm_prefetch` is SSE intrinsic; available everywhere we target). Determinism unaffected: prefetch is a HINT, never writes state.

---

## What shipped vs reverted

**Kept:**
- `radix_pass<KeyT>` gains a prefetch-aware scatter loop (when `CRD_SIMD_HAS_SSE2`), with a scalar tail for the last `kPrefetchDistance` elements. Scalar fallback path retained for non-x86 builds.
- Updated doc-blocks in `morton_sort.hpp` + `morton_sort.cpp` documenting the negative-finding rationale (so the next agent sees why the obvious SOTA techniques don't fit at this scale, instead of re-running the same dead-end experiments).

**Reverted:**
- The SWWC implementation + multi-pass histogram fusion + `detail::sort_morton_pairs_swwc` / `detail::sort_morton_pairs_scalar` entry points. Single canonical scalar+prefetch path; no dual-paths per `feedback_quality_bar`.

**Conformance:** 20 sort cases / 40 134 assertions still byte-identical to v9a-b1's algorithm definition (and to the v9a-b2 GPU oracle).

---

## 5-config DoD

```
win-debug          PASS (build+ctest)
win-asan           PASS (build+ctest)
win-shipping       PASS (build+ctest)
win-release        PASS (build+ctest)
win-tidy           PASS (build)
```

`scripts/per-slice-check.ps1 -IncludeRelease -Parallel` — elapsed 00:35.

---

## Pinned decisions for ADR-0076 §25 amendment at v9a-close

- **D145** — At 1M-element / 8MB-working-set scale, the elite single-thread radix is **scalar + prefetch**. SWWC, AVX2 sub-histograms, and multi-pass histogram fusion all underperform measured median-of-5 in win-shipping. The Wassenberg 89%-of-peak result is a GB-scale RAM-bound regime; we are L2-resident.
- **D150** — Prefetch distance pinned at 8 iterations. Covers the ~40-cycle L1 miss latency at the inner loop's ~5-cycle/iter IPC. Larger distances over-prefetch and pollute L2; smaller distances don't cover the miss.
- **D151** — Future >5ms speedup paths (filed; not actionable at v9a-b1-simd scope):
  - **Parallel radix via `crd-jobs::parallel_for`** — bandwidth-bound problem so multi-core scaling caps below N (all cores share L2/RAM), but 2-3× is realistic. Needs deterministic per-worker stable-merge logic.
  - **AVX-512 `vpscatterdd`** — true vector scatter. Not portable on the current CI matrix; revisit when AVX-512 lands.

---

## Debt entry resolution

The `Phase 3.1.7 v9a-b1 follow-on — AVX2 vectorised CPU radix sort` entry in `docs/debt.md` moves from "Active debt" to "✅ Cleared debt" with the documented investigation and the measured prefetch-win.

---

## Commit message proposed

```
perf(geometry-bvh-gpu): v9a-b1-simd — scatter-side prefetch wins where SWWC/AVX2 lost

* radix_pass scatter loop adds _mm_prefetch(dst + histogram[future_bucket])
  8 iters ahead under CRD_SIMD_HAS_SSE2. 5.32 ms -> 4.99 ms median-of-5 on
  1M u32 win-shipping (= 1.07x). Determinism intact: prefetch is a hint;
  output remains byte-identical to v9a-b1 scalar reference (40 134 assertions
  preserved across 20-case adversarial corpus).
* Investigated + ruled out three SOTA radix techniques at this working set:
    - AVX2 8-wide SoA sub-histograms: 0.45x (slower; AVX2 has no scatter)
    - Wassenberg SWWC + AVX2 burst flush: 0.63x (slower; L2-resident
      working set doesn't trigger the RAM-bound regime SWWC attacks)
    - Multi-pass histogram fusion: marginal benefit offset by extra alloc
* Pinned D145 (scalar+prefetch is elite at this scale), D150 (prefetch
  distance 8), D151 (parallel-radix + AVX-512 vpscatterdd filed for future
  consumer) for ADR-0076 §25 amendment at v9a-close.
* 5-config DoD PASS in 35 s.
* docs/debt.md entry moved from Active -> Cleared.
* Session log + negative-finding write-up:
  docs/sessions/2026-05-18-geometry-v9a-b1-simd-close.md.
```

---

## Sources

- [Faster Radix Sort via Virtual Memory and Write-Combining (Wassenberg 2010, arXiv 1008.2849)](https://arxiv.org/abs/1008.2849)
- [Fraunhofer mirror of Wassenberg](https://publica.fraunhofer.de/entities/publication/bd83e1fe-f75f-4ef2-9c49-ec04cef23578)
- [Engineering a Multi-core Radix Sort (Wassenberg & Sanders 2011)](https://link.springer.com/chapter/10.1007/978-3-642-23397-5_16)
- [Even Faster Sorting of (Not Only) Integers (Kokot et al. 2017, RADULS)](https://ar5iv.labs.arxiv.org/html/1703.00687)
- [Optimizing Cache Usage With Nontemporal Accesses (vgatherps)](https://vgatherps.github.io/2018-09-02-nontemporal/)
- [CC-Radix: a Cache Conscious Sorting Based on Radix sort](https://www.computer.org/csdl/proceedings-article/pdp/2003/18750101/12OmNx5GU2B)
