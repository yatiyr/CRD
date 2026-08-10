# CEIR-11b — Plan compiler + `CompiledExecutionPlan` + the differential harness — DESIGN NOTE

> Status: **DESIGN LOCKED (2026-08-10, advisor consult).** Substrate for **ADR-0123** (at 11b close; 0123 verified free
> — 0120/0121/0122 exist). CEIR band 11, slice 11b (§84 two-tier · §119 differential · §153 hot-path rules). The COMPILED
> tier — the fast path the 11a §118 reference oracle differential-compares against. crd-ceir core (new `plan.{hpp,cpp}`).

## 0. What the slice is

A **second, slot-based execution engine**: a **plan compiler** (`Module` → `CompiledExecutionPlan`) + a **plan executor**
+ a **differential harness** (reference vs compiled, byte-compare via `pin_values`, over the 5z/6z/7z corpus). The compiled
tier must be **§153-clean**: the hot loop has NO string lookup, NO per-op heap, NO dynamic map lookup, NO source parsing,
and ⛔ **NO authoring-graph traversal** (no `Operation*`/`Region*` pointer-chasing at runtime). It compiles the SEQUENTIAL
semantics (matching the oracle); provider composition + the §69 compile→plan interface stay CEIR-21/26 (⛔ NOT named here —
the 10b discipline).

## 1. The representation — STRUCTURED CONTROL FLOW COMPILED TO DENSE INSTRUCTION ARRAYS (locked)

⛔ **The fork the advisor corrected:** a "thunk tree that structurally walks the Module's Regions" STILL violates §153
("authoring graph traversal" — pointer-chasing the rich form is exactly what the rule bans). And a FLAT jump-stream (a)
is rejected — §153 demands DENSITY, not flatness; flattening buys nothing the 11z audit checks and makes regions / state /
async drastically harder. The answer is the middle: **the Module is compile-time INPUT ONLY; the plan owns a tree of dense
instruction sequences; the hot loop never touches an `Operation*`.** (Structured-over-dense — how real structured VMs work.)

- **`Instr`** (compact, arena-allocated at compile): a **thunk** (fn-ptr or index into a static thunk table) · an
  **operand-slot span** (into a dense u32 index pool) · a **result-slot span** · an **immediate payload** (the compiled
  attr — see §2) · **child-sequence indices** for region-bearing ops (a control-flow thunk recurses into child
  **instruction arrays** by index, NOT into Regions).
- **Slots:** every SSA `Value` in a function gets a dense **u32 slot** at compile time; the runtime value store is a flat
  `i64` array per frame (⛔ replaces the reference's `HashMap<const Value*, i64>` env — the first §153 map killed).
- **Dispatch:** each `Instr`'s thunk is pre-resolved at compile (⛔ replaces `m_sem.find(op.kind())` — the second map
  killed). Thunks are slot-aware: they read operands from `slots[instr.operand_slots[j]]`, write `slots[instr.result_slots[k]]`.
- **Yields:** a region's `core.yield` becomes **pre-baked slot copies** (the yield's source slots → the region op's result
  slots) — the reference's `out_yield` `Array` disappears.

## 2. The three HIDDEN §153 violations (the real 11z-audit risks — not the two maps)

1. **Per-eval attr reads (VERIFIED in exec.cpp):** `eval_const` reads `op.attr("value")` + `attr_value` EVERY eval;
   `eval_cmpi` reads the `"predicate"` **STRING** attr every eval; `eval_state` reads `"depth"`. ⇒ all become **compile-time
   immediates/enums** in the `Instr` payload (the const's i64, the predicate as a dense enum, the depth as a u32). No attr
   table touch in the loop.
2. **State cells:** the reference keys `m_cells` by `HashMap<Operation*, Cell>` — a map per state access. ⇒ **dense cell
   indices** assigned at compile; **plan-owned ring storage** pre-sized from the compile-time depths; ⛔ **pre-baked
   per-block latch lists** (the read-all-then-latch semantics preserved as a slot list, NOT a runtime trait scan).
3. **Call resolution:** `resolve_call` per call (a symbol-table lookup). ⇒ the callee resolved at compile to a
   **compiled-function index**; frames are **windows into a plan-owned slot stack** (arena — no per-call heap).

## 3. Independence — the compiled thunks are SEPARATE implementations (by necessity AND principle)

The slot thunks structurally CANNOT run on the reference EvalFns (those take `Operation*` + the `Value*` map). And ⛔ they
MUST NOT delegate to them: the differential's entire value is INDEPENDENCE — compiled thunks calling reference EvalFns
would make 11b's harness compare a thing to itself (the bit-exact-blind scar in its purest form; the conditional-pooling
memory [[feedback_optimizing_tier_never_rejects_what_reference_ran_conditional_pooling]] demands independent computation).
What is SHARED is the **spec** — the TOML-pinned semantics (wrapping i64, index-order fold, launch-runs-at-launch) — never
code. Divergence is what the differential exists to catch.

## 4. Compile-time rejection — a legitimate tier difference

A defect the reference fails at RUNTIME (a bogus `cmpi` predicate → `UnknownPredicate`; a `core.for` step ≤ 0 → `BadForStep`
only when reached) fails the COMPILER with a typed **`CompileError`**. ⛔ The differential contract covers **programs that
compile** — state this in the harness header. (Dynamic-count features — launch-in-a-loop tokens, call recursion — draw
from plan-owned ARENAS, amortized; 11z's CountingAllocator gate needs the precise "zero-alloc scope": straight-line
compute strictly zero, token/frame growth amortized-arena.)

## 5. Home + the 21/26 boundary

**crd-ceir core**, a new file pair `plan.{hpp,cpp}` (⛔ keep `exec.cpp` from bloating). The compiled tier compiles the
**SEQUENTIAL** semantics for parallel_for/map_reduce/async (matching the §118 oracle); provider composition + the §69
compile→plan interface stay **CEIR-21/26** — ⛔ **do not name that interface** (the 10b discipline, verbatim).

## 6. PlanCache — decide-and-document (NOT wedged into 10b)

The `CompiledExecutionPlan` is an **IN-MEMORY object** — raw thunk fn-ptrs are non-serializable, so the 10b byte-artifact
contract CANNOT hold it; ⛔ do NOT wedge it in. The **serializable blueprint** (the dense arrays with thunk-TABLE indices
instead of ptrs) is the named-forward artifact for the slice with a cross-session consumer. No open contract forces the
wiring now — 10z's hit-count assertions are already met. (This refines the 10b named-forward: a plan is not the opaque
byte-blob 10b's cache stores.)

## 7. The corpus inventory (quantifies the DoD)

⛔ **"the whole 5z/6z/7z corpus" — "7z" is a PRE-RENUMBER reference** (band 7's 7z → 10z at the 2026-08-09 re-baseline;
band 7 is paused at 7b). Concretely the differential corpus is:
- **5z:** the pinned band-5 gate program (`test_band5_gate.cpp` — loop + switch + calls + state accumulator), the richest
  single control-flow+state program.
- **6z:** the map-reduce gate (`test_host_provider.cpp` — a non-associative reduce; the compiled tier runs the SEQUENTIAL
  reference, byte-matched).
- the **exec / async / task** test programs (arith, core control flow, func calls, async launch/await, the six task ops).
- **band-7's real contribution (the "7z" intent):** the **§121 no-privileged-path property AT THE PLAN LAYER** — a
  cooked→loaded module compiles to byte-identical results vs its builder-form twin.
⭐ **The corpus mechanism is a SHARED BUILDER HEADER** (the `rich_graph.hpp` / 1f precedent — build each corpus program
ONCE, run it through both engines), NOT copy-pasted IR.

## 8. Staging (ONE tracker row — the 10a/11a pattern) + the gate

1. ✅ (this tick) **the durable design note.**
2. **Straight-line arith** — the compiler + executor for arith + the **slot allocator** + the **differential-harness
   skeleton** (one program, `pin_values` byte-compare — inherit the 11a agreement-test shape).
3. **Control flow + state** — if/for/while/switch compiled to child instruction arrays + the latch lists.
4. **Calls + async/task** — compiled-function indices + arena frames + the dense token store.
5. **The full-corpus differential** + `map_output` parity + the race pin (reuse 11a's — a decided race OR excluded-with-
   reason) + ADR-0123 + close.
Each stage **core-gated** (3 targets × 4 configs — all crd-ceir core now). ⛔ **NO recook / fuzz / binary version bump
expected** (the plan is a runtime object; nothing serializes). ADR-0123 at close.

**The §153 / 11z bar (design against it from LINE 1):** the compiled hot loop touches NO `Operation*`, NO attr table, NO
string, NO `HashMap`; values are dense-slot array reads; dispatch is a fn-ptr/index array; state is dense cell indices +
latch lists; calls are compiled-function indices + arena frames. CEIR-11z audits this (CountingAllocator + a
no-string-table-touch assert); designing against it now is why 11z is a gate, not a rewrite.

## 9. Named-forwards (explicit)

- **The serializable plan blueprint** (thunk-table indices, not ptrs) → the slice with a cross-session plan-cache consumer
  (refines the 10b named-forward — a plan is not 10b's opaque byte-artifact).
- **Provider composition + the §69 compile→plan interface** → CEIR-21/26 (not named here).
- **The §153 / alloc-free / string-free hot-loop AUDIT** → CEIR-11z (the band gate).
- **The three converging 10x forwards** (10a leaf-allow, 10a/10b compile-affected, 10b persistence) consume the compiled
  artifact — the compiled tier they waited for lands HERE (11b); the persisted/cross-session form is the blueprint forward.
