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
| `crd-platform`   | ✅      | 1     | window, timer, input, filesystem, dynlib, threading baseline all shipped. |
| `crd-app`        | ✅      | 1.5   | Application, LayerStack, propagated events, sync typed EventBus shipped. |
| `crd-rhi`        | 🚧      | 2.0   | v1a interface scaffold, v1b Vulkan bootstrap, v1c frame execution shipped. First triangle next. |
| `crd-rhi-vulkan` | 🚧      | 2.1   | Vulkan instance/device/surface/swapchain + command submission path shipped |
| `crd-renderer`   | ⏳      | 2.5   | high-level rendering: camera, renderables, lighting, materials |
| `crd-jobs`       | ⏳      | 2.7   | job system; pulled forward to feed async I/O + GPU recording |
| GPU memory + streaming | ⏳ | 2.5   | TLSF, BuddyAllocator, StreamingAllocator, GPUAllocator       |
| Shader system    | ⏳      | 2.6   | GLSL→SPIRV→reflection→cache→hot-reload; layered above RHI triangle milestone |
| `crd-resources`  | ⏳      | 2.7   | async I/O, LRU eviction, runtime binary asset format         |
| `crd-tools/asset_cooker` | ⏳ | 2.3 | offline pipeline: glTF/PNG/HDR → Cerid binary formats. Separate exe. |
| First scene      | ⏳      | 2.8   | camera + mesh + forward renderer + skybox                    |
| PBR + lighting   | ⏳      | 2.9   | punctual lights + Cook-Torrance + IBL + CSM shadows          |
| Post-FX          | ⏳      | 2.10  | HDR + ACES tonemap + bloom + TAA                             |
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
| 8b   | `crd-platform`   | Timer + FrameClock (chrono-only)                                  | ✅     |
| 8c   | `crd-platform`   | Input (polling snapshot + opt-in event queue)                     | ✅     |
| 8d   | `crd-platform`   | Filesystem (`<filesystem>`-backed, custom `Path`), DynamicLibrary, threading helpers | ✅     |
| 9    | closeout         | Phase 1 retrospective + CONTEXT.md sweep + Phase 2 prep + bench refresh | 🚧     |

Estimate: closeout is functionally done except for one benchmark-suite
cleanup if we want the book-keeping perfectly clean before deeper graphics.

---

## Phase 1.5 — `crd-app` (Layer + Event)

Sits between Phase 1 close and Phase 2. Lands before graphics so the main
loop, layer stack, and event routing are in place when RHI starts and the
"first triangle" milestone arrives.

Slices:

| Slice | Scope                                                                       |
| ----- | --------------------------------------------------------------------------- |
| v1a   | `Event` base + `EventType`/`EventCategory` + `EventDispatcher` + concrete events (Key/Mouse/Window/App) | ✅ |
| v1b   | `Layer` interface + `LayerStack` (single-vector, overlays at tail; update bottom-up, event dispatch top-down) | ✅ |
| v1c   | `Application` (main loop owner, `crd-platform` `InputEvent` → `Event` lifting, push_layer/push_overlay API) | ✅ |
| v1d   | First real `Layer` example + smoke runtime that wires a custom layer through Application | ✅ |

Decisions baked in (see decision log entry "2026-04 — App / Event design"):

- **Hazel-style virtual `Event` hierarchy**, not tagged-union. Stack-
  allocated, dispatcher takes a reference, no heap traffic. The shipped
  identity model is per-type static token based, so applications can define
  custom events without touching engine enums.
- **Layer ownership:** `Application::push_layer(std::unique_ptr<Layer>)`.
  Application owns; user code holds raw pointers if it needs them.
- **`on_render()` lives in the API but is empty until graphics arrives.**
  Phase 2 layers (a future `RenderLayer`) fill it.
- **`crd-app` does NOT depend on `crd-rhi`, `crd-rhi-vulkan`, or
  `crd-renderer`.** It depends only on `crd-core / crd-log /
  crd-containers / crd-platform`. Render-aware layers are written
  downstream and pulled in by user code.
- **EventBus is sync for now.** Async remains a later extension, not part of
  the shipped Phase 1.5 surface.
- **Handled semantics belong only to propagated events.** Bus events are
  broadcast notifications, not consumable routing decisions.

Estimate: shipped.

---

## Phase 2 — Graphics

Vulkan-first, but split into explicit layers from day one:

- **`crd-rhi`** = minimal, API-agnostic GPU interface only
- **`crd-rhi-vulkan`** = Vulkan backend implementation only
- **`crd-renderer`** = high-level rendering built on top of RHI

`crd-rhi` is intentionally narrow. It owns low-level GPU concepts such as
devices, swapchains, buffers, images, command buffers, shader modules, and
pipelines. It does **not** own materials, scene structures, ECS, lighting
logic, or other high-level rendering policy. Those are layered on top later
so the engine can reach a first working frame quickly and grow vertically.

The early strategy is incremental and vertical-slice driven: get a real GPU
device online, submit commands, build one pipeline, and draw the **first
triangle** before attempting a feature-rich renderer.

### 2.0 — `crd-rhi` interfaces (2–3 sessions)

Interface only. Minimal public concepts:

- `Instance` / `Device`
- `Swapchain`
- `Queue`
- `Buffer`
- `Image`
- `CommandBuffer`
- `ShaderModule`
- `Pipeline`

Notes:

- `crd-rhi` stays API-agnostic and must not leak Vulkan types.
- No materials, no render graph, no scene, no ECS, no lighting in this layer.
- Only the abstractions needed to get commands onto the GPU belong here.

### 2.1 — `crd-rhi-vulkan` bootstrap (3–4 sessions)

Backend implementation only:

- Vulkan instance creation
- physical device selection
- logical device + queues
- surface creation from `Window::native_handle()`
- swapchain creation and resize path

Goal: a valid Vulkan-backed RHI device with presentable images.

### 2.2 — Command buffers + frame synchronization (2–3 sessions)

- command pool strategy
- command buffer allocation / reset / reuse
- per-frame synchronization objects
- double/triple buffering policy
- present / acquire loop stability

Goal: submit empty or clear-only frames reliably, with correct resize and
shutdown behaviour.

### 2.3 — Pipeline + `ShaderModule` + FIRST TRIANGLE (3–5 sessions)

- minimal shader-module path
- minimal graphics pipeline creation
- vertex buffer upload path
- render pass / framebuffer wiring (or equivalent minimal pass model)
- **FIRST TRIANGLE** on screen

This is a hard milestone. Phase 2 does not move "up" into richer rendering
until a real triangle renders through the full RHI + Vulkan backend path.

### 2.4 — ImGui debug overlay integration (2–3 sessions)

ImGui is explicitly **not** part of RHI.

- ImGui integrates after the first triangle
- it lives as a debug layer (e.g. `ImGuiLayer`) on top of renderer/RHI
- retained-mode `crd-ui` is still a later phase and is not replaced by ImGui

### 2.5 — GPU memory + streaming (3–4 sessions)

- TLSF general-purpose allocator
- BuddyAllocator for fixed-size pools
- StreamingAllocator: virtual memory reservation + page commit/decommit
- backend memory allocation strategy for buffers and images
- frame fence-based deferred deletion

### 2.6 — Shader system (5–7 sessions)

Three layers:

- **Authoring:** GLSL via glslang → SPIR-V. No custom DSL. Compute path
  separate from raster.
- **Compilation pipeline:** `foo.glsl → SPIR-V (binary cached under
  <exe>/cache/shaders/) → reflection (SPIRV-Cross): inputs/outputs,
  descriptor bindings, push constant ranges, specialization constants`.
  Cache key = source hash + flags.
- **`ShaderProgram` + `MaterialTemplate` + `Material`:**
  These are no longer part of the early RHI bootstrap. They land only after
  the triangle milestone and sit above the low-level RHI substrate.
  - `ShaderProgram` owns VS+FS modules, descriptor set layouts,
    pipeline layout, reflection data.
  - `MaterialTemplate` = ShaderProgram + default `PipelineState`
    (blend / depth / cull).
  - `Material` = template instance + parameter overrides. Parameters
    are **named in the public API** (string), **indexed in dispatch**
    (cached `string→index` map from reflection).
- **Hot-reload:** file watcher (lands here in 2.2, not earlier).
  Source change → recompile → ShaderProgram rebuild → bound Materials
  invalidated. Vulkan's `PipelineCache` is reused.
- **RenderPass validation:** ShaderProgram's reflected I/O is matched
  against the pass it's bound to at init time; mismatch is a fatal log
  + assert.

### 2.7 — `crd-jobs` + `crd-resources` + asset_cooker (3–4 sessions)

- **`crd-jobs`** pulled forward from 2.5: thread pool, fiber-free
  task graph, per-frame allocator. Gates async asset I/O and parallel
  command recording.
- **`crd-resources`:** async load, LRU eviction, refcounted handles.
  Reads runtime binary formats only.
- **`crd-tools/asset_cooker`** (separate executable, NOT linked into
  runtime): glTF / PNG / HDR / WAV → Cerid binary formats
  (`.crd_mesh`, `.crd_tex` (BC-compressed), `.crd_envmap` etc.).
  Runtime never imports source assets. Editor will eventually drive
  the cooker.

### 2.8 — `crd-renderer` first scene (2–3 sessions)

`crd-renderer` is where high-level rendering policy begins.

- Camera (FPS controller + orbit camera)
- Mesh + Material + Transform = minimal `Renderable` list
- Forward renderer (single-pass, simple light setup)
- Skybox (cubemap sampling, infinite-plane trick)

Scene / ECS are still intentionally absent here. The renderer starts from a
simple explicit renderable list rather than waiting for a full scene stack.

### 2.9 — `crd-renderer` PBR + lighting (4–5 sessions)

- Punctual lights: point, spot, directional.
- Cook-Torrance BRDF.
- IBL: HDR cubemap → prefiltered radiance + irradiance + BRDF LUT.
- CSM (cascaded shadow maps) for directional lights.

### 2.10 — Post-FX (3–4 sessions)

- HDR pipeline + ACES tonemap.
- Bloom (downsample / upsample chain).
- TAA (temporal anti-aliasing).
- SSAO / SSR are deferred — not in this slice.

Estimate: 25–35 sessions across 2.0 → 2.10.

---

## Phase 3 — Scripting

Hot-reload C++ via DLLs, C API boundary, plugin system for third-party
extensions. Reuses the file watcher + DynamicLibrary helpers shipped in
Phases 1 / 2.

## Phase 4 — Game UI System

`crd-ui` on top of `crd-renderer`. Retained mode, layout, animation.
ImGui demoted to debug overlay only.

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

### 2026-04 — Platform v1b shipped (timer + frame clock)

- **`Timer` and `FrameClock` are built on `std::chrono::steady_clock`**, not
  `glfwGetTime()`. Tying engine timing to the windowing backend would force
  every measurement to wait for a context to exist. steady_clock is
  monotonic, never jumps, and works without any backend.
- **`FrameClock`'s first `tick()` reports zero delta**, not "time since
  construction". Otherwise the first frame would always look like a hitch
  whose magnitude is engine startup time. Subsequent ticks are real
  inter-tick deltas.
- **`FrameClock::total_seconds()` is measured from construction**, not from
  the first tick. This gives a single stable origin for things like log
  timestamps and replays.
- **`reset()` zeroes everything** — frame count, delta, total — and
  re-seeds, so the next tick is again a "zero-delta seed". This is the
  clean way to handle pause/unpause boundaries.
- **No tests need a Window.** Timing tests use `std::this_thread::sleep_for`
  with conservative thresholds (typical: assert >=2ms after a 3ms sleep)
  to stay deterministic on busy CI runners.
- Quality pass at session end:
  - `win-debug`: 167/167 tests pass (8 new timer tests)
  - `win-release`: 166/166 (Debug-only stats test correctly skipped)
  - `win-asan`: 167/167, no leaks, no UAF
  - `smoke_frame_clock` runtime example prints a clean 5-frame loop

### 2026-04 — Platform v1c shipped (input)

- **Hybrid input model.** Frame-coherent `InputState` snapshot is always
  there; the ordered event queue is opt-in. Most game code reads
  `is_key_down(Key::W)`-style polls; code that needs ordered presses
  (text edit, debug overlay) calls `enable_event_queue(N)` once and
  drains with `try_pop_event()`.
- **`Key` / `MouseButton` are Cerid-owned indices, not GLFW codes.**
  Backend translation lives in `window.cpp` only. Adding a key is a
  one-line enum append plus one table entry.
- **`Input` lives inside `Window`'s Impl.** GLFW user-pointer points at
  the `Input` directly; callbacks dispatch with one indirection.
- **`Window` move-assign re-binds the GLFW user pointer** after adopting
  the moved-from Impl. Move ctor doesn't need this because heap-stored
  Impl keeps its address; assignment swaps Impl objects so the pointer
  must follow.
- **First mouse-move seeds without spurious delta.** Same pattern as
  FrameClock's first-tick zero-delta seed: avoid synthesizing motion
  from default-init coordinates.
- **`KeyDown` while already-down does not re-fire `was_key_pressed`.**
  `pressed`/`released` are transitions, not held-state alternates.
- **Edge state (pressed/released, mouse delta, scroll delta) clears in
  `Input::on_poll_begin()`.** `Window::poll_input()` calls this at the
  start of every frame. The contract: call `context.poll_events()` first,
  then `window.poll_input()`, then read state.
- **Events without an enabled queue are silently dropped.** State is
  always updated regardless. The queue is purely additive.
- **No `Event` base type, no propagation, no consumption.** Hardware-only
  POD union. Layer/event router is the future `crd-app` module's job.
- **No gamepad in this slice.** Real gamepad work needs SDL3 / XInput.
- Quality pass at session end:
  - `win-debug`: 179/179 (12 new input tests)
  - `win-release`: 178/178 (Debug-only stats test correctly skipped)
  - `win-asan`: 179/179, no leaks (OptionalQueue heap alloc/free clean)
  - `smoke_window` now closes via ESC through the polling API

### 2026-04 — Phase 1.5 / Phase 2 strategy locked

A planning session at the end of platform v1c set the direction for
everything between here and the first rendered scene. Decisions:

**Platform v1d scope (next session):**

- **Filesystem implementation: `<filesystem>` standard library**, not
  raw Win32+POSIX. Cuts implementation time, gets us a battle-tested
  base.
- **`Path` is our own `String`-backed type**, not a re-exposed
  `std::filesystem::path`. Wide-char issues on Windows, UTF-8 SSO
  consistency in our `String`, and a single-place backend swap (e.g.
  Android SAF later) all push us toward owning the type.
- **Threading helpers stay tiny:** `set_current_thread_name`,
  `current_thread_id`, `hardware_concurrency`, `logical_core_count`,
  `physical_core_count`, `set_thread_affinity`, `cpu_pause`. No
  Mutex/CondVar wrappers (std is enough), no thread pool (that's the
  job system's job in 2.3).
- **No file watcher in v1d.** Hot-reload arrives in shader system
  (Phase 2.2) where it has a real consumer.

**`crd-app` (Phase 1.5) shape:**

- **Hazel-style virtual `Event` hierarchy is confirmed.** RTTI-free
  via static `EventType`. We deliberately picked the polymorphic shape
  over a tagged-union variant because editor / scripting / UI will
  benefit from real polymorphism. Stack-allocated, dispatcher takes a
  reference, no heap traffic.
- **Layer ownership: `unique_ptr<Layer>` to Application.** User code
  holds raw pointers if it needs callbacks back into a specific layer.
- **Lands BEFORE graphics**, not after. Phase 1.5, not "deferred until
  Phase 4 like the original plan." Reason: the main loop needs an
  owner; Phase 2's first-triangle milestone wants a real `RenderLayer`
  hook.

**Phase 2 graphics strategy:**

- **Job system (`crd-jobs`) pulled forward from 2.5 to 2.3.** Async
  asset I/O, parallel GPU command recording, and parallel shader
  compilation all need it. Writing 2.3 / 2.4 single-threaded and
  rewriting later is wasted effort.
- **RHI is multi-backend from day one in INTERFACE shape**, even though
  only Vulkan is implemented. Headers are designed assuming Metal /
  D3D12 will be filled in someday; this disciplines the abstraction.
  No abstraction-purity exercises just to satisfy the rule.
- **Asset pipeline is a separate executable**
  (`crd-tools/asset_cooker`). Runtime only reads Cerid binary
  formats. glTF / PNG / HDR never enter runtime code. Editor (Phase 5)
  drives the cooker eventually.
- **Reference-counting split is intentional.** If Cerid needs generic
  shared-lifetime primitives, they land in `crd-memory` as intrusive
  ref-counting (`RefCounted`, `IntrusivePtr<T>`, later maybe atomic
  variants). Resource-facing shared references, eviction, lazy loading,
  and hot-reload ownership semantics belong in `crd-resources`, not in
  the generic memory layer.
- **First-triangle gate.** Phase 2.0 cannot be declared shipped until
  the screen shows a triangle. Prevents the "endless RHI refactor" trap.
- **Shader system gets its own slice (2.2)**, not folded into 2.0.
  GLSL → glslang → SPIR-V → SPIRV-Cross reflection → on-disk cache →
  ShaderProgram → MaterialTemplate → Material, with hot-reload at the
  source-file level. Material parameters: named in API, indexed in
  dispatch (cached map from reflection).

### 2026-04 — Graphics module split refined (`crd-rhi` / Vulkan backend / renderer)

- **`crd-rhi` is now the explicit low-level graphics module.** It replaces
  the older implicit "graphics foundation" idea and owns only minimal GPU
  abstraction: device, swapchain, queue, buffers, images, command buffers,
  shader modules, and pipelines.
- **`crd-rhi-vulkan` is a separate backend module.** Vulkan is the first and
  only backend initially, but Vulkan types must not leak through the public
  `crd-rhi` surface.
- **High-level rendering moves out of the low-level layer.** Materials,
  cameras, renderables, lighting, and scene-facing rendering logic belong in
  `crd-renderer` (plus `crd-resources`), not in `crd-rhi`.
- **Vertical slice first, renderer later.** The first hard milestone is:
  Vulkan device + swapchain + command buffers + pipeline + first triangle.
  No attempt is made to build a rich renderer before that path is proven.
- **ImGui is not part of RHI.** It integrates only after the triangle
  milestone as a debug layer on top of renderer/RHI, while the long-term UI
  plan remains `crd-ui`.

### 2026-04 — `crd-rhi` v1a shipped (minimal interface scaffold)

- **`crd-rhi` now exists as a real module.** v1a intentionally ships only
  the API-agnostic types, descriptors, and abstract interfaces needed to
  describe the first-triangle path.
- **No Vulkan types leak through the public surface.** The backend module
  (`crd-rhi-vulkan`) will implement these interfaces later, but the v1a
  headers are clean and backend-neutral.
- **The abstraction remains intentionally narrow.** No materials, no scene,
  no ECS, no lighting, no renderer policy. Just the low-level concepts:
  `Instance`, `Device`, `Queue`, `Swapchain`, `Buffer`, `Image`,
  `CommandBuffer`, `ShaderModule`, and `Pipeline`.
- **Tests use a fake backend, not a real GPU.** That is deliberate: v1a
  validates ergonomics, ownership, descriptor flow, and first-triangle API
  shape before any Vulkan implementation exists.
- Quality pass at session end:
  - `win-debug`: 200/200 tests pass
  - `win-release`: 199/199 (Debug-only stats test correctly skipped)
  - `win-asan`: 200/200, no leaks, no UAF, no OOB

### 2026-04 — `crd-rhi-vulkan` bootstrap shipped

- **Vulkan is now connected to the real platform window path.** The backend
  creates a Vulkan instance, enumerates adapters, creates a logical device,
  creates a surface from `Window::native_handle()`, and bootstraps a
  swapchain.
- **Public `crd-rhi` stayed clean.** The backend entrypoint returns an
  `rhi::Instance`; no `Vk*` type leaks through the public headers.
- **Bootstrap is intentionally narrower than a rendering backend.** Real
  command-buffer recording, synchronization, shader modules, pipelines,
  and buffer/image allocation policy remain later slices.
- **Release-safety mattered here.** The initial assert-only Vulkan path was
  tightened into runtime-checked control flow so failure cases return
  `nullptr` / empty results instead of relying on debug-only asserts.
- Quality pass at session end:
  - `win-debug`: 202/202 tests pass
  - `win-release`: 201/201 (Debug-only stats test correctly skipped)
  - `win-asan`: 202/202, no leaks, no UAF, no OOB

### 2026-04 — `crd-app` shipped (layer stack + propagated events + sync bus)

- **`crd-app` is not a pure Hazel clone.** Cerid keeps Hazel-style
  propagated event dispatch for input/window/application routing through
  the `LayerStack`, but adds a separate typed `EventBus` for broadcast-
  style notifications.
- **Propagated events vs bus events are distinct concepts.**
  `handled` semantics exist only on the propagated side. Bus events are
  broadcast and never participate in consumption.
- **The bus is sync in v1.** This is intentional. The API is typed-template
  (`subscribe<T>(fn)`, `publish(event)`), and the roadmap explicitly keeps
  async evolution open for later if cross-thread producers become real.
- **Applications can define their own event classes without patching the
  engine.** Event identity is based on per-type static tokens, not a fixed
  global enum registry.
- **`Application` is NOT a singleton.** Ownership stays explicit and testable.
- **`LayerStack` remains application-composition machinery, not a catch-all
  engine architecture rule.** Resources, renderer internals, and other core
  systems do not need to become layers.
- Quality pass at session end:
  - `win-debug`: 197/197 tests pass
  - `win-release`: 196/196 (Debug-only stats test correctly skipped)
  - `win-asan`: 197/197, no leaks, no UAF, no OOB
  - tests prove custom app-defined events work with the shipped bus

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

**Last session:** 2026-04-28 — `crd-rhi-vulkan` frame execution slice.
See `docs/sessions/2026-04-28-rhi-vulkan-frame-sync.md`.

What landed:

1. **`crd-rhi-vulkan` now has a real per-frame execution path** on top of the
   already-landed bootstrap: command pool, command buffers, frame sync,
   acquire, submit, and present.
2. **A clear-only frame can now be recorded and submitted** through the real
   backend; first triangle is the next vertical slice, not the first time a
   command buffer touches the GPU.
3. **Dynamic rendering is already the chosen minimal rendering path.** That
   keeps the backend aligned with the intended lightweight triangle milestone.
4. **Pragmatism won over forced modernity for this exact slice.** sync2 support
   is detected, but the shipped execution path uses classic submit/barrier
   flow because it is currently the most stable path across release testing.
5. **The verification matrix stayed green after introducing the execution
   path:**
   - Debug: `203/203`
   - Release: `202/202`
   - ASan: `203/203`

Current test counts:

- Debug: `203/203`
- Release: `202/202` (Debug-only stats test correctly skipped)
- ASan: `203/203`

**Next session starts with: pipelines + shader modules + first triangle.**

One small optional cleanup from closeout still exists:

1. Fix or replace the disabled-trace benchmark so the release bench suite is
   fully green again.
2. Refresh `docs/bench/baseline_2026-04.md` only after that microbenchmark is
   stable.

The concrete short-path from here is now:

1. minimal shader-module path
2. minimal graphics pipeline creation
3. vertex buffer upload path
4. first triangle through dynamic rendering
5. then GPU allocation strategy and shader path growth

Roughly 1–2 sessions from here to "first triangle on screen": one small
benchmark cleanup if desired, then the pipeline/shader/triangle milestone.
