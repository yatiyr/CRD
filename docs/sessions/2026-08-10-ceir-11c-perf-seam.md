# CEIR-11c — crd-perf integration: the profiling seam + per-op regions + a plan-compile cost counter — session log

> The autonomous grind ([[project_ceir_autonomous_loop_grant]]) opened CEIR-11c after 11b closed (ADR-0123). Contract
> (D-007 band-11): "every plan op wrapped in `CRD_PERF_SCOPE`-compatible regions (crd-perf, no new profiler)" + a
> plan-compile cost counter — WITHOUT breaking the §153 hot-loop discipline 11z audits. Design: advisor consult (recon +
> design fork).

## The decisive constraint (recon)

⛔ **crd-ceir core CANNOT link crd-perf.** The `crd-ceir-invariants` I5 allowlist excludes crd-perf, and crd-perf
PUBLIC-links crd-jobs — linking it into core would break the repeatedly-gated "crd-ceir STILL jobs-free" invariant. So
`plan.cpp` cannot `#include <crd/perf/...>`. The house pattern is the §112 `StepHook` seam (11a) + the crd/perf
`jobs_adapter` inversion: **the instrumented subsystem exposes a null-default fn-ptr seam; a perf-linking BRIDGE consumer
owns the crd-perf calls.** The bridge (crd-ceir-host) already sits above both ceir + perf and may link crd-jobs — the
documented home for what core can't link. (A `plan_adapter` in engine/perf would invert layering perf→ceir.)

## ✅ Part 1 — the CORE SEAM — DONE + gated (2026-08-10)

**What shipped** (`plan.{hpp,cpp}`, crd-ceir core — perf-FREE):
- **`RunHooks{pre, post, user}`** — null-default fn-ptrs; `run()` gains a defaulted `RunHooks hooks = {}` 4th param (zero
  call-site churn in the existing 436 tests). `run_seq` fires `pre(op)` before the dense `switch(Op)` and `post(op)` after
  a SUCCESSFUL dispatch. ⛔ The hook receives only the dense **`Op` id (u8)**, NOT an `Operation*` (§153 — the loop never
  touches one; the §112 seam in op-id form). Null-default → ONE predicted branch per instr (11z-clean on the shipping
  path). ⛔ **OBSERVATION-ONLY:** the hook reads only the op id, so it cannot perturb the differential.
- ⛔ The seam counts **dispatched instrs** — terminators (`core.yield`/`func.return`) and §20 latches are NOT instrs
  (captured at compile), matching §112 semantics. `post` fires only on success, so the pre/post stream is **UNBALANCED**
  (pre>post) on an erroring run — the consumer resets per run + tolerates it.
- **`CompileResult.stats` (`PlanStats`)** — cheap plan-SHAPE counts (num_instrs/seqs/funcs/cells/maps), all already
  computed; a scan at compile end. `compile()`'s signature is unchanged. The perf bridge publishes these as counters.

**The volume-tension ruling (advisor).** "Every plan op wrapped" as a crd-perf push/pop REGION per instruction is a
LYING profiler: at fuel-scale (`1<<24`) the 4096-slot ring overflows and silently drops samples (a no-silent-caps
violation). Counts-only is level-down. The gold middle: the SEAM observes every dispatched instr (contract satisfied at
the seam — a region-driving consumer stays possible, which is what "`CRD_PERF_SCOPE`-compatible" means); the shipped
BRIDGE ADAPTER does per-op-class dispatch COUNTS + per-op-class SELF-TIME (the parent-pause algorithm) + ONE region per
`run()` + ONE wrapping `compile()` + the CompileResult stats as COUNTERS (atomic slots — ring-safe).

**Tests** (`test_plan.cpp`, +2 `[ceir][plan]`): (1) the seam fires pre/post per dispatched instr on the composing program
— exact `pre == post == 35` (observe-once, the 11a count-29 precedent) AND results byte-identical with vs without the
seam (the observation-only witness) + the plan-shape stats (composing: 2 cells, 1 dp, ≥3 funcs); (2) an erroring run
(`await(const 99)` → `BadToken`) leaves the seam UNBALANCED (`pre > post`).

**Gate.** crd-ceir-tests + host + cook **438/438 ctest** (436 + 2) on **win-debug + win-asan + linux-gcc-debug +
linux-gcc-asan** + LLVM-20 tidy (`plan.{hpp,cpp}` + `test_plan.cpp`, clean) + GCC `-Werror=switch` + opgen +
`crd-ceir-invariants` (⛔ I5 intact — the seam is fn-ptrs, NO crd-perf in core; still jobs-free/asset-free). NO
recook/fuzz/version-bump.

## ✅ Part 2 — the BRIDGE ADAPTER (crd-perf wiring) — DONE + gated (2026-08-10) → CEIR-11c CLOSED

**What shipped** (`engine/ceir-host/{include,src}/…/plan_perf.{hpp,cpp}` — the perf-linking BRIDGE; crd-perf + crd-time
added to `crd-ceir-host` link deps, PRIVATE):
- **`PlanProfile`** — a POD: per-`Op`-class `dispatch[32]` counts + `self_s[32]` (SELF-time seconds, parent-pause) +
  `total_dispatch` + `max_depth` + a `depth_overflow` **witness** (⛔ no silent cap — a >256-deep parent-pause is counted).
- **`profiled_run`** — a `CRD_PERF_SCOPE("ceir.plan.run")` region; resets the seam state per run; wires the null-default
  hooks (`adapter_pre`/`adapter_post`) to the parent-pause algorithm (`pre`: bank now−mark to the stack-top parent, push,
  count; `post`: bank now−mark to THIS op via the ARG — overflow-robust; pop); publishes `ceir.plan.dispatch_total`.
  ⛔ The self-time uses `crd::time::MonotonicClock` (always available) → the profile fills even on a perf-OFF build; only
  the crd-perf regions/counters compile out when off.
- **`profiled_compile`** — a `CRD_PERF_SCOPE("ceir.plan.compile")` region; publishes the `PlanStats`
  (instrs/seqs/funcs/cells/maps) via `CRD_PERF_COUNTER_SET_I64`.

⛔ **The volume ruling realized:** NO region per instr (the 4096-ring would overflow at fuel 1<<24 — a lying profiler).
The adapter aggregates per Op class (counters + self-time, atomic/plain slots — ring-safe) + 2 regions (run / compile). A
region-driving consumer stays possible through the same seam ("CRD_PERF_SCOPE-compatible").

**Tests** (`tests/ceir-host/test_plan_perf.cpp`, +3 `[ceir][plan-perf]`) — the advisor pre-close mandated the loop + error
cases (a straight-line program's ns spans can round to 0 on a 100ns-granularity clock, and the adapter's parent-pause
state was untested):
1. **straight-line** (`const 5`, `const 3`, `addi` → 8; `return` is a terminator, NOT dispatched): exact
   `total_dispatch == 3`, `dispatch[ConstI]==2`, `dispatch[AddI]==1`, `depth_overflow==0`, the shape stats
   (`num_funcs==1`, `num_instrs==3`), all `self_s ≥ 0` (non-negative), and (`#if CRD_PERF_ENABLED`) the published counters
   read back (`ceir.plan.compile.funcs == 1`, `ceir.plan.dispatch_total == 3`). ⛔ NO `>0` self-time here (flake-averse).
2. ⭐ **a LOOP** (`for(0,1000,1){ iv: muli(iv,iv) }`): `dispatch[For]==1`, `dispatch[MulI]==1000`, `dispatch[ConstI]==4`,
   `total_dispatch==1005`, `max_depth ≥ 2` (the adapter's first real NESTING), and `total_self > 0` — a THOUSAND dispatch
   spans → µs-scale, granularity-proof on ANY clock (the flake-free way to prove self-time is measured; ⛔ still never a
   quantitative duration).
3. ⭐ **an ERRORING run** (`await(const 99)` → `BadToken`): the adapter's parent-pause state (stack/mark/banking) survives
   the UNBALANCED pre>post stream (the Await case returns before `post`) — `total_dispatch ≥ 1`, `depth_overflow == 0`, no
   crash (ASan-checked in-gate). Part 1's unbalanced test drove a plain counting hook — this exercises the adapter's own
   state machine (the ContinuationArity/depth-2 gap shape).

⛔ **A `static_assert(u8(plan::Op::MapReduce) < PlanProfile::kMaxOps)`** guards the per-Op arrays against a silent drop if
`plan::Op` ever grows past 32 (the widen-enum-audit-every-consumer rule).

**Gate.** crd-ceir-tests + host + cook **441/441 ctest** (438 + 3) on **win-debug + win-asan + linux-gcc-debug +
linux-gcc-asan** + LLVM-20 tidy (`plan_perf.cpp` + the test, clean) + GCC `-Werror=switch` + opgen + `crd-ceir-invariants`
(⛔ crd-ceir CORE still jobs-free/asset-free — the crd-perf/jobs link is BRIDGE-only). NO recook/fuzz/version-bump.

**Close.** ⛔ **NO ADR** — 11c is an INTEGRATION slice (a seam + a bridge adapter), not a foundation decision; this log +
the tracker row carry it. The real consumer (a live profiling front-end / provider composition) is a named-forward →
**CEIR-21/26** (the 10b PlanCache precedent: a test-consumer now, the contract row mandates it, the real wiring later).
Advisor pre-close review, then the 11c tracker-row flip ◧→✅ (band 11 → next is **11z**, the §153 hot-loop audit — the
seam it audits is now in place: null-default fn-ptr, one predicted branch, zero per-op heap/strings in the shipping loop).

## Proposed commit — CEIR-11c part 2 (the bridge adapter + the 11c close; user commits; NO AI trailer)

```
feat(ceir-11c): the crd-perf bridge adapter for the compiled-plan profiling seam + CLOSE 11c

Part 2 of CEIR-11c (the perf-linking consumer). A BRIDGE change (crd-ceir-host) -- crd-ceir core cannot
link crd-perf (I5 + jobs-free), so the bridge owns the crd-perf calls (the jobs_adapter pattern).

- engine/ceir-host plan_perf.{hpp,cpp}: PlanProfile (per-Op dispatch counts + parent-pause SELF-time +
  a depth_overflow witness); profiled_run (a CRD_PERF_SCOPE("ceir.plan.run") region wiring the seam hooks
  to the parent-pause algorithm; publishes ceir.plan.dispatch_total); profiled_compile (a
  CRD_PERF_SCOPE("ceir.plan.compile") region publishing the PlanStats as counters). Self-time uses
  MonotonicClock (always available -> the profile fills even perf-OFF; only the crd-perf regions/counters
  compile out). crd-perf + crd-time added to crd-ceir-host link deps (PRIVATE).
- The volume ruling: NO region per instr (the 4096-ring would overflow at fuel 1<<24 -- a lying
  profiler); aggregate per Op class (counters + self-time, ring-safe) + 2 regions (run/compile).
- tests/ceir-host/test_plan_perf.cpp +3: straight-line (exact dispatch counts + shape stats + perf-ON
  counter readback); a LOOP for(0,1000){muli} (nesting max_depth>=2 + granularity-proof total_self>0,
  1000 spans); an ERRORING await->BadToken run (the adapter's parent-pause survives the unbalanced
  pre>post). Self-time STRUCTURAL only (non-negative + measurable; NEVER a quantitative duration -- the
  flake scar). A static_assert guards the per-Op arrays vs a plan::Op widening past kMaxOps.
- No ADR (11c is an integration slice); the real consumer is a named-forward -> CEIR-21/26. The D-007
  tracker 11c row flips to CLOSED; band 11 -> 11z next.

Gated: 441/441 on win-debug + win-asan + linux-gcc-debug + linux-gcc-asan + LLVM-20 tidy + GCC
-Werror=switch + opgen + crd-ceir-invariants (CORE still jobs-free/asset-free -- crd-perf is bridge-only).
No recook, no fuzz, no version bump.
```

## Proposed commit — CEIR-11c part 1 (the core seam; user commits; NO AI trailer)

```
feat(ceir-11c): a sec-153-clean profiling seam on the compiled plan + plan-compile stats

Part 1 of CEIR-11c (crd-perf integration). A crd-ceir CORE change -- but crd-ceir core CANNOT link
crd-perf (I5 + jobs-free), so this adds only the SEAM; a perf-linking bridge consumer (part 2) owns the
crd-perf calls (the sec-112 StepHook / crd-perf jobs_adapter pattern).

- plan.hpp/cpp: RunHooks{pre, post, user} null-default fn-ptrs; run() gains a defaulted hooks param.
  run_seq fires pre(op) before the dense switch and post(op) after a SUCCESSFUL dispatch, passing the
  dense Op id (u8) -- NOT an Operation* (sec-153: the loop never touches one). Zero-cost when unset (one
  predicted branch). OBSERVATION-ONLY: the hook reads only the op id, so it cannot perturb the
  differential. post fires only on success -> the stream is UNBALANCED (pre>post) on an erroring run.
- CompileResult.stats (PlanStats): cheap plan-shape counts (instrs/seqs/funcs/cells/maps), computed
  once at compile end; the bridge publishes them as crd-perf counters. compile() signature unchanged.
- test_plan.cpp +2 [ceir][plan]: the seam fires pre==post==35 on the composing program AND results are
  byte-identical with vs without the seam (observation-only) + the shape stats; an erroring run
  (await const 99 -> BadToken) leaves the seam unbalanced (pre>post).

Gated: 438/438 on win-debug + win-asan + linux-gcc-debug + linux-gcc-asan + LLVM-20 tidy + GCC
-Werror=switch + opgen + crd-ceir-invariants (I5 intact -- NO crd-perf in core; still jobs-free). No
recook, no fuzz, no version bump. Part 2 (the bridge adapter) wires the actual crd-perf calls.
```
