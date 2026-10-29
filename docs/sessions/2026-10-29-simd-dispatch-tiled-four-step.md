# 2026-10-29 — SIMD-DISPATCH: the tiled four-step, the f32 1M forward's third board arm

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch); follows
[the forced-arm levels note](2026-10-29-simd-dispatch-forced-arm-levels.md).

## CI first

Every completed run in the last 15 is a pending push run the concurrency group replaced before any job started, so no
hosted result is newer than the last audit and no Needs CI row changes. Push run 40784988994 (92db578d) still runs
win-asan, in its Test step since 00:24 UTC; its other failed jobs are the ones earlier batches traced to fixes at HEAD.
The forced-arm levels note's Left stays: whether plain arms should drop their differential cases waits for the first
hosted asan times of 5fbf494d.

## Why

The row's first Left item asked for the removed M18 tiled four-step (0.85x MKL on the 2026-06 harness) on the board
against the deep-split (0.80x to 0.91x) and MKL at every level. The fft-kernels note's recipe, a scratch include
override of `fft.hpp` over the old bodies, no longer works: since 20c9f2a1 the codelets sit in kernel units a header
override does not reach, and the old bodies are raw Vec8f with `m16tr::transpose8x8` intrinsics, so they cannot run at
portable, sse2 or avx512. A port onto the lanes is the only route to every level. The scout brief's claim that M18's
twiddle products are two-rounded with no fma is wrong: its leaf DAGs use `simd::fma`/`fnmadd`, which round once on
every level through `codelet_lanes.hpp`.

Neither old body fits the split tool's grammar (one tile loop, one depth): `codelet32_batched_tiled_bb128` turns eight
tiles 128 columns apart through a register transpose, and `codelet32_p2_fused_tile64` loops 32 times over a 64 KiB
tile array. Rather than hand-edit 2,000 generator-absent lines, the dataflow is now four leaves of the generator that
already emits the deep-split's AoSoA editions, each one tile loop on the lanes.

## What landed

- `scripts/gen_fft_batched.py`: the tiled emitters, on a `load` hook of `_render_node`. Each 1024-point axis is
  32 x 32. P1, per block of 128 columns i2: `codelet32_tiled_p1_leaf` (leaf over i1's high digit, W_1024, a
  transposed store of each eight outputs into rows of 32) and `codelet32_tiled_p1_corner` (leaf over the low digit
  into the tiled intermediate NT: tiles of 8 i2 by 16 k1 in AoSoA rows; the corner turn is the first leaf's
  transposed store, so neither needs a cross-tile register transpose). P2, per block of 16 k1: `codelet32_tiled_p2_leaf`
  (NT times W_n^{i2·k1} = base·lane, leaf, W_1024) and `codelet32_tiled_p2_final` (leaf into X, interleaved). A block's
  stage (1 MiB, then 132 KiB) sits in the plan's scratch, off the stack (jobs' small fiber stack is 64 KiB). The
  strides of a step's 32 loads or stores are padded off the 4 KiB alias (stage groups, NT tile rows, the P2 stage's
  33rd row). A twiddle known to be 1 is skipped, so its products keep their signs. The units regenerate on WSL, the
  reference host (`--check` and the refreshed hashes in `scripts/generated-sources.json`).
- `fft.hpp`: `fft::detail::LargeForward` (DeepSplit, SixStep, TiledFourStep) and a constructor taking it, for an f32
  plan of 2^20 only; the other two turn the deep-split off, and TiledFourStep builds NT and the tables
  (`build_tiled_four_step`, each entry rounded once from its exponent mod its period). The six-step arm is the path
  the f64 1M takes, now measured for f32 too. The winner becomes the planner's only path and the choice goes.
- `fft_kernels.hpp`: `FftPlanState::tf_*` and `kTiledFourStepNt`; `fft_kernels.cpp`: `execute_tiled_four_step`,
  entered for an f32 forward with `tf_nt` before the six-step.
- Tests: the every-level and specials bodies of `test_fft_kernels.cpp` now take a plan, and a new `[kernels]` case runs
  both arms forward (aligned, one word off, negative zeros, subnormals, an infinity and a NaN) at every level against
  the reference and portable's bits; `test_fft.cpp` runs both through a public plan.
- An index model of the four leaves in numpy (scratch) gave 6.0e-16 against `numpy.fft.fft` before any C++ was built.

## Checks

win-debug: crd-hesap-fft and its tests build at /W4 /WX. The new kernel case passes unforced (portable, sse2, avx2;
126 assertions, 18 s) and under SDE 10.13.1 `-skx` at an applied avx512 with `[cpu-level-applied]` (89 assertions);
the public case passes. Tidy on fft_kernels.cpp, fft.hpp, fft_kernels.hpp, both tests and the four regenerated units
holding the new leaves; clang-format adds no finding to any touched file; Allman, master-plan, repository and the
ctest guards pass. Two Python runs died on impossible states and passed when rerun: the WSL generator in its scheduler
(Python 3.12, `'list' object has no attribute 'id'`), then `crd-diag-coverage` and one Allman run (Windows Python
3.14.4, `'tuple' object is not an iterator`, the error the forced-arm batch's Allman run met). Four in two batches, in
two interpreters: this host still misexecutes under load; recorded, not chased.

## Left

- The row-close board: the three arms and MKL at every level from one binary (`bench_fft_f32`, after the pocketfft
  item splits `crd-fft-peers`); the winner becomes the only path, the others and `LargeForward` go.
- If levels pick different winners, one choice must hold for every level: a user decision at the board.
