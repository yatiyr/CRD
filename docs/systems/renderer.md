# crd-renderer

High-level rendering layer above `crd-rhi` / `crd-rhi-vulkan` and
`crd-shader`. `crd-renderer` is where renderables, camera data, and draw-item
preparation start becoming engine-facing concepts rather than raw backend
plumbing.

## Status

| Slice | Ships | Status |
| --- | --- | --- |
| v1a | explicit renderable list + camera + draw-item preparation | ✅ |
| v1b | real draw execution / pass orchestration | ⏳ |
| v1c | material binding growth | ⏳ |

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

- no real draw execution loop
- no pass graph
- no scene graph
- no ECS
- no material system ownership

## Long-term direction

- next slices add actual pass execution and richer resource/material binding
- scene/world systems can layer on top later
- renderer stays the first concrete consumer of the full shader packet
