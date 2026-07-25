# REN-3.1 — depth-only pre-pass cost vs the equivalent colour pass (Vulkan + DX12)

**Measured** 2026-07-25 · **why**: this is the per-frame baseline REN-3.2 (CSM) multiplies by cascade count, so it
must exist *before* cascades land. The question the board answers is "what does adding a shadow pass cost?", not
an abstract throughput number.

## Machine / config

| | |
|---|---|
| CPU | Intel i9-14900K (32 logical) |
| GPU | the host's device — Vulkan (headless, `VK_EXT_shader_object`) and D3D12 |
| Build | `win-debug` (`/Od /RTC1`) — **CPU-side numbers are debug-build costs**, not shipping |
| Harness | `tests/gpu-context-{vulkan,dx12}/test_*_frame_graph.cpp`, tag `[ren3-bench]` (tracked, re-runnable) |
| Method | CPU wall-clock ms/frame, 30 frames per timing, **min-of-5** repetitions |

## What is compared

**Both arms use the SAME imported colour+depth target and the SAME frame-graph shape** — import target, import
storage, one pass, `build()`, `execute()`, `reset()`. The *only* difference is which draw runs:

- **depth-only** — `draw_storage_depth_only` (no colour attachment bound, depth writes only)
- **colour+depth** — `draw_storage_depth` / `_load` (colour attachment + depth)

## Board — BOTH backends

*ratio > 1 ⇒ the depth-only pass is cheaper.*

### Vulkan

| resolution | draws | depth-only (ms) | colour+depth (ms) | ratio |
|---:|---:|---:|---:|---:|
| 512² | 1 | 0.1993 | 0.1969 | 0.99× |
| 512² | 16 | 0.4449 | 0.5514 | **1.24×** |
| 1024² | 1 | 0.4459 | 0.4572 | 1.03× |
| 1024² | 16 | 0.6947 | 0.8132 | **1.17×** |
| 2048² | 1 | 1.4881 | 1.5053 | 1.01× |
| 2048² | 16 | 1.7843 | 1.9207 | **1.08×** |

### DX12

| resolution | draws | depth-only (ms) | colour+depth (ms) | ratio |
|---:|---:|---:|---:|---:|
| 512² | 1 | 0.1595 | 0.1557 | 0.98× |
| 512² | 16 | 0.2099 | 0.1720 | **0.82×** |
| 1024² | 1 | 0.4145 | 0.4051 | 0.98× |
| 1024² | 16 | 0.5744 | 0.4356 | **0.76×** |
| 2048² | 1 | 1.4670 | 1.4335 | 0.98× |
| 2048² | 16 | 2.0386 | 1.5416 | **0.76×** |

## Verdict — and a backend ASYMMETRY reported as measured

**At one draw the two passes are within noise on both backends** (0.98–1.03×): the frame is dominated by submit
+ fence, not by the attachment set.

**At 16 draws the backends DISAGREE, and the disagreement is consistent and reproducible:**
- **Vulkan**: depth-only is **8–24% cheaper**, as expected — no colour attachment writes per draw.
- **DX12**: depth-only is **~24% MORE expensive** (0.76–0.82×), at every resolution.

This is counter-intuitive and is reported rather than tuned away. Two candidate causes were **eliminated by
measurement**, not by argument:
1. *A clear per draw* — the first draft cleared the depth map on every depth-only draw while the colour arm
   cleared once and loaded. Fixed (see the scar below); the DX12 asymmetry **survived** the fix.
2. *Different per-draw state cost* — the two record paths are structurally identical: same
   `frame_alloc_storage_slot`, same `OMSetRenderTargets`, same viewport/scissor/root-signature/PSO/topology
   sets, same `DrawInstanced`. The only difference is `0` vs `1` bound RTVs.

**Leading hypothesis**: the driver's handling of a `NumRenderTargets = 0` PSO / zero bound RTVs on this
hardware — plausibly losing a fast path that the colour configuration keeps. **What would settle it**: per-pass GPU
timestamps, which arrive with REN-8's timestamp-query backend. CPU wall-clock cannot attribute this.

**Practical consequence for REN-3.2**: do **not** assume a shadow pass is cheaper than a colour pass on DX12.
Budget CSM cascades at *colour-pass cost or worse* on that backend.

**Sizing REN-3.2 from this:** a shadow pass is not free. 4 cascades at 1024² ≈ **4 × 0.45 ms ≈ 1.8 ms/frame**
(Vulkan) or **4 × 0.57 ms ≈ 2.3 ms/frame** (DX12) of CPU-side pass cost in a debug build, before any scene
geometry. That is the budget CSM must justify, and the reason cascade culling matters as much as `csm_texel_snap`.

## ⚠ Methodology scar — the first draft of this board was WRONG

The first run reported depth-only as **3× SLOWER** at 512² (ratio 0.31×). That was a harness defect, not a
result: the depth arm created a fresh `D32Float` **transient** inside the timing loop while the colour arm
imported a pre-made target, so it measured **transient allocation + graph rebuild**, not pass cost. Both arms
now share one imported target and an identical graph shape. Kept here because a plausible-looking board with an
unequal comparison is exactly the kind of number that gets quoted later (SANITY #5/#6).

## A REAL API GAP this bench found (fixed at the spot)

Writing the multi-draw arm surfaced that `draw_storage_depth_only` had **no `_load` companion** — every call
clears. A shadow pass draws many meshes into ONE map, so the second mesh would have **wiped the first** and the
shadow map would have contained only the last occluder. A latent correctness bug queued for REN-3.2, found
because the bench needed an honest multi-draw comparison. `draw_storage_depth_only_load` was added on both
backends (appended at END, D135) and both benches now clear once and load.

## Honest limits

- **Debug build.** These are `/Od` CPU-side costs; shipping numbers will be lower. The *ratio* is the portable
  part of the result, not the absolute milliseconds.
- **CPU wall-clock, not GPU timestamps.** The frame graph still submits-and-waits (REN-1 kept the wait), so
  wall-clock is a fair per-frame measure, but it cannot attribute time to the GPU — which is exactly why the DX12
  asymmetry above has a hypothesis rather than a cause. REN-8's timestamp backend is what closes that.
- **Fullscreen-triangle geometry.** The draws are trivial; a real shadow pass is geometry-bound, so the relative
  saving of dropping colour will be smaller in a real scene.
