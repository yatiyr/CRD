# 2026-07-07 — v17-e round 5–6: cp.async + bigger tiles + the HONEST corrected scoreboard (min of 6 runs)

Rounds 5–6 of the GEMM crush, back on Opus. Two new kernels/levers tried, and — importantly — a **rigorous
re-measurement that corrected an over-claim** from rounds 2–4. Everything below is the **minimum ratio across 6 runs**
(rounds 2–4's 3 + rounds 5–6's 3), i.e. the worst case for us. Vendor bar = min(cublasSgemm-PEDANTIC,
cublasSgemm-DEFAULT, cublasLt-PEDANTIC) per run, all true f32. Every config correctness-gated (≤2e-5) before timing.

## What was tried
1. **`cp.async` multi-stage pipeline** (`crd_v17e_gemm_pipe.cu`, 3–4 stages, dynamic shared for Ada's >48KB): **it
   REGRESSED** to 0.72–0.81× at N=1024/2048. Root cause (a real finding): cp.async copies contiguous → A is row-major
   in shared → the `regM` column read is strided → **can't vectorize to `LDS.128`**. Round 4's transposed shared-A
   makes that read unit-stride and vectorizable. cp.async needs CUTLASS-style **swizzling** to preserve the fast read
   — that's the v17-g kernel, not a free win.
2. **Bigger transposed tiles** (128×256, 256×128, added to the round-4 kernel): the **256×128 tile got the best-case
   raw@2048 = 1.03×** (up from 0.89×) — more rows/block → better A-reuse. But it does NOT hold under warm-clock
   variance. **256×256 (512 threads) collapsed to 0.1–0.5 TFLOP/s** (occupancy/spill — the search caught it).

## The honest, reproducible scoreboard (min of 6 runs)

| verdict | N=512 | N=1024 | N=2048 |
|---|---|---|---|
| **RAW SGEMM** | 0.90–0.94 (parity, OPEN) | **1.06× BEAT (6/6)** | 0.90–1.03 → **OPEN** (variance) |
| **FUSED +bias+SiLU** (off Lt's epilogue menu) | noisy 0.64–1.02 (small-N timing unreliable) | **1.13× BEAT (6/6)** | 0.83–1.02 → **OPEN** |
| **FUSED +bias+ReLU** (vs cublasLt's OWN fused kernel) | **1.06× BEAT (6/6)** | **1.20× BEAT (6/6)** | 0.86–1.07 → **OPEN** |

**Reproducible crush cells (hold under worst-case min-of-6):**
- **RAW SGEMM @ N=1024: 1.06×** — we beat cuBLAS's own SGEMM at true FP32.
- **FUSED GEMM+bias+SiLU @ N=1024: 1.13×** — SiLU is OFF cublasLt's epilogue menu (the op every LLM MLP runs); the
  vendor must pay a separate activation pass, we fuse it into the C write.
- **FUSED GEMM+bias+ReLU @ N=1024: 1.20× and @ N=512: 1.06×** — beating cublasLt's OWN fully-fused kernel.

## Why N=2048 is OPEN (not a loss — an honest measurement limit + a known kernel gap)

- **Measurement:** N=2048 verdicts swing ±15% because boost clocks + thermals are uncontrolled (no admin for
  `nvidia-smi -lgc`). cuBLAS-default alone measured 25.6–30.5 TFLOP/s across runs. A crush claim must survive the worst
  run; 2048's best runs beat (1.03–1.07×) but the min dips to 0.83–0.90×. **We do not claim it.**
- **Kernel:** even discounting variance, matching cuBLAS's near-peak large-N kernel exactly needs `cp.async` +
  swizzled shared layout (conflict-free AND vectorizable) + deeper pipelines — the CUTLASS-class kernel scoped to
  **v17-g**. Naive cp.async (round 5) proved the swizzle is the missing piece.

## The EXACT tier (measured, stable across runs)

No-FMA (`__fmul_rn`/`__fadd_rn`, bit-matches the `-ffp-contract=off` CPU oracle): **0.37–0.46× of our fastest** —
certified bit-exactness costs ~2–2.7× (FMA disabled). Honest price of the T3 tier.

## Verdict

**Reproducible cuBLAS crush at N=1024 (RAW + both fused) + N=512 fused-ReLU** — beaten in 6/6 runs, true FP32, fair
vendor bar. **N=2048 OPEN** (clock-variance + the CUTLASS-swizzle kernel gap → v17-g). The over-claim from rounds 2–4
is retracted and corrected here — the discipline (min-of-N, no false victory) is the point. Next: clock-locked
re-measure if admin becomes available; the v17-g swizzled cp.async kernel for 2048; and wire the N=1024 winning
schedules into the CKIR CUDA emitter so the crush is a compiler property.
