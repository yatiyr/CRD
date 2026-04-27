# Session — 2026-04-27 — crd-app v1 (event + layer + application + bus)

## Goal

Ship `crd-app` as the layer above `crd-platform` and below future graphics /
editor code. The target shape is a Cerid-specific hybrid: Hazel-style
propagated event routing through layers, plus a separate typed event bus for
broadcast notifications.

## What we built / changed

- **`engine/app/`** new module, linked into the engine root build.
- **`event.hpp`**
  - `Event` virtual base with `handled`, `type_id()`, `name()`,
    `categories()`
  - `EventCategory` bitmask enum
  - `EventT<Derived, Categories>` helper for easy built-in and
    application-defined event classes
  - final identity mechanism uses **per-type static tokens**, not a fixed
    global enum, so app code can define its own event classes without
    patching engine headers
- **`event_dispatcher.hpp`**
  - typed dispatch helper against a concrete event class
- **`event_bus.hpp/.cpp`**
  - typed sync bus: `subscribe<T>(fn)`, `unsubscribe(token)`, `publish(event)`
  - uses erased `std::function<void(Event&)>` internally, but the public
    surface stays typed
  - bus is intentionally **sync in v1**; roadmap note keeps async evolution
    open for later
- **`layer.hpp` + `layer_stack.hpp/.cpp`**
  - `Layer` interface: attach/detach/update/render/event hooks
  - `LayerStack` stores raw pointers, while `Application` owns actual layer
    lifetimes through `unique_ptr`
  - update/render iterate forward; event propagation iterates reverse
- **Built-in events**
  - `events/app_events.hpp`: `AppTickEvent`, `AppUpdateEvent`,
    `AppRenderEvent`
  - `events/window_events.hpp`: `WindowCloseEvent`, `WindowResizeEvent`
  - `events/input_events.hpp`: key / mouse move / mouse button / scroll
- **`application.hpp/.cpp`**
  - owns `PlatformContext`, `Window`, `FrameClock`, `EventBus`,
    `LayerStack`, and owned layers
  - `run()` main loop plus single-step `tick()` for tests and future tools
  - lifts `crd-platform::InputEvent` into typed app events
  - `push_layer(std::unique_ptr<Layer>)`
  - `push_overlay(std::unique_ptr<Layer>)`
  - `close()` explicit shutdown path, no singleton
- **`app.hpp`** umbrella header
- **`runtime/examples/smoke_app.cpp`**
  - real app loop
  - demo layer subscribes to `AppTickEvent` on the bus
  - ESC closes through propagated `KeyPressedEvent`
- **Tests**
  - `tests/app/test_event.cpp`
    - dispatcher correctness
    - custom app-defined event over the shipped bus
    - propagated category semantics
  - `tests/app/test_layer_stack.cpp`
    - layer/overlay ordering
    - pop semantics respect partition boundaries
  - `tests/app/test_application.cpp`
    - invisible-window app boots, runs one tick, and closes through a layer

## Plain-English explanation

Cerid now has the missing "application glue" layer. `crd-platform` knows how
to open a window and collect raw input, but it does not know how to run a game
loop, route events through gameplay/editor layers, or publish higher-level
notifications. `crd-app` does that job.

This is intentionally not a pure Hazel clone. Input/window/application events
still propagate through layers in the familiar Hazel order, because that model
is genuinely good for UI-style consumption. But Cerid also ships a second path:
a typed event bus for broadcast notifications that should reach everyone rather
than being consumed by the first interested listener.

The other important difference from a typical toy engine design is that app
code can define its own events without editing engine enums or global
registries. Event identity is per-type, not centrally assigned.

## Decisions made

- **Hazel-style propagation stays** for input/window/app routing.
- **EventBus is separate from propagation**, not a replacement for it.
- **Bus is sync in v1**; async stays on the roadmap, not in this slice.
- **Bus is typed-template**, not string-keyed.
- **Handled semantics apply only to propagated events.** Bus events are
  broadcast and non-consuming.
- **`Application` is not a singleton.** Lifetime stays explicit and testable.
- **Custom app-defined events are first-class.** Static type tokens won over
  a fixed enum registry because downstream applications should not need to
  patch engine code just to add their own event types.

## Files touched

- `CMakeLists.txt` — added `engine/app`
- `engine/app/CMakeLists.txt` — new
- `engine/app/include/crd/app/app.hpp` — new umbrella
- `engine/app/include/crd/app/application.hpp` — new
- `engine/app/include/crd/app/event.hpp` — new
- `engine/app/include/crd/app/event_dispatcher.hpp` — new
- `engine/app/include/crd/app/event_bus.hpp` — new
- `engine/app/include/crd/app/layer.hpp` — new
- `engine/app/include/crd/app/layer_stack.hpp` — new
- `engine/app/include/crd/app/events/app_events.hpp` — new
- `engine/app/include/crd/app/events/input_events.hpp` — new
- `engine/app/include/crd/app/events/window_events.hpp` — new
- `engine/app/src/application.cpp` — new
- `engine/app/src/event_bus.cpp` — new
- `engine/app/src/layer_stack.cpp` — new
- `tests/CMakeLists.txt` — added `tests/app`
- `tests/app/CMakeLists.txt` — new
- `tests/app/test_event.cpp` — new
- `tests/app/test_layer_stack.cpp` — new
- `tests/app/test_application.cpp` — new
- `runtime/CMakeLists.txt` — added `smoke_app`
- `runtime/examples/smoke_app.cpp` — new
- `docs/systems/app.md` — new
- `docs/ROADMAP.md` — status, decisions, re-entry notes
- `CONTEXT.md` — module status + dependency notes + doc link

## Tests / verification

- `win-debug`: 197/197
- `win-release`: 196/196 (Debug-only stats test correctly skipped)
- `win-asan`: 197/197, no leaks, no UAF, no OOB

## Next session starts with

Closeout / retrospective, then graphics entry. The practical next step is
to define the first render-aware layer that will sit inside `crd-app` while
Phase 2 boots the Vulkan/RHI path toward the first triangle.
