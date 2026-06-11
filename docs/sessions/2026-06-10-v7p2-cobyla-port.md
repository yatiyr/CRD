# 2026-06-10 — hesap v7-p-2 CLOSE: the COBYLA port, differentially verified (same session, part 12)

**Phase:** 3.1.6 `crd-hesap` · v7 optimisation (`crd-hesap-opt`, ADR-0090)
**Slice:** v7-p-2 — COBYLA, the first of the three user-chosen FULL-PORT slices (the L-BFGS-B playbook).
**Status: CLOSED — port + functional gates + the differential harness: 2050 checks, 0 failures.**

---

## `cobyla.hpp` — the faithful port

Line-for-line port of the NLopt C reference (`nlopt/src/algs/cobyla/cobyla.c`, MIT — Roy's C translation of
Powell's Fortran COBYLA2 plus Steven G. Johnson's documented modifications, all carried: ENFORCE_BOUNDS,
the deterministic LCG simplex perturbation seeded with (n+m), the SAS ρ-increase rule, nlopt's stop
semantics with `relstop` verbatim). The f2c idiom is kept deliberately: 1-based pointer adjustments, the
ORIGINAL goto control flow (the L-BFGS-B lesson — restructuring Fortran-66 spaghetti is the bug farm), and
the **exact float-literal artifacts** (`.1f`/`.2f`/`1e-6f` are float-then-promote in C; ported as
`static_cast<T>(0.1F)` etc. so the oracle diff can be bit-exact). Named deltas: no force-stop / wall-clock
stop (Cerid has no async stop), iprint stripped (no numerics). `detail::cobyla_impl::trstlp` keeps the
reference's exact signature — it is the per-routine diff-harness target. Public driver:
`minimize_cobyla(obj, cons*, x0, lower, upper, alloc, CobylaOptions)` — c_I(x) ≥ 0 (COBYLA's own convention
= the pinned v7-j one), equalities rejected by assert (model as ± pairs), optional bounds, value-only.

## Functional gates (23 asserts / 6 cases; suite 3552/121; 4 configs + guards green)

- **Powell's own unit-disk product problem**: min x0·x1 in the disk → f* = −½ at the anti-diagonal, hit to
  1e-6.
- **Rosenbrock-in-the-unit-disk at the scipy reference x*** — the strongest gate: a derivative-free
  linear-model method from a different algorithm family lands on the SAME point the v7-n SQP/auglag/IPM
  battery pinned (1e-5; disk constraint active). Cross-family adjudication.
- Active variable bounds (pinned exactly; ENFORCE_BOUNDS keeps every eval inside the box).
- Bounded Rosenbrock (m = 0): converges; needs >20K evals at rhoend 1e-10 — COBYLA's documented slow crawl
  on curved smooth valleys (linear models), not a port bug; **eval-count-vs-oracle goes on the harness
  checklist** to confirm.
- **Bit-identical run-twice** (including the reference's LCG — deterministic by its (n+m) seed).
- n = 0 boundary.

## ⭐⭐ The differential harness — 2050 checks, 0 failures (the close gate, DONE)

Oracle (`scripts/setup-nlopt-ref.sh`, WSL): cmake-builds stock `libnlopt.a`; generates an EXPOSED-STATICS
TU (`#define static` + `#include "cobyla.c"` — no source patch) so `trstlp`/`cobylb` are linkable; dedupes
a lib copy via `ar d` (no duplicate symbols); appends the **`crd_cobyla_e2e` shim** — a rescaling-free
entry at the exact `cobyla()` layer `minimize_cobyla` mirrors (minimal `func_wrap_state` with con_tol
zeros + a zeroed `nlopt_stopping` with maxeval/ftol set, eval count out).

Harness (`runtime/examples/cobyla_difftest.cpp`, g++ on WSL + the linux-gcc-release crd libs):
- **Per-routine `trstlp`, BIT-EXACT** (dx + vmultc reals via memcmp, iact integer arrays, ifull, return
  code): 5 targeted regimes — feasible-at-zero (straight to stage 2), violated sets, **duplicate/parallel
  gradients (the L130 linear-dependence path)**, m = 0, n = 1 — plus 400 randomized instances with
  parallel rows injected every 5th trial, ρ ∈ {1e-3, 0.5, 5}.
- **End-to-end, bit-exact x and minf with IDENTICAL EVALUATION COUNTS** on all 5 shared problems
  (disk-product, rosen-in-disk, unconstrained rosen, boxed rosen, bounds-active sum). The boxed-Rosenbrock
  slow crawl from the functional battery is hereby confirmed as the REFERENCE'S OWN behavior — the eval
  counts match the oracle exactly.

`Rc::RoundoffLimited` aligned to nlopt's actual −4 (was −10; internal-only). Suite re-verified after:
3552/121 on debug + asan + shipping, tidy green.

## Next

p-3 NEWUOA → p-4 BOBYQA (same playbook; the oracle script extends per-algorithm) → v7-q global →
v7-r MIP → v7-z. User commits at v7 end.
