# Memory reference: rendering

<!-- doc-role: reference -->
> Technical reference; verify dated claims against current contracts/source. Current work: [ROADMAP](../../ROADMAP.md); current rules: [AGENTS](../../../AGENTS.md).

> Reference corpus, consolidated 2026-09-12; not a live tracker. Read [AGENTS](../../../AGENTS.md),
> [MEMORY](../../../MEMORY.md) and [ROADMAP](../../ROADMAP.md) for current rules/status.
> Dated state, loop grants, tool paths and schedules below are historical. Reusable engineering lessons remain
> applicable unless superseded by current instructions. Retrieve one named record; do not load this whole file on entry.

<a id="memory-feedback_a_perf_flag_that_can_measure_an_empty_frame_must_exit"></a>
## feedback_a_perf_flag_that_can_measure_an_empty_frame_must_exit

---
name: feedback_a_perf_flag_that_can_measure_an_empty_frame_must_exit
description: "A benchmark arm whose asset/graph fails to install must EXIT, not log an error and continue - an empty frame produces a spectacular number that will later be quoted as a result"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: dd844f83-0821-41ab-8607-fa93ccf37a76
  modified: 2026-07-30T19:35:07.122Z
---

**Any flag that switches a measured arm must REFUSE TO RUN when the thing it switches to did not install.**
Logging `[ERR] no such asset` and continuing is not graceful degradation for a benchmark — it silently measures
a *different, faster, wrong* configuration.

**Why:** measured in Cerid 2026-07-30. `crd-sandbox --gpu-cull` reported **`gpu 0.344 ms` at ONE MILLION
instances** — believable-looking, ~250× the real number, and an EMPTY CANVAS. `forward_csm_gpu.frame.toml` ships
as a FILE and is not in the built-in pack, so without `CRD_ASSETS_DIR` the frame graph never installed: no cull
passes ran, every GPU-written indirect command stayed at the reset's zero instance count, and nothing was drawn.
The `perf:` line, the pass count, and the frame all looked plausible. The only instrument that could see it was
the arm that compares the DEVICE's per-view survivor counts with the CPU's (`--gpu-cull-verify`), which printed
`gpu=0 cpu=6714` five times. Same failure shape as the `bounds_off = 104` scar: a plausible artifact with a clean
log.

**How to apply:** when adding a `--feature` flag whose whole point is a performance A/B, make the install path
`if (!install()) { log the reason AND the fix; return non-zero; }`. Then, before quoting any board, sanity-check
the arm against a physical floor — GPU time that falls by more than the change could possibly explain is an
empty frame until proven otherwise, and a count/parity readback is what proves it. Related:
[feedback_declared_header_words_must_be_validated_at_cook_time](workflow-and-correctness.md#memory-feedback_declared_header_words_must_be_validated_at_cook_time),
[feedback_multi_pass_scene_draws_must_load_not_clear_smoke_is_pixel_blind](workflow-and-correctness.md#memory-feedback_multi_pass_scene_draws_must_load_not_clear_smoke_is_pixel_blind),
[feedback_gates_run_configs_the_app_never_ships](workflow-and-correctness.md#memory-feedback_gates_run_configs_the_app_never_ships).


<!-- end-memory:feedback_a_perf_flag_that_can_measure_an_empty_frame_must_exit -->

<a id="memory-feedback_authored_asset_slice_done_only_when_cpp_deleted_and_renders"></a>
## feedback_authored_asset_slice_done_only_when_cpp_deleted_and_renders

---
name: feedback_authored_asset_slice_done_only_when_cpp_deleted_and_renders
description: "An \"author it as data\" slice is done only when the C++ it replaces is DELETED and the engine still RENDERS — cook-layer gates miss every image-destroying bug"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: a3482f73-d858-400b-816d-942216e20052
  modified: 2026-07-26T22:11:54.748Z
---

When a capability moves from C++ into an authored asset (`.frame.toml` · `.crdt` · `.crdm` · `.crdv` · `.crdl`),
the slice is **not** done when the asset cooks. It is done when the C++ it replaced is **deleted** and the engine
still renders.

**Why:** REN-38's C/D/E bands were all closed with green cook gates while `scene_renderer.cpp` still rendered from
`scene_build_surface` and three hand-written VS builders. The assets were authorable and checked; nothing used
them. A post-band audit (grep for callers outside the cooker's own module) is what found it — three modules with
**zero** external callers.

Worse, a *second* material vocabulary was live the whole time: `assets/materials/*.mat.toml` → GLSL files, cooked
by its own asset-cooker handler. Two vocabularies for one thing is not tidy-up-later redundancy; it is a silent
decision about which one is real, and the old one wins by default.

**How to apply:**
- Before closing an "author it as data" row: `grep -rl <cook_fn>` outside the module. Zero callers = not done.
- The deletion is the proof. If the old builder still compiles, the claim is one layer short of where it matters.
- **Cook-layer gates and render gates find different bugs.** Every defect in that band that could destroy an
  image — a varying pair that disagreed, a uv width the emitter rejected, a light direction negated in the wrong
  place — was invisible until something actually rendered.
- An unreachable library is indistinguishable from a missing one: `ckir_lighting.hpp` held 1100 lines of
  gold-standard shading while the ABI carried one directional light. Nothing was unfinished; there was no
  vocabulary to name it.

See [feedback_every_render_pass_through_our_own_frame_graph_machinery](rendering.md#memory-feedback_every_render_pass_through_our_own_frame_graph_machinery),
[project_material_technique_composition_ren37](project-history.md#memory-project_material_technique_composition_ren37), and the session log
`docs/sessions/2026-07-27-ren38-authored-programs.md`.


<!-- end-memory:feedback_authored_asset_slice_done_only_when_cpp_deleted_and_renders -->

<a id="memory-feedback_ckir_emitter_hoist_must_not_lift_materialized_consumers_over_loop"></a>
## feedback_ckir_emitter_hoist_must_not_lift_materialized_consumers_over_loop

---
name: feedback_ckir_emitter_hoist_must_not_lift_materialized_consumers_over_loop
description: "CKIR GLSL/HLSL emitters hoist shared temps to an If-scope top, but a computation reading a WRITTEN buffer (or a materialized/statement-produced value) got hoisted ABOVE its producing loop/store and read stale/zero memory. ONE fix (defer such nodes in hoist) repaired 5 device tests: B18-e hair filter, RT-1 rayQuery emit, IB-1 path tracer, B17-c A-buffer + stochastic."
metadata: 
  node_type: memory
  type: feedback
  originSessionId: bf0ab64f-0cb7-4b04-970c-78c4f58c02b9
  modified: 2026-08-06T07:26:17.835Z
---

**One root cause, 5 device tests (B18-e, RT-1, IB-1, B17-c ×2), both backends.** The CKIR GLSL/HLSL emitters'
`hoist_decls` pre-declares shared temps at the ENCLOSING `If` scope so a temp used in sibling `if` scopes is visible to
both. But `BufferLoad`/`SharedLoad` are INLINE ops, so a computed temp reading one inlines `buf[...]` AT ITS EMISSION
POINT. Hoisting such a temp to the scope top lifts the read ABOVE the loop/stores that PRODUCE the value → it reads
stale/pre-loop memory. Three flavours, all the same bug:

- **B18-e (hair bilateral filter):** the normalise `sum_rgb / sum_w` divisions hoisted above the accumulation loop →
  colour sums read as pre-loop 0 → every pixel rgb=0 (weight, a plain load, was correct). 0.807 LSB off.
- **IB-1 (full path tracer):** the radiance average `buf5[out] * (1/spp)` hoisted above the samples loop → output all
  zero (worst 0.204 = the oracle's own magnitude).
- **RT-1 (inline rayQuery):** distinct symptom, same family — a value-returning STATEMENT (`TraceRayClosest`, or a
  value-returning atomic) materialises its result at its in-order point and has NO pure `rhs` form. A guarded store of
  the result made `hoist_decls` call `decl(result)` BEFORE the statement ran → `decl` tried to emit it as an expression
  → `emit_compute_kernel_glsl` returned FALSE (hard emit failure). B17-c's A-buffer (value-returning atomics) same.

⛔⛔ Guarded (`if tid < N`) kernels are where it bites — the guard's If body is what `hoist_decls` hoists. `n_rays=0`
variants (no guard, no hoist) passed, which is why only the guarded RT tests failed.

**The fix (both emitters, ONE rule):** during the hoist pre-pass (`in_hoist`), `decl` DEFERS a node to its in-order
statement emission when `must_defer(node)` — the node is (or transitively reads) **(a)** a MATERIALIZED value (explicit
`stmt_materialize` OR a value-returning statement's result — trace/atomic), or **(b)** a LOAD of a buffer/shared array
that is WRITTEN anywhere in the kernel (read-after-write sensitive). Deferred nodes emit at their in-order point, after
the producing loop/stores, where the value is correct (and the statement's temp already exists). Pure temps and reads of
UNWRITTEN input buffers still hoist (cross-sibling-scope visibility preserved). Scan `written_buf` from every
Store/Atomic target; mark trace/atomic results in `materialized`.

**How to apply.** GPU-kernel output that is zero/stale while a sibling quantity is right ⇒ dump the emitted GLSL/HLSL and
check STATEMENT ORDER: a temp reading an accumulated buffer must sit AFTER the loop, never hoisted above it. A buffer
load is not a pure value; the emitter's CSE/hoist must respect memory order. A guarded value-returning statement whose
result is stored can make emit return false the same way. Related:
[feedback_ckir_inline_buffer_load_read_after_write](device-programs.md#memory-feedback_ckir_inline_buffer_load_read_after_write), [feedback_oracle_must_round_every_elementary_op](numerics-and-performance.md#memory-feedback_oracle_must_round_every_elementary_op),
[feedback_ckir_if_block_shared_temp_scope_materialize](rendering.md#memory-feedback_ckir_if_block_shared_temp_scope_materialize), [feedback_llvmpipe_campaign_three_kernel_defects](workflow-and-correctness.md#memory-feedback_llvmpipe_campaign_three_kernel_defects).


<!-- end-memory:feedback_ckir_emitter_hoist_must_not_lift_materialized_consumers_over_loop -->

<a id="memory-feedback_ckir_emitter_materializes_multiuse_node_at_first_loop_use_out_of_scope_for_later_loops"></a>
## feedback_ckir_emitter_materializes_multiuse_node_at_first_loop_use_out_of_scope_for_later_loops

---
name: feedback_ckir_emitter_materializes_multiuse_node_at_first_loop_use_out_of_scope_for_later_loops
description: "A kernel-tier node shared across MULTIPLE For loop bodies gets declared inside the FIRST loop by the emitter's decl-DAG-memo, leaving it out-of-scope for later loops; stmt_materialize it at body scope first. Scalar eval + emit-smoke can't catch it; only the device compile does."
metadata: 
  node_type: memory
  type: feedback
  originSessionId: cb9df3b8-2389-479b-9d99-d3d6ce3ba327
  modified: 2026-09-04T19:08:27.535Z
---

The ckir GLSL/HLSL emitter declares a multi-use node at its **first use**. If that first use is
inside a `For` loop body, the declaration lands **inside that loop's braces** — so a node also
consumed by a **later** loop is out-of-scope there, and glslang/dxc reject the emitted source
(→ null pipeline → UnresolvedKernel on device).

**Where it bit:** CEIR-26d-3b generalized softmax to a spec-const `For c in [0,Sk)` loop with THREE
sequential passes (max / exp-sum / normalize). `base = r*Sk` (a Mul) is consumed in all three loop
bodies. First use is inside pass-1's loop → declared there → passes 2/3 see it undeclared.

**Fix:** `g.stmt_materialize(base)` right after `kernel_stmt_mark()` (at BODY scope, before any loop),
freezing it to a register visible to every following loop. This is the loop-carried sibling of the
if-block temp-scope scar [feedback_ckir_if_block_shared_temp_scope_materialize](rendering.md#memory-feedback_ckir_if_block_shared_temp_scope_materialize) — same mechanism
(scope of the first-use declaration), different container (For vs If).

**Why the cheap gates missed it (⛔ the load-bearing lesson):**
- `eval_cpu_kernel` is a **scalar tree interpreter with no lexical scope** — it re-walks the DAG per
  thread and never sees the out-of-scope decl. It stayed green.
- `emit_compute_kernel_glsl/_hlsl` return `true` once a **string** is produced — they do NOT compile.
  So the "emit smoke" REQUIRE passed on source glslang would reject. **Emit-returns-true is not a
  compile check.** (kir cannot link the compiler either: ADR-0103 moved shaderc into the Vulkan
  backend, out of kir — so the device-free reading gate can't add a compile check; the DEVICE legs
  are the compile authority.)
- Only the on-device gate (glslang for Vk / dxc for DX12, which both compile then run) caught it.

**How to apply:** (1) Any authored kernel-tier `.ckir` where a non-leaf node is consumed inside more
than one loop body → `stmt_materialize` it before the loops. (2) Don't trust `eval_cpu_kernel` + emit
REQUIRE as proof a kernel compiles — a multi-loop kernel MUST have a real device compile+run gate.
(3) Single-For kernels are safe by construction (first use == the only loop == in scope).

**For-scope census (2026-09, assets/ckir/):** softmax=3 For sharing `base` → FIXED (Materialize);
skin_compute=2 sequential For (shipped + passing on both backends → its device gate already compiled
it through glslang+dxc); palette_snapshot / spmv_csr / transpose = 1 For each (safe by construction).
No other kernel carries the multi-loop shared-temp hazard.


<!-- end-memory:feedback_ckir_emitter_materializes_multiuse_node_at_first_loop_use_out_of_scope_for_later_loops -->

<a id="memory-feedback_ckir_if_block_shared_temp_scope_materialize"></a>
## feedback_ckir_if_block_shared_temp_scope_materialize

---
name: feedback_ckir_if_block_shared_temp_scope_materialize
description: "CKIR per-output If-store makes emitters declare shared lazy temps inside if-block-0, out of scope in siblings — hoist via stmt_materialize"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 1487a581-3392-44fb-bc9e-ebeaffd19da5
  modified: 2026-08-15T22:45:37.331Z
---

**A CKIR kernel that wraps per-output stores in `stmt_if_begin`/`stmt_if_end` blocks (e.g. the R2C conditional half-store)
breaks the emitters: a value node FIRST used inside if-block-0 gets its lazy `float tN = …;` temp declared INSIDE that block's
`{ }`, so a SIBLING if-block-1 that references the same node (temped=1 → decl skipped) sees `tN undeclared`.** The GLSL emitter
(and HLSL/CUDA/MSL/WGSL — same lazy-temp structure) only hoists the `If`'s CONDITION subtree to the enclosing scope, not the
store body's operands. Shared nodes that bite: the store base (`obase = wrow*stride`) AND the butterfly intermediates the
dft4/dft16 SHARE across outputs (dft4's `t0r` feeds both output 0 and 2).

**FIX — in the KERNEL, not the emitter (backend-agnostic, fixes all 5 at once):** before the per-output `if` blocks,
`g.stmt_materialize(...)` every value the stores touch — the store base(s), the per-butterfly `gidx`/`jidx`, and ALL the
butterfly outputs `xr[leg]`/`xi[leg]`. Materialize emits the temp at that statement's position (the ENCLOSING scope), so the
`if` bodies just reference already-declared temps. Symptom before the fix: `error: 'tN' undeclared identifier` +
`'[]' : scalar integer expression required` at the store lines; a low-numbered `t12 = float cannot convert to int` points at
the store base specifically.

**⭐ GENERALISED 2026-07-19 (B18-c) — it is NOT only `If` blocks: SIBLING `For` LOOPS BITE IDENTICALLY.** Recurred in the deep
opacity map kernel, which has two sequential `stmt_for_begin/end` loops (pass 1 reduces fragment depths to z0; pass 2
accumulates opacity). `fbase = tid*(nfrag*2)` was CREATED at top level but FIRST USED inside loop 1, so its temp was declared
in loop 1's body — and loop 2, a sibling scope, hit `error: 't10' : undeclared identifier`.

**The precise trigger (memorise this, not the examples):** *a value node whose FIRST USE is inside any nested scope (`If` body
OR `For` body) but which is also referenced from a later sibling/enclosing scope.* Creation order in C++ is irrelevant — the
temp is emitted at first USE. **Rule: in any multi-pass kernel, `stmt_materialize` every index base / shared value at top level
immediately after constructing it, before the first loop.** Cheap insurance; the failure is a compile error, never silent.

**How to apply:** any CKIR kernel emitting conditional stores (or any statement whose operands are computed OUTSIDE the
conditional and shared across sibling conditionals) must materialize those operands into the enclosing scope first. Prefer this
over adding a per-output branch-free clamp (which needs a guaranteed pad slot and wastes writes). Found building the R2C real
FFT (`build_fft1d_r2c` in `ckir_fft.hpp`, D-007). See [feedback_bit_exact_fft_crushes_only_when_dram_bound](numerics-and-performance.md#memory-feedback_bit_exact_fft_crushes_only_when_dram_bound),
[feedback_raster_emitters_lag_compute_wire_both_and_uint_suffix](workflow-and-correctness.md#memory-feedback_raster_emitters_lag_compute_wire_both_and_uint_suffix) (the sibling "new construct ⇒ verify every emitter" rule).

**⭐ RECURRENCE 2026-08-15 (CEIR-18p impostor STEP 0.5) — the CULL-COMPACT kernel's DITHER DUAL-WRITE.** `scene_cull_compact.crdv`
(`emit_cull_compact_kernel` in `engine/vertex-cook/src/vertex_asset.cpp`) has TWO sibling `stmt_if` blocks — the PRIMARY store
(`keep`) and the SECONDARY dither write — emitted ONLY when `dither_band>0 && lod_slots>1` (i.e. an LOD policy WITH a dither
band). `in_band_u` (the u32 in-band mask) and `alpha_fine` are shared: first EMITTED use is inside `keep`, re-used by the
secondary write → `'t571'/'t585' : undeclared identifier` at high slot count. **Fix used:** materialize the two STORED VALUES +
the secondary atomic operand — `g.stmt_materialize(primary_val); stmt_materialize(sec_vis); stmt_materialize(sec_val);` — right
after building them, before the `keep` block. Materializing the SINK expressions pulls their whole shared subtrees (in_band_u,
alpha_fine, the packed entries) into the enclosing scope in one shot; cleaner than chasing each leaf. All three are u32 (never the
bool `in_band` — the corollary). ⛔ WHY IT SURVIVED: the SHIPPED `forward_csm_gpu` + `scene_default.crdlod` (7 slots, dither
0.05) path had NEVER been exercised by a scene-render DEVICE gate (no test installed an LOD policy) — the CPU oracle ran the same
graph fine, so only a real backend caught it. The impostor STEP 0.5 gate (first LOD-policy device gate) found the cull kernel
`create_program`-null → `forward_csm_gpu` silently recorded 0 compute passes → LOD/impostor routing dead, no error.

**⛔ TOOLING (the loud-failure fix that made this diagnosable):** `create_program(KGraph,KEntry)` in `vulkan_context.cpp` had
FOUR silent `return nullptr` exits — a GLSL emit that can't lower, and (crucially) `compile_glsl_to_spirv`'s `!ok` whose
`ShaderCompileResult::error_message` (the shaderc line+text, e.g. `ckir_kernel:285: error: 't571' : undeclared identifier`) was
DISCARDED. Added `g_log_vkctx` + a `CRD_LOG_ERROR` at each exit naming the link, the emitted-GLSL byte size, and the shaderc
error. A megabyte-sized GLSL ⇒ the unroll-explosion scar; a plausible size + a shaderc error line ⇒ this scope bug. Without that
log it was a full instrumentation session; with it, one run points at the bug. Diagnose ANY create_program-null with it first.

**⭐ RECURRENCE 2026-08-16 (CEIR-18a-2 Stage 2b light-cull) — SERIAL COMPACTION is a canonical instance.** The re-authored
`build_cluster_light_cull` (`ckir_light_cull.hpp`) compacts per-cluster survivors with a running write cursor `w`: each light's
store lives in `stmt_if(hit)`, and `w = w + hit` threads the cursor from one store-`if` to the next SIBLING, plus the cluster's
list-slot base `lbase` is read by every store-`if` + the padding loop. Emitted GLSL: `t43`(lbase) declared inside light-0's
store-block → `undeclared` in light-1's; same for each `w`. **Fix:** `g.stmt_materialize(lbase)` before the loop AND
`g.stmt_materialize(w)` after EACH `w = uadd(w, cast(hit))` (both u32). ⛔ SAME first-device trap: `eval_cpu_kernel` (CPU oracle)
ran the graph fine; only the Vulkan/DX12 emit caught it (via the null-pipeline SIGSEGV → the emit_ok=false check pointed at it).
**Whole-class rule now proven 3×: ANY compaction / accumulate-across-sibling-if kernel (a write cursor + conditional stores)
needs the cursor AND the store base materialized at the enclosing scope.**

**⛔ COROLLARY 2026-07-21 (B19-a4 tile-ranges kernel) — NEVER `stmt_materialize` a BOOL-typed value.** The materialize temp
is declared `int tN` / `precise float tN` (there is no bool temp type), so `int tN = (a != b);` fails GLSL with
`cannot convert bool to uint/int` and the later `if (tN)` fails with `boolean expression expected`. Comparison results
(`Cmp*`) and their `BitAnd`/`BitOr` combinations are Bool — leave them INLINE (single-use, consumed directly by the `If`
condition or a `Select`), exactly as the tiled-render keep-mask does; only materialize the NON-bool shared operands (the
store index `ridx = t*2` used across two sibling `If` bodies, the loaded u32 keys). So the two rules compose: materialize the
shared u32 index bases, do NOT materialize the bool guards. Both are compile errors, never silent — but the CPU oracle passes
the exact same graph, so ONLY a real-backend emit (Vulkan SPIR-V here) catches it: emit to a device before calling an
If/For kernel done.


<!-- end-memory:feedback_ckir_if_block_shared_temp_scope_materialize -->

<a id="memory-feedback_draw_storage_mrt_needs_coherent_frame_graph_transients_not_standalone_targets"></a>
## feedback_draw_storage_mrt_needs_coherent_frame_graph_transients_not_standalone_targets

---
name: feedback_draw_storage_mrt_needs_coherent_frame_graph_transients_not_standalone_targets
description: "CORRECTED (RAF-7 2026-08-03): draw_storage_mrt / indexed-indirect / shadowed / bindless are FRAME-RECORDING verbs by design (they record into the frame's command buffer; readback is the graph's job). The old 'they need COHERENT transient allocation' claim was a misdiagnosis, and so was 'they need a synchronous verb body' — the real fix was to drive crd-render-graph in ONE-SUBMISSION frame recording, where they all work via their existing bodies."
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 4e6ed9c1-65ab-4a33-8421-6aaa2743a4c2
  modified: 2026-08-03T13:24:47.295Z
---

⛔ THIS SCAR WAS MISDIAGNOSED TWICE. Its original claim — "`draw_storage_mrt` renders attachment 1 BLACK with
standalone `create_color_target` objects, so it needs a COHERENT frame-graph transient-set allocation
(`create_*_transient_set`)" — is **struck in place**. The correct story (RAF-7 close, 2026-08-03):

**The real mechanism.** `draw_storage_mrt`, `draw_storage_multi_indexed_indirect`, `draw_storage_shadowed_depth`,
and `draw_bindless` are **frame-recording verbs**: they record into the frame's ONE command buffer, and the
end-of-frame **readback is the FRAME GRAPH's job** (`VulkanFrameGraph::execute` copies every written imported target
to its host readback). There is nothing wrong with the targets being standalone `create_color_target` objects —
Vulkan dynamic rendering with N independent same-size `VkImageView`s and DX12 `OMSetRenderTargets(n, rtvs, FALSE)`
are both fully legal (no shared `VkDeviceMemory`, no contiguous RTV heap). The "attachment 1 black" the RAF-2 gate
saw was a **missing per-attachment readback**, because the RAF-2 gate drove the verbs SYNCHRONOUSLY (outside a
frame), where a frame-recording verb legitimately no-ops or leaves the readback unpopulated.

**Two wrong diagnoses, both refuted by the code:**
1. "Needs coherent transient allocation" (RAF-2 era) — a HYPOTHESIS never checked against the verb bodies. The
   verbs already bind N independent views; co-allocation is not required on either backend.
2. "Needs a synchronous verb body" (my first RAF-7 read) — I even wrote sync bodies. Also wrong/unnecessary: the
   graph's REAL execution mode is one-submission FRAME RECORDING, not per-verb synchronous submit+wait.

**The actual fix (RAF-7).** `crd::rendergraph::execute_frame` drives the compiled graph as a real frame: it builds a
gpu-context `IFrameGraph`, imports the resolved resources, and records each pass's RECORD function through a
FRAME-RECORDING `ICommandEncoder` — ONE command buffer, cross-pass barriers + readback owned by the frame graph
(mission Gate 7 "one submission where expected"). In that mode all four kinds gate via their EXISTING bodies with
**zero synchronous scaffolding**. Distinguishable gates (MRT att0=RED/att1=GREEN; bindless left=tex0/right=tex1;
shadow left-lit/right-shadowed; indexed-indirect centred triangle) pass on Vulkan AND DX12 in `crd-render-graph-gpu-tests`.

**Rules:**
- A frame-recording verb read back all-zero on a LATER attachment is a **readback/lifecycle** question (was it read
  back? was it consumed by a later pass and left in TRANSFER_SRC?), NOT an allocation-coherency defect.
- To gate a frame-graph-shaped verb, drive the graph in its NATIVE one-submission frame-recording mode — do not add
  synchronous scaffolding to a soon-relocated verb (the user caught this: "if we delete them in RAF-12, why introduce
  things that get deleted?").
- A scar that names a *hypothesized* mechanism ("coherent transients") it never verified against the code will send
  the next agent chasing a phantom. Verify the mechanism in the source before recording it as the cause.
- ⚠ One genuine per-backend difference remains for indexed-indirect: DX12's `ExecuteIndirect` command signature
  prepends a DrawIndex root constant (args = 6 u32 / 24 B), Vulkan reads a bare `VkDrawIndexedIndirectCommand`
  (5 u32 / 20 B). Same verb, same encoder — only the device args bytes differ.

Related: [feedback_draw_mesh_storage_had_no_synchronous_path_both_backends](workflow-and-correctness.md#memory-feedback_draw_mesh_storage_had_no_synchronous_path_both_backends) (a DIFFERENT, real missing-sync-path
case — do not conflate), [project_frame_graph_is_a_recording_mode_of_raster_context](project-history.md#memory-project_frame_graph_is_a_recording_mode_of_raster_context),
[feedback_frame_graph_war_needs_resource_lifetime_gpu_context_twin](rendering.md#memory-feedback_frame_graph_war_needs_resource_lifetime_gpu_context_twin).


<!-- end-memory:feedback_draw_storage_mrt_needs_coherent_frame_graph_transients_not_standalone_targets -->

<a id="memory-feedback_dx12_clearrendertargetview_uint_value_converts_not_bitcast"></a>
## feedback_dx12_clearrendertargetview_uint_value_converts_not_bitcast

---
name: feedback_dx12_clearrendertargetview_uint_value_converts_not_bitcast
description: "SCAR (CEIR-14z-4c 2026-08-11): DX12 ClearRenderTargetView on an INTEGER (R32_UINT/SINT) RTV VALUE-CONVERTS the float[4] to the target format — pass float(id), NOT the bit pattern. A bit-reinterpret (memcpy id's bits→float) is a denormal ≈0 that value-converts to 0. ⛔ ANY clear-to-ZERO test is blind to the difference (both agree at 0), which is exactly why the engine's draw_visbuffer comment claimed bitcast for years + misled the draw_storage_mrt uint arm."
metadata: 
  node_type: memory
  type: feedback
  originSessionId: fade8ea4-87ca-470f-83e0-cdfe82a44e7f
  modified: 2026-08-11T01:14:32.119Z
---

**The mechanism.** `ID3D12GraphicsCommandList::ClearRenderTargetView(rtv, const FLOAT[4], ...)` on an RTV whose resource is
an **integer format** (R32_UINT, R32_SINT, …) **VALUE-CONVERTS** the float array to the target's integer format. So to clear
an R32_UINT target to id `v` you pass `rgba[0] = static_cast<float>(v)` — exact for `v < 2^24` (every id a raster proof uses).

**The trap (why it's a scar, not a lookup).** The WRONG mechanism (bit-reinterpret: `memcpy(&rgba[0], &v, 4)` so the float
carries v's bit pattern) produces a DENORMAL (`v=100` → ≈1.4e-43) that value-converts to **0**. Therefore **every test that
clears an integer target to 0 passes under BOTH mechanisms** — the difference is invisible until a test clears to a NON-ZERO
value. In this engine, `draw_visbuffer` only ever cleared to 0 (its `clear_id` arg was ignored), so its comment asserted
"D3D12 reinterprets the bits" for a long time, untested — and that comment misled the CEIR-14z-4b `draw_storage_mrt` uint arm
into using `memcpy` bit-reinterpret. The CEIR-14z-4c uint-MRT gate (the FIRST non-zero DX12 uint clear: clears 100/200) caught
it — DX12 corners read 0 while Vulkan read 100/200 — and the fix was `static_cast<float>(v)`.

**Rules:**
- To clear a DX12 integer RTV to a non-zero value via `ClearRenderTargetView`, VALUE-CONVERT: `rgba[0] = float(v)`. (For
  exactness beyond 2^24 or full-precision integer clears, use `ClearUnorderedAccessViewUint` via a UAV — not needed for ids.)
- Vulkan is unaffected: `VkClearValue.color.uint32[0] = v` is the exact typed clear.
- ⛔ A test that clears an integer/typed target to **0** proves NOTHING about the clear mechanism — always prove a NON-ZERO
  typed clear (both value-convert and bit-reinterpret agree at 0; only non-zero discriminates).
- ⛔⛔ GENERAL (second instance of the [feedback_draw_storage_mrt_needs_coherent_frame_graph_transients_not_standalone_targets](rendering.md#memory-feedback_draw_storage_mrt_needs_coherent_frame_graph_transients_not_standalone_targets)
  "hypothesized mechanism never verified against code" rule): a COMMENT that asserts a device mechanism its only test never
  exercised (here: cleared to 0) is a latent falsehood that will mislead the next implementer. Verify the mechanism against a
  discriminating test before trusting the comment — and when you disprove one, FIX the comment in place, don't leave it.


<!-- end-memory:feedback_dx12_clearrendertargetview_uint_value_converts_not_bitcast -->

<a id="memory-feedback_every_render_pass_through_our_own_frame_graph_machinery"></a>
## feedback_every_render_pass_through_our_own_frame_graph_machinery

---
name: feedback_every_render_pass_through_our_own_frame_graph_machinery
description: "⛔⛔⛔ TOP RENDERING RULE (user, restated IN ANGER 2026-07-25): WE WILL ONLY USE OUR AUTHORED FRAME GRAPHS. Every technique ships as a .frame.toml ASSET, never as C++ that builds passes. FrameGraphBuilder is for tests/editors ONLY"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: a3482f73-d858-400b-816d-942216e20052
  modified: 2026-07-25T19:11:38.218Z
---

⛔⛔⛔ **USER HARD RULE, restated IN ANGER on 2026-07-25 after I broke it:**
*"THAT'S THE WHOLE REASON WHY WE BUILD THE SYSTEM! WHY WE DID THAT! AND YOU SILENTLY CARRIED ON DOING THIS!
PLEASE WRITE THAT RULE EVERYWHERE YOU CAN! WE WILL ONLY USE OUR AUTHORED FRAME GRAPHS!"*

**Every rendering technique ships as an AUTHORED FRAME-GRAPH ASSET** (`.frame.toml` → cooked `.crdr`), loaded and
run by `execute_frame_graph`. Not C++ that builds passes. Not a `draw_*` sequence. An **asset**. Shadow cascades,
sky/LUT bakes, IBL prefilter, HDR + tonemap chains, TAA resolve, GI, OIT, editor overlays, picking, debug viz.
Engine techniques live in the **built-in pack** that mounts first; apps override by shadowing the name.

**⛔ HOW I BROKE IT (do not repeat).** I built REN-36 — schema, cooker, 19 named validations, loader, executor,
`for_each` expansion, lossless TOML emit, both backends bit-identical — and then implemented REN-3.2-b's cascaded
shadow maps as **hardcoded C++ in `scene_renderer.cpp`**: atlas creation, four cascade passes, program selection,
all in the renderer. It satisfied the LETTER of the earlier rule (passes in a frame graph, one submission) while
defeating its PURPOSE: the user cannot change cascade count, resolution, or pass structure without recompiling.
The user found out by inspecting the sandbox and was rightly furious.

**⛔ THE WORDING THAT ENABLED IT IS RETRACTED.** The earlier rule said the two provenances were "equal and
interchangeable — build the description in C++ and run it through the same executor". That was read as licence to
hardcode. Interchangeable means **the asset path must produce identical pixels**; it does NOT mean you may pick
the C++ path for an engine feature. `FrameGraphBuilder` is for **tests, node editors, and runtime-generated
graphs ONLY**.

**THE TEST — apply before calling any rendering work done:**
> Can a user change this technique — pass order, resource formats, cascade count, an inserted pass — by editing
> an asset, WITHOUT recompiling the engine?

If no, it is not done. A step that cannot be expressed is a missing **`FramePassKind`**: add the kind with its own
gate, then author the technique on top of it. Never route around the system.

**Why it is not negotiable:** the whole REN-36 investment exists so applications, tools and agents can invent
rendering techniques without engine changes. Every technique hardcoded in C++ is one the authoring system
provably cannot express, and it makes the contract a lie.

Recorded in `AGENTS.md` (Agent Conduct, top of the rendering rules), `context.md`,
`docs/design/ren-36-authorable-frame-graph.md`, `docs/design/ren-3-lighting-shadow-pipeline.md`, and D-007 rows
100 + 139. Related: [project_obj_torus_normals_unlit_ren3](project-history.md#memory-project_obj_torus_normals_unlit_ren3),
[feedback_gates_run_configs_the_app_never_ships](workflow-and-correctness.md#memory-feedback_gates_run_configs_the_app_never_ships), [feedback_no_followons_implement_every_vendor_feature](workflow-and-correctness.md#memory-feedback_no_followons_implement_every_vendor_feature).


<!-- end-memory:feedback_every_render_pass_through_our_own_frame_graph_machinery -->

<a id="memory-feedback_frame_graph_compute_pass_dispatch_groups_from_cull_groups"></a>
## feedback_frame_graph_compute_pass_dispatch_groups_from_cull_groups

---
name: feedback_frame_graph_compute_pass_dispatch_groups_from_cull_groups
description: "A NEW frame-graph compute pass silently does NOT dispatch if dispatch_groups is derived only from the GPU-cull's count — a device compute pass added outside the GPU-cull graph needs its own groups source"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 192f4d9e-b3f7-485b-a775-5391bcd4183c
  modified: 2026-08-15T23:44:34.494Z
---

**Scar (CEIR-18a-2 Stage 2b-iv, 2026-08-16).** Added a `light_cull` compute pass to a NEW frame graph
(`forward_plus_gpu.frame.toml`) — kernel registered, pass in the cooked graph, ordering edge declared.
It NEVER RAN: `ensure_light_cull_kernel` was never called, so the cluster list it should have written
stayed at its buffer default (0), and the FS read slot 0 for every tile (only the first light showed,
and only where it physically illuminated). No error, no log — a silent no-op.

**Root cause.** In scene_renderer.cpp the per-frame draw-item fill sets `it.dispatch_groups` ONLY on
the GPU-cull path (`gpu_cull_on` → `d.cull_groups`) and the GPU-skin path (`gpu_skinning_on` →
`d.cull_groups`). `d.cull_groups` is the GPU-cull's own group count and is **0 when there is no GPU
cull graph installed**. `forward_plus_gpu` has a compute pass but NO cull passes ⇒ `cull_groups==0` ⇒
`dispatch_groups==0` ⇒ the frame executor treats the compute pass as a zero-grid no-op (the same
mechanism that skips gpu_skin for non-skinned groups).

**Rule.** A frame-graph COMPUTE pass added OUTSIDE the GPU-driven-cull graph will not dispatch on the
inherited `dispatch_groups` — that field is wired to the cull's grid, not to "this pass exists". A new
device compute pass needs its OWN groups source. Fix used: a `device_light_cull` flag detected at
`rebuild_frame_plans` (scan `frame.passes` for `pass_is_compute(p) && pass_str(p, pp::kKernel) ==
"engine://scene/light_cull"`), and a new branch in the dispatch_groups fill that sets
`it.dispatch_groups = 1` for slot-0 items when the flag is on — one workgroup covers the fixed 16
froxels (extra threads/workgroups guard out; a multi-instance group re-runs it as an
identical-inputs→identical-values write race, a named follow-up).

**How to apply.** (a) When a device compute pass renders NOTHING but its kernel would fail loud on a
resolve error, suspect it never dispatched — put a one-line log on the kernel-ensure SUCCESS path; if
that log never fires, the pass isn't in the executed plan (dispatch_groups==0, not a resolve failure).
(b) `dispatch_groups` in this renderer is a per-DRAW-ITEM field SHARED across compute passes walking
the same draw list — two compute passes needing different grids on the same list is a latent conflict
(the single-grid assumption); a genuinely new grid needs its own detection + branch, not the cull's.
Related: [feedback_gpu_frame_graph_requires_device_command_draw_path](rendering.md#memory-feedback_gpu_frame_graph_requires_device_command_draw_path),
[feedback_deleting_the_imperative_fallback_unmasks_a_migrated_null_plan_hole](workflow-and-correctness.md#memory-feedback_deleting_the_imperative_fallback_unmasks_a_migrated_null_plan_hole) (the silent-no-op class).


<!-- end-memory:feedback_frame_graph_compute_pass_dispatch_groups_from_cull_groups -->

<a id="memory-feedback_frame_graph_pass_must_not_read_what_it_writes"></a>
## feedback_frame_graph_pass_must_not_read_what_it_writes

---
name: feedback_frame_graph_pass_must_not_read_what_it_writes
description: "⛔⛔ A read-modify-write is a WRITE with a WAW dependency — declaring it as read+write makes N such passes CYCLE, and the device graph's build() returned a bare `false` so NOTHING recorded while the canvas kept its previous contents (a plausible frame, missing exactly the passes that mattered, clean log). The executor was silently adding the read: every draw item's pull buffer was imported as a graph READ, including on a pass that WROTE it"
metadata:
  node_type: memory
  type: feedback
  originSessionId: ce31551a-2d4a-49f7-9c12-7bb0acdebc75
  modified: 2026-07-29T23:35:23.605Z
---

REN-40-A, 2026-07-30. Six compute passes accumulate into one `cull_args` buffer and compact into one `instances`
buffer. The authored asset first declared `reads = ["cull_args"]` **and** `writes = [..., "cull_args"]` on each —
which the frame cooker correctly rejected as a **dependency CYCLE**: if every view reads what every view writes,
each depends on all the others.

The fix in the asset is that **a read-modify-write is a WRITE**, ordered against the previous writer by
declaration order (the graph's stated tie-break for two writers of one resource). The hazard between consecutive
writers is covered by the dispatching verb's own compute-write barrier
(`SHADER_READ | INDIRECT_COMMAND_READ` in the destination mask).

**But the cycle came back with the reads removed** — because the *executor* was adding them: the frame runtime
imports **every draw item's vertex-pull buffer as a graph READ**, and a GPU-driven cull pass walks that same draw
list to find the buffers it compacts INTO. So the pass was a writer and a reader of one handle, and two such
passes cycle. The rule now: **if a pass already declared a buffer as a write, do not also declare a read of it** —
the write already carries the ordering and the barrier; the read adds nothing but the cycle.

**⛔ The expensive part was not the cycle — it was the silence.** `IFrameGraph::build()` returned a bare `false`
and nobody reported it. `record()` has named rejections for every way an ASSET can be wrong, but the DEVICE build
(topological sort, transients, barriers) had none. The frame then showed the canvas's **previous contents**: a
complete-looking picture missing exactly the passes that mattered, with a clean log. That sent a long hunt into the
cull kernel for a failure one layer below it. `SceneRenderer` now logs `frame graph '<name>' failed to BUILD (N
passes) — nothing was drawn`.

**How to apply:**
- In any graph, model accumulate/RMW as a **write**, and rely on declaration order + the writer's own barrier.
- Audit implicit edges the executor adds on the author's behalf; they can contradict what the asset declared.
- **Every `bool`-returning build/compile step that can fail must report by name.** A false that renders the
  previous frame is indistinguishable from success.

Related: [project_frame_graph_is_a_recording_mode_of_raster_context](project-history.md#memory-project_frame_graph_is_a_recording_mode_of_raster_context) ·
[feedback_every_render_pass_through_our_own_frame_graph_machinery](rendering.md#memory-feedback_every_render_pass_through_our_own_frame_graph_machinery) ·
[feedback_multi_pass_scene_draws_must_load_not_clear_smoke_is_pixel_blind](workflow-and-correctness.md#memory-feedback_multi_pass_scene_draws_must_load_not_clear_smoke_is_pixel_blind).


<!-- end-memory:feedback_frame_graph_pass_must_not_read_what_it_writes -->

<a id="memory-feedback_frame_graph_war_needs_resource_lifetime_gpu_context_twin"></a>
## feedback_frame_graph_war_needs_resource_lifetime_gpu_context_twin

---
name: feedback_frame_graph_war_needs_resource_lifetime_gpu_context_twin
description: The gpu-context frame graph (BOTH backends) had the same WAR-heuristic-masks-cycles bug frame-cook fixed; a fix to one cycle detector must be ported to its twin
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 4e6ed9c1-65ab-4a33-8421-6aaa2743a4c2
  modified: 2026-08-03T01:09:58.322Z
---

The runtime gpu-context frame graph has its OWN cycle detector (`Dx12FrameGraph::build()` /
`VulkanFrameGraph::build()`), SEPARATE from the frame-COOK cook-time detector. A prior session fixed the
"WAR-heuristic masks genuine mutual-RAW cycles" bug in frame-cook (→ 407/407, memory
[feedback_frame_graph_war_needs_resource_lifetime_not_declaration_order](rendering.md#memory-feedback_frame_graph_war_needs_resource_lifetime_not_declaration_order)) but left the SAME bug in both
gpu-context backends: to support the TAA history ping-pong (taa_resolve reads history, taa_store writes it), the
edge rule had been changed from "a reader ALWAYS follows the writer" (`add_edge(w,r)` regardless of order) to a
plain declaration-order WAR heuristic (`w > r ⇒ add_edge(r,w)`, reader-before-writer if declared first). That
UNCONDITIONAL WAR masked real cycles: the REN-1 gates "build() REJECTS a dependency cycle" and "a producer declared
after its consumer runs first" both went RED on BOTH backends (a genuine A-reads-y/writes-x + B-reads-x/writes-y
pair schedules A→B instead of being rejected; a transient producer declared last runs AFTER its consumer).

**Rule:** a read matching a writer declared LATER is a legitimate WAR (edge reader→writer, no cycle) ONLY when the
resource HAS A VALUE at the read: a PERSISTENT image (TAA history — read old, written new), an EXTERNAL buffer
(host-provided), OR some pass WRITES it BEFORE the reader this frame (the two-phase occlusion re-cull rewrites
`instances`/`cull_args`). Otherwise the later writer is the ONLY producer and must PRECEDE the reader (a forward
RAW), and two passes each reading what the other writes is the real cycle. Discriminator on gpu-context nodes:
`ImageNode.own == Own::Persistent` (imported/transient images have NO frame-start value) and buffer `!transient`
(external buffers do). Imported render TARGETS deliberately do NOT count — their contents are overwritten each
frame, which is why the cycle gate uses them.

**How to apply:** when you touch a cycle detector / dependency-ordering pass, remember there are THREE in this repo
(frame-cook + gpu-context ×2) and they must not diverge — fixing one means porting to the twins and running the
REN-1 gates on BOTH backends. A cycle-detection change is verified by the cycle gate AND the producer-declared-last
gate AND the real 22-pass frame still building (TAA ping-pong + occlusion re-cull are the WAR-legitimate cases).
Related: [feedback_frame_graph_war_needs_resource_lifetime_not_declaration_order](rendering.md#memory-feedback_frame_graph_war_needs_resource_lifetime_not_declaration_order), [feedback_rmw_not_rwm](rendering.md#memory-feedback_frame_graph_pass_must_not_read_what_it_writes) (if present), [feedback_dx12_upload_needs_batch_not_per_call_submit_wait](device-programs.md#memory-feedback_dx12_upload_needs_batch_not_per_call_submit_wait).


<!-- end-memory:feedback_frame_graph_war_needs_resource_lifetime_gpu_context_twin -->

<a id="memory-feedback_frame_graph_war_needs_resource_lifetime_not_declaration_order"></a>
## feedback_frame_graph_war_needs_resource_lifetime_not_declaration_order

---
name: feedback_frame_graph_war_needs_resource_lifetime_not_declaration_order
description: Frame-graph cycle detection — a read-before-later-write is a valid WAR only if the resource has a value at that point; declaration order alone masks genuine cycles.
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 984411b4-b778-4edd-b8f9-0024eec2f3e3
  modified: 2026-08-02T23:37:14.833Z
---

In the cook's Kahn-algorithm cycle detector, when pass A READS resource R and a LATER-declared pass B WRITES R, it
is a legitimate WAR (reader-before-writer, edge A→B, no cycle) **only when R already has a value at that point** — R
is host-provided (external buffer/texture/acceleration structure), cross-frame (persistent/ping-pong, e.g. the TAA
history `taa_resolve` reads then `taa_store` writes), OR some pass writes R **before** A this frame (the two-phase
occlusion re-cull writes `instances`/`cull_args` again after the depth prepass read them). Otherwise the "later
writer" is the ONLY producer: a forward-reference RAW where the writer must precede the reader (edge B→A), and when
two passes each read what the other writes that pair is a real cycle the `DependencyCycle` gate must catch.

**Why:** a prior REN-41 WIP rewrote the detector to add WAR/WAW edges (correct — needed so the TAA ping-pong is not
a false cycle) but resolved the reader-vs-writer direction by DECLARATION ORDER for every resource. That silently
masked the genuine mutual-RAW cycle the gate tests (p1 reads b/writes a, p2 reads a/writes b on transients) →
frame-cook 1/407 red, shipped in a WIP commit. Declaration order cannot distinguish "reader wants the old value"
(valid WAR) from "reader wants a value that does not exist yet" (a cycle) — resource lifetime is the disambiguator.

**How to apply:** gate the WAR (later-writer) branch on `has_frame_start_value(R) || written_before(R, reader_idx)`;
else treat as RAW (writer→reader). Classify external/persistent/ping-pong/AS as having a frame-start value; transient/
indirect-args/structured/counter do not (they need an earlier producer). See `feedback_rmw_not_rwm` and
`feedback_ab_pixel_compare_needs_a_deterministic_clock` for adjacent frame-graph-ordering scars.


<!-- end-memory:feedback_frame_graph_war_needs_resource_lifetime_not_declaration_order -->

<a id="memory-feedback_frame_graph_wboit_composite_wrong_for_asymmetric_transparency"></a>
## feedback_frame_graph_wboit_composite_wrong_for_asymmetric_transparency

---
name: feedback_frame_graph_wboit_composite_wrong_for_asymmetric_transparency
description: "RESOLVED (2026-08-06): the 2-pass frame-graph WBOIT was numerically wrong for asymmetric transparency — THREE defects (dropped per-attachment accumulate blend, wrong composite blend, wrong revealage clear). Root-caused + fixed; fused draw_wboit DELETED (RAF-12.4 59/59). Kept as a scar: a symmetric 1-quad gate is blind to all three."
metadata:
  node_type: memory
  type: feedback
  originSessionId: bf0ab64f-0cb7-4b04-970c-78c4f58c02b9
  modified: 2026-08-06T01:20:29.124Z
---

**RESOLVED 2026-08-06 (RAF-12.4 hard tail).** The 2-pass frame-graph WBOIT (`kOitGraph`, REN-38-A12) was numerically
wrong for ASYMMETRIC transparency. Root-caused to **THREE** defects (the memory originally named two + "a further
discrepancy"; the dominant one was the third):

1. **Composite blend inverted.** The `raster.composite` used `BlendMode::Alpha` `{SRC_ALPHA, 1-SRC_ALPHA}`; the composite
   FS emits `vec4(avg, reveal)`, so WBOIT needs the INVERTED `{1-SRC_ALPHA, SRC_ALPHA}` ⇒ `avg·(1-reveal)+bg·reveal`.
   Fix: added `BlendMode::RevealComposite` (enum + Vulkan `blend_equation` + DX12 factor map + cook parse/emit) and
   pointed the template at `blend = ["reveal_composite"]`.
2. **Revealage cleared to 0, not 1.** `dst·(1-src)` from 0 stays 0 forever ⇒ background never revealed. Fix, made
   INTRINSIC not WBOIT-special: `draw_storage_mrt` (both backends) clears a `Multiply`/`RevealageMultiply` attachment to
   the multiplicative IDENTITY **1**, never the pass's single `clear_color`. (The `raster.mrt` pass has ONE clear_color
   for all attachments — correct for accum=0; the per-blend override handles reveal=1.)
3. **⛔⛔ The per-attachment ACCUMULATE blend was DROPPED** — the dominant defect (why RevealComposite made it diverge
   MORE, 66→112 LSB). The RAF-12.4 **F6 encoder migration** of the live MRT path (`frame_runtime.cpp` RasterMrt) built
   the `RenderingDesc` colour attachments with target/load/clear but **never set `c.blend`**, so the accumulate pass
   rendered OPAQUE (each fragment OVERWROTE instead of additive/revealage accumulate). Fix: `c.blend = d.blend[k]`.
   ⛔ The `[frame-graph]` **byte-identical gate is BLIND** to this: NO LIVE frame has a blended MRT pass (velocity prepass
   is single-colour + Opaque; WBOIT is unshipped), so A==A held while the absolute WBOIT pixels were wrong.

⛔⛔ **The 1-quad REN-38-A12 gate was BLIND to ALL three.** At alpha 0.5, `reveal=0.5`, so `avg·(1-reveal)+bg·reveal` and
the inverted form are IDENTICAL, and a single opaque-overwrite still looks plausible. Only an ASYMMETRIC scene (≥2 layers,
DIFFERENT alphas, `reveal=Π(1-aᵢ)` far from 0.5) exposes them. **Gate that landed: REN-38-A12 ORACLE (both backends)** —
the shared asymmetric 4-quad scene (alphas 0.5/0.4/0.3/0.6, reveal≈0.084), per-texel vs `wboit_oracle_pixel`, worst ≤3
LSB (the fused verb met ≤2). This IS the frontier-coverage payoff the fused verb's 4-quad oracle had and the graph gate
lacked.

**Outcome:** the fused `draw_wboit` verb (both backends + its DX12 accum/composite PSO builders + the orphaned
vs/ps_bytecode/device accessors + both fused device-oracle tests) was DELETED — RAF-12.4 is 59/59, ONE authored WBOIT
path. Related: [project_b17_oit_two_tiers_and_rtt_blend_capability](project-history.md#memory-project_b17_oit_two_tiers_and_rtt_blend_capability), [feedback_never_simplify_gate_tests_frontier_always](build-and-verification.md#memory-feedback_never_simplify_gate_tests_frontier_always),
[feedback_bit_exact_blind_to_symmetric_bugs_energy_comp](workflow-and-correctness.md#memory-feedback_bit_exact_blind_to_symmetric_bugs_energy_comp), [feedback_multi_pass_scene_draws_must_load_not_clear_smoke_is_pixel_blind](workflow-and-correctness.md#memory-feedback_multi_pass_scene_draws_must_load_not_clear_smoke_is_pixel_blind).


<!-- end-memory:feedback_frame_graph_wboit_composite_wrong_for_asymmetric_transparency -->

<a id="memory-feedback_frame_pass_field_accessor_and_two_tier_forward_fs"></a>
## feedback_frame_pass_field_accessor_and_two_tier_forward_fs

---
name: feedback_frame_pass_field_accessor_and_two_tier_forward_fs
description: "⛔⛔ CEIR-18a-3 scars — a FramePassDesc field can be an ENUM (set_pass_enum → read pass_u32 vs the enum) OR a STRING (set_pass_str → pass_str); the wrong accessor reads empty/0 and SILENTLY falls to a fallback. AND the renderer cooks TWO forward FS variants (fwd=flat/shadows-off, csm=shadowed) mapped to the two CAPABILITY TIERS (main requires-shadows frame → csm; fallback forward_basic → fwd) — NOT one technique for both. AND every frame-install site re-cooks + needs atomic rollback."
metadata:
  node_type: memory
  type: feedback
  originSessionId: 192f4d9e-b3f7-485b-a775-5391bcd4183c
  modified: 2026-08-16T07:27:43.054Z
---

Three scars from CEIR-18a-3 (make the frame graph's forward-pass `technique` drive the forward FS cook), each caught by a gate — read before touching frame-pass reading or the forward-technique cook in `scene_renderer.cpp`:

**1. ⛔⛔ A `FramePassDesc` field is stored as an ENUM *or* a STRING — use the matching accessor.** `material_pass` is written with `set_pass_enum` and read with `crd::framecook::pass_u32(pass, pp::kMaterialPass, 0) == static_cast<u32>(FrameMaterialPass::Forward)` (see `frame_runtime.cpp` / `frame_asset.cpp:969`). `technique` is written with `set_pass_str`/`pass_technique(StringView)` and read with `pass_str(pass, pp::kTechnique)`. **The wrong accessor does not error — it returns empty/0, and the code SILENTLY falls through to whatever fallback follows.** My first cut read `material_pass` with `pass_str == "Forward"`, matched NOTHING, so `graph_tech` was always empty → every render used the C++ setter fallback → standard_forward and unlit produced identical frames + a bogus technique "resolved". The 18a-3 gate caught it. Before reading ANY frame-pass field, grep how it is SET (set_pass_enum vs set_pass_str) — same disease as [feedback_ceir_attr_reader_must_check_valid_absent_reads_as_zero](execution-ir.md#memory-feedback_ceir_attr_reader_must_check_valid_absent_reads_as_zero).

**2. ⛔⛔ The renderer cooks TWO forward FS variants, mapped to the two shadow CAPABILITY TIERS — NOT one technique for both.** `fwd` (flat / shadows-OFF, default setter `standard_forward`) and `csm` (shadowed, default setter `forward_csm`). Under 18a-3 they come from the two tiers: the main frame with `requires=["shadows"]` (forward_csm/forward_plus) names the SHADOWED technique → `csm`; its shadows-off step-down (the fallback, `forward_basic`) names the FLAT one → `fwd`. When the main frame is itself flat (no `requires shadows`), `fwd ← main.forward.technique` and `csm` is cooked-but-never-selected. ⛔ Collapsing to `csm = fwd = graph_tech` unconditionally cooked the DEFAULT renderer's FLAT variant as `forward_csm`, which samples an UNBOUND shadow atlas with shadows off → black/inverted shading. The per-slice 18a-3 gate stayed green (it only installed shadowed/flat-matching graphs); the FULL MODULE SUITE (REN-2 textured, REN-3.2-b shadow, REN-38, CEIR-18a-2 point-light) caught it — a reminder that a graph-drives-technique change is a whole-module blast radius, run the whole module suite.

**3. ⛔ Every frame-install site re-cooks the FS and needs ATOMIC ROLLBACK.** `impl.frame` is swapped at FOUR sites: `set_frame_graph_toml` (re-cooks ✅ — needs rollback), the RAF-11 `Impl::frame_commit` reload (was plans-only — added the re-cook), `set_asset_root` (name-invariant: default is always forward_csm), and the `set_soft_shadows` tier swap (name-invariant: forward_csm ⇄ forward_csm_moment BOTH name `forward_csm`). The two that can change the technique NAME (`set_frame_graph_toml`, `frame_commit`) stash the previous frame + `frame_overridden` BEFORE the swap and, on a re-cook failure (e.g. a typo'd technique), restore + re-cook the old frame — never a half-installed frame with retired programs. This is [feedback_plan_table_must_rebuild_at_every_frame_install_site](build-and-verification.md#memory-feedback_plan_table_must_rebuild_at_every_frame_install_site) transposed from plans to programs, plus the atomic-rollback discipline [feedback_atomic_rollback_settle_identity_at_begin_and_erase_checks_both_subtree_boundaries](workflow-and-correctness.md#memory-feedback_atomic_rollback_settle_identity_at_begin_and_erase_checks_both_subtree_boundaries).

**How to apply:** any future frame-pass consumer (18a-4, 19c, a new technique/material_pass) — check enum-vs-string first, honour the two-tier fwd/csm split, and route a live frame swap through a re-cook-with-rollback. A field that was "aspirational / does nothing" (the F2 comments in forward_csm/forward_plus `.frame.toml`) becomes load-bearing the instant a consumer reads it — strike the stale comment in place ([feedback_superseded_adr_clause_must_be_struck_in_place](workflow-and-correctness.md#memory-feedback_superseded_adr_clause_must_be_struck_in_place)).


<!-- end-memory:feedback_frame_pass_field_accessor_and_two_tier_forward_fs -->

<a id="memory-feedback_fullscreen_chain_inherits_tonemap_no_flip_and_orientation_gate_needs_absolute_prediction"></a>
## feedback_fullscreen_chain_inherits_tonemap_no_flip_and_orientation_gate_needs_absolute_prediction

---
name: feedback_fullscreen_chain_inherits_tonemap_no_flip_and_orientation_gate_needs_absolute_prediction
description: "Deciding whether a NEW fullscreen post/UI pass needs the backend Y-flip (`!ndc_y_points_down()` at its `cook_stage_named`), and how to GATE orientation on device. The discriminant is NOT 'reads geometry-pass output' — a chain that cooks the same `vertex/post_fullscreen.crdv` (UV=ndc*0.5+0.5) as the shipped tonemap and samples a plain-COLOR RTT inherits tonemap's no-flip (the 'flips together, invisible' case); only pixel-space / clip-derived buffers whose VALUE meaning changes under a mirror need the flip. And an orientation gate must assert an ABSOLUTE predicted value, not merely relative sign-agreement between two read paths. Read before adding `!ndc_y_points_down()` to a new post/UI pass, or writing an A/B orientation gate."
metadata:
  type: feedback
  originSessionId: cb9df3b8-2389-479b-9d99-d3d6ce3ba327
  modified: 2026-09-06T07:55:55.068Z
---

CEIR-31b-4-b-iii-1, 2026-09-06. Ruling on whether the frosted-glass UI fullscreen chain (backdrop_fetch → blur → tint_noise → composite) needs the backend Y-flip. It does NOT. Two generalizable lessons, both refining [feedback_clip_space_y_convention_mirrors_every_rtt_sampled_by_uv](workflow-and-correctness.md#memory-feedback_clip_space_y_convention_mirrors_every_rtt_sampled_by_uv) (the ⛔⛔⛔ NDC±Y memory — read it first: every RTT is stored vertically mirrored between Vulkan and DX12).

**1 — a fullscreen chain INHERITS the flip decision of the shipped pass it shares a VS cook with; the discriminant is the BUFFER's kind, not "reads geometry output."** The flip is folded in per-pass at `cook_stage_named(asset, flip_clip_y)` (scene_renderer.cpp): the shipped tonemap (`ensure_post_program_named`, cooks `vertex/post_fullscreen.crdv` with NO flip arg) is visually correct, and the UI chain (`ensure_ui_program`) cooks the SAME VS with the SAME no-flip call and no other orientation touch (no viewport negation, no sampler-V flip between the cook and `create_raster_program` — VERIFY by reading both, do not assume) ⇒ it inherits tonemap's no-flip. **A reads-based grouping — "backdrop is a scene_color copy = geometry-pass output = the shape of the flip group, so it MAY need `!ndc_y_points_down()`" — is WRONG** (the advisor floated it, then struck it). `post_fullscreen.crdv` DOES bake `uv = ndc*0.5+0.5`, yet needs no flip, because it samples a plain-COLOR RTT: the target and this pass flip TOGETHER and the mirror is invisible (the NDC±Y memory's "ordinary rendering" case). The REAL flip group (taa / velocity_debug / deferred_lighting / rt_composite, which DO pass `!ndc_y_points_down()`) reads *pixel-space / clip-derived* buffers whose VALUE's spatial meaning changes under a mirror — velocity vectors, gbuffer normals/positions reconstructed from FragCoord, RT results indexed by pixel, shadow depth compared against a computed position. Plain color/depth images that are merely resampled (tonemap / moment blur / hzb min / this UI chain) are the no-flip group.

**2 — an orientation gate must pin an ABSOLUTE predicted value, not merely relative sign-agreement.** The natural cheap gate — render a hard-edge geometry quad (world +Y ⇒ TOP half via `look_at(up={0,1,0})`), read it through the effect chain vs through a 1-hop scene read, assert both put the bright half on the same side — only proves the chain is INTERNALLY consistent. A GLOBAL flip (every hop mirrored together) passes it. Add the free absolute tooth: `look_at(up=+Y)` maps world +Y to the TOP rows on BOTH backends (each backend's projection is built for its own ndc_y convention so geometry renders upright), so the 1-hop scene read MUST have `top(row 8).r − bottom(row 56).r > 0` — a PREDICTED value (`scn > 20`), not just a sign to compare. `scn < 0` on either backend = the flip. Combined with `|eff| > 20` and `(eff>0)==(scn>0)`, this forces the effect chain upright too. Gate: `CEIR-31b-4-b-iii-1 GATE (Vulkan/DX12)` (`run_hard_edge_orientation_arm`, test_ui_frosted_glass_gpu.cpp), green both backends.

**How to apply:** before adding `!ndc_y_points_down()` to a new fullscreen post/UI pass, ask what the sampled buffer IS — a plain color/depth image resampled 1:1 (inherit the shipped tonemap's no-flip) or a pixel-space/clip-derived buffer whose value meaning is spatial (flip). When writing an A/B orientation gate, assert an absolute predicted side, never only relative agreement. Related: [feedback_gate_assertions_check_identity_not_category](workflow-and-correctness.md#memory-feedback_gate_assertions_check_identity_not_category), [feedback_ab_pixel_compare_needs_a_deterministic_clock](workflow-and-correctness.md#memory-feedback_ab_pixel_compare_needs_a_deterministic_clock), [scars_render_frame_graph](rendering.md#memory-scars_render_frame_graph).


<!-- end-memory:feedback_fullscreen_chain_inherits_tonemap_no_flip_and_orientation_gate_needs_absolute_prediction -->

<a id="memory-feedback_gpu_frame_graph_requires_device_command_draw_path"></a>
## feedback_gpu_frame_graph_requires_device_command_draw_path

---
name: feedback_gpu_frame_graph_requires_device_command_draw_path
description: A GPU-driven frame graph (device-written draw commands) needs the device-command DRAW path enabled too — selecting the graph without it renders background-only
metadata: 
  node_type: memory
  type: feedback
  originSessionId: bf0ab64f-0cb7-4b04-970c-78c4f58c02b9
  modified: 2026-08-03T17:06:25.609Z
---

Selecting the GPU-driven frame graph (`forward_csm_gpu.frame.toml`) WITHOUT also enabling the device-command draw
path (`SceneRenderer::set_gpu_cull(true)`) renders the **entire frame as background** — 0 instances, though the log
still says "draws 7 inst 5669". The graph's cull passes write draw commands to device memory and its forward pass
draws INDIRECT from them; but the renderer only populates `DrawItem::args` (the device-command wiring) when
`gpu_cull_on`. Graph says "draw from device memory", renderer feeds CPU counts → nothing lands.

**The scar (REN-40-F, fixed 2026-08-03):** `--gpu-skin` in the sandbox selected that graph (line ~896:
`want_gpu_cull || want_gpu_skin ? "forward_csm_gpu..." : nullptr`) but only called `set_gpu_skinning(true)`, never
`set_gpu_cull(true)` — so `--gpu-skin` alone was background-only on BOTH backends (distinct colours ~800 vs ~70k
working), while `--gpu-skin --gpu-cull` worked. Fix: **any flag that selects the GPU graph must enable the
device-command draw path** — the sandbox now does `if (want_gpu_cull || want_gpu_skin) set_gpu_cull(true)`.

**Why:** the GPU graph (passes) and the renderer's draw-command wiring (`DrawItem::args`) are TWO halves of one
device-driven-draw contract that must agree; the graph alone is half the contract.

**How to apply:** when a device-driven draw path is broken ("background only" but draws are logged), check whether the
INSTALLED graph expects device-written commands while the renderer is still recording CPU-count draws — the mismatch,
not the shader, is the bug. This coupling is exactly what RAF-8's resolved `DrawList` + `scene.raster` executor unify;
post-flip the [project_ren38_bindless_multidraw_slices](project-history.md#memory-project_ren38_bindless_multidraw_slices) draw-list resolution owns both halves coherently. Related:
smoke tests are pixel-blind ([feedback_multi_pass_scene_draws_must_load_not_clear_smoke_is_pixel_blind](workflow-and-correctness.md#memory-feedback_multi_pass_scene_draws_must_load_not_clear_smoke_is_pixel_blind)) — this
rendered "PASS" while showing nothing, so verify with a SCREENSHOT + distinct-colour count, not the smoke verdict.


<!-- end-memory:feedback_gpu_frame_graph_requires_device_command_draw_path -->

<a id="memory-feedback_hair_aa_subpixel_position_and_ldr_bilateral"></a>
## feedback_hair_aa_subpixel_position_and_ldr_bilateral

---
name: feedback_hair_aa_subpixel_position_and_ldr_bilateral
description: "Strand AA needs sub-pixel POSITION (bilinear splat), not just coverage alpha; and a bilateral colour term must run in tonemapped space or it silently no-ops in HDR highlights"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: b0138d6a-548b-428b-87b2-fe30c9f36f7c
---

Two traps hit while making B18-e hair actually look right (Lipp 2026 compositing filter + coverage).

**1. Opacity cannot encode position.** Rasterizing each DDA sample into a single `int(px)` quantises every strand
onto the pixel lattice. Adding partial coverage alpha makes strands *translucent* but leaves the staircase exactly
where it was — because the pixel still doesn't know *where inside itself* the strand lies. The fix is a **bilinear
splat over the straddling 2x2 block** (weights `(1-tx|tx)*(1-ty|ty)` about `px-0.5`), which is analytic coverage for
a thin line. Silhouettes and flyaways only resolved after this; the filter alone could not do it.

**2. A bilateral colour term is LDR-calibrated.** Lipp's `sigma_c = 0.9` presumes display-range values. Our peak
radiance was ~20, so in linear HDR every colour difference swamps sigma_c, `exp(-||dC||^2/sigma_c^2)` collapses to
~0, and the filter becomes a **silent no-op exactly where hair is brightest** — the highlights, which is where
aliasing is most visible. Run the filter *after* tonemap+gamma, or rescale sigma_c to the working range.

**Also worth remembering:** a depth-guarded filter deliberately refuses to blend hair with background, so it can
*never* antialias a silhouette. That is correct behaviour, not a bug — silhouette AA has to come from coverage.
If you find yourself widening the depth threshold to soften an edge, you are defeating the guard's purpose
(it exists to stop halos across silhouettes) and the real missing piece is partial coverage.

Related: [project_ocean_visual_gaps_before_b16_close](project-history.md#memory-project_ocean_visual_gaps_before_b16_close) — same pattern, where only a rendered image exposed defects
that thousands of passing unit assertions did not.


<!-- end-memory:feedback_hair_aa_subpixel_position_and_ldr_bilateral -->

<a id="memory-feedback_hair_deferred_one_strand_per_pixel_is_line_art"></a>
## feedback_hair_deferred_one_strand_per_pixel_is_line_art

---
name: feedback_hair_deferred_one_strand_per_pixel_is_line_art
description: "Deferred hair keeps ONE strand per pixel while ~150 overlap — hard-sampling that signal IS line art, and no downstream filter can fix it; measure strands-per-pixel BEFORE tuning anything else"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: b0138d6a-548b-428b-87b2-fe30c9f36f7c
---

A deferred strand G-buffer stores the depth-test winner, so a pixel is shaded from **one** strand. At production
density that pixel actually has ~150 strands overlapping it. Hard-sampling a signal with 150 contributors is
**line art** — not metaphorically; that is what the algorithm computes. The groom reads as hand-drawn pen strokes.

**The arithmetic that finds it in one line** (2026-07-20, B18 showcase, 1440 px wide, flen 1.45, camera 4.2 away):

    px_per_world      = flen * width / (2 * distance)        = 249
    strand_len_px     = strand_len_world * px_per_world      = 746
    total_footprint   = n_strands * strand_len_px            = 129 M px
    strands_per_pixel = total_footprint / covered_pixels     = 148

**How to apply:**
- Compute strands-per-pixel BEFORE tuning anything. I spent four rounds on strand count, fibre radius, resolution,
  filter width and lobe roughness — every one of them improving a signal that was already being destroyed at the
  sampling stage.
- The fix is **supersampling** (render at N x, resolve): N*N independent strand samples per output pixel. Resolve in
  LINEAR light — averaging display-encoded values darkens edges, and a groom is almost entirely edges.
- The Lipp compositing filter **cannot** repair this: it only smears the one sample it was handed and can never
  recover what the depth test discarded. Do not widen it to compensate — see
  [feedback_hair_aa_subpixel_position_and_ldr_bilateral](rendering.md#memory-feedback_hair_aa_subpixel_position_and_ldr_bilateral) for how over-widening produces the "oil painting" look
  (its sigma_par = 4 is calibrated for 1 spp WITH gaps; at density it becomes a directional smear).
- Speckled white glints in the primary highlight are the same disease: each pixel sees one strand's fibre offset `h`
  instead of the average over `h`. Broadening beta_m/beta_n reduces that variance the way more samples would.


<!-- end-memory:feedback_hair_deferred_one_strand_per_pixel_is_line_art -->

<a id="memory-feedback_hair_rt_shading_five_defects"></a>
## feedback_hair_rt_shading_five_defects

---
name: feedback_hair_rt_shading_five_defects
description: "Path-traced hair - five defects that each looked like a \"look\" problem but were real bugs (flat tangents, missing cos-theta, plane-over-hair, binary shadows, MC shadow noise)"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: b0138d6a-548b-428b-87b2-fe30c9f36f7c
---

Rendering the B18-f path-traced hair swatch surfaced five defects. Every one of them first presented as an
art-direction complaint ("looks like pen strokes", "colour is off", "looks like a phantom"). None of them was.

1. **FLAT TANGENTS.** Taking the fibre tangent as `normalize(pb - pa)` from the hit segment is flat shading. The
   tangent steps at every segment boundary and the R lobe is only a couple of degrees wide (β_m ≈ 0.22), so each step
   BREAKS the specular — the highlight comes out chopped into bright dashes exactly one segment long. Fix: a per-segment
   ENDPOINT tangent buffer, central-differenced along the strand, lerped by the hit's axial coordinate. The hair
   equivalent of vertex normals. **Analytic thickness buys an exact silhouette; it does not buy a continuous tangent
   field.**

2. **THE cosθi TERM WAS ABSENT.** `L_o = ∫ f(ωo,ωi)·L_i·cosθi dω`. Computing `f·L` over-weights light arriving nearly
   ALONG a fibre — exactly the grazing geometry that dominates a backlit groom — and the over-counted lobe is not the
   one carrying the fibre's colour, so the hue goes wrong too.

3. **PLANE LIGHT ADDED OVER HAIR.** Adding the lit ground plane wherever the primary ray reached it, without testing
   whether a fibre was in front, stacks a full achromatic term onto EVERY hair pixel. It carries no σₐ, so it destroys
   the colour the BCSDF just computed: the groom washes to flat white and reads as a translucent phantom. Gate on the
   PRIMARY ray's miss flag.

4. **BINARY SHADOW RAYS ARE WRONG FOR HAIR.** Inside a groom every point is occluded from every light, so binary
   occlusion renders the whole interior black and leaves only a silhouette. A fibre is a FILTER, not an occluder: march
   the shadow ray through N fibres multiplying per-channel `exp(-σₐ·2)`. Because σₐ is spectral the shadow is COLOURED
   — the light surviving deep inside blonde hair comes out gold. Measured: blonde's mean radiance rose 25× while
   black's barely moved, which is the physically right split.

5. **BLACK SPECKLE = MONTE-CARLO NOISE, NOT GEOMETRY.** Dots along the fibres looked like a joint/cap artifact. It was
   shadow variance: for dark hair a single crossing absorbs ~93% (`exp(-2σₐ)` ≈ 0.066/0.011/0.0002), so an occluded
   sample is ~0 and an unoccluded one is full — a binary signal at fibre frequency. 192 spp cannot average it. Fixed by
   4× the samples AND tighter light sources (the source's angular radius IS the shadow-ray direction variance).
   ⚠ I first "fixed" this by offsetting the shadow origin along the surface normal — correct in itself, but it changed
   the mean image by 0.0004. The A/B that settled it was rendering once with self-shadowing disabled.

**RADIUS AND COUNT MOVE TOGETHER.** Real hair is ~35 µm radius on a 30 cm strand (~1.2e-4 of length) and is genuinely
SUB-PIXEL at any sane framing — that is the regime, not a problem to design around. But thinning the fibres without
adding many more of them makes the groom transparent. Scalp density is ~150 hairs/cm².

Related: [feedback_ckir_kernel_eval_is_scalar_vec3_evaluates_to_garbage](device-programs.md#memory-feedback_ckir_kernel_eval_is_scalar_vec3_evaluates_to_garbage) (same slice — the compute tier is scalar, so
use `hair_bcsdf_eval_angles`, never the vec3 wrapper), [project_ckir_material_surface_contract](project-history.md#memory-project_ckir_material_surface_contract).


<!-- end-memory:feedback_hair_rt_shading_five_defects -->

<a id="memory-feedback_jobs_parallel_for_frame_arena_exhaustion"></a>
## feedback_jobs_parallel_for_frame_arena_exhaustion

---
name: jobs-parallel-for-frame-arena-exhaustion
description: crd::jobs::parallel_for allocates JobDecl arrays from a per-thread 1 MB frame arena that is NOT reclaimed until frame_reset(). Long-running benchmarks must call frame_reset() between iterations.
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 8232c613-08b5-412f-9d51-73f8c05a65d9
---

**Rule:** code that calls `crd::jobs::parallel_for` many times per
"frame" (benchmarks, batch loops) MUST call `crd::jobs::frame_reset()`
between calls to reclaim the frame arena. Default frame arena is 1 MB
per thread (`Config::frame_alloc_bytes`); each parallel_for consumes
~64 bytes × num_jobs of arena permanently until reset.

**Why:** `parallel_for` is implemented as `frame_alloc(num_jobs *
sizeof(JobDecl), …)` + `run(span<JobDecl>)`. The JobDecl array is
allocated from the **per-thread frame arena**, which is a bump-pointer
allocator with no per-allocation reclamation — it only frees on
`frame_reset()`. So every call permanently grows the arena's
high-water-mark within a "frame".

For typical engine usage this is fine (a frame ends with a frame_reset
at top-of-loop, only a few parallel_for calls per frame). For batch
code (benchmarks, batch CG solvers, batch GEMMs) that calls
parallel_for hundreds of times without an intervening frame_reset,
the arena exhausts → `FrameArena::alloc: arena exhausted` assertion.

Case study 2026-05-19 (hesap v0d-parallelism bench): `gemm_parallel`
emits 4 parallel_for calls per invocation at N=1024 (one per Kc panel).
Across the timing loops at 6 worker counts × ~50 iters × 2 types =
~600 gemm_parallel calls × 4 = 2400 parallel_for calls × ~600 B per
call ≈ 1.4 MB → exhausts 1 MB arena mid-bench.

**How to apply:**
- Production code that loops parallel_for many times: `crd::jobs::
  frame_reset()` between iterations (safe iff all jobs from the
  previous iter are waited-on, which is true for synchronous
  parallel_for + wait pairs).
- Library code that itself calls parallel_for many times in one
  synchronous API call (e.g. `gemm_parallel`): cannot safely call
  frame_reset internally (would invalidate the calling thread's
  outer-scope frame allocs). The caller is responsible for resetting
  between batches OR for raising `Config::frame_alloc_bytes` at
  `jobs::init`.
- Tests using parallel_for repeatedly: same — call frame_reset
  between cases.

Open design question: a future variant of `parallel_for` (or a
`parallel_for_scoped`) could heap-allocate the JobDecl array for
synchronous-by-construction callers, avoiding the arena entirely.
Filed as Cerid debt; not in v0d-parallelism scope.

Related: [jobs-worker-index-aliasing](workflow-and-correctness.md#memory-feedback_jobs_worker_index_aliasing) (sister parallel_for gotcha
hit the same session).


<!-- end-memory:feedback_jobs_parallel_for_frame_arena_exhaustion -->

<a id="memory-feedback_krylov_operator_size_adaptive_and_frame_reset"></a>
## feedback_krylov_operator_size_adaptive_and_frame_reset

---
name: feedback_krylov_operator_size_adaptive_and_frame_reset
description: "For iterative solvers, the spmv LinearOp must be size-adaptive (serial sub-cache / parallel large) and frame_reset after each parallel apply"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: b7a3a237-3bf1-46e1-9658-3a8d33e2d696
---

The matrix-free operator a Krylov solver (CG/PCG/GMRES/…) applies every iteration has TWO non-obvious requirements that decide whether you beat or lose to Eigen. Both surfaced in hesap v4a-2 (CG-vs-Eigen on real SuiteSparse SPD).

1. **Size-adaptive serial/parallel spmv.** Parallelizing the spmv on a SUB-CACHE matrix LOSES: the per-iteration `parallel_for` dispatch costs more than the serial spmv saves (a Krylov loop calls apply thousands of times). `ParallelSparseLinearOp` therefore picks serial SELL below a working-set threshold (≈ L2, `stored*sizeof(T)`) and parallel SELL-C-σ above. Measured flip on bcsstk13 (n=2003, cache-resident): **forcing parallel = 0.67× LOSS vs Eigen; serial-SELL = 1.81× WIN** — same iteration count, pure dispatch overhead. With the adaptive op, Cerid CG wins the whole SPD corpus (1.49–1.86×). This is the v1b spmv regime ([feedback_memory_wall_diagnosis_two_signals](workflow-and-correctness.md#memory-feedback_memory_wall_diagnosis_two_signals)) inherited by CG: WIN at DRAM-bound, the only risk is OVER-parallelizing small work.

2. **`frame_reset()` after each parallel apply.** `crd::jobs::parallel_for` bump-allocates its JobDecl array from the per-thread frame arena (reclaimed only on `frame_reset`). A Krylov solve calling parallel spmv thousands of times EXHAUSTS the arena (`FrameArena::alloc: arena exhausted` assert) unless reclaimed. The parallel operator owns its jobs scratch: call `crd::jobs::frame_reset()` at the end of `apply()` (sanctioned — it's "after the last wait()"). Contract: apply must NOT be nested inside another frame-arena consumer (a parallel_for body); CG/PCG call it serially from the main thread, which is fine.

**Why:** these are the difference between a Krylov solver that crushes Eigen and one that crashes or loses. Eigen's CG spmv is single-threaded scalar; our SELL (serial OR parallel) beats it per-iteration, but only if we don't pay parallel dispatch on tiny matrices and don't leak the frame arena.

**How to apply:** every v4b+ Krylov method consumes the same `ParallelSparseLinearOp` (or any LinearOp following the same two rules). When benching a solver vs a frontier lib, make the operator size-adaptive BEFORE concluding a loss is a kernel defect — a sub-cache parallel-dispatch loss is an operator-config bug, not a memory wall. Also: a release-only `C4189` (assert-only `bool ok`) on the block-inverse needed `[[maybe_unused]]` — win-shipping caught it, win-debug/asan didn't.


<!-- end-memory:feedback_krylov_operator_size_adaptive_and_frame_reset -->

<a id="memory-feedback_layered_depth_atlas_four_scars"></a>
## feedback_layered_depth_atlas_four_scars

---
name: feedback_layered_depth_atlas_four_scars
description: "⛔⛔ REN-3.2 layered depth-array (CSM) atlas: 4 scars — comparison sampler computes compare(REF,STORED); GLSL sampler2DArrayShadow needs vec4 not vec3; DX12 per-slice DSV needs its OWN heap; DX12 transition needs ALL_SUBRESOURCES"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: a3482f73-d858-400b-816d-942216e20052
  modified: 2026-07-25T15:55:19.824Z
---

Landing the CSM depth-**ARRAY** atlas (REN-3.2-a, 2026-07-25, both backends) surfaced four defects. Every one of
them is **silent** — each produces a plausible image with wrong or missing cascades.

**1. ⛔⛔ A hardware comparison sampler computes `compare(REF, STORED)`, not `compare(stored, ref)`.**
With `LessEqual` and ref = 0.5, the slices that PASS are the ones storing *larger* depths (0.6, 0.8). I asserted
the mirror image and the gate failed on all three channels while the implementation was already correct. **When a
shadow gate fails wholesale, check the operand order BEFORE touching the device code.**

**2. ⛔ GLSL folds the compare ref into the coordinate vector, so its width tracks the sampler.**
`sampler2DShadow` takes `vec3(uv, ref)` but `sampler2DArrayShadow` takes `vec4(uv, LAYER, ref)`. `ckir_glsl.hpp`
hardcoded `vec3` — every cascade lookup was a compile error. HLSL needed NO change: `SampleCmp(samp, coord, ref)`
keeps the ref a separate argument, so arrayness rides on the `Texture2DArray` declaration alone. This is the
"new fragment path ⇒ wire BOTH emitters" scar again, in its asymmetric form — check each emitter separately;
"HLSL compiles" proves nothing about GLSL. See [feedback_raster_emitters_lag_compute_wire_both_and_uint_suffix](workflow-and-correctness.md#memory-feedback_raster_emitters_lag_compute_wire_both_and_uint_suffix).

**3. ⛔ DX12: each slice's DSV/RTV needs its OWN descriptor heap.** `Dx12RasterTarget` renders to its heap's
**START** handle, so N descriptors in one shared heap all resolve to slice 0 and every cascade stacks on cascade 0.
Also: the view must be `TEXTURE2DARRAY` with `FirstArraySlice = l` / `ArraySize = 1` — a `TEXTURE2D` DSV over an
array resource silently addresses slice 0. Same for the SRV: `TEXTURE2DARRAY` with `ArraySize = N`, or every
lookup reads slice 0. (No separate heap array is needed to keep them alive — `Dx12RasterTarget` takes the ComPtr
BY VALUE.)

**4. ⛔ DX12: `Transition.Subresource` must be `D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES`, not `0`.** The frame
graph's transition used `0`, which moved only cascade 0 out of `DEPTH_WRITE` and left slices 1..N-1 in the wrong
state when the lighting pass sampled the array. Vulkan's equivalent: `VK_REMAINING_ARRAY_LAYERS`. A layered image
is ONE graph node, so whole-resource transitions are conservative and always correct.

**How to gate this (the part that matters).** Defects 3 and 4 and a wrong SRV dimension ALL collapse the per-slice
results to EQUAL values. So: write a DIFFERENT constant depth per slice, sample every slice with one shared ref,
and pack the results into a **k-bit code in one channel**. Equal-collapse gives code 0 or all-ones; only genuinely
distinct slices give an INTERMEDIATE code. A `CHECK(handle.valid())` or "it rendered something" assert passes
under every one of these bugs. Cap layer counts by **rejection, not clamping** — a truncated cascade atlas renders
missing cascades that look like art direction.

Related: [feedback_multi_pass_scene_draws_must_load_not_clear_smoke_is_pixel_blind](workflow-and-correctness.md#memory-feedback_multi_pass_scene_draws_must_load_not_clear_smoke_is_pixel_blind),
[feedback_dx12_hlsl_masks_type_bugs_run_vulkan](device-programs.md#memory-feedback_dx12_hlsl_masks_type_bugs_run_vulkan), [feedback_shader_capability_needs_device_feature_run_validation](device-programs.md#memory-feedback_shader_capability_needs_device_feature_run_validation),
[feedback_every_render_pass_through_our_own_frame_graph_machinery](rendering.md#memory-feedback_every_render_pass_through_our_own_frame_graph_machinery).


<!-- end-memory:feedback_layered_depth_atlas_four_scars -->

<a id="memory-feedback_materialx_argument_order_in_authored_materials"></a>
## feedback_materialx_argument_order_in_authored_materials

---
name: feedback_materialx_argument_order_in_authored_materials
description: "⛔⛔⛔ .crdm node args are MaterialX-ordered — smoothstep(in, low, high), the VALUE FIRST, not GLSL's (edge0, edge1, x). Writing it GLSL-style cooks a reversed range: lines render narrow+translucent AND a full-alpha 1px sliver appears at the quad edge (the 'dotted line beside the gizmo'). A contains(\"smoothstep\") gate is what let it ship"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: ce31551a-2d4a-49f7-9c12-7bb0acdebc75
  modified: 2026-07-29T11:00:01.804Z
---

**The scar (2026-07-29).** `assets/material/draw_line.crdm` declared its AA falloff as
`op = "smoothstep"; inputs = ["edge0", 1.0, "d"]` — **GLSL order**. But a `.crdm`'s node arguments are
**MaterialX-ordered**: `smoothstep(in, low, high)` — the VALUE first, then the range
(`ckir_nodes.hpp`: *MaterialX smoothstep(in,low,high) == GLSL smoothstep(low,high,in)*). It therefore cooked to
`smoothstep(1.0, d, edge0)`: a **reversed range whose bound is the very quantity being tested**.

Consequences, both visible for months and neither obviously "a shader bug":
- alpha peaked at ~0.65 mid-line and fell to 0 at |cy| > edge0 ⇒ **every debug line rendered NARROWER and
  TRANSLUCENT** than authored (a 5 px line measured 2 px);
- the reversed range divides by `(1 - d)`, so fragments whose interpolated |cy| landed just **past 1** clamped the
  other way to **alpha = 1** ⇒ a **1-px, full-alpha, intermittent sliver hugging one edge of the quad** (one side
  only, by the rasterizer's top-left fill rule). That sliver is what read as *"a dotted jittery line beside the
  gizmo"*. **Nothing was drawn twice — one quad was shaded wrong.**

**Why it survived three earlier "fixes":** the symptom (a thin broken line parallel to a good one) is *identical*
to a duplicate draw, so it kept attracting synchronisation theories (barriers, upload races, stale matrices).
Those turned up three genuine, separately-real defects — which is why each fix "helped" without curing it.

**How to apply:**
- ⛔ In any authored `.crdm`, use **MaterialX** operand order. `smoothstep(in, low, high)`, `clamp(in, low, high)`,
  `mix(fg, bg, mask)`. When in doubt, read `ckir_nodes.hpp` — every node cites its MaterialX signature.
- ⛔ **PRESENCE IS NOT CORRECTNESS.** The gate here was `contains(emitted_glsl, "smoothstep")`; it passed happily
  on the inverted call. Gate **operand ROLES on the cooked graph** (`KNode::c` must be the `Abs`, `KNode::a` the
  `Sub`) — text/presence checks cannot see an argument rotation. Same disease as
  [project_world_normal_varying_reads_zero](project-history.md#memory-project_world_normal_varying_reads_zero) (a number believed without reading what produced it).
- ⭐⭐ **A NEW GATE MUST BE SEEN TO FAIL.** Re-introduce the bug, watch the gate go red, then restore. A gate never
  observed failing is a claim, not a check.
- ⭐⭐ **THE WINNING TOOL: a dedicated close-up probe app** — `crd-gizmo-probe` (a second executable in
  `sandbox/CMakeLists.txt`, so it shares the cooked demo pack) rendering ONE subject on the SAME authored
  frame-graph/technique path, with cut-down switches: `--shapes none` (control — proves whether the artifact even
  comes from the geometry), `--shapes stem` (one primitive), `--freeze` (kills motion theories), `--no-grid`,
  `--width N` (scales the defect until it is measurable). Five minutes of A/B on a big subject beat hours of
  pixel-archaeology on a 2-px feature buried in 10k instances. **Build the probe FIRST next time.**
- Process scars re-paid: a header edit needs EVERY dependent test target rebuilt before believing its red
  ([feedback_header_struct_layout_change_stale_obj_config_specific_fail](workflow-and-correctness.md#memory-feedback_header_struct_layout_change_stale_obj_config_specific_fail)); PowerShell `Set-Content -Encoding
  utf8` put a BOM + mojibake into a source file (the documented PS 5.1 text-I/O scar — use the Edit tool or
  python); `scripts/tidy-files.ps1` drives from `build/win-debug`, so a NEW target must be configured there too or
  the gate reports UNGATED rather than clean.

Related: [feedback_overlay_same_region_reupload_last_write_wins](workflow-and-correctness.md#memory-feedback_overlay_same_region_reupload_last_write_wins) (the three real sync defects found while
chasing this symptom), [feedback_skinned_mesh_missing_normals_nan_black](workflow-and-correctness.md#memory-feedback_skinned_mesh_missing_normals_nan_black) (the probe-ladder discipline).


<!-- end-memory:feedback_materialx_argument_order_in_authored_materials -->

<a id="memory-feedback_one_read_fullscreen_binds_single_texture_not_bindless"></a>
## feedback_one_read_fullscreen_binds_single_texture_not_bindless

---
name: feedback_one_read_fullscreen_binds_single_texture_not_bindless
description: "A raster.fullscreen pass with EXACTLY ONE read binds its texture via draw_textured at set 0/binding 1, not the bindless heap at binding 16. ⭐ CEIR-19b-F2: a 1-read fullscreen that ALSO reads a per-pixel STORAGE buffer now uses draw_textured_storage (texture@1 + storage@0) — the constants operand must be cooked into the PLAIN plan branch (build_fullscreen_ceir), not just the bindless one, else StorageLoad reads set0/binding0 unbound → black"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 4e6ed9c1-65ab-4a33-8421-6aaa2743a4c2
  modified: 2026-08-16T15:39:40.756Z
---

⭐⭐ **CEIR-19b-F2 UPDATE (2026-08-16): the 1-read fullscreen shape now ALSO supports a per-pixel STORAGE buffer.** The RT-shadow COMPOSITE is a single sampled texture (scene_hdr @1) + a storage buffer it StorageLoads per pixel (shadow_mask_buf @0, `mask = intBitsToFloat(sbuf[py*W+px])`). Before F2, the constants/storage buffer was ONLY bound on the bindless/TAA branch (`draw_bindless_storage`), so a 1-read fullscreen FS that StorageLoaded read set0/binding0 UNBOUND → **black everywhere** (even lit pixels; the exact black-frame symptom below, but from the STORAGE side not the texture side). THREE coordinated changes closed it: (1) a NEW verb `draw_textured_storage` on BOTH raster contexts (texture@1 + sampler@2 + storage@0 via `write_scene_textured`'s mold — the RHI-verb-per-binding-shape precedent, `draw_storage_indexed_mrt` mold); (2) the dispatch branch in `engine/gpu-context/include/crd/gpu/detail/command_lowering.hpp` (`plain_sampled_texture(bindings) && first_storage(bindings) → draw_textured_storage`, inserted before the plain `draw_textured`); (3) ⛔ THE REAL ROOT — the CEIR PLAN cook: `build_fullscreen_ceir` (`engine/ceir-gpu/src/render_fullscreen_build.cpp`) emitted the constants StorageBuffer operand ONLY in the bindless branch, so the PLAIN 1-read plan had NO storage operand → `render_materialize` produced no StorageBuffer binding → the encoder never saw one. Added the same `if (desc.constants_param != 0) {declare StorageBuffer@0}` to the PLAIN branch. ⛔⛔ MECHANISM CORRECTION: the executor dispatch is `command_lowering.hpp`'s `CommandEncoder` driven by the CEIR PLAN's draw-op operands (the migrated path), NOT frame_runtime's read-count branch described below — frame_runtime's `add_draws` DOES bind fs_constants into the AuthoredPass PAYLOAD (fs_constants.valid=1), but the payload is DEAD for a migrated pass; the plan drives the packet. Debug recipe that nailed it: probe mask_buf(CPU) vs composite out_lum(GPU) per-pixel — buffer correct + output black = a BINDING gap, not a shader-math bug. ⬇ the ORIGINAL 2026-08-03 scar (the TEXTURE-side black; still valid for the single-texture binding):

A CKIR fullscreen FS that samples the bindless heap at **set 0 / binding 16** (copying the TAA resolve) renders
**fully black** when its authored pass declares only ONE read. The `RasterFullscreen` executor in
`frame_runtime.cpp` branches on read count: `n_sampled > 1` → `draw_bindless` / `draw_bindless_storage` (the
bindless heap at binding 16, declaration order = index); `n_sampled == 1` → the SINGLE-TEXTURE path
`draw_textured` (or `draw_shadow` for a depth read), which binds the texture at **set 0 / binding 1** and the
sampler at **binding 2**; `n_sampled == 0` → `draw` (procedural, no texture).

**Why:** the multi-read TAA resolve (`ensure_taa_program`) samples `fg.texture(0, 16, …, count)` +
`fg.tex_sample_at(tex, samp, uv, ku(idx))` because it has 2–4 reads bound bindless. A 1-read fullscreen pass
(the tonemap `post`, the HZB build, a motion-vector debug encode) is bound the OTHER way. Sampling binding 16 in
a 1-read pass reads an unbound descriptor → black; the frame still reports `draws > 0` from the geometry passes,
so it looks like the fullscreen pass "ran" but produced nothing.

**How to apply:** for a 1-read `raster.fullscreen` program, mirror `body_hzb_build` — `fg.texture(0, 1, DType::F32,
TexDim::Tex2D, false, false, false)` (binding 1, no count), `fg.sampler(0, 2, false)`, and a plain
`fg.tex_sample(tex, samp, uv)` (NOT `tex_sample_at` with an index). Only reach for binding 16 + `tex_sample_at`
when the pass declares >1 read. When a fullscreen encode/blit comes out black, check the read COUNT against the
executor's binding branch before suspecting the shader math. Related: [feedback_pcss_three_defects_unbound_sampler_ring_search_receiver_plane](workflow-and-correctness.md#memory-feedback_pcss_three_defects_unbound_sampler_ring_search_receiver_plane)
(an unbound sampler dims/blackens a plausible frame), [project_frame_graph_is_a_recording_mode_of_raster_context](project-history.md#memory-project_frame_graph_is_a_recording_mode_of_raster_context).


<!-- end-memory:feedback_one_read_fullscreen_binds_single_texture_not_bindless -->

<a id="memory-feedback_probe_needs_control_and_shadowed_program_ignores_frame_technique"></a>
## feedback_probe_needs_control_and_shadowed_program_ignores_frame_technique

---
name: feedback_probe_needs_control_and_shadowed_program_ignores_frame_technique
description: "⛔⛔⛔ A shader-modifying probe is WORTHLESS until a CONTROL with a stated expected value passes — mine silently did nothing because with shadows active the renderer draws with its SHADOWED program and IGNORES the frame asset's declared technique. Plus: DX12 ignores `depth_buffer = true` on colour transients (zero companion-depth code); a backend whose pixels don't change when the declared depth state changes is not testing depth"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: ce31551a-2d4a-49f7-9c12-7bb0acdebc75
  modified: 2026-07-29T12:18:45.299Z
---

**Three lessons from one DX12 hunt (2026-07-29), in order of how much time each cost.**

**1. ⛔⛔ A PROBE THAT MODIFIES A SHADER IS WORTHLESS UNTIL A CONTROL PASSES.** I edited
`body_standard_forward` to output the normal as colour, read the result, and drew conclusions — twice. Then I
replaced it with a CONSTANT RED whose expected value I could state in advance: *every shaded pixel must be pure
red on both backends*. It returned **zero red pixels on BOTH**. The probe had never been running. Every reading
taken from it, and every "unlit works / doesn't work" bisection built on `set_forward_technique`, was void. After
the fix the same control returned **151804 identical pixels on both backends** — only then were its readings
worth anything. This is [project_world_normal_varying_reads_zero](project-history.md#memory-project_world_normal_varying_reads_zero) in a new costume: *state the expected value
first, or you are reading noise.*

**2. ⛔⛔ WITH SHADOWS ACTIVE, `SceneRenderer` DRAWS WITH ITS SHADOWED PROGRAM AND IGNORES THE FRAME ASSET'S
DECLARED `technique`.** `record_one_group` picks `program_shadowed` (cooked from `forward_csm`) whenever
`shadows_active()`, so a frame declaring `technique = "standard_forward"` — and `set_forward_technique(...)` —
change nothing on screen. That is why the control failed. Diagnostically: **pass `--no-shadows` before trusting
any technique-level experiment.** Arguably a real design defect too (the authored frame says one thing, the
renderer draws another); recorded in D-007 row 39-D1.

**3. ⭐⭐ DX12 IGNORES `depth_buffer = true` ON A COLOUR TRANSIENT — the root cause of "the DX12 scene is
black".** `depth_buffer`/`companion` appears 8+ times in `vulkan_raster_context.cpp` (the 38-G1 companion-depth
image, its memory, its barriers) and **ZERO times** in `dx12_raster_context.cpp`. So `scene_hdr` has no depth on
DX12, `has_depth()` is false, the pass binds no DSV, and whichever triangle rasterizes last wins: silhouettes
correct, INTERIORS visible, blackness correlated with face orientation (the surviving surfaces are back-facing,
normals pointing away from the light). **The decisive test, and the reusable trick: change the DECLARED depth
state and see whether the backend's pixels move.** A frame flipped to standard-Z (`clear_depth = 1.0`,
`depth = "LessEqual"`) broke Vulkan exactly as predicted and left DX12 **pixel-identical** — *a backend that does
not change when the depth declaration changes is not testing depth at all.*

**Other measurement traps paid for in the same hunt:** a "non-black pixel" count that was really counting the
CLEAR COLOUR (the clear is `0.09,0.10,0.13`, not black), and isolating with `forward_basic` — a frame with **no
display transform**, so it renders dark on BOTH backends and proves nothing. Pick an isolation frame that still
tonemaps.

Related: [feedback_materialx_argument_order_in_authored_materials](rendering.md#memory-feedback_materialx_argument_order_in_authored_materials) (presence≠correctness, same session),
[feedback_dx12_hlsl_masks_type_bugs_run_vulkan](device-programs.md#memory-feedback_dx12_hlsl_masks_type_bugs_run_vulkan), [feedback_mission_portable_gpu_compute_all_backends](device-programs.md#memory-feedback_mission_portable_gpu_compute_all_backends).


<!-- end-memory:feedback_probe_needs_control_and_shadowed_program_ignores_frame_technique -->

<a id="memory-feedback_shadow_acne_needs_normal_offset_not_a_bigger_depth_bias"></a>
## feedback_shadow_acne_needs_normal_offset_not_a_bigger_depth_bias

---
name: feedback_shadow_acne_needs_normal_offset_not_a_bigger_depth_bias
description: "⛔⛔⛔ A DEPTH bias alone CANNOT fix a near-edge-on face: the depth needed across one texel is texel*tan(theta), unbounded at grazing, and any cap big enough peter-pans everything else. Use NORMAL-OFFSET bias (move the LOOKUP ~2.5 texels along the surface normal, prop to sin theta). Fixed the sandbox cube stripes; the cascade refit was NOT needed"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: ce31551a-2d4a-49f7-9c12-7bb0acdebc75
  modified: 2026-07-29T15:31:19.331Z
---

REN-39-D1, 2026-07-29. The user: *"look at this! On the cube for example... it is a bias thing I remember from my
own experiments with csm couple of years ago"* — with a screenshot of fine diagonal striping at the shadow-texel
frequency across the cube's large flat face. **They were right, and their instinct beat my analysis.**

**WHY A DEPTH BIAS CANNOT WIN THERE.** Across one shadow texel a receiver's depth changes by
`texel_world · tan θ` (θ = angle between the surface normal and the light). tan θ **diverges** as the surface
turns edge-on, so the depth bias a grazing face needs is unbounded — while any cap large enough to cover it
peter-pans everything else. The sandbox's cube face is VERTICAL under a high light: capping the slope at 3
(72°) still leaves the surface **31% lit**, which is exactly where the stripes lived. Tuning constants cannot fix
this; the formulation is wrong.

**THE FIX — NORMAL-OFFSET BIAS.** Move the LOOKUP off the surface along its own normal before projecting:

    nofs = texel_world_of_this_cascade · 2.5 · sin θ
    lp   = light_vp · vec4(worldPos + N·nofs, 1)

Bounded (~2.5 texels), perpendicular to the light rather than along it (so it does NOT detach contact shadows the
way depth bias does), and applied PER CASCADE in world units — so it must live INSIDE the per-cascade loop,
before the projection, using that cascade's own texel size. With it in place the depth bias becomes a junior
partner covering only quantization + the ±0.5-texel PCF footprint: `bsc·(1 + 1·min(tan θ, 2))`.

**MEASURED**: the cube face went from striped to perfectly flat; tile/duck field clean at 5–8× magnification;
Vulkan↔DX12 parity 0.75%; 125/125 gates green; tidy clean.

⛔⛔ **THE CASCADE REFIT WAS A RED HERRING — and the user said so before I proved it.** `near_plane = 0.1` does
leave cascades 0–1 in empty air (0 casters, 0 pixels selecting them, measured), putting the whole screen on
cascade 3 at 0.18 world-units/texel. Fitting the split range to the VISIBLE depth span sharpened the near field
3.3× and removed some speckle — but it cost **GPU 5.81 → 9.70 ms**, and once normal offset landed it was **not
needed at all**: cube and tile field are clean without it, at the old cost. Built, measured, A/B'd, REMOVED.
⭐ The lesson: when an artifact has a cheap correct fix and an expensive one, prove the cheap one insufficient
BEFORE paying for the expensive one — and when the user says "we don't need more cascades, we need a better
bias", test that first.
⛔ (If cascade resolution ever IS the problem: the SCENE AABB is the wrong input and measures as an exact NO-OP —
a field that surrounds the camera has an AABB reaching behind it, so the near clamp does nothing. Use the
visible set, consumed one frame late; texel snapping makes the latency harmless.)

⛔ **MAGNIFY BEFORE DECLARING AN ARTIFACT ABSENT.** I twice reported no acne from downscaled/far-away captures;
it was obvious at 8× on a single object. A downscaled screenshot destroys exactly the frequency in dispute, and
an aggregate "speckle count" is swamped by legitimate edges (grid lines, tile textures) — it moved 4% while the
artifact was plainly there. Also: `--screenshot-at` fires on wall-clock, so two runs catch different camera
moments; use the frozen probe (`--freeze --width N`) for any A/B.

Also corrected: the slope term is a real **tan θ = √(1−N·L²)/N·L**, never the `(1 − N·L)` proxy (at 60° it reads
0.5 against a true slope of 1.73).
Related: [feedback_shadow_bias_must_be_scale_invariant_texels_not_ndc](rendering.md#memory-feedback_shadow_bias_must_be_scale_invariant_texels_not_ndc),
[feedback_clip_space_y_convention_mirrors_every_rtt_sampled_by_uv](workflow-and-correctness.md#memory-feedback_clip_space_y_convention_mirrors_every_rtt_sampled_by_uv).


<!-- end-memory:feedback_shadow_acne_needs_normal_offset_not_a_bigger_depth_bias -->

<a id="memory-feedback_shadow_bias_must_be_scale_invariant_texels_not_ndc"></a>
## feedback_shadow_bias_must_be_scale_invariant_texels_not_ndc

---
name: feedback_shadow_bias_must_be_scale_invariant_texels_not_ndc
description: "⛔⛔ CSM depth bias as a CONSTANT in normalized depth is a different WORLD distance in every cascade (0.2 units in c1, 2.9 in c3) — bigger than the casters, so small objects stop casting entirely. Express it in TEXELS and convert per cascade, recovering both factors from the cascade's own matrix"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: ce31551a-2d4a-49f7-9c12-7bb0acdebc75
  modified: 2026-07-29T14:21:17.824Z
---

REN-39-D1 (2026-07-29). The sandbox's shadows looked "completely wrong — shadows in the wrong places, objects
casting onto themselves." The cascade FIT was fine: the CPU plane-cull and the shader's containment test agreed
to within a few percent (c2: 818 plane / 723 centres; c3: 3306 / 3273). The defect was the BIAS.

`body_forward_csm` used `bias = 0.0015 + 0.0045*(1 - N·L)` in NORMALIZED depth. But each cascade's ortho maps a
DIFFERENT world depth span onto that same [0,1], and `caster_extrusion` (100 world units) inflates every one of
them. Measured on a 200×200 field:

| cascade | texel (world) | depth range | bias in WORLD units | casters |
|---|---|---|---|---|
| 0 | 0.0075 | 115 | 0.17 – 0.69 | **0** |
| 1 | 0.0177 | 136 | 0.20 – 0.82 | 135 |
| 2 | 0.045 | 193 | 0.29 – 1.16 | 818 |
| 3 | 0.184 | 477 | **0.72 – 2.86** | 3312 |

A duck is ~1.5 units tall — **the bias exceeded the casters**. Classic peter-panning: shadows detach, small
objects stop casting at all, and only big objects survive, leaving a few stray blobs that read as "shadows in
completely wrong places" and point at neither the fit nor the atlas.

**THE RULE: a shadow bias is a number of TEXELS, never a constant fraction of a depth range.** Convert per
cascade. Recover BOTH factors from the cascade's own matrix so no new binding can drift out of agreement with
it — `ortho_rh_zo` puts `1/radius` in `c0.x` and `−1/(far−near)` in `c2.z`, so

    texel_world  = 2 / (map_size · c0.x)
    1/depth_range = −c2.z
    bias = texel_world · (1 + 3·(1 − N·L)) · (1/depth_range)

Extract the columns with `mat_mul_vec(vp, vec4(1,0,0,0))` / `vec4(0,0,1,0)` — constant-folds to a single header
word each. Thread `bias_scale[ci]` through the SAME select chain as the UVs, or it will not track the cascade
actually chosen. Result: ~10× smaller bias in cascade 1, every duck casting a contact shadow, no acne.

⚠ Also measured and deliberately NOT changed: cascade 0 received ZERO casters and was selected by ZERO pixels
(splits with `near_plane = 0.1` over a 160-unit far land the first two cascades inside 15 units, where a camera
17 units up sees nothing). Fitting the split range to the scene AABB measured **identical** (5.808/15.811 vs
5.817/15.31) because the field SURROUNDS the camera, so the AABB reaches behind it and the near clamp is inert;
and making cascade 0 carry casters would COST more than the 0.055 ms it wastes. Related:
[feedback_bit_exact_blind_to_symmetric_bugs_energy_comp](workflow-and-correctness.md#memory-feedback_bit_exact_blind_to_symmetric_bugs_energy_comp), [project_ren38_bindless_multidraw_slices](project-history.md#memory-project_ren38_bindless_multidraw_slices).


<!-- end-memory:feedback_shadow_bias_must_be_scale_invariant_texels_not_ndc -->

<a id="memory-feedback_taa_motion_vectors_ghost_reverted_screenshots_miss_motion"></a>
## feedback_taa_motion_vectors_ghost_reverted_screenshots_miss_motion

---
name: feedback_taa_motion_vectors_ghost_reverted_screenshots_miss_motion
description: "REN-41 TAA motion vectors smeared moving instances: prev_world went STALE for intermittently-updated instances (double-buffer only advanced on re-extract). Fixed by advancing prev_world=world for last frame's movers. Screenshots HIDE motion artifacts — examine LIVE."
metadata:
  node_type: memory
  type: feedback
  originSessionId: bf0ab64f-0cb7-4b04-970c-78c4f58c02b9
  modified: 2026-08-05T19:29:25.627Z
---

Persistent user complaint ("visuals distort when instanced meshes or the camera move"). Cause: the REN-41 per-object
motion-vector TAA smeared a ghost trail on moving instances. **Root cause (finally found, FIXED — not reverted):**
the velocity double-buffer `prev_world` (per-instance previous transform, shadowing `group.instances[].world`) is
only advanced inside `write_slot` — i.e. ONLY on the frame an instance's chunk is re-extracted (dirty). The sandbox
grid animates as a TRAVELLING WAVE that updates **one grid row per frame** (`row = frame % side`), so each instance
moves only every `side` frames. On the `side−1` frames AFTER it moves it is static, but `prev_world` still holds its
PRE-move transform, so the velocity prepass reads a stale one-move delta EVERY frame and the resolve reprojects a
permanent ghost. Continuously-updated objects (spinning monuments, every frame) were always fine; intermittently-
updated ones smeared. **Fix (`SceneRenderer::sync`, engine/scene-render):** track the runs that moved LAST sync
(`prev_dirty_runs`); at the start of the next sync, before the new extract, advance `prev_world = world` for them and
push their runs into `dirty_runs` to re-upload — so a mover's velocity lasts EXACTLY one frame then decays to 0. Work
is proportional to what moved, not the scene (keeps the dirty-grain upload; does NOT full-copy prev_world per frame).
Also widened `velocity` to RGBA16F and store the raw ndc.z delta so the resolve reprojects the FULL previous ndc
(x,y,z) through R before the perspective divide — omitting z left a depth-motion residual. Verified sharp on Vulkan
AND DX12; scene-render 1467/1467.

**Debugging lessons (the expensive ones — this took MANY wrong turns):**
- ⛔⛔⛔ **SCREENSHOTS HIDE MOTION ARTIFACTS.** A single `--screenshot` of a moving scene looks SHARP even when the
  LIVE window drifts (TAA converges when motion stops). I declared "fixed" from stills THREE times and was wrong each
  time until the USER screenshotted the live smear. For any temporal/motion/AA bug you MUST examine the LIVE running
  sandbox (`--smoke-test <N>` shows the window N seconds) — and since you can't see the live window, have the USER
  look. Do not trust your own stills.
- ⭐⭐ **A PROVABLE CONTROL cracks it.** The winning move was viz-ing the velocity buffer amplified with a STATIC
  instance as the control: a static instance's velocity MUST be exactly zero, so any colour on it = the bug, on the
  GPU, isolated from the resolve. Then a cooker hack (force prev_clip to use the CURRENT world) proved the prev_world
  READ was the source; then a CPU `prev_world vs world` probe found the diverging matrix ELEMENT (13 = translation.y,
  ±1.2 = the sandbox's bob amplitude) → the animation model. Layer isolating probes; don't guess resolve-side signs.
- ⛔ Resolve-side sign/scale/composition guessing is a TRAP when the data is wrong upstream — every `±velv`, half-mag,
  and NDC-composition variant still ghosted because the velocity VALUES were stale. Fix the producer, not the consumer.
- ⛔ DX12 vs Vulkan diverge on the TAA Y sign (`sgn = ndc_y_points_down ? +1 : -1`); verify BOTH backends
  (`--backend dx12`). RenderDoc in this build CANNOT dump texture contents — use an in-engine probe with a control.

**How to apply:** any velocity/motion-vector double-buffer MUST guarantee `prev = last-frame world` for EVERY drawn
instance every frame, not just the ones re-extracted this frame — an incremental/dirty-grain extract leaves stale
prev for movers that paused. Reproduce LIVE, null-isolate, control against a provably-static instance, verify both
backends. Related: [feedback_velocity_prev_palette_two_paths_and_device_gate](workflow-and-correctness.md#memory-feedback_velocity_prev_palette_two_paths_and_device_gate),
[feedback_clip_space_y_convention_mirrors_every_rtt_sampled_by_uv](workflow-and-correctness.md#memory-feedback_clip_space_y_convention_mirrors_every_rtt_sampled_by_uv), [reference_renderdoc_headless_capture_and_xml_query](rendering.md#memory-reference_renderdoc_headless_capture_and_xml_query).


<!-- end-memory:feedback_taa_motion_vectors_ghost_reverted_screenshots_miss_motion -->

<a id="memory-feedback_verifier_that_materializes_its_input_must_surface_a_failed_materialization"></a>
## feedback_verifier_that_materializes_its_input_must_surface_a_failed_materialization

---
name: feedback_verifier_that_materializes_its_input_must_surface_a_failed_materialization
description: "A verifier/validator layer that MATERIALIZES its input before checking (reconstruct a desc via a round-trip converter, parse a blob, load a file) must SURFACE a failed materialization — assert loud or return a non-None diagnostic — NEVER guard the check behind `if (materialized_ok)` and fall through to a green verdict; and a per-category CAP must degrade to check-the-first-N, not skip-the-whole-layer"
metadata:
  node_type: memory
  type: feedback
  originSessionId: fade8ea4-87ca-470f-83e0-cdfe82a44e7f
  modified: 2026-08-11T10:00:42.706Z
---

CEIR-15c-1d-1 wired the program-contract layer into `validate_ceir_frame`: it materializes the full `FrameGraphDesc`
(`from_ceir_frame(ctx,m,alloc,desc)`) then runs the shared `pass_contract_diag` per pass. My first cut guarded the whole
loop as `if (from_ceir_frame(...) && desc.passes.size() == npass) { … }`. **Two silent-green holes** the advisor caught:

- ⛔ **`from_ceir_frame == false`** after the structural walk already PASSED = a broken converter invariant, but the
  guard just skipped the loop and `validate_ceir_frame` returned `None` (well-formed). An unrunnable verifier layer that
  reads GREEN is the exact disease of [feedback_tidy_gate_clean_on_unparsed_files](build-and-verification.md#memory-feedback_tidy_gate_clean_on_unparsed_files) (a TU that never parsed emits zero
  diagnostics → blind == clean). **Fix:** `CRD_ASSERT_MSG(ok, …)` (loud in the debug/asan gate configs) + keep the
  `if (ok)` only for release memory-safety. A loud branch with no test is acceptable here — don't rabbit-hole a fixture
  for an impossible path.
- ⛔ **`desc.passes.size() == npass`** (an equality guard against a per-category cap) skipped the ENTIRE layer on a
  `> kMaxRes` graph, not just the overflow tail. The rest of the verifier already caps each category at `kMaxRes=128`
  (`nres/nimp/npass`) and checks the first N; the new layer must match that — `min(npass, desc.passes.size())`, pairing
  `desc.passes[i]` with the walk-collected `pass_ops[i]` (both graph-order) — NOT skip wholesale.

**Why it generalizes:** any validator with a "reconstruct/parse/load, THEN check" shape has a hidden third outcome
besides pass/fail — *couldn't-run* — and the default `if (ok) {check}` collapses couldn't-run into pass. **How to apply:**
whenever a verifier materializes its input, make the couldn't-run branch LOUD (assert / non-None diag), never a silent
skip; and make any capacity cap degrade to check-the-first-N with the cap documented at the guard, never a whole-layer
bypass. Sibling of the differential-oracle rule [feedback_differential_oracle_must_run_the_full_legacy_pipeline_not_one_phase](numerics-and-performance.md#memory-feedback_differential_oracle_must_run_the_full_legacy_pipeline_not_one_phase)
(there the oracle must run the FULL pipeline; here the verifier must not go green when its own setup step failed).


<!-- end-memory:feedback_verifier_that_materializes_its_input_must_surface_a_failed_materialization -->

<a id="memory-reference_froxel_slice_aabbs_via_clip_w_lerp"></a>
## reference_froxel_slice_aabbs_via_clip_w_lerp

---
name: reference_froxel_slice_aabbs_via_clip_w_lerp
description: "How to build per-slice 3D-clustered froxel AABBs consistent with a published z-slice boundary table — unproject near/far once, GLOBAL w=1/h.w, exponential boundaries, LINEAR interp in clip.w per slice (CEIR-18b compute_froxel_slices)"
metadata: 
  node_type: memory
  type: reference
  originSessionId: 192f4d9e-b3f7-485b-a775-5391bcd4183c
  modified: 2026-08-16T01:36:56.161Z
---

**CEIR-18b `compute_froxel_slices` (scene_renderer.cpp).** To build the per-slice froxel AABBs for a Doom-2016
clustered renderer so the CPU AABB cuts AGREE with the shader's z-binning by construction:

1. `inv = inverse(view_proj)`. Unproject each screen-tile corner at **ndc-near AND ndc-far** through `inv` → the two
   world ray endpoints `P_near`, `P_far` (the reverse-Z convention: near=ndc_z 1, far=a FINITE substitute like ndc_z 0.01,
   because ndc_z 0 = the infinite far → w≈0 degenerate; the finite far's world plane must sit beyond the scene lights).
2. **clip.w at a plane = `1/h.w`** where `h = inv·(nx,ny,ndc_z,1)` (the forward clip.w of the unprojected point). For a
   standard perspective this depends ONLY on ndc_z, not x/y (because `inv(view_proj)`'s 4th row has zero x/y components),
   so `w_near`/`w_far` are **GLOBAL** — compute them ONCE on the view axis (nx=ny=0). This is the load-bearing insight;
   it's what makes ONE boundary table valid for every tile. (Empirically confirmed by the non-circular test below passing
   for an off-axis point.)
3. Pick N+1 **exponential** boundaries `w_i = w_near·(w_far/w_near)^(i/N)` — near slices thin, far thick. This table is
   published to header words (kHdrSliceBoundsOff..+N) and is the SAME table the FS Step-sums against.
4. A world point at clip.w=`w` is **LINEAR along the ray** (world is linear in view_z; clip.w = −view_z is linear in
   view_z) → `P(w) = lerp(P_near, P_far, (w−w_near)/(w_far−w_near))`. Slice `s`'s box brackets `P(w_s)…P(w_{s+1})` over
   the 4 tile corners. No re-unproject per slice — interpolate the two endpoints.
5. **z needs NO per-backend flip** (clip.w is backend-neutral); the x/y corners carry the SAME `flip_y =
   !ndc_y_points_down()` as the 2D `compute_froxel_aabbs` (the froxel NDC±Y scar). Degenerate corners (h.w≈0) skip; a
   zero-span (w_far==w_near) or non-positive w falls back to a LINEAR table so the fill never divides by zero.

Cluster index `c = tz*gx*gy + ty*gx + tx` (matches the FS `fi = tx + ty*gx + slice*gx*gy`). See
[feedback_publish_a_boundary_table_not_a_dual_formula](workflow-and-correctness.md#memory-feedback_publish_a_boundary_table_not_a_dual_formula) for WHY a published table (not a dual formula) and the
non-circular gate that validates this fill. Related: [feedback_cpu_frustum_data_vs_fragcoord_needs_per_backend_ndc_y](workflow-and-correctness.md#memory-feedback_cpu_frustum_data_vs_fragcoord_needs_per_backend_ndc_y)
(the x/y flip), [feedback_matrix_element_is_not_a_scale_use_the_row_norm](workflow-and-correctness.md#memory-feedback_matrix_element_is_not_a_scale_use_the_row_norm).


<!-- end-memory:reference_froxel_slice_aabbs_via_clip_w_lerp -->

<a id="memory-reference_renderdoc_headless_capture_and_xml_query"></a>
## reference_renderdoc_headless_capture_and_xml_query

---
name: reference_renderdoc_headless_capture_and_xml_query
description: "How to use RenderDoc HEADLESSLY on this host: renderdoccmd capture + synthetic F12, then `convert -c xml` and query the XML with python (the winget build ships NO renderdoc python module). Gives ground truth on clears, depth funcs, samplers, viewports, raster state, resources and SRVs in minutes"
metadata: 
  node_type: memory
  type: reference
  originSessionId: ce31551a-2d4a-49f7-9c12-7bb0acdebc75
  modified: 2026-07-29T13:00:24.468Z
---

RenderDoc 1.45 is installed at `C:\Program Files\RenderDoc` (winget `BaldurKarlsson.RenderDoc`). It ships
`renderdoccmd.exe`, `qrenderdoc.exe`, `renderdoc.dll` — and **no `renderdoc.pyd`**, so the usual "script the
replay in Python" route is unavailable. The GUI is not scriptable from here either. What DOES work, entirely
headless:

**1. Capture.** `renderdoccmd capture -d <workdir> -c <prefix> <exe> [args...]` launches the app hooked. There is
no `--capture-frame` flag, so trigger the capture by sending the default hotkey to the window:
find it with `EnumWindows`+`GetWindowTextW`, `SetForegroundWindow`, then `keybd_event(0x7B /*F12*/, ...)` down+up.
Output lands as `<prefix>_frameNNN.rdc`.

**2. Convert to XML.** `renderdoccmd convert -f cap_frameNNN.rdc -o cap.zip.xml -c zip.xml`. Despite the name it
writes **plain XML** (`cap.zip.xml`) plus a sibling `cap.zip` holding the binary blobs. A 12 MB capture → ~1.5 MB
XML.

**3. Query it with python + regex.** Every API call is a `<chunk name="ID3D12GraphicsCommandList::X" ...>` with
typed `<struct>`/`<enum>`/`<uint>` children; enums carry a readable `string="..."` attribute. Useful pulls:
- clears: `<chunk ... ClearDepthStencilView>` → `name="Depth"`, `name="ClearFlags"`, `name="FirstArraySlice"`,
  `name="Resource"`
- pipeline depth state: `name="DepthFunc"` / `name="DepthEnable"`
- samplers: `ComparisonFunc`
- viewports: `<chunk ... RSSetViewports>` → `TopLeftX/Y`, `Width`, `Height`, `MinDepth`, `MaxDepth`
- raster: `<struct name="RasterizerState">` → `CullMode`, `FrontCounterClockwise`, `DepthBias`,
  `SlopeScaledDepthBias`, `DepthBiasClamp`
- resources: `<struct name="pDesc" typename="D3D12_RESOURCE_DESC">` → `Format`, `Width/Height`,
  `DepthOrArraySize`
- views: `<struct name="Descriptor" typename="D3D12_SHADER_RESOURCE_VIEW_DESC">` → `Format`, `ViewDimension`,
  `ArraySize`, `FirstArraySlice`

**What it is GOOD for (proved 2026-07-29 on the DX12 shadow hunt):** eliminating the entire pipeline-state
hypothesis space in one pass. One capture confirmed clears, depth funcs, comparison sampler, viewports,
rasterizer bias and cull, the atlas resource AND its array SRV were all correct and Vulkan-identical — which
retired "the depth is offset / it's a bias or binding bug" as a class and pointed at shader-computed values
instead. **What it is NOT good for here:** dumping a TEXTURE's contents (needs the Python replay API this build
lacks) — for pixel values, use an in-engine probe with a validated control instead
([feedback_probe_needs_control_and_shadowed_program_ignores_frame_technique](rendering.md#memory-feedback_probe_needs_control_and_shadowed_program_ignores_frame_technique)).

⛔ PowerShell note: `Remove-Item` on a path built from `"C:\Program Files\..."` unquoted trips the protected-path
guard; keep the exe path in a variable and pass args via an array to `Start-Process`.


<!-- end-memory:reference_renderdoc_headless_capture_and_xml_query -->

<a id="memory-scars_render_frame_graph"></a>
## scars_render_frame_graph

---
name: scars_render_frame_graph
description: Scars for rendering / frame-graph / perf / skinning — WBOIT asymmetry, depth atlas, load-not-clear, upload barrier, POD-hash padding, PSO cache-by-content, fps-median, gates, smoke-false-green, world-normal, velocity/TAA, skinned normals, LOD, anyhit, frag_xy — relocated out of MEMORY.md; open before "fixing" a render / frame-graph / perf symptom.
metadata: 
  node_type: memory
  type: reference
  originSessionId: cb9df3b8-2389-479b-9d99-d3d6ce3ba327
  modified: 2026-09-06T06:11:05.187Z
---

Rendering / frame-graph / perf / skinning scars, moved out of the always-loaded `MEMORY.md` (CEIR-26z compaction,
2026-09-05). Recall on demand when the work touches a render pass, the frame graph, perf measurement, or skinning.
Read before "fixing" a matching symptom.

## Frame graph + perf
- [⛔⛔ WBOIT asym](rendering.md#memory-feedback_frame_graph_wboit_composite_wrong_for_asymmetric_transparency); [⛔⛔ depth atlas](rendering.md#memory-feedback_layered_depth_atlas_four_scars)
- [⛔⛔ LOAD not clear](workflow-and-correctness.md#memory-feedback_multi_pass_scene_draws_must_load_not_clear_smoke_is_pixel_blind); [upload barrier](workflow-and-correctness.md#memory-feedback_dispatch_1wg_missing_upload_barrier_race); [skip diag](device-programs.md#memory-feedback_gpu_kernel_profiling_standalone_not_skip_diag); [⛔⛔ POD hash](workflow-and-correctness.md#memory-feedback_struct_padding_in_content_hash_and_cooked_blobs); [⛔⛔ PSO cache=C](device-programs.md#memory-feedback_gpu_pipeline_cache_key_by_content_not_pointer)
- [⛔⛔ fps median](workflow-and-correctness.md#memory-feedback_fps_single_run_is_noise_median_of_five); [⛔⛔ gates](workflow-and-correctness.md#memory-feedback_gates_run_configs_the_app_never_ships); [⛔⛔ smoke needs](workflow-and-correctness.md#memory-feedback_sandbox_smoke_overlay_only_false_green_needs_crd_assets_dir); [⛔⛔⛔ world_norm](project-history.md#memory-project_world_normal_varying_reads_zero); [⛔⛔ velocity oom](workflow-and-correctness.md#memory-feedback_velocity_prev_transform_64b_per_instance_oom_at_1m); [velocity](workflow-and-correctness.md#memory-feedback_velocity_prev_palette_two_paths_and_device_gate); [⛔⛔⛔ TAA](rendering.md#memory-feedback_taa_motion_vectors_ghost_reverted_screenshots_miss_motion)
- [⛔⛔⛔ 0-draw pass NEVER clears: the encoder folds begin_rendering+Clear into the FIRST draw; empty world → UNDEFINED attachment; end_rendering must flush clear_scope](workflow-and-correctness.md#memory-feedback_command_encoder_folds_clear_into_first_draw_zero_draw_pass_never_clears)

## Skinning / geometry / shading
- [⛔⛔ Fox normals](workflow-and-correctness.md#memory-feedback_skinned_mesh_missing_normals_nan_black); [⛔⛔ gpu_skin](device-programs.md#memory-feedback_device_skin_passes_both_need_khdrgpuskinactive_gate); [torus](project-history.md#memory-project_obj_torus_normals_unlit_ren3); [⭐⭐ SHAPE=ORACLE](numerics-and-performance.md#memory-feedback_shape_checker_mirrors_oracle_semantics); [⭐⭐ inert ROT](workflow-and-correctness.md#memory-feedback_inert_asset_copies_rot_pin_canonical_form); [⛔⛔ anyhit](workflow-and-correctness.md#memory-feedback_rt_anyhit_opaque_flag_and_compile_only_backend_claims); [⛔⛔ B7 Storage](workflow-and-correctness.md#memory-feedback_b7_lower_entry_miscompiles_cooked_forward); [⛔⛔ frag_xy](device-programs.md#memory-feedback_shader_frag_xy_unit_conflict_pixel_vs_normalized)


<!-- end-memory:scars_render_frame_graph -->

