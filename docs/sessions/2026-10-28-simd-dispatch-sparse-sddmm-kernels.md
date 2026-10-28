# 2026-10-28 — SIMD-DISPATCH: the SDDMM dots move into the sparse kernels

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). HEAD before: `a6a594ff`.

## CI first

No completed run among the last 15 has a failed job: all are push runs the concurrency group cancelled before any job
ran. Push run 40701630725 (`3c1debc6`) is still running; its six failed jobs have causes fixed at HEAD, as the
[batched note](2026-10-28-simd-dispatch-tensor-batched-kernels.md#ci-first) records. No new green run, so DIAG.11c and
DIAG.12 stay Needs CI.

## What moved

`sddmm.hpp` no longer includes a SIMD header. The f64 and f32 dots are two entries appended to `SparseKernels`
(`sddmm_rows_f64`, `sddmm_rows_f32`, over the data-only `SddmmRows`), with their bodies in the existing sparse kernel
unit; `sddmm` and `sddmm_parallel` make one table call per row range. The complex dot has no lane type (four scalar
partial sums of `Complex<T>`) and stays in general code, where it compiles the same at the floor.

The bits do not change at any level: every level runs the one accumulator shape `SddmmRows` declares (f64 two
`Vec4d` sums over stripes of 8, then stripes of 4; f32 one `Vec8f` sum), and `horizontal_sum` folds through memory
pairwise on every backend. The move keeps the AVX2 body once general code builds at SSE2. At avx512 the f32 sum must
not widen (a `Vec16f` would split the stripes); a `Vec8d` f64 form that splits its halves before the fold gives the
same bits and is a row-close board candidate, beside `runtime/examples/bench_hesap_sddmm_vs_reference.cpp`.

In passing: `kMaxJobs` in `sddmm.hpp`, `bsr.hpp`, `dia.hpp` and `ell.hpp` and `kMaxDynB` in `bsr.hpp` are local
constants, which the naming rule spells lower case; strict tidy flagged the one in `sddmm.hpp`.

## Tests

- `test_sparse_kernels.cpp` gains `[kernels]` cases: every level the host runs against a scalar oracle that replays
  the declared order, at r 0 to 17, 31, 32, 33, 64 and 70, two alphas, three row ranges with guards on every slot
  outside the range and past the last, empty rows, NaN padding past r in X and Y, and a signed zero, a subnormal,
  infinities, a NaN and the largest finite value in stripes and tails; `sddmm` and `sddmm_parallel` give the oracle's
  bits on a `TripletBuilder` mask. The archive case checks both entries.
- `crd-hesap-sparse-kernels-at-<level>` now runs `[kernels],[sddmm]` under each forced level and under SDE.
- `include-budget.json` holds the `crd/hesap/sparse/sparse.hpp` umbrella with the SSE2 intrinsics headers forbidden.

Local: win-debug native (portable, sse2, avx2) and SDE 10.13.1 `-skx` at avx512 pass `[kernels],[sddmm]`; the
crd-hesap-sparse suite, `crd-check-include-budget` and strict tidy on the changed files pass.

## Next

The jets need a per-level evaluation boundary (ADR-0134 §5), then the flip.
