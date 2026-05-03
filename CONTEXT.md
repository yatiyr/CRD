# Cerid — Live Context

> Engine'in kısa-vadeli hafızası. "Şu an neredeyiz?" sorusuna cevap verir.
> "Master plan ne?" sorusunun cevabı `docs/ROADMAP.md` ve oradan
> dallanan dosyalardadır.
>
> Her session sonu `@docs-keeper` günceller. Kısa kalır. Eski session
> detayları `docs/sessions/YYYY-MM-DD-*.md`'de yaşar, burada değil.

---

## Current focus

**Phase 2.6 — `crd-resources` + asset cooker. v1a SHIPPED.**

v1a shipped: `ResourceId` (UUID v4/v5), CRDR chunked binary container, `ManifestEntry` (48-byte
disk format), `ResourceManager` shell (register + mount + unmount + collision), `manifest_dump`
CLI sub-command. 38 new tests. String SSO regression (new MSVC 14.50.35717) fixed simultaneously.

Next: v1b (cooker cook subcommand + zstd), then v1c (`RefCounted<T>` + `ResourceHandle<T>`).

Full design packet: `docs/phases/phase-2.6-resources.md`.

Aktif phase dosyası: `docs/phases/phase-2.6-resources.md` (active)

## Active detour

_none — running on the main roadmap._

> When a detour opens, this section names it (e.g. "D-001: investigate
> shader-cache corruption") and the main roadmap pauses until it closes.
> Detour file: `docs/detours/D-NNN-<slug>.md`. Queue rules:
> `docs/detours/README.md`.

## Last shipped milestone

**2026-05-03 — Phase 2.6 v1a shipped: `crd-resources` + `asset_cooker` manifest_dump.**

`ResourceId` (UUID v4 via mt19937_64, UUID v5 via SHA-1 SHA-1 + Cerid namespace, parse/to_string,
36-char hyphenated format). CRDR chunked binary container (reader + writer, chunk sort, 16-byte
padding, LE serialization). `ManifestEntry` 48-byte disk format (MFST/STRP/DEPS chunks).
`ResourceManager` shell: `register_loader`, `mount_manifest` (reads CRDR PACK, populates live
index, newest-mount-wins collision), `unmount` (by MountId). `asset_cooker manifest_dump` CLI
sub-command. 38 new tests across three test files.

Also fixed: `crd-containers String` SSO encoding changed to remaining-capacity (`size_or_flag =
kSsoCapacity - size`) to eliminate `buf[kSsoCapacity]` UB exposed by new MSVC 14.50.35717
optimizer. A 23-char SSO string now has `size_or_flag = 0 = '\\0'` which doubles as the null
terminator, so `c_str()` is always correct and no out-of-bounds array access occurs.

Six-configuration green:
- win-debug:          393/393
- win-relwithdebinfo: 393/393
- win-release:        390/390
- win-asan:           393/393
- win-clang-cl:       393/393
- win-tidy:           393/393

## Previous shipped milestone

**2026-05-02 — `crd-jobs` v1k integration smoke + crd-app wiring shipped. Phase 2.5 COMPLETE.**

`smoke_jobs.cpp` rewritten from raw fiber demo (v1a) to full public API exercise: `init/shutdown`,
`run+wait`, `parallel_for` (1 000-element sum), H/N/L priority all-ran (10/20/40 jobs),
`frame_alloc/frame_reset`. All sections PASS, exit 0.

`Application::run()` now calls `crd::jobs::init(m_desc.jobs_config)` before the tick loop and
`crd::jobs::shutdown()` after, guarded by `if (!m_valid) return`. `ApplicationDesc` gained
`crd::jobs::Config jobs_config{}`. `crd-app` CMakeLists links `crd-jobs` PUBLIC.
`smoke_renderer` verified clean (exit 0) with the wired Application.

Six-configuration green:
- win-debug:          355/355
- win-relwithdebinfo: 355/355
- win-release:        352/352
- win-asan:           355/355
- win-clang-cl:       355/355
- win-tidy:           355/355 (exit 0)

## Previous shipped milestone

**2026-05-02 — `crd-jobs` v1h Public API layer shipped.**

`engine/jobs/include/crd/jobs/jobs.hpp` — full public API: `Config`, `init()`, `shutdown()`,
`run(span)` / `run(single)`, `wait()`, `run_and_wait(span)` / `run_and_wait(single)`,
`is_worker_fiber()`, `worker_index()`, `num_workers()`.

Key design decision: counter pointer stored in `Fiber::job_counter` (not TLS) so it survives
fiber suspension. 5 public-API tests. Total at v1h: 346 tests.

## Previous shipped milestone

**2026-05-02 — `crd-jobs` v1g Worker thread pool + main-thread fiber shipped.**

`WorkerPool` class in `engine/jobs/src/worker_pool.hpp/.cpp`: owns `Scheduler`, `FiberPool`, and
`CounterPool`; spawns N-1 background worker threads (indices 1..N-1) each running `worker_loop`;
thread 0 driven by `pump()` for main-thread use. All jobs run inside fiber context switches via
`job_fiber_trampoline` (looping entry fn burned into every fiber stack at pool init).

Key fix: `fiber_switch_win64.asm` now saves/restores GS:[8] (StackBase) and GS:[16] (StackLimit)
alongside RSP, so `__chkstk` and guard pages work correctly when switching between fiber stacks.
`FiberContext` extended with `tib_stack_base` / `tib_stack_limit` on Windows. `fiber_context.hpp`
now explicitly includes `platform.hpp` (was relying on PCH ordering).

Key fix: fiber reuse was broken — snapshot copy `target->context = target->initial_ctx` was
corrupted because `fiber_switch`'s register saves (push r14...) overwrite the initial frame data
on the fiber stack. Fix: call `fiber_init_stack` on completion to rebuild a fresh initial frame.
`Fiber` struct: `initial_ctx` field removed; `usable_base`, `usable_size`, `trampoline` fields
added so `WorkerPool` can re-initialize without pool context.

10 unit tests in `tests/jobs/test_jobs.cpp`: init/shutdown, re-init, default thread count,
single worker job, pump on thread 0, pinned job via pump, multiple jobs, fiber stack isolation,
pump empty probe, 1000-job concurrent stress. Also fixed pre-existing clang-tidy warnings in
`test_scheduler.cpp` (u→U suffix, member m_ prefix, braces-around-statements).

Six-configuration green:
- win-debug:          341/341
- win-relwithdebinfo: 341/341
- win-release:        338/338
- win-asan:           341/341
- win-clang-cl:       341/341
- win-tidy:           341/341

## Previous shipped milestone

**2026-05-02 — `crd-jobs` v1f Counter + wait mechanism shipped.**

`Counter` (alignas(64), one cache line: `atomic<u32> value`, `atomic<Waiter*> waiters`, pool
metadata) + `CounterPool` (generation-tagged 64-bit Treiber free list `[gen:32|idx:32]`, same
pattern as FiberPool). `Waiter` struct: `Fiber* fiber`, `u32 target`, `atomic<bool> canceled`,
`atomic<Waiter*> next`. `counter_decrement` steals waiter list atomically; `counter_wait` is a
6-step ABA-safe protocol (fast path, Treiber push, double-check, fiber_switch suspend). 14 unit
tests incl. real fiber_switch suspension + 16-thread concurrent stress.

Six-configuration green:
- win-debug:          331/331
- win-relwithdebinfo: 331/331
- win-release:        328/328
- win-asan:           331/331
- win-clang-cl:       331/331
- win-tidy:           331/331

## Previous shipped milestone (–2)

**2026-05-02 — `crd-jobs` v1e Priority Scheduler shipped.**

`Scheduler` class: three global Vyukov MPMC injection queues (High/Normal/Low), per-thread three
Chase-Lev local deques, and a single-slot pinned-job lane per thread. Drain order: pinned →
H-inject → H-local → H-steal → N-inject → N-local → N-steal → L-inject → L-local → L-steal.
`std::counting_semaphore<>` sleep/wake. 16 unit tests.

Detail: `docs/sessions/2026-05-02-jobs-v1e-scheduler.md`.

Six-configuration green:
- win-debug:          317/317  
- win-relwithdebinfo: 317/317  
- win-release:        314/314  
- win-asan:           317/317  
- win-clang-cl:       317/317  
- win-tidy:           317/317  

## Previous shipped milestone (–2)

**2026-05-02 — `crd-jobs` v1d Vyukov MPMC injection queue shipped.**

`MpmcQueue<T>` header-only template implementing the Vyukov bounded MPMC queue algorithm
(Dmitry Vyukov, 1024cores.net). Producers and consumers each have their own `alignas(64)`
atomic position counter (separate cache lines) to prevent false sharing. Each ring-buffer
cell holds an `atomic<u64>` sequence and one T. The sequence handshake uses `acquire`/`release`
orderings: producers read sequence with acquire and publish with release; consumers mirror
this. enqueue() returns false (non-blocking) when full; dequeue() returns false when empty.
Capacity must be a power of two.

10 unit tests: construction, single-item round-trip, empty returns false, full returns false,
FIFO ordering, wrap-around across 3 full laps, SPSC/MPSC/SPMC/MPMC concurrent stress
(each verifying all items consumed exactly once).

Six-configuration green:
- win-debug:          301/301
- win-relwithdebinfo: 301/301
- win-release:        298/298
- win-asan:           301/301
- win-clang-cl:       301/301
- win-tidy:           301/301

## Previous shipped milestone (–2)

**2026-05-02 — `crd-jobs` v1c Chase-Lev work-stealing deque shipped.**

`WorkStealingDeque<T>` header-only template implementing the Lê et al. 2013
algorithm ("Correct and Efficient Work-Stealing for Weak Memory Models").
Owner thread uses push() (LIFO via `m_bottom`) and pop(); any thread calls
steal() (FIFO via `m_top`). Fixed power-of-two capacity; indices are `i64`
monotonically increasing counters masked with `& (capacity-1)`.

Memory ordering: push uses `release` on `m_bottom`; pop uses `seq_cst` on
`m_bottom`-store + `seq_cst` on `m_top`-load + `seq_cst` CAS for the last-
element race; steal uses `acquire` load of `m_top`, `seq_cst` fence (required
for correctness on weak memory models per Lê et al. Thm 1), `acquire` load
of `m_bottom`, then `seq_cst` CAS.

`m_bottom` and `m_top` on separate `alignas(64)` cache lines to prevent
false sharing between owner and thieves. `CRD_COMPILER_MSVC` pragma suppresses
C4324 (structure padded due to alignment specifier).

12 tests: LIFO/FIFO ordering, exhaustion assertion, last-element race
(4 000 trials, each must give exactly one winner), two concurrent stress tests
(pre-fill + concurrent drain; concurrent push + pop + steal with back-pressure).

Six-configuration green:
- win-debug:          291/291
- win-relwithdebinfo: 291/291
- win-release:        288/288
- win-asan:           291/291
- win-clang-cl:       291/291
- win-tidy:           291/291

## Previous shipped milestone

**2026-05-02 — `crd-jobs` v1b fiber pool shipped.**

Three-tier fiber pool (Small 64 KB × 128, Medium 512 KB × 64, Large 2 MB × 16).
Platform stack allocation: VirtualAlloc (Windows) / mmap+mprotect (Linux) with
an uncommitted guard page below each stack — overflow crashes immediately.
Lock-free Treiber free list per tier using a tagged 64-bit head
`[gen:32 | idx:32]`; generation is bumped on every pop, making ABA structurally
impossible without CMPXCHG16B. `alignas(64)` on `Tier` struct prevents inter-tier
false sharing. Debug-only explicit state machine: `Idle / Active / Waiting / Ready`
with asserted transitions at acquire/release (Waiting/Ready stubs ready for v1f).
Peak-usage watermark tracked per tier for profiling. Trampoline is injected at
pool creation (looping, zero re-init cost on the hot acquire path).
13 new tests; concurrent 4-thread × 8 000-iteration ABA stress test included.

Also fixed in this session: renderer LTO miscompilation under win-relwithdebinfo
(`FrameGraph::build()` local-array tracking moved to `ImageResource` members;
`ForwardRenderPath` lambda captures changed from `&draw_list` to `m_draw_list`
via new member pointer — both were MSVC `/GL` miscompile sites).

Six-configuration green for the first time:
- win-debug:          279/279
- win-relwithdebinfo: 279/279
- win-release:        276/276
- win-asan:           279/279
- win-clang-cl:       279/279
- win-tidy:           279/279

## Previous shipped milestone

**2026-05-01 — `crd-jobs` v1a hand-rolled asm context switch shipped.**

`fiber_switch` in Windows x64 MASM and Linux x86-64 AT&T assembly: saves/restores
all callee-saved registers mandated by each ABI (Windows: RBX RBP RDI RSI R12–R15
XMM6–XMM15 MXCSR FCW; Linux: RBX RBP R12–R15). `fiber_init_stack` in C++ sets up
the initial stack frame so the first `fiber_switch` to a fresh fiber jumps to the
entry function; a sentinel `fiber_abort` return-address catches runaway fibers.
5 unit tests: round-trip, multiple re-entries, stack-local data survives,
callee-saved registers verified, two independent fibers.

Detail: (session combined with v1b above).

## Previous shipped milestone (–2)

**2026-05-01 — `crd-renderer` v1i swapchain blit + first full frame loop shipped.**

`CommandBuffer::blit_image(src, dst, src_extent, dst_extent)` added to RHI interface
and implemented in `VulkanCommandBuffer` via `vkCmdBlitImage` with `VK_FILTER_LINEAR`.
Swapchain creation now sets `VK_IMAGE_USAGE_TRANSFER_DST_BIT`. `ForwardRenderPath`
color image adds `TransferSrc` usage. New free function `add_swapchain_blit_pass`
(in `crd/renderer/swapchain_blit.hpp`) adds two frame graph passes per frame:
"swapchain-blit" (ColorWrite→TransferSrc, Undefined→TransferDst, blit_image call) and
"present-barrier" (TransferDst→Present, empty execute). All fake `CommandBuffer`
implementations updated. 4 new unit tests. Smoke updated with end-to-end blit path.

Three-flavour green:
- win-debug:    261/261
- win-release:  260/260
- win-asan:     261/261

Detail: `docs/sessions/2026-05-01-renderer-v1i-swapchain-blit.md` (to be written).

## Previous shipped milestone (–3)

**2026-05-01 — `crd-renderer` v1g `ForwardRenderPath` shipped.**

First concrete `IRenderPath`: depth prepass (opaque items, depth-only) + main color pass
(opaque + masked, full shading). `PerFrameUbo` (288 bytes) at set 0, `PerDrawPush`
(model matrix, 64 bytes) as push constants. `ForwardRenderPath::create()` allocates
ring UBO + descriptor set per frame-in-flight. Render targets (B8G8R8A8Unorm color,
D32Sfloat depth) owned by path, recreated on `resize()`.

Vulkan backend hardened: `begin_rendering` now caller-managed transitions (no implicit
layout changes), color attachment optional (null = depth-only), null fragment shader
supported in `create_graphics_pipeline` (`colorAttachmentCount = 0`). Added `inverse(Mat4f)`
via Laplace cofactor expansion. 5 new unit tests.

Three-flavour green:
- win-debug:    253/253
- win-release:  252/252
- win-asan:     253/253

Smokes: `smoke_rhi_vulkan_bootstrap` (120 frames, clean), `smoke_renderer` (frame graph
transitions verified).

Detail: `docs/sessions/2026-05-01-renderer-v1g-forward-render-path.md`.

## Previous shipped milestone (–4)

**2026-05-01 — `crd-renderer` v1e+f merged: push constants + descriptor system + material binding shipped.**

Merged v1e + v1f into one slice. RHI surface: `push_constants()`, `bind_descriptor_sets()`,
`create_descriptor_set_layout()`, `create_pipeline_layout()`, `create_descriptor_allocator()`.
Vulkan backend: `VulkanDescriptorSetLayout`, `VulkanPipelineLayout`, `VulkanDescriptorSet`,
`VulkanDescriptorAllocator` (ring-buffer, `frames_in_flight` pools — see session doc).
`ShaderStage` promoted to bitmask (Vertex=1, Fragment=2, Compute=4). Explicit `PipelineLayout`
added to `GraphicsPipelineDesc` (optional, at end — no positional-init breakage). Renderer
material system: `MaterialLayout` + `MaterialInstance` wrapping the allocator-backed
descriptor set lifecycle. 10 new unit tests, 4 new material tests.

Three-flavour green:
- win-debug:    248/248
- win-release:  247/247
- win-asan:     248/248

Detail: `docs/sessions/2026-05-01-renderer-v1ef-descriptors.md`.

## Previous shipped milestone (–5)

**2026-05-01 — `crd-renderer` v1b real draw execution shipped.**

`crd-renderer` now has a real execution layer over the prepared draw items:
minimal pass orchestration, command buffer recording, pipeline resolution, and
draw-call submission into one rendering pass without taking ownership of native
pipeline objects.

Three-flavour green:
- Debug: 228/228
- Release: 227/227
- ASan: 228/228

Detail: `docs/sessions/2026-05-01-renderer-v1b-draw-execution.md`.

## Previous shipped milestone (–6)

**2026-05-01 — `crd-renderer` v1a explicit renderables shipped.**

The engine now has its first high-level renderer consumer over the completed
shader packet: camera, explicit renderable list, draw-item preparation, and a
clean frame-plan handoff without scene/ECS commitments.

Three-flavour green:
- Debug: 227/227
- Release: 226/226
- ASan: 227/227

Detail: `docs/sessions/2026-05-01-renderer-v1a-explicit-renderables.md`.

## Previous shipped milestone (–7+)

**2026-05-01 — `crd-shader` 2.3g pipeline handoff / descriptor growth shipped.**

`crd-shader` now produces a backend-neutral handoff surface describing compiled
module usage, normalized descriptor bindings, push-constant visibility, and
vertex-input requirements for a variant. This cleanly separates shader-owned
metadata from backend-owned pipeline objects.

Three-flavour green:
- Debug: 226/226
- Release: 225/225 (Debug-only Vulkan triangle integration test correctly skipped)
- ASan: 226/226

Detail: `docs/sessions/2026-05-01-shader-2.3g-pipeline-handoff.md`.

## Previous shipped milestone

**2026-04-30 — `crd-shader` 2.3f hot reload shipped.**

Successful reload now compiles and swaps atomically, failed reload keeps the
last-good live state, and reload observability is exposed through `ReloadEvent`
without crashing consumers or invalidating effect/variant identity.

Three-flavour green:
- Debug: 225/225
- Release: 224/224 (Debug-only Vulkan triangle integration test correctly skipped)
- ASan: 225/225

Detail: `docs/sessions/2026-04-30-shader-2.3f-hot-reload.md`.

## Previous shipped milestone

**2026-04-30 — `crd-shader` 2.3e cache hierarchy shipped.**

Source/preprocessed/SPIR-V cache keys are now explicit, local include graphs
participate in the key path, and the runtime now has both in-memory and on-disk
SPIR-V cache reuse. Compile diagnostics expose cache hit/miss behavior without
leaking backend types.

Three-flavour green:
- Debug: 223/223
- Release: 222/222 (Debug-only Vulkan triangle integration test correctly skipped)
- ASan: 223/223

Detail: `docs/sessions/2026-04-30-shader-2.3e-cache-hierarchy.md`.

## Previous shipped milestone

**2026-04-29 — `crd-shader` 2.3d variant key + mechanism policy shipped.**

Structural variant identity is now deterministic and typed. `VariantKey`
generation uses only structural axes, specialization values are excluded from
the structural key by design, and the hybrid mechanism policy is now encoded
as public helper decisions (`Permutation`, `SpecializationConstant`,
`ResourceBinding`, `DynamicBranch`).

Three-flavour green:
- Debug: 220/220
- Release: 219/219 (Debug-only Vulkan triangle integration test correctly skipped)
- ASan: 220/220

Detail: `docs/sessions/2026-04-29-shader-2.3d-variant-key.md`.

## Previous shipped milestone

**2026-04-29 — `crd-shader` 2.3c reflection consumption shipped.**

`spirv-reflect` now drives descriptor bindings, push-constant layout, vertex
input metadata, and material-parameter discovery from the canonical internal
SPIR-V modules. Reflection data is consumed into Cerid-owned effect/module
metadata with no public GLSL/SPIR-V/Vulkan leakage.

Three-flavour green:
- Debug: 216/216
- Release: 215/215 (Debug-only Vulkan triangle integration test correctly skipped)
- ASan: 216/216

Detail: `docs/sessions/2026-04-29-shader-2.3c-reflection.md`.

## Previous shipped milestone

**2026-04-29 — `crd-shader` 2.3b frontend → IR seam + GLSL ingest shipped.**

GLSL source file ingestion now compiles through a runtime-loaded `shaderc`
frontend into canonical internal SPIR-V modules, without leaking GLSL/SPIR-V/
Vulkan through the public API. Successful and failing compile paths are both
covered in tests.

Three-flavour green:
- Debug: 215/215
- Release: 214/214 (Debug-only Vulkan triangle integration test correctly skipped)
- ASan: 215/215

Detail: `docs/sessions/2026-04-29-shader-2.3b-glsl-ingest.md`.

## Previous shipped milestone

**2026-04-29 — `crd-shader` 2.3a public envelope shipped.**

Opaque handles, backend-neutral metadata types, minimal `Effect` / `Runtime`
interfaces, and an in-memory runtime seam proving effect/variant/reload
observability without leaking GLSL/SPIR-V/Vulkan through the public API.

Three-flavour green:
- Debug: 214/214
- Release: 213/213 (Debug-only Vulkan triangle integration test correctly skipped)
- ASan: 214/214

Detail: `docs/sessions/2026-04-29-shader-2.3a-envelope.md`.

## Previous shipped milestone

**2026-04-29 — GPU memory + streaming foundation shipped.**

Centralized Vulkan allocator helper, backend-owned buffer/image allocation,
real image creation path, and allocator-backed smoke/test coverage. This is not
the final allocator architecture, but it stabilizes resource ownership before
shader/renderer growth.

Three-flavour green:
- Debug: 210/210
- Release: 209/209 (Debug-only stats test correctly skipped)
- ASan: 210/210

Detail: `docs/sessions/2026-04-29-gpu-memory-streaming-foundation.md`.

## Previous shipped milestone

**2026-04-28 — ImGui debug overlay shipped.**

Debug-only Dear ImGui layer over `crd-app` + `crd-rhi-vulkan`, configured via
`crd-config`. Docking on by default, multi-viewport off by default, theme and
panel visibility from `runtime/configs/imgui_layer.toml`, and a real
triangle+overlay smoke.

Three-flavour green:
- Debug: 210/210
- Release: 209/209 (Debug-only stats test correctly skipped)
- ASan: 210/210

Detail: `docs/sessions/2026-04-28-imgui-debug-overlay.md`.

## Previous shipped milestone

**2026-04-28 — `crd-config` core shipped.**

Typed TOML wrapper over `toml++` with non-fatal schema-with-defaults behavior:
typed `get<T>(key, fallback)`, typed `set<T>(key, value)`, parse errors via
`g_log_config`, dot-path nested lookup, sample TOML, and `smoke_config`.

Three-flavour green:
- Debug: 209/209
- Release: 208/208 (Debug-only stats test correctly skipped)
- ASan: 209/209

Detail: `docs/sessions/2026-04-28-config-core.md`.

## Previous shipped milestone

**2026-04-28 — First triangle on screen.**

Full RHI/Vulkan path real: instance → device → swapchain → command buffer →
shader modules → graphics pipeline → vertex buffer → draw → present.
Dynamic rendering chosen as the minimal path. Triangle stayed narrow: no
descriptors, materials, scene graph, camera system, or allocator policy
creep.

Three-flavour green:
- Debug: 203/203
- Release: 202/202 (Debug-only stats test correctly skipped)
- ASan: 203/203

Detail: `docs/sessions/2026-04-28-rhi-vulkan-first-triangle.md`.

## Next up (next 1–3 sessions)

1. **Phase 2.6 v1b** — `tools/asset_cooker/` cook subcommand, blob passthrough cook handler, CMake `cook` target, `cook.log.toml` determinism, zstd compression wired.
2. **Phase 2.6 v1c** — `crd::memory::RefCounted<T>` (ADR-0014 prerequisite) → `ResourceHandle<T>` + `ILoader` + `load_sync<T>` + transitive dependency resolution.
3. **Phase 2.6 v1d** — Async I/O via `crd-platform` `AsyncFile`; `load_async<T>`.

## Roadmap ordering (post-jobs)

- **Phase 2.6** — `crd-resources` + asset cooker. Seven slices v1a–v1g: binary manifest +
  ResourceId (UUID hybrid), cooker CLI, sync handles + loaders, async via `crd-platform`
  `AsyncFile`, shader + material loaders end-to-end, hot-reload, streaming + 2Q eviction.
  Architecture: ADRs 0036–0041. Loader-registry pattern keeps `crd-resources` LOW in the
  dependency graph (no `crd-rhi`/`crd-shader`/`crd-renderer` deps).
- **Phase 3.0** — `crd-scene` / ECS (hybrid hierarchy + SoA components + TOML → binary
  serialization). **Ships before physics** — physics integration requires a scene to sync
  transforms into. Plugs into `crd-resources` as a `SceneLoader`.
- **Phase 3.1** — Physics (PhysX 5 backend + scene integration + fixed-step + deterministic mode).
- **Phase 4.0** — C++ hot-reload DLL scripting (ADR-0034). Cooker handler plug-ins ride this same DLL substrate.
- **Phase 4.2** — Networking: transport layer → deterministic simulation → client-server sync (ADR-0035).
- **Phase 8** — Domain modules: robotics, aerospace, cinematic, procedural generation — after Phase 4 + editor foundations.

Full plan: `docs/ROADMAP.md` → `docs/phases/`.

## Open questions

- `crd-config` hot-reload remains 1.6b unless ImGui integration proves it
  should move earlier.
- Runtime scene binary format — FlatBuffers vs Cap'n Proto? Park for
  Phase 3.1c.

## Test counts (last quality pass)

- win-debug:          393/393
- win-relwithdebinfo: 393/393
- win-release:        390/390
- win-asan:           393/393
- win-clang-cl:       393/393
- win-tidy:           393/393

(win-release is 3 fewer: debug-only `FiberState` tests excluded by `#if CRD_ENABLE_ASSERTS`)

## Pointers (lazy-load reference)

Agents: don't read everything. Use these breadcrumbs.

- **Hub:** `docs/ROADMAP.md` (small navigation page; safe to read fully)
- **Principles:** `docs/PRINCIPLES.md` (read every session, short)
- **Active phase only:** `docs/phases/phase-2.6-resources.md` (active) and `docs/phases/phase-2.5-jobs.md` (reference; complete)
- **Other phases:** `docs/phases/phase-<X>.md` (read ONLY when relevant)
- **Specific decision:** `docs/decisions/<NNNN>-<slug>.md` (find via
  `docs/decisions/README.md` tag index)
- **Last session detail:** the single file linked above, not the whole
  `docs/sessions/` folder
- **Module overview:** `docs/systems/<module>.md` (when working on that
  module)
- **Module deep-dive:** `docs/<module>/<MODULE>_FILE.md` (only when doing
  surgery)
- **Open debt:** `docs/debt.md`
- **Detour queue + rules:** `docs/detours/README.md`

When in doubt, ASK before reading large files.

## Session log (rolling, last 5)

- **2026-05-03** — Phase 2.6 v1a shipped: `crd-resources` (ResourceId, CRDR, ResourceManager shell) + `asset_cooker manifest_dump`; String SSO remaining-capacity fix; all 6 configs green (393/393).
- **2026-05-03** — Phase 2.6 architecture designed and documented; ADRs 0036–0041 written; `phase-2.6-resources.md` created; `crd-resources` placement, ResourceId UUID scheme, CRDR container format, ResourceHandle semantics, cooker CLI + CMake, and `crd-platform` async I/O all locked. No code shipped this session.
- **2026-05-02** — `crd-jobs` v1k integration smoke + crd-app wiring shipped; Phase 2.5 COMPLETE; all 6 configs green (355/355 win-debug).
- **2026-05-02** — `crd-jobs` v1j per-thread frame allocator shipped; 4 new tests; all 6 configs green (355/355 win-debug).
- **2026-05-02** — `crd-jobs` v1i SBO lambda helpers shipped; `make_job<F>()` + `parallel_for()`; 5 new tests; all 6 configs green (351/351 win-debug).
- **2026-05-02** — `crd-jobs` v1h Public API shipped; `jobs.cpp` + 5 new public-API tests; `Fiber::job_counter` field (fiber-survives-suspension counter fix); all 6 configs green (346/346 win-debug).
- **2026-05-02** — `crd-jobs` v1g Worker thread pool + main-thread fiber shipped; 10 new tests; TIB save/restore fix; fiber re-init fix; all 6 configs green (341/341 win-debug).
- **2026-05-02** — `crd-jobs` v1f Counter + wait mechanism shipped; 14 new tests; NDEBUG fix for Release; all 6 configs green (331/331 win-debug).
- **2026-05-02** — `crd-jobs` v1e Priority Scheduler shipped; 16 new tests; /EHsc fix; all 6 configs green (317/317 win-debug).
- **2026-05-02** — `crd-jobs` v1d Vyukov MPMC injection queue shipped; all 6 configs green (301/301 win-debug).
- **2026-05-02** — `crd-jobs` v1c Chase-Lev work-stealing deque shipped; all 6 configs green (291/291 win-debug).
- **2026-05-02** — `crd-jobs` v1b fiber pool shipped; renderer LTO fix; all 6 configs green (279/279 win-debug).
- **2026-05-01** — `crd-jobs` v1a hand-rolled asm context switch shipped (266/266 win-debug).
- **2026-05-01** — `crd-renderer` v1i swapchain blit + first full frame loop shipped (261/261 win-debug).
- **2026-05-01** — `crd-renderer` v1h index buffer + `draw_indexed` shipped (257/257 win-debug).
- **2026-05-01** — `crd-renderer` v1g `ForwardRenderPath` shipped (253/253 win-debug).
- **2026-05-01** — `crd-renderer` v1c frame graph v1 shipped (233/233 win-debug).
- **2026-05-01** — `crd-renderer` v1b real draw execution shipped.
- **2026-05-01** — `crd-renderer` v1a explicit renderables shipped.
- **2026-05-01** — `crd-shader` 2.3g pipeline handoff shipped.
- **2026-04-29** — `crd-shader` 2.3c reflection consumption shipped.
- **2026-04-29** — `crd-shader` 2.3b frontend → IR seam + GLSL ingest shipped.
- **2026-04-29** — `crd-shader` 2.3a public envelope shipped.
- **2026-04-29** — GPU memory + streaming foundation shipped.
- **2026-04-28** — ImGui debug overlay shipped.
- **2026-04-28** — `crd-config` core shipped.
- **2026-04-28** — First triangle through full RHI/Vulkan path.
- **2026-04-27** — `crd-rhi-vulkan` bootstrap (instance/device/surface/swapchain).
- **2026-04-26** — `crd-rhi` v1a scaffold with fake-backend tests.
- **2026-04-26** — `crd-app` Phase 1.5 shipped (LayerStack + propagated
  events + sync EventBus).
- **2026-04-25** — Platform v1c (input) shipped.

> Older entries: `docs/sessions/`.
