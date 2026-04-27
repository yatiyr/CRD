# crd-platform

Platform abstraction over the OS: window, input, timing, filesystem, dynamic
libraries. Built on top of GLFW today; the public API is backend-agnostic so
we can swap GLFW for SDL3 or a custom Win32+Cocoa+Xlib stack later without
touching downstream engine code.

## Status

| Slice | Ships | Status |
| --- | --- | --- |
| v1a | `PlatformContext`, `Window` (PIMPL'd over GLFW), `Extent2D` | ✅ |
| v1b | `Timer`, `FrameClock` (chrono-only, GLFW-independent) | ✅ |
| v1c | `Input` (polling snapshot + opt-in event queue) | ✅ |
| v1d | filesystem, dynamic library, threading helpers | ✅ |

## Core decisions

- Backend is hidden. GLFW is a `PRIVATE` link dependency; no GLFW header
  appears in any public crd-platform header. `Window` is a concrete class
  with a PIMPL backend, not an `IWindow` interface.
- Vulkan-ready by default. `WindowDesc::client_api_none = true` maps to
  GLFW's `GLFW_NO_API`, so no OpenGL context is created. The Vulkan surface
  will be built later through `Window::native_handle()` plus a graphics-side
  helper.
- Backend errors flow through the engine logger. GLFW's error callback is
  bridged to `g_log_platform` (Error level by default), so platform-level
  failures show up next to every other engine log line.
- The `crd-platform` channel is owned inside `crd-platform`. Unlike the
  `g_log_containers` historical exception (which lives inside `crd-log` to
  break a cycle), this channel's `CRD_DEFINE_LOG_CHANNEL` is in
  `engine/platform/src/log_channel.cpp`. Dependency direction is one-way:
  `crd-platform → crd-log`, `crd-log` knows nothing about `crd-platform`.
- No layer stack, no event router, no `Event` base type at this layer.
  Those belong in a future `crd-app` module that lives above the engine
  modules; doing them now without their real consumers (ImGui debug,
  editor panels, game UI) would lock in the wrong shape.
- `Extent2D` is a small platform-local POD instead of `crd::math::Vec2<i32>`.
  Math's `MathScalar` concept is float/double only by design; pulling
  integer scalars in just to satisfy a window API would weaken the math
  contract. Keeping `Extent2D` here also drops `crd-platform → crd-math`
  from the dependency graph, which is a small win.

## What ships today (v1a)

- `PlatformContext`
  - RAII over `glfwInit` / `glfwTerminate`. Move-only, default-constructible
    to an invalid state.
  - `create()` factory installs the GLFW error → log bridge and validates
    init.
  - `poll_events()` is a thin wrapper that pumps the OS event queue.
- `Window`
  - PIMPL holding a `GLFWwindow*`. Construction via the static
    `Window::create(context, desc)` factory; failure returns an invalid
    `Window` and logs the underlying GLFW reason.
  - `should_close()` / `request_close()` / `framebuffer_size()` /
    `window_size()` / `set_title()` / `native_handle()`.
  - Move-only with a hand-written move-assign that destroys the existing
    GLFW handle before adopting the other side's impl, so swapping windows
    can't leak.
- `WindowDesc` defaults to a Vulkan-ready 1280x720 resizable visible
  window titled "Cerid".
- `Extent2D` — `{ i32 width; i32 height; }`, used for both window and
  framebuffer queries.
- `g_log_platform` channel and the per-translation-unit force-link anchor
  pattern reused from previous modules.
- `smoke_window` example under `runtime/examples/` opens a window, pumps
  events, and shuts down on user close.

## What ships today (v1b)

- `Timer` — monotonic stopwatch over `std::chrono::steady_clock`.
  Construct to start, `reset()` to re-anchor, `elapsed_seconds() /
  elapsed_milliseconds() / elapsed_nanoseconds()` to read.
- `FrameClock` — main-loop timing facade. `tick()` once per frame,
  read `delta_seconds()` / `total_seconds()` / `frame_count()`. The
  first tick seeds without a spike (delta is reported as 0 instead of
  "engine startup time").
- Both are intentionally GLFW-independent: timing should not require a
  windowing backend. Tests run without a Window and stay deterministic.
- `smoke_frame_clock` runtime example exercises a synthetic 5-frame
  loop without opening a window.

## What ships today (v1c — input)

- Backend-agnostic `Key` and `MouseButton` enums. Values are contiguous
  Cerid indices, NOT GLFW key codes — backend swaps don't reshuffle the
  enum. Translation tables are isolated in `window.cpp`.
- `InputState` — frame-coherent snapshot. `is_key_down`,
  `was_key_pressed`, `was_key_released` and the equivalent mouse
  accessors. Mouse position, accumulated mouse delta, scroll delta, and
  the latest `KeyMods` (shift/ctrl/alt/super).
- `Input` — owned by `Window`, mutated by GLFW callbacks via the
  user-pointer. Read through `window.input().state()`. Call
  `window.poll_input()` once per frame BEFORE `context.poll_events()`
  to clear the previous frame's edge state; the callbacks from the
  upcoming OS poll then populate the fresh frame.
- `InputEvent` — POD union (tagged `Type`). One event per hardware
  signal: KeyDown / KeyUp / KeyRepeat / MouseDown / MouseUp / MouseMove /
  Scroll / Resize. No `handled` flag, no propagation — that lives in
  the future `crd-app` module.
- Opt-in event queue. Default state is **off**. Call
  `window.input().enable_event_queue(64)` (capacity must be a power
  of two) to allocate, then drain with `try_pop_event()`. Without it,
  events are dropped after updating `InputState` — most game code
  doesn't need ordered events.
- First mouse position seeds without a spurious large delta. Repeat
  KeyDown without a prior KeyUp does NOT re-trigger
  `was_key_pressed` (transitions only, not "is held").
- Gamepad: deliberately not in this slice. Real gamepad support will
  come through SDL3 / XInput in a later phase; GLFW joystick API is too
  thin.

## What ships today (v1d — platform services)

- `fs::Path` — UTF-8, `String`-backed path wrapper with forward-slash
  canonical internal form. Backend-facing native conversion happens at
  the platform boundary only.
- Filesystem helpers on top of `<filesystem>`:
  - roots: `current_working_dir()`, `executable_dir()`,
    `user_config_dir(app_name)`
  - metadata: `exists`, `is_file`, `is_directory`, `file_size`,
    `last_modified_unix_seconds`
  - I/O: `read_file_text`, `read_file_binary`, `write_file_text`,
    `write_file_binary`
  - directory ops: `create_directories`, `remove_file`, `remove_all`,
    `list_directory`
- `DynamicLibrary` — move-only RAII wrapper over the host OS dynamic
  loader. `open(Path)`, `resolve()`, `resolve_as<Fn>()`, `is_valid()`.
  The public API stays opaque; no `HMODULE` / `dlopen` handle leaks into
  headers.
- `threading` helpers — deliberately tiny v1 baseline:
  `set_current_thread_name`, `current_thread_id`, `hardware_concurrency`,
  `logical_core_count`, `physical_core_count`, `set_thread_affinity`,
  `cpu_pause`.
- `smoke_filesystem` runtime example prints cwd / exe dir / config dir.

## How to use it (today)

```cpp
auto ctx = crd::platform::PlatformContext::create();
if (!ctx.is_valid()) { /* fatal */ }

crd::platform::WindowDesc desc;
desc.size  = {1280, 720};
desc.title = crd::containers::String("My Game");

auto window = crd::platform::Window::create(ctx, desc);
if (!window.is_valid()) { /* fatal */ }

while (!window.should_close())
{
    window.poll_input();
    ctx.poll_events();
    if (window.input().state().was_key_pressed(crd::platform::Key::Escape))
    {
        window.request_close();
    }
    // ... render here when crd-graphics lands ...
}
```

Vulkan integration (later) will look up the surface through GLFW directly
using `window.native_handle()` cast back to `GLFWwindow*` from the graphics
module's translation units; engine code outside graphics never needs to do
this.

## Long-term direction

- Timer / FrameClock built on `std::chrono::steady_clock`, deliberately
  decoupled from GLFW so they can be unit tested without a backend.
- Input: polling-first (frame-snapshot `InputState`) plus an opt-in
  `RingBuffer<InputEvent>` for code that needs ordered key presses /
  releases. No layer / propagation / consumption at this layer.
- Filesystem and dynamic-library helpers are now in place before Phase 2.
- Threading helpers stay intentionally small until `crd-jobs` lands in
  Phase 2.3; the API is there, but scheduler-grade policy waits for the
  real job-system consumer.
- Gamepad support stays out of Phase 1; GLFW's joystick API is too thin
  for real gamepad work and a proper implementation will lean on SDL3 or
  XInput in a later phase.
