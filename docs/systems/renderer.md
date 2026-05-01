# crd-renderer

High-level rendering layer above `crd-rhi` / `crd-rhi-vulkan` and
`crd-shader`. `crd-renderer` is where renderables, camera data, and draw-item
preparation start becoming engine-facing concepts rather than raw backend
plumbing.

## Status

| Slice | Ships | Status |
| --- | --- | --- |
| v1a | explicit renderable list + camera + draw-item preparation | ✅ |
| v1b | real draw execution / pass orchestration | ✅ |
| v1c | frame graph v1 — typed handles, pass DAG, automatic barriers | ⏳ |
| v1d | `IRenderPath` interface on top of frame graph | ⏳ |
| v1e | push constants + descriptor set RHI surface | ⏳ |
| v1f | material system v1 (`MaterialLayout`, `Material`, `MaterialInstance`) | ⏳ |
| v1g | `ForwardRenderPath` v1 — depth prepass + main color as graph passes | ⏳ |
| v1h | index buffer + real mesh support (`draw_indexed`) | ⏳ |

## Core decisions

- Starts from an **explicit renderable list**, not ECS and not a scene graph.
- Consumes `crd-shader`'s backend-neutral handoff surface instead of talking to
  backend shader details directly.
- Owns no native pipeline objects; that remains in `crd-rhi`/backend.

## What ships today

- `Camera`
  - view matrix
  - projection matrix
  - derived `view_projection()`
- `Renderable`
  - transform
  - vertex buffer pointer
  - vertex count
  - material instance id
  - shader variant handle
- `DrawItem`
  - model matrix
  - view-projection matrix
  - vertex buffer pointer
  - vertex count
  - material instance id
  - `VariantPipelineDesc` handoff
- `FramePlan`
  - array of prepared draw items
- `Renderer`
  - `submit()`
  - `clear()`
  - `build_frame()`

## What it does not do yet

- no full pass graph
- no scene graph
- no ECS
- no material system ownership

## What ships today (v1b)

- `PipelineResolver`
  - renderer-side interface that resolves a `VariantPipelineDesc` into a
    backend-owned `rhi::Pipeline`
- `Renderer::execute_frame()`
  - begins command buffer
  - opens one rendering pass
  - resolves pipeline per draw item
  - binds vertex buffer and issues draw calls
  - closes rendering + command buffer

This is intentionally still narrow:

- renderer does not own native pipeline objects
- renderer does not create shader modules
- renderer does not own material binding state yet
- pass orchestration is one minimal pass, not a graph system

## Architecture layers

```
Layer 0  RHI          API-agnostic GPU surface — backend swap point
Layer 1  Frame graph  typed resource handles, pass DAG, automatic Vulkan barriers, transient aliasing
Layer 2  IRenderPath  a set of frame graph passes (Forward, Deferred, Forward+, ...)
Layer 3  Material     render-path-agnostic parameter binding
Layer 4  crd-ui       rendered as a frame graph UI canvas pass (Phase 5)
```

## Long-term direction

- Frame graph (v1c) is the foundation — all render paths and techniques build on it
- `IRenderPath` (v1d) makes Forward+, Deferred, Visibility Buffer all pluggable
- Material system (v1f) is render-path-agnostic; paths adapt at draw time
- Scene/world systems layer on top in Phase 3
- Renderer stays the primary consumer of the full shader packet
