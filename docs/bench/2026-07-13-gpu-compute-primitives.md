# B-cmp GPU compute primitives — CUB/cuBLAS gold board (2026-07-13)

The compute-primitive crush campaign (after the FFT: `2026-07-13-gpu-fft-cufft-gold.md`). Each B-cmp primitive is authored in
CKIR (portable, bit-exact CPU oracle + Vulkan + DX12) and benched against NVIDIA's production library. Machine: **RTX 4070 Ti
SUPER** (Ada AD103, ~672 GB/s DRAM, **48 MB L2**), CUDA 13.3, ⚠ UNLOCKED clocks (re-lock for a headline). GPU-timed, min-of-N.

> ⛔ CUDA-13.3-on-this-driver gotcha: nvcc's default PTX JIT fails with *"the provided PTX was compiled with an unsupported
> toolchain"* (driver older than the 13.3 toolkit). Build CUB/cuBLAS benches with **`-arch=sm_89`** (direct Ada SASS, no PTX
> JIT) + `-std=c++17 -Xcompiler /Zc:preprocessor` (CCCL requires both). The cuFFT DLLs worked because they ship multi-arch SASS.

## ⭐ REDUCTION — CKIR `build_reduce` vs CUB `DeviceReduce::Sum` ✅ WIN (2026-07-13)

A device-wide reduction is MEMORY-BOUND (reads N once, emits one scalar) ⇒ the metric is achieved DRAM/L2 bandwidth. Ours is a
2-pass plan (`ckir_reduce.hpp`): pass 0 = a grid of workgroups, each SERIALLY pre-reduces its span (block-strided/coalesced,
`per_thread = 8`) then a log₂(threads) shared TREE combine → one partial; pass 1 = one workgroup reduces the partials → scalar.
Vendor = `bench/gpu-compute/cub_reduce_bench.cu` (CUB `DeviceReduce::Sum`, f32, wall-clock over a 200-reduce batch, min-of-20).
Ours = `[.reduce-bench]` in `tests/gpu-context-vulkan` (GPU-timed `last_gpu_ms` bracketing both dispatches, min-of-30). Both
self-verifying (input all-1.0 ⇒ sum = N, exact in f32 for N = 2ᵏ).

| N | size | regime | **OURS** | CUB | **speedup** |
|--:|--:|:--|--:|--:|:--:|
| 4.19M (2²²) | 16 MB | L2-resident | **0.00957 ms · 1753 GB/s** | 0.01360 ms · 1233 GB/s | **1.42×** ✅ |
| 16.7M (2²⁴) | 64 MB | DRAM-bound | **0.10643 ms · 630 GB/s** | 0.11136 ms · 602 GB/s | **1.05×** ✅ |

**We beat NVIDIA's production reduction.** Decisively L2-resident (1.42× — our minimal 2-pass streams at higher effective L2
bandwidth than CUB's more general kernel), and parity+ at the DRAM wall (**94 % of the 672 GB/s peak vs CUB's 90 %** — a simple
coalesced streaming reduce leaves almost nothing on the table). Same doctrine as the FFT: memory-bound ⇒ the win is in
achieving bandwidth, and a specialized single-purpose kernel edges the general-purpose vendor library. Bit-exact (sum) /
order-invariant (min/max) on the **CPU oracle + Vulkan + DX12** (`[kir][kernel][reduce]` + `[gpu][kernel][reduce]` both
backends). CKIR-pure — the serial+tree combine is plain index arithmetic + `If`-guarded shared writes, every backend identical.

## SCAN — CKIR `build_scan` vs CUB `DeviceScan::InclusiveSum` ◧ CORRECT + PORTABLE, loses on the multi-pass tax (2026-07-13)

A device-wide prefix sum, portable + deterministic ⇒ a NO-ATOMICS **3-pass** plan (`ckir_scan.hpp`): pass 0 `build_scan_block`
(COALESCED striped global I/O + a BLOCKED shared scan: per-thread local scan → Hillis-Steele cross-thread scan of the thread
totals, branchless `Select`+clamped-index, `Materialize` across the barriers → local scan + block total) → pass 1 exclusive-scan
the block totals → pass 2 `build_scan_addoff` add each block's offset. Inclusive + exclusive. Bit-exact on the **CPU oracle +
Vulkan + DX12** (`[kir][kernel][scan]` + `[gpu][kernel][scan]`; unit input ⇒ prefix sums exact in f32).

| N | size | regime | OURS 3-pass | CUB | ratio |
|--:|--:|:--|--:|--:|:--:|
| 4.19M | 16 MB | L2-resident | 0.0622 ms | 0.0212 ms | 0.34× |
| 16.7M | 64 MB | DRAM-bound | 0.4255 ms · **630 GB/s actual** | 0.2271 ms · 591 GB/s | **0.53×** |

**Honest: this is the first primitive we do NOT crush — and the reason is STRUCTURAL, not kernel inefficiency.** Our kernels hit
**630 GB/s actual (94 % of the 672 peak — identical to our reduce)**; we simply move **4N** bytes (pass 0 reads N + writes the
local scan N; pass 2 reads N + writes N) where CUB's SINGLE-PASS decoupled-look-back moves **2N** (read once, write once) using
device-scope ATOMICS + a forward-progress spin. A portable, deterministic scan cannot be single-pass without those. The portable
floor is **3N** (a 2-pass reduce-then-scan that re-reads the input instead of writing the local scan ⇒ ~0.67× — the next lever);
true 2N parity needs CKIR atomics + forward progress (a future device capability, like FFT's no-FMA handicap but structural).
⛔ two scars fixed here: (1) CKIR re-reads shared LAZILY ⇒ a running accumulator that reads-then-writes the same array corrupts
(the FFT ping-pong scar) — freeze the originals with `Materialize` first; (2) the shared `dispatch_kernel_1wg` harness was
MISSING a TransferDst→ShaderRead barrier between upload and dispatch — a latent RACE that only a FAST kernel (the add-offset map,
no shared/barrier) exposed (it read zeros); slower shared-mem kernels masked it by luck. Both fixed.

**SINGLE-PASS attempt (the crush path) — built + oracle/GPU-correct, but the CHAINED variant SERIALIZES (2026-07-13):** to reach
2N traffic (parity with CUB) a scan must be SINGLE-PASS, which needs inter-block communication ⇒ **atomics + a spin-wait**. Built
that substrate in CKIR — `buffer_decl_coherent` (`coherent volatile` GLSL / `globallycoherent` HLSL) + a new
`KStmtKind::SpinUntilNonzero` (all 5 emitters + the oracle) — and a `build_scan_single_pass` (chained look-back: each block scans
its span, SPINS on `flag[bid-1]`, reads the predecessor's published prefix, publishes its own). **Bit-exact on the CPU oracle
(sequential workgroups ⇒ the spin is a no-op) AND correct on Vulkan (out[0]=1, out[N-1]=N — forward progress + coherence WORK,
no deadlock)** — the substrate is proven. BUT the *chained* form SERIALIZES the prefix propagation (block i waits for i-1's FULL
prefix) ⇒ **48–53 GB/s (~8 % peak) = 0.03–0.08× CUB.** The FAST single-pass is CUB's **decoupled look-back** (a block looks back
at predecessor AGGREGATES, published immediately, without waiting for the full chain) — which needs an EARLY-EXIT look-back loop
(CKIR's imperative body has bounded `For` but no `break`) + a per-block state machine (NOT_READY/AGGREGATE/PREFIX). ⛔ scar: GPU shared memory is NOT zero-initialized (the oracle zeros it, masking the bug) — the block-0
`sh_excl` was garbage until explicitly written 0.

**⛔⛔ FUNDAMENTAL CONCLUSION — a BIT-EXACT portable scan CANNOT crush CUB (investigated exhaustively 2026-07-13):** CUB's speed
comes from **decoupled look-back**, where a block sums its exclusive prefix from whatever predecessor states (AGGREGATE vs
PREFIX) happen to be ready — a **TIMING-DEPENDENT summation order** ⇒ non-deterministic f32 rounding ⇒ **NOT bit-exact, not even
run-to-run reproducible.** That directly violates our ⭐⭐ mission (all backends bit-exact). The two bit-exact single-pass forms
BOTH lose: **chained** (block waits for predecessor's full PREFIX) serializes the prefix propagation → 0.03–0.08× (measured);
**all-aggregate** (sum ALL predecessor aggregates in fixed ascending order — bit-exact, matches the 3-pass) is O(nblocks²) +
wave-serialized → even slower (measured 0.015–0.019×, and buggy on GPU). So scan is the ONE primitive where bit-exactness +
portability STRUCTURALLY forbid a crush — analogous to FFT's no-FMA handicap, but here it forbids the whole algorithm class, not
just an arithmetic mode. **Verdict: the portable bit-exact scan is the 3-pass (0.53×) / 2-pass-3N (~0.67×, the next lever).**
**Kept:** the coherent/spin substrate (`buffer_decl_coherent` + `SpinUntilNonzero`, proven on GPU — reusable for sort/histogram/
compaction where the atomics need not be bit-exact) + the correct chained single-pass as the demonstration.

## ⭐ SORT — CKIR stable LSD radix sort ◧ FULL SYSTEM on GPU; crush needs the parallel-rank scatter (2026-07-13)

A sort is a PERMUTATION (no float arithmetic) and a STABLE radix sort has deterministic tie-breaking ⇒ bit-exact by construction
— so unlike scan, sort has NO mission conflict and CAN crush. Built the full pipeline in `ckir_sort.hpp` (+ a `SharedAtomicAdd`
primitive, all 5 emitters + oracle): **[✅] histogram** (per-block 8-bit digit counts via shared atomic-add — the count is
order-independent ⇒ bit-exact) · **[✅] offset scan** (per-bin block-prefix via a runtime `For` + Hillis-Steele of bin totals →
exact bin-major global prefix, partitions [0,n)) · **[✅] scatter** (stable local rank + scatter). **The full 4-pass sort
DISPATCHES on Vulkan end-to-end (histogram→offset→scatter ping-pong) — output fully SORTED + valid permutation (XOR+sum)**;
bit-exact on the CPU oracle too (`[kir]`+`[gpu][kernel][sort]`). ⛔ 2 scars: `stmt_for_begin` takes a VALUE NODE not a C++ int
(int ⇒ runaway loop = hang); the scatter's `rk` fed both `s_cnt[d]=rk+1` and `dest=off+rk` — lazy re-read gave `dest=off+rk+1`,
shifting the output by 1 (gap at 0) → `stmt_materialize(rk)` before the increment.

**Vendor gold** (`bench/gpu-compute/cub_radixsort_bench.cu`, CUB `DeviceRadixSort::SortKeys` u32, min-of-N):

| N | CUB min_ms | CUB Gkeys/s | eff GB/s (8N) |
|--:|--:|--:|--:|
| 1M | 0.0794 | 13.2 | ~423 |
| 4M | 0.1851 | 22.7 | L2-ish |
| 16.7M | **1.0546** | 15.9 | **~508 (76% peak)** |

**PARALLEL-RANK scatter built + measured — CORRECT but COMPUTE-BOUND (0.056×), the crush is an engineering gap not a wall.**
Replaced the serial thread-0 rank with a DETERMINISTIC SEGMENT-TILED parallel rank (`build_sort_scatter`, S=16 segments): each
segment-thread serially ranks its `elems/S` keys into a per-segment 256-histogram `seg[S·nbins]` (16 KB), then a cross-segment
exclusive scan per digit gives block-rank = `seg_base[s][d] + within_segment_rank` — stable ⇒ bit-exact (matches the serial
rank + oracle). Bench (`[.sort-bench]`, 16.7M keys, GPU-timed min-of-N):

| | ms | Mkeys/s | vs CUB |
|---|--:|--:|--:|
| CUB DeviceRadixSort | 1.055 | 15 900 | 1× |
| OURS (4-pass, S=16 rank) | **18.87** | 889 | **0.056×** |

### ⚠ CORRECTION (2026-07-13, later) — the "18× / 0.056×" above was a MEASUREMENT ARTIFACT, not the kernel

Two bugs corrupted the first sort bench: (1) a **67 MB host→device staging `copy` inside the timed region** (PCIe-bound,
~5 ms) and — far worse — (2) a **silently-failing build**: `getenv()` in the diagnostics tripped MSVC `C4996` under `/WX`, so
every "isolation" run for hours executed a **stale binary**. Both are now fixed (copy hoisted to its own untimed submit;
`getenv` removed). With **confirmed-fresh builds**, GPU-timestamp and CPU wall-clock **agree**, and per-kernel isolation
(`diag` compile-time switch, batch of 32) gives the TRUE breakdown for 16.7 M keys, 4 passes, epb=2048:

| config | ms/sort | ⇒ kernel cost |
|---|--:|--|
| empty submit / barriers-only | 0.04–0.05 | (no per-submit floor; 12 barriers are ~free) |
| skip histogram | 14.9 | **histogram ≈ 0** |
| skip scatter | 9.2 | **scatter ≈ 5.7 ms** (2-level rank) |
| skip offset | 4.8 | **offset ≈ 10 ms** ← dominant |
| FULL | 14.9 | 1122 Mkeys/s vs CUB 15 900 = 0.07× |

**The bottleneck is the OFFSET kernel (~10 ms), not the rank.** `build_sort_offsets` runs in **ONE workgroup** and loops over
`nblocks`=8192 twice with GLOBAL loads/stores — single-WG unhidden-latency-bound. It processes only 2 M elements (≪ N) so a
proper PARALLEL scan should take ~0.4 ms: a **>20× kernel win** and the clear next step (bin-major histogram ⇒ the offset is a
flat device exclusive-scan, which we already have). The histogram (shared atomic-add) is essentially free. The **2-level
parallel rank works and is bit-exact** but the scatter is still compute-bound (~1.4 ms/pass vs ~0.2 ms memory-bound).

**Ceiling analysis — the crush IS reachable, but gated on THREE things (one a missing IR feature):** CUB's default is *onesweep*
— ONE fused histogram pass (all 4 digits) + 4 decoupled-lookback scatter passes = **~9 N** traffic at ~572 GB/s ⇒ 1.055 ms. To
beat it we need (1) **fused histogram** (compute all 4 digit-histograms in one N-read pass → 9 N, not our current 12 N); (2) a
**parallel offset** (the ~10 ms single-WG scan → ~0.4 ms); and (3) a **memory-bound rank**. At our proven 630 GB/s, 9 N = 603 MB
⇒ **0.96 ms = 1.1× over CUB** — a real crush — *if the rank is free*. (1) and (2) are ordinary engineering. (3) is the wall: our
deterministic 2-level rank is compute-bound (~1.4 ms/pass); CUB's rank is a near-free warp `ballot`/`match`. **CKIR has no
subgroup ops** — so a cheap bit-exact rank needs a new IR capability (subgroup `ballot`/`add`, which ARE deterministic ⇒
bit-exact). So unlike scan (a TRUE fundamental conflict — the fast algorithm is inherently non-bit-exact), the sort crush is
gated on a **buildable CKIR feature (subgroups)** + histogram fusion + a parallel offset. **Today: full portable bit-exact sort
SYSTEM DONE + correct; parity/loss on perf (offset unoptimized). The crush is a bounded feature-add away, not a wall.** The
DRAM-bound FFT (1.99×/1.16×), reduction (1.42×), and R2C (2×) crush TODAY with no new features.

### ⭐ CRUSH CAMPAIGN round 2 (2026-07-13, serial deep-profile) — 4.40 → **2.41 ms (0.437×)**, every lever measured

**Methodology fix first:** the skip-a-kernel diag is CONTAMINATED (downstream kernels get stale data); the correct isolation is
the `[.sort-kprof]` STANDALONE profiler — each kernel batch-timed alone with VALID precomputed inputs. That immediately
overturned the previous session's inference: the offset kernels were NOT ~10 ms and the histogram was NOT free.

| lever (in order) | kernel | before → after (ms/pass) | why |
|---|---|--:|---|
| — baseline profile | hist / off_l / off_g / scatter | 0.121 / 0.177 / 0.123 / **0.814** | scatter = 65%: SCATTERED 4B writes (≤1 line/key) |
| **local reorder** (CUB structure): register-stage key+digit+rank over the rank rounds, reorder in shared, COALESCED run writes | scatter | 0.814 → **0.429** | write runs per digit instead of scatter |
| epb=4096 / 512-thread / tagged-2-barrier variants | scatter | 0.69 / 0.53 / 0.66 — **all WORSE** | rounds↑ or shared↑ (occupancy) dominate; empirics beat models |
| **gbase fold**: offset_gbase (8 MB rewrite) → 1-WG totals-scan `gb[256]`; scatter adds gb[d] at dest | off_g | 0.123 → **0.001** | the full-buffer add was pure waste |
| **bin-major layout**: histogram writes out[bin·nblocks+blk] (L2-coalesced across blocks); off_l reads+writes its bin column CONTIGUOUSLY | off_l | 0.177 → **0.041** | transposed 4B gathers → streamed |

**Final: 2.41 ms / 6957 Mkeys/s vs CUB 1.055 = 0.437×** (session total 14.9 → 2.41 = 6.2×), bit-exact oracle+Vulkan green.
Profile now: **scatter 0.432 (72%)** + hist 0.125 + off_l 0.041 + gb 0.001 = 0.60/pass.

**Reverse-engineered remaining gap to CUB (quantified):** CUB's whole sort = 1.055 ⇒ their scatter ≈ 0.236/pass on THIS GPU.
Ours is 0.43: ≈0.21 memory (r+w floor) + ≈0.22 rank machinery (8-ballot match emulation ×N + 24 barriers + serial-8 seg scan).
The two structural levers left, in expected-value order:
1. **`subgroupPartitionNV`** (VK_NV_shader_subgroup_partitioned — hardware match_any, supported on this RTX): one op replaces
   the 8-ballot loop; IR op `SubgroupMatch` lowered per backend (partition on NV, ballot-loop elsewhere — SAME mask ⇒ bit-exact
   both paths). Est. scatter → ~0.30 ⇒ total ~1.9 (0.55×).
2. **ONESWEEP** (integer decoupled-lookback): u32 count sums are ORDER-INDEPENDENT ⇒ lookback IS bit-exact for sort (the f32
   scan wall does NOT apply). Kills hist+off_l per pass (−0.54 total): one fused global hist + 4 lookback-scatters ⇒ est.
   ~1.3-1.5 ms (0.7-0.8×). Needs `stmt_for_break_if` IR (For + per-thread break; oracle = per-iteration active-set filter) +
   coherent publish (already have) + spin (already have).
Both together + tile tuning ⇒ ~1.05-1.2 ms (0.88-1.0×) — parity band; >1× additionally needs CUB-class scatter efficiency
(their CUDA 99KB-shared 11K-key tiles vs our Vulkan 48KB cap). Sort remains PARITY-class portable; the gap is now fully
itemized, not mysterious.

### ⭐⭐ ONESWEEP LANDED (2026-07-14) — integer decoupled-lookback, bit-exact: **2.20 ms = 0.48× CUB** (best portable result)

Both remaining structural levers were built and MEASURED:

**Lever A — `SubgroupMatch` (hardware `subgroupPartitionNV`): built, bit-exact, MEASURED SLOWER — reverted from the hot path.**
New IR op (oracle lane-compare + GLSL `subgroupPartitionNV(x).x` + `VK_NV_shader_subgroup_partitioned` device enable in
vulkan_context). Scatter 0.432 → 0.586 ms/pass: the 8 independent ballots PIPELINE on Ada; the partition op serializes.
The op stays in CKIR (validated primitive); the sort keeps the ballot-loop match. Empirics beat vendor-intrinsic intuition.

**Lever B — ONESWEEP: built, works, the new best.** Legal because u32 count sums are ORDER-INDEPENDENT — the lookback's
timing-dependent arrival order yields identical bytes (the f32 scan wall does not apply; a keys-only u32 sort's output is
pinned uniquely by sorted+permutation). New IR: `KStmtKind::ForBreakIf` (per-thread break out of a kernel For; oracle =
per-iteration active-set filtering; all 5 emitters) + `KStmtKind::BufferAtomicAdd` (all 5 emitters; HLSL byte-addressed).
New kernels: `build_sort_ghist` (fused 4-digit global histogram, ONE N-read), `build_sort_clear`, grid-indexed
`build_sort_gbase` (grid=4), `build_sort_scatter_onesweep` (rank rounds → publish (count<<2|status) to a coherent look cell →
walk back: spin, add, BREAK on prefix → publish own prefix → reorder → coalesced write with dest = gb[pass][d]+excl[d]+rank).
Per sort: clear + ghist + gbase4 + 4 lookback scatters = 7 dispatches (was 13).

| path | ms | Mkeys/s | vs CUB 1.055 |
|---|--:|--:|--:|
| 4-kernel (hist+off_l+gb+scatter) | 2.61 | 6437 | 0.40× |
| **ONESWEEP** | **2.20** | **7440** | **0.48×** |

Session trajectory: 14.9 → 2.20 ms (**6.8×**), 0.07× → 0.48× CUB — all bit-exact (oracle + Vulkan, 650+151 assertions).
**Honest verdict: the crush (>1×) was NOT reached.** The remaining gap is now a single item: the scatter's rank machinery
(~0.22 ms/pass of ballots + 24 barriers + serial-8 seg scans) that CUB hides inside CUDA-only resources (99 KB shared →
11K-key tiles → 3× fewer rank phases per byte moved; hardware match in their tuned SASS). Within Vulkan's 48 KB shared cap and
our measured variant space (epb 1-4K, threads 256-512, tagged/partition/reorder), 2.2 ms is the empirical frontier; the
theoretical floor of the onesweep structure at our bandwidth is ~1.3-1.5 ms (0.7-0.8×) with a rank ~2× cheaper than ours —
paths there: A/B paired-round ranks (halve barriers at +8 KB shared) or a CUDA-backend bench (99 KB tiles) where the same IR
could legitimately close to parity+. Sort stands as PARITY-CLASS portable; FFT/reduction/R2C remain the genuine crushes.

### Final micro-round (2026-07-14) — the issue-bound frontier, measured

Three more reverse-engineered hypotheses, each MEASURED on the onesweep scatter:
- **Software-pipelined loads** (all pt DRAM loads hoisted above the round barriers → pt in flight): **no change** (2.20→2.27,
  noise) ⇒ DRAM latency was already hidden by occupancy; the rank phase is INSTRUCTION-ISSUE-bound, not latency-bound.
- **Branchless XOR match** (`keep = bal ^ (bitv−1)` replaces compare+select, −2 ops/ballot ×32/key): **~1.5%** (2.27→2.24)
  ⇒ the driver was already optimizing the select; the residual cost is the 8 VOTE ops + 4 barriers/round themselves.
- (Earlier) `subgroupPartitionNV` hardware match: **slower** (likely driver-emulated on consumer Ada).

**FINAL: ONESWEEP 2.25 ms / 7469 Mkeys/s = 0.47× CUB.** The portable-Vulkan frontier for a bit-exact 8-bit radix sort on this
GPU: 8 ballots/key is the information-theoretic minimum for a ballot-based 256-way match, 4 barriers/round is the minimum for
the cross-subgroup stable scan, and Vulkan's 48 KB shared cap forbids CUB's 11K-key tiles that amortize rank 3× further. The
one remaining credible >1× vector: run THIS SAME IR through the CUDA backend (99 KB shared opt-in, native warp intrinsics,
identical bit-exact semantics) — a fair fight on CUB's own turf.

### ⭐⭐ CUDA-BACKEND CAMPAIGN (2026-07-14) — same IR on CUB's turf: **1.77 ms = 0.59× CUB** (new best)

The CKIR onesweep, emitted via `emit_compute_kernel_cuda` (tool test `[.emit-cuda-sort]` → `bench/gpu-compute/
ckir_onesweep_gen.cu`; driver `ckir_onesweep_bench.cu` runs CKIR configs + CUB in ONE binary, same method). Getting it to run
surfaced three hard scars (each is now a memory + doctrine):
1. **`*_sync` in divergent flow = data-dependent warp HANG**: the CUDA emitter lazily inlines values — `popc(match(d))` used
   inside the leader-if executed a full-mask `__match_any_sync` with inactive lanes. Random data passed; sorted-ish pass-3
   data (fewer leader lanes) hung. Diagnosed with device-printf traces injected into the GENERATED source. Fix:
   `stmt_materialize(mask)` in uniform flow (both scatters — GLSL was immune only because it auto-temps subgroup ops).
2. **`blockIdx`-based lookback DEADLOCKS on Ada** (launch order unguaranteed) → new IR `BufferTicket` (block-scoped
   `atomicAdd` ticket = virtual block id; residents always hold the lowest unprocessed ids). All 5 emitters + oracle.
3. **`__threadfence()` per spin iteration = L2 livelock** → volatile re-read + `__nanosleep(64)`.

| config (CUDA) | ms | Mkeys/s | vs CUB |
|---|--:|--:|--:|
| ONESWEEP-A (epb 2048) | **1.775** | 9453 | **0.59×** |
| ONESWEEP-B (epb 4096) | 1.82 | 9204 | 0.57× |
| ONESWEEP-C (epb 8192, 41 KB shared) | 2.16 | 7764 | 0.48× — 32 barrier-rounds lose |
| CUB DeviceRadixSort (same binary) | 1.043 | 16088 | 1× |

CUDA backend beats Vulkan (2.20) by 24% — native `__match_any_sync` + stream-ordered barriers. All configs sorted+permutation
verified (= bit-exact for keys-only u32). Campaign total: **14.9 → 1.77 ms (8.4×)**, 0.07× → 0.59× CUB.
**Remaining quantified lever to parity/crush: the warp-synchronous rank** — CUB ranks a whole tile with ~3 `__syncthreads`
(per-warp digit counters accumulated warp-synchronously + ONE block scan) vs our 32 (4/round × 8 rounds). Needs a
syncwarp-safe warp-accumulate pattern in CKIR (new `SyncWarp`/warp-scoped statement tier) — the specified next increment.

### ⭐⭐⭐ WARP-SYNCHRONOUS RANK (2026-07-14) — the CUB structure in CKIR: **1.42 ms = 0.735× CUB** (campaign 14.9 → 1.42 = 10.5×)

New IR tier: **`KStmtKind::SyncWarp`** (CUDA `__syncwarp` / GLSL `subgroupBarrier` / MSL `simdgroup_barrier`; HLSL+WGSL lower to
the conservative block barrier; oracle = commit). The onesweep scatter's rank was rebuilt as CUB's warp-synchronous structure:
warp `sg` owns the CONTIGUOUS chunk [sg·cpw,(sg+1)·cpw) — position order == (warp, round, lane) == rank order ⇒ STABILITY
preserved — and accumulates per-digit counters in seg[sg][·] across its rounds with only 2 warp-syncs/round and **ZERO block
barriers in the rank loop** (was 4/round). One cross-warp scan per digit then yields warp bases; rank = wbase + within-warp
running rank. Block barriers per scatter: 33 → ~6. Also: per-config clear grids; digr register array dropped (recompute).

| config | Vulkan | CUDA | vs CUB (1.041-1.047, same binary) |
|---|--:|--:|--:|
| onesweep A (epb 2048) | 1.87 ms | 1.62 ms | 0.64× |
| **onesweep B (epb 4096)** | — | **1.415 ms / 11 852 Mk/s** | **0.735×** |
| onesweep C (epb 8192) | — | 1.54 ms | 0.68× |

Bigger tiles finally PAY (barrier-free rounds); B is the sweet spot. All configs sorted+permutation verified (keys-only u32 ⇒
bit-exact by uniqueness). Register pressure measured (75-111 regs, zero spills). **Remaining 1.36× gap, itemized:** ghist
~0.12 (fused 4-digit N-read + 268M shared atomics), clear ~0.03, and ~0.08/pass scatter overhead above the 0.21 memory floor
(16 match ops + reorder shared traffic + lookback + lbase). Next levers: per-warp-privatized ghist, clear‖ghist dual-stream
overlap, shared-padding for reorder bank conflicts, 384-thread config. The gap to CUB is now pure kernel-tuning inches — the
STRUCTURE is at parity with onesweep+match+warp-rank, expressed portably in CKIR and bit-exact on every backend.

### Final inch-grind (2026-07-14) — the measured close of the sort campaign

Per-kernel standalone profile of the CUDA B pipeline settled the remaining list with DATA: **ghist 0.109 ms = the N-read
floor (the fused 4-digit histogram is ALREADY optimal — 268M shared atomics are free); clear 0.012; gbase 0.004.** The
predicted "ghist privatization / clear overlap" wins did not exist. Bank-conflict padding on the reorder: **measured WORSE**
(1.464 vs 1.418 — digit-scattered addresses are near-conflict-free; the index math cost more) → reverted. The scatter's
remaining ~0.11 ms/pass over the 0.21 memory floor is the warp-synchronous round chain latency (16 rounds × 2 warp-syncs ×
shared RMW) — further reduction needs double-key rounds or ncu-guided micro-surgery: diminishing returns, structure at parity.

**SORT CAMPAIGN FINAL: CKIR onesweep B = 1.42-1.47 ms (best 1.415) = 0.71-0.735× CUB (1.04), 14.9 → 1.42 = 10.5×, bit-exact
on oracle + Vulkan + CUDA, portable IR.** Vendor-gold scoreboard stands: FFT 1.99×/1.16× ✅ CRUSH · reduction 1.42× ✅ CRUSH ·
R2C 2× ✅ CRUSH · scan 0.53× (fundamental f32 wall) · sort 0.73× (structural parity, tuning-inch gap). Campaign closed.

## GEMM — pending (the NRC moat) — vs cuBLAS.
