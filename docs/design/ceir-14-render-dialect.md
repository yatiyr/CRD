# CEIR-14 — `ceir.render` dialect (design note / design-lock)

**Status:** DESIGN-LOCKED 2026-08-10 (advisor-consulted; three discriminating checks run). Implementation starts at
CEIR-14a next tick. Mirrors the CEIR-13 (compute/transfer) build pattern: dialect TOML → opgen → verifier → the
`crd-ceir-gpu` lowering → a device proof. → §40 §41 · PROOF §169.

## Band contract (from the tracker)

`ceir.render` (§40) on RAH's hardened model: **attachments are the RAH-1 typed `ColorAttachmentDesc`/`RenderingDesc`
model** — G-buffer / visbuffer / velocity / object-ID / primitive-ID / material-ID are **program-defined output
contracts over general typed attachments, NOT ops** (§41 — the RAH-1a.1 lesson generalized). Draw family
(draw / indexed / indirect / indirect-count / mesh-dispatch / indirect-mesh / patch-tess, §40); dynamic state
(viewport / scissor / shading-rate); resource tables per RAH-2 (§157). Lowers to the **same canonical command verbs
the frame graph records today** (`RenderingDesc` begin/end + the draw verbs). §40 is explicit: **no material / shadow /
deferred / G-buffer semantics in op NAMES** — combinations stay typed data/program composition.

**Scars pulled in (tracker):** indirect draws must push the DrawIndex row · a depth-only pass borrowing a color
program dies when the FS gains `discard` · the NDC±Y mirror on RTT-sampled passes · widen-`LoweredKind` ⇒ audit EVERY
consumer.

## Three discriminating checks (run 2026-08-10 — they decided the forks)

1. **opgen supports region-carrying ops — YES.** `REGION_KINDS = {graph, ssacfg}`, `regions` is an op-field;
   `core`/`async`/`task` TOMLs declare `regions = [ { kind = "graph" } ]` (e.g. `async.scope`, resultless). ⇒
   `render.scope` as a **region op** needs no opgen extension.
2. **`lower_region` walks ONE FLAT block** (`for op = block.first_op(); …; op = op->next_in_block()` — no recursion
   into op regions). ⇒ region-recursion in the lowering is **NEW machinery for 14b/14z** (when there are draws to
   lower); 14a stays device-free and does not touch the lowering.
3. **`AttrKind::Float` exists** (f64 interned by bit pattern — exact/NaN-stable). ⇒ typed clears use **Float attrs
   directly** (float4 = 4 Float attrs), no `kernel_interface`-style bit-pattern-in-int hack.

## Design-lock (the five forks, resolved)

1. **Scope shape → a region-carrying `render.scope`, NOT begin/end marker pairs.** §40's "begin/end render scope"
   names the *semantics*; the structured op IS begin/end (the lowering emits the begin/end verbs around the region
   body at 14b/z). Regions make ill-formed programs unrepresentable (a draw cannot float outside a scope; no
   unbalanced-pair verifier class) and give the draws a real containment edge (flat markers have no data edge, so
   effects alone would not stop a draw reordering across `begin`). RESULTLESS, like `compute.dispatch` /
   `async.scope`. `regions = [ { kind = "graph" } ]`.
2. **Attachments → OPERANDS via attachment-DECLARE ops producing a dialect attachment TYPE.** The targets MUST be
   operands (not attributes): the 13d per-resource barrier machinery + `resource_root` normalization only see
   OPERANDS — attribute-encoded targets are invisible to hazard derivation (re-earning the RTT/WAR scar family). The
   verified opgen constraint "only the LAST operand may be variadic" forbids separate variadic-color + fixed-depth +
   optional-shading-rate operand groups. So:
   - `render.color_attachment(%img) {load, store, clear_kind, clear_r/g/b/a | clear_uint, blend}` and
     `render.depth_attachment(%img) {load, store, clear_depth, read_only, compare}` → each PRODUCES a value of a new
     **attachment type class** (the 8a `register_type_class` machinery).
   - `render.scope(%attachments…) {width, height, sample_count}` consumes them through its single uniform variadic
     tail. **Role lives in the TYPE** (12a one-source-of-truth — no role string attr); per-attachment config gets
     verifier LOCALITY (no parallel-token mini-grammar the way `access` would need for §41's resolve/MSAA/RO-depth
     surface); hazard derivation sees through the attachment value to the image via the existing view→root
     normalization.
   - Fallback (only if a check had killed the above — it did not): one variadic image tail + `access`-style parallel
     token attrs. Recorded, not taken.
3. **14a scope → ops + verifier + structural tests, DEVICE-FREE** (the 13a precedent). Draws = 14b; region-recursion
   lowering = 14b/z; device pixel-proof = 14z. Because the machinery exists now, **pull the §121 text≡builder test
   into 14a from day one** (`print(parse(text)) == print(builder)` for a scope + attachment-declares) — no reason to
   defer it to 14z as band 13 had to. ⛔ cook Registrars must install the render dialect (the 13z-4 lesson).
4. **Effects → the compute precedent verbatim.** `render.scope`: `GPUCommand` + a conservative ambient
   `MemoryReadWrite` (more-hazards-never-fewer; narrowed later in lowering). Attachment-declare ops are **effect-free
   metadata** (the `resource.declare` shape). Per-operand static effects cannot index a variadic tail anyway.
5. **NO-FOLLOWs named in the TOML summary** (the compute-TOML "named, never silently subset" pattern), each to an
   owning row: multiview / view-selection, fragment-density/foveation, shading-rate attachment → 14b dynamic state;
   mip/layer/aspect attachment views → existing view ops / a named row; patch/tessellation draw → 14c/14z.

## `find_render_misuse` (the verifier — mirrors `find_dispatch_misuse`)

Module-walk verifier: attachment-declare operand is an image/view; `render.scope` operands are all
attachment-typed; region rule (**draw-family ops are legal ONLY inside a `render.scope` region**); depth/RO-depth
coherence; and — the RAH-1a.1 lesson lifted to IR — **typed-clear vs attachment-format consistency** (a `Uint` clear
only on a uint-format attachment). That statically checks 14z's "typed clears asserted per-target".

## Sub-slice sequencing (tracker rows)

- **14a** — `render.scope` region op + `render.color_attachment`/`render.depth_attachment` declare ops + the
  attachment type class + `find_render_misuse` + structural & §121-text tests. DEVICE-FREE, 4 configs. §41.
- **14b** — draw ops + dynamic state (viewport/scissor/shading-rate); the `crd-ceir-gpu` region-recursion lowering
  (`render.scope` → begin/end `RenderingDesc` + draw verbs); the widen-`LoweredKind` audit (FakeRec, `validate_lowered`,
  execute's total switch — every consumer, not just `-Werror=switch`). §40.
- **14c** — indirect + indirect-count + mesh/task dispatch; the DrawIndex scar as a lowering-level ctest. §40.
- **14d** — resource-table binding semantics over RAH-2's resident tables. §156 §157.
- **14z** — PROOF: triangle · MRT (typed clears asserted per-target) · depth-only · indexed-indirect(-count) · mesh
  dispatch — pixel-asserted on BOTH backends, ValidationCapture-silent. §169.

## CEIR-14b design-lock (draw ops + the region-recursion lowering) — 2026-08-10 (advisor + 4 checks)

**Four discriminating checks (run 2026-08-10):** (1) opgen `kernel_ref` IS parameterized by attr name
(`KERNEL_REF_FIELDS = {symbol, interface}`) ⇒ draws use `kernel_ref = {symbol="program", interface="program_interface"}`
for free dependency extraction + the §107 interface pin. (2) ⛔ the command model has **NO viewport/scissor verbs** ⇒
dynamic-state ops would have no execution landing at 14z (the cook-only-gates-ship scar) ⇒ **dynamic state
(viewport/scissor/shading-rate) is NAMED-FORWARD, NOT 14b** — this narrows 14b to the DRAW ops. (3) `execute_lowered` +
`validate_lowered` handle `Transfer` as `UnsupportedCommand` (execute.cpp) ⇒ the exact mirror for the render kinds.
(4) the grid const-fold (`groups_x`/`dynamic_grid`, lower.cpp:123) is reusable for draw counts.

**Architecture — ONE lowered list, widened `LoweredKind`, `execute_lowered` rejects render kinds TYPED** (the Transfer
precedent; a separate render-lowered list would fork the barrier machinery). The 14z RASTER executor (a different surface —
`IRasterEncoder`, NOT `IComputeContext`) consumes them through the frame-graph verbs (`RenderingDesc`/`GeometrySource`).

**⛔ THE LOAD-BEARING TRAP — the region-interior binding hazard hole.** The 13d barrier walk gathers conflicts from
OPERANDS on the flat block; a draw's resource bindings live INSIDE the scope's region, so the flat walk at the scope op
sees only the attachment operands. A dispatch that writes B, then a scope whose draw samples B, MUST still barrier — via
the scope's conservative ambient `MemoryReadWrite` (the hazards-against-everything baseline + the nullptr-resource →
all-bound-buffers replay) — but that path has NEVER been exercised by a region-carrying op. ⭐ **Non-negotiable 14b test
(device-free):** dispatch-writes-B → a scope whose draw binds B → assert a barrier lands before the scope. If the gather
misses it, that is a 13z-3-class correctness fix, and it is IN 14b's scope.

**Op shapes (the compute precedent):** separate `render.draw` (None/StoragePull geometry — vertex-pull is program
semantics) + `render.draw_indexed` (the distinction is the OP KIND); indirect/count/mesh stay 14c. Counts
(`vertex_count`/`index_count`, `instance_count`) are required index OPERANDS (const-fold into the lowered command;
dynamic → flag → `UnsupportedCommand`, the grid precedent; callers pass `arith.const 1`). `first_vertex`/`first_index` =
optional int ATTRS (the `GeometrySource` u32 fields). `draw_indexed` operand order: counts → `index_buffer` (a static
`MemoryRead` on it, the dispatch_indirect args precedent) → variadic bindings; `access` string over the variadic tail.
`kernel_ref = {symbol="program", interface="program_interface"}`. ⛔ draws are RESULTLESS with `GPUCommand` + ambient
`MemoryReadWrite` (two alpha-blended draws to one attachment must never reorder — pairwise hazards pin raster order).

**Verifier extensions** (append to `find_render_misuse`, its check order is contractual — extend, don't reorder;
`scan_render` grows an `in_scope` context param): `DrawOutsideScope` (a draw-family op outside a render.scope region),
`NestedRenderScope` (begin-inside-begin is illegal in Vulkan/D3D12 though structurally permitted), `ComputeInRenderScope`
(a `compute.dispatch` / non-render GPUCommand op inside a scope region is illegal — pure value ops + structured CF stay
legal), `ProgramNotSymbol` (the `KernelNotSymbol` analog — identity before contract), `DrawCountNotIndex`, draw `access`
arity. Misuse-kind enum APPENDS at end (the widen-enum discipline).

**Lowering:** `BeginRender`/`Draw`/`EndRender` (+ nothing else — dynamic state is named-forward) APPENDED at end of
`LoweredKind`; each carries the source `Operation*` + folded consts (the Dispatch pattern — do NOT duplicate
`GeometrySource` into `LoweredCommand`; the 14z executor materializes it from the op). Recursion: on `render.scope`, emit
`BeginRender` → lower the region body in order → `EndRender`. **Barriers stay at SCOPE granularity** (raster order +
blending own intra-pass ordering; the scope's ambient covers scope-vs-neighbors — verified by the trap test). Per-attachment
effect narrowing = the 13d-narrowing analog, named-forward.

**Sequencing (3 ticks, the 14a build-up):** (i) the draws TOML + verifier extension + structural/text tests, device-free;
(ii) the lowering (recursion + widen) + the FULL consumer audit (`execute`'s switches [GCC `-Werror=switch`], `validate_lowered`,
`FakeRec`, `execute_lowered_cpu` in the shared harness — a render kind reaching the CPU reference must ERROR typed, not skip;
rebuild all 3 GPU test targets on BOTH OSes — the harness header changes) + ⭐ the hazard-hole test; (iii) the 4-config gate.
⛔ ASCII test names (the `§` scar was re-hit at 14a — the rule rides `test_render.cpp`'s header now).

## DoD / mechanics (each slice)

opgen `--check` + validator (`test_opgen.py` gains render entries) after generating · new test sources listed
explicitly in `tests/ceir/CMakeLists.txt` · ASCII test names · LLVM-20 tidy per touched file · GCC `-Werror=switch` ·
`crd-ceir-invariants` (I3/I4/I5 — crd-ceir core stays host-only/jobs-free/asset-free; the lowering + tests are the
bridge/test side) · the 4-config gate (win-debug + win-asan + linux-gcc-debug + linux-gcc-asan).
