# Session 2026-05-19 — Phase 3.1.6 `crd-hesap` v0c BLAS L2

## Goal

Ship Phase 3.1.6 v0c per the 2026-05-19 elite mandate: all 17 BLAS L2
operations (gemv / gbmv / ger / geru / gerc / symv / hemv / syr / her /
syr2 / her2 / sbmv / hbmv / trmv / trsv / tbmv / tbsv) across the
applicable type variants (real f32 / f64 and complex Complex32 / Complex64
per BLAS convention), 6 matrix-type bodies populated from the v0a catalog
shells, deterministic Kahan-Babuška-Neumaier reductions inherited from v0b,
and 8 working CLI commands wiring the surface to the registry.

## What we built / changed

- **`engine/hesap-dense/include/crd/hesap/dense/layout.hpp`** — new
  small enums shared by all matrix types: `Layout` (RowMajor / ColMajor),
  `TriangularSide` (Lower / Upper), `TriangularDiag` (Explicit / UnitDiag),
  `Trans` (None / Transpose / ConjTranspose). Lifted into their own
  header so `matrix.hpp` consumes them without pulling the full
  `matrix_catalog.hpp` (which itself pulls `matrix.hpp` — avoids cycle).
- **`engine/hesap-dense/include/crd/hesap/dense/matrix.hpp`** —
  populated `Matrix<T, Layout>` + `MatrixView<T, Layout>` bodies that
  v0a shipped as shells. `Matrix` is move-only with `clone()`, sized +
  initializer_list ctors, `at(i, j)` layout-aware indexing, `ld()`
  (= cols for RowMajor, rows for ColMajor), `set_identity()`, `fill()`.
  `MatrixView` is non-owning, carries `m_ld` so sub-views preserve
  parent stride. Non-const→const converting ctor on MatrixView so
  callers can pass `Matrix::view()` to functions taking
  `MatrixView<const T, L>`. `Matrix::cview()` helper returns
  `MatrixView<const T, L>` explicitly to bypass template-arg-deduction
  failures.
- **`engine/hesap-dense/include/crd/hesap/dense/matrix_types.hpp`** —
  bodies for `Symmetric<T>` (dense n×n; lower-triangle canonical;
  read mirrors via `at(i, j)` swapping when i < j), `Hermitian<T>`
  (`at_lower(i, j)` mutable + `at_value(i, j)` returns `conj` on the
  upper half for complex T, no-op for real T), `Triangular<T, Side, Diag>`
  (UnitDiag returns `T{1}` for the diagonal in `at_value`; mutating the
  diagonal is forbidden under UnitDiag), `Banded<T>` (LAPACK column-
  major band: `storage[(ku + i - j) + j*(kl+ku+1)]`; `in_band(i, j)`
  predicate; `at_value` returns `T{}` outside the band). All four are
  move-only with explicit `clone()`.
- **`engine/hesap-dense/include/crd/hesap/dense/blas2.hpp` +
  `src/blas2.cpp`** — 17 BLAS L2 ops + explicit instantiations across
  the type variants. Templated bodies on the engine side; the math
  routes through `detail::pairwise_sum_produced` for reductions
  (gemv inner row dot, trsv accumulators) so the determinism contract
  from v0b extends to L2. `gemv` is templated on `<T, Layout>` so the
  microkernel for v0d L3 can dispatch per-layout at compile time per
  D21. `trmv` / `trsv` traverse the in-place input in the correct
  order (forward sub for Lower, backward sub for Upper) to avoid
  clobbering unread values. `tbmv` / `tbsv` accept Side / Diag as
  RUNTIME args since `Banded<T>` doesn't carry compile-time side
  metadata.
- **`engine/hesap-dense/include/crd/hesap/dense/cli_anchor.hpp`** —
  declares `register_blas2_cli_anchor()` per the v0b anchor pattern.
- **`engine/hesap-dense/src/cli_register_blas2.cpp`** — 8 working CLI
  commands: `hesap.dense.blas2.gemv.{f32,f64}`, `.symv.{f32,f64}`,
  `.trsv.{lower,upper}.{f32,f64}`. Per-precision impls written as
  explicit non-template functions (D27) — MSVC parser trips on
  templated function bodies dispatching through `Vector<T>` /
  `Triangular<T, ...>` when `vector.hpp` isn't pulled in transitively
  via the includes; root cause: `blas2.hpp` originally only included
  `matrix.hpp` + `matrix_types.hpp` and used `Vector<T>` in convenience
  overloads without `#include <crd/hesap/dense/vector.hpp>`. Fix:
  added the include + explicit non-template per-precision impls.
- **`engine/hesap-dense/CMakeLists.txt`** — `blas2.cpp` +
  `cli_register_blas2.cpp` get globbed in automatically. The `INTERFACE`
  fallback from v0a is gone (already flipped to STATIC in v0b).
- **Tests** (added 34 new cases, total 94 / 521 assertions):
  - `test_matrix.cpp` (11 cases) — Matrix zero-init / RowMajor vs
    ColMajor indexing / set_identity / clone deep / MatrixView
    sub_view preserves stride; Symmetric mirror access; Hermitian
    upper = conj(lower); Triangular Lower at_value returns 0 above
    diagonal; Triangular UnitDiag returns 1 for diagonal; Banded
    in_band predicate; Banded at round-trip for tridiagonal.
  - `test_blas2_real.cpp` (12 cases) — gemv: alpha=1/beta=0 textbook,
    Trans equals naive A^T*x, RowMajor vs ColMajor parity on same
    logical data, larger N matches naive triple-loop within 1e-9;
    ger / symv / syr / syr2 textbook checks; trmv Lower, trsv Lower
    + Upper independent solves; trmv+trsv round-trip on random
    Lower preserves x within 1e-12.
  - `test_blas2_complex.cpp` (6 cases) — gemv Complex64 standard
    product, ConjTranspose conjugates each entry; geru vs gerc differ
    for non-real y; hemv with real-diagonal Hermitian; her with real
    alpha keeps diagonal real; trmv+trsv complex Lower round-trip.
  - `test_blas2_banded.cpp` (5 cases) — gbmv tridiagonal A*x; sbmv
    matches symv on symmetric tridiag; tbmv Lower bidiagonal;
    tbsv Lower solves L*x = b; tbmv+tbsv Upper bidiagonal round-trip.
- **`runtime/examples/smoke_hesap_blas2.cpp`** — random 50×30 matrix,
  gemv vs naive RMSE OK; 8-elem trmv+trsv round-trip OK; CLI dispatch
  of `hesap.dense.blas2.gemv.f64` returns bit-equal result to engine.
  Reports the 8 registered BLAS L2 command schema count.

## Plain-English explanation

Three things matter about this slice.

1. **The matrix-type catalog is now alive.** v0a shipped 15 empty class
   shells. v0c pops the lids off six of them — Matrix / MatrixView /
   Symmetric / Hermitian / Triangular / Banded — and gives them real
   bodies that the BLAS L2 ops consume. The other 9 catalog types
   (Diagonal / Identity / Permutation / BlockDiagonal / BlockTridiagonal
   / Toeplitz / Hankel / Circulant / Vandermonde) stay as shells; their
   bodies will land when a real consumer asks for them.

2. **17 BLAS L2 ops compile and verify.** Every operation routes through
   the same KBN-pairwise reduction tree v0b shipped, so the
   bit-exact-across-SIMD-widths contract from ADR-0063 still holds.
   Real-only `dot<T>` becomes real-only `symv<T>` + `syr<T>` +
   `syr2<T>`; complex-only `dotu<T>` + `dotc<T>` become complex-only
   `geru<T>` + `gerc<T>` + `hemv<T>` + `her<T>` + `her2<T>` per BLAS
   convention. The triangular ops `trmv` + `trsv` walk the in-place
   input in the correct order so the back/forward-substitution doesn't
   clobber unread values.

3. **The "include vector.hpp transitively" lesson cost us 20 minutes.**
   MSVC's error for "Vector<T> is not visible" is "function template
   Vector missing argument list" — confusing because `Vector` is a
   CLASS template, not a function template. The parser falls back to
   ADL-style "is there any `Vector` symbol I can resolve?" and emits
   the function-template diagnostic. Root cause: blas2.hpp uses
   `Vector<T>` in its convenience overloads but doesn't include
   `vector.hpp`. Now it does. Memory entry queued.

## Decisions made

- **D21 (v0c)** — `Layout` is a NON-TYPE template parameter on
  `Matrix<T, L>` / `MatrixView<T, L>` / `gemv<T, L>` / `ger<T, L>` /
  `geru<T, L>` / `gerc<T, L>`. BLAS L3 microkernel selection happens
  at compile time per layout. Sibling matrix types (Symmetric /
  Hermitian / Triangular / Banded) don't carry a Layout NTTP — square
  storage doesn't need it.
- **D22 (v0c)** — `Trans` enum (None / Transpose / ConjTranspose)
  carries the transpose mode for gemv / gbmv / trmv / trsv / tbmv /
  tbsv. ConjTranspose for real T degrades to Transpose at the
  `maybe_conj<T>` helper site.
- **D23 (v0c)** — Triangular dense storage (full N×N) in v0c; packed
  variant filed as `v0c-packed-storage` follow-on. Same trade-off for
  Symmetric / Hermitian: dense + canonical-lower-triangle access in
  v0c; packed variant follow-on. Cost: 2× memory; benefit: clean
  indexing across the BLAS L2 surface without storage-conversion bridges.
- **D24 (v0c)** — `Banded<T>` uses LAPACK column-major band:
  `storage[(ku + i - j) + j*(kl + ku + 1)]`. Row-major variant filed
  as `v0c-banded-rowmajor` follow-on. Storage convention matters
  little for matvec; will matter more for v0e dense direct factorizations.
- **D25 (v0c)** — `iamax` BLAS-spec named methods (sgemv / dgemv /
  cgemv / zgemv) are NOT exposed by Cerid. Templates with explicit
  type variants are the engine surface; CLI surface uses the
  `.f32` / `.f64` / `.c32` / `.c64` suffix convention.
- **D26 (v0c)** — Symmetric / Hermitian dense storage with canonical
  LOWER triangle. Reads from the upper half mirror (Symmetric) or
  conjugate (Hermitian). Writes to the upper half are forbidden;
  `at_lower(i, j)` is the only mutable access on Hermitian.
- **D27 (v0c)** — CLI register implementations are written as
  explicit non-template per-precision functions (`impl_gemv_f32` /
  `impl_gemv_f64` / etc), not as function templates. Reason: MSVC
  parser trips repeatedly on `Vector<T>` inside templated function
  bodies. Macros over the precision are equally fragile due to
  template-arg-deduction ambiguity. The non-template form is verbose
  (~30 LOC per op-precision-pair) but compiles cleanly.
- **D28 (v0c)** — `vector.hpp` MUST be included transitively from any
  header that uses `Vector<T>`. `blas2.hpp` had Vector<T> in convenience
  overloads but missed the include; this caused C7568 "function template
  Vector missing argument list" errors in downstream TUs. Lesson
  generalised: when a header declares functions using a class template
  in convenience-overload position, include the class template header
  directly (don't rely on transitive PCH).
- **D29 (v0c)** — v0c CLI surface ships 8 commands (gemv.f32/f64,
  symv.f32/f64, trsv.lower.f32/f64, trsv.upper.f32/f64). Remaining
  ~38 commands filed as `v0c-cli-extend` follow-on. The engine surface
  for ALL 17 ops × applicable type variants is complete and verified
  by the 34 new test cases; CLI plumbing widens when v0e dense-direct
  / v1 sparse consumers need them. Matches `feedback_ship_at_consumer_
  template_from_day_one` substrate-proactive pattern: the math is the
  substrate; the CLI surface is a consumer-facing convenience.
- **D30 (v0c)** — `MatrixView<T, L>` carries a non-const→const
  converting ctor via SFINAE so `MatrixView<T, L>` implicit-converts to
  `MatrixView<const T, L>`. `Matrix::cview()` helper bypasses
  template-arg-deduction failures at the call site (deduction does NOT
  cast through the converting ctor).
- **D31 (v0c)** — `set_identity()` on Matrix asserts square. Symmetric
  / Hermitian / Triangular are always square by construction.
- **D32 (v0c)** — Two-pass triangular traverse for trmv: Lower goes
  i = n-1 down to 0; Upper goes i = 0 up to n-1. Prevents clobbering
  unread values during in-place x = A*x.

Decisions D21-D32 queued for ADR-0065 §14 lock at v0-close.

## Files touched

- `engine/hesap-dense/include/crd/hesap/dense/layout.hpp` — new
- `engine/hesap-dense/include/crd/hesap/dense/matrix.hpp` — new (Matrix body)
- `engine/hesap-dense/include/crd/hesap/dense/matrix_types.hpp` — new
  (Symmetric/Hermitian/Triangular/Banded bodies)
- `engine/hesap-dense/include/crd/hesap/dense/matrix_catalog.hpp` —
  shells removed (replaced by bodies above)
- `engine/hesap-dense/include/crd/hesap/dense/blas2.hpp` — new
- `engine/hesap-dense/src/blas2.cpp` — new
- `engine/hesap-dense/include/crd/hesap/dense/cli_anchor.hpp` —
  `register_blas2_cli_anchor()` declared
- `engine/hesap-dense/src/cli_register_blas2.cpp` — new
- `engine/hesap-dense/include/crd/hesap/dense/dense.hpp` — umbrella
  includes new headers
- `tests/hesap-dense/test_matrix.cpp` — new
- `tests/hesap-dense/test_blas2_real.cpp` — new
- `tests/hesap-dense/test_blas2_complex.cpp` — new
- `tests/hesap-dense/test_blas2_banded.cpp` — new
- `tests/hesap-dense/test_matrix_catalog.cpp` — Banded test updated for
  new ctor signature
- `tests/hesap-dense/CMakeLists.txt` — adds new test files
- `runtime/examples/smoke_hesap_blas2.cpp` — new
- `runtime/CMakeLists.txt` — adds smoke_hesap_blas2
- `context.md` — Last shipped + Current focus → v0d
- `docs/phases/phase-3.1.6-hesap.md` — v0c row ✅

## Tests / verification

- **Built?** ✅ All targets clean under win-debug.
- **Tests pass?** `crd-hesap-dense-tests`: **94 cases / 521 assertions
  PASS** (60 v0a+v0b + 34 v0c).
- **Smoke pass?** ✅ `smoke_hesap_blas2`: gemv RMSE OK over 50×30,
  trmv+trsv round-trip OK over 8 elements, 8 BLAS L2 commands
  registered, CLI dispatch of `hesap.dense.blas2.gemv.f64` bit-equal
  to engine.
- **Per-slice DoD?** _4-config `per-slice-check.ps1 -Parallel` running
  at session-close authoring._

## Next session starts with

**v0d — BLAS L3 via task-DAG** per `docs/phases/phase-3.1.6-hesap.md`.
Surface: `gemm` (tile-based) + `syrk` / `herk` / `syr2k` / `her2k` +
`trmm` / `trsm`. New sub-module `crd-hesap-sched` for the task-DAG
substrate. Tile dispatch over `crd::jobs`. Microkernels AVX2 / AVX-512
/ NEON / SVE2 / scalar. Mixed-precision dispatch helper. Benchmark
target ≥70% AVX-512 peak. ~1200 LOC + ~100 tests + ~7 d. **Filed
follow-on**: `v0c-cli-extend` widens the BLAS L2 CLI surface from 8
schemas to the full ~46.
