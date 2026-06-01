# 2026-05-31 — hesap v5b-1: Gilbert-Peierls sparse LU (the serial correctness oracle)

Opens the **v5b sparse-direct LU cluster**. v5b-1 is the SERIAL reference oracle (correctness-gated, not
a crush); v5b-2 is the deterministic + parallel crush (SuperLU-class supernodal + MC64 + threshold static
pivot) validated against this. Followed v5a (Cholesky, now winning across the board vs CHOLMOD).

## What shipped

**Algorithm — `sparse_lu.hpp` / `sparse_lu.cpp`.** Faithful CSparse `cs_lu` (Davis 2006): per column k,
`x = L\A(:,k)` over the DFS-reachable pattern (`cs_reach`/`cs_dfs`, walked in topological order via
`cs_spsolve`), then DYNAMIC partial pivot (the unpivoted row of max |x|; threshold knob `tol`, default 1 =
pure partial pivot) → P·A = L·U stored CSC (unit-lower L + upper U + row perm). `solve` = apply P →
unit-lower forward → upper backward, multi-RHS, in place. `SparseLU<T> : IFactorization<T>` +
`factor_gp_lu`, 4 types (f32/f64/c32/c64; complex pivot uses the modulus).

Divergence from CSparse (pinned): used a clean `marked` byte array for the DFS visited-set instead of the
Lp-sign-flip trick — clearer + leaves the growing Lp untouched. Capacity-doubling for L/U
(`resize_uninitialized`→`reserve` preserves existing entries on grow; verified).

**Determinism boundary (pinned, structural).** v5b-1 is SERIAL — no `num_workers`, no `parallel_for`.
Dynamic partial pivoting is order-dependent, so a parallel factorization would pick a dispatch-dependent
pivot sequence; v5b-1 is therefore explicitly NOT the {1,2,4,8}-worker moat. It is run-to-run
deterministic (a fixed matrix is a pure function — tested). The deterministic + parallel LU is v5b-2
(MC64 + threshold static pivot).

**Tests — 9 cases / 81 asserts** (advisor-hardened):
- known-x_true residual on an unsymmetric matrix + 2D-grid Laplacian residual;
- pivoting: a 3×3 zero-diagonal forces an interchange + a heavy reverse-permuted diagonally-dominant
  matrix at scale (pivots on every column — exercises `pinv`≠identity, the final `Li` remap, the
  `x[pinv]` solve perm);
- singular: empty-column (no pivot candidate) + numerically-zero pivot (`a_max≤0` path);
- complex (Complex64) residual; multi-RHS (3 cols); run-to-run bit-identity; capacity-grow (the grid's
  heavy fill ≫ initial cap → grow ran, residual clean ⇒ grow preserves).

**CLI — `hesap.direct.lu_gp.{f32,f64,c32,c64}`** (user-directed: expose the oracle now, not just v5b-2's
production `lu.*`). COO triplets (general/unsymmetric) + RHS → [info, x]; complex flattened {re,im}.
+2 CLI tests (registration + general-matrix solve).

**Bench — `bench_hesap_lu_vs_reference`** (CRD_BUILD_HESAP_VS_REFERENCE) vs Eigen SparseLU
(NaturalOrdering on the same AMD-permuted matrix). HONEST oracle framing in the banner ("expect to LOSE
on time; gate = residual"):

| matrix | Cerid factor | Eigen factor | fill cerid / eigen | resid c / e |
|---|---|---|---|---|
| bcsstk13 (2k) | 239.7ms | 49.2ms (0.21×) | 1.38M / 1.40M | 1.1e-8 / 4.7e-9 ✓ |
| bcsstk24 (3.5k) | 1540ms | 176ms (0.11×) | 4.37M / 4.37M | 7.7e-9 / 2.4e-9 ✓ |

**CORRECTNESS ✓** (residual matches Eigen — the conditioning floor), **fill ≈ identical** (both AMD +
partial-pivot), **loses factor time 5–9× BY DESIGN** (serial CSparse reference). Bench is **small-corpus
only**: v5b-1 has no column reorder, and an AMD-SYMMETRIC ordering does not bound LU fill the way COLAMD
would — bcsstk25 (15k 3D-FEM) balloons to multi-GB (measured, killed). bcsstk25 + hood + ldoor are
v5b-2's turf (supernodal + COLAMD + parallel).

## Verification

win-debug: full hesap-direct suite **591429 asserts / 31 cases, no regression** (the +9 lu + the existing
Cholesky/frontal/cli + determinism moats). gcc `-Werror` clean (sparse_lu.cpp / cli_register_direct.cpp /
test — the i32/u32 pivot bookkeeping + static_casts pass sign-conversion). Bench validated with
CRD_BUILD_HESAP_VS_REFERENCE=ON (compiles vs Eigen + runs).

**Owed before the v5b phase-boundary commit:** clang-cl + ctest guards (non-ASCII test names etc.) +
win-shipping (CI can cover per "CI owns the sweep"). No commit (user commits at phase boundaries).

## Next — v5b-2 (the LU crush)

Supernodal LU (SuperLU / Demmel-Eisenstat-Gilbert-Li) + MC64 (v4j-1a) + threshold partial pivot for
DETERMINISTIC + PARALLEL pivoting (the {1,2,4,8} moat returns) + COLAMD column reorder + CLI
`hesap.direct.lu.*` + crush bench vs Eigen SparseLU / UMFPACK at scale (hood/ldoor). Validated against
this v5b-1 oracle.
