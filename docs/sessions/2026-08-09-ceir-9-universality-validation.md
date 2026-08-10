# CEIR-9 — Universality Validation (the universality quest, part 2: PROOF, not features)

> Band 9 drives real mock-domains through the closed foundation (bands 1–8) to prove universality by open-world
> semantics. ⛔ Every 9x slice applies the **U-§97 failure criterion**: *did this domain require a second generic
> scheduler / compiler / runtime / dependency-graph?* If yes → EXTEND CEIR (a missing universal concept) or FORMALIZE a
> provider (legitimate specialization) — DOCUMENT WHICH; never hide duplication. The full 17-domain matrix + the
> U-§126 final answer land at CEIR-9z. Live state → `context.md`; the standing report → `docs/research/2026-08-09-ceir-universality-review.md`.

## CEIR-9a — U1 Notebook/incremental (no ADR) — 2026-08-09

**The proof (not a feature).** A notebook/reactive-recompute workload runs on the CEIR foundation with ZERO new
machinery — the 8h `crd::containers::IncrementalDag` IS the dirty engine, 8d stable ids ARE cell identity, 8i
transactions ARE the edit path. The cells are REAL CEIR arith ops (const/addi/muli); deps are SSA operands.

**The design fork (advisor-first): content = formula hash, interface = computed-value hash.** This is the load-bearing
insight: each cell's 8h CONTENT revision hashes its FORMULA (op kind + operand DEP stable-ids + attrs — reorder- and
id-INDEPENDENT, so a reorder/rename is a cache HIT); its INTERFACE revision hashes its computed VALUE. ⭐ That mapping
makes the §107 rule the notebook **early-cutoff**: a formula edit that preserves the value hot-swaps its dependents
(they never recompute). This is what elevates 9a above a re-run of `test_incremental_dag` (which used synthetic
revisions) — here the revisions come from a real evaluator.

**Every edit rides a transaction (the 8i→8h seam).** `tx.set_attr` (change a const) / `tx.set_operand` (swap operands)
→ commit → `tx.touched()` seeds the incremental eval. ⭐ A no-op edit (set a value to its current value) reports the
cell as touched but its content hash is unchanged → the content-addressed memo recomputes NOTHING: the transaction
says WHAT was edited; the hash decides WHETHER work happens.

**The U-§97 verdict — NO second engine, as a DEMONSTRATED boundary (advisor-approved, three parts):** (1) single-level
cutoff IS the engine — `recompute_after_change` with an unchanged interface returns no dependents; (2) the CHAINED
driver is necessarily consumer-side because chaining requires re-evaluation and the engine holds NO results by the 8h
division of labour — it structurally cannot chain; (3) the consumer's direct-dependents map is an inverted index that
mirrors the engine's own dep edges — no dependency information exists outside the engine. The generic machinery (topo
order, dep structure, revision storage, the §107 rule) is ALL `IncrementalDag`; the consumer is ~100 lines of
evaluate/compare/propagate glue.

**The content/interface generalization (honest).** In the COOK reading (8h) interface is a projection of content; in
the NOTEBOOK reading interface = the computed value, which legitimately changes while content (the formula) does not
during propagation (`set_revision(C, sameContent, newInterface)` is routine). The engine compares `u64`s and is
AGNOSTIC to the reading — that generality is itself U-§97 evidence (one engine serves both), stated so it does not
later read as a semantic bug someone "fixes."

**Advisor landmines, all landed:** value-hash is nonzero (a cell computing `A*0 = 0` must be distinguishable from the
dag's unset-0 default — the chained-cutoff node exercises exactly this trap); the engine's structural dependent set is
asserted a CONSERVATIVE SUPERSET of the evaluator's precise set with the cut node (`F`) named; transactions drive ALL
edits including the no-op; position-independence asserted directly (two identical-formula cells hash equal regardless
of id/position — no contorted cell-move, which has no tx primitive).

**Two near-misses (observations, NOT gaps):** no per-op content-hash PRIMITIVE (the consumer computes the formula hash
from the public op surface — correct at cell granularity); no direct-dependents accessor on the dag (derived from the
same edges). **Named-forward:** multi-cell edits in a SINGLE transaction (touched-set > 1 seeding the eval) are
untested here — that is 9d/9g territory where transactions are the headline.

**Tests.** `test_notebook.cpp` (7 `[ceir][notebook]`): initial eval once-each; edit an input → exactly its
value-dependents with the `A*0` chained cutoff (`F` cache-hits); the engine-conservative-superset ⊇ evaluator-precise
assertion (`F` the named cut node); edit a different input → a precise partial set (a non-dependent mid-node
cache-hits); a value-preserving operand swap → the single-level §107 cutoff; a no-op tx edit → zero recomputes;
identical formulas hash equal regardless of cell id/position. Exact counts, not "≥".

**Gate.** crd-ceir-tests + host + cook **327/327 ctest** (320 + 7) on win-debug + win-asan + linux-gcc-debug +
linux-gcc-asan (the money config proving the notebook + engine memory-safe) + LLVM-20 tidy (test_notebook.cpp) + GCC
`-Werror=switch` + opgen drift/validator + `crd-ceir-invariants`. **NO recook (a proof serializes nothing).** NEXT =
CEIR-9b (U2 DAW/timeline).

## Proposed commit — CEIR-9a (user commits; NO AI trailer; NO ADR — a proof composes; NO new serialized record)

```
test(ceir-9a): U1 notebook/incremental proof -- a reactive workload on the foundation, no second engine

- BAND 9 (universality validation) opens. A notebook/reactive-recompute workload runs on the CEIR
  foundation with ZERO new machinery: 8h IncrementalDag IS the dirty engine, 8d stable ids ARE cell
  identity, 8i transactions ARE the edit path. Cells are REAL CEIR arith ops (const/addi/muli),
  deps are SSA operands.
- The load-bearing mapping: content revision = a cell's FORMULA hash (op kind + operand dep
  stable-ids + attrs, reorder/id-INDEPENDENT), interface revision = its computed-VALUE hash -> the
  section-107 rule becomes the notebook EARLY-CUTOFF (a formula edit that preserves the value
  hot-swaps its dependents). Every edit rides a transaction; a no-op edit's unchanged content hash
  yields zero recomputes (the content-addressed memo -- the tx says WHAT changed, the hash decides
  WHETHER work happens).
- U-97 verdict: NO second engine, DEMONSTRATED -- single-level cutoff IS the engine; the chained
  driver is consumer-side because the engine holds no results by the 8h division (it cannot chain);
  the direct-dependents map mirrors the engine's edges. The generic machinery (topo/deps/revisions/
  section-107) is all IncrementalDag; the consumer is ~100 lines of glue.
- test_notebook.cpp (7 [ceir][notebook]): exact recompute counts across initial eval, input edits
  (with the A*0 chained cutoff), a value-preserving swap (single-level cutoff), a no-op memo, the
  engine-conservative-superset assertion (F the named cut node), and position-independence.
- Review doc: the CEIR-9a proof narrative + the section-I domain-matrix row (2nd runtime? NO).

Gated: crd-ceir-tests + host + cook 327/327 on win-debug + win-asan + linux-gcc-debug +
linux-gcc-asan + LLVM-20 tidy + GCC -Werror=switch + opgen drift/validator + crd-ceir-invariants.
No recook; no ADR; no new serialized record.
```

## CEIR-9b — U2 DAW/timeline (no ADR) — 2026-08-09

**The proof.** A DAW/audio graph runs on the CEIR foundation with ZERO new machinery. The mock `audio` dialect is
INLINE-registered (zero central-enum edits — the open-world proof): `source`/`gain`/`mix` (RT-safe), `delay` (carrying
`OpTrait::StateEdge`), `load_sample` (a FileIO effect), `scratch_alloc` (an Allocate effect), `graph` (the
region-owner), each with EXPLICIT determinism + domain axes (the 4z Unspecified-default trap applies to proof
fixtures). Four foundation surfaces compose in one audio graph: **8f time**, **5d state/feedback**, **4c region
legality**, **8e op-interfaces**.

**The U-§97 verdict — NO second engine (advisor-approved, three parts):** (1) the feedback discipline keys on the
**trait, never a kind name** — the plugin `audio.delay` merely carries `OpTrait::StateEdge`, so the 5d
`find_structure_error` cycles-only-through-state rule exempts its last operand exactly as it would `core.delay`; the
proof routes the feedback through the PLUGIN op ONLY (no `core.delay`) to make the point. A combinational `gain` loop
is `FeedbackWithoutState`, offender pointed. (2) offline-vs-realtime is a region-TAG property of ONE module object —
`set_region_exec(graph, audio_rt)` → `find_domain_violation` flags the disk load; retag offline → clean; retag
realtime → flagged again, all on the SAME pointer (the no-second-graph proof, U-§38). (3) latency is a typed
`LatencyInterface` (8e) with EMPTY≠UNKNOWN — a missing interface makes the chain latency UNKNOWN, never silently 0. The
two-RT-oracles subset is pinned by THREE data points: `gain` (legal+safe), `scratch_alloc` (legal-but-UNSAFE — the
witness the converse fails), `load_sample` (illegal+unsafe); `realtime_safe ⟹ legal` holds, the converse does not.

⭐ **Multi-rate assessed, not waved (the candidate foundation gap):** distinct sample rates are expressible NOW as
distinct time type-classes (or a rate param) — mixing rates is a type error the moment the operand checker looks (the
`wall != sim` property applied to `44.1k != 48k`); a **resample** op is an ORDINARY op whose operand and result live in
different time domains; rate-match ENFORCEMENT rides the SAME operand-type checker already named-forward from 8f. No
new time model. Sidechain = an extra operand; sub-sample = a fractional underlying.

**Honest boundaries (NOT gaps):** latency was proven at op-KIND level (querying each kind's interface with
unknown-propagation — the U-§39 substance); a graph-WALKING accumulator (`first_op`/`next_in_block`, summing each op's
kind) is trivial consumer glue, not exercised here. The delay fixture's `init` operand is a source result, not a
time-typed constant — structurally fine, a legality/feedback probe, not a numeric evaluator.

**Tests.** `test_daw.cpp` (5 `[ceir][daw]`): sample-accurate time ≠ wall + a plugin `tempo.beat` clock; feedback
through the plugin delay legal vs a combinational loop rejected (offender pointed); a disk-load flagged in audio-RT +
the offline tag-flip of the SAME module legal + flip-back flagged; the two-oracles subset (3 points); latency summed
per op-kind + a missing interface ⇒ unknown.

**Gate.** crd-ceir-tests + host + cook **332/332 ctest** (327 + 5) on win-debug + win-asan + linux-gcc-debug +
linux-gcc-asan + LLVM-20 tidy (test_daw.cpp) + GCC `-Werror=switch` + opgen drift/validator + `crd-ceir-invariants`.
**NO recook; no ADR; no new serialized record.** NEXT = CEIR-9c (U3 DCC modifier graph).

## Proposed commit — CEIR-9b (user commits; NO AI trailer; NO ADR — a proof composes; NO new serialized record)

```
test(ceir-9b): U2 DAW/timeline proof -- an audio graph on the foundation, no second engine

- A DAW/audio graph runs on the foundation with ZERO new machinery. The mock `audio` dialect is
  INLINE-registered (zero central edits): source/gain/mix (RT-safe), delay (OpTrait::StateEdge),
  load_sample (FileIO effect), scratch_alloc (Allocate effect), graph (region-owner), each with
  explicit determinism+domain axes.
- Four foundation surfaces compose: 8f time (time.audio_sample<T> distinct from wall + a plugin
  tempo.beat clock), 5d state (feedback = the plugin delay's last operand; the cycles-only-through-
  state verifier accepts it, rejects a combinational gain loop), 4c legality (find_domain_violation
  flags the disk load in an audio-RT region), 8e interfaces (latency = a typed query).
- U-97 verdict: NO second engine -- (1) the feedback discipline keys on the TRAIT not a kind name
  (a plugin delay is exactly as legal as core.delay, proven via the plugin-only path); (2) offline
  is an in-place region-tag flip on ONE module object (flag->retag->clean->retag->flag, no second
  graph); (3) latency is a typed interface with EMPTY!=UNKNOWN. The two-oracles subset pinned by
  three points (realtime_safe => legal, the allocating op the converse-witness).
- Multi-rate assessed: distinct rates = distinct time domains, resample = an ordinary op, rate-match
  enforcement rides the named-forward operand checker -- no new time model.
- test_daw.cpp (5 [ceir][daw]). Review doc: the CEIR-9b proof narrative + the section-I DAW row.

Gated: crd-ceir-tests + host + cook 332/332 on win-debug + win-asan + linux-gcc-debug +
linux-gcc-asan + LLVM-20 tidy + GCC -Werror=switch + opgen drift/validator + crd-ceir-invariants.
No recook; no ADR; no new serialized record.
```

## CEIR-9c — U3 DCC modifier graph (no ADR) — 2026-08-09

**The proof.** A Blender-class modifier stack runs on the foundation with ZERO new machinery — the 8h
`IncrementalDag::affected_by` IS the depsgraph. The mock `dcc` dialect is INLINE-registered (base_mesh / subdivide /
transform / boolean / deform / output, explicit determinism+domain axes); a "mesh" is a mock poly count. The topology
is a DAG with a JOIN over TWO SEPARATE-source branches (base→subdivide and cutter→transform meet at
boolean→deform→output — the branches share no op).

⭐ **The differentiator from 9a — the U-§97 sentence this pair uniquely earns: TWO DRIVERS, ONE ENGINE, NO CODE
MOTION.** 9a PUSHED forward from a seed (Salsa-style: recompute, compare, push direct dependents). 9c PULLS —
`affected_by(M)` computes the full invalidation set UPFRONT (Blender's TAG pass), then ONE topo walk evaluates only
tagged nodes, early-outing any whose inputs all turned out unchanged (Blender's "no update needed"). The engine changed
by ZERO lines between the two proofs. That is stronger U-§97 evidence than either alone — a **9z synthesis input**.

**The U-§97 verdict — NO second depsgraph (advisor-approved, three parts):** (1) the TAG pass IS the engine —
`affected_by(edited)` is the invalidation set, asserted EXACT both directions on the subdivide and deform edits, and
excluding the sibling branch on the cutter edit (precise upstream isolation); (2) the eval walk is necessarily consumer
POLICY because the early-out requires re-evaluation and the engine holds NO results by the 8h division (per the 9a
verdict, not re-derived); (3) the consumer's `deps[]` mirrors the engine's own edges (the operand walk records the same
edges `add_edge` did). The §107 early-out is proven by a topology-preserving deform edit: its formula changes, its
output mesh does not, so its tagged dependent `output` is the NAMED cut.

⭐ **Topology-vs-geometry facets (assessed, not demonstrated).** The mock's "topology" is the whole mesh value (one
scalar poly count), so the early-out proves the §107 MECHANICS but not genuine facet separation. A facet (a downstream
caring only about topology) is an ORDINARY additional output node; dependency granularity = which node you edge to (the
9b resample/multi-rate shape); enforcement rides the same named-forward operand checker. No new engine. Modifier
REORDER (a structural stack edit) is named-forward — the 8i structural-edit tx path 8i itself named-forward.

**Doc precision:** tag-set exactness was asserted on the subdivide and deform edits; the cutter edit pinned its sets via
the exclusion check (subdivide ∉ `affected_by(cutter)`) plus all-seven recompute counts.

**⚠ Forward note (test-helper hoist due at 9d):** the `fnv_mix`/`content_hash`/`value_hash`-shaped helpers now live in
BOTH `test_notebook.cpp` (9a) and `test_dcc.cpp` (9c). 9d (CAD) will want a third copy — hoist them into a shared test
header AT 9d (the third consumer is when the absorb-with-a-real-consumer discipline says to unify, not before).

**Tests.** `test_dcc.cpp` (5 `[ceir][dcc]`): initial-once; a subdivide edit (tag set == eval set, exact both ways); a
cutter-branch edit (the subdivide branch cache-hits — precise upstream isolation); a topology-preserving deform edit
(tag set ⊃ eval set, `output` the named cut); a no-op tx edit → zero recomputes.

**Gate.** crd-ceir-tests + host + cook **337/337 ctest** (332 + 5) on win-debug + win-asan + linux-gcc-debug +
linux-gcc-asan + LLVM-20 tidy (test_dcc.cpp) + GCC `-Werror=switch` + opgen drift/validator + `crd-ceir-invariants`.
**NO recook; no ADR; no new serialized record.** NEXT = CEIR-9d (U4 CAD parametric — the 8i transaction is the
HEADLINE: multi-op edits in one transaction + a mid-edit rollback leaving the feature graph byte-identical).

## Proposed commit — CEIR-9c (user commits; NO AI trailer; NO ADR — a proof composes; NO new serialized record)

```
test(ceir-9c): U3 DCC modifier-graph proof -- affected_by IS the Blender depsgraph, no second engine

- A Blender-class modifier stack runs on the foundation with ZERO new machinery: the 8h
  IncrementalDag affected_by IS the depsgraph, 8d stable ids ARE modifier identity, 8i transactions
  ARE the parameter-edit path. Mock `dcc` dialect INLINE-registered (base_mesh/subdivide/transform/
  boolean/deform/output); a mesh = a mock poly count; a DAG with a JOIN over two SEPARATE-source branches.
- ⭐ The differentiator from 9a -- TWO DRIVERS, ONE ENGINE, NO CODE MOTION: 9a pushed forward from a
  seed (Salsa); 9c pulls -- affected_by(M) computes the invalidation set UPFRONT (Blender's tag pass),
  then one topo walk evaluates only tagged nodes, early-outing unchanged inputs. The engine changed
  by zero lines between the two proofs (a 9z synthesis input).
- U-97 verdict: NO second depsgraph -- (1) the tag pass IS the engine (affected_by asserted exact
  both ways, sibling branch excluded -> precise upstream isolation); (2) the eval walk is consumer
  policy (early-out needs re-eval, the engine holds no results); (3) deps[] mirrors the engine's edges.
  The section-107 early-out proven by a topology-preserving deform edit (output the named cut).
- Facets assessed (a facet = an extra output node, the 9b resample shape -- not a gap); modifier
  reorder named-forward (the 8i structural-edit tx path).
- test_dcc.cpp (5 [ceir][dcc]). Review doc: the CEIR-9c proof narrative + the section-I DCC row.

Gated: crd-ceir-tests + host + cook 337/337 on win-debug + win-asan + linux-gcc-debug +
linux-gcc-asan + LLVM-20 tidy + GCC -Werror=switch + opgen drift/validator + crd-ceir-invariants.
No recook; no ADR; no new serialized record.
```

## CEIR-9d — U4 CAD parametric + transaction (no ADR) — 2026-08-09

**The proof.** A parametric-CAD feature graph runs on the foundation with ZERO new machinery, and the **8i TRANSACTION
is the headline**. The mock `cad` dialect is INLINE-registered (sketch/extrude/assembly/fillet/body); a two-feature
assembly (sketch1→extrude1, sketch2→extrude2 join at assembly→fillet→body); a "geometry" is a mock scalar.

**The test-helper hoist (the riskiest edit, sequenced first).** The `fnv_mix`/`content_hash`/`value_hash`/`set_eq`
helpers, duplicated in 9a + 9c, were hoisted into `tests/ceir/incremental_helpers.hpp` (namespace `crd::ceir::test`,
`inline`, self-contained) at this THIRD consumer — the absorb-with-a-real-consumer discipline (unify at the third
consumer, not before). 9a + 9c were refactored onto it and the FULL ceir suite proven neutral at **337/337 before a
line of test_cad.cpp existed** — so any later failure is 9d's own. ⚠ **The 9c forward-note is DISCHARGED.**

**The U-§97 verdict — NO second transaction-manager / depsgraph / constraint-solver (advisor-approved, three parts):**
(1) **multi-op atomicity IS 8i, unmodified** — one transaction editing several dimensions returns a `touched()` set with
MULTIPLE stable ids; the eval seeds from the UNION of the affected subtrees (consumer glue over `affected_by`), so BOTH
feature branches re-evaluate where a single-dimension edit leaves the sibling branch cached. **The 9a
multi-cell-per-transaction named-forward is now CLOSED** — and a no-op edit inside a multi-op transaction recomputes only
the real edit's subtree (multi-op UNION × content-addressed memo, a composition no prior slice had; the driver checks
content for EVERY seed). (2) **a CAD constraint is TWO existing surfaces doing their own jobs, zero new mechanism** — the
8c `ConstraintRead` effect is CLASSIFICATION, the sketch's `VerifyFn` (run by the transaction's existing commit-verify)
is ENFORCEMENT; a violating commit is REJECTED (diagnostic `ceir.transaction.verify_failed` → rollback → byte-identical,
dimensions restored), a satisfying one succeeds. (3) a bidirectional constraint SOLVER is an ordinary fixpoint NODE (a
solve is a computation, not a new dependency-edge model); the feature graph stays one-way; enforcement granularity rides
the same named-forward operand checker.

⭐ **The CAD argument for transactions as the mutation surface (the insight this slice uniquely earns).** The satisfying
edit (width=height=4 → 6,6) necessarily transits a VIOLATING intermediate state (width=6, height=4 after the first
`set_attr`) and commits ANYWAY, because verification is ATOMIC at commit. A constrained model CANNOT be edited
dimension-by-dimension without passing through inconsistent states — so enforcement MUST be transactional, and 8i gives
exactly that for free. That is why the transaction is the right authored-mutation surface — demonstrated, not asserted.

**The mid-slice failure (honest fixture hygiene, not a weakening).** Two tests first went red because the constraint
enforcement WORKED on an unintentionally-constrained fixture (sketch1 carried the equal-constraint, so a plain
width-edit legitimately failed commit). The fix — unconstrained sketch by default, the constraint activated in its own
test — is fixture hygiene, and it made the constraint proof cleaner.

**Doc precision:** the single-dimension-edit case (test 489) pins behavior by recompute COUNTS; tag-set exactness is the
9c contract, cited not re-proven. The byte-identical rollback reuses the 8i money-test shape (serialize A/B).

**Tests.** `test_cad.cpp` (5 `[ceir][cad]`): a single dimension edit (partial re-eval, cite 9c); MULTI-OP edits in one
transaction (both branches re-eval, a FRESH-fixture control showing the sibling cached); a no-op edit inside a multi-op
transaction (only the real edit's subtree); a byte-identical rollback (serialize A/B + dimensions read back); a
constraint-violating commit rejected (verify-fail → rollback + the diagnostic) then a satisfying commit succeeds.

**Gate.** crd-ceir-tests + host + cook **342/342 ctest** (337 + 5) on win-debug + win-asan + linux-gcc-debug +
linux-gcc-asan + LLVM-20 tidy (incremental_helpers.hpp + test_cad.cpp + the refactored test_notebook/test_dcc) + GCC
`-Werror=switch` + opgen drift/validator + `crd-ceir-invariants`. **NO recook; no ADR; no new serialized record.** NEXT =
CEIR-9e (U5 PCB/EDA + external provider).

## Proposed commit — CEIR-9d (user commits; NO AI trailer; NO ADR — a proof composes; NO new serialized record)

```
test(ceir-9d): U4 CAD parametric proof -- the 8i transaction as the mutation surface, no second engine

- A parametric-CAD feature graph runs on the foundation with ZERO new machinery; the 8i TRANSACTION
  is the headline. Mock `cad` dialect INLINE-registered (sketch/extrude/assembly/fillet/body); a
  two-feature assembly; a mock scalar geometry.
- Hoist the shared incremental-proof helpers into tests/ceir/incremental_helpers.hpp at the third
  consumer (test_notebook.cpp [9a] + test_dcc.cpp [9c] refactored onto it, proven neutral at 337/337
  before a line of the CAD proof).
- U-97 verdict: NO second transaction-manager/depsgraph/constraint-solver -- (1) multi-op atomicity
  IS 8i unmodified (a multi-id touched() seeds the eval from the UNION of affected subtrees; the 9a
  multi-cell-per-transaction named-forward is now CLOSED; a no-op inside a multi-op tx recomputes
  only the real subtree); (2) a CAD constraint is two existing surfaces -- 8c ConstraintRead effect
  = classification, the sketch VerifyFn via commit-verify = enforcement (a violating commit rejected
  with ceir.transaction.verify_failed -> rollback byte-identical); (3) a constraint solver is an
  ordinary fixpoint node (the graph stays one-way).
- The CAD argument for transactions: the satisfying edit (4,4->6,6) transits a VIOLATING intermediate
  (6,4) and commits anyway because verify is ATOMIC at commit -- a constrained model can't be edited
  dimension-by-dimension without inconsistent states, so enforcement must be transactional.
- test_cad.cpp (5 [ceir][cad]). Review doc: the CEIR-9d proof narrative + the section-I CAD row.

Gated: crd-ceir-tests + host + cook 342/342 on win-debug + win-asan + linux-gcc-debug +
linux-gcc-asan + LLVM-20 tidy + GCC -Werror=switch + opgen drift/validator + crd-ceir-invariants.
No recook; no ADR; no new serialized record.
```

## CEIR-9e — U5 PCB/EDA + external provider (no ADR) — 2026-08-09

**The proof.** A PCB/EDA board pipeline (board_document → drc → route → gerber_export) runs on the foundation with ZERO
new machinery; its EXTERNAL TOOLS are ordinary capability-bearing ops. The mock `eda` dialect is INLINE-registered (zero
central edits). `test_eda.cpp` (4 `[ceir][eda]`).

**The U-§97 verdict — NO second provider / capability system (advisor-approved, three parts):** (1) **an external tool
IS an ordinary capability-bearing op** — 8f capabilities + 4a effects + 4b determinism + a content-addressed `tool` attr
form the complete TYPED CONTRACT, queried generically with zero hard-coded op knowledge; `capabilities_satisfied` is the
sandbox boundary (a GPU-only host PROVABLY cannot run the pipeline). The runtime DISPATCH is the `IExecutionProvider`
seam, named-forward CEIR-24/29 — the contract is proven, the execution is not claimed. The `tool` identity is
content-addressed (two boards differing only in the router `tool` attr hash differently — a re-route is a different
program). (2) **⛔ The op-LOCAL commit-verify boundary (the finding this slice earns, stated not softened):** 8i's
commit-verify runs on TOUCHED ops only, so a design rule fires iff its carrier op is edited — a rule ON the board
catches board edits (the DRC here), but a whole-board DRC SWEEP (the real-EDA shape) needs a `find_*_violation`-style
walk or a pass over the committed module (consumer-band machinery, named-forward to 9g's post-commit verifier sweep +
CEIR-26). Op-local is the CORRECT commit-time granularity; module-wide is a different pass that COMPOSES on top — a
boundary of what was proven, not a gap in 8i. (3) **U-§34, DEMONSTRATED:** a program-level walk over every op unions
Document/Constraint/ExternalCall/FileIO families and finds NO `GPUCommand` effect, NO `DeviceTime` domain, NO
`gpu.compute` capability — **the vocabulary was domain-neutral before this domain arrived.**

**Gaps assessed (all expressible, none real):** a typed ERROR contract is an 8g Diagnostic emitted by the runtime
dispatch (named-forward WITH the dispatch); streaming export is an op/token; a bidirectional tool is Document read+write
effects.

**⭐ Running tally for the 9z gate.** FIVE domains in (9a notebook, 9b DAW, 9c DCC, 9d CAD, 9e EDA), the per-slice
U-§97 answer has been **NO every time — zero foundation edits, zero engine-line changes**. The 9z gate should tally the
edits-to-foundation column across 9a–9h explicitly (all zeros so far); the two strongest cross-slice claims are 9c's
"two drivers, one engine" (push vs pull over one `IncrementalDag`) and 9e's "the vocabulary was domain-neutral before
the domain arrived."

**Tests.** `test_eda.cpp` (4 `[ceir][eda]`): the host-grant contract (a GPU-only host cannot run the pipeline +
EMPTY≠UNKNOWN in a separate module); the queryable typed contract + content-addressed provenance (h1≠h2 on a `tool`
change); a design-rule violation rejected by the board verifier at commit (op-local); the U-§34 program-level
domain-neutral walk.

**Gate.** crd-ceir-tests + host + cook **346/346 ctest** (342 + 4) on win-debug + win-asan + linux-gcc-debug +
linux-gcc-asan + LLVM-20 tidy (test_eda.cpp) + GCC `-Werror=switch` + opgen drift/validator + `crd-ceir-invariants`.
**NO recook; no ADR; no new serialized record.** NEXT = CEIR-9f (U6 Game/ECS effects).

## Proposed commit — CEIR-9e (user commits; NO AI trailer; NO ADR — a proof composes; NO new serialized record)

```
test(ceir-9e): U5 PCB/EDA proof -- external tools as capability-bearing ops, domain-neutral vocabulary

- A PCB/EDA board pipeline (board_document -> drc -> route -> gerber_export) runs on the foundation
  with ZERO new machinery; external tools are ordinary CAPABILITY-bearing ops. Mock `eda` dialect
  INLINE-registered (zero central edits).
- U-97 verdict: NO second provider/capability system -- (1) an external tool is an ordinary
  capability-bearing op: 8f caps + 4a effects + 4b determinism + a content-addressed `tool` attr =
  the typed contract, queried generically; capabilities_satisfied is the sandbox boundary (a GPU-only
  host provably cannot run the pipeline); runtime dispatch = IExecutionProvider, named-forward 24/29.
- (2) the op-LOCAL commit-verify boundary (named, not softened): 8i commit-verify runs on TOUCHED ops
  only, so a rule fires iff its carrier is edited -- the DRC is the board's verifier; a whole-board
  DRC sweep needs a find_*_violation walk / a pass (named-forward to 9g + CEIR-26). A boundary, not a gap.
- (3) U-34 DEMONSTRATED: a program-level walk finds Document/Constraint/ExternalCall/FileIO families
  but NO GPUCommand effect / DeviceTime domain / gpu.compute cap -- the vocabulary was domain-neutral
  before this domain arrived.
- test_eda.cpp (4 [ceir][eda]). Review doc: the CEIR-9e proof narrative + the section-I EDA row.

Gated: crd-ceir-tests + host + cook 346/346 on win-debug + win-asan + linux-gcc-debug +
linux-gcc-asan + LLVM-20 tidy + GCC -Werror=switch + opgen drift/validator + crd-ceir-invariants.
No recook; no ADR; no new serialized record.
```

## CEIR-9f — U6 Game/ECS effects (no ADR) — 2026-08-09

**The proof.** ECS system parallelism runs on the foundation with ZERO new machinery — the 4d hazard analysis
(`ops_hazard` / `collect_block_hazards`) over 4a `EcsRead`/`EcsWrite` effects IS the parallelism oracle. The mock `ecs`
dialect is INLINE-registered (movement/render/physics/input/spawn/coarse). `test_ecs.cpp` (4 `[ceir][ecs]`).

**The U-§97 verdict — NO second scheduler / parallelism analysis (advisor-approved, three parts):** (1) **ECS
parallelism inference IS the 4d hazard analysis over declared effects** — `ops_hazard`/`collect_block_hazards`
UNMODIFIED; no scheduler was built (the scheduler CONSUMES these edges at CEIR-12d, named-forward). Disjoint components
(a Position writer vs a Health writer) → NO hazard → parallel; a shared component with ≥1 write → ordered (exact
RAW/WAR/WAW); the compiler derives the FULL 6-edge ordering set from declared effects alone, the four disjoint pairs
edgeless. (2) **Per-component identity IS the SSA Value** — each component is a distinct block-arg Value, a system's
effect targets an OPERAND (the canonical 4d resource model, not a workaround). The 8c `EcsComponent` location KIND was
exercised ONLY as the conservative whole-class fallback (a `coarse` system conflicts with EVERY Ecs access, even
mutually-parallel ones — safe-but-pessimal, MORE hazards never fewer; EMPTY≠UNKNOWN applied to location identity;
precision is opt-in via the operand). (3) A structural mutation needs NO special mechanism — a whole-store write (a null
resource) is the barrier by the existing nullptr conflict rule.

**⛔ The one foundation change any proof has forced: a STALE forward-claim corrected.** The 4d source comment
(`op_access_at`) said the whole-class location fallback was "named-forward to 8d" — but 8d shipped per-OP stable
identity, NOT per-LOCATION identity; the pointer was never fulfilled (the 8z "never inherit a stale forward-claim into a
verdict" scar). The comment was fixed IN PLACE to the honest unbound form (a deliberate conservative fallback; per-
instance location identity is an unbound future refinement, explicitly NOT bound to a fake band). A comment-only
crd-ceir source edit, re-gated across 4 configs (all 3 targets rebuilt).

**The mid-slice failure upgraded the proof.** The `render → spawn` edge was first asserted RAW; it is WAR (render READS
before spawn WRITES → read-then-write). The exact-kind discipline (the 4z precedent) caught the direction error — a
`!= None` assertion would have passed silently over the wrong mental model. The fix pins the spawn↔render pair in BOTH
directions (RAW when spawn precedes, WAR when render precedes), proving the analysis is genuinely directional. The
failure was the assertion discipline working.

**⭐ Running tally for the 9z gate.** SIX domains in (9a notebook, 9b DAW, 9c DCC, 9d CAD, 9e EDA, 9f ECS), the
per-slice U-§97 answer has been **NO every time — zero engine-line changes; the ONLY foundation change any proof has
forced is correcting one stale comment.** The 9z gate should tally the edits-to-foundation column across 9a–9h (all
zeros bar the one comment); the strongest cross-slice claims: 9c "two drivers, one engine", 9e "the vocabulary was
domain-neutral before the domain arrived", and this slice's "the parallelism oracle already existed".

**Tests.** `test_ecs.cpp` (4 `[ceir][ecs]`): disjoint systems parallel + a shared component with a write ordered (exact
kinds); a whole-store structural barrier against every system; the full 6-edge `collect_block_hazards` set + the four
disjoint pairs absent; the `EcsComponent` conservative whole-class fallback pinned (coarse-vs-precise contrast).

**Gate.** crd-ceir-tests + host + cook **350/350 ctest** (346 + 4) on win-debug + win-asan + linux-gcc-debug +
linux-gcc-asan (all 3 targets rebuilt for the context.cpp comment fix) + LLVM-20 tidy (test_ecs.cpp + context.cpp) +
GCC `-Werror=switch` + opgen drift/validator + `crd-ceir-invariants`. **NO recook; no ADR; no new serialized record.**
NEXT = CEIR-9g (U7 Agent transaction — the full agent loop).

## Proposed commit — CEIR-9f (user commits; NO AI trailer; NO ADR — a proof composes; NO new serialized record)

```
test(ceir-9f): U6 Game/ECS proof -- the 4d hazard analysis IS the parallelism oracle, no scheduler

- ECS system parallelism runs on the foundation with ZERO new machinery: the 4d hazard analysis
  (ops_hazard/collect_block_hazards) over 4a EcsRead/EcsWrite effects IS the parallelism oracle.
  Mock `ecs` dialect INLINE-registered (movement/render/physics/input/spawn/coarse).
- U-97 verdict: NO second scheduler -- (1) ops_hazard/collect_block_hazards UNMODIFIED (disjoint
  components -> parallel, shared+write -> ordered with exact RAW/WAR/WAW; the full 6-edge set from
  effects alone; the scheduler consumes these edges at CEIR-12d, named-forward); (2) per-component
  identity IS the SSA Value (Operand target, canonical 4d); the 8c EcsComponent KIND is exercised
  only as the conservative whole-class fallback (safe-but-pessimal, EMPTY!=UNKNOWN for location
  identity); (3) a structural mutation (whole-store write, a null resource) is the barrier by the
  existing nullptr conflict rule.
- ⛔ Corrected a STALE forward-claim in engine/ceir/src/context.cpp: the 4d op_access_at comment
  said the whole-class location fallback was "named-forward to 8d", but 8d shipped per-OP identity,
  not per-LOCATION identity -- fixed in place to the honest unbound form (a comment-only edit).
- test_ecs.cpp (4 [ceir][ecs]). Review doc: the CEIR-9f proof narrative + the section-I ECS row.

Gated: crd-ceir-tests + host + cook 350/350 on win-debug + win-asan + linux-gcc-debug +
linux-gcc-asan (all 3 targets rebuilt for the comment fix) + LLVM-20 tidy + GCC -Werror=switch +
opgen drift/validator + crd-ceir-invariants. No recook; no ADR; no new serialized record.
```

## CEIR-9g — U7 Agent transaction (no ADR) — 2026-08-09 — the CULMINATION of band 9

**The proof.** The FULL agent loop runs on the foundation with ZERO new machinery. A mock agent DISCOVERS ops via the
machine-readable `OpSchema` registry (the header's own "CLI/MCP/agent discovery (section 122)" surface; `.ops.json` is
its OUT-of-process serialization, and NO no-std JSON reader exists in the house → the in-process
`<dialect>_op_schemas()` tables ARE the registry). ⛔ ZERO hard-coded op knowledge: the agent SELECTS by property
(arity + result count), interns via the discovered `schema.dialect`/`name`, and `op_name(kind) == schema.qualified`
closes the loop. `test_agent.cpp` (3 `[ceir][agent]`).

**The discovery-surface fork (resolved with the advisor, before code):** `.ops.json` JSON-parse vs `OpSchema` static
tables. Grep confirmed NO no-std JSON reader exists → writing one is out of scope → the OpSchema tables are the surface
(the same registry `.ops.json` is generated from; no parser needed).

**The U-§97 verdict — NO second authoring / verification / diagnostic / discovery system (advisor-approved, three
parts):** (1) the agent loop is a THIN composition over four EXISTING surfaces — OpSchema discovery + 8i authoring
(unmodified) + verification at two granularities + 8g diagnostics (read by CODE). (2) **The three-legged authoring
contract, layered HONESTLY.** The schema (arity + required attrs + kinds) + a chosen type + STRUCTURAL verify is
DEMONSTRATED (test 3: discovery-by-arity-alone is insufficient — a lazy agent authoring a source WITHOUT its
schema-required attr is caught by the op-local commit-verify backstop). SEMANTIC type validity is a FURTHER, BOUND
named-forward: the generated verifiers are structural, as their own source stamps ("Semantic verification
(types/effects/domain) lands at CEIR-3/4"); the full type-CONSTRAINT language is CHIR. ⛔ Faking a type-checking
verifier to fit the design-call assumption would have been the real failure — the proof adapted on primary evidence
(the generated verifiers are structural-only), which is what the design call was FOR. (3) **The op-local / module-wide
composition DEMONSTRATED, closing the 9e named-forward:** the agent runs a MODULE-WIDE `find_structure_error` SWEEP
MID-TX (8i applies eagerly, so the authored state is visible before commit) — it catches a defect NO op-local
commit-verify can see (a same-block forward reference → `FeedbackWithoutState`, a combinational feedback edge in a Graph
region, NOT UseBeforeDef — exact kind pinned) → the agent emits its OWN 8g diagnostic (`agent.sweep.structure_defect`,
carrying the offender's loc), reads it by code, and rolls back byte-identically; the op-local commit-verify is the
BACKSTOP behind the sweep (belt-and-suspenders).

**⭐ Running tally.** SEVEN domains in (9a–9g), U-§97 = NO every time — zero engine-line changes; the ONLY foundation
change any proof has forced is one stale-comment correction (9f). Strongest cross-slice claims for 9z: 9c "two drivers,
one engine", 9e "the vocabulary was domain-neutral before the domain arrived", 9f "the parallelism oracle already
existed", 9g "the agent is a thin composition — discovery + tx + verify + diagnose, no new systems".

**Tests.** `test_agent.cpp` (3 `[ceir][agent]`): discover-by-property + author + modify + mid-tx sweep clean + commit
(with `op_name == schema.qualified`); the module-wide-sweep defect (same-block forward ref → FeedbackWithoutState) →
agent diagnostic → byte-identical rollback; the op-local commit-verify backstop catches a lazy agent (a source without
its required attr → `verify_failed` → rollback).

**Gate.** crd-ceir-tests + host + cook **353/353 ctest** (350 + 3) on win-debug + win-asan + linux-gcc-debug +
linux-gcc-asan + LLVM-20 tidy (test_agent.cpp) + GCC `-Werror=switch` + opgen drift/validator + `crd-ceir-invariants`.
**NO recook; no ADR; no new serialized record.** NEXT = CEIR-9h (U8 the MANDATORY external-plugin proof).

## Proposed commit — CEIR-9g (user commits; NO AI trailer; NO ADR — a proof composes; NO new serialized record)

```
test(ceir-9g): U7 agent-transaction proof -- the full agent loop, a thin composition, no new systems

- The CULMINATION of band 9: the FULL agent loop runs on the foundation with ZERO new machinery.
  A mock agent DISCOVERS ops via the machine-readable OpSchema registry (no no-std JSON reader exists
  -> the in-process <dialect>_op_schemas() tables ARE the registry .ops.json serializes), ZERO
  hard-coded op knowledge (selects by property, interns via schema.dialect/name; op_name(kind) ==
  schema.qualified closes the loop) -> 8i tx author/modify -> verify at TWO granularities -> 8g
  diagnostics read by code -> commit/rollback byte-identical.
- U-97 verdict: NO second authoring/verify/diagnostic system -- (1) a thin composition over four
  existing surfaces; (2) the three-legged contract layered honestly: schema arity+attrs + chosen
  type + STRUCTURAL verify demonstrated, SEMANTIC type validity a bound named-forward (the generated
  verifiers are structural -- "Semantic verification lands at CEIR-3/4"); (3) the op-local/module-wide
  composition DEMONSTRATED (closing the 9e named-forward): the module-wide sweep catches a same-block
  forward reference (-> FeedbackWithoutState, exact kind) NO op-local verify sees; the op-local
  commit-verify is the backstop.
- test_agent.cpp (3 [ceir][agent]). Review doc: the CEIR-9g proof narrative + the section-I agent row.

Gated: crd-ceir-tests + host + cook 353/353 on win-debug + win-asan + linux-gcc-debug +
linux-gcc-asan + LLVM-20 tidy + GCC -Werror=switch + opgen drift/validator + crd-ceir-invariants.
No recook; no ADR; no new serialized record.
```

## CEIR-9h — U8 the MANDATORY external-plugin proof (no ADR) — 2026-08-09 — the ACID TEST

**The proof.** The band's heaviest slice: a genuinely EXTERNAL plugin — a whole `plugin` dialect authored in a
test-side namespace (`plugin_ext`) that includes ONLY the public crd-ceir headers and edits NO engine source —
registers its FULL surface (NINE pieces) through the PUBLIC open-world APIs. `test_plugin.cpp` (5 `[ceir][plugin]`).

**The mechanism fork (resolved with the advisor, before code):** "outside crd-ceir" = a test-side namespace over the
public headers, zero engine source edited; the "zero central-enum edits" claim made MECHANICAL (not narrative) by a new
pin in `check_ceir_invariants` (both .ps1 + .sh) — ⛔ **U-§116: TypeKind/AttrKind's LAST enumerator must be `Extern`
(a plugin rides the Extern door + registration, never a new enum value); EffectFamily is already pinned at compile time
by its `kLastEffectFamily` static_assert — TWO mechanisms, ONE gate.** Tested passing on both platforms before a line
of the plugin was written.

**The U-§97 / U-§116 verdict — ZERO core edits, NO private wall (advisor-approved, three parts):** (1) the acid test
passed — NINE surfaces (custom TYPE class 8a + ATTR class 8b + effect-LOCATION class 8c + op INTERFACE 8e + OPS with
verifiers + a REWRITE + a lowering `ConversionTarget` + an `IExecutionProvider`) all registered through PUBLIC doors;
the U-§116 pin is a STANDING GATE (9z inherits it), and the in-test assertions confirm the custom type/attr went through
the `Extern` doors. (2) **the 8g rewrite framework carried its FIRST real pattern** — `A(B(x)) → C(x)` fired via
`try_apply` (create-C-wired-to-x → RAUW A's result → erase A then B-if-dead, MLIR-shaped hygiene), authored ENTIRELY
plugin-side, with the worklist driver still cleanly RESERVED — evidence 8g's skeleton boundary was drawn RIGHT (the
pattern needed only the caller-driven shape); a non-match returns false. (3) **two honest ASYMMETRIES, named:** a plugin
HAND-AUTHORS its OpSchema table (a first-party dialect gets it generated — a tooling asymmetry, unbound named-forward);
execution is a CONTRACT stub (`advertises` real, `execute` = `NoSemantics` — dispatch → CEIR-24/29). The registration
surface is COMPLETE. Foreign U-§56 round-trip/preserve/unify + a plugin-blob fuzz sweep ASan-clean; discoverable by the
9g agent path over the hand-built schema table (U-§117).

**The `ExecResult` allocator discovery.** The provider stub's `execute` returns `ExecResult`, which is NOT
aggregate-constructible — it holds an `Array<i64> values` and takes an allocator ctor. Read the primary source, built it
`ExecResult(ctx.allocator())` + set the error, rather than forcing an aggregate init (the read-the-source discipline).

**⭐ Running tally — EIGHT domains in (9a–9h), U-§97 = NO every time — ZERO engine-line changes across all eight; the
only foundation changes any proof has forced are one stale-comment correction (9f) + one STANDING GATE ADDED (the
U-§116 enum-pin — gate infrastructure, not engine).** The 9z headline is assembled: strongest cross-slice claims — 9c
"two drivers, one engine", 9e "the vocabulary was domain-neutral before the domain arrived", 9h "a whole foreign
dialect — type to provider — landed without one core line."

**Tests.** `test_plugin.cpp` (5 `[ceir][plugin]`): the full surface registers through the public doors (+ Extern-door
assertions); the plugin type/attr preserve through an unregistered host + re-register unify + a single-byte fuzz sweep;
the rewrite `A(B(x))→C(x)` fires + mutates the IR + a non-match returns false; the 9g agent discovers + authors a plugin
op over the hand-built schema table; the provider advertises the plugin ops + declines a foreign one.

**Gate.** crd-ceir-tests + host + cook **358/358 ctest** (353 + 5) on win-debug + win-asan + linux-gcc-debug +
linux-gcc-asan + LLVM-20 tidy (test_plugin.cpp) + GCC `-Werror=switch` + opgen drift/validator + `crd-ceir-invariants`
(EXTENDED with the U-§116 enum-pin, both scripts, both platforms) + the plugin-blob fuzz sweep. ⛔ **ZERO engine source
edited** (only tests/ + the invariants scripts). **NO recook; no ADR.** NEXT = CEIR-9z (the BAND-9 GATE).

## Proposed commit — CEIR-9h (user commits; NO AI trailer; NO ADR — a proof composes)

```
test(ceir-9h): U8 external-plugin proof -- the acid test, a whole foreign dialect, zero core edits

- The band's heaviest, MANDATORY slice: a genuinely EXTERNAL plugin (a whole `plugin` dialect in a
  test-side namespace over public headers only, ZERO engine source edited) registers its FULL surface
  through the PUBLIC open-world APIs -- NINE pieces: custom TYPE class + ATTR class + effect-LOCATION
  class + op INTERFACE + ops with verifiers + a REWRITE + a lowering ConversionTarget + an
  IExecutionProvider. Every piece has a public door; no private wall.
- U-97/U-116 verdict: (1) the acid test passed + the "zero central-enum edits" claim is MECHANICAL --
  a STANDING GATE in check_ceir_invariants (U-116: TypeKind/AttrKind end at Extern via the script pin;
  EffectFamily via its compile-time static_assert -- two mechanisms, one gate, both platforms);
  (2) the 8g rewrite framework carried its FIRST real pattern -- A(B(x))->C(x) via try_apply, a real
  IR mutation, worklist driver still reserved; (3) two honest asymmetries named -- a plugin
  hand-authors its OpSchema table (named-forward), execution is a contract stub (dispatch -> 24/29).
- The plugin type/attr round-trip + preserve through an unregistered host + unify + a fuzz sweep;
  discoverable by the 9g agent path (U-117).
- GATE INFRASTRUCTURE: check_ceir_invariants.{ps1,sh} extended with the U-116 enum-pin (a standing
  forever-gate; not engine source).
- test_plugin.cpp (5 [ceir][plugin]). Review doc: the CEIR-9h narrative + the DoD item-22 update.

Gated: crd-ceir-tests + host + cook 358/358 on win-debug + win-asan + linux-gcc-debug +
linux-gcc-asan + LLVM-20 tidy + GCC -Werror=switch + opgen drift/validator + crd-ceir-invariants
(with the U-116 pin) + the plugin-blob fuzz sweep. Zero engine source edited; no recook; no ADR.
```

---

## CEIR-9z — the BAND-9 GATE — ✅ CLOSED 2026-08-10 → ⭐⭐ BAND 9 CLOSED (universality validation complete)

**A DOC-SYNTHESIS gate — no code, no gate test, no ADR, no recook. These are DECISIONS, not omissions.**
The 3z/4z/5z/8z gates each earned a *composing* test because their band's guarantees lived in separate tests that
never met in one program — the gate was the first place they had to coexist. Band 9 is structurally different: each of
its eight slices is *already* a composition (a real workload driven through the substrate), so the executable half of
this gate is the 358-test suite × 4 configs that the eight slices already stand up. A ninth mega-test would be the
band's first redundant artifact. The gate's work is therefore SYNTHESIS: fill the matrix from evidence, re-verify it
against code, record the verdict, answer the final question. (Advisor-confirmed: doc-synthesis-only is the honest shape.)

**Deliverable 1 — the U-§122 seventeen-domain matrix, filled from evidence** (`docs/research/2026-08-09-ceir-universality-review.md` §I).
Every one of the 17 rows carries an evidence tier, none overclaimed:
- **7 PROVEN** (a band-9 test consumer drives the domain through the substrate): Notebook 9a · DAW 9b · DCC 9c · CAD 9d ·
  EDA 9e · Game/ECS 9f · Agent 9g.
- **2 ◧ foundation-validated** (every mechanism proven by other slices; no domain consumer driven — a composition of
  proven parts, not a prediction): Build pipeline (8h + 9a/9c) · Reactive UI (8e + 9b/9d).
- **4 ◑ analogous-proven** (planned for a consumer band; the characteristic shape proven by an analogous 9x slice):
  MATLAB (9a+9c incremental) · CAM (9e external-post) · Physics/CAE (9f effect-ordering) · Media (9b time-domains).
- **4 ○ future** (planned; architectural space validated, domain unproven): Renderer-15 · Compute-13 · ML-24 · Distributed-30.
A **9th proof**, 9h (the foreign external plugin), validates the open-world *doors* rather than a matrix domain, so it is
NOT a matrix row → **8 proof slices in all** = 7 mock-domain rows + 1 foreign plugin.

⛔ **Re-verified every mid-band §I row against the code (the 8z gate discipline — never inherit a stale claim into a gate
answer).** The sweep caught two stale "Agent introspection" rows (§B + DoD item 23) still citing the query surface as
"9g-forward / test_transaction.cpp" *after* 9g had shipped and proven the full agent loop via test_agent.cpp — both
fixed to the honest split (9g proved discovery + authoring + verify + diagnostics; the richer *semantic-query* surface
beyond arity selection stays forward).

**Deliverable 2 — the U-§97 verdict tally, in one place** (the review doc). The single question asked at each slice —
*did this domain need a second scheduler / compiler / runtime / depsgraph / transaction-manager / capability-system?* —
answered **NO at all eight slices (9a–9h)**, with **zero engine-line changes** across the entire band. The only two
foundation deltas any proof forced: one stale-comment correction (9f `op_access_at`) and one standing gate *added* (the
U-§116 enum-pin in `crd-ceir-invariants` — gate infrastructure that mechanically enforces "zero central-enum edits", not
an engine change). This is the band's falsifiable result.

**Deliverable 3 — the U-§126 final answer, both halves affirmative, bounded honestly to PROOF scale.** *Is CEIR a
universal execution substrate?* **Yes — validated, not merely designed** — across 7 mock-domains + 1 foreign plugin,
each a *composition* of already-shipped foundation slices (which is the operational definition of universal). The
FOUNDATION half was answered at 8z (the substrate is architecturally universal); 9z adds the EMPIRICAL half (eight
independent domains proved it). **The boundary, stated plainly:** this is validation at proof / mock-domain scale — each
consumer is a ~100-line test driver, not a production DAW or renderer. Production-scale validation (real data volumes,
real provider dispatch, real solver kernels) accrues as the consumer bands CEIR-10→35 build the actual domains, each
re-asking U-§97 against reality. What CEIR-9 establishes is that the substrate is *ready*: no known domain in the
seventeen requires a second runtime, and eight have been proven not to.

**Advisor pre-close** caught a real defect: the freshly-written legend + U-§126 paragraph miscounted the matrix
(8 proven + 5 future + "9 planned" — arithmetic that didn't close). The matrix is **17 rows = 7 + 2 + 4 + 4**; 9h is a
proof but not a matrix row. Fixed every count (legend, tally summary, U-§126) to be internally consistent. The gate
stays valid without a re-run — the corrections are `.md`-only, no crd-ceir source touched.

**Gate (doc-only ⇒ no rebuild; the binaries are current from 9h — the WSL `cmake --build` no-op at gate time confirmed
no source newer than the binaries).** crd-ceir-tests + host + cook **358/358 ctest** on **win-debug + win-asan +
linux-gcc-debug + linux-gcc-asan** + opgen validator + drift `--check` clean + `crd-ceir-invariants` (incl. the U-§116
enum-pin) + GCC `-Werror=switch` (linux builds clean). **NO tidy** (docs-only, zero source touched) — **NO fuzz** (no new
serialized record). **NO ADR, NO gate test, NO recook** — stated as decisions above.

## Proposed commit — CEIR-9z + BAND 9 CLOSE (user commits; NO AI trailer; NO ADR — a doc-synthesis gate)

```
docs(ceir-9z): BAND 9 CLOSED -- universality validation, the 17-domain matrix + U-126 final answer

- The BAND-9 GATE, doc-synthesis only (no code, no gate test, no ADR, no recook -- DECISIONS not
  omissions: unlike the 3z/4z/5z/8z composition gates whose guarantees lived in separate tests that
  never met, band-9's eight slices are each ALREADY a composition, so the 358-suite x 4 configs IS the
  gate's executable half; a mega-test would be the band's first redundant artifact).
- The U-122 seventeen-domain matrix filled from evidence (universality-review.md sec I): 7 PROVEN
  (9a-9g) + 2 foundation-validated (build pipeline, reactive UI) + 4 analogous-proven (MATLAB, CAM,
  physics/CAE, media) + 4 future (renderer-15, compute-13, ML-24, distributed-30) = 17; a 9th proof
  (9h foreign plugin) validates the open-world DOORS not a matrix row -> 8 proof slices in all.
- RE-VERIFIED every mid-band matrix row against code (the 8z discipline) -- caught + fixed two stale
  "agent introspection" rows citing 9g-as-forward after 9g shipped.
- The U-97 tally recorded in one place: NO x8, zero engine-line changes across the band (the only
  deltas: one 9f stale-comment fix + one standing gate ADDED, the U-116 enum-pin).
- The U-126 final answer, both halves affirmative (foundation + empirical), bounded honestly to PROOF
  scale: CEIR is a universal substrate -- validated across 7 mock-domains + 1 foreign plugin, each a
  composition of shipped slices; production-scale validation accrues as consumer bands 10->35 build the
  real domains.
- Docs only: universality-review.md (sec I matrix + legend + U-97 tally + U-126 empirical answer),
  D-007 tracker (9z + BAND 9 CLOSED + CEIR-10 NEXT), context.md, this session log.

Gate (doc-only, binaries current from 9h): 358/358 ctest on win-debug + win-asan + linux-gcc-debug +
linux-gcc-asan + opgen drift/validator + crd-ceir-invariants (with the U-116 pin) + GCC -Werror=switch.
No tidy (docs-only), no fuzz (no new record), no ADR, no gate test, no recook.
```
