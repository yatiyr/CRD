# CEIR-5 — Structured control flow + functions + the reference executor (§13/§14/§20/§118) — session log

Band 5 is a GEAR CHANGE: region-carrying control-flow *ops* (`ceir.core` if/for/while/match/scope), SSACFG blocks+branches
beneath, explicit state (§20), and the first reference executor (§118). Band gate: a nontrivial program *executes*.

**Accumulated landings placed at band-4 close (advisor):** `EffectsFn` (func.call callee-derived effects) → 5c; cross-block
hazards → 5b at earliest, honestly re-deferred to 12d (5b is dominance/terminator *verification*, not the reachability
hazard analysis); nested-region-relax → 5d/5z (needs an executor that *runs* regions).

## CEIR-5a — structured region ops + the constant-cond `if` fold (CHECKPOINT, ◧ in progress)

**Honest scoping.** The advisor's opening directive was "all seven ops, no subset." That directive was unsatisfiable in
one turn: value-producing structured ops (results = yields) require a THIRD generator extension — **variadic results** —
that neither of us had scoped, on top of typed region args AND variadic regions. The gold-standard resolution is
STATEMENTS-FIRST with an explicit ◧ checkpoint, not a rushed or false close. The advisor confirmed the divergence was
correct once the missing extension surfaced.

**DONE (gated).**
- **Generator region-SIGNATURE schema evolution.** A `regions` list entry may now declare `{kind, args=[{name,
  type_hint}], variadic}`. `type_hint` is a DOC string — ⛔ NO TOML type grammar (the 3a lesson: real arg types are
  construction-time); the arg COUNT is the structural contract the generated verifier enforces; `variadic` (last entry
  only) is the switch/match capability (validation + verifier emission landed; the builder region-count param is REMAINING).
- **`ceir.core`** via 2a schemas: `scope` (1 region), `if` (cond + 2 regions), `for`/`foreach` (1 region with a typed
  entry-block arg), `yield` (variadic-operand Terminator). ⛔ Every core op is **BitExact** (a control-flow construct is
  deterministic) and declares **ZERO effects** — a structured op is not itself effectful; its effects are its CONTENTS',
  which the CEIR-4d per-block hazard walk already sees (the 4z BitExact-baseline lesson applied at authoring time).
- **`Context::fold_constant_if`** — the partial-eval seed. A constant `arith.const` condition splices the taken region's
  single block into the parent before the `if` and erases the `if`; the dead cond const remains (DCE is a SEPARATE
  canonicalization). Zero results now ⇒ no RAUW (the value-forwarding half arrives with variadic results).

**Two design decisions a resumer cannot infer:**
- **SKELETON-VERIFIES.** The region entry-block arg count is checked only WHEN a block exists, so a freshly-built builder
  skeleton (empty regions) still verifies; full block-EXISTENCE is CEIR-5b's SSACFG rule.
- **STATEMENTS-FIRST.** Structured ops have zero results this checkpoint; `yield` forwards nothing to owner results yet.

**The soundness fix (advisor pre-close, BLOCKING).** The fold's bail set must cover not just a `region_exec`-tagged `if`
but any `region_exec`-tagged op INSIDE the taken block — splicing a tagged inner region-owner into the parent changes its
enclosing execution context. This is the **2nd instance** of band-4 metadata making a band-5 rewrite unsound (the 4c
class): **the standing lesson is that every future rewrite must audit the ATTRIBUTES it MOVES, not only the op it
deletes.** Fixed with a pre-splice scan; a named test (a tagged inner `scope` in the taken branch → fold bails, no mutation).

## REMAINING (CEIR-5a, each with its blocker)

- `while` — a region-RESULT condition (the cond region yields a bool).
- `switch` / `match` — VARIADIC regions (validation landed; the region-count builder param is not emitted).
- value-producing variants (results = yields) + the RAUW half of the fold — VARIADIC RESULTS (schema+validator+builder+
  verifier; not started — the larger of the two owed generator extensions).

## Gate (what's landed)

crd-ceir-tests **134/134 ctest** on **win-debug · win-asan · linux-gcc-debug · linux-gcc-asan** (both OSes via `ctest`) +
LLVM-20 tidy + GCC `-Werror=switch` + `crd-ceir-opgen-{drift,validator}` (47 py, drift green — `core` regens byte-stable,
other dialects untouched) + invariants green. New surface: the generator region-signature schema; the `core` dialect
(scope/if/for/foreach/yield); `Context::fold_constant_if`; `Operation::result(u32)` is const (from 4d). **No binary version
bump.** 5a stays **◧**; the wakeup continues it (variadic regions builder + variadic results + while/switch/match).

## Proposed commit — CEIR-5a checkpoint (user commits; NO AI trailer)

```
feat(ceir-5a): ceir.core structured region ops + constant-cond if fold [checkpoint]

- ceir_opgen.py: region-SIGNATURE schema evolution -- a `regions` list entry may
  declare {kind, args=[{name, type_hint}], variadic}; type_hint is doc-only (no TOML
  type grammar), the arg COUNT is the verifier contract, variadic is last-only.
  Validation + verifier emission (entry-block arg-count, checked when a block
  exists) + py tests.
- engine/ceir/ops/core.ceirop.toml (new): ceir.core -- scope / if / for / foreach +
  the yield terminator. Every op BitExact + zero effects (a control-flow construct
  is not itself effectful). Regenerated (core_ops.{hpp,cpp,json,md} + smoke).
- Context::fold_constant_if: constant-cond if fold (the partial-eval seed) -- splice
  the taken region's block into the parent, erase the if. BAILS on: not-a-core.if,
  non-constant cond, multi-block region, missing core.yield, a region_exec-tagged if,
  OR any region_exec-tagged op inside the taken block (a rewrite must audit the attrs
  it moves).
- tests: test_controlflow.cpp -- build+verify (incl the region-arg contract +
  wrong-arity failure + skeleton-verifies), the fold folds/erases (then + else),
  the five bails, a typed-region-arg `for` round-trips through text.

CHECKPOINT (5a IN PROGRESS): while / switch+match (variadic regions) / value-
producing variants (variadic results) remain -- see the tracker 5a row. Gated:
crd-ceir-tests 134/134 ctest on win-debug + win-asan + linux-gcc-debug +
linux-gcc-asan + LLVM-20 tidy + GCC -Werror=switch + opgen drift/validator (47 py)
+ invariants. No binary version bump.
```

## CEIR-5a — CONTINUATION → CLOSED ✅

The remaining work landed in the advisor-recommended order (SMALL extension first to de-risk the turn):

**Variadic regions (small, first).** Discovered the generated builder is MIN-ARITY on every variadic axis (it already
was for operands — `build_dummy` makes fixed+1) — so a variadic-region op needs NO builder region-count param; the builder
makes the minimum, full-arity via `create_operation`. `switch`/`match` = a selector operand + N variadic case regions,
structural (patterns are CHIR's). This DISPROVED the checkpoint's "region-count builder param owed" blocker.

**Variadic results (big).** The generator's 3rd variadic axis — validation (last-only) + a variadic-aware `num_results`
verifier (a lone variadic result emits NO count check). `scope`/`if` become value-producing (`results = [{variadic}]`).
⭐ **`create_operation` needed NO overload:** result types are HOMOGENEOUS placeholders (per-result typing joins at
CEIR-6 inference) AND the fold REPLACES the result values with the yields regardless — so a homogeneous `result_type` is
correct, not a shortcut. This DISPROVED the checkpoint's implied `create_operation`-overload blocker.

**The value-producing fold.** `fold_constant_if` now RAUWs the `if`'s results with the taken `yield`'s operands (the
spliced yielded values dominate every former use of the `if`'s results). ⛔ New bail: a yield-operand-count ≠
if-result-count MISMATCH — else `term->operand(i)` for `i < num_results` reads OOB (a UAF-shaped bug). Both the THEN fold
and the **ELSE fold** are tested (the advisor caught that the false-branch selector was only exercised degenerate — a
region-swap bug would have passed a THEN-only suite).

**`while`** — 2 regions (cond + body), structural; the cond-yields-exactly-1-bool contract is CEIR-5b. ⚠ **Re-scoped
at 5b (struck in place):** 5b enforces the cond region's yield **COUNT** (exactly 1 — the loop test); the **BOOL-ness**
half joins **CEIR-6** typing — core-op results are HOMOGENEOUS placeholders today, so a cond fed by a structured-op
result carries no checkable element type, and checking bool-ness would check garbage. Count now, type at inference.

**The yield↔owner contract** is enforced at its SOLE consumer (the fold's count-bail) — a standalone
`Context::yield_matches_owner` with no second caller would be the params-span violation; the general region-walking
verifier lands at CEIR-5b, where `yield` is the walk's start and the 3f `parent_op` back-link earns its keep.

⚠ **Process lesson (worth carrying):** this continuation DISPROVED TWO blockers the checkpoint row asserted (the
builder region-count param; the `create_operation` overload) — both dissolved once the code was actually read
(min-arity + homogeneous placeholders). A checkpoint's remaining-work list is honest, but its BLOCKER-CLAIMS are guesses
unless the code was read — mark them *unverified*, or a resumer builds unnecessary extensions.

## Gate (5a CLOSED)

crd-ceir-tests **138/138 ctest** on **win-debug · win-asan · linux-gcc-debug · linux-gcc-asan** + LLVM-20 tidy + GCC
`-Werror=switch` + `crd-ceir-opgen-{drift,validator}` (49 py) + invariants green. **No binary version bump.** Next =
CEIR-5b (SSACFG region verifier: dominance, terminators, block args — where the general yield contract + the 3f back-link
earn their keep, §13/§115).

## Proposed commit — CEIR-5a (user commits; NO AI trailer). ⚠ SUPERSEDES the checkpoint commit proposal above (that commit was never taken — this one covers both turns' work).

```
feat(ceir-5a): ceir.core structured control flow + the constant-cond if fold

- ceir_opgen.py: THREE variadic axes now (operands existed; + regions + results) and
  a region-SIGNATURE schema -- a `regions` entry declares {kind, args=[{name,
  type_hint}], variadic}; type_hint is doc-only, the arg COUNT is the verifier
  contract. Builders are MIN-ARITY on every variadic axis (noted in the generated
  hpp banner); full arity is create_operation. Validation + verifier emission + py.
- engine/ceir/ops/core.ceirop.toml (new): ceir.core -- scope / if (value-producing,
  variadic results) / for / foreach (typed region arg) / while (cond+body) / switch /
  match (variadic case regions) / yield (variadic-operand Terminator). Every op
  BitExact + zero effects (a control-flow construct is not itself effectful).
- Context::fold_constant_if: constant-cond if fold (partial-eval seed) -- splice the
  taken region (THEN or ELSE) into the parent, RAUW the if's results with the taken
  yield's operands, erase. BAILS on: not-a-core.if, non-constant cond, multi-block
  region, missing yield, region_exec-tagged if, any region_exec-tagged op INSIDE the
  taken block, and a yield/result COUNT MISMATCH.
- tests: test_controlflow.cpp -- build+verify (region-arg contract, skeleton-verifies,
  switch/match variadic regions, while), the fold folds THEN and ELSE + RAUW value-
  forwarding + count-mismatch bail + the five structural bails, a typed-region-arg
  for round-trips through text; test_opgen.py +6.

Design: HOMOGENEOUS result types (placeholders until CEIR-6 inference; the fold
replaces result values regardless), MIN-ARITY builders, SKELETON-VERIFIES (block
existence is 5b), loop-carried values -> 5d. Gated: crd-ceir-tests 138/138 ctest on
win-debug + win-asan + linux-gcc-debug + linux-gcc-asan + LLVM-20 tidy + GCC
-Werror=switch + opgen drift/validator (49 py) + invariants. No binary version bump.
```

## CEIR-5b — the §115 structure verifier — CLOSED ✅

`Context::find_structure_error(const Module&) -> StructureError{op, value, kind}` — a single top-down region walk that
returns the FIRST structural defect (deterministic pre-order = the printer's SSA order, the 3z first-offender convention)
or `None`. `StructureErrorKind` is a no-default `-Werror=switch` enum (6 values): `UseBeforeDef`, `CaptureThroughIsolation`,
`MissingTerminator`, `TerminatorNotLast`, `YieldCountMismatch`. The three checks the §115 structure layer needs in the
current (no-branch-op) world:

- **Dominance = per-block def-before-use + capture visibility.** A `DomScope` chain (each block frame carries its own
  `defs` set — block args, then op results appended as the walk passes each op) models the visible SSA environment; an
  operand must be found walking the scope chain. ⛔ **Isolation-aware:** the walk STOPS at an `IsolatedFromAbove` frame —
  a capture that would be visible without isolation but is blocked by it is `CaptureThroughIsolation` (a distinct pointing
  diagnostic from `UseBeforeDef`, discriminated by re-running the visibility probe with isolation OFF). No branch ops
  exist, so sibling-block dominance is not yet a question — real CFG dominance lands with branches; cross-block reachability
  hazards stay re-deferred to 12d.
- **Terminators by RegionKind.** An `SsaCfg` block MUST end with a `Terminator`-trait op (an EMPTY block fails too); a
  `Graph` region imposes no terminator (order is from data/effects). ⭐ **The 3f `parent_op` back-link earns its keep
  HERE:** an empty SsaCfg block has no op to blame, so `MissingTerminator` points at the region's OWNER via
  `Region::parent_op()` — a diagnostic must point at *something* (3z). `TerminatorNotLast` catches a Terminator op that
  isn't its block's last.
- **The yield↔owner COUNT contract**, generalized from its sole 5a consumer (the fold) to every `core.*` region owner: the
  terminating `core.yield`'s operand count must equal the owner's result count — with the table override that
  `core.while`'s cond region (region 0) yields exactly 1 (the loop test). This is where the general contract the 5a log
  promised finally lands. ⚠ **Re-scope carried from the 5a `while` line (struck in place above):** 5b enforces the
  cond COUNT; **bool-ness is CEIR-6** (core-op results are homogeneous placeholders — a cond fed by a structured result
  has no checkable element type today).

**Four conscious relaxations a resumer must not mis-infer (advisor pre-close):**
1. The yield contract binds `core.*` region owners ONLY — a foreign-dialect region owner is unconstrained (open-world /
   I6-consistent; its dialect owns its own region contract).
2. A **0-result** core region with **no yield** passes (an implicit-fall-through statement region); only a >0-result region
   MUST yield.
3. A value-producing op with **EMPTY regions FAILS** `find_structure_error` (a >0-result region with no block ⇒ no yield ⇒
   mismatch) — deliberately **STRICTER than the op verifier's SKELETON-VERIFIES**: `find_structure_error` is the
   whole-program VALIDITY layer (the same syntactic-vs-structural split that made the 1h fuzz corpus — which builds
   terminator-less SsaCfg blocks and self-capturing nested regions — inappropriate to assert `== None` over; validity is a
   hand-fixture concern, exercised by the "rich nested valid module" over-strictness test).
4. Dominance is **per-block** def-before-use — sibling blocks are invisible to each other (conservative-correct with no
   branch ops; refined when branches + real CFG dominance land).

Enabling surface (all pre-existing, no API break): `Region::parent_op()` (3f), `Operation::result(u32) const` (loosened
4d), `has_trait`/`op_name`/`dialect_of`. **Zero generator/TOML/regen** — a schema-quiet C++-only slice (opgen drift green
with no regen, correct). **§167 map ticked:** the CEIR-1h "dominance + region legality → CEIR-5b" DEFERRED item is now
covered by `test_structure.cpp`; the 3z "the CFG/dominance verifier is CEIR-5b" pointer is satisfied (`find_borrowed_escape`
stays structural — a different, use-walking layer).

`test_structure.cpp` — 7 `[ceir][structure]` cases (19 assertions): well-formed → None; a **rich nested valid module**
(captures + a value-producing `core.if` with block args + a use of the if's result → None, the over-strictness guard);
use-before-def (forward ref + self-ref); capture into a non-isolated region (None) vs through `iso.box` isolation
(`CaptureThroughIsolation`); capture-after-owner (`UseBeforeDef`); terminator rules (SsaCfg `MissingTerminator`, incl. an
**empty block pointing at its owner** via `parent_op`, vs Graph → None; `TerminatorNotLast`); the yield↔owner count
contract (value-producing if wrong count; `while`-cond=2 mismatch, `while`-cond=1 None).

## Gate (5b CLOSED)

crd-ceir-tests **145/145 ctest** (138 + the 7 `[structure]` cases) on **win-debug · win-asan · linux-gcc-debug ·
linux-gcc-asan** (both OSes via `ctest`) + LLVM-20 tidy (3 files clean) + GCC `-Werror=switch` (the no-default
`StructureErrorKind` switch) + `crd-ceir-opgen-{drift,validator}` (49 py, drift green with NO regen — correct for a
schema-quiet slice) + invariants green. **No binary version bump.** Next = CEIR-5c (calls + recursion policy + the
`EffectsFn` callee-derived-effects hook accumulated at band-4 close, §14/§34).

## Proposed commit — CEIR-5b (user commits; NO AI trailer)

```
feat(ceir-5b): the §115 structure verifier -- dominance, terminators, yield contract

- Context::find_structure_error(Module&) -> StructureError{op, value, kind}: one
  top-down region walk returning the FIRST structural defect (printer SSA order, the
  3z first-offender convention) or None. StructureErrorKind is a no-default
  -Werror=switch enum: UseBeforeDef, CaptureThroughIsolation, MissingTerminator,
  TerminatorNotLast, YieldCountMismatch.
- dominance = per-block def-before-use over a DomScope chain (block args + op results
  appended as the walk passes each op); isolation-aware -- the chain STOPS at an
  IsolatedFromAbove frame, so a would-be-visible capture blocked by isolation is
  CaptureThroughIsolation (discriminated by re-probing with isolation off).
- terminators by RegionKind: an SsaCfg block must end with a Terminator (empty fails);
  Graph imposes none. An empty SsaCfg block points at the region OWNER via the 3f
  Region::parent_op() back-link (a diagnostic must point at something). TerminatorNotLast
  catches a non-last Terminator.
- the yield<->owner COUNT contract generalized from the 5a fold to every core.* region
  owner: yield operand count == owner result count (table override: core.while cond
  region yields exactly 1). bool-ness of the cond -> CEIR-6 (homogeneous placeholders).
- tests: test_structure.cpp -- 7 cases: well-formed, a rich nested valid module
  (over-strictness guard), use-before-def (forward + self), capture through isolation
  vs not, capture-after-owner, terminator rules (incl empty-block-points-at-owner),
  the yield count contract (if + while cond).

Relaxations (advisor pre-close, documented in the tracker/log): the yield contract
binds core.* owners only (open-world); a 0-result region may omit the yield; a
value-producing op with EMPTY regions FAILS (find_structure_error is the stricter
validity layer, vs the op verifier's skeleton-verifies); dominance is per-block (no
branch ops yet). Schema-quiet -- zero generator/regen. Gated: crd-ceir-tests 145/145
ctest on win-debug + win-asan + linux-gcc-debug + linux-gcc-asan + LLVM-20 tidy + GCC
-Werror=switch + opgen drift/validator (49 py) + invariants. No binary version bump.
```

## CEIR-5c — calls: the callee-derived-effects hook + the recursion-policy verifier — CLOSED ✅

TWO features (§14/§34), both HAND-side — ⛔ **schema-quiet** (zero TOML/generator/regen; opgen drift green with NO regen).

**§34-text check first (advisor).** The design semantics were derived from cross-refs, so before locking them I read the
constitution (`docs/research/2026-08-07-ceir-universal-programming-master-roadmap.md`): §34 lists "recursion metadata";
the §13 control-flow menu lists "recursion where legal / bounded recursion attributes / tail recursion optimization where
useful." Recursion is attribute-DECLARED and the compiler verifies the declaration — no override of the plain reading.

**(1) The EffectsFn hook (the CEIR-4a landing).** A NEW `OpInfo::effects_fn` / `OpSpec::effects_fn` fn-ptr (the `verify`
precedent — ⛔ the core NEVER name-checks a dialect, I6 intact; the **func dialect registers** `call_effects_fn` for
`func.call`). `Context::effective_effects(op, table, out)` is an op's INSTANCE-level effect set: for `func.call`, its
CALLEE's — resolved via the SymbolTable and unioned TRANSITIVELY across the callee body (nested regions + further calls) as
a **`u32` family bitmask** (`collect_effective_mask` = one op's own hook-or-static contribution; `collect_region_effective_
mask` = the recursive callee-body walker the hook re-enters), with a **visited-set RECURSION cycle guard** — a self-/
mutually-recursive call graph TERMINATES and the union is complete (a skipped cycle member's effects are already collected
up-stack). ⛔ Lifted callee effects are **AMBIENT** `{family, None, 0, 0}` (a callee's operand/result target names the
CALLEE's position — meaningless at the call site; the honest whole-class lift, per-arg-passthrough precision deferred with
reason). ⛔ **DEGRADES to the full `ExternalCall` barrier** if ANY transitively-reached callee is unresolved OR any reached
op is UNREGISTERED — the interprocedural form of the 4a "registered-empty reads as provably-none" landmine (tested
explicitly); a resolvable nested call's static `ExternalCall` is **SKIPPED** (the hook early-returns, replaced by the
callee walk — else every transitive walk would degrade to a barrier).

**Two real consumers (no unused surface — the params-span rule):**
- **Hazards** — `ops_hazard` / `collect_block_hazards` gain `table`-taking OVERLOADS sharing the SAME conflict core
  (`accesses_conflict`/`pair_hazard`), only the per-op effect-fetch differs. A call to a pure func is reorderable; the
  no-table paths stay the conservative `ExternalCall`-barrier baseline — the **A/B pair**. ⛔ A NON-call op keeps its
  PRECISE per-Value static access on the table path (tested: two operand-targeted writes over distinct non-aliasing Values
  → None; same Value → Waw) — the table adds call precision, never subtracts non-call precision.
- **Domain legality** — `find_domain_violation` resolves calls internally against `m.symbols()`. ⭐ **The 4c "ExternalCall
  stays legal-for-now" gap is CLOSED:** `effect_legal_in_region` FLIPS `ExternalCall`→forbidden in audio-RT, so a call to
  a pure func passes an audio region, a FileIO/unresolved call flags. ⛔ **The table is NON-optional (advisor BLOCKER):**
  the first cut guarded `if (table != nullptr)`, which on a null table would leave the effect set empty and silently no-op
  the WHOLE §32 verifier (the perf-flag-measures-an-empty-frame shape). `create_module` guarantees a symbol table
  (asserted); the guard is gone.

**(2) The §34 recursion-policy verifier** (in the FUNC namespace — §34 dialect semantics; Context stays generic, gaining
only `effects_fn`/`effective_effects`/the hazard overload). `RecursionPolicy{Unspecified,None,Bounded,Unbounded}` — a
closed int-enum attr `recursion` (+ `recursion_max_depth`), the `region_exec` precedent. ⛔ `set_recursion_policy` ALWAYS
overwrites the depth attr (never guards on `max_depth>0`) — a re-declaration from Bounded(4) to Bounded(0) must CLEAR the
old bound, else `BoundedMissingDepth` can't fire (the declared-words-must-be-validated scar, moved from a test workaround
to the SETTER on advisor push). `find_recursion_violation(ctx, m, table)` walks the module body PRE-ORDER (deterministic —
funcs are linked into the body by the caller, NOT the hash-ordered symbol-table index; a `top->append(fn)` model) building
the resolved call graph, returns the FIRST offender: `DeclaredNoneRecurses` (a `None` func on any cycle incl. self-, via a
DFS reachability over `collect_callees` which DESCENDS nested regions — tested), `BoundedMissingDepth`, `InvalidPolicyAttr`
(corrupt attr).

⛔ **Verifies only where DECLARED (advisor fork ruling, not reading-(c)):** an UNDECLARED (Unspecified) func on a cycle is
NOT a violation — flagging it would verify where NOT declared and silently invalidate every unannotated recursive module.
Unacknowledged recursion is the executor's concern at 5d/5z (a documented RELAXATION). `None` is proven acyclic over the
**resolved single-module** graph only (an unresolvable callee = an unverifiable edge). Runtime-depth ENFORCEMENT lands with
the executor — the attr is presence+positivity today.

**Cross-refs ticked (struck in place):** the 4a "EffectsFn hook … NOT built now" and the 4c "ExternalCall stays
legal-for-now" promises are now FULFILLED; the 4c "FileIO stands PROXY" clause struck (the unregistered case reports
`ExternalCall`, the barrier its effective mask carries). Minor behavior note: multi-effect domain violations now report the
lowest-ORDINAL illegal family, not declaration order (deterministic either way).

**⚠ Process notes.** (a) The ASCII-only-test-names scar (4b) recurred: four `[calls]` TEST_CASE names with an em-dash `—`
passed in-process but FAILED under ctest (`catch_discover_tests` can't invoke a non-ASCII name) — fixed to hyphens at the
gate, the standing rule re-confirmed. (b) `create_func` links the func op nowhere — the CALLER appends it into the module
body (the printer walks the body); the SymbolTable is an index only, so the call-graph enumeration walks the body
pre-order (deterministic), not the hash-ordered table.

`test_calls.cpp` — 8 `[ceir][calls]` cases (99 assertions with `[domain]`): static-passthrough + resolved-call-not-opaque;
transitive A→B→C + nested-region traversal; self-/mutual-recursion TERMINATE; degrade-on-unresolved / unresolved-nested /
unregistered-in-callee; hazard A/B + non-call-precision-preserved; domain pure-passes / FileIO-flags / unresolved-flags;
recursion None-acyclic / None-self-points-at-self / None-mutual-points-at-declared / nested-region-recursion / Bounded-with-
depth / Bounded-without-depth / corrupt-attr.

## Gate (5c CLOSED)

crd-ceir-tests **152/152 ctest** (145 + the 7 `[calls]` cases) on **win-debug · win-asan · linux-gcc-debug ·
linux-gcc-asan** (both OSes via `ctest`) + LLVM-20 tidy (9 files clean) + GCC `-Werror=switch` (the two new no-default
switches `recursion_policy_name` / `recursion_violation_kind_name`) + `crd-ceir-opgen-{drift,validator}` (49 py, drift green
with NO regen — schema-quiet) + invariants green. **No binary version bump.** Next = CEIR-5d (`state`/`history`/`delay`
ops + the cycles-only-through-state verifier rule, §20).

## Proposed commit — CEIR-5c (user commits; NO AI trailer)

```
feat(ceir-5c): callee-derived effects (the EffectsFn hook) + the recursion-policy verifier

- OpInfo/OpSpec gain an `effects_fn` hook (the `verify` precedent; the core never
  name-checks a dialect -- I6). Context::effective_effects(op, table, out): a func.call's
  effects are its CALLEE's, resolved via the SymbolTable and unioned TRANSITIVELY (nested
  regions + further calls) as a u32 family bitmask with a visited-set recursion cycle
  guard. Lifted effects are AMBIENT (whole-class). DEGRADES to an ExternalCall barrier on
  any unresolved callee or unregistered reached op (the interprocedural registered-empty
  landmine); a resolvable nested call's static ExternalCall is skipped, not unioned.
- the func dialect registers `call_effects_fn` for func.call (the static ExternalCall is
  the conservative no-table baseline the hook refines when a table is present).
- two consumers: ops_hazard/collect_block_hazards gain table overloads (a pure call is
  reorderable; no-table = the ExternalCall-barrier baseline -- the A/B pair; non-call ops
  keep precise per-Value access); find_domain_violation resolves calls against m.symbols()
  and effect_legal_in_region FLIPS ExternalCall->forbidden in audio-RT -- the 4c
  "ExternalCall stays legal-for-now" gap closed. The domain table is non-optional (an
  empty-set fallback would silently no-op the §32 verifier).
- §34 recursion policy (func namespace): RecursionPolicy{Unspecified,None,Bounded,
  Unbounded} as the int attr `recursion` (+ recursion_max_depth; set_recursion_policy
  ALWAYS overwrites the depth so a re-declaration can't keep a stale bound).
  find_recursion_violation walks the module body PRE-ORDER building the resolved call
  graph: DeclaredNoneRecurses (a None func on any cycle, via collect_callees descending
  nested regions), BoundedMissingDepth, InvalidPolicyAttr. Verifies ONLY where declared --
  an undeclared func on a cycle is a documented relaxation (the executor's concern, 5d/5z).
- tests: test_calls.cpp -- 8 [calls] cases (effective effects incl transitive/nested,
  cycle-termination, degrade-on-unmodeled, hazard A/B + non-call precision, domain
  refinement + the flip, the recursion-policy verifier).

Design: schema-quiet (no TOML/generator/regen); ambient-family lift (per-arg precision
deferred); None acyclic over the resolved single-module graph; runtime depth is the
executor's (attr = presence+positivity today). Cross-refs ticked: the 4a EffectsFn and 4c
ExternalCall promises are fulfilled (struck in place). Gated: crd-ceir-tests 152/152 ctest
on win-debug + win-asan + linux-gcc-debug + linux-gcc-asan + LLVM-20 tidy (9 files) + GCC
-Werror=switch + opgen drift/validator (49 py) + invariants. No binary version bump.
```

## CEIR-5d — §20 STATE IS EXPLICIT: the explicit-state ops + the cycles-only-through-state rule — CLOSED ✅

**§20-text checked first (advisor).** The constitution §20 ("STATE IS EXPLICIT"): represent persistent state explicitly as
`state<T>` / `history<T>` / `delay<T>` / `persistent_resource<T>`, and **"Graph cycles must pass through explicit
state/delay semantics."** Real cycles, a real verifier — no override.

**The model (advisor, Model A over B/C).** Model B (a state cell with no SSA cycle) has no §20 verifier content; Model C's
loop iter_args appear nowhere in §13/§20 (an MLIR-ism) and 5z's "state<T> accumulator" is a `core.state` *inside* the loop
body. So: an actual SSA feedback edge + a trait that legalizes it.

- **`OpTrait::StateEdge`** (`1<<5`). Its CONVENTION: the op's **LAST operand is the feedback (`next`)** — the ONLY operand
  ever exempt from CEIR-5b def-before-use (it may forward-reference the value the cycle produces). A trait, not a per-kind
  index field (one consumer) and not all-operands-exempt (`init` must dominate on the first evaluation).
- **Three `ceir.core` ops** via the 2a generator (NOT schema-quiet): `state` / `delay` / `history`, all `(init, next) →
  current`, StateEdge, BitExact, ⛔ **NOT Pure** — each instance is a DISTINCT cell; a Pure trait would let CSE MERGE two
  cells (the CSE trap, written in the TOML). Zero effects. `history` takes an optional int `depth` attr (RESERVED
  vocabulary; absent = 1). The executor semantics are pinned in the TOML `docs` NOW (advisor: the cell is PER STATIC OP
  INSTANCE and **PERSISTS across evaluations** — it does NOT reset on region re-entry, which §20's use cases (TAA/GI/audio
  delay) require and which the `fold_constant_if` state-splice soundness silently assumes; `current` = `init` on the
  FIRST-EVER evaluation, thereafter the previous `next`) so 5z implements written words. A 1-operand StateEdge op's sole
  operand is its feedback (no dominating init — a resumer hits this via the hand-registered path).

**The verifier folds into `find_structure_error` (advisor — NO separate SCC pass).** Per block, an ORDER-FREE `all_defs`
set (block args + every op result). When the running def-before-use check fails, discriminate: a value in same-block
`all_defs` is a same-block BACK-EDGE — legal ONLY as a StateEdge op's LAST operand (else **`FeedbackWithoutState`**); a
value NOT in `all_defs` is the existing `CaptureThroughIsolation` / `UseBeforeDef`. ⭐ **The soundness one-liner:** every
def-use cycle has ≥1 same-block back-edge; a back-edge is legal ONLY as a state feedback ⇒ every surviving cycle passes
through a StateEdge op — the §20 rule, enforced by the existing walk, no cycle detector. `depth` is validated
trait-generically (any StateEdge op carrying `depth` → Int≥1 else **`StateDepthInvalid`**) — the declared-words scar at
the verifier, no I6 name-check. `StructureErrorKind` grows +2 (the no-default `-Werror=switch` forced both cases).

**FOUR conscious decisions (advisor Catch #2 + soundness):**
1. **The state op must HEAD its cycle in list order** (its feedback operand is the back-edge). An equivalently-cyclic
   program listed otherwise must be re-ordered — a canonicalization concern, not a semantics loss; preserves the
   module-body-Graph behavior a 5b test relies on.
2. **Cross-block / cross-region feedback stays `UseBeforeDef`** (same-block `all_defs` membership required — conservative).
3. A normally-visible feedback operand (a degenerate pass-through register) is legal — no back-edge at all.
4. The exemption applies **uniformly across region kinds** (simpler than Graph-only; documented). Open-world: it keys on
   the TRAIT, so a hand-registered non-core StateEdge op gets it (tested) — the core never name-checks a kind (I6).

**Composition with 5c:** orthogonal — recursion is the §34 call-graph (over `resolve_call`); feedback is the def-use graph.
Neither `collect_callees`/effective-effects/domain walk traverses operand edges. `fold_constant_if` splicing a state op out
of a constant-cond `if` is sound (same op = same cell, same evaluation count) — no new bail.

**⚠ Catch #1 — two 5b tests re-classify.** The 5b same-block forward-ref (test_structure.cpp) and self-ref cases now report
`FeedbackWithoutState`, not `UseBeforeDef` (strictly more informative — a same-block forward ref IS a feedback edge). Updated
in place with a comment; noted in the 5b tracker row. The capture-after-owner test survives (cross-block → still
`UseBeforeDef`); the 1h fuzz never forward-refs — unaffected.

**⚠ Struck the 5a "iter_args → CEIR-5d" promise (in place, tracker 5a row).** §20's loop-carried accumulator is a
`core.state` inside the loop body (what 5z executes), NOT MLIR iter_args-as-structural-sugar (absent from §13/§20). Don't
build two feedback mechanisms. **`persistent_resource<T>`** (§20's fourth construct) is routed to **ceir.resource / §36** (a
resource-lifetime concern), NAMED not silently subset (NO-FOLLOW); the §66/§67 `ceir.state` dialect is state MACHINES — a
different surface.

**⭐ Round-trip finding (advisor pre-close — item 1, resolved POSITIVELY).** The advisor suspected the 1e parser / 1f
decoder (built against a fuzz corpus that never forward-refs) would REJECT a feedback cycle's forward value reference, and
that 5z's text-parsed gate would inherit a broken path. Probed it: **both the parser and the binary decoder resolve the
forward reference — the round-trip SUCCEEDS** (hypothesis disproven). The real subtlety surfaced instead: the `StateEdge`
trait is REGISTRY state (from registration), NOT serialized module content (the open-world design) — so VERIFYING a
round-tripped module requires registering its dialects (an unregistered state op has no trait ⇒ its feedback conservatively
reads as `FeedbackWithoutState`). That's the correct EMPTY≠UNKNOWN contract; 5z's executor registers dialects, so it
inherits an open path, not a surprise. Documented in the test + row.

`test_state.cpp` — 9 `[state]` cases (41 assertions): legal cycle through state/delay/history; self-feedback; a
pass-through register (visible feedback operand); a combinational two-op cycle → `FeedbackWithoutState` at the non-state op;
an `init` (non-last) forward-ref → `FeedbackWithoutState` (pins the last-operand convention); cross-block feedback →
`UseBeforeDef`; the `history` `depth` attr absent/=3/=0/negative/non-int; the open-world trait test; **a text + binary
round-trip of a legal cycle** (forward-ref survives; register-to-verify contract).

## Gate (5d CLOSED)

crd-ceir-tests **161/161 ctest** (152 + the 9 `[state]` cases) on **win-debug · win-asan · linux-gcc-debug ·
linux-gcc-asan** (both OSes via `ctest`) + LLVM-20 tidy (5 files clean) + GCC `-Werror=switch` (the now-8-case
`StructureErrorKind` switch) + `crd-ceir-opgen-{drift,validator}` (50 py, drift green AFTER the committed regen — the `core`
dialect grew state/delay/history) + invariants green. **No binary version bump.** Next = CEIR-5z (BAND-5 GATE: a pinned
nontrivial program — bounded loop + `match` + calls + a `state<T>` accumulator — executes in the FIRST reference executor
with byte-pinned output, identical from text-parsed and builder-built forms, §118).

## Proposed commit — CEIR-5d (user commits; NO AI trailer)

```
feat(ceir-5d): sec 20 explicit-state ops + the cycles-only-through-state verifier

- OpTrait::StateEdge (1<<5): a state op's LAST operand is the feedback (`next`) -- the
  only operand exempt from CEIR-5b def-before-use, legalizing "graph cycles must pass
  through explicit state/delay semantics" (sec 20). Open-world: the verifier keys on the
  trait, never an op name (I6).
- ceir.core state/delay/history (via the generator; StateEdge added to OP_TRAITS): all
  (init, next) -> current, BitExact, NOT Pure (each instance is a distinct cell -- a Pure
  trait would let CSE merge cells), zero effects. history has an optional int `depth`
  attr (absent = 1). Executor semantics pinned in the TOML docs for CEIR-5z.
- find_structure_error extended (no separate SCC pass): a per-block order-free all_defs
  set discriminates a same-block back-edge (legal ONLY as a StateEdge op's last operand
  -> else FeedbackWithoutState) from a genuine UseBeforeDef (later block / never). depth
  validated trait-generically (Int >= 1 else StateDepthInvalid). StructureErrorKind +2
  (the no-default -Werror=switch forces both cases). Proof: every cycle has >=1 same-block
  back-edge, legal only through state ⇒ every surviving cycle passes through a state op.
- RE-CLASSIFY: the 5b same-block forward-ref + self-ref tests now report
  FeedbackWithoutState (more informative) -- expectations updated in place. STRUCK the 5a
  "iter_args -> 5d" note: sec 20's accumulator is a core.state inside the loop body, not
  MLIR iter_args. persistent_resource<T> routed to ceir.resource/sec 36 (named).
- tests: test_state.cpp -- 9 [state] cases (legal state/delay/history cycle, self-feedback,
  pass-through register, combinational-cycle, init-forward-ref, cross-block feedback, depth
  validation, open-world trait, a text+binary round-trip of a legal cycle) + test_opgen.py
  +1 (StateEdge accepted) + regenerated core.

Decisions: the state op must head its cycle in list order (canonicalization, not semantics
loss); cross-block feedback stays UseBeforeDef; the exemption is uniform across region
kinds; the cell PERSISTS across evaluations (pinned in the TOML for 5z). A feedback cycle
round-trips through text + binary (forward-ref survives); verifying a round-tripped module
requires registering its dialects (traits are registry state, not content). Orthogonal to
5c (recursion=call-graph, feedback=def-use). Gated: crd-ceir-tests 161/161 ctest on
win-debug + win-asan + linux-gcc-debug + linux-gcc-asan + LLVM-20 tidy (5 files) + GCC
-Werror=switch + opgen drift/validator (50 py, drift green post-regen) + invariants. No
binary version bump.
```

## CEIR-5z — BAND-5 GATE: the first reference executor (sec 118) — CLOSED ✅ (BAND 5 CLOSED)

The band gate + the first component that actually RUNS a CEIR program. `engine/ceir/exec.{hpp,cpp}` — a slow, correct
semantic interpreter for the core subset (arith + ceir.core control flow + sec 20 state + ceir.func calls).

**The design (advisor forks, all ruled):**
- **Dispatch = an Interpreter-OWNED `OpId -> EvalFn` table, NOT an OpInfo hook.** The mechanical reason: `register_op` is
  first-wins idempotent (the generated `register_core_ops` runs first, hookless), so a second `register_op` to attach eval
  hooks would SILENTLY NO-OP (the registered-default-empty scar). Semantics are INSTALLED per dialect (`install_{arith,
  core,func}_semantics` — open-world; a caller may add or override). An op with no installed EvalFn is a typed error, never
  skipped (EMPTY!=UNKNOWN at the executor). A table keyed by OpId is not an I6 switch — it is how the registry itself works.
- **Output = the entry function's `func.return` values** (NOT a state-cell dump). The loop-doesn't-yield problem
  (for/while/switch/match produce no SSA result) dissolves with the **call-based accumulator**: `@acc(%delta)` holds a
  `state` cell that persists ACROSS calls; the loop calls it each iteration; `@acc(0)` after the loop reads the total
  unchanged. `cell_value(op)` is a separate inspection API (sec 118 deterministic debugging), builder-form only (cells are
  pointer-keyed — pointers don't survive a round-trip).
- **State cells = a depth-N RING keyed by the STATIC op instance, persisting across evaluations (incl. across calls).**
  READ at eval (init-fill on first-ever eval), LATCH `cell <- env[next]` at BLOCK-EVAL END (read-all-then-latch, so
  cross-feeding cells latch simultaneously — hardware-register timing). ⭐ `state` = `delay` = `history(depth=1)` under one
  ring — this PAYS 5d's two TOML IOUs ("depth taps land at 5z" + "the distinction is the executor's update timing").
- **Frames:** a fresh env per `func.call` (recursion-safe); nested regions (if/for/switch bodies) SHARE the function frame
  (non-isolated capture). Blocks evaluate in LIST order (consistent with 4d list-order hazards + 5d state-heads-its-cycle).
- **Fuel** (`max_steps`) -> `FuelExhausted` at the loop op (timeout is not a hang-proof). Typed `ExecResult{values, error,
  op}` over 11 sec 118 failure modes (a no-default `-Werror=switch`). `foreach` = a NAMED NoSemantics deferral (no
  collection value domain until types land). `match` = `switch` reference behavior. An Interpreter is ONE SESSION (fuel +
  cells carry across `invoke`s).
- ⛔ **Wrapping i64 via u64 casts** (signed overflow is UB — don't hand gcc/asan a signed overflow). ⛔ **The CALLER
  registers the module's dialects** (the 5d trait-is-registry-state finding — the executor reads StateEdge/Terminator
  traits + resolve_call).

**Advisor pre-close (item 1, BLOCKING, fixed).** The first gate exercised `switch`, but the 5z row promises **`match`**,
and value-producing `core.if` (the only yield->results mechanism) was untested in the pinned program (source-must-match-
scoreboard). Fixed: the loop arm-selection now runs through **`core.match`**, and a value-producing **`core.if`** bonus was
added after the loop (`@main(6)` moved 18 -> 118). Also pinned "an Interpreter is one session" in the header.

**Obligation B (advisor).** The band header routed nested-region-relax -> 5d/5z. Resolved by STRIKING it in place ->
CEIR-12 (the scheduler band): the reference executor is SEMANTIC and never consults `region_exec`, so it informs but does
not decide relax. A written promise re-routed with a reason, not dropped.

**⚠ Process notes.** `OpId.value` is `u64` (not u32) — the semantics table `HashMap<u64, EvalFn>` (a narrowing warning
caught it at `-WX`). The lib globs `src/`, so `exec.cpp` needed no CMakeLists edit (checked before assuming). The gate's
CamelCase-suffix locals (`ctxA`) tripped readability-identifier-naming -> snake_case.

`test_exec.cpp` — 9 `[exec]` cases (57 assertions with `[gate5]`): arith incl the two's-complement wrap; if branch +
yield-forward; a for+state sum 0..n-1; the ring across depths (history(1)=register, history(3) delays by 3, warmup);
cross-call state persistence; switch + out-of-range; the 7 typed error modes; the open-world install path. THE GATE
(`test_band5_gate.cpp`, `[gate5]`): the pinned program built twice (builder + text-parse) + binary-round-tripped, all
executing to `@main(6)=118` byte-identically, the in-loop cell reading 15.

## Gate (5z CLOSED — BAND 5 CLOSED)

crd-ceir-tests **171/171 ctest** (160 + 9 `[exec]` + 1 `[gate5]` + 1) on **win-debug . win-asan . linux-gcc-debug .
linux-gcc-asan** (both OSes via `ctest`) + LLVM-20 tidy (4 files clean) + GCC `-Werror=switch` (the 11-case `ExecError` +
8-case `StructureErrorKind` switches) + `crd-ceir-opgen-{drift,validator}` (50 py) + invariants green. **No binary version
bump.** **BAND 5 (structured control flow + functions) CLOSED.** Next = CEIR-6a (BAND 6 OPENS: `ceir.async` ops +
verifier, sec 37/30 — async/task/runtime domains lowering onto crd-jobs).

## Proposed commit — CEIR-5z / BAND-5 CLOSE (user commits; NO AI trailer)

```
feat(ceir-5z): the reference executor (sec 118) -- BAND-5 GATE, band 5 closed

- engine/ceir/exec.{hpp,cpp}: a slow, correct semantic interpreter for the core subset
  (arith + ceir.core control flow + sec 20 state + ceir.func calls). Dispatch is an
  Interpreter-OWNED OpId->EvalFn table (NOT an OpInfo hook -- register_op is first-wins
  idempotent, so a late hook would silently no-op); semantics INSTALLED per dialect
  (open-world); no-semantics op = a typed error, never skipped.
- values are wrapping i64 (via u64 casts -- signed overflow is UB). sec 20 state cells
  are a depth-N ring keyed by the static op instance, persisting across evaluations incl.
  across calls: read at eval, latch cell<-env[next] at block-eval end. state=delay=
  history(1) under one mechanism (pays 5d's depth-taps + update-timing IOUs). Frames per
  func.call (recursion-safe); nested regions share the function frame. Fuel -> a typed
  FuelExhausted (timeout is not a hang-proof). Output = the entry func's return values;
  byte-pin = 8-byte LE per i64. cell_value() inspection (builder-form only).
- ExecResult{values, error, op} over 11 typed failure modes (a no-default -Werror=switch).
  foreach = a NAMED NoSemantics deferral (no collection domain until types). match = switch
  reference behavior. The caller registers the module's dialects (traits are registry state
  -- the 5d finding). An Interpreter is one execution session.
- tests: test_exec.cpp (9 [exec] cases) + test_band5_gate.cpp ([gate5]) -- a pinned program
  (for + match + a value-producing if + calls + a state<T> accumulator across calls) built
  builder AND text-parsed AND binary-round-tripped, all executing to @main(6)=118
  byte-identically.
- band 5 close: 5z row + CEIR-5 header ◧->✅ CLOSED; nested-region-relax re-routed to
  CEIR-12 (the semantic executor never consults region_exec).

Gated: crd-ceir-tests 171/171 ctest on win-debug + win-asan + linux-gcc-debug +
linux-gcc-asan + LLVM-20 tidy (4 files) + GCC -Werror=switch + opgen drift/validator
(50 py) + invariants. No binary version bump.
```
