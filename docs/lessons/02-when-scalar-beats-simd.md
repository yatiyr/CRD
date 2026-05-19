# Lesson 02 — When scalar code beats SIMD

> **The question that motivated this lesson:** "We added AVX2 and SWWC, they were slower than scalar. Why? When does SIMD actually help?"

## TL;DR

**SIMD wins when:**
1. The bottleneck is *arithmetic throughput* (math ops per element, parallelizable in registers).
2. Memory accesses are *streaming* (sequential, prefetcher-friendly).
3. The working set exceeds *cache size* (so memory bandwidth, not latency, is the wall).

**SIMD loses when:**
1. The bottleneck is *random-store latency* (scatter to non-sequential addresses).
2. The working set fits in *L1 or L2* (cache hierarchy already gives you the bandwidth you need).
3. The SIMD pattern requires *scatter* without `vpscatterdd` (AVX-512 only) — you end up with scalar stores in a vector wrapper.

LSD radix sort at 1M elements / 8MB working set is squarely in the "loses" category. We measured it. Three times. Then we found that a single one-line `_mm_prefetch` won where 200 LOC of AVX2 intrinsics lost.

## Part 1 — What we tried and what happened

| Implementation | Time (1M u32 win-shipping) | vs scalar | Verdict |
|---|---|---|---|
| Scalar minimal (reference) | 5.32 ms | 1.00× | baseline |
| AVX2 8-wide SoA sub-histograms | 11.81 ms | 0.45× | **1.55× slower** |
| Wassenberg SWWC + AVX2 burst flush | 8.00 ms | 0.66× | **1.51× slower** |
| Multi-pass histogram fusion | ~10.5 ms | 0.51× | **slower** |
| **Scalar + `_mm_prefetch` 8 iters ahead** | **4.99 ms** | **1.07×** | **WINS** |

Wassenberg's paper claims **89% of peak memory bandwidth** for SWWC radix sort. That's a real, peer-reviewed, well-cited result. So why didn't it work for us?

## Part 2 — Why the SOTA techniques lost

### AVX2 sub-histograms — wrong target

The "obvious" SIMD vectorization of radix sort:
- Load 8 keys at once via `_mm256_loadu_si256`
- Extract 8 buckets in parallel via vector shift + and
- 8 sub-histograms eliminate the dependent-increment chain

What this misses: **the bottleneck isn't bucket extraction.** The compiler already pipelines 8 scalar `shift + and` operations efficiently. The bottleneck is the **store** — the random write `dst[histogram[bucket]++] = pair` that touches a different cache line every iteration.

AVX2 has `vpgatherdd` (vector gather — random reads) but **no `vpscatterdd`** (vector scatter). That's AVX-512. So even with bucket extraction vectorized, the actual scatter has to be scalar. We added complexity (SoA split + sub-histogram merge) to optimize a step that wasn't the bottleneck.

Result: 0.45× speedup. AVX2 was 1.55× **slower** than scalar.

### Wassenberg SWWC — right technique, wrong scale

Software Write Combining is genuine SOTA:
- Maintain 256 small buffers (one per bucket), 64 bytes each.
- Append input items to their bucket's buffer.
- When a buffer fills (8 items for u32), flush via a single 64-byte burst write.

This works because:
- The buffer table (16 KB total) stays L1-resident.
- The actual scatter destination is touched **once per 8 items** instead of 8 times — 8× fewer destination cache transactions.
- Add non-temporal stores (`_mm256_stream_si256`) to bypass cache entirely on flush, and you don't pollute L1d at all.

Wassenberg measured 89% of peak memory bandwidth. So why was it 1.5× slower for us?

**Because we ran it on a working set that fits in L2.** 1 M MortonPair<u32> = 8 MB. The dev box has 16-32 MB of L2/L3 per core. The destination buffer is **already cached**. The "destination cache pollution" SWWC eliminates is L2 pollution, which is ~5-10× cheaper than the RAM round-trips Wassenberg was attacking.

At Wassenberg's scale (GB-sized inputs), SWWC saves: random-bucket store hits L2 miss → L3 miss → RAM access → ~100-200 cycle stall. SWWC's flush hits the same RAM but in a 64-byte burst that the memory controller handles efficiently.

At our scale, the random-bucket store hits L2 (10-15 cycle latency). SWWC's bookkeeping (buffer table lookup, count increment, conditional branch on full, burst flush) costs **more** than that latency. We pay extra to avoid a hit that isn't expensive.

**The rule that emerges:**

> **Microarchitectural optimization papers only apply at the scale they were measured on.** Wassenberg measured at GB scale (RAM-bound). At MB scale (L2/L3-resident), the same technique is overhead. Cite the paper, read the section on measured throughput, *and check whether your working set sits in the same memory tier they did*.

### Multi-pass histogram fusion — small saving, real overhead

Standard 8-bit-digit LSD radix reads the input twice per pass (once for histogram, once for scatter). Across 4 passes that's 8 reads of the input. Fusing all histograms in a single pass saves 3 reads — 12 MB of memory traffic.

At ~30 GB/s effective bandwidth, 12 MB = 0.4 ms saved. **But** the fused histogram has a 4-deep dependent-increment chain per input element (one histogram per pass), which the compiler can't always pipeline as well as 4 independent single-pass histograms. Plus you need an extra 4MB `codes_flat` buffer if you don't want strided reads.

Net: we measured no improvement. Marginal techniques don't show up in noise unless the underlying improvement is large enough.

## Part 3 — What actually worked: `_mm_prefetch`

```cpp
// In the scatter loop, before the actual write to dst:
const usize fut_bucket = (src[i + 8].code >> shift) & 0xFF;
_mm_prefetch(reinterpret_cast<const char*>(dst + histogram[fut_bucket]),
             _MM_HINT_T0);

// Then the real store, 8 iterations later:
const usize bucket = (src[i].code >> shift) & 0xFF;
dst[histogram[bucket]++] = src[i];
```

**Why this wins:**
- The hardware streaming prefetcher cannot anticipate the random-bucket scatter pattern (it only chases sequential strides).
- A demand-load miss (when the store hits an unmapped cache line) stalls the store for ~10 cycles on L2 hit, ~100+ on L3 miss.
- Manually staging the destination cache line 8 iterations ahead (= ~40 cycles wall-time, comfortably covers the L1 miss latency) turns the demand miss into a hit.

**Why this stays correct:**
- `_mm_prefetch` is a **hint**, not a memory operation. It never writes program state. Determinism unaffected.
- The future bucket we prefetch (`src[i+8]`) might not even be the bucket we eventually write to — `histogram[bucket]` has been bumped multiple times between iteration `i` and `i+8`. The prefetched address is **approximate**. That's OK; the actual store lands within 64 bytes (= one cache line) of the prefetched address most of the time. We're staging the cache line, not the byte.

**Cost: 12 lines of code, gated on `CRD_SIMD_HAS_SSE2` (which is everywhere). Benefit: +7% throughput.**

## Part 4 — The general rule

When you face a "the obvious SIMD is slow" puzzle, ask in this order:

1. **What's the working-set size?** Compare to L1 (typically 32-48 KB), L2 (256 KB - 16 MB), L3 (16-256 MB), RAM (GB+). Different tiers have different bottlenecks.
2. **Are the accesses streaming or random?** Sequential reads = prefetcher does the work, SIMD's vector load helps. Random reads = gather (AVX2 OK), or pray. Random writes = scatter (AVX-512 only on Intel/AMD).
3. **Is the bottleneck arithmetic or memory?** If arithmetic, SIMD wins (more math/cycle). If memory, SIMD typically doesn't help (the load/store ports are already saturated).
4. **What does a one-line prefetch do?** Often more than 100 lines of intrinsics.

For Cerid's LSD radix at 1 M elements:
- Working set: 8 MB → L2/L3 resident (not RAM)
- Pattern: sequential reads, **random writes** to destination buffer
- Bottleneck: random-write latency → L2 hit time

`_mm_prefetch` directly addresses the L2-hit-latency bottleneck. AVX2 sub-histograms address a non-bottleneck. SWWC addresses the RAM-bound bottleneck that doesn't exist at this scale.

## Part 5 — When SIMD WILL win for sorting

If our working set grows to GB scale (e.g., billion-primitive scene), SWWC starts paying off. At that point we'd re-implement it — but with measurements showing the regime has changed, not because "SOTA says so." The current code's doc-block explicitly notes this as a future path:

```cpp
// Future paths to >5 ms (not pursued; filed for crd-jobs era):
//   - Parallel radix via `crd-jobs::parallel_for` across `num_threads`
//     workers. Bandwidth-bound problem ⇒ N-core scaling caps below N
//     because all cores share the L2/RAM bus, but 2-4x speedup is real.
//   - AVX-512 vpscatterdd hardware scatter. Not portable to dev box's
//     consumer CPU lineage; revisit when the CI matrix adds AVX-512.
```

The parallel-radix path *did* land (1.86× speedup, see [Lesson 04](04-parallel-stable-merge.md)). It works because parallelism scales bandwidth-bound problems by using more memory controllers' worth of L1/L2 ports — a different mechanism than SIMD.

## What to read next

- [Lesson 03 — Measuring performance correctly](03-measuring-performance-correctly.md) — the median-of-5 discipline that revealed the AVX2-was-slower truth.
- [Lesson 05 — CPU vs GPU performance tiers](05-cpu-vs-gpu-perf-tiers.md) — when even parallel CPU loses to GPU.
