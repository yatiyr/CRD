# crd-imgui

Debug-only immediate-mode overlay layer built on Dear ImGui's docking branch.
This module is a backend adapter over `crd-app`, `crd-config`, and
`crd-rhi-vulkan`. It exists to improve engine iteration and diagnostics; it is
not Cerid's long-term UI system.

## Status

| Slice | Ships | Status |
| --- | --- | --- |
| 2.1 | ImGui debug overlay | ✅ |

## Core decisions

- ImGui is debug-only forever. It does not grow into editor or gameplay UI.
- Docking is enabled by default.
- Multi-viewport is **off by default** and only configurable as an opt-in.
- Theme/style comes from `crd-config` TOML, not hard-coded engine constants.
- `crd-app` stays graphics-agnostic; `crd-imgui` is the bridge module that
  knows both app-level layer semantics and Vulkan backend details.

## What ships today

- `ImGuiLayer`
  - derives from `crd::app::Layer`
  - owns ImGui context lifetime
  - `on_frame_begin()` performs ImGui new-frame setup before propagated event
    dispatch, so `WantCaptureMouse` / `WantCaptureKeyboard` can participate in
    app-layer event handling
  - `on_render()` builds default debug panels
  - `render(CommandBuffer&)` records ImGui draw data into the current Vulkan
    command buffer
- Config-driven settings (`load_settings`)
  - docking
  - multi-viewport
  - show demo window
  - show metrics window
  - show stats panel
  - theme preset
- `runtime/configs/imgui_layer.toml`
- `smoke_imgui_overlay`
  - real triangle + ImGui overlay together

## What it does not do

- no replacement for `crd-ui`
- no editor framework
- no permanent user-facing widget system
- no deep backend abstraction beyond the one Vulkan path Cerid ships today

## Long-term direction

- stay debug-only
- grow only as needed for diagnostics, profiling, live tuning, and developer
  tooling
- once `crd-ui` matures, ImGui remains behind it as an internal debug layer
