# CEIR-11 — Reference executor + compiled host plan — session log

> The autonomous grind ([[project_ceir_autonomous_loop_grant]]) opened band 11 after BAND 10 closed. Design substrate:
> `docs/design/ceir-11a-reference-executor.md` (advisor consults on the design fork + the six-op signatures).
> Master-map: §38 `ceir.task` · §84 two-tier · §112 debugger hooks · §118 the correctness oracle.

## CEIR-11a — Reference executor (full host subset) + async-host + step hooks

Grow the CEIR-5z reference `Interpreter` to the full host subset (the §118 oracle for the 11b compiled tier). Staged
internally (ONE tracker row): (1) audit + design note · (2a) the six §38 ops · (2b) parallel_for/map_reduce sequential
reference + §112 hooks + stale fixes · (3) jobs-backed ON-POOL async · (4) DoD gate. Value-model risk closed at design
(arith is integer-only → the i64 Interpreter covers the subset; no reshaping fork).

### Stage 2a — the six §38 residual task ops — ✅ DONE + gated (2026-08-10)

**The signatures were an advisor-designed sub-fork** (§38 is a VOCABULARY list — spawn/continuation/main-thread/worker/
fiber-wait/task-group, "own host/job execution semantics" — not signatures). The layering invariant that resolved it:
**`ceir.async` states concurrency STRUCTURE** (no execution commitment — any provider must honor it); **`ceir.task`
states host/job PLACEMENT + execution semantics.** In the reference tier several task ops' semantics COINCIDE with async
ops' — which is not duplication but the state/delay/history + switch/match precedent (distinct vocabulary, SHARED
EvalFn); placement lives in the host tier.

⛔ **The blocking pre-TOML verify PASSED** (a host contract the bridge can't honor = the cook-only-gate scar): crd-jobs
offers `run(JobDecl)→Counter*` (spawn/worker), `wait(counter)` (fiber_wait), `JobDecl.pin_thread=0` + `pump_main_thread`
(main_thread), `run(span)+wait` (group), chained run-after-wait (continuation). Every lowering is honorable.

**The six ops** (TOML + opgen regen, drift-clean; `install_task_semantics`):

| op | shape | traits | reference EvalFn |
|---|---|---|---|
| `task.spawn` | `{body} -> %token` | TokenProducer | share `eval_launch` |
| `task.main_thread` | `{body} -> %token` | TokenProducer | share `eval_launch` (placement value-unobservable in the oracle) |
| `task.worker` | `{body} -> %token` | TokenProducer | share `eval_launch` |
| `task.continuation` | `(%tok) {body(%vals…)} -> %next` | Consumer+Producer | **NEW `eval_continuation`** |
| `task.fiber_wait` | `(%tok) -> (%vals…)` | Consumer | mirror `eval_await` (host wait yields the fiber) |
| `task.group` | `{body} -> (%results…)` | — (bounds, doesn't fork) | share `eval_scope` |

`eval_continuation` is the one new EvalFn: consume the antecedent token → bind ITS stored yields as the body's block-args
(arity-checked — the `invoke_region` contract) → run the body → store the body's yields as a NEW token. ⛔ The antecedent
values are read into the env BEFORE `run_region` (the body may `store_yields`, growing the store under the span).

⭐ **The open-world payoff, ASSERTED (not asserted-by-faith):** the 6a `find_token_misuse` verifier covers the new
TokenProducer/Consumer ops with **ZERO verifier edits** (task.spawn Unconsumed; fiber_wait double-consume →
MultiplyConsumed), and the generated smoke tests auto-grew to cover all six. The §32 audio-RT flip fires on a task op
(a task.spawn's Synchronization effect in an AudioRealTime region → `find_domain_violation`).

**Tests.** `test_task.cpp` (4 `[ceir][task]`): spawn→fiber_wait round-trips 42; a continuation chain threads the
antecedent's value (10 + 5 = 15); the zero-edit verifier coverage (Unconsumed + double-consume); the audio flip. + 3
auto-grown generated smoke cases.

**Gate.** crd-ceir-tests + host + cook **386/386 ctest** (382 + 4) on **win-debug + win-asan + linux-gcc-debug +
linux-gcc-asan** (a crd-ceir CORE change — the 3-target rebuild discipline returned after 4 bridge-only slices; win-asan
clean on the new EvalFns) + LLVM-20 tidy (`exec.{hpp,cpp}` + `test_task.cpp`, clean) + GCC `-Werror=switch` + opgen
drift/validator (drift-clean post-regen) + `crd-ceir-invariants` (U-§116 holds). ⛔ **NO binary/cook format motion, NO
fuzz, NO recook** — the six ops are name-interned (the CEIR-2 "an op is a TOML edit + regen" doctrine held exactly as
the design predicted).

**Next = stage 2b:** the sequential reference for `task.parallel_for` / `task.map_reduce` (the oracle needs them
host-independently — ⭐ move the parallel-purity pre-flight `check_parallel_region` from the bridge to crd-ceir core, the
9d hoist-at-second-consumer precedent, so oracle + provider agree by construction) + the §112 step hooks (pre/post
`void(*)(const Operation&, void*)` on the Interpreter, null-default, post fires only on successful dispatch, not
proto-copied) + the three stale-comment fixes (exec.hpp ×2, task.toml header ×1).

## Proposed commit — CEIR-11a stage 2a (user commits; NO AI trailer; NO ADR yet — mid-slice)

```
feat(ceir-11a): the six sec-38 residual task ops + their sequential reference semantics

Stage 2a of CEIR-11a (design note ceir-11a-reference-executor.md sec 7). Grows the sec-118 reference
oracle with sec 38's residual host-task vocabulary. A crd-ceir CORE change.

- The layering invariant (advisor): ceir.async states concurrency STRUCTURE (no execution commitment);
  ceir.task states host/job PLACEMENT. In the reference tier spawn/main_thread/worker share
  eval_launch and group shares eval_scope (distinct vocabulary, shared EvalFn -- the state/delay/history
  precedent); placement lives in the crd-ceir-host tier (stage 3).
- The six ops (TOML + opgen regen, drift-clean): task.spawn / main_thread / worker {body}->%token
  (TokenProducer); task.continuation (%tok){body(%vals)}->%next (Consumer+Producer, NEW
  eval_continuation: antecedent yields -> body block-args -> new token); task.fiber_wait (%tok)->(%vals)
  (mirror await, host wait yields the fiber); task.group {body}->(%results) (no token traits -- bounds,
  doesn't fork; confinement layered on 5b+6a). New install_task_semantics.
- Blocking pre-TOML verify PASSED: crd-jobs offers every lowering (run/wait/pin_thread=0/parallel_for) --
  no cook-only-gate.
- OPEN-WORLD payoff asserted: the 6a find_token_misuse verifier + the generated smoke cover all six with
  ZERO verifier edits; the sec-32 audio-RT flip fires on a task op.
- test_task.cpp (4 [ceir][task]) + 3 auto-grown generated smoke.

Gated: 386/386 on win-debug + win-asan + linux-gcc-debug + linux-gcc-asan + LLVM-20 tidy + GCC
-Werror=switch + opgen drift/validator (drift-clean) + crd-ceir-invariants (U-116 holds). No format
motion, no fuzz, no recook (name-interned -- the CEIR-2 doctrine). No ADR (yet -- lands at 11a close).
```

### Stage 2b-i — §112 step hooks + 3 stale-comment fixes — ✅ DONE + gated (2026-08-10)

**Split from stage 2b (advisor-sanctioned) by blast radius:** the §112 hooks + stale fixes are crd-ceir CORE-ONLY (no
bridge relink); 2b-ii's pre-flight move touches core AND relinks the bridge. Two scoped gates beat one sprawling one.

**What shipped** (`exec.{hpp,cpp}` + `test_task.cpp` + one TOML comment):
- **§112 step hooks — the debugger SEAM, hooks only** (no debugger / stepping UI): `using StepHook = void(*)(const
  Operation&, void*)`; `set_step_hooks(pre, post, user)` on the Interpreter. The `pre` hook fires BEFORE each op
  dispatches; the `post` hook fires ONLY after a SUCCESSFUL dispatch (never on an error). ⛔ Null by default → ZERO work
  when unset (a single null check in `eval_op`). ⛔ NOT copied by the prototype ctor (per-session, like set_user/cancel —
  the member default-initializers give the proto null hooks).
- **3 stale-comment fixes** (the 8z/9f discipline): `exec.hpp` "real per-width typing joins CEIR-6" → the arith dialect
  is integer-only through 11a (per-width/float is a future refinement when a float op is defined); `exec.hpp` "Jobs-backed
  parallel async is CEIR-6c" → CEIR-11a (routed at 6z); `task.ceirop.toml` header "route to CEIR-8 (compiled host plan)"
  → LANDED at CEIR-11a (pre-renumber).

**Tests.** `test_task.cpp` +1 `[ceir][task]`: a counting hook over `@main(){ const 42; return }` asserts EXACT pre=2 /
post=2 (the not-≥ discipline — every eval_op is enumerable) + a fresh interpreter with NO hooks runs identically
(null-default zero-cost).

**Gate.** crd-ceir-tests + host + cook **387/387 ctest** (386 + 1) on **win-debug + win-asan + linux-gcc-debug +
linux-gcc-asan** (a crd-ceir CORE change — all 3 targets rebuilt) + LLVM-20 tidy (`exec.{hpp,cpp}` + `test_task.cpp`,
clean) + GCC `-Werror=switch` + opgen drift/validator (a TOML header-comment change is drift-clean — comments aren't
generated) + `crd-ceir-invariants` (U-§116 holds). ⛔ **NO format motion, NO fuzz, NO recook.**

**Next = stage 2b-ii:** the sequential reference for `task.parallel_for` / `task.map_reduce` (the §118 oracle needs them
host-independently). ⭐ Move the parallel-purity pre-flight (`check_parallel_region`/`preflight_region`/`preflight`, today
in the bridge's anon namespace — pure IR analysis over StateEdge/Terminator traits + `func::resolve_call`, NO jobs types)
to crd-ceir core; the bridge's `preflight()` delegates → oracle + provider agree by construction (the 9d
hoist-at-second-consumer precedent). Relinks the bridge — gate accordingly.

## Proposed commit — CEIR-11a stage 2b-i (user commits; NO AI trailer; NO ADR yet — mid-slice)

```
feat(ceir-11a): sec-112 step hooks (the debugger seam) + 3 stale-comment fixes

Stage 2b-i of CEIR-11a (split from 2b by blast radius: core-only, no bridge relink). A crd-ceir CORE
change.

- sec-112 step hooks (the debugger SEAM, hooks only -- no debugger/stepping UI): StepHook =
  void(*)(const Operation&, void*); set_step_hooks(pre, post, user) on the Interpreter. pre fires before
  each op dispatches; post fires ONLY after a successful dispatch. Null-default (zero work when unset --
  one null check in eval_op); NOT proto-copied (per-session, like set_user/cancel).
- 3 stale-comment fixes: exec.hpp "per-width typing joins CEIR-6" -> integer-only-through-11a;
  exec.hpp "jobs-backed parallel async CEIR-6c" -> CEIR-11a; task.ceirop.toml header "route to CEIR-8"
  -> landed at CEIR-11a (pre-renumber).
- test_task.cpp +1 [ceir][task]: a counting hook asserts EXACT pre=2/post=2 (const+return; the not->=
  discipline) + null-default zero-cost.

Gated: 387/387 on win-debug + win-asan + linux-gcc-debug + linux-gcc-asan + LLVM-20 tidy + GCC
-Werror=switch + opgen drift/validator (drift-clean) + crd-ceir-invariants (U-116 holds). No format
motion, no fuzz, no recook. No ADR (yet -- lands at 11a close).
```

### Stage 2b-ii-A — the parallel-purity pre-flight MOVED to crd-ceir core — ✅ DONE + gated (2026-08-10)

**A design consult first** (the sequential reference is a genuine sub-fork). The advisor confirmed the design and caught
a landmine for 2b-ii-B (below); this stage is the shared-foundation move it blessed as independently gateable.

**What shipped** (`exec.{hpp,cpp}` + `host_provider.cpp` — a MOVE, zero behavior change): the parallel-purity pre-flight
(`PfResult`/`preflight_region`/`check_parallel_region`/`preflight` — the StateEdge-free + block-arity + terminator-yields-1
analysis, PURE IR over traits + `func::resolve_call`, ⛔ NO jobs types) lifted from the crd-ceir-host bridge's anon
namespace to public `exec::` surface: `exec::PreflightResult` · `exec::check_parallel_region` · `exec::preflight_parallel`.
The provider's `execute()` now DELEGATES to `exec::preflight_parallel`. ⭐ **The 9d hoist-at-second-consumer:** the
provider's PARALLEL submit-thread check and the (2b-ii-B) reference's SEQUENTIAL per-op check will share ONE legality
analysis — they agree by construction, not by duplication. Swapped the bridge-era `"func.call"` / `"task.parallel_for"` /
`"task.map_reduce"` STRING compares for interned OpIds (idempotent intern → the public functions take non-const `Context&`,
documented). The module-wide over-rejection (rejects an impure parallel op the entry never reaches) is documented in the
moved function's comment as a deliberate oracle/provider divergence, not a bug.

**Gate.** crd-ceir-tests + host + cook **387/387 ctest** on **win-debug + win-asan + linux-gcc-debug + linux-gcc-asan**
(core + bridge relink) + LLVM-20 tidy (`exec.{hpp,cpp}` + `host_provider.cpp`, clean) + GCC `-Werror=switch` +
`crd-ceir-invariants` (⛔ **crd-ceir STILL jobs-free — I4 holds:** the moved pre-flight names no jobs type). ⛔ **NO format
motion, NO new tests** — a behavior-preserving move; the existing host-provider parallel_for/map_reduce tests (incl. the
6z {1..16} bit-identity gate) exercise the moved pre-flight through the delegation.

**Next = stage 2b-ii-B — the sequential reference EvalFns** (the §118 oracle for parallel_for/map_reduce), advisor-designed:
- ⛔ **The landmine (the body-run shape):** NOT `run_region` in the current frame (outer captures wrongly RESOLVE — the
  TOML pins captures as UndefinedValue) NOR `invoke_region` on the live `in` (the 6z fold landmine). The correct shape is
  the **6z fold pattern:** one FRESH sub-interpreter from the prototype ctor (semantics copied, fresh env/cells/fuel), then
  per index `invoke_region(module, body, {iv})` on the SUB — captures are structurally UndefinedValue, matching the
  provider. `in.cancelled()` checked per index (eval_for granularity).
- **Interpreter-owned `map_output` store** (the m_yield_store/m_cells family — NOT a caller sink; the sink is the bridge's
  workaround for not being able to add core members), keyed by `const Operation*` with the `cell_value` builder-form-only
  caveat; accessor named `map_output(op)` verbatim (so 11b reads `in.map_output(op)` vs `provider.map_output(op)`
  symmetrically). map_reduce stores its map output too (provider parity).
- Two forced core additions: `invoke()` captures `const Module*` + a `module()` accessor; a `fuel()` accessor (the sub
  gets the parent's remaining budget).
- ⛔ **NO execution sharing** (share the pre-flight ANALYSIS, never the execution — else 11b compares a thing to itself,
  the bit-exact-blind scar). Two adjacent wins: the provider proto gains `install_task_semantics` (so a task.spawn through
  HostProvider isn't NoSemantics); mirror the provider's empty-range store exactly.
- Tests (the 2b-ii-B DoD): the first oracle-vs-provider agreement (the 6z gate program via core-sequential vs HostProvider,
  `pin_values`-byte-identical + `map_output` span equality); contract agreement (outer-capture → UndefinedValue; stateful →
  ParallelBodyStateful from the sequential EvalFn) — the 11b differential harness in miniature.

## Proposed commit — CEIR-11a stage 2b-ii-A (user commits; NO AI trailer; NO ADR yet — mid-slice)

```
refactor(ceir-11a): move the parallel-purity pre-flight from crd-ceir-host to crd-ceir core

Stage 2b-ii-A of CEIR-11a (design note sec 7; advisor-designed). A behavior-preserving MOVE that sets
up the sequential reference (2b-ii-B) to share ONE legality analysis with the provider (9d
hoist-at-second-consumer -> agree by construction).

- Lift PfResult / preflight_region / check_parallel_region / preflight (the StateEdge-free + block-arity
  + terminator analysis; pure IR, no jobs types) from the crd-ceir-host bridge anon namespace to public
  exec:: surface: exec::PreflightResult / exec::check_parallel_region / exec::preflight_parallel. The
  provider's execute() delegates.
- Swap the bridge-era "func.call" / "task.*" string compares for interned OpIds (idempotent intern ->
  the public functions take non-const Context&). The module-wide over-rejection is documented as a
  deliberate oracle/provider divergence.

Gated: 387/387 on win-debug + win-asan + linux-gcc-debug + linux-gcc-asan (core + bridge relink) +
LLVM-20 tidy + GCC -Werror=switch + crd-ceir-invariants (crd-ceir STILL jobs-free -- I4 holds). No
format motion, no new tests (behavior-preserving; the host-provider parallel tests exercise it via the
delegation). No ADR (yet -- lands at 11a close).
```

### Stage 2b-ii-B — the parallel_for/map_reduce SEQUENTIAL reference (the §118 oracle) — ✅ DONE + gated (2026-08-10)

**The "full host subset" reference is now COMPLETE.** New file-local `eval_parallel_for_seq` / `eval_map_reduce_seq`
(installed by `install_task_semantics`; the jobs-backed provider OVERRIDES via last-install-wins). The map runs
SEQUENTIALLY in index order → the SAME output as the provider's parallel run (index-order-disjoint slots), so the §118
differential compare is byte-identical WITHOUT sharing execution.

**The advisor landmine, avoided.** The body runs on ONE fresh SUB-interpreter cloned from `in` via the prototype ctor
(the 6z fold pattern), per-index `invoke_region(module, body, {iv})` on the SUB — ⛔ NOT `run_region` in `in`'s frame
(outer captures would wrongly RESOLVE — the TOML pins them UndefinedValue) NOR `invoke_region` on the live `in`. The sub
gets `in`'s remaining fuel; captures are structurally UndefinedValue, matching the provider. `in.cancelled()` checked
per index.

**New core surface** (all in the m_yield_store/m_cells family — Interpreter-owned, not a caller sink):
- **`map_output` store** — `store_map_output(op, out)` / `map_output(op)`, keyed by `const Operation*` (the `cell_value`
  builder-form caveat); named `map_output` VERBATIM to mirror `HostProvider::map_output` (so 11b reads
  `in.map_output(op)` vs `provider.map_output(op)` symmetrically). map_reduce stores its intermediate map too (provider
  parity). NOT proto-copied (fresh per session).
- `invoke()` + `invoke_region()` capture **`const Module*`** + a `module()` accessor (the sub invoke_regions on it); a
  `fuel()` accessor (the sub gets the parent's remaining budget — fuel non-refund mirrors the provider's sub_fuel model).

**Adjacent win:** the provider proto gains `install_task_semantics` before overriding pf/mr with its parallel versions —
so a `task.spawn` through HostProvider now runs (sequential), no longer NoSemantics (last-install-wins, the seam).

⛔ **NO execution sharing** (Q2): the provider + reference share only the pre-flight ANALYSIS (2b-ii-A), computing results
INDEPENDENTLY — else the 11b differential compare is a thing compared to itself (the bit-exact-blind scar; 6z's precedent).

**Tests — the first oracle-vs-provider AGREEMENT (the 11b differential harness in miniature).** `test_host_provider.cpp`
+3 `[ceir][host]`: (1) a NON-associative map_reduce (map `iv*iv`, combine `acc*31+elem` — bit-identity alone can't pass a
wrong) run core-sequential vs HostProvider across **{1..16}** → `pin_values` byte-identical reduced value + `map_output`
span-equal (13 elements); (2) a stateful parallel body → `ParallelBodyStateful` from the sequential reference (the shared
pre-flight — agree by construction); (3) an outer-capture body → `UndefinedValue` in BOTH the sequential reference and the
provider (the 6z fold pattern's self-contained sub — the TOML-pinned capture contract).

**Gate.** crd-ceir-tests + host + cook **390/390 ctest** (387 + 3) on **win-debug + win-asan + linux-gcc-debug +
linux-gcc-asan** (⭐ **linux-gcc-asan the money config — the {1..16} threaded jobs run ASan-clean against the sequential
reference**) + LLVM-20 tidy (`exec.{hpp,cpp}` + `host_provider.cpp` + `test_host_provider.cpp`, clean after a decl-split +
a const-correctness fix on `check_parallel_region`'s owner param — removing two const_casts) + GCC `-Werror=switch` +
`crd-ceir-invariants` (⛔ crd-ceir STILL jobs-free — the sequential reference names no jobs type). ⛔ **NO format motion,
NO fuzz, NO recook.**

**Next = stage 3** (jobs-backed launch/await ON-POOL — the crd-ceir-host provider extends HostProvider; today's async is
sequential-reference) + stage 4 (the 11a DoD gate). Then ADR-0122 + 11a close.

## Proposed commit — CEIR-11a stage 2b-ii-B (user commits; NO AI trailer; NO ADR yet — mid-slice)

```
feat(ceir-11a): the parallel_for/map_reduce SEQUENTIAL reference (the sec-118 oracle)

Stage 2b-ii-B of CEIR-11a (design note sec 7; advisor-designed). Completes the "full host subset"
reference. A crd-ceir CORE change + a bridge relink.

- New eval_parallel_for_seq / eval_map_reduce_seq (installed by install_task_semantics; the jobs-backed
  provider overrides via last-install-wins). The map runs SEQUENTIALLY in index order -> the SAME output
  as the provider's parallel run, so the sec-118 differential compare is byte-identical without sharing
  execution.
- The 6z fold pattern (the advisor landmine): the body runs on ONE fresh SUB-interpreter cloned from
  `in` (per-index invoke_region on the sub) -- NOT run_region in `in`'s frame (captures wrongly resolve)
  nor invoke_region on the live `in`. Captures are structurally UndefinedValue, matching the provider.
- New core surface: an Interpreter-owned map_output store (store_map_output / map_output(op), named to
  mirror HostProvider::map_output); invoke()/invoke_region() capture const Module* + a module()
  accessor; a fuel() accessor. The provider proto gains install_task_semantics (a task.spawn through
  HostProvider is no longer NoSemantics).
- NO execution sharing: the provider + reference share only the pre-flight analysis (2b-ii-A), computing
  results independently (the bit-exact-blind scar).
- test_host_provider.cpp +3 [ceir][host]: the first oracle-vs-provider agreement (a non-associative
  map_reduce core-sequential vs HostProvider across {1..16} -- byte-identical + map_output span-equal);
  a stateful body -> ParallelBodyStateful (shared pre-flight); an outer-capture -> UndefinedValue in both.

Gated: 390/390 on win-debug + win-asan + linux-gcc-debug + linux-gcc-asan (core + bridge relink;
linux-gcc-asan is the {1..16} threaded money config) + LLVM-20 tidy + GCC -Werror=switch +
crd-ceir-invariants (crd-ceir STILL jobs-free). No format motion, no fuzz, no recook. No ADR (yet).
```

### Stage 3 — jobs-backed launch/await ON-POOL (conditional pooling) — ✅ DONE + gated (2026-08-10)

**The crux the advisor caught** (the design fork): `async.launch` bodies run IN-FRAME in the sequential reference —
captures resolve, stateful bodies work — UNLIKE `task.parallel_for` (whose TOML pins captures as UndefinedValue). So
"require async bodies pure" would be BOTH a contract change AND a capability regression: the provider TODAY runs
stateful/capturing launch bodies via its sequential installer. ⛔ **Stage 3 must never reject what stage 2 ran.**

**The design — CONDITIONAL POOLING** (`host_provider.{hpp,cpp}` + one core export):
- A bridge-local **pool-eligibility classifier**: `exec::region_state_free` (the StateEdge-free + calls-resolved walk,
  EXPORTED from core so it isn't duplicated — a variadic-body consumer that can't use `check_parallel_region` verbatim)
  + a NEW **no-outer-captures** walk (every operand region-local or a block-arg; bridge-local, the genuinely new part).
- Eligible → **pool**: a heap-owned `PooledToken{own MallocAllocator, result Array, counter, job ctx}` (⛔ heap so the
  growable table never moves an entry the JobDecl captured by pointer — the push-back-UAF scar). The worker runs the body
  on a fresh sub over the token's own scratch and fills `result` BEFORE returning — ⛔ **the counter is the happens-before
  edge**; the worker never touches `in`'s (non-thread-safe) yield-store.
- Ineligible → **sequential in-frame fallback** (`launch_seq_inframe` — run_region in `in`'s frame + store_yields).
- ⭐ **Byte-parity holds by CONSTRUCTION** in both branches (pooled = pure + self-contained ⇒ schedule-independent, the
  6b theorem; fallback = literally the reference semantics).

**The token model — a disjoint handle space, all five consumers unified.** Sequential handles are u32 store indices; a
pooled handle would TRUNCATE in the `u32(tok)` cast and alias one → a silent wrong read. So pooled handles live at
`kPoolBase = 1<<40` (all > u32-max), and ALL FIVE token consumers (await/join/race/cancel/fiber_wait) route through one
`resolve_yields` (pooled: wait the counter once + read the result; else the sequential store) / `token_valid` (no-wait
validity for race/cancel). **spawn/worker** → the pooled path (any thread); **main_thread** → `pin_thread=0` (awaiting
from the main thread works because `wait()` pumps thread 0 — VERIFY 2); **group/scope** → no override (sequential boundary
+ the drain). ⛔ `execute()` **drains** the pooled table at exit (leak containment — the provider runs no verifier, so an
unverified program can leak a pooled token → a leaked Counter/worker).

**The witness (advisor).** A never-pools impl passes every parity test (the perf-flag-measures-empty-frame scar). So
`HostProvider::pooled_count()` is a CUMULATIVE counter (⛔ NOT the live table size — `drain_pooled` clears the table at
exit; a live count would read 0 afterward). Asserted `==N` in the pooled tests, `==0` in the fallback tests.

**The two pre-write verifies** (design note §8): intern_op is READ-ONLY on a hit (no race with worker Context reads —
every op kind is pre-interned before the pool runs); `wait()` pumps thread 0 (main_thread awaitable from main; execute()
must run on the main thread — T4 asserts it).

**Tests.** `test_host_provider.cpp` +6 `[ceir][host]`: **T1** N pure launches + await → byte-identical to the core
sequential reference, `pooled_count()==2`; **T2** a capturing body → correct via fallback, `pooled_count()==0` (the test
that would have caught a reject-design); **T3** a stateful body → same; **T4** a main_thread-pinned pooled task awaited
from main → completes (no deadlock — the pump path); **T5** join/race/cancel resolve pooled tokens (join concatenates,
race→0, cancel consumes); **T6** a leaked pooled token drained at exit (ASan-clean).

**Gate.** crd-ceir-tests + host + cook **396/396 ctest** (390 + 6) on **win-debug + win-asan + linux-gcc-debug +
linux-gcc-asan** (⭐ win-asan + linux-gcc-asan the money configs — the pooled workers under real threads + the drain,
ASan-clean) + LLVM-20 tidy (5 files) + GCC `-Werror=switch` + `crd-ceir-invariants` (⛔ **crd-ceir core STILL jobs-free** —
`region_state_free` names no jobs type; the entire pool machinery is bridge-side). ⛔ **NO format motion, NO fuzz, NO
recook.** (Compile fixes en route: const-correctness on the capture walks + `Region*` on `region_state_free`; the witness
made cumulative after T1 caught the drain clearing the live count.)

**Next = stage 4 (the 11a DoD gate)** — the reference covers the full host subset + hooks observed + ON-POOL async
byte-matches the sequential reference (all met across the stages); ADR-0122 (verify the number); then 11a close.

## Proposed commit — CEIR-11a stage 3 (user commits; NO AI trailer; NO ADR yet — stage 4 + ADR-0122 close 11a)

```
feat(ceir-11a): jobs-backed launch/await ON-POOL via conditional pooling

Stage 3 of CEIR-11a (design note sec 8; advisor-designed). Bridge (crd-ceir-host) + one core export.

- The crux (advisor): async.launch bodies run IN-FRAME in the reference (captures + state legally
  work, unlike parallel_for), so stage 3 must never reject what stage 2 ran. CONDITIONAL POOLING: a
  bridge-local classifier (exec::region_state_free [exported] + a new no-outer-captures walk) -> pool
  (heap-owned PooledToken{scratch,result,counter}; the worker fills result before the counter
  decrements = the happens-before edge; heap so the table never moves a JobDecl-captured entry) or a
  sequential in-frame fallback. Byte-parity by construction in both branches.
- Disjoint handle space (kPoolBase=1<<40) so a pooled token never truncates in the u32(tok) cast and
  aliases a sequential one; ALL FIVE consumers (await/join/race/cancel/fiber_wait) route through one
  resolve_yields / token_valid. spawn/worker -> pooled; main_thread -> pin_thread=0 (wait pumps
  thread 0); group/scope -> no override. execute() DRAINS the pooled table at exit (leak containment).
- pooled_count() CUMULATIVE witness (survives the drain): a never-pools impl would pass every parity
  test otherwise. Verifies: intern_op read-only on hit; wait pumps thread 0.
- test_host_provider.cpp +6 [ceir][host]: T1 parity+witness / T2 capture-fallback / T3 stateful-fallback
  / T4 main_thread pump / T5 join.race.cancel / T6 leak-drain.

Gated: 396/396 on win-debug + win-asan + linux-gcc-debug + linux-gcc-asan (win-asan + linux-gcc-asan the
threaded money configs) + LLVM-20 tidy (5) + GCC -Werror=switch + crd-ceir-invariants (crd-ceir core
STILL jobs-free -- the pool is bridge-side). No format motion, no fuzz, no recook. No ADR (yet).
```

### Stage 4 — the 11a DoD gate (the full-subset composing program) + CEIR-11a CLOSE — ✅ (2026-08-10, ADR-0122)

**The advisor blocked a close-only stage 4:** the 11a DoD headline ("the reference covers the full host subset") was an
INFERENCE from parts — no single program had ever run arith + core-control + state + func + async + task through ONE
reference interpreter session. The band's own precedent (8z's composing gate, 10z's composing walk) + the 9z distinction
says a composing test is owed exactly when the guarantees live in separate tests that never met. They never met here, and
the cross-dialect interactions (a state cell latching across a block; a task token beside an async token; hooks counting
across nested-region dispatch) are where an interpreter breaks.

**The composing DoD gate** (`test_task.cpp` +1 `[ceir][task]`, "the full host subset composes in one reference program"):
ONE program — `@sq` (func+arith) + `@main` with a `core.for` loop accumulating into a `core.state` cell (control+state,
the piece most absent from every prior 11a composition), an `async.launch` whose body CALLS `@sq` (async+func, in-frame),
a `task.spawn`/`task.fiber_wait` pair (task), a `core.if` on a `cmpi` (control) — run through the CORE reference (builtin
+ async + task; the §118 oracle). Exact assertions: **result == 21**; the state cell **latched across 3 iterations**
(`cell_value == 0+1+2`); **`find_token_misuse == None`** across dialects (async + task producers/consumers in one module);
⛔ **exact §112 dispatch count == 29** (the not-≥ discipline — I ran it once to observe the count over the nested regions +
the loop + the sq call + the launch/spawn bodies, then pinned it exactly).

**Gate.** crd-ceir-tests + host + cook **397/397 ctest** (396 + 1) on **win-debug + win-asan + linux-gcc-debug +
linux-gcc-asan** + LLVM-20 tidy (test_task.cpp) + GCC `-Werror=switch` + opgen drift/validator + `crd-ceir-invariants`
(crd-ceir core jobs-free). NO recook/fuzz/version-bump.

**ADR-0122** (advisor-reviewed sound pre-close; counts corrected to 6 `[ceir][task]` / 397) records the settled 4-stage
architecture, the layering invariant (async=STRUCTURE / task=PLACEMENT), the fold-pattern landmine, the conditional-pooling
crux, and the five rejected alternatives.

⭐ **CEIR-11a CLOSED — BAND 11 OPENS.** The §118 reference oracle covers the full host subset and is READY for the CEIR-11b
compiled tier (the sequential reference and the parallel provider agree byte-for-byte; ON-POOL async byte-matches the
sequential reference). Memory: the **conditional-pooling principle** ("an optimizing tier never rejects what the reference
ran — classify, optimize the provable, fall back; cumulative witness; no execution sharing") — the reusable lesson the
11b compiled tier will re-face (compile the compilable, interpret the rest).

**Next = CEIR-11b** (plan compiler + `CompiledExecutionPlan` + the differential harness over the 5z/6z/7z corpus) — it
inherits the 11a oracle + the T1–T6 / agreement patterns; the 10b PlanCache gets its first real artifact; ⛔ the plan
representation must be designed against §153 (zero strings / zero per-op heap / zero map lookups) from line 1, or CEIR-11z
becomes a rewrite. Advisor on the design fork first.

## Proposed BATCH commit sequence — CEIR-11a (user commits; NO AI trailer; ordered)

The five stage commits are drafted in their sections above; apply them in order, then the close:

1. `feat(ceir-11a): the six sec-38 residual task ops + their sequential reference semantics` (2a)
2. `feat(ceir-11a): sec-112 step hooks (the debugger seam) + 3 stale-comment fixes` (2b-i)
3. `refactor(ceir-11a): move the parallel-purity pre-flight from crd-ceir-host to crd-ceir core` (2b-ii-A)
4. `feat(ceir-11a): the parallel_for/map_reduce SEQUENTIAL reference (the sec-118 oracle)` (2b-ii-B)
5. `feat(ceir-11a): jobs-backed launch/await ON-POOL via conditional pooling` (3)
6. the close:

```
docs(ceir-11a): CLOSE -- ADR-0122, the reference executor's full host subset

- ADR-0122 (reference executor: full host subset): the settled 4-stage architecture -- the six sec-38
  task ops (async=STRUCTURE/task=PLACEMENT, shared EvalFns), the parallel sequential reference (the 6z
  fold pattern), sec-112 hooks, and jobs-backed launch/await ON-POOL via conditional pooling (never
  reject what the reference ran). 5 rejected alternatives; the named-forwards to 11b/11z/3-4/24-29.
  Accepted under the standing autonomous loop grant; design + close advisor-reviewed.
- Stage 4 / the DoD gate (advisor-mandated -- the guarantees never met in one program): a full-subset
  COMPOSING test -- arith + core(for/if/state) + func(call) + async(launch/await) + task(spawn/
  fiber_wait) in ONE reference session; exact result (21), exact sec-112 dispatch count (29), state-cell
  latching, cross-dialect find_token_misuse==None.
- D-007 tracker: CEIR-11a row -> CLOSED (ADR-0122 pointer, the 4-stage narrative, the named-forwards);
  band header 11a done / 11b NEXT. context.md + this session log close section.

CEIR-11a CLOSED -- the sec-118 reference oracle covers the full host subset, ready for the 11b compiled
tier. 397/397 x 4 configs. crd-ceir core jobs-free. No recook, no fuzz, no version bump.
```
