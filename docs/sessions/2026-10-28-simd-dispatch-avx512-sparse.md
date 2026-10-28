# 2026-10-28 — SIMD-DISPATCH: the sparse zmm bodies

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). HEAD before: `bf700525`.

## CI first

No completed run has a failure left to fix. Complete run 40649898318 (`3e849d96`, in progress) still shows only the
two failures fixed in the last batch (linux-gcc-debug-scalar's download stall, `137b89f1`; win-clang-cl-shipping's
fiber keep race, `6210ab6f`). The push runs after it were cancelled before any job failed. No row is Needs CI that a
finished run can flip: DIAG.11c and DIAG.12 wait on a finished complete-tier run.

## The bodies

The slice heights stay 256 bits of data (`kSellSliceF64` 4, `kSellSliceF32` 8, four complex values), so a ZMM holds
two real slices or one complex f64 slice.

- **Pairs in one ZMM.** A body may declare a `Pair`: `run_pair` then runs the columns both slices store in full in one
  register, slice s in the low half and s + 1 in the high half, and splits it into the two 256-bit sums that finish
  each slice. f64 gathers both slices' eight x with one `vgatherdpd` (or eight scalar loads, the resident pick), f32
  sixteen with one `vgatherdps`, complex f32 multiplies both slices' four complex values at once. Both column forms
  take it: the pair reads whatever the two walks (U32 or Delta16) give.
- **Complex f64.** One slice's four complex values fill a ZMM, so `Avx512C64Body` keeps one register a slice and pairs
  run two of them, as before.
- **addsub.** AVX-512 has none: `a + b` with the even lanes replaced by a masked `a - b`, the same operations.
- **Masked columns.** The 256-bit bodies' masked gathers take an opmask from one unsigned compare, `len > w`, in place
  of the max, compare and widen.

Each lane still adds `vals * x[col]` to its row's sum column by column with two roundings from +0, so avx512 gives
the CSR spmv's bits like every other level. The pick of bodies and pair widths per sweep is avx2's.

## Tests

The `[kernels]` cases already cover pairs of unequal widths, slices alone, both column forms mixed, an infinite or NaN
x[0] with padded lanes, rows past the last slice and every slice range, so they test the pair split as written.

- win-debug: `[kernels]` 100 assertions in 9 cases (portable, sse2, sse4.2, avx2).
- SDE 10.13.1 `-skx`, `CRD_CPU_LEVEL=avx512`: 120 assertions in 9 cases (21 s).
- Hosted: the win-release and linux-gcc-release SDE arm `crd-hesap-sparse-kernels-at-avx512-sde-skx`.

## Board candidates (row close, on AVX-512 hardware)

- The ZMM pair against two YMM sets (avx2's pair) per sweep and type, and its pair width.
- One 16-lane f64 pair with scalar loads in L2 against `vgatherdpd`.
- The opmask gather against the vector-mask gather on the tail columns.
