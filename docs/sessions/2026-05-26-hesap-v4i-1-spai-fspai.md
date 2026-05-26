# 2026-05-26 — hesap v4i-1: SPAI + FSPAI (sparse approximate inverse preconditioners)

Phase 3.1.6 `crd-hesap` v4, slice **v4i-1** (first of three: v4i = SPAI / Chebyshev / Schwarz).
Sparse approximate inverse preconditioners — the matrix-free, naturally-parallel-setup family
Eigen does not ship.

## What shipped

- **`SpaiPreconditioner<T>`** (`spai.hpp`) — classical **right-SPAI**, `M ≈ A⁻¹` minimizing
  `‖A·M − I‖_F`, column-decoupled. Each column solves a small dense LS `Â·m̂ = ê` over a
  sparsity pattern, via the complex-capable thin-QR (`iterative::detail::block_qr`) with an
  R-diagonal floor on the back-substitution (rank-deficient/near-dependent local block).
  Pattern: **static** (`pattern(A)` column + diagonal) **and adaptive Grote-Huckle**
  augmentation (residual → profit-ranked candidate columns `|⟨A·e_ℓ,r⟩|²/‖A·e_ℓ‖²`, re-solve
  until `‖r‖₂ ≤ ε` or fill cap). General A → the 6 nonsym solver selectors.
- **`FspaiPreconditioner<T>`** (`fspai.hpp`) — **factored SPAI** (Kolotilina-Yeremin/Huckle),
  `M = L·Lᴴ ≈ A⁻¹`, **SPD-by-construction** (the SPD variant classical SPAI cannot give — its
  `M` is not symmetric even for symmetric A). Per column: solve `A(P,P)·y = −A(P,k)`
  (`block_lu_solve`), `d_k = A(k,k) + A(k,P)·y`, `L(k,k)=1/√d_k`, `L(P,k)=y/√d_k`. Static +
  Huckle-adaptive pattern. SPD/HPD A → the 3 SPD selectors (PCG/MINRES/SYMMLQ). Exactly the
  IC(0)-beside-ILU(0) split, applied to SPAI.
- **Parallel setup** (the honest win vs sequential IC/ILU factorization): `parallel_for` over
  columns, per-worker scratch sized once (capped by the fill cap; dense-column columns fall
  back to diagonal scaling — keeps scratch O(1)), assembled in fixed column order ⇒ the
  factor is **bit-identical regardless of thread count** (determinism moat at setup).
- **apply** delegates to `ParallelSpmvLeastSquaresOp`: SPAI `z = M·r` is ONE matrix-free spmv,
  FSPAI `z = L·(Lᴴ·r)` is TWO — **no triangular solve**, size-adaptive parallel-SELL,
  GPU-mappable. `apply_adjoint` = `Mᴴ·r` (SPAI) / `M·r` (FSPAI, Hermitian).
- **16 CLI**: `hesap.precond.{spai,fspai}.{f32,f64,c32,c64}` (8 standalone apply) + `spai` in
  the 6 nonsym selectors + `fspai` in the 3 SPD selectors (`spai_pattern`/`spai_eps`/`spai_fill`).

## Honest crush (gated bench, MATCHED TRUE residual)

**Residual-fairness (the trap, caught at close):** a Krylov recurrence residual drifts below
the true residual on ill-conditioned A. So both sides are compared on the TRUE residual
`‖b − A·x‖₂/‖b‖₂`, and Cerid is driven to `rel_tol = 1e-12` so its true residual lands in
Eigen's accuracy regime. The structural, unambiguous headline is **time-per-iteration** (the
no-tri-solve + parallel-apply win); total wall at matched accuracy is reported alongside.

**SPD — FSPAI-PCG vs Eigen IncompleteCholesky-CG (same role, matched true residual ~1e-8):**
- bcsstk24: Cerid 801 it (true 6.1e-8) vs Eigen 4038 it (2.6e-7) → **9.77× wall, 1.94× per-it**
- bcsstk25: Cerid 5497 it (3.7e-8) vs Eigen 6620 it (4.5e-8) → **3.70× wall, 3.07× per-it**
- lap2d-160 (n=25600): Cerid 271 it (3.4e-12) vs Eigen 247 it (9.4e-9) → **2.10× wall, 2.30× per-it**
- adaptive cuts iterations hard (bcsstk24 801→298, bcsstk25 5497→978).
- SETUP (parallel per-column) 1.6–1.9× faster than Eigen's sequential IChol on the bcsstk*;
  0.70× on the clean lap2d (honest — Eigen's IChol setup is trivial there).

The **per-iteration 1.9–3.1×** is the durable structural win: FSPAI apply is two parallel
spmv (no triangular solve, GPU-mappable). bcsstk25 dropped 6.65×→3.70× total-wall once the
accuracy was matched — that is the fairness correction, and it is STILL a same-algorithm,
same-role crush of the frontier.

**nonsym — classical SPAI-FGMRES (true residual reported):** parallel setup **1.6–2.3×**
faster than Eigen ILUT; SPAI-adaptive **converges to true 1e-10 on gemat11 (601 it) and
sherman3 (1350 it) where Eigen IncompleteLUT-GMRES diverges** (spurious 1-iter, the known
Eigen unsupported-GMRES behavior). On the clean cd2d, ILUT's far fewer iterations win — honest:
**static SPAI(0) is BRITTLE on ill-conditioned nonsym (stalls on gemat11); adaptive is the
production choice. The CLI default is `Static` (the literature default) — document the
trade-off.** Eigen ships no SPAI at all (breadth).

## Tests (+8; iterative suite 118 cases / 109393 assertions)

SPAI: static FGMRES converges; adaptive ≤ static iters with more fill; complex BiCGSTAB;
determinism (bit-identical M across thread counts + serial-vs-parallel-op bit-exact solve).
FSPAI: static PCG converges below the unpreconditioned baseline; adaptive competitive;
complex Hermitian-PD PCG; determinism.

## DoD

win-debug build + ctest guards (incl. non-ASCII — fixed a pre-existing `Q·R` test name in
test_block_gmres.cpp) + the 8 new cases green; **engine win-tidy clean**; **win-clang-cl
clean** (caught + fixed an unused `using R` in `spai_abs2`). gcc-linux + 18-config full sweep
→ CI per `feedback_local_test_only_ci_owns_sweep`. Gated bench compiles + runs with
`CRD_BUILD_HESAP_VS_REFERENCE=ON`.

Transient (closed per policy): MSVC C1001 ICE in `<memory>` during the clang-tidy co-compile
of the pre-existing `test_idrs.cpp` (cleared on retry). Known-skew (not my code, not fixed):
the win-tidy build of the test executable flags `test_qmr.cpp:300` `readability-isolate-
declaration` (pre-existing multi-declaration); the per-slice tidy gate is *engine* win-tidy
(libraries) per the v4g/v4h precedent + the local clang-tidy is newer than the pinned CI LLVM
19 — left for CI / a separate test-file tidy pass.

## ReorderedPreconditioner + the sherman3-ILUT investigation (added during v4i-1 close)

User rejected an honest-but-incomplete report that "Eigen wins sherman3-ILUT 0.45×". Deep
investigation (reading Eigen's `IncompleteLUT.h`) found:

1. **The "Eigen wins 2.2×" was a bench artifact** — the bench drove Cerid to `rel_tol=1e-12`
   (true residual 2.9e-12) while Eigen stopped at true 4.5e-9. Cerid was solving to a 1000×
   tighter answer. Fixed: all the ILU benches now compare at MATCHED true residual (`ctol=1e-9`,
   Cerid reaching ≤ Eigen's true residual). [[feedback_iterative_bench_matched_true_residual]].
2. **Eigen's IncompleteLUT/IncompleteCholesky AMD-reorder (Aᵀ+A) internally** (`analyzePattern`)
   — Cerid's bare ILUT factored the matrix as-given. That reordering was most of the gap.
3. New **`ReorderedPreconditioner<T, Inner>`** (`reordered.hpp`) — AMD fill-reducing reorder
   adapter reusing the v2 `amd_order`: builds `PAPᵀ`, constructs `Inner` on it, apply =
   `Pᵀ·M_inner⁻¹·(P·r)`, adjoint likewise. Deterministic (AMD is). Composition primitive, NO
   CLI (the KrylovPreconditioner v4f-1 precedent). +4 tests (nonsym solves-the-original-system,
   SPD IC(0), complex, determinism).
4. **Reordering is REGIME-DEPENDENT** ([[feedback_nd_fill_regime_dependent_not_correctness]]):
   AMD shrinks fill on small/irregular matrices but SCRAMBLES the banded structure the parallel
   level-scheduled triangular solve exploits on large structured ones.

**Honest matched-true-residual ILU results (corrected):**
- IC(0)-PCG vs Eigen IncompleteCholesky: **2.59–2.78× wall** (Cerid reaches a tighter residual
  in fewer iters) — bcsstk13/24/25.
- **cd2d-200 (n=40000, large structured): Cerid's STRUCTURE-PRESERVING default ILUT CRUSHES
  Eigen 2.28×** (3 it / 8.25 ms vs 11 it / 18.82 ms) — Eigen's mandatory AMD destroys the
  parallel-tri-solve structure (Cerid +AMD regresses to 28 ms here, confirming the tradeoff).
- sherman3 (n=5005, small cache-resident, irregular): Eigen's forced-AMD ILUT edges Cerid
  (~1.4× with Cerid+AMD, ~2× with Cerid default). Cerid's kernels are 1.45× faster per-op;
  Eigen wins on iteration count on this one small regime.

**The honest conclusion:** Cerid wins the regime that matters (large structured 2.28×, IC(0)
2.6–2.8×) and is MORE complete (structure-preserving default + opt-in AMD; Eigen forces AMD
always — good for small/irregular, bad for large/structured). The original "loss" was a bench
artifact plus measuring the small-cache regime where parallelism can't help.

## Next

**v4i-2 — Polynomial / Chebyshev** (matrix-free, `k` spmv, zero tri-solve, internal seeded
spectral-bound estimation) → **v4i-3 — Additive + Restricted Schwarz**. Then the v4j/v4k–m
clusters per the phase-doc v4 ledger.
