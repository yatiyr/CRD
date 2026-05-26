# 2026-05-26 — hesap v4j-1b: MultilevelIlu scaffold (MC64-preprocessed ILUT)

Phase 3.1.6 `crd-hesap` v4, slice **v4j-1b** — the multilevel-ILU scaffold consuming MC64
(v4j-1a). A working, robust preconditioner now; the structure the v4j-2 inverse-based-pivoting
recursion plugs into.

## What shipped

- **`MultilevelIlu<T>`** (`multilevel_ilu.hpp`, crd-hesap-preconditioners):
  - MC64 (v4j-1a) match + scale → build `B = D_r·A·D_c·Pᶜ` (matched/largest entries on the
    diagonal, scaled toward an I-matrix) → `ILUT(B)`.
  - `apply M⁻¹r = D_c·Pᶜ·(ILUT(B)⁻¹·(D_r·r))`; `apply_adjoint = D_r·(ILUT(B)⁻ᴴ·(Pᶜᵀ·(D_c·r)))`
    (D_r, D_c real diagonal ⇒ Dᴴ = D). The transform derivation: `B = D_r·A·D_c·Pᶜ` ⇒
    `A⁻¹ = D_c·Pᶜ·B⁻¹·D_r`.
  - **Single-level placeholder**: `num_levels() == 1`. v4j-2 replaces the single ILUT with the
    inverse-based pivot test (rows whose elimination would push ‖L⁻¹‖/‖U⁻¹‖ past κ are DEFERRED
    to a recursively-factored coarser level) — the actual multilevel. The class + the MC64
    preprocessing pipeline + the threaded apply are the scaffold.
  - Deterministic (MC64 + triplet build + ILUT all deterministic); real + complex.
- **8 CLI** (`hesap.precond.mlilu.{f32,f64,c32,c64}` + `mlilu` in the 6 nonsym solver selectors —
  general A; MC64 handles nonsymmetry).
- **+4 tests**: solves an off-diagonal-dominant system (a cyclically-column-shifted tridiagonal
  whose largest entries sit off the diagonal — validates the MC64 transform bookkeeping end to
  end); **beats plain ILUT on it** (the MC64 matching+scaling payoff — plain ILUT factors the
  small shifted diagonal, MC64 recovers the well-conditioned core); complex; determinism (serial
  vs parallel spmv bit-exact). Iterative suite 135 cases / 112068 assertions.

## Why it's already useful (not just a placeholder)

MC64's two-sided scaling + max-weight matching is the robustness front-end that fixes the
conditioning of hard, badly-scaled, non-diagonally-dominant matrices on which plain ILUT (and
even AMD-reordered ILUT, which permutes but does not scale) struggles. So v4j-1b is a real
preconditioner — the multilevel recursion (v4j-2) adds the κ-bounded deferral that competes with
AMG on the hardest systems.

## DoD

win-debug build + ctest guards + 4 new cases green; engine win-tidy clean (tidy-checks
multilevel_ilu.hpp via cli_register); win-clang-cl clean; gcc + 18-config sweep → CI.

## Next

**v4j-2** — the inverse-based pivot test + the accept/defer multilevel recursion (replacing the
single-level placeholder), then **v4j-3** (head-to-head vs ILUPACK on Linux/WSL + AMGCL).
