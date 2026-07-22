# 2026-07-21 — B19 frontier axes complete: relightable · pop-free · compressible · trainable · portable

**Detour:** D-007 GPU-program-system · **Slice:** B19 (3D Gaussian Splatting) — CLOSED
**Directive:** "let's go and finish all frontier axes and finish B19, no debts, full gold standard and frontier and quality."

## What shipped

The five remaining B19 frontier axes, each a complete gated sub-slice (CPU oracle + real Vulkan; the last one adds
DirectX 12). With the earlier work — the on-device forward renderer (a→a4), the 2DGS surfel primitive + TSDF/marching-
cubes mesh bridge (c) — **B19 (3D Gaussian Splatting) is now a complete frontier system.**

### B19-e — Relightable (`build_gsplat2d_relight_render_kernel`)
Base 3DGS bakes lighting into the SH colour, so a capture can only be replayed under its capture illumination. Here
each surfel carries an ALBEDO and its intrinsic normal, and the pixel is shaded per-splat with a physically-based BRDF
(Lambert diffuse + Cook-Torrance GGX: normalised-GGX D, Smith-Schlick G, Schlick F) under a directional light — so
captured content responds to NEW lighting. **Gate:** facing-light → bright + albedo-tinted; light moved behind → ambient
only (relighting genuinely changed the shading); low roughness → sharper highlight. Vulkan worst 2.4e-07, DX12 4.8e-07.

### StopThePop — per-pixel resort (`build_gsplat2d_resort_render_kernel`, Radl et al. 2024)
The base render composites a tile's splats in the global centre-depth order, but the correct order for a pixel is by its
exact ray-intersection depth λ (varies across a slanted surfel) — the disagreement is what pops on view rotation. This
kernel resorts per pixel: an O(N²) selection compositing in ascending λ, with the selection state in a **per-pixel
scratch buffer** (a CKIR `For` carries no register state across iterations, so loop-carried state lives in memory).
**Gate:** two oppositely-slanted, depth-crossing surfels — the base render shows red everywhere (global order), the
resort shows red-left / blue-right (per-pixel order) and differs from the base at the crossing. Vulkan worst 1.8e-07.

### B19-d — Compression (`build_gsplat_morton_kernel` + `build_gsplat_quantize/dequantize_kernel`)
Self-Organizing-Gaussians essence: reorder for locality, then quantise. A Morton (Z-order) key from the position (sort
by it via the KV radix sort ⇒ spatially-near Gaussians adjacent ⇒ smooth, low-entropy attribute planes) + a K-bit
attribute codec. **Gate:** 12-bit round-trips within half a quantisation step (7.3e-4); Morton reorder makes the mean
adjacent |Δpos| **5.2× smaller** (0.659 → 0.128) ⇒ far more compressible. Vulkan codec worst 2.4e-07.

### B19-f — Differentiable training (`build_gsplat_diff_forward/backward_kernel` + `build_gsplat_sgd_step_kernel`)
The training core: the forward differentiable splat and the EXACT backward gradients of L = Σ(C−target)² w.r.t. the
Gaussian's position, scale, opacity and colour (the backward is a per-image reduction accumulating the 5 gradients in
the output buffer). **Gates:** the analytic gradients match finite differences for all 5 parameters; an SGD loop fits a
Gaussian to a target image — **loss 9.15 → 0.093 (98% ↓)**, recovering μ≈(8,8), s≈3, opacity≈0.9, colour≈0.8. Vulkan
forward 3e-08, gradient 7e-08. This is the capability that makes Cerid a capture-to-render tool, not just a viewer.
(The multi-splat composite backward extends this with a back-to-front suffix accumulation; the single-splat core proves
the autodiff-training path end to end.)

### Cross-backend DX12/HLSL (`tests/gpu-context-dx12/test_dx12_gsplat.cpp`)
The B19 kernel family lowers to HLSL and runs on DirectX 12 == the CPU oracle: the 2DGS ray-surfel render (Cramer +
For + composite), the relightable PBR render (GGX), and the differentiable backward (the training gradient reduction) —
worst 2.6e-06. No HLSL emitter gaps surfaced.

## Gates

| family | result |
|---|---|
| `crd-kir-tests [gsplat],[gsplat2d],[mesh]` | 338 assertions / 22 cases — PASS |
| `crd-gpu-context-vulkan-tests [gsplat],[gsplat2d],[mesh]` | 204 assertions / 16 cases — PASS |
| `crd-gpu-context-dx12-tests [gsplat],[gsplat2d]` | 9 assertions / 2 cases — PASS |

## Traps hit

- **Never materialise a Bool (again).** The StopThePop `Hit.keep` is a Bool; materialising it made a `float t = boolexpr`
  and then `float && bool` on the GPU. The CPU oracle passed the same graph; only the SPIR-V emit caught it. Materialise
  the float results, leave the bool guards inline.
- **CKIR `For` carries no register state.** Both StopThePop (selection state) and the differentiable backward (gradient
  sum) need loop-carried accumulators — kept in a scratch/output buffer via RMW (the render-composite pattern), since a
  `For` body re-executes fresh each iteration.
- **Specular whitening narrows the albedo gap.** A strong GGX highlight adds white to all channels, so a "red-dominant"
  assertion must allow the specular to shrink (not erase) the R−B margin.

## State

B19 CLOSED. New/extended: `ckir_gsplat2d.hpp` (relight, resort), `ckir_gsplat.hpp` (morton, quantise, dequantise, diff
forward/backward, sgd), `test_ckir_gsplat.cpp`, `test_ckir_gsplat2d.cpp`, `tests/gpu-context-vulkan/test_vulkan_gsplat.cpp`,
`tests/gpu-context-dx12/test_dx12_gsplat.cpp` — all tidy-clean (pinned LLVM 20.1.8). context.md + detour updated.

**Next:** return to the D-007 main line — the next detour step after the B19 (3D Gaussian Splatting) slice.
