# Session: `crd-renderer` v1g — `ForwardRenderPath`

**Date:** 2026-05-01
**Slice:** Phase 2.4g
**Status:** shipped

## What was built

First concrete `IRenderPath` implementation. `ForwardRenderPath` declares two frame graph passes
each frame into the `FrameGraph`, owns its render targets, and submits draw items via `PipelineResolver`.

### Pass layout

| Pass | Name | Writes | Draws |
|------|------|--------|-------|
| 1 | `depth-prepass` | depth (clear) | `draw_list.opaque` |
| 2 | `main-color` | color (clear) + depth (load) | `draw_list.opaque` + `draw_list.masked` |

The depth prepass uses `LoadOp::Clear` and stores; the color pass uses `LoadOp::Load` on depth so
early-Z from the prepass carries into the color stage without a barrier between them.

### Data layout

```
Set 0  — PerFrameUbo (288 bytes, CpuToGpu, ring per frame-in-flight)
           view, proj, view_proj, inv_view_proj (Mat4f × 4)
           camera_pos_ws (Vec4f), viewport_width, viewport_height, time_seconds, _pad (f32 × 4)
Push   — PerDrawPush (64 bytes, Vertex)
           model (Mat4f)
```

`PerFrameUbo` is mapped, filled, and unmapped every `build()`. The descriptor set for set 0 is
allocated from the ring allocator and updated with `update_buffer(0, ubo_buf, 0, sizeof(PerFrameUbo))`.

### Render targets

- `m_color_image` — `B8G8R8A8Unorm`, `ColorAttachment`, owned, recreated on `resize()`
- `m_depth_image` — `D32Sfloat`, `DepthStencilAttachment`, owned, recreated on `resize()`

Both are imported into the frame graph each frame with `ImageAccess::Undefined` (frame graph inserts
the transition to `ColorWrite` / `DepthWrite` before the first write).

## Supporting changes

### `inverse(Mat4f)` — `engine/math/include/crd/math/mat.hpp`

Added Laplace cofactor expansion for 4×4 inverse (GLM algorithm adapted for column-major layout).
Used in `build()` to fill `PerFrameUbo::inv_view_proj`. Implemented with scalar arithmetic only;
does not require element-wise Vec4 multiplication.

### `VulkanCommandBuffer::begin_rendering` — `engine/rhi-vulkan/src/vulkan_backend.cpp`

Hardened for multi-pass use:

- **Color attachment optional.** `info.color_attachment.image == nullptr` → depth-only pass.
  `colorAttachmentCount = 0` in `VkRenderingInfo`; no color attachment struct emitted.
- **No implicit layout transitions.** The previous implementation called
  `transition_to_color_attachment()` at `begin_rendering` and `transition_to_present()` at
  `end_rendering`. These are removed. Callers (frame graph, tests, smokes) are responsible for
  explicit transitions. The frame graph's barrier insertion pass covers this correctly.
- `m_render_target` member and the two transition helpers removed.

### `VulkanDevice::create_graphics_pipeline`

- Null fragment shader supported: `stage_count` reduced to 1 (vertex-only) when fragment is absent.
- `colorAttachmentCount` now derived from `has_color_output = (desc.color_format != Format::Undefined)`.

### Callers updated with explicit transitions

- `tests/rhi_vulkan/test_rhi_vulkan.cpp` — Vulkan triangle integration test
- `runtime/examples/smoke_imgui_overlay.cpp`
- `runtime/examples/smoke_rhi_vulkan_bootstrap.cpp`

All three now bracket their draw with:

```cpp
command_buffer->transition_image(img, ImageAccess::Undefined, ImageAccess::ColorWrite);
// ... begin_rendering ... draw ... end_rendering ...
command_buffer->transition_image(img, ImageAccess::ColorWrite, ImageAccess::Present);
```

### `FrameContext::frame_index` — `engine/renderer/include/crd/renderer/renderer.hpp`

Added `crd::u32 frame_index = 0;` so `ForwardRenderPath::build()` can index the UBO/set ring
via `slot = frame_index % frames_in_flight`.

### New files

- `engine/renderer/include/crd/renderer/per_frame_data.hpp` — `PerFrameUbo` + `PerDrawPush` + static_asserts
- `engine/renderer/include/crd/renderer/forward_render_path.hpp` — `ForwardRenderPath` class declaration
- `engine/renderer/src/forward_render_path.cpp` — `ForwardRenderPath` implementation

## Tests

5 new unit tests in `tests/renderer/test_renderer.cpp`:

- `ForwardRenderPath::create() returns non-null when fakes succeed`
- `ForwardRenderPath creates color and depth images on create`
- `ForwardRenderPath::build() registers two passes into the frame graph`
- `ForwardRenderPath::resize() does not recreate if extent unchanged`
- `ForwardRenderPath::output_image() returns color handle after build`

`FakeDevice::create_buffer` and `create_image` updated to return non-null fakes (were returning
nullptr) — required to unblock `ForwardRenderPath::create()` through to the render-target
allocation step.

## Deferred optimizations logged

Wrote to `docs/debt.md` (Renderer optimization backlog):

- **Transient image aliasing in the frame graph** — lifetime analysis pass prerequisite
- **HDR render target** — switch color to `R16G16B16A16Sfloat` + tone-map pass
- **Depth-only pipeline for the depth prepass** — vertex-only variant, no fragment shader
- **Async pipeline compilation** — job-thread compile + "pending" sentinel fallback
- **Bindless material system** — global descriptor heap, per-draw material index in push constants
- **GPU-driven rendering** — compute cull + `VkDrawIndirectCommand`, requires Phase 3 scene buffer

## Quality pass

Three-flavour green (win-debug 253/253, win-release 252/252, win-asan 253/253).

Smokes validated:
- `smoke_rhi_vulkan_bootstrap.exe` — 120 frames, zero Vulkan validation errors
- `smoke_renderer.exe` — frame graph build + execute with 3 barrier transitions correct
