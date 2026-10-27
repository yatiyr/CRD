# 2026-10-27 — SIMD-DISPATCH: complex SELL bodies and unread padding

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). Decision: [ADR-0134](../decisions/0134-runtime-cpu-dispatch.md).

## CI read

- No completed run has a new failed job. Push runs 40567336132 back to 40530655387 were cancelled before any job ran;
  40568166569 (`892d4725`) is pending. Complete run 40515770381 (`31e2b9ce`) is still running (win-relwithdebinfo);
  its four failed jobs were fixed by `b306e2ea` and `14840123`. Nothing dispatched (a complete run is in progress).
- Needs CI audit: [DIAG.11c](../ROADMAP.md#slice-diag.11c) and [DIAG.12](../ROADMAP.md#slice-diag.12) stay Needs CI;
  no complete run at or after `14840123` has finished.

## The defect first: padding multiplied x[0]

SELL pads each slice row with the value 0 at column 0, and every level (and the scalar complex path) added
`0 * x[0]` there. An infinite or NaN `x[0]` therefore made every padded row NaN where the CSR spmv, which never reads
a pad, gave a number: "bit-exact with CSR" (D(sparse)-3) was false for non-finite x. Reproduced on win-debug by
setting `x[0]` to +inf and NaN in the [kernels] differential: `counts.wrong` failed at portable, sse2 and avx2.

Fix: `SellMatrix::row_len` (each σ row's stored count, `num_slices * C` long, 0 for pad rows), passed in
`SellSlices`. Each slice runs its columns unmasked up to its shortest row; past it a padded lane reads x from a zero
(the AVX2 f32 body masks its gather), so its product is +0, and a sum from +0 is never -0, so adding it keeps the bits.

## Complex bodies

- The table gains `sell_spmv_c64` and `sell_spmv_c32` over (re, im) pairs: `sell.hpp` passes `Complex<U>` values,
  x and y as `U` pointers (standard layout, `sizeof == 2 sizeof(U)` asserted) and alpha/beta as their parts
  (`SellComplexScales`); no `Complex` object or operator reaches a kernel unit. The scalar complex branch is gone.
- Each step adds `(vr xr - vi xi, vr xi + vi xr)` part by part and the write is `alpha sum + beta y` in the same
  part formulas (beta (0, 0), -0 included, never reads y), so every level gives the CSR bits, NaN payloads aside.
  portable: scalar parts; sse2: one c64 or two c32 lanes a register, `unpack`/`shuffle` duplicates and swaps and the
  re lane adds the sign-flipped `vi xi` (no addsub); avx2: two c64 or four c32 lanes a register, `movedup`/`moveldup`
  plus `addsub`. x loads are one 128-bit load a c64 lane and one 64-bit load a c32 lane. The slice height stays 4 for
  both (`kSellSliceComplex`).
- D(sparse)-4 now reads complex→4 on the per-level kernels and records the unread padding.

Verification (win-debug): `test_sparse_kernels.cpp` runs all four types at every level against CSR bit for bit (any
NaN equal, complex part by part) over 14 sizes, three σ, six slice ranges, five alpha/beta pairs (complex beta (0, 1)
and (-0, 0)) and x[0] finite, +inf and NaN, plus the row lengths `to_sell` records; `test_sell_parallel.cpp` checks
complex `spmv_sell_parallel` at 1, 3 and 7 jobs against CSR. The [kernels] cases pass at portable, sse2 and avx2 and
the whole program passes; `crd-hesap-iterative-tests` builds. Boards: the row-close pass.

## Cache sizes for the SELL policy

The policy's resident size needs the caches, and nothing read them. `cpu_features` (the one CPUID site) now decodes
the data and unified caches of every visited CPU from leaf 4 (Intel) or 0x8000001D (AMD topology extensions), else
the legacy 0x80000005/6 leaves, into `CpuReport::caches` (line, L1d, L2, L3 bytes, source); the selection keeps the
smallest of each over the CPUs (a hybrid part's efficiency cores set L1d). CPUID's sharing word bounds addressable
IDs, not sharers (on this host it gave a 256 KiB L2 share where each core has 1 MiB), so no share is derived.

Verification (win-debug): decode cases for leaf 4, 0x8000001D, the legacy leaves and none, the intersection over a
performance and an efficiency core, and the host against the OS (`RelationCache` on Windows, sysfs `cache/index*` on
Linux: each level's smallest size and L1's line, where the OS reports them). This host: line 64, L1d 32 KiB, L2
2 MiB, L3 36 MiB, as Windows reports; the child prints the caches for the CI logs.

## The SELL pairing and gather policy

- `sell.hpp` gives every kernel call a `SellSweep`: Resident when the range's matrix bytes (`range_bytes`: values,
  columns, row lengths), all of x and its rows of y fit `sparse_resident_bytes()` (the smallest L2, 2 MiB where no
  CPUID leaf describes the caches), else Streaming. Serial calls and each parallel job judge their own range.
- Each level and type picks a body and a pair width per sweep (`SellPolicy`), seeded from the 2026-10-27 board:
  avx2 f64 scalar loads in L2 and `vgatherdpd` beyond it; avx2 f32 `vgatherdps` both; sse2 f32 runs every slice alone
  from DRAM (pairs cost its power-law rows 9%); the rest keep HEAD's forms. Every pick gives the CSR bits.
- Found in passing: a vector gather's index is signed, so a matrix past 2^31 columns would have gathered below x. A
  gathering body now takes its scalar-load form there (`SellSlices::cols`).
- The operators' serial-or-parallel cut (`ParallelSparseLinearOp`, the least-squares and spmm operators) defaults to
  the same `sparse_resident_bytes()` instead of a fixed 2 MiB, judged on `stored_bytes()` (every array) instead of
  the values alone.
- Row-close board candidates: the sse2 f32 pairing trade (uniform-16 1M gains from pairs), complex gather forms, and
  whether a job's cut should be its core's share of L2 rather than the whole.

Verification (win-debug): the [kernels] differential runs every entry at both sweeps and at Streaming with `cols`
past 2^31 (the scalar-load fallback) for all four types at portable, sse2 and avx2, bit for bit with CSR; the byte
counts and the resident cut are checked; the whole sparse program passes; iterative and eigen tests build.

## SELL with 16-bit column deltas

- CI read: no completed run has a new failed job; push runs 40573013280 back to 40539013278 were cancelled with no
  failed job, 40573536615 (`db9692aa`) is pending, complete run 40515770381 is still running (win-relwithdebinfo).
  DIAG.11c and DIAG.12 stay Needs CI.
- Format: a slice stores its columns as u32 or Delta16, each σ row's first column (`col_first`, one u32 a row once any
  slice is Delta16) plus a u16 gap a column (`col_delta`, 0 at w = 0 and in the padding). `delta_ptr` counts the
  Delta16 elements before each slice, so a U32 slice's columns start at `slice_ptr[s] - delta_ptr[s]` and the kind is
  `delta_ptr[s + 1] != delta_ptr[s]`; no array is stored twice.
- `to_sell(..., SellIndex)`: U32, Delta16 (each slice wider than 2 whose gaps fit 16 bits, as u32 differences, so an
  unsorted row stays u32) or Auto (Delta16 when a whole-matrix spmv's bytes exceed `sparse_resident_bytes()`). A
  slice 2 wide or less reads fewer bytes as u32 (4 C first columns plus 2 C a column against 4 C a column).
- Kernels: the driver walks a slice's columns (`U32Columns`, `Delta16Columns`: a load and an add a lane; the avx2
  gathering bodies widen four or eight gaps in one `vpmovzxwd` add) and hands each step its C columns; a pair runs two
  slices of one form. The bodies are unchanged apart from taking the columns, so every form gives the same bits.
- `range_bytes`/`stored_bytes` count 2 bytes a gap and the first columns; the CLI `spmv_sell` and `to_sell` commands
  take `index` (auto, u32, delta16).
- Verification (win-debug): the [kernels] differential runs both forms at every level for all four types, adding
  matrices of 200003 columns with gaps of exactly 65535 and 65536, columns 0 and cols - 1 and empty rows, each holding
  both forms, and decodes every slice against the CSR rows; Auto's cut is checked at the resident size; the complex
  parallel case runs Delta16 at 1, 3 and 7 jobs; the CLI case runs every `index`. The sparse program passes; the
  iterative and eigen tests build. Board candidates for the row-close pass: the Auto cut (L2 or L3) and Delta16's
  cost on L2-resident ranges.
