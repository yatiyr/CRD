# 2026-10-30 — SIMD-DISPATCH: fft f64 plans per level, the avx2 re-run and win-asan green

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch) (the boards; fft's two open items). Before:
[fft level plans](2026-10-30-simd-dispatch-fft-level-plans.md). Board:
[fft, board 4](../bench/2026-10-30-simd-dispatch-fft-board.md#board-4-the-f64-plans-and-the-avx2-re-run).

## CI first

No completed run has a failure left to fix. Scheduled run 41034829979 (530d0f4e, in progress) failed win-release and
win-shipping-profile on five and six fft kernel cases, `SIGSEGV - Stack overflow` at avx2 and avx512: the codelet
frames fixed in fa525bfe, after that commit's parent; every cl kernel unit now builds with `/we4883`. Push run
41021189146 (4ab1cd90, in progress) holds the same and the NaN-payload cases fixed in 530d0f4e. The cancelled runs
41059057789 to 41038031504 list no failed jobs.

Needs CI audit: **win-asan is green**, the row's last guard clause. Scheduled run 41034829979 ran all 8,157 cases with
no failure in 7,088 s on an EPYC 9V45 (the ISA guard 203 s), push run 41021189146 in 15,016 s on an EPYC 7763 (the
guard 379 s), both with at least 12.0 GiB available and a commit peak of 9.4 and 10.9 GiB: 4.2 h at most against the
6 h cap. DIAG.11c and DIAG.12 still wait on a green complete-tier run; nothing dispatched (one is in progress).

## f64 at sse2 behind portable: the cause

Board 2 had f64 at sse2 and sse4.2 1.6x to 1.9x behind portable. portable keeps the host's glibc (ADR-0134 §8), whose
`fma` is the FMA instruction here; the sse2 unit computes every `fma` of the fused class exactly (`fma_exact.hpp`).
One run with glibc's features above sse2 hidden from portable as well: 1M 209.4 ms against sse2's 46.8 (host glibc
29.5). On a CPU without FMA, sse2 is 3.2x to 5.0x ahead of portable; only a CPU with FMA below avx2 (AMD family 15h)
keeps the emulation's cost. Pre-row needed avx2, so nothing regressed; recorded under the 2026-10-30 acceptance rule.

## The f64 plans

- `bench_fft_vs_refs` runs every forward `forward_builds<f64>` offers at 1K to 4M after each shootout row, the level's
  own first, interleaved every rep, the input copied in untimed, each gated against FFTW at the program's bound, and
  prints `fft kernel level:`.
- `gen_fft_level_plans.py` keeps one data file per type (`--import DIR --type f32|f64`), writes both into
  `level_plans_gen.hpp` and takes each size's canonical arm from the top level's planned one (it had no row during the
  boards), so a top-level winner becomes a row too.
- Board 4 (five passes, 6.6 minutes each): portable and sse2 run their own f64 plans at 1K to 512K but 32K, 1.04x to
  1.92x faster than the canonical plan in the same run (the four-step 1.62x and 1.92x at 512K); avx2 runs the
  canonical planner but at 2K, where the standalone-hier ran 0.003 ms against IP4-AoS's 0.004 in every pass. The
  canonical planner is unchanged (CrossPlatform's bits stay).
- The avx2 re-run on the current codelet edition (pre-row's programs on both engines): f32 and f64 accepted, every
  range overlapping pre-row's but f32 1M (0.72, the tiled four-step).
- avx512 and neon stay canonical: this host has neither and SDE does not time avx512; QUAL.1-HW now names their
  per-level boards. Every inverse stays canonical.

## Tests

`test_fft_kernels.cpp`: the forced forwards and the own-plan cross-level case are templates over the element type,
with f64 cases beside the f32 ones (every forward at 1K to 4M on every level the host runs; each level's own f64 plan
bit-identical run to run, within the ADR-0092 §5 class of the radix-2 reference and twice it of portable's, the
canonical plan portable's bits on every level). The table case no longer assumes avx2 is canonical everywhere; the
mode-switch count holds every row, f64 2K at avx2 included.

## Checks

win-debug: crd-hesap-fft-tests built; the eight table, plan, own-plan and f32 and f64 forced-forward cases passed
(98 s). linux-gcc-release mirror: `bench_fft_vs_refs` built and the board ran; tidy (LLVM 20.1.0) clean on it there
(it is in no Windows database) and on the test file on win-debug. The generator's `--check`, check-generated,
clang-format on the changed lines, Allman braces, the ctest guards, the repository tooling tests, the plan and
repository checks.

## Left

- The other families' boards (the row's list), the retune's thread sweep beside them.
