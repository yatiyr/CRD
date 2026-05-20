# 2026-05-20 — Phase 3.1.6 `crd-hesap` v0e-c + v0e-d: LDLT (Bunch-Kaufman) + QR (Householder)

Combined session log for v0e-c (LDLT, Bunch-Kaufman indefinite) and
v0e-d (Householder QR). Both shipped on 2026-05-20 in the same session
as v0e-a + v0e-b. Patterns reuse the established v0e-a substrate
(Permutation, hesap_jobs_fixture, allocator propagation).

## v0e-c — LDLT with Bunch-Kaufman pivoting

`A = P · L · D · Lᵀ · Pᵀ` where D mixes 1×1 + 2×2 blocks.

**Pivot selection** (UPLO=Lower, ALPHA = (1+√17)/8 ≈ 0.6404):
1. `absakk = |A[k,k]|`; `colmax = max |A[i,k]| for i > k`; `imax` = arg max.
2. If `absakk ≥ ALPHA · colmax`: 1×1 pivot at (k, k), no swap.
3. Otherwise scan `rowmax` in row `imax`:
   - If `absakk * rowmax ≥ ALPHA · colmax²`: 1×1 at (k, k).
   - Else if `|A[imax, imax]| ≥ ALPHA · rowmax`: 1×1 at (imax, imax) with row/col swap.
   - Else: 2×2 pivot at (k, k+1); swap k+1 with imax if needed.

**Storage** (LAPACK xSYTRF):
- Strict lower triangle of `packed()` holds L (unit diag implicit; for
  a 2×2 block, the L[k+1, k] slot stores D[k+1, k] instead).
- Diagonal of `packed()` holds D's 1×1 blocks (or D[k,k] and D[k+1, k+1]
  of a 2×2 block).
- `block_kinds[k]`: `1` for a 1×1 pivot; `2` for the start of a 2×2
  block; `0` for the continuation marker.
- `Permutation` carries pivot indices (same value for both rows of a
  2×2 block).

**Solve** is 5 steps: apply P (forward) → forward-sub L (block-aware,
skips L[k+1,k] for 2×2) → D · z = y (1×1 divide or 2×2 inverse) →
back-sub Lᵀ (block-aware, skips Lᵀ[k, k+1] for 2×2) → apply Pᵀ
(reverse).

**Trailing update**: unblocked nested loops for v0e-c-MVP. `v0e-c-blocked`
is filed to route this through `gemm_parallel` once the 2×2-pivot
bookkeeping is stable (the (k, k+1) factor block makes the trailing
update non-trivial vs LU/Cholesky single-column).

**Symmetric row+column swap helper** (`swap_sym`): swap rows k1↔k2 AND
columns k1↔k2 in the lower triangle of a packed n×n matrix. Three
phases: left-of-min, between-the-indices (with off-diagonal pair
swap), below-max.

**Tests**: 7 cases / 132 assertions PASS.
- 2×2 indefinite textbook (A = [[1,2],[2,1]] → eigenvalues 3, -1).
- 4×4 indefinite mixed-sign solve.
- N=16 random indefinite (`build_symmetric_indefinite`).
- N=64 random indefinite.
- f32 N=32.
- Singular detection (zero matrix).
- SPD matrix correctness — confirms only 1×1 pivots used.

**Verification**: win-debug + win-asan + win-tidy all PASS.

## v0e-d — Householder QR

`A = Q · R` where Q is m×m orthogonal, R is m×n upper-triangular.
Q stored implicitly via Householder reflectors `H_k = I - τ_k · v_k · v_kᵀ`.

**Algorithm** (LAPACK xGEQRF / xLARFG pattern, unblocked):
```
for k = 0..min(m,n)-1:
  xnorm_sq = Σ_{i>k} A[i, k]²
  if xnorm_sq == 0: τ_k = 0; continue
  β = -sign(A[k,k]) · √(A[k,k]² + xnorm_sq)
  τ_k = (β - A[k,k]) / β
  inv_diff = 1 / (A[k,k] - β)
  A[i, k] *= inv_diff  for i > k          # v subvec, v[0] = 1 implicit
  A[k, k] = β                              # R[k, k]
  for j = k+1..n-1:                        # apply H_k to trailing block
    w = A[k, j] + Σ_{i>k} A[i, k] · A[i, j]
    w *= τ_k
    A[k, j] -= w
    A[i, j] -= w · A[i, k] for i > k
```

**Storage**:
- Upper triangle (incl. diagonal) of `packed()` holds R.
- Strict lower triangle of column k holds `v_k[k+1:m]` (the "implicit"
  Householder vector elements; `v_k[k] = 1` is not stored).
- `taus()` is a length-`min(m,n)` array of τ_k values.

**API**: `factor_qr`, `apply_q_transpose` (forward replay for LS-style
`Qᵀ · b`), `apply_q` (reverse replay), `solve_qr` (square + least-
squares via `Qᵀ·b → c → R⁻¹·c[0:n]`).

**Tests**: 7 cases / 164 assertions PASS.
- 2×2 textbook (R[0,0] = -√2 due to sign convention).
- Square solve N=4 → x_true to 1e-11.
- Q·R reconstruction N=8 to 1e-10 (materializes Q via `apply_q` on identity).
- Orthogonality QᵀQ == I to 1e-12 at N=8.
- Over-determined LS 6×3 (Vandermonde) → exact recovery to 1e-10.
- f32 square N=16.
- `apply_q ∘ apply_q_transpose == identity` to 1e-12.

**Verification**: win-debug + win-asan + win-tidy all PASS.

**Filed follow-ons**:
- `v0e-d-blocked` — WY-representation blocked QR (LAPACK xGEQRT) with
  `gemm_parallel` trailing update. Required for large-N perf parity
  with Eigen HouseholderQR.
- `v0e-d-colpiv` — rank-revealing column-pivoting QR. Required for
  rank-deficient + LS-with-pivot selection.

## Combined slice metrics

| Slice | LOC (lib) | Test cases | Assertions |
|---|---|---|---|
| v0e-c LDLT | ~330 | 7 | 132 |
| v0e-d QR | ~210 | 7 | 164 |
| **Combined this session** | ~540 | 14 | 296 |

**Full hesap-dense suite after v0e-d**: 152 cases / **60,908 assertions**
PASS on win-debug (started this session at 128/41,887; gained
24 cases / 19,021 assertions across v0e-a + v0e-b + v0e-c + v0e-d).

## What's next

v0e-e (LinearOp + Hager condition estimation), v0e-f (iterative
refinement + mixed-precision), v0e-g (CLI + reference-class shootout
vs Eigen), v0e-close (5-config DoD + rollup).
