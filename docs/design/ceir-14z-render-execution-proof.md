# CEIR-14z — the render DEVICE pixel proof (14z-0 decision packet)

**Status: ✅ FORK RESOLVED — Option A RATIFIED by the user 2026-08-11. 14z-1/14z-2/14z-3 DONE (gated 4 configs).** CEIR-14z uses a
test-surface `execute_render_lowered` driving `ICommandEncoder` (as 13z proved compute on `IComputeContext`); real frame-graph
integration is DEFERRED to CEIR-15/16. ⭐ **14z-3 is the FIRST render pixels from CEIR** (2026-08-11): the shared CEIR
`render.scope{render.draw}` renders the shared CKIR triangle RED on a blue clear, pixel-asserted on Windows-Vulkan (real GPU),
Windows-DX12 (real GPU) AND Linux-Vulkan (llvmpipe), validation-SILENT. Files: `tests/gpu-shared/ceir_render_triangle.hpp`
(the shared builder + the `run_ceir_render` executor wrapper), `tests/ceir-gpu-vulkan/test_ceir_render_vulkan.cpp` (+ the
device-free lower-shape guard), `tests/ceir-gpu-dx12/test_ceir_render_dx12.cpp`. ⭐ **PIVOT (user verdict 2026-08-11): the executor DRIVES FRAME-RECORDING MODE (gold-standard, no name-forward) — see the 14z-4 sections below.** ✅ **14z-4 (MRT + typed clears) COMPLETE 2026-08-11 (both backends):** 4a frame-recording drive + 2-scope · 4b per-attachment typed clears (`AttachmentClear`) · 4c(c1) CEIR binding resolver + first CEIR MRT · c2 uint MRT (+DX12 uint-clear value-convert fix) · c3 heterogeneous uint@0+float@1 MRT (+DX12 per-attachment RTV-format fix). NEXT = 14z-5 (depth-only). The test-bridge is a reversible proof harness — shipping render stays authored-graph-only. The technical
design-lock below (14z-1 materializers → 14z-2 execute_render_lowered → 14z-3..7 per-shape device proofs) is now the ACTIVE
plan. (14a–14d — scope/attachments, draw ops, indirect/mesh, resource-table semantics — are all device-FREE + closed.) Proof target (§169): triangle · MRT (typed clears per-target) · depth-only · indexed-indirect(-count) ·
mesh dispatch — pixel-asserted BOTH backends, ValidationCapture-silent.

## ⚠ THE FORK — how does a CEIR render program relate to the engine's AUTHORED frame graphs?

A CEIR raster executor is a **second producer into the engine's most mandate-laden machinery.** The ⛔⛔⛔
only-authored-frame-graphs mandate requires every render pass to go through the authored frame-graph machinery;
`ICommandEncoder` is "what both authored and hand-built graphs record into" (RAF-7); RAF-8/RAF-12 spent whole bands flipping
live rendering onto graph executors. So 14z is NOT "13z again on a different interface" — 13z's `IComputeContext` seam was
architecturally *quiet* (a test-surface compute bridge). 14z's seam touches the render single-path mandate, and how it
relates is exactly the **CEIR-15 (FrameGraph unification) + CEIR-16 (Executor migration)** question — which is why those band
rows exist. **This is the user's call (ADR-0108-flip class), not the render.scope-region class.**

### Option A — test-surface encoder bridge NOW; unify at CEIR-15/16 (⭐ recommended default, reversible)
`execute_render_lowered(ctx, cmds, ICommandEncoder&, resolvers…)` drives an encoder directly on a headless raster device —
a TEST surface, exactly as 13z proved compute on `IComputeContext` without touching the frame graph. Proves CEIR render
executes device-side + pixel-correct on both backends NOW. Does NOT integrate with authored frame graphs yet;
the unification (does a CEIR program *become* an authored graph? replace executor registration?) is deferred to CEIR-15/16
where it's the explicit subject. **Consequence:** a throwaway test-bridge to unwind at 15/16, but 14z closes band-14 with a
real device proof and the architecture decision stays open (correct — the data to decide it is what 15/16 build).

### Option B — CEIR lowers INTO an authored frame graph NOW
A CEIR render program is lowered to an authored graph (the mandate-compliant path); CEIR becomes a producer of authored
graphs immediately. **Consequence:** no test-bridge to unwind, but pulls CEIR-15's unification design forward into 14z
(much larger now), and commits the direction before 15/16's evidence exists.

### Option C — CEIR drives an encoder BESIDE the graph (parallel producer)
Likely REJECTED: two producers into the render path violates the single-authored-path mandate. Named for completeness.

**Recommendation: Option A** (test-bridge now, unify at 15/16) — it matches how 13z closed compute, keeps 14z bounded, and
leaves the 15/16 direction decision to the user with the evidence those bands produce. But the choice is the user's.

## Conditional technical design-lock (IF Option A — recorded now, reversible)

- **14z-1 (device-FREE): the materializers** — `materialize_rendering_desc(scope op, RasterTargetResolveFn) → RenderingDesc`
  (color/depth attachment ops → `ColorAttachmentDesc`/`DepthStencilAttachmentDesc`; ⭐ thread the RAH-1a.1 typed clear
  END-TO-END: a `clear_kind=uint` color attachment yields `RenderingDesc.color[i].clear_kind == Uint` + `clear_uint`) and
  `materialize_draw_packet(draw op, program/binding resolvers) → RasterDrawPacket` (GeometryKind + counts + first_vertex/
  max_draws per draw-op shape). PURE functions in crd-ceir-gpu, tested with fakes. This is most of the engineering, none of
  the device risk. ⛔ `IRasterProgram` is DISTINCT from `IGpuProgram` (a raster program = assembled VS+FS,
  `create_raster_program(vs, fs)`) — so `RasterProgramResolveFn(const Operation* draw, void*) → IRasterProgram*` (NOT the
  13z `KernelResolveFn` typing). `RasterTargetResolveFn(const Operation* attachment, void*) → IRasterTarget*`.
- **14z-2 (device-FREE): `execute_render_lowered`** walks Begin/Draw/End calling the materializers → the encoder; a
  `FakeEncoder` counts + captures the descs. ⛔ **barrier design point:** `execute_lowered`'s barrier replay is
  `ComputeRecorder`-shaped; on the encoder surface the frame graph owns barriers today — the render path's cross-scope
  barriers here are a design question (ANOTHER face of the fork; resolve with the architecture choice).
- **14z-3..7 (device, one per tick): the per-shape proofs** — triangle → MRT+typed-clears → depth-only → indexed-indirect
  (-count) → mesh dispatch. Each pixel-asserted BOTH backends, ValidationCapture-silent (Vulkan). Program source = the KIR
  cooked path (`create_program(KGraph)` → DXIL/SPIRV → `create_raster_program(vs, fs)`) — same route as the compute proofs
  (`test_dx12_raster.cpp` precedent). ⛔ SCARS to re-read per proof: depth-only ≠ forward (its OWN program, never a borrowed
  color program — the ⛔⛔⛔ discard scar); mesh-shader-device silent-scars; the NDC±Y mirror on any RTT sampled by UV. ⭐
  the REN-40 DrawIndex PUSH assertion + the forced-value probe (render one mesh LARGE, pin the field) land at the
  indexed-indirect proof (14z-6) — the 14c preservation split's other half.
- **BindingKind**: if any proof binds a resource TABLE, `BindingKind` needs a `ResourceTable` enumerator (the 14d
  named-forward; widen-enum-audit — `binding.hpp` is shared RAF-4). The base proofs bind buffers/images, so this may stay
  forward to CEIR-16.

## 14z-3 device-proof recon (2026-08-11, from the `test_dx12_raster.cpp` B3-e precedent)

The full headless triangle pattern (B3-e, DX12; the Vulkan mirror is the shared CKIR): `gctx = create_{dx12,vulkan}_gpu_context()`
→ `raster = create_{dx12,vulkan}_raster_context()` → the SHARED CKIR triangle `crd::gputest::build_triangle_vs(vg,ve)` +
`build_triangle_fs(fg,fe)` (`tests/gpu-shared/verb_packet_helpers.hpp`) → `vs = gctx->create_program(vg,ve)` (KIR→DXIL/SPIRV;
nullptr ⇒ WARN-skip when dxc/glslc absent) + `fs = gctx->create_program(fg,fe)` → `program = raster->create_raster_program(*vs,*fs)`
→ `target = raster->create_color_target(dim,dim)`. The encoder lifecycle (from `enc_fullscreen`): `enc = raster->create_command_encoder()`
→ `enc->begin_rendering(RenderingDesc{color[0]={target, LoadOp::Clear, clear}})` → `enc->draw(RasterDrawPacket{program, Draw,
GeometryKind::None, vertex_count=3})` → `enc->end_rendering()`; then `target->read_pixel(centre)` = red (triangle),
`read_pixel(corner)` = blue (clear). ⭐ **14z-3 drives this through CEIR:** build a `render.scope`(color_attachment over the
target; `load=clear`, `clear_b=1.0` blue) `{ render.draw(3,1) program=@tri }` → `lower_region` → `execute_render_lowered(ctx,
cmds, *enc, target_resolver→target, program_resolver→program)` → read_pixel. The GeometryKind None-vs-StoragePull refinement
(0 bindings ⇒ None) landed 2026-08-11 so the procedural triangle materializes as `None` (gated win 522, linux 527). **NEXT-TICK
WIRING:** a new device test (mirror `tests/ceir-gpu-vulkan`/`-dx12` — links crd-ceir + crd-ceir-gpu + crd-kir + the raster
context + gputest helpers) on BOTH backends; the target/program resolvers return the real objects; ValidationCapture-silent
(Vulkan). ⛔ still device-guarded (WARN-skip no adapter / no dxc) + a device-free always-runs guard.

## Open dependency to confirm at 14z-1 (not to discover at 14z-3)
Whether the raster contexts expose the `create_program(KGraph)` → `create_raster_program` path headlessly on BOTH backends
(the `test_dx12_raster.cpp` route is DX12; confirm the Vulkan raster context mirror + a headless pixel-readback target).

## ⚠ 14z-4/14z-6 MECHANISM DISCOVERY (2026-08-11) — MRT + indexed-indirect are FRAME-RECORDING verbs → a proof-list fork

**The finding (verified in the backend source, both platforms).** `execute_render_lowered` drives a **standalone synchronous
`ICommandEncoder`** (the Option A test-surface — a bare `create_command_encoder()`, NOT a frame graph). But several of the
§169 proof shapes are **frame-recording verbs that no-op outside a frame**:
`vulkan_raster_context.cpp:6219` — `draw_storage_mrt(...) { … if (!frame_recording()) { return; } … }` (the DX12 mirror gates
the same way across the storage/MRT/indirect/bindless verbs). Per the corrected RAF-7 scar
([[feedback_draw_storage_mrt_needs_coherent_frame_graph_transients_not_standalone_targets]]): `draw_storage_mrt`,
`draw_storage_multi_indexed_indirect`, `draw_storage_shadowed_depth`, `draw_bindless` are **frame-recording verbs by design**
— "the graph is the only consumer"; readback is the frame graph's job. They are NOT broken; they simply require the raster
context's one-submission frame-recording mode, which is exactly the **CEIR-15/16 frame-graph integration** Option A defers.

So on the synchronous encoder test-surface the §169 list splits:
- **Synchronously provable (encoder surface, 14z):** triangle (`draw`, ✅ 14z-3), depth-only (`draw_depth`/plain-depth — has a
  synchronous branch; confirm at 14z-5), mesh (`draw_mesh` — confirm at 14z-7).
- **Frame-recording-only (→ CEIR-15/16):** **MRT** (only synchronous MRT path is `draw_gbuffer`, the bundled `IGBufferTarget`
  — and it applies ONE shared clear to all attachments, so "typed clears asserted **per-target**" is *not expressible* there;
  `draw_storage_mrt` with per-target `color[]` clears is frame-recording-only) and **indexed-indirect(-count)**
  (`draw_storage_multi_indexed_indirect`, frame-recording-only; also the REN-40 DrawIndex push).

~~**Decision (recommended): NAME-FORWARD MRT + indexed-indirect to CEIR-15/16.**~~ **⛔ OVERRULED by the user's verdict
2026-08-11** ("the most architecturally convenient and gold-standard way — we don't take shortcuts; if we need to change the
engine or make a former decision better, we do it; if we see bugs we solve them immediately"). Name-forwarding is a shortcut;
struck. **RATIFIED gold-standard direction: the CEIR render executor DRIVES FRAME-RECORDING MODE** — this makes the earlier
Option-A "synchronous encoder test-surface" decision *better* (the user explicitly authorized that), so every render shape
proves device-side through its REAL body, MRT with GENUINE per-target clears included.

### RATIFIED PLAN (gold-standard frame-recording executor — supersedes the 14z-3..7 synchronous-encoder decomposition)

Both backends expose `IRasterContext::create_frame_graph()` (Vulkan REN-1 + DX12 `Dx12FrameGraph` REN-1 pt-2 — the
"DX12 until its port" header comment was STALE; the port shipped). Frame-graph API: `import_target(IRasterTarget&)→FgImage`
· `import_storage(IStorageBuffer&)→FgBuffer` · `add_pass(name,kind).writes(img).reads(buf).execute(FgExecuteFn,user)` ·
`build()` · `execute()` (ONE submission; barriers + end-of-frame readback owned by the graph — mirror
`crd::rendergraph::execute_frame`, `engine/render-graph/src/frame_graph.cpp:1337`). Record fn = `void(IFrameContext&, void*)`;
inside it `ctx.raster()` is in frame-recording mode, so the RAF-2 command-model encoder's verbs record into the frame.

- **14z-4a — the drive mechanism. ✅ DONE + gated 2026-08-11.** `execute_render_frame(ctx, cmds, IRasterContext&,
  IAllocator&, resolvers…) → ExecuteError` in crd-ceir-gpu (`render_materialize.cpp`): owns fg-create + `create_command_encoder`
  + imports (materialize each `render.scope`'s RenderingDesc → import its color/depth targets), ONE PASS per scope
  (declared `writes` = its targets — the fg DERIVES barriers + owns readback), whose record fn (`ceir_render_pass_cb`) calls
  the EXISTING `execute_render_lowered` on that scope's command SLICE through the frame-recording encoder (14z-2's walk reused
  UNCHANGED). ⛔ the per-scope pass closures live in a RESERVED `Array` (stable pointers across `execute()` — the
  `execute_frame` contract). ⛔ lowered `Barrier`s stay INERT; CEIR-barrier ↔ fg-barrier reconciliation is CEIR-15/16.
  `ExecuteError` gained `NoFrameGraph` + `FrameBuildFailed` (widen-enum-audit clean — the only exhaustive switch is
  `execute_error_name`; gcc `-Werror=switch` green). The TRIANGLE renders RED through the frame graph on Vulkan + DX12 (+
  llvmpipe), ONE submission, ValidationCapture-SILENT. The green synchronous 14z-3 proof stays as-is (the frame drive is
  ADDITIVE — a `run_ceir_render_frame` twin in the shared header). Gate: win-debug/asan 537/537, linux-gcc-debug/asan 530/530,
  LLVM-20 tidy(7). ~~SINGLE-SCOPE proven only — the N-scope machinery has executed ZERO times~~ **✅ SUPERSEDED 2026-08-11: the N-scope path
  is now PROVEN** by the 14z-4c TWO-SCOPE device test (triangle→A / triangle→B, distinct clears; the reserve-then-push closure
  array = the ⭐⭐ 11b MIRROR scar's shape — ASan-clean on both sanitized configs). **NEXT = 14z-4b.**
- **14z-4b — FIX the per-attachment clear engine gap.** `command_lowering.hpp:139` collapses every attachment's clear to
  `color[0].clear` and `:365` passes ONE `ClearColor` to `draw_storage_mrt` (per-attachment BLENDS are threaded via
  `blends[]`; clears are NOT). Gold-standard fix: thread the FULL typed clear (kind + float + uint — `ColorAttachmentDesc`
  already carries all three) per attachment, mirroring the `blends[]` idiom, through `draw_storage_mrt` → both backends (the
  Vulkan body already builds per-attachment `VkRenderingAttachmentInfo`; the DX12 RTV clear is per-attachment too — just wire
  the value). ⛔ VIRTUAL-SIGNATURE change → widen-audit EVERY override + caller: both backend `draw_storage_mrt`,
  `command_lowering.hpp:139/365`, `enc_draw_storage_mrt` (verb_packet_helpers), stub raster contexts in tests
  (`tests/scene-render/test_scene_render.cpp` has one), AND `crd::rendergraph` pass recording (grep `engine/render-graph` —
  the RAF-7 MRT gates run through `crd-render-graph-gpu-tests`, a caller outside the obvious list). ⛔ **PIN BEFORE CODING
  (advisor 2026-08-11):** (1) read DX12's `draw_storage_mrt` body too (only Vulkan's read so far — RTV-clear on
  `OMSetRenderTargets` vs `VkRenderingAttachmentInfo`); (2) the ⛔⛔ WBOIT multiplicative-blend IDENTITY-clear special case
  (Vulkan `vulkan_raster_context.cpp:6228-6235`) must SURVIVE on BOTH backends — DECIDE + write down whether a caller-supplied
  per-attachment clear OVERRIDES or DEFERS to the multiplicative identity; (3) SETTLE the signature shape first (a
  `const ColorAttachmentDesc*` span — one struct carries kind+float+uint+blend, replacing both `clear` and `blends` — is the
  gold-standard shape IF `ColorAttachmentDesc` is visible from the verb layer without a `raster_context.hpp ↔ command_model.hpp`
  include cycle; else parallel arrays) — consult the advisor before touching signatures (a mid-slice revision multiplies the
  widen-audit). Also fix the stale `create_frame_graph` "DX12 until its port" comment (`raster_context.hpp`) in this sweep
  (the header is edited anyway).
- **14z-4b SETTLED DESIGN (advisor 2026-08-11, after full recon).** ⛔ `draw_storage_mrt` is NON-VIRTUAL (a per-backend
  method called through the `detail::CommandEncoder<CtxT>` template — NOT an IRasterContext vtable slot), so the widen-audit
  surface is just: the 2 backend bodies + the ONE template call site `command_lowering.hpp:365`. (`crd::rendergraph` only
  MENTIONS it in a comment; `test_scene_render`'s StubRaster returns a null encoder ⇒ never instantiates the template ⇒ not a
  caller; no Vulkan intra-backend caller.) Include direction: `command_model.hpp` includes `raster_context.hpp`, so the verb
  layer has `ClearColor`+`BlendMode` but NOT `ClearKind`/`ColorAttachmentDesc` (a desc-span is a cycle). **Signature = (B):**
  MOVE `ClearKind` (`{Float=0, Uint}`) DOWN to `raster_context.hpp` (next to its sibling `ClearColor`; transparent — every
  user reaches it via `command_model.hpp`) + add `struct AttachmentClear { ClearKind kind=Float; ClearColor color{}; u32
  uint_value=0; BlendMode blend=Opaque; }` (⭐ defaults so `{}` == today's behavior); `draw_storage_mrt` takes
  `const AttachmentClear* attachments` REPLACING both `ClearColor clear` and `const BlendMode* blend` (guard
  `attachments==nullptr` like `targets==nullptr`). `command_lowering.hpp:139/365` builds the `AttachmentClear[]` from each
  `RenderingDesc.color[i]` (kind/clear/clear_uint/blend). ⛔ **WBOIT precedence RULE (write on both backends):** the
  multiplicative-identity clear keys off `attachments[i].blend` and OVERRIDES `attachments[i].color/kind` (`dst·(1-src)` from 0
  is unrecoverable regardless of caller intent — the scar's invariant). ⛔ **no CEIR-side change** — `materialize_rendering_desc`
  ALREADY emits per-attachment `ColorAttachmentDesc` clears; the fix is entirely `command_lowering` + the 2 verbs. Both backends
  already clear PER-ATTACHMENT (Vulkan per-`VkRenderingAttachmentInfo`, DX12 per-RTV `ClearRenderTargetView`) fed one value —
  float threading is trivial; the **UINT arm**: Vulkan `VkClearValue.color.uint32`; DX12 the peer trick
  (`draw_visbuffer:2876` — `ClearRenderTargetView` bit-reinterprets `float[4]` into R32_UINT; proven clearing to 0 only, so a
  non-zero uint clear is NEW territory to prove). **PROOFS (14z-4b):** float-distinct MRT (blue@0/magenta@1, RED@0/GREEN@1 via
  `build_gbuffer_two_output_fs`) + uint-HOMOGENEOUS MRT (2× R32_UINT, distinct uint clears) — both HOMOGENEOUS-format so
  `pass_pso`'s single `rt_fmt` suffices. ⛔ **`pass_pso`/`pso_for` take ONE `rt_fmt` for all N targets** (`:2717`) — a
  HETEROGENEOUS uint@0+float@1 MRT PSO is inexpressible ⇒ the honest SPLIT: per-attachment RTV formats + PSO cache-key
  (content-hash scar) = **14z-4c**, NOT silently narrowed. The `enc_draw_storage_mrt` test helper needs no signature change
  (it feeds `RenderingDesc`) but gets a per-attachment-clear twin so 14z-4b's proofs can author distinct clears.
  **✅ 14z-4b DONE + gated 2026-08-11.** Engine: `ClearKind` moved to raster_context.hpp + `AttachmentClear` struct; both
  backends' `draw_storage_mrt` take `const AttachmentClear*` (per-attachment kind/color/uint/blend) with the WBOIT identity
  OVERRIDE; `command_lowering.hpp:355` builds the array. PROOF (strengthened the RAF-7 MRT gate, `tests/render-graph/
  test_frame_graph_gpu.cpp::run_mrt_gpu`, BOTH backends): c0 clears BLUE + c1 clears RED (DISTINCT) — the corners now read
  blue@0 / red@1 (a shared-clear broadcast would match them), while centres stay RED@0/GREEN@1 (behaviour preserved). Gate:
  win-debug raf7 both backends + Vulkan [frame-graph] 52 cases/884 assertions; win-asan raf7 both; linux-gcc-debug/asan raf7
  Vulkan(llvmpipe) + ceir render 13/13 (gcc -Werror compiled the enum move + signature); LLVM-20 tidy(6). ⛔ the UINT arm is
  IMPLEMENTED (Vulkan `VkClearValue.uint32`; DX12 the `draw_visbuffer` bit-reinterpret) but NOT yet proven (the float-distinct
  proof exercises only kind=Float) — proven at 14z-4c with the uint-homogeneous MRT. ⛔ this proof is at the RENDER-GRAPH
  layer (where `draw_storage_mrt` lives); the CEIR→MRT integration (`execute_render_frame` + the binding resolver) is 14z-4c.
- **14z-4c — PRE-SPLIT into 4 dependency-ordered workloads (advisor 2026-08-11; 2–3 ticks, NOT one).**
  **(c1) CEIR binding resolver + CEIR→MRT integration** (FIRST — the band's actual subject; needs only landed engine work):
  wire `materialize_draw_packet` to resolve the CEIR draw's buffer operand → an `IStorageBuffer` → `RasterDrawPacket.bindings`
  (the 14z-1 named-forward), `execute_render_frame` declares the storage buffer as a pass `reads`; a CEIR
  `render.scope{2 color_attachments, DISTINCT clears} { render.draw(%vbuf) }` (StoragePull) through `execute_render_frame`,
  RED@0/GREEN@1 + distinct corners, both backends. Fold the **2-SCOPE frame-graph test** here (triangle→A, triangle→B —
  exercises the unproven N-scope reserve-then-push closure path). ✅ **the 2-SCOPE test is DONE + gated 2026-08-11** (a
  separable increment, no engine change): `build_ceir_two_scope_render` + a per-op `CeirTargetMap`/`ceir_mapped_target_resolver`
  + `run_ceir_render_frame_mapped` (shared header, refactored to `ceir_fresh_render_module` + `ceir_append_triangle_scope`);
  scope0 clears BLUE / scope1 GREEN drive TWO frame-graph passes into TWO targets — DISTINCT corners (blue@A/green@B) prove
  per-scope target+clear routing, both backends; the reserve-then-push closure array is ASan-CLEAN. Gate: win-debug/asan
  539/539, linux-gcc-debug/asan 531/531, tidy(3). ✅ **the CEIR BINDING RESOLVER ENGINE INFRA is DONE
  2026-08-11 (uncommitted, win-debug-verified, behavior-preserving)**: `render_draw_binding_start` EXPORTED from crd-ceir
  (context.hpp + a thin public wrapper over the internal `draw_shape_of` — one source, no second table);
  `RasterBindingResolveFn(const Value*,void*)→IStorageBuffer*` + OPTIONAL default-nullptr `binding_resolver`/`binding_user`
  threaded through `materialize_draw_packet` (resolves the variadic binding tail → `ResourceBinding{Object,StorageBuffer,
  ordinal,buffer}`; a null binding → typed FALSE, never silent-skip; slot = ordinal, not 0), `execute_render_lowered`,
  `execute_render_frame`. ⛔ storage-buffer CONTRACT documented (NOT frame-graph-tracked; pre-upload; no same-frame write;
  compute→raster hazard = 15/16, NOT a speculative `reads` import). Existing 14z tests (49 assertions) still pass. ✅ **c1 COMPLETE +
  gated 2026-08-11 — THE FIRST CEIR PROGRAM TO DRIVE MRT.** `build_ceir_mrt_render` (`render.scope{color0{BLUE}, color1{RED}}
  { render.draw(3,1, %vbuf) }`) + `ceir_render_binding_resolver` (identity sentinel) + `run_ceir_render_frame_mrt`; the
  storage-pull VS `build_vertex_pull_vs` + `build_gbuffer_two_output_fs` (RED@0/GREEN@1); 36 verts uploaded. Drives
  `draw_storage_mrt` through `execute_render_frame` — color0 centre RED + corner BLUE, color1 centre GREEN + corner RED
  (distinct corners ⇒ per-attachment clears [14z-4b]; distinct centres ⇒ correct MRT ordering; the %vbuf binding ⇒ the binding
  resolver [14z-4c] ⇒ the StoragePull MRT arm), both backends + llvmpipe, ValidationCapture-silent. This composes ALL of 14z:
  the 14z-4a frame-recording executor + the 14z-4b per-attachment clears + the 14z-4c binding resolver. Gate: win-debug/asan
  541/541, linux-gcc-debug/asan 532/532 (gcc -Werror=switch clean on the crd-ceir export + the rebuilt verifier suite
  crd-ceir-tests intact — context.cpp is core), LLVM-20 tidy(7). ⛔ **SINGLE-BINDING proven only** — the identity-sentinel
  resolver returns the one buffer for ANY operand + the proof binds exactly ONE buffer, so `slot = ordinal` and the
  operand→buffer MAPPING are structurally in place but OBSERVATIONALLY untested (a resolver that ignored its operand would
  pass this proof). First real multi-buffer consumer: draw_indexed's index buffer (14z-6) — not overclaimed here.
  **(c2) uint-HOMOGENEOUS MRT proof** (proves the
  IMPLEMENTED uint arm): 2× R32_UINT targets + a uint 2-output FS + distinct uint clears, both backends (this is where the DX12
  bit-reinterpret uint clear is first proven for a NON-ZERO value). ✅ **c2 DONE + gated 2026-08-11 — AND caught+fixed a real
  DX12 engine bug.** Feasibility (checked first): the KIR GLSL emitter lowers a scalar-uint `out[k]` generically to
  `layout(location=k) out uint o_k;` (no sub-fix); `create_visbuffer_target` is R32_UINT (read_pixel → raw u32) and 2 coexist
  in one `RenderingDesc.color[]` (pass_pso fills all RTVs with the one R32_UINT format). `build_visbuffer_two_output_fs`
  (ids 7/9) + `build_ceir_mrt_uint_render` (2 uint attachments over `type_int(32,false)` images, clear_uint 100/200 — the
  ClearKindFormatMismatch verifier accepts uint-clear⇔unsigned-Int) + the shared `ceir_append_mrt_body` (DRY-extracted from
  the float builder). Drives `draw_storage_mrt` uint arm: color0 centre=7/corner=100, color1 centre=9/corner=200 — distinct
  corners ⇒ per-attachment TYPED (uint) clears; distinct centres ⇒ MRT ordering. ⭐ **ENGINE BUG FIXED (the first NON-ZERO
  DX12 uint clear):** the 14z-4b DX12 uint arm used a BIT-REINTERPRET (`memcpy` v's bits → a float ≈ 0 that ClearRenderTargetView
  value-converts to 0 — corners read 0). c2 caught it; fixed to VALUE-CONVERT (`static_cast<float>(v)` — exact for v < 2^24).
  Vulkan (`VkClearValue.uint32`) was already correct. Gate: win-debug/asan ceir 543/543 + raf7 7/7 (the uint fix left the float
  MRT/WBOIT paths intact), linux-gcc-debug/asan ceir+raf7 539/539 (uint MRT on llvmpipe), LLVM-20 tidy(5). ⛔ NOTE: the
  `draw_visbuffer` comment still claims bit-reinterpret but only ever clears to 0 (both agree) — stale comment, correct code;
  a non-zero visbuffer clear would need the same value-convert (named-forward, no consumer). **(c3) HETEROGENEOUS uint@0+float@1 MRT — ✅ DONE + gated 2026-08-11 (BOTH backends; a real DX12 per-attachment-format engine fix).**
  ✅ pre-checks: `scan_render_region` has NO format-agreement check
  (mixed-format scope accepted); the HLSL emitter types each output independently (`uint o0:SV_Target0` + `float4 o1:SV_Target1`)
  — mixed FS expressible. ✅ `build_gbuffer_uint_float_fs` + `build_ceir_mrt_mixed_render` (color0 R32_UINT+uint clear, color1
  RGBA8+float clear). ✅ **VULKAN mixed proof PASSES (CONTROL): color0(uint) 7/100, color1(float) GREEN/BLUE — Vulkan derives
  formats from the per-attachment VIEWS ⇒ heterogeneous MRT works with ZERO engine change ⇒ c3 was DX12-ONLY.** ⭐ **DX12 ENGINE
  FIX LANDED (per-attachment RTV formats):** the DX12 `pso_for`/`build_graphics_pso` broadcast ONE `rt_fmt` to all RTVFormats —
  a mixed scope (R32_UINT@0 + RGBA8@1) got the wrong PSO. Fix: `build_graphics_pso` gained `const DXGI_FORMAT* rt_fmts=nullptr`
  (`RTVFormats[i] = rt_fmts ? rt_fmts[i] : rt_fmt` — nullptr broadcasts, byte-identical for the homogeneous path); `pso_for`
  gained the same param, materializes the full `DXGI_FORMAT fmts[8]` + `all_default` (fast-path/u32-key use `all_default` — the
  key stays a prefilter, content-hash scar), the cache compare adds `std::memcmp(m_cache[i].rt_fmts, fmts, nf*sizeof(...))`,
  store `std::memcpy`, `PsoCacheEntry` gained `DXGI_FORMAT rt_fmts[8]{}`; `pass_pso` threads it; `draw_storage_mrt` builds the
  array from each target's `color_format()`. ✅ **DX12 mixed device proof PASSES: color0(uint) 7/100, color1(float) GREEN/BLUE**
  (15 assertions). ⚠ `build_mesh_pso`/line ~241 has a 2nd RTFormats fill site hardcoding kColorFormat — NAMED-FORWARD (no
  mesh-MRT consumer yet; 14z-7). Gate (4 configs): win-debug ceir **545/545** + raf7 7/7 + dx12-raster 100 cases (regression-free),
  win-asan ceir **545/545** + raf7 (pso_for ASan-covered via the mixed test + raf7 + c1/c2), linux-gcc-debug **540/540**,
  linux-gcc-asan **540/540** (Vulkan mixed on llvmpipe), LLVM-20 tidy(5). ⭐ **14z-4 (MRT + typed clears) COMPLETE — the CEIR
  render executor drives heterogeneous per-attachment-typed MRT on BOTH backends through the frame-recording drive.** Legacy note below (superseded by this split): The MRT arm (`command_lowering.hpp:355`, `r.color.size() >= 2`) requires StoragePull +
  a storage binding (it derefs `*buf`) → the CEIR MRT draw MUST author ≥1 binding (14z-3's `>2 operands ⇒ StoragePull`
  refinement handles it) — this pulls the 14z-1 BINDING RESOLVER into 14z-4 scope. Proof: 2 color targets with DISTINCT
  per-target clears + `build_gbuffer_two_output_fs` (RED@0 / GREEN@1 — the "bound one target twice" catcher) → read each
  target's centre (RED/GREEN, correct order) + corner (its OWN clear, proving per-target clears). ⭐ Uint-mixed attachment:
  the plainest reading of "typed clears per-target" is the 14z-1 unit shape (uint@0 + float@1 + depth) ON DEVICE — feasibility
  first (can the KIR emitter produce a MIXED uint+vec4 output FS? the visbuffer FS is uint-only). If yes, fold into 14z-4c; if
  no, that is ANOTHER engine gap — record + decide, never narrow silently.
- **14z-5 — DEPTH-ONLY. ✅ DONE + gated 4 configs 2026-08-11 — the device proof caught+fixed TWO real 2-backend ENGINE BUGS.**
  Gate: win-debug/asan ceir-14z 18/18 + raf7 7/7 + REN-3 178/178 + REN-38 117/117 + command-encoder 4/4; linux-gcc-debug/asan
  688/688 (1 VRS skip) — LeakSanitizer CLEAN; LLVM-20 tidy(6). ⭐ **BUG 2 (a pre-existing leak the broader linux-asan run
  surfaced): `free_transients` deleted the transient IMAGE wrappers (`delete n.texture`) but NOT the transient BUFFER wrapper
  (`new VulkanTransientBuffer`/`new Dx12TransientBuffer` from build_tail) — a 24-byte leak PER GRAPH REBUILD, on BOTH backends
  (LeakSanitizer named it via REN-38 authored graphs + SceneRenderer::render; Windows ASan has no LSan so DX12's twin had NO
  gate — fixed for symmetry anyway).** Fix: `delete n.buffer; n.buffer=nullptr;` gated on `n.transient` (imported nodes BORROW
  n.buffer), mirroring the image path + the vkDestroyBuffer/resource.Reset guard. ⛔ **the pre-code check (b) was WRONG in a
  load-bearing way (BUG 1): the LOWERING calls `set_next_draw_load_depth`,
  but `record_plain` — the FRAME-RECORDING path for `draw`/`draw_depth` (the None-geometry fullscreen depth draw scope 1 routes
  through) — HARDCODED the depth loadOp to CLEAR (Vulkan `VK_ATTACHMENT_LOAD_OP_CLEAR`, DX12 an unconditional `ClearDepthStencilView`),
  IGNORING `m_next_load_depth`.** The 8 sites that honor the flag are the SCENE verbs; `draw_depth` was never wired because no
  prior consumer drove it with a depth LOAD (the cook-only-gates / no-consumer blind spot). ⇒ scope 1 re-cleared the depth to 1.0,
  so 0.75 ≤ 1.0 everywhere → RED everywhere (the occlusion proof's designed-for failure mode fired FIRST run). FIX: `record_plain`
  reads `m_next_load_depth` for the depth loadOp + consumes it (`vulkan_raster_context.cpp` record_plain, `dx12_raster_context.cpp`
  record_plain — both mirror the existing scene-verb idiom). ⭐ so 14z-5 is NOT a pure-harness slice — it's a gold-standard engine
  fix surfaced by the device proof, exactly the user's "solve bugs immediately" mandate. Regression-clean (win-debug): ceir 547/547
  (+2 new), raf7 7/7, REN-3 178/178 (the depth/shadow prepass pipeline, unaffected), command-encoder 4/4 (the draw_depth gate).
  ⭐ **ADVISOR VERDICT: prove depth-only by DEPTH-TEST OCCLUSION (attachment-only observation), NOT
  by mirroring REN-3.1's shadow-SAMPLE into CEIR.** Why occlusion: (1) the subject is CEIR driving `draw_storage_depth_only`
  through the frame drive; a shadow-sample mirror needs a sampled-texture + comparison-sampler binding the `RasterBindingResolveFn`
  (StorageBuffer-only) does NOT have — vocabulary-without-a-consumer (the 12a/14d rule; 14z-6's 2nd binding consumer is an index
  BUFFER, not a texture). (2) A scope-1 SAMPLE of a scope-0-written map needs a real fg `reads()` edge for a CEIR-bound resource
  — which reopens the "CEIR-bound resources are NOT fg-tracked; hazard = 15/16" contract closed at 4c. Occlusion keeps every
  cross-scope dep in ATTACHMENTS (which `execute_render_frame` already imports + WAW-orders) + never samples (the NDC±Y RTT scar
  never bites). (3) REN-3.1 already proves the raster-layer sample round-trip — don't re-prove it. **PROOF (one CEIR program, TWO
  scopes over ONE `create_color_depth_target` T):** scope 0 = `render.scope(depth_attachment{→T, clear_depth=1.0}){ render.draw(3,1,%vbuf) @depth }`
  — the shared StoragePull triangle drawn with `build_depth_only_const_fs(0.5)` (n_out=0: the ⛔ depth-only-≠-forward scar honored
  BY CONSTRUCTION — a genuine depth-only program, never a borrowed color one) ⇒ depth 0.5 where the triangle covers, 1.0 elsewhere.
  scope 1 = `render.scope(color_attachment{→T, clear BLUE}, depth_attachment{→T, load=LOAD}){ render.draw(3,1) @cd }` — a fullscreen
  (None geometry) draw, `build_color_depth_fs(RED, frag_depth=0.75)`, depth-test LessEqual. Read back T's COLOR: **centre = BLUE**
  (0.75 ≤ 0.5 FAILS ⇒ clear survives), **corner = RED** (0.75 ≤ 1.0 PASSES ⇒ fragment written). Failure modes discriminate: a
  verb no-op OR a depth-re-clear-to-1.0 (the `set_next_draw_load_depth` LOAD path broken) flips centre→RED; a clear-to-0 flips
  corner→BLUE. Values sit OFF the compare boundary (the REN-3.1 ramp lesson). **The four checks (each a candidate engine gap):**
  (a) `draw_storage_depth_only` requires `t.has_depth()` ⇒ takes the bundled color+depth T — ✅ no gap. (b) color-CLEAR + depth-LOAD
  is `set_next_draw_load_depth(true)` at `command_lowering.hpp:152` driven by the per-attachment LoadOp, consumed by `m_next_load_depth`
  in every depth record path (8 sites) ⇒ the materializer's `depth.load` reaches the verb — ✅ no gap. (c) `import_target` DEDUPES
  by pointer on BOTH backends (`vulkan_raster_context.cpp:7657`, `dx12_raster_context.cpp:6815`) ⇒ T imported 3× (scope0-depth,
  scope1-color, scope1-depth) collapses to ONE FgImage, no aliasing — ✅ no gap. (d) the GLSL/HLSL emitters type color outputs
  (`layout(location=k) out`) and `frag_depth` (`gl_FragDepth`/`o_depth`) as INDEPENDENT paths; validation only requires frag_depth
  be a float scalar ⇒ an FS with n_out=1 AND frag_depth is expressible — ✅ no gap. **HARNESS (test-only):** `build_color_depth_fs`
  (ckir_raster_triangle.hpp: n_out=1 RED + frag_depth); `build_ceir_depth_occlusion_render` + a per-op `CeirProgramMap`/`ceir_mapped_program_resolver`
  (the two scopes use DIFFERENT programs — the `CeirTargetMap` twin) + `run_ceir_render_frame_depth` (ceir_render_triangle.hpp);
  the target resolver is the identity sentinel (all 3 attachments → T). Device tests in test_ceir_render_{vulkan,dx12}.cpp. Gate:
  4 configs + tidy. ⛔ `read_only` depth (per-draw depth-WRITE disable, RasterState) stays named-forward (no consumer this slice).
- **14z-6 — INDEXED-INDIRECT(-count). ✅ DONE + gated 4 configs 2026-08-11 (c1 + c2; advisor-consulted).** Gate: win-debug/asan
  ceir 551/551 + 14z-6 4/4 (opgen drift clean); linux-gcc-debug/asan 543/543 (Vulkan on llvmpipe) — LeakSanitizer CLEAN; LLVM-20
  tidy(6). ✅ **c2 (indexed-indirect-COUNT):** `build_ceir_draw_indirect_count_render` (render.draw_indirect_count(%args, %count,
  %vbuf)) — the materializer's count resolution (op1→`g.count_buffer`, landed in c1) needed NO further engine change. The proof
  runs the SAME program twice: count=2 ⇒ both sub-draws (left+right RED), count=1 ⇒ only sub-draw 0 (left RED, right BLUE). The
  CONTRAST (right RED vs BLUE) proves the device count GATES execution (not merely plumbed); guarded on indirect_count_supported()
  (Vulkan 1.3 core). ⭐ **14z-6 COMPLETE both backends — only 14z-7 (mesh) remains before band-14 closes.** ✅ c1 IMPLEMENTED: `index_offset` attr added
  to render.draw_indirect/_count (opgen regen, drift-clean exit 0); the materializer resolves op0→`g.args_buffer` + reads
  `index_offset`→`g.index_offset` (g.first_draw_index stays 0, 15/16 fwd); harness `build_vertex_pull_drawindex_vs` (X-shift by
  the pushed SV_DrawIndex) + `CeirBufferMap`/`ceir_mapped_binding_resolver` (the FIRST operand-keyed multi-buffer resolver) +
  `build_ceir_draw_indirect_render` + `run_ceir_render_frame_indirect`; device tests both backends (args buffer built as u32
  words per indirect_command_stride/arg_offset — DX12's leading u32 = DrawIndex). ⭐ **PROOF PASSES both backends**: left RED
  (sub-draw 0), right RED (sub-draw 1 ⇒ SV_DrawIndex=1 PUSHED — the REN-40 proof), corner BLUE. ⛔ **advisor's batch-counter
  assertion DROPPED — VERIFIED the REN-40 indirect verb ticks NO multi_(indexed_)batch_count (that counter is the CPU-args multi
  verbs'; the indirect verb at vulkan_raster_context.cpp:4453 has no ++); the POSITIONAL DrawIndex split discriminates every
  no-op mode instead (no-op⇒left BLUE, missing-push⇒right BLUE), and batch-vs-loop is moot on a real device (loop is stub-only).**
  win-debug ceir 549/549 (+2). ⭐ **ADVISOR
  VERDICTS:** (1) **Proof = per-sub-draw DrawIndex POSITIONAL split** (NOT a `first_draw_index` attr — its value is runtime scene
  state, no authored program pins it; 15/16 forward, materializer leaves `g.first_draw_index=0`). `max_draws=2`, args buffer =
  TWO IDENTICAL commands, a VS reading `KBuiltin::DrawIndex` that OFFSETS POSITION (draw 0 → left, draw 1 → right); assert
  left-centre drawn + right-centre drawn + corner clear. If the row isn't pushed → both read DrawIndex 0 → both land left →
  right-centre reads CLEAR. Positional (not colour) so the result is draw-ORDER-independent. This IS the scar's forced-value
  demand (sub-draw 1 pinned to DrawIndex=1). (2) **No non-indexed path** — the engine unified on INDEXED indirect deliberately
  (`draw_storage_multi_indexed_indirect`, args structs binary-identical VK/DX12); author indices `[0,1,2]` in the SAME pull
  buffer after the verts at `index_offset`, gl_VertexIndex receives the index (build_vertex_pull_vs unchanged). (3) **ADD an
  `index_offset` int attr** to render.draw_indirect/_count (HAS a consumer — every authored indirect draw locates its index
  section; symmetric with args_offset/count_offset; optional ⇒ additive, no version bump — the 13c program_interface precedent).
  (4) **Materializer extension** = the whole engine delta: resolve the NAMED buffer operands via the existing `RasterBindingResolveFn`
  — draw_indirect: op0→`g.args_buffer`; _count: op0,1→args/count; read `index_offset`→`g.index_offset`; null→typed fail. Named
  operands do NOT ride `out.bindings` (they're command-source buffers, not descriptor bindings; the tail loop already starts at
  `render_draw_binding_start`). ⭐ **14z-6 is the FIRST REAL MULTI-BUFFER consumer** (args + vbuf from DISTINCT operands) — the 4c
  identity-sentinel resolver (returns one buffer for any operand) now FAILS; needs an operand-keyed `CeirBufferMap`. (5) **Frame-graph
  contract: nothing new** — args/count/index are the 4c storage class (pre-uploaded, synchronous, NOT fg-tracked; GPU-WRITTEN args =
  the 15/16 compute→raster hazard, named-forward). **Pre-code checks:** (a) ✅ `KBuiltin::DrawIndex` EXISTS (ckir.hpp:449) + both
  emitters handle it (GLSL `pc_draw.index+gl_DrawIDARB`, HLSL root constant b7) — NO new emitter work; a DrawIndex-reading VS is
  test-harness only (`build_vertex_pull_drawindex_vs`). (b) ✅ create_storage_buffer args source passes validation (REN-40
  precedent). (c) `indirect_count_supported()` defaults false → the c2 count test needs the capability skip guard (REN-40
  precedent `REQUIRE(indirect_count_supported())`). (d) ⛔ assert `multi_indexed_batch_count()` DELTA==1 — the verbs no-op
  silently on bad state (the 14z-4 discovery) + pixels can't tell the real verb from a fallback loop; the counter can. ⛔ args
  buffer is BACKEND-SPECIFIC layout: `indirect_command_stride()` (20 VK / 24 DX12) + `indirect_command_arg_offset()` (0 / 4 —
  DX12's leading u32 = DrawIndex), mirror test_vulkan_frame_graph.cpp:7407-7420. **SPLIT: c1 = materializer + `index_offset` attr +
  the draw_indirect DrawIndex-split proof (4 configs); c2 = draw_indirect_count with the count DISCRIMINATOR (max_draws=2, count=1
  ⇒ right half must read CLEAR — proves the device count GATES execution).** ⛔ plain render.draw_indexed's index_buffer-operand
  reconciliation is NOT this slice (§169 is indexed-INDIRECT); don't drift.
- **14z-7 — MESH DISPATCH. ✅ DONE + gated 4 configs 2026-08-11 (advisor-consulted; the LAST band-14 shape).** A CEIR
  `render.scope(color{BLUE}) { render.mesh_dispatch(1,1,1) {program=@mesh} }` drives `draw_mesh` (a PROCEDURAL mesh shader, no
  bindings ⇒ Meshlet, buf=null) through the frame graph — `build_triangle_mesh` (the mesh KIR entry, one meshlet) renders the
  shared triangle: centre RED, corner BLUE, both backends, ValidationCapture-silent. ⛔ the mesh program is `create_mesh_program`
  (a DISTINCT factory; draw_mesh no-ops on a non-mesh program via is_mesh()). Mostly a pure-harness slice (materializer→Meshlet→
  draw_mesh + the frame-recording record_mesh path all pre-existed) — `build_ceir_mesh_dispatch_render` + reuses `run_ceir_render_frame`.
  ⭐ **ONE in-scope ENGINE addition (advisor-caught): the mesh_dispatch op is 3D (the compute.dispatch mirror) but every Meshlet
  VERB consumes group_count_x ONLY — a (gx, gy>1, gz) would SILENTLY draw (gx) (the cook-only-gates-ship-impossible shape).** Fix:
  `materialize_draw_packet` returns false (→ UnsupportedCommand, the dynamic-grid precedent) when group_count_y or z folds to ≠1;
  a device-free negative pins it (`test_execute.cpp`). ⚠ **API-STEP-DOWN NAMED-FORWARD: draw_mesh leveled the 3D device APIs
  (vkCmdDrawMeshTasksEXT / D3D12 DispatchMesh are BOTH 3D) down to 1D** — widening the verb to 3D is a signature widen-audit with
  no y/z consumer, the USER's call to schedule (flagged, not done). Guards: Vulkan shader_object()+mesh_shader(); DX12 create_mesh_program
  null = no OPTIONS7 MeshShaderTier. ⭐ **mesh RAN on llvmpipe** (both linux configs, 0.4–0.5s — NOT skipped; the device-free
  lower-shape leg is the all-skip guard regardless). ⛔ `render.mesh_dispatch_indirect` (MeshletIndirect) device consumer =
  NAMED-FORWARD: §169 "mesh dispatch" is the DIRECT op (proven here); the indirect MECHANISM is device-proven at 14z-6. Gate (4
  configs): win-debug/asan ceir 554/554 + 14z-7 3/3; linux-gcc-debug/asan 545/545 (mesh on llvmpipe) — LeakSanitizer CLEAN; tidy(5).
  ⭐ **BAND-14 (CEIR-14z render device proof) is now COMPLETE — every §169 shape proven device-side both backends.**
- Then depth-only (14z-5), indexed-indirect (14z-6, +REN-40 DrawIndex push), mesh (14z-7) — all through the frame-recording
  drive. ValidationCapture around the whole fg lifecycle, error+warning=0 (REN-1 precedent). Band-14 closes with NO
  name-forward. This does NOT replace CEIR-15 (FrameGraph unification — authored `FrameGraphDesc` ↔ `ceir.frame`) / CEIR-16
  (Executor migration): those remain the larger authored-graph↔CEIR efforts; the 14z frame-recording executor is their
  device-proof foundation.
