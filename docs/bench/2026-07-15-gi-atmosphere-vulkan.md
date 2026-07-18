# CKIR GI + atmosphere — Vulkan GPU throughput board (2026-07-15)

The thesis for the visual-frontier kernels (B14 real-time GI + B15 sky/atmosphere): a **portable IR pays NO performance tax**.
The exact same CKIR source that is verified **bit-exact vs the CPU oracle and across backends** (Vulkan/DX12) compiles to GPU
code that runs at **real-time 1080p rates**, memory-bandwidth-bound — the correct regime for these gather/stencil kernels
(like the FFT board, you target the bandwidth wall, you don't beat it with more FLOPs). Portability + determinism **and**
real-time speed from one authored kernel is the frontier-2026 claim.

## Machine / config

- **GPU:** NVIDIA GeForce RTX 4070 Ti SUPER (Ada, AD103), ~44 TFLOP/s FP32 peak, **~672 GB/s** peak memory bandwidth.
- **Path:** CKIR statement-tier compute → GLSL (`emit_compute_kernel_glsl`) → SPIR-V (shaderc) → Vulkan compute pipeline.
- **Timing:** `VulkanComputeContext::last_gpu_ms()` — device timestamps bracketing the recorded dispatch, **kernel-only**
  (upload/readback excluded), **min-of-30**. Harness: `tests/gpu-context-vulkan` `[.gi-bench]` (hidden — run explicitly).
- **Clocks:** NOT locked (`nvidia-smi -lgc` needs elevation); min-of-30 mitigates boost/thermal variance.
- **Correctness:** every kernel here is separately verified bit-exact vs the CPU oracle in the non-hidden `[atmos]`/`[ddgi]`/
  `[restir]`/`[svgf]` cases; this board measures only their throughput.

## Board — kernel-only GPU time (min-of-30)

| Kernel | slice | work | min ms | throughput | achieved BW¹ |
|---|---|--:|--:|--:|--:|
| **DDGI probe-sample** (8-probe trilinear × Chebyshev × oct-irradiance) | B14-b | 2,073,600 queries (1080p) | **0.171** | 12.2 Gqueries/s | ~436 GB/s (**65% peak**) |
| **SVGF à-trous** (1 edge-stopping wavelet iter, 5×5) | B14-c | 2,073,600 px (1080p) | **0.236** | 8.8 Gpixels/s | L2-bound (stencil reuse) |
| **ReSTIR RIS** (M=32 WRS reservoir) | B14-a | 262,144 px × 32 cand | **0.318**† | 824 Mpixels/s | reservoir-loop bound |
| atmos **transmittance** LUT (256×64, 40-step march) | B15-a-1 | 16,384 texels | **0.0029** | — | once/frame |
| atmos **multiscatter** LUT (32×32, sphere×march) | B15-a-2 | 1,024 texels | **0.0422** | — | once/frame |
| atmos **sky-view** LUT (192×108, single+multi) | B15-a-3 | 20,736 texels | **0.0686** | — | once/frame |
| atmos **aerial-perspective** froxels (32×32×16) | B15-a-4 | 16,384 cells | **0.0076** | — | once/frame |

† ReSTIR RIS was measured at 0.527 ms with the ORIGINAL unrolled kernel; the head-to-head board
(`2026-07-15-ckir-vs-handwritten-glsl.md`) found the unroll tanked occupancy, and the loop-rewrite fix brought it to **0.318 ms
= parity with hand-written**. This row reflects the fixed kernel.

¹ achieved bandwidth from the guaranteed global traffic (DDGI: pos3+nrm3+out3 = 9 f32/query = 74.6 MB / 0.171 ms). SVGF's 5×5
stencil is served from L2 (neighbour reuse) so it is compute/cache-bound, not DRAM-bound.

## Reading it

- **The entire physically-based sky costs ~0.12 ms/frame** (transmittance 0.003 + multiscatter 0.042 + sky-view 0.069 +
  aerial 0.008). The four Hillaire LUTs together are a rounding error in a frame budget — exactly as intended (they are small
  LUTs consumed by the full-screen sky/fog composite, which is the renderer leaf).
- **DDGI probe-sample is DRAM-bandwidth-bound at ~65% of peak** on a full 1080p screen in 0.171 ms — near the bandwidth wall
  for a gather kernel that touches 8 probes per query. This is the "you can't beat bandwidth, you ride it" regime.
- **SVGF** runs one edge-stopping iteration over 1080p in 0.236 ms; the gold 5-iteration pipeline is ~1.2 ms — real-time
  denoising headroom.
- **ReSTIR RIS** is bound by the per-pixel reservoir loop (M=32 weighted-reservoir updates, each a div + select), not DRAM;
  0.527 ms for 262 k pixels ⇒ ~2 ms extrapolated to 1080p for a 32-candidate initial pass (the spatial/temporal reuse passes
  are far cheaper — they merge single reservoirs).

## Honest caveats / next levers

- **No direct vendor peer is benchmarked here.** The natural peers (NVIDIA RTXGI/DDGI, NRD denoiser, RTXDI/ReSTIR) are
  proprietary CUDA/DXR and not runnable head-to-head in this harness. The defensible "crush" for a *portable IR* is the
  **zero-portability-tax** result above (real-time, bandwidth-bound, from bit-exact-portable source). The **definitive** proof
  is a CKIR-emitted-GLSL vs hand-tuned-GLSL head-to-head (same algorithm) showing the emitter matches hand-written — that is
  the next board.
- **Clocks unlocked** — re-measure with locked clocks before any final parity/crush claim.
- The **coopmat/cooperative-vector** MLP path (the NRC perf tier, B14-d) rides **B10**; the FP32 portable MLP already crushes
  cuBLAS fwd 2.37× / bwd 1.90× (`docs/bench` NRC board) — that is the tensor-tier crush this GI board complements.
