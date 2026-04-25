# CRD Engine — Roadmap

> Living document. Update the status table when a module ships, append to
> the decision log when an architectural choice is made, and add a session
> note under `docs/sessions/` when work happens.

---

## Quick status

| Module           | Status | Phase | Notes                                                        |
| ---------------- | ------ | ----- | ------------------------------------------------------------ |
| `crd-core`       | ✅      | 1     | types, platform, assert, build_config                        |
| `crd-log`        | ✅      | 1     | levels, channels, sinks, sync/async dispatch                 |
| `crd-memory`     | ⏳      | 1     | next up — IAllocator + 4 simple allocators                   |
| `crd-math`       | ⏳      | 1     | column-major, radians                                        |
| `crd-containers` | ⏳      | 1     | allocator-aware, no template-allocator pattern               |
| `crd-platform`   | ⏳      | 1     | window/input/timer/filesystem (GLFW)                         |
| `crd-graphics`   | ⏳      | 2     | Vulkan-first, RHI abstraction                                |
| GPU memory + streaming | ⏳ | 2     | TLSF, BuddyAllocator, StreamingAllocator, GPUAllocator       |
| `crd-resources`  | ⏳      | 2.5   | async I/O, LRU eviction, open-world streaming pipeline       |
| `crd-scripting`  | ⏳      | 3     | hot-reload C++ DLLs, C API boundary                          |
| `crd-ui`         | ⏳      | 4     | retained-mode game UI, replaces ImGui for non-debug          |
| Editor           | ⏳      | 5     | built on `crd-ui`                                            |

Legend: ✅ done · ⏳ planned · 🚧 in progress · ❌ blocked

---

## Phase 1 — Core Modules

Goal: a working foundation where every module can log, allocate, do math,
hold containers, and talk to the OS.

| Step | Module           | Sub-task                                                          | Status |
| ---- | ---------------- | ----------------------------------------------------------------- | ------ |
| 1    | `crd-core`       | types, platform, assert, build_config                             | ✅     |
| 2    | `crd-log`        | full implementation (see `docs/log/LOG_FILE.md`)                  | ✅     |
| 3a   | bridge           | wire `crd-core` assert handler → `crd-log` Critical               | ⏳     |
| 3b   | `crd-memory` v1  | IAllocator interface + alignment + memory_stats + MallocAllocator | ⏳     |
| 3c   | `crd-memory` v1  | LinearAllocator + StackAllocator                                  | ⏳     |
| 3d   | `crd-memory` v1  | PoolAllocator + construct/destroy helpers + tests                 | ⏳     |
| 4a   | `crd-math`       | Vec2/3/4 + basic ops                                              | ⏳     |
| 4b   | `crd-math`       | Mat2/3/4 (column-major) + Quat + Transform                        | ⏳     |
| 4c   | `crd-math`       | AABB, Sphere, Ray, Plane, Frustum                                 | ⏳     |
| 5a   | `crd-containers` | Array<T>, FixedArray<T,N>, Span<T>                                | ⏳     |
| 5b   | `crd-containers` | HashMap<K,V>, String (SSO), RingBuffer<T>                         | ⏳     |
| 5c   | cleanup          | move log's RingBufferSink onto `crd-containers::RingBuffer`       | ⏳     |
| 6a   | `crd-platform`   | Window (GLFW), Timer, basic input                                 | ⏳     |
| 6b   | `crd-platform`   | Filesystem, DynamicLibrary, threading helpers                     | ⏳     |
| 7    | finishing        | ASan/UBSan across all modules, benchmark pass, CONTEXT.md sweep   | ⏳     |

Estimate: ~13 sessions to finish Phase 1.

---

## Phase 2 — Graphics

Vulkan-first, API-agnostic abstraction.

1. Vulkan bootstrap (instance, device, swapchain, command pools)
2. GPU memory + `crd-memory` v2: TLSF, BuddyAllocator, GPUAllocator
3. RHI layer (API-agnostic), shader reflection
4. `crd-resources` module: async I/O, LRU eviction
5. **StreamingAllocator** — virtual memory reservation, page commit/decommit. Real
   open-world streaming pipeline lives here.
6. Frame graph / render graph
7. ImGui integration (debug overlay only)
8. First renderable 3D scene

Estimate: 25–30 sessions.

---

## Phase 3 — Scripting

Hot-reload C++ via DLLs, C API boundary, plugin system for third-party extensions.

## Phase 4 — Game UI System

`crd-ui` on top of `crd-graphics`. Retained mode, layout, animation. ImGui demoted to
debug overlay.

## Phase 5 — Editor

Built on `crd-ui` (replaces ImGui for editor panels).

---

## Architectural decisions (chronological log)

Each decision is permanent unless explicitly revisited. Add new entries at the bottom.

### 2026-04 — Build & language

- **C++20**, no compiler extensions. Allman braces, 4-space indent, 120 col.
- **CMake 3.25+ + Ninja**. `GLOB_RECURSE CONFIGURE_DEPENDS` for source detection.
- **CPM.cmake** for third-party deps; static libraries for engine modules; dynamic
  only for scripting / plugins.
- **MSVC `/Zc:preprocessor` is required** (for `__VA_OPT__` in log macros).

### 2026-04 — Logging

- **`std::format`** as the formatting backend (no fmtlib dependency).
- **Hybrid sync + async dispatch.** Default sync; async opt-in via `LoggerConfig`.
- **`Critical` always bypasses async**, so a dying engine still gets its last words
  to disk.
- **Compile-time channels** declared per subsystem via `CRD_DEFINE_LOG_CHANNEL`,
  registered into a global lock-free intrusive list.
- **`std::source_location`** captures call site automatically through the macro.
- **Compile-time level stripping** via `CRD_LOG_MIN_LEVEL` cache option:
  Trace in Debug, Info in Release. Critical never strips.
- **Default sinks at startup are NOT auto-attached.** User code must add sinks.
- **Log depends only on `crd-core`.** No reverse dependency from core to log.

### 2026-04 — Memory (planned, not yet implemented)

- **Two-phase strategy.**
  - Phase A (now, in `crd-memory` v1): `IAllocator` interface + 4 simple allocators
    (Malloc / Linear / Stack / Pool). Architecture-correct, implementation-simple.
  - Phase B (in Phase 2 alongside graphics): TLSF, BuddyAllocator, RingAllocator,
    StreamingAllocator, GPUAllocator. Real workload-driven implementations.
- **`IAllocator` exposes `reallocate` and `allocation_size` from day one** with
  default implementations, so Phase B can override without breaking interface.
- **Containers take `IAllocator*` as a constructor argument**, not as a template
  parameter. Type stays stable when allocator changes (EA STL / Bitsquid pattern,
  not std::vector pattern). This keeps open-world streaming a drop-in change later.
- **Default alignment = 16 bytes** (SSE-friendly). Cache line = 64 bytes constant.
- **Allocators are not thread-safe** by default. Concurrent users get separate
  instances or use a wrapper.
- **OOM behavior: `CRD_FATAL` and crash.** No pretend-recovery.
- **MemoryStats tracking is debug-only.** Release builds zero overhead.

### 2026-04 — Math (planned)

- **Column-major matrices** (Vulkan / GL convention). `Mat * Vec` ordering.
- **Radians everywhere.** Helpers for degrees → radians, never the other way as
  default.
- **Scalar first, SIMD later.** No SSE/NEON in the initial cut.

### 2026-04 — Open-world streaming

Streaming is a *pipeline*, not just an allocator. The full pipeline needs:

1. Allocator architecture (bugün, Phase A)
2. Job system (Phase 2.5)
3. Async filesystem I/O (`crd-platform` + Phase 2.5)
4. Resource manager / streamer (`crd-resources`, Phase 2.5)
5. Streaming allocator implementation (Phase 2)

Doing the streaming allocator alone, without 2–4, is wasted work. Doing the
allocator interface correctly today (Phase A) makes 2–5 a drop-in addition.

---

## Glossary

- **Channel** — a named log filter for one subsystem (e.g. `Renderer`).
- **Sink** — a log destination (Console / File / Debugger / RingBuffer / Null).
- **RHI** — Render Hardware Interface; the API-agnostic graphics layer.
- **TLSF** — Two-Level Segregated Fit; an O(1) general-purpose allocator.
- **VMA** — Vulkan Memory Allocator (the AMD library; we'll write our own).

---

## How to use this folder

- `docs/ROADMAP.md` (this file) — the master plan and decision log.
- `docs/sessions/` — one file per work session. Use `SESSION_TEMPLATE.md` as a
  starting point. Filename convention: `YYYY-MM-DD-short-topic.md`.
- `docs/systems/` — one short overview per shipped subsystem. Plain-English
  "what it is, what it does, how to use it". Long deep-dives (like
  `docs/log/LOG_FILE.md`) live alongside.
- After a system has shipped, **prefer adding to its session log over rewriting
  its overview**. The overview should be stable; sessions tell the story.

---

## Where I left off

> Update this section at the end of every session so future-you can re-enter
> the project without thinking.

**Last session:** 2026-04-26 — `crd-log` shipped (see
`docs/sessions/2026-04-26-log-module.md`).

**Next session starts with:**

1. `docs/sessions/SESSION_TEMPLATE.md` → copy to a dated file.
2. Update `CONTEXT.md` if needed.
3. Wire `crd-core` assert handler → `crd-log` Critical.
4. Begin `crd-memory` v1 — start with `IAllocator`, `alignment.hpp`, `memory_stats.hpp`,
   `MallocAllocator`.

End-of-session goal: `crd-memory` library links, `MallocAllocator` works,
5–8 Catch2 tests green.
