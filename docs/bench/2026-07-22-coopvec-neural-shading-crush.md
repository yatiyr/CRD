# Bench — B10 cooperative-vector neural shading vs scalar-FMA (2026-07-22)

**Board:** per-pixel neural-material MLP at 1080p — the cooperative-vector tensor path vs the identical MLP hand-written as a
scalar-FMA shader. The moat: real-time per-pixel neural materials/textures/BRDFs.

- **Hardware:** NVIDIA GeForce RTX 4070 Ti SUPER (Ada, 16 GB), `VK_NV_cooperative_vector` rev 4.
- **Harness:** `[.coopvec-bench]` in `tests/gpu-context-vulkan/test_vulkan_context.cpp`. GPU-timestamped (`VulkanComputeContext::last_gpu_ms()`, excludes CPU record/submit), min-of-6 after a warm-up. Build: win-debug (C++ debug, but the timing is GPU-only ⇒ build-independent).
- **Workload:** MLP `16 → 32 → 32 → 32 → 16` (3 hidden layers, ReLU; linear output), one evaluation PER PIXEL over 1920×1080 = **2,073,600 invocations**. fp16 weights/activations/result (the only fp16-input coopvec matmul combo). ~3072 MACs/pixel.
- **Kernels** (same 5 bindings, same data, same MLP): `crd::kir::neural::emit_coopvec_mlp_glsl` (`coopVecMatMulAddNV` per layer + component-wise ReLU) vs `emit_scalar_mlp_glsl` (plain per-invocation loops, fp16→fp32 registers, register-accumulated MACs — no tensor units).

## Result

| kernel | time | throughput | GMAC/s |
|---|---|---|---|
| **coopvec (tensor units)** | **0.204 ms** | **4893 fps** | **31 167** |
| scalar (ALU FMA) | 2.547 ms | 393 fps | 2 501 |
| **speedup** | **12.46×** | | 12.5× |

**Cross-check (self-verifying):** both kernels compute the identical MLP; worst |coopvec − scalar| over a 4096-sample stride = **0.0001** (fp16 accuracy — the outputs agree, so the 12.46× is a pure throughput win, not a corner cut). The CKIR CPU reference (`eval_coopvec_mlp_cpu`) matches the coopvec device output to worst **0.00027** (the `[neural]` gate).

## Reading

- **0.204 ms for a full-screen 1080p per-pixel MLP** means the neural-shading pass is ~5% of a 4 ms (250 fps) frame — real-time
  neural materials with enormous headroom (and it scales sub-linearly with tensor occupancy at 4K).
- The **12.46× over a hand-written scalar shader** is the tensor-core lever: coopvec issues the per-invocation matrix×vector on the
  Ada tensor units instead of the ALU. The scalar baseline is itself a competent fp16→fp32 register-MAC shader, not a strawman —
  they produce bit-identical fp16 results.
- **Honest scope:** this is the FORWARD inference crush on Vulkan. Matched fp16 accuracy (tensor cores reorder the accumulation —
  the FP32-precise CKIR tier owns bit-exactness). Cross-backend (DX12 Cooperative-Vectors, SM6.9/Agility preview) + the CUDA path
  + the inferencing-OPTIMAL weight layout (`vkConvertCooperativeVectorMatrixNV` — a further perf lever on top of RowMajor) are the
  next B10 increments. No reference-peer comparison here (this is our-tensor-path vs our-scalar-path; a DLSS/NRC-style external
  peer would need its own harness).

## Reproduce

```
crd-gpu-context-vulkan-tests.exe "[.coopvec-bench]"
```
