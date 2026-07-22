# Recipe — A neural material (2-D neural field) on the cooperative-vector tensor path

> How to build a *learned texture*: a small MLP that maps a uv coordinate to RGB, trained to reproduce a target, then evaluated
> per pixel inline on the `VK_NV_cooperative_vector` tensor units. The tangible neural-shading deliverable — the substrate for
> neural textures / materials / BRDFs. Code: `engine/kir/include/crd/kir/ckir_neural.hpp`; gate `[.neural-material]` in
> `tests/gpu-context-vulkan/test_vulkan_context.cpp` → `neural_material.bmp` / `neural_target.bmp`.

## Parameters

| dial | value (this recipe) | note |
|---|---|---|
| encoding | frequency (positional), `in_dim` = 4·bands | `neural_uv_encode`: band k (freq 2^k·π) → [sin(f·u),cos(f·u),sin(f·v),cos(f·v)] |
| bands | 4 (→ in_dim 16) | more bands = higher representable frequency (fights the MLP's spectral bias) |
| MLP | 16 → 32 → 32 → 3, ReLU hidden, linear out | `CoopVecMlpConfig{16,32,3,2}`; fp16 weights/activations/result |
| optimizer | **Adam** lr 0.01, β 0.9/0.999 | full-batch over a 32² uv grid, ~1200 epochs |
| render | 512², 8×8 workgroup | one invocation/pixel: encode → MLP → `packUnorm4x8` |

## Physics / why it works

A **neural field** stores a signal (here an RGB texture) in the *weights* of a small MLP instead of a pixel grid — the network is
a continuous, resolution-independent function `f(u,v) → colour`. Two ingredients make a tiny net represent detail:

1. **Frequency (positional) encoding.** A plain MLP on raw `(u,v)` can only learn very smooth functions — its *spectral bias*
   (Rahaman 2019) means high frequencies converge glacially. Lifting the input to `[sin(2^k π·uv), cos(2^k π·uv)]` for several bands
   (NeRF / Fourier features, Tancik 2020) gives the net a basis of frequencies to combine, so it fits detail in a few thousand
   steps. With 4 bands the top frequency is 8π ≈ 25 rad/unit — plenty for a smooth-to-moderate texture.
2. **Adam, not plain GD.** This is the load-bearing lesson from building it: plain full-batch gradient descent (even with a
   hand-tuned lr) stalled at **~11 dB** — the low-frequency *structure* emerged (blob positions right) but the colours were wrong,
   because the per-channel output weights need precise, well-scaled steps that a single global lr can't give across layers with very
   different gradient magnitudes. Swapping in **Adam (lr 0.01)** — per-parameter adaptive step from the running gradient
   mean/variance — took the *same* net, *same* epochs to **~39 dB**. Under-fitting a neural field reads as *correct shape, wrong
   colour*; reach for a per-parameter optimizer before more epochs.

The tensor render is faithful: train in fp32, quantize the weights to fp16, and the fp16 cooperative-vector render tracks the fp32
CPU forward to **0.08 dB** (38.99 → 38.91). fp16 inference is not the accuracy bottleneck — training convergence is.

## Assembly

- `neural_uv_encode(u,v,dim,feat)` is the ONE encoding, called by both the CPU trainer and the GLSL render kernel, so they agree.
- `emit_neural_material_render_glsl` fuses encode + MLP + pixel write in **one pass**: the input cooperative vector is built by
  **component assignment** (`a0[i] = float16_t(sin(...))`) — coopvec supports per-lane writes, so no feature round-trip through
  memory. Each layer is one `coopVecMatMulAddNV` (matmul + bias) + component-wise `max` ReLU; the first 3 outputs clamp to RGBA8.
- Train on the CPU (fp32 forward/backward/Adam) → quantize weights to fp16 → upload → dispatch → read back → PSNR + BMP.

## Traps

- **Correct structure but washed / wrong colours ⇒ under-training, not a render or layout bug.** Isolate with a CPU fp32 forward
  PSNR: if it matches the (bad) GPU PSNR, the net didn't converge — the render/layout is fine. Here CPU-fp32 = 10.92 dB confirmed it
  was training, not the tensor path. Then switch GD → Adam.
- **A plain MLP on raw uv cannot fit texture detail** (spectral bias). Always frequency-encode the input for a neural field.
- coopvec load/store/stride offsets are **bytes** (multiple of 16); fp16 input matmul only supports an **fp16 result** combo (see
  the C6 scar). The `RowMajor` layout renders correctly with tightly-packed weights — no optimal-layout conversion needed.
- **Portability:** this is the Vulkan `VK_NV_cooperative_vector` path. DX12 Cooperative Vectors (HLSL `dx/linalg.h`, SM6.9 + the
  D3D12 Cooperative-Vector tier via the Agility SDK) is a platform preview — not present in this toolchain, so the DX12 neural path
  is env-blocked (documented, not declined). CUDA/Metal have no per-invocation coopvec primitive; the batched neural path there is
  the coopmat/WMMA fused MLP (`ckir_mlp.hpp`).
