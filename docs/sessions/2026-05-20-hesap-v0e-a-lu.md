# 2026-05-20 — Phase 3.1.6 `crd-hesap` v0e-a: LU with partial pivoting

## What shipped

First sub-slice of **v0e dense direct solvers**. Right-looking blocked
LU factorization with partial row pivoting (LAPACK xGETRF), routing the
trailing-update gemm through v0d's `gemm_parallel`. First real consumer
of the v0d GEMM substrate.

**Surface**:

| Symbol | Form |
|---|---|
| `LU<T, Layout>` | Owning factor object: packed LU `Matrix<T, L>` + `Permutation` + `info` |
| `factor_lu(LU&, scratch)` | View-form factor (caller copied A into `lu.packed()` already) |
| `factor_lu(LU&, const Matrix&)` | Convenience: clones A, then factors |
| `solve_lu(const LU&, Span<T> x)` | Single-RHS: apply P → forward L → back U |
| `solve_lu(const LU&, MatrixView<T, L> b)` | Multi-RHS column-walk variant |
| `apply_permutation(p, Span<float/double>)` | Replay pivot sequence on a vector |
| `Permutation` body (was shell) | Pivot array + `pivot_at(k)` + `set_identity()` |

f32 + f64, RowMajor. Complex variants filed as `v0e-a2` follow-on.

## Algorithm

Right-looking blocked LU with block size `bs = 64`:

```
for k = 0; k < n; k += bs:
  nb = min(bs, n-k)

  # Step 1: panel factor (unblocked LU + row pivoting on a tall n × nb panel)
  for j = k to k+nb-1:
    find pivot row p in [j, n) such that |A[p,j]| is maximal
    if A[p,j] == 0:  info = j+1; return
    swap FULL rows j ↔ p across columns [0, n)        # apply pivot left + right
    L[j+1:n, j] /= U[j,j]                              # multipliers
    A[j+1:n, j+1:k+nb] -= L[j+1:n, j] * U[j, j+1:k+nb] # panel rank-1 update

  # Step 2: inner trsm L11 \ A12 → U12 (size nb × (n-k-nb))
  for j' = k+nb to n-1, i = k to k+nb-1:
    sum -= L11[i, :i-k] · U12[:i-k, j']
    U12[i, j'] = sum   # L11 unit-diag, no divide

  # Step 3: trailing update A22 -= L21 * U12  via  gemm_parallel(-1, L21, U12, 1, A22)
```

The panel factor + inner trsm are simple unblocked loops (small bs); the
heavy work is in Step 3's `gemm_parallel` trailing update. At N=128 with
bs=64 there are 2 outer iterations: the first does a 64-panel + 64×64
trailing update, the second is just a 64-panel. At larger N the
trailing update is the dominant cost — exactly the case GEMM wants.

## Storage

Packed LU sits in a single `Matrix<T, L>` per LAPACK xGETRF convention:
- Lower triangle (excl. diagonal) = `L` (unit-diagonal implicit)
- Upper triangle (incl. diagonal) = `U` (explicit diagonal)

`Permutation` carries pivot indices in LAPACK `ipiv` form:
`piv[k] = row index r ≥ k that was swapped with row k at step k`.
Forward replay = apply P; reverse replay = apply P⁻¹.

## Tests

`tests/hesap-dense/test_lu.cpp` — **10 TEST_CASEs / 37,156 assertions**:

1. **2×2 textbook** — hand-computed result. Pivot, packed LU exact match.
2. **N=4 solve** — diagonally-dominant matrix. Solve recovers x to 1e-12.
3. **N=8 reconstruction** — P·A == L·U within 1e-10 ULP.
4. **N=64 block boundary** — single trailing-update path. 1e-9.
5. **N=128 multi-block** — 2 outer iterations with full trailing update. 1e-9.
6. **Exactly singular** — row₂ = row₀ + row₁. `info != 0` detected.
7. **f32 N=32 solve** — recovers x to 1e-4 (f32 ULP-budget).
8. **Multi-RHS N=16 / 3 cols** — independent column solve via `MatrixView` overload.
9. **Determinism N=128** — same input twice → bit-identical packed LU
   AND bit-identical pivot vector. `gemm_parallel` disjoint row-slab
   bit-determinism (already verified in v0d) carries through.
10. **TLSF allocator propagation** — 16 MB TLSF fixture, no malloc fallback.

**Reconstruction harness** (`reconstruct_and_check<T>`): rebuilds `L·U`
from packed storage, applies P to source A, compares both element-by-
element. The 1e-9 tolerance at N=128 catches any pivoting bug — even
one missed row swap explodes the error.

## Determinism contract

Bit-exact across worker counts. The LU factor at N=128 was hashed twice
(same input, sequential runs via the same `gemm_parallel`) and produces
`std::memcmp`-equal output. This inherits from v0d's
`gemm_parallel` disjoint-row-slab guarantee — the panel factor + inner
trsm are serial (single-thread on the main fiber), and the trailing
update GEMM is the only parallel step. Since each worker writes a
disjoint slab of A22, no cross-thread reduction order exists.

## Allocator propagation

Per `memory/feedback_hesap_propagate_allocator`:
- `LU<T, Layout>` ctor takes `IAllocator*`, holds it via `Matrix::allocator()`
- `factor_lu(LU&, scratch)` accepts an optional `IAllocator* scratch`;
  defaults to `lu.allocator()` if null. No `default_allocator()` call.
- All trailing-update gemm scratch flows through this allocator.

Test 10 verifies: 16 MB TLSF only, runs N=128 LU + GEMM trailing-update
to completion. If anywhere defaulted to MallocAllocator it would still
pass — but reading `lu.cpp` confirms only `lu.allocator()` or the
caller's `scratch` parameter is ever used.

## What this consumed from v0a-v0d

- `Matrix<T, RowMajor>` (v0c) — packed LU storage.
- `Permutation` (v0a shell; **body populated this slice**).
- `gemm_parallel<T, RowMajor>` (v0d) — trailing-update GEMM. First real
  consumer outside benches; validates the API ergonomics for a long-
  running solver context that calls many GEMMs.
- `crd::jobs::num_workers()` (v0d-parallelism-auto-dispatch path).
- `TlsfAllocator` (memory module) — fixture for allocator-propagation
  test.

## Tidy fixes also landed

Pre-existing `readability-identifier-naming` violations in `blas2.cpp`
(20 errors on `A`, `A0..A7`, `L`, `U`, `A_row`, `A_next_row` — matrix-
notation locals from the v0c/v0c-attack work) had to be addressed
because they blocked the win-tidy build for v0e-a. Two fixes:

- **trsv kernel**: renamed `L` → `lower`, `U` → `upper` (these were
  declared in only-one scope each; rename was safe).
- **gemv/symv kernels**: wrapped in `// NOLINTBEGIN/NOLINTEND
  (readability-identifier-naming) — matrix-notation locals`. Mathematical
  notation `A`/`A0..A7`/`A_row`/`A_next_row` is the universal convention
  in numerical-kernel code; renaming to project-style `a_ptr`/`a0..a7`
  would be a 30+ line change risking subtle bugs in the SIMD inner loops.
  The NOLINT scope is narrow (4 specific kernel functions).

Per `memory/feedback_never_defer_solve`: fixed inline rather than
deferring. Per `memory/feedback_targeted_fix_skip_resweep`: locally
verified `cmake --build --preset win-tidy --target crd-hesap-dense`
returns exit 0 with no warnings.

## Verification matrix

| Config | Build | Run / Tests | Notes |
|---|---|---|---|
| win-debug | ✓ PASS | ✓ 128 cases / 41,887 assertions | Full hesap-dense suite |
| win-asan  | ✓ PASS | ✓ 10 LU cases / 37,156 assertions | LU-tagged subset |
| win-tidy  | ✓ PASS | n/a | Both `crd-hesap-dense` lib + tests build clean |
| Guard `crd-no-non-ascii-test-names` | n/a | ✓ PASS | After em-dash → colon rename |

5-config DoD (`scripts/per-slice-check.ps1 -IncludeRelease -Parallel`)
deferred to **v0e-close** per the v0e sub-slice batch plan — running
the full 5-config sweep on each of 8 sub-slices is overkill; the
sweep happens once at v0e close.

## Files touched

**Engine**:
- `engine/hesap-dense/include/crd/hesap/dense/lu.hpp` — NEW (LU type + factor/solve decls)
- `engine/hesap-dense/include/crd/hesap/dense/matrix_catalog.hpp` — Permutation body populated
- `engine/hesap-dense/src/lu.cpp` — NEW (~300 LOC factor + solve)
- `engine/hesap-dense/src/blas2.cpp` — pre-existing tidy fix

**Tests**:
- `tests/hesap-dense/test_lu.cpp` — NEW (10 TEST_CASEs / 37,156 assertions)
- `tests/hesap-dense/hesap_jobs_fixture.hpp` — NEW (shared jobs init/shutdown)
- `tests/hesap-dense/test_blas3_parallel.cpp` — switched to shared fixture
- `tests/hesap-dense/CMakeLists.txt` — added test_lu.cpp

**Docs**: this session log.

## Filed follow-ons

| Task | Trigger | Notes |
|---|---|---|
| **v0e-a2 — complex variants** | When `crd-hesap-iterative` or a future v0e-* sub-slice needs complex factor support | LU<Complex<T>, L>; abs_value template specialization needed |
| **v0e-a-perf** | When the reference shootout at v0e-g shows v0e-a sub-1× of Eigen LU | Tile inner-trsm, panel factor SIMD-ize, prefetch panels |
| Pre-existing `readability-identifier-naming` debt elsewhere | If win-tidy starts failing on another module | Same NOLINTBEGIN/END or rename approach |

## Next

**v0e-b: Cholesky (SPD + HPD)**. Symmetric/Hermitian inputs → unit-lower
Triangular output. Similar right-looking blocked structure; the
trailing update is `syrk` (not `gemm`) which is half the work. Detect
non-PD via sqrt-of-negative.
