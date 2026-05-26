# 2026-05-26 — hesap v4i-3: Additive + Restricted Schwarz (closes v4i)

Phase 3.1.6 `crd-hesap` v4, slice **v4i-3** — overlapping domain-decomposition preconditioners.
Closes v4i (SPAI + Chebyshev + Schwarz).

## What shipped

- **`SchwarzPreconditioner<T>`** (`schwarz.hpp`) — overlapping DD: `M⁻¹ = Σ R̃ᵢᵀ Aᵢᵢ⁻¹ Rᵢ`.
  [0,n) is partitioned into disjoint base subdomains (contiguous chunks, or ND-reordered chunks
  via v2 `nd_order`), grown by `overlap` graph-neighbour BFS layers into the overlapping Ωᵢ;
  each local block Aᵢᵢ is factored ONCE (factor-once/solve-many dense LU, partial pivot,
  `detail/dense_lu.hpp`) and its forward/back solve runs every apply.
  - **`SchwarzType::Additive`** (AS): full-Ωᵢ prolongation. `M_AS = Σ Rᵢᵀ Aᵢᵢ⁻¹ Rᵢ` is SYMMETRIC
    for symmetric A WITH EXACT LOCAL SOLVES (dense LU is exact) ⇒ SPD-valid (CG/MINRES/SYMMLQ);
    overlap writes summed ⇒ serial apply (fixed order, deterministic). [Contract: an inexact
    local solve would break the symmetry.]
  - **`SchwarzType::Restricted`** (RAS, default; Cai-Sarkis): non-overlap Ω⁰ᵢ prolongation. NOT
    symmetric (⇒ FGMRES/BiCGSTAB) but converges better, and every output index is written by
    exactly one subdomain ⇒ CONTENTION-FREE PARALLEL apply, bit-exact across threads.
  - `SchwarzPartition::{Contiguous, NestedDissection}`; cap `|Ωᵢ| ≤ kSchwarzLocalMax=1024` with
    base-block fallback (dense-column safety net); sorted-ascending Ωᵢ ⇒ deterministic LU.
  - Adjoint solves the conjugate-transpose blocks. Real + complex.
- **8 CLI** (`hesap.precond.schwarz.{f32,f64,c32,c64}` + `schwarz` in all 9 solver selectors —
  AS in the SPD selectors PCG/MINRES/SYMMLQ, RAS in the 6 nonsym selectors).
- **+5 tests**: AS-symmetric-accelerates-CG, RAS-BiCGSTAB-solves-the-original-system,
  overlap-monotone strength + ND-partition converges, complex, determinism (RAS parallel ==
  serial bit-exact). Iterative suite **131 cases / 111662 assertions**.

## Honest bench — vs the CORRECT peer (block-Jacobi), not Eigen

**Eigen ships NO domain-decomposition preconditioner of any kind** (only Diagonal /
IncompleteCholesky / IncompleteLUT). Benchmarking Schwarz against Eigen IncompleteCholesky was
an apples-to-oranges category error (a DD method vs a factorization preconditioner) — corrected.
Schwarz's true peer is **block-Jacobi** (Schwarz = block-Jacobi + overlap + exact local solves),
so the honest measurement is the **overlap value-add** (both Cerid; breadth Eigen entirely lacks):

- **cd2d-200 (n=40000, nonsym, RAS-BiCGSTAB): RAS-Schwarz(overlap=1) = 117 it / 766 ms BEATS
  block-Jacobi 264 it / 854 ms — 1.11× wall AND 2.3× fewer iterations.** The overlap pays off.
- lap2d-200 (SPD, AS-PCG): overlap cuts iterations hard (block-Jacobi 374 → ov=1 178 → ov=2 120)
  but the bigger overlapping dense blocks (max|Ω| 194 → 328) raise per-iter cost, so block-Jacobi
  wins wall here — honest: overlap is a knob (ov=1 sweet spot), not "more is always faster", and
  one-level Schwarz's dense local solves are inherently more work than a sparse factor.

**Positioning (honest):** one-level Schwarz is the domain-decomposition building block — its
design regime is distributed memory (one subdomain per node, minimal-communication parallel
apply) and as the **two-level / AMG component** (the coarse-space correction that fixes the
1/H² one-level degradation lives with AMG at v4k). On a single shared-memory node, IC/ILU/FSPAI
(which Cerid crushes Eigen on) remain the right tool for grid problems; Schwarz adds the DD
family Eigen lacks + the overlap-beats-block-Jacobi win on the nonsym regime.

## DoD

win-debug build + ctest guards + 5 new cases green; engine win-tidy clean (tidy-checks
schwarz.hpp + dense_lu.hpp via cli_register); win-clang-cl clean; gated bench compiles + runs.
gcc + 18-config sweep → CI.

## 🎉 v4i CLOSED

SPAI + FSPAI (+ AMD-reorder adapter) + Chebyshev + Additive/Restricted Schwarz — the complete
matrix-free / naturally-parallel / domain-decomposition preconditioner families Eigen lacks.
NEXT per the v4 ledger: v4j (multilevel-ILU) → v4k–m (AMG).
