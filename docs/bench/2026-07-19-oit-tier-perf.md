# 2026-07-19 — B17 OIT tier GPU PERF board (kernel-only cost per tier, high-overdraw)

The companion to the accuracy board (`2026-07-19-oit-tier-scoreboard.md`): *how much each order-independent-transparency
tier costs on the GPU*. Accuracy tells you which tier is close enough; this tells you what you pay for it. All tiers are
CKIR compute kernels on Vulkan; GPU-timed with `last_gpu_ms` (brackets only the recorded compute dispatches — upload/readback
excluded, like the FFT/GI boards), min-of-30.

## Config
- **GPU:** RTX 4070 Ti SUPER (Vulkan `VK_EXT_shader_object`); host i9-14900K. win-debug functional build (`/O2` shaders via
  glslang; the C++ harness build config does not affect GPU kernel time).
- **Scene:** 1024×1024 px, **4 translucent layers** (4,194,304 fragments), background composited. High overdraw via resolution;
  layers=4 because the fully-unrolled sort-network resolve explodes the inline-`select` emit at 8 layers (a separate emitter
  axis — the *comparison* between tiers is valid at 4-layer depth). Stochastic S=32 sub-samples.
- **Harness (re-runnable):** `crd-gpu-context-vulkan-tests "[.oit-bench]"` (hidden bench;
  `tests/gpu-context-vulkan/test_vulkan_context.cpp`). Kernels: `engine/kir/include/crd/kir/ckir_oit.hpp`.
- **Method:** the shared deferred STORE is timed once (the fragment-capture cost the moment/A-buffer resolves share); each
  resolve is timed reading the prefilled store; the atomic tier resets its head buffer each run (untimed) then times
  build+resolve; stochastic is the single kernel. min-of-30, kernel-only.

## Board — kernel-only GPU time (Vulkan, `last_gpu_ms`, min-of-30)

| Tier | Kernel(s) | GPU time | Total (with shared store) | Notes |
|---|---|---|---|---|
| **STORE (shared)** | deferred fragment capture | **0.130 ms** | — | coalesced write of `layers` fragments/pixel; shared by A-buffer + MBOIT |
| **A-buffer (static)** | store + sort-resolve | 0.155 ms | **0.285 ms** | exact; per-pixel unrolled compare-exchange sort + `over` |
| **MBOIT-4** | store + Hamburger-2 | 0.153 ms | **0.283 ms** | 2 masses exact; reconstruction is FREE (memory-bound) |
| **MBOIT-6** | store + Hamburger-3 | 0.144 ms | **0.274 ms** | 3 masses; the cubic root-solve is FREE — same cost as plain A-buffer resolve |
| **A-buffer (atomic list)** | atomic build + walk-resolve | **3.010 ms** | 3.010 ms | ~10× the static tiers: `atomicExchange`(head)/`atomicAdd`(counter) contention + divergent list walk — the price of UNBOUNDED depth |
| **Stochastic** | single kernel, S=32 | 1.964 ms | 1.964 ms | per-frame TAA cost @ S=1 ≈ **0.061 ms** — the CHEAPEST unbounded tier in a TAA pipeline (1 sample/frame) |

## Verdict
- **The static store-based tiers are the cheapest — and MBOIT's better accuracy is FREE at this depth.** A-buffer resolve,
  MBOIT-4, and MBOIT-6 all land at **0.14–0.16 ms** (≈0.28 ms with the shared store): they are **memory-bound**, so the moment
  reconstruction — even MBOIT-6's 4×4 Hankel Cholesky + cubic root-solve — is completely hidden behind the fragment-gather
  bandwidth. MBOIT-6 is even a hair *faster* than plain A-buffer resolve (it reads fixed-size moments, not `layers` fragments
  to sort). **So in the bounded-depth regime you get MBOIT's accuracy win over WBOIT at zero cost over the exact A-buffer.**
- **The atomic linked-list A-buffer costs ~10× (3.0 ms)** — the honest price of *unbounded* depth: `atomicAdd` on one global
  counter + `atomicExchange` on per-pixel heads serialize under contention, and the resolve's pointer-chase is divergent.
  This is the deployable exact tier when depth exceeds the moment budget AND the static fixed-slot layout won't fit; you pay
  for the dynamism. (The static A-buffer is far cheaper but needs a known max layer count.)
- **Stochastic is the cheapest UNBOUNDED tier per frame.** 1.96 ms for S=32 in one dispatch, but the real deployment is
  **1 sample/frame + TAA** — the linear per-sample cost is **~0.061 ms/frame**, cheaper than *every* other tier, at the price
  of temporal noise that TAA resolves. No sort, no atomics, no per-pixel list; unbounded depth. The tier for a TAA pipeline.
- **The cost/accuracy map is complete:** bounded depth → MBOIT (free accuracy) or static A-buffer (exact, cheap); unbounded
  depth in a TAA pipeline → stochastic (cheapest/frame, noisy); unbounded depth, exact, single-frame → atomic A-buffer (exact,
  10× cost). WBOIT (raster blend, measured on the raster path) remains the single-pass floor when even 0.28 ms is too much.

Numbers are min-of-30 and stable to <1% across runs. Re-run: `crd-gpu-context-vulkan-tests "[.oit-bench]"`.
