# Session — 2026-04-27 — crd-platform v1b (timer + frame clock)

## Goal

Add the timing facade to `crd-platform` without touching the windowing
layer. Two types: `Timer` (general stopwatch) and `FrameClock` (main-loop
delta + total + frame-count). Both must be GLFW-independent so the rest
of the engine can measure time before any window exists.

## What we built / changed

- **`engine/platform/include/crd/platform/timer.hpp`** — public API for
  `Timer` and `FrameClock`. Both expose `reset()` and a small set of
  read-only accessors. `FrameClock` is the only stateful one (delta /
  total / frame count).
- **`engine/platform/src/timer.cpp`** — implementations using
  `std::chrono::steady_clock` exclusively. The first `FrameClock::tick()`
  seeds the cadence and reports zero delta so the first frame doesn't
  look like a multi-second hitch.
- **`engine/platform/include/crd/platform/platform.hpp`** — umbrella
  header now also pulls in `<crd/platform/timer.hpp>`.
- **`tests/platform/test_timer.cpp`** — 8 deterministic tests covering
  Timer monotonicity, reset, ns/s consistency, FrameClock first-tick
  seed, real deltas after sleep, total time, and reset behaviour.
- **`runtime/examples/smoke_frame_clock.cpp`** — synthetic 5-frame loop
  with logged per-frame delta and total. No window opened.
- **`runtime/CMakeLists.txt`** — added `smoke_frame_clock` target.

## Plain-English explanation

Cerid now knows how to measure time. Two tools:

- `Timer` is a stopwatch. Make one and it starts. Ask it how long it's
  been running, in seconds, milliseconds, or nanoseconds. Hit `reset()`
  and it starts over.
- `FrameClock` is what the game loop will use. Call `tick()` at the top
  of every frame. It tells you how many seconds passed since the last
  frame (`delta_seconds`), how long the engine has been running
  (`total_seconds`), and how many frames you've rendered
  (`frame_count`). The very first tick reports zero delta on purpose,
  so your first frame doesn't look like a giant hitch caused by engine
  startup.

Both are built directly on the C++ standard's monotonic clock, not on
GLFW's `glfwGetTime()`. That means timing works even before a window
exists — useful for things like "how long did module init take?" — and
it stays the same if we ever swap GLFW for another backend.

## Decisions made

- **Use `std::chrono::steady_clock`, not `glfwGetTime()`.** Engine
  timing must not depend on a windowing backend.
- **Seed-on-first-tick.** `FrameClock`'s first `tick()` sets the
  cadence and reports zero delta. This avoids a guaranteed first-frame
  spike whose magnitude is engine startup time.
- **Total time origin = construction**, not first tick. Single stable
  origin, useful for log timestamps and replays.
- **`reset()` exists on both types** and re-seeds the FrameClock. Pause
  / unpause flow is the obvious caller.
- **No `static`/global FrameClock.** Caller owns its instance. Engine
  loop will hold one as a member of whatever drives the loop.

## Files touched

- `engine/platform/include/crd/platform/timer.hpp` — new
- `engine/platform/include/crd/platform/platform.hpp` — added timer include
- `engine/platform/src/timer.cpp` — new
- `tests/platform/test_timer.cpp` — new (8 cases)
- `runtime/examples/smoke_frame_clock.cpp` — new
- `runtime/CMakeLists.txt` — added smoke_frame_clock target
- `docs/systems/platform.md` — v1b ticked, what-ships-today expanded
- `docs/ROADMAP.md` — status table, step 8b ticked, decision log entry,
  "Where I left off"

## Tests / verification

- `win-debug`: 167/167 (8 new)
- `win-release`: 166/166 (Debug-only stats test correctly skipped)
- `win-asan`: 167/167, no leaks, no UAF
- `smoke_frame_clock` prints a clean 5-frame loop with realistic deltas

## Next session starts with

`crd-platform` v1c — Input. Hybrid model from the v1a plan: polling-first
`InputState` snapshot plus an opt-in `RingBuffer<InputEvent>` queue.
Hardware-level POD events only; no propagation / consumption semantics.
