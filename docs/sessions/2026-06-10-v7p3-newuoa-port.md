# 2026-06-10 — hesap v7-p-3 CLOSE: the NEWUOA port, differentially verified (same session, part 13)

**Phase:** 3.1.6 `crd-hesap` · v7 optimisation (`crd-hesap-opt`, ADR-0090)
**Slice:** v7-p-3 — NEWUOA, the second FULL-PORT slice (after the differentially-verified COBYLA).
**Status: CLOSED — all 6 routines ported + functional gates + the differential harness: 3773 checks, 0
failures (per-routine trsapp/update/biglag/bigden BIT-EXACT on 250 randomized model states; end-to-end
bit-exact x/minf with IDENTICAL EVAL COUNTS on 4 problems). Suite 3571/126; 4 configs + guards green.
One port bug caught at build (a dropped 5th workspace arg in the newuob→biglag call, C2672).**

---

## Scope (pinned in the phase doc)

**Powell's CLASSIC UNCONSTRAINED NEWUOA.** NLopt's `newuoa.c` carries SGJ's NEWUOA_BOUND variant behind
`if (lb && ub)` blocks that nest an MMA optimizer (`nlopt_create(NLOPT_LD_MMA)` inside `trsapp_`/`biglag_`,
plus a truncation hack in `bigden_`/`newuob_`) — NOT ported (no nested-solver dependency; bounds are
BOBYQA's job, Powell's own position). The port takes the NULL-bounds paths verbatim; the e2e diff passes
NULL bounds so the comparison stays apples-to-apples.

## Ported so far (`engine/hesap-opt/include/crd/hesap/opt/newuoa.hpp`, detail::newuoa_impl, f2c idiom)

1. **`trsapp`** (ref 99–489) — truncated-CG TR subproblem + 2-D angle refinement, the L170 inline HD = H·D
   "subroutine" dispatched by ITERC. CRVMIN semantics kept.
2. **`update`** (ref 1396–1577) — the BMAT/ZMAT(+IDZ) rank-2 update. ⚠ Reference artifact kept VERBATIM and
   commented: `if (*idz == 1 && temp < zero)` where temp = sqrt(|denom|) ≥ 0 (Powell's Fortran tested
   DENOM) — the oracle diff is the contract; do NOT "fix".
3. **`biglag`** (ref 1059–1389) — |Λ_knew| maximizer, 49-point angle sweep; SGJ's isinf guard kept.
4. **`bigden`** (ref 495–1019) — the denominator maximizer (5-harmonic Fourier sweep, DEN/DENEX/PAR);
   isinf guards kept; the L340 lb/ub truncation hack skipped (scope).

Shared plumbing reused from the COBYLA port: `detail::cobyla_impl::{Rc, Stop, relstop, stop_ftol,
stop_evals}` (newuoa.hpp includes cobyla.hpp). `NewuoaOptions{rhobeg, rhoend, ftol_rel/abs, npt (0 ⇒ 2n+1),
max_evals}` declared.

## Remaining

5. **`newuob_`** (ref 1583–2489, ~900 lines) — the main driver (RESCUE-less NEWUOA loop: initial points,
   trsapp/biglag/bigden orchestration, the ρ schedule, nlopt stop checks at the same sites as the
   reference). The `if (lb && ub)` blocks at refs 1771/2111 skipped per scope.
6. **`newuoa()`** (ref 2490–2583) — the workspace partition; then the public
   `minimize_newuoa(obj, x0, alloc, NewuoaOptions)` (value-only, unconstrained).
7. Functional gates (rosenbrock + quadratic-exactness — NEWUOA's quadratic models interpolate a quadratic
   EXACTLY at npt = (n+1)(n+2)/2 — + determinism + n edges) + the differential harness: extend
   `scripts/setup-nlopt-ref.sh` with a `newuoa_exposed.c` TU (same `#define static` + `ar d` recipe + an
   e2e shim at the `newuoa()` layer passing NULL bounds) + `runtime/examples/newuoa_difftest.cpp`
   (per-routine trsapp/biglag/bigden/update bit-exact + e2e identical eval counts).

## References

WSL `~/cerid-deps/nlopt/src/algs/newuoa/newuoa.c` + repo `external/nlopt-ref/newuoa.c` (gitignored copy,
line numbers cited above match it).
