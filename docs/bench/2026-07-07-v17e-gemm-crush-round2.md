# 2026-07-07 — v17-e round 2–4: warp-tiled GEMM schedule bank vs cuBLAS/cublasLt — FIRST CRUSH CELLS (3-run stable)

> **⚠ CORRECTION (rounds 5–6, same day, back on Opus): the N=2048 cells below were OVER-CLAIMED.** A fresh, rigorous
> 6-run min-of-N pass on a *thermally warm* GPU shows N=2048 verdicts swing (RAW 0.90–1.03×, SiLU 0.83–1.02×, ReLU
> 0.86–1.07×) — cuBLAS-default alone swings 25.6→30.5 TFLOP/s run-to-run, and without `nvidia-smi -lgc` (admin) 2048
> can't be measured under worst-case conditions. **The REPRODUCIBLE crush is N=1024 (all three verdicts, 6/6 runs) +
> N=512 fused-ReLU. N=2048 is OPEN.** See `docs/bench/2026-07-07-v17e-gemm-crush-round6.md` for the corrected board.
> This honesty correction is itself the lesson (⛔ no false victory; a crush must hold under min-of-N).

Rounds 2–4 of the v17-e GEMM campaign (`crd_v17e_gemm_warptiled.cu`): the CUTLASS hierarchy (block-tile → **warp-tile**
→ thread-tile), float4 loaders, **padded transposed shared-A** (bank-conflict kill), **two-stage shared-memory double
buffering** (ONE `__syncthreads` per K-tile), **threadblock swizzle** (L2-friendly rasterization), and a **per-size
config search** — the autotuner story made concrete: 8 schedule configs, best-of per size.

**Fairness (per the bench-gate rule):** vendor bar = the FASTEST of `cublasSgemm` PEDANTIC / `cublasSgemm` default /
`cublasLtMatmul` PEDANTIC per size (all three are true-f32 numerics on SGEMM — default math ≠ TF32; it only changes
kernel selection). Fused comparisons use the strongest vendor composition available, **including cublasLt's own fused
epilogues**. Correctness: every config gated `max rel ≤ 2e-5` vs the pedantic reference before timing.

**Measurement honesty:** boost-clock/thermal variance is ±10–15% run-to-run (no clock locking without admin). Every
claim below is therefore the **MINIMUM ratio across 3 full runs** — the most conservative reading.

## The verdicts (best CKIR config vs best vendor; min across 3 runs)

| verdict | N=512 | N=1024 | N=2048 |
|---|---|---|---|
| **RAW SGEMM** | 0.90–1.02× (parity, OPEN) | **≥1.04× BEAT (all 3 runs: 1.04/1.05/1.08)** | 0.89–0.91× (**OPEN**) |
| **FUSED GEMM+bias+SiLU** (off Lt's epilogue menu — the LLM-MLP op; vendor MUST pay a separate pass) | 0.99–1.18× (parity⁺) | **≥1.09× BEAT (1.09/1.12/1.18)** | **1.02× BEAT (1.02 in ALL 3 runs)** |
| **FUSED GEMM+bias+ReLU** (ON Lt's menu — vs cublasLt's own fully-fused kernel) | **≥1.08× BEAT (1.08/1.15/1.18)** | **≥1.16× BEAT (1.16/1.24/1.96)** | 0.95–1.06× (parity) |

Raw throughput: best CKIR config 12.3–12.9 (512) / 25.2–26.6 (1024) / 25.8–28.4 (2048) GFLOP/s×10³ vs vendor
12.1–13.9 / 22.0–24.6 / 29.1–31.3. Peak card f32 FMA ≈ 44.9 TFLOP/s.

## The EXACT tier price (measured, stable)

`__fmul_rn`/`__fadd_rn` (no FMA contraction ⇒ **bit-matches the `-ffp-contract=off` CPU oracle**): **~12.2–12.5
TFLOP/s at N=2048 ≈ 0.43–0.52× of our fastest** — certified bit-exactness costs ~2× (FMA disabled halves FLOP rate).
That's the honest price tag of the T3 tier; the fast tier stays fixed-order deterministic (T1) with FMA.

## What moved the needle (the schedule ladder, cumulative)

| lever | effect |
|---|---|
| warp-tile level (CUTLASS hierarchy) | ~0.86× → the platform for everything below |
| per-size tile configs (64² at N=512) | N=512: 0.44× → 0.90×+ — the "gap" was 16 blocks on 66 SMs (grid underutilization), not kernel quality |
| padded transposed As (+4) | kills the 4-way store bank conflict (2-way residual) |
| 2-stage smem double buffer | ONE sync per K-tile; +10–30% at 1024 (Bd/Fd win rows); the *register*-prefetch variant from round 1 hurt — smem staging is the right buffer |
| threadblock swizzle (SWZ=8) | mixed: helps some (Bds/Fds at 1024/2048 SiLU), hurts others — a per-size autotuner decision, not a global default |
| fused epilogue in the writeout | the structural win: bias+activation lands in registers before the C store — the vendor pays a full extra C read+write when its epilogue menu doesn't cover the op (SiLU!) |

## Honest read

- **First real crush cells, conservatively measured:** raw SGEMM beaten at N=1024; fused-SiLU beaten at 1024 AND 2048
  (the op every LLM MLP runs — cublasLt cannot fuse SiLU); fused-ReLU beats cublasLt's own fused kernel at 512 + 1024.
- **Still open (⛔ solve, don't accept):** raw square SGEMM at N=2048 (0.89–0.91× — cuBLAS's large-N kernels use
  cp.async multi-stage pipelines + likely 128×256 tiles; that's the v17-g CUTLASS-class kernel) and at N=512
  (0.90–1.02 parity band — split-K is the known lever). Fused-ReLU@2048 is parity (0.95–1.06).
- The **per-size config search is not optional** — the winning schedule differs at every size (D2@512, Bd/Fds@1024,
  Bd/Bds@2048), which IS the v17-e autotuner thesis, now proven on silicon.

## Next (named)

1. **cp.async multi-stage pipeline + split-K** → close raw@2048 and raw@512 (v17-e cont. / v17-g).
2. **Integrate the winning schedules into the CKIR CUDA emitter** as the first checked-in tuning-DB entries
   (Contract → warp-tiled schedule instead of one-thread-per-output).
3. Tensor-core (TF32/f16 mma) tier for the reduced-precision fight (v17-g).
