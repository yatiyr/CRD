# CEIR-11b — Compiled execution plan + differential harness — session log

> The autonomous grind ([[project_ceir_autonomous_loop_grant]]) continued band 11 into the COMPILED tier after 11a
> closed the §118 reference oracle. Design substrate: `docs/design/ceir-11b-compiled-execution-plan.md` (advisor consult).
> Master-map: §84 (two-tier) · §119 (differential) · §153 (hot-path rules).

## CEIR-11b — plan compiler + `CompiledExecutionPlan` + the differential harness

Staged (ONE tracker row): (1) design note · (2) straight-line arith + slot allocator + harness skeleton · (3) control
flow + state · (4) calls + async/task · (5) full-corpus differential + ADR-0123 + close.

### Stage 2 — straight-line arith + the slot allocator + the differential-harness skeleton — ✅ DONE + gated (2026-08-10)

**The representation, per the locked design** (`plan.{hpp,cpp}`, crd-ceir core): the Module is compile-time INPUT ONLY;
the plan owns dense arrays; ⛔ the hot loop touches NO `Operation*` (a runtime authoring-graph walk is itself a §153
violation — the advisor's core correction).

- **`compile(ctx, module, entry)` → `CompiledPlan`:** finds the entry func, walks its straight-line body, and emits a
  dense `Instr{op, operand-slot span, result-slot span, immediate}` per op into `instrs` + the `operand_pool`/`result_pool`
  slot-index arrays. The **compile-time slot allocator** is a `Value* → u32` map (⛔ COMPILE-time only — never the hot
  loop): entry-block args → `param_slots`; each op's result → a fresh dense slot. The **§153 immediates**: the const's
  `"value"` attr and — the headline — the `cmpi` `"predicate"` STRING (read per-eval in the reference) are folded ONCE at
  compile into the `Instr` (a dense `CmpPred` enum), so the loop never touches the attr table or a string.
- **`run(plan, args)`:** a flat `i64` slot array (`num_slots`), args bound to `param_slots`, then the hot loop — a **dense
  `switch(instr.op)`** (a compile-time jump table, NOT a dynamic map lookup; §153-clean) reading operands from
  `slots[operand_pool[...]]` and writing `slots[result_pool[...]]`; the output is the `func.return` operands' slots.
- ⛔ **INDEPENDENT thunks:** the compiled arith/cmpi semantics are re-implemented from the TOML-pinned spec (wrapping-i64
  add/mul; the ten cmpi predicates), NEVER delegating to the reference EvalFns — the differential's value IS that
  independence (delegating would be the bit-exact-blind scar; the conditional-pooling memory demands it).
- **Compile-time rejection** = a typed `CompileError` (`UnsupportedOp` for a stage-3/4 op, `NoEntry`, `BadConst`,
  `BadPredicate`, `ArityUnsupported`): a legitimate tier difference — the differential contract covers programs that
  compile.

**The DIFFERENTIAL harness skeleton** (`test_plan.cpp`, 3 `[ceir][plan]`): a `differential(ctx, m, entry, args)` helper
runs `@entry` through BOTH the reference `Interpreter` (install_builtin + invoke) AND `plan::compile`+`plan::run`, then
`exec::pin_values` **byte-compares** the two result sets. Tests: straight-line arith (`5*5+3 == 28`, both engines agree);
cmpi `sgt` both ways (5>10→0, 20>10→1); the compiler REJECTS an `async.launch` with `UnsupportedOp` + an absent entry
with `NoEntry`. This is the 11a agreement-test shape, now reference-vs-compiled — the seed the full-corpus differential
(stage 5) grows from.

**Gate.** crd-ceir-tests + host + cook **400/400 ctest** (397 + 3) on **win-debug + win-asan + linux-gcc-debug +
linux-gcc-asan** + LLVM-20 tidy (`plan.{hpp,cpp}` + `test_plan.cpp`, clean) + GCC `-Werror=switch` (the `Op`/`CmpPred`/
`CompileError` switches) + `crd-ceir-invariants` (⛔ crd-ceir STILL jobs-free — the plan is pure). ⛔ **NO recook, NO fuzz,
NO binary version bump** (the plan is a runtime object; nothing serializes).

**Next = stage 3** (control flow + state): if/for/while/switch compiled to CHILD instruction arrays (a control-flow thunk
recurses into a child dense sequence by index, never into a Region) + the pre-baked per-block LATCH LISTS for §20 state
cells (dense cell indices + plan-owned ring storage — killing the state-cell `HashMap<Operation*,Cell>`).

## Proposed commit — CEIR-11b stage 2 (user commits; NO AI trailer; NO ADR yet — ADR-0123 at 11b close)

```
feat(ceir-11b): the compiled execution tier -- plan compiler + executor + differential harness (arith)

Stage 2 of CEIR-11b (design note ceir-11b-compiled-execution-plan.md). The COMPILED tier the 11a sec-118
reference oracle differential-compares against. A crd-ceir CORE change (new plan.{hpp,cpp}).

- The representation (advisor-corrected): the Module is compile-time INPUT ONLY; the plan owns dense
  arrays; the hot loop touches NO Operation* (a runtime authoring-graph walk is itself a sec-153
  violation). compile(ctx,module,entry) -> a dense CompiledPlan{Instr[], operand/result slot pools,
  num_slots, param/result slots} for straight-line arith (const/addi/muli/cmpi + func.return).
- sec-153 clean: a compile-time slot allocator (Value*->u32, compile-time only) kills the env map;
  a dense switch(op) kills the dispatch map; the cmpi "predicate" STRING (read per-eval in the reference)
  is folded ONCE at compile into a dense CmpPred enum -- the loop touches no attr table / string / heap.
- INDEPENDENT thunks (never delegate to the reference EvalFns -- the differential's value is that
  independence). Compile-time rejection = a typed CompileError (a legit tier difference; the differential
  covers programs that compile).
- test_plan.cpp (3 [ceir][plan]): the differential harness skeleton -- reference vs compiled,
  pin_values byte-compare (arith 5*5+3=28; cmpi sgt both ways; UnsupportedOp/NoEntry rejects).

Gated: 400/400 on win-debug + win-asan + linux-gcc-debug + linux-gcc-asan + LLVM-20 tidy + GCC
-Werror=switch + crd-ceir-invariants (crd-ceir STILL jobs-free -- the plan is pure). No recook, no fuzz,
no version bump. No ADR (yet -- ADR-0123 at 11b close).
```

### Stage 3a — control flow (if/for/while/switch) compiled to CHILD instruction arrays — ✅ DONE + gated (2026-08-10)

**Split from stage 3** (the advisor sanctioned splitting when large): control flow this stage, §20 state → 3b. Each is
substantial and independently gateable.

**What shipped** (`plan.{hpp,cpp}` restructured; crd-ceir core): the plan is now a TREE of dense **`Seq{instrs,
yield_slots}`** — each Seq OWNS its instrs (a nested `Array<Instr>`, movable, so a control-flow op's child instrs live in
SEPARATE seqs, never interleaved into the parent). A control-flow `Instr` records **child SEQ indices** (into
`child_pool`); the recursive **`compile_seq`** compiles each region's first block → a child seq; ⛔ the executor
**`run_seq` recurses into child seqs BY INDEX** — never an `Operation*`/`Region` (the §153 "no authoring-graph traversal"
rule, honored). The reference semantics reproduced INDEPENDENTLY: `If`/`Scope` forward the child seq's yield slots → the
op's result slots (forward_yield); `For` binds the induction slot (recorded as the Instr's imm) + loops `[lo,hi)` step
(⛔ `RunError::BadForStep` on step≤0); `While` loops {run cond child (yields 1), break on 0, run body child} (`CondArity`);
`Switch`/`Match` run `children[sel]` (`SelectorOutOfRange`). A region's terminator (`core.yield` / `func.return`) is NOT
an instr — its operand slots become the seq's `yield_slots` (matching `run_region`'s terminator capture).

⭐ **New `RunError` / `RunResult`:** control flow introduces RUNTIME errors, so the executor now returns a typed error and
the differential compares the ERROR too (a runtime `BadForStep`/`SelectorOutOfRange`/`CondArity` must agree between the
engines, not just the values). A fuel budget (the reference's 1<<24 default) is the runaway-loop hang-guard.

**Tests.** `test_plan.cpp` +3 `[ceir][plan]`: `core.if` branch-forwarding (10>5→100, 3>5→200, both engines byte-equal);
a bounded `core.for` (a pure no-op body — no state yet — runs 3× and the function returns 42, proving the loop executes
without corrupting slots); the `RunError` agreement (a `for` step-0 → `BadForStep` in BOTH; a `switch` selector-5-of-2 →
`SelectorOutOfRange` in BOTH). ⛔ The value-producing loop (an accumulator) needs §20 state → 3b (the honest boundary:
for/while/switch are statements, so without state their only observable is control reaching an error or a no-op run).

**Gate.** crd-ceir-tests + host + cook **403/403 ctest** (400 + 3) on **win-debug + win-asan + linux-gcc-debug +
linux-gcc-asan** + LLVM-20 tidy (3 files) + GCC `-Werror=switch` (the `Op`/`RunError` switches) + `crd-ceir-invariants`
(crd-ceir jobs-free). ⛔ **NO recook, NO fuzz, NO version bump.**

**Next = stage 3b** (§20 state): dense cell indices + plan-owned ring storage (pre-sized from the compile-time depths) +
pre-baked per-block LATCH LISTS (the read-all-then-latch semantics as a slot list, killing the state-cell
`HashMap<Operation*,Cell>`) — then the for+state accumulator differential (the 11a-style loop that finally produces a value).

## Proposed commit — CEIR-11b stage 3a (user commits; NO AI trailer; NO ADR yet — ADR-0123 at 11b close)

```
feat(ceir-11b): control flow (if/for/while/switch) compiled to child instruction arrays

Stage 3a of CEIR-11b (design note sec 8; split from stage 3 -- state is 3b). A crd-ceir CORE change.

- The plan is now a TREE of dense Seq{instrs, yield_slots}: each Seq owns its instrs (movable nested
  array -> a control-flow op's child instrs live in separate seqs, never interleaved). A control-flow
  Instr records child SEQ indices; the recursive compile_seq compiles each region's block -> a child
  seq; the executor run_seq recurses into child seqs BY INDEX -- never an Operation* (the sec-153
  no-authoring-graph-traversal rule).
- Reference semantics, INDEPENDENTLY: If/Scope forward the child's yield -> results; For binds the
  induction slot + loops (BadForStep on step<=0); While = cond-yields-1 loop (CondArity); Switch/Match
  = run children[sel] (SelectorOutOfRange). The terminator's operand slots become the seq's yield_slots.
- New RunError/RunResult: control flow has runtime errors, so the differential compares the ERROR too
  (a runtime BadForStep/SelectorOutOfRange must agree). A fuel budget (1<<24) is the hang-guard.
- test_plan.cpp +3 [ceir][plan]: if branch-forwarding both ways; a bounded for (no-op statement, no
  state yet); BadForStep + SelectorOutOfRange agreement.

Gated: 403/403 on win-debug + win-asan + linux-gcc-debug + linux-gcc-asan + LLVM-20 tidy + GCC
-Werror=switch + crd-ceir-invariants (crd-ceir jobs-free). No recook, no fuzz, no version bump.
No ADR (yet -- ADR-0123 at 11b close).
```

### Stage 3b — §20 state (dense cells + plan-owned rings + pre-baked latch lists) — ✅ DONE + gated (2026-08-10)

**What shipped** (`plan.{hpp,cpp}` extended; crd-ceir core): `core.state`/`delay`/`history` compile to `Op::State` — a
depth-N **ring keyed by a dense CELL index** (the Instr's imm), ⛔ replacing the reference's `HashMap<Operation*,Cell>`.
Per-run storage is a `containers::Array<Ring>` (one `Ring{ring, pos, init}` per dense cell; ring pre-sized from the
compile-time `cell_depths[cell]`). `Op::State` = the reference cell_read reproduced INDEPENDENTLY: on first eval it
init-fills the ring from operand(0) (init) and sets `init`; the result slot = `ring[pos]`. The **latch** is a pre-baked
`Latch{cell, next_slot}` list on each `Seq` — at the SEQ (block) eval END, `ring[pos] = slots[next_slot]; pos =
(pos+1) % depth` (⛔ read-all-then-latch, matching `eval_block`'s block-end latch, no runtime trait scan — the §153 win).

⭐ **The feedback-edge bug (found by the for+state test, fixed):** a state cell's `next` operand is a FEEDBACK edge —
it names a value defined LATER in the same block (e.g. `state(0, acc+iv)`, where `acc+iv` is an `addi` after the state
op). The compiler slots operands eagerly in program order, so `slot_of(next)` rejected the forward reference
(`UnsupportedOp`). Fix: a state op pushes ONLY operand(0) (init) to its operand span; the `next` slot is **deferred** —
recorded as `{cell, next VALUE}` and resolved into the `Latch` AFTER the whole block compiles (by then `next` is
slotted). This is exactly the read-all-then-latch contract expressed at compile time: the latch fires at block END, so
its operand need only be defined by block end, not at the state op's position.

⭐ **Cell inspection (`RunResult.cells`):** a for+state accumulator's value lives in the CELL, not the return — the loop
is a statement. So the differential grew a second axis: `RunResult.cells[i]` = each cell's current `ring[pos]` after the
run, byte-compared to the reference's `cell_value(op)` (the 11a-style `map_output` inspection parity, §118).

**Tests.** `test_plan.cpp` +4 `[ceir][plan]`: a `state(7, 0)` whose result reaches `return` (cell_read yields the init
`7`, both engines agree on the value); a `core.for(0,3,1)` with a `state(0, acc+iv)` accumulator — the reference
`cell_value(acc) == 3` (0+1+2) and the compiled `RunResult.cells[0] == 3` agree (the LATCHED cell, cross-checked via
cell inspection), and the `return 0` also agrees; ⭐ a **depth-2** delay-line WITNESS (`for(0,4,1)` + `state[depth=2](0,
iv)` reads `0,0,0,1` and ends `ring[pos]==2` — a depth-1 cell would leave `3`, so the value discriminates the ring
wraparound + whole-ring init-fill, not just cell existence); a `core.delay(3,0)` + `core.history(4,0)` → `3+4=7`
routing/multi-cell test (the reference installs both on the SAME `eval_state`; the classifier routes both to `Op::State`,
two cells coexist) — the advisor-mandated coverage of depth≥2 + the delay/history arms (else compiled-accepts-what-
reference-errors would surface only at the stage-5 corpus, the inverse of the never-reject scar).

**Gate.** crd-ceir-tests + host + cook **407/407 ctest** (403 + 4) on **win-debug + win-asan + linux-gcc-debug +
linux-gcc-asan** + LLVM-20 tidy (`plan.{hpp,cpp}` + `test_plan.cpp`, clean) + GCC `-Werror=switch` (the `Op::State`
arm) + opgen validator/drift + `crd-ceir-invariants` (I3/I5/I6/U-116; crd-ceir STILL jobs-free — the rings are plain
arenas). ⛔ **NO recook, NO fuzz, NO version bump.**

**Next = stage 4a** (calls): compiled-function indices + arena call frames (async/task → 4b).

**⚠ Stage-5 watch-list (advisor, non-blocking — resolve when the corpus harness compares cells generically):**
(a) **Uninit-cell convention** — the compiled `RunResult.cells[i]` reports `0` for a state op in an UNTAKEN branch
(never read), whereas the reference `cell_value` reports ABSENT (`false`). The single-cell 3b tests dodge this (the cell
is always read), but the generic corpus cell-compare needs an agreed sentinel/skip before it holds. (b) **Deferred-latch
nesting** — the deferred `slot_of(next)` shares one block-scoped slot map, so it would happily resolve a `next` that lives
in a NESTED child region; confirm the 5d state verifier forbids a cell whose `next` escapes its own block (it should — §20
is block-local), else that's a silent semantic hole to close before the corpus. (c) **Defined/undefined erasure**
(advisor, 4a) — the compiled tier's zero-filled frames erase the reference's UNSET-vs-set distinction: a program whose
reference run hits `UndefinedValue` (an over-declared call result read, or any undominated read) diverges (reference
errors, compiled returns 0), and there is no `RunError::UndefinedValue` to pair them. Such programs sit OUTSIDE the
differential contract — same status as programs that don't compile, and the general form of item (a). State this in the
stage-5 harness header.

## Proposed commit — CEIR-11b stage 3b (user commits; NO AI trailer; NO ADR yet — ADR-0123 at 11b close)

```
feat(ceir-11b): sec-20 state -- dense cells + plan-owned rings + pre-baked latch lists

Stage 3b of CEIR-11b (design note sec 8; state, split from stage 3). A crd-ceir CORE change.

- core.state/delay/history compile to Op::State: a depth-N ring keyed by a dense CELL index (the
  Instr imm), replacing the reference HashMap<Operation*,Cell>. Per-run storage is an Array<Ring>
  (ring pre-sized from the compile-time cell_depths). cell_read reproduced INDEPENDENTLY: first eval
  init-fills the ring from operand(0), result = ring[pos]. The latch is a pre-baked Latch{cell,
  next_slot} list per Seq: at block-eval END, ring[pos]=slots[next_slot]; pos=(pos+1)%depth
  (read-all-then-latch, no runtime trait scan -- the sec-153 win).
- The feedback-edge fix: a cell's `next` names a value defined LATER in the block (state(0, acc+iv)).
  Eager operand slotting rejected the forward ref, so a state op now pushes ONLY operand(0) (init);
  the `next` slot is deferred and resolved into the Latch after the whole block compiles (the latch
  fires at block END, so `next` need only be defined by then).
- RunResult.cells: a for+state accumulator's value lives in the cell, not the return, so the
  differential grew cell inspection -- cells[i]=ring[pos] after the run, byte-compared to the
  reference cell_value (the 11a map_output parity, sec-118).
- test_plan.cpp +4 [ceir][plan]: state(7,0) cell_read returns the init (7); a for+state accumulator
  latches 0+1+2=3 in BOTH engines (compiled cells[0] == reference cell_value); a depth-2 delay-line
  witness (reads 0,0,0,1 -> ring[pos]==2, discriminating depth-1's 3); delay(3)+history(4)=7 routing.

Gated: 407/407 on win-debug + win-asan + linux-gcc-debug + linux-gcc-asan + LLVM-20 tidy + GCC
-Werror=switch + opgen + crd-ceir-invariants (crd-ceir STILL jobs-free -- the rings are arenas).
No recook, no fuzz, no version bump. No ADR (yet -- ADR-0123 at 11b close).
```

### Stage 4a — func.call (compiled-function indices + arena frame windows) — ✅ DONE + gated (2026-08-10)

**Split from stage 4** (calls now, async/task → 4b — the frame-window restructure is the heavy lift and independently
gateable). Advisor-consulted at the design fork (the restructure is verbatim in the design note §2.3: "compiled-function
index; frames are windows into a plan-owned slot stack").

**What shipped** (`plan.{hpp,cpp}`; crd-ceir core): the plan is now a table of **`CompiledFn{entry_seq, num_slots,
param_slots}`** with **FRAME-RELATIVE slots** — each function's slot allocator starts at 0. `compile` is a **work-list over
the call graph**: @entry is fn 0, and every `func.call` resolves its callee ONCE at compile (`resolve_call` → a
compiled-function INDEX in the `Instr` imm), assigning the callee's index *before* its body compiles (so recursion /
mutual recursion resolves) and enqueuing it. ⛔ Each function gets a FRESH slot map (frame-relative); seqs/pools/cells/the
fn-table stay plan-global. The executor `run_seq` now threads a **`frame_base`**: every slot access is `stack[base +
slot]` on a growing **slot STACK**; control-flow child seqs share the parent's base, and only **`Op::Call`** pushes a
WINDOW (base = stack size, grow by `callee.num_slots` zero-filled, bind args caller→callee frame, run the callee entry
seq, **min-copy yields → results BEFORE popping**, shrink back — capacity retained, amortized arena). Recursion is shallow
by construction — both tiers recurse on the C++ stack, which dies at thousands of frames long before the 1<<24 fuel could
fire, so deep recursion is a shared native-stack limit (not a fuel guard). ⛔ The realloc discipline (advisor item 1 — the
push_back-UAF scar in new clothes): NEVER hold a slot pointer/`i64&` across the callee run — index through the Array
everywhere (ASan-clean confirms it).

⭐ **Cells stay GLOBAL per-op** — `m_cells` keys by op pointer, so a callee's state cell is shared across every
invocation; the dense cell index (assigned once at compile) already matches, and the per-seq latch runs once per
block-execution. A callee called twice latches its ONE cell twice (witnessed below).

⭐ **Error tiers (advisor item 6):** an unresolved callee or a call/param arity mismatch — both STRUCTURAL (static counts)
— fail the COMPILER (`CompileError::UnresolvedCall` / `CallArity`), where the reference fails at RUNTIME
(`UnresolvedCall` / `BadArity`); the §4 tier-difference (the differential covers programs that compile). Result copy is
`min(results_cnt, yields.size())` — the COPY COUNT matches the reference; the RESIDUE differs (reference leaves extras
UNSET → `UndefinedValue` if read; our zero-filled frame yields `0`). ⛔ NOT an arity error (never reject what the
reference ran — it runs fine when the extra result is never read; the defined/undefined gap → stage-5 watch item (c)).

**Tests.** `test_plan.cpp` +5 `[ceir][plan]`: a basic `call add1(41)==42` (frame window + arg bind + return); a nested
`add2→inc→inc` (stacked frames, 42); ⭐ shallow **recursion** `sum(5)==15` (base-case `if` + `call sum(n-1)`, proving the
pre-assigned index resolves the self-call, depth 6 — shallow per advisor item 8); ⭐ a **state-cell-in-callee-called-twice**
witness (`tick(){ s=state(0,s+1); return s }` called twice → `0+1==1`, and `cells[0]==2`; a per-FRAME cell would give
`0+0==0`, so the value discriminates global-vs-per-frame) vs the reference `cell_value`; the compiler REJECTS an
unresolved callee (`UnresolvedCall`) + a 2-arg call of a 1-param func (`CallArity`).

**Gate.** crd-ceir-tests + host + cook **412/412 ctest** (407 + 5) on **win-debug + win-asan + linux-gcc-debug +
linux-gcc-asan** (win-asan + linux-gcc-asan the frame-stack UAF money configs) + LLVM-20 tidy (`plan.{hpp,cpp}` +
`test_plan.cpp`, clean) + GCC `-Werror=switch` (the `Op::Call` + `UnresolvedCall`/`CallArity` arms) + opgen + `crd-ceir-
invariants` (I3/I5/I6/U-116; crd-ceir STILL jobs-free/asset-free — the slot stack is a plain arena). ⛔ **NO recook, NO
fuzz, NO version bump.**

**Next = stage 4b** (async/task): the token store + the six async/task thunks.

## Proposed commit — CEIR-11b stage 4a (user commits; NO AI trailer; NO ADR yet — ADR-0123 at 11b close)

```
feat(ceir-11b): func.call -- compiled-function indices + arena frame windows

Stage 4a of CEIR-11b (design note sec 2.3 + 8; calls, split from stage 4 -- async/task is 4b). A
crd-ceir CORE change (the executor gains frame windows).

- The plan is now a table of CompiledFn{entry_seq, num_slots, param_slots} with FRAME-RELATIVE slots.
  compile() is a work-list over the call graph: @entry is fn 0; every func.call resolves its callee
  ONCE at compile (resolve_call -> a compiled-function INDEX in the Instr imm), assigning the index
  BEFORE the body compiles (recursion/mutual recursion resolves) and enqueuing it. Each function gets
  a FRESH frame-relative slot map; seqs/pools/cells/the fn-table stay plan-global.
- run_seq threads a frame_base: every slot is stack[base+slot] on a growing slot STACK; control-flow
  child seqs share the base, only Op::Call pushes a WINDOW (grow by callee num_slots, bind args
  caller->callee, run, min-copy yields->results before popping, shrink -- amortized arena; recursion
  is shallow by construction, deep recursion exhausts the native C++ stack in BOTH tiers before fuel).
  The realloc discipline: never hold a slot ref across the callee run -- index through the Array
  (ASan-clean).
- Cells stay GLOBAL per-op (m_cells keys by op pointer): a callee's cell is shared across invocations;
  the dense cell index already matches, the per-seq latch fires once per block-execution.
- Error tiers: unresolved callee / call-param arity mismatch (both structural) fail the COMPILER
  (UnresolvedCall / CallArity) where the reference fails at runtime (sec-4 tier-difference). Result
  copy is min(results, yields) -- the copy COUNT matches the reference; the residue differs
  (reference unset -> UndefinedValue if read, compiled zero).
- test_plan.cpp +5 [ceir][plan]: basic call (42); nested add2->inc->inc (42); shallow recursion
  sum(5)=15; a state-cell-in-callee-called-twice witness (0+1=1, cells[0]=2 -- a per-frame cell would
  give 0) vs reference cell_value; unresolved-callee + call-arity compile rejects.

Gated: 412/412 on win-debug + win-asan + linux-gcc-debug + linux-gcc-asan + LLVM-20 tidy + GCC
-Werror=switch + opgen + crd-ceir-invariants (crd-ceir STILL jobs-free/asset-free -- the slot stack
is a plain arena). No recook, no fuzz, no version bump. No ADR (yet -- ADR-0123 at 11b close).
```

### Stage 4b — async/task (run-global token store + the six §37/§38 thunks) — ✅ DONE + gated (2026-08-10)

**What shipped** (`plan.{hpp,cpp}`; crd-ceir core): the SEQUENTIAL §37/§38 reference, compiled. A **run-global TOKEN STORE**
(`Array<Array<i64>>` in `RS`, grows per launch — ⛔ NOT per-frame; `store_token` appends + returns the handle, mirroring
the reference `store_yields`) + six new INDEPENDENT thunks, **all running in the CURRENT frame** (only `func.call` pushes):
- **`Op::Launch`** (async.launch / task.spawn/main_thread/worker → the shared `eval_launch`): run the body child seq NOW
  in the current frame (captures resolve), store its yields → a handle; result = handle.
- **`Op::Await`** (async.await / task.fiber_wait): op0 = token; a bad handle → `RunError::BadToken`; min-copy the stored
  yields → results.
- **`Op::Continuation`** (task.continuation): read the antecedent token, ⛔ **COPY its yields to a local BEFORE running
  the body** (the body may `store_token`, reallocating the store out from under a span — the reference's exact hazard),
  bind them as the body's block-args (`Seq::arg_slots`, recorded at compile), run, store the body's yields → a new
  handle. A body-arg-count ≠ antecedent-yield-count (DYNAMIC) → `RunError::ContinuationArity` (reference `BadArity`).
- **`Op::Join`** (async.join): read every operand token's yields into `merged` (spans stay valid — the store is not grown
  mid-loop), then ONE `store_token(merged)` (the reference order); result = handle.
- **`Op::Race`** (async.race): the sequential winner is index **0** (deterministic); result = 0. ⛔ validates NO handle
  (the reference does `value_of` only — validating would DIVERGE).
- **`Op::Cancel`** (async.cancel): consume op0, pure no-op (real cancellation is CEIR-6c). ⛔ validates NO handle.
- async.scope / task.group fold into the existing `Op::Scope` (a no-op structured boundary forwarding the yield).

⭐ **The `Seq::arg_slots` addition:** each compiled block now records its arg slots (frame-relative), so a continuation can
bind the antecedent's dynamic yields into the body's args at run time (the For induction slot already rode `imm`; this
generalizes it for the continuation's N-ary bind).

⭐ **NAMED FORWARD → STAGE 4c — task.parallel_for / map_reduce:** these run an ISOLATED sub-interpreter with captures
pinned `UndefinedValue` (the §118 6z fold). They currently compile-reject as `UnsupportedOp`; the advisor (pre-close)
corrected the deferral rationale — this is NOT a provider-tier punt but a **small stage 4c**: compile the body as a
mini-function (a **fresh slot map**, so a capture fails `slot_of` at COMPILE — the §4 pattern: the reference errors on a
capture-read at runtime, the compiler rejects it structurally), then non-capturing bodies (the only ones the reference
runs clean) execute in a 4a frame WINDOW + a per-index run + the independent index-order fold + the state-free preflight
as a compile check. ⛔ Also a locked stage-5 corpus entry ("the 6z map-reduce"), so it must be built (4c), not dropped.

⭐ **The token store is RUN-GLOBAL (advisor):** a token launched in a CALLEE and awaited in the CALLER must survive the
callee's frame pop — witnessed below (a per-frame store would `BadToken`).

**Tests.** `test_plan.cpp` +9 `[ceir][plan]`: launch→await round-trip (42); ⭐ a **token launched in a callee, awaited in
the caller** (run-global store witness — 7; a per-frame store would `BadToken`); a **continuation** chain (`launch{10}` →
`continuation{x: x+5}` → await → 15, the antecedent bound as the body arg); **join** concat (`[3]`+`[4]` → await-2 → 3+4
= 7); **race** → 0; **cancel** no-op (8); **task.spawn→fiber_wait** routing (9, the task dialect → `Op::Launch`/`Await`);
⭐ **BadToken** agreement (`await(const 99)` → reference `ExecError::BadToken` AND compiled `RunError::BadToken` — the EXACT
error, per the advisor: `!ok` alone couldn't tell BadToken from NoSemantics); ⭐ **ContinuationArity** agreement (a 1-yield
antecedent into a 2-arg body → reference `BadArity` AND compiled `RunError::ContinuationArity`). The differential helper
now installs `install_async_semantics` + `install_task_semantics` (SEPARATE from `install_builtin_semantics` — the
yield-store dialects).

⭐ **The i64≥2³² handle-wrap (advisor):** the reference guards a token with `valid_yield_handle(static_cast<u32>(tok))` —
a 2³²+ handle TRUNCATES and can read a live token. All three sites (Await/Continuation/Join) now u32-truncate identically
(no divergence even on corpus-unreachable inputs), rather than comparing the full u64.

**Gate.** crd-ceir-tests + host + cook **421/421 ctest** (412 + 9) on **win-debug + win-asan + linux-gcc-debug +
linux-gcc-asan** (the ASan configs validate the token-store realloc + the continuation copy-before-store discipline) +
LLVM-20 tidy (`plan.{hpp,cpp}` + `test_plan.cpp`, clean) + GCC `-Werror=switch` (the six new `Op` arms + `BadToken`/
`ContinuationArity`) + opgen + `crd-ceir-invariants` (I3/I5/I6/U-116; crd-ceir STILL jobs-free/asset-free — the token
store is a plain per-run arena). ⛔ **NO recook, NO fuzz, NO version bump.** ⭐ **Closes the async/task TOKEN-STORE op
class; the data-parallel pair (parallel_for/map_reduce) is stage 4c — so stage 4 is NOT yet complete.**

**Next = stage 4c** (task.parallel_for / map_reduce): the mini-function compile + per-index frame-window run + index-order fold.

## Proposed commit — CEIR-11b stage 4b (user commits; NO AI trailer; NO ADR yet — ADR-0123 at 11b close)

```
feat(ceir-11b): async/task -- run-global token store + the six sec-37/38 thunks

Stage 4b of CEIR-11b (design note sec 8; async/task, completing stage 4). A crd-ceir CORE change.

- A run-global TOKEN STORE (Array<Array<i64>> in RS, grows per launch -- NOT per-frame; store_token
  appends + returns the handle, mirroring the reference store_yields) + six INDEPENDENT thunks, all
  running in the CURRENT frame (only func.call pushes):
  * Op::Launch (async.launch / task.spawn/main_thread/worker): run the body NOW, store its yields,
    result = handle.
  * Op::Await (async.await / task.fiber_wait): op0=token; bad handle -> RunError::BadToken; min-copy
    stored yields -> results.
  * Op::Continuation (task.continuation): COPY the antecedent yields BEFORE running the body (the body
    may store_token, reallocating the store under a span), bind them as the body args (Seq::arg_slots),
    run, store -> new handle. Body-arg-count != antecedent-yield-count -> RunError::ContinuationArity.
  * Op::Join: read all operand tokens' yields into merged, then ONE store. Op::Race: winner index 0,
    validates NO handle. Op::Cancel: consume op0, no-op, validates NO handle. async.scope/task.group
    fold into Op::Scope.
- Seq::arg_slots records each block's frame-relative arg slots (the continuation binds the antecedent's
  dynamic yields there; generalizes the For induction slot).
- NAMED FORWARD -> STAGE 4c: task.parallel_for / map_reduce run an isolated sub-interpreter with
  captures pinned UndefinedValue. They compile-reject as UnsupportedOp for now; the fix is a small 4c
  (compile the body with a FRESH slot map so a capture fails slot_of at COMPILE -- the sec-4 pattern --
  then a per-index frame-window run + the independent index-order fold), NOT a provider punt.
- The token guard u32-truncates the handle EXACTLY like the reference valid_yield_handle (a 2^32+
  handle wraps identically in both tiers) at all three sites (Await/Continuation/Join).
- test_plan.cpp +9 [ceir][plan]: launch->await (42); a token launched in a CALLEE awaited in the CALLER
  (run-global store witness, 7); a continuation chain (15); join concat (7); race->0; cancel no-op (8);
  task.spawn->fiber_wait routing (9); BadToken agreement (await const 99 -> the EXACT ExecError::BadToken,
  not merely !ok); ContinuationArity agreement (1-yield antecedent into a 2-arg body -> BadArity). The
  differential helper now installs async + task semantics (SEPARATE installers from install_builtin).

Gated: 421/421 on win-debug + win-asan + linux-gcc-debug + linux-gcc-asan + LLVM-20 tidy + GCC
-Werror=switch + opgen + crd-ceir-invariants (crd-ceir STILL jobs-free/asset-free -- the token store is
a per-run arena). No recook, no fuzz, no version bump. No ADR (yet -- ADR-0123 at 11b close). Closes the
async/task TOKEN-STORE op class; the data-parallel pair is stage 4c (stage 4 not yet complete).
```

### Stage 4c — data-parallel (isolated body fns + per-index run + index-order fold) — ✅ DONE + gated (2026-08-10)

**COMPLETES STAGE 4** — the mandate-consistent build of the pair 4b named-forward (the advisor corrected the deferral).
`task.parallel_for` / `task.map_reduce`, the SEQUENTIAL 6z-fold reference, compiled.

**What shipped** (`plan.{hpp,cpp}`; crd-ceir core):
- **Isolated mini-function bodies:** the map/combine body compiles via **`compile_fn_body`** — a **FRESH slot map**, so an
  outer CAPTURE fails `slot_of` at COMPILE (`CompileError::CapturedValue`; the reference errors on the capture-read at
  runtime — the §4 pattern). It's a `CompiledFn` (own frame, own num_slots), run per-index in a 4a frame WINDOW. ⛔
  `compile_fn_body` does NOT hold a `funcs`/`seqs` reference across its inner `compile_seq` (a nested op reallocs `funcs`
  — index fresh after, the push_back-UAF scar's compile-time edition).
- **SHARED pre-flight (the design fork — advisor: reuse):** at compile, `exec::check_parallel_region` (moved to core in
  11a for exactly this — legality analysis is the explicitly shared layer, execution stays independent) validates the map
  region (1 arg, yields 1, transitively state-free) + the combine region (2 args, yields 1). Its `ExecError` maps to a
  `CompileError` (`ParallelArity`/`ParallelYield`/`ParallelStateful`/`UnresolvedCall`). ⭐ Sharing the pre-flight makes
  accept/reject agreement true BY CONSTRUCTION — a drifted reimplementation could compile-accept a stateful body the
  reference rejects (the dangerous direction).
- **The thunks (independent execution):** `Op::ParallelFor` — per index in index order, run the map fn (arg=iv), collect
  the one yield → `map_output[imm]` (a statement, no SSA result). `Op::MapReduce` — the map, then FOLD the combine fn
  (args=acc,elem) in INDEX order; result = the reduced acc. `count = (hi>lo)?(hi-lo+step-1)/step:0` (verbatim); `step<=0`
  → `RunError::BadForStep`. ⛔ **Fuel is snapshot/restored** around the body loops — the reference gives the body a
  SEPARATE budget (a fresh sub per phase), so the parent is uncharged. ⛔ `child_pool` holds **compiled-FUNCTION indices**
  for dp ops (not seq indices — documented at the `Instr` field + both thunks; a trap for the 11z audit otherwise).
- ⭐ **FULL body isolation — the fresh per-phase TOKEN STORE (advisor pre-close):** the reference sub isolates THREE
  things — fresh env (the frame window), fresh cells (the preflight rejects state), and **fresh `m_yield_store`**. The
  preflight guards `StateEdge` only, so a `launch`/`await`/`join` INSIDE a parallel body passes it and the reference runs
  it against an EMPTY, per-phase, DISCARDED token store. An RAII **`TokenScope`** now swaps the run-global `rs.tokens` for
  an empty scratch at each phase start and restores+discards at phase end (MapReduce's fold gets its own — two subs).
  Without it: a body `await(parent-handle)` would WRONGLY resolve (reference: `BadToken`) — compiled-accepts-what-the-
  reference-rejects, the dangerous direction — and a body-launched handle would leak the parent's numbering.
- **`RunResult.map_outputs`** (`Array<Array<i64>>`, dense map index) — the §118 inspection parity, compared to the
  reference `map_output(op)`. No empty-vs-absent watch item: the reference `map_output` returns `{}` for both find-fail and
  a stored-empty range, and the compiled default-empty agrees with both (advisor-confirmed).

**Tests.** `test_plan.cpp` +6 `[ceir][plan]`: ⭐ a **parallel_for map inspection** (`iv²` over [0,4) → `map_outputs[0] ==
in.map_output(op) == [0,1,4,9]`); ⭐ a **NON-ASSOCIATIVE map_reduce** (`acc*2 + elem` over [0,4) init 0 → 11 — an
associative fold would prove nothing about order; this is the index-order witness, per the 11a precedent); a **capture**
reject (`CapturedValue`); a **stateful** body reject (reference RUNTIME `ParallelBodyStateful` vs compiled COMPILE
`ParallelStateful` — paired exact errors, NOT a differential — the tier shapes differ); a **BadForStep** runtime agreement
(step 0 is dynamic → both error at run); an **empty range** (both yield empty maps); ⭐ a **fresh-store `await`** witness (a
parent launches handle 0, the body does `await(const 0)` → `BadToken` in BOTH — without the swap the compiled body would
resolve the parent's handle); ⭐ a **fresh-store numbering** witness (a parent holds handle 0, the body launches + yields
its handle → map `[0,1]` in BOTH, not the parent-global `[1,2]`). The unsupported-op reject test migrated from
`task.parallel_for` (now supported) to **`core.foreach`** (no compiled semantics, no collection domain).

**Gate.** crd-ceir-tests + host + cook **429/429 ctest** (421 + 8) on **win-debug + win-asan + linux-gcc-debug +
linux-gcc-asan** (the ASan configs validate the map/combine frame-window realloc + isolated-fn compile + the TokenScope
move/discard) + LLVM-20 tidy
(`plan.{hpp,cpp}` + `test_plan.cpp`, clean) + GCC `-Werror=switch` (the `ParallelFor`/`MapReduce` `Op` arms + 4 new
`CompileError` arms) + opgen + `crd-ceir-invariants` (I3/I5/I6/U-116; crd-ceir STILL jobs-free/asset-free — the shared
pre-flight `check_parallel_region` is core + jobs-free, and `plan.cpp` including `exec.hpp` stays within core). ⛔ **NO
recook, NO fuzz, NO version bump.** ⭐ **STAGE 4 is now COMPLETE — the full op-class coverage (arith, control flow, state,
calls, async/task, data-parallel), no named-forward remaining.**

⭐ **Stage-5 watch item (d) — nested dp `map_output` (advisor):** a dp op nested INSIDE a parallel body runs on the
reference's sub, whose `m_map_output` is discarded — so the parent-visible `map_output(nested_op)` is find-fail/empty,
while the compiled tier writes the dense parent-visible slot. Inspection-only (NO acceptance effect; the tokens ARE
isolated via `TokenScope`). The proportionate call (per-phase map_outputs swap would cost a num_maps scratch per exec):
stage-5's generic `map_output` compare SKIPS dp ops nested inside dp bodies. State it in the harness-header list with
(a)–(c).

**Next = stage 5** (full-corpus differential + ADR-0123 + 11b close): the 5z/6z/exec/async/task corpus via a SHARED BUILDER
header (reference-vs-compiled over values + cells + map_outputs), the stage-5 watch items stated in the harness header
(uninit cells; deferred-latch nesting; the defined/undefined erasure — incl. ⭐ a capture in a DEAD branch of a parallel
body compile-rejects while the reference runs fine, a §4 tier-difference like `BadPredicate`; and (d) nested dp
map_output), ADR-0123, the 11b row flip.

## Proposed commit — CEIR-11b stage 4c (user commits; NO AI trailer; NO ADR yet — ADR-0123 at 11b close)

```
feat(ceir-11b): data-parallel -- parallel_for/map_reduce (isolated body fns + index-order fold)

Stage 4c of CEIR-11b (design note sec 8; the data-parallel pair, COMPLETING stage 4). A crd-ceir CORE
change. The advisor-corrected build of the 4b named-forward (NOT a provider punt).

- The map/combine body compiles as an ISOLATED mini-function (compile_fn_body, a FRESH slot map) so an
  outer CAPTURE fails slot_of at COMPILE (CapturedValue -- the reference errors on the capture-read at
  runtime, the sec-4 pattern). It runs per-index in a 4a frame WINDOW. compile_fn_body never holds a
  funcs/seqs ref across its inner compile_seq (realloc; index fresh).
- SHARED pre-flight: at compile, exec::check_parallel_region (moved to core in 11a for exactly this --
  legality analysis is the shared layer, execution independent) validates the map region (1 arg,
  yields 1, state-free) + combine (2 args, yields 1); its ExecError maps to a CompileError. Sharing it
  makes accept/reject agreement true by construction (a drifted reimpl could accept a stateful body).
- Thunks (independent): Op::ParallelFor maps the range -> map_output[imm] (statement); Op::MapReduce
  maps then folds the combine fn in INDEX order -> the reduced SSA result. count = the verbatim
  reference expr; step<=0 -> BadForStep. Fuel is snapshot/restored around the body loops (the
  reference gives the body a separate budget). child_pool holds compiled-FUNCTION indices for dp ops.
- FULL body isolation: the reference sub isolates env + cells + m_yield_store. A RAII TokenScope gives
  each parallel body phase a FRESH per-phase token store (swap the run-global rs.tokens out, discard the
  body's tokens after; the fold gets its own -- two subs). Without it a body await(parent-handle) would
  WRONGLY resolve (reference: BadToken -- the dangerous accept direction) and body-launched handles
  would leak the parent's numbering.
- RunResult.map_outputs (dense map index) = the sec-118 inspection parity vs the reference map_output
  (default-empty agrees with the reference's find-fail-or-stored-empty; no watch item). Watch (d): a
  nested dp op's map_output is sub-local in the reference (discarded) -- stage-5 skips nested dp ops.
- test_plan.cpp +8 [ceir][plan]: a parallel_for map inspection (iv^2 = [0,1,4,9] vs in.map_output);
  a NON-ASSOCIATIVE map_reduce (acc*2+elem = 11, the index-order witness); capture reject
  (CapturedValue); stateful body reject (reference runtime ParallelBodyStateful vs compiled compile
  ParallelStateful, paired); BadForStep runtime agreement; empty range; a fresh-store await witness
  (parent handle 0 + body await(0) -> BadToken both); a fresh-store numbering witness (body launches
  number from 0 -> map [0,1] both, not parent-global [1,2]). Unsupported-op reject -> core.foreach.

Gated: 429/429 on win-debug + win-asan + linux-gcc-debug + linux-gcc-asan + LLVM-20 tidy + GCC
-Werror=switch + opgen + crd-ceir-invariants (crd-ceir STILL jobs-free/asset-free -- check_parallel_region
is core + jobs-free). No recook, no fuzz, no version bump. No ADR (yet -- ADR-0123 at 11b close).
COMPLETES STAGE 4 (calls + async/task + data-parallel).
```

### Stage 5 — the FULL-CORPUS differential + §121 plan-layer twin + ADR-0123 — ✅ DONE + gated (2026-08-10) → CEIR-11b CLOSED

**The SHARED corpus builder** (`tests/ceir/corpus.hpp`, the `rich_graph.hpp` / 1f precedent — build each program ONCE, run
it through BOTH engines). Five programs, each returning `Built{module, cells[], maps[]}` (the §20 state ops + dp ops in
COMPILE ORDER, for the reference `cell_value`/`map_output` inspection): **P1 5z** (the band-5 gate — loop + match +
value-producing if + calls + a §20 accumulator across calls; @main(6)→118); **P2 6z** (non-associative map_reduce, map
iv²/combine acc·31+elem → 33930); **P3 async** (launch/await/join/continuation → 29); **P4 the six task ops**
(spawn/main_thread/worker/group/fiber_wait/continuation → 46); **P5 composing** (all dialects: arith + cf + 2 §20 cells +
calls + async + task + a parallel_for → 118, cells [10,20], map [0,1,4]). ⛔ **build_5z is REUSED EXACTLY** —
`test_band5_gate.cpp` was refactored to consume it (the "build once" extraction; no drift).

**The corpus differential** (`test_plan.cpp`, +6 `[ceir][plan]`): each program through the reference interpreter AND the
compiled plan; `pin_values` byte-compare of VALUES + handle-based compare of CELLS (compile order; watch (a): read →
equal, unread → 0) + MAP_OUTPUTS (top-level dp; watch (d): skip nested-dp). The differential CONTRACT (the §4 tier
boundary + the accumulated watch items (a)–(d)) is stated ONCE in the harness header.

⭐ **§121 no-privileged-path AT THE PLAN LAYER:** compile the builder-form module AND a **cooked→loaded twin**
(`serialize` → `deserialize` into a fresh context with the dialects re-registered) — run BOTH through the COMPILED tier
and compare values + cells + map_outputs ARRAYS. ⛔ NO handles: the loaded module's ops are different pointers, but dense
cell/map indices are assigned by COMPILE ORDER, so structurally-identical twins align by construction.

⭐⭐ **THE CORPUS DID ITS JOB — a real stage-3a bug, caught by the 5z differential and FIXED:** compiling a control-flow
op captured `instr.children_off = child_pool.size()` BEFORE compiling its region children — but a child that itself
contains NESTED control flow (a for-body containing a `match`) pushes ITS children to `child_pool` first, so the parent's
`children_off` aliased a nested op's child seqs. `Op::For` ran the match's `arm0` (`acc(iv)`) every iteration instead of
the loop body → 15, not 18. Every simpler test missed it (stage-3a's for-body was a no-op; the composing for-body is a
dead `iv²` — no NESTED control flow). **Fix:** compile the region children FIRST (into a local buffer), THEN reserve a
CONTIGUOUS block in `child_pool` and set `children_off` — so nested pushes can't shift the parent's slot. Pinned by a
minimal **nested-cf regression** test (a `for` containing an `if` driving a §20 accumulator → 22) + the 5z corpus program.

**Gate (this checkpoint).** crd-ceir-tests + host + cook **436/436 ctest** (429 + 6 corpus + 1 nested-cf regression) on
**win-debug + win-asan + linux-gcc-debug + linux-gcc-asan** + LLVM-20 tidy (`plan.cpp` + `test_plan.cpp` +
`test_band5_gate.cpp` clean; `corpus.hpp` checked via its consumers) + GCC `-Werror=switch` + opgen + `crd-ceir-invariants`
(crd-ceir STILL jobs-free/asset-free). ⛔ **NO recook, NO fuzz, NO version bump.**

**✅ CLOSED (part 2).** **ADR-0123 ACCEPTED** (the compiled execution plan — the representation [Module = compile-time
INPUT only; a dense `Seq` tree; frame windows; run-global tokens + per-phase isolation; dense cells/latches; map_outputs],
the three killed §153 violations, independence [thunks share the SPEC never code; the ONE shared *analysis* layer is the
parallel pre-flight], the **§4 error-tier table** [the differential contract — 12 rows, the `BadConst`↔`UndefinedValue`
vs `BadPredicate`↔UnknownPredicate rows split per `eval_const`], and watch items (a)–(d) as documented limits).
**ADR-0121 §2.1 "opaque bytes" STRUCK IN PLACE** (the plan is an IN-MEMORY dense-array object, not a byte-blob; the
serializable blueprint is the named-forward). Advisor pre-close review (mandated: fixed the "six→seven stages" miscount +
the split table row before the flip). **The tracker 11b row flipped ◧→✅**; band 11 stays ◧ (11c crd-perf regions ◀ NEXT;
11z the §153 hot-loop audit). A consolidated **"compiled-tier mirror scars"** memory landed (nested-cf child-pool
aliasing · feedback-edge latch · residue/defined-undefined erasure · three-store isolation).

## Proposed commit — CEIR-11b stage 5 corpus + the nested-cf fix (user commits; NO AI trailer; ADR-0123 lands with the close)

```
fix(ceir-11b): nested control-flow child indexing + the full-corpus differential (stage 5, part 1)

The CEIR-11b stage-5 corpus differential (the shared builder, run through BOTH engines) surfaced a
real stage-3a compiler bug, now fixed.

- The BUG: compile_seq captured a control-flow op's children_off = child_pool.size() BEFORE compiling
  its region children. A child containing NESTED control flow (a for-body with a match) pushes ITS
  children to child_pool first, so the parent's children_off aliased a nested op's child seqs -- Op::For
  ran the match's arm0 every iteration instead of the loop body. Fix: compile the children FIRST into a
  local buffer, THEN reserve a contiguous child_pool block and set children_off. Every simpler test
  missed it (no NESTED control flow); the 5z corpus program caught it.
- tests/ceir/corpus.hpp: the SHARED corpus builder (rich_graph.hpp precedent) -- 5 programs (5z, 6z,
  async, the six task ops, composing), each returning the module + state/dp op handles in compile order.
  build_5z is REUSED EXACTLY (test_band5_gate.cpp refactored to consume it -- the build-once extraction).
- test_plan.cpp +7 [ceir][plan]: the corpus differential (values + cells + map_outputs, per program) +
  the sec-121 plan-layer twin (a cooked->loaded module compiles to byte-identical results) + a minimal
  nested-cf regression (a for containing an if -> 22).

Gated: 436/436 on win-debug + win-asan + linux-gcc-debug + linux-gcc-asan + LLVM-20 tidy + GCC
-Werror=switch + opgen + crd-ceir-invariants. No recook, no fuzz, no version bump. ADR-0123 + the 11b
row flip land with the close (stage 5, part 2).
```

## Proposed commit — CEIR-11b close: ADR-0123 + the row flip (docs-only; user commits; NO AI trailer)

```
docs(ceir-11b): ADR-0123 (the compiled execution plan) + CLOSE the band-11 compiled tier

Stage 5 part 2 -- the ceremonial close of CEIR-11b (the compiled tier of sec-84 two-tier execution),
differential-verified against the sec-118 reference oracle. Docs only (the code + gate landed in part 1).

- docs/decisions/0123-compiled-execution-plan.md (ACCEPTED): the representation (Module = compile-time
  INPUT only; a dense Seq tree; frame windows; a run-global token store + per-phase isolation; dense
  cells + latch lists; map_outputs); the three killed sec-153 violations (per-eval attrs -> immediates;
  the state HashMap -> dense cells + latch lists; resolve_call -> compiled-fn index + arena frames);
  independence (thunks share the SPEC never code; the ONE shared analysis layer is check_parallel_region);
  the sec-4 error-tier table (the differential contract, 12 rows); watch items (a)-(d) as documented limits.
- ADR-0121 sec-2.1 "opaque bytes" STRUCK IN PLACE: the real 11b plan is an in-memory dense-array object
  (raw dispatch, non-serializable), NOT a byte-blob; the serializable blueprint is ADR-0123's named-forward.
- The D-007 tracker 11b row flipped to CLOSED; band 11 stays IN PROGRESS (11c crd-perf regions NEXT; 11z
  the sec-153 hot-loop audit). context.md advanced.
- A consolidated "compiled-tier mirror scars" memory (nested-cf child-pool aliasing; feedback-edge latch;
  residue / defined-undefined erasure; three-store isolation).

No code change (docs only) -- no build, no gate, no recook, no fuzz, no version bump.
```
