# Session — 2026-05-01 — `crd-renderer` v1a explicit renderables

## Goal

Start `crd-renderer` as the first real consumer of the completed shader packet.
Keep it deliberately small: camera + explicit renderable list + draw-item
preparation, without scene graph or ECS commitments.

## What we built / changed

- **`engine/renderer/`** new module
- **Public surface**
  - `Camera`
  - `Renderable`
  - `DrawItem`
  - `FramePlan`
  - `Renderer`
- **Behavior**
  - renderer collects explicit renderables
  - `build_frame()` consumes `crd-shader` handoff and produces renderer-facing
    draw items
  - camera contributes view/projection matrices
- **Tests**
  - explicit renderable list turns into one valid draw item with shader handoff
- **Smoke**
  - `smoke_renderer` proves the renderer consumes shader handoff and produces
    a coherent frame plan

## Plain-English explanation

This slice is intentionally not a full renderer. It is the first point where
the engine has a real consumer above RHI and shader infrastructure. The goal is
to prove the integration direction, not to jump into scene systems too early.

That means the renderer today is basically a preparation layer: given a camera,
an explicit list of renderables, and a compiled shader variant, produce the
draw items a future pass executor will need.

## Decisions made

- Explicit renderable list first, no ECS/scene graph now.
- Renderer consumes `crd-shader` handoff, not backend-native pipeline details.
- Renderer remains a high-level layer; actual native pipeline object ownership
  still belongs to `crd-rhi` / backend.

## Files touched

- `CMakeLists.txt` — added `engine/renderer`
- `engine/renderer/CMakeLists.txt` — new
- `engine/renderer/include/crd/renderer/renderer.hpp` — new
- `engine/renderer/src/renderer.cpp` — new
- `tests/CMakeLists.txt` — added `tests/renderer`
- `tests/renderer/CMakeLists.txt` — new
- `tests/renderer/test_renderer.cpp` — new
- `runtime/CMakeLists.txt` — added `smoke_renderer`
- `runtime/examples/smoke_renderer.cpp` — new
- `docs/systems/renderer.md` — new
- `docs/ROADMAP.md` — current active graphics slice moved to renderer
- `context.md` — current focus / last shipped updated

## Tests / verification

- `win-debug`: 227/227
- `win-release`: 226/226
- `win-asan`: 227/227
- `smoke_renderer` builds a coherent frame plan from one renderable cleanly

## Next session starts with

Renderer execution growth or material binding growth, depending on whether you
want the next slice to deepen draw execution or shader/material integration.
