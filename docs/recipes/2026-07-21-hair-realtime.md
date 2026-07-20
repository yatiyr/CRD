# Recipe — Real-time hair (games)

> The same physically-based hair as the offline recipe, but at frame rate. This is not a different look — it is
> the SAME fibre BCSDF and geometry with the transport approximated so it fits a 16–33 ms budget. Read the offline
> recipe first (`2026-07-21-hair-offline-film.md`) for the fibre model and its parameters; this recipe is only the
> performance transformation and the two things it adds (temporal denoising + a pure-raster fallback).

---

## 1. Parameters first — what changes from the film config

Everything in the offline parameter table still applies (same BCSDF, same geometry). The renderer config changes:

| Parameter | Film | Real-time | Why |
|---|---|---|---|
| `bounces` | 3 | **1** | The interior GI becomes a cheap ambient / dual-scattering term, not a ray path. Biggest single lever after shadows. |
| `shadow_steps` (per light) | 8 | **1–2**, or a DOM lookup | The 8-step transmittance march dominates per-sample cost. |
| `spp` per FRAME | 384–512 total | **1** | One sample per frame is the real-time contract. |
| convergence | high spp | **temporal accumulation + spatial denoise** | Frame 1 is noisy; accumulate across frames (static camera) or denoise a 1–4 spp frame (moving camera). |
| internal resolution | native | **~half, + upscale** | Hair covers a fraction of a game frame; render smaller, upscale. |
| `env_lo/hi` | dim | **lifted** | A brighter ambient stands in for the multiple bounces removed. |

Everything else — melanin σₐ, β_m/β_n/α, the groom parameters — is identical. The *hair* is the same; only the
*transport budget* changes.

---

## 2. Why this works — the measured lever breakdown

Board: `docs/bench/2026-07-20-hair-rt-swatch-perf.md` (§ lever sweep). RTX 4070 Ti SUPER, 1400×1000, per full-frame
sample, optimised SPIR-V:

| Config | ms / full-frame-sample | speedup vs film |
|---|---:|---:|
| OFFLINE 3-bounce, 8-step shadow | 193.6 | 1× |
| 2-bounce, 4-step | 92.3 | 2.1× |
| **1-bounce, 2-step** | **29.0** | **6.7×** |
| 1-bounce, 1-step (hard) | 20.7 | 9.4× |
| 1-bounce, 0-step (no shadow) | 13.4 | 14× |

Read this carefully:

- **The primary + BCSDF floor is 13.4 ms** at a FULL 1.4 Mpix screen of nothing but hair. That is already ~75 fps
  for the visibility + shading with no shadow.
- **Each shadow step costs ~7 ms** (for 3 lights). The shadow is the expensive part, which is exactly why the
  deep-opacity-map lever (below) matters most.
- **1-bounce + 2-step = 29 ms = 34 fps** full-screen at 1 spp. That is ALREADY 30-fps real-time for a full screen
  of hair. Half-resolution or partial screen coverage (hair is rarely the whole frame) clears 60 fps.

So the film path was never "too slow to be real-time" — it was doing 512 samples of a 3-bounce, 8-step-shadow path
FOR A REFERENCE. The real-time path is the same renderer at 1 spp with the cheap config, and a denoiser.

---

## 3. The levers, in order of impact — and where each lives in the engine

Every lever here is a technique the engine already has. Real-time hair is an ASSEMBLY of existing pieces, not new
maths.

1. **Temporal accumulation + spatial denoise (the ~100× lever).** Nobody converges per frame. A 1–4 spp frame is
   noisy (see the images: one real-time frame is speckled); a denoiser turns it into the clean converged image.
   - **Temporal**: reproject last frame by the motion vector, blend with this frame's 1 spp. Free convergence
     while the camera is still; a history-clamp handles motion. This recipe DEMONSTRATES it: `rt_1frame.bmp` (one
     1-spp frame, noisy) vs `rt_accum.bmp` (96 frames accumulated, clean) — the same result a denoiser produces
     instantly.
   - **Spatial (SVGF à-trous)**: `engine/kir/include/crd/kir/ckir_svgf.hpp` (B14). The engine's own board measures
     the à-trous pass at **0.236 ms / 1080p** (`docs/bench/2026-07-15-gi-atmosphere-vulkan.md`) — negligible next
     to the 29 ms trace. Edge-stopping on depth/normal/luminance keeps the fibre detail.
   - **ReSTIR** (`ckir_restir.hpp`, B14) reuses light samples spatiotemporally — a further variance cut for the
     many-lights case.
2. **Deep opacity map shadows (the ~5–10× shadow lever).** Replace the per-light 8-step transmittance MARCH (up to
   72 curve traces/sample) with a light-space **deep opacity map** built once per frame, then ONE lookup per shade.
   Yuksel & Keyser 2008; `build_dom_build_kernel` + `build_dom_lookup_kernel` (`ckir_hair_scatter.hpp`, B18-c). The
   DOM positions ~3–4 opacity layers from a per-pixel front-depth z0 so they conform to the groom — 3 layers where
   opacity shadow maps need 16–128 and still stripe. This is the single biggest per-sample lever because shadows
   dominate (each step ≈ 7 ms measured).
3. **Dual scattering for multiple scattering (the ~3× bounce lever).** Instead of true multi-bounce, approximate
   the groom's global multiple scattering with **Zinke 2008 dual scattering** (`build_dual_scatter_kernel`, B18-c):
   a global forward-transmittance term T_f (from the DOM's strand count n) plus a local back-scatter S_f. This is
   THE real-time answer to interior GI — it is what makes pale hair glow without tracing bounces. Drive it from the
   same DOM the shadows use, so the cost is shared.
4. **Reduced internal resolution + upscale (~2–4×).** Hair covers a fraction of a game frame; render the hair
   region at ~half res and upscale (DLSS-class). Sub-pixel fibres + coverage make hair tolerant of this.
5. **Native LSS hardware — future.** `VK_NV_ray_tracing_linear_swept_spheres` traverses swept spheres in silicon
   (the analytic intersector runs per-candidate-AABB in software here). Blackwell-only; N/A on this Ada card, but a
   free win where present, from the same scene description.

Product of the measured/known levers: 6.7× (config) × ~100× (denoise vs 512 spp) × ~2× (res) ≈ **1000×+**,
comfortably inside budget.

---

## 4. The full real-time assembly (games)

Two tiers ship; pick per platform / quality setting.

### Tier A — RT hair (high-end / RT-capable GPUs)

Per frame:
1. **Build the DOM** from the fibres in light space (once per light, ~3–4 layers).
2. **Trace 1 spp**: primary `TraceRayCurves` → the fibre BCSDF, direct lighting with a DOM shadow LOOKUP (not a
   march), + the dual-scattering ambient for interior GI. Output radiance + motion vector + depth/normal.
3. **Denoise**: temporal reproject + accumulate, then SVGF à-trous. → clean frame.
4. **Composite + tonemap.**

The kernel is the offline `build_rt_hair_swatch_kernel` with `bounces=1`, the shadow march replaced by a DOM
lookup, and the env lifted — plus the denoise passes.

### Tier B — pure raster (all GPUs, the shipping default)

No RT at all. The engine's B18-a…e raster tier: deferred rasterisation of the strands into a G-buffer + the SAME
Chiang BCSDF in the fragment shader + deep opacity map self-shadow + dual scattering + the **Lipp 2026
tangent-oriented compositing filter** that resolves the sub-pixel coverage the deferred buffer loses. This is the
path that runs on hardware without ray tracing, and it is where the sub-pixel strand-per-pixel problem is solved by
the compositing filter rather than by rays.

Both tiers use the identical fibre model — the difference is purely how visibility and shadows are resolved.

---

## 5. The traps specific to real-time

The offline traps (recipe §6) ALL still apply — they are in the shared BCSDF/geometry. Additional real-time ones:

- **1-spp noise is not a bug, it is the input to the denoiser.** Judge a real-time frame AFTER temporal+SVGF, never
  the raw 1-spp buffer. The `rt_1frame.bmp` vs `rt_accum.bmp` pair shows the difference.
- **The DOM must position layers from a per-pixel z0**, not from fixed light-space depths, or it stripes (Yuksel's
  whole point). See `build_dom_build_kernel`'s comments.
- **Dual scattering is a DISTRIBUTION, not a scalar.** T_f is the scalar attenuation; S_f is a spread over incoming
  directions (a delta at n=0) and must be integrated against the BCSDF, not multiplied onto the direct lobe — doing
  the latter blacks the frame. (B18-c scar, recorded when the dual-scatter tier was built.)
- **Temporal history clamping** is required under motion or the hair ghosts; the fibre detail is high-frequency and
  reprojection is imperfect at sub-pixel width.

---

## 6. What is built vs. documented here

Honest status (2026-07-21):
- **Built + measured:** the lever breakdown (the config transformation), and the 1-spp + temporal-accumulation
  real-time path (`kRealtime` in `test_vulkan_hair_swatch_rt.cpp`) — measured 29 ms/frame GPU, with the noisy/clean
  image pair proving temporal convergence.
- **Built in-engine, not yet wired into the RT hair frame:** the DOM shadow (B18-c), dual scattering (B18-c), SVGF
  (B14). Each has its own gates + measured cost; assembling them into the single Tier-A frame above is the
  documented next slice.
- **Documented, measured elsewhere:** the SVGF à-trous cost (0.236 ms/1080p) is from the engine's GI board, not
  re-measured on hair.

## 7. Where the code lives

- `engine/kir/include/crd/kir/ckir_hair_rt.hpp` — the trace kernel (set `bounces=1`, short shadow for real-time)
- `engine/kir/include/crd/kir/ckir_hair_scatter.hpp` — DOM build/lookup + dual scattering (B18-c)
- `engine/kir/include/crd/kir/ckir_svgf.hpp`, `ckir_restir.hpp` — the denoisers (B14)
- `tests/gpu-context-vulkan/test_vulkan_hair_swatch_rt.cpp` — `kRealtime` mode + `kPerfSweep` lever measurement
- Board: `docs/bench/2026-07-20-hair-rt-swatch-perf.md`

## 8. Papers (additional to the offline recipe)

- Yuksel & Keyser 2008, *Deep Opacity Maps* (EG) — the conforming-layer self-shadow.
- Zinke et al. 2008, *Dual Scattering Approximation for Fast Multiple Scattering in Hair* (SIGGRAPH) — the
  real-time multiple-scattering model.
- Schied et al. 2017, *Spatiotemporal Variance-Guided Filtering (SVGF)* (HPG) — the 1-spp denoiser.
- Bitterli et al. 2020, *Spatiotemporal Reservoir Resampling (ReSTIR)* (SIGGRAPH) — many-light variance reduction.
- Lipp et al. 2026 — the tangent-oriented deferred compositing filter (the raster tier).
