# 2026-11-01 — SIMD-PERF: the f64 forward's avx512 plans from the benchmark machine's level board (real AVX-512)

<!-- doc-role: evidence -->
> Dated evidence; counts and results are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).
> Owner row: [SIMD-PERF](../ROADMAP.md#slice-simd-perf); session:
> [2026-11-01](../sessions/2026-11-01-simd-perf-fft-f64-avx512-level-plans.md); method:
> [fast A/B boards](README.md#fast-ab-boards). Never compare these numbers with the i9-14900K boards; ratios only.

- **Machine:** the optimization track's benchmark machine, as in the
  [first board](2026-10-31-simd-perf-fft-f32-xeon.md): GCE spot `c4-standard-8`, Intel Xeon Platinum 8581C (Emerald
  Rapids), L1d 48 KiB and L2 2 MiB a core, Ubuntu 24.04, GCC 13.3. **Real AVX-512 hardware.** Every process pinned
  to CPU 3 (the level board to CPU 4), the peers one thread (`MKL_NUM_THREADS=1`, `OMP_NUM_THREADS=1`);
  `build-bench.py host` before the boards: 28.0 to 28.6 of 29.4 GiB available, others 0.0 cores, idle. One start.
- **Binaries:** `bench_fft_vs_refs` from the linux-gcc-release lane tree (GCC 13.3, `-O3`, program LTO), with the
  session's `feat(bench)` method (each timed transform warm, every time to 10 ns, the buffer at a declared offset,
  each arm's output digest): HEAD (`d5a58201`, `~/ab/b21-head`; avx512 ran the canonical f64 planner, no avx512 f64
  row) and the candidate (`6da9bad8`, `~/ab/b21-cand`; the avx512 rows below). The detected level selects the avx512
  fft unit (`cpu detected=avx512 selected=avx512`).
- **Peers:** Intel MKL 2020.0.4, FFTW 3.3.10 (`FFTW_MEASURE`, planned on the declared buffer) and pocketfft
  `4d865cba` (`-march=native`), timed in the same process; every Cerid arm and pocketfft gated against FFTW at
  32 eps log2 n.

## Board 1: the level board (the plan table's data)

`CRD_BENCH_LEVELS=avx512 scripts/board_fft_level_plans.sh ~/ab/b21-level --type f64 --passes 5`: five passes of
`bench_fft_vs_refs` (every f64 forward the planner offers at 1K to 4M, each warm, best of the program's repetitions,
a 64-byte-aligned buffer), one process a pass, about 2 minutes a pass. `gen_fft_level_plans.py --import` filed them
as the f64 data file's avx512 entry
([2026-10-30-simd-dispatch-fft-f64-arms.json](2026-10-30-simd-dispatch-fft-f64-arms.json), every pass's time per
arm, with this machine, commit and board) beside the i9's portable, sse2 and avx2. The passes' rule: the fastest
median whose every pass beat every pass of the planned (canonical) arm.

| n | canonical (HEAD) | median ms [min-max] | fastest | median ms [min-max] | passes' row |
|---|---|---|---|---|---|
| 1K | ip4 | 0.00161 [0.00156-0.00165] | standalone-hier | 0.00147 [0.00146-0.00150] | **standalone-hier** |
| 2K | ip4 | 0.00441 [0.00431-0.00463] | standalone-hier | 0.00365 [0.00351-0.00381] | **standalone-hier** |
| 4K | ip4 | 0.00915 [0.00904-0.00961] | standalone-hier | 0.00859 [0.00833-0.00892] | **standalone-hier** |
| 8K | ip4 | 0.02181 [0.01758-0.02194] | deep-split-3 | 0.01809 [0.01481-0.01884] | canonical (ranges overlap) |
| 16K | ip4 | 0.04170 [0.04158-0.04191] | deep-split-3 | 0.04049 [0.03800-0.04673] | canonical (ranges overlap) |
| 32K | ip4 | 0.10471 [0.09014-0.12350] | deep-split-3 | 0.09127 [0.08435-0.09794] | canonical (ranges overlap) |
| 64K | ip4 | 0.25573 [0.24162-0.29559] | deep-split-3 | 0.22732 [0.21053-0.23736] | **deep-split-3** |
| 128K | deep-split-4 | 0.60706 [0.60335-0.61288] | deep-split-3 | 0.57477 [0.51410-0.57779] | **deep-split-3** |
| 256K | deep-split-4 | 1.30551 [1.29919-1.31390] | deep-split-3 | 1.26342 [1.18171-1.27389] | **deep-split-3** |
| 512K | deep-split-4 | 2.76399 [2.74396-2.77441] | deep-split-4 | | canonical |
| 1M | six-step | 6.65298 [6.63911-6.79770] | deep-split-4 | 5.77978 [5.70784-5.85032] | **deep-split-4** |
| 2M | four-step | 18.76593 [18.52174-18.83636] | deep-split-4 | 13.31165 [12.87731-14.60247] | **deep-split-4** |
| 4M | four-step | 39.32263 [39.27882-40.07798] | six-step | 32.64842 [30.67524-33.64988] | **six-step** |

At 8K to 32K one pass of an arm or another fell outside the rest (8K's ip4 0.01758, 32K's ip4 0.12350), so the
passes' ranges overlap while the medians differ by 1.03x to 1.21x: the passes are unpaired, one process each.

## Board 2: the rows at both offsets

Each candidate row against the canonical arm in one binary, through `scripts/ab-board.py` (the arms' rows of
`bench_fft_vs_refs --sizes A-B --offset B`, one process a repetition, one warm-up discarded): median paired speed
ratio [percentile-bootstrap 95%], 2% band, 5 to 9 repetitions. Offset 0 is a 64-byte boundary, 16 a malloc'd
buffer's alignment.

| n | row against canonical | offset 0 | offset 16 | kept |
|---|---|---|---|---|
| 1K | standalone-hier / ip4 | 1.088 [1.052-1.346] faster | 1.178 [1.173-1.201] faster | standalone-hier |
| 2K | standalone-hier / ip4 | 1.245 [1.213-1.259] faster | 1.323 [1.242-1.340] faster | standalone-hier |
| 4K | standalone-hier / ip4 | 1.068 [1.027-1.075] faster | 1.076 [1.049-1.149] faster | standalone-hier |
| 8K | deep-split-3 / ip4 | 1.227 [1.223-1.236] faster | 1.122 [1.113-1.156] faster | **deep-split-3 (judged)** |
| 16K | deep-split-3 / ip4 | 1.086 [1.059-1.091] faster | 1.073 [1.065-1.117] faster | **deep-split-3 (judged)** |
| 32K | deep-split-3 / ip4 | 1.129 [1.103-1.130] faster | 1.061 [1.039-1.084] faster | **deep-split-3 (judged)** |
| 64K | deep-split-3 / ip4 | 1.030 [1.020-1.066] faster | 1.062 [1.053-1.085] faster | deep-split-3 |
| 128K | deep-split-3 / deep-split-4 | 1.052 [1.041-1.056] faster | 1.002 [0.985-1.032] unresolved | deep-split-3 |
| 256K | deep-split-3 / deep-split-4 | 1.021 [1.008-1.044] unresolved | 0.958 [0.942-0.969] **slower** | **deep-split-4 (judged)** |
| 1M | deep-split-4 / six-step | 1.177 [1.116-1.182] faster | 1.207 [1.192-1.218] faster | deep-split-4 |
| 2M | deep-split-4 / four-step | 1.405 [1.386-1.418] faster | 1.327 [1.289-1.347] faster | deep-split-4 |
| 4M | six-step / four-step | 1.255 [1.222-1.279] faster | 1.218 [1.212-1.227] faster | six-step |

The paired ratios decide 8K to 32K, which the passes left open, at both offsets; at 256K the passes' row loses 4.2%
off a line and is not resolved aligned, so the README's rule (a choice the two alignments decide differently takes
the arm whose worse case is better) keeps the canonical deep-split-4. At 128K the aligned buffer gains 5% and the
offset-16 interval's lower end (0.985) lies inside the band, so no alignment loses and the passes' row stands.
`gen_fft_level_plans.py --judge avx512:8192=deep-split-3 ... avx512:262144=deep-split-4` records the four
judgements in the avx512 entry, which the generator takes in place of the passes' rule.

## Board 3: the candidate's planned forward against MKL

Both builds' planned forward (the shootout's Cerid column) and MKL from the candidate's process, alternating
processes, offset 0, through `ab-board.py`, MKL the reference: Cerid's speed over MKL's, at most 9 repetitions (7
at 128K to 1M, 5 at 2M to 8M).

| n | HEAD's plan, offset 0 | the candidate's, offset 0 | HEAD's plan, offset 16 | the candidate's, offset 16 |
|---|---|---|---|---|
| 1K | 0.677 [0.661-0.714] | 0.748 [0.722-0.788] | 0.570 [0.553-0.578] | 0.682 [0.662-0.691] |
| 2K | 0.609 [0.592-0.618] | 0.739 [0.668-0.746] | 0.557 [0.541-0.579] | 0.746 [0.717-0.798] |
| 4K | 0.668 [0.638-0.671] | 0.715 [0.662-0.729] | 0.664 [0.654-0.671] | 0.783 [0.750-0.804] |
| 8K | 0.755 [0.741-0.764] | 0.936 [0.909-0.956] | 0.757 [0.722-0.780] | 0.920 [0.886-0.929] |
| 16K | 0.801 [0.791-0.847] | 0.843 [0.834-0.859] | 0.869 [0.832-0.884] | 0.950 [0.910-0.978] |
| 32K | 0.807 [0.781-0.848] | 0.942 [0.861-0.975] | 0.853 [0.801-0.866] | 0.868 [0.831-0.918] |
| 64K | 0.916 [0.870-0.931] | 0.930 [0.891-0.977] | 0.872 [0.854-0.902] | 0.915 [0.894-0.946] |
| 128K | 1.691 [1.670-1.696] | 1.850 [1.845-1.917] | 1.805 [1.722-1.859] | 1.828 [1.821-1.929] |
| 256K | 1.807 [1.662-1.859] | 1.757 [1.665-1.853] | 1.821 [1.721-1.902] | 1.830 [1.763-1.887] |
| 512K | 1.786 [1.734-1.841] | 1.724 [1.693-1.838] | 1.710 [1.622-1.762] | 1.744 [1.629-1.761] |
| 1M | 1.565 [1.522-1.591] | 1.839 [1.811-1.926] | 1.367 [1.360-1.407] | 1.718 [1.631-1.764] |
| 2M | 1.291 [1.204-1.423] | 1.852 [1.774-2.053] | | |
| 4M | 1.678 [1.658-1.847] | 2.107 [2.009-2.266] | | |
| 8M | 1.214 [1.122-1.242] | 1.190 [1.131-1.237] | | |

256K, 512K and 8M plan the same forward in both builds (8M has no row; the arms run to 4M), so their two columns
measure one algorithm twice: their differences are the board's noise. The offset-16 board at 2M to 8M was stopped
after its warm-up to save the machine's time (each repetition plans FFTW at 2M to 8M); board 2 had judged those rows
at both offsets.

## The unchanged rows on the same bits

256K and 512K plan the canonical deep-split-4 in both builds (`ab-board.py --same` on the arm rows' digests, HEAD
the reference, 7 repetitions): 256K 1.000 [0.998-1.005] level, 512K 0.996 [0.994-1.004] level; the two builds
printed the same bits at both sizes in every repetition. Every other measured size changed its plan.

## Verdict

- **Kept:** avx512 runs its own f64 rows under `DeterminismMode::Default`, each faster than the canonical arm at
  both buffer offsets: the standalone-hier at 1K to 4K (1.07x to 1.32x), the deep-split-3 at 8K to 128K (1.03x to
  1.23x; 128K aligned only, level off a line), the deep-split-4 at 1M and 2M (1.18x to 1.41x) and the six-step at
  4M (1.22x to 1.26x). Against MKL (offset 0): 8K from 0.76x to 0.94x, 32K from 0.81x to 0.94x, 1K to 4K from 0.61x
  to 0.68x to 0.72x to 0.75x, 16K 0.80x to 0.84x, 64K 0.92x to 0.93x; 1M from 1.57x to 1.84x, 2M from 1.29x to 1.85x,
  4M from 1.68x to 2.11x. 256K and 512K keep the canonical deep-split-4, level with HEAD on the same bits.
- **Judged:** the paired board decided 8K to 32K (the passes' ranges overlapped) and kept 256K canonical (the
  passes' deep-split-3 loses 4.2% off a line); `gen_fft_level_plans.py --judge` records the four in the plan data.
- **Results:** each row's forward meets 32 eps log2 n against FFTW in the board (every arm gated in the program) and
  the cross-level case's ADR-0092 §5 class against the radix-2 reference and twice it against portable's own plan,
  now walking the f64 case to 4M on this host. The deterministic modes keep the canonical plan, so their bits are
  unchanged on every level.
- **Scope:** one Intel AVX-512 microarchitecture; AMD's (Zen 4 and 5) stays QUAL.1-HW's. f64 stays behind MKL at
  1K to 64K (0.72x to 0.94x aligned): the kernels, not the plan, are the next lever.
