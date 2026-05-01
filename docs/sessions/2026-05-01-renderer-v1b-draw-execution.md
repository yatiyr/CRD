# Session — 2026-05-01 — `crd-renderer` v1b real draw execution

## Goal

Move `crd-renderer` from frame-plan preparation into real renderer-side
execution: one rendering pass, command-buffer recording, pipeline resolution,
and draw-call submission while keeping native pipeline ownership outside the
renderer.

## What we built / changed

- **Renderer execution surface**
  - `PipelineResolver`
  - `Renderer::execute_frame()`
- **Behavior**
  - renderer begins a command buffer
  - opens one rendering pass
  - resolves a backend-owned pipeline from the shader handoff per draw item
  - binds vertex buffer and issues draw calls
  - closes rendering and command buffer
- **Tests**
  - new execution test validates one-pass command recording behavior
- **Smoke**
  - `smoke_renderer` now proves both:
    - frame-plan creation
    - frame execution through fake backend-owned pipeline/command objects

## Plain-English explanation

This slice gives the renderer an actual execution path without crossing the
ownership boundary into backend-native pipeline management. The renderer now
does more than prepare draw items: it can orchestrate one pass and emit draw
commands against a resolver-provided pipeline.

That is the correct narrow next step before material binding and richer
resource/state ownership land.

## Decisions made

- Renderer still does not own native pipeline objects.
- Execution is pass-oriented but deliberately not yet a full render graph.
- Shader handoff remains the source of truth for what the pipeline resolver
  must interpret.

## Files touched

- `engine/renderer/include/crd/renderer/renderer.hpp` — execution surface
- `engine/renderer/src/renderer.cpp` — execute_frame implementation
- `tests/renderer/test_renderer.cpp` — execution test
- `runtime/examples/smoke_renderer.cpp` — execution smoke
- `docs/systems/renderer.md` — v1b shipped state
- `docs/phases/phase-2-graphics.md` — v1b marked shipped
- `docs/ROADMAP.md` — renderer active / material growth next
- `context.md` — current focus moved to v1c

## Tests / verification

- `win-debug`: 228/228
- `win-release`: 227/227
- `win-asan`: 228/228
- `smoke_renderer` reports both build-frame and execute-frame success cleanly

## Next session starts with

`crd-renderer` v1c — material binding growth.
