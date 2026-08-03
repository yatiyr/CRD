# Session 2026-08-03 — RAF band: full command-model close + the desc/cooked/runtime substrate through the frame-graph architecture (RAF-2 completion → RAF-7)

> Re-entry pointer: `context.md` §"STATE AT 2026-08-03 SESSION END". Band contract: D-007 "RAF band".
> Mission constitution: `docs/research/2026-08-03-gold-standard-asset-driven-rendering.md`. Everything below is
> **green, LLVM-20 tidy-clean, purely additive (sandbox untouched), and uncommitted** (user controls commits).

## What shipped (in order)

### RAF-2 — closed FULL Gate 2 (every command kind, no deferral)
`engine/gpu-context/include/crd/gpu/command_model.hpp` + `src/command_model.cpp` + `src/command_encoder.cpp`:
the canonical data model that collapses all **53** combinatorial `draw_*/dispatch_*/trace_*` verbs, plus a
backend-agnostic **`TranslatingCommandEncoder`** (`IRasterContext::create_command_encoder()`, appended at vtable END)
that lowers every kind through the context's existing virtual verbs — one encoder, both backends, zero duplicated
lowering. Reworked `GeometrySource`/`DispatchDesc` indirect fields to `IStorageBuffer*` (matching the real verbs);
enriched `TraceDesc` (full SBT + acceleration structure); added `validate_trace` + `MissingCountBuffer`/
`MissingAccelerationStructure`. Fixed a real encoder bug: a depth-less scope must impose `DepthCompare::Always`, not
the struct's `LessEqual` default. **Gates:** `crd-gpu-context-tests` 6/6 (device-free validation, no-per-draw-alloc
proven at compile time) + `crd-gpu-context-encoder-gpu-tests` 2/2 (**1025 assertions** — Vulkan AND DX12 drive
fullscreen · clear · copy · blit · resolve · storage-pull · indexed · color+depth · load-store · mesh · tess THROUGH
the encoder, each BYTE-IDENTICAL to the legacy verb).

⚠ **Scar (recorded):** `draw_storage_mrt` (and the indexed-indirect / shadowed verbs) **do not work with
independently-created standalone `create_color_target` objects** — attachment 1 stays unwritten, in BOTH the legacy
call and the encoder call (identical args). They are built for **frame-graph transients** allocated as a coherent
set. So MRT / indirect / comparison-sampler / bindless are **mapped in the encoder + hermetically validated** but not
gate-able through standalone targets; they gate once a coherent-transient path exists (RAF-7's remaining item). This
is a backend verb/allocation limitation, NOT an encoder defect.

### RAF-3 — desc/cooked/runtime substrate (`crd-render-asset-core`)
`cooked.hpp`/`cooked.cpp`: `CookedHeader` (canonical blob prefix: magic·type·`SchemaVersion`·`InterfaceHash`·
`ContentHash`·`AssetId`·deps, serialized FIELD-BY-FIELD little-endian ⇒ byte-deterministic — the struct-padding
scar); `interface_hash_of`/`content_hash_of`; generational `RuntimeHandle`/`RuntimeSlot` (stale-after-replacement,
the RAF-11 hot-reload safety property); `read_cooked_header` validation. `crd-render-asset-core-tests` **12/12**.
Per-family blob adoption folds into RAF-4 (shader) / RAF-5 (material,technique) / RAF-7 (frame) — each cooker
touched once. (Audit finding: `render-asset-core` was previously unconsumed by any cooker.)

### RAF-4 — shader+program contract (`crd-render-program`, NEW module)
`ProgramStage` · typed `StageIoVar` I/O · `ResourceDecl` (kind+frequency) · `ProgramContract`
(`add_module`/`validate`/`resolve_layout`/`validate_attachment_compat`/`interface_hash`) · `ResolvedBinding`
(deterministic compact slots by frequency — the layout that replaces hard-coded `binding N`) · `VariantKey` (9 axes)
+ `variant_space_size`. **No-debt unification:** `BindingFrequency`/`BindingKind` moved to render-asset-core
(`binding.hpp`), `command_model.hpp` `using`-aliases it — ONE definition; transparent (encoder GPU still 1025/1025).
`crd-render-program-tests` **9/9**.

### RAF-5 — material+technique contract (`crd-render-material`, NEW module)
`RenderChannel` surface/lighting split (materials structurally CANNOT touch lighting); `RuntimeMaterialDefinition`/
`RuntimeMaterialInstance` (many instances share one def; invalid-override + missing-texture rejected);
`RuntimeTechnique` (surface-input contract + phases); `validate_surface_compat`/`validate_phase`; `resolve_variant`
(instance-independent ⇒ instances share a variant) + collision-robust `VariantCache`. `crd-render-material-tests`
**7/7** (2 tidy issues fixed on the spot: enum-value consistency, unused const).

### RAF-6 — pass-executor registry (`crd-render-pass`, NEW module)
`ExecutorTypeId` (name hash, resolved once at cook — never a string at record); versioned `ExecutorSchema` (typed
params · resource slots [`SlotResourceKind`] · queue); `ExecutorRegistry` (binary-search, duplicate-id rejection);
typed `PassPayload` (`TypedValue` union — NO `void*`); `validate_payload`; `register_builtin_executors` (9 built-ins:
scene.raster · fullscreen.raster · compute.dispatch · transfer.{clear,copy,blit,resolve} · raytrace.dispatch ·
present). `crd-render-pass-tests` **5/5** (incl. an APP executor registered + used from a payload with zero
engine-enum edits). (transfer.copy/blit `src` set to ColorTarget; fullscreen.raster `input` made optional.)

### RAF-7 — unified frame-graph runtime (`crd-render-graph`, NEW module)
Separated forms compile-once/execute-many: `FrameGraphTemplate` → `CompiledFrameGraph` (deterministic Kahn schedule
over reads/writes + transient ALIASING [non-overlapping lifetimes share a physical slot] + persistent/history
PINNING) → execution. Executor `PassRecordFn`s emit the RAF-2 canonical command model into an `ICommandEncoder`
(retiring the FramePassKind→verb switch); `RecordContext` enforces declared==recorded. **Architecture gate (device-
free, command-capturing mock encoder): `crd-render-graph-tests` 5/5** — hand-built==authored · multiple-packets-in-
scope · undeclared-diagnosed · aliasing+persistent · resize-minimal. **Live-GPU wiring gate: `crd-render-graph-gpu-
tests` 2/2 (39 assertions)** — a 2-pass graph (scene.raster→transfer.copy) executes on **Vulkan AND DX12** via
`raster.create_command_encoder()` → real draw+copy → correct pixels. The architecture runs on hardware.

## The ONE bounded item left in Phase 7 (next session's first task)
The 4 frame-graph-shaped kinds (**MRT · indirect/indirect-count · comparison-sampler/shadow · bindless**) need a
**coherent transient-set allocation** capability on the raster context (both backends) — because `draw_storage_mrt`
et al. fail with standalone targets (the scar above). Add that capability, then gate the 4 through a frame-graph
pass. Then RAF-8 (scene-render → orchestration; the first LIVE-code phase) onward.

## Discipline held
Every phase: a new **leaf module** (no edits to live frame-cook/frame_runtime/scene-render/backends except the
append-only `create_command_encoder` vtable slot + the transparent binding-vocab move), ⛔ no-std, LLVM-20 tidy-clean
per-slice, each gated `raf<N>` in ctest. D-007 rows RAF-0…7 + `context.md` updated. Nothing committed.
