# 2026-10-31 — SIMD-PERF: the fft f32 forward against MKL on the benchmark machine (real AVX-512), first profile

<!-- doc-role: evidence -->
> Dated evidence; counts and results are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).
> Owner row: [SIMD-PERF](../ROADMAP.md#slice-simd-perf); session:
> [2026-10-31](../sessions/2026-10-31-simd-perf-bench-machine-fft-f32-profile.md); method:
> [fast A/B boards](README.md#fast-ab-boards). Never compare these numbers with the i9-14900K boards; ratios only.

- **Machine:** the optimization track's benchmark machine (`ci/bench_vm.py`): GCE spot `c4-standard-8`, Intel Xeon
  Platinum 8581C (Emerald Rapids, 2.30 GHz base), 4 cores with 2 threads each (CPUs n and n+4 share a core), L1d
  48 KiB and L2 2 MiB per core, L3 260 MiB shared with the other tenants of the socket, 29 GiB, Ubuntu 24.04, kernel
  7.0.0-1014-gcp, transparent huge pages `madvise`, the guest PMU on (perf 7.0.14, `perf_event_paranoid` 1).
  **Real AVX-512 hardware** (avx512f/bw/vl, vnni, bf16, fp16, AMX), the first such evidence of the row, beside and never
  mixed with the SDE runs. Census by `scripts/bench-host-setup.sh`. Every process pinned to CPU 3, its sibling CPU 7
  idle; no other job ran on the machine during a board; `build-bench.py host` was not taken for these boards (the
  drivers take it from now on).
- **HEAD:** the engine sources of `06f099ce` (this session changed no engine code), the linux-gcc-release lane tree
  built from the native checkout (GCC 13.3, `-O3`, program LTO; kernel units per level, ADR-0134 §6; CMake 4.4.5,
  pinned Ninja 1.11.1). `bench_fft_f32` with this session's `--planned`, `--sizes` and `--loop` options.
  `CRD_CPU_LEVEL` unset runs the detected level, avx512; `CRD_CPU_LEVEL=avx2` the avx2 units (whose plans are the
  i9-measured level plans; avx512 runs the canonical planner, ADR-0134 §7).
- **Pre-row:** `2f2aed72` exported and configured with its own linux-gcc-release preset by `scripts/build-prerow.sh`
  (its build has the avx2 level only), kept under `~/prerow` on the machine.
- **Peers:** Intel MKL 2020.0.4 (Ubuntu `libmkl-dev` 2020.4.304-4; on this CPU it runs `libmkl_avx512.so`), FFTW
  3.3.10 (`libfftw3-dev` 3.3.10-1ubuntu3, the f64 truth every side is gated against), pocketfft `4d865cba` (pinned).
- **Method:** `scripts/ab_fft_f32.sh` (the level's planned forward and MKL in one process, each timed best of the
  program's repetitions), `scripts/ab_fft_prerow.sh` (`prof_fft_kernel`, the same source on both engines, alternating
  processes), both through `scripts/ab-board.py`: one warm-up repetition discarded, then repetitions until no cell is
  unresolved (5 to 15); a cell is the median of the per-repetition speed ratios [percentile-bootstrap 95%], judged
  against a 2% band. Profiles by `scripts/prof_fft_f32.sh` (`bench_fft_f32 --loop`, one side per process).

## The planned forward against MKL (speed ratio = MKL time / Cerid time; above 1 Cerid is faster)

| n | planned forward (avx512) | MKL ms | Cerid ms | avx512 ratio | MKL ms | Cerid ms | avx2 ratio |
|---|---|---|---|---|---|---|---|
| 1024 | standalone-hier | 0.00057 | 0.00068 | 0.82 [0.81-0.85] | 0.00055 | 0.00092 | 0.60 [0.60-0.62] |
| 2048 | ip4 | 0.00128 | 0.00250 | 0.52 [0.48-0.53] | 0.00125 | 0.00285 | 0.44 [0.43-0.45] |
| 4096 | standalone-hier | 0.00347 | 0.00498 | 0.70 [0.67-0.95] | 0.00341 | 0.00554 | 0.62 [0.61-0.64] |
| 8192 | standalone-hier | 0.00709 | 0.00901 | 0.81 [0.78-0.84] | 0.00810 | 0.01337 | 0.60 [0.60-0.61] |
| 16384 | ip4 | 0.02414 | 0.03126 | 0.78 [0.59-0.83] | 0.02194 | 0.02532 | 0.82 [0.80-0.90] |
| 32768 | ip4 | 0.04667 | 0.05359 | 0.88 [0.85-0.90] | 0.04568 | 0.05644 | 0.78 [0.77-0.84] |
| 65536 | ip4 | 0.09915 | 0.1149 | 0.86 [0.75-0.89] | 0.09991 | 0.1129 | 0.89 [0.88-0.91] |
| 131072 | deep-split-4 | 0.4184 | 0.3219 | 1.30 [1.28-1.34] | 0.4058 | 0.3485 | 1.17 [1.12-1.23] |
| 262144 | deep-split-4 | 1.052 | 0.6944 | 1.51 [1.51-1.94] | 1.068 | 0.8079 | 1.31 [1.26-1.34] |
| 524288 | deep-split-4 | 2.313 | 1.597 | 1.45 [1.44-1.47] | 2.306 | 1.796 | 1.28 [1.27-1.30] |
| 1048576 | tiled-four-step | 4.916 | 3.300 | 1.49 [1.47-1.53] | 4.890 | 2.878 | 1.70 [1.69-1.72] |
| 2097152 | four-step | 11.15 | 16.14 | 0.70 [0.65-0.72] | 10.63 | 11.96 | 0.90 [0.87-0.90] |
| 4194304 | four-step | 26.86 | 38.39 | 0.73 [0.68-0.75] | 27.15 | 29.92 | 0.89 [0.88-0.93] |

Both boards decided every cell after 5 repetitions. Every side passed the ADR-0092 §5 f32 class against the FFTW
f64 truth.

## Every forward the planner offers (one full pass each level, not a board)

One run of `bench_fft_f32` without `--planned` (all arms interleaved with MKL each repetition). Where another arm
beat the planned one:

| n | level | planned (x MKL) | fastest arm (x MKL) |
|---|---|---|---|
| 2048 | avx512 | ip4 0.80 | standalone-hier 0.89 |
| 2048 | avx2 | ip4 0.74 | standalone-hier 0.85 |
| 8192 | avx2 | standalone-hier 0.75 | deep-split-4 0.87 |
| 131072, 262144 | avx2 | deep-split-4 1.15, 1.28 | deep-split-3 1.30, 1.39 |
| 2097152 | avx512 | four-step 0.85 | deep-split-4 1.43 |
| 2097152 | avx2 | four-step 1.07 | deep-split-4 1.53 |
| 4194304 | avx512 | four-step 0.84 | six-step 1.44 |
| 4194304 | avx2 | four-step 0.97 | six-step 1.55 |

At 2M and 4M the order is the i9's reversed: its board 3 data
([f32 arms](2026-10-30-simd-dispatch-fft-f32-arms.json), avx2) has the four-step at 6.5 ms against deep-split-4's
8.5 ms at 2M, and 13.9 ms against six-step's 20.1 ms at 4M. On this machine the four-step costs 63M cycles a
transform against deep-split-4's 35M (profile below). The best forward at these sizes depends on the machine, not the
level, so neither level table nor the canonical planner can hold it as it stands.

## HEAD against the pre-row engine (prof_fft_kernel, f64 forward, ns per transform)

| n | pre-row | HEAD avx512 | ratio | HEAD avx2 | ratio | repetitions |
|---|---|---|---|---|---|---|
| 1024 | 2086 | 1441 | 1.45 [1.43-1.47] faster | 2047 | 1.014 [1.012-1.027] unresolved | 15 |
| 16384 | 51630 | 39830 | 1.29 [1.29-1.30] faster | 51160 | 1.01 [1.00-1.01] level | 7 |
| 131072 | 762800 | 663500 | 1.15 [1.13-1.16] faster | 702600 | 1.08 [1.07-1.10] faster | 5 |
| 1048576 | 8311000 | 6366000 | 1.30 [1.30-1.31] faster | 8336000 | 0.99 [0.98-1.02] level | 6 |

No regression against pre-row on this machine at either level; the 1K avx2 cell is 1.2% to 2.7% faster, inside the
band's upper edge after 15 repetitions.

## Profiles

`prof_fft_f32.sh` at 4096 (400,000 transforms a side, avx512). The `copy` side is the loop's own overhead (the input
copy every transform starts from, as in the board); per transform net of it, the planned forward takes 17.1k cycles
and 27.9k instructions, MKL 9.9k and 21.8k: **1.28x MKL's instructions at 0.74x its IPC** (the table's IPC
includes the copy).

| side | cycles | instructions | IPC | retiring | frontend | backend (memory, core) | hottest symbols |
|---|---|---|---|---|---|---|---|
| planned (standalone-hier) | 7.90G | 11.22G | 1.42 | 24.8% | 10.6% (fetch latency 8.6%) | 64.2% (43.1%, 20.4%) | `codelet64_batched_sc` 45%, `codelet64_stage1_fused_sh_cs` 44%, copy 10% |
| MKL | 5.03G | 8.79G | 1.75 | 30.1% | 1.6% | 67.9% (37.4%, 31.9%) | `rowbatch_twidl_fwd_032` 50%, `colbatch_plain_fwd_16` 16%, `colbatch_twidl_fwd_08` 14%, copy 16% |
| copy | 1.06G | 0.06G | 0.05 | 7.8% | 2.0% | 89.8% (89.3%, 0.0%) | `memmove` 97% |

Top-down level 3 (multiplexed, about 2.6% of the run each): the planned forward is store bound 19.8%, L1 bound
14.6%, L2 bound 10.3% and port-utilization bound 18.6%, MKL 17.7%, 9.1%, 12.1% and 16.3%. The planned forward's code
is delivered by the legacy decoder (`IDQ.MITE_CYCLES_ANY` 2.44G against `IDQ.DSB_CYCLES_OK` 2.7M; icache-miss stalls
6.2%); MKL's by the uop cache (0.98G against 15M). The two radix-64 passes' whole straight-line codelets do not fit
the uop cache, and the stage-1 pass streams the input, the full stage-twiddle table and the AoSoA scratch (about 96 KiB
at 4096 against the 48 KiB L1d).

At 2M (80 transforms a side; every side includes the program's setup, which the copy side also measures), net per
transform: four-step 63M cycles, deep-split-4 35M, MKL 44M.

| side | cycles | instructions | backend (memory) | retiring | hottest symbols |
|---|---|---|---|---|---|
| planned (four-step) | 5.69G | 5.50G | 79.4% (71.0%) | 15.9% | the engine's `execute_four_step` body 50%, codelets 22%, copy 14% |
| deep-split-4 | 3.43G | 3.67G | 75.1% (62.1%) | 18.9% | `codelet32_fused_notr_ss` 31%, `codelet64_batched_strided_sc` 23%, `codelet32_stage1_fused_sh_csf` 13% |
| MKL | 4.16G | 3.36G | 80.7% (66.5%) | 13.7% | `gather_c_c` 32%, `scatter_c_c` 19% |
| copy | 0.63G | 0.51G | 79.1% (66.3%) | 15.4% | `memmove` 52%, setup |

**Not the TLB:** with the default `madvise` the four-step's page walks are active in 376M of its 5.90G cycles (6.4%);
with transparent huge pages set to `always` for the experiment (restored afterwards) its walks fall to 2.5M cycles and
its time is unchanged (18.57 and 18.55 ms a transform with the copy). Its cost is the memory passes of its own body.
Single runs of deep-split-4 moved between 9.2 and 13.9 ms and MKL between 12.6 and 13.0 ms on this machine at a 16
MiB working set (the shared L3 and memory): only interleaved boards judge here.

## The f64 forward against FFTW, MKL and pocketfft (one run, not a board)

`bench_fft_vs_refs` once at avx512 after the boards (the program's best-of-repetitions milliseconds, three decimals;
every side passed its gate, exit 0). Below 64K the three-decimal column cannot rank the sides; from 2M the planned
forward (the canonical four-step at 4M, where the program's own arm rows rank the six-step first) falls to 0.32x to
0.36x of FFTW, the f32 plan problem at a larger cost:

| n | Cerid | FFTW | pocketfft | MKL | FFTW / Cerid | MKL / Cerid |
|---|---|---|---|---|---|---|
| 1024 | 0.002 | 0.001 | 0.004 | 0.001 | - | - |
| 2048 | 0.005 | 0.003 | 0.008 | 0.002 | - | - |
| 4096 | 0.010 | 0.007 | 0.017 | 0.006 | - | - |
| 8192 | 0.022 | 0.016 | 0.042 | 0.016 | - | - |
| 16384 | 0.044 | 0.041 | 0.085 | 0.038 | - | - |
| 32768 | 0.095 | 0.094 | 0.179 | 0.078 | - | - |
| 65536 | 0.230 | 0.239 | 0.442 | 0.199 | 1.04 | 0.87 |
| 131072 | 0.627 | 0.521 | 0.984 | 1.018 | 0.83 | 1.62 |
| 262144 | 1.368 | 1.214 | 1.955 | 2.189 | 0.89 | 1.60 |
| 524288 | 3.076 | 3.890 | 5.280 | 4.603 | 1.26 | 1.50 |
| 1048576 | 6.923 | 8.643 | 10.915 | 10.259 | 1.25 | 1.48 |
| 2097152 | 49.659 | 17.776 | 29.101 | 20.527 | 0.36 | 0.41 |
| 4194304 | 111.534 | 38.740 | 68.637 | 66.368 | 0.35 | 0.60 |
| 8388608 | 266.988 | 84.331 | 204.812 | 99.379 | 0.32 | 0.37 |

## Verdict

On real AVX-512 hardware MKL leads the planned f32 forward at 1K to 64K (0.52x to 0.88x at avx512, 0.44x to 0.89x at
avx2) and at 2M and 4M (0.70x to 0.73x at avx512, 0.89x to 0.90x at avx2); Cerid leads at 128K to 1M (1.17x to
1.70x). Two causes, each with its evidence: **the kernel at 1K to 64K** (1.28x MKL's instructions at 0.74x its IPC at
4096, code from the legacy decoder, a working set twice the L1d), and **a machine-dependent plan at 2K, 2M and 4M**
(another offered forward beats MKL by 1.4x to 1.55x at 2M and 4M here while the i9 ranks it last); the f64
planned forward from 2M runs at 0.32x to 0.36x of FFTW (one run). No engine change this session; HEAD is at or ahead
of the pre-row engine on this machine at both levels.
