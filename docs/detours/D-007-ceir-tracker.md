# D-007 · CEIR — THE MASTER TICK TRACKER

> **THE single live tracker for all active work** (user-directed 2026-08-07: CEIR is the master spine; strict band
> order; RAH-1/2 parallel; everything else paused). One row per sub-slice; first column is the tick:
> `✅` done · `◧` in progress · `⬜` todo · `◀ NEXT` marks the front. **A row ticks ✅ ONLY at its GATE** — never
> because code or a "head" exists (the D-007 house rule).
>
> ### The three documents, and which one wins
> | Document | Role | When you read it |
> |---|---|---|
> | **Mission constitution** — [`../research/2026-08-07-ceir-universal-programming-master-roadmap.md`](../research/2026-08-07-ceir-universal-programming-master-roadmap.md) (§0–§185) | **THE LAW.** The full architecture, every contract, every rule. | Read the §§ a row cites **before implementing that row.** |
> | **This tracker** | THE INDEX + tick state. Summarizes each slice and cites its §§; never restates the law. | To see what's next and what's done. |
> | **Per-band design spec** — `docs/design/ceir-<band>-<slug>.md` (written at band OPEN) | The reuse audit (what already exists, file:line) + sequenced increments + per-increment gate. The house convention (see `docs/design/README.md`). | When you were told "read the slice and implement it." |
> | **Landed history** — [`D-007-gpu-program-system.md`](D-007-gpu-program-system.md) | The CKIR/RAF/RET ledger + the pre-CEIR post-RAF plan. Its live-tracking role **MOVED here at CEIR-0f (2026-08-08)** — that doc's master table is now historical. | For "how did we get here." |
>
> **Citation convention:** `§NN` = section NN of the mission constitution. Its headings are `# NN. TITLE`, so
> searching `# 39.` jumps to §39. A row's `→ §§` list is the *complete* set of sections that define its contract.

---

# Architecture in one screen — READ THIS BEFORE THE ROWS

**The one sentence (§1 north star).** Every reusable algorithm expressible from capabilities Cerid *already*
understands must be a **versioned, inspectable, serializable, hot-reloadable program asset** — whether it does
rendering, GPU compute, ray tracing, ML/AI, scientific computing, media, geometry, UI, physics, animation, audio,
or application logic. New native C++ is required **only** to introduce a genuinely new *host capability / OS
integration / device provider / hardware primitive* — never merely because someone invented a new *algorithm* from
existing capabilities.

**The mantra (§1).**
```
ALGORITHMS ARE PROGRAM ASSETS.   CAPABILITIES ARE NATIVE PRIMITIVES.
COMPILERS CHOOSE LOWERINGS.      BACKENDS EXECUTE.
```

**The stack (§3) — five layers, one-way lowering.**
```
AUTHORING   text · CR-D007 visual · TOML/domain frontends · C++ builder · importers      §9 §11 §121 §124
      │     (all projections emit the SAME canonical IR — there is no privileged path)     §121 §166
      ▼
CHIR        Cerid High-level IR — future language layer (modules, generics, ADTs,          §4  (design-only now;
      │     closures, traits, ownership, async/state/events). Lowers/erases into CEIR.          implemented CEIR-29)
      ▼
CEIR        Cerid Execution IR — typed SSA + graph/CFG/structured regions; resources,       §2 §12 §13
      │     effects, capabilities, evaluation domains; ~30 open-world dialects (§33–§68).
      ├───────────────────────────┐
      ▼                           ▼
CKIR (per-invocation kernel math,  EXECUTION PROVIDERS (CPU/GPU/NPU/media/remote —          §69 §85
 the shipped shader/kernel IR)      native graph/program APIs; advertise ops/costs)
      └───────────────┬───────────┘
                      ▼
COMPILED EXECUTION PLAN  →  Vulkan · D3D12 · CUDA · HIP · Metal · WebGPU · CPU/SIMD/jobs     §144 §153
                            · codecs · NPU · remote nodes
```

**The three IRs, crisply.**
- **CKIR** *(exists, 100% of the engine, bit-exact)* — **what one GPU invocation computes.** Per-thread shader/
  kernel math. Unchanged by this detour; CEIR references CKIR programs by asset identity and may *generate* CKIR
  during lowering (§85 §86).
- **CEIR** *(this detour)* — **what work exists, what data/resources flow, what controls execution, where work can
  run, how work generates more work, how resources synchronize, and which lower program implements each op** (§2).
- **CHIR** *(design-only now, §4)* — **source-language semantics** (generics, ADTs, closures, ownership, reflection)
  that must NOT be baked into an execution IR. Lowers/erases into CEIR. Implemented at CEIR-29, after the CEIR
  corpus exists to teach the language its ergonomics.

**Why it is NOT a giant enum (§6 §7).** Completeness comes from **open-world extensibility**, not a finite opcode
list. CEIR is MLIR-shaped: `Operation` + typed operands/results + attributes + **regions** + **dialects**
(registered op sets) + **interfaces/traits** — analyses dispatch on interfaces, never `switch(op.kind)`. A new
algorithm is a new *asset*; a new abstraction is a CEIR function or a custom op that *lowers* to existing ops
(§101 Level A/B); only a genuinely new hardware/host primitive is native C++ (§100, §101 Level C). ⛔ A giant
`enum CeirOp { Draw, Dispatch, ForwardPlus, Deferred, Nanite, Lumen … }` is the FORBIDDEN destination — it repeats
the exact `FramePassKind` combinatorial mistake RAF-12 just deleted.

**What stays native FOREVER (§177):** backend API calls, OS/window, filesystem/network impl, device enumeration,
swapchain acquire/present, vendor compiler invocation, hardware codec/NPU providers, native sensors, the job-system
impl, the compiler/runtime itself. **What must NOT stay native-only (§178):** renderer architectures, culling,
lighting, post-processing, GI, RT workflows, compute chains, numerical workflows, tensor/ML graphs, media chains,
UI effect graphs, geometry pipelines.

**Visual = a projection, not a runtime (§9 §10 §166).** Visual and textual authoring are two lenses on ONE typed
semantic program. Graph coordinates/edges are UI; canonical semantics are operations/values/regions/symbols. A
user function with a typed interface automatically becomes a node (§11). Lenses: dataflow · structured control-flow
· frame/workflow · state-machine · reactive/event · timeline (§9).

### Where the architecture is defined — the jump table
| To understand… | Research §§ |
|---|---|
| North star + mantra · CEIR vs CKIR vs CHIR · the stack | §1 · §2 §4 §85 · §3 |
| Open-world / no-enum / dialects · table-driven op defs (ODS) | §6 §7 §101 · §8 |
| Visual = source projection · lenses · text/graph round-trip · user-fn→node | §9 · §9 · §10 §166 · §11 |
| Typed SSA · regions (graph/CFG/structured) · complete control flow | §12 · §13 · §14 |
| Evaluation domains + partial evaluation | §15 §73 |
| Types · units · semantic-domain types · ownership · explicit state · shapes · layout | §16 · §17 · §18 · §19 · §20 · §21 · §22 |
| Resource types · memory domains · residency/streaming | §23 · §24 · §25 |
| Effects · determinism · numerical semantics · error model · concurrency | §26 · §27 · §28 · §29 · §30 |
| Jobs/fiber integration · real-time domains | §31 · §32 |
| The ~30 dialects (`ceir.*`) | §33–§68 |
| Providers · native graph backends · provider partitioning · sharding | §69 · §70 · §102 · §103 |
| Optimizer (partial-eval/DCE/CSE/fusion/split/memory) · transform/rewrite · autotune/PGO/cost | §73–§78 · §71 §72 §146 · §80 §81 §82 |
| CPU/tiered host execution · CKIR integration · CKIR frontier features | §83 §84 · §85 · §86 |
| Expressiveness corpora (renderer/shadow/GI/RT/vgeo/compute/hesap/ML/media/UI/audio) | §87–§97 |
| Capability security · native intrinsics · custom dialect/plugin levels | §99 · §100 · §101 |
| Asset model · deps · interface hash · hot reload · state migration · incremental | §105 · §106 · §107 · §108 · §109 · §110 |
| Provenance · debugger · profiler · capture/replay | §111 · §112 · §113 · §114 |
| Validation layers · race detection · certification hooks | §115 · §116 · §117 |
| Reference executor · fuzz/differential · transformation correctness | §118 · §119 · §120 |
| Programmatic API · CLI/RPC/MCP · source control/diff/merge · import/export | §121 · §122 · §123 · §124 |
| Material/technique relationship · command-model & framegraph relationship | §125 · §158 §159 |
| Migrations: framegraph · executor | §126 · §127 |
| The PROOFS (scene.raster→script) | §128–§143 |
| Compiler pipeline · multi-level IR snapshots · frontier transform schedules | §144 · §145 · §146 |
| Capability system · required/preferred/optional · quality tiers | §147 · §148 · §149 |
| Hot-path rules · editor-mode richness · specialization | §153 · §154 · §155 |
| Resource binding model · global resource tables | §156 · §157 |
| Editor views · node UX · large-graph scalability | §163 · §164 · §165 |
| Test matrices (IR/compiler/render/compute/tensor-ML/hot-reload) | §167–§172 |
| Maturity model (L0–L8) · machine-readable manifest | §173 · §174 |
| The 33 implementation bands · pause policy · native-forever vs must-not | §175 · §176 · §177 §178 |
| Definitions of Done (universality/visual/scripting/GPU) | §179 · §180 · §181 · §182 |
| Reference reading (MLIR/IREE/XLA/Triton/DX/Vulkan/Slang) | §183 |

**House rules on EVERY row** (not repeated per row): crd containers only (`IAllocator` ctor arg, no std) ·
serialization is FIELD-BY-FIELD, never raw-POD memcpy (the padding/content-hash scar) · ASCII test names · run
`ctest`, never the bare binary · tidy per touched file · GPU gates = both backends + `crd::gpu::ValidationCapture` ·
parallel results carry the `{1..16}` determinism moat · Allman / 4-space / 120-col.

**Naming note:** the CEIR-1 C++ type names are **FINALIZED by ADR-0109 §5** (`crd::ceir::{Context, Module,
Operation, Value, Block, Region, SymbolTable, Type, Dialect, OpId, SourceLoc, KernelRef, IExecutionProvider,
ModuleBuilder}`) — no longer a working proposal. Later-band op/dialect names remain proposals until their band opens.

**Row-refinement rule:** CEIR-0…13 carry full sub-slice rows now; CEIR-14…32 carry one gated row each and split
into sub-slices at band OPEN, sized by CEIR-0z's estimates (far-band detail is not invented before the inventory).

**Pause policy (§176):** until CEIR-13z, allowed work = bug fixes · unsafe half-migration completion · CKIR fixes ·
tests · docs · RAH-1/2 · CEIR slices. Parked work is tabled at the bottom so nothing is silently dropped.

---

## Parallel track — RAH (the binding/attachment vocabulary CEIR-9/-11 lower onto; → §156 §157 §41)

| ✔ | Slice | Contract (detail: D-007 RAH-1 row + `docs/systems/rah-0-canonical-model-audit.md`) | → §§ |
|---|---|---|---|
| ◧ | RAH-0 | Canonical-model audit — design note drafted, **pending user review** | §156 |
| ✅ | RAH-1a.1 | Visbuffer fold — `ColorAttachmentDesc` typed clear (`clear_kind` Float/Uint + `clear_uint`, appended at struct end); `RenderingDesc.visbuffer`/`clear_id` DELETED; encoder derives id-write structurally. Gate: REN-38-F6 97 asserts both backends + gpu-context-vulkan visbuffer 16 | §41 |
| ⬜ ◀ NEXT | RAH-1a.2 | DELETE the legacy G-buffer mechanic: `IGBufferTarget` + `draw_gbuffer` + `create_gbuffer_target` (both backends) + `RenderingDesc.gbuffer`; migrate ~8 test sites (`test_dx12_raster` ×4 · `test_vulkan_context` ×4 · `enc_draw_gbuffer` helper · `test_scene_render` mock · the two frame-graph tests · `test_ckir_hair_geom`) to the typed `color`-span MRT path (live deferred already uses `color1..3`). ⚠ Prereq: plain-vertex-MRT-into-color-span path + regular-target readback (RAH-5 seam). Proves §41's "G-buffer is not a canonical concept — it is a program-defined output contract over general attachments." | §41 |
| ⬜ | RAH-1a-close | Frame-blob byte-identity + full no-loss sweep (RAH-0 no-loss table: only `read_pixel` host-readback *moves* → staging transfer), both backends | §156 |
| ⬜ | RAH-2 | Complete resource-view model + resident global resource tables (retires fixed `input0..7`/`storage0..3` slot arrays + the desc cap-8 escape hatch — the concrete scar). **This IS §157's resource-table semantics; co-design the descriptor-buffer/heap strategy with CEIR-9d.** Unblocks I2D later. | §156 §157 |

---

## CEIR-0 — Repository inventory + architecture ADRs — ⬜ ◀ NEXT (opens after the hygiene-pass commit)  → §175 band 0

**Band contract (§175 CEIR-0).** No code. Produce the ground-truth map of every execution-program representation in
the tree (from CODE, §127), the three ADR drafts + the CHIR design note, and restructure D-007 so exactly one live
tracker exists. Close = the mission doc's **§184 fifteen-item report**, answered from evidence, plus honest KLOC/
duration sizing for CEIR-1…13. **Gate: no ambiguous ownership remains.**

| ✔ | Slice | Deliverable | Gate | → §§ |
|---|---|---|---|---|
| ✅ | CEIR-0a | **Execution-path inventory FROM CODE** — [`docs/design/ceir-0a-execution-path-inventory.md`](../design/ceir-0a-execution-path-inventory.md). Every execution-program representation inventoried + classified with file:line evidence (the 14 executors + their `record_*` lowerings, the ONE frame-graph runtime [verified from code], `scene_renderer.cpp`'s 9-block `render()` composite catalog + program hand-list, `IComputeContext` consumers [CKIR backend + bvh-gpu 24 sites + 258 test occ.], the five cookers, `.crdr`, draw, media). **Headline: RAF already did the atomic-vs-composite split** — CEIR is a promotion, not a rewrite (executors = atomic verbs but mechanically pre-§6/§8; frame graph = composite sequencer → `ceir.frame`; scene_renderer's C++ orchestration = the real CEIR-13 target; `visbuffer.raster` = a residual special-case CEIR-11a dissolves). CEIR-13 sizing now concrete: 9 `render()` blocks + hand-list, not a 6610-line file. | ✅ table w/ file:line evidence; zero "per the docs"; §9 empty | §127 §128 |
| ✅ | CEIR-0b | **Scripting-direction ADR** — [`docs/decisions/0108-…md`](../decisions/0108-ceir-owned-language-stack-supersedes-cpp-only-scripting.md) (ADR-0108) — **ACCEPTED 2026-08-07 (user-approved).** **Surgically** supersedes ADR-0081 §9 ONLY (C++-is-ONLY clause); §1-§8 (agent-native CLI/RPC/MCP) reaffirmed. C++ stays first-class native + hot-reload; Cerid gains an OWNED textual/visual language stack; no Lua/Python/JS runtime; CLI/RPC/MCP stays source-of-truth; program assets agent-authorable. Non-negotiables pinned (§98/§99): no mandatory GC in hot paths · deterministic time/RNG · capability security · C++ FFI. ⚠ **DEFERRED ACTION (do NOT forget):** the cornerstone flip (PRINCIPLES/AGENTS/README/ROADMAP) + the ADR-0081 §9 in-file strike land as ONE coordinated commit at the **first CEIR vertical slice** (§5 gate 2; ≈ CEIR-10z/CEIR-13). | ✅ ADR accepted | §5 §98 §99 |
| ✅ | CEIR-0c | **CEIR/CHIR/CKIR ownership ADR** — [`docs/decisions/0109-…md`](../decisions/0109-ceir-chir-ckir-ownership-and-module-placement.md) (ADR-0109) — ✅ **ACCEPTED.** Layer contract (CEIR=orchestration; CKIR=kernels, unchanged, referenced by `KernelRef` identity; CHIR=reserved → CEIR-29); one-way lowering; **`engine/ceir` (crd-ceir) host-only, deps core/log/memory/containers/units ONLY** — acyclic by construction; GPU/jobs via **dependency inversion**: abstract `IExecutionProvider` in crd-ceir, impls in bridge modules **`crd-ceir-host`** (→jobs) + **`crd-ceir-gpu`** (→gpu-context/render-graph/kir, where CEIR-0a `record_*`+CKIR-compile STAY). Invariants I3/I4/I5 (extend ADR-0103 I1/I2). Finalized CEIR-1 names + §10 semantic-identity model. **Gates CEIR-1.** | ✅ **ACCEPTED 2026-08-07** (gates CEIR-1; 0d/0e can draft in parallel) | §2 §4 §85 §10 §69 |
| ✅ | CEIR-0d | **Native-intrinsic ADR** — [`docs/decisions/0110-…md`](../decisions/0110-native-intrinsic-schema-and-plugin-levels.md) (ADR-0110) — ✅ **ACCEPTED.** An intrinsic = an ordinary CEIR-2 op + §100 native-binding metadata + a bridge handler (`IntrinsicRegistry` in crd-ceir, ADR-0109). Legitimacy IFF test (capability=intrinsic · algorithm=program · **composable-but-slow=provider, never Level C**); plugin levels A/B/C. Classifies the CEIR-0a atomic set (`present`/codecs/resolvers ✅; `submit_overlay`→`ceir.render` @ CEIR-11). Registry lands after CEIR-4; first shipped `present` @ CEIR-12. | ✅ **ACCEPTED 2026-08-07** (gates CEIR-2a field set + CEIR-13c; NOT critical path) | §100 §101 |
| ✅ | CEIR-0e | **CHIR-0 language design NOTE** (not an ADR; design-only) — [`docs/design/ceir-0e-chir-0-language-design-note.md`](../design/ceir-0e-chir-0-language-design-note.md) — ✅ **ACCEPTED; ZERO implementation.** §98 feature set (erased-vs-lowered-vs-runtime); ownership options weighed → **leaning: values + generational handles + arenas + state stores + a LIGHT borrow (NOT a full checker)**, tiebreak = visual+agent authorability; syntax sketches; text/visual projection (shares ADR-0109 §6 identity model); all binding decisions deferred to the CEIR-29 ADR against the corpus. | ✅ **ACCEPTED 2026-08-07** (design note; decides nothing; NOT critical path) | §4 §98 §19 §10 |
| ✅ | CEIR-0f | **D-007 restructure DONE (2026-08-08):** CEIR-spine section added to `D-007-gpu-program-system.md`; the old master table re-hung under the CEIR bands (banner: RPL→CEIR-15 · MLR→CEIR-21 · frame/executor→CEIR-12/13 · I2D→CEIR-28 · hesap-GPU→CEIR-19 · D7E→CEIR-30, marked "do NOT tick live"); **THIS file wired as the one live tracker** (context.md + ROADMAP pointers moved here); the **ADR-0106 supersession PLAN written** (strike executes @ CEIR-12f); **§PR-3's maturity ladder struck-in-place** → §173 + the registry header. All additive (D-007 +41 lines, zero content deleted). | ✅ exactly one live tracker; no row tracked twice; no third live maturity ladder | §175 §159 |
| ✅ | CEIR-0g | **Maturity merge + §174 manifest** — [`docs/design/ceir-0g-maturity-and-manifest.md`](../design/ceir-0g-maturity-and-manifest.md) — ✅ **ACCEPTED.** Finding: the two ladders measure DIFFERENT axes (RAF-asset maturity vs CEIR-program maturity) — merge ≠ renumber. ONE forward model = CEIR L0–L8; a **two-axis transition** (`raf_level` today's reality + `ceir_level` forward track, both honest; per-class §PR-4: A/A+R/A+E converge, B keeps both, T = n/a). §174 manifest adds `providers` + `determinism_tier`; registry-migration plan (schema 1→2 now, →3 at 13z). §PR-3 supersession is a CEIR-0f action. | ✅ **ACCEPTED 2026-08-08** | §173 §174 |
| ✅ | CEIR-0h | **Migration + DELETION ledger** — [`docs/design/ceir-0h-migration-and-deletion-tables.md`](../design/ceir-0h-migration-and-deletion-tables.md) — ✅ **ACCEPTED.** Built FROM CEIR-0a (not re-derived): what PROMOTES (14 verbs, runtime, cookers, CKIR, IComputeContext) vs DELETES (composite C++ orchestration + duplicate/privileged paths). Every deletion names its parity gate FIRST: frame-path F1–3 @ CEIR-12f · orchestration E1–5 @ CEIR-13z (E1 = the program-variant ladder = the §128 proof) · residual R1–2 (visbuffer/overlay) @ CEIR-11 · §PR-3 supersession @ 0f. CEIR-31 executes it verbatim. | ✅ **ACCEPTED 2026-08-08** | §126 §127 §178 |
| ✅ | CEIR-0z | **CEIR-0 close report + sizing** — [`docs/design/ceir-0z-close-report-and-sizing.md`](../design/ceir-0z-close-report-and-sizing.md) — ✅ **ACCEPTED.** The §184 fifteen-item report (each answered from 0a–0h evidence). Honest DERIVED sizing: CEIR-1…13 ≈ **34–55 KLOC** (in-tree anchors + per-band confidence; the four Low bands 10–13 dominate) · **~4–8 mo dark period** (very-low confidence, banded on RAH-parallelism + no-band-2/4-redesign). 5 unresolved design Qs docketed to their bands. ⚠ item 4 finalizes when 0f executes. | ✅ **ACCEPTED 2026-08-08** (15 answered) | §184 |

## CEIR-1 — Core IR substrate — ✅ CLOSED 2026-08-08 (1a–1z) → §175 band 1 · core: §12 §7

> **BAND 1 CLOSED.** The full host-only IR substrate: identity + the in-arena def-use graph (1a) · SymbolTable +
> `ceir.func` (1b) · interned typed attributes + source map (1c) · open-world dialect registry + traits/interfaces +
> verifier (1d) · deterministic textual printer + parser (1e) · FourCC-chunked binary serialization (1f) ·
> `ModuleBuilder` fluent API (1g) · the round-trip fuzz + malformed corpus + stable-hash harness (1h) · the
> hello-world band gate (1z). `tests/ceir` **49/49** (from 7 at 1a) + a 7/7 `crd::memory::GrowableLinearAllocator`
> gate. Every slice gated on **win-debug + win-asan + linux-gcc-debug + linux-gcc-asan + LLVM-20 tidy** +
> `crd-ceir-invariants` (I3/I5/I6). Headline finding: the **1h fuzz caught two real OOM crashes** in loaders that had
> already passed four slices of unit gates. **NEXT: user commits the batch → GitHub CI green → CEIR-2.**

**Band contract.** New module `engine/ceir` — the in-memory IR + its two serial forms. Working shape (finalized in
0c): `crd::ceir::Context` (owns dialect registry + interned types/attrs/strings), `Module`, `Op` (interned
`(dialect,name)` id + `ValueId` operand/result spans + attr map + region list, **arena-allocated** per §153),
`Value` (typed, def-use chain), `Block`, `Region{Graph|SsaCfg}` (§13), `SymbolTable`. Textual form is MLIR-shaped
(`%v = dialect.op(%a,%b) {attr=…} : type`), printed DETERMINISTICALLY. Binary form is FourCC-chunked like CRDR,
versioned from byte one (§104), field-by-field (⛔ padding scar). **Reuse:** `crd::containers` + TLSF/arena; FNV-1a
interning; the CRDR chunk pattern. **Gate: typed hello-world round-trips text/binary/builder (§167).**

| ✔ | Slice | Deliverable | → §§ |
|---|---|---|---|
| ✅ | CEIR-1a | ✅ **CLOSED (4-config sweep PASS, 2026-08-08):** new module `engine/ceir` (`crd-ceir`, host-only, deps core/log/memory/containers/units per ADR-0109) — `Context`/`Module`/`Operation`/`Value`/`Block`/`Region` + **intrusive in-arena def-use** (MLIR-shape `Use`; RAUW = splice, zero-alloc) + O(1) intrusive op/block edit + `SourceLoc` field from day one. The growable bump **arena is `crd::memory::GrowableLinearAllocator`** — it lives in **crd-memory** (its proper home; the growable sibling of `LinearAllocator`), NOT a crd-ceir-private `detail/Arena`, with its own **7/7 boundary-adversary gate** in `tests/memory` (chunk growth · oversized-own-chunk · exact fill-to-tail · reset-reuse · no-per-alloc-malloc-within-a-chunk · dealloc-noop · reallocate-copies · destructor-frees-every-chunk). `tests/ceir` **7/7** incl. the ⭐ **no-per-op-malloc gate** (32 ops, 0 parent allocs) + def-use/RAUW/set_operand/erase/insert_before/boundaries. **I3/I5 grep-gates** — `crd-ceir-invariants` ctest (`scripts/check_ceir_invariants.{ps1,sh}`, both OS blocks): I5 link-edge whitelist, I3 no forbidden-module include NOR shading-lang name; **proven to bite** (negative fixture → exit 1). **Gate: `per-slice-check.ps1` PASS all four — win-debug + win-asan + win-shipping(LTCG) + win-tidy, zero failures.** En route the sweep peeled **7 pre-existing cross-band blockers** (RAF/REN/CKIR bands had never passed shipping-LTCG/asan-complete/tidy) — all fixed gold-standard, none quarantined: frame_asset bare-scalar; RAF-10 `catch_discover_tests` ENVIRONMENT split; **DX12 + Vulkan RT pipeline-cache keyed by pointer/handle → content-hash** (real engine bug, DX12 flake 200/200 after); AS-4 CUDA autotuner timing guarded under ASan; C4743 LTCG ODR = stale-obj (build/win-shipping wiped); 37 clang-tidy errors across 12 files. See session log 2026-08-08. | §12 §13 |
| ✅ | CEIR-1b | ✅ **CLOSED 2026-08-08.** `SymbolTable` (per-Module, arena-backed `crd::containers::HashMap<StringView,SymbolEntry>` with an explicit `SvHash`; `define` rejects duplicates, no silent overwrite) + `Visibility{Public,Private,Nested}` (a real typed field — `symbol_table.hpp`). The **`ceir.func`** dialect (`func.hpp`/`func.cpp`, `crd::ceir::func`) built ENTIRELY on the generic Context factories (open-world; core never switches on func): `create_func` (func.func + body Region + entry-Block params, registered in the module table), `create_return` (terminator over the params), `create_call` (callee recorded via a Context symbol-ref side-table — the CEIR-1c-replaced interim for a `SymbolRefAttr`), `resolve_call` (lazy, **cross-module by name** — resolve a call against any module's table). `tests/ceir` **12/12** (7 core + 5 func: define/lookup/visibility, duplicate-reject + empty-name, body+params+return with def-use, call resolves, cross-module resolve/unresolved) + `crd-ceir-invariants` green (I3/I5 held). Tidy-clean (7 files). **Gate:** crd-ceir-tests PASS **all four configs** (debug/asan/shipping-LTCG/tidy) — a COMPLETE gate since crd-ceir has **zero downstream consumers** (grep-proven), so a full-tree sweep adds no signal for a leaf-module change (re-earns its keep at the band close / when the crd-ceir-host/gpu bridges land). ⚠ scoped-gate rationale flagged for user. | §34 |
| ✅ | CEIR-1c | ✅ **CLOSED 2026-08-08.** **Interned typed attribute values** (`attr.hpp`: `AttrValue` = tagged union over Int/Float(bit-exact)/Bool/String/SymbolRef/Type + `AttrId`; the Context dedups identical values → one `AttrId`, so equality is a u32 compare). **Per-op AttrDict** — `Operation` gains an arena `NamedAttr[]` (interned name→AttrId), grow-by-rebuild (operand-grow policy); read via `op->attr(name)`/`has_attr`/`num_attrs`, set via `Context::set_attr` (overwrite-in-place). **Source map / provenance (§111)** — `Context::register_file`→stable dedup'd `file_id`, `file_path`; every op carries `SourceLoc{file_id,line,col}` from day one (the field reserved in 1a is now *fed* — no retrofit). **Dissolved the CEIR-1b interim**: `func.call`'s callee is now a real **`SymbolRef` attribute** (`Context::m_symbol_refs` deleted; `resolve_call` reads `op->attr("callee")`). `tests/ceir` **17/17** (+guard = 18: attr dedup/kind-round-trip, dict set/overwrite/lookup, SymbolRef-callee resolves, source-map dedup + SourceLoc round-trip). Tidy-clean (7 files). **Gate:** crd-ceir-tests PASS all four configs (debug/asan/shipping-LTCG/tidy) + crd-ceir-invariants — scoped-complete (crd-ceir still has zero downstream). | §111 |
| ✅ | CEIR-1d | ✅ **CLOSED 2026-08-08.** **Dialect registry** (`dialect.hpp`/`dialect.cpp`): `Context::register_dialect(name)` → `Dialect`; `Dialect::register_op(op, traits, verifier)` interns "dialect.op" + records an **`OpInfo`** (ODS-lite: kind/name/dialect/traits/verify + interface list; printer/parser hooks reserved for 1e). **Traits** (`OpTrait` flags — Terminator/Symbol/SymbolTable/Pure/IsolatedFromAbove) via `has_trait`; **verifier** dispatch via `Context::verify` (opaque/unknown ⇒ valid); **interfaces** via `intern_interface`/`register_interface`/`get_interface` (an analysis dispatches through a registered function-table, NEVER a switch). **Open-world proven:** an UNREGISTERED-dialect op is a first-class Operation (dialect_of=null, has_trait=false, verify=valid). The `func` dialect self-registers (`func::register_dialect`: func.func=Symbol, func.return=Terminator+verifier). **⛔ I6 grep-gate** added to `crd-ceir-invariants` (both OS): forbids `switch (op.kind())` — **proven to bite** on a negative fixture. Shared `detail::StringViewHash` (DRY'd symbol_table). `tests/ceir` **22/22** (+5 dialect: registry/traits, opaque-unknown, verifier, interface-dispatch). Tidy-clean (10 files). **Gate:** crd-ceir-tests PASS all 4 configs + crd-ceir-invariants (I3/I5/I6) — scoped-complete (zero downstream). | §6 §7 §101 |
| ✅ | CEIR-1e | ✅ **CLOSED 2026-08-08.** **Deterministic textual PRINTER** (`print.hpp`/`print.cpp`): IR → canonical MLIR-flavored text (`module { ^bb0(%0 : !t1): %1 = dialect.op(%0, %0) {attr = val, …} : !t1 <nested {region}> }`); SSA values numbered by a fixed **pre-order walk** (block-args → op-results → recurse op-regions), attributes emitted **sorted by name** → the same semantic graph prints **byte-identical**; floats via `std::to_chars` (shortest round-trippable) with an enforced `.`/`e` marker so `4.0` never re-reads as an int; unknown-dialect ops print opaquely by their interned `dialect.op`; **NO layout emitted** — the text is semantics only (§10; coords/edges are UI, regenerated). **Recursive-descent PARSER** (`parse.hpp`/`parse.cpp`, `parse(ctx,text)→ParseResult{module,ok,error_offset,error}`): whitespace-skipping cursor, grammar mirrors the printer; **id→Value fixup pass** so a Graph-region **use-before-def** (legal under free block-insertion order) resolves; **strings unescaped before interning** (else `\`/`"` grow each round); `count_trailing_regions` scans **balanced braces skipping string literals** (a nested op's string attr may hold `{`) so `create_operation` gets `num_regions` upfront; `{`-disambiguation (attrs `{name =` vs region `{^`/`{}`); **malformed input REJECTED** with a byte offset (trailing garbage · duplicate SSA id · undefined operand · non-`dialect.op` name · truncation) — the signal CEIR-1h's corpus reuses. **MLIR-faithful symbol identity** (advisor call): the func's name/visibility now ride **ON the op** as `sym_name`/`sym_visibility` string attrs (`func.cpp` — the SymbolTable is an INDEX over `sym_name`, not the source of truth; no `@name` grammar, no per-dialect hooks), so identity prints + round-trips through the generic attr machinery; the parser **rebuilds the module SymbolTable** from those attrs (duplicate name → parse error) and a `func.call` resolves against the rebuilt table post-parse. `tests/ceir` **31/31** (+5 roundtrip: rich-graph byte-exact print⇄parse [every attr kind · negative int · `4.0`/exponent floats · escaped string w/ quote+backslash+brace · nested+empty+multi-block regions · use-before-def fixup · func/call/return], double-parse determinism, opaque unknown-dialect, func symbol-identity survives, malformed-rejection). Tidy-clean (5 files). **Gate (ratified per-slice contract):** crd-ceir-tests PASS **2 Windows (win-debug + win-asan) + 2 Linux (linux-gcc-debug + linux-gcc-asan, WSL) + LLVM-20 tidy**, + `crd-ceir-invariants` (I3/I5/I6) green both OSes — scoped-complete (crd-ceir zero downstream). **⚠ D-007-divergence (→CEIR-1f):** the TEXT form is semantics-only and does **not** encode `Region::kind` (Graph vs SsaCfg) — an SsaCfg body round-trips as Graph textually (invisible to the byte-exact text DoD; both sides print without a kind). The BINARY form (1f) MUST carry the region-kind field. | §10 §166 |
| ✅ | CEIR-1f | ✅ **CLOSED 2026-08-08.** **Binary serial form** (`binary.hpp`/`binary.cpp`: `serialize(ctx,module,alloc)→Array<u8>` + `deserialize(ctx,bytes)→ParseResult`) — the compact sibling of the text form, in the CRDR mould (ADR-0038): magic `'CEIR'` + a version word + FourCC/length-prefixed chunks a reader iterates and **SKIPS by length** when the FourCC is unknown (forward-compat, tested by splicing a synthetic `'XXXX'` chunk). Chunks (v1): **`STRP`** string pool · **`SRCM`** source-file map · **`ATTR`** attribute-value pool · **`BODY`** the region graph. **⛔⛔ FIELD-BY-FIELD little-endian** — never a struct blast (the padding-in-content-hash scar); the CKIR-serialize `put_u*` writers + bounds-checked `.ok`-latching `Cursor` reader, self-contained (crd-ceir cannot link crd-kir; the CRDR container likewise). **⭐ Content-pure:** the pools are built from the MODULE WALK (first-use order, only what the module references) and BODY holds pool INDICES — never a Context id — so **the blob is a pure function of module content** (dirty-context byte-equality PROVEN: the same graph built in a clean vs a pre-polluted Context serializes byte-equal). This closes the **1e region-kind divergence**: the binary form carries `Region::kind` (Graph/SsaCfg), restored on load via the new `Context::set_region_kind` (verified structurally at BOTH the module body and an op region). `SourceLoc` survives BY PATH (re-`register_file`'d), a Graph-region use-before-def resolves via the same fixup pass as the parser, and symbol identity rebuilds through the **shared `detail::register_symbol`** helper (extracted from the parser so the two loaders never drift). `serialize∘deserialize∘serialize` is **byte-exact**; `print∘deserialize∘serialize == print` (the two forms agree). Malformed input REJECTED with a byte offset: bad magic (0) · unsupported version (4) · truncation · trailing bytes after the last chunk · out-of-range pool index. `tests/ceir` **37/37** (+6 binary; `build_rich` extracted to a shared `rich_graph.hpp` reused by the text + binary gates). Tidy-clean (11 files). **Gate:** crd-ceir-tests PASS **win-debug + win-asan + linux-gcc-debug + linux-gcc-asan (WSL) + LLVM-20 tidy** + `crd-ceir-invariants` (I3/I5/I6) both OSes. **⚠ scar:** the I6 raw-line grep bites PROSE that spells `switch`+`kind()` — comments must describe the rule without writing the pattern. **→ CEIR-3** adds a `TYPE` chunk (TypeId is an inline opaque u32 here) and bumps the version. | §104 §123 |
| ✅ | CEIR-1g | ✅ **CLOSED 2026-08-08.** **`ModuleBuilder` fluent C++ API** (`builder.hpp`/`builder.cpp`) — an ergonomic host-side builder that emits ORDINARY canonical IR. **⛔⛔ NO privileged bypass:** every op is created through the SAME `Context::create_operation`, placed with the SAME intrusive block edits, attributed via `set_attr`, and (if symbol-defining) registered through the SAME shared `detail::register_symbol` — a builder-made module is byte-identical to the hand-built one (PROVEN: `print(builder) == print(hand)`). Surface: **`ModuleBuilder`** (owns the module; `add_block(args,type,into)` append+select, `set_insertion`/`insertion`, `op(dialect,name)`, `func`/`ret`/`call` convenience reusing `crd::ceir::func`, and **`verify(&failing)`** that walks every op through the REAL per-kind `Context::verify` — no stub); **`OpBuilder`** (fluent `.operand/.operands/.result/.results(n,t)/.attr/.regions/.loc` → terminal `build()`/`build_result(i)`); **`InsertionGuard`** (RAII save/restore for nested-region building). ⛔ **`build()` returns nullptr on a duplicate `sym_name`** — the op is `erase()`d (no silent overwrite, mirroring `create_func`); `build_result` asserts success. Insertion point asserted (in `func()`, before `create_func` so a bail can't leave a registered-but-unplaced func). Verifier-routing proven by a rejection test (a custom-dialect verifier requiring ≥1 operand; a 0-operand build → `verify()` false + `failing` pinpoints it) and the builder module binary-round-trips. `tests/ceir` **41/41** (+4 builder). Tidy-clean (4 files). **Gate:** crd-ceir-tests PASS **win-debug + win-asan + linux-gcc-debug + linux-gcc-asan (WSL) + LLVM-20 tidy** + `crd-ceir-invariants` (I3/I5/I6). **⚠ note (→ later canonicalization):** the 1f binary BODY stores an op's attrs in dict INSERTION order, where the text printer SORTS them by name — deterministic content either way (not a purity bug), but a builder-vs-hand *blob* equality would require matching `.attr()` call order; a canonical-order pass is a later-band consideration. | §121 |
| ✅ | CEIR-1h | ✅ **CLOSED 2026-08-08.** **The permanent test harness, seeded now** (§119/§167). **Round-trip fuzz** (`test_fuzz.cpp`): random-but-VALID modules generated THROUGH `ModuleBuilder` (dogfoods 1g — operands drawn only from already-defined SSA values, 0/1/**2**-region ops, 0..N-op blocks incl. empty, attrs of every kind, nested regions) must round-trip **byte-exact** through BOTH forms — text `print→parse→print` AND binary `serialize→deserialize→serialize` — across a fixed seed array (⛔ inline xorshift64, nonzero state, NO `<random>`, never time-seeded — the A/B-deterministic scar) plus the dense `build_rich` fixture. **Stable content hash** — `stable_hash(ctx,module,scratch)` = FNV-1a over the content-pure 1f blob (NEW surface): deterministic, Context-history-independent (clean-vs-dirty hash EQUAL), discriminating, and stable under a BINARY round-trip (never across the text path — region kind / NaN are text-invisible). **Malformed corpus** (`test_malformed.cpp`): a table of bad text + bad binary — each REJECTED via `ParseResult{ok=false,error_offset}`, never a crash — plus a **single-byte-corruption SWEEP** (XOR every position of a valid text and a valid blob → assert no crash; ASan/UBSan is the memory-safety proof). ⛔⛔ **Two loader HARDENINGS this slice — real OOM crashes the harness caught in code that had already passed FOUR slices of gates** (the fuzz doing its job on day one): (a) the **text parser** OOM'd on a huge def-id (`%4000000000`) — now bounded by text length in `register_value`; (b) the **binary decoder** OOM'd on a corrupt count — now every count is bounded (operands/attrs/regions/blocks by the chunk length; num_args/num_results by `kMaxDecodeCount`, a **documented v1 format constraint** in binary.hpp). `tests/ceir` **46/46** (+5). Tidy-clean (5 files). **Gate:** crd-ceir-tests PASS **win-debug + win-asan + linux-gcc-debug + linux-gcc-asan (WSL) + LLVM-20 tidy** + `crd-ceir-invariants` (I3/I5/I6). **§167 map:** SEEDED now — parser/printer round-trip · binary round-trip · stable hash · malformed IR · builder/text equivalence · source-map preservation · unknown-dialect; DEFERRED with their band — dominance + region legality → CEIR-5b, op-schema generation → CEIR-2, dialect versioning → later. **⚠ v1 limitation:** a `SymbolRef` attr's text is identifier-only (the printer emits `@name` raw, no quoting — MLIR quotes; ours doesn't yet); a later band adds quoting + bumps the format. | §119 §167 |
| ✅ | CEIR-1z | ✅ **CLOSED 2026-08-08 — BAND 1 GATE.** A **typed hello-world** (`func.func @add1` with a param → `arith.const {value=1}` → `math.add` → `func.return`, plus a top-level `arith.const {value=41}` + `func.call @add1`) built TWO ways (hand factories + `ModuleBuilder`), asserting the full §167 acceptance: `print(hand) == print(builder)` (builder/text equivalence), `print(parse(print(m))) == print(m)` (text round-trip), `serialize→deserialize→serialize` byte-exact + `print(deserialize(serialize(m))) == print(m)` (binary ⇄ text agreement), and `func.call` **resolves** `func.func` after the builder-native form AND a text parse AND a binary load (symbol survives all three forms). "const" is a GENERIC op with a `value` attr — no dedicated dialect. `tests/ceir/test_hello.cpp` 3 cases; the **fuzz corpus is green in ctest** (test_fuzz/test_malformed) and the **guard greps are registered** (the `crd-ceir-invariants` ctest — I3/I5/I6, both OSes). `tests/ceir` **49/49**. Tidy-clean. **Gate:** crd-ceir-tests PASS **win-debug + win-asan + linux-gcc-debug + linux-gcc-asan (WSL) + LLVM-20 tidy**. | §167 |

## CEIR-2 — Schema/ODS-like op-definition generator — ⬜  → §8

**Band contract (§8).** Ops are DEFINED in TOML (authoring-text house rule), generated to C++ — never hand-kept in
parallel. `*.ceirop.toml` per dialect under `engine/ceir/ops/`; generator `tools/ceir_opgen/ceir_opgen.py` (checked
in, output committed — the `gen_fft_batched.py` precedent) emits: typed op wrapper + builder + verifier scaffold +
reflection record + CR-D007 node metadata + CLI/MCP schema + doc stub + test skeleton. Schema fields per §8: name ·
dialect · version · operands(+variadic) · results · attrs · regions · traits · effects · domain legality ·
capability requirements · type/shape inference · fold · serialization schema · editor hints · docs · deprecation.
**Gate: a new pure op is added with no central enum/switch edits (proves §7).**

| ✔ | Slice | Deliverable | → §§ |
|---|---|---|---|
| ⬜ | CEIR-2a | The `*.ceirop.toml` schema format + its validator (a schema error is a cook-time diagnostic pointing at the TOML line — the declared-contract rule). **Includes the optional ADR-0110 §2.1 native-binding fields** (provider class · determinism · thread-safety · lifetime · cost hints · hot-reload compat) so intrinsic ops define through the SAME generator — no parallel schema | §8 |
| ⬜ | CEIR-2b | Codegen: typed C++ wrapper (`ceir::core::IfOp`-style accessors), builder overloads, verifier scaffolding wired to the 1d registry | §8 |
| ⬜ | CEIR-2c | Reflection record + editor-node metadata + CLI/MCP schema emission (feeds agent-native authoring) | §8 §122 §161 |
| ⬜ | CEIR-2d | Doc + test-skeleton generation; regen-drift guard (regenerate → diff clean — the canonical-form drift gate) | §8 §123 |
| ⬜ | CEIR-2z | **Gate:** add a `test.dummy` op by writing ONLY a TOML entry — wrapper/builder/verifier/reflection/docs all appear, zero central-file edits | §7 §8 |

## CEIR-3 — Type/shape/unit/lifetime foundation — ⬜  → §16–§22

**Band contract.** The interned type system CEIR values carry. Scalars/aggregates (§16, incl. tagged-union/`Option`/
`Result`); resource types (§23); symbolic shapes (§21/§35); **quantity metadata = ADR-0078 dimension tags on
high-level types, erased at the kernel/raw boundary** (§17 — reuse `crd::units`, do NOT reinvent); ownership/view
qualifiers (§19). **CHIR headroom NOW:** generic params + constraint records + interface/trait types + callable
types exist in the IR before any frontend emits them (append-at-END, type-system edition). **Gate: invalid unit/
shape/view combinations are rejected.**

| ✔ | Slice | Deliverable | → §§ |
|---|---|---|---|
| ⬜ | CEIR-3a | Scalar + aggregate type kinds, interned in `Context`; canonical print forms | §16 |
| ⬜ | CEIR-3b | Generics: type params, constraint records, interface/trait types, callable types — verifier-checked substitution | §16 §98 |
| ⬜ | CEIR-3c | Resource type kinds + view types (byte/element/mip/layer/aspect ranges) | §23 |
| ⬜ | CEIR-3d | `ceir.shape`: shape values, rank/extents, symbolic dims, broadcast/reshape-compat relations, shape assertions | §21 §35 |
| ⬜ | CEIR-3e | Quantity metadata via crd-units dims; explicit erase op at the raw boundary; ⛔ untagged physical numerics rejected on public domain-API ops (extends `crd-no-untagged-physical-numeric` to CEIR) | §17 §18 |
| ⬜ | CEIR-3f | Ownership/view qualifiers + escape rules (a borrow may not outlive its region — the allocator-outlives-borrowers scar, IR edition) | §19 |
| ⬜ | CEIR-3z | **Gate:** verifier rejects with POINTING diagnostics: `Length+Time`, rank-mismatched broadcast, borrowed-view region escape, generic-constraint violation | §16 §17 §19 §21 |

## CEIR-4 — Effect + determinism model — ⬜  → §26 §27 §15

**Band contract.** Every effectful op declares semantic effects (§26 families) optionally carrying resource/range
identity — the frame graph's proven read/write/lifetime discipline (WAR-needs-lifetime, RMW-not-RWM scars) promoted
to first-class IR. Determinism classes (§27) **align 1:1 with the ADR-0098 T1/T2/T3 tiers CKIR already certifies** —
ONE vocabulary. Evaluation domains (§15) + real-time classes (§32) as metadata with legality checking. Numerical
semantics (§28) attach here. **Gate: compiler distinguishes reorderable vs ordered ops correctly.**

| ✔ | Slice | Deliverable | → §§ |
|---|---|---|---|
| ⬜ | CEIR-4a | Effect interface + effect records w/ resource/range identity; declared via the 2a schema | §26 |
| ⬜ | CEIR-4b | Determinism classes wired to ADR-0098 tiers; compiler modes (`certified_deterministic`/`deterministic`/`normal`/`fast`); numerical-semantics attrs (IEEE/FMA/FTZ/rounding); ⛔ passes may not silently violate an active contract | §27 §28 |
| ⬜ | CEIR-4c | Evaluation-domain + realtime-class metadata + the domain-legality verifier (file I/O in a DeviceTime/audio-RT region = rejected with a pointing error) | §15 §32 |
| ⬜ | CEIR-4d | Hazard foundations: effect-derived ordering constraints between ops over the same range — the analysis CEIR-12d's scheduler consumes | §26 §116 |
| ⬜ | CEIR-4z | **Gate:** a curated effect-pair module — every pair classified reorderable/ordered correctly, incl. same-range WAR (the lifetime-not-decl-order scar as a test) | §26 §168 |

## CEIR-5 — Structured control flow + functions — ⬜  → §13 §14 §20

**Band contract.** `ceir.core` structured ops — `if`/`switch`/`for`/`foreach`/`while`/`match`/`scope` as
region-carrying ops (§13 §14); SSACFG blocks+branches beneath them; calls + bounded-recursion attrs; explicit
state: `state<T>`/`history<T>`/`delay<T>` (§20) — graph cycles ONLY through state/delay (verifier rule). Plus the
first **reference executor** skeleton (host, slow, correct — §118) running core+func+state. **Gate: a nontrivial
program executes in the reference host runtime.**

| ✔ | Slice | Deliverable | → §§ |
|---|---|---|---|
| ⬜ | CEIR-5a | Structured region ops via 2a schemas; canonicalizations (constant-cond `if` folds — the partial-eval seed) | §13 §14 |
| ⬜ | CEIR-5b | SSACFG region verifier: dominance, terminators, block args (the §115 structure layer) | §13 §115 |
| ⬜ | CEIR-5c | Calls + recursion policy attrs (bounded-depth verified where declared) | §14 §34 |
| ⬜ | CEIR-5d | `state`/`history`/`delay` ops + the cycles-only-through-state verifier rule | §20 |
| ⬜ | CEIR-5z | **Gate:** a pinned program — bounded loop + `match` + calls + a `state<T>` accumulator — executes in the reference executor with byte-pinned output; identical result from text-parsed and builder-built forms | §118 |

## CEIR-6 — Async/task/runtime domains — ⬜  → §30 §31 §37 §38

**Band contract.** `ceir.async` (tokens/async values/join/race/cancel, §37) + `ceir.task` (§38) with the **host job
provider lowering onto crd-jobs** — `task.parallel_for`→`crd::jobs::parallel_for`, `task.spawn`/waits→jobs
counters/fiber waits, main-thread pinning, deadline/priority attrs. ⛔ No second scheduler (§31): CEIR is an
authorable front-end to the existing fibers. This is where the **first execution provider** (host/jobs) is born in
minimal form (§69). **Scars:** worker_index aliasing · frame-arena exhaustion under parallel_for · workers reset on
shutdown. **Gate: a job-system parallel program is authored as CEIR.**

| ✔ | Slice | Deliverable | → §§ |
|---|---|---|---|
| ⬜ | CEIR-6a | `ceir.async` ops + verifier (token misuse = §116 class) | §37 §30 |
| ⬜ | CEIR-6b | `ceir.task` ops + the crd-jobs provider — the first `IExecutionProvider`, in the **`crd-ceir-host` bridge module** (ADR-0109 §4.2), NOT inside `crd-ceir` (which stays host-only + jobs-free; putting it in the core breaks I4) | §38 §31 §69 |
| ⬜ | CEIR-6c | Structured-concurrency scopes; cancellation/deadline plumbed through the provider | §30 §32 |
| ⬜ | CEIR-6z | **Gate:** a parallel map-reduce authored as CEIR runs on the fiber pool, result bit-identical across `{1..16}` (fixed-order reduction), ASan-clean | §30 §31 |

## CEIR-7 — Asset/cook/runtime lifecycle — ⬜  → §105–§110

**Band contract.** CEIR programs become ordinary Cerid assets. **Reuse hard:** CRDR container (new `'CEIR'` chunk),
the ADR-0104 content-hash cook cache + variant dedup, the RAF-11 hot-reload machinery (⛔ its reentrant-
`init_programs` guard+reserve+drain scar applies verbatim), D5 generation-retire. New: the **interface hash** (§107,
params/results/caller-visible effects/capabilities/state schema) split from the content hash so implementation-only
edits hot-swap without invalidating callers; asset classes (§105); `engine://`+`app://`+`runtime://` namespaces
(RAF-9's `engine://` registry). **Gate: a live CEIR program hot-swaps safely.**

| ✔ | Slice | Deliverable | → §§ |
|---|---|---|---|
| ⬜ | CEIR-7a | `'CEIR'` cook: source → verified → binary chunk; content hash + interface hash; dependency records (called funcs · CKIR refs · intrinsics · providers) | §105 §106 §107 |
| ⬜ | CEIR-7b | RuntimeSlot/handle model over crd-resources (generation-safe handles) | §105 |
| ⬜ | CEIR-7c | Hot reload: detect→verify→cook→compile-affected→validate-set→atomic install→generation bump→deferred destroy; last-good on failure; ⛔ no mixed-generation call graph; state migration (compatible reuse / migration fn / reject) | §108 §109 |
| ⬜ | CEIR-7d | Execution-plan cache keyed by (content hash × target × compiler version) — compiled artifacts are caches, never truth | §110 §104 |
| ⬜ | CEIR-7z | **Gate:** the §172 matrix, host subset: body-edit hot-swaps live; signature-edit REJECTS + keeps last-good; dep-edit recompiles exactly the affected region (cache-hit counts asserted) | §172 |

## CEIR-8 — Reference executor + compiled host plan — ⬜  → §83 §84 §118 §153

**Band contract.** Two execution tiers (§84). Reference tier: the CEIR-5 executor grown to the full host subset,
maximal diagnostics (the §118 correctness oracle). Compiled tier: `CompiledExecutionPlan` = dense arrays of
pre-resolved op thunks + slot-indexed values — **§153 audited: zero strings, zero per-op heap, zero map lookups,
zero source parsing in the loop** (CountingAllocator-gated, the v14-m allocation-free-infer precedent). Profiler:
every plan op wrapped in `CRD_PERF_SCOPE`-compatible regions (crd-perf, no new profiler). **Gate: no source
parsing / string lookup in the shipping execution loop.**

| ✔ | Slice | Deliverable | → §§ |
|---|---|---|---|
| ⬜ | CEIR-8a | Reference executor: full core/func/state/async-host subset; step hooks (the §112 debugger seam, hooks only) | §118 §84 §112 |
| ⬜ | CEIR-8b | Plan compiler + `CompiledExecutionPlan`; the differential harness: reference vs compiled byte-compare over the whole 5z/6z/7z corpus | §84 §119 |
| ⬜ | CEIR-8c | crd-perf integration: per-op regions, plan-compile cost counter | §113 |
| ⬜ | CEIR-8z | **Gate:** allocation-free + string-free hot loop proven by CountingAllocator + a no-string-table-touch assert; differential corpus green | §153 |

## CEIR-9 — Resource/memory subsystem — ⬜  → §36 §24 §78

**Band contract.** `ceir.resource` (§36): views/ranges/subresources over the 3c types; transient/persistent/history
lifetimes; alias groups; memory-domain INTENT (§24 — ⛔ never Vulkan memory types / D3D heap flags as semantics);
residency/streaming hooks (§25); the memory-planner interface (§78 profiles: latency/balanced/memory/deterministic).
**Reuse:** the render-graph's greedy interval-coloring transient aliaser + lifetime analysis = the planner's first
implementation. **Scars, all here:** WAR-needs-resource-lifetime (both backends) · aliaser-must-check-slot-SIZE ·
RMW-not-RWM · transient-borrowed-bundle format. **Co-design with RAH-2.** **Gate: resource-graph lifetime +
aliasing tests.**

| ✔ | Slice | Deliverable | → §§ |
|---|---|---|---|
| ⬜ | CEIR-9a | Resource decl/view/range ops; import/export for externals | §36 §23 |
| ⬜ | CEIR-9b | Lifetime classes + history (`history<T>` binds here) + memory-domain intent attrs + residency hints | §24 §25 |
| ⬜ | CEIR-9c | Alias/lifetime analysis over 4d effects — ports the frame graph's interval model; the WAR + slot-size scars become IR-level ctest cases | §78 §26 |
| ⬜ | CEIR-9d | Planner interface + first planner (interval-coloring port); plan inspectable (§162 explainability: "why 64 MB") | §78 §162 |
| ⬜ | CEIR-9z | **Gate:** lifetime + aliasing corpus green incl. ported scar cases; `transient_memory < transient_logical` asserted on an aliasable module (REN-1 proof, IR edition) | §36 §78 |

## CEIR-10 — Compute + transfer (first GPU contact) — ⬜  → §42 §50 §85 · PROOF §129

**Band contract.** `ceir.compute` (§42) + `ceir.transfer` (§50), lowered onto the EXISTING canonical command model
(`gpu-context` `command_model.hpp` / `detail/command_lowering.hpp`) and `IComputeContext` **via the `crd-ceir-gpu`
bridge module** (ADR-0109 §4.2 — the render/compute provider; `crd-ceir` core never links gpu-context) — CEIR
orchestrates the proven layer, it does not invent GPU plumbing (§158). CKIR by identity (§85): `compute.dispatch @kernel` references
CKIR programs by content-hash; compiler-EMITTED CKIR deferred to CEIR-23. **The proof reuses already-bit-exact
kernels** (`build_reduce`, `build_scan`, radix sort, FFT) — the new thing under test is ORCHESTRATION, not math.
**Scars:** upload→first-read barrier (grid-size time bomb) · dispatch_1wg · timing asserts same-pass-only.
**Proof (§129):** add/reduction/scan/FFT program asset on both backends.

| ✔ | Slice | Deliverable | → §§ |
|---|---|---|---|
| ⬜ | CEIR-10a | `ceir.compute`: direct/indirect dispatch ops, kernel binding records (against the RAH-era binding model) | §42 |
| ⬜ | CEIR-10b | `ceir.transfer`: buffer/image copies, upload/readback, clear, mip-gen — each declaring 4a effects | §50 |
| ⬜ | CEIR-10c | CKIR-by-identity: kernel asset refs resolved through the ADR-0104 cache; interface-hash checked vs the dispatch signature at cook (declared-contract) | §85 §107 |
| ⬜ | CEIR-10d | Lowering pass: CEIR region → canonical command stream with 4d-derived barriers (incl. the upload barrier BY CONSTRUCTION) | §158 §79 |
| ⬜ | CEIR-10z | **Proof (§129):** `add`·`reduce`·`scan`·FFT as CEIR program ASSETS — authored text AND builder, dispatched Vulkan AND DX12, ValidationCapture-silent, bit-exact vs the existing CPU oracles, hot-reload live-swaps, reference executor validates host-visible semantics. ⚠ **THIS is the "first CEIR vertical slice" (§5 gate 2):** on close, execute ADR-0108's deferred cornerstone flip — PRINCIPLES/AGENTS/README/ROADMAP C++-only → CEIR/CHIR + the ADR-0081 §9 in-file strike, as ONE commit (ADR-0108 §7). | §129 §170 |

## CEIR-11 — Render dialect — ⬜  → §40 §41 · PROOF §169

**Band contract.** `ceir.render` (§40) on RAH's hardened model: attachments are the RAH-1 typed
`ColorAttachmentDesc` span — G-buffer/visbuffer/velocity/object-ID are program-defined output contracts, NOT ops
(§41 — the RAH-1a.1 lesson generalized); draw family (draw/indexed/indirect/indirect-count/mesh-dispatch/patch,
§40); resource tables per RAH-2 (§157). Lowers to the same canonical command verbs the frame graph records today.
**Scars:** indirect draws must push the DrawIndex row · depth-only borrowing a color program dies when FS gains
discard · NDC±Y mirror on RTT-sampled passes. **Proof: triangles/MRT/depth/indirect/mesh/tess on both backends.**

| ✔ | Slice | Deliverable | → §§ |
|---|---|---|---|
| ⬜ | CEIR-11a | Render scope op with typed N-attachment span (float/int/uint + depth/stencil + typed clears + resolve + MSAA + RO-depth) | §41 |
| ⬜ | CEIR-11b | Draw ops + dynamic state (viewport/scissor/shading-rate) with effects/hazards declared | §40 |
| ⬜ | CEIR-11c | Indirect + indirect-count + mesh/task dispatch (the DrawIndex scar as a lowering-level ctest) | §40 |
| ⬜ | CEIR-11d | Resource-table binding semantics over RAH-2's resident tables | §156 §157 |
| ⬜ | CEIR-11z | **Proof:** triangle · MRT (typed clears asserted per-target) · depth-only · indexed-indirect(-count) · mesh dispatch — pixel-asserted both backends, ValidationCapture-silent | §169 |

## CEIR-12 — FrameGraph unification — ⬜  → §39 §126 §159

**Band contract.** The §126 eight steps, seam by seam. `frame-cook`'s `FrameGraphDesc` parse emits `ceir.frame`
(§39) instead of the private blob; `FrameGraphBuilder` emits the same; the 38-D4-class cook validations become CEIR
verifiers; `FrameGraphTemplate`/`CompiledFrameGraph` (ADR-0106) become the lowering/plan layer; `execute_frame`
runs CEIR plans. **Invariant preserved:** `frame-cook ⊥ crd-scene` via the `IFrameGraphHost` seam (host resolves
ECS→pre-resolved `DrawItem`; CEIR never sees a scene type). **Per-asset parity gate:** every shipped built-in frame
asset (`forward_csm`, deferred, the sandbox frames) renders PIXEL-IDENTICAL through CEIR before its old path dies —
with a deterministic clock (the A/B-needs-deterministic-clock scar). **ADR-0106 struck in place @ 12f.** Asset IDs
preserved. FrameGraph is not deprecated syntax — its semantics (topology/lifetime/history) become a CEIR dialect
(§159).

| ✔ | Slice | Deliverable | → §§ |
|---|---|---|---|
| ⬜ | CEIR-12a | `.frame.toml` frontend → `ceir.frame` (passes, reads/writes, transients/history, queue hints, capability variants) | §39 §126 |
| ⬜ | CEIR-12b | `FrameGraphBuilder` → same canonical `ceir.frame` (builder-vs-toml semantic-equality ctest) | §126 §121 |
| ⬜ | CEIR-12c | Cook validations → CEIR verifiers (varying-contract, header-word, shape — each a pointing diagnostic) | §115 §126 |
| ⬜ | CEIR-12d | Frame compile → CEIR scheduling + the CEIR-9 planner (one-submission execution preserved — Gate-7 semantics) | §78 §79 §126 |
| ⬜ | CEIR-12e | Runtime executes CEIR plans; A/B harness runs old + new per asset with the deterministic clock | §126 |
| ⬜ | CEIR-12f | Adapters + the duplicate frame path DELETED; **ADR-0106 superseded-in-place**; blob compat per the 0h table | §126 §159 |
| ⬜ | CEIR-12z | **Gate:** every shipped frame asset runs through CEIR pixel-identically both backends; the old path is gone (the-deletion-is-the-proof); `crd-sandbox --smoke-test` green, validation on | §126 |

## CEIR-13 — Executor migration — ⬜ (the PAUSE LIFTS at this gate)  → §127 · PROOF §128

**Band contract.** Execute the 0a/0h classification. Every **composite** executor (scene raster orchestration,
fullscreen setup, compute setup, RT orchestration, transfer workflows — final list from 0a, not from docs) becomes
a CEIR program asset in the `engine://` built-in pack; every **atomic** capability registers as a CEIR-0d intrinsic
(scene resolvers land as host intrinsics: `scene.resolve_material/technique/program/geometry` — §45's replaceable
convenience tier). The decisive proof: **`scene.raster` as ordinary CEIR (§128)** — begin-render → foreach draw →
resolve chain → build bindings → draw — pixel-identical to the C++ executor on the live sandbox scenes.

| ✔ | Slice | Deliverable | → §§ |
|---|---|---|---|
| ⬜ | CEIR-13a | Migration order from 0a (smallest composite first; each with a named parity gate) | §127 |
| ⬜ | CEIR-13b | Composite executors → `engine://ceir/*` assets, one at a time, old+new A/B per step (the RAF-8 one-kind-at-a-time discipline) | §127 |
| ⬜ | CEIR-13c | Atomic intrinsics registered per the 0d schema (effects/domain/determinism declared, capability-gated) | §100 §45 |
| ⬜ | CEIR-13d | **`scene.raster` proof (§128):** the full resolve+draw loop as a CEIR asset, pixel-identical both backends incl. the bindless/multi-draw path | §128 |
| ⬜ | CEIR-13z | **Gate:** the legacy composite-executor path deletable AND deleted per 0h; `crd-sandbox --smoke-test` + REN-38-F6-class suites green through CEIR; **→ broad feature development resumes as CEIR program assets** | §127 §178 |

---

## CEIR-14…32 — one gated row each; splits into sub-slices AT BAND OPEN (sized by CEIR-0z)

Each row cites the **dialect/subsystem §§ it builds**, the **proof §§ it must pass**, and the **corpus §§ it must
eventually express** — plus a reuse note (what already exists, so the band starts from the engine, SANITY #8).

| ✔ | Band | Scope → **gate/proof** · reuse | → §§ |
|---|---|---|---|
| ⬜ | CEIR-14 | Scene/ECS/geometry bridge → rigid/skinned/indirect scene rendering + GPU culling as CEIR · reuse: REN-39 indexed-pull, 40-A device cull, GVA-2 skinning, crd-geometry CPU | §45 §46 §47 |
| ⬜ | CEIR-15 | Renderer proof suite (absorbs RPL) → **Forward+ · Clustered · Deferred · Visibility · GPU-driven as assets; no new native pass algorithm** · reuse: the shipped technique/material stack + B8 lighting | build §40; corpus §87 §88 §89; proof §130 §131 §132; test §169 |
| ⬜ | CEIR-16 | `ceir.rt` → hybrid RT + **wavefront path tracer** · reuse: A16 RT pipelines + inline rayQuery both backends, hair LSS, the anyhit-OPAQUE scar suite | build §49; corpus §90; proof §133 §134 |
| ⬜ | CEIR-17 | `ceir.work` dynamic/device work → one semantic program on ≥2 lowerings: compute+indirect fallback FIRST, then Work Graphs/DGC/shader-enqueue/ICB/CUDA-graph · reuse: F16 amplification, 40-A indirect | build §43 §44; proof §135 |
| ⬜ | CEIR-18 | `ceir.tensor`/`ceir.shape` high-level tensor IR → chained tensor workflow, no premature dispatch lowering · reuse: hesap-tensor dtypes/layouts, v14 I/O | §51 §21 §22 §70 |
| ⬜ | CEIR-19 | CRD-Hesap integration (absorbs hesap-GPU) → **GEMM→FFT→reduction→viz-prep as ONE CEIR asset, no CPU round-trip, oracle-gated** · reuse: coopmat GEMM (~89% cuBLAS), the GPU FFT/sort/reduce suite (bit-exact) | build §52; §59; corpus §93; proof §137 |
| ⬜ | CEIR-20 | `ceir.sparse` + `ceir.quant` → sparse op + quantized-MLP path · reuse: hesap sparse formats, v14-m Q8 kernels | §53 §54 |
| ⬜ | CEIR-21 | `ceir.ml` + provider partitioning (absorbs MLR) → MLP + attention via CKIR AND a native-provider strategy · reuse: NRC fused MLP (2.37× cuBLAS), C6 coopvec | build §55 §56; §69; corpus §94; proof §136 §138 |
| ⬜ | CEIR-22 | `ceir.autodiff` as compiler transform → differentiable compute/render w/ custom VJP + checkpointing · reuse: v15/v16 AD architecture + graph_ad IR | build §57; proof §139 |
| ⬜ | CEIR-23 | Optimizer phase 1 → canonicalize/DCE/CSE/partial-eval/specialize/inline/basic-fusion(→generated CKIR)/memory/schedule — each pass differential-tested vs reference | §73 §74 §76 §77; correctness §120 |
| ⬜ | CEIR-24 | `ceir.transform`/`ceir.rewrite` programs → two schedules optimize ONE semantic program; schedules are assets | §71 §72 §146 |
| ⬜ | CEIR-25 | Autotune/PGO/cost model → per-device config cache + deterministic locked mode · reuse: the v17 autotuner (checked-in tuning DB) | §80 §81 §82 |
| ⬜ | CEIR-26 | Native graph providers → one graph partitioned across two provider classes (D3D MLIR Programs / VK data-graph / NPU research) | §70 §102 §69 |
| ⬜ | CEIR-27 | `ceir.dist` multi-device → sharded reduction/training slice; placement is semantic | §68 §103; proof §140 |
| ⬜ | CEIR-28 | Media/UI/audio bridges (absorbs I2D effect graphs, MED workflows, audio graphs) → routed through CEIR, no new schedulers · reuse: MED-1 codecs (stay native, §177), the UiWorld/Canvas plan | build §61 §62 §63; corpus §95 §96 §97; proof §141 §142 |
| ⬜ | CEIR-29 | **CHIR + language prototype** — implements the CEIR-0e design CORRECTED by the CEIR corpus → same program authored visually AND textually, hot reload | §4 §98; proof §143; DoD §180 §181 |
| ⬜ | CEIR-30 | CR-D007 universal program editor → all views incl. multi-level IR snapshots; D7E editor lands here · reuse: the I2D-9 widget plan | §163 §164 §165 §145 |
| ⬜ | CEIR-31 | Legacy deletion → **one execution-program architecture**; every 0h row executed | §175-31 §178 |
| ⬜ | CEIR-32 | Production qualification → cross-backend/ASan/fuzz/deterministic-cook/hot-reload-stress/large-graph/perf boards/docs; the DoD lists answered item by item | §179 §180 §181 §182; tests §167–§172 |

---

## Paused (explicitly parked, NOT dropped — resumes per row after CEIR-13z unless noted)

| Was | State at pause (2026-08-07) | Resumes as |
|---|---|---|
| Track A: RPL→VGE bands | ⬜ (nothing beyond RAH started) | CEIR-15/16/17 proof assets + post-CEIR bands |
| Track B: I2D/SPR (+ ADR-0107 review) | I2D-0 ADR drafted, review PARKED | review after the CEIR-0 ADRs; effect graphs via CEIR-28; widgets on RAH-2 + CEIR |
| Track C: CGP selector + HGP/MLR | CGP-0 ◧ (CUDA backend ✅ landed) | selector when a CEIR provider needs it; HGP→CEIR-19; MLR→CEIR-21 |
| Track D: MED codecs, D7E, PQP, EYL | MED-1 ◧ (GIF+LZW landed; external-oracle corpus follow-up owed) | codecs stay native capabilities (§177); workflows via CEIR-28; D7E→CEIR-30 |
| Main roadmap (hesap v18, eylem v1c+) | unchanged (pre-CEIR posture) | after the detour as before; eylem orchestration lands on `ceir.physics` (§60) when v1c+ resumes |
