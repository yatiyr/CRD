# Session — 2026-04-27 — crd-platform v1c (input)

## Goal

Wire keyboard + mouse input into `crd-platform`, hybrid model from v1a's
plan: a frame-coherent `InputState` snapshot for the common case, an
opt-in ordered event queue for code that genuinely needs it. Hardware
events only. No layer stack, no consumption — those belong in the future
`crd-app` module.

## What we built / changed

- **`engine/platform/include/crd/platform/input.hpp`** — public API:
  - `Key` enum: letters, digits, F-keys, common navigation/editing keys,
    modifier keys. Values are arbitrary contiguous Cerid indices, not
    GLFW codes.
  - `MouseButton` enum: Left, Right, Middle, X1, X2.
  - `KeyMods` POD bitfield (shift/ctrl/alt/super).
  - `InputEvent` tagged-union POD with one payload per event Type.
  - `InputState` — `is_key_down`, `was_key_pressed`,
    `was_key_released`, mouse position/delta, scroll delta, latest
    `KeyMods`. Mutated only by friend `Input`.
  - `Input` class — owns state + opt-in `OptionalQueue<InputEvent>`.
    Public surface: `state()`, `enable_event_queue(N)`, `try_pop_event()`,
    `event_queue_enabled()`. Backend-side helpers (`push_*`,
    `on_poll_begin`) are public-but-internal so the friend `Window`'s
    callbacks can write into it.
  - `OptionalQueue<T>` is a small inline shim that lazily heap-allocates
    a `RingBuffer<T>` because `RingBuffer` requires capacity at
    construction. Move-only, RAII.
- **`engine/platform/src/input.cpp`** — state-update logic. KeyDown
  flips `pressed` only on a real transition; KeyUp flips `released`
  only when `down` was true. Mouse first-move seeds without a spurious
  delta. Scroll and mouse delta accumulate within a frame; clearing
  happens in `on_poll_begin()`.
- **`engine/platform/include/crd/platform/window.hpp`** — `Window` now
  exposes `Input& input()`, `const Input& input() const`, and
  `poll_input()`. The frame contract is documented:
  `context.poll_events()` first, then `window.poll_input()`, then read
  state.
- **`engine/platform/src/window.cpp`**:
  - `Window::Impl` now contains an owned `Input`.
  - `glfw_key_to_crd`, `glfw_button_to_crd`, `unpack_mods` translation
    helpers in an anonymous namespace, scoped to this TU only.
  - GLFW callbacks (`key`, `mouse_button`, `cursor_pos`, `scroll`,
    `framebuffer_size`) registered at `Window::create()` time. They
    look up `Input*` through `glfwGetWindowUserPointer` and dispatch.
  - Move-assign re-binds the user pointer after Impl swap.
- **`engine/platform/CMakeLists.txt`** — added `crd-memory` to
  `PUBLIC` link targets (RingBuffer needs the allocator interface).
- **`tests/platform/test_input.cpp`** — 12 cases: default-init
  emptiness, key down/up transitions, repeat suppression, unknown key
  drop, mouse button mirror, mouse first-move seed, mouse delta clear,
  scroll accumulate-then-clear, queue opt-in FIFO, queue-disabled drop,
  mods propagation. All run without GLFW.
- **`runtime/examples/smoke_window.cpp`** — now calls
  `window.poll_input()` and closes on `Key::Escape` through the polling
  API, demonstrating the end-to-end flow.

## Plain-English explanation

Cerid can now read input. The default way is "polling": every frame, the
engine asks the Input object whether a given key is held, whether it
*just* got pressed this frame, whether the mouse moved by some amount,
and so on. That's enough for most game code.

If something needs to know *the order* keys were pressed in (a chat
window, a debug overlay), it can opt in to an event queue. While the
queue is enabled, every input GLFW reports also gets appended to a ring
buffer in arrival order; the user code drains it with `try_pop_event`.
While disabled (the default), the queue costs nothing.

The keys aren't GLFW's keys — Cerid has its own enum. The translation
table is one place in the source. If we ever swap to SDL3 or write our
own Win32 backend, only that table changes.

## Decisions made

- **Hybrid: snapshot always on, queue opt-in.** Doesn't penalise the
  common case; doesn't lose ordered events for the cases that need them.
- **Backend-agnostic key codes.** Cerid `Key` is a contiguous enum
  starting from `Unknown=0`. GLFW translation is one switch + a few
  range maps in `window.cpp`.
- **Pressed / released are transition flags, not state alternates.** A
  second KeyDown without an intervening KeyUp does NOT re-trigger
  `was_key_pressed`.
- **First mouse-move seeds without delta.** Same shape as FrameClock's
  first-tick zero-delta — don't synthesize motion from default-init
  coordinates.
- **`Input` lives in `Window::Impl`.** GLFW user-pointer points at the
  `Input` directly. Move-assign re-binds the pointer because Impl
  objects swap.
- **No gamepad in v1c.** GLFW joystick API is too thin for real
  controller work; that comes later through SDL3 / XInput.
- **No `Event` base, no consumption, no layer routing.** Hardware POD
  union only. Higher-level routing is `crd-app`'s job.

## Files touched

- `engine/platform/include/crd/platform/input.hpp` — new
- `engine/platform/include/crd/platform/window.hpp` — Input accessors,
  poll_input
- `engine/platform/include/crd/platform/platform.hpp` — input umbrella
  pulled in by window.hpp
- `engine/platform/src/input.cpp` — new
- `engine/platform/src/window.cpp` — GLFW callback registration + key
  translation tables
- `engine/platform/CMakeLists.txt` — added `crd-memory` to PUBLIC link
- `tests/platform/test_input.cpp` — new (12 cases)
- `runtime/examples/smoke_window.cpp` — ESC-to-close via polling API
- `docs/systems/platform.md` — v1c ticked, what-ships-today expanded
- `docs/ROADMAP.md` — status table, step 8c ticked, decision log,
  Where I left off
- `docs/sessions/2026-04-27-platform-v1c-input.md` — this file

## Tests / verification

- `win-debug`: 179/179 (12 new)
- `win-release`: 178/178 (Debug-only stats test correctly skipped)
- `win-asan`: 179/179, no leaks (OptionalQueue heap alloc/free clean)
- `smoke_window` opens, accepts ESC, closes cleanly

## Implementation wrinkles captured

- **`RingBuffer` has no default ctor.** It needs capacity at
  construction. We can't hold one as a member of a class that's
  default-constructible. The `OptionalQueue<T>` shim lazily allocates
  on heap when `enable_event_queue` is called.
- **Explicit template instantiation of a private nested class is
  awkward in MSVC.** Worked around by inlining all `OptionalQueue<T>`
  members in the header. The instantiation only needs to see one TU.
- **`IAllocator::deallocate(void*)` is single-argument.** Earlier
  modules took size+align; this one takes the pointer only. Worth a
  glance when you reach for it for the first time.
- **Window move-assign must re-bind the GLFW user pointer.** Move ctor
  doesn't because heap-stored Impl keeps its address; move-assign does
  because Impl objects swap.

## Next session starts with

Either Phase 1 closeout (retrospective + crd-app module placement +
forward look at `crd-graphics` entry), or `crd-platform` v1d
(Filesystem + DynamicLibrary + threading helpers). Filesystem is more
useful before graphics; the closeout is more useful for momentum.
Whichever you pick, the platform substrate is now Phase-1-complete for
engine bring-up.
