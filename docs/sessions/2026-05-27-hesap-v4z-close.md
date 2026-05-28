# 2026-05-27 — hesap v4z: v4 CLOSE (audits + reorder default-ON + robustness + ADR §26)

Phase 3.1.6 `crd-hesap` v4, slice **v4z** — the cluster close. No new algorithms: quantify the
convection result, settle the `reorder` default, audit complex + CLI completeness, fix a
robustness gap that surfaced during the audit, and lock the decisions into ADR-0065 §26.

## What shipped

### 1. Factor-vs-solve split + reuse break-even (the convection "crush", quantified)

`bench_hesap_mlilu_vs_ilupack` already timed factor + solve separately; it now also prints the
**reuse break-even** `k* = (factor_I − factor_C)/(solve_C − solve_I)` — how many re-solves of the
same factorization before ILUPACK's cheaper per-solve repays its dearer factor. Measured (WSL
release, cd2d β=0.3, `mlilu_ib`+reorder vs ILUPACK V2.4, matched true residual):

| mesh | Cerid factor/solve | ILUPACK factor/solve | single-shot | break-even |
|---|---|---|---|---|
| 50² | 3.5 / 2.25 ms | 12.1 / 0.8 ms | Cerid 2.24× | k\* = 5.9 |
| 100² | 13.2 / 17.0 ms | 59.2 / 4.4 ms | Cerid 2.11× | k\* = 3.7 |
| 150² | 30.7 / 47.2 ms | 154.8 / 12.8 ms | Cerid 2.15× | k\* = 3.6 |

The "crush" is now a precise statement: single-solve / few-RHS workloads favour Cerid (2.1–2.2×
total wall via cheaper factor + cheap AMD + low fill); many-RHS / frozen-preconditioner
time-stepping (≥~4 re-solves) favours ILUPACK's fewer iterations. The β=0.3 *iteration* gap
(45–76 vs 7–10) remains the documented aggregation-AMG wall (6 levers exhausted, v4k-e-2).

### 2. `reorder` default → ON for nonsym `InverseBasedIlu`

Flipped the ctor default `reorder = true` (matches ILUPACK's always-reorder posture). Evidence
from a new pure-Cerid reorder OFF-vs-ON comparison added to `bench_hesap_ilu_vs_reference`
(in-regime matrices):

| matrix | reorder=off | reorder=on | verdict |
|---|---|---|---|
| sherman3 (non-convection) | 1066 it, 145 ms, fill 2.43× | 557 it, 99 ms, fill 2.49× | ON wins 1.47×, fill flat |
| cd2d-150 (convection) | 269 it, 158 ms | 68 it, 91 ms | ON wins 1.75× |
| gemat11 (wrong-tool) | 51 levels / 727× fill | (now degrades — see §3) | not a reorder signal |

No regression on any in-regime matrix. The recursion already propagated `m_reorder`; the
`mlilu_ib` CLI gained a `reorder` Bool param (additive, default true). Four natural-order-intent
tests pinned to explicit `reorder=false` (the `m_nat` baseline, the κ=1.05 defer-count test, the
v4j-2b multilevel test, the non-reorder adjoint test).

**gemat11 is a wrong-tool case, not a knob issue:** 51 levels / 727× fill is *identical* at κ=5
and κ=100, so κ is not the lever — gemat11 (power-circuit, structural zero diagonals) is outside
the inverse-based-ILU PDE regime. It does not discriminate `reorder`.

### 3. Robustness fix — singular dense Schur leaf degrades instead of asserting

The reorder=on gemat11 run exposed a real gap: the multilevel ILU's deferred coarsest block
became numerically singular and `DenseLuLeaf` handed it to `solve_lu`, which (correctly) asserts
(`factor is singular`). Per `feedback_incomplete_factorization_robustness` a preconditioner must
degrade, never abort. Fix: `DenseLuLeaf::factor_robust` detects a singular factor and shifts the
diagonal by a relative pivot floor `√ε·max|diag|` (geometric back-off, ≤40 tries) then refactors,
so the leaf stays an applicable perturbed preconditioner. `solve_lu`'s contract is unchanged (the
fix is upstream of it). gemat11 reorder=on now completes (53 iters, true r=7.6e-09) — the bench
runs to EXIT 0. +1 test (a structurally-singular 4×4 leaf yields finite `apply`/`apply_adjoint`).

### 4. Complex-completeness audit

Every solver/preconditioner/AMG op registers all four `.f32/.f64/.c32/.c64` variants, and every
one has a complex *residual* test (not just compile coverage). Two gaps found + closed: a
`block_pcg<C>` test (block-CG c64 was tested, the preconditioned complex path was not) and a
point-`Jacobi<C>` PCG test (block-Jacobi c64 was tested, point was not).

### 5. CLI-completeness audit

Diffed the op list against the registered command list across `hesap-iterative`,
`hesap-preconditioners`, `hesap-amg` — zero gaps (every op × 4 types). Added the `reorder` knob to
`mlilu_ib` (the one knob that became meaningful with the default flip).

### 6. ADR-0065 §26 lock

Locked the v4 cluster: **D(iter)-1..7** (the determinism moat — serial reductions, size-adaptive
operator + frame_reset, GMRES/short-recurrence/block breakdown guards, packed-MGS block
orthonormalization, recycling selection, graceful-degrade) + **D(iter)-8..10** (reorder default-ON,
O(1) AMD bucket-head selection, the β=0.3 quantified-not-slogan result). Existing D(mlilu)-1..6 and
D(amg)-1..6 carry forward.

## Verification

- hesap-iterative suite (win-debug): **112600 assertions / 144 cases** green (incl. new robustness +
  block_pcg<C> + point-Jacobi<C> tests, and the 4 pinned natural-order tests).
- hesap CLI registry (win-debug): **151 assertions / 39 cases** green.
- Both reference benches (ILUPACK on WSL, ilu-vs-reference on Windows) rebuilt + run clean.
- Touched-module local gate green; the 18-config full sweep runs in CI per
  `feedback_local_test_only_ci_owns_sweep`.

## Decisions

ADR-0065 §26 (D(iter)-1..10). Reorder default-ON for nonsym `InverseBasedIlu` (user-approved with
the Step-2 evidence). Singular-leaf graceful degradation (user-approved fix-in-v4z).

## Next

**v5 — sparse-direct factorization** (supernodal Cholesky + Gilbert-Peierls LU + multifrontal QR +
LDLT + HSS reserve + complex + CLI), then eylem v1c resume.
