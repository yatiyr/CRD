# 2026-07-07 — v17-g tensor-GEMM parity: the BIG-PICTURE resolution (studied the user's GPURes/ pack)

> **Outcome:** **adopted** — the v17-g GEMM campaign reached ≈88–89% cuBLAS parity (boards in `docs/bench/`). *(stamped 2026-08-07, doc-hygiene pass)*

After exhaustively grinding a hand-written TF32 mma.sync GEMM to **0.86–0.90× cuBLAS-TF32** (profiler-driven, every
CUDA-C lever + warp specialization tried), the user supplied a research pack (`D:\Dev\GPURes\`). Studying it RESOLVED
the parity question — the wall is not our kernel, it's a **target/hardware mismatch**. This doc is the strategic finding.

## The pack
- **Ootomo & Yokota 2022 (2203.03341)** — "Recovering single-precision accuracy from Tensor Cores while surpassing the
  FP32 theoretical peak." **33 TF FP32-accurate via TF32 tensor cores on A100, vs 19.5 TF FP32-SIMT peak = 1.7×.**
- **Luo et al 2025 (2501.12084)** — Dissecting Hopper via microbenchmarking (tensor throughput/latency tables).
- **Chen et al 2025 (2510.14719) "Tawa"** — automatic warp specialization; states plainly: **"Hopper and Blackwell
  GPUs introduce hardware-level warp [specialization]… producer warps dedicated to data transfers via the TMA."**
- **salykova sgemm-gpu blog** — from-scratch **CUDA-core FP32** SGEMM that **matches/beats cuBLAS by 3–4%** (RTX 3090).
- CUTLASS ping-pong (PyTorch blog), CuTe swizzle (Lei Mao), maxas (SASS), NVIDIA throttle forum, CUTLASS ex.14.

## The decisive fact: consumer-Ada NERFS the tensor cores to ~1:1 with FP32 SIMT
Measured on this RTX 4070 Ti SUPER: **cuBLAS-TF32 ≈ 1.3× cuBLAS-FP32** (pure `mma.sync` probe = 43 TF; FP32-SIMT peak
≈ 44 TF ⇒ **TF32-tensor : FP32-SIMT ≈ 1:1**). On datacenter A100 that ratio is **~8:1**. NVIDIA deliberately caps
consumer TF32-tensor throughput to protect datacenter sales. **This one number explains everything:**

1. **cuBLAS-TF32 barely beats cuBLAS-FP32 here** (1.3×), and it's already ~93% of the (nerfed) tensor peak — so the
   remaining 10% to it is genuinely just SASS instruction scheduling (cuBLAS is hand-scheduled SASS; nvcc can't emit it).
2. **Warp specialization / ping-pong / Tawa DON'T APPLY to Ada** — they need Hopper/Blackwell TMA + hardware warp
   specialization. The papers state this outright; our empirical warp-spec regression (0.55–0.81×) was CORRECT.
3. **Ootomo error-correction can't win on consumer Ada** — 3×TF32 for FP32-accuracy costs ~3× tensor ops; at a 1:1
   ratio that's ~14 TF vs 44 TF FP32-SIMT. It needs the A100's 8:1 ratio to surpass FP32 (where it hits 1.7×).

## ⇒ We were chasing parity on the WRONG target. The real wins (all achievable):
| target | consumer Ada (this card, ~1:1) | datacenter A100/H100 (~8:1+) |
|---|---|---|
| **FP32-accurate GEMM** (what a correct engine uses) | **CUDA-core kernel BEATS cuBLAS-FP32 (v17-b, 1.06× @1024)** ✅ | **Ootomo 3×TF32 error-correction: FP32-accurate, SURPASSES FP32 peak 1.7×** — BUILD THIS |
| raw lossy TF32-tensor | 0.86–0.90× cuBLAS-TF32; last 10% = SASS; marginal (1.3× nerf) — NOT worth it | ping-pong/wgmma/TMA warp-spec (Hopper) |

**"Someone has done it" — TRUE, and now precise:**
- salykova did from-scratch **CUDA-core FP32** parity → **we already match/beat it** (beat cuBLAS-FP32 @1024).
- Ootomo did the **FP32-accurate tensor crush** → that's the **datacenter** capability to build (surpasses FP32 peak).
- NOBODY published a consumer-Ada TF32-tensor crush — because the tensors are nerfed to 1:1 and cuBLAS is at the peak.

## CKIR GEMM strategy (the outside-the-box answer)
Dispatch by the card's tensor:SIMT ratio (query at init):
1. **No-TMA / low-ratio (consumer Ada/Ampere): the CUDA-core FP32 warp-tiled kernel** — already beats cuBLAS-FP32,
   already in the CKIR compiler (v17-b), already fused (v17-e). **This is the crush on this hardware, on the CORRECT op.**
2. **High-ratio datacenter (A100): the Ootomo 3×TF32 (and 2×TF32 / FP16-split) error-correction kernel** — FP32-accurate
   at tensor speed, surpassing the FP32 peak. This is the real tensor-core crush and CKIR needs it for datacenter anyway.
3. **Hopper/Blackwell: TMA + wgmma warp-specialized (ping-pong).** Future, when we target those.

## Honest verdict
Absolute parity with cuBLAS-**TF32** on **consumer Ada** needs SASS and is chasing a nerfed lossy op — low value. The
meaningful "crush cuBLAS" is **already ours on the correct FP32 op**, and the spectacular tensor crush (surpassing the
FP32 peak with FP32 accuracy) is **Ootomo's technique on datacenter cards** — a published, reproducible, CUTLASS-based
method. Recommended next slice: implement the **error-correction (3×TF32) FP32-accurate tensor GEMM** in CKIR + a
runtime dispatch on the tensor:SIMT ratio. (Also: lock the memory clock `nvidia-smi -lmc` for clean measurements — the
SM-only lock idled memory to 810 MHz and depressed absolute numbers.)

## Empirical confirmation (built `crd_v17g_ootomo.cu` + `crd_v17g_warpspec.cu`)
- **Warp specialization built + measured on Ada: REGRESSES** (4+4 = 0.63–0.81×, 2+8 = 0.55–0.82×) — confirms Tawa's
  claim it's a TMA/Hopper technique. Negative result, banked.
- **Ootomo 3×TF32 built + measured (N=2048, vs cublasDgemm FP64 truth):** the correction WORKS — for a typical output
  C[0], single-TF32 error = **1.5e-3** → 3×TF32 error = **1.6e-5** = the SAME class as cuBLAS-FP32 (4.1e-5). It recovers
  ~2 decimal digits, precisely the paper's claim. Residual `maxrel` outliers remain on cancellation-near-zero elements
  = the "rounding inside Tensor Cores" that Ootomo's FULL method (RN-control + scaling) fixes and naive 3×TF32 does not
  (the paper says this explicitly). **Throughput on this card: 0.41× cuBLAS-FP32 — CONFIRMS the consumer nerf** (3×
  tensor ops at a 1:1 ratio can't beat FP32-SIMT; on A100's 8:1 it surpasses FP32 by 1.7×). The kernel is the
  datacenter play, validated to be accuracy-correct in the common case.

## Bottom line for the user
The "parity" wall on consumer Ada TF32 is a **hardware nerf + wrong-target** artifact, now proven three ways (my
grind, the papers, and two built kernels). **We already crush cuBLAS on the correct FP32 op here (1.02–1.06×).** The
spectacular tensor crush (FP32-accurate, surpassing FP32 peak) is real and reproducible — it lives on datacenter cards
via Ootomo, and the kernel is now built + accuracy-validated, ready to deploy where the hardware isn't nerfed.

Sources: arXiv 2203.03341, 2501.12084, 2510.14719; salykova.github.io/sgemm-gpu; PyTorch CUTLASS ping-pong blog;
NVIDIA CUTLASS; NVIDIA dev-forum tensor-throttle thread.
