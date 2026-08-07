# CEIR-0z — The §184 close report + honest CEIR-1…13 sizing

> **Band:** D-007 · CEIR-0 · slice 0z (the CEIR-0 close). **Tracker:** `docs/detours/D-007-ceir-tracker.md` → CEIR-0z.
> **Gate:** the mission §184 fifteen items answered FROM EVIDENCE; honest per-band sizing; user sign-off.
> **Derived from (not estimated fresh):** CEIR-0a (inventory: 14 verbs · 9 `render()` blocks · 5 cookers · the §9.3
> site lists) + the accepted ADRs 0108/0109/0110 + CEIR-0e/0g/0h + the tracker band contracts.
> **Status:** ✅ ACCEPTED 2026-08-08. ⚠ Item 4 (D-007 sections changed) reports the CEIR-0f *plan*; 0z
> finalizes when 0f executes.

---

## 1. The §184 fifteen-item report (each answered from evidence, with its source)

| # | §184 item | Answer / where answered |
|---|---|---|
| 1 | current working-tree architecture discovered | CEIR-0a §1–§8 — the frame graph (composite sequencer) + 14 atomic executor verbs + `scene_renderer.cpp` orchestration + `IComputeContext` + 5 cookers + `.crdr` + draw + media, all with file:line |
| 2 | stale/conflicting statements found | (a) `executor_registry.hpp`'s "8 executors" header comment — there are **14** (CEIR-0a §3); (b) the stale `frame_runtime.hpp` header comment — fixed "when that file is next touched" (CEIR-0a §9.4), NOT at 0f (0f touches no engine headers); (c) the §PR-3 vs §173 two-ladder drift — struck at CEIR-0f (CEIR-0g §1); (d) the **managed** conflict: the C++-only-scripting rule still standing in PRINCIPLES/AGENTS/README/ROADMAP vs Accepted ADR-0108 — deliberately deferred to the first CEIR vertical slice (ADR-0108 §7), not an oversight |
| 3 | new ADRs proposed | **0108** (scripting reversal, Accepted) · **0109** (ownership + `crd-ceir` placement, Accepted) · **0110** (native-intrinsic schema, Accepted); + the CEIR-0e CHIR-0 design note (direction accepted) |
| 4 | D-007 sections changed | ⚠ **the CEIR-0f plan** (not yet executed): CEIR spine section added; the 122-row table re-hung under CEIR bands (RPL→CEIR-15 · MLR→CEIR-21 · frame/executor→CEIR-12/13 · I2D→CEIR-28 · hesap-GPU→CEIR-19); the tracker wired live; the **§PR-3 ladder struck-in-place** (at 0f); the **ADR-0106 supersession PLAN written** (the strike itself executes @ CEIR-12f, per CEIR-0h F3 — NOT at 0f) |
| 5 | final CEIR/CHIR/CKIR ownership map | ADR-0109 §2/§3 — the one-way layer contract + currencies (CHIR source semantics → CEIR execution → CKIR kernels by `KernelRef` identity) |
| 6 | exact executor migration inventory | CEIR-0a §3 (the 14 verbs, schema+`record_*` file:lines) + CEIR-0h §4 (the E1–E5 orchestration deletions) — the verbs PROMOTE, the C++ orchestration migrates |
| 7 | exact FrameGraph migration inventory | CEIR-0a §2 (one runtime, verified) + CEIR-0h §3 (F1–F3) + the §126 eight steps mapped to CEIR-12a–f |
| 8 | exact current program-asset inventory | CEIR-0a §6 (the five cookers' outputs: `.frame.toml`/`.crdt`/`.crdv`/`.crdl`/`.crdm`) + §7 (`.crdr` SHDR/VART) |
| 9 | current CKIR capabilities relevant to CEIR | CKIR is 100% of the engine, bit-exact, 6 backends (source: D-007's verified "⭐⭐ the engine is 100% CKIR (verified 2026-08-06)" section; frontier feature list §86); referenced by `KernelRef` identity (ADR-0109 §2), unchanged; the reduce/scan/sort/FFT/NRC kernels are the CEIR-10/19 proof corpus (CEIR-0a §5) |
| 10 | module dependency plan | ADR-0109 §4 — `crd-ceir` host-only (deps core/log/memory/containers/units); `crd-ceir-host`/`crd-ceir-gpu` bridges (dependency inversion); I3/I4/I5 gates |
| 11 | CEIR band order | the tracker: CEIR-1 core → 2 schema-gen → 3 types → 4 effects → 5 control-flow → 6 async → 7 asset/cook → 8 executor → 9 resource → 10 compute(first GPU) → 11 render → 12 framegraph → 13 executor-migration (pause lifts) → 14…32 |
| 12 | first implementation slice | **CEIR-1a** (`Context`/`Module`/`Operation`/`Value`/`Block`/`Region` with arena storage) — gated on ADR-0109 acceptance (✅ Accepted) |
| 13 | tests that will guard it | CEIR-1h fuzz + round-trip harness (built with the core); the I3/I4/I5 grep gates; the §167–§172 matrices as bands land; the reference-executor differential (CEIR-8b) |
| 14 | explicit deletion list | CEIR-0h §3–§6 — F1–3, E1–5, R1–2, M1, each with a parity gate named first; CEIR-31 executes it |
| 15 | design questions unresolved from code/research | §3 below |

## 2. Honest sizing — CEIR-1…13 (DERIVED; confidence marked; NOT smoothed)

⚠ **Read the confidence column, not just the numbers.** This is **compiler-infrastructure**, a different discipline
than the kernel work Cerid has excelled at — IR soundness fails in invariants (verifier confluence, serialization
stability), not flops, and the estimates below carry wider error bars than a kernel board would. The **band order +
dependency plan are certain**; the KLOC/duration are planning aids, not commitments. "Reuse" lowers a band; "first
GPU/live-renderer contact" raises its risk.

| Band | Scope (from the contract) | KLOC (eng+test) | In-tree anchor | Conf. | Risk driver |
|---|---|---|---|---|---|
| CEIR-1 | core IR + printer/parser + bytecode + builder + fuzz seed | 4–6 | `engine/kir/…/ckir.hpp` = 2,193 lines is the closest IR-core analog | **Med** | foundational surface; MLIR-core-lite |
| CEIR-2 | `*.ceirop.toml` schema + generator tool | 1.5–3 | the `gen_fft_batched.py` codegen precedent | Med-low | codegen tool correctness |
| CEIR-3 | types (scalar/aggregate/resource/shape/units/ownership/generics) | 3–5 | — | Med | generics + shape reasoning |
| CEIR-4 | effects + determinism + domains + hazard | 1.5–3 | the frame-graph WAR/lifetime analysis (ported) | Med | effect-analysis correctness |
| CEIR-5 | structured control flow + state + reference executor v0 | 2–4 | — | Med | the first executor |
| CEIR-6 | async/task + the `crd-ceir-host` jobs bridge | 1.5–3 | reuses `crd-jobs` (no new scheduler) | Med | mapping to fibers |
| CEIR-7 | asset/cook/hot-reload (reuses CRDR + RAF-11) | 2–3 | CRDR + the RAF-11 reloader | Med-high (reuse) | interface-hash split |
| CEIR-8 | reference executor (full) + compiled plan + profiler | 2.5–4 | crd-perf (no new profiler) | Med | the §153 no-alloc/no-string plan |
| CEIR-9 | resource/memory (reuses the interval aliaser) | 2–3 | the render-graph interval-coloring aliaser | Med-high (reuse) | planner correctness |
| CEIR-10 | compute+transfer + `crd-ceir-gpu` bridge + §129 proof | 3–5 | `frame_graph.cpp` `record_*` region ≈ 770 lines (the verb-lowering scale to match) | **Low** | ⚠ first GPU contact |
| CEIR-11 | render dialect (on RAH-1/2) + attachments + draws | 2.5–4 | `executor_registry.cpp` = 407 lines (the verb schemas) | **Low** | RAH-2 dep; both-backend |
| CEIR-12 | framegraph unification + per-asset A/B parity harness | 4–6 | `frame_template_bridge.cpp` = 796 lines (the existing desc→template frontend — CEIR-12a is the same shape) | **Low** | ⚠ touches the LIVE renderer |
| CEIR-13 | migrate the 9 `render()` blocks + the §128 scene.raster proof | 4–6 | `render()` ≈ 900 lines + the `init_programs` region | **Low** | ⚠ the pause-lift gate; the E1 ladder |
| **CEIR-1…13 total** | the substrate through the pause-lift | **≈ 34–55 KLOC** | **plausibility:** sits between `crd-geometry` (~22 KLOC) and v17's planned (~63–66 KLOC) — consistent for a substrate of this breadth | **Low-Med aggregate** | the four Low bands (10–13) dominate |

**Duration (banded, VERY-LOW confidence — the honest coarse estimate the 0z gate asks for, not a commitment).**
No precise figure (no in-tree precedent sizes the compiler-infra learning curve), but a banded range on named
assumptions:
- **CEIR-1…9 (host-only, device-free, independently testable):** the lower-risk block — order **~2–4 months** at
  focused pace; can move fast because everything is CPU-testable with the reference executor + fuzz harness.
- **CEIR-10…13 (GPU contact + the LIVE renderer):** where the schedule risk concentrates — order **~2–4 months**,
  dominated by the A/B parity harnesses (not new features) and both-backend proving; widen if RAH-2 slips.
- **Aggregate ≈ 4–8 months** to the pause-lift (CEIR-13z), **assuming:** RAH-1/2 lands in parallel (not serial), the
  compiler-infra discipline is absorbed without a major re-architecture, and no CEIR-10-era discovery forces a
  band-2/4 redesign (the strict-order risk). ⚠ Treat as an order-of-magnitude planning aid — the four Low-confidence
  bands could each move the total materially. This is the length of the "dark period" before broad feature work
  resumes as CEIR assets; it is the number to steer by, held loosely.

## 3. Design questions that could NOT be resolved from code/research (§184 item 15)

Carried forward to their bands (not blocking CEIR-1):

1. **`frame_runtime.cpp` fate** (CEIR-0h §8.1) — deleted outright vs retained as a thin cook→CEIR frontend. A
   CEIR-12 decision.
2. **Generic-fn × CKIR-kernel-variant** (CEIR-0e §9.5) — how CHIR monomorphization interacts with ADR-0104 VART
   selection. A CEIR-29 question, but the CEIR-10c `KernelRef` seam should not preclude it.
3. **Determinism: 5 classes ↔ 3 tiers** (CEIR-0g §3) — the §27 five-class ↔ ADR-0098 T1/T2/T3 alignment. Owned by
   CEIR-4b.
4. **Full-borrow expressiveness** (CEIR-0e §9.1) — does the light-borrow surface catch enough real dangling bugs, or
   does device-work/wavefront-PT demand more? Answered by the corpus at CEIR-29.
5. **Monomorphization vs dictionary-passing** for generics (CEIR-0e §9.2) — decided against measured corpus code
   size, CEIR-29.

None of these blocks the CEIR-1 start; each is docketed to the band that has the evidence to close it.

## 4. CEIR-0 close status

CEIR-0a ✅ · 0b/0c/0d ✅ ADRs Accepted · 0e ✅ direction accepted · 0g/0h/0z ◧ drafted. **On acceptance of 0g/0h/0z**,
CEIR-0's *design* is complete; the one remaining CEIR-0 slice is **0f** (the D-007 restructure — a mechanical doc
rewrite, to run after the accepted 0a–0z batch is committed). Then CEIR-1 opens (gated on ADR-0109, ✅ Accepted).
