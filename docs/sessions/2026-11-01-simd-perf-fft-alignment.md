# 2026-11-01 — SIMD-PERF: the fft board and loop reconciled by the buffer's alignment

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-perf).

Row: [SIMD-PERF](../ROADMAP.md#slice-simd-perf), Left item 1 (fft's f32 forward where MKL leads): the last batch's
open question, the board's best transform and the loop's mean ranking the 8K arms oppositely, and the 8K plan that
waited on it. Board: [2026-11-01-simd-perf-fft-alignment](../bench/2026-11-01-simd-perf-fft-alignment.md).
Engine code is unchanged; the bench program and its two scripts changed.

## CI first

Every push run on main since about 23:00 UTC was cancelled by the next push, as in the last three batches. Run
41262267004 (`8a243a34`) is still alive: win-vs, win-arm64-debug and linux-clang-arm64-debug passed, its Windows
repository checks failed on the ci/ orchestrator end-to-end case this track has not touched, and win-tidy and the
other lanes were still in progress or queued. The push run for `6b4cf519` (41268493203) had no jobs yet. Nothing
reported against this track's commits, so nothing was fixed first.

## Profile (what made the two measures disagree)

`bench_fft_f32 --loop --distribution` gave each side's per-transform distribution in a stream and
`prof_fft_f32.sh`'s rounds each side's clock, alternating the sides:

- **Not the statistic.** Minimum, percentiles and mean order the arms alike at 1K to 8K in every round.
- **Not the clock at 8K.** All three 8K sides ran 3.60 to 3.69 GHz. At 2K and 4K in-place-3 and MKL ran about
  0.3 GHz below the standalone-hier, so cycle counts do not compare across those arms (time does).
- **The buffer's alignment.** The loop's process placed `work` 16 bytes past a cache line, the board's on one.
  Reordering the loop's allocations alone took its in-place-3 from 11.6 to 7.26 microseconds and its
  standalone-hier from 9.8 to 7.8; sweeping the buffer by whole cache lines changed nothing. Past the L1d every
  64-byte access of a misaligned buffer touches two lines: the 8K to 64K forwards lose 1.24x to 1.27x (in-place-3
  at 8K 1.50x, all three passes in the caller's buffer) and MKL's 1.28x to 1.35x. At 8K (misaligned, as the loop
  ran) `l1d.replacement` net of the copy side is 7.0 a line of data a transform for in-place-3, 8.0 for the
  standalone-hier and 8.2 for MKL, and 4K aliasing (`ld_blocks.address_alias`, at most about 600 a transform) is minor.

## Decision: 8K stays the standalone-hier

Decided cells at both alignments (`ab_fft_f32.sh`, `CRD_AB_OFFSET`): in-place-3 is 1.077x [1.068-1.136] the
standalone-hier on an aligned buffer and 0.858x [0.855-0.872] on a malloc'd one at avx512 (1.091x and 0.816x at
avx2). A forward cannot change with its caller's address without changing its bits (ADR-0063), and running
in-place-3 on a plan-owned aligned copy would cost two 64 KiB copies (about 3.2 microseconds) for a 0.6 microsecond
lead, so the plan keeps the arm with the better worst case. The 2K and 4K in-place-3 plans hold misaligned too
(1.41x and 1.66x the standalone-hier).

## The change (tooling)

- `bench_fft_f32 --offset B` places the transformed buffer B bytes past a 64-byte boundary, on the board and in the
  loop (default 0), so a board measures a declared alignment, not its allocator's history; `--distribution` prints
  a stream's per-transform minimum, percentiles, mean and maximum.
- `ab_fft_f32.sh` passes `CRD_AB_OFFSET`, `prof_fft_f32.sh` passes `CRD_PROF_OFFSET`, counts task-clock and
  ref-cycles, and with `CRD_PROF_ROUNDS` (and `CRD_PROF_PASSES=0`) alternates the sides' streams with each one's
  clock. Both pass the offset only when it is set, so older saved builds still run.
- [docs/bench/README.md](../bench/README.md#fast-ab-boards) item 5: declare where the data sits.

## Checks

The program built under `-Werror` on the machine (linux-gcc-release) and every side stayed within the bound against
the FFTW f64 truth at every offset; each script path ran there once (offset set and unset, rounds with and without
the passes, a refused odd offset). The engine is unchanged, so no engine test changed. Locally: the plan,
repository and Allman checks. The machine ran 0.47 h in one start and is stopped.

## Left

- **16K (0.69x MKL aligned, 0.74x misaligned):** the widest f32 gap from 1K to 64K; profile it first.
- **1K (0.79x):** the shuffles between the 32 x 32 passes (the last profile), a 64 x 16 split or whole 16-column
  blocks from stage 1.
- **8K (0.83x):** the standalone-hier's 128-point leaf spills; in-place-3's misaligned passes (1.50x) if it is to
  win both alignments.
- avx512 2K's two-pass shape, ARM64's canonical 2K and 4K plans, the i9 re-board of the avx2 plan rows, the jobs
  retune's boards per level, torch and onnxruntime pinned and boarded.
