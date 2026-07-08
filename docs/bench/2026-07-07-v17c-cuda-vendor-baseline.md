# 2026-07-07 — v17-c: CUDA backend + the first vendor benchmark (cuBLAS SGEMM) — HONEST BASELINE

The CKIR CUDA backend (`crd-kir-cuda`, `KirBackendCuda` over the CUDA driver API + NVRTC) runs on the RTX 4070 Ti,
**bit-exact vs the CPU reference** for correctly-rounded ops. This board is the FIRST vendor benchmark — the naive
CKIR matmul vs cuBLAS SGEMM. It is a **baseline, not a crush**: at v17-c the CKIR kernel is the naive
one-thread-per-output-element kernel (no tiling, no tensor cores), so it trails hand-tuned cuBLAS. The gap is the
target the v17-e autotuner + the v17-g cooperative-matrix GEMM close. Reported honestly per the no-partial-victory rule.

## Conformance (the real v17-c result — `test_backend_cuda.cpp`, 2863 asserts GREEN)

| kernel | vs the CPU oracle |
|---|---|
| elementwise (incl. **division**) | **BIT-EXACT** — CUDA `--fmad=false --prec-div=true --prec-sqrt=true` gives correctly-rounded div/sqrt (Vulkan's fast reciprocal is ~2 ULP, so CUDA's bit-exact core is *wider*) |
| matmul (Contract) | **BIT-EXACT** (sequential-k, dtype-faithful) |
| reduce (sum/max) | **BIT-EXACT** (fixed ascending order, no atomics) |
| determinism | run-to-run bit-identical |

**Two GPU backends now run bit-exact from the same CKIR IR: Vulkan + CUDA.**

## GEMM perf — CKIR naive vs cuBLAS SGEMM (f32, RTX 4070 Ti, `crd_v17c_gemm_vendor.cu`)

| N | CKIR naive | cuBLAS | gap | numerical agree (max abs) |
|--:|--:|--:|--:|--:|
| 512  | 1924 GFLOP/s | 13443 GFLOP/s | cuBLAS **7.0×** | 1.8e-6 |
| 1024 | 1561 GFLOP/s | 12752 GFLOP/s | cuBLAS **8.2×** | 4.5e-6 |
| 2048 | 1432 GFLOP/s | 31724 GFLOP/s | cuBLAS **22.2×** | 1.1e-5 |

**Honest read:** the naive CKIR kernel does ~1.4–1.9 TFLOP/s (respectable for a one-thread-per-output f32 kernel), but
cuBLAS's tensor-core + tiled kernels hit 13–32 TFLOP/s — 7× at N=512 growing to 22× at N=2048 (tensor cores dominate at
scale). CKIR and cuBLAS **agree numerically** (1e-6–1e-5; cuBLAS is a different, non-bit-exact algorithm). This is the
baseline the crush is measured against.

## What CKIR wins NOW (before the perf work)

- **Bit-exact to the CPU reference + deterministic** — verifiable compute; cuBLAS gives no bit-reproducibility guarantee.
- **Portable** — the SAME CKIR matmul already runs on Vulkan too; cuBLAS is NVIDIA-only.
- **Differentiable** — the matmul's VJP is free (CKIR-Graph is the autodiff graph); cuBLAS is not.

## The path to the crush (named, not hand-waved)

1. **v17-e — the autotuner** (tile sizes, shared-memory blocking, pipelining) — closes the tiling gap.
2. **v17-g — cooperative-matrix / tensor-core GEMM** (`wmma`/`mma`) — closes the tensor-core gap (the 22× at N=2048).
   The goal there: cuBLAS-parity-or-better at matched precision + the determinism column cuBLAS lacks.

## Verdict

v17-c **CUDA backend: DONE + bit-exact** (the correctness crush — two backends bit-identical to one oracle). Perf vs
cuBLAS: **honest baseline, 7–22× behind the naive kernel**, gap owned by v17-e/g. No false victory claimed.
