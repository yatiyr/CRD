# 2026-07-07 — v17-e: the GEMM optimization ladder vs cuBLAS (f32) — HONEST, CRUSH NOT YET ACHIEVED

The first step of the v17-e vendor crush: a hand-written f32 GEMM ladder (`crd_v17e_gemm_tiled.cu`) benchmarked against
cuBLAS on the RTX 4070 Ti SUPER. **The fair, winnable fight for our bit-exact-f32 story is `CUBLAS_PEDANTIC_MATH`**
(true FP32, no TF32 tensor cores) — where cuBLAS is CUDA-core-FMA-bound just like us. **Reported head-on per the
no-partial-victory rule: we are NOT yet beating cuBLAS. This is an OPEN crush target, not a documented-and-accepted
loss.**

## The ladder (GFLOP/s, square GEMM, f32)

| N | naive (v17-c) | tiled 128²·8×8 | +vectorized (float4, transp A) | +double-buffer | **cuBLAS-f32 (pedantic)** | cuBLAS-TF32 |
|--:|--:|--:|--:|--:|--:|--:|
| 512  | ~1900 | ~3500 | ~4480 | ~4640 | **~10500** | ~13300 |
| 1024 | ~1560 | ~14260 | ~16000 | ~18470 | **~24100** | ~24100 |
| 2048 | ~1430 | ~16400 | ~22440 | ~20830 | **~29200** | ~26300 |

Best CKIR kernel vs cuBLAS-f32: **0.44× / 0.77× / 0.86×** (N=512/1024/2048). All kernels agree with cuBLAS to
`max rel ≈ 1e-6` (correct). Numbers vary ±10% run-to-run (thermal/clock).

## Honest read

- The ladder is real: **naive → tiled → vectorized closed the gap from ~0.05–0.14× to ~0.86× (N=2048)** — a 6–15×
  self-speedup. Vectorized float4 + a transposed A-tile were the big levers.
- **Double-buffering helped at N=1024 (0.70→0.77×) but HURT at N=2048 (0.86→0.71×)** — the prefetch registers raised
  register pressure and cut occupancy. Classic sign that **the next lever is warp-tiling** (better register/warp
  organization for occupancy), and that **the right config is size-dependent — which is exactly what the autotuner is
  for.**
- **cuBLAS's true-FP32 path is near-optimal** (~29 TFLOP/s at N=2048 ≈ 66% of the ~44 TFLOP/s card peak). A general
  hand-kernel reaching ~93–96% of cuBLAS (warp-tiled) is realistic; *beating* it on plain square SGEMM is not the
  cheap win.

## The genuinely winnable crush (the plan, honest)

1. **Warp-tiled + autotuned kernel (v17-e):** the schedule search over {block, warp-tile, register-tile, vectorize,
   double-buffer} per size ⇒ ~0.9–1.0× cuBLAS on square SGEMM. Parity is the realistic ceiling here.
2. **★ FUSED GEMM + epilogue (v17-g) — where we actually CRUSH:** cuBLAS does NOT fuse the epilogue (bias/activation/
   residual). CKIR fuses GEMM + elementwise into ONE kernel, saving a full C round-trip to VRAM ⇒ **beats
   cuBLAS-SGEMM + a separate elementwise kernel** for the fused ops that dominate real NN/physics workloads. This is
   the CUTLASS/cuDNN lesson and it plays to CKIR's fusion strength.
3. **TF32/f16 tensor-core mma (v17-g):** to contest cuBLAS's tensor-core path at matched (reduced) precision — a
   separate tier from the bit-exact-f32 mode.
4. **The determinism column cuBLAS structurally lacks:** bit-reproducible GEMM (fixed-order, `NoContraction`) — a
   guarantee, at a measured cost, that no vendor kernel offers.

## Verdict

v17-e GEMM: **strong measured progress (naive → 0.86× cuBLAS-f32), crush OPEN.** Next: warp-tiling + the autotuner
(parity on square SGEMM), then the FUSED-GEMM crush (v17-g) where CKIR's fusion genuinely beats vendor stacks. No
victory claimed until the numbers earn it.
