# Session — 2026-04-28 — crd-rhi-vulkan frame execution slice

## Goal

Turn the Vulkan bootstrap into a real per-frame execution path before first
triangle work begins. That means command-pool creation, command-buffer
allocation/reset, swapchain-owned frame sync objects, acquire/submit/present,
and a clear-only frame path.

## What we built / changed

- **RHI surface refinement**
  - `Queue::submit` now takes both `CommandBuffer&` and `Swapchain&`
  - `Swapchain::acquire_next_image()` now returns success/failure
  - fake RHI tests/smokes were updated to match this more realistic low-level
    frame boundary
- **`engine/rhi-vulkan/src/vulkan_backend.cpp`**
  - command-pool creation on the device
  - real command-buffer allocation/free
  - command-buffer begin/end/reset path
  - clear-only dynamic-rendering path using the swapchain image as a color
    attachment
  - swapchain image views
  - per-frame synchronization objects inside the swapchain:
    - image-available semaphore
    - render-finished semaphore
    - in-flight fence
  - acquire / submit / present loop
  - device wait-idle path kept intact
- **Tests**
  - existing bootstrap tests kept
  - added Vulkan command-buffer + frame-loop test for a clear-only frame
  - the heaviest integration part stays debug/ASan-focused in automation;
    present-loop behaviour still runs in the smoke app
- **Smoke**
  - `smoke_rhi_vulkan_bootstrap` now runs a real per-frame loop for a short
    bounded number of frames and then closes itself cleanly

## Plain-English explanation

Before this session, Cerid could open Vulkan, create a device, and create a
swapchain, but it still did not have a real frame loop on the GPU side. Now it
does. The engine can acquire a swapchain image, record a command buffer, submit
it, and present.

It is still not rendering geometry yet, but the most important execution
backbone is in place. The next session can focus on pipeline/shader/vertex
data work instead of still fighting the frame lifecycle itself.

## Decisions made

- **Dynamic rendering is already the minimal rendering path.** We do not build
  an early heavyweight render-pass architecture just to clear an image.
- **Classic submit/barrier flow ships in this slice.** sync2 support is still
  detected and remains a roadmap direction, but the actual shipped execution
  path uses the classic Vulkan submission/barrier model because it proved more
  stable across the current release-path testing.
- **Per-frame sync belongs to the swapchain in this slice.** It is the most
  natural owner for image acquisition and presentation lifetime.
- **The automated test matrix stays conservative.** Real submit+wait-idle is
  covered in tests; continuous present-loop coverage stays in the bounded smoke
  app.

## Files touched

- `engine/rhi/include/crd/rhi/queue.hpp` — submit signature refined
- `engine/rhi/include/crd/rhi/swapchain.hpp` — acquire returns success/failure
- `engine/rhi-vulkan/src/vulkan_backend.cpp` — frame execution path
- `tests/rhi/test_rhi.cpp` — fake backend updated for new queue/swapchain API
- `tests/rhi_vulkan/test_rhi_vulkan.cpp` — added clear-only frame-path test
- `runtime/examples/smoke_rhi_api.cpp` — fake smoke updated for new API
- `runtime/examples/smoke_rhi_vulkan_bootstrap.cpp` — bounded real frame loop
- `docs/systems/rhi.md` — v1c shipped state
- `docs/ROADMAP.md` — status, decision log, re-entry notes
- `CONTEXT.md` — backend responsibility note updated

## Tests / verification

- `win-debug`: 203/203
- `win-release`: 202/202 (Debug-only stats test correctly skipped)
- `win-asan`: 203/203, no leaks, no UAF, no OOB

## Next session starts with

Pipelines + shader modules + first triangle. The GPU can now execute frames;
the next slice finally gives it real geometry and a real pipeline to draw.
