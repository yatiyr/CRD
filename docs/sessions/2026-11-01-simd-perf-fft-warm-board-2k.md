# 2026-11-01 — SIMD-PERF: the fft f32 board's warm transforms and the 2K plan

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-perf).

Row: [SIMD-PERF](../ROADMAP.md#slice-simd-perf), Left item 1 (fft's f32 forward where MKL leads), at the widest
planned gap on the benchmark machine: the f32 2K forward at 0.52x MKL at avx512.
Board: [2026-11-01-simd-perf-fft-warm-board-2k](../bench/2026-11-01-simd-perf-fft-warm-board-2k.md).

## CI first

No completed run covers this track's commits since `58e6399d`: every push run on main is still cancelled by the next
push. Dispatch 41254049879 at `31404ec4` (it holds `4e5f41f0` and `7c05c168`, not `1ce11a1e`, `590071f1` or the
in-place-3 commits) was still running; its clang-cl, ARM64 and repository lanes had passed, and one lane had failed:
linux-gcc-shipping, GCC's LTO `-Werror=null-dereference` at `tests/foundation/compression/test_deflate.cpp:755-756`
(another track's file, fixed on main by `95ec6965`). `win-tidy` had not finished, so the avx2 and avx512 nn units and
the in-place codelets still have no clang-tidy result.

## Profile (the bound, with its evidence)

`prof_fft_f32.sh 11`, `12` and `13` at avx512 and avx2 for in-place-3, the standalone-hier and MKL, then the profile
loop at 1K to 8K for every offered arm. At 2K the steady-state loop has in-place-3 1.28x the standalone-hier and 1.57x
the planned IP4-AoS at avx512 (core-bound 41%, against the standalone-hier's 38% memory bound), while the boards had
ranked the standalone-hier first by 1.10x. The cause was the board, not the kernels: it timed each arm's transform
right after five other forwards and MKL's, so its tables and code came back cold, and the profile loop does not. Two
further findings: MKL runs its AVX-512 library at every `CRD_CPU_LEVEL` (with `MKL_ENABLE_INSTRUCTIONS=AVX2` the
avx2 in-place-3 runs 0.75x MKL at 2K and 1.09x at 4K, where the board's 0.75x had been against AVX-512 code), and the
cold full board had put MKL's 1K transform at 0.00099 ms against 0.00061 ms warm (the earlier "1K 1.37x MKL").

## The change

- **`bench_fft_f32`'s board:** each timed transform follows an untimed transform of the same side, from a fresh copy
  of the input, so it runs warm as in a stream of transforms. The warm board agrees with the loop's ranking at every
  2K arm and level. The loop prints its time to the nanosecond (four decimals of a millisecond printed the copy as
  0.0001 and 0.0005 in two runs).
- **The planner:** the canonical f32 2K forward is in-place-3 (avx512 runs it): 1.497x HEAD's IP4-AoS [1.488-1.541],
  0.795x MKL (was 0.532x). The 2K inverse stays IP4-AoS; the avx2 per-level row keeps IP4-AoS (the i9's board, the
  machine-dependent plan item) though in-place-3 runs 1.30x it here.

## Checks

On the machine (linux-gcc-release, no warning under `-Werror`): `crd-hesap-fft-tests` (52 cases), `crd-hesap-dsp-tests`
(104), `crd-hesap-comms-tests` (27) and `crd-hesap-wavelet-tests` (25), all passing, and the fft cases at forced
portable, sse2, avx2 and avx512, all passing (the planner case's per-size table updated). HEAD's and the candidate's
planned forwards print the same bits at 4K to 64K at both levels. Locally: the plan, repository and Allman checks.
The machine ran 0.46 h in one start and is stopped.

## Left

- **avx512 2K and 4K, the rest to MKL:** the engine's group transposes (24% and 21% of the samples) and 1.40x and
  1.31x MKL's instructions; where one avx512 step holds a whole 16 x 16 group, pass 3 can load it transposed in
  registers instead of through the 2 KiB scratch.
- **8K:** the warm board's minimum has in-place-3 level with the standalone-hier at avx512 and the loop's mean 1.24x
  behind it (64 KiB leaves the L1d); the plan stays the standalone-hier.
- **1K:** the standalone-hier at 0.82x MKL at avx512 and 0.63x at avx2, not yet profiled.
- **ARM64:** the canonical 2K and 4K plans (in-place-3) are unmeasured on ARM hardware; the hosted ARM64 lanes prove
  their results only.
- The avx2 per-level rows at 2K to 8K with the i9 re-board, the jobs retune's boards per level, torch and onnxruntime
  pinned and boarded.
