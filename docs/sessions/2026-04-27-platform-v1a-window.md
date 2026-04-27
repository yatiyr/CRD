# Session — 2026-04-27 — crd-platform v1a (window + context)

## Goal

Open Phase 1 step 8a. Stand up `crd-platform` with a backend-agnostic window
abstraction over GLFW, plus a `PlatformContext` that owns GLFW's lifecycle
and bridges its errors into the engine logger. Decide where `Event` and
layer-stack concepts belong and codify that decision.

## What we built / changed

- **GLFW dependency**: pulled in via CPM (`gh:glfw/glfw#3.4`) with
  examples / tests / docs / install all turned off. GLFW's source tree is
  C and triggers warnings under `/W4 /WX`, so we strip warnings from the
  `glfw` target only.
- **`engine/platform/`** scaffolded as a static library:
  - `crd-platform` links `PUBLIC` to `crd-core`, `crd-log`, `crd-containers`
    and `PRIVATE` to `glfw`. Public headers do NOT include any GLFW header.
- **`PlatformContext`** (`include/crd/platform/context.hpp`,
  `src/context.cpp`):
  - default ctor produces an invalid context (so callers can hold a
    member that initialises later)
  - `create()` factory installs a GLFW error → `CRD_LOG_ERROR` bridge,
    calls `glfwInit`, logs the GLFW version on success, returns invalid
    context on failure with a `CRD_LOG_CRITICAL`
  - move-only with hand-written move-assign that calls `glfwTerminate`
    on the existing context before adopting the moved-from one
  - `poll_events()` is a thin wrapper over `glfwPollEvents`
- **`Window`** (`include/crd/platform/window.hpp`, `src/window.cpp`):
  - PIMPL'd over `GLFWwindow*`; the impl struct lives in `.cpp` only
  - `Window::create(context, desc)` is the factory; failure returns an
    invalid Window and logs the underlying reason
  - `WindowDesc` defaults to a 1280x720 visible resizable Vulkan-ready
    window titled "Cerid"
  - `client_api_none = true` maps to `GLFW_NO_API` (no OpenGL context)
  - hand-written move-assign destroys the held GLFW handle before
    adopting the other side's impl, so swapping windows can't leak the
    OS handle
  - exposed surface: `should_close` / `request_close` / `framebuffer_size` /
    `window_size` / `set_title` / `native_handle`
- **`Extent2D`** small POD (`{ i32 width, height; }`) used for both window
  and framebuffer queries. Lives inside `crd-platform`, not `crd-math`.
- **`g_log_platform`** declared in `<crd/platform/log_channel.hpp>`,
  defined in `engine/platform/src/log_channel.cpp`. No cycle, so no
  first-party-channel detour through `crd-log` is needed (unlike
  `g_log_containers`).
- **`include/crd/platform/platform.hpp`** umbrella header.
- **`tests/platform/test_platform.cpp`** — 7 test cases covering:
  - context create/destroy
  - context move semantics
  - re-creation after destruction (init/terminate cycle)
  - `Extent2D` zero-init
  - invisible window create/close/handle/size lifecycle
  - invalid-context Window::create returns invalid Window
  - Window move keeps the GLFW handle alive
  - all window-creating cases short-circuit if `CRD_PLATFORM_HEADLESS=1`
- **`runtime/examples/smoke_window.cpp`** — opens a real Vulkan-ready
  window and pumps events until the user closes it.

## Plain-English explanation

Cerid now has a "platform" module — the layer that talks to the OS. Today
it does two things: it starts and stops GLFW (the windowing backend), and
it can open a window. Both are deliberately wrapped so the rest of the
engine never sees GLFW's types directly. If we ever swap GLFW for SDL3 or
write our own Win32 backend, no engine code outside this module changes.

The window is also "Vulkan-ready" by default — meaning we tell GLFW NOT
to create an OpenGL context. When the graphics module arrives, it will
ask the window for a native handle (an opaque pointer) and use it to
create a Vulkan surface. Engine code outside graphics never has to know
that the handle is really a `GLFWwindow*`.

Errors that GLFW reports (e.g. failed init, failed window creation) flow
into the engine logger like any other module's errors. There's a
dedicated `Platform` log channel for them.

## Decisions made

- **Backend abstraction is concrete + PIMPL**, not an `IWindow` interface.
  Phase 1 has exactly one backend; an interface is over-engineering until
  there's a second consumer.
- **Vulkan-first by default.** `WindowDesc::client_api_none = true`. We
  keep the toggle for legacy-OpenGL experiments but never expect to flip
  it in real code.
- **GLFW errors bridge through the engine logger**, not through exceptions.
  All public APIs in `crd-platform` are `noexcept`.
- **`g_log_platform` lives inside `crd-platform`.** The
  `g_log_containers`-defined-inside-`crd-log` pattern is a historical
  cycle-break, not a default.
- **Layer stack and `Event` base type are deferred** to a future
  `crd-app` module that will land before Phase 4. Designing them now,
  with no real consumer (no ImGui debug, no UI, no editor), would lock
  in the wrong shape. `crd-platform` only emits hardware-level
  `InputEvent`s; routing/consumption belongs higher up.
- **Hybrid input model is decided but unimplemented**: v1c will ship
  polling-first `InputState` plus an opt-in `RingBuffer<InputEvent>`
  queue for code that needs ordered events.
- **Gamepad is out of Phase 1.** GLFW joystick API is too thin for real
  gamepad support; that comes via SDL3 / XInput in a later phase.
- **`Extent2D` is a small platform-local POD**, not `Vec2<i32>`. Math's
  `MathScalar` concept is intentionally float/double only and we don't
  want to weaken it for a window API.
- **`std::getenv` warning suppression is target-scoped**, not engine-wide:
  only the test target gets `_CRT_SECURE_NO_WARNINGS`. The library itself
  stays under `/W4 /WX` clean.

## Files touched

- `CMakeLists.txt` — added GLFW via CPM, `add_subdirectory(engine/platform)`
- `engine/platform/CMakeLists.txt` — new
- `engine/platform/include/crd/platform/platform.hpp` — new (umbrella)
- `engine/platform/include/crd/platform/context.hpp` — new
- `engine/platform/include/crd/platform/window.hpp` — new
- `engine/platform/include/crd/platform/log_channel.hpp` — new
- `engine/platform/src/log_channel.cpp` — new (channel definition)
- `engine/platform/src/context.cpp` — new
- `engine/platform/src/window.cpp` — new
- `tests/CMakeLists.txt` — added `add_subdirectory(platform)`
- `tests/platform/CMakeLists.txt` — new
- `tests/platform/test_platform.cpp` — new (7 cases)
- `runtime/CMakeLists.txt` — added `smoke_window` target
- `runtime/examples/smoke_window.cpp` — new
- `CONTEXT.md` — module status + dependency note + systems link
- `docs/ROADMAP.md` — status table, step 8 split into 8a..8d, decision
  log entry, "Where I left off"
- `docs/systems/platform.md` — new (system overview)

## Tests / verification

- Built? ✅ on `win-debug`, `win-release`, `win-asan` (each from a clean cache)
- `win-debug`: `159/159` Catch2 tests pass
- `win-release`: `158/158` (Debug-only memory-stats test correctly skipped)
- `win-asan`: `159/159`, no leaks, no use-after-free, no out-of-bounds
- `smoke_window` opens a real 1280x720 Vulkan-ready window and exits
  cleanly on user close

## Implementation wrinkles captured

- **GLFW build pollutes warning policy.** `add_compile_options(/W4 /WX)`
  at root scope applies to GLFW too. Stripping warnings on the `glfw`
  target after `CPMAddPackage` is the right knob.
- **`unique_ptr` default move-assign does not run a custom Impl
  destructor in time.** Our `Impl` is POD, so `glfwDestroyWindow` would
  never get called on the overwritten window. Hand-written move-assign
  fixes the leak.
- **`std::getenv` triggers MSVC C4996**. Don't reach for
  `_CRT_SECURE_NO_WARNINGS` engine-wide — scope it to the one test
  target that needs it.
- **`MathScalar` is float/double only.** Trying to make `Vec2<i32>`
  failed with concept-not-satisfied; this is by design. Use a local
  POD instead.

## Next session starts with

`crd-platform` v1b — `Timer` and `FrameClock`, both built on
`std::chrono::steady_clock` and deliberately decoupled from GLFW so they
can be unit tested without a backend.
