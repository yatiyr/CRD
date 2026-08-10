# ADR-0122 — Reference executor: the full host subset (task vocabulary · sequential parallel reference · step hooks · ON-POOL async)

**Status:** **ACCEPTED** (2026-08-10, under the standing autonomous loop grant [[project_ceir_autonomous_loop_grant]];
design + close advisor-reviewed — multiple consults across the slice) — the D-007 **CEIR band 11 (Reference executor +
compiled host plan)**, slice **CEIR-11a**. The CEIR-5z reference `Interpreter` is grown to the **full host subset** — the
§118 correctness oracle the CEIR-11b compiled tier differential-compares against.
**Phase:** D-007. Opens band 11 (§83/§84 two-tier execution). Law: §38 `ceir.task` · §84 (two-tier) · §112 (debugger
hook seam) · §118 (the correctness oracle). Inbound contract: the 6z routing note. Design: `docs/design/ceir-11a-reference-executor.md`.
**Tags:** `[ceir]` `[reference-executor]` `[task]` `[async-host]` `[oracle]`

---

## 1. Context

Band 11 provides two execution tiers (§84): a REFERENCE tier (the CEIR-5z interpreter, the §118 oracle — slow, correct,
maximal diagnostics) and a COMPILED tier (`CompiledExecutionPlan`, CEIR-11b). 11a completes the reference tier's coverage
of the full HOST subset. Three gaps closed, plus the inbound band-6 async work the 6z routing note deferred here:
- the **six §38 residual task ops** (spawn · continuation · main-thread · worker · fiber-wait · task-group) did not exist;
- `task.parallel_for` / `task.map_reduce` had NO host-independent reference (only the crd-ceir-host parallel provider);
- the **§112 debugger hook seam** was unbuilt;
- **jobs-backed launch/await ON-POOL** — today's async was sequential-reference only.

Value-model risk closed at design: the arith dialect is integer-only, so the wrapping-i64 Interpreter covers the whole
subset (no value-representation reshaping).

## 2. Decision — four gated stages (one tracker row)

### 2.1 The six §38 task ops (stage 2a) — vocabulary, not new runtime

**The layering invariant:** `ceir.async` states concurrency STRUCTURE (no execution commitment); `ceir.task` states
host/job PLACEMENT. In the reference tier spawn/main_thread/worker SHARE `eval_launch`, group SHARES `eval_scope`
(distinct vocabulary, shared EvalFn — the state/delay/history precedent), fiber_wait mirrors `eval_await`; only
`continuation` needs a new EvalFn (antecedent yields → body block-args → new token). Name-interned (TOML + opgen) → **no
format motion**. ⭐ The 6a `find_token_misuse` verifier covers the new TokenProducer/Consumer ops with **zero verifier
edits** — the open-world payoff, asserted.

### 2.2 The parallel sequential reference + §112 hooks (stage 2b)

- **The pre-flight moved to core** (stage 2b-ii-A): `exec::preflight_parallel` / `check_parallel_region` / the exported
  `region_state_free` — the 9d hoist-at-second-consumer, so the provider's parallel run and the reference's sequential
  run share ONE legality analysis (agree by construction).
- **The sequential reference** (stage 2b-ii-B): `eval_parallel_for_seq` / `eval_map_reduce_seq` run the map SEQUENTIALLY
  in index order (⛔ the 6z FOLD PATTERN — a fresh sub-interpreter, per-index `invoke_region` on the sub; NOT run_region
  in `in`'s frame, NOR invoke_region on the live `in`) → byte-identical to the provider's parallel run WITHOUT sharing
  execution. New core surface: an Interpreter-owned `map_output` store, `invoke()` captures `const Module*`, a `fuel()`
  accessor.
- **§112 step hooks** (stage 2b-i): pre/post `StepHook` fn-ptrs on the Interpreter, null-default (zero work when unset),
  post fires only on a successful dispatch, not proto-copied. The debugger SEAM only — no debugger/stepping UI.

### 2.3 Jobs-backed launch/await ON-POOL — CONDITIONAL POOLING (stage 3)

⛔ **The crux:** `async.launch` bodies run IN-FRAME in the reference — captures + state legally work (unlike
`task.parallel_for`) — so "require async bodies pure" would be a capability regression. Stage 3 must never reject what
stage 2 ran. So: a bridge-local **pool-eligibility classifier** (`region_state_free` + a no-outer-captures walk) routes
each launch to the **pool** (a heap-owned `PooledToken{own scratch, result, counter}`; the worker fills the result before
the counter decrements — the happens-before edge; heap-owned so the growable table never moves a JobDecl-captured entry)
or the **sequential in-frame fallback**. Byte-parity holds by CONSTRUCTION in both branches. Pooled handles live in a
DISJOINT space (`kPoolBase = 1<<40`) so a pooled token never truncates in the `u32(tok)` cast and aliases a sequential
handle; **all five consumers** (await/join/race/cancel/fiber_wait) route through one `resolve_yields`/`token_valid`.
spawn/worker → pooled; main_thread → `pin_thread=0`; group/scope → no override. `execute()` **drains** the pooled table at
exit (leak containment). A CUMULATIVE `pooled_count()` witness (asserted ==N pooled / ==0 fallback) — a never-pools impl
would pass every parity test otherwise.

## 3. Consequences

- The reference covers the full host subset: `test_task.cpp` (6 `[ceir][task]` — the six ops + the §112 hooks + ⭐ **the
  full-subset composing DoD gate: ONE reference program spanning arith + core(for/if/state) + func(call) + async(launch/
  await) + task(spawn/fiber_wait) in ONE interpreter session — exact result, exact §112 dispatch count (29), state-cell
  latching, and `find_token_misuse==None` across dialects**), `test_host_provider.cpp` (the sequential-vs-provider
  agreement + 6 ON-POOL tests), `test_async.cpp` (the sequential async), `test_exec.cpp` (the core subset).
- ⭐ The §118 oracle is READY for CEIR-11b: the sequential reference and the parallel provider agree byte-for-byte
  (proven on a non-associative map_reduce across {1..16}), and ON-POOL async byte-matches the sequential reference.
- **crd-ceir core stays jobs-free (I4):** every jobs-backed piece is in the crd-ceir-host bridge; the one core export
  (`region_state_free`) names no jobs type.
- **No binary/cook format motion, no fuzz, no recook** across all four stages — the new ops are name-interned (the CEIR-2
  doctrine held exactly as designed).

## 4. Named-forwards (explicit)

- **The COMPILED tier** (`CompiledExecutionPlan` — dense thunk arrays, slot-indexed values) → **CEIR-11b**. 11a is the
  REFERENCE tier only. The FULL differential-corpus harness is 11b; the alloc-free/string-free hot-loop audit is CEIR-11z.
- **The three converging 10x forwards** (10a leaf-allow, 10a/10b compile-affected, 10b persistence) consume the compiled
  tier at **CEIR-11b**, not 11a.
- **`continuation` static cross-op yield-count checking** → CEIR-3/4 (semantic verifiers; runtime `BadArity` today).
- **`!async<T>` typed async values** → the slice needing typed results (§37; NO-FOLLOW from 6a).
- **Real first-ready race + deadline/priority** → CEIR-24/29 (race is deterministic index-0 today; priority landed at 6c).

## 5. Alternatives rejected

- **Require async bodies pure (reject impure launch on the pool):** a contract change AND a capability regression — the
  provider already runs stateful/capturing launch bodies sequentially. → conditional pooling (§2.3).
- **A caller-provided map-output sink (the ParallelCtx pattern) for the sequential reference:** the sink exists only
  because the bridge can't add core members; the reference tier can. → an Interpreter-owned store (§2.2).
- **Sharing execution between the provider and the sequential reference** (delegate the parallel per-index run to the
  sequential path): the differential compare would compare a thing to itself (the bit-exact-blind scar). → share only the
  pre-flight ANALYSIS; compute results independently.
- **Partial override of the token consumers:** a pooled handle > u32-max would truncate + alias a sequential handle. →
  all five consumers through one `resolve_yields` (§2.3).
- **A live-table-size `pooled_count()`:** the drain clears the table at exit → reads 0 → a never-pools impl passes. →
  cumulative witness (§2.3).

## Gate

`test_task.cpp` (6 `[ceir][task]` — the six ops + the §112 hooks + the full-subset composing DoD gate) +
`test_host_provider.cpp` (the sequential-vs-provider agreement + 6 ON-POOL `[ceir][host]` tests) +
`test_reload_migration.cpp`/`test_exec.cpp`/`test_async.cpp` (the rest of the subset). **397/397 ctest** across
**win-debug + win-asan + linux-gcc-debug + linux-gcc-asan** (win-asan + linux-gcc-asan the threaded money configs — the
pooled workers + the drain ASan-clean) + LLVM-20 tidy + GCC `-Werror=switch` + opgen drift/validator +
`crd-ceir-invariants` (crd-ceir core jobs-free). Stage progression: 382 → 386 (2a) → 387 (2b-i) → 390 (2b-ii-B) → 396 (3)
→ 397 (4, the composing DoD gate). **No recook, no fuzz, no binary version bump.**
