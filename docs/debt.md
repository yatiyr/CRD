# Cerid — Open Debt

Items that are not blockers but should not be forgotten. When picked up,
move to a session log entry and remove from here.

## Active debt

### ~~`linux-gcc-release` ctest intermittent flake~~ — ROOT-CAUSED + FIXED 2026-05-12 (jobs hardening)

Was: `linux-gcc-release` intermittently failed ctest with an opaque
`Errors while running CTest` (exit 8); a retry passed. The opaque exit
masked a crash in the test `jobs: run_and_wait from inside a worker
fiber`. Diagnosed on a native Linux VM (gdb on core dumps / live hangs);
**four distinct, all pre-existing, bugs in the fiber job system** were
behind it, fixed together:

1. **Optimizer caching the per-thread TLS base across `fiber_switch`**
   (`worker_pool.cpp`). `job_fiber_trampoline` reads thread-locals after
   the job call; a job that `jobs::wait()`s from inside its fiber can
   resume on a *different* OS thread, but the C++ abstract machine has no
   notion of that, so GCC `-O3` hoisted `lea tl_sched_ctx@tpoff(%fs:0)`
   out of the trampoline loop into a callee-saved register and reused it
   across the job call → on a cross-thread resume the trailing
   `fiber_switch` jumped onto the *wrong* thread's scheduler stack. (MSVC
   was masked by the pre-existing `/Od` on this TU.) **Fix:**
   `CRD_JOBS_TLS_OPAQUE` (`__attribute__((noipa))` on GCC, `CRD_NOINLINE`
   elsewhere) on the `tl_scheduler_context` / `tl_current_fiber_ref` /
   `tl_worker_pool` accessors, and the trampoline reaches every
   post-resume thread-local through them — forcing a fresh TLS-base load.
   This is the textbook fix for the issue (LLVM #98479 / #47179 / #63022,
   LDC #666: "define TLS accessors in a separate TU and call them").

2. **`run_job_in_fiber` left `tl_fiber` set to the parked fiber** when a
   fiber suspended (`worker_pool.cpp`) — the completed path nulled it (via
   the trampoline) but the suspended path didn't. So after a `pump()`-
   driven job suspended inside `jobs::wait()`'s spin, the *main thread's*
   `tl_fiber` was stale, and the next `jobs::wait()` on the main thread
   read it, took the fiber-suspend path with a garbage "current fiber" →
   `counter_wait` corrupted the runtime / asserted `fiber must be Active`.
   (Caught via a core dump: the asserting thread's backtrace was
   `main → jobs::wait → counter_wait`, impossible if `tl_fiber` were
   correct.) **Fix:** `run_job_in_fiber` clears `tl_fiber` on the
   suspended path before returning.

3. **`counter_wait` published the `Waiter` before the fiber parked.** A
   `counter_decrement` could grab a just-published `Waiter`, wake the
   fiber, and enqueue a resume before the fiber's own `fiber_switch` had
   saved its context → the resume `fiber_switch`'d into a stale context.
   And the `canceled`-bool cancel/wake handoff had a TOCTOU window
   (a decrement could see a published `Waiter` as "not canceled, target
   matches" before `counter_wait` decided to cancel → phantom resume into
   a still-running fiber). **Fix:** *switch-then-publish* — the fiber
   stashes a park request (`tl_set_pending_park`) and switches to the
   scheduler *first* (saving its context); the scheduler then publishes
   the `Waiter` (`counter_finish_park`), ABA-re-checks, and on a satisfied
   re-check resumes the fiber itself. Cancel/wake is now a single CAS on a
   3-state token `WaiterClaim {Pending, Canceled, Wakeup}`. A
   `Waiter::park_finalized` flag (set by `counter_finish_park` at its end,
   spun on by `counter_wait` after the switch) keeps the resumed fiber
   from racing ahead and freeing the counter / unwinding the `Waiter`
   while `counter_finish_park` is still mid-flight.

4. **`counter_decrement`'s "steal list → partition → re-push" raced
   concurrent decrements.** Decrement A steals a not-yet-satisfied
   `Waiter`, is about to re-push it; decrement C (the one to 0) runs its
   `exchange` in the gap, sees an empty list; A re-pushes onto a list
   nobody will ever drain again → lost wakeup → deadlock. **Fix:** since
   the job system only ever waits for the counter to reach 0,
   `counter_decrement` now touches the waiters list at *exactly one* point
   — the decrement that hits 0 — and only ever drains it, never partially
   rebuilds (no `put_back`). `counter_wait` / `counter_finish_park` assert
   `target == 0`.

**Verification:** disassembly (the trampoline no longer caches the TLS
base); ~9,000+ aggressive cross-thread-resume stress runs (4/6/8 worker
threads, deeply-nested `run_and_wait`) across release / `-O2 -g` asserts-on
/ `-O0` asserts-on builds — 0 failures (vs ~1-in-100-to-400 crashes before
the `tl_fiber` fix); full Linux ctest sweep (debug / debug-scalar / asan /
relwithdebinfo / release) green; in-tree regression test
`jobs: cross-thread fiber resume stress` (`[jobs][stress]`). Files (the fix):
`engine/jobs/src/worker_pool.{cpp,hpp}`, `engine/jobs/src/counter.{cpp,hpp}`,
`engine/jobs/CMakeLists.txt`, `tests/jobs/test_jobs.cpp`,
`tests/jobs/test_counter.cpp`. Session log:
`docs/sessions/2026-05-12-jobs-fiber-tls-hoist-fix.md`.

**Follow-up cleanups (2026-05-12, on the Windows dev box):** full 14→17-config
`scripts/full-sweep.ps1` (Win ×10 + Linux ×7, now incl. AVX2 codegen) PASS +
Windows `[jobs][stress]` hammered ×500/config + the old `crd-resources-tests`
streaming repro ×200 on `win-release` — all clean. With that confirmed: the
MSVC `/Od` on `worker_pool.cpp` + `fiber_init.cpp` was **dropped**
(`engine/jobs/CMakeLists.txt`); a plain-`sse2` SIMD lane was **added**
(`win-debug-sse2` / `linux-gcc-debug-sse2` config/build/test presets, CI matrix
entries, `full-sweep.ps1` lanes); the `CounterPool::release` debug assert-walk
over leftover `Waiter`s was **removed** (post the switch-then-publish protocol,
no Pending waiter can survive to `release()`, so the walk could only ever read
already-unwound `counter_wait()` stack frames); and `scripts/wsl-build.ps1` was
**hardened** — `linux-gcc-debug-sse2` added to its `[ValidateSet]`, and the
`& wsl.exe -- bash …` call moved out from under `$ErrorActionPreference='Stop'`
(PowerShell 5.1 was surfacing the inner bash's stderr — a benign CMake
`cmake_minimum_required` deprecation warning on a fresh `_deps` configure — as a
native-command error and killing the whole sweep; the v0e-post-mortem stderr
brittleness, now actually fixed; `$LASTEXITCODE` is still the real failure
signal). `docs/jobs/WINDOWS_VERIFICATION.md` is satisfied. This debt item is
fully closed.


### Phase 3.1 v0c `crd::math::deterministic` — debt paid 2026-05-10

The v0c original deferral list (5 items) was closed in a same-day v0c-debt-A pass. Status:

- ✅ **f64 overloads** — all 26 functions ship with Cephes f64 coefficients (sin/cos/tan/asin/acos/atan/atan2/exp/exp2/log/log2/log10/pow/expm1/log1p/sinh/cosh/tanh + rounding/abs/copysign/fmod). Proper Cephes Padé for atan (full f64 precision, ≤4 ulp).
- ✅ **`Vec4f` / `Vec8f` SIMD-batched overloads — full branchless implementation.** sin/cos/exp/log overloads ship for both Vec4f and Vec8f with the same Cephes coefficients as the scalar versions, evaluated branchlessly via `select()` for octant routing + bitwise-mask sign tracking. AVX2 builds emit 256-bit `vaddps/vmulps ymm` for the inner polynomial; SSE2/NEON builds use 128-bit lanes; scalar fallback gives correct lane-wise results. Lane-wise bit-exact parity with scalar is verified across all 12 configs.
- ✅ **Hyperbolic `sinh`/`cosh`/`tanh`** — f32 + f64.
- ✅ **`erf`/`erfc`/`gamma`/`lgamma`/`beta`** — f32 + f64 (f32 forwards to f64; f64 uses Cephes erf.c / erfc.c / gamma.c).
- ✅ **`expm1`/`log1p`** — f32 + f64 (Taylor band for tiny x, exp/log fallback otherwise).

Remaining v0c-related items (re-scoped):

#### Bessel + orthogonal polynomials (deferred to `crd-hesap-stats` v13)

`bessel_j0`/`j1`/`y0`/`y1`/`i0`/`i1`/`k0`/`k1` (Bessel of first/second/modified kind) and `legendre_p`/`hermite_h`/`chebyshev_t` (orthogonal polynomials) are statistics-module concerns — they belong in `crd-hesap-stats` v13 (Phase 3.1.6, ADR-0065) where they sit alongside distribution PDFs/CDFs that consume them. Cephes has battle-tested implementations; the cooker over there will copy them.

**Where referenced:**
- `engine/math/include/crd/math/deterministic.hpp` — doc-block at the top of the file points at this debt entry.
- `docs/sessions/2026-05-10-v0c-deterministic.md` — closing session log (original v0c).
- Pending: `docs/sessions/2026-05-10-v0c-debt-A-paydown.md` — debt-paydown session log.

---

### Phase 3.0 v1m Öbek system — three deferred follow-ups (2026-05-08)

The full Öbek system (ADR-0058) shipped across v1m1–v1m5b in twelve sub-slices. Three items were explicitly carved out as post-Phase-3.0 follow-ups so the v1m closure stayed focused.

1. **Hot-reload watcher with OCHN graph awareness** (v1m5c) — the öbek format already emits OCHN entries listing every transitive dependency (extends + nested) with FNV-1a 64 source-byte content hashes. What's missing: a filesystem watcher that consumes OCHN, detects upstream changes, and triggers transitive re-cook + atomic ResourceManager swap (matching the existing shader hot-reload pattern). Lands when filesystem-watching infrastructure is established (likely Phase 7 editor or earlier if a content workflow needs it).

2. **`obekc extract <source.obek.toml> --root <name> --output <new>` CLI tool** (v1m5c, ADR-0058 pillar 14 "Decompose") — extract a sub-graph rooted at a named entity into a new standalone `.obek.toml` file, with optional `--rewrite-source` to convert the original's inlined entities into a nested `obek = "..."` reference. Editor "make this a sub-prefab" operation. Needs its own binary entry point under `tools/`. Defer until the editor (Phase 7) or a real content workflow surfaces the need.

3. **InheritPolicy CoW: dense-buffer optimization** (v1m4b future) — v1m4b's CoW backend wastes `sizeof(component)` bytes per shared slot in the dense buffer (the bytes are unused for shared slots; only used after CoW write-break). For sizeof(component) >> sizeof(pool_idx), this dilutes the memory savings. A future optimization could allocate dense bytes lazily per-slot (e.g., a separate "owned slots only" dense buffer indexed by per-entity offset). Acceptable trade-off at v1m4b — pool-side dedup still gives N→1 sharing across instances, which is the dominant savings axis for the canonical "10k tree forest" workload.

**Where referenced:**
- `engine/scene/include/crd/scene/obek.hpp` — doc-block at the top of the file points at this debt entry.
- `docs/sessions/2026-05-08-scene-v1m5-revert-batch.md` — v1m closure session log.
- `docs/sessions/2026-05-08-scene-v1m4b-cow-backend.md` — pin #8 about wasted dense-buffer bytes.

---

### Phase 3.0 v1l cook_scene cooker — eight deferred follow-ups (2026-05-08)

`SceneCooker` + `scene_cooker_inline()` + `Transform`/six-relation built-in TOML readers + cooker-side propagation bake shipped in v1l. The authoring layer is in place; the following items are explicitly out of v1l scope.

1. **asset_cooker file-handler integration** — v1l ships the `SceneCooker` API but not the `.scene.toml` extension dispatcher. `tools/asset_cooker/src/cook_command.cpp`'s extension router does not yet route `.scene.toml` to `SceneCooker::cook_inline`. v1m (sandbox) or earliest content workflow will wire it; the API is ready and tested.

2. **Hierarchical entity addressing** — `[entity.player.weapon]` is rejected at cook time (test case in `test_scene_cooker.cpp`). A first-class child-as-nested-table syntax with cycle detection would simplify deep hierarchies; deferred to v1m+ once the sandbox surfaces a real authoring need.

3. **Per-instance prefab overrides** (v1k debt #5 reframed) — TOML `extends = "base.scene.toml"` with override blocks. The cooker is the right layer (instantiation-time merge). Reserved.

4. **Multi-file scene composition** — `[include = "level/region_a.scene.toml"]` recursive include with hot-reload-aware dependency tracking. Reserved for the streaming-load era (Phase 3.5+).

5. **Hot-reload of `.scene.toml`** — TOML watcher → recook → `SceneLoader.reload`. Same pattern as shader hot-reload but at the cooker layer. Reserved until the editor needs it.

6. **TOML schema migration** — when a component bumps its FourCC version, TOML migration tables let old `.scene.toml` files cook correctly without manual edits. Pairs with v1k debt #3 (binary-side migration).

7. **Compressed SCEN at the cooker** (v1k debt #7 picked up here) — CRDR supports zstd-compressed chunks (chunk-flag bit 0). v1l emits uncompressed. Multi-MB scenes will benefit; one-line flip in `SceneArtifactBuilder` once the cooker has size-based heuristics.

8. **Big-endian cooker output** (v1k debt #6 picked up here) — v1l SCEN is little-endian per CRDR. Cross-platform byte-order swap at cook time is a v1n+ concern.

**Where referenced:**
- `tools/asset_cooker/include/crd/cooker/scene_cooker.hpp` — doc-block points at this debt entry.
- `docs/sessions/2026-05-08-scene-v1l-cooker.md` — full session log with the propagation-bake fix and decisions.

---

### Phase 3.0 v1k SceneResource — seven deferred follow-ups (2026-05-07)

`SceneResource` + `SceneLoader` + `SceneArtifactBuilder` shipped in v1k. The persistence layer is in place; the following items are explicitly out of v1k scope. (Item #1 of the original eight closed by v1l on 2026-05-08; items #6 big-endian and #7 compressed SCEN repointed to the cooker layer in v1l's debt list above.)

1. **Streaming / incremental scene loading** — v1k loads-all-or-fail. Streaming visible-only entities (camera-frustum LOD, region-of-interest persistence) is Phase 3.5+. The current `SceneArtifactBuilder` filters at build time but the loader instantiates everything; partial-instantiation API is reserved.

2. **Schema migration** between SCEN versions — `kSceneSchemaVersion = 1` is fixed. v1n+ adds migration tables (v1 → v2 → ... transformer functions) once a layout change is needed. Pairs with v1l debt #6 (TOML-side migration).

3. **Entity-name lookup** post-load — finding a spawned entity by string name. Out of v1k scope; user-defined `Name` component or query-by-component is the path. v1m sandbox may want explicit name lookup; addressed there.

4. **Per-instance component overrides** — prefab+override pattern (instantiate scene, then override specific component values per entity). v1k loads verbatim. Now reframed as v1l debt #3 (cooker is the right layer).

5. **`World::mark_all_transforms_dirty()` helper** — convenience for callers loading a SCEN with stale world matrices who want propagation to re-derive. v1l's cooker bakes world matrices into SCEN bytes, so most callers no longer need this; the helper is still reserved if a use case appears.

**Where referenced:**
- `engine/scene/include/crd/scene/scene_resource.hpp` — doc-block points at this debt entry.
- `docs/sessions/2026-05-07-scene-v1k-scene-resource.md` — full session log with the eight decisions.
- `docs/sessions/2026-05-08-scene-v1l-cooker.md` — cooker session that closed item #1 and repointed #6/#7.

---

### Phase 3.0 v1j Transform — seven deferred follow-ups (2026-05-07)

`Transform` + `TransformPropagation` shipped in v1j with cross-domain robustness for games / robotics / aerospace / DAW. The following items are explicitly out of v1j scope; each has its own pickup phase or trigger condition.

1. **Polar decomposition for skewed Mat4** — `crd::math::to_trs` uses `from_mat3` on the post-scale-removal columns, which silently loses skew. True polar decomposition (SVD or iterative orthogonalisation) is reserved for a v1j+1 follow-up if a use case (CAD / mesh-import shear) appears. Documented in `to_trs`'s doc-block.

2. **TransformF64 (f64 precision) component + propagation system** — orbital / aerospace / atomic-resolution domains need f64 precision. Math layer already ships `crd::math::Transformd`. The v1j architecture supports it: register a `TransformF64` component + write a `TransformPropagationF64` `ISystem` that mirrors the f32 path. v1n's reserved-slot freeze test will verify the registration grammar accepts the custom type. v1k SceneLoader will accept it without changes.

3. **Parallel propagation** — single-threaded per ADR-0054. Phase 3.5 evolution once `par_each` over Query chunks lands. Per-subtree parallelism is straightforward (independent dirty roots → independent subtree DFS); each dirty root is one work-item.

4. **AttachedTo socket propagation** — Phase 3.2 (animation) ships an attachment-pose system that composes with TransformPropagation (sockets snap to bones).

5. **Per-system change tracking for `.changed<T>()`** — current ChangeDetect snapshot is "modified during current frame" (v1i pin). Cross-frame "what changed since my system last ran" needs per-system state. v1h+1 evolution.

6. **Auto-renormalize rotation policy** — v1j makes renormalize OFF-by-default. A registration trait (`AutoNormalizeRotation{}`) could opt-in per component. Reserved slot if accumulated drift becomes visible in real workloads.

7. **`set_rotation_look_at` direction convention** — current implementation uses (right, up, -forward) columns matching the right-handed convention. Some domains (aerospace yaw-pitch-roll) need (forward, right, up) variants. Reserved as a follow-up trait or alternative API if a domain needs the explicit convention.

**Where referenced:**
- `engine/scene/include/crd/scene/transform.hpp` — doc-block points at this debt entry for items 2 and 6.
- `engine/math/include/crd/math/quat.hpp` — `to_trs` doc-block points at item 1.
- `docs/sessions/2026-05-07-scene-v1j-transform-propagation.md` — full session log with the seven decisions.

---

### Memory allocator infrastructure (D-001 closed 2026-05-07)

`TlsfAllocator` (D-001-a) and `GrowablePoolAllocator` + ChunkAllocator-pooled (D-001-b) shipped. The v1c1 O(N) `ChunkAllocator::free` perf debt is closed — chunk allocate / free are now both O(1) via the GrowablePool's intrusive free-list. See `docs/sessions/2026-05-07-detour-D-001b-growable-pool.md` for the closing summary.

**Implicit-but-untracked debt also closed 2026-05-07 (allocator-audit Option C):** `ArchetypeGraph` was using `std::make_unique<Archetype>` — the only place in Phase 3.0 that bypassed the World's `IAllocator` chain. Closed by pooling Archetype structs via `GrowablePoolAllocator(slots_per_page = 32, parent = m_alloc)`. `test_world_tlsf.cpp` (5 cases) proves the deployment pattern: a `World` on a `TlsfAllocator` pool runs full ECS lifecycle and returns every byte to the pool on destruction. Session log: `docs/sessions/2026-05-07-archetype-pool-tlsf-world.md`.

### TLSF allocator — three deferred enhancements (D-001-a, 2026-05-07)

`TlsfAllocator` ships in production-grade form: arbitrary alignment, O(1) operations under ASan stress (1000 iterations × 16/32/64/128/256-byte alignments), `try_allocate` non-throwing path. Three enhancements are consciously deferred:

1. **Conte's 8-byte block-header overlap trick.** Saves 8 B per allocation by overlapping the next block's `prev_phys_block` field with the previous block's payload tail. Documented in `docs/sessions/2026-05-07-detour-D-001a-tlsf.md`. Layout change is high-risk; the 8-byte saving is marginal at engine scale (1000 allocations of 100 bytes each saves ~8 KB). Pick up if memory pressure ever justifies — likely never.

2. **32-bit pointer support.** Cerid CI is 64-bit. Constants (`kFlIndexMax = 32`, the `unsigned long long` cast in `fls_size`) assume 64-bit. Adding template parameterization on pointer width adds bug surface for zero current benefit. Pick up if a 32-bit embedded target ever appears.

3. **Multi-threaded TLSF.** `IAllocator` base class documents "not thread-safe by default; hand them out per-thread or wrap them yourself" — this is the engine-wide convention. Lock-based TLSF kills the O(1) latency claim; lock-free TLSF is research-tier (Marotta et al. 2018). The standard scaling pattern is per-thread arenas. Don't pick up — this isn't TLSF-specific debt; it's a project architecture decision.

**Where it's referenced:**
- `engine/memory/src/allocators/tlsf_allocator.cpp` — current implementation comments document each deferred item at the relevant code site.
- `docs/sessions/2026-05-07-detour-D-001a-tlsf.md` — full design rationale.

---

### Async GPU upload (`GpuUploader`) — design closed by ADR-0061; impl lands in v1o1+v1o2

**Status (2026-05-09):** **Design half closed.** ADR-0061 locks the contract: three layers, owned by three modules.
- `crd-rhi`: adds `Fence` + non-waiting `Queue::submit(cmd, fence)`.
- `crd-renderer`: adds `UploadHandle` + `GpuUploader::upload_mesh_async` / `upload_texture_async` + `PendingMeshUpload` component + `RenderUploadSystem` (RenderExtract phase).
- `crd-scene`: unchanged — already exposes `AsyncAwareIndex` + `query<...>().skip_pending<Renderable>()`.

**Implementation half:** lands as Phase 3.0 v1o1 (RHI fence) + v1o2 (UploadHandle plumbing + RenderUploadSystem). v1o3 is the sandbox integration that uses the async path — the first real consumer.

**Why it matters:** `GpuUploader::upload_mesh` / `upload_texture` today end with `device.graphics_queue().submit_and_wait(*cmd)` — a `vkQueueWaitIdle` on the main thread. For BoomBox-class assets (~10 MB GLB → ~30 MB raw mesh) that's a visible hitch even though the CPU-side load is already async (Phase 2.8 v1g). The sync entry points stay (some smokes/tests need immediate readiness); the async siblings join them.

**Reserved follow-ups (NOT blocking v1o):**
- `Device::transfer_queue()` — opportunistic dedicated transfer queue (Vulkan: separate `VK_QUEUE_TRANSFER_BIT` family); falls back to graphics when absent. Reserved for Phase 3.5+ when streaming pressure makes it worthwhile.
- Timeline semaphores — replace binary fences when a consumer needs multi-step ordering or batched waits.
- Streaming budget — at most N concurrent uploads; queue the rest. Phase 3.5+ when terrain/LOD streaming arrives.
- Async texture upload consumer — `PendingTextureUpload` sibling component. Lands when a real texture-streaming workload surfaces (likely Phase 3.5 IBL or 3.8 GPU-driven rendering).

**Where it's referenced:**
- `docs/decisions/0061-async-gpu-upload-contract.md` — full design + module ownership + caller pattern.
- `docs/phases/phase-3.0-scene-ecs.md::v1o` — implementation slicing.
- `engine/renderer/src/gpu_uploader.cpp` — current synchronous implementation; v1o1+v1o2 add the async siblings.
- `sandbox/src/sandbox_layer.cpp::try_finalize_pending_load()` — current consumer; v1o3 migrates to async.

---

## Material system v1 known gaps

`MaterialResource` as shipped in Phase 2.6 v1e is a loader proof-of-concept, not a production material
abstraction. Phase 2.7 v1c (ADR-0048) redesigns it as a full material system foundation: `MaterialTemplate`
+ `MaterialInstance` two-tier split, new MATR artifact format (INFO/PRMS/DFLT/PASS/PSOS/OPTS chunks),
`ParameterType` enum, `ShaderOption` system with inline functor, `SurfaceData` GLSL contract, `PassType` enum,
`MaterialDomain` (pulled forward from Phase 2.8), `RasterState` encoding in the artifact.

**Updated status (post-ADR-0048):**
- **Items 1–3 (artifact layer)** — Closed by Phase 2.7 v1c. The artifact format now carries: parameter
  schema (PRMS), defaults (DFLT), pass-keyed shaders (PASS), PSO state per pass (PSOS), shader options (OPTS).
- **Items 1–3 (GPU wiring)** — Phase 2.8 wires the artifact data to Vulkan pipeline compilation
  (per-material pipeline cache, multi-pass ForwardRenderPath, depth-only prepass).
- **Items 4–5** — Still deferred (item 4 → Phase 3.5 CSM; item 5 → Phase 3.7 post-FX or Phase 3.8 GPU-driven).
  No consumer exists yet.

### 1. Material parameters, texture slots, and full parameter system ✅ Closes Phase 2.7 v1c

**What was missing:** `MaterialResource` held two shader handles and nothing else. No parameter schema,
no texture slots, no shader variants, no material domain, no render-pass awareness.

**What v1c delivers:**
- `MaterialTemplate` (replaces `MaterialResource`): loaded from MATR artifact. Carries parameter schema
  (`Array<CookedParameter>` sorted by name_hash), default values blob, pass-keyed shader handles
  (`HashMap<PassType, ResourceHandle<ShaderResource>>`), PSO state per pass, shader option declarations.
- `MaterialInstance` (caller-owned, not in ResourceManager): mutable overrides atop a `MaterialTemplate`.
  `set_float` / `set_vec4` / `set_texture` write into a `values_blob`. `variant_for_pass(pass)` evaluates
  inline functor rules and returns the correct `ShaderResource` permutation.
- `ParameterType` enum: Float/Float2/Float3/Float4/Color/Bool/Int/Enum/Texture2D/TextureCube/Sampler.
- Cook-time SPIR-V reflection: spirv-reflect extracts UBO offsets; cooker emits `CookedParameter` entries
  sorted by name_hash for O(log N) binary search at bind time.
- Inline functor: `enables_option = "USE_NORMAL_MAP"` on a texture parameter — no C++ subclass needed.

### 2. PSO state in the material artifact — ✅ Artifact layer closes Phase 2.7 v1c; GPU wiring Phase 2.8 v1a

**Artifact layer (v1c):** `PSOS` chunk carries a `RasterState` per PassType (present_mask + RasterState
array). `RasterState`: AlphaMode, CullMode, FillMode, depth_test, depth_write, src/dst BlendMode.

**GPU wiring (Phase 2.8 v1a):** `ForwardRenderPath` reads `material->pso_states[pass_type]` and
incorporates it into the `GraphicsPipelineDesc` key. Per-material pipeline cache keyed by
`(VariantKey, RasterState)`. `ForwardRenderPath` skips non-`Surface` domain materials.

### 3. Shader variant awareness (VariantKey + pass-keyed variants) — ✅ Artifact layer closes Phase 2.7 v1c; GPU wiring Phase 2.8 v1b

**Artifact layer (v1c):** `PASS` chunk stores `HashMap<PassType, ResourceId>`. `OPTS` chunk stores shader
option declarations. `MaterialInstance::variant_for_pass(pass)` evaluates inline functor rules, constructs
a `VariantKey`, and returns the appropriate `ShaderResource` from `tmpl->pass_shaders[pass]`.

**GPU wiring (Phase 2.8 v1b):** `ForwardRenderPath` calls `mat_inst.variant_for_pass(DepthPrepass)` in
the depth prepass and `mat_inst.variant_for_pass(Forward)` in the color pass. Each pass uses the shader
selected by the instance, not a hardcoded vert+frag pair.

### 4. Descriptor layout — per-material bindings — Deferred Phase 3.5

**What's missing:** Nothing in `MaterialTemplate` drives descriptor set creation or layout for set 1+
(per-material bindings). The `VulkanDescriptorAllocator` and `MaterialBindGroup` (formerly `MaterialInstance`)
are wired to hardcoded layouts, not artifact-driven layouts.

**What to add (Phase 3.5):**
- `MaterialTemplate` carries enough reflected binding data to construct a `VkDescriptorSetLayout` at load
  time (or defer to the first bind).
- `MaterialResourceLoader` merges spirv-reflect results across pass shaders to build the per-material
  binding table.
- `MaterialBindGroup` is rebuilt from `MaterialTemplate` rather than from a manually-constructed layout.

**Why deferred:** No concrete consumer (texture arrays, multiple samplers) until CSM and area-light
materials land in Phase 3.5, and post-FX materials in Phase 3.7.

### 5. Additional shader stages — Deferred Phase 3.5+

**What's missing:** The PASS chunk stores vertex+fragment shader pairs (one `ShaderResource` per PassType).
There is no slot for compute, mesh, or task shaders. A compute-only material (post-FX, particle simulation)
cannot be expressed.

**What to add (Phase 3.5):**
- Extend `ShaderResource` to carry multiple stages (vertex/fragment/compute/mesh as a tagged union).
- Update `MaterialTemplate::pass_shaders` value type to `ResourceHandle<ShaderResource>` where each
  `ShaderResource` declares its own stage set (already possible via the existing shader mechanism).
- The PASS chunk format is already stage-agnostic (one ResourceId per PassType entry). Only the shader
  artifact format changes — the material artifact format is unaffected.

**Why deferred:** Compute and mesh shaders are Phase 5 concerns. The PASS chunk format already accommodates
them — the `ShaderResource` inside can carry any combination of stages.

---

**Updated execution plan:**
- Phase 2.7 v1c closes the artifact layer of items 1–3 (full material foundation: ADR-0048).
- Phase 2.8 wires items 2–3 to actual Vulkan pipeline compilation and multi-pass rendering.
- Items 4 and 5 remain open; deferred until consumers create real demand (item 4 → Phase 3.5 CSM /
  area lights; item 5 → Phase 3.7 post-FX compute / Phase 3.8 GPU-driven culling).

See `docs/phases/phase-2.7-asset-import.md`, `docs/phases/phase-2.8-material-completion.md`,
ADR-0044, ADR-0046, ADR-0048.

## Long-term deferred

- **Stress `[.soak]` nightly lane** (deferred by detour D-002, 2026-05-12) — `tests/stress/`
  ships four `[.soak]`-tagged tests (TLSF churn, freeze + parallel_for, `ConcurrentQueue`
  MPMC, `AtomicArray`) with deliberately huge iteration counts (catch a 1-in-10M torn-write
  / false-share that the bounded variants miss). They're Catch-`[.]`-hidden so CI `ctest`
  skips them; today they only run on demand via `crd-stress-tests "[.soak]"`. Wanted: a
  scheduled nightly CI job (and a place to add future `[.soak]` tests as v3–v6's primitives
  pick up consumers). Until then, run the soak suite manually before relying on a new
  consumer of these primitives.
- **Concurrent hash map** (deferred by detour D-002, 2026-05-12) — a split-ordered /
  Cliff-Click-style lock-free hash map is genuinely hard and should not be built
  speculatively. D-002 ships `crd::containers::ConcurrentQueue<T>` (MPMC) and
  `crd::containers::AtomicArray<T>` (bounded atomic-append); a concurrent map lands
  only when a concrete consumer demands it. Until then: per-fiber scratch maps + a
  serial merge, or a `ConcurrentQueue` of update-requests drained by one fiber.
- **Thread-safe / sharded global allocator** (deferred by detour D-002, 2026-05-12) —
  `TlsfAllocator` and the pool/linear/stack allocators are single-threaded-by-contract
  and will *not* get a lock bolted on. If a shared cross-fiber heap is ever needed it is
  a new sharded/thread-caching allocator (tcmalloc-style: per-thread free lists + a
  locked central heap), built then, not now. `RefCounted` objects whose final release can
  occur off the creating thread must be backed by a thread-safe allocator or a
  deferred-free queue.

- **Multi-viewport ImGui** — Vulkan multi-viewport has known rough edges.
  Single-viewport docking only until `crd-ui` ships (planned Phase 5+).
  At that point, game/editor surfaces move to `crd-ui`; ImGui stays debug-only
  and multi-viewport is no longer needed.

## GPU instancing (planned Phase 3.2)

v1h ships `draw_indexed(index_count, first_index, vertex_offset)` — non-instanced only
(`instance_count` hardwired to 1 in the Vulkan call). When instancing lands:

**RHI changes:**
- Add `draw_instanced(vertex_count, instance_count, first_vertex, first_instance)` to
  `CommandBuffer` (non-indexed instanced path).
- Add `draw_indexed_instanced(index_count, instance_count, first_index, vertex_offset, first_instance)`
  to `CommandBuffer` (indexed instanced path, mirrors `vkCmdDrawIndexed` fully).
- All four draw variants (`draw`, `draw_indexed`, `draw_instanced`, `draw_indexed_instanced`)
  coexist; `VulkanCommandBuffer` implements all four.

**Renderer changes:**
- `Renderable` and `DrawItem` gain `instance_count = 1` (default keeps backward compat).
- `ForwardRenderPath` dispatch logic: `instance_count == 1` → non-instanced path (no
  regression); `instance_count > 1` → instanced path.
- GPU instance data buffer (transforms, material indices) is a Phase 3 GPU scene buffer
  concern — `crd-resources` provides per-frame upload; `ForwardRenderPath` binds it as
  a storage buffer at set 0 binding 1 or via push constants for the base instance.

**When:** After Phase 3.1 (stable entity/transform storage in the scene system) ships and
a GPU instance data layout is frozen. Instancing without a stable instance buffer contract
produces nothing useful. Target: Phase 3.2.

**Do NOT prematurely add `instance_count` to `Renderable` / `DrawItem` before that point.**

## Renderer optimization backlog (post-v1g)

Intentionally deferred. These require the render path to be working end-to-end
before they pay off. Implement in order of demonstrated need, not in anticipation.

- **Transient image aliasing in the frame graph** — `FrameGraph::execute` currently
  creates transient images fresh each frame and destroys them on `reset()`. A proper
  aliasing pass would reuse GPU heap pages across mutually-exclusive transients,
  reducing VRAM by the sum of the largest non-overlapping resource sets. Prerequisite:
  lifetime analysis pass in `FrameGraph::build()`.

- **HDR render target** — `ForwardRenderPath` uses `B8G8R8A8Unorm` (LDR). Switch to
  `R16G16B16A16Sfloat` (scene linear HDR) and add a tone-map pass before the swapchain
  blit. Required before bloom, exposure, or any physically-based lighting integral.

- **Depth-only pipeline for the depth prepass** — `ForwardRenderPath` v1g reuses the
  full vertex+fragment pipeline in the depth prepass. A vertex-only pipeline (null
  fragment shader, `Format::Undefined` color, `Format::D32Sfloat` depth only) removes
  unnecessary fragment work during the prepass. Requires the per-variant pipeline cache
  to store `{depth_pipeline, color_pipeline}` pairs.

- **Async pipeline compilation** — `PipelineResolver::resolve_pipeline()` is currently
  synchronous. Slow variant compiles stall the main thread. Solution: compile on a job
  thread, return a "pending" sentinel, and render with a fallback pipeline until the
  real one is ready. Integrates with `crd-jobs` (Phase 2.5).

- **Bindless material system** — Current: one descriptor set per material instance per
  frame (set 1), allocated from the ring pool. Future: global bindless descriptor heap
  (one giant `DescriptorSet` with an array of all textures + material CBs), indexed
  via a per-draw material index in the push constants. Eliminates per-draw
  `vkCmdBindDescriptorSets` calls. Requires Vulkan device features: `descriptorIndexing`.

- **GPU-driven rendering** — CPU culling + indirect draw. Replace per-object draw calls
  with a compute dispatch that reads a scene buffer, outputs `VkDrawIndirectCommand`
  structs, and optionally writes a visible-object list. Requires: stable GPU scene buffer
  (Phase 3 scene system), `VkDrawIndirectCount` (Vulkan 1.2 core), and a GPU frustum
  cull shader. Significant throughput gain for dense scenes (> ~10k draws).

- **Split vertex streams** — Separate position-only VBO from full-attribute VBO. The
  depth prepass only needs positions; pulling the full vertex (UVs, normals, tangents)
  wastes memory bandwidth. Requires `DrawItem` to carry both VBOs and shader variants to
  declare which stream they consume.

## Cleared debt

- **Disabled-trace benchmark** (2026-05-03) — Replaced compile-time-eliminated
  `CRD_LOG_TRACE` call with `CRD_LOG_INFO` gated by `runtime_level = Error`. The
  benchmark now measures the runtime short-circuit cost in all build configurations.
- **Doxygen per-symbol comments in crd-core** (2026-05-03) — Added `///` docs to all
  symbols in `types.hpp` (14 aliases), `platform.hpp` (18 macros + 3 functions), and
  `assert.hpp` (2 type aliases + 4 functions + 4 macros).
- **No SPSC RingBuffer** (2026-05-03) — Added `SpscQueue<T>` in
  `engine/containers/include/crd/containers/spsc_queue.hpp`. Lock-free, cache-line
  padded head/tail atomics, wait-free push and pop. Tested single-threaded and with
  concurrent 1M-item producer/consumer.
- **No file watcher in crd-platform** (2026-05-03) — Added polling-based `FileWatcher`
  in `engine/platform/`. Uses `fs::last_modified_unix_seconds()` on each polled path.
  Handles add/remove by handle, fires callbacks synchronously in `poll()`.
- **Multi-viewport ImGui deferred** (2026-05-03) — Moved to "Long-term deferred" above.
  Will not land until `crd-ui` ships; ImGui stays debug-only forever.
