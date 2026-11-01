# 2026-11-01 — SIMD-PERF: the f32 forward's avx512 plans from the benchmark machine's level board

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-perf).

Row: [SIMD-PERF](../ROADMAP.md#slice-simd-perf), Left item 1 (fft's f32 forward where MKL leads), the previous
session's first Left entry: avx512 ran the canonical planner, whose 32K and 64K plan (IP4-AoS) serves avx2, while
the whole-block stores had put the deep-splits ahead at avx512. Board:
[2026-11-01-simd-perf-fft-avx512-level-plans](../bench/2026-11-01-simd-perf-fft-avx512-level-plans.md).

## CI first

- Manual dispatch 41295647725 (`43963cb2`, the first run covering `bcec86fd` and the previous batch) had passed its
  preflight and was running its first four lanes, 25 queued, when the batch ended; every push run since was
  cancelled by the next push or pending. No completed lane has built `bcec86fd` or later, so there is nothing of this track's to fix
  yet; the next batch reads them first.

## Profile first

- **Where the plan left time.** The level board times every arm the planner offers, so it is the profile of the
  plan choice: at avx512 the canonical IP4-AoS ran 32K and 64K 1.27x and 1.37x slower than the deep-split-4 in
  medians, and the four-step 2M and 4M 1.62x and 1.44x slower. The bounds behind them are recorded: IP4-AoS runs
  1.70x and 1.66x MKL's instructions at 32K and 64K, core bound
  ([profile](2026-11-01-simd-perf-fft-32k-64k-profile.md)); the four-step's own memory passes bound it from 2M
  ([profile](2026-10-31-simd-perf-bench-machine-fft-f32-profile.md)); and the deep-splits' store-buffer bound fell
  with the whole-block stores ([session](2026-11-01-simd-perf-fft-whole-block-stores.md)), which only the avx512
  unit runs. A plan row is the lever: the deep-split's passes stay within the L2 where the four-step's transposes
  stream the whole array.

## Decision reversed: this machine's avx512 rows enter the table

The [2026-10-31 profile session](2026-10-31-simd-perf-bench-machine-fft-f32-profile.md#decisions) had recorded this
Xeon's avx512 rows as evidence only, leaving the table to QUAL.1-HW's per-level boards on AMD and Intel. ADR-0134 §7
itself runs avx512 on the canonical planner "until a board on such a host measures them", and this is such a host:
real AVX-512 hardware, its arms measured on one binary at every size. The rows now come from it, each level's
provenance recorded; an AMD board stays QUAL.1-HW's (one Intel microarchitecture does not stand for every AVX-512
host), and §7 is amended to say so. The QUAL.1-HW row is not edited.

## The change

- **The generator keeps each level's own board** (`feat(fft)`): a level entry in a type's data file now records the
  machine, binary commit and board record that measured it, and the canonical forward of each size as that binary
  printed it, against which its rows' notes are compared (the i9 boards' binaries planned 2K to 16K differently from
  HEAD's). `--import` replaces only the levels its board ran (`--machine`, `--board` beside `--commit`). The two data
  files moved to schema 2 with the i9 boards' provenance; every existing row is unchanged, the notes print each median
  at the precision its program printed. Four repository-tool cases: the import, the canonical refusal, a second
  machine's level merged beside the first's with its own canonical, a level without its provenance refused.
- **The board as a script** (`feat(bench)`): `scripts/board_fft_level_plans.sh` runs every arm at each named level
  in passes through `run-lane-bench.sh` and writes the directory the importer reads; board 3's method, which had been
  a hand loop.
- **The avx512 rows** (`perf(fft)`): five passes at 1K to 4M; the rule (the fastest median whose every pass beat
  every pass of the planned arm) gave the deep-split-4 at 32K, 64K, 2M and 4M and the deep-split-3 at 256K; 128K's
  deep-split-3 was faster in medians but its range overlapped the deep-split-4's, so it stays canonical.
- **The test** (`test(fft)`): the cross-level case walked 1K to 1M; it now also runs each larger size of the table
  at which a level the host runs plans other than canonically, so avx512's 2M and 4M rows are held to the class;
  there it checks every level the host runs (5.1 s on the avx512 arm in release, 1.1 to 1.6 s on the forced arms
  below avx512, which skip those sizes).

## Results

- Each new row against the canonical arm, at offset 0 and 16: 32K 1.258x and 1.261x, 64K 1.348x and 1.351x, 256K
  1.069x and 1.063x, 2M 1.651x and 1.530x, 4M 1.566x and 1.675x, every interval clear of the band.
- The planned forward against MKL: 32K 1.020x and 1.056x (from 0.79x and 0.83x), 64K 0.997x and 1.073x (from 0.75x
  and 0.79x), 2M 2.056x and 1.743x (from 1.22x and 1.11x), 4M 1.832x and 1.541x (from 1.23x and 0.96x); 128K to 1M
  1.58x to 2.08x as before.
- Unchanged sizes against HEAD, the same bits: 1K to 8K level, 16K, 512K and 1M unresolved and not slower.

## Checks

- On the machine (linux-gcc-release, the candidate's tree): the whole tree built with `-k 0`, no failure; CTest's
  `fft|dsp|stft|cwt|conv|nn|simd` cases with the `crd-no-*`, `crd-check*` and diag guards, 914 of 914 in 612 s,
  every `crd-hesap-fft-kernels-at-<level>` arm among them; after the test change, the 100 `hesap-fft` cases.
- Locally: `gen_fft_level_plans.py --check`, the generated-sources record refreshed, the `FftLevelPlans` cases, the
  plan, repository and Allman checks.
- The machine ran 0.71 h in one start and is stopped.

## Left

- f64's avx512 rows: `bench_fft_vs_refs` prints to 1 us, which cannot resolve 4K to 64K; it needs the f32 board's
  precision first, then the same board.
- 1K (0.92x to 0.95x MKL in the level board's passes) and 2K (0.90x to 0.97x) at avx512, the loads' halves where port
  5 has room, the 32-point leaves' spills, and the rest of the 32K and 64K session's list.
