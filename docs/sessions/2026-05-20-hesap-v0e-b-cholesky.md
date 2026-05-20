# 2026-05-20 — Phase 3.1.6 `crd-hesap` v0e-b: Cholesky (SPD real)

## What shipped

Right-looking blocked Cholesky factorization for symmetric positive-
definite real matrices. Second sub-slice of v0e dense direct solvers;
shares the right-looking blocked structure with v0e-a (LU) and consumes
the same v0d `gemm_parallel` substrate for the trailing update.

**Surface**:

| Symbol | Form |
|---|---|
| `Cholesky<T, Layout>` | Owning factor: packed lower-triangle Matrix + info |
| `factor_cholesky(Cholesky&, scratch)` | View-form factor (caller pre-copies A's lower triangle) |
| `factor_cholesky(Cholesky&, const Symmetric&)` | Convenience: copy + factor |
| `solve_cholesky(const Cholesky&, Span<T> x)` | Single-RHS: forward-sub L + back-sub Lᵀ |
| `solve_cholesky(const Cholesky&, MatrixView<T, L> b)` | Multi-RHS column-walk |

f32 + f64 RowMajor. HPD complex variant filed as `v0e-b-hpd`.

## Algorithm

For each block of size `bs = 64`:

```
1. Unblocked Cholesky on the L11 diagonal block (size nb × nb):
   for j = 0..nb-1:
     diag = A[k+j, k+j] - Σ_{p<j} L[k+j, k+p]²
     if diag <= 0:  info = k+j+1; return
     L[k+j, k+j] = sqrt(diag)
     for i = j+1..nb-1:
       L[k+i, k+j] = (A[k+i, k+j] - Σ_{p<j} L[k+i, k+p] * L[k+j, k+p]) / L[k+j, k+j]

2. Inner trsm: solve L21 · L11^T = A21 (column-walk in-place).

3. Trailing update A22 -= L21 · L21^T  via  gemm_parallel(-1, L21, L21, 1, A22, None, Transpose).
   Writes both triangles of A22; upper-triangle garbage is never read.
```

`v0e-b-syrk-optim` is filed to replace step 3 with a true `syrk`
(half the FLOPs — exploits the symmetry of L21·L21ᵀ). Requires a
`SymmetricView<T>` non-owning class which doesn't exist yet.

## Storage

LAPACK xPOTRF convention: packed lower-triangle in a single `Matrix<T,
L>`. Upper triangle is GEMM-trailing-update garbage and is never read
by solve. The `factor_cholesky(Cholesky&, const Symmetric&)`
convenience overload zero-fills the upper triangle on the initial copy
so the panel-factor inner loops see deterministic zeros (not
uninitialized memory) during the first iteration — this matters for
ASan + debug builds.

## Tests

`tests/hesap-dense/test_cholesky.cpp` — **10 TEST_CASEs / 18,725 assertions**:

1. **2×2 textbook** — A = [[4,12],[12,37]] → L = [[2,0],[6,1]]. Exact match.
2. **N=4 solve** — diagonally-dominant SPD. Solve recovers x to 1e-12.
3. **N=8 reconstruction** — L·Lᵀ ≈ A within 1e-9.
4. **N=64 single trailing update** — 1e-7.
5. **N=128 multi-block** — 2 outer iterations, full trailing update. 1e-7.
6. **Non-PD detection** — indefinite matrix with negative-discriminant
   diagonal element → `info != 0`.
7. **f32 N=32 solve** — recovers x to 1e-3 (f32 budget).
8. **Multi-RHS N=16 / 3 cols** — independent column solve.
9. **Determinism N=128** — two runs produce bit-identical LOWER
   triangle. Upper triangle is GEMM garbage (verified by checking
   only the canonical half).
10. **TLSF allocator propagation** — 16 MB TLSF, no malloc fallback.

SPD inputs synthesized via `build_spd<T>(a, seed)`: A = BᵀB + n·I for
deterministic seeded B, giving a strongly diagonally-dominant SPD.

## Verification matrix

| Config | Build | Run | Notes |
|---|---|---|---|
| win-debug | ✓ PASS | ✓ 138 cases / 60,612 assertions | Full hesap-dense suite (was 128 / 41,887) |
| win-asan  | ✓ PASS | ✓ 10 Cholesky cases / 18,725 assertions | ASan-clean |
| win-tidy  | ✓ PASS | n/a | No new tidy violations |

## What this consumed from v0a-v0e-a

- `Symmetric<T>` (v0c) — input matrix, lower-triangle canonical storage.
- `Matrix<T, RowMajor>` (v0c) — packed factor output.
- `gemm_parallel<T, RowMajor>` (v0d) — trailing-update GEMM. Second
  consumer (after v0e-a LU).
- `crd::jobs::num_workers()` (v0d-parallelism).
- `TlsfAllocator` — fixture; hesap_jobs_fixture shared with v0e-a + blas3.

## Files touched

**Engine**:
- `engine/hesap-dense/include/crd/hesap/dense/cholesky.hpp` — NEW
- `engine/hesap-dense/src/cholesky.cpp` — NEW (~230 LOC)

**Tests**:
- `tests/hesap-dense/test_cholesky.cpp` — NEW (10 cases)
- `tests/hesap-dense/CMakeLists.txt` — added test_cholesky.cpp

**Docs**: this session log + phase doc updated.

## Filed follow-ons

| Task | Trigger | Notes |
|---|---|---|
| **v0e-b-hpd** | When consumer needs complex Hermitian factor | A = L·Lᴴ; sqrt of real-valued diagonal stays the same, off-diagonal multiplications use conj |
| **v0e-b-syrk-optim** | When v0e-g shootout shows Cholesky sub-1× Eigen LLT | Replace trailing-update gemm_parallel with true syrk via new `SymmetricView<T>` non-owning class |
| **v0e-b-perf** | If reference shootout flags non-trivial gap | Panel-factor SIMD; tile inner-trsm; prefetch |

## Next

**v0e-c: LDLT (Bunch-Kaufman indefinite).** Same general structure but
pivoting needs to handle 2×2 blocks for indefinite cases. Output factor
includes a `BlockDiagonal<T>` D mixing 1×1 + 2×2 blocks.
