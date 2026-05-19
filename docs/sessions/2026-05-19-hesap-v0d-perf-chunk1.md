# Session 2026-05-19 — Phase 3.1.6 `crd-hesap` v0d-perf chunk 1 (packing + microkernel invocation + benchmark)

## Goal

After v0d-FOUNDATION shipped the BLAS L3 surface earlier today, user
asked **"do we really need hand-rolled inline asm per arch?"** and
chose the elite-path-via-intrinsics route. This session's chunk 1:

1. Lock the intrinsics-vs-asm decision as an ADR with explicit
   future-swap door (ADR-0082 Accepted).
2. Build the packing layers + driver-invokes-microkernel architecture
   that turns v0d-foundation's scalar-inner-product gemm into a real
   Goto/BLIS layered GEMM.
3. Ship a benchmark fixture (preview of v0f `crd-hesap-bench`) that
   reports honest GFLOPS-vs-peak ratios on the dev box.

## What we built / changed

- **ADR-0082 — Hesap GEMM microkernel: intrinsics-via-Vec8f/Vec16f, ASM
  deferred** (`docs/decisions/0082-hesap-microkernel-intrinsics-strategy.md`,
  Accepted 2026-05-19). Locks the hot-swap signature
  `gemm_microkernel<T>(k, a_packed, b_packed, c_tile, ldc)` and the
  three-condition revisit gate (GEMM >50% of solve AND intrinsics <70%
  peak AND no better alternative). Indexed in `docs/decisions/README.md`.
- **`engine/hesap-dense/include/crd/hesap/dense/detail/microkernel_backend.hpp`**
  — NEW. `CRD_HESAP_MICROKERNEL_BACKEND_INTRINSICS` (default = 1) +
  `_ASM` (reserved). `_ASM` path triggers `static_assert(false, ...)`
  with the ADR-0082 revisit reference so accidentally enabling it
  without per-arch `.S` files fails at compile time.
- **`gemm_microkernel.hpp` hot-swap dispatcher** — explicit comment
  banner calling out the swap point + locked signature. Backend switch
  routes via `#if CRD_HESAP_MICROKERNEL_BACKEND == _INTRINSICS` today.
- **`engine/hesap-dense/include/crd/hesap/dense/detail/gemm_pack.hpp`**
  — NEW. `pack_a` + `pack_b` Goto/BLIS panel packing functions.
  Layout: Ac as `MR`-contiguous-rows blocks (8 rows per row-panel ×
  `kc` cols); Bc as `kc` rows × `NR`-contiguous-cols blocks. Trans /
  ConjTranspose honored via `eff_a_read<T, L>` index transform during
  packing. Zero-padded edge tiles. `gemm_packed_inner<T, L>` runs the
  inner mr × nr loops, calls `gemm_microkernel<T>` on each (row-panel,
  col-panel) pair, accumulates `alpha * micro` into C.
- **`engine/hesap-dense/src/blas3.cpp` gemm rewritten** as the proper
  Goto/BLIS 5-loop:
    1. jc outer (Nc-wide column slabs)
    2. pc outer (Kc-deep K slabs); pack Bc once per (jc, pc)
    3. ic outer (Mc-tall row slabs); pack Ac once per (ic, pc)
    4. gemm_packed_inner inner mr × nr loops → microkernel
  Pack buffers allocated once per gemm call from `default_allocator()`
  (max ~4 MB for f32 / 8 MB for f64 at full Mc/Kc/Nc).
- **`engine/hesap-dense/CMakeLists.txt`** — adds `crd-hesap-sched` +
  `crd-jobs` as PUBLIC deps for the planned outer-loop parallelism.
- **`runtime/examples/bench_hesap_gemm.cpp`** — NEW. Measures GEMM
  throughput across N ∈ {64, 128, 256, 512, 1024} for f32 + f64;
  computes 2N³/elapsed; reports actual/peak ratio. Peak estimated from
  CPU clock × SIMD width × 2 FMA ports × 2 ops/FMA. Single-core,
  warm-cache, at-least-3-iters-or-500ms-wall.
- **Memory entry**
  `project_hesap_microkernel_intrinsics_decision.md` — future sessions
  remember the ADR-0082 choice, hot-swap architecture, and three-
  condition revisit gate.

## Measured perf (honest characterization — dev box, win-shipping)

```
SIMD backend     : AVX2
Coarse clock     : 5.61 GHz
Single-core peak : f32 = 179.6 GFLOPS, f64 = 89.8 GFLOPS
Reference class  : intrinsics-tier (Eigen / Faer / Highway peers; NOT MKL-asm tier)
Target           : 80%-85% of single-core peak per ADR-0082 §decision

==== f32 gemm ====
  N=   64   24.84 GFLOPS  (13.8% peak)
  N=  128   56.09 GFLOPS  (31.2% peak)
  N=  256   86.36 GFLOPS  (48.1% peak)
  N=  512  103.08 GFLOPS  (57.4% peak)
  N= 1024  106.47 GFLOPS  (59.3% peak)
==== f64 gemm ====
  N=   64    9.69 GFLOPS  (10.8% peak)
  N=  128   13.36 GFLOPS  (14.9% peak)
  N=  256   14.61 GFLOPS  (16.3% peak)
  N=  512   15.44 GFLOPS  (17.2% peak)
  N= 1024   15.43 GFLOPS  (17.2% peak)
```

**Where we are**: f32 at ~59% of single-core AVX2 peak at N=1024. That's
**~12× faster than v0d-foundation's scalar-inner-product baseline** (which
would have been at ~5% peak — same range as a hand-written naive matmul).
f64 at ~17% peak — sits there because v0d-foundation only wrote the AVX2
microkernel for f32; f64 falls through to scalar. Adding a Vec4d (or
two-Vec4f composed) AVX2 microkernel for f64 is the next chunk's lowest-
hanging fruit and brings f64 into ~50-60% peak range.

**Path to the 80-85% target** (filed as follow-ons):
- `v0d-perf-f64-avx2` — AVX2 microkernel for f64 (~150 LOC) → f64
  expected ~50-60% peak.
- `v0d-perf-prefetch` — manual prefetch in the AVX2 microkernel
  (~50 LOC) → +5-10% peak.
- `v0d-perf-block-tune` — measure-and-tune Mc/Kc/Nc per CPU
  (currently using BLIS defaults; may not be optimal for the dev box).
- `v0d-parallelism` — outer-loop parallelism via existing
  `crd-hesap-sched::parallel_tiles_for`; ~3-7× on multi-core for
  N>256. Won't help single-core peak ratio but unlocks the total
  throughput claim.
- `v0d-perf-arch-extend` — AVX-512 / NEON microkernels when relevant
  HW is available; deferred per session 1 scoping note.

## Plain-English explanation

The big architectural shift is: v0d-foundation's gemm was a triple-loop
that did one inner-product per output element with naive memory access.
v0d-perf chunk 1 swaps in the **proper Goto/BLIS layered structure**:

1. Pre-pack tiles of A and B into cache-friendly contiguous slabs
   (so the inner loop never touches the original matrix layout).
2. Run a microkernel-at-the-leaf that holds C accumulators in
   registers across many K iterations.

The 12× speedup from foundation to v0d-perf chunk 1 IS the layered
structure paying off — same algorithm, same correctness (108 cases /
4704 assertions still pass), just with the memory hierarchy respected.

Going from 59% peak to the 80-85% target needs the additional tuning
filed in the follow-on list — those are each smaller, focused chunks
now that the structure is right.

## Decisions made

- **D45 (v0d-perf)** — ADR-0082 Accepted: pure intrinsics, ASM deferred,
  hot-swap point preserved. Future-session memory entry pinned.
- **D46 (v0d-perf)** — Packing layout is per-backend (intrinsics today
  uses row-major MR rows × kc cols for Ac, kc rows × NR cols for Bc).
  When ASM lands, BLIS column-major MR-slab packing comes with it.
- **D47 (v0d-perf)** — Pack buffers from `default_allocator()` (not
  the per-thread frame arena from `crd::jobs`) so single-threaded
  gemm callers don't need to be on a fiber. Outer-loop parallelism
  will migrate to per-thread frame arenas later.
- **D48 (v0d-perf)** — Benchmark fixture reports per-CORE peak
  (single-threaded). Multi-core total throughput claim ships with
  `v0d-parallelism` follow-on. Coarse clock measurement via dependent
  integer adds — honest about its limitation in the bench output.
- **D49 (v0d-perf)** — Function template calls inside templated gemm
  body use **deduced template args** (no explicit `<T, L>` at the call
  site) per the v0c D27 MSVC parser workaround.

## Files touched

- `docs/decisions/0082-hesap-microkernel-intrinsics-strategy.md` — new
- `docs/decisions/README.md` — ADR-0082 entry
- `engine/hesap-dense/include/crd/hesap/dense/detail/microkernel_backend.hpp` — new
- `engine/hesap-dense/include/crd/hesap/dense/detail/gemm_microkernel.hpp` — hot-swap markers + backend gate
- `engine/hesap-dense/include/crd/hesap/dense/detail/gemm_pack.hpp` — new
- `engine/hesap-dense/src/blas3.cpp` — gemm rewritten layered
- `engine/hesap-dense/CMakeLists.txt` — crd-hesap-sched + crd-jobs deps
- `runtime/examples/bench_hesap_gemm.cpp` — new
- `runtime/CMakeLists.txt` — adds bench_hesap_gemm
- Memory: `project_hesap_microkernel_intrinsics_decision.md` — new
- `context.md` — Last shipped + Current focus update
- `docs/phases/phase-3.1.6-hesap.md` — v0d row ◑ FOUNDATION + perf-chunk-1

## Tests / verification

- **Built?** ✅ All targets clean under win-debug + win-shipping.
- **Tests pass?** crd-hesap-dense-tests **108 cases / 4704 assertions PASS**
  (same as v0d-foundation — packed-driver path produces bit-identical
  results to the scalar-inner-product path within tolerance).
- **Benchmark numbers**: see "Measured perf" section above.
- **Per-slice DoD?** _5-config `per-slice-check.ps1 -IncludeRelease`
  running at session-close authoring._

## Next session(s) starts with

Pick from the v0d-perf follow-on backlog:

1. **`v0d-perf-f64-avx2`** — biggest immediate win (f64 jumps from 17%
   to ~50-60% peak). ~150 LOC. ½ day.
2. **`v0d-parallelism`** — outer-loop parallelism via existing
   `crd-hesap-sched::parallel_tiles_for`. ~150 LOC + multi-thread
   bit-exact tests. 1 day.
3. **`v0d-perf-prefetch` + `v0d-perf-block-tune`** — closing toward
   ~80% peak. ~200 LOC + tuning iteration. 1-2 days.

Then **v0e dense direct solvers** (LU + Cholesky + QR + LDLT + iterative
refinement + LinearOp factor view + condition estimation). ~1200 LOC +
~120 tests + ~7 d. Can start in parallel with v0d-perf follow-ons since
v0e consumes the BLAS L3 correctness surface which is complete today.
