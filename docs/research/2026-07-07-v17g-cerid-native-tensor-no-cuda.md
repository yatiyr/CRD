# 2026-07-07 — v17-g BREAKTHROUGH: Cerid-native tensor-core GEMM, ZERO CUDA (Vulkan cooperative_matrix)

> **Outcome:** **adopted** — the Vulkan coopmat tensor-core path shipped (v17-g GEMM ≈89% cuBLAS). Note: the no-CUDA-toolkit framing was later superseded — CUDA is a required compute backend (user directive 2026-08-07; `gpu-context-cuda`). *(stamped 2026-08-07, doc-hygiene pass)*

After proving that beating cuBLAS-TF32 on consumer Ada needs SASS (a CUDA-only, vendor-hand-tuned, nerfed-hardware
fight), the strategic pivot: **stop fighting cuBLAS inside CUDA — get tensor cores the PORTABLE way, through Vulkan
`cooperative_matrix`, where the DRIVER schedules the tensor-core SASS for us.**

## Proven this session (`external/coopmat_gemm.comp` + `coopmat_bench.cpp`)
- **The hardware exposes `VK_KHR_cooperative_matrix` (rev 2)** + NV variants + bf16/fp8 (`cooperativeMatrix = true`).
- **Wrote a Cerid-native GEMM in GLSL** using `coopmat` / `coopMatMulAdd` — compiles to valid SPIR-V (8.2 KB).
- **It RUNS ON THE TENSOR CORES with ZERO CUDA** (raw Vulkan compute) and is **CORRECT: maxrel 4.6e-8** vs an fp32 CPU
  reference (fp16 inputs, fp32 accumulate). No `cuda.h`, no `nvcc`, no cuBLAS anywhere in the path.
- First-cut throughput: **~7 TFLOP/s** (device-local VRAM; host-visible was 1.8 TF — PCIe-starved, fixed).

## Why this is the right direction (not a detour)
1. **No CUDA dependency.** Cerid runs tensor cores through the same Vulkan backend that already targets NVIDIA + AMD +
   Intel, and the browser (WebGPU is getting subgroup/cooperative matrix). One IR → tensor cores everywhere.
2. **The driver owns the hard part.** `coopMatMulAdd` lowers to the vendor's optimal tensor-core SASS — the exact
   dual-issue instruction schedule cuBLAS hand-writes and that **ptxas could NOT emit from CUDA C** (the wall that
   capped our hand kernel at ~0.9×). Here we get vendor-class scheduling for free, because we're above it.
3. **The parity ceiling is therefore higher via coopmat than via hand-CUDA-C** — the 10% SASS gap is the driver's job,
   not ours.

## The remaining work (a normal optimization journey, portable this time)
The first-cut is loader-bound (~7 TF), NOT tensor-bound. The knobs — same as any GEMM, but the mma scheduling is free:
1. **Vectorized shared-memory loaders** (load fp16 as `uvec4`/`u16vec8`, not scalar) — the #1 bottleneck; the scalar
   loader and the no-reuse direct-global path both sat at ~7 TF.
2. **Shared staging with reuse** + K-unrolling + double-buffering (the block loads once, all subgroups reuse).
3. **Tile/subgroup tuning** (WM×WN per subgroup; WM=4 regressed via register pressure — needs balancing with occupancy).
4. Compare fairly: fp16-coopmat vs `cublasHgemm` (matched precision), tf32/bf16 coopmat vs the matched cuBLAS mode.

## Decision
**The CKIR tensor-core GEMM is the Vulkan `cooperative_matrix` path, not hand-written CUDA/PTX.** CUDA/mma.sync stays
as one backend for max NVIDIA control + the benchmark, but the portable, driver-scheduled coopmat kernel is the one we
optimize toward parity — because it's vendor-class-scheduled and runs everywhere. Next slice: the vectorized-loader +
shared-staging coopmat GEMM, then wire `cooperative_matrix` emission into the CKIR Vulkan backend as the tensor path.

Files: `external/coopmat_gemm.comp` (the Cerid tensor kernel), `external/coopmat_bench.cpp` (Vulkan runner, no CUDA).
Proof: correct on the tensor cores with zero CUDA. Perf: optimization pending, but the SASS-scheduling wall is gone.
