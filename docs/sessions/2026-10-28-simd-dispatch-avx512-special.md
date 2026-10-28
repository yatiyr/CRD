# 2026-10-28 — SIMD-DISPATCH: the special zmm body

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). HEAD before: `7e230e22`. CI read for this batch:
[sparse zmm bodies](2026-10-28-simd-dispatch-avx512-sparse.md#ci-first).

## The body

The Lanczos lgamma and tgamma bodies are now written once over a register width (`Lanes8`, `Lanes4`, `Lanes2`), which
replaces the level's single set of free functions. At avx512 the loop runs eight lanes at a time with `crd_log8` and
`crd_exp8`, then at most one four-lane register with avx2's twins, then the scalar loop. So a call of four to seven
values still runs vector code. The half-plane test is two opmask compares. Lanes outside the half-plane take the
scalar functions, as on every level, and every lane gives `detail::lanczos_lgamma`'s and `lanczos_tgamma`'s bits.

## Tests

The existing case puts the edge inputs (reflection, poles, ±0, subnormals, ±inf, NaN, the half-plane bounds) seven
apart, which lands them in every lane of 2, 4 and 8, at offsets 0 to 3 and every length to 41.

- win-debug: `[kernels]` 29 assertions (portable, sse2, sse4.2, avx2).
- SDE 10.13.1 `-skx`, `CRD_CPU_LEVEL=avx512`: 32 assertions, avx512 included.
- `philox_blocks.hpp` (the stats batch) is tidy-clean on its own. hesap-tensor's avx512 unit runs the new Philox
  groups only through `philox4x32_fill_blocks`, which the stats case checks bit for bit at every count to 79; the
  hosted `crd-hesap-tensor-kernels-at-avx512-sde-skx` arm is its own evidence.

## Board candidate (row close, on AVX-512 hardware)

- The eight-lane step against avx2's four, with the scalar abnormal lanes per register.
