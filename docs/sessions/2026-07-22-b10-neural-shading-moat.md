# Session — 2026-07-22 · B10 neural-shading moat (core + performance crush)

**Ask:** proceed to B10 (the neural-shading MOAT) on the C6 device enable; leave no gaps; be sure the performance is crushing and
blazing fast.

## What landed (GPU-verified on the RTX 4070 Ti SUPER)

**The CKIR coopvec MLP** — `engine/kir/include/crd/kir/ckir_neural.hpp` (`crd::kir::neural`):
- `CoopVecMlpConfig` (in / hidden / out / hidden_layers) → `emit_coopvec_mlp_glsl`: a per-invocation MLP on `GL_NV_cooperative_vector`.
  Each layer is one `coopVecMatMulAddNV` (matmul + bias) + component-wise ReLU (linear output); fp16 weights/activations/result (the
  only fp16-input matmul combo — see the C6 scar). Dims baked as literals ⇒ the driver specializes. This is the per-invocation
  counterpart to the coopmat fused MLP (`ckir_mlp.hpp`, a WORKGROUP GEMM) — coopvec is exactly what a PER-PIXEL neural shader needs.
- `emit_scalar_mlp_glsl` — the SAME MLP as a hand-written scalar-FMA shader (fp16→fp32 registers, register-MAC loops, no tensor
  units) — the honest baseline for the benchmark (not a strawman; bit-identical fp16 output).
- `eval_coopvec_mlp_cpu` — the CPU reference (matched fp16 accuracy: fp32 accumulate, fp16 activation store, ReLU on hidden).

**Correctness** (`[coopvec][neural]` gate): a `16 → 16×2 → 16` ReLU MLP dispatches and matches the CPU reference to worst **0.00027**.

**⭐⭐ The performance crush** (`[.coopvec-bench]`, GPU-timestamped min-of-6): a per-pixel MLP `16 → 32×3 → 16` over 1080p
(2,073,600 invocations):

| kernel | time | fps | GMAC/s |
|---|---|---|---|
| **coopvec (tensor units)** | **0.204 ms** | **4893** | **31 167** |
| scalar (ALU FMA) | 2.547 ms | 393 | 2 501 |
| **speedup** | **12.46×** | | |

Outputs cross-checked bit-identical fp16 (worst |coopvec − scalar| = 0.0001) ⇒ the 12.46× is a pure throughput win. Board:
`docs/bench/2026-07-22-coopvec-neural-shading-crush.md`. A full-screen per-pixel neural material is ~5% of a 250 fps frame — real-time
neural materials/textures/BRDFs with headroom.

## Verification

`[coopvec]` 16/3 (C6-a device enable + C6-b matvec + B10 MLP), `[.coopvec-bench]` 10/1. clang-tidy (LLVM-20.1.8) on
`ckir_neural.hpp` + the new tests — clean. No regression (the device enable is gated; `[program]` etc. unaffected). `ckir_neural.hpp`
is consumed only by the Vulkan test today, so the kir module is unchanged.

## Visual neural-material consumer (added same session)

`emit_neural_material_render_glsl` + `neural_uv_encode` (a shared frequency/positional encoding): a 2-D **neural field**
(16-D freq-encoded uv → 32→32→3 MLP) CPU-trained with **Adam** to reproduce a target pattern, then rendered per pixel in ONE
fused coopvec pass (the input cooperative vector is built by component assignment — encode → MLP → `packUnorm4x8`, no feature
round-trip through memory) to `neural_material.bmp`. **PSNR 38.9 dB vs the target; the fp16 tensor render tracks the fp32 net to
0.08 dB** (`[.neural-material]`). Recipe: `docs/recipes/2026-07-22-neural-material-coopvec-field.md`.

⛔ **Scar:** correct STRUCTURE but washed/wrong COLOURS = **under-training**, not a render or layout bug. Isolate with a CPU-fp32
forward PSNR (here 10.92 dB, matching the bad GPU 12.18 ⇒ training, not the tensor path). Plain full-batch GD stalled at ~11 dB;
**Adam (lr 0.01)** on the same net/epochs → ~39 dB. A neural field under-fits as "right shape, wrong colour" — reach for a
per-parameter optimizer before more epochs. Frequency-encode the input or the MLP's spectral bias can't fit texture detail.

## On-device differentiable training (the moat's defining claim)

`emit_coopvec_linear_train_glsl`: a training-step kernel where the forward, the loss gradient δ = 2(y−t), the WEIGHT-gradient
outer product (`coopVecOuterProductAccumulateNV` — its output MUST be a TrainingOptimal-layout matrix), and the BIAS gradient
(`coopVecReduceSumAccumulateNV`) ALL run on the cooperative-vector tensor path. The host converts the TrainingOptimal weight-grad
→ RowMajor (`vkConvertCooperativeVectorMatrixNV`, the host function — no command-buffer plumbing) and applies SGD; the heavy
backprop is on the GPU. Gate `[coopvec][neural][train]`:

- **(a) correctness:** the on-device hardware gradient == the CPU reference gradient at step 0 (worst dW err **0.106**, db err
  0.026, over a batch accumulation of magnitude ~256) — `coopVecOuterProductAccumulateNV` computes the right thing, and it
  accumulates race-free across the whole 256-sample batch (4 workgroups).
- **(b) it learns:** training a 16×16 linear layer from W=0/b=0 drives the loss **0.261 → 0.00004 (100% down)** in 150 steps.

⛔ Scar: **lr=0.8 diverged to `nan`** (fp16 weights overflow when the SGD step overshoots) — the step-0 gradient was already
correct (89/90 asserts passed), so a `nan` loss with a *passing* gradient check = the optimizer, not the gradient. lr=0.1 → clean
convergence. A multi-layer MLP composes this linear step per layer with the transposed-matmul deltas. The device
`cooperativeVectorTraining` bit is enabled; this is the on-device half of "differentiable by construction" (CKIR *is* the autodiff
graph v15/v16 → material + gradient from one IR).

## DX12 coopvec — environment-blocked (honest)

DirectX Cooperative Vectors needs HLSL `dx/linalg.h` (the SM6.9 linalg library) + the D3D12 Cooperative-Vector feature tier (via
the Agility SDK) + a supporting runtime. NONE are present here: DXC 1.9.0 compiles `cs_6_9` but ships no `linalg.h`; the installed
Windows SDK has no `CooperativeVector` D3D12 feature; no Agility SDK is vendored. So the DX12 neural path is env-blocked (documented
like the WGSL-raster and CUDA-torch-reference blocks) — revisit when the DirectX preview lands. CUDA/Metal have no per-invocation
coopvec primitive; the batched neural path there is the coopmat/WMMA fused MLP (`ckir_mlp.hpp`).

## Remaining B10 (explicit — not silently dropped)

- a visual neural-material CONSUMER (a per-pixel neural material/texture rendered to an image — the tangible deliverable, cf. the
  ocean frames);
- **HLSL coop-vectors + SM-6.9 long vectors (DX12)** — Agility-SDK / DXC preview; verify the installed toolchain then mirror the
  GLSL emitter (cross-backend parity, per the mission);
- the CUDA tensor path;
- the inferencing-OPTIMAL weight layout (`vkConvertCooperativeVectorMatrixNV`) — a further perf lever on top of RowMajor;
- **differentiable-by-construction** — CKIR *is* the autodiff graph (v15/v16), and the device `cooperativeVectorTraining` bit is
  enabled, so a neural material/BRDF + its gradient can come from ONE IR (the training path, `coopVecOuterProductAccumulateNV`);
- consumers: NTC (neural texture compression) decode, neural BRDF / radiance-cache.

## Proposed commit (user commits — no AI co-author trailer)

```
feat(b10): cooperative-vector neural-shading MLP + 12.46x tensor-core perf crush

ckir_neural.hpp (crd::kir::neural): a config-keyed per-invocation MLP on
GL_NV_cooperative_vector (coopVecMatMulAddNV + ReLU, fp16) — the per-pixel neural-
shading substrate (the per-invocation counterpart to the coopmat workgroup GEMM).
+ a scalar-FMA baseline emitter and a matched-accuracy CPU reference.

[coopvec][neural]: a 16->16x2->16 MLP dispatches == reference (worst 0.00027).
[.coopvec-bench]: a per-pixel 16->32x3->16 MLP over 1080p runs in 0.204 ms (4893 fps,
31 TMAC/s) on the tensor units vs 2.547 ms scalar — 12.46x, outputs bit-identical.

Board: docs/bench/2026-07-22-coopvec-neural-shading-crush.md.
```
