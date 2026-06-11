# 2026-06-10 — hesap v7-p-4 CLOSE: the BOBYQA port, differentially verified (same session, part 14)

**Phase:** 3.1.6 `crd-hesap` · v7 optimisation (`crd-hesap-opt`, ADR-0090)
**Slice:** v7-p-4 — BOBYQA, the LAST full-port slice. **CLOSED: all 6 routines ported + functional gates
(incl. the EXACT bound landing, x[0] == 1.0 bit-equal) + the differential harness: 3045 checks, 0 failures**
(update/altmov/trsbox bit-exact on 250 randomized states — half with tight bounds firing the active-set
paths; prelim bit-exact via the stop-building shim on interior/near-bound/at-bound starts; e2e bit-exact
x/minf with IDENTICAL EVAL COUNTS on 4 problems via the equal-dx identity-rescaling shim). Honest gap
(named): rescue_ has no targeted per-routine diff — coverage opportunistic via e2e. Suite 3588/131; 4
configs + guards green. **v7-p COMPLETE: the trio + COBYLA 2050/0 + NEWUOA 3773/0 + BOBYQA 3045/0 = 8868
oracle checks, 0 failures.**

---

## Scope

Powell's bounded quadratic-model method — bounds are NATIVE (no nested-solver variant to exclude; this is
why the NEWUOA port could pin the classic unconstrained scope). The NLopt wrapper's variable RESCALING
layer (`rescale_fun` + `bobyqa_minimize`) is NOT ported — the e2e shim diffs at the `bobyqa()` layer
beneath it, like the COBYLA/NEWUOA shims. Stop plumbing shared via `detail::cobyla_impl`.

## Ported so far (`engine/hesap-opt/include/crd/hesap/opt/bobyqa.hpp`, detail::bobyqa_impl, f2c idiom)

1. **`update`** (ref 18–140) — BMAT/ZMAT rank-2 update, NEWUOA's form WITHOUT the IDZ partition (BOBYQA
   keeps DENOM positive); the ZTEST small-entry threshold kept.
2. **`prelim`** (ref 1710–1950) — the bound-aware initial interpolation set (steps flip/shrink at active
   SL/SU; the stepa·stepb < 0 point switch; exact bound landing on x). Stop checks: minf_max + maxeval
   (force/time not carried).
3. **`altmov`** (ref 743–1159) — XNEW by the through-points line search (PREDSQ/PRESAV; exact bound landing
   via IBDSAV) + XALT by the constrained Cauchy step tried with both gradient signs.

## Remaining

4. **`trsbox_`** (ref 1161–1709, ~550 ln) — the BOUNDED truncated-CG TR subproblem (xbdi active-set fixing,
   the alternative angle iteration).
5. **`rescue_`** (ref 142–742, ~600 ln) — the denominator-degeneracy re-initialization.
6. **`bobyqb_`** (ref 1952–3063, ~1100 ln) — the driver; then `bobyqa()` (workspace partition + SL/SU
   setup from xl/xu — that layer IS ported since SL/SU shifts are algorithmic, only the NLopt rescaling
   above it is excluded), public `minimize_bobyqa(obj, x0, lower, upper, alloc, BobyqaOptions)`.
7. Functional gates (bounded quadratic exactness, bounds-active optimum, Rosenbrock-in-box, determinism,
   n edges) + the harness: extend `setup-nlopt-ref.sh` with `bobyqa_exposed.c` (+ e2e shim at the bobyqa()
   layer with real xl/xu) + `runtime/examples/bobyqa_difftest.cpp` (per-routine update/prelim/altmov/trsbox
   bit-exact + e2e identical eval counts). ⚠ rescue_ is hard to hit on benign problems — diff it
   per-routine on synthetic states (its e2e coverage is opportunistic).

## References

WSL `~/cerid-deps/nlopt/src/algs/bobyqa/bobyqa.c` + repo `external/nlopt-ref/bobyqa.c` (gitignored copy;
line numbers cited match it).
