# 2026-10-27 — SIMD-DISPATCH: the hesap-dense GEMM kernels

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). Decision: [ADR-0134](../decisions/0134-runtime-cpu-dispatch.md).

## CI read

- No completed run has a failed job. Push run 40455017769 (`09ba6a3a`) resolved to the complete tier and finished
  green on all 26 jobs. 40478910518 (`b975919e`) was cancelled before any job ran; 40466574508, 40462897802 and
  40458470799 were cancelled with no failed job. Scheduled run 40475028681 and push run 40479322092 (`f91847f5`) are in
  progress with none failed, so nothing was dispatched.
- Needs CI audit: [DIAG.11c](../ROADMAP.md#slice-diag.11c) and [DIAG.12](../ROADMAP.md#slice-diag.12) already carry
  their hosted evidence (complete runs 40307186916 and 40434280766); 40455017769 is green on the same lanes. Both stay
  Needs CI until the rows before them close.

## Mechanism

hesap-dense is the sixth family: `crd_add_cpu_kernels(crd-hesap-dense NAME dense LEVELS portable sse2 avx2)`.

- `dense_kernels.hpp` holds only data: `GemmTraits` (the register tile, which shapes the packed panels, so it is the
  same on every level), `GemmUpdate`, the `GemmBlock<T>` view (packed A panels with a row stride, packed B panels, C
  in either layout, rows, columns, depth, an update and an optional lower mask) and the table (`gemm_block_f64`,
  `gemm_block_f32`). `dense_kernels()` (`src/dense_kernel_dispatch.cpp`) resolves it once.
- `detail/gemm_block.hpp` is the driver in the level namespace: the tile walk, the strict-upper skip of a lower block,
  the masked and edge merges and `PortableTile`. The kernel units run it on each level's tile; general code runs it on
  `PortableTile` for the complex types.
- One table call per block, never per microkernel (ADR-0134 §5, ADR-0088's measured loss): `gemm_packed_inner` is one
  block, the lower syrk sweep is one subtracted lower block, the small-GEMM path runs each worker's panels as one
  block, and the supernodal Cholesky's packed-TRSM walk, which called the microkernel per tile from hesap-direct, makes
  one subtract and one assign block per row panel and column block.
- `gemm_microkernel.hpp` (the locked dispatcher, the AVX2 bodies and its `CRD_SIMD_HAS_AVX2` tests) and
  `microkernel_backend.hpp` (the reserved ASM switch) are deleted; ADR-0082's hot-swap section and its cross-width
  bit-exact claim are struck in place (an asm body would be a level's table entry). `blas3.hpp` claimed the scalar and
  AVX2 builds agree through a no-FMA contract; they never did, and the comment now states the declared arithmetic.
- `kernels/dense_kernels.cpp`: portable runs `PortableTile`; sse2 sums with the same two roundings in 128-bit
  registers (f64 as two 6 by 4 halves, twelve accumulators; f32 as two 4 by 8 halves); avx2 keeps HEAD's FMA tiles
  (f64 6 by 8 in twelve Vec4d, f32 8 by 8 in eight Vec8f). Every tile hands each finished row to a sink while its
  accumulators are in registers: the product stores the tile, the fused path merges a full row-major tile into C with
  the driver's expression. The syrk sweep and the supernodal walk now take the fused path on full tiles (the same
  bits). The sse2 and avx2 tiles prefetch the next B panel one line a step.
- The objects define only their level namespace and the table (`nm -C --defined-only` on the GCC objects, plus
  `DW.ref.__gxx_personality_v0`).

### Determinism

The family is the first one declared by its arithmetic per level (ADR-0134 §7): each level is bit-exact with it.
Every product element is summed in depth order from +0; portable and sse2 round each multiply and add (they agree bit
for bit, and SSE2 has no FMA), avx2 uses a single-rounded FMA a step, as hesap's GEMM has since ADR-0082's 2026-05-20
update. Across the boundary an element stays within depth * eps * sum |a| |b|. HEAD's scalar lanes summed with two
roundings and its AVX2 lanes with FMA, so each level reproduces its HEAD lane.

## Verification

`test_dense_kernels.cpp` (`[kernels]`, forced arms `crd-hesap-dense-kernels-at-<level>`):

- every level the host runs, f64 and f32: rows 1, MR - 1, MR, MR + 1, 2 MR + 3 by columns 1, NR - 1, NR, NR + 1,
  2 NR + 5, depth 0, 1, 7, 8, 33, an A row stride equal to or 3 past the depth, plain and lower blocks at four
  origins, four updates, both layouts: every written element has its level's bits, no other element changes, and the
  product is within the inner-product bound of the portable level's. Panel padding and A columns past the depth hold
  NaN;
- `gemm` (packed path), `gemm_parallel` on three workers (small-GEMM path) and the lower syrk give the selected
  level's bits; the archive lists one distinct table per built level; the process runs the level its selection
  resolves.

Teeth, each built, observed failing, restored, rebuilt and rerun on win-debug:

| Break | Fails |
|---|---|
| avx2 f64 row 3 reads row 2; avx2 f32 row 6 reads row 5 | f64, f32 differential |
| avx2 fused merge scales c by alpha; fused assign adds c; fused subtract adds | differential |
| avx2 f64 tile reads one column past the depth | f64 differential |
| portable sums in reverse depth order | f64 and f32 differential |
| a masked tile takes the fused path | differential (touched) |
| row-major mask one column too many; column-major mask one row late; strict-upper skip one tile early | differential |
| column-major C written row-major | differential |
| sse2 f64 row 4 reads row 3; sse2 f32 second half reads the first; sse2 f64 alpha merge scales c; sse2 f32 product second half overwrites the first | differential |
| sse2 or avx2 listed with the portable table | archive case (distinct tables), and for avx2 the differential and selected-level cases |
| a small-GEMM worker runs only its first panel | selected-level case |

linux-gcc-debug: the avx2 f64 row, sse2 f64 row and portable order teeth fail and pass restored; the include budget
reports `OVER` (`<immintrin.h>` through `crd/math/simd/vec4d.hpp`) when `gemm_pack.hpp` includes `vec4d.hpp`.

The full crd-hesap-dense-tests (355 cases) and crd-hesap-direct-tests (190 cases) pass on win-debug at the selected
level and forced to sse2 and portable, and on linux-gcc-debug; the five forced arms and the 15 guards (19 on Linux)
pass. Strict tidy is clean on the changed sources and on the sse2 and avx2 entries of the kernel unit (the tool reads
a source's first database entry, the portable one, so the others ran from one-entry databases); the bench is
`UNGATED` (no tree resolves its Eigen include) and built with the lane's warnings on the board. clang-format reports
nothing on the new files and no more findings than HEAD on the changed ones; Allman passes.

## Board

`bench_hesap_gemm_vs_reference` gained `--one-thread` (every library on one thread at 64 to 1024) and the kernel
level line; its unused `run_cerid_best` is deleted. linux-gcc-release, `taskset -c 4`, three interleaved rounds,
medians, GFLOP/s, C = A B square RowMajor. New at forced `CRD_CPU_LEVEL`; HEAD's `blas3.cpp` and dense headers
through a scratch include directory at the lane's flags (`HEAD avx2`) and at the sse2 lanes' flags (`HEAD sse2`).
Eigen 3 and the system OpenBLAS (pthreads) at one thread. sse2 is from the second board, the rest from the third
(after the small-GEMM and register changes below).

| Type | N | portable | sse2 | avx2 | HEAD sse2 | HEAD avx2 | Eigen | OpenBLAS | sse2 / HEAD sse2 | avx2 / HEAD avx2 | avx2 / Eigen | avx2 / OpenBLAS |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| f32 | 64 | 9.75 | 43.27 | 105.58 | 9.94 | 108.24 | 136.09 | 138.06 | 4.35x | 0.98x | 0.78x | 0.76x |
| f32 | 256 | 9.96 | 48.81 | 132.91 | 10.12 | 131.72 | 130.06 | 156.91 | 4.82x | 1.01x | 1.02x | 0.85x |
| f32 | 512 | 9.91 | 49.28 | 137.68 | 10.07 | 139.32 | 147.87 | 163.32 | 4.89x | 0.99x | 0.93x | 0.84x |
| f32 | 1024 | 10.02 | 48.54 | 140.35 | 10.15 | 141.45 | 155.65 | 162.76 | 4.78x | 0.99x | 0.90x | 0.86x |
| f64 | 64 | 3.26 | 20.80 | 61.06 | 3.45 | 60.81 | 67.34 | 65.09 | 6.03x | 1.00x | 0.91x | 0.94x |
| f64 | 256 | 3.39 | 22.54 | 76.19 | 3.59 | 75.00 | 78.73 | 77.12 | 6.28x | 1.02x | 0.97x | 0.99x |
| f64 | 512 | 3.30 | 22.41 | 75.95 | 3.55 | 77.02 | 78.68 | 79.38 | 6.31x | 0.99x | 0.97x | 0.96x |
| f64 | 1024 | 3.42 | 22.51 | 76.52 | 3.60 | 77.07 | 79.15 | 80.16 | 6.25x | 0.99x | 0.97x | 0.95x |

- The first board had avx2 at 0.95x to 0.98x of HEAD on f32: the tile returned its accumulators as a struct (stored
  and reloaded per tile), the small-GEMM path made one table call per 8-row panel, and the driver reloaded the block's
  fields after every store through C. The row sink, one block per worker range and a local copy of the block brought
  every row to 0.98x to 1.02x; f32 at 64 keeps 0.98x (105.6 against 108.2, about 100 ns of call and setup on a 5 µs
  product; HEAD's own binary moved from 108.2 to 110.1 between boards).
- An SSE2 CPU runs the sse2 tile at 4.3x to 4.9x (f32) and 6.0x to 6.3x (f64) of HEAD's sse2 lanes.
- portable is 0.93x to 0.98x of HEAD's sse2-lane body on the same scalar loop (both spill the 6 by 8 accumulator
  array); on x64 the sse2 level replaces it, so it runs only in scalar builds and on architectures without a body.
- Single-thread avx2 trails OpenBLAS (f32 0.76x to 0.86x, f64 0.94x to 0.99x) and Eigen at most sizes; HEAD did the
  same. The levers are the f32 tile (eight of sixteen YMM hold accumulators; a 6 by 16 tile holds twelve), the small
  sizes' fixed cost, and AVX-512 bodies.

## Left in the row

The fft and geometry kernels and the rest of dense (blas2's gemv and symv prefetch blocks, `dot_simd`, the LDLT, QR,
eigen and supernodal SIMD loops) as per-level units; the GEMM levers above and a register-blocked portable tile for
scalar builds join the rest the row lists.
