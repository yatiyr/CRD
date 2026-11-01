# 2026-11-01 — SIMD-PERF: the f64 forward's avx512 plans from the benchmark machine's level board

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-perf).

Row: [SIMD-PERF](../ROADMAP.md#slice-simd-perf), Left item 1 (the fft forwards where MKL leads), the previous
[session](2026-11-01-simd-perf-fft-avx512-level-plans.md)'s first Left entry: f64's avx512 rows, which needed
`bench_fft_vs_refs` to resolve below 1 us first. Board:
[2026-11-01-simd-perf-fft-f64-avx512-level-plans](../bench/2026-11-01-simd-perf-fft-f64-avx512-level-plans.md).

## CI first

- Manual dispatch 41295647725 (`43963cb2`, the first run covering `bcec86fd` and batch 19) was still running when
  the batch ended: win-vs, win-arm64-debug, linux-gcc-arm64-debug and both repository checks passed, so MSVC and the
  ARM64 compilers have now built the stage-1 transposed store and the whole-block stores; win-tidy, the clang-cl
  lanes and the Linux x64 lanes were queued or running, none failed. Every push run since was cancelled by the next
  push. Nothing of this track's to fix yet.

## Profile first

- **Where f64 stood at avx512.** The level board's shootout rows (HEAD, offset 0, medians of five passes): Cerid
  0.60x to 0.89x MKL at 1K to 64K (2K the widest, 0.60x), 1.16x to 1.78x from 128K to 8M; 0.90x to 1.07x FFTW at 1K
  to 8K and 1.09x to 1.61x above; 1.83x to 2.81x pocketfft everywhere. MKL 2020's f64 falls from 0.216 ms at 64K to
  1.05 ms at 128K here, so the gap is the small and mid band.
- **Where the plan left time.** avx512 ran the canonical planner, tuned on the i9 at avx2. The level board times
  every arm the planner offers, so it is the profile of the plan choice: the standalone-hier ran 1K to 4K 1.07x to
  1.32x faster than the canonical IP4-AoS, the deep-split-3 8K to 64K 1.03x to 1.23x faster, the deep-split-4 1M and
  2M 1.18x to 1.41x faster than the six-step and four-step, and the six-step 4M 1.22x to 1.26x faster than the
  four-step (board 2, both offsets). The bound behind each is the f32 sessions': the deep-splits' passes stay in the
  L2 where IP4-AoS's and the four-step's do not
  ([32K and 64K profile](2026-11-01-simd-perf-fft-32k-64k-profile.md),
  [four-step profile](2026-10-31-simd-perf-bench-machine-fft-f32-profile.md)), and the standalone-hier's two passes
  replace IP4-AoS's radix-4 stages at 1K to 4K. A plan row is the lever; the kernels themselves are next.

## The change

- **The f64 board's method** (`feat(bench)`): `bench_fft_vs_refs` takes `bench_fft_f32`'s method. Each timed
  transform follows an untimed one of the same contender (warm, as in a stream), every time prints to 10 ns (1 us
  could not separate the f64 arms below 64K: 1K is 1.5 us), every contender transforms one buffer at `--offset`
  bytes past a 64-byte boundary (FFTW planned on it, pocketfft's destination at the same alignment), `--sizes`
  bounds a run, and each arm's row ends with its output's FNV-1a digest (`ab-board.py --same`).
  `board_fft_level_plans.sh` passes `--sizes` to the f64 program too.
- **The avx512 rows** (`perf(fft)`): five passes at 1K to 4M imported beside the i9's levels; the passes' rule gave
  the standalone-hier at 1K to 4K, the deep-split-3 at 64K to 256K, the deep-split-4 at 1M and 2M and the six-step
  at 4M.
- **A heap for the 4M walk** (`test(fft)`): the cross-level case walks each size past 1M at which a level the host
  runs plans other than canonically; avx512's f64 2M and 4M rows took the f64 case to 4M, where seven 64 MiB buffers
  and three plans ran its 1 GiB heap out (`TlsfAllocator: out of memory`, reported by CTest as SIGILL from the fatal
  handler, at the plain and the forced-avx512 arm). The heap now holds 32 transforms of the walk's largest size,
  at least 1 GiB.
- **The paired board's judgement** (`perf(fft)`): board 2 measured every row against the canonical arm at offsets
  0 and 16. It decided 8K to 32K, which the passes' overlapping ranges had left canonical (the deep-split-3 1.06x to
  1.23x faster at both offsets), and found the passes' 256K row 4.2% slower off a line. `gen_fft_level_plans.py
  --judge LEVEL:N=ARM --type T --board LINK` records the arm a paired board kept at a size in that level's entry;
  the generator takes it in place of the passes' rule, refuses an arm the level's board did not time or a size it
  did not measure, marks the header row "judged", lists the judging board, and a later import of the level drops
  the judgement with the board it was made of. One repository-tool case covers the override, the refusals and the
  drop. avx512 now plans the deep-split-3 at 8K to 32K and keeps the canonical deep-split-4 at 256K.

## Decisions

- **A paired board may decide a plan row.** The passes are unpaired (one process each, best of the program's
  repetitions), so one outlying pass overlaps two arms' ranges while the paired ratios of the same arms resolve
  them by 1.06x to 1.23x at both offsets; and an aligned-buffer board cannot see a row that a malloc'd buffer loses.
  The README's items 5 and 6 already made the paired board at both offsets the decision; the judgement records it
  in the data the header is generated from, so the table and its evidence stay one record.
- **128K keeps the passes' deep-split-3:** 1.05x faster aligned, and off a line its interval's lower end (0.985)
  lies inside the band, so no alignment loses.

## Results

- Each kept row against the canonical arm (board 2, offsets 0 and 16): 1K to 4K the standalone-hier 1.07x to 1.32x,
  8K to 64K the deep-split-3 1.03x to 1.23x, 128K 1.05x aligned and level off a line, 1M and 2M the deep-split-4
  1.18x to 1.41x, 4M the six-step 1.22x to 1.26x; every interval clear of the band but 128K's off a line.
- The planned forward against MKL (board 3, offset 0, HEAD's plan then the candidate's): 1K 0.68x to 0.75x, 2K
  0.61x to 0.74x, 4K 0.67x to 0.72x, 8K 0.76x to 0.94x, 16K 0.80x to 0.84x, 32K 0.81x to 0.94x, 64K 0.92x to 0.93x,
  1M 1.57x to 1.84x, 2M 1.29x to 1.85x, 4M 1.68x to 2.11x; off a line 1K to 64K 0.68x to 0.95x.
- Unchanged rows against HEAD, the same bits: 256K and 512K level.

## Checks

- On the machine (linux-gcc-release, the final candidate's tree): the whole tree built with `-k 0`, no failure;
  CTest's `fft|dsp|stft|cwt|conv|nn|simd` cases with the `crd-no-*`, `crd-check*` and diag guards, 920 of 920 (the
  first candidate's run had failed only the two f64 cross-level arms, the heap above).
- Locally: `gen_fft_level_plans.py --check`, the generated-sources record refreshed, the `FftLevelPlans` cases, the
  plan, repository and Allman checks.
- The machine ran 1.70 h in one start and is stopped.

## Left

- f64 at 1K to 4K: 0.72x to 0.75x MKL with the standalone-hier, the widest f64 gap; profile it against MKL
  (top-down, the leaves' instruction mix) before a kernel lever. f64 16K: 0.84x.
- A fast planned-only mode for `bench_fft_vs_refs` (the planned forward and MKL, the truth from `FFTW_ESTIMATE`, no
  FFTW, pocketfft or arms timed), so a board of the f64 plan against MKL takes minutes: this session's board 3 ran
  every peer and arm a repetition.
- The f32 1K and 2K gaps (0.90x to 0.97x MKL), the loads' halves where port 5 has room, the 32-point leaves'
  spills, and the rest of the 32K and 64K session's list.
