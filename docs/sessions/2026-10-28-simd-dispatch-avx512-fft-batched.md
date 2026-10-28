# 2026-10-28 — SIMD-DISPATCH: the fft zmm codelets, tranche 2 (the batched generator)

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). HEAD before: `0a5f288b`.

## CI first

No completed run has an unfixed failed job. Push run 40662109393 (`7e230e22`) gained a win-debug failure, the same
nonlinear CG moat `LineSearchFailed` that `31f8c3d0` fixed; complete run 40649898318 (`3e849d96`) still holds only
failures fixed in earlier batches; every other completed run in the last 15 was cancelled with no failed job. DIAG.11c
and DIAG.12 stay Needs CI: no complete-tier run has finished since.

## Follow-up: a failed case left the jobs pool up

The [tranche 1 note](2026-10-28-simd-dispatch-avx512-fft-codelets.md#left) recorded it: a case that fails a REQUIRE
between `crd::jobs::init` and `shutdown` leaves the pool up, and in a multi-case process (a kernel-level arm, a direct
run under SDE) the next case stops the run on `WorkerPool::init called twice`, every later case unreported. The 142
test files that call `init` mostly pair it by hand, so the fix is one mechanism instead of hundreds of edits:
`crd_discover_tests` links `tests/support/catch/jobs_pool_listener.cpp` into every test executable whose link closure
passes on crd-jobs' usage (65 on win-debug; a library's private link-only edge does not count, its headers never reach
the test). The listener notes whether the pool was up when a run of a case (one SECTION path) starts and shuts it down
when that run failed and left it up. A pool a listener or fixture keeps across cases stays, and a passing case that
leaks one still stops the next init, as the leak it is. `num_workers()` documents its 0 while the pool is down.
`test_jobs_pool_listener.cpp` re-enters crd-jobs-tests on two hidden cases (the first fails with the pool up, the
second brings its own up) and requires the child to report `2 | 1 passed | 1 failed` without the double init. Every
one of the 65 listener objects compiles on win-debug.

## The batched generator on the lanes

`gen_fft_batched.py` emitted its own Vec4d/Vec8f body and a lane-scalar edition under `#ifdef`. It now emits one body
per codelet on `codelet_lanes.hpp`'s lanes, which `fft_codegen_split.py` drives as for tranche 1: two tiles a step on
Vec8d/Vec16f at avx512, an odd last tile on Vec4d/Vec8f, a tile above portable, a column on scalars. Three addressing
patterns depended on the register width; the lanes header now owns them:

- `load_block`/`store_block`: the AoSoA rows keep their blocks of `kTile` real then `kTile` imaginary parts (fork 2
  of tranche 1: the layout does not change with the level). A two-tile step takes two adjacent blocks through the
  halves; a column takes `c / kTile * 2 * kTile + c % kTile`.
- `store_transposed`: stage 1's groups of a tile of outputs go out transposed into the block rows, one transpose per
  half on a two-tile step and a plain store per output on a column.
- The runtime-indexed twiddles stay one value over a step: S2 broadcasts per `t >> ash` with 2^ash = A >= 16 columns,
  and the factored stage-1 table needs M = 2^msh >= a two-tile step, so the planner's floor rises to 2^3 (f64) and
  2^4 (f32). It never binds (msh >= 5 for the n2 >= 1024 band), so no plan changes.

A codelet computes whole tiles: every caller passes a multiple of `kTile` (the 569 gate, the ds/sh factors, the hier
batches), which the generated header now states. The old lane-scalar edition covered every column; the SIMD editions
never did.

Removed with the dual body: `_assemble`, the scalar renderers and the four interleaved emitters `main()` no longer
called; `fft_codegen_split.py` keeps only its lanes path (every codelet is on the lanes, a guarded or fixed-type body
is refused) and its test covers that path. All three generators reproduce the tree: `gen_fft_batched.py --check` and
`gen_fft_codelets.py --check` on WSL, and the hier units from the `20c9f2a1^` bytes. Per unit set, `fma` 10,404,
`fnmadd` 10,404, the complex loads and stores and the twiddle loads are the old SIMD bodies' counts; the 744 transposed
groups and 2,448 block loads and stores replace the old transposes and `V::load`/`.store` calls. The seven units fell
from 327,866 to 205,679 lines. Compiled alone without the cache (MSVC, win-release), the slowest units of the
parallel build take 48 s at sse2 and at most 8 s at avx512, inside the 63 s budget; in the parallel build, with the
twenty-eight units at once, an sse2 unit took up to 365 s.

## Tests

- win-debug (scalar edition): `[kernels]` 3,603 assertions in 7 cases.
- win-release natively at portable, sse2 and avx2: 3,603 each; the whole program 3,884 in 36.
- win-release under SDE 10.13.1 `-skx`, `CRD_CPU_LEVEL=avx512`: `[kernels],[cpu-level-applied]` 4,740 in 8 (57 s),
  bit-exact with portable through the deep-split and sh sizes of the sweep.

## Left

Tranche 3, the hand-written vector sites of `fft_kernels.cpp`. Board candidates for AVX-512 hardware: the two-tile
step against avx2's one, and the portable batched leaves (now parts a column at a time) against HEAD's scalar edition.
