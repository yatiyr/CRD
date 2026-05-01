# Cerid — Live Context

> Engine'in kısa-vadeli hafızası. "Şu an neredeyiz?" sorusuna cevap verir.
> "Master plan ne?" sorusunun cevabı `docs/ROADMAP.md` ve oradan
> dallanan dosyalardadır.
>
> Her session sonu `@docs-keeper` günceller. Kısa kalır. Eski session
> detayları `docs/sessions/YYYY-MM-DD-*.md`'de yaşar, burada değil.

---

## Current focus

**Phase 2.5 — `crd-jobs` fiber-based job system. v1a + v1b + v1c shipped.**

Design complete (ADR-0033). Decisions locked:
- Main thread converts to fiber and joins the worker pool (pinned-job mechanism for GLFW thread 0).
- Hand-rolled asm context switch (Windows x64 MASM + Linux x64 AT&T).
- Chase-Lev work-stealing deques (3 per thread: High/Normal/Low) + Vyukov MPMC injection queues.
- 64-byte SBO for job closures (48-byte buffer + fn ptr + metadata = one cache line).
- Pool-allocated counters; ABA-safe double-check wait mechanism.
- Three priority levels: High / Normal / Low.
- Stack sizes: Small 64 KB × 128, Medium 512 KB × 64, Large 2 MB × 16.

Full design packet: `docs/phases/phase-2.5-jobs.md`. 11 slices (v1a–v1k).
**Shipped:** v1a (asm context switch), v1b (fiber pool), v1c (Chase-Lev deque), v1d (Vyukov MPMC queue).
**Next:** v1e — Priority scheduler (3-level drain + pinned-job slot).

Aktif phase dosyaları: `docs/phases/phase-2.5-jobs.md` (active) + `docs/phases/phase-2-graphics.md` (2.4 ongoing)

## Active detour

_none — running on the main roadmap._

> When a detour opens, this section names it (e.g. "D-001: investigate
> shader-cache corruption") and the main roadmap pauses until it closes.
> Detour file: `docs/detours/D-NNN-<slug>.md`. Queue rules:
> `docs/detours/README.md`.

## Last shipped milestone

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

## Previous shipped milestone

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

1. **`crd-jobs` v1e** — Priority scheduler: 3-level drain (High → Normal → Low), pinned-job slot.
2. **`crd-jobs` v1e** — Priority scheduler: 3-level drain (High → Normal → Low), pinned-job slot.
3. **`crd-jobs` v1f** — Counter + wait mechanism: `Counter` pool, Treiber waiter list, ABA-safe double-check.
4. **`crd-resources` + `asset_cooker` 2.6** — after crd-jobs v1k complete.

## Open questions

- `crd-config` hot-reload remains 1.6b unless ImGui integration proves it
  should move earlier.
- Runtime scene binary format — FlatBuffers vs Cap'n Proto? Park for
  Phase 3.1c.

## Test counts (last quality pass)

- win-debug:          301/301
- win-relwithdebinfo: 301/301
- win-release:        298/298
- win-asan:           301/301
- win-clang-cl:       301/301
- win-tidy:           301/301

(win-release is 3 fewer: debug-only `FiberState` tests excluded by `#if CRD_ENABLE_ASSERTS`)

## Pointers (lazy-load reference)

Agents: don't read everything. Use these breadcrumbs.

- **Hub:** `docs/ROADMAP.md` (small navigation page; safe to read fully)
- **Principles:** `docs/PRINCIPLES.md` (read every session, short)
- **Active phase only:** `docs/phases/phase-2.3-shader.md`
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
