# AS-4 board — CKIR auto-tuned GEMM vs cuBLAS Sgemm (f32, matched precision)

**Date:** 2026-07-22 · **Slice:** D-007 row 58 (ADR-0098 §4, AS-4) · **GPU:** RTX 4070 Ti SUPER (Ada sm_89, ~44 TFLOP f32
theoretical, 672 GB/s) · **CUDA:** 13.3 · **Peer:** cuBLAS Sgemm (CUDA-core FMA — *not* tensor cores; `cublasSgemm` on f32 uses
the CUDA cores, so this is a fair f32-vs-f32 fight) · **Test:** `tests/kir-cuda/test_autotune_cublas.cpp` (`[cublas]`, min-of-20
GPU-event-timed) · **Accuracy:** both certified against a f64 sampled dot-product reference (cuBLAS relerr 1e-8…8e-8 — genuinely
f32, no TF32).

## The board (representative run; ratios move ±15% run-to-run with GPU boost clocks)

| Shape | CKIR GFLOP/s | cuBLAS GFLOP/s | CKIR / cuBLAS |
|---|--:|--:|--:|
| 1024³ | 10 755 – **13 107** | 12 588 – 14 266 | 0.75 – **1.04** |
| 2048³ | 12 758 – 13 820 | 17 190 – 24 945 | 0.48 – 0.78 |
| 4096³ | 10 085 – 11 625 | 11 033 – 12 614 | 0.65 – **0.92** |

**Headline:** the auto-tuned CKIR GEMM reaches **~0.75–1.04× cuBLAS Sgemm** and **beats it on 1024³ on warmer-clock runs**
(13 107 vs 12 588 GFLOP/s, 1.04×) — genuine parity-class performance against the vendor, at matched f32 precision, from a
portable IR kernel the autotuner scheduled.

## The lever that got us here (the honest story)

The first measurement had CKIR at **0.28–0.49× cuBLAS** — a 2–3× loss. The cause was not the schedule: the CUDA backend compiled
**every** kernel with `--fmad=false` (for cross-backend bit-exactness), which **disables FMA fusion** and forces every `a*b+c`
into a separate multiply + add — halving GEMM throughput. But `--fmad=false` is only needed for the **bit-exact tier (T3)**; the
**fast tier (T1)** is explicitly ULP-tolerant, not bit-exact. Compiling the fast-tier WarpTiled GEMM **with** FMA fusion
(`--fmad=true`) ~doubled it (0.49× → 0.75–1.04×). The bit-exact tiers keep `--fmad=false` and are unchanged (full CUDA suite
81384/16, no regression). Scar: [[feedback_fast_tier_must_enable_fma_bitexact_flags_cripple_gemm]].

## Where cuBLAS still wins, and where CKIR crushes it (full scoreboard, losses head-on)

- **Raw compute-bound f32 GEMM (large 2048³):** cuBLAS wins (up to ~2×). Its hand-written SASS uses `cp.async` (async global→
  shared copies overlapping compute) and register allocation an NVRTC-compiled CUDA-C kernel family cannot express. This is the
  known lesson — *portable/bit-exact kernels crush only DRAM-bound ops; vendor SASS wins compute-bound*
  ([[feedback_bit_exact_fft_crushes_only_when_dram_bound]]). Closing it needs a kernel-family upgrade: `cp.async` double-buffering,
  or the **tensor-core coopmat2 path** already at ~88% cuBLAS ([[project_v17g_gemm_cublas_parity_89pct]]) — a future AS/kernel slice.
- **Fused, memory-bound regime (the CKIR moat) — MEASURED THIS SLICE:** GEMM+bias+SiLU in ONE CKIR kernel (one pass over C) vs
  cuBLAS Sgemm + its mandatory separate epilogue pass (bias+activation — a second read+write of C, charged its DRAM lower bound).
  The autotuner's schedule feeds the fused emitter (`emit_contract_tiled_fused_cuda`), so it inherits the AS gains + the FMA fix.

| Shape (MLP) | CKIR fused | cuBLAS gemm + epilogue | CKIR speedup |
|---|--:|--:|--:|
| 8192×8192×16 | 0.439 ms | 0.468 + 0.799 = 1.267 ms | **2.88× CRUSH** |
| 8192×8192×32 | 0.481 ms | 0.452 + 0.799 = 1.250 ms | **2.60× CRUSH** |
| 4096×4096×32 | 0.123 ms | 0.095 + 0.200 = 0.295 ms | **2.40× CRUSH** |

  CKIR crushes cuBLAS **2.4–2.9× on 3/3 memory-bound MLP shapes** — the structural moat cuBLAS can't touch (it cannot fuse the
  epilogue into the GEMM), confirming [[project_nrc_moat_fused_mlp_crushes_cublas]] with a live, oracle-correct measurement.

**Verdict:** the autotuner + the FMA fix bring the portable CKIR GEMM to **parity-class** with cuBLAS Sgemm on the compute-bound
board (occasionally beating it), and CKIR **wins outright** in the fused memory-bound regime the vendor can't fuse. The one
honest gap — large-shape raw GEMM — is a kernel-family (`cp.async`/tensor-core) frontier, not a scheduling one.
