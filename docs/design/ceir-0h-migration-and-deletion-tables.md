# CEIR-0h — Migration + explicit DELETION tables

> **Band:** D-007 · CEIR-0 · slice 0h. **Tracker row:** `docs/detours/D-007-ceir-tracker.md` → CEIR-0h.
> **Gate:** the tables are complete; **every deletion names its parity gate FIRST** (the-deletion-is-the-proof rule).
> **Law:** mission §126 (framegraph migration), §127 (executor migration), §178 (must-not-stay-native).
> **Built FROM (not re-derived):** `docs/design/ceir-0a-execution-path-inventory.md` — §2 (frame graph), §3 (the 14
> executors), §4/§4a (`scene_renderer.cpp` orchestration + the 9-block `render()` catalog), §5 (IComputeContext),
> §8 (the composite-vs-atomic verdict). **Status:** ✅ ACCEPTED 2026-08-08. This is the PLAN; the deletions
> EXECUTE at their named slices (CEIR-11/12/13/31), not here.

---

## 1. The rule this note enforces

Every row below is a thing that gets DELETED, and **its parity gate is named before the deletion** — the deletion
is proven, not asserted (the "deletion is the proof" house rule; RAF-8's one-kind-at-a-time A/B discipline). ⛔ A
deletion with no green parity gate is forbidden. Nothing here is deleted at CEIR-0h; this is the schedule the later
slices execute against.

## 2. What is DELETED vs what is PROMOTED (the critical distinction from CEIR-0a §8)

CEIR-0a's headline — RAF already did the atomic-vs-composite split — means most of the tree is **promoted, not
deleted**. Precisely:

- **PROMOTED (kept, re-based — NOT in the deletion tables):** the 14 executor verbs + their `record_*` lowerings
  (→ `ceir.render`/`compute`/`transfer`/`rt` op lowerings in `crd-ceir-gpu`, ADR-0109); the render-graph runtime
  (→ the `crd-ceir-gpu` provider); CKIR (`.crdr`, untouched); the five cookers (→ CEIR frontends / domain assets);
  `IComputeContext` (→ the compute provider); media codecs (→ intrinsics, §177).
- **DELETED (the tables below):** the composite C++ ORCHESTRATION and the DUPLICATE/privileged paths that the CEIR
  promotion makes redundant.

## 3. Frame-path deletions (§126 steps 6–8)

| # | Deleted | Parity gate (named FIRST) | Executes at |
|---|---|---|---|
| F1 | The private frame-blob execution path / `frame_runtime.cpp` adapter's role as a *distinct* driver (CEIR-0a §2 confirmed it's the cook-side feeder into the ONE render-graph runtime) — folded into the CEIR-12 plan path | every shipped built-in frame asset (`forward_csm`, deferred, sandbox frames) renders **pixel-identical** through the CEIR path, both backends, deterministic clock (the A/B-clock scar) — CEIR-12z | CEIR-12f |
| F2 | The direct `FrameGraphTemplate::add_pass` construction-and-execute path as a **privileged bypass** (§121 — no path executes without going through canonical CEIR) | `FrameGraphBuilder`→`ceir.frame` produces a graph **semantically equal** to the same program's `.frame.toml` (builder-vs-toml equality ctest) — CEIR-12b | CEIR-12f |
| F3 | `ADR-0106`'s "render-graph is THE runtime" **as a standalone claim** — struck in place, superseded by CEIR-12 (render-graph becomes the `crd-ceir-gpu` provider's execution of CEIR plans) | ADR-0106 struck-in-place + CEIR-12z green (its lifetime/aliasing/barrier/one-submission contracts preserved by the CEIR planner) | CEIR-12f |

## 4. Executor / orchestration deletions (§127, from CEIR-0a §4a)

⛔ The 14 executor VERBS are **not** here (they promote). What deletes is `scene_renderer.cpp`'s composite C++
orchestration — the 9 `render()` blocks (CEIR-0a §4a) + the program hand-list — once they are authored CEIR assets +
host resolvers.

| # | Deleted (CEIR-0a §4a ref) | Parity gate (named FIRST) | Executes at |
|---|---|---|---|
| E1 | The **program-variant selection matrix** — the hand-coded permutation ladder (skinned×textured×shadowed×indexed×depth×velocity), `scene_renderer.cpp:6169–6260` (block 7) | `scene.resolve_program` host intrinsic + the authored `ceir.frame` render the **same pixels** as the C++ ladder, both backends incl. bindless/multi-draw — the §128 `scene.raster` proof, CEIR-13d | CEIR-13z |
| E2 | The `render()` frame-graph assembly + cull + draw-list blocks (blocks 3–8) → the authored `ceir.frame` + `ceir.scene` resolvers | `crd-sandbox --smoke-test` + the REN-38-F6-class suites green **through CEIR**, pixel-identical — CEIR-13z | CEIR-13z |
| E3a | The **cascade-fit + shadow orchestration** C++ in `render()` (blocks 2/6: `compute_csm_cascades_from_vp`, the shadow-caster cull, `csm.cpp` sequencing) → a host resolver for the camera fit + a `ceir.frame` shadow sub-graph. ⚠ Must migrate at CEIR-13 (not CEIR-15) or E5's "no private C++ render path" cannot close at 13z. | the CSM cascade-selection + shadow observables reproduced through the CEIR shadow sub-graph, both backends — a CEIR-13 gate | CEIR-13z |
| E3b | The **gold shadow-corpus breadth** (§88 PCSS/EVSM/MSM/VSM/contact variants — the `ckir_lighting.hpp` shadow math) authored as CEIR shadow-technique assets (breadth, not the base orchestration E3a covers) | each shadow variant's B8 observable reproduced as a CEIR asset — the CEIR-15 shadow corpus (§88) | CEIR-15 |
| E4 | The program **hand-list** (`init_programs` / `register_default_programs`, `scene_renderer.cpp:2863`) — the code's own comment (`:1050-1053`) says the RAH-7 dependency-driven registry replaces it | **enumerable gate:** the RAH-7 registry serves the **identical program set** the hand-list did for the live scenes (set-equality ctest: `register_default_programs` output vs registry output) **+** the RAF-11 reload suite green through the registry path. **Two-slice resolution:** the hand-list is DELETED at RAH-7 (the registry is its stated replacement); CEIR-7 then subsumes the registry's invalidation with asset deps — **no second deletion**, just a capability upgrade. | RAH-7 (deleted) · CEIR-7 (invalidation subsumed) |
| E5 | `SceneRenderer::contribute`/`render` as the C++ render path (the whole composite) — reduced to an orchestrator that loads + executes the CEIR frame asset | E1+E2+E3 gates all green; no private C++ render path remains (§128) | CEIR-13z |

## 5. Residual special-case deletions (dissolve into general ops)

| # | Deleted | Parity gate | Executes at |
|---|---|---|---|
| R1 | `visbuffer.raster` executor (CEIR-0a §3 — a residual special-case; §41 says a visbuffer is `scene.raster` into a uint attachment) | a `ceir.render` scope over an `R32_UINT` typed attachment with a typed clear produces the **same visibility ids** as `visbuffer.raster` — CEIR-11z | CEIR-11 |
| R2 | `submit_overlay`'s composite + the `draw_overlay` device verb as a permanent special case (ADR-0110 §3 verdict) | the overlay renders identically as a small CEIR program over `ceir.render` LOAD+blend+RO-depth ops — CEIR-11 | CEIR-11 |

## 6. Supersessions executed by CEIR-0 (NOT deletions — struck-in-place, text stays)

These are the ADR-0032-treatment class: the clause is struck in place with a forward-pointer, the text remains as
history. They are NOT lines in CEIR-31's deletion close.

| # | Superseded (struck-in-place) | Proof | Executes at |
|---|---|---|---|
| M1 | D-007 §PR-3's standalone maturity ladder (CEIR-0g §4 — three live ladders would drift) — struck in place, pointed at §173 + the registry header | the unified two-axis model (CEIR-0g) is the single operational definition; §PR-3 carries only a pointer, its ladder text preserved as history | **CEIR-0f** |

## 7. Consequences + how this feeds CEIR-31

- The deletion set is **small and late** — nothing deletes before CEIR-11, and the big deletions (E1–E5) are the
  CEIR-13 pause-lift gate. This matches CEIR-0a's "promotion, not rewrite" finding: the tree is mostly re-based, and
  only the composite C++ orchestration + duplicate/privileged paths actually die.
- **CEIR-31 (legacy deletion) executes this ledger** — every row here is a line in CEIR-31's "one execution-program
  architecture" close, each already carrying its parity gate.
- ⛔ **No row deletes without its named gate GREEN.** If a gate can't be made green, the deletion does not happen and
  the item returns to design — never a silent removal.
- **Registry-field consequence (not covered by CEIR-0g's field-only plan):** after R1/R2 dissolve
  `visbuffer.raster`/`draw_overlay` and CEIR-13c re-bases the rest, the capabilities registry's `executors = [...]`
  entries (seeded from RAF executor names) go stale. **They migrate to `ceir.*` op / provider references at CEIR-13c**
  (when the executors become CEIR op lowerings) — so the §174 manifest never carries dead executor names into the
  CEIR era. (One line CEIR-0g's field-additions plan didn't cover; captured here.)

## 8. Open items

1. Confirm at CEIR-12 whether `frame_runtime.cpp` is deleted outright or retained as a thin cook→CEIR frontend (F1
   says folded; the exact seam is a CEIR-12 decision — flagged, not pre-decided).
2. E5 ("no private C++ render path") can only close at CEIR-13z if **E1 + E2 + E3a** are all green there — E3b
   (shadow-corpus breadth) may lag to CEIR-15 because it is *added variants*, not the base path. Confirmed consistent
   with §128/§176 sequencing.
