# Session — 2026-04-26 — log module

## Goal

Build a complete custom logging system (`crd-log`) on top of `crd-core`, with
real production features: levels, channels, multiple sinks, sync + async
dispatch, and compile-time stripping. Hook the runtime up to it as a smoke test.

## What we built / changed

- **New module `crd-log`** under `engine/log/`. Static library, depends only on
  `crd-core`. Public headers in `engine/log/include/crd/log/`, implementation in
  `engine/log/src/`. Five sinks (Console, File, Debugger, RingBuffer, Null) and
  the dispatch core.
- **Per-channel filtering** via `CRD_DEFINE_LOG_CHANNEL(VAR, "Name", DefaultLevel)`.
  Each subsystem owns its own channel; channels register themselves at startup
  into a lock-free intrusive list.
- **User-facing macros** `CRD_LOG_TRACE/DEBUG/INFO/WARN/ERROR/CRITICAL` with
  compile-time level stripping driven by the new CMake option `CRD_LOG_MIN_LEVEL`.
  Stripped levels become `((void)0)` — no code, no string, no symbol.
- **Async pipeline** (opt-in): single worker thread, mutex + condvar queue. Drops
  on overflow by default so the game thread never blocks. `Critical` always
  bypasses the queue and flushes synchronously so a dying engine still gets its
  last words to disk.
- **MSVC `/Zc:preprocessor`** added globally to enable C++20 `__VA_OPT__` in the
  log macros.
- **Runtime hooked up**: `runtime/src/main.cpp` now declares four demo channels
  (Engine / Renderer / Physics / Audio), spawns two worker threads, exercises
  every level, and writes to both Console and a rotating `engine.log`.
- **13 Catch2 tests** under `tests/log/test_log.cpp`. All pass.
- **Long-form documentation** at `docs/log/LOG_FILE.md` — eleven sections, plain
  English, deep-dive into every design decision and the path of one log call.

## Plain-English explanation

The logger lets any part of the engine write a line of text — with a level,
a subsystem name, a timestamp, and the source file + line — to one or more
destinations. You write `CRD_LOG_INFO(g_log_renderer, "loaded {} meshes", n)`
and the system handles formatting, filtering, threading, and routing.

It exists because every engine needs to tell you what it's doing, and using
`std::cout` everywhere is slow, ugly, and unmanageable. With named channels
you can say "show me only what the physics module is doing" without touching
any other code. With sinks you can capture everything to disk while only
showing important things on the terminal. With async dispatch the game thread
doesn't wait for I/O — a worker thread does the writing. With compile-time
stripping the chatty `Trace` and `Debug` calls vanish completely from release
builds, so they cost literally zero.

You'd use it by declaring one channel per subsystem in its `.cpp`, calling
`crd::log::init()` once at startup, attaching the sinks you want
(`ConsoleSink`, `FileSink`, etc.), and then sprinkling `CRD_LOG_*` calls
anywhere. At shutdown you call `crd::log::shutdown()` and it drains and
flushes everything cleanly.

## Decisions made

- `std::format` as the formatting backend. No fmtlib dependency.
- Hybrid sync + async dispatch. Default sync; async opt-in.
- `Critical` always bypasses async and flushes immediately.
- Compile-time channels via macro, registered through a lock-free intrusive list.
  No std::vector, no SIOF risk.
- `std::source_location` for call site capture (not `__FILE__`/`__LINE__`).
- Default sinks at startup are NOT auto-attached — user code adds what it wants.
- Log depends only on `crd-core`. No reverse dependency.
- MSVC `/Zc:preprocessor` is mandatory for the project from now on.

## Files touched

- `CMakeLists.txt` — added `CRD_LOG_MIN_LEVEL` option, `/Zc:preprocessor`,
  uncommented `add_subdirectory(engine/log)`.
- `engine/core/include/crd/core/build_config.hpp.in` — exposed
  `CRD_LOG_LEVEL_*` and `CRD_LOG_MIN_LEVEL_NUM`.
- `engine/log/**` — entire new module (12 headers, 9 source files, 1 CMakeLists).
- `runtime/CMakeLists.txt` — link `crd-log`.
- `runtime/src/main.cpp` — replaced the placeholder with a real logger smoke test.
- `tests/CMakeLists.txt` + `tests/log/CMakeLists.txt` + `tests/log/test_log.cpp`.

## Tests / verification

- Build: ✅ `cmake --build --preset win-debug`
- Tests: ✅ `13/13` Catch2 tests pass with `ctest --preset win-debug`.
- Manual: ran `crd-runtime.exe`. Console output and `engine.log` are correct;
  multi-thread interleaving is visible; the suppressed Audio Info message is
  correctly absent; Critical lands before Error in `engine.log` because of the
  sync bypass — exactly as designed.

## Next session starts with

1. Set up the `docs/` structure (this folder, ROADMAP, session template,
   systems overviews). [Done in the next session, 2026-04-26-roadmap-planning.]
2. Wire `crd-core` assert handler → `crd-log` Critical (small bridge layer so
   `CRD_ASSERT` failures land in the log).
3. Begin `crd-memory` v1.
