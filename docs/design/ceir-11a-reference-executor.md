# CEIR-11a — Reference executor (full host subset) + async-host + step hooks — DESIGN NOTE

> Status: **DESIGN LOCKED (2026-08-10, advisor consult).** Substrate for **ADR-0122** (at 11a close). Opens **CEIR-11**
> (Reference executor + compiled host plan, §83/§84 two-tier). Law: §38 `ceir.task` (the six residual ops) · §84
> (two-tier contract) · §112 (debugger hook seam) · §118 (the correctness oracle). The 6z routing note is the inbound
> contract (preserved verbatim below).

## 0. What the slice is

Grow the CEIR-5z reference `Interpreter` (`exec.{hpp,cpp}`) to the **full host subset** — the §118 correctness oracle
for the compiled tier (11b). Three additions: (1) the **six §38 residual task ops** (spawn · continuation · main-thread ·
worker · fiber-wait · task-group), which do not exist yet; (2) **jobs-backed launch/await ON-POOL** (today's async is
sequential-reference); (3) **§112 step hooks** (the debugger seam — hooks only, no debugger). Plus the executor growth
to cover the full subset with maximal diagnostics.

⛔ **The inbound contract (6z routing note, verbatim):** *"jobs-backed launch/await-ON-POOL (today's async is
sequential-reference) + §38's residual six task ops — spawn/continuation/main-thread/worker/fiber-wait/task-group — land
in this async-host subset."* (The scope×provider seam was PAID at 6z; deadline/priority routed away — priority landed in
the bridge at 6c, deadline → CEIR-24/29.)

## 1. The audit — coverage matrix (the "full host subset" evidence; stage-2 work-list)

Every op in the arith/core/func/async/task dialects × has a reference `EvalFn` in `exec.cpp` today:

| dialect | ops | reference EvalFn? |
|---|---|---|
| arith | const · addi · muli · cmpi | ✅ all (`install_arith_semantics`) |
| core | scope · if · for · while · switch · match · yield · state · delay · history | ✅ all (`install_core_semantics`) |
| func | call · return · func | ✅ all (`install_func_semantics`) |
| async | scope · launch · await · join · race · cancel | ✅ all SEQUENTIAL (`install_async_semantics`) — jobs-backed ON-POOL is stage 3 |
| task | parallel_for · map_reduce | ⚠ **NO core EvalFn** — host-only (HostProvider lowers to `jobs::parallel_for`); a **sequential reference** for the oracle is stage 2 |
| task | spawn · continuation · main-thread · worker · fiber-wait · task-group | ❌ **do not exist** — stage 2 authors TOML + opgen + sequential reference |

⭐ **The value model is i64-and-fine (risk closed):** the arith dialect is INTEGER-ONLY (`const`/`addi`/`muli`/`cmpi`;
the TOML summary says "Integer/float" aspirationally but defines no float op), so the Interpreter's wrapping-i64 scalar
covers the whole subset — **no value-representation reshaping fork**. Two stale forwards in `exec.hpp` to fix in place
during the core pass: *"real per-width typing joins CEIR-6"* (never landed — arith stayed integer) and *"Jobs-backed
parallel async is CEIR-6c"* (the 6z routing moved it here).

## 2. The four forks (advisor-resolved)

- **Staging (one row, one DoD — do NOT split; the note stages it):** four internal stages, 10a-style.
- **Ops-first is MECHANICAL, not a fork** (the CEIR-2 doctrine: "an op is a TOML edit + regen"). The regen touches
  committed generated files (drift gate clean after), the generated smoke tests auto-grow, agent-discovery schemas
  update free. New ops are **name-interned → NO binary/cook format motion, NO fuzz, NO recook** anywhere in 11a. ⛔ If any
  stage finds otherwise, STOP and reassess.
- **Core-vs-host is already decided by existing seams:** the SEQUENTIAL reference semantics for EVERYTHING (incl. the six
  ops + a sequential parallel_for/map_reduce reference) live in **crd-ceir core** (the `install_async_semantics`
  precedent; the reference tier IS the §118 oracle and must cover the full subset host-independently). JOBS-BACKED
  versions live in **crd-ceir-host** and **rebind via the existing "last install wins" `Interpreter::install()`** — no
  new mechanism; the seam was built for this. ⛔ crd-ceir names no jobs type (I4 — the invariants gate).
- **The six ops' SEMANTICS is a genuine design sub-fork** (stage 2): §38 is a VOCABULARY list ("spawn · continuation ·
  main-thread · worker · fiber-wait · task-group — own host/job execution semantics"), NOT signatures. Each op's
  operands/results/regions/effects/determinism must be DESIGNED from that vocabulary + the token/structured-concurrency
  substrate (§30 structured concurrency, the 6a TokenProducer/TokenConsumer traits, the Synchronization effect, the §32
  audio-RT flip). ⛔ **Advisor consult at the stage-2 design point BEFORE authoring the TOML** (the shapes are not
  mechanical even though the plumbing is).

## 3. Internal staging (ONE tracker row; the note stages it)

1. ✅ (this tick) **Audit + design note.**
2. **The core pass** (crd-ceir core — the 3-target rebuild + full 4-config discipline RETURNS after 4 bridge-only slices;
   possibly split 2a/2b if large, each independently gateable):
   - the six §38 ops as **TOML + opgen regen** + their SEQUENTIAL reference `EvalFn`s (semantics advisor-designed first);
   - a sequential reference for `task.parallel_for` / `task.map_reduce` (the oracle needs them host-independently);
   - **§112 step hooks:** a pre-op + post-op `void(*)(const Operation&, void*)` fn-ptr + `void* user` on the Interpreter,
     null by default, **zero work when null** (checked in `eval_op`); no debugger, no stepping UI — hooks only;
   - the two stale-comment fixes.
3. **Jobs-backed ON-POOL async** (crd-ceir-host, extending `HostProvider`): `launch`/`await` run the body on the
   crd-jobs pool (vs today's run-at-launch sequential), rebinding the async EvalFns via `install()`; the jobs-backed
   variants of the six ops that have a pool semantics (spawn/worker/fiber-wait/task-group; main-thread/continuation are
   scheduling-shape ops — designed at stage 2). Reuses the 6b prototype-sub-interpreter + cancel-flag + purity-pre-flight
   machinery (⛔ NOT a second pattern — the third-graph shape).
4. **The 11a DoD gate.**

## 4. Pins (advisor)

- **Race determinism** (the differential-vs-compiled and sequential-vs-pool comparisons): the sequential reference picks
  index 0 deterministically; a pool-backed race is honestly NONDETERMINISTIC. A comparison must FORCE a decided race
  (single live token) or EXCLUDE race's winner with the reason documented — ⛔ never fake determinism (the 6a finding is
  the precedent).
- **Jobs scars (stages 3/4):** tests own the crd-jobs pool (a Catch listener init/shutdown — the provider never does);
  jobs shutdown must reset num_workers; timeout≠hang; perf-jobs ASan flake=race → **win-asan + linux-gcc-asan are the
  money configs** for the pooled semantics.
- **§112 is hooks ONLY** — the fn-ptr seam, null-default, zero-cost-when-null. No debugger.
- **The 11a DoD boundary** (do NOT let stage 3/4 creep): 11a closes on **reference-covers-the-subset + hooks observed +
  ON-POOL async byte-matching the sequential reference on TARGETED tests** (the 6b {1..16} bit-identity precedent). The
  FULL differential-corpus harness is **CEIR-11b**; the allocation-free / string-free hot-loop audit is **CEIR-11z**.

## 5. Named-forwards (explicit)

- **The compiled tier** (`CompiledExecutionPlan` — dense thunk arrays, slot-indexed values) → **CEIR-11b**. 11a is the
  REFERENCE tier only (the oracle 11b differential-compares against).
- **deadline/priority** → priority landed at 6c (`priority_for`, bridge); deadline → CEIR-24/29 (no crd-jobs primitive).
- **`!async<T>` typed async values** → the slice needing typed results (§37; NO-FOLLOW from 6a).
- **The three converging 10x forwards resolve in this band (11):** 10a leaf-allow ("when the executable form can
  recompile/validate callers"), 10a/10b compile-affected ("nothing to compile until 11"), 10b persistence ("waits for
  the real plan payload") — all consume the compiled tier at **11b**, not 11a.

## 6. Gate plan

Per stage: the CHANGED module(s) across **win-debug + win-asan + linux-gcc-debug + linux-gcc-asan** + LLVM-20 tidy (per
touched file) + GCC `-Werror=switch` (new `ExecError`/task enums) + opgen drift/validator (regen must be drift-clean) +
`crd-ceir-invariants` (U-§116 holds; crd-ceir stays jobs-free). ⛔ Stage 2 is a crd-ceir CORE change → all 3 targets +
full 4-config. **NO recook, NO fuzz, NO binary version bump expected** (new ops are name-interned — §2). The 11a DoD gate
(stage 4): the reference covers the full subset, step hooks fire, ON-POOL async byte-matches the sequential reference on
the targeted tests. ADR-0122 at close (verify the number then).

## 7. The six §38 ops — SIGNATURES LOCKED (2026-08-10, advisor consult) + stage split

**Blocking pre-TOML verify PASSED — crd-jobs offers every lowering** (so no cook-only-gate; the TOML `docs` promise
honorable host contracts): `run(JobDecl)→Counter*` (spawn/worker) · `wait(counter,target)` (fiber_wait) ·
`JobDecl.pin_thread=0` + `pump_main_thread` (main_thread) · `run(span)+wait` (group) · chained run-after-wait
(continuation) · `parallel_for` (already, parallel_for/map_reduce).

**The layering invariant (Q1):** `ceir.async` states concurrency STRUCTURE with no execution commitment (any provider —
future GPU/distributed — must honor it); `ceir.task` states HOST/JOB PLACEMENT + execution semantics (§38's own words).
In the REFERENCE tier spawn/group's semantics COINCIDE with launch/scope — that is NOT duplication but the
state/delay/history + switch/match precedent (distinct vocabulary, SHARED EvalFn); the placement distinction lives in the
host tier, honestly documented (like launch's sequentialization). **Six distinct ops (Q2), not attributes:** the row's
letter is user vocabulary; ops are cheap (TOML+regen, open-world, no central enum); per-kind `install()`/`advertises`
dispatch means placement-as-kind needs zero attribute-branching in the provider.

| op | shape | traits | effects | reference EvalFn |
|---|---|---|---|---|
| `task.spawn` | `{ body } -> %token` | TokenProducer | Synchronization | share `eval_launch` (fork; run body at spawn) |
| `task.continuation` | `(%token) { body(%vals…) } -> %token2` | TokenConsumer, TokenProducer | Synchronization | `stored_yields`→bind block-args via `invoke_region`→`store_yields` |
| `task.main_thread` | `{ body } -> %token` | TokenProducer | Synchronization | share `eval_launch` (placement value-unobservable in the oracle) |
| `task.worker` | `{ body } -> %token` | TokenProducer | Synchronization | share `eval_launch` (calling-thread run is correct for the oracle) |
| `task.fiber_wait` | `(%token) -> (%vals…)` | TokenConsumer | Synchronization | mirror `eval_await` (host wait YIELDS the fiber, never blocks the thread) |
| `task.group` | `{ body } -> (%results…)` | — (bounds, doesn't fork) | Synchronization | share `eval_scope` (confinement = layered 5b+6a, like async.scope) |

⛔ **Determinism: none is Nondeterministic** (only race's VALUE is scheduling-dependent) — no `determinism` field, default
Unspecified (matches launch). **continuation arity** mismatch = runtime `BadArity` (the `invoke_region` contract);
static cross-op yield-count is a semantic-verifier question → named-forward CEIR-3/4. If the generator can't express
variadic region block-args, declared args become advisory+docs (let regen decide mechanically). **group carries NO token
traits** — it bounds, it doesn't fork.

**Stage split (advisor):**
- **2a** = the six ops: TOML + opgen regen (drift-clean) + the sequential reference EvalFns (mostly shared, per the table)
  + `test_task.cpp` (⛔ explicit tests/ceir list). ⭐ **The open-world payoff, ASSERTED:** the 6a token verifier covers
  every new producer/consumer op with ZERO verifier edits (task.spawn Unconsumed; fiber_wait double-consume); the §32
  audio-RT flip fires on ≥1 task op; sequential exec of a continuation chain; ASCII names.
- **2b** = the sequential reference for `parallel_for`/`map_reduce` + §112 hooks + the stale-comment fixes. ⭐ **Decide+
  document:** the parallel-purity pre-flight (`check_parallel_region`, today bridge-only IR analysis) gets a SECOND
  consumer (the sequential reference) → the 9d hoist-at-second-consumer precedent: MOVE it to crd-ceir core, the bridge
  delegates, so oracle + provider agree by construction (relinks the bridge — gate accordingly).

**§112 hooks pin:** `void(*)(const Operation&, void*)` pre + post on the Interpreter, `void* user`, null-default, checked
in `eval_op`; **post fires only on a SUCCESSFUL dispatch**; NOT proto-copied (consistent with set_user/cancel). A
counting-hook test asserts EXACT op counts (the not-≥ discipline).

**Three stale-comment fixes (2b):** `exec.hpp` "real per-width typing joins CEIR-6" (never landed) + "Jobs-backed
parallel async is CEIR-6c" (→11a); `task.ceirop.toml` header "route to CEIR-8 (compiled host plan)" (→CEIR-11a,
pre-renumber).

## 8. Stage 3 — jobs-backed launch/await ON-POOL (SIGNATURES + design LOCKED 2026-08-10, advisor consult)

**The crux the sequential contract dictates: `async.launch` bodies run IN-FRAME in the reference** — captures legally
resolve, stateful bodies legally work (UNLIKE `task.parallel_for`, whose TOML pins captures as UndefinedValue). So
"require async bodies pure" would be BOTH a contract change AND a capability regression (the provider TODAY runs
stateful/capturing launch bodies via its sequential installer). ⛔ **Stage 3 must never reject what stage 2 ran.**

### The design — CONDITIONAL POOLING (pool the provably-equivalent; sequential-fallback the rest)

Per launch, a **pool-eligibility classifier** (bridge-local — one consumer, the 9d doctrine; hoist when a 2nd arrives):
(1) StateEdge-free transitively + calls-resolved — reuse the moved `exec::preflight_region_walk` shape (NOT
`check_parallel_region` verbatim: launch bodies take 0 block-args + yield VARIADIC); (2) **no outer captures** — a NEW
walk (every operand of every op in the region is defined region-locally or is a block-arg). **Eligible → run ON-POOL.
Ineligible → the sequential in-frame semantics, reimplemented bridge-side** via public surface (`run_region` +
`store_yields` + `set_value` — the core EvalFns are file-local). ⭐ **Byte-parity holds by CONSTRUCTION in both branches:**
pooled bodies are pure + self-contained → schedule-independent (the 6b theorem); fallback bodies literally run the
reference semantics. (Option (c) "exclude impure from the differential" DISSOLVES — nothing needs excluding.)

### The token model — provider table, DISJOINT handle space, ALL FIVE consumers overridden

Sequential handles are u32 store indices; `eval_await` casts `u32(tok)`. ⛔ **A pooled handle > u32-max would TRUNCATE in
that cast and alias a valid sequential handle — a silent wrong read.** So partial override is unsafe: the provider
overrides **await, join, race, cancel, AND fiber_wait**, each a thin wrapper over ONE bridge helper
`resolve_token(i64) → yields` that checks the pooled table by FULL i64 value first, else the sequential path. Pins:
- **Pooled entry** `{jobs::Counter*, result Array from the provider's m_alloc}`. The worker runs the body on a **fresh
  sub from the proto over its OWN scratch** (6b verbatim), copies yields into the pre-allocated slot BEFORE the counter
  decrements — ⛔ **the counter is the happens-before edge.** The worker NEVER touches `in`'s yield-store (not
  thread-safe — a data race).
- **Nested launch inside a pooled body:** `set_user` is NOT proto-copied → the sub sees `user()==nullptr` → sequential
  fallback (NOT NoSemantics — nested async is legal). Sound: 6a's confinement theorem means nested tokens can't escape
  the body (consumed against the sub's own store).
- **race** → index 0 (wait it if pooled) — legal under its Nondeterministic contract, parity-preserving (real first-ready
  is 24/29). **cancel** on a pooled token = wait + discard — observationally identical to consume/no-op *because the body
  is pure* (crd-jobs has no preemption; the 6c cooperative doctrine). Both documented as deliberate.
- ⛔ **`execute()` DRAINS the pooled table unconditionally at exit** — the provider runs no verifier (6c), so an unverified
  program can leak a pooled token → a leaked Counter leaks a pool slot (jobs.hpp's warning) + a worker dereferencing
  module/ctx after execute returns. The drain is the containment.
- **spawn/worker** ride the same pooled path (default JobDecl); **main_thread = `JobDecl.pin_thread=0`** — awaiting it from
  the main thread works ONLY because `wait()` pumps on thread 0 (VERIFY 2); a deadlock-shaped assumption → **T4 tests it.**
  **group/scope** need NO override (sequential boundary + the drain).

### The witness (parity tests can't detect a lazy impl)

A never-pools implementation passes EVERY parity test (the perf-flag-measures-empty-frame scar). Expose
**`HostProvider::pooled_count()`** and ASSERT it: `==N` in the pooled-parity test, `==0` in the capturing/stateful
fallback tests — the positive proof the feature is ON.

### The two pre-write verifies (DONE 2026-08-10)

- **VERIFY 1 — `intern_op` on an existing name is READ-ONLY** (`context.cpp`: hash → loop `m_op_names` → return `OpId{h}`
  on hit, "no arena/heap churn"; only a NEW kind pushes to the arena). During pooled execution every op kind is already
  interned (proto setup interns task/async before the run), so the main thread's seq pf/mr EvalFns calling
  `func::call_kind(ctx)` are HITS → no mutation → **no race with worker Context reads.** ✅ Safe (state the finding: safe
  IFF no new kind is interned during the pooled phase — true here).
- **VERIFY 2 — `wait()` pumps on thread 0** (`jobs.hpp`: "on the main thread, spins calling pump() so thread-0-pinned jobs
  make progress"). Awaiting a `pin_thread=0` job from the main thread works. ⛔ **Assumption:** `execute()` is called from
  the main thread (thread 0); from a worker fiber awaiting a pin_thread=0 job, no one pumps thread 0 → deadlock. Document
  + T4 asserts the main-thread path.

### Tests (stage-3 DoD)

T1: N pure launches + join through the provider → byte-identical (`pin_values`) to the core-sequential reference,
`pooled_count()==N`. T2: a CAPTURING body through the provider → correct result via fallback, `pooled_count()==0` (the test
that would have caught a reject-design). T3: a STATEFUL body → same shape (fallback, correct, `pooled_count()==0`). T4:
main_thread-pinned spawn awaited from main → completes (the pump path). T5: pooled race→0 + pooled cancel consumed,
verifier-clean. T6: leak containment — an unverified token-leaking program returns from `execute()` without hang or ASan
leak. ⛔ Money configs win-asan + linux-gcc-asan; the Catch listener owns the pool (the workers-reset scar); timeout≠hang.

### Scope + boundary

The full 6z letter (launch/await ON-POOL + the six ops' pool placement) fits ONE implementation stage via `resolve_token`
— but it is a lot of lines. The 11a DoD boundary (do NOT creep): ON-POOL async byte-matches the sequential reference on
T1–T6; the FULL differential corpus is CEIR-11b; the alloc-free/string-free hot-loop audit is CEIR-11z. Stage 4 = the 11a
DoD gate + ADR-0122 (0122 was free at band open — re-verify then).
