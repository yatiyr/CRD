# Session — 2026-04-28 — crd-rhi-vulkan bootstrap

## Goal

Create the first real GPU backend module for Cerid. The slice is deliberately
bootstrap-only: Vulkan instance, adapter enumeration, logical device,
surface creation, and swapchain bootstrap. No command recording, no pipeline
creation, no first triangle yet.

## What we built / changed

- **`engine/rhi-vulkan/`** new module with its own CMake target.
- **Vulkan discovery in CMake**
  - `find_package(Vulkan REQUIRED)`
  - Windows fallback scans `C:/VulkanSDK/*` and seeds `VULKAN_SDK` /
    `CMAKE_PREFIX_PATH` if needed so builds work even before a shell restart
- **Public backend entrypoint**
  - `include/crd/rhi/vulkan_backend.hpp`
  - `create_vulkan_instance(InstanceDesc)` returning `std::unique_ptr<rhi::Instance>`
- **Backend implementation** (`src/vulkan_backend.cpp`)
  - Vulkan instance creation
  - optional validation-layer enablement when available
  - debug-utils messenger callback for warnings/errors
  - physical-device enumeration mapped into `rhi::AdapterInfo`
  - logical-device creation with graphics queue selection
  - surface creation through `glfwCreateWindowSurface` from
    `Window::native_handle()`
  - swapchain creation and wrapping of swapchain images into backend-neutral
    `Image` objects
- **Backend log channel**
  - `g_log_rhi_vulkan`
- **Tests** (`tests/rhi_vulkan/test_rhi_vulkan.cpp`)
  - Vulkan instance enumerates at least one adapter
  - Vulkan device bootstrap creates a swapchain for an invisible window
- **Smoke** (`runtime/examples/smoke_rhi_vulkan_bootstrap.cpp`)
  - creates platform context + real window
  - creates Vulkan instance
  - enumerates adapters
  - creates device + swapchain
  - reports bootstrap success in logs
- **Tooling**
  - cross-platform Vulkan helper script was upgraded earlier in the session
    so missing SDK/shader tooling can be detected and installed cleanly

## Plain-English explanation

Cerid can now talk to a real GPU backend. Not enough to render yet, but enough
to stand up the Vulkan side of the engine and prove that our `crd-rhi`
interface can be implemented for an actual API.

This is the critical bridge from architecture to reality. Before this session,
RHI was a contract plus fake backend tests. After this session, a real Vulkan
instance can be created, a real adapter selected, a real logical device
constructed, and a real swapchain attached to a real window.

Just as important is what we did **not** do: we did not rush command buffers,
sync, pipelines, shaders, or materials into the bootstrap. That keeps the
backend clean and lets each later slice land on a stable base.

## Decisions made

- **Backend remains hidden behind `crd-rhi`.** Public backend header returns
  `rhi::Instance`; no `Vk*` in public engine-facing headers.
- **GLFW surface creation is used directly in the backend.** `Window` already
  exposes a native handle escape hatch; this is the cleanest bootstrap path.
- **Runtime checks matter in release.** Bootstrap paths now log and return
  `nullptr`/empty results on failure rather than depending purely on asserts.
- **Surface ownership lives with the device in this slice.** One surface,
  created lazily on first swapchain creation, is enough for the current
  bootstrap path.
- **Unimplemented deeper GPU paths stay explicit.** `create_buffer`,
  `create_shader_module`, `create_graphics_pipeline`, and
  `create_command_buffer` intentionally log "not implemented in bootstrap
  slice" instead of pretending to be ready.

## Files touched

- `CMakeLists.txt` — added `engine/rhi-vulkan`
- `engine/rhi/include/crd/rhi/types.hpp` — added `InstanceDesc`, refined `DeviceDesc`
- `engine/rhi-vulkan/CMakeLists.txt` — new
- `engine/rhi-vulkan/include/crd/rhi/vulkan_backend.hpp` — new
- `engine/rhi-vulkan/src/log_channel.hpp` — new
- `engine/rhi-vulkan/src/log_channel.cpp` — new
- `engine/rhi-vulkan/src/vulkan_backend.cpp` — new
- `tests/CMakeLists.txt` — added `tests/rhi_vulkan`
- `tests/rhi_vulkan/CMakeLists.txt` — new
- `tests/rhi_vulkan/test_rhi_vulkan.cpp` — new
- `runtime/CMakeLists.txt` — added `smoke_rhi_vulkan_bootstrap`
- `runtime/examples/smoke_rhi_vulkan_bootstrap.cpp` — new
- `docs/systems/rhi.md` — v1b status + usage/docs update
- `docs/ROADMAP.md` — status, decision log, re-entry notes
- `CONTEXT.md` — dependency note updated

## Tests / verification

- `win-debug`: 202/202
- `win-release`: 201/201 (Debug-only stats test correctly skipped)
- `win-asan`: 202/202, no leaks, no UAF, no OOB
- `smoke_rhi_vulkan_bootstrap` creates a real window, enumerates adapters,
  and creates a swapchain successfully

## Next session starts with

Command buffers + frame synchronization. That slice should turn the current
bootstrap device/swapchain path into a real per-frame execution path, still
before pipelines and the first triangle.
