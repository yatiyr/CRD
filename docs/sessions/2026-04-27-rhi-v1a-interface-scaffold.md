# Session — 2026-04-27 — crd-rhi v1a (interface scaffold)

## Goal

Start the graphics track by creating `crd-rhi` as a real module. The first
slice must stay narrow and backend-agnostic: low-level GPU concepts only, no
Vulkan leaks, no materials, no scene, no renderer policy.

## What we built / changed

- **`engine/rhi/`** new module with its own CMake target.
- **`types.hpp`**
  - backend-neutral enums and descriptors for the first-triangle path
  - `BackendApi`, `Format`, `PresentMode`, `MemoryUsage`, `BufferUsage`,
    `ImageUsage`, `ShaderStage`, `PrimitiveTopology`, `LoadOp`, `StoreOp`
  - `AdapterInfo`, `DeviceDesc`, `SwapchainDesc`, `BufferDesc`, `ImageDesc`,
    `ShaderModuleDesc`, `GraphicsPipelineDesc`, `RenderingInfo`
- **Abstract interfaces**
  - `Instance`
  - `Device`
  - `Queue`
  - `Swapchain`
  - `Buffer`
  - `Image`
  - `CommandBuffer`
  - `ShaderModule`
  - `Pipeline`
- **`rhi.hpp`** umbrella header.
- **`runtime/examples/smoke_rhi_api.cpp`**
  - local fake device path that exercises the intended API shape without a
    real backend: create shaders, buffer, pipeline, command buffer, begin /
    bind / draw / submit.
- **`tests/rhi/test_rhi.cpp`**
  - fake-backend tests for:
    - flag composition
    - adapter enumeration + device creation
    - first-triangle resource flow through the interface set
- **Root build wiring**
  - added `engine/rhi` to root CMake
  - added `tests/rhi`
  - added `smoke_rhi_api`
- **Docs**
  - `docs/systems/rhi.md`
  - roadmap / context updates

## Plain-English explanation

Cerid now has a real RHI module, but not a real GPU backend yet. This is the
contract layer: the set of interfaces that say "a backend must be able to do
these things" and that higher-level rendering code will later talk to.

The important part is what is *not* here. There is no scene, no lighting, no
material system, no renderer graph, no Vulkan type in public headers. The
module only describes the low-level path needed to eventually draw the first
triangle.

That means the next session can focus purely on `crd-rhi-vulkan` bootstrap
without still arguing about whether the public API should exist at all.

## Decisions made

- **Minimal first.** Only low-level GPU concepts that the earliest vertical
  slice needs are in the interface.
- **Backend-neutral headers.** `crd-rhi` ships no Vulkan surface types,
  no `Vk*`, no backend-specific enums.
- **Fake backend testing is enough for v1a.** This slice proves interface
  ergonomics and ownership shape, not real graphics output.
- **The first-triangle path is already encoded** in the API: create device,
  swapchain, buffer, shader modules, pipeline, command buffer, then
  begin/bind/draw/submit.

## Files touched

- `CMakeLists.txt` — added `engine/rhi`
- `engine/rhi/CMakeLists.txt` — new
- `engine/rhi/include/crd/rhi/types.hpp` — new
- `engine/rhi/include/crd/rhi/buffer.hpp` — new
- `engine/rhi/include/crd/rhi/image.hpp` — new
- `engine/rhi/include/crd/rhi/shader_module.hpp` — new
- `engine/rhi/include/crd/rhi/pipeline.hpp` — new
- `engine/rhi/include/crd/rhi/command_buffer.hpp` — new
- `engine/rhi/include/crd/rhi/swapchain.hpp` — new
- `engine/rhi/include/crd/rhi/queue.hpp` — new
- `engine/rhi/include/crd/rhi/device.hpp` — new
- `engine/rhi/include/crd/rhi/instance.hpp` — new
- `engine/rhi/include/crd/rhi/rhi.hpp` — new
- `engine/rhi/src/rhi.cpp` — new
- `tests/CMakeLists.txt` — added `tests/rhi`
- `tests/rhi/CMakeLists.txt` — new
- `tests/rhi/test_rhi.cpp` — new
- `runtime/CMakeLists.txt` — added `smoke_rhi_api`
- `runtime/examples/smoke_rhi_api.cpp` — new
- `docs/systems/rhi.md` — new
- `docs/ROADMAP.md` — status / decision log / where-left-off
- `CONTEXT.md` — module status + dependency notes + doc link

## Tests / verification

- `win-debug`: 200/200
- `win-release`: 199/199 (Debug-only stats test correctly skipped)
- `win-asan`: 200/200, no leaks, no UAF, no OOB

## Next session starts with

`crd-rhi-vulkan` bootstrap: instance, physical device selection, logical
device, surface creation from `Window::native_handle()`, and swapchain setup.
