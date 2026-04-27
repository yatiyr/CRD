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
| `crd-memory`     | ✅      | 1     | IAllocator + Malloc/Linear/Stack/Pool, streaming-ready iface |
| `crd-containers` | ✅      | 1     | Array, FixedArray, Span, String, RingBuffer, HashMap, HashSet |
| Phase 1 quality  | ✅      | 1     | CI, benchmarks, PCH, runtime split, clang-cl, tidy, assert   |
| `crd-math`       | ✅      | 1     | float+double, scalar-first, column-major, radians           |
| `crd-platform`   | 🚧      | 1     | v1a window+context shipped (GLFW). Timer/Input/FS pending.   |
| `crd-app`        | ⏳      | 4-pre | LayerStack + Event router; deferred until ImGui/UI/Editor    |
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
| 3a   | bridge           | wire `crd-core` assert handler → `crd-log` Critical               | ✅     |
| 3b   | `crd-memory` v1  | IAllocator interface + alignment + memory_stats + MallocAllocator | ✅     |
| 3c   | `crd-memory` v1  | LinearAllocator + StackAllocator                                  | ✅     |
| 3d   | `crd-memory` v1  | PoolAllocator + construct/destroy helpers + tests                 | ✅     |
| 4a   | `crd-containers` v1a | Array<T>, FixedArray<T,N>, Span alias, hash defaults          | ✅     |
| 4b   | `crd-containers` v1b | String (SSO 23B), StringView alias, RingBuffer<T>             | ✅     |
| 4c   | `crd-containers` v1c | HashMap<K,V> (Robin Hood backshift), HashSet<K>               | ✅     |
| 4d   | cleanup          | RingBufferSink storage -> Array<T>; broke crd-log↔crd-containers cycle | ✅     |
| 5    | mid-phase eval   | quality scorecard, risk review, finishing scope (no code)         | ✅     |
| 6a   | quality pass     | GitHub Actions CI: matrix(Debug \| Release \| ASan) × MSVC        | ✅ |
| 6b   | quality pass     | Benchmark suite: log call-site cost, async producer push, Array push, HashMap insert/find | ✅ |
| 6c   | quality pass     | PCH for crd-core (types/asserts/platform); measure full-build delta | ✅ |
| 6d   | quality pass     | Split runtime/main.cpp into runtime/examples/ (smoke_log, smoke_memory, smoke_containers) | ✅ |
| 6e   | quality pass     | clang-cl on Windows + Linux GCC build matrix (cross-compiler)     | ✅ |
| 6f   | quality pass     | Run win-tidy preset, triage warnings (fix or explicitly suppress) | ✅ |
| 6g   | quality pass     | Doxygen-friendly review of public headers (no generation yet)     | ✅ |
| 6h   | quality pass     | Real CRD_ASSERT(false) bridge test (handler bypass for tests)     | ✅ |
| 7a   | `crd-math`       | design contract + scalar foundations + Vec2/3/4                  | ✅     |
| 7b   | `crd-math`       | Mat2/3/4 (column-major)                                           | ✅     |
| 7c   | `crd-math`       | Quat (Hamilton, xyzw) + Transform                                 | ✅     |
| 7d   | `crd-math`       | Ray, Plane, AABB, Sphere, Triangle, Frustum                       | ✅     |
| 8a   | `crd-platform`   | Window (GLFW) + PlatformContext + smoke_window                    | ✅     |
| 8b   | `crd-platform`   | Timer + FrameClock (chrono-only)                                  | ⏳     |
| 8c   | `crd-platform`   | Input (polling snapshot + opt-in event queue)                     | ⏳     |
| 8d   | `crd-platform`   | Filesystem, DynamicLibrary, threading helpers                     | ⏳     |
| 9    | closeout         | CONTEXT.md sweep, retrospective session log, Phase 2 prep         | ⏳     |

Estimate: ~11-13 sessions to finish Phase 1 from here (4 math + 2 platform +
1 closeout, with some headroom for cross-compiler shakeout and follow-up bench
work).

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
- **Current shipped dependency graph:** `crd-log -> crd-core, crd-containers`.
  `crd-core` never depends on log; the assert bridge stays one-way.

### 2026-04 — Memory v1 (shipped)

- **Two-phase strategy.**
  - Phase A (shipped, `crd-memory` v1): `IAllocator` interface + 4 simple
    allocators (Malloc / Linear / Stack / Pool). Architecture-correct,
    implementation-simple.
  - Phase B (in Phase 2 alongside graphics): TLSF, BuddyAllocator, RingAllocator,
    StreamingAllocator, GPUAllocator. Real workload-driven implementations.
- **`IAllocator` exposes `reallocate` and `allocation_size` from day one** with
  default implementations, so Phase B can override without breaking interface.
- **Containers take `IAllocator*` as a constructor argument**, not as a template
  parameter. Type stays stable when allocator changes (EA STL / Bitsquid pattern,
  not std::vector pattern). This keeps open-world streaming a drop-in change later.
- **Default alignment = 16 bytes** (SSE-friendly). Cache line = 64 bytes constant.
- **Allocators are not thread-safe** by default. Concurrent users get separate
  instances or use a wrapper. `MallocAllocator` is the exception (libc serialises).
- **OOM in heap allocators is fatal** (`CRD_LOG_CRITICAL` + `CRD_FATAL`).
  Sub-budget allocators (linear/stack/pool) return `nullptr` on exhaustion so
  callers can fall back gracefully.
- **MemoryStats tracking is debug-only.** Release builds zero overhead. Public
  API (struct + `snapshot()`) is identical between builds for ABI stability.
- **`deallocate(nullptr)` is always safe.**
- **`StackAllocator::Marker`** carries an owner pointer in debug builds so a
  cross-allocator rollback trips an assert.
- **`crd-memory` depends on `crd-log`** for `g_log_memory` channel diagnostics.
  Module dependency list updated accordingly.

### 2026-04 — Math (planned)

- **Column-major matrices** (Vulkan / GL convention). `Mat * Vec` ordering.
- **Radians everywhere.** Helpers for degrees ↔ radians exist, but radians are
  the default semantic unit everywhere else.
- **Scalar first, SIMD later.** v1 is the reference implementation; SIMD lands
  only after scalar semantics are locked and benchmarked.
- **`float` and `double` are both first-class in the public API.** Template
  backbone with named aliases (`Vec3f`, `Vec3d`, etc.).
- **Quaternion convention:** Hamilton, stored as `xyzw`, identity =
  `(0, 0, 0, 1)`.
- **`Transform` ships in v1.** Graphics and robotics can share the same engine-
  owned spatial-math substrate from the start.

### 2026-04 — Math roadmap (long-range)

Planned slices, in order:

- **M0 Design Contract** — semantics, naming, failure policy, SIMD policy,
  benchmark policy.
- **M1 Scalar Foundations** — constants, angle helpers, approx compares,
  finite/NaN helpers.
- **M2 Vectors** — `Vec2/3/4<T>` with `f32` and `f64` aliases.
- **M3 Matrices** — `Mat2/3/4<T>`, column-major, `Mat * Vec`.
- **M4 Quat + Transform** — Hamilton quaternions, `Transform<T>`.
- **M5 Core Geometry** — `Ray`, `Plane`, `AABB`, `Sphere`, `Triangle`,
  `Frustum`.
- **M6 SIMD Foundation** — internal acceleration layer, scalar/SIMD parity,
  perf baselines.
- **M7 Dense Numerical** — small dense solves, factorisations, least squares.
- **M8 Sparse + Solvers** — sparse formats, assembly, iterative solvers.
- **M9 Robust Computational Geometry** — predicates, clipping, hulls,
  intersection robustness.

### 2026-04 — Containers v1 (v1a + v1b shipped, v1c in progress)

Scope (deliberately limited):

- `Array<T>`, `FixedArray<T, N>`, `Span<T>` (alias to `std::span`). **shipped in v1a**
- `hash.hpp` defaults: splitmix64 for integers, FNV-1a 64-bit for byte sequences. **shipped in v1a**
- `String` (SSO, 23-byte inline buffer), `StringView` alias to `std::string_view`. **shipped in v1b**
- `RingBuffer<T>` (single-threaded v1, power-of-two capacity). **shipped in v1b**
- `HashMap<K, V>` (open addressing + Robin Hood + backshift deletion),
  `HashSet<K>` (HashMap with empty value). **planned for v1c**

**Not in v1:** Vector (= Array), linked list, RB-tree-backed map, std::stack
adapter, PriorityQueue, generic Tree, generic Graph, SmallVector,
IntrusiveList, Optional/Variant/Pair (use std). Those either duplicate std,
duplicate Array, are speculative, or are subsystem-specific structures
(SceneGraph, RenderGraph, BVH, BehaviorTree) that belong in their own modules.

Decisions:

- **Allocator-aware via constructor argument**, not template parameter. Type
  remains stable across allocator changes.
- **`Array<T>` grows 1.5x**, initial capacity 8 elements.
- **Two push APIs**: `push_back` (assert + grow / fatal on OOM) and
  `try_push_back` (returns false if a sub-budget allocator refused).
- **`HashMap` uses Robin Hood probing with backshift deletion** (no tombstones).
  Erase cost is proportional to probe distance, but lookups stay fast.
- **`String` SSO = 23 inline bytes** (24-byte payload). Allocator pointer
  brings total `sizeof(String)` to 32 bytes — two strings per cacheline.
- **Iterators are std-compatible** so `<algorithm>` and range-for "just work".
- **Hash defaults**: splitmix64 for `u32/u64/i32/i64`, FNV-1a 64-bit for raw
  bytes, `std::hash<T>` fallback for everything else.
- **Heterogeneous lookup for string-keyed maps**: `HashMap<String, V>::find`
  accepts `StringView` / `const char*` so callers don't allocate temporary
  Strings just to look up.
- **`crd-containers` depends on `crd-core`, `crd-memory` and only includes**
  `crd-log` headers for channel declarations. The link arrow is one-way:
  `crd-log -> crd-containers`.

### 2026-04 — Containers v1b shipped (String, RingBuffer)

Concrete decisions made during implementation:

- **`String` ctors from `const char*` and `std::string_view` are explicit.**
  Eliminates `s == "literal"` overload ambiguity. Cost: `String s = "x";`
  doesn't compile — use `String s("x");`. Worth it.
- **`String` SSO discriminant byte at offset 23 is `0..23` in small mode,
  `0xFF` in heap mode.** Encodes capacity in the low 56 bits of the heap
  struct's `cap_and_flag`; top byte is the sentinel.
- **`String` heterogeneous hash equality is mandatory.**
  `DefaultHash<String>{}(s) == DefaultHash<StringView>{}(StringView{s})`
  pinned by a unit test. Required prerequisite for v1c HashMap.
- **`RingBuffer::try_push` does not overwrite when full.** Refuses
  instead. Overwriting is a separate policy that will be wrapped on
  top of RingBuffer in v1d.
- **`RingBuffer<T>` is move-only, single-threaded in v1.** SPSC lock-free
  version comes when the job system arrives in Phase 2.
- **Force-link anchor extended to two `.cpp`s.** Each non-template `.cpp`
  added to `crd-containers` from now on needs a `force_link_X()` helper
  paired with a per-TU anchor in `containers.hpp`.

### 2026-04 — Mini quality pass (containers v1b)

First time the engine has been built and tested in all three flavours:

- `win-debug`: 100/100 tests pass.
- `win-release`: 99/100 (one Debug-only memory-stats test correctly
  skipped via `#if defined(CRD_DEBUG)`). Compile-time level stripping of
  `CRD_LOG_TRACE`/`CRD_LOG_DEBUG` visually verified — the binary
  contains no trace/debug log strings.
- `win-asan`: 100/100 with no leaks, no use-after-free, no out-of-bounds.

This is now the standard quality pass we'll run at the end of every
container/math/platform session. Phase 1's "finishing" step will graduate
this into CI.

### 2026-04 — Containers v1c + v1d shipped (HashMap, HashSet, cycle break)

v1c: open-addressing `HashMap<K, V>` with **Robin Hood probing +
backshift deletion** (no tombstones, no per-erase tombstone cleanup).
`HashSet<K>` is a thin wrapper over `HashMap<K, EmptySetValue>`.

- **Heterogeneous lookup** for `HashMap<String, V>`: `find(StringView)` /
  `find(const char*)` resolves directly without allocating a temporary
  String. `DefaultHash<String>` has overloads producing identical u64
  for identical bytes regardless of the input type.
- **`kMaxLoadFactor = 0.875`**, power-of-two capacity, 2x growth.
- **`std::equal_to<>`** (transparent) is the default `KeyEqual`, so
  String == StringView heterogeneous comparisons "just work" via v1b's
  friend operators.

v1d: two cleanups in one go:

1. **Module dependency cycle broken.** `g_log_containers` (and any
   future first-party channel) is now *defined* in
   `engine/log/src/log_channels_first_party.cpp`, only *declared* in
   `<crd/containers/log_channel.hpp>`. CMake graph: `crd-log →
   crd-containers` (one-way, no cycle).
2. **Log's `RingBufferSink` storage** migrated from `std::vector` to
   `crd::containers::Array<StoredLogRecord>`. Same external API
   (`snapshot()` still returns `std::vector` for caller convenience).
   Same overwrite-on-full behaviour (we did NOT migrate to
   `crd::containers::RingBuffer<T>`, which refuses on full — the
   storage swap was sufficient).

Implementation gotchas captured for future modules:

- **Force-link anchors must use `volatile int`, not `const int`.**
  MSVC was folding `inline const int = func()` initialisations away —
  the function returned a constant, optimizer concluded the call had
  no effect, no UNDEF reference was emitted, the linker stripped the
  TU. Verified with `dumpbin /symbols`. `volatile` forces a real load
  + call. Apply this to every new force-link anchor.
- **The `force_link_first_party_channels()` pattern is reusable.** Any
  future module-specific channel that would create a circular link
  goes into `log_channels_first_party.cpp` and the consumer's umbrella
  header anchors it.

Quality pass at session end:

- `win-debug`: 119/119 tests pass.
- `win-release`: 118/118 (Debug-only stats test correctly skipped).
- `win-asan`: 119/119 with no leaks, no UAF, no OOB.

`crd-containers` v1 is **complete**.

### 2026-04 — Mid-phase quality evaluation (no-code session)

After `crd-containers` v1 closed, we did a one-shot **review session**
(see `docs/sessions/2026-04-26-mid-phase-evaluation.md`) and concluded:

- The engine has four solid modules, 119 green tests across three
  build flavours, and a clean one-way module dependency graph. Aggregate
  quality score: ~7.4/10. Largest weak axes: **performance** (no
  baseline), **build infrastructure** (no CI, no PCH), and
  **cross-platform** (MSVC only).
- Math is a 3-session block; starting it on a foundation with these
  weaknesses risks compounding errors that are easier to catch
  per-module than at Phase 1 end.
- Decision: **move the "finishing" step forward** — do a dedicated
  quality pass (CI / benchmark / PCH / runtime split / cross-compiler
  / tidy / Doxygen review / real assert bridge test) **before** math
  begins, not after platform.

The Phase 1 step list was rewritten to reflect this. New numbering:
1–4 = core/log/memory/containers (done), 5 = this evaluation (done),
6a–6h = quality pass (next), 7 = math, 8 = platform, 9 = closeout.

Implementation notes captured for the upcoming quality session:

- **CI matrix is `(Debug | Release | ASan) × MSVC` to start.**
  clang-cl and Linux GCC are step 6e; if they surface a lot, that
  step bleeds into a follow-up session — by design.
- **Benchmark numbers must be committed to the repo as a baseline file.**
  Otherwise we re-litigate "is this slower than before?" at every PR.
- **PCH lands first** before math touches anything, so the
  full-build time delta is attributable to PCH alone.
- **`runtime/main.cpp` split happens before math** so math's smoke
  section starts in `runtime/examples/smoke_math.cpp` and never enters
  the kitchen-sink main.cpp.

### 2026-04 — Phase 1 quality pass shipped

- **CI upgraded** to a Windows MSVC matrix: `win-debug`, `win-release`,
  `win-asan`, plus separate `win-tidy` and `win-clang-cl` jobs.
- **Benchmarks now live in `tests/bench/`** and the first committed
  baseline is `docs/bench/baseline_2026-04.md`.
- **`runtime/main.cpp` is now the startup skeleton** and per-module smoke
  demos moved to `runtime/examples/` (`smoke_log`, `smoke_memory`,
  `smoke_containers`).
- **PCH landed across engine, tests, and runtime targets.** Measured on
  this small codebase, a clean Debug build was slightly slower with PCH
  (14.11 s) than without (13.61 s). We keep it anyway because the
  substrate is now in place before math/platform increase TU count.
- **Real assert bridge test is now end-to-end.** Tests install a no-op
  platform UI hook, trigger `CRD_ASSERT(false)`, and verify the Critical
  record lands in a `RingBufferSink`.
- **clang-cl now compiles the full tree.** The one portability issue found
  was MSVC-only `/Zc:preprocessor`, now only passed to non-clang MSVC.
- **Header review completed.** No blocker for math; main remaining doc
  weakness is uneven Doxygen-style per-symbol comments in `crd-core`.

### 2026-04 — Platform v1a shipped (window + context)

- **Window backend is GLFW**, pulled in via CPM (`gh:glfw/glfw#3.4`) with
  examples / tests / docs / install all turned off. GLFW's `/W4 /WX` would
  fail our build, so we strip warnings on the `glfw` target only.
- **Public API is backend-agnostic.** `PlatformContext` and `Window` never
  expose a `GLFWwindow*` in their headers. `Window` is a concrete class
  with a PIMPL (not an `IWindow` interface). Future SDL3 / custom Win32
  backends are a private rewrite, not a public API change.
- **Vulkan-ready by default.** `WindowDesc::client_api_none = true` maps
  to `GLFW_NO_API`, so no OpenGL context is created. The Vulkan surface
  will go through `Window::native_handle()` from inside `crd-graphics`,
  not from generic engine code.
- **GLFW errors bridge to the logger.** Error callback installs at
  `PlatformContext::create()` time, formats GLFW's error code + message
  through `CRD_LOG_ERROR` into the `g_log_platform` channel.
- **`g_log_platform` is owned inside `crd-platform`.** No cycle, so the
  channel does NOT live inside `crd-log`. The `g_log_containers`
  cycle-break is a historical exception, not the default pattern.
- **`Extent2D` is a small platform-local POD**, not `crd::math::Vec2<i32>`.
  The math `MathScalar` concept is float/double only by design and we
  don't want to weaken it for window coordinates. Side benefit:
  `crd-platform → crd-math` is no longer needed in the link graph.
- **`Window`'s move-assign is hand-written.** Default `unique_ptr`
  move-assign would not call `glfwDestroyWindow` on the Impl being
  overwritten, leaking the OS handle.
- **No layer stack / event router / `Event` base type at this layer.**
  Those will live in a future `crd-app` module that has real consumers
  (ImGui debug overlay, editor panels, game UI). Designing them now,
  with no real consumer, would lock in the wrong shape.
- **Hybrid input model still planned for v1c**, not v1a:
  polling-first `InputState` + opt-in `RingBuffer<InputEvent>` queue,
  hardware-only event POD union, no propagation/consumption semantics.
- **`std::getenv` warning suppression is scoped to the test target only.**
  The platform library itself stays under `/W4 /WX` clean.
- Quality pass at session end:
  - `win-debug`: 159/159 tests pass
  - `win-release`: 158/158 (Debug-only stats test correctly skipped)
  - `win-asan`: 159/159 with no leaks, no UAF, no OOB
  - `smoke_window` opens a real Vulkan-ready 1280x720 window

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

**Last session:** 2026-04-27 — `crd-platform` v1a (window + context). See
`docs/sessions/2026-04-27-platform-v1a-window.md`.

What landed:

1. **GLFW** wired in through CPM (3.4 tag pin), warnings stripped on the
   GLFW target only so its source tree doesn't fail our `/W4 /WX` policy.
2. **`PlatformContext`** RAII wrapper around `glfwInit` / `glfwTerminate`,
   with a GLFW error → log bridge installed at create time. Move-only,
   default-constructible to an invalid state.
3. **`Window`** PIMPL'd over `GLFWwindow*`, Vulkan-ready by default
   (`GLFW_NO_API`). Hand-written move-assign so the OS handle can't leak
   on overwrite.
4. **`Extent2D`** small platform-local POD for window/framebuffer sizes,
   so platform doesn't have to depend on math.
5. **`g_log_platform`** channel owned inside `crd-platform` itself
   (no cycle-break needed; the `g_log_containers` pattern is a historical
   exception, not the default).
6. **7 new tests** in `tests/platform/`, all green across Debug, Release
   and ASan. CI/headless-friendly: `CRD_PLATFORM_HEADLESS=1` skips
   window-creating cases.
7. **`smoke_window`** runtime example opens a real 1280x720 Vulkan-ready
   window and pumps events until close.

Current test counts:

- Debug: `159/159`
- Release: `158/158` (Debug-only stats test correctly skipped)
- ASan: `159/159`

**Next session starts with: `crd-platform` v1b — Timer + FrameClock.**

Key platform decisions now locked in:

- backend hidden behind PIMPL; GLFW header never appears in a public crd-platform header
- concrete `Window`, no `IWindow` interface
- Vulkan-first (`GLFW_NO_API`) — Vulkan surface comes through `native_handle()` from inside `crd-graphics` only
- `Extent2D` instead of `Vec2<i32>` — math contract stays float/double-only
- no layer/event stack at platform level; that's a future `crd-app` module's job
- v1c hybrid input: polling snapshot + opt-in `RingBuffer<InputEvent>`, hardware-only POD union, no consumption semantics

Approximately 3–5 sessions from here to Phase 1 close (1 timer/clock,
1 input, 1 filesystem/dynlib/threading, 1 closeout, plus headroom for
cross-compiler / Linux GLFW shakeout).
