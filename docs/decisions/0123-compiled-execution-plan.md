# ADR-0123 — The compiled execution plan (CEIR-11b): a dense two-tier, differential-verified against the §118 oracle

**Status:** **ACCEPTED** (2026-08-10, under the standing autonomous loop grant [[project_ceir_autonomous_loop_grant]];
design + close advisor-reviewed — a consult at every stage's design fork and pre-close) — the D-007 **CEIR band 11
(Reference executor + compiled host plan)**, slice **CEIR-11b**. The COMPILED tier of §84 two-tier execution: a
`CompiledExecutionPlan` compiled from a Module to DENSE instruction arrays, proven byte-for-byte identical to the
CEIR-11a §118 reference oracle across a program corpus (§119 differential).
**Phase:** D-007. Completes band 11's compiled tier (§83/§84 two-tier execution). Law: §84 (two-tier) · §119 (the
differential) · §153 (hot-path rules) · §118 (the correctness oracle, from 11a). Inbound: ADR-0122 (the reference oracle).
Design: `docs/design/ceir-11b-compiled-execution-plan.md`.
**Tags:** `[ceir]` `[compiled-tier]` `[plan]` `[differential]` `[oracle]`

---

## 1. Context

Band 11 provides two execution tiers (§84): a REFERENCE tier (the CEIR-11a `Interpreter`, the §118 oracle — slow, correct,
maximal diagnostics) and a COMPILED tier (this slice). 11b compiles a Module to a dense `CompiledPlan` and proves it
byte-identical to the reference across a corpus (§119). ⛔ **The §153 bar, designed against from line 1:** the compiled
hot loop touches NO `Operation*`, NO attr table, NO string, NO `HashMap`; values are dense-slot array reads; dispatch is a
compile-time jump table. Designing against it now is why CEIR-11z is a gate, not a rewrite.

## 2. Decision — the representation + seven gated stages (one tracker row)

### 2.1 The representation (advisor-corrected): the Module is compile-time INPUT ONLY

Structured control flow COMPILED to dense instruction arrays. The plan owns a tree of dense **`Seq{instrs, yield_slots,
latches, arg_slots}`** — each Seq OWNS its instrs (a nested movable `Array`); a control-flow op's child instrs live in
SEPARATE seqs and the executor **recurses into CHILD sequences BY INDEX**. ⛔ The hot loop NEVER touches an `Operation*`
(a runtime authoring-graph walk is itself a §153 violation — the advisor's core correction; flat jump-streams rejected
too — §153 wants DENSITY, not flatness). Values are dense `u32` SLOTS in a flat `i64` frame stack; dispatch is a dense
`switch(Op)` (a compile-time jump table). A compile-time slot allocator (`Value*→u32`, COMPILE-time only) kills the env
map.

### 2.2 The three killed §153 violations (the real 11z risks, all closed)

1. **Per-eval attr reads** (`eval_const` `"value"`, `eval_cmpi` `"predicate"` STRING, `eval_state` `"depth"`) → compile-time
   **IMMEDIATES / dense enums** in the `Instr` (the const's i64, a `CmpPred`, a `u32` depth). No attr/string touch in the loop.
2. **State cells:** the reference's `HashMap<Operation*,Cell>` → dense **CELL indices** + plan-owned ring storage +
   pre-baked per-block **LATCH LISTS** (the read-all-then-latch semantics as a slot list, no runtime trait scan). Cells
   are per-OP GLOBAL (shared across calls), matching `m_cells`.
3. **Call resolution:** `resolve_call` per call → a compiled-function **INDEX**; frames are **WINDOWS** into a plan-owned
   slot stack (arena; grow-bind-run-copy-pop; recursion-safe; no per-call heap). ⛔ Never hold a slot ref across a callee
   run (growth reallocs — index through the Array; ASan-clean proves it).

### 2.3 Independence — the compiled thunks are SEPARATE implementations (by necessity AND principle)

The slot thunks structurally cannot run on the reference EvalFns (those take `Operation*` + the `Value*` map), and ⛔ MUST
NOT delegate to them: the differential's entire value is INDEPENDENCE (a shared-code oracle is the bit-exact-blind scar in
its purest form). What is SHARED is the **SPEC** (the TOML-pinned semantics — wrapping i64, index-order fold,
launch-runs-at-launch), never code — with ONE deliberate exception: the parallel-region **legality PRE-FLIGHT**
(`exec::check_parallel_region`, moved to core in 11a for exactly this) is reused at COMPILE. ⭐ Sharing legality ANALYSIS
(not execution) makes accept/reject agreement true BY CONSTRUCTION — a drifted reimplementation could compile-ACCEPT a
stateful parallel body the reference REJECTS (the dangerous direction).

### 2.4 The stages (seven, one tracker row) — each core-gated on 4 configs

(2) straight-line arith + the slot allocator + the differential-harness skeleton · (3a) control flow (if/for/while/switch)
→ child instruction arrays · (3b) §20 state → dense cells + latch lists · (4a) calls → frame windows · (4b) async/task →
a run-global token store + six thunks · (4c) data-parallel → isolated body mini-functions + index-order fold · (5) the
full-corpus differential + the §121 plan-layer twin + this ADR + close.

### 2.5 The §4 error-tier boundary — a typed error is a legitimate tier difference (the differential CONTRACT)

A defect the reference fails at RUNTIME fails the COMPILER when it is STRUCTURAL (statically decidable). ⛔ The differential
contract covers programs that COMPILE. The mapping table IS the contract:

| condition | compiled | reference | tier |
|---|---|---|---|
| no/absent entry, bodyless entry | `NoEntry` / `NoModuleBody` | `NoEntry` | compile |
| op with no compiled semantics (`core.foreach`) | `UnsupportedOp` | NoSemantics | compile |
| absent / non-int const `"value"` | `BadConst` | `UndefinedValue` | compile |
| bad cmpi `"predicate"` string | `BadPredicate` | UnknownPredicate | compile |
| call to a missing callee | `UnresolvedCall` | `UnresolvedCall` | compile |
| call operand ≠ callee param count | `CallArity` | `BadArity` | compile |
| isolated parallel body reads a capture | `CapturedValue` | `UndefinedValue` | compile |
| map/combine body arity / yields-1 / state-free | `ParallelArity` / `ParallelYield` / `ParallelStateful` | `BadArity` / `ParallelYieldArity` / `ParallelBodyStateful` | compile |
| for/parallel step ≤ 0 (dynamic) | `BadForStep` | `BadForStep` | RUNTIME |
| switch/match selector out of range | `SelectorOutOfRange` | `SelectorOutOfRange` | RUNTIME |
| while cond ≠ 1 yield | `CondArity` | `CondArity` | RUNTIME |
| await/join/continuation invalid token | `BadToken` | `BadToken` | RUNTIME |
| continuation body arity ≠ antecedent yields | `ContinuationArity` | `BadArity` | RUNTIME |
| runaway loop | `FuelExhausted` | `FuelExhausted` | RUNTIME |

Runtime errors AGREE between the tiers (the differential compares the error too); compile-tier rejections are §4
differences (the reference runs the program until it hits the defect; the compiler rejects it structurally).

### 2.6 Full body isolation for data-parallel — THREE stores, not one

`task.parallel_for` / `map_reduce` compile the map/combine body as an ISOLATED mini-function (a FRESH slot map → a capture
fails `slot_of` at COMPILE), run per-index in a frame window, and fold the combine fn in INDEX order. ⭐ The reference sub
isolates THREE things and the compiled tier mirrors all three: fresh env (the frame window), fresh cells (the preflight
rejects state), and a fresh **per-phase TOKEN STORE** (a RAII `TokenScope` swaps the run-global `rs.tokens` out per phase
and discards). Without the third, a body `await(parent-handle)` would WRONGLY resolve (reference: `BadToken`) — the
dangerous accept direction.

## 3. Consequences

- **The §118 oracle is byte-matched by the compiled tier** across the corpus: `test_plan.cpp` (`[ceir][plan]`) — the
  SHARED corpus builder (`tests/ceir/corpus.hpp`, the `rich_graph.hpp`/1f precedent, build-once) runs 5 programs (5z the
  band-5 gate → 118; 6z non-associative map_reduce → 33930; async → 29; the six task ops → 46; composing all-dialects →
  118) through BOTH engines, comparing VALUES + CELLS + MAP_OUTPUTS. ⛔ `build_5z` is REUSED EXACTLY (`test_band5_gate.cpp`
  refactored to consume it — no drift).
- ⭐⭐ **THE DIFFERENTIAL EARNED ITS KEEP — a real stage-3a bug it caught + we FIXED:** `compile_seq` captured a
  control-flow op's `children_off` BEFORE compiling its region children, so a for-body containing a `match` (NESTED
  control flow) aliased the match's `arm0` → `Op::For` ran `arm0` (`acc(iv)`) every iteration → 15, not 18. Every simpler
  test missed it (no NESTED control flow). Fixed (compile children FIRST into a local buffer, THEN reserve a contiguous
  `child_pool` block); pinned by a minimal nested-cf regression + the 5z corpus program.
- ⭐ **§121 no-privileged-path AT THE PLAN LAYER:** a cooked→loaded module (`serialize`→`deserialize`, dialects
  re-registered) compiles to BYTE-IDENTICAL results vs its builder-form twin — compile both twins, compare the dense
  arrays (NO op handles: dense cell/map indices are assigned by COMPILE ORDER, so structurally-identical twins align).
- **crd-ceir core stays jobs-free (I4) + asset-free (I5)** across all six stages — the frame stack + token store + cell
  rings are plain per-run arenas; the shared `check_parallel_region` is core + jobs-free.
- **No cook/binary format motion, no fuzz, no recook, no version bump** — the plan is a RUNTIME object; nothing serializes.

## 4. Named-forwards (explicit)

- **The SERIALIZABLE plan blueprint** (dense arrays with thunk-TABLE indices instead of raw dispatch) → the slice with a
  cross-session plan-cache consumer. ⛔ This **refines ADR-0121 §2.1's "opaque bytes"**: the real 11b plan is an IN-MEMORY
  dense-array object (raw index/enum dispatch — non-serializable as-is), NOT the opaque byte-blob 10b's cache stores; the
  serializable form waits for a real cross-session consumer. (Struck in place in ADR-0121 §2.1.)
- **Provider composition + the §69 compile→plan interface** → CEIR-21/26 (the partitioner band; `provider.hpp` defers it).
  ⛔ 11b does NOT name that interface.
- **The §153 / hot-loop audit** (a `CountingAllocator` proving zero per-op heap in straight-line compute + a
  no-string-table-touch assert) → CEIR-11z.
- **The differential-contract watch items (a)–(d)** as documented limits (stated in the harness header): (a) an untaken
  cell reads compiled-`0` vs reference-absent (agree when both tiers take the same control flow); (b) a cell's `next` is
  block-local (the 5d verifier forbids escape); (c) a program whose reference run hits `UndefinedValue` (an over-declared
  call result / a dead-branch capture) is OUTSIDE the contract, like a program that does not compile; (d) a dp op NESTED
  inside a parallel body has a sub-local (discarded) `map_output` in the reference, so the generic compare SKIPS nested dp.
- **`continuation` static cross-op yield-count checking** → CEIR-3/4 (runtime `ContinuationArity` today, inherited from 11a).

## 5. Alternatives rejected

- **A thunk tree that structurally walks Regions at runtime:** still an "authoring-graph traversal", a §153 violation
  (the advisor's correction at the design fork). → dense instruction arrays; the Module is compile-time INPUT ONLY.
- **Flat jump-streams (a single instruction vector with jump targets):** §153 wants DENSITY, not flatness; a Seq tree with
  recursion-by-index is dense AND structured. → the `Seq` tree.
- **Delegating compiled thunks to the reference EvalFns:** the differential would compare a thing to itself (the
  bit-exact-blind scar). → independent thunks that share the SPEC, never code.
- **Sharing execution for parallel_for's per-index run** (delegate to the reference sub): the same scar. → an independent
  per-index run + index-order fold; share only the pre-flight ANALYSIS (`check_parallel_region`, §2.3).
- **Deferring `parallel_for`/`map_reduce` to the provider tier** (the stage-4b instinct): the reference RUNS these
  programs — deferral would be a never-defer violation. The compiled body is a mini-function with a FRESH slot map, so a
  capture fails `slot_of` at COMPILE (the §4 pattern); non-capturing bodies run in a frame window. → stage 4c.
- **Requiring async bodies pure OR validating token handles in Race/Cancel:** a capability regression / a divergence from
  the reference (which validates NO handle in race/cancel). → mirror the reference exactly (run launch/await bodies
  in-frame; Race = deterministic index 0; Cancel = no-op).
- **Wedging the plan into 10b's byte-artifact cache:** raw dispatch is non-serializable — a plan is not 10b's opaque
  byte-blob. → in-memory; the serializable blueprint is a named-forward (refines ADR-0121, §4).

## Gate

The corpus differential + the §121 plan-layer twin + the nested-cf regression (`test_plan.cpp`, `[ceir][plan]`) + the
SHARED corpus builder (`tests/ceir/corpus.hpp`) + the extracted 5z (`test_band5_gate.cpp`). **436/436 ctest** across
**win-debug + win-asan + linux-gcc-debug + linux-gcc-asan** (the ASan configs the frame-stack / token-store / map-window
UAF money configs) + LLVM-20 tidy + GCC `-Werror=switch` + opgen drift/validator + `crd-ceir-invariants` (crd-ceir core
jobs-free + asset-free). Stage progression: 397 → 400 (2) → 403 (3a) → 407 (3b) → 412 (4a) → 421 (4b) → 429 (4c) → 436
(5). **No recook, no fuzz, no binary version bump** (the plan is a runtime object; nothing serializes).
