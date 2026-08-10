# CEIR-11z — the §153 hot-loop audit gate — session log → CEIR band 11 CLOSED

> The autonomous grind ([[project_ceir_autonomous_loop_grant]]) reached the last band-11 slice after 11c closed. 11z is a
> GATE/PROOF slice — it proves the §153 hot-loop property already DECIDED in ADR-0123 (the compiled tier was designed
> against §153 from line 1), so ⛔ NO ADR. Contract (D-007 11z row): "allocation-free + string-free hot loop proven by
> CountingAllocator + a no-string-table-touch assert; differential corpus green."

## ✅ DONE + gated (2026-08-10) — CEIR-11z, and with it CEIR band 11

**What shipped** (`test_plan.cpp`, +3 `[ceir][plan]` — an AUDIT, no engine change): a `CountingAllocator` decorator
(forwards to a backing allocator, tallies every memory-acquiring call — `allocate`/`reallocate`/`try_allocate`).
- ⭐ **ALLOCATION-FREE arith hot loop:** `@main(){ for(0,M,1){ iv: muli(iv,iv) }; return 0 }` — compile with `root`
  (uncounted), RUN with the CountingAllocator, for M=10 and M=1000. The alloc COUNT is **INDEPENDENT of the trip count**
  (`a10 == a1000`) AND small (`≤ 4` — the fixed setup: the frame stack + the result array). ⇒ the arith hot loop does
  ZERO heap per iteration (the v14-m allocation-free-infer precedent, applied to the plan executor). ⛔ Run with **hooks
  NULL** (the default) — the shipping path 11z audits (the 11c seam is one predicted branch, not a per-op call, on this).
- ⭐ **CELLS + CALL FRAMES are AMORTIZED-arena** (the advisor pre-close addition — the arith loop covers only a third of
  the op classes; ADR-0123 §2.2's "killed violations" claim is precisely that state + frames are amortized): `@inc(%x){
  return x+1 }  @main(){ for(0,M,1){ iv: %c = call inc(iv); %acc = state(0, acc + c) }; return 0 }` — the FIRST iteration
  init-fills the §20 ring + grows the call-frame stack (one-time); STEADY STATE is ZERO (the ring is sized, the frame
  reuses capacity after the pop). So `count(M=10) == count(M=1000)` — the equality IS the amortization proof (both include
  the same one-time first-iteration cost). ⛔ **The data-parallel ops (per-launch token arrays) are LEGITIMATELY out of
  the strict-zero scope** — amortized-arena BY DESIGN (ADR-0123 §4 / the design note's "zero-alloc scope": straight-line
  compute strictly zero, token/frame growth amortized-arena). So "§153-clean hot loop" = strict-zero for arith/control
  flow + amortized-arena for cells/frames/tokens, both audited/documented — NOT an unqualified "zero-alloc everywhere".
- ⭐ **NO-STRING-TABLE-TOUCH:** `@main(){ return 5*5 + 3 }` (an attr-bearing program — the const `"value"` folded at
  compile) is compiled inside an INNER scope, then the **Context is DESTROYED** (its op/attr/string INTERNING tables
  gone), and the plan is RUN with NO Context in scope → correct result (28). The plan is a self-contained DENSE object in
  `root` (no `Operation*`, no `OpId`/`AttrId`, no string-table indices), so correctness after the Context dies PROVES the
  hot loop touches no string/attr table (§153, by construction — `run()` has no `Context` parameter).
- **Differential corpus green:** already proven — the 6 CEIR-11b corpus programs (5z/6z/async/six-task-ops/composing) +
  the §121 twin run in every gate. 11z is the §153 lens over that same corpus.

**Gate.** crd-ceir-tests + host + cook **444/444 ctest** (441 + 3) on **win-debug + win-asan + linux-gcc-debug +
linux-gcc-asan** (⭐ the CountingAllocator count is IAllocator-level → stable under ASan) + LLVM-20 tidy (`test_plan.cpp`,
clean) + GCC `-Werror=switch` + opgen + `crd-ceir-invariants` (crd-ceir core jobs-free/asset-free). ⛔ **NO recook, NO
fuzz, NO version bump.** ⭐⭐ **CEIR BAND 11 (Reference executor + compiled host plan) is COMPLETE** — the §84 two-tier
execution model: 11a the §118 reference oracle (ADR-0122) · 11b the compiled tier, differential-verified (ADR-0123) · 11c
the crd-perf seam+adapter · 11z the §153 audit. Next band → CEIR-12.

## Proposed commit — CEIR-11z + the band-11 close (user commits; NO AI trailer)

```
test(ceir-11z): the sec-153 hot-loop audit (allocation-free + no-string-table) -- CLOSE band 11

The last band-11 slice: a GATE/proof of the sec-153 property ADR-0123 designed the compiled tier
against. No ADR (a proof slice), no engine change.

- test_plan.cpp +3 [ceir][plan]: a CountingAllocator decorator; an ALLOCATION-FREE arith-loop audit
  (M=10 vs M=1000 -> alloc COUNT independent of the trip count + small -> zero heap per iteration); an
  AMORTIZED-arena audit (for{ call inc + state cell } -> count(10)==count(1000): the ring init-fill +
  first frame growth are one-time, steady-state zero -- the sec-153 claim for cells + frame windows;
  dp token arrays are amortized-arena BY DESIGN, out of strict-zero scope); a NO-STRING-TABLE-TOUCH
  audit (a plan compiled in an inner scope runs correctly AFTER the Context -- and its op/attr/string
  tables -- is destroyed, since run() is Context-free and the plan is dense). All hooks-NULL.
- The differential corpus (5z/6z/async/tasks/composing + the sec-121 twin) is already green in-gate.

Gated: 444/444 on win-debug + win-asan + linux-gcc-debug + linux-gcc-asan + LLVM-20 tidy + GCC
-Werror=switch + opgen + crd-ceir-invariants. No recook, no fuzz, no version bump. CEIR BAND 11 CLOSED
(11a reference oracle / 11b compiled tier / 11c crd-perf / 11z sec-153 audit).
```
