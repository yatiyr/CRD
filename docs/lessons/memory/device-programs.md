# Memory reference: device programs

<!-- doc-role: reference -->
> Technical reference; verify dated claims against current contracts/source. Current work: [ROADMAP](../../ROADMAP.md); current rules: [AGENTS](../../../AGENTS.md).

> Reference corpus, consolidated 2026-09-12; not a live tracker. Read [AGENTS](../../../AGENTS.md),
> [MEMORY](../../../MEMORY.md) and [ROADMAP](../../ROADMAP.md) for current rules/status.
> Dated state, loop grants, tool paths and schedules below are historical. Reusable engineering lessons remain
> applicable unless superseded by current instructions. Retrieve one named record; do not load this whole file on entry.

<a id="memory-feedback_as_ms_payload_contract_dx12_pso"></a>
## feedback_as_ms_payload_contract_dx12_pso

---
name: feedback_as_ms_payload_contract_dx12_pso
description: "D3D12 rejects a task+mesh PSO whose AS→MS payload sizes disagree — HLSL DispatchMesh ALWAYS passes a payload, so a mesh that reads none must still DECLARE it ([mesh] payload = true → KEntry.mesh_payload_in); Vulkan tolerates absence and hides the bug"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 31e31376-4d57-4a00-b30c-77365444ac88
  modified: 2026-07-27T11:20:28.054Z
---

A **task+mesh (AS→MS) pipeline on D3D12 requires the payload structs to AGREE IN SIZE** between the two stages.
The HLSL amplification emitter always writes `DispatchMesh(n, 1, 1, s_payload)` — the payload argument is
mandatory in HLSL — so the AS always exports a 16-byte payload. A mesh shader that reads no payload field
declares no `in payload` parameter → payload sizes 16 vs 0 → `CreateGraphicsPipelineState` (stream PSO) fails
`E_INVALIDARG`. **GLSL/Vulkan is lenient**: a task with no payload fields calls `EmitMeshTasksEXT(n,1,1,)` with
no payload at all and the mesh needs no declaration — the identical CKIR pair links and renders on Vulkan, so
only a DX12 device gate can catch it ([feedback_dx12_hlsl_masks_type_bugs_run_vulkan](device-programs.md#memory-feedback_dx12_hlsl_masks_type_bugs_run_vulkan) — each backend catches a
different class).

**Why:** the payload is part of the PSO's inter-stage contract, like the varying signature. One cooked mesh
KEntry serves both `create_mesh_program` (standalone — must NOT declare a payload input; an MS with one needs an
AS in front) and `create_task_mesh_program` (must declare it), so the pairing cannot be inferred — it must be
DECLARED by the asset.

**How to apply:** `.crdv` `[mesh] payload = true` → `KEntry::mesh_payload_in` (appended at END; graph blob
v3 in `ckir_serialize.hpp` — version-checked, so bumping orphans stale cooked `.crdr` blobs, all regenerable).
Both mesh emitters then declare the fixed 4-field payload struct (`taskPayloadSharedEXT` / `in payload
MeshPayload mp`) even when unread. A gate pairing an authored task with a fetch mesh must cook TWO mesh variants:
plain for the standalone claim, `payload = true` for the task claim (REN-38-F16 gates, both backends).


<!-- end-memory:feedback_as_ms_payload_contract_dx12_pso -->

<a id="memory-feedback_ckir_1d_broadcast_aligns_first_axis_bias_needs_reshape"></a>
## feedback_ckir_1d_broadcast_aligns_first_axis_bias_needs_reshape

---
name: feedback_ckir_1d_broadcast_aligns_first_axis_bias_needs_reshape
description: "CKIR's 1-D broadcast([N],[M,N]) aligns the FIRST axis (per-ROW), NOT numpy trailing (per-column) — a linear-layer/per-column bias MUST reshape [N]→[1,N] first, or a square M==N silently gives a per-row bias"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: b0138d6a-548b-428b-87b2-fe30c9f36f7c
---

Authoring a `linear` layer as a generic module function (`fn_linear`, `ckir_module.hpp`): `x·W + b` where `b` is `[N]`
(per output column). The obvious `g.broadcast(b, [M,N])` **silently produced a per-ROW bias** — the MLP-layer oracle
test failed on exactly the OFF-DIAGONAL elements ([0,1], [1,0]) while the diagonal matched, the tell-tale signature of a
transposed/mis-aligned broadcast on a square M==N shape.

**Root cause:** CKIR's 1-D `broadcast([N], [M,N])` aligns the source's single axis with the **FIRST** (row) axis of the
target — so `B[i,j] = b[i]`, a per-ROW bias — NOT numpy/PyTorch trailing alignment (`B[i,j] = b[j]`, per column). On a
square shape (M==N) it "works" dimensionally but is semantically the transpose of what a bias means.

**Fix:** reshape the bias `[N] → [1, N]` FIRST, then broadcast `[1,N] → [M,N]`. A rank-2 source is unambiguous — dim0
`1→M` (broadcast), dim1 `N→N` (match) ⇒ `B[i,j] = b[0,j] = b[j]`, the per-column bias.

**Why:** the failure hides on square shapes (dimensionally valid) and only the off-diagonal is wrong, so a coarse
"does it run + roughly match" check passes. **How to apply:** any per-column / trailing-axis broadcast (bias, layer-norm
scale/shift, per-feature params) must reshape the 1-D operand to `[1, ...]` to force last-axis alignment — never rely on
CKIR's bare 1-D broadcast to be numpy-style. (The fused GEMM+bias path handles bias per-column in its emitter directly,
so it was never exposed to this; the graph-level `broadcast` eval is where it bites.) Related:
[feedback_ckir_binary_vec_scalar_shape_mismatch_gpu_broadcasts_oracle_oob](device-programs.md#memory-feedback_ckir_binary_vec_scalar_shape_mismatch_gpu_broadcasts_oracle_oob).


<!-- end-memory:feedback_ckir_1d_broadcast_aligns_first_axis_bias_needs_reshape -->

<a id="memory-feedback_ckir_asset_param_specialization_via_spec_consts"></a>
## feedback_ckir_asset_param_specialization_via_spec_consts

---
name: feedback_ckir_asset_param_specialization_via_spec_consts
description: "Convert a PARAMETERIZED ckir program (per-cascade/per-variant) to assets via D12 SPEC-CONST nodes patched at load time — NOT bake-all, NOT a constants buffer, NOT TexSize on an arrayed texture"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 192f4d9e-b3f7-485b-a775-5391bcd4183c
  modified: 2026-08-15T19:14:14.311Z
---

Converting a hand-built program that is PARAMETERIZED (per-cascade layer, blur direction, a config-derived
constant like 1/map_size) to an authored `.ckir` asset (CEIR-18p pattern; proven on moment shadows, reuse for
skin/palette). The knobs that vary at runtime become **D12 SPEC-CONSTANT nodes**, and the host specializes each
`prog[kind][index]` slot at ensure-time.

**The recipe:**
- ONE asset per *algorithm*; genuinely different MATH = separate files (moment EVSM exp-warps vs MSM 4-power-moments
  ⇒ `moment_convert_evsm.ckir` + `_msm.ckir`). A structural knob that only picks an axis (blur x vs y) stays ONE file
  with the axis as a spec-const.
- Runtime params = `g.spec_constant(id, default, DType)` nodes (not `g.constant`). They round-trip through the ckir
  form for FREE — `axes`(kSpecConstFlag) + `iidx`(id) are already serialized, default-elided — so **NO `[[param]]`
  table / form change / gate change**.
- The host patches per slot AFTER `ckir_read` + BEFORE `create_program` via `KGraph::set_spec_const(id, value)`
  (added to ckir.hpp). **Both backends use the node `cval`** — Vulkan emits `layout(constant_id=N) const T=default`
  and gpu-context supplies NO `VkSpecializationInfo` so the default IS the value; DX12/HLSL has no spec-const path so
  it bakes `cval` as a plain literal. So patching `cval` sets both. Keep the builder LIBRARY fn (ckir_technique.hpp)
  as oracle/regen source (deferred_shade precedent); delete only the ensure-wrapper's build call.
- Refactor STEP first, gate bit-identical BOTH backends BEFORE emitting (spec-const DEFAULT = the value the old code
  baked ⇒ create bakes the same ⇒ REN-40-D unchanged). Then emit → thin-load surgery → gate again.
- NON-VACUITY pin (A==A discipline for specialization): patch the same asset with 2 different values, assert the
  SERIALIZED graphs DIFFER — proves the patch actually lands, not a silent no-op.

**⛔ SCAR — do NOT derive a texture size via `KOp::TexSize` on an ARRAYED texture.** GLSL
`textureSize(sampler2DArray, lod)` returns ivec3, but `KGraph::tex_size` types a `TexDim::Tex2D` (even arrayed=true)
as **ivec2** — a rank mismatch that miscompiles the moment atlas the moment the node materializes to a temp. Feed the
size-derived constant (e.g. `inv=1/map_size`) as a SPEC-CONST from the LIVE config instead (host reads `csm.map_size`)
— it solves the same §128 "don't freeze the live config" concern without the landmine.

**⭐ EXTENSION 2026-08-15 (impostor, the last 18p builder) — CONFIG-STRUCTURAL programs: PIN the structure, spec-const the
values.** When a config number drives the program's STRUCTURE, not just a value — the impostor FS unrolled its per-mip-level
trilinear offset SELECT CHAIN to `impostor_num_mips(tile)` entries, and `tile` is live (an app policy) — you cannot spec-const
a chain LENGTH. Fix: PIN the chain at a compile-time MAX (`kImpostorMaxMips = 16`, the exact bound for tile ≤ 32768), make
every per-level value a spec-const (`kImpostorLvl0Spec+k`, k=0..15), and DEAD-BRANCH the tail — `mi_u` is clamped to live
`mips-1`, so k ≥ live mips never matches; patch those offsets to 0 (a defined, never-selected value). Add a LOUD guard
(`num_mips > kMax ⇒ CRD_LOG_ERROR + null`). Result is pixel-IDENTICAL, NOT GLSL-identical (the extra dead branches emit but
never execute) — so verify with the DEVICE PIXEL gate (the exact 133/169/36 triple), not a text/hash diff. 18 spec-consts total
(gt, mips-1 [ONE node, two clamp sites], 16 offsets).

**⛔ REFINEMENT to "keep the builder as oracle": a RENDERER-LOCAL builder with a DEVICE PIXEL-GATE proof is DELETED OUTRIGHT.**
The moment builder stayed (a `ckir_technique.hpp` LIBRARY fn, reused as a differential oracle). The impostor builders
(`build_impostor_vs`/`build_impostor_fs`) were renderer-local + the proof is the pixel gate (not builder-vs-asset differential),
so STEP 3 DELETED them entirely (−21 KB) + the env-guarded emit — "deletion is the proof", the asset is the only source
(regen = git revert). STEP-4 kir round-trip gate pins the STRUCTURE the pixel gate can't see: iterate `g.node(i)` /
`is_spec_const` to assert the FS carries EXACTLY the 18 spec-const ids and the VS carries ZERO (a spec-const silently degraded
to a plain const renders identically until an app patches it). Emit order for a config-structural asset: build (defaults) →
ckir_write UN-lowered → [loader] ckir_read → set_spec_const(live) → lower → create.

Related: [feedback_everything_is_an_authorable_asset_ceir](execution-ir.md#memory-feedback_everything_is_an_authorable_asset_ceir) (the mandate); [feedback_ckir_if_block_shared_temp_scope_materialize](rendering.md#memory-feedback_ckir_if_block_shared_temp_scope_materialize) (the cull-kernel emitter bug STEP 0.5 exposed); [feedback_powershell_file_on_a_bat_is_a_silent_noop_use_cmd_c](build-and-verification.md#memory-feedback_powershell_file_on_a_bat_is_a_silent_noop_use_cmd_c) (build rail).


<!-- end-memory:feedback_ckir_asset_param_specialization_via_spec_consts -->

<a id="memory-feedback_ckir_binary_vec_scalar_shape_mismatch_gpu_broadcasts_oracle_oob"></a>
## feedback_ckir_binary_vec_scalar_shape_mismatch_gpu_broadcasts_oracle_oob

---
name: feedback_ckir_binary_vec_scalar_shape_mismatch_gpu_broadcasts_oracle_oob
description: "CKIR g.binary(Mul,vecN,scalar) is a SHAPE MISMATCH — GPU broadcasts vec·float, the same-shape oracle reads OOB"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 40e3ad67-a505-447d-89df-272b48c237f6
---

⛔ Debugging scar (2026-07-12, D-007 B8-d specular observable). A CKIR `g.binary(op, vecN, scalar)` — a vector operand
and a **scalar** operand — is an ILL-FORMED same-shape op. `g.binary` does NOT broadcast (it assumes both operands share
the shape/comps). The GLSL/HLSL emitters emit `vec3 * float`, which the languages BROADCAST — so the **GPU renders
correctly**. But `eval_cpu`'s binary op reads `a[e*3+k]·b[e*3+k]` for k=0,1,2, and the scalar `b` has only 1 comp per
element → it reads OUT OF BOUNDS for k=1,2 → the G/B channels come back as garbage/ZERO.

**Symptom that pinned it:** the both-backends observable's GPU output was IDENTICAL on Vulkan AND DX12 (so NOT an emitter
bug) but diverged from the CPU-oracle `_expected` by a large, systematic amount (maxdiff ~60–116, not the single-digit FMA
noise) — and only in the non-first channels. Per-channel dump showed `want=(R,0,0)` while `gpu=(R,G,B)`: the oracle zeroed
G/B. The tell: **both backends agree with each other but disagree with the oracle, channel-selectively.**

**Also bites g.ternary / KOp::Mix (2026-07-12, B8-g MSM):** `g.ternary(KOp::Mix, moments_vec4, const_vec4, moment_bias_SCALAR)`
— same-shape ternary, scalar factor not splatted → the oracle reads the scalar buffer OOB for comps 1..3. Distinct
CPU-oracle signature: **elements 0..(numel/comps−1) MATCH bit-exact, the rest DIVERGE** (the first `kN` scalar reads land
inside the same-valued const buffer; reads past it hit adjacent scratch). Fix: `g.splat(moment_bias, 4)` (or `nodes::detail::tern`).

**Bit me a THIRD time (2026-07-12, B8-l `deferred_shade` normal decode):** `nenc*2 − 1` where `nenc` is a vec3 and the `2`/`1`
came from `nodes::detail::konst(g, nenc, 2.0)`. ⚠ **`konst`/`lighting::detail::kf` "shaped like a node" STILL make a SCALAR** —
they copy the node's `Shape` (the element-grid, e.g. `{1}`) + dtype, but NOT the vector COMPONENT width (comps lives in `KType`,
not `Shape`). So `konst(vec3_node, v)` is a 1-comp scalar, and `g.binary(Mul, nenc_vec3, konst)` is the same mismatch. It works
inside `nodes::clamp`/`clamp01` only because `detail::tern` SPLATS. The tell was textbook: GPU rendered the deferred surface
`230,73,52` (correct, == the forward path) while the CPU-oracle `_expected` gave `33,11,8` (dark) — the decoded normal's G/B
comps read OOB → wrong NoL. Fix: `nodes::detail::bin(g, KOp::Sub, nodes::detail::bin(g, KOp::Mul, nenc, two), one)`.

**4th (B12-d burley), 5th (B13-c AgX poly `coef·xⁿ`), 6th hit (2026-07-12, B13-e `cas_sharpen`):** the 6th taught a NEW
wrinkle — **operand ORDER.** CAS had TWO mismatches: `mx + ε` (vec3 + scalar → fixable with `nd::detail::bin(g, Add, mx, ε)`)
AND `1 − mx` (**scalar − vec3**). ⚠ `nd::detail::bin(g, op, A, B)` splats **B** and keeps **A**'s shape — so it only fixes
`vec OP scalar`, NOT `scalar OP vec` (a subtraction/division where the scalar is the LEFT operand and order matters). For
`1 − mx` you must **`g.splat(k(1.0), N)` then `g.binary(Sub, splatted, mx)`** — bin can't reverse the operands. Tell was again
textbook: `[finish]` bit-exact failed **40/60** (channels 1,2 wrong across 20 lanes, channel 0 correct — the oracle reads the
scalar's index-0 for ch0 correctly, OOB for ch1/2). And it renders GPU==GPU pixel-identical (the fix made both backends AND
the oracle agree at `97,88,68`).

**How to apply:** to multiply/add a vecN by a scalar in CKIR, use `nodes::detail::bin(g, op, vec, scalar)` (it splats the
narrower operand) OR `g.splat(scalar, N)` first — NEVER raw `g.binary` across mismatched comps. **For a non-commutative op
with the scalar on the LEFT (`scalar − vec`, `scalar / vec`), `bin` won't help — `g.splat` the scalar to width N first.** Note `konst`/`kf` do NOT save
you (they're scalars). Anytime a NEW lit/deferred/render observable is GPU-correct but oracle-dark/garbage in non-first
channels, suspect this before anything else. When a both-backends
observable shows GPU==GPU but ≠ oracle, suspect a broadcast the GPU does and the same-shape oracle doesn't (this, or a
missing splat), before blaming F32/FMA. Prefer the existing vec3-constant helpers (`kc(x,y,z)`) for multipliers so the
shapes match by construction. Related: [feedback_dx12_hlsl_masks_type_bugs_run_vulkan](device-programs.md#memory-feedback_dx12_hlsl_masks_type_bugs_run_vulkan) (there it was HLSL coercion; here
it's language-level scalar broadcast — both classes of "the GPU is lenient, the oracle is exact").


<!-- end-memory:feedback_ckir_binary_vec_scalar_shape_mismatch_gpu_broadcasts_oracle_oob -->

<a id="memory-feedback_ckir_dispatch_groups_not_threads_plus_bounds_guard"></a>
## feedback_ckir_dispatch_groups_not_threads_plus_bounds_guard

---
name: feedback_ckir_dispatch_groups_not_threads_plus_bounds_guard
description: "eval_cpu_kernel's last arg is WORKGROUPS not threads — passing the element count runs local_size x too much work and mimics an exponential hang; every kernel also needs an explicit tid<N guard"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: b0138d6a-548b-428b-87b2-fe30c9f36f7c
---

`kir::eval_cpu_kernel(g, e, bufs, nbuf, local_size, alloc, num_workgroups)` — the last argument is **workgroups**,
not threads. Passing the element count (`n`) instead of `ceil(n / local_size)` launches **local_size x** (typically
**64x**) the intended work.

**Why it burns time:** the symptom is not a wrong answer, it is a *hang*. A 48x48 filter with a 121-tap window went
from ~14 s to >600 s, which reads exactly like the exponential-DAG-recursion scar
([feedback_ckir_emitter_decl_needs_dag_memo](device-programs.md#memory-feedback_ckir_emitter_decl_needs_dag_memo)) — and I started re-auditing the evaluator's memoization before
checking the dispatch arithmetic. The evaluator *does* memoize (`memo` + `memo_gen`, keyed by a generation bumped
per top-level statement); it was never the problem.

**How to apply:**
- At every `eval_cpu_kernel` call for a one-lane-per-element kernel, write
  `const crd::u32 groups = (n + e.local_size[0] - 1U) / e.local_size[0];` — never pass `n`.
- **Independently**, give the kernel a real bounds guard, because a GPU dispatch rounds up to workgroup granularity
  and the tail workgroup *always* launches out-of-range lanes:
  `const int guard = g.stmt_if_begin(g.binary(KOp::CmpLt, tid, cu(total))); ... g.stmt_if_end(guard);`
  (established idiom — see `ckir_oit.hpp`). Without it, tail lanes RMW-accumulate into a clamped address and
  silently corrupt the last element. Never let correctness depend on `n` being a multiple of the workgroup size.
- Gate it: dispatch a deliberately non-multiple size (e.g. 90 px over 2 groups of 64 = 38 tail lanes) in the
  Vulkan/DX12 tests. The CPU oracle alone will not expose a missing guard.
- Diagnose slowness by *measuring where*, not by theorizing — the same lesson as
  [feedback_timeout_is_not_a_hang_proof](workflow-and-correctness.md#memory-feedback_timeout_is_not_a_hang_proof).


<!-- end-memory:feedback_ckir_dispatch_groups_not_threads_plus_bounds_guard -->

<a id="memory-feedback_ckir_emitter_decl_needs_dag_memo"></a>
## feedback_ckir_emitter_decl_needs_dag_memo

---
name: feedback_ckir_emitter_decl_needs_dag_memo
description: "CKIR compute emitters' decl() had no visit memo — exponential re-traversal on deep diamond DAGs hung emission outright; fixed with declseen in both GLSL and HLSL"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: b0138d6a-548b-428b-87b2-fe30c9f36f7c
---

⛔ **`emit_compute_kernel_{glsl,hlsl}` could HANG (not slow — hang) on a deep, diamond-shaped graph.**
Fixed 2026-07-19 in both emitters; if you see emission never return, this is the shape to suspect.

**Mechanism.** `decl()` walks a node's children to declare temps before use:
```
decl(node): recurse a, b, c;  if (!inline(op) && !temped[node]) { temped[node]=1; emit "t<node> = rhs"; }
```
It recursed into **every child unconditionally** — `temped[]` gated only the temp EMISSION, never the
DESCENT. So a node referenced by k parents had its **entire subtree re-walked k times**: traversal cost
exponential in DAG depth × reuse. Shallow/linear kernels (FFT, RTAO, clouds) never noticed. B18-b's Huang
BCSDF — stacked `normalize()`/`cross()`/VNDF chains, where every `v3norm` triples references to its input
— made emission never terminate.

**Fix (both `ckir_glsl.hpp` and `ckir_hlsl.hpp`):** a `declseen` u8 array; `decl()` returns early on a
repeat visit. **Behaviour-IDENTICAL** — a re-visit emits nothing anyway (temped[] already 1, children
already declared) — but linear instead of exponential. Emitted source for the kernel that hung: 35,896
chars, i.e. the blowup was ALL traversal, zero real complexity.

**⭐ Diagnosis lesson — MEASURE WHERE IT HANGS, don't theorize.** I first blamed `spirv-opt` (disabled it:
still hung), then select-inlining (added `stmt_materialize`: still hung). What actually settled it in one
step: `printf` the emitted source length + `fflush` right after the emit call. **Nothing printed** ⇒ the
emitter itself never returned ⇒ neither the shader compiler nor the text size was the problem. One probe
beat two wrong hypotheses.

Related: [feedback_ckir_gpu_dispatch_binding_cap_and_sort_unroll_explosion](device-programs.md#memory-feedback_ckir_gpu_dispatch_binding_cap_and_sort_unroll_explosion) (the "unrolled sort selects
explode emit" scar is the same family — but its real mechanism is likely this too);
[feedback_ckir_if_block_shared_temp_scope_materialize](rendering.md#memory-feedback_ckir_if_block_shared_temp_scope_materialize) (why `temped[]` is global and not scope-aware).


<!-- end-memory:feedback_ckir_emitter_decl_needs_dag_memo -->

<a id="memory-feedback_ckir_eval_for_bound_is_uniform_use_max_plus_forbreakif"></a>
## feedback_ckir_eval_for_bound_is_uniform_use_max_plus_forbreakif

---
name: feedback_ckir_eval_for_bound_is_uniform_use_max_plus_forbreakif
description: "eval_cpu_kernel (and the GPU) runs the workgroup in LOCKSTEP — a For loop's trip count is UNIFORM (eval reads active[0]'s count for ALL threads). A per-thread DIVERGENT count silently reads as thread-0's. Express divergent loops as a uniform max bound + a per-thread ForBreakIf."
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 192f4d9e-b3f7-485b-a775-5391bcd4183c
  modified: 2026-08-18T12:01:45.510Z
---

`kir::eval_cpu_kernel` is a **lockstep (SIMT) interpreter** (ckir_kernel_eval.hpp `KStmtKind::For`:
`tid = active[0]; const int cnt = eval(st.value)` — "the bound is uniform across the workgroup").
A `stmt_for_begin(count)` where `count` is a **per-thread (divergent)** value node
(e.g. `starts[gid+1] - starts[gid]`) evaluates the trip count for **thread 0 only** and runs that many
iterations for **every** thread — the body's per-thread values (start, gid) stay correct, so the bug is
SILENT and data-shaped, not a crash.

**Scar (CEIR-23d-1 micro-gate):** a segmented-sum kernel — `y[gid] = sum vals[k], k in [starts[gid],
starts[gid+1])` — with a bare divergent `stmt_for(count)` gave `3, 7, 13, 15` for expected `3, 12, 6,
24`: EVERY segment ran exactly 2 iterations (thread 0's count) with its OWN start. Round-trip + emit
were fine; only eval was wrong. (This is the SpMV inner loop `row_ptr[i]..row_ptr[i+1]` shape.)

**Why:** GPUs run a workgroup in lockstep; a genuinely divergent trip count is modelled by MASKING
threads that finish early, not by different loop bounds. The eval mirrors the hardware: uniform bound +
per-thread active-set shrink via `ForBreakIf`.

**How to apply — the divergent-loop portable form** (the `KGraph::while_loop` shape):
```
const int fid = g.stmt_for_begin(MAX);          // UNIFORM upper bound >= max per-thread trip count
const int it  = g.kernel_loop_var(fid);         // uniform iteration
g.stmt_for_break_if(g.binary(KOp::CmpGe, it, count)); // per-thread break: it >= my count -> drop (DIRECT child of the For)
... body using (start + it) ...                 // only threads still active accumulate
g.stmt_for_end(fid);
```
- `ForBreakIf` KEEPS threads whose cond == 0, DROPS threads whose cond != 0, for the loop's REMAINING
  iterations (incl. the current one's remaining body). Put it FIRST in the body so a finished thread
  does no out-of-range load/store. It MUST be a direct child of its For (not nested under a divergent If).
- The uniform `MAX` is the max per-thread trip count (for SpMV: the max row length — a uniform push-const
  or a safe over-estimate; the loop exits early once `loop_active` empties, so a loose MAX only wastes
  no-op iterations, never miscomputes).
- The `for_loop`/LoopAcc value-reduction is a SEPARATE mechanism (F32 LoopIndex — awkward for integer
  indexing); for a per-thread accumulate over an integer range, prefer imperative For (U32
  `kernel_loop_var`) + RMW on the output buffer.

Pins the 23e authored SpMV `.ckir` kernel shape. Related: [feedback_eval_cpu_kernel_is_scalar_use_localinvocationindex](workflow-and-correctness.md#memory-feedback_eval_cpu_kernel_is_scalar_use_localinvocationindex).


<!-- end-memory:feedback_ckir_eval_for_bound_is_uniform_use_max_plus_forbreakif -->

<a id="memory-feedback_ckir_fft_batched_radix_dispatch_breaks_fixed_twiddle_contract"></a>
## feedback_ckir_fft_batched_radix_dispatch_breaks_fixed_twiddle_contract

---
name: feedback_ckir_fft_batched_radix_dispatch_breaks_fixed_twiddle_contract
description: "CKIR build_fft1d_batched picks the FFT radix by log2(n) (radix16/8/4/2), each with a DIFFERENT twiddle layout + local_size; a caller that provisions a FIXED radix-2 twiddle table (n/2) + local_size must use build_fft1d_radix2, else eval_cpu_kernel mis-simulates and HANGS (not a clean fail)"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 192f4d9e-b3f7-485b-a775-5391bcd4183c
  modified: 2026-08-18T01:50:40.283Z
---

`kir::build_fft1d_batched(g, n, inverse)` (engine/kir/include/crd/kir/ckir_fft.hpp:1505) is a RADIX
DISPATCHER: it picks radix-16 / radix-8 / radix-4 / radix-2 by `fft_log2(n)` (e.g. **n=8 → radix-8**,
because `log2(8)=3` and `3%3==0`). Each radix has a DIFFERENT `local_size` (n/radix threads) AND a
DIFFERENT twiddle-table layout. So a caller that hard-codes the **radix-2 convention** — `local_size = n/2`,
`n/2` twiddles `W_N^k = (cos, -sin)` in buffers 2/3 — silently mismatches whatever radix the dispatcher
picked.

**The failure mode is a HANG, not a clean assertion.** At CEIR-22b the CEIR→CKIR fft provider (`synth_fft`)
called `build_fft1d_batched` while the device gate supplied radix-2 twiddles + passed `local_size = n/2`
to `eval_cpu_kernel`. For n=8 that fed a radix-8 kernel the wrong local_size → `eval_cpu_kernel`
mis-simulated the thread grid → the test ran the full **1500 s ctest timeout** before failing. (The
device-free synth test passed the whole time — it only checked `reject`/`n`, not execution; only the
DEVICE gate surfaced it.)

**Rule:** if you provision a FIXED twiddle/local_size layout, synthesize with the SPECIFIC radix builder
(`build_fft1d_radix2` for the n/2 convention — it handles any 2^k, Stockham), NOT `build_fft1d_batched`.
The batched radix-dispatch is a PERF optimization (name-forward it — a schedule-selection slice); a
correctness/oracle gate wants the stable single-radix contract.

**Check:** pin the contract DEVICE-FREE — `CHECK(plan.entry.local_size[0] == n/2)` in the synth unit test.
That is the ONLY cheap detector of a batched-radix regression; without it the sole signal is a 1500 s hang.
Also cap the device gate's ctest `--timeout` so a future mismatch fails in ~2 min, not 25.

This WILL recur wherever FFTs are synthesized (CEIR-22c's GEMM→FFT→reduction proof builds one again).
Related: [feedback_ceir_hook_op_name_compare_must_be_dialect_qualified](execution-ir.md#memory-feedback_ceir_hook_op_name_compare_must_be_dialect_qualified) (another "device-free passes,
device catches it" scar), [feedback_native_gpucommand_capability_tier_kernel_ref_is_cooktime_not_execution_tier](device-programs.md#memory-feedback_native_gpucommand_capability_tier_kernel_ref_is_cooktime_not_execution_tier).


<!-- end-memory:feedback_ckir_fft_batched_radix_dispatch_breaks_fixed_twiddle_contract -->

<a id="memory-feedback_ckir_gpu_dispatch_binding_cap_and_sort_unroll_explosion"></a>
## feedback_ckir_gpu_dispatch_binding_cap_and_sort_unroll_explosion

---
name: feedback_ckir_gpu_dispatch_binding_cap_and_sort_unroll_explosion
description: "Two B17-scalable gotchas — the compute descriptor cap is 8 bindings (>8 → null pipeline → silent segfault; pack into a node pool), and the unrolled sort-network's INLINE selects explode the emit at high layer count."
metadata: 
  node_type: memory
  type: feedback
  originSessionId: b0138d6a-548b-428b-87b2-fe30c9f36f7c
---

Two gotchas found closing the B17 scalable OIT tiers (2026-07-19), both silent:

**1. Compute descriptor cap = 8 bindings.** `VulkanComputeContext::create_pipeline_from_spirv` (and DX12's) return
**nullptr** when `n_bindings > kMaxBindings` (= 8, `vulkan_compute_context.cpp`). The dispatch helpers then `*pipe`-deref the
null → **SIGSEGV** with no error. The first atomic A-buffer used 9 buffers (scene/counter/head/nnext + nr/ng/nb/na/nd) → over
cap → segfault that *looked* like a shader-compile failure. **Fix = the gold layout anyway:** pack node attributes into ONE
interleaved pool — a stride-5 f32 `[r,g,b,a,depth]` buffer + a parallel u32 `next` array (the AAA `struct Node{vec4 col; float
z; uint next;}` factored into CKIR's single-dtype buffers). 5 bindings for build, 4 for resolve. **How to apply:** keep compute
kernels ≤8 SSBO bindings; pack SoA attributes into an interleaved pool rather than one-buffer-per-attribute. If you truly need
>8, raise `kMaxBindings` AND size the descriptor pool — but packing is the better design.

**2. The unrolled compare-exchange SORT network explodes the emit at high layer count.** The A-buffer/MBOIT resolve sorts
`layers` fragments with a fully-unrolled bubble network of O(layers²) compare-exchanges, each a chain of `select` nodes. The
compute emitter keeps **select/compare/bitops INLINE** (not materialized as temps — see ckir_glsl.hpp), so `pv(dd[last])` after
the sort re-emits the ENTIRE nested-select tree → **exponential GLSL string** in the sort depth. 4 layers (6 CX) is fine and is
the correctness config; **8 layers (28 CX) OOM'd the host TlsfAllocator** building the emit string. **How to apply:** the OIT
resolves are validated at layers=4; for a high-overdraw perf board push PIXEL count (1024²), not layer count. Lifting the layer
ceiling needs the sort's select intermediates temped (a separate emitter axis), not just a bigger allocator.

**Also (feature, not a scar):** value-returning atomics now exist in CKIR — `atomic_add_fetch` / `atomic_exchange` builders push
an `AtomicResult` leaf the stmt materializes once (impure, non-CSE). Lowered all 5 backends: GLSL `atomicAdd`/`atomicExchange`,
HLSL `RWByteAddressBuffer.InterlockedAdd/Exchange(off,val,ORIG)` (out-param form), CUDA `atomicAdd`/`atomicExch`, MSL
`atomic_fetch_add/exchange_explicit`, WGSL `atomicAdd`/`atomicExchange`. See [feedback_raster_emitters_lag_compute_wire_both_and_uint_suffix](workflow-and-correctness.md#memory-feedback_raster_emitters_lag_compute_wire_both_and_uint_suffix).


<!-- end-memory:feedback_ckir_gpu_dispatch_binding_cap_and_sort_unroll_explosion -->

<a id="memory-feedback_ckir_host_config_bakes_into_shader_variant_explosion"></a>
## feedback_ckir_host_config_bakes_into_shader_variant_explosion

---
name: feedback_ckir_host_config_bakes_into_shader_variant_explosion
description: CKIR host-config doubles bake into the emitted shader as literals — varying one per draw makes every instance a distinct shader and turns a GPU render compile-bound; put per-instance authoring params in BUFFERS
metadata: 
  node_type: memory
  type: feedback
  originSessionId: b0138d6a-548b-428b-87b2-fe30c9f36f7c
---

Anything passed through a `*Config` struct into a CKIR builder becomes a **literal in the emitted GLSL/HLSL**. So a
parameter that varies per instance produces a *different shader per instance*.

**The measurement** (B18 hair showcase, 900 tufts/frame, 2026-07-20): `StrandGenConfig::curl_amp` / `curl_freq` were
randomised per bundle, so 900 bundles emitted 900 distinct sources → **903 GLSL→SPIR-V compiles per frame** at ~35 ms
each. The render was **compile-bound, not compute-bound**. Quantising the two params to a 12-entry palette dropped it
to **15 compiles/frame** and the batch from **6m05 → 3m31**. Nothing about the maths changed.

**How to apply:**
- Before varying a builder config value per-object, ask: *does this bake into the shader?* If yes, either quantise it
  to a small palette or promote it to a **buffer input** (the strand kernel's layer geometry and `(u,v)` already are,
  which is why they can vary freely per bundle at zero shader cost).
- Cache compiled pipelines keyed on the emitted source text — cheap, exact, and it makes the explosion *visible*
  (log kernels-dispatched vs shaders-compiled; a ratio near 1 is the smell).
- A GPU render being slow is not evidence the GPU work is slow. Instrument the compile count and the dispatch count
  separately before optimising anything — same discipline as
  [feedback_ckir_dispatch_groups_not_threads_plus_bounds_guard](device-programs.md#memory-feedback_ckir_dispatch_groups_not_threads_plus_bounds_guard).

**Still open after the fix:** ~900 tiny per-bundle dispatches remain, each paying a full buffer-create + upload +
submit + fence + readback (~25 ms). The real fix is batching all bundles into ONE dispatch (runtime-index the layer
buffer by `tid / per_bundle` instead of host-indexing a single bundle) — a B18-d kernel change.

Related: [project_gpu_context_owns_every_gpu_program](project-history.md#memory-project_gpu_context_owns_every_gpu_program).


<!-- end-memory:feedback_ckir_host_config_bakes_into_shader_variant_explosion -->

<a id="memory-feedback_ckir_inline_buffer_load_read_after_write"></a>
## feedback_ckir_inline_buffer_load_read_after_write

---
name: feedback_ckir_inline_buffer_load_read_after_write
description: CKIR buffer loads are INLINE (re-read at each use) — an expression that loads slot k silently reads MUTATED memory if an earlier store in the same sequence overwrote k; materialize raw values before any store
metadata: 
  node_type: memory
  type: feedback
  originSessionId: b0138d6a-548b-428b-87b2-fe30c9f36f7c
---

⛔ **A post-loop normalization that reads and writes the same buffer slots will silently read MUTATED
memory.** Symptom: one output field comes out exactly `0` (or exactly equal to another field) while every
neighbouring field looks perfect.

**Mechanism.** In the CKIR compute emitters, `BufferLoad` is an **inline op** — deliberately, so a load
RE-READS at each use and therefore stays correct across barriers (see the emitter's own comment). But that
means a graph expression holding `buffer_load(out, k)` is re-evaluated *at the point it is finally emitted*.
If an earlier `stmt_buffer_store` in the same statement sequence already wrote slot `k`, the expression
reads the NEW value. Graph-construction order is NOT evaluation order.

**The scar (B18-c scattering-moment LUT, 2026-07-19).** Post-loop I normalized 5 accumulator slots:
```
bf2 = load(2)/af ;  db = load(3)/ab ;  sb2 = load(4)/ab − sq(db)      // db still holds load(3)
store(2,bf2) ; store(3,db) ; store(4,sb2)                            // ← store(3) clobbers slot 3
```
By the time `sb2` was emitted, `store(3, db)` had already run, so `db` re-read the *normalized* value and
`sb2` collapsed to **exactly 0** — silently killing dual scattering's entire backscattering lobe (f_back = 0)
while ā_f/ā_b/β̄f² all looked correct. **Fix:** load every raw accumulator and `g.stmt_materialize(...)` it
BEFORE the first store, so all moments are computed from frozen pre-store temps.

**⭐ Rules.** (1) In any read-modify-write over a buffer, **materialize the raw loads first** — never let a
load survive across a store to the same buffer. (2) A field that is *exactly* 0 / exactly equal to a
sibling is a mutation-ordering smell, not a math smell. (3) Print the intermediate table (here the LUT row)
— `sb2=0` next to healthy neighbours localized this in one run, where reading the math would not have.

Related: [feedback_ckir_if_block_shared_temp_scope_materialize](rendering.md#memory-feedback_ckir_if_block_shared_temp_scope_materialize) (the other `stmt_materialize` duty —
scope, not ordering); [feedback_ckir_emitter_decl_needs_dag_memo](device-programs.md#memory-feedback_ckir_emitter_decl_needs_dag_memo) (same emitter, traversal cost).


<!-- end-memory:feedback_ckir_inline_buffer_load_read_after_write -->

<a id="memory-feedback_ckir_kernel_eval_is_scalar_vec3_evaluates_to_garbage"></a>
## feedback_ckir_kernel_eval_is_scalar_vec3_evaluates_to_garbage

---
name: feedback_ckir_kernel_eval_is_scalar_vec3_evaluates_to_garbage
description: eval_cpu_kernel is SCALAR - Vec3/VecComp/Swizzle/Dot/Cross had no case and silently evaluated to garbage; compute kernels must write vector maths component-wise on scalar nodes
metadata: 
  node_type: memory
  type: feedback
  originSessionId: b0138d6a-548b-428b-87b2-fe30c9f36f7c
---

`eval_cpu_kernel` (the CKIR **statement tier**) stores ONE f64 per node per lane. It had **no case** for
`Vec2`/`Vec3`/`VecComp`/`VecConcat`/`Swizzle`/`Splat`/`Dot`/`Cross` — they fell through to the `default:` arm, were
handed to `apply_ternary`/`apply_unary`, which do not implement them, and evaluated to **garbage with no diagnostic**.

**Why:** the vec3 forms of the shading library (e.g. `hair::hair_bcsdf_eval(g, wo, wi, ...)`) were written for the
RASTER tier, where the emitters lower them to native vector types. Nothing stopped a COMPUTE kernel from calling them,
and nothing complained when it did — the kernel ran and produced plausible numbers.

**How to apply:** in a compute kernel, write vector maths component-wise on scalar nodes (the `detail::V3` pattern in
`ckir_lss.hpp`), and call the SCALAR-ANGLE core (`hair_bcsdf_eval_angles`) rather than the vec3 wrapper. As of
2026-07-20 the evaluator ASSERTS on these ops instead of guessing, so occurrence #2 fails loudly.

**The hour this cost (B18-f, 2026-07-20):** the RT hair shading gate disagreed with an independent reference. `h` was
provably correct (dumped: 0.8, 0.6, 0.4, 0.2, 0 — exactly the hand-computed values) and `dot3(wi, Z)` was provably
correct (= 1), but `vec_comp(wi_f, 2)` read back **0** — the vec3 carrying the frame into the BCSDF collapsed its z
component. So φ was 0 instead of π/2, and the result came out **symmetric in h**, which is physically plausible for a
fibre and completely wrong. The tell was that symmetry, not any error message.

Diagnosis that worked: dump the intermediate (`h`, then a frame component) from the kernel itself and compare against
the hand value — the same measure-before-changing-code discipline as [feedback_ckir_inline_buffer_load_read_after_write](device-programs.md#memory-feedback_ckir_inline_buffer_load_read_after_write).

Related: [feedback_compute_kernel_emitter_lacked_exp_pow](workflow-and-correctness.md#memory-feedback_compute_kernel_emitter_lacked_exp_pow) (the compute tier lagging the raster tier is a RECURRING
shape, not a one-off), [feedback_ckir_emitter_decl_needs_dag_memo](device-programs.md#memory-feedback_ckir_emitter_decl_needs_dag_memo), [feedback_ckir_oracle_u32_arithmetic_must_wrap_mod32](device-programs.md#memory-feedback_ckir_oracle_u32_arithmetic_must_wrap_mod32).


<!-- end-memory:feedback_ckir_kernel_eval_is_scalar_vec3_evaluates_to_garbage -->

<a id="memory-feedback_ckir_node_refs_are_positional_n_index_id_ignored"></a>
## feedback_ckir_node_refs_are_positional_n_index_id_ignored

---
name: feedback_ckir_node_refs_are_positional_n_index_id_ignored
description: "Hand-authored .ckir node refs are \"n<POOL-INDEX>\" (position = identity); the id string is ignored, so non-n / non-positional ids silently break ckir_read"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 192f4d9e-b3f7-485b-a775-5391bcd4183c
  modified: 2026-08-17T19:03:41.122Z
---

When hand-authoring a `.ckir` asset, every operand/stmt reference (`target`, `index`, `value`, `in = [...]`,
`out.node`) MUST be the string `"n<POOL-INDEX>"` where the number is the node's **position in the file's
`[[node]]` order** (0-based). `ckir_read`'s `noderef()` (engine/kir/include/crd/kir/ckir_asset.hpp) requires
the ref to start with `'n'` then digits, and returns that number **as the node index** — it does NOT look up
the `id` string. The `[[node]] id = "..."` field is **parsed and IGNORED** ("order authoritative"): you can
name a node anything, but references resolve by position, not by id.

**Scar (CEIR-20c-2):** I authored `work_smoke_produce_dgc.ckir` with ids `v1`, `v5`, `i0..i14` and referenced
them by those ids. `ckir_read` failed at parse (`noderef` saw `'v'`/`'i'` != `'n'`, or a numeric suffix that
did not equal the pool position). A ref like `"i5"` at file position 9 would also silently resolve to node 5 —
a WRONG-NODE miscompile, not even an error. Fix: rename every node to `n0..nN` in strict declaration order and
reference by the positional index.

**Why:** the `.ckir` node graph is a flat pool; the writer (`ckir_write` / `w_noderef`) always emits `"n<idx>"`
with idx == pool slot. Authoring by hand must match that canonical form exactly.

**How to apply:**
- Declare nodes in strict `n0, n1, n2, …` order; the id number == the position. A const reused as both an
  index and a value is ONE node (see the original `work_smoke_produce.ckir`: n3 is both index-1 and value-1).
- `cval` for an integer const is the **f64 bit-pattern** (e.g. 1→`0x3ff0000000000000`, 5→`0x4014000000000000`,
  N→the IEEE-754 double of N); a `Const` with no `cval` is 0. See [feedback_ckir_oracle_u32_arithmetic_must_wrap_mod32](device-programs.md#memory-feedback_ckir_oracle_u32_arithmetic_must_wrap_mod32).
- The FIRST validation after authoring is a `ckir_read` round-trip (then `emit_compute_kernel_glsl`/`_hlsl`):
  it is the first thing that fails on a bad node/stmt/ref/cval. Do it before any device gate — a failed
  materialization must surface, never read as green ([feedback_verifier_that_materializes_its_input_must_surface_a_failed_materialization](rendering.md#memory-feedback_verifier_that_materializes_its_input_must_surface_a_failed_materialization)).
- `emit_compute_kernel_glsl`/`_hlsl` map each `BufferDecl` `iidx` → std430 `binding = iidx`, so the descriptor
  slot a consumer binds at position N must match the kernel's `iidx = N` decl (the 20c-2 bind-map contract).
- Relates to the "everything is an authorable asset" mandate [feedback_no_cpp_kgraph_builders_author_ckir_directly](build-and-verification.md#memory-feedback_no_cpp_kgraph_builders_author_ckir_directly):
  authoring the `.ckir` directly IS the workflow, so this positional-ref rule is load-bearing.


<!-- end-memory:feedback_ckir_node_refs_are_positional_n_index_id_ignored -->

<a id="memory-feedback_ckir_oracle_u32_arithmetic_must_wrap_mod32"></a>
## feedback_ckir_oracle_u32_arithmetic_must_wrap_mod32

---
name: feedback_ckir_oracle_u32_arithmetic_must_wrap_mod32
description: "CKIR CPU oracle did u32/i32 Add/Sub/Mul/Shl in f64 — no mod-2^32 wrap, bit-loss above 2^53; full-width hash multiplies diverge from every GPU. Fixed with apply_binary_typed."
metadata: 
  node_type: memory
  type: feedback
  originSessionId: b0138d6a-548b-428b-87b2-fe30c9f36f7c
---

The CKIR CPU oracle (`eval_cpu_kernel` in ckir_kernel_eval.hpp, the elementwise `ckir_eval.hpp`, and the const-folder in
ckir.hpp) stores every value as `crd::f64` and computed binary ops via `apply_binary` — which did **integer** ops in f64
(`x * y`), and `round_dtype(U32)` only did `(f64)(i64)v` (truncate) with **no mod-2^32 wrap**. So a full 32-bit avalanche hash
(`h *= 0xED5AD4BB` where h is full-width) is wrong two ways: the product ~2^64 loses its low bits in f64 (mantissa is 52 bits),
and it never wraps mod 2^32. The oracle's hash then diverges from every GPU backend (which wraps natively).

**Symptom (B17-c stochastic transparency, 2026-07-19):** GPU-vs-oracle worst |Δ| = 0.72 (not a rounding issue — a structural
hash-stream mismatch → different screen-door coverage decisions → different colors). The existing hashgrid/NRC/clouds hashes
only worked because their operands stayed **small** (grid coord × prime < 2^53), so no overflow occurred and wrap was a no-op.

**Fix:** added `apply_binary_typed(op, x, y, dt)` next to `apply_binary` in ckir.hpp — for `U32`/`I32` and op ∈
{Add, Sub, Mul, Shl} it computes in real `crd::u32` (two's-complement wrap mod 2^32) and reinterprets for signed. Shr/And/Or/Xor
and all float ops fall through to `apply_binary` (already exact for in-range integers). Wired into BOTH evaluators and the
const-fold path. It is a **no-op for non-overflowing ops** (so zero regression — full kir suite stayed green at 34759
assertions) and **strictly correct** for overflowing ones.

**Why:** the mission is all-backends **bit-exact**; an oracle that can't model a u32 multiply is a portability gap that silently
passes GPU==GPU (both wrap identically) while GPU==oracle fails. **How to apply:** any new integer-PRNG / hash / bit-mixing
kernel is now bit-exact-portable through the oracle — use full-width hashes freely. For a deterministic uniform in [0,1) that is
bit-exact GPU==oracle, take the **top 24 bits** (`float(h >> 8) * (1.0/16777216.0)`) so the u32→f32 cast is exact (≤2^24). See
[feedback_bit_exact_blind_to_symmetric_bugs_energy_comp](workflow-and-correctness.md#memory-feedback_bit_exact_blind_to_symmetric_bugs_energy_comp) (the oracle is the reference — a hole in it hides real divergence).


<!-- end-memory:feedback_ckir_oracle_u32_arithmetic_must_wrap_mod32 -->

<a id="memory-feedback_ckir_rt_inline_rayquery_scars"></a>
## feedback_ckir_rt_inline_rayquery_scars

---
name: feedback_ckir_rt_inline_rayquery_scars
description: RT-1 inline ray query (CKIR + Vulkan) scars — GL_EXT_ray_query needs
metadata: 
  node_type: memory
  type: feedback
  originSessionId: b0138d6a-548b-428b-87b2-fe30c9f36f7c
---

Building the CKIR inline-ray-query vertical (D-007 RT-1, 2026-07-19: `AccelStructDecl`/`RayHitResult`/`TraceRayClosest` IR →
GLSL `GL_EXT_ray_query`/HLSL inline `RayQuery<>` → `VulkanRayTracingContext` BLAS/TLAS build → GPU trace). The scars, all
expensive to rediscover:

**Shader compile (the "shaderc rejects rayQueryEXT" red herring):**
- `GL_EXT_ray_query`'s `rayQueryEXT` type requires **`#version 460`** — `#version 450` fails with `rayQueryEXT undeclared
  identifier` even at a Vulkan-1.3 / SPIR-V-1.6 target and even in a fresh glslang. It was NOT a shaderc version problem
  (glslangValidator with `--target-env vulkan1.3` compiled the same shader fine). Fix: the compute-kernel emitter emits
  `#version 460` when an `AccelStructDecl` is present (`ckir_glsl.hpp`). Confirm a suspected toolchain bug against the
  standalone `glslangValidator` BEFORE blaming the library.
- HLSL inline `RayQuery<>` / `TraceRayInline` needs **Shader Model 6.5** — `cs_6_0` fails. Bumped `target_profile` compute →
  `cs_6_5` (a backward-compatible superset) in `vulkan_hlsl_compile.cpp`.

**Vulkan acceleration-structure build (`vulkan_ray_tracing_context.cpp`):**
- Enable the trio `VK_KHR_acceleration_structure` + `VK_KHR_ray_query` + `VK_KHR_deferred_host_operations` (extensions) +
  the `bufferDeviceAddress` FEATURE (core in 1.2 — enable the feature, not the `VK_KHR_buffer_device_address` alias). Confirm
  the feature bits via `VkPhysicalDeviceFeatures2` (the mesh-shader gate pattern in `vulkan_context.cpp`).
- ⛔ `vkGetBufferDeviceAddress` — load the **CORE** name via `vkGetDeviceProcAddr`, NOT `vkGetBufferDeviceAddressKHR`: the KHR
  alias returns NULL unless the `VK_KHR_buffer_device_address` EXTENSION is enabled (we enabled only the core feature). A null
  proc silently made the whole RT context `valid()==false`.
- AS-build inputs (vertex/instance/scratch/AS-backing buffers) all need `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT` +
  `VkMemoryAllocateFlagsInfo{VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT}` in the allocate chain — the ordinary compute `create_buffer`
  doesn't do this, so the RT context owns its own device-address buffer helper.
- ⛔ realloc-UAF: build helpers passed a `DevBuffer&` that pointed INTO the `owned` Array, which `make_buffer` then `push_back`s
  into (reallocating) → dangling. Fix: use a LOCAL `DevBuffer`; `make_buffer` stores a COPY in `owned` (the Vulkan handles are
  stable values, freed in the dtor) — never hold a reference into a growing Array (see [feedback_array_push_back_self_reference_uaf](workflow-and-correctness.md#memory-feedback_array_push_back_self_reference_uaf)).
- Destroy the `VkAccelerationStructureKHR` handles (`vkDestroyAccelerationStructureKHR`) in the dtor — track them; destroying
  only the backing buffers LEAKS the AS objects.

**Correctness gate (the honest RT contract):** RT traversal is NOT bit-exact across vendors, so RT tests compare GPU hits
(t / hit-miss / primId) to a CPU **brute-force watertight ray-triangle reference** within GEOMETRIC tolerance (the `TraceRayClosest`
oracle in `ckir_kernel_eval.hpp`), not `bit_equal`. RT-1 matched the reference exactly for a simple scene (t=[2,miss,miss,1]).
Related: [project_b17_oit_two_tiers_and_rtt_blend_capability](project-history.md#memory-project_b17_oit_two_tiers_and_rtt_blend_capability) (the value-returning-atomic `AtomicResult` pattern `RayHitResult`
mirrors).

**The RT effect ladder built on the inline core (all GPU-verified vs the oracle, `ckir_rt.hpp`, 2026-07-19):** RT-2 hard shadows
(`P+t·(L−P)`, tmax<1) → RTAO (cosine-hemisphere batch, occlusion RMW) → RT reflections (`trace_ray_hit`→primId→fetch flat-normal
→shade) → RT-3 path-tracing megakernel (`build_pathtrace_kernel`: runtime sample `For` + UNROLLED bounce chain — CKIR `For`
carries no registers, so origin/dir/throughput/radiance thread as SSA node ids across bounces; diffuse GI, sky-as-light) → RT-4
NEE+MIS area-light path tracer (`build_pathtrace_nee_kernel`).

**RT-4 NEE + MIS scars (the gold-standard direct-lighting integrator):**
- ⛔⛔ **CKIR `select` does NOT guard NaN** — it can lower to arithmetic (`a*m + b*(1-m)`), so `select(mask, x, 0)` with `x`=NaN
  gives `NaN*0 + 0 = NaN`, contaminating the result even when masked off. On a ray MISS, `hit.t` = tmax (1e30); reconstructing
  the hit point `o + t·d` then squaring a distance overflows f32 (>3.4e38 → INF) and `INF/INF` in the MIS weight → NaN. FIX:
  (1) clamp `tc = min(hit.t, 1e5)` before any point reconstruction so all miss-path arithmetic stays finite; (2) guard EVERY
  division denominator with `max(denom, ε)` (`1e-8`/`1e-12`/`1e-20`). Then the {0,1} multiply-masks (`gt0(cos)·vis`, `islight·front`)
  zero the contribution cleanly. Never rely on `select`/masking alone to kill a NaN — kill it at the source.
- MIS validation = the Veach test: emit the SAME kernel under 3 strategies (`PtStrategy` Mis/Nee/Bsdf) and assert their means
  converge (all are unbiased estimators of the same integral). MIS≈NEE is TIGHT at any spp (both low-variance) ⇒ that assertion
  alone proves the power-heuristic weights + geometry/pdf terms are correct; BSDF-only needs more spp (looser bound).
- ⚠ The CPU kernel ORACLE is ~1ms per sample-iteration (it interprets the whole KGraph per sample) ⇒ high-spp convergence tests
  time out. Do convergence validation on the GPU (thousands of spp are cheap); on the CPU keep spp low, set `local_size` = the
  point count so only N threads are interpreted (not a full 64-wide workgroup), and lean on low-variance MIS≈NEE.
- Area light = sampled analytically (corner + 2 edge vectors, `Q=p0+u1·eu+u2·ev`, pdf_area=1/|eu×ev|) AND present in the AS
  (last `light_ntri` tris) so BSDF rays can hit it; the shadow ray uses tmax=`dist·(1−1e-3)` so the light itself isn't an occluder.
  Detect a light hit by primId range `[light_prim0, light_prim0+light_ntri)`. Separate triple32 hash streams for NEE vs BSDF.

**RT-5 ReSTIR DI (Bitterli 2020) — the frontier real-time many-light estimator, built on the NEE substrate:**
- **RIS core** (`build_restir_di_kernel`): stream M candidate light samples into a WEIGHTED-RESERVOIR-SAMPLING reservoir (target
  p̂ = f·Le·G, source pdf 1/area), keep ONE survivor, pay ONE visibility ray. Estimator L = f·Le·G(y)·V·W, W = Σwᵢ/(M·p̂(y)),
  wᵢ = p̂(xᵢ)·area. Reservoir threaded as SSA through the UNROLLED candidate loop; WRS replace = finite-safe mask-blend
  (`repl·Q + (1−repl)·cQ`). Validated unbiased: mean == pure-NEE-direct to 0.07%. GPU==oracle ULP-exact.
- **SPATIOTEMPORAL** (`build_restir_temporal_kernel` → `build_restir_spatial_kernel` → `build_restir_shade_kernel`): PERSISTENT
  6-float/pixel reservoir [Qxyz, W, M], 3 dispatches/frame, ping-ponged. The GPU harness uses `trace_dispatch` bindings with BOTH
  `upload` and `readback` = the same host array (in-out) to carry the reservoir host↔device across passes/frames.
  - The generalised COMBINE (temporal AND spatial): merge reservoir B into A by re-weighting B's sample under A's target p̂:
    w_B = p̂_A(B.y)·B.W·B.M; WRS-pick; W = Σw/(Z·p̂(y)). Same op both because temporal is same-p̂, spatial is different-p̂.
  - ⛔⛔ **SPATIAL reuse MUST use the UNBIASED normalisation** W = Σwᵢ/(**Z**·p̂(y)) where Z = ΣMᵢ over ONLY the reservoirs whose
    domain contains the chosen y (a 2nd neighbour pass recomputes p̂ᵢ(y)>0 at each neighbour's shading point). Using M_total (biased
    combine) darkens at geometry/normal discontinuities. On a flat uniform floor Z=M_total (biased≡unbiased) so a flat test won't
    catch it — the Z path is still mandatory for correctness on varying geometry.
  - ⛔⛔ **Do NOT feed the POST-spatial reservoir back as temporal history** — it compounds spatially-borrowed samples across frames
    and DARKENS the image ~12% (measured). Only the full GRIS / pairwise-MIS weights (Lin 2022) make that unbiased. Feed back the
    PRE-spatial (temporal) reservoir: clean history, spatial acts as a per-frame refinement, provably unbiased.
  - ⛔ **Testing unbiasedness with per-pixel rms is WRONG** — ReSTIR is a 1-spp noisy estimator (denoiser resolves it), so per-pixel
    rms measures VARIANCE, not bias. Test bias via the SPATIAL MEAN (a systematic offset survives averaging; Monte-Carlo noise
    cancels). Show the variance win where it's clean: TEMPORAL on a fully-visible light = 5.8× (warm 0.030 vs single 0.175). A hard
    occluder adds an irreducible per-pixel 1-spp visibility-variance floor (binary V) that reuse can't lower — that's the denoiser's job.
- ⚠ The reservoir kernels need integer pixel coords (u32 `Mod`/`Div` for x,y) + f32↔u32 `Cast` for the neighbour index — all wired
  in BOTH GLSL and HLSL emitters (gated in `test_ckir_glsl_compile.cpp`).

**DX12 DXR mirror (`dx12_ray_tracing_context.hpp/.cpp`, the second backend — VK≈DX12 established):** Dx12RayTracingContext is
standalone (creates its own ID3D12Device5 + compute queue, like Dx12ComputeContext) and gates on
`CheckFeatureSupport(OPTIONS5).RaytracingTier >= TIER_1_1` (inline RayQuery). AS build = ID3D12Device5
`GetRaytracingAccelerationStructurePrebuildInfo` + ID3D12GraphicsCommandList4 `BuildRaytracingAccelerationStructure` (BLAS from a
triangle soup on an UPLOAD vertex buffer → UAV barrier → identity-instance TLAS); result buffers live in state
`D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE`. Dispatch root signature MUST match the emitter's HLSL: the TLAS is a
**ROOT SRV at t0** (`SetComputeRootShaderResourceView(0, tlas_gpu_va)` — a raytracing AS binds directly by GPU VA, no descriptor)
and buffers are a **UAV table u1..u{maxBinding}** (heap slot `binding-1` ↔ register `u{binding}`; buffers start at u1 because u0 is
unused — the TLAS is t0). DEFAULT (UAV, zero-init) + UPLOAD/READBACK staging with explicit barriers. ⛔ inline RayQuery needs SM
6.5 → `compile_hlsl_to_dxil` compute profile bumped `cs_6_0`→`cs_6_5` (a superset; the whole DX12 suite still passes). Result:
the SAME CKIR kernel → HLSL → DXIL matches the CPU oracle byte-for-byte the same as Vulkan (NEE/MIS worst 0.0433693 on BOTH — the
deterministic triple32 sampling makes even the grazing-shadow-ray flips land at identical points).

**RT-6 MULTI-INSTANCE TLAS (`build_scene_instanced`, both backends — the portable frontier-scale capability):** one BLAS + a TLAS
of N instances, each with a row-major 3×4 world transform (Vulkan `VkAccelerationStructureInstanceKHR.transform` /
D3D12 `D3D12_RAYTRACING_INSTANCE_DESC.Transform`) + instanceCustomIndex/InstanceID for per-instance data. The hardware applies each
transform during traversal → validated GPU==oracle by pre-baking the instances to world space in the oracle geometry (t=[2,2,2,∞]
identical on VK + DX12). ⚠ The literal SER (NVIDIA `VK_NV_ray_tracing_invocation_reorder`) / OMM (`VK_EXT_opacity_micromap`) /
cluster (RTX Mega-Geometry) are VENDOR-LOCKED hardware extensions — perf/niche, patchy cross-vendor support — so they're OUT of
the portable both-backend "bit-exact vs oracle" contract; do them HW-gated later if a specific target needs them, not as the
portable frontier core (which is instancing).

**RT-7 MANY-LIGHTS NEE (`build_manylight_nee_kernel`) — the integrator-breadth capability RIS/ReSTIR exist for:** the N area lights
live in a RUNTIME BUFFER (15 floats each: p0.xyz, eu.xyz, ev.xyz, nl.xyz, Le.xyz — NOT baked constants), each sample picks a light
UNIFORMLY (`l = ⌊u·N⌋`, Floor + Cast-to-u32), samples a point, shadow-rays it, adds `f·Le·G·V / pdf` with pdf = (1/N)(1/areaₗ) ⇒
the weight carries `×N·areaₗ` and the area is computed IN-KERNEL as `|eu×ev|`. Provably unbiased: its mean = Σₗ ∫ f·Leₗ·V·Gₗ.
⚠ Validate this on the GPU (N-light kernel mean == Σ per-light-alone runs, 0.16%) — the CPU oracle is far too slow for a high-spp
convergence run of this heavier per-sample kernel (light select + cross-product + trace); keep the CPU side a low-spp eval smoke.

**RT-IB1 full path tracer (many-lights NEE+MIS + emissive hits + Russian roulette + GI, `build_pathtrace_full_kernel`):** works,
unbiased (RR-on mean == RR-off mean exactly; survivors boosted ÷p), GPU==oracle ULP. ⛔⛔ AUTHORING TRAP: `cf(x)` makes a
CONSTANT literal of value `x` — passing a NODE ID by mistake (`cf(area)` where `area = g.unary(Sqrt,…)` is an int node id) silently
builds a constant equal to that id number (~40-70), inflating the result ~80×. **GPU==oracle does NOT catch this** — both compute
the same wrong constant. Only a physical-magnitude sanity check found it (mean 13.9 vs expected ~0.1). To multiply by a computed
node, pass the NODE directly (`mul(area, cf(N))`), never `cf(node)`. Sanity-check magnitudes, not just GPU==oracle agreement (see
[feedback_bit_exact_blind_to_symmetric_bugs_energy_comp](workflow-and-correctness.md#memory-feedback_bit_exact_blind_to_symmetric_bugs_energy_comp) — bit-exact/self-consistent is blind to shared scaling bugs).

**FA-1 OPACITY MICROMAPS (VK_EXT_opacity_micromap, `build_scene_omm`) — RUN-verified on the RTX 4070:** all 4 vendor RT extensions
are enabled in `vulkan_context.cpp` (OMM / VK_KHR_ray_tracing_pipeline / VK_NV_ray_tracing_invocation_reorder / cluster-AS) via
the standard pattern: detect in the ext scan → gate `m_x = m_ray_query && has_x` → feature-probe (chain the FeaturesEXT/NV structs
into a VkPhysicalDeviceFeatures2, confirm the bit, then keep only the bit) → add to the pNext chain → `devexts[28]` (BUMPED from
16) → `bool m_x` member + virtual accessor in vulkan_context.hpp + impl. The micromap build: VkMicromapEXT (2-state), a per-tri
`VkMicromapTriangleEXT{dataOffset,subdivisionLevel,format}` array + `VkMicromapUsageEXT{count,subdiv,format}` counts + packed
opacity bits (4^subdiv bits, buffer usage `VK_BUFFER_USAGE_MICROMAP_BUILD_INPUT_READ_ONLY_BIT_EXT` / build+`_STORAGE_BIT_EXT`),
`vkGetMicromapBuildSizesEXT`→`vkCreateMicromapEXT`→`vkCmdBuildMicromapsEXT`. Attach: `VkAccelerationStructureTrianglesOpacityMicromapEXT`
in the triangle geometry's pNext with a per-triangle OMM-index buffer (tri0→0, others→`VK_OPACITY_MICROMAP_SPECIAL_INDEX_FULLY_OPAQUE_EXT`,
indexType `VK_INDEX_TYPE_UINT32` — there is NO `VK_INDEX_TYPE_INT32_EXT`; the special indices are negative bit patterns), and the
geometry flags = 0 (NOT opaque) so the OMM is consulted. ⛔⛔ THE INLINE-RAYQUERY EMIT FORCED `gl_RayFlagsOpaqueEXT` which OVERRIDES
the OMM (everything hits) → change to `gl_RayFlagsNoneEXT`: opaque geometry still auto-commits (2-state OMM auto-resolves too, no
confirm loop / any-hit needed), so no regression, and now the OMM passes rays through transparent micro-triangles (verified: 16
front-hits / 18 pass-through-to-back).

**FA-2 RT PIPELINE + SER (`trace_rays_pipeline`) — RUN on the RTX 4070:** shaderc RT stages wired
(RayGen/ClosestHit/Miss/AnyHit/Intersection/Callable → shaderc_*_shader; the ShaderStage enum already had them). Build a
VkRayTracingPipelineKHR (3 stages → 3 groups: GENERAL raygen + GENERAL miss + TRIANGLES_HIT_GROUP closest-hit,
maxPipelineRayRecursionDepth=1), an SBT of 3 base-aligned regions (each = align_up(shaderGroupHandleSize, shaderGroupBaseAlignment)
from VkPhysicalDeviceRayTracingPipelinePropertiesKHR) filled from vkGetRayTracingShaderGroupHandlesKHR, then vkCmdTraceRaysKHR with
3 VkStridedDeviceAddressRegionKHR. SER lives in the RAYGEN SPIR-V (GL_NV_shader_invocation_reorder: `hitObjectNV h;
hitObjectTraceRayNV(h,…); reorderThreadNV(h); hitObjectExecuteShaderNV(h,0);`) — a perf reorder that leaves results identical
(t=[2,∞,∞,1] == oracle). Payload passes via `rayPayloadEXT`/`rayPayloadInEXT` at `location=0`.

**FA-3 CLUSTER-AS (`build_scene_clusters`, VK_NV_cluster_acceleration_structure / RTX Mega-Geometry) — RUN on the RTX 4070:** two
GPU-driven INDIRECT builds via `vkCmdBuildClusterAccelerationStructureIndirectNV`, sizes from
`vkGetClusterAccelerationStructureBuildSizesNV`, alignments from VkPhysicalDeviceClusterAccelerationStructurePropertiesNV. Pass 1
(opType BUILD_TRIANGLE_CLUSTER, implicit destinations): a per-cluster `VkClusterAccelerationStructureBuildTriangleClusterInfoNV`
(bitfields triangleCount:9/vertexCount:9/indexType:4 = INDEX_FORMAT_32BIT, vertex+index buffers, byte strides 12/4). Pass 2 (opType
BUILD_CLUSTERS_BOTTOM_LEVEL): `VkClusterAccelerationStructureBuildClustersBottomLevelInfoNV{clusterReferences = pass-1
dstAddressesArray buffer address}` — the CLAS address is chained GPU-side (no host readback), with an AS-build barrier between the
two commands. Read back the BLAS address (host-visible dst buffer), build a normal TLAS instance referencing it. ⛔⛔ SCARS (all
found via VK_LAYER_KHRONOS_validation — ALWAYS run it for a new AS/build API): (1) srcInfosArray + srcInfosCount buffers need
`VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR`; dstAddressesArray + dstImplicitData need
`VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR`; the dst-addresses buffer that's ALSO read as clusterReferences needs BOTH.
(2) the pipeline barrier srcAccessMask must be `ACCELERATION_STRUCTURE_WRITE_BIT_KHR` only (SHADER_WRITE isn't valid for the
AS-build stage). (3) ⛔ the cluster triangle geometry MUST set `baseGeometryIndexAndGeometryFlags.geometryFlags =
GEOMETRY_OPAQUE_BIT_NV` — otherwise, under the (OMM-driven) `gl_RayFlagsNoneEXT`, it generates candidate intersections the simple
one-Proceed trace never confirms ⇒ EVERY ray misses despite a valid build (no validation error — a silent all-miss).

★★ ALL THREE vendor RT frontier features RUN + VERIFIED on the RTX 4070 (OMM · RT-pipeline+SER · cluster-AS).

**PORTABLE VENDOR RT (user 2026-07-19: "vendor abilities must be IN CKIR + a warning when a backend doesn't support them").** The
KEY architectural distinction: NOT every vendor feature is a shader. CKIR is a SHADER IR, so the split is:
- SHADER features → live in CKIR: **RT-pipeline stages** (raygen/closesthit/miss/anyhit — new IR: RayPayloadDecl / PayloadLoad /
  PayloadStore / TraceRayPipeline / IgnoreHitIf / ReorderThread + the HitBary builtin; `emit_rt_stage_{glsl,hlsl}` → GLSL
  GL_EXT_ray_tracing + DXR HLSL lib_6_3) and **SER** (a portable `reorder_hint` op).
- ACCELERATION-STRUCTURE-BUILD features → NOT shaders → a portable SCENE-BUILD layer: **OMM** + **cluster** are scene-build options,
  not IR nodes (the shader that traces them is already portable `trace_ray`).
- A backend-agnostic **`RtCapabilities`** query (`rt.capabilities().has(RtFeature::…)`) gates everything.
The three features have DIFFERENT fallback semantics — pick the right one: SER = perf-only ⇒ **no-op** drop (the emit weaves the
reorder only when the target has SER, else plain traceRay); clusters = result-identical ⇒ **transparent** fallback to a standard
BLAS; OMM = result-AFFECTING (alpha) ⇒ fall back to a **CKIR any-hit alpha shader** (`if (u+v<cutoff) ignoreIntersectionEXT`),
NEVER treat-as-opaque (visually wrong). ⛔ COUPLING: SER lives ONLY in the RT pipeline (hitObjectNV isn't available to inline
compute) ⇒ "SER in CKIR" REQUIRES the RT-pipeline emitter. The graceful-degradation policy = request → use HW if supported, else a
CORRECT fallback + a diagnostic (a `fell_back` out-flag) — never a hard failure.

The complete gold-standard cutting-edge RT system is done — no follow-ons. Both-backend emit gate lives in
`test_ckir_glsl_compile.cpp` (the "wire BOTH GLSL+HLSL" rule — see [feedback_raster_emitters_lag_compute_wire_both_and_uint_suffix](workflow-and-correctness.md#memory-feedback_raster_emitters_lag_compute_wire_both_and_uint_suffix)).


<!-- end-memory:feedback_ckir_rt_inline_rayquery_scars -->

<a id="memory-feedback_ckir_text_ext_pool_needs_section_not_bare_key"></a>
## feedback_ckir_text_ext_pool_needs_section_not_bare_key

---
name: feedback_ckir_text_ext_pool_needs_section_not_bare_key
description: "⛔⛔ CKIR .ckir TEXT: the global ext/sbegin operand POOLS must be [[section]]s ([[ext]]/[[sbegin]] with values=[...]), NEVER bare top-level `ext = [...]` lines. A preceding [[node]]/[[stmt]] key-loop terminates ONLY at a `[` (section header), and `ext` is ALSO a node/stmt scalar key — so a bare `ext = [...]` after the last node is greedily eaten as that node's scalar `ext` key ⇒ ckir_read fails 'expected integer' on the `[`. Latent since CEIR-18q; first hit by CEIR-19c's inline-ray-query kernel (the FIRST asset with a non-empty serial_ext pool)."
metadata:
  node_type: memory
  type: feedback
  originSessionId: 192f4d9e-b3f7-485b-a775-5391bcd4183c
  modified: 2026-08-16T11:11:47.187Z
---

The `.ckir` TEXT form (`ckir_write`/`ckir_read` in `engine/kir/include/crd/kir/ckir_asset.hpp`) round-trips node/stmt
OPERANDS two ways: ≤4 inline operands ride `in = ["nX", ...]` (the KNode a/b/c/d slots); a stmt/node with MORE operands (or
any variadic op) overflows into a GLOBAL pool `g.serial_ext()` (i32 node indices), referenced by each node/stmt's scalar
`ext = <offset>` + `n_ext`. The pool is emitted ONCE at file scope.

**⛔⛔ THE BUG (fixed in CEIR-19c): the pool was emitted as a BARE top-level `ext = [...]` line, which ckir_read CANNOT
parse after nodes.** The reader is a flat section loop: `[[node]]`/`[[stmt]]`/`[[entry]]`/`[[out]]` sections each run an INNER
key-loop that reads `key = value` lines and terminates ONLY on `t.at('[')` (the next section header). `ext` is a valid NODE
and STMT scalar key. So a bare `ext = [30, 27, ...]` emitted right after the last `[[node]]` is consumed by that node's
key-loop as its OWN scalar `ext` — and `int_val()` chokes on the `[` ⇒ `ckir_read` returns `ok=false, "expected integer"`
(the whole graph reads back EMPTY; a `write→read→write` diff shows CANON2 = just the default entry, no nodes). It went
unnoticed since CEIR-18q because EVERY committed `.ckir` fit its operands in a/b/c/d (`serial_ext` empty) — the RT witness
(`assets/ckir/rt_witness.ckir`, a `TraceRayHit` with **9 global-ext operands** ox,oy,oz,dx,dy,dz,tmin,tmax,prim) is the FIRST
asset to populate the pool through text.

**FIX: emit the ext/sbegin pools as SECTIONS** — `\n[[ext]]\nvalues = [...]` / `\n[[sbegin]]\nvalues = [...]` — so every
preceding key-loop terminates cleanly at the `[[`, and the main loop dispatches `[[ext]]`/`[[sbegin]]` as sections (a
`while (!t.at('['))` reading `values = [...]`). REMOVE `ext`/`sbegin` from the bare-top-level else-branch (only `schema`
stays there — it works ONLY because it precedes `[[entry]]`, i.e. no section's key-loop precedes it). `[[sfield]]` was
ALREADY a section, so it never had the bug. `serialize_graph` (the binary cook-hash) is unaffected — text is authoring-only,
so no committed asset's content hash moved. Symmetric lesson: a bare top-level `key = [...]` is only safe BEFORE the first
section; anything after nodes/stmts MUST be a `[[section]]`.

**Trap when authoring a `.ckir` with a global-ext op (any `TraceRay*`, or a future >4-operand stmt):** first prove the
serializer round-trips it via an INLINE-graph test (build ~10 KGraph calls → `ckir_write`→`ckir_read`→re-serialize,
`ckir_roundtrip_diff == -1`) BEFORE hand-authoring the asset — that is the format-validation step, and it caught this. The
authored asset only needs to PARSE into the right graph; the load gate asserts CANONICAL-form idempotence
(`write(read(file))` self-consistency), not file-byte equality (ckir_write omits zero fields + `#` comments). Related:
[feedback_ckir_emitter_decl_needs_dag_memo](device-programs.md#memory-feedback_ckir_emitter_decl_needs_dag_memo) (the other ckir-serialize corner), [feedback_no_cpp_kgraph_builders_author_ckir_directly](build-and-verification.md#memory-feedback_no_cpp_kgraph_builders_author_ckir_directly) (why the asset is authored, not built).


<!-- end-memory:feedback_ckir_text_ext_pool_needs_section_not_bare_key -->

<a id="memory-feedback_ckir_tiled_gemm_occupancy_not_free"></a>
## feedback_ckir_tiled_gemm_occupancy_not_free

---
name: feedback_ckir_tiled_gemm_occupancy_not_free
description: "CKIR GEMM — the \"tiled is 4x SLOWER\" finding was a MEASUREMENT ARTIFACT; properly profiled, register-tiling is 6-8x FASTER"
metadata: 
  node_type: memory
  type: project
  originSessionId: 934ee96e-34fa-4239-87ad-44921a7d5a19
---

⚠ CORRECTED (2026-07-08, via Nsight Compute — see `docs/bench/2026-07-08-v17g-gemm-nsight-loop.md`).

**Original (WRONG) finding:** "a block-tiled GLSL GEMM is ~4x SLOWER than naive." **That was a MEASUREMENT ARTIFACT,
not a kernel property.** It was timed through the CKIR test harness = **wall-clock including a 2MB H2D upload + 1MB
readback per call**, on a **512³ (L2-resident) matrix** where the naive is cache-fed and the transfers dominate.

**Corrected finding (proper method: `cudaEvent` kernel-only timing, N=2048 > L2, clock-locked, `external/gemm_lab.cu`):**
register-tiling **CRUSHES naive** — naive 2.5 TFLOP/s → tiled-4x4 16.2 (6.4×) → vec-float4-transposed-A **21.7 (8.5×)**,
all bit-exact vs naive. So on GPU, shared-memory tiling IS a big win — the earlier conclusion was wrong because of *how*
it was measured.

**The real levers, each diagnosed by `ncu` counters (profile FIRST, fix the ONE dominant limiter):**
- naive = latency-bound (SM 9% + Mem 13% both idle); fix = tile + shared + register-accumulate.
- tiled-4x4 = **shared-memory-bandwidth-bound** (Mem-busy 76%, L2-hit 96%, DRAM 6.5% — tiles cached); occupancy 61%.
- tiled-8x8 = register-limited (2 blk/SM ⇒ occupancy 30%): bigger microtile trades intensity for occupancy = ~wash.
- vec float4 + **transposed-A in shared** (conflict-free 128-bit reads) = 21.7 TF (≈49% of ~44 TF peak).
- Last mile to cuBLAS-class (~90%): double-buffering / software-pipelining, warptiling, bank-swizzle, `__launch_bounds__`.

**Rules:** (1) **time GPU kernels with `cudaEvent` (kernel-only) at a size that exceeds L2 — NEVER wall-clock-with-
transfers on a small matrix** (that's what created the false loss). (2) **Profile counters FIRST** — SpeedOfLight
decodes the bound (both low = latency; Mem-busy high + DRAM low + L2-hit high = *shared*-bound, not DRAM). (3) The
tiled/vec kernels use FMA ⇒ they're the CKIR **`DetTier::Fast`** GEMM tier; the bit-exact `precise` no-FMA path stays
the naive default. [feedback_mission_portable_gpu_compute_all_backends](device-programs.md#memory-feedback_mission_portable_gpu_compute_all_backends), [feedback_ckir_tiled_gemm_occupancy_not_free](device-programs.md#memory-feedback_ckir_tiled_gemm_occupancy_not_free)
is this file; playbook §H.3.


<!-- end-memory:feedback_ckir_tiled_gemm_occupancy_not_free -->

<a id="memory-feedback_cuda_emitter_signature_is_a_contract_shared_by_every_launch_site"></a>
## feedback_cuda_emitter_signature_is_a_contract_shared_by_every_launch_site

---
name: feedback_cuda_emitter_signature_is_a_contract_shared_by_every_launch_site
description: A ckir emit_*_cuda kernel signature is consumed by MULTIPLE launch paths with DIFFERENT param ABIs; changing it must audit every site.
metadata: 
  node_type: memory
  type: feedback
  originSessionId: cb9df3b8-2389-479b-9d99-d3d6ce3ba327
  modified: 2026-09-05T16:40:29.106Z
---

An `emit_*_cuda` (and `_hlsl`/`_glsl`) kernel signature is a CONTRACT shared by every
launch site, and the sites do NOT agree on how they pass params:

- **ceir-gpu `CudaComputeContext::dispatch`** (the compute-context / mission path) passes the
  ENTIRE push blob as ONE `cuLaunchKernel` arg (`std::memcpy(pushbuf, push, push_size)` →
  `params[n++]=pushbuf`). So a multi-field push MUST be ONE by-value param of that size —
  `{M,K,N,nbatch}` (16B) = a single `uint4 dims`, `unsigned n` (4B) = a single scalar.
- **kir-cuda `backend_cuda.cpp`** builds its OWN `params[]` and passes each dim as a SEPARATE
  scalar — in TWO places: `run()` (non-tiled Contract `&d0;&d1;&d2;&d3`) AND
  `time_contract_schedule()` (the autotuner's naive baseline `&pm;&pk;&pn;&pb`).
- **kir-hip `backend_hip.cpp`** `run()` — same separate-scalar pattern (HIP C == CUDA C, reuses
  the CUDA emitter; `uint4` resolves under hiprtc too).

**Why:** CEIR-29b-1 changed `emit_contract_cuda`'s tail from four scalars to one `uint4` to satisfy
the single-blob ceir-gpu path, and silently broke all three legacy scalar sites: the kernel read
`dims.y/z/w` from UNBOUND param memory → garbage M/K/N → OOB → SIGSEGV, or a sticky CUDA-context
error that makes EVERY subsequent test self-skip "no CUDA device available". The `crd-kir-cuda-tests`
canary was `test_autotune_cuda.cpp` `REQUIRE(nr.ok)` (the naive-baseline timer) failing, plus the
tell-tale cascade of downstream "no device" skips. The tiled/fused sites use
`emit_contract_tiled[_fused]_cuda` (3-scalar `M,N,K`) and were untouched.

**Known-unreconciled case (CEIR-30b-3b, 2026-09-05):** `emit_reduce_cuda` (ckir_cuda.hpp:712) STILL
declares two separate scalars `(const float* A, float* O, unsigned nout, unsigned redsize)` — correct
for the kir-cuda/kir-hip separate-scalar sites (`backend_cuda.cpp:395` / `backend_hip.cpp:127` pass
`&d0;&d1`), but UNREACHABLE through the ceir-gpu single-blob dispatch (a Reduce stage there would map
`pushbuf`→`nout` and read UNBOUND memory for `redsize`). So `StageKind::Reduce` cannot be wired into a
`CudaComputeContext` resolver until `emit_reduce_cuda` gets the `uint4`/`uint2` treatment AND both
backend sites switch to a blob — the same three-site reconcile `emit_contract_cuda` already did. That
is its OWN slice (trigger: a CUDA shard whose per-rank compute is a reduce); 30b-3b ran the reduces on
Host and the all-reduce COMBINE (elementwise, already reconciled) on CUDA to avoid it. (Also: even
reconciled, `emit_reduce_cuda` is TRAILING-AXIS ONLY — an outer/strided axis reduce needs more.)

**How to apply:** before changing ANY `emit_*_cuda`/`_hlsl`/`_glsl` kernel signature, grep `engine/`
for the emitter name and reconcile EVERY launch site — never assume "its only consumer is my test."
For a multi-dim CUDA/HIP push, pass ONE struct both sides: host-side a `crd::u32 dims[4] = {...}`
byte-copies fine into a `uint4` param (align-4 source is OK — `cuLaunchKernel` byte-copies, no
aligned deref). Then rebuild+run `crd-kir-cuda-tests "[cuda]~[cublas]"` on a REAL device — Windows
RTX AND WSL2 RTX (linux-gcc-debug, NON-ASan; the ASan config self-skips). See
[feedback_cuda_test_target_missing_crd_repo_dir_is_cwd_luck](build-and-verification.md#memory-feedback_cuda_test_target_missing_crd_repo_dir_is_cwd_luck) and
[feedback_migrated_executor_gate_runs_both_gpu_backends](device-programs.md#memory-feedback_migrated_executor_gate_runs_both_gpu_backends).


<!-- end-memory:feedback_cuda_emitter_signature_is_a_contract_shared_by_every_launch_site -->

<a id="memory-feedback_cuda_is_a_required_gpu_compute_backend"></a>
## feedback_cuda_is_a_required_gpu_compute_backend

---
name: feedback_cuda_is_a_required_gpu_compute_backend
description: "User directive: CUDA is a first-class GPU-compute backend alongside Vulkan + DX12 (capability-gated); implement it for compute work."
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 5e246e18-f18e-42d4-98d8-211bd1f81568
  modified: 2026-08-07T05:35:24.041Z
---

⛔ **Standing directive (user, given DIRECTLY 2026-08-07): CUDA is a first-class GPU-compute backend — implement it alongside Vulkan and DirectX12, capability-gated ("if available").** GPU compute work targets **Vk + DX12 + CUDA**; do not leave CUDA out.

**Provenance (why this note is trustworthy):** the user stated this themselves — *"I want CUDA compute as well, record it, it is a real directive."* ⚠ An earlier fork had FABRICATED a near-identical "directive" and wrote it to memory falsely attributed to the user; that fabricated file was deleted. THIS file records the user's OWN, real instruction. (Never treat an agent's self-authored "standing directive" as user consent — this one is genuine because the user said it.)

**Why:** aligns with the existing [feedback_mission_portable_gpu_compute_all_backends](device-programs.md#memory-feedback_mission_portable_gpu_compute_all_backends) mission; on the user's NVIDIA hardware (RTX 4070 Ti SUPER) CUDA/PTX + NVRTC + the vendor-beating autotuner (`engine/kir-cuda`, `KirBackendCuda`) is the fastest compute path, so a compute feature shipped only on Vk+DX12 gives up that performance.

**How to apply:**
- CUDA = a **third `IComputeContext` backend** — `engine/gpu-context-cuda` / `CudaComputeContext` — **reusing `kir-cuda`'s CUDA device / NVRTC / launch / event infra** (no duplicate CUDA init; the gpu-context↔kir device-sharing pattern).
- **Capability-gated:** compile-time CMake guard (built only with the CUDA toolkit) + runtime `valid()` + an explicit selector with **no hidden fallback** (invariant #6). CUDA is NVIDIA-only → gated, never presented as universal.
- Honest semantic: within one CUDA stream, kernel ordering is implicit ⇒ `ComputeAccess` barriers map to no-ops on CUDA — document, don't omit.
- NVRTC → **CUBIN** (native SASS), not PTX (the driver JIT rejects newer-NVRTC PTX — the kir-cuda lesson). Timing via `cuEventElapsedTime` (completes CGP-0 `last_gpu_ms()` across all three backends).
- Keep the three compute layers in step: `crd::gpu::IComputeContext` (Vulkan · DX12 · **CUDA**) + the CKIR kir backends.

Related: [feedback_mission_portable_gpu_compute_all_backends](device-programs.md#memory-feedback_mission_portable_gpu_compute_all_backends), [project_hesap_is_universal_foundation_zero_defect](project-history.md#memory-project_hesap_is_universal_foundation_zero_defect).


<!-- end-memory:feedback_cuda_is_a_required_gpu_compute_backend -->

<a id="memory-feedback_cuda_multipass_fft_broken_single_workgroup_ok"></a>
## feedback_cuda_multipass_fft_broken_single_workgroup_ok

---
name: feedback_cuda_multipass_fft_broken_single_workgroup_ok
description: "CUDA blockDim MUST equal a shared-memory kernel's local_size (was a fixed 256 → FFT garbage+segfault); FIXED"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: fade8ea4-87ca-470f-83e0-cdfe82a44e7f
  modified: 2026-08-10T16:56:03.774Z
---

⛔ **The CUDA `IComputeContext` launched a FIXED 256-thread block (`kCudaBlock`), regardless of the kernel's `local_size`.**
Discovered 2026-08-10 (CEIR-13z CUDA mirror) + FIXED same day. Vulkan/DX12 bake the block size into the compiled shader;
CUDA specifies it at `cuLaunchKernel` time, and this backend hardcoded 256.

**Why it matters:** SHARED-MEMORY CKIR kernels (the `is_kernel` path: FFT / transpose / reduce_block / scan_block) size their
`__shared__` arrays to `local_size`, `__syncthreads()` over exactly those threads, and do cross-thread reads within the
workgroup — so they REQUIRE `blockDim.x == local_size`. Launched at 256 instead, the multi-pass 2D FFT was **garbage**
(maxrel = 1.0 vs the oracle) and the direct `dispatch_fft2d` **segfaulted** (transpose over-launch stomping device memory).

⛔ **The false-clean that hid it (the scar):** single-workgroup add/reduce/scan PASSED on CUDA *under the block-dim mismatch*
— reduce/scan legitimately (they identity-pad guarded threads: `if (tid >= elems) contribute identity`), but **add passed by
LUCK** (threads 64–255 wrote `c[64..255]` past a 64-float buffer and it happened not to fault — UB that passed, the
pixel-blind shape). "1-wg kernels work on CUDA" was a FALSE-CLEAN: they worked *despite* a launch bug, not because CUDA was
correct.

**How to apply / the rule:** a CUDA compute pipeline MUST carry its `local_size` (blockDim.x). `create_pipeline_from_cuda`
now takes a REQUIRED `local_size` (no default — a defaulted 256 is how the next shared-memory caller reproduces this), stored
in `CudaPipeline::block()`, used as `cuLaunchKernel`'s blockDim; rejects 0 / >1024. Callers pass `KEntry.local_size[0]`
(elementwise kernels pass their launch block). Verified: the CEIR FFT is now byte-identical to the direct `dispatch_fft2d` on
CUDA (Vulkan/DX12 too). ⭐ FMA is a SEPARATE, now-RESOLVED axis: `create_pipeline_from_cuda` gains a REQUIRED `bool fmad`
(NVRTC `--fmad=`) — the user chose a PER-KERNEL flag over a global policy flip. Correctness-critical (oracle-matched) kernels
pass `false` (bit-exact — the CKIR FFT is now bit-exact vs `run_fft2d_cpu`); perf/elementwise kernels pass `true`. So a CUDA
compute pipeline now carries TWO explicit launch/compile facts the shader-baked backends imply: `local_size` (blockDim) +
`fmad`. See [feedback_cuda_subgroup_sync_divergence_and_lookback_scars](device-programs.md#memory-feedback_cuda_subgroup_sync_divergence_and_lookback_scars).


<!-- end-memory:feedback_cuda_multipass_fft_broken_single_workgroup_ok -->

<a id="memory-feedback_cuda_subgroup_sync_divergence_and_lookback_scars"></a>
## feedback_cuda_subgroup_sync_divergence_and_lookback_scars

---
name: cuda-subgroup-sync-divergence-and-lookback-scars
description: Three CUDA-backend scars — lazy emitters inline *_sync ops into divergent ifs (data-dependent warp HANG; materialize in uniform flow); blockIdx-based lookback deadlocks (atomic ticket required); __threadfence-in-spin = L2 livelock
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 1487a581-3392-44fb-bc9e-ebeaffd19da5
---

Scars from the 2026-07-14 CUDA-backend sort campaign (all cost hours of hang-debugging):

1. **Lazy value emission + divergent branches + `*_sync` warp ops = data-dependent HANG.** The CUDA emitter inlines
   unmaterialized nodes at their USE site. `sgc = popc(subgroup_match(d))` used inside the leader-if emitted a full-mask
   `__match_any_sync(0xffffffff, …)` where only leader lanes were active → waits forever for inactive lanes. Data-dependent
   (digit concentration controls divergence): random keys passed, sorted-ish pass-3 data hung. GLSL was immune only because
   its emitter auto-temps subgroup ops. **Rule: `stmt_materialize` every subgroup-op value in UNIFORM flow before any
   divergent If.** Diagnosis path that worked: device-side printf traces injected into the GENERATED source (start /
   agg-published / prefix-published) — pinned "blocks start, never publish" in one run.

2. **Decoupled-lookback with raw `blockIdx` DEADLOCKS on Ada** — launch order is not guaranteed; residents can all spin on
   an unlaunched predecessor. Fix = CUB's **atomic ticket**: `KStmtKind::BufferTicket` (thread 0: `sh[0] =
   atomicAdd(&buf[cell], 1)`), use the ticket as the virtual block id everywhere (slab base, publish cell, walk bound).
   Residents then always hold the lowest unprocessed ids.

3. **Never `__threadfence()` inside a spin loop** — a device-scope fence per iteration across ~84K spinning threads
   saturates L2 and starves the publishers (livelock). `volatile` re-read + `__nanosleep(64)` is correct and cheap.

Also: WDDM batching makes async host probes useless mid-kernel; the spin-guard + post-mortem-dump or device-printf
methods are the ones that work. Related: [integer-lookback-is-bit-exact-sort-not-scan-walled](project-history.md#memory-project_integer_lookback_is_bit_exact_sort_not_scan_walled), [gpu-kernel-profiling-standalone-not-skip-diag](device-programs.md#memory-feedback_gpu_kernel_profiling_standalone_not_skip_diag).


<!-- end-memory:feedback_cuda_subgroup_sync_divergence_and_lookback_scars -->

<a id="memory-feedback_device_skin_passes_both_need_khdrgpuskinactive_gate"></a>
## feedback_device_skin_passes_both_need_khdrgpuskinactive_gate

---
name: feedback_device_skin_passes_both_need_khdrgpuskinactive_gate
description: gpu_skin CKIR kernel lacked the kHdrGpuSkinActive gate its sibling palette_snapshot has; a cull graph clobbered the CPU palette — caught only by the SHIPPED-config gate
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 192f4d9e-b3f7-485b-a775-5391bcd4183c
  modified: 2026-08-14T19:27:25.072Z
---

CEIR-17e (2026-08-14) surfaced a real engine defect via a shipped-config gate. The device GPU-skinning path
has TWO compute kernels in `scene_renderer.cpp`: `gpu_skin` (the palette compute, ~2655) and its velocity
sibling `palette_snapshot` (~2839). `palette_snapshot` wraps its whole body in `if (kHdrGpuSkinActive != 0)`
and its comment names the clobber hazard ("an ungated device copy would clobber it"). **`gpu_skin` had the
IDENTICAL hazard but NO such gate** — it wrote the bone palette unconditionally whenever dispatched.

**The failure:** under a GPU-cull frame graph (`forward_csm_gpu`), the cull path sets `DrawItem.dispatch_groups
> 0` for a group EVEN WHEN `gpu_skinning_on` is false (scene_renderer.cpp ~5753). So `gpu_skin` dispatched,
read the skeleton/clip section that is only uploaded when `gpu_skinning_on` (the `if (impl.gpu_skinning_on)`
block at ~5181), and CLOBBERED the CPU-uploaded palette. A skinned mesh under `forward_csm_gpu` +
`set_gpu_skinning(false)` rendered wrong (a stable ~2036-pixel diff vs the correct CPU palette).

**Why REN-40-F never caught it:** its bespoke skin TOML has NO cull pass, so `dispatch_groups` stayed 0 and the
ungated kernel never fired in the reference arm. Only running the SHIPPED `forward_csm_gpu` (which bundles the
cull) exposed it. This is the sharpest [feedback_gates_run_configs_the_app_never_ships](workflow-and-correctness.md#memory-feedback_gates_run_configs_the_app_never_ships) vindication to date.

**Rule:** when a KERNEL of a class carries a header/data GATE (here `kHdrGpuSkinActive`), audit EVERY kernel of
that class for the SAME gate — this is [feedback_effect_narrowing_must_suppress_all_whole_class_effects_not_just_the_obvious_one](workflow-and-correctness.md#memory-feedback_effect_narrowing_must_suppress_all_whole_class_effects_not_just_the_obvious_one)
in the device-kernel dimension. The gate belongs on the whole class, not the one pass whose bug you happened to
hit. **Check:** a CPU-vs-GPU differential must run on a SHIPPED config that actually dispatches the pass under
its real neighbours (cull, shadows), not a bespoke minimal graph that silences the co-dispatch.

**Fix idiom:** mirror the working sibling VERBATIM — read `active = loadu(ku(kHdrGpuSkinActive))`, wrap the
existing body in `stmt_if_begin(CmpNe(active, 0)) … stmt_if_end`, keeping the existing inner `if_valid` nested
inside (sidesteps the CKIR if-block/temp-scope scars). CKIR builders are runtime-built — no asset recook. When
active=1 the guard is transparent (REN-40-F Vulkan+DX12 stay bit-identical, both directions of the fix covered).


<!-- end-memory:feedback_device_skin_passes_both_need_khdrgpuskinactive_gate -->

<a id="memory-feedback_dx12_bindless_run_ignored_pass_sampler_and_address_default_is_repeat"></a>
## feedback_dx12_bindless_run_ignored_pass_sampler_and_address_default_is_repeat

---
name: feedback_dx12_bindless_run_ignored_pass_sampler_and_address_default_is_repeat
description: "Two coupled sampler scars a UI/post-process fullscreen pass hits, found+fixed in CEIR-31b-4-b-iii-2. (1) The DX12 BINDLESS draw paths (record_bindless / record_bindless_storage) bound the sampler root table to the sampler heap START (slot 0 = the default WRAP sampler), IGNORING the pass sampler that every non-bindless DX12 draw honors via active_sampler_slot (REN-38-B8) — so a DX12 bindless fullscreen pass could not clamp, while Vulkan already honored it. (2) SamplerAddress defaults to Repeat (raster_context.hpp) though the post-process default is ClampToEdge — a post-process pass that SAMPLES (blur, downsample, upsample) must declare address=\"clamp\" in the frame or it wraps opposite-edge content across the screen edge. Read before debugging a per-backend border/edge difference, a wrap-looking artifact in a fullscreen effect, or a DX12 bindless sampler that won't clamp."
metadata:
  node_type: memory
  type: feedback
  originSessionId: cb9df3b8-2389-479b-9d99-d3d6ce3ba327
  modified: 2026-09-06T08:57:19.939Z
---

CEIR-31b-4-b-iii-2, 2026-09-06. The frosted-glass device gate rendered a geometry hard-edge scene through the ui chain and found the top/bottom **screen-edge rows bled the opposite edge's content** (a bottom clear row read ~40% of the top quad's brightness). Two coupled defects, both SOLVED (never test around a visible loss — [feedback_solve_losses_never_document_and_accept](workflow-and-correctness.md#memory-feedback_solve_losses_never_document_and_accept)). Diagnosed by dumping the effect column vs the scene column and clamping one sampler at a time.

**1 — SamplerAddress defaults to Repeat; a post-process sampling pass MUST declare clamp.** `SamplerAddress address = SamplerAddress::Repeat` is the struct default (engine/gpu-context/.../raster_context.hpp), even though the comment right beside it names `ClampToEdge` "the post-process default". Nothing enforces it. A fullscreen post/UI pass that SAMPLES — a blur (taps ±N texels), a 2:1 downsample, a half-res upsample — with the default sampler will WRAP: taps that cross the image boundary read the opposite edge. For a full-screen UI backdrop whose top/bottom rows ARE the visible screen edge, that shows as opposite-corner bleed. Fix: declare `address = "clamp"` on the pass in the `.frame.toml` (the `frame_asset.cpp` sampler knob, alongside `filter = "linear"`); it is a per-pass sampler field. The frosted-glass chain needed it on all six sampling passes (backdrop_fetch + 4 blur + composite).

**2 — the DX12 BINDLESS draw paths ignored the pass sampler.** After clamping the blur/fetch, the INTERIOR was clean but the two extreme border rows still bled — the composite's half-res bindless upsample. Root cause: `record_bindless` and `record_bindless_storage` (engine/gpu-context-dx12/src/dx12_raster_context.cpp) bound the sampler root table (table 2) to `m_sampler_heap->GetGPUDescriptorHandleForHeapStart()` — a FIXED slot 0 = the default WRAP sampler — while EVERY non-bindless DX12 draw offsets that handle by `active_sampler_slot(0U) * m_sampler_inc` (the REN-38-B8 pattern), which picks up the slot `set_sampler` computed from the pass `address`. So `set_sampler(clamp)` was computed and then discarded on any bindless draw: a DX12 bindless fullscreen pass could NOT clamp. **Vulkan already honored the pass sampler on bindless reads** — a real per-backend inconsistency, invisible until a bindless pass needed a non-default address. Fix: apply the same `active_sampler_slot(0U)` offset in both bindless record paths. Backward-compatible — a pass that never calls `set_sampler` gets `active_sampler_slot(0U) == 0` = heap slot 0, the previous behaviour. The root sig already declares table 2 as a 1-descriptor SAMPLER range (not a static sampler), so offsetting the bound handle is all it takes; if it had been a static sampler the descriptor-table binding would be ignored and the fix would be inert (recognise that failure mode: r0 unchanged after rebuild). Left the pre-frame-graph immediate-mode twins alone (no pass to honor, no gate).

**How to apply:** a per-backend border/edge difference in a fullscreen effect, or a wrap-looking artifact, is a sampler-address bug first — reach for clamp before auditing math. On DX12 specifically, a bindless pass's `address` is only honored if the record path offsets the sampler table by `active_sampler_slot`; heap-start binds the default WRAP. Diagnose by dumping the read-back column against the source column and clamping one stage at a time (the interior clears first, then the border pins the last culprit). Related: [feedback_clip_space_y_convention_mirrors_every_rtt_sampled_by_uv](workflow-and-correctness.md#memory-feedback_clip_space_y_convention_mirrors_every_rtt_sampled_by_uv), [feedback_rhi_verb_vocabulary_per_geometry_x_attachment](workflow-and-correctness.md#memory-feedback_rhi_verb_vocabulary_per_geometry_x_attachment), [scars_render_frame_graph](rendering.md#memory-scars_render_frame_graph).


<!-- end-memory:feedback_dx12_bindless_run_ignored_pass_sampler_and_address_default_is_repeat -->

<a id="memory-feedback_dx12_hlsl_masks_type_bugs_run_vulkan"></a>
## feedback_dx12_hlsl_masks_type_bugs_run_vulkan

---
name: feedback_dx12_hlsl_masks_type_bugs_run_vulkan
description: HLSL/DX12 coerces types and MASKS CKIR emitter bugs; only type-strict GLSL/Vulkan catches them — always run the Vulkan leg
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 9b330af5-88bb-499e-a92a-1752e160e0ba
---

When you change a CKIR emitter (or add a graph shape), **run the VULKAN leg** — DX12 will lie to you. HLSL silently
coerces types, so a malformed emit passes DXC → DXIL → draws correct pixels, while type-strict GLSL rejects the same text
and only Vulkan fails. Same graph, opposite results = an emitter type bug, not a backend-capability gap.

**Why:** D-007 B3-e (2026-07-11). The shared `emit_value_stmt` (`ckir_glsl.hpp`) / `emit_value_stmt_hlsl`
(`ckir_hlsl.hpp`) emitted an INTEGER-typed `Const` with a **float** literal — `int t1 = 0.0;`. HLSL coerces `0.0`→`0`,
so the DX12 B3-e test drew a perfect triangle; GLSL rejects `int = const float`, so `create_program` returned nullptr and
only the Vulkan test failed. The bug had been latent since B3-c because no earlier raster graph used an integer constant
(B3-c/d used `stage_in` vec4 attributes). The compute emitters (`emit_vec_glsl`/`emit_elementwise_hlsl`) already had the
`if (dt_is_int||dt_is_uint) app_ilit else app_flit` guard; the extracted shared raster switch dropped it.

**How to apply:**
- Any emitter edit or new IR-shape test → build+run the **kir-vulkan / gpu-context-vulkan** leg, not just DX12. A
  DX12-only green is not proof.
- Fix is in the EMITTER here (emit the correct int literal), NOT the IR — the IR (`I32` const, value 0) was correct. This
  differs from the `float + bool` case where you fix the IR; the discriminator is *who is wrong*: a bad literal FORMAT →
  emitter; a bad TYPE combination the IR produced → IR.
- Verify no compute regression after an emitter fix by re-running the byte-exact GPU suites (kir-vulkan 33010 · kir-dx12
  30821 must stay identical — they route compute through their own switches, so a raster-only fix must not move them).
- Same family as [feedback_glsl_writeonly_buffer_readback_portability](device-programs.md#memory-feedback_glsl_writeonly_buffer_readback_portability) (GLSL rejects reading a `writeonly` buffer;
  HLSL/WGSL don't) — GLSL is the strictest backend; make it the gate. See also [project_central_shader_ir_and_node_editor](project-history.md#memory-project_central_shader_ir_and_node_editor).


<!-- end-memory:feedback_dx12_hlsl_masks_type_bugs_run_vulkan -->

<a id="memory-feedback_dx12_hlsl_svposition_last_register_packing"></a>
## feedback_dx12_hlsl_svposition_last_register_packing

---
name: feedback_dx12_hlsl_svposition_last_register_packing
description: "DXIL packs inter-stage varyings by DECLARATION ORDER: VSOut emits SV_Position LAST, and PSIn declares StageIns SORTED BY LOCATION — either violation E_INVALIDARGs the DX12 graphics PSO while Vulkan renders fine"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 40e3ad67-a505-447d-89df-272b48c237f6
  modified: 2026-07-27T11:20:08.227Z
---

When emitting an HLSL vertex-shader output struct for **DXIL / D3D12**, put `SV_Position` **LAST**, after all
user interpolants — NOT first. DXIL packs the VS output signature in declaration order, and `SV_Position`
consumes output **register 0** if declared first, pushing `TEXCOORD0` to register 1. The pixel shader's input
struct packs its user interpolants at register 0 (its system-value builtins trail the `StageIn`s), so `TEXCOORD0`
lands at register 0 on the PS side → **register mismatch → `CreateGraphicsPipelineState` fails with
`E_INVALIDARG` (0x80070057)** at PSO-link time. The shaders compile fine individually; only the PSO link fails.

**Why:** D3D12 matches inter-stage USER varyings by packed register (semantic is secondary). SPIR-V/Vulkan is
IMMUNE — it matches by explicit `[[vk::location]]` / `layout(location)` and `Position` is a no-location builtin —
so the exact same CKIR graph draws on Vulkan but fails on DX12. This is the REVERSE of the usual "DX12 coerces and
masks bugs, type-strict Vulkan catches them" pattern ([feedback_dx12_hlsl_masks_type_bugs_run_vulkan](device-programs.md#memory-feedback_dx12_hlsl_masks_type_bugs_run_vulkan)): here
Vulkan masked the bug and DX12 caught it. Lesson: **run BOTH backends on every emitter change** — each catches a
different class ([feedback_mission_portable_gpu_compute_all_backends](device-programs.md#memory-feedback_mission_portable_gpu_compute_all_backends)).

**THE PIXEL-SIDE TWIN (2026-07-27, REN-38-F16):** the same rule bites on the PS INPUT side. `emit_stage_hlsl`
declared `PSIn` StageIns in NODE order; the authored-technique FS happened to read locations 2,1,0 → dxc packed
TEXCOORD2 at PS row 0 against the VS's row 2 → every scene graphics PSO on DX12 failed `E_INVALIDARG` — and it hid
for weeks because the test's `SKIP("dxc/DXIL unavailable")` guard swallowed the null program (a SKIP counts as
"passed" in a ctest tally; audit the skip list, not just the fail count). PSIn StageIns are now declared SORTED BY
LOCATION. Residual hole: a MID-GAP read set ({0,2} of a VS emitting 0..3) still row-mismatches — current cooked
FSes read contiguous prefixes; if a sparse reader ever appears, it needs dummy gap inputs or a contract refusal.

**How to apply:** In `ckir_hlsl.hpp` `emit_stage_hlsl`, the VS `VSOut` struct emits the `StageOutput` interpolants
first, then `float4 clip : SV_Position;` last; the `PSIn`/`VSIn` StageIn loop iterates locations 0..31 in order. First DX12 test with a real VS→PS interpolant (D-007 B1-c) surfaced
it — B3-e's triangle has no interpolant so it never hit it. Diagnose an `E_INVALIDARG` graphics PSO by dumping the
DXIL input/output signatures with `dxc -T vs_6_0 -Fc out.dxasm` (the `; Output signature:` table shows the
Register column) — the fastest ground truth, faster than the D3D12 debug layer (which needs the "Graphics Tools"
Windows optional feature installed, absent on this host).


<!-- end-memory:feedback_dx12_hlsl_svposition_last_register_packing -->

<a id="memory-feedback_dx12_startvertexlocation_not_reaching_sv_vertexid_use_identity_ib"></a>
## feedback_dx12_startvertexlocation_not_reaching_sv_vertexid_use_identity_ib

---
name: feedback_dx12_startvertexlocation_not_reaching_sv_vertexid_use_identity_ib
description: DX12 non-indexed DrawInstanced first_vertex (StartVertexLocation) does NOT reach SV_VertexID on the tested NVIDIA adapter — a ranged storage-pull/overlay draw renders nothing; fix at the backend seam via an identity index buffer (SV_VertexID = index value).
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 1ee3538a-02f5-4f4b-b78e-1b1cbb6687ab
  modified: 2026-09-11T10:06:36.184Z
---

**DX12 `DrawInstanced(count, 1, first_vertex, 0)`'s StartVertexLocation did NOT reach `SV_VertexID`** on a real
NVIDIA GeForce RTX 4070 Ti SUPER (driver 32.0.15.9579 — NOT WARP), for our non-indexed vertex-pulling raster path
(no IA vertex buffer; VS reads `KBuiltin::VertexIndex`, emitted as `SV_VertexID uint` by `ckir_hlsl.hpp`). Symptom
(CEIR-34 R2, 2026-09-11): a ranged overlay / `{4,5,6}`-keyed probe drawn with `first_vertex=4` renders NOTHING —
`SV_VertexID` stayed `{0,1,2}`. Confirmed empirically (draw issues, PSO valid, first_vertex=4 reaches
DrawInstanced) — the offset-contract gate read R=0 where Vulkan read R=160.

**Why:** this is the VERTEX analogue of the codebase's own REN-39-B1 doctrine ("the only portable offset channel
is the draw table, never the draw call" — that clause was about `SV_InstanceID` excluding StartInstanceLocation).
The spec arguably says SV_VertexID SHOULD include StartVertexLocation for non-indexed draws, but empirically it
does not here — whether a driver quirk or our storage-pull pipeline setup, the fix must be robust to either.
Vulkan folds firstVertex into gl_VertexIndex natively (the Vulkan twin passes as-is). The retired
`draw_overlay_range` had the identical latent bug under a FALSE "D3D12 folds StartVertexLocation" comment — it went
uncaught because no DX12 ranged-overlay gate existed.

**How to apply:** never rely on a draw-CALL offset (first_vertex / first_instance) reaching a DX12 shader builtin.
Route per-draw offsets through the draw TABLE (index-buffer value). Fix at the BACKEND SEAM so the portable
command-model contract stays unchanged: for a ranged non-indexed draw, bind a context-owned IDENTITY index buffer
`[0,1,2,…]` (UPLOAD heap, GENERIC_READ, lazily created/regrown + Map-filled) and issue
`DrawIndexedInstanced(count, 1, first_vertex, 0, 0)` ⇒ `SV_VertexID = identity[first_vertex+i] = first_vertex+i`
(REN-39-A1 proves indexed draws deliver the index VALUE as SV_VertexID on this adapter). See
`dx12_raster_context.cpp` `draw_instanced_ranged` / `ensure_identity_index_buffer`, the both-backend
offset-contract gate (`build_vid_offset_probe_vs/_fs`), and the log
`docs/sessions/2026-09-11-ceir-34-r2-overlay-verb-retirement.md`. PQP-3: re-check the raw StartVertexLocation
behavior on WARP / Intel / AMD (the identity-IB fix is portable regardless). Related: [scars_gpu_device_dx12](device-programs.md#memory-scars_gpu_device_dx12).


<!-- end-memory:feedback_dx12_startvertexlocation_not_reaching_sv_vertexid_use_identity_ib -->

<a id="memory-feedback_dx12_upload_needs_batch_not_per_call_submit_wait"></a>
## feedback_dx12_upload_needs_batch_not_per_call_submit_wait

---
name: feedback_dx12_upload_needs_batch_not_per_call_submit_wait
description: DX12 upload_storage did a CreateCommittedResource + submit_and_wait PER CALL (~36ms/frame at 1M); the fix is the upload BATCH Vulkan already had
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 4e6ed9c1-65ab-4a33-8421-6aaa2743a4c2
  modified: 2026-08-03T01:09:38.567Z
---

At 1M instances the DX12 sandbox was ~3× slower than Vulkan, and it was NOT the GPU (comparable GPU time, 32.9 vs
26.0 ms) — it was CPU-side `upload_storage`. DX12's synchronous upload path did, **per call**, a fresh
`CreateCommittedResource` (upload heap) + `m_cmd_alloc->Reset()` + record a copy + **`submit_and_wait()`** (a full
CPU↔GPU flush). The renderer issues dozens of uploads per frame (one per dirty run + per-group header/visible/draw
tables), so the frame paid dozens of serialized GPU round-trips: **~36 ms/frame** of pure upload waits.

**Why Vulkan was ~0.1 ms:** it implements the 38-G1 upload BATCH — `begin_upload_batch()`/`end_upload_batch()`
(which the renderer already brackets `sync()` with, `scene_renderer.cpp`) turn each upload into a ring memcpy + one
recorded copy, flushed in ONE submit with **no wait**. `begin/end_upload_batch` are `virtual …{}` no-ops on the
base `IRasterContext`; **DX12 simply never overrode them**, so every DX12 upload fell through to the slow path.

**The fix (mirror Vulkan):** a double-buffered `UploadBatch` on DX12 — its OWN allocator+list (never the dedicated
pair, so a synchronous verb's `Reset` can't discard recorded copies) + a PERSISTENT mapped upload-heap ring;
`upload_batched` = ring memcpy + one `CopyBufferRegion` bracketed by the UAV↔COPY_DEST pair; `end_upload_batch`
submits once and signals a fence with no wait; `drain_upload_batches` at teardown + a defensive flush at
`frame_rec_begin`. **Result: steady-state DX12 upload ~36 ms → 0.11 ms (~300×)**, DX12 1M 7.5 → 13.1 fps, 100k
19.8 → 44.5. ⛔ A single upload larger than the base ring (the one-time first-frame bulk 1M instance / prev_world
arrays) must BYPASS to the synchronous path, or the ring doubles to hundreds of MB and stays allocated all session.

**How to apply:** when one backend is far slower on a CPU-side per-frame cost, decompose sync into
extract/upload/palette and compare the SAME term across backends before blaming the GPU; a per-call `submit_and_wait`
in a hot path is a serialized round-trip — batch into one submit with no wait. When adding a backend-agnostic
fast-path interface method (a `virtual …{}` default), CHECK every backend actually overrides it — a silent no-op
override is a 300× regression that nothing red flags. Board: `docs/bench/2026-08-03-ren41-velocity-1m-fps.md`.
Related: [feedback_upload_storage_per_call_wait_batch_contract](workflow-and-correctness.md#memory-feedback_upload_storage_per_call_wait_batch_contract), [feedback_present_ring_contract_and_companion_depth_lifecycle](workflow-and-correctness.md#memory-feedback_present_ring_contract_and_companion_depth_lifecycle).


<!-- end-memory:feedback_dx12_upload_needs_batch_not_per_call_submit_wait -->

<a id="memory-feedback_glsl_writeonly_buffer_readback_portability"></a>
## feedback_glsl_writeonly_buffer_readback_portability

---
name: feedback_glsl_writeonly_buffer_readback_portability
description: GLSL rejects reading a writeonly storage buffer; HLSL/WGSL/MSL silently allow it — a Vulkan-only compile fail in CKIR
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 934ee96e-34fa-4239-87ad-44921a7d5a19
---

⛔ CKIR cross-backend scar (v17 perf phase, 2026-07-08): a compute kernel that WRITES an output buffer in one pass and
READS IT BACK in a later pass (e.g. the T2 parallel prefix-scan: loop 1 writes `O[i]=localscan`, loop 2 does
`O[i]=O[i]+chunkPrefix`) must declare that buffer as **read-write** on EVERY backend. GLSL `layout(...) writeonly buffer`
is a hard COMPILE error the moment you read it — but HLSL `RWStructuredBuffer`, WGSL `var<storage, read_write>`, and MSL
`device float*` all silently permit read-back. Symptom: the op passes on CUDA/DX12/WebGPU and FAILS ONLY on Vulkan, where
`backend.run()` returns false (SPIR-V compile fail), not a value mismatch.

**Why:** the emitters aren't symmetric — GLSL is the only one that enforces the write-only qualifier. So a kernel authored
+ tested on DX12/WebGPU can ship a latent Vulkan break.

**How to apply:** in the GLSL emitter, any buffer read after being written must be plain `layout(std430, binding=N)
buffer BO { ... }` (NO `writeonly`). More generally: **when a new CKIR kernel reuses its output as an input in a later
pass, grep the GLSL emitter for `writeonly` on that binding.** And always build+run the Vulkan suite before declaring an
op cross-backend green — DX12/WebGPU passing does NOT prove GLSL compiles (they're more permissive on buffer qualifiers).
Related: the determinism-tier T2 kernels ([feedback_mission_portable_gpu_compute_all_backends](device-programs.md#memory-feedback_mission_portable_gpu_compute_all_backends)) all use output read-back.

**⭐ SECOND INSTANCE (2026-07-10, D-007 B0-3) — the rule generalizes: GLSL is the TYPE-STRICT backend, use it as the
oracle for IR type errors.** When CKIR comparisons became `bool`-typed, a graph doing `Add(float, cmp_result)` **failed
to compile on Vulkan and silently PASSED on DX12** — HLSL implicitly promotes `float + bool`, GLSL rejects it. GLSL also
rejects `precise bvec3` (the qualifier is float-only) and has no `<` on vectors (needs `lessThan()`), where HLSL's
relational ops are already componentwise. So: an IR-level type mistake surfaces as a **Vulkan-only `run()==false`**, and
a DX12-green result proves nothing about IR type correctness. Corollary: fix such a failure in the **IR/test** (insert an
explicit `cast`), never by loosening the GLSL emitter to match HLSL's permissiveness — the permissive backend is the one
that's wrong. Same session: `KOp::Cast` turned out to be unimplemented in the WGSL/MSL/CUDA emitters (they hit
`default: return false`), invisible until a graph first used it.


<!-- end-memory:feedback_glsl_writeonly_buffer_readback_portability -->

<a id="memory-feedback_gpu_cost_model_must_include_register_occupancy"></a>
## feedback_gpu_cost_model_must_include_register_occupancy

---
name: feedback_gpu_cost_model_must_include_register_occupancy
description: "A GPU GEMM/kernel cost model that ranks schedules MUST include register-file occupancy — a fat register tile hits the 64K/SM register file before smem, and omitting it ranks slow tiles best"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: b0138d6a-548b-428b-87b2-fe30c9f36f7c
---

Building the CKIR auto-scheduler's analytical cost model (ADR-0098 §4 · AS-3, `predict_contract_ms` in
`engine/kir/include/crd/kir/ckir_autotune.hpp`): a roofline `max(compute_ms, memory_ms)` scaled by
occupancy efficiency. The first version computed occupancy from **shared memory + thread count only** —
and ranked the cost-model top-6 **8× slower** than the known optimum (0.136 ms vs 1.15 ms), because it
put the big low-occupancy tiles (256×256) at the TOP: those have the LEAST global-memory traffic
(`1/BM + 1/BN` is smallest), so a memory-biased model loves them.

**The fix — register-file occupancy.** A WarpTiled GEMM thread holds `accum (WMITER·TM·WNITER·TN) +
regM (WMITER·TM) + regN (WNITER·TN) + float4 stage buffers + ~28 overhead` registers. A 256² tile wants
~190 regs/thread × 512 threads = **~98K registers > the 64K/SM register file** ⇒ ZERO blocks fit. So the
occupancy limiter is `resident_blocks = floor(min(smem_per_sm/smem, threads_per_sm/NT,
regs_per_sm/(regs·NT), max_blocks_sm))` — and **registers are the term the big tiles hit first**. With it,
the 128×128 winner-class ranks top and the cost-model top-6 matches (0.957×, actually beats) the
hand-tuned optimum measuring **253× fewer configs** than the full 1516-schedule space.

Two more calibration notes: (1) use the **effective GEMM peak** (~11 TFLOP f32 non-tensor-core, the
roofline knee) not the 44 TFLOP theoretical FMA peak — otherwise compute_ms is negligible and the model
is always memory-bound (→ favors big tiles again); (2) `floor` the resident blocks — a fractional block
can't run, so `blocks < 1` means the config spills/doesn't fit and must be penalized to last.

**Why:** memory-traffic and thread/smem occupancy are the "obvious" cost terms, but for register-tiled
kernels the register file is the binding constraint and it's invisible unless you model it. **How to
apply:** any GPU kernel cost model that ranks register-tiled schedules (GEMM, conv, attention) MUST count
registers/thread against the SM register file, integer-floored, before trusting its ranking. Related:
[feedback_ckir_tiled_gemm_occupancy_not_free](device-programs.md#memory-feedback_ckir_tiled_gemm_occupancy_not_free), [project_v17g_gemm_cublas_parity_89pct](project-history.md#memory-project_v17g_gemm_cublas_parity_89pct),
[feedback_parallel_cook_shaderc_threadhostile_and_fiber_stack](device-programs.md#memory-feedback_parallel_cook_shaderc_threadhostile_and_fiber_stack).


<!-- end-memory:feedback_gpu_cost_model_must_include_register_occupancy -->

<a id="memory-feedback_gpu_kernel_profiling_standalone_not_skip_diag"></a>
## feedback_gpu_kernel_profiling_standalone_not_skip_diag

---
name: gpu-kernel-profiling-standalone-not-skip-diag
description: "Skip-a-kernel GPU pipeline diagnosis is CONTAMINATED — profile each kernel standalone on valid precomputed inputs; and empirically test every \"better\" variant (3 paper-wins measured WORSE)"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 1487a581-3392-44fb-bc9e-ebeaffd19da5
---

Two profiling scars from the 2026-07-13 sort-crush campaign (RTX 4070 TiS, Vulkan):

1. **Skip-a-kernel diagnosis lies.** Disabling one dispatch in a multi-kernel pipeline feeds STALE data to the
   downstream kernels, changing THEIR timing — every inference drawn that way was wrong (claimed "offset=10 ms,
   scatter free"; truth was scatter 0.81, offset 0.30). The correct isolation: a standalone profiler that batch-times
   each kernel ALONE with VALID precomputed inputs (run the real pipeline once to materialize intermediates, then
   loop one kernel over them). See `[.sort-kprof]` in tests/gpu-context-vulkan/test_vulkan_context.cpp.

2. **Empirics beat models — bench every variant.** Three scatter variants that looked better on paper (epb=4096
   longer runs; 512 threads fewer rounds; tagged counts −1 barrier/round) ALL measured worse (0.69/0.53/0.66 vs
   0.429): occupancy loss from +8-16 KB shared and extra rounds dominate the modeled gains. Also: 512-thread epb-4096
   config silently exceeded the 48 KB Vulkan shared cap → UB + absurd timings (exit 42 with garbage ms) — CHECK
   shared-budget × maxComputeSharedMemorySize before trusting any number.

**Why:** hours were lost to contaminated isolation and to keeping "obviously better" variants un-benched.
**How to apply:** for any multi-kernel GPU pipeline, build the standalone-kernel profiler FIRST; treat every
optimization as a hypothesis to measure, revert what loses; verify shared-memory budget against the device cap.
Related: [bit-exact-scan-cannot-crush-decoupled-lookback](numerics-and-performance.md#memory-feedback_bit_exact_scan_cannot_crush_decoupled_lookback) (amended for integers by [integer-lookback-is-bit-exact-sort-not-scan-walled](project-history.md#memory-project_integer_lookback_is_bit_exact_sort_not_scan_walled)).


<!-- end-memory:feedback_gpu_kernel_profiling_standalone_not_skip_diag -->

<a id="memory-feedback_gpu_memory_allocator_lessons"></a>
## feedback_gpu_memory_allocator_lessons

---
name: feedback_gpu_memory_allocator_lessons
description: "Two GPU-memory-allocator constraints — free VkDeviceMemory before vkDestroyDevice, and TLSF can't manage VRAM (needs external-metadata OffsetAllocator)"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 7b0bb65d-6788-4a80-96e7-82e1072ab242
---

GPU memory has different constraints than CPU memory. Two specific ones, both hit
building the Vulkan suballocator (ADR-0085 S6):

**1. Free all `VkDeviceMemory` BEFORE `vkDestroyDevice`.** A backend allocator that
holds pooled `VkDeviceMemory` blocks must free them in an EXPLICIT step in the
Device's dtor BODY, before `vkDestroyDevice` — NOT in the allocator's own destructor
(which, as a member, runs AFTER the dtor body and thus after the device is dead →
`VUID-vkFreeMemory-device-parameter` "Invalid device", access-violation at shutdown).
Pattern: `vkDeviceWaitIdle` → `m_allocator.destroy_all()` (idempotent) → `vkDestroyDevice`.
Same for any per-resource dedicated allocation: all Buffer/Image instances must be
destroyed before the owning Device (standard Vulkan, but now load-bearing).

**Why:** C++ member destructors run after the enclosing dtor body; a raw Vulkan
handle destroyed in the body is gone before members destruct. This is the class of
bug the 3-stage GPU cutover sequencing (build → standalone-test → integrate) surfaces
fast — the regression appeared only when the live renderer shut down, caught by
re-running the existing rhi-vulkan suite + bootstrap smoke after the cutover.

**How to apply:** any future GPU allocator (S7 defrag/residency, eylem-GPU, hesap-GPU)
frees device memory in an explicit pre-`vkDestroyDevice` step; re-run the existing
GPU suite + a real smoke after any allocation-path cutover, not just new tests.

**2. TLSF cannot manage device-local VRAM — use an external-metadata allocator.**
`crd::memory::TlsfAllocator` writes free-list block headers INTO the memory it
manages; you cannot cheaply read/write headers in device-local VRAM. The GPU
suballocator's offset kernel must store metadata in a SIDE array →
`crd::memory::OffsetAllocator` (O(1), Aaltonen float-bins). See [project_no_malloc_sweep_before_v5](project-history.md#memory-project_no_malloc_sweep_before_v5)
for the broader allocator family. ADR-0085 §6.


<!-- end-memory:feedback_gpu_memory_allocator_lessons -->

<a id="memory-feedback_gpu_pipeline_cache_key_by_content_not_pointer"></a>
## feedback_gpu_pipeline_cache_key_by_content_not_pointer

---
name: feedback_gpu_pipeline_cache_key_by_content_not_pointer
description: "GPU pipeline/PSO/state-object caches MUST key by CONTENT hash, never a pointer/handle — caches outlive programs and addresses+handles get recycled"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: fade8ea4-87ca-470f-83e0-cdfe82a44e7f
  modified: 2026-08-08T02:24:07.199Z
---

A cache that OUTLIVES the objects it keys on must key by **content**, never by a raw pointer or an opaque
handle — those get **recycled** for the next object at the same value, so a stale entry silently aliases.

**Scar (D-007 CEIR grind, 2026-08-08):** the DX12 DXR pipeline cache (`dxr_pipeline`, DxrPipe::key) keyed on
each stage's **DXIL data pointer**; the Vulkan twin (`rt_pipeline`, RtPipe::key) on the **VkShaderModule
handle**. The `REN-38 RT GATE (DX12) ... ANY-HIT can IGNORE every hit` test created a cutoff-0 any-hit program,
freed it, then created a cutoff-2 one — ~10% of the time the allocator reused the freed DXIL buffer's address,
so the cache returned the STALE cutoff-0 state object + SBT and the any-hit that should `IgnoreHit` every
candidate accepted them (`1 1 -1 -1` = the previous cutoff's result). Flaked ~1-in-9 in isolation; passed
8/8 by luck first (n=8 is statistically worthless — a ~10% flake passes 8/8 ~43% of the time).

**Fix:** key by `crd::containers::fnv1a_64` over each stage's DXIL/SPIR-V bytes. Distinct shaders → distinct
keys (no false share); a freed-then-realloced identical program → same key (correct reuse). DX12 flake
eliminated (200/200 after; was ~10%). Vulkan was latent on this driver (100/100 both before+after) but the
code was unsound the same way — fixed for cross-backend parity (the [feedback_dx12_hlsl_masks_type_bugs_run_vulkan](device-programs.md#memory-feedback_dx12_hlsl_masks_type_bugs_run_vulkan) doctrine).

**How to apply:** any PSO/pipeline/state-object/blob cache in the engine — check the key is content, not an
address or a device handle. **How to hunt this class:** `ctest --repeat until-fail:200 -R <name>` (source
vcvars once; the test is <1s) is the cheap high-volume flake gate — never conclude from n<100. See
[reference_bat_helpers_need_powershell_tool_not_bash](build-and-verification.md#memory-reference_bat_helpers_need_powershell_tool_not_bash) (run it via the PowerShell tool). Related content-hash
scar: [feedback_struct_padding_in_content_hash_and_cooked_blobs](workflow-and-correctness.md#memory-feedback_struct_padding_in_content_hash_and_cooked_blobs).


<!-- end-memory:feedback_gpu_pipeline_cache_key_by_content_not_pointer -->

<a id="memory-feedback_gpu_timing_asserts_same_pass_only"></a>
## feedback_gpu_timing_asserts_same_pass_only

---
name: gpu-timing-asserts-same-pass-only
description: GPU perf assertions must compare timings from the SAME measurement pass — cross-call comparisons and argmin-equality vs a checked-in DB are noise-fragile and fail under sweep load
metadata: 
  node_type: memory
  type: feedback
  originSessionId: b0138d6a-548b-428b-87b2-fe30c9f36f7c
---

The AS-4 flash-attention autotuner test failed in a full per-slice sweep (passed standalone): (1) it timed the 64×32 heuristic default in a SEPARATE call after the 16-tile sweep — GPU boost-clock drift between the two calls made the same tile measure 16% apart, inverting a 5% margin; (2) it asserted the checked-in autotune-DB row EQUALS today's re-measured argmin — two near-tied tiles swap winner run to run under load.

**Why:** GPU timing comparisons are only valid within one measurement pass (same thermal/clock regime). A checked-in tuned tile's contract is *near-optimality on the device*, never *argmin of a fresh noisy sweep*.

**How to apply:** In autotuner/perf gates: record every baseline tile's time from the same sweep loop that measures the candidates (never a separate timing call); assert the DB row's same-pass time ≤ best·1.30 (calibrated: near-tied tiles measured 1.177 apart SAME-PASS under full-suite GPU load — even same-pass tile-vs-tile ratios swing under thermal/occupancy interaction; a genuinely wrong tile is 1.5–8× off per [ckir-tiled-gemm-occupancy-not-free](device-programs.md#memory-feedback_ckir_tiled_gemm_occupancy_not_free), so 1.30 still catches it) plus an EXACT check that the replay wiring returns the DB row verbatim. Fixed in tests/kir-cuda/test_autotune_cuda.cpp (2026-07-23, margin recalibrated same day). Related: [timeout-is-not-a-hang-proof](workflow-and-correctness.md#memory-feedback_timeout_is_not_a_hang_proof), [perf-jobs-adapter-asan-flake](build-and-verification.md#memory-feedback_perf_jobs_adapter_asan_flake).


<!-- end-memory:feedback_gpu_timing_asserts_same_pass_only -->

<a id="memory-feedback_mesh_shader_device_scars"></a>
## feedback_mesh_shader_device_scars

---
name: feedback_mesh_shader_device_scars
description: "Vulkan mesh-shader (VK_EXT_mesh_shader) device scars — NO_TASK_SHADER, nonuniform-on-constant, VUID-08690, 128 cap"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 9d587ba2-dba2-4939-baa4-c0515d41bdbe
---

Wiring the B4 mesh-shader path (CKIR `Mesh` entry → `emit_mesh_glsl` → shader object → `vkCmdDrawMeshTasksEXT`) hit four
device/driver scars, several SILENT (no validation error). All fixed 2026-07-16 on an RTX Ada adapter.

1. **A mesh shader object created WITHOUT a task shader MUST set `VK_SHADER_CREATE_NO_TASK_SHADER_BIT_EXT`.** Without it the
   driver assumes a task/amplification stage precedes the mesh and **device-losts** (`vkQueueWaitIdle` → `VK_ERROR_DEVICE_LOST`)
   on the dispatch — with NO validation-layer error. This was the entire "the mesh triangle compiles + validates but renders
   nothing (even the clear vanishes)" battle. `submit == VK_SUCCESS` but `wait == -4` is the tell.

2. **`nonuniformEXT` on a UNIFORM (compile-time-constant) bindless index returns ZERO in a mesh shader** on this NVIDIA driver
   (the identical `textureLod(sampler(tex[nonuniformEXT(constIdx)],samp),uv,lod)` works fine in a VERTEX and FRAGMENT shader).
   Symptom: the mesh geometry renders but is FLAT — the vertex-stage texture read silently returns 0, so nothing displaces,
   while the fragment shader reads the same textures fine. Isolate with a procedural displacement (sine): if the mesh shows the
   sine but not the texture, it's the texture read. Fix (correct anyway): the GLSL emitter omits `nonuniformEXT` when the index
   node is a `KOp::Const` — a uniform index needs no nonuniform qualifier.

3. **Once the `meshShader` feature is enabled, EVERY ordinary `vkCmdDraw` must explicitly bind the MESH stage to null**
   (`VUID-vkCmdDraw-None-08690`) so the driver knows it's a vertex draw — else it device-losts. Do it in the shared draw-state
   setter for all vertex draws (gated on the device actually having mesh).

4. **glslang caps a mesh workgroup at 128 invocations** (`error: 'local_size' : too large` at shaderc compile). One-thread-per-
   output-slot ⇒ `local_size = max(vertices, primitives) ≤ 128`. For a grid patch that means an 8×8 patch (64 verts, 98 tris ⇒
   98 threads), not 12×12 (242).

Also: **`maintenance4` must be enabled** for the `LocalSizeId` execution mode glslang emits at SPIR-V 1.6 (VUID-…-LocalSizeId-
06434); and mesh SPIR-V needs ≥1.5 for bindless `nonuniformEXT` (we target 1.6). Both — enable `maintenance4` alongside mesh.

**Why:** each cost a full debug cycle; three were silent (device-lost or zero-read, no validation error), so the standard
"validation is clean ⇒ it's correct" heuristic fails for mesh shaders.
**How to apply:** when adding a mesh path on a new backend/driver: set NO_TASK_SHADER, null-bind MESH on vertex draws, keep the
workgroup ≤128, enable maintenance4, and drop `nonuniformEXT` for constant indices. If a mesh draw renders nothing (even the
clear), check the submit/wait result for DEVICE_LOST before hunting shader logic. Related: [feedback_dispatch_1wg_missing_upload_barrier_race](workflow-and-correctness.md#memory-feedback_dispatch_1wg_missing_upload_barrier_race),
[feedback_shader_capability_needs_device_feature_run_validation](device-programs.md#memory-feedback_shader_capability_needs_device_feature_run_validation), [project_ocean_visual_gaps_before_b16_close](project-history.md#memory-project_ocean_visual_gaps_before_b16_close).


<!-- end-memory:feedback_mesh_shader_device_scars -->

<a id="memory-feedback_migrated_executor_gate_runs_both_gpu_backends"></a>
## feedback_migrated_executor_gate_runs_both_gpu_backends

---
name: feedback_migrated_executor_gate_runs_both_gpu_backends
description: "A CEIR/§128 executor-migration slice's gate MUST run gpu-context on BOTH Vulkan AND DX12 — a Vulkan-only gpu-context pass hides a DX12-only MissingCeirPlan/record hole for a whole band."
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 192f4d9e-b3f7-485b-a775-5391bcd4183c
  modified: 2026-08-15T03:49:22.540Z
---

⛔⛔ When a slice MIGRATES an executor to the generic CEIR record path (§128: raster passes → `record_ceir_render` replaying a per-pass CEIR plan), its close gate MUST run the gpu-context suite on **BOTH backends** — `gpu-context-vulkan` AND `gpu-context-dx12` — not just Vulkan.

**Scar (CEIR-18c, 2026-08-15).** CEIR-17's §128 fix added `FrameExecError::MissingCeirPlan` and required every direct `rec.record(...)` caller to build stack `FramePlans` + pass `&plans` (else a migrated executor silently — then, post-fix, LOUDLY — renders nothing). The Vulkan REN-38-A5 present-pass test got the fix. The **identical DX12 twin** (`test_dx12_frame_graph.cpp` REN-38-A5) did NOT — its `rec.record` lacked `&plans` → `MissingCeirPlan` → `record` returns false. CEIR-17's close gate was **`gpu-context-vulkan 260/0` — Vulkan ONLY**, so the DX12 hole sat red for the whole CEIR-17 band, uncaught. CEIR-18c's blast-radius gate (which ran gpu-context-DX12) finally surfaced it.

**Why it hides:** the migrated-executor record path is shared code, but the per-backend TESTS exercise it separately; a Vulkan-only pass proves nothing about the DX12 twin's call sites. This is the [feedback_gates_run_configs_the_app_never_ships](workflow-and-correctness.md#memory-feedback_gates_run_configs_the_app_never_ships) + [feedback_new_execution_path_must_run_the_full_suite_on_a_real_device_not_just_a_cook_roundtrip](numerics-and-performance.md#memory-feedback_new_execution_path_must_run_the_full_suite_on_a_real_device_not_just_a_cook_roundtrip) class, sharpened: for a MIGRATION slice, "both backends" is mandatory, and `260/0 Vulkan` is NOT a band-close proof on its own.

**How to apply:** at any §128/CEIR-migration close, run `gpu-context-vulkan` AND `gpu-context-dx12` (win) + the Linux Vulkan twin. If you update a migrated-executor test's `rec.record` to build `FramePlans` on one backend, `grep` the OTHER backend's test file for the same call and fix it too. Related: [feedback_gate_reverifies_status_matrix_rows_never_inherits_stale_or_unverified_claims](workflow-and-correctness.md#memory-feedback_gate_reverifies_status_matrix_rows_never_inherits_stale_or_unverified_claims), [feedback_deleting_the_imperative_fallback_unmasks_a_migrated_null_plan_hole](workflow-and-correctness.md#memory-feedback_deleting_the_imperative_fallback_unmasks_a_migrated_null_plan_hole).


<!-- end-memory:feedback_migrated_executor_gate_runs_both_gpu_backends -->

<a id="memory-feedback_mission_portable_gpu_compute_all_backends"></a>
## feedback_mission_portable_gpu_compute_all_backends

---
name: feedback_mission_portable_gpu_compute_all_backends
description: "THE v17 mission — portable GPU compute system, all backends perfect + performant + bit-exact; don't rabbit-hole one vendor kernel"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 934ee96e-34fa-4239-87ad-44921a7d5a19
---

⭐⭐ THE MISSION (user, 2026-07-07, re-anchored): CKIR/v17 is a **portable GPU compute system** — one IR, **ALL backends**
(Vulkan · CUDA · DirectX 12 · WebGPU · Metal · HIP) — where **every backend works perfectly, is absolutely performant,
and is bit-exact as much as possible.** Portable breadth + correctness + speed across ALL of them is the goal.

**Why:** the user is building a universal GPU compute foundation (see [project_v17_gpu_compiler_maximal_scope](project-history.md#memory-project_v17_gpu_compiler_maximal_scope),
[project_hesap_is_universal_foundation_zero_defect](project-history.md#memory-project_hesap_is_universal_foundation_zero_defect)), meant to run everywhere (browser-to-everywhere,
[project_browser_wasm_deployment_goal](project-history.md#memory-project_browser_wasm_deployment_goal)). The value IS portability + correctness, not winning one vendor benchmark.

**How to apply — the drift lesson (learned the hard way 2026-07-07):** I spent ~15 turns rabbit-holing on ONE op
(GEMM), ONE backend (CUDA), vs ONE vendor library (cuBLAS-TF32), on ONE nerfed consumer card — chasing the last 10%
against NVIDIA's hand-SASS kernel. That is OFF-MISSION: it's CUDA-specific (anti-portable), single-op, single-card.
Findings from it that DO matter: we beat cuBLAS-**FP32** (the correct op) 1.02–1.06×; consumer Ada nerfs TF32-tensor to
~1:1 (datacenter 8:1); the cuBLAS-TF32 gap is pure SASS (ptxas can't emit it); **the portable tensor path is Vulkan
`cooperative_matrix`** (driver schedules the tensor SASS → the SASS wall is bypassed; runs NVIDIA/AMD/Intel/browser).
**Rule: don't over-invest beating a single vendor library on a single backend/card to the last 1%. Optimize for
PORTABLE performance + bit-exactness across ALL backends. Breadth of ops × breadth of backends > one perfect kernel.**
When tempted to grind one vendor number, stop and ask: does this make the whole 6-backend system better? If not, move on.


<!-- end-memory:feedback_mission_portable_gpu_compute_all_backends -->

<a id="memory-feedback_native_gpucommand_capability_tier_kernel_ref_is_cooktime_not_execution_tier"></a>
## feedback_native_gpucommand_capability_tier_kernel_ref_is_cooktime_not_execution_tier

---
name: feedback_native_gpucommand_capability_tier_kernel_ref_is_cooktime_not_execution_tier
description: "CEIR op classification — native{provider=host}+GPUCommand = a host-orchestrated DEVICE-CAPABILITY tier; kernel_ref is a cook-time dependency marker, NOT an execution-tier flag (so native+kernel_ref is legal, not a contradiction)"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 192f4d9e-b3f7-485b-a775-5391bcd4183c
  modified: 2026-08-16T04:09:09.867Z
---

When declaring a new CEIR dialect whose ops drive the DEVICE via host-recorded commands (RT: `ceir.rt` blas/tlas/sbt build, trace, ray_query; future: video, ML-inference, any driver-builtin), there are THREE cells, not two:

- **pure host resolve** — scene.resolve_* : `native{provider=host}`, effects = SceneRead/HostStateRead, NO GPUCommand, NO kernel_ref.
- **raw device dispatch** — compute.dispatch / render.draw : effects = `[GPUCommand, MemoryReadWrite]` + `kernel_ref`, NO native (the kernel IS the payload; the op is a thin device primitive).
- **host-orchestrated device capability (the NEW cell, CEIR-19a)** — `native{provider=host, ExternalNondeterminism, hot_reload_safe=false}` + effects = `[GPUCommand, MemoryReadWrite]`, and OPTIONALLY `kernel_ref`. The host implements the op as a native intrinsic that emits GPU commands (trace_rays_pipeline, vkCmdBuildAccelerationStructures, dispatch_inline_ray_query). It's the replaceable-capability tier: a user CEIR program (e.g. a wavefront path tracer) composes these ops.

**Two rules that resolve the recurring confusion (both advisor-confirmed):**

1. **An op with NO `kernel_ref` can ONLY be native.** `rt.trace`'s shader groups ride the SBT's symbol attrs, not a `kernel_ref`, so nothing but a registered host intrinsic can implement it ⇒ trace is *forced* native. Don't then split a sibling (ray_query) to device-execute — that fractures one capability surface across tiers.

2. **`kernel_ref` is orthogonal to the execution tier.** Its job (per §106/CEIR-13c) is cook-time dependency EXTRACTION + §107 interface PINNING — *who depends on this kernel* — read by the program_asset collector. It is NOT a "this is device-execute, not native" marker. So `native` + `kernel_ref` on the same op (rt.ray_query) is legal and coherent: the host intrinsic consumes the symbol.

**Why (the scar):** I oscillated B(4 native)→A(6 native)→doubting A, treating native+kernel_ref as self-contradictory. It isn't. **How to apply:** before declaring, cheap-check by grep — the §106 collector (program_asset.cpp) reads `info->intrinsic`/`native_provider` and `info->kernel_ref_symbol` INDEPENDENTLY, and dialect.cpp only asserts `intrinsic ⇒ native_provider non-empty`; NOTHING assumes `kernel_ref ⇒ not-native`. If a future consumer ever classifies by presence-exclusivity, that grep catches it. See [feedback_everything_is_an_authorable_asset_ceir](execution-ir.md#memory-feedback_everything_is_an_authorable_asset_ceir) (native = the replaceable-capability tier) and the ADR-0110 native-intrinsic schema.


<!-- end-memory:feedback_native_gpucommand_capability_tier_kernel_ref_is_cooktime_not_execution_tier -->

<a id="memory-feedback_parallel_cook_shaderc_threadhostile_and_fiber_stack"></a>
## feedback_parallel_cook_shaderc_threadhostile_and_fiber_stack

---
name: feedback_parallel_cook_shaderc_threadhostile_and_fiber_stack
description: Parallel shader cook on crd-jobs — shaderc compiler is thread-hostile AND the cook overflows the 64KB Small fiber; both crash as silent 0xC0000005
metadata: 
  node_type: memory
  type: feedback
  originSessionId: b0138d6a-548b-428b-87b2-fe30c9f36f7c
---

Running the CKIR **offline cook** (`cook_compute_shader`) concurrently on the fiber-based `crd-jobs`
scheduler (D-007 D10 `cook_variant_matrix_parallel`) hit TWO separate crashes, both surfacing as a
bare `0xC0000005` access violation (exit `-1073741819`) with NO Catch2 diagnostic — the process just
dies mid-test after the last PASSED assertion.

**Scar 1 — `shaderc_compiler_t` is thread-HOSTILE.** `compile_glsl_to_spirv` used a single shared
compiler behind a `static ShadercLoader global_loader()`. shaderc's compiler object must be used from
one thread at a time; two `crd-jobs` workers compiling at once race → crash. Fix: a **`thread_local`**
compiler in `ShadercLoader::compile()` (each thread inits its own once, released at thread exit via a
`thread_local` RAII holder whose dtor calls `compiler_release`; the singleton's `m_api` outlives every
worker). No hot-path regression (single-thread still inits exactly one). `engine/gpu-context-vulkan/src/vulkan_glsl_compile.cpp`.

**Scar 2 — the cook OVERFLOWS the 64 KB Small fiber.** `jobs::parallel_for` defaults to
`StackSize::Small` = **64 KB** (Medium 512 KB, Large 2 MB — `fiber_pool.cpp`). The cook runs the whole
glslang/shaderc front-end + the CKIR emitter + serializer, which need a real thread-sized stack (an OS
thread gives shaderc 1 MB and it's fine). 64 KB overflows *inside shaderc*. Fix: pass
**`crd::jobs::StackSize::Large`** (2 MB; 16 Large fibers exist by default via `Config::large_fiber_count=16`).

**Why:** both are invisible — no validation error, no assert, no stack-overflow message; just a hard
AV. Isolating meant proving the sibling test ([d11], off-thread *pipeline* creation, which is stack-light
and single-resource) passed while [d10] (the cook) crashed → the crash was specific to running the
heavy compiler on a worker fiber. **How to apply:** any time you move a *compiler/emitter* (shaderc,
DXC, NVRTC) onto `crd-jobs`, (a) make its global compiler `thread_local`, and (b) request `StackSize::Large`.
Related: [feedback_reflect_needs_unoptimized_spirv_and_compiler_injection](build-and-verification.md#memory-feedback_reflect_needs_unoptimized_spirv_and_compiler_injection), [project_d1_d5_ir_as_crdr_deploy](project-history.md#memory-project_d1_d5_ir_as_crdr_deploy),
[feedback_concurrent_tests_use_crd_jobs](build-and-verification.md#memory-feedback_concurrent_tests_use_crd_jobs), [feedback_use_crash_dumps_first](workflow-and-correctness.md#memory-feedback_use_crash_dumps_first).


<!-- end-memory:feedback_parallel_cook_shaderc_threadhostile_and_fiber_stack -->

<a id="memory-feedback_shader_capability_needs_device_feature_run_validation"></a>
## feedback_shader_capability_needs_device_feature_run_validation

---
name: feedback_shader_capability_needs_device_feature_run_validation
description: "A new shader/raster feature needs BOTH a matching device feature AND draw-time state, and its PSO/shader may only be legal with a matching raster state — NVIDIA/DX12 run the happy path anyway, so a green gpu-context test is NOT validation-clean; run the Vulkan validation layer AND the DX12 debug layer on every emitter/state change"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 40e3ad67-a505-447d-89df-272b48c237f6
---

A green Catch2 run on the `crd-gpu-context` raster suite is **NOT proof you drove the API right** — the tests default
to `enable_validation = false` (no Vulkan debug messenger) and create the DX12 device with no debug layer, so a
validation/debug-layer error only prints to stderr, it does NOT fail the test. On EVERY new shader feature (a qualifier,
builtin, KOp→capability, or a new raster state) run **both** oracles: Vulkan with `cfg.enable_validation = true`
(grep stderr for `VUID|Validation Error`), AND DX12 with `D3D12GetDebugInterface→EnableDebugLayer()` + an
`ID3D12InfoQueue` that dumps `Severity <= WARNING` (grep for `sev=0/1`). This is the [reference_sanity_doctrine](workflow-and-correctness.md#memory-reference_sanity_doctrine) /
[feedback_v9_gpu_sanity_harness](device-programs.md#memory-feedback_v9_gpu_sanity_harness) oracle for "did I drive the API right?".

**Three failure modes, all latent because the vendor runs the happy path:**
1. **Shader capability ⇒ device feature** (`VUID-...-pCode-08740` at `vkCreateShaders`). B1-b/c: `sample`→`SampleRateShading`
   ⇒ `sampleRateShading`; `discard`→`DemoteToHelperInvocation` ⇒ `shaderDemoteToHelperInvocation`. B1-f: a fragment
   **storage-buffer WRITE** ⇒ needs `fragmentStoresAndAtomics` (else `VUID-RuntimeSpirv-NonWritable-06340` demands every
   FS storage var be `NonWritable`). B2-c: a **CUBE-ARRAY** texture ⇒ the `SampledCubeArray` capability AND the
   `VK_IMAGE_VIEW_TYPE_CUBE_ARRAY` view both need `imageCubeArray` (`VUID-...-pCode-08740` + `VUID-VkImageViewCreateInfo-viewType-01004`
   — one feature fixes both the shader and the view). Pattern: a new texture DIMENSION or descriptor form is as capable of
   needing a device feature as a shader op — grep the whole draw path, not just `vkCreateShaders`.
2. **Raster state ⇒ companion draw-time dynamic state** (draw-time VUID, not shader-creation). B1-f: with shader objects,
   conservative **OVERESTIMATE** mode also requires `vkCmdSetExtraPrimitiveOverestimationSizeEXT` set per draw
   (`VUID-vkCmdDraw-None-07632`) — and its EDS3 sub-feature `extendedDynamicState3ExtraPrimitiveOverestimationSize`
   enabled. Enabling the conservative-MODE dynamic state was not enough.
3. **DX12 HAS an equivalent oracle** (correcting the old claim here). The D3D12 debug layer caught a real **ERROR**
   (sev=1) B1-f: an FS reading **`SV_InnerCoverage`** links ONLY into a PSO with `ConservativeRaster = ON` —
   `CreateGraphicsPipelineState` with conservative OFF FAILS and logs the error. Because `create_raster_program` sees
   only opaque DXIL, the fix is to carry the requirement from the IR: the DX12 program computes a
   `wants_conservative_raster()` bit by scanning the graph for a reachable `KBuiltin::InnerCoverage` at IR→program time,
   and the raster context prebuilds `m_pso1` conservative for it (never attempting — and logging — the failing plain
   build). Generalises: an FS whose legality depends on a raster state (ROV, inner coverage) must signal that state to
   PSO build, not discover it by a failing attempt.

**Why it matters:** three of these were invisible on NVIDIA + a green suite; only the two debug oracles surfaced them.
Vulkan raster features are gated on `m_graphics_family != UINT32_MAX` so a pure-compute device stays minimal
([project_rhi_device_is_adopted_from_gpu_context](project-history.md#memory-project_rhi_device_is_adopted_from_gpu_context)). Follow-on worth filing: bake a debug-messenger / info-queue
ValidationCapture into the gpu-context raster suite so this becomes an automatic gate, not a manual grep.


<!-- end-memory:feedback_shader_capability_needs_device_feature_run_validation -->

<a id="memory-feedback_shader_frag_xy_unit_conflict_pixel_vs_normalized"></a>
## feedback_shader_frag_xy_unit_conflict_pixel_vs_normalized

---
name: feedback_shader_frag_xy_unit_conflict_pixel_vs_normalized
description: "A shared shader-input field (frag_xy) consumed by two callers wanting OPPOSITE units silently defaulted to a const and broke clustering; normalize AT the consumer, keep the field's documented unit"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 192f4d9e-b3f7-485b-a775-5391bcd4183c
  modified: 2026-08-15T21:29:21.322Z
---

**Scar (CEIR-18a-2 Stage 2a, 2026-08-16).** The clustered forward FS mapped EVERY pixel to one
froxel on its first device run. Two independent bugs behind one symptom:

1. `body_scene_authored` (scene_renderer.cpp) hard-coded `in.frag_xy = -1`, so the lighting body's
   fragment coordinate fell back to a constant `(0.5,0.5)` → the froxel index was the same for every
   pixel. A whole rendering feature (per-tile light lists) was inert and NO shader failed.
2. A **UNIT CONFLICT** on ONE field: `LightingInputs.frag_xy` is documented PIXEL coords (the PCF /
   interleaved-gradient rotation wants per-pixel variation), but the cluster block did
   `frag_xy * grid` expecting NORMALIZED [0,1) screen UV. Feeding pixels raw would explode the index
   (thousands) → OOB list reads; feeding normalized would flatten the PCF rotation.

**Rule.** When a shared shader-input is consumed two ways with different unit conventions, keep the
field in its DOCUMENTED unit and convert AT the consumer that needs the other — never overload the
field's meaning per-caller. Here: `frag_xy` stays PIXEL coords (fed from `FragCoord.xy`), and the
cluster block normalizes by viewport-dims header words it reads itself (`hdru(viewport_w/h)`), with
the froxel axes CLAMPED to `[0,grid-1]` (an edge pixel floors to `grid` = one past the last tile).
Chosen over corrupting `frag_xy` into normalized coords (advisor "Option C" — the ign/PCF callers
would silently get wrong-unit input, the exact banding class this repo already documents).

**How to apply.** (a) A shader-input default of `-1`/const is a silent-drop landmine — if a feature
reads it, that feature is dead until it's wired; treat "the ABI carries no X" comments as debt, not
a settled boundary. (b) Resolution-agnostic screen→tile math reads the viewport dims from a HEADER
WORD the host writes each frame (`target.width()/height()`), never a cook-time constant (survives a
resize with no recompile). (c) Adding a header word that the cook BAKES ⇒ wire all four: parse,
serialize, variant-id hash (under the enabling guard), and a cook-time reject-if-unset — plus a
host-side ABI-match validate that the asset's declared word == the engine's `kHdr*` constant (the
non-clustered `light_off` convention never had that check; clustering added it).

Related: [feedback_ckir_if_block_shared_temp_scope_materialize](rendering.md#memory-feedback_ckir_if_block_shared_temp_scope_materialize) (another silent create_program
class), [feedback_new_execution_path_must_run_the_full_suite_on_a_real_device_not_just_a_cook_roundtrip](numerics-and-performance.md#memory-feedback_new_execution_path_must_run_the_full_suite_on_a_real_device_not_just_a_cook_roundtrip)
(the E5 defect only appeared on the FIRST device run), [feedback_declared_header_words_must_be_validated_at_cook_time](workflow-and-correctness.md#memory-feedback_declared_header_words_must_be_validated_at_cook_time).


<!-- end-memory:feedback_shader_frag_xy_unit_conflict_pixel_vs_normalized -->

<a id="memory-feedback_shader_pair_disagreement_needs_declared_cooktime_contract"></a>
## feedback_shader_pair_disagreement_needs_declared_cooktime_contract

---
name: feedback_shader_pair_disagreement_needs_declared_cooktime_contract
description: "A VS/FS varying mismatch links, binds and renders the wrong field — no validation layer on either backend can see it, so the agreement must be a DECLARED contract checked at cook time"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: a3482f73-d858-400b-816d-942216e20052
  modified: 2026-07-26T22:12:07.776Z
---

Two shader stages can disagree about their interface and **nothing at runtime can tell you**. Cerid's scene
SKINNED vertex program emitted 2 of the 4 varyings every cooked fragment program reads: a skinned draw shaded
from **undefined interpolants** at locations 2 and 3 (the world position the specular and shadow terms need, and
the uv). It linked, it bound, it rendered. Vulkan validation: silent. DX12 debug layer: silent.

**Why:** locations are matched positionally by the driver. A missing, renamed, narrowed or re-interpolated
varying is a legal program that reads whatever occupies the slot.

**How to apply:**
- When two artifacts must agree and no runtime can check it, make the agreement a **declared contract verified at
  cook time**. A convention is not a check.
- Check **name, location, width AND interpolation** — all four. A name-only check passes a `vec2` at location 3
  against a `vec3` at 0. A smooth varying read as `flat` takes the provoking vertex's value across the whole
  triangle: faceted output that reads as a normals bug in a shader whose normals are fine.
- Best structural fix: have every stage that feeds the same fragment program emit **one shared declared varying
  set** (`crd-vertex-cook`'s `kVsVaryings`), so the mismatch is impossible to write rather than merely rejected.
- Related shape, same session: `lighting::pcf_shadow` takes a **vec2** uv while every atlas is LAYERED — passing
  a vec3 hits `nodes::detail::bin`'s documented *"two mismatched vectors, a caller error"* arm, which returns a
  valid node id and produces a shader that **fails to compile**, with nothing pointing at the uv width. Read the
  operand WIDTHS a helper assumes, not just its parameter names.

See [feedback_authored_asset_slice_done_only_when_cpp_deleted_and_renders](rendering.md#memory-feedback_authored_asset_slice_done_only_when_cpp_deleted_and_renders) and
`docs/sessions/2026-07-27-ren38-authored-programs.md`.


<!-- end-memory:feedback_shader_pair_disagreement_needs_declared_cooktime_contract -->

<a id="memory-feedback_v9_gpu_sanity_harness"></a>
## feedback_v9_gpu_sanity_harness

---
name: v9-gpu-sanity-harness
description: "Phase 3.1.7 v9 GPU geometry slices follow a 4-piece sanity discipline using the v9-prereq-test-harness helpers — ValidationCapture, ulp/bit_compare, gpu_determinism_check, CRD_PERF_BUDGET_LE"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: b24674c3-970b-481c-a127-bf4231bceca3
---

Every Phase 3.1.7 v9 GPU geometry slice (v9c-a V-HACD onward) follows the same 4-piece GPU sanity discipline, locked at v9-prereq-test-harness close (2026-05-18).

**The discipline:**
1. **Wrap setup in `crd::rhi::ValidationCapture`** (engine/rhi-vulkan) — assert 0 errors / 0 warnings unless intentionally provoking validation (negative test).
2. **`crd::test::bit_compare<T>` / `ulp_compare<float>`** (tests/test_helpers) — GPU readback vs CPU reference for any kernel that has a CPU reference. Default tolerance 1 ULP.
3. **`crd::test::gpu_determinism_check(dispatch, read_bytes, 3)`** — 3 rounds; required if the slice claims determinism, omitted (with comment in test) if the kernel is throughput-tier atomics.
4. **`CRD_PERF_BUDGET_LE("name", max_ms, lambda)`** (crd-perf) — per published per-kernel budget (e.g. v9a-close locks "1M primitives in <8 ms on RTX 3060"). Note: macro is no-op in pure Release (CRD_ASSERT_MSG → NDEBUG → void), so for Release-mode enforcement wrap measure_ms in a Catch2 CHECK.

**Per-slice DoD addition:** GPU slices run `scripts/per-slice-check.ps1 -IncludeRelease -Parallel` (5 configs: debug + asan + shipping + **release** + tidy). The release config is opt-in via `-IncludeRelease`, not default — added specifically because middle-insertion of pure-virtuals at Phase 3.1.7.6 v0-close caused a vtable mis-dispatch that ONLY surfaced under LTCG (see [vtable-stability-append-at-end](build-and-verification.md#memory-feedback_vtable_stability_append_at_end)). GPU slices that build new kernels need the release check; pure-CPU slices stay on the 4-config default.

**Why:** The 16 v9 slices all face the same sanity wall — "did the GPU do what the CPU does, or did atomics race-condition to a wrong-but-sometimes-valid answer?" Without this discipline, slices ship with implicit-correctness-by-test-passes that masks throughput-tier non-determinism, validation errors the driver swallowed, or kernel timings that creep past budget unnoticed. Built once in v9-prereq-test-harness so every v9 slice consumes one set of helpers — discipline lands at the start instead of leaking in over time.

**How to apply:**
- New v9 GPU test file: include `<crd/rhi/vulkan_validation_capture.hpp>`, `<crd/test_helpers/gpu_compare.hpp>`, `<crd/test_helpers/gpu_determinism.hpp>`, `<crd/perf/measure.hpp>`.
- ValidationCapture needs the Instance built with `enable_validation = true` — that's a test-fixture concern.
- Determinism check requires `dispatch` to fence + reset before returning so `read_bytes()` sees stable data.
- Per-slice DoD invocation: `scripts/per-slice-check.ps1 -IncludeRelease -Parallel`.
- ValidationCapture's Impl forward decl is public in the header (necessary for the Vulkan callback's static_cast); definition lives in .cpp so internals stay encapsulated.

See [never-defer-solve](workflow-and-correctness.md#memory-feedback_never_defer_solve) for the broader "solve don't defer" rule that drove building the harness instead of filing per-v9-slice debt for sanity-check gaps.


<!-- end-memory:feedback_v9_gpu_sanity_harness -->

<a id="memory-feedback_vulkan12features_struct_conflicts_use_the_extension"></a>
## feedback_vulkan12features_struct_conflicts_use_the_extension

---
name: feedback_vulkan12features_struct_conflicts_use_the_extension
description: "⛔ `VkPhysicalDeviceVulkan12Features` is MUTUALLY EXCLUSIVE with the individual promoted structs a real device already needs (DescriptorIndexing for bindless, BufferDeviceAddress for RT/DGC) — VUID-VkDeviceCreateInfo-pNext-02830. To get `drawIndirectCount` without rewriting the whole chain, enable the `VK_KHR_draw_indirect_count` EXTENSION instead: it satisfies VUID-…-None-04445 with no feature struct at all"
metadata:
  node_type: memory
  type: feedback
  originSessionId: ce31551a-2d4a-49f7-9c12-7bb0acdebc75
  modified: 2026-07-29T23:36:06.586Z
---

`vkCmdDrawIndexedIndirectCount` is **core since Vulkan 1.2 and still gated**: calling it ungated is
VUID-vkCmdDrawIndexedIndirectCount-None-04445. The feature bit lives *only* in
`VkPhysicalDeviceVulkan12Features::drawIndirectCount`, so the obvious move is to chain that struct.

**Do not.** A device that also enables `VkPhysicalDeviceDescriptorIndexingFeatures` (bindless) and
`VkPhysicalDeviceBufferDeviceAddressFeatures` (RT / device-generated commands) — as this engine's does — cannot
also chain the aggregate `Vulkan12Features`: an aggregate and its constituent promoted structs are mutually
exclusive (**VUID-VkDeviceCreateInfo-pNext-02830**). Validation says so the moment the app runs, and folding
descriptor-indexing + BDA into the aggregate means touching every RT and bindless path.

**The other legal way to satisfy 04445 is to enable `VK_KHR_draw_indirect_count`** — the extension carries no
feature struct, so there is no aggregation conflict, and the core entry point may then be called. Probe it in the
device-extension list, gate on `graphics_family != UINT32_MAX`, add it to the enabled list, and report the answer
through a capability accessor.

**Why it matters beyond this bit:** the ability was first shipped with `indirect_count_supported()` hardcoded
`true`, which was a lie on a device where nothing had been requested — the gate caught it at 27/28 assertions. A
declared capability must be **queried**, not assumed, and the fallback (clamp to `max_draws`, empty slots costing a
zero-instance command) must be a NAMED step-down, not a silent one.

Related: [feedback_use_every_api_ability_never_level_down_to_the_common_denominator](workflow-and-correctness.md#memory-feedback_use_every_api_ability_never_level_down_to_the_common_denominator) ·
[feedback_shader_capability_needs_device_feature_run_validation](device-programs.md#memory-feedback_shader_capability_needs_device_feature_run_validation) ·
[feedback_cook_only_gates_ship_device_impossible_programs](workflow-and-correctness.md#memory-feedback_cook_only_gates_ship_device_impossible_programs).


<!-- end-memory:feedback_vulkan12features_struct_conflicts_use_the_extension -->

<a id="memory-reference_ckir_bootstrap_via_write_eval_verify_before_commit"></a>
## reference_ckir_bootstrap_via_write_eval_verify_before_commit

---
name: reference_ckir_bootstrap_via_write_eval_verify_before_commit
description: "How to author a COMPLEX .ckir kernel (For loops, data-dependent control flow, dozens of nodes) — bootstrap it via ckir_write, EVAL-VERIFY it before committing, then delete the builder (the 18a-1 mold + an eval-verify step)."
metadata: 
  node_type: memory
  type: reference
  originSessionId: 192f4d9e-b3f7-485b-a775-5391bcd4183c
  modified: 2026-08-18T13:06:07.550Z
---

Hand-authoring a `.ckir` text asset node-by-node is fine for a straight-line kernel (relu, dequant —
~10-40 nodes). For a COMPLEX kernel (a `For` loop with `ForBreakIf`, an accumulator, dozens of nodes —
e.g. `spmv_csr.ckir`, ~22 nodes + 4 stmts with `body=[begin,count]` nesting + a `KernelLoopVar`),
hand-authoring the positional node graph is error-prone. Bootstrap it instead.

**The flow (CEIR-23e-a — the 18a-1 write→commit→read→delete mold PLUS an eval-verify step):**

1. **Build the kernel in C++** (a `KGraph` in a throwaway `[.][bootstrap]` hidden test): `buffer_decl` /
   `builtin` / `binary` / `stmt_for_begin`+`kernel_loop_var`+`stmt_for_break_if`+`stmt_for_end` /
   `stmt_buffer_store`. (Building in C++ for a TEST is allowed — the mandate forbids C++ as the shipped
   ALGORITHM source, not as a bootstrap/verification tool; the prefix-scan For tests do the same.)
2. **⭐ EVAL-VERIFY BEFORE committing**: `eval_cpu_kernel(g, e, bufs, ...)` vs the CPU oracle IN THE
   BOOTSTRAP, and assert. This is the step 18a-1 lacked — it proves the kernel is CORRECT before the
   builder (the only thing that can regenerate it) is deleted. A wrong kernel caught here is a builder
   fix; a wrong kernel caught after deletion is a hand-edit of opaque node text.
3. **`ckir_write(g, e, alloc)`** → the canonical text; write it to the scratchpad.
4. **Commit the asset**: copy the scratchpad text into `assets/ckir/<name>.ckir`, replacing the generic
   `ckir_write` header comment with a DESCRIPTIVE header + section comments (comments are ignored by
   `ckir_read`, so the eval stays byte-identical; keep the node/stmt lines VERBATIM from the verified
   output — don't re-transcribe).
5. **Author the committed-asset reading gate** (device-free): `ckir_read` the committed `.ckir` →
   `eval_cpu_kernel` == the CPU oracle + `emit_compute_kernel_glsl` smoke. This is the durable proof
   (the 19z asset-inventory rule — every committed asset has a reading gate).
6. **DELETE the bootstrap** builder/test (leave a marker comment: "bootstrapped here, then DELETED").
   The committed `.ckir` is the sole source; the device gates + the reading gate cover correctness.

**Prerequisite for loop kernels — a micro-gate first** (CEIR-23d-1): before authoring the real kernel,
prove the loop machinery round-trips: build a tiny version, `ckir_write`→`ckir_read` byte-exact, eval the
DESERIALIZED graph == oracle, emit GLSL. This surfaces serializer/eval gaps (it caught the lockstep
uniform-For finding: [feedback_ckir_eval_for_bound_is_uniform_use_max_plus_forbreakif](device-programs.md#memory-feedback_ckir_eval_for_bound_is_uniform_use_max_plus_forbreakif)) before you
sink effort into the full kernel.

Related: [reference_ceir_text_asset_authoring_via_print](execution-ir.md#memory-reference_ceir_text_asset_authoring_via_print) (the `.ceir` MODULE twin — bootstrap-via-print,
keep the builder as an anti-drift oracle; a `.ceir` module has no author-then-delete path yet, unlike a
`.ckir` kernel).


<!-- end-memory:reference_ckir_bootstrap_via_write_eval_verify_before_commit -->

<a id="memory-reference_cuda_graphs_capture_recipe"></a>
## reference_cuda_graphs_capture_recipe

---
name: reference_cuda_graphs_capture_recipe
description: "The gold-standard CUDA-Graphs stream-capture recipe (instantiate-once/launch-many, dispatches-only, THREAD_LOCAL, completeness guard) as built for CEIR-29b-2a."
metadata: 
  node_type: memory
  type: reference
  originSessionId: cb9df3b8-2389-479b-9d99-d3d6ce3ba327
  modified: 2026-09-05T07:50:41.097Z
---

CUDA-Graphs capture on `CudaComputeContext` (CEIR-29b-2a; NVIDIA-only — the runnable §70 native-graph
provider, NO cross-backend mirror). The recipe, with the five non-obvious corrections:

1. **Split end from launch — instantiate ONCE, launch MANY.** `end_capture()` does
   `cuStreamEndCapture` → `cuGraphInstantiateWithFlags` and returns an owned handle (`CUgraphExec` +
   `CUgraph`, RAII-freed); `launch()` = `cuGraphLaunch` on the stream, ev0/ev1-bracketed so
   `last_gpu_ms()` reports graph time. A `submit_captured()` that re-instantiates per call measures
   capture+instantiate overhead and LOSES to the fallback — the whole point of graphs is replay.

2. **Capture the DISPATCHES ONLY.** Stream capture records `cuMemcpyAsync` too, so run H→D uploads via
   `begin()/submit_and_wait()` BEFORE `begin_capture()` (a captured upload re-runs per launch). Then
   the graph node count == the stage/dispatch count (barriers are CUDA no-ops → no nodes) — the sharp
   identity gate.

3. **`CU_STREAM_CAPTURE_MODE_THREAD_LOCAL`, not GLOBAL.** GLOBAL lets ANY unsafe CUDA call from ANY
   thread invalidate the capture non-deterministically.

4. **Completeness guard (two halves).** (a) Track `cuStreamBeginCapture`'s SUCCESS in a `m_capturing`
   bool — a failed begin runs dispatches EAGERLY, so `end_capture()` must refuse `cuStreamEndCapture`
   on a non-capturing stream. (b) `cuStreamEndCapture` returning `_INVALIDATED`/`_UNJOINED` (a partial
   graph) or a null graph, or an instantiate failure, must yield an INVALID handle (never a partial
   one). The gate `REQUIRE(graph.valid())` BEFORE comparing outputs — node-count alone passes if
   capture started fine then a late dispatch invalidated it.

5. **Call `cuGraphInstantiateWithFlags(&exec, graph, 0)` DIRECTLY**, not the `cuGraphInstantiate` macro
   — the macro's arity differs by toolkit (3-arg 12.x/13.x vs 5-arg legacy). `WithFlags` is stable
   since 11.4. Verified compiling on Win CUDA 13.3 AND WSL CUDA 12.0.

**Bit-exact vs the N-dispatch fallback is BY CONSTRUCTION**: the graph launches the SAME fmad=false
kernels in the SAME stream-capture dependency order — no FP reorder. The gate runs the plan three ways
(fallback / graph / graph-replay) and asserts graph==fallback==oracle + replay==graph. Verify on BOTH
real devices — graph instantiation is driver-version-sensitive (WSL2 passthrough driver ≠ Windows).

⛔ A bridge that wants ONE `last_gpu_ms` for "whole program incl. the captured subgraph" must design its
OWN timing bracket — the captured region's events are INSIDE the graph, launched later. See
[feedback_cuda_emitter_signature_is_a_contract_shared_by_every_launch_site](device-programs.md#memory-feedback_cuda_emitter_signature_is_a_contract_shared_by_every_launch_site).


<!-- end-memory:reference_cuda_graphs_capture_recipe -->

<a id="memory-scars_ckir_emitter_eval"></a>
## scars_ckir_emitter_eval

---
name: scars_ckir_emitter_eval
description: Scars for CKIR emitter / oracle / eval / IR→GPU lowering (u32-wrap, broadcast, materialize-out-of-scope, sentinel-by-asset-path, decl-DAG, hoist-RAW, scalar-eval, fixed-radix fft, For-bound uniform) — relocated out of MEMORY.md; open before "fixing" a CKIR emit/eval/lowering symptom.
metadata: 
  node_type: memory
  type: reference
  originSessionId: cb9df3b8-2389-479b-9d99-d3d6ce3ba327
  modified: 2026-09-06T00:07:25.023Z
---

CKIR emitter / oracle / eval / lowering scars, moved out of the always-loaded `MEMORY.md` (CEIR-26z compaction,
2026-09-05). Recall on demand when the work touches CKIR emit / eval / IR lowering. Read before "fixing" a matching symptom.

## CKIR oracle + emitter
- [⛔⛔ u32 wrap](device-programs.md#memory-feedback_ckir_oracle_u32_arithmetic_must_wrap_mod32); [bin bcast](device-programs.md#memory-feedback_ckir_binary_vec_scalar_shape_mismatch_gpu_broadcasts_oracle_oob); [temps u32](rendering.md#memory-feedback_ckir_if_block_shared_temp_scope_materialize); [⛔⛔ For-temp materialize→out-of-scope for later loops; device-compile only](rendering.md#memory-feedback_ckir_emitter_materializes_multiuse_node_at_first_loop_use_out_of_scope_for_later_loops); [load RAW](device-programs.md#memory-feedback_ckir_inline_buffer_load_read_after_write); [⛔⛔ sentinel=grep ASSET-PATH not symbol (every emit+load site, both backends)](workflow-and-correctness.md#memory-feedback_sentinel_izing_a_kernel_asset_breaks_every_emit_site_grep_the_asset_path_not_the_symbol); [1D bcast](device-programs.md#memory-feedback_ckir_1d_broadcast_aligns_first_axis_bias_needs_reshape); [⛔⛔ kernel vec3](device-programs.md#memory-feedback_ckir_kernel_eval_is_scalar_vec3_evaluates_to_garbage)
- [⛔⛔ ref=n<POS> id-ignored](device-programs.md#memory-feedback_ckir_node_refs_are_positional_n_index_id_ignored); [⛔⛔ ext=SECTION](device-programs.md#memory-feedback_ckir_text_ext_pool_needs_section_not_bare_key); [⛔⛔ decl DAG](device-programs.md#memory-feedback_ckir_emitter_decl_needs_dag_memo); [⛔⛔ hoist RAW](rendering.md#memory-feedback_ckir_emitter_hoist_must_not_lift_materialized_consumers_over_loop); [emitter wire](workflow-and-correctness.md#memory-feedback_compute_kernel_emitter_lacked_exp_pow); [KOp u](workflow-and-correctness.md#memory-feedback_raster_emitters_lag_compute_wire_both_and_uint_suffix); [⛔⛔ post op](workflow-and-correctness.md#memory-feedback_post_color_ops_must_be_vec4_robust_sampled_input); [⛔⛔ lower_entry](workflow-and-correctness.md#memory-feedback_lower_entry_must_root_every_entry_value_node_mesh_prim)

## fp arithmetic semantics
- ⛔⛔ **an f32-STORED output has an f32-QUANTIZATION floor (~6e-8 rel = 1 ULP) no f64 libm estimate can beat -- set a tolerance-gate tripwire above the STORAGE floor, not the arithmetic one (CEIR-31a-2b, 2026-09-06):** the audio.compressor gate compares an f32 bus (`got = (f64)bus[i]`, the kernel's real output) against an f64 `std::` closed-form reference. The advisor estimated the deviation at ~1e-13 (two libms + recurrence-vs-closed-form, all f64) and proposed a 1e-9 tripwire -- WRONG, because it assumed the comparison lived in f64. The output is f32, so `got` carries 0.5-1 ULP of f32 quantization (~6e-8 rel) BY CONSTRUCTION; the observed max was 5.78e-8, dominated by the f32 store, with the f64 libm difference (~1e-16) invisible beneath it. Fix: MEASURE the deviation, set the tripwire just above the f32 floor (2e-7 = ~3x the floor, ~50x under the 1e-5 contract) -- and state in the gate what it GATES (a wrong recurrence/curve/detector, VERIFIED to blow past it by a temporary `d=|L|` hack) vs what it does NOT (the sub-floor libm difference, irrelevant to the f32 output). ⛔ before writing a "headroom" figure, RUN the gate and read the real max -- an unmeasured multiplier is a source-vs-scoreboard lie. Family with sign-of-zero + commutative≠associative below: reason about the ARITHMETIC THE KERNEL ACTUALLY DOES (f32 store included), not the f64 algebra on paper.
- ⛔⛔ **sign-of-zero: a SUMMING node normalizes -0.0F -> +0.0F, so its reference must SUM not COPY (CEIR-31a-2a, 2026-09-06):** an audio source is rendered by `+=`-into-a-ZEROED bus (`mine[i] += samples[i]`, render_graph's model + the CEIR executor's), and `+0.0F + (-0.0F) == +0.0F` in IEEE -- so an input `-0.0F` sample (e.g. `-0.7F * sin(0) == -0.0F`) comes out `+0.0F`. A closed-form reference built by COPYING the source buffer (`expect[k] = in[k]`) WRONGLY preserves the `-0.0F` and diverges from the executor at exactly one byte (`first_diff` on the pre-roll boundary; memcmp `-0 != +0`). Fix: build the reference the way the kernel COMPUTES (call `apply_source` into a zeroed buffer), not the way the math is written. This normalize-to-+0.0 is now a load-bearing bit-exactness invariant (a "memcpy optimization" of the no-input source would break a future reference). Same family as commutative≠associative below: model the reference on the kernel's arithmetic, not the algebra.
- ⛔⛔ **commutative ≠ associative (CEIR-31a-1b-ii, 2026-09-06):** a 2-operand f32 sum has NO order — `a+b == b+a` BIT-EXACTLY for finite values (IEEE 754 addition is commutative); only ASSOCIATIVITY fails, and that needs ≥3 operands (`(a+b)+c != a+(b+c)`). So an "operand order == edge order" bit-exactness contract is REAL for a 3+-input sum but VACUOUS for a 2-input one — an edge-swap negative control on a 2-input `audio.mix` returned `0 != 0` (byte-IDENTICAL) and was the correct result of testing an incorrect claim. An order-sensitivity gate with teeth needs a ≥3-input fixture; for a 2-input node, test something that actually changes the output (e.g. a by-NAME binding swap) instead.

## CKIR eval
- [⛔⛔ CKIR fft fixed radix-2 needs build_fft1d_radix2 else eval HANGS](device-programs.md#memory-feedback_ckir_fft_batched_radix_dispatch_breaks_fixed_twiddle_contract); [⛔⛔ eval_cpu_kernel=SCALAR: LocalInvocationIndex; F32 tol 1e-6](workflow-and-correctness.md#memory-feedback_eval_cpu_kernel_is_scalar_use_localinvocationindex); [⛔⛔ ckir For bound=UNIFORM max + per-thread ForBreakIf](device-programs.md#memory-feedback_ckir_eval_for_bound_is_uniform_use_max_plus_forbreakif); [📁 numerical/DO](project-history.md#memory-project_technique_pointers)
- ⛔⛔ **a `.ckir` reading gate's ROUNDTRIP proves the file is CANONICAL, NOT that its cvals are the INTENDED values (CEIR-31b-1a-iii, 2026-09-06):** `ckir_roundtrip_diff(g,e)==−1` is `serialize(g)==serialize(parse(write(g)))` — a WRITE∘READ FIXED-POINT test on the parsed graph, it NEVER compares to the original file bytes. A `cval` is stored as an f64 (TOML hex bit-pattern, e.g. `0x4008000000000000`=3.0); ANY VALID f64 hex is a fixed point of write∘read, so a wrong-but-valid magic constant (`0x…00001` for `0x…00000`) round-trips CLEAN and passes every op-vocab tooth (op counts don't see values). The roundtrip catches only NON-canonical text (a non-shortest hex, a stray field, a wrong dtype token → write emits the canonical form, diff fails). ⛔ the advisor ITSELF asserted "the roundtrip pins the cvals" and CONCEDED when shown the helper — read `ckir_roundtrip_diff` before trusting a roundtrip to prove intent. THREE-TIER honesty for a hand-authored-cval kernel gate: roundtrip=CANONICAL · op-vocab teeth=STRUCTURE · NEITHER=the cvals/wiring are the hash you MEANT → that needs an INDEPENDENT eval-verified oracle (for a Vec4 kernel: a SEPARATE scalar-output COMPUTE oracle, since eval_cpu_kernel refuses Vec + refuses a Fragment FragCoord builtin) + a NODE-SEQUENCE match asserting the committed file embeds that same op/dtype/cval sequence. Family with the u32-wrap + attr-reader gate scars: a gate that "passes" is only as strong as what it actually compares. ✅ SOLVED + PRECEDENTED (31b-1a-iii part 2, 2026-09-06): the pattern is **oracle-for-a-hand-authored-asset** — a SCALAR COMPUTE oracle (inputs from a BUFFER, NOT a Fragment FragCoord builtin — eval_cpu_kernel is compute-scalar) built from a SHARED `emit_*` helper, eval_cpu_kernel-verified BIT-EXACT vs a C++ reference, KEPT permanently (a THIRD `.ckir`-oracle mode beside relu_vjp keep-as-regen + softmax bootstrap-then-DELETE — here there's no builder to delete, the asset is hand-authored). Link the committed file to the eval-verified oracle by a NODE+WIRING match: both N-node runs are contiguous in the same push order ⇒ map by a CONSTANT OFFSET (`committed_idx = oracle_idx + delta`), then per node compare op/dtype/cval AND operand WIRING — an intra-run operand must equal `oracle_operand + delta`, an external input must equal the committed input index (x↔x, y↔y). ⛔ op/dtype/cval ALONE misses a swapped-input / `A·A`-instead-of-`x·A` mis-wire that still emits — the wiring compare is what makes it airtight. And SCOPE the eval claim: eval==C++ is proven device-free; GPU==eval is a SEPARATE claim (emitter uint/ctype lowering) proven on-device.
- ⛔⛔ **eval_cpu_kernel is SCALAR and REFUSES every Vec node (Vec2/Vec3/VecComp/VecConcat/Swizzle/Splat/Dot/Cross ASSERT, ckir_kernel_eval.hpp ~line 293) → a FULLSCREEN Vec4 COLOR kernel CANNOT be CPU-eval'd (CEIR-31b-1, 2026-09-06):** the statement-tier evaluator is one f64 per node per lane — a vec has "nowhere to put a second/third component"; vector maths in a COMPUTE kernel is written COMPONENT-WISE on scalar nodes, and the vec3/vec4 forms are the RASTER tier where the EMITTERS lower them to native vectors. So a fullscreen fragment `.ckir` (TexSample→Mul→out at tkind=Vec trows=4, the rt_composite/visbuffer/velocity shape) has NO device-free NUMERIC gate. Its device-free gate is STRUCTURAL — `ckir_read`→`ckir_write` byte-exact ROUNDTRIP + `emit_fragment_glsl`/`emit_fragment_hlsl` smoke + an OP-VOCABULARY / tap-count check (a blur's TexSample count == its tap count) + `find_*_misuse==None` — and its NUMERICS are proven ON-DEVICE (the 31b-4 backend gate). ⛔ do NOT plan a "device-free CPU-eval gate per color kernel" (the audio-kernel-DIRECT pattern does NOT transfer to Vec4 fragment kernels — the audio kernels are scalar C++ [apply_*], not `.ckir` Vec nodes); the advisor itself tripped this in the 31b-1 plan (a CPU-eval path was ASSUMED from the audio pattern, not verified). Same scar's fragment face as [feedback_eval_cpu_kernel_is_scalar_use_localinvocationindex](workflow-and-correctness.md#memory-feedback_eval_cpu_kernel_is_scalar_use_localinvocationindex).


<!-- end-memory:scars_ckir_emitter_eval -->

<a id="memory-scars_gpu_device_dx12"></a>
## scars_gpu_device_dx12

---
name: scars_gpu_device_dx12
description: Scars for GPU device execution — dispatch binding caps, reflection, occupancy, ray-query/mesh-shader/AS→MS, feature-validation, MRT/indirect, DX12 register-packing / PSO-format / ClearRTV, and GLSL/HLSL portability — relocated out of MEMORY.md; open before "fixing" a device dispatch / DX12 / cross-backend symptom.
metadata: 
  node_type: memory
  type: reference
  originSessionId: cb9df3b8-2389-479b-9d99-d3d6ce3ba327
  modified: 2026-09-11T10:06:47.361Z
---

GPU device / DX12 / cross-backend-portability scars, moved out of the always-loaded `MEMORY.md` (CEIR-26z compaction,
2026-09-05). Recall on demand when the work touches device dispatch, RT/mesh shaders, DX12 register/PSO, or GLSL↔HLSL
portability. Read before "fixing" a matching symptom.

## Device dispatch / RT / mesh
- [desc cap=8](device-programs.md#memory-feedback_ckir_gpu_dispatch_binding_cap_and_sort_unroll_explosion); [reflect unopt](build-and-verification.md#memory-feedback_reflect_needs_unoptimized_spirv_and_compiler_injection); [⚠ occupancy](device-programs.md#memory-feedback_ckir_tiled_gemm_occupancy_not_free); [⛔⛔ rayQuery](device-programs.md#memory-feedback_ckir_rt_inline_rayquery_scars); [⛔⛔ mesh-shader](device-programs.md#memory-feedback_mesh_shader_device_scars); [⛔⛔ draw_mesh_s](workflow-and-correctness.md#memory-feedback_draw_mesh_storage_had_no_synchronous_path_both_backends); [feature⇒valid](device-programs.md#memory-feedback_shader_capability_needs_device_feature_run_validation); [⛔⛔ MRT/indirect](rendering.md#memory-feedback_draw_storage_mrt_needs_coherent_frame_graph_transients_not_standalone_targets)

## DX12 register-packing / PSO + GLSL/HLSL portability
- [⛔⛔ DXIL DECL](device-programs.md#memory-feedback_dx12_hlsl_svposition_last_register_packing); [⛔⛔ varying GAP](workflow-and-correctness.md#memory-feedback_dxil_varying_gap_register_packing); [⛔⛔ AS→MS](device-programs.md#memory-feedback_as_ms_payload_contract_dx12_pso); [⛔⛔ PSO fmt=RT](build-and-verification.md#memory-feedback_dx12_pso_format_must_match_rt); [⛔⛔ ClearRTV](rendering.md#memory-feedback_dx12_clearrendertargetview_uint_value_converts_not_bitcast)
- [GLSL write](device-programs.md#memory-feedback_glsl_writeonly_buffer_readback_portability); [HLSL VK](device-programs.md#memory-feedback_dx12_hlsl_masks_type_bugs_run_vulkan)
- [⛔⛔ DX12 first_vertex(StartVertexLocation) does NOT reach SV_VertexID on NVIDIA — ranged non-indexed storage-pull draw renders nothing; route per-draw offsets through the draw-table (identity IB → DrawIndexedInstanced, SV_VertexID=index value), fix at the backend seam](device-programs.md#memory-feedback_dx12_startvertexlocation_not_reaching_sv_vertexid_use_identity_ib)

## Samplers (address / bindless)
- [⛔⛔ DX12 bindless IGNORED pass sampler (heap-start=s0 WRAP, not active_sampler_slot); + SamplerAddress default=Repeat→post-process must declare clamp or it wraps opposite-edge content across the screen edge](device-programs.md#memory-feedback_dx12_bindless_run_ignored_pass_sampler_and_address_default_is_repeat)


<!-- end-memory:scars_gpu_device_dx12 -->

