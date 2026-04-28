# Session — 2026-04-28 — ImGui debug overlay

## Goal

Ship the first debug overlay over the now-working triangle path, using ImGui's
docking branch as a debug-only backend adapter and `crd-config` as its first
real consumer.

## What we built / changed

- **`engine/imgui/`** new module (`crd-imgui`)
- Dear ImGui dependency added via CPM (docking branch)
- ImGui backend wiring:
  - `imgui_impl_glfw`
  - `imgui_impl_vulkan`
- `ImGuiLayer`
  - derives from `crd::app::Layer`
  - owns ImGui context lifetime
  - new `on_frame_begin()` hook in `Layer`/`Application` lets ImGui start the
    frame before propagated event dispatch, so capture semantics are meaningful
  - uses `crd-config` for settings
  - records draw data into the real Vulkan command buffer
- `runtime/configs/imgui_layer.toml`
- `smoke_imgui_overlay`
  - real triangle + ImGui overlay in one bounded runtime example
- `tests/imgui/test_settings.cpp`
  - config-driven settings parsing
- backend-specific native Vulkan helper surface (`vulkan_native.hpp`) now used
  by the adapter module rather than polluting `crd-rhi`

## Plain-English explanation

Cerid now has a real developer cockpit on top of the graphics stack. The RHI
and Vulkan backend no longer exist only as invisible infrastructure; they can
be observed and interacted with through a debug overlay while the engine is
running.

This is not Cerid's long-term UI system. It is a deliberate debug adapter over
the working app/platform/RHI path. That distinction matters: the immediate goal
is faster iteration and better diagnostics, not locking in an immediate-mode UI
as the future of the engine.

## Decisions made

- **ImGui stays debug-only** (already pinned by ADR-0023).
- **Docking enabled by default.**
- **Multi-viewport disabled by default** and only available as a config opt-in
  because the Vulkan/platform path is rougher there without enough payoff for
  the current debug role.
- `crd-app` remains graphics-agnostic; `crd-imgui` is the bridge module.

## Files touched

- `CMakeLists.txt` — added ImGui CPM package + `engine/imgui`
- `engine/app/include/crd/app/layer.hpp` — added `on_frame_begin()`
- `engine/app/src/application.cpp` — calls `on_frame_begin()` before event dispatch
- `engine/imgui/CMakeLists.txt` — new
- `engine/imgui/include/crd/imgui/imgui.hpp` — new
- `engine/imgui/include/crd/imgui/imgui_layer.hpp` — new
- `engine/imgui/include/crd/imgui/log_channel.hpp` — new
- `engine/imgui/include/crd/imgui/settings.hpp` — new
- `engine/imgui/src/imgui_layer.cpp` — new
- `engine/imgui/src/log_channel.cpp` — new
- `engine/imgui/src/settings.cpp` — new
- `engine/rhi-vulkan/include/crd/rhi/vulkan_native.hpp` — backend-native helper surface
- `engine/rhi-vulkan/src/vulkan_backend.cpp` — exported native helper functions
- `tests/CMakeLists.txt` — added `tests/imgui`
- `tests/imgui/CMakeLists.txt` — new
- `tests/imgui/test_settings.cpp` — new
- `runtime/CMakeLists.txt` — added `smoke_imgui_overlay`
- `runtime/examples/smoke_imgui_overlay.cpp` — new
- `runtime/configs/imgui_layer.toml` — new
- `docs/systems/imgui.md` — new
- `docs/decisions/0024-imgui-single-viewport-default.md` — new
- `docs/decisions/README.md` — new ADR indexed
- `docs/phases/phase-2-graphics.md` — 2.1 marked shipped
- `docs/ROADMAP.md` — hub status updated
- `context.md` — current focus / last shipped / test counts updated

## Tests / verification

- `win-debug`: 210/210
- `win-release`: 209/209 (Debug-only stats test correctly skipped)
- `win-asan`: 210/210, no leaks, no UAF, no OOB
- `smoke_imgui_overlay` runs a bounded real triangle + ImGui overlay loop

## Next session starts with

GPU memory + streaming (2.2). The backend is now visible and debuggable; the
next risk to reduce is allocator policy before renderer growth widens.
