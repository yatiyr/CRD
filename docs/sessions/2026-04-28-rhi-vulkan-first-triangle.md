# Session — 2026-04-28 — first triangle milestone

## Goal

Finish the first real graphics milestone: draw a triangle through the full
Cerid RHI/Vulkan path without polluting the backend with higher-level renderer
policy.

## What we built / changed

- **RHI type refinement**
  - `Format` gained the float vertex formats needed for real geometry
  - `GraphicsPipelineDesc` now carries `viewport_extent`
- **Backend resource implementation**
  - Vulkan buffer creation + memory allocation for the early host-visible path
  - Vulkan shader-module creation from SPIR-V
  - Vulkan graphics-pipeline creation for the minimal dynamic-rendering path
  - command-buffer `bind_pipeline`, `bind_vertex_buffer`, `draw`
  - swapchain image layout tracking between color-attachment and present
- **Shader assets**
  - `runtime/examples/shaders/triangle.vert`
  - `runtime/examples/shaders/triangle.frag`
  - build-time SPIR-V compilation through `glslangValidator`
  - runtime/tests load compiled `.spv` from the target-local `shaders/` dir
- **Tests**
  - Vulkan triangle integration test now records and submits a real draw path
  - fake RHI tests were updated to the refined pipeline descriptor shape
- **Smoke**
  - `smoke_rhi_vulkan_bootstrap` now draws a real triangle for a bounded
    number of frames and exits cleanly

## Plain-English explanation

Cerid now does the thing graphics engines are supposed to do: it draws the
first triangle on a real GPU through its own RHI and Vulkan backend.

This is more than a visual milestone. It proves that the abstraction layers,
window integration, frame loop, shader ingestion, pipeline creation, and early
buffer path all fit together coherently.

Just as importantly, the triangle path stayed narrow. We did not let
descriptor systems, materials, scene logic, or camera systems creep into the
backend just to hit the milestone. That discipline keeps the foundation clean
for the renderer work that comes next.

## Decisions made

- **Dynamic rendering is the triangle path.** No early heavyweight render-pass
  architecture was introduced.
- **Host-visible vertex-buffer allocation is good enough for this milestone.**
  The proper GPU allocation strategy is still a follow-on slice.
- **Shader compilation is part of the build graph now.** The milestone does
  not depend on hand-managed SPIR-V blobs.
- **Release automation stays conservative.** The heaviest Vulkan integration
  test remains debug/ASan-focused; the runtime smoke still exercises the real
  bounded present loop.

## Files touched

- `CMakeLists.txt` — GLSL compile helper
- `engine/rhi/include/crd/rhi/types.hpp` — float vertex formats + viewport extent
- `engine/rhi-vulkan/src/vulkan_backend.cpp` — buffers, shader modules,
  pipelines, bind/draw path, layout tracking
- `runtime/examples/shaders/triangle.vert` — new
- `runtime/examples/shaders/triangle.frag` — new
- `runtime/CMakeLists.txt` — shader compilation wiring for Vulkan smoke
- `tests/rhi_vulkan/CMakeLists.txt` — shader compilation wiring for Vulkan tests
- `tests/rhi/test_rhi.cpp` — pipeline descriptor shape update
- `tests/rhi_vulkan/test_rhi_vulkan.cpp` — real triangle integration test
- `runtime/examples/smoke_rhi_api.cpp` — pipeline descriptor shape update
- `runtime/examples/smoke_rhi_vulkan_bootstrap.cpp` — real triangle smoke
- `docs/systems/rhi.md` — v1d shipped state
- `docs/ROADMAP.md` — status, decision log, re-entry notes
- `CONTEXT.md` — backend responsibility note updated

## Tests / verification

- `win-debug`: 203/203
- `win-release`: 202/202 (Debug-only stats test correctly skipped)
- `win-asan`: 203/203, no leaks, no UAF, no OOB
- `smoke_rhi_vulkan_bootstrap` draws a bounded real triangle loop and exits
  cleanly

## Next session starts with

Either ImGui debug overlay integration or GPU allocation strategy. The engine
has crossed the "can it render at all?" threshold; the next step is making the
graphics stack usable and extensible rather than merely alive.
