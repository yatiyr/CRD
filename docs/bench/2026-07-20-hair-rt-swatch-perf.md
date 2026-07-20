# 2026-07-20 — B18-f path-traced hair swatch — GPU per-sample cost + real-time analysis

## Machine / config

- GPU: NVIDIA RTX 4070 Ti SUPER (Ada), Vulkan, `VK_KHR_ray_query` inline traversal (procedural-AABB BLAS + the
  analytic round-cone intersector; native `VK_NV_ray_tracing_linear_swept_spheres` is Blackwell-only, NOT on this card)
- Shader: CKIR → GLSL → SPIR-V via shaderc, **optimize = true** (default) — so these are optimised-shader numbers
- Scene: 1400×1000, one wavy-chestnut lock, 2600 strands / 109,200 swept segments, 68 µm root radius
- Path: 3 bounces, next-event estimation over 3 lights, 8-step per-channel transmittance shadow marches, 1 uniform-sphere indirect bounce, an analytic ground plane with a 5-step contact-shadow march
- Harness: `tests/gpu-context-vulkan/test_vulkan_hair_swatch_rt.cpp`, `kPerfSweep = true` (single dispatch per spp value)

## Method

`trace_dispatch` rebuilds the pipeline + reallocates buffers + blocks on a fence + reads back the whole image on
EVERY call, so a single dispatch folds a large FIXED cost into the GPU work. Sweeping spp in single dispatches
separates them: the SLOPE (Δtime/Δspp) is the pure per-sample GPU cost; the INTERCEPT is the fixed per-dispatch
overhead a real renderer pays ONCE (persistent pipeline, GPU-side accumulation, one readback), not per frame.

## Measured (wall-clock around one dispatch each)

| spp | total ms | fixed-removed ms/spp |
|----:|---------:|---------------------:|
|   2 |   3718.9 | — |
|   8 |   4926.5 | — |
|  32 |   9549.3 | — |
|  96 |  21997.4 | — |

Linear fit across the three largest points (the 2-spp point agrees):

> **time ≈ 3350 ms fixed + 194 ms × spp**

- **Pure GPU cost of ONE full-frame sample: ~194 ms** at 1400×1000 (1.4 Mpix) = **~139 ns / sample / pixel**.
- Fixed per-dispatch overhead ≈ **3.35 s** — pipeline rebuild + buffer realloc + 16 MB readback + fence. This is
  a harness artifact of the one-shot `trace_dispatch`, NOT a rendering cost; a real renderer pays it once.

## Verdict — this configuration is an OFFLINE / film renderer

A converged film frame needs ~256–512 spp. At 194 ms/spp that is **~50–100 s of pure GPU per frame**
(384 spp ≈ 75 s). That is squarely film/offline, not interactive.

Real-time budgets for reference: 16.6 ms (60 fps), 33.3 ms (30 fps). At 194 ms/spp we cannot afford even ONE
full-frame sample per frame — the whole frame is ~12× over a 60 fps budget at 1 spp before any convergence.

## Can we do real-time? — the honest lever analysis

Real-time hair IS reachable, but NOT with this exact path. Nobody path-traces hair to convergence per frame; the
real-time recipe is "1–4 spp + denoise + temporal + cheaper shadows", and **the engine already has every piece**:

| Lever | Factor | Notes |
|---|---:|---|
| **Denoise + temporal accumulation** (B14 ReSTIR / SVGF, already built + GPU-verified) | ~50–100× | The big one. A denoiser turns 1–4 spp into a clean image; converged needs ~384. This is how ALL real-time path tracing works. |
| **Deep-opacity-map shadows** (B18-c, already built) in place of the 8-step transmittance march | ~5–10× | The 8-step × 3-light × 3-bounce shadow march dominates per-sample cost; a DOM is one texture fetch. Biggest per-sample lever. |
| **1 indirect bounce** instead of 3 | ~2–3× | The interior GI is bounces 2+; real-time keeps 0–1 + the denoiser fills the rest. |
| **Reduced internal res + upscale** (DLSS-class) | ~2–4× | Hair covers a fraction of a game frame; render at ~half res and upscale. |
| **Native LSS hardware** (`VK_NV_ray_tracing_linear_swept_spheres`, Blackwell) | unquantified | The analytic intersector runs per-candidate-AABB in software here; hardware LSS traverses swept spheres natively. Not measurable on this Ada card. |

Product of the four quantified levers ≈ **1000–24000×**, comfortably past the ~12× (1 spp/60 fps) to ~700×
(converged/60 fps) gap. So real-time RT hair on this hardware is plausible with the denoiser + DOM shadow +
single-bounce path already present in the engine.

Separately: the engine ALSO ships the **pure-raster hair tier** (B18-a…e: deferred rasterisation + deep opacity
maps + the Lipp compositing filter) — the shipping-games real-time path, no RT required. This RT path is the
FILM-quality reference that tier is measured against.

## Lever sweep — MEASURED (2026-07-21)

Per-config GPU slope (time at spp=96 minus spp=8, ÷88 — isolates GPU cost from the fixed per-dispatch overhead).
Same machine/scene. This replaces the earlier estimate-only lever analysis with real numbers.

| Config | ms / full-frame-sample | speedup | 60 fps buys |
|---|---:|---:|---:|
| OFFLINE 3-bounce, 8-step shadow | 193.6 | 1× | 0.09 spp |
| 2-bounce, 4-step | 92.3 | 2.1× | 0.18 spp |
| **1-bounce, 2-step (real-time)** | **29.0** | **6.7×** | 0.57 spp |
| 1-bounce, 1-step (hard shadow) | 20.7 | 9.4× | 0.80 spp |
| 1-bounce, 0-step (no shadow) | 13.4 | 14× | 1.24 spp |

Reads: the **primary + BCSDF floor is 13.4 ms** (full 1.4 Mpix screen of hair, no shadow) — already ~75 fps for
visibility+shading. **Each shadow step ≈ 7 ms** (3 lights) — the shadow is the expensive part, which is why the DOM
lever matters most. **1-bounce + 2-step = 29 ms = 34 fps** full-screen at 1 spp — already 30-fps real-time for a
whole screen of hair; half-res or partial coverage clears 60 fps.

## Real-time path — MEASURED + rendered (2026-07-21)

The `kRealtime` mode: the 1-bounce/2-step config at **1 spp per frame** with temporal accumulation. Per-frame GPU
cost ~29 ms (the lever figure; the wall-clock includes the ~3.35 s one-shot harness overhead, a harness artifact).
Rendered proof — one 1-spp frame is noisy (`build/rt_1frame.bmp`), 96 frames accumulated is clean
(`build/rt_accum.bmp`): temporal accumulation converges the noise, which is what an SVGF denoiser does INSTANTLY
from a 1–4 spp frame. The engine's SVGF à-trous costs **0.236 ms/1080p** (`docs/bench/2026-07-15-gi-atmosphere-vulkan.md`),
negligible next to the 29 ms trace.

**Verdict:** real-time RT hair is reached — 1-bounce + short-shadow + 1 spp/frame + temporal/SVGF fits a real-time
budget on this Ada card. The full lever recipe (incl. DOM shadows + dual scattering, both built in-engine, not yet
wired into one frame) is `docs/recipes/2026-07-21-hair-realtime.md`.

## Not yet measured (open)

- The Tier-A assembly (DOM shadow + dual scattering + SVGF in ONE frame) is documented in the real-time recipe but
  not yet wired into a single measured pass — the next perf slice if real-time hair is pursued to shipping.
- DX12 per-sample cost (the correctness gate passes on DX12; perf not swept there).
