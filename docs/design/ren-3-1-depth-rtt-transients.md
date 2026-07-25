# REN-3.1 — RTT **DEPTH** transients (the shadow-map substrate)

**Parent**: REN-3 (`ren-3-lighting-shadow-pipeline.md`, D-007 row 100). **Status**: spec, 2026-07-25. First
just-in-time per-slice spec written under the `docs/design/README.md` convention.

**Why this is first**: `ckir_lighting.hpp:992` — *"the per-cascade shadow maps live in a 2D-array atlas the
shadow pass renders (B8-k/l — **the device has no float/array-depth upload yet**)"*. Every shadow test in the
repo binds a depth texture **uploaded from the CPU**. Nothing on the device can *produce* a shadow map. That
single gap blocks REN-3.2 (CSM) and every future depth pre-pass (SSAO, SSR, DoF, occlusion culling).

---

## Reuse audit — grepped, with evidence

This is the section the REN-5 mistake exists to enforce: whole-tree grep for the named symbols, read the whole
mechanism, cite file:line. **Result: substantially more exists than the parent spec assumed, and the two
backends are NOT symmetric.**

### Already present — reuse, do not rebuild

| capability | evidence |
|---|---|
| `FgImageFormat::D32Float` in the public frame-graph API | `frame_graph.hpp:89` |
| **VK: depth transients already create correctly** — `VK_FORMAT_D32_SFLOAT`, `VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT`, `VK_IMAGE_ASPECT_DEPTH_BIT` | `vulkan_raster_context.cpp:4090` |
| ⭐ **VK: `sampled` already adds SAMPLED usage GENERICALLY** — `if (desc.sampled) { usage \|= VK_IMAGE_USAGE_SAMPLED_BIT; }`, i.e. **a `D32Float`+`sampled` transient already gets `DEPTH_STENCIL_ATTACHMENT\|SAMPLED` today** | `vulkan_raster_context.cpp:3919` |
| `ImageNode` already carries `aspect` **and** a distinct `depth_layout` | `vulkan_raster_context.cpp:4004,4011` |
| The **borrowed-wrapper** discipline (dtor frees nothing; the ImageNode owns the bundle) | `set_borrowed()` at `:259`/`:777`, `m_borrowed` at `:218`/`:753` |
| The borrowed target+texture construction for a sampled transient | `vulkan_raster_context.cpp:4265-4278` |
| Frame-recording paths to mirror: `record_offscreen` (render into a transient) / `record_textured` (sample it) | `:2429`, `:2447` |
| **The depth SAMPLE side already works** — `create_depth_texture` (D32), the **comparison sampler** `m_cmp_sampler`, and `draw_shadow` which binds depth + cmp sampler | `raster_context.hpp:294,298`; `vulkan_raster_context.cpp:919,2917,2962` |
| Depth **testing** in the forward pass | `draw_storage_depth` / `_load` (`raster_context.hpp:462,475`) |
| DX12 depth transients create with `ALLOW_DEPTH_STENCIL` | `dx12_raster_context.cpp:3384,3541` |

**So on Vulkan the image is already created with the right usage.** The VK delta is the *depth-aware* borrowed
texture, the depth-only pass, and the barrier — not the allocation.

### ⛔ The DX12 asymmetry — the defect this slice will actually trip on

`dx12_raster_context.cpp:3541` returns **`DXGI_FORMAT_D32_FLOAT`** and `:3384` sets
`D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL`. **A D3D12 resource created with a fully-typed depth format cannot
have a Shader-Resource View created over it.** Sampling a depth target on DX12 requires the *three-format*
dance:

| view | format |
|---|---|
| the **resource** | `DXGI_FORMAT_R32_TYPELESS` |
| the **DSV** | `DXGI_FORMAT_D32_FLOAT` |
| the **SRV** | `DXGI_FORMAT_R32_FLOAT` |

None of that exists today — the transient path has no DSV heap and no SRV for depth. **This is the single
largest piece of work in REN-3.1 and it is DX12-only.** Budget for it explicitly; do not discover it.
(Vulkan needs no equivalent — one `VK_FORMAT_D32_SFLOAT` image serves both uses via aspect + layout.)

---

## The delta — what REN-3.1 actually builds

1. **`draw_storage_depth_only`** — a depth-write-only pass with **no colour attachment**.
   - ⛔ **This is a real interface change**: a new pure-virtual on `IRasterContext`, **appended at the END**.
     Inserting mid-vtable silently dispatches to the wrong method under win-release LTCG (the D135 scar,
     AGENTS.md "Append new pure-virtuals at the END").
   - VK: `vkCmdBeginRendering` with `colorAttachmentCount = 0` and only `pDepthAttachment`.
   - DX12: `OMSetRenderTargets(0, nullptr, FALSE, &dsv)`.
   - Frame-recording sibling `record_depth_only`, branched exactly as `draw_storage`/`draw_sampled` branch today.
2. **A borrowed depth TEXTURE** for a `D32Float`+`sampled` transient — mirror `:4273`'s color path but with
   the depth aspect (VK) / the `R32_FLOAT` SRV (DX12).
3. **DX12 only**: `R32_TYPELESS` placed resource + a per-transient **DSV heap** + the `R32_FLOAT` SRV, wired
   into the existing per-draw frame-heap ring.
4. **The RTT depth barrier** in `execute()`:
   - VK `VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL → VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL` (or
     `SHADER_READ_ONLY_OPTIMAL`; pick one and assert it — `depth_layout` already exists to track it);
   - DX12 `D3D12_RESOURCE_STATE_DEPTH_WRITE → D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE`.
5. **Bind the comparison sampler** when a pass samples a depth transient — `m_cmp_sampler` exists; the
   frame-recording set-write path must offer it (today `record_textured` takes an explicit sampler, so this is
   a caller change, not a new object).

## Gate (both backends, before REN-3.2 starts)

A **two-pass frame graph**, one submission:
- pass 1 — `draw_storage_depth_only` renders an occluder into a `D32Float`+`sampled` transient;
- pass 2 — samples it through the comparison sampler with `shadow_factor` and shades;
- readback shows **lit vs shadowed pixels differing** where the occluder blocks the light.

Assertions: exactly one submission · the depth RTT barrier is counted · **Vulkan validation-silent (0/0)** ·
DX12 debug layer clean · the transient still participates in aliasing (`physical_bytes < logical_bytes` stays
green) · a `sampled=false` depth transient still works as a plain depth buffer (no regression to
`draw_storage_depth`).

**Negative gate** (cheap, catches the DX12 trap): creating a `D32Float`+`sampled` transient must **succeed**
on DX12 and produce a non-null `texture()` — today it would either fail SRV creation or silently hand back
garbage.

## Bench

Depth-pre-pass cost vs the equivalent colour pass, both backends → `docs/bench/` at measurement time. This is
the per-frame baseline REN-3.2 multiplies by cascade count, so it must exist before CSM lands.

## Files

- `raster_context.hpp` — `draw_storage_depth_only` **appended at END**.
- `vulkan_raster_context.cpp` — `record_depth_only`; depth-aspect borrowed texture; the depth RTT barrier via
  the existing `depth_layout` field.
- `dx12_raster_context.cpp` — `R32_TYPELESS` + DSV heap + `R32_FLOAT` SRV for sampled depth transients;
  `record_depth_only`; the `DEPTH_WRITE → PIXEL_SHADER_RESOURCE` barrier.
- `tests/gpu-context-vulkan/test_vulkan_frame_graph.cpp`, `tests/gpu-context-dx12/test_dx12_frame_graph.cpp` —
  the `[ren3]` gate above.

## Risks

1. **The DX12 typeless dance** (above) — the only genuinely new mechanism; everything else mirrors REN-2.
2. **Layout choice**: `DEPTH_READ_ONLY_OPTIMAL` vs `SHADER_READ_ONLY_OPTIMAL` on Vulkan. Both are legal for
   sampling; mixing them across passes is not. Pick one, put it in `depth_layout`, assert it.
3. **Aliasing interaction**: a depth transient shares the slot pool with colour transients but has different
   memory type bits (`type_bits` already exists on the slot). Verify a depth transient does not get placed in
   a colour-only slot — the existing aliasing gates must stay green, which is why they are in the gate list.
4. **Do not regress `draw_storage_depth`** — the SceneRenderer's forward pass depends on it; the
   `sampled=false` case is in the gate for exactly this reason.
