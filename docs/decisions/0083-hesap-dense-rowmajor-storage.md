# ADR-0083 — hesap-dense row-major storage (with per-factor escape hatch)

- **Status**: Accepted (2026-05-20)
- **Phase**: 3.1.6 `crd-hesap` v0e (dense direct solvers)
- **Tags**: arch, hesap, dense, storage, layout, perf, rowmajor

## Context

`crd-hesap-dense` stores all matrix types (`Matrix<T, Layout>`,
`Symmetric`, `Triangular`, `Banded`, factor objects) **row-major by
default** (`Layout::RowMajor`, pinned in v0a per D21). During the v0e
reference-class shootout vs Eigen-MT (i9-14900K, AVX2), small-N dense
**factorizations** (Cholesky/LU/QR/LDLT, N ≤ 256) measured ~0.5–0.94×
of Eigen. Deep investigation (read Eigen's `LLT.h:332` +
`GeneralMatrixVector.h:156`, ran 3 controlled experiments — see
`memory/project_cholesky_smalln_rowmajor_limit`) established the cause:

- Dense factorizations eliminate **column by column**, so their hot
  update (`A21 -= A20·A10ᵀ`) is column-oriented. Column-major (LAPACK /
  Eigen / MKL / cuSOLVER convention) makes that operand contiguous for
  free; row-major makes it a horizontal-sum-bound row-dot or a strided
  access.
- This is a **data-layout fit** issue, not a kernel-quality gap.
  Reproducing Eigen's column-major register-tiled gemv in a column-major
  scratch *still* lost at small N (copy overhead + worse small-matrix
  cache behavior + MSVC tightly compiling the simple per-row dot).

The question raised: is row-major a decision that will age badly?

## Decision

**Keep hesap-dense row-major as the public default**, for these reasons:

1. **Modern array/ML alignment.** NumPy, PyTorch, JAX, TensorFlow are
   all row-major (C-order). Column-major is the Fortran/LAPACK legacy.
   Cerid's direction (agent-native CLI/RPC per ADR-0081, tensor +
   autodiff in Phase 3.1.6 v14-15, notebook/REPL surface) is ML-aligned —
   row-major is the natural fit and the least-surprising layout for the
   ecosystem hesap plugs into.
2. **GEMM is layout-neutral** (packing normalizes it) — Cerid beats
   Eigen-MT at GEMM regardless, and large-N factorizations are
   GEMM/syrk-dominated, so they WIN (Cholesky 1.41× at N=1024).
3. **GEMV is naturally row-major** (`y = A·x` row dots) — Cerid beats
   Eigen on gemv; column-major would make the common matvec worse.
4. **Sparse (v1+) is independent** (CSR/CSC chosen per-format).
5. **Iterative solvers (v4) are matrix-free / GEMV-heavy** — row-major
   friendly.

**Escape hatch (the insurance):** factor objects (`LU`/`Cholesky`/`QR`/
`LDLT`) are **opaque** — consumers call `solve()`, never touch the raw
factor layout. So an individual factorization is free to store its
*internal* factor buffer column-major if a future hot-loop consumer
proves it measurably matters, WITHOUT changing the row-major public
matrix catalog. We do not build this now; we keep the option.

## Consequences

**Accepted cost:** small-N dense factorizations (N ≤ ~256) trail
column-major LAPACK/Eigen by up to ~1.4× — a bounded, niche penalty.
Large-N factorizations win (GEMM-dominated). The cost recurs for each
new dense factorization (SVD, eig in v3/v6) at small N.

**Mitigations available if a consumer demands it (none built now):**
- Per-factor column-major internal storage (the escape hatch above).
- Batched / fixed-size kernels for hot loops of tiny dense solves
  (e.g. per-contact 6×6 in physics) — a different technique where layout
  matters less. This is the *right* answer for that workload, not a
  global layout flip.

## Revisit gate

Reconsider ONLY if a real consumer slice does **thousands of small
dense factorizations in a hot loop** AND the ~1.4× is a measured
system-level bottleneck AND batched/fixed-size kernels (the preferred
fix) are insufficient. Until then this is settled; do not re-litigate.

## References

- `memory/project_cholesky_smalln_rowmajor_limit.md` — the 3-experiment
  evidence + Eigen source line numbers.
- `docs/sessions/2026-05-20-hesap-v0e-perf-attack.md` — the shootout.
- ADR-0082 — microkernel intrinsics strategy (asm deferred; same
  general-substrate-scope reasoning applies here).
- ADR-0081 — agent-native direction (ML/notebook alignment motivation).
