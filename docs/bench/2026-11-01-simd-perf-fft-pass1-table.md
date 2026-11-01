# 2026-11-01 — SIMD-PERF: the in-place forward's plans at every level, pass 1's twiddles as one table, and the f32 8K plan (real AVX-512)

<!-- doc-role: evidence -->
> Dated evidence; counts and results are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).
> Owner row: [SIMD-PERF](../ROADMAP.md#slice-simd-perf); session:
> [2026-11-01](../sessions/2026-11-01-simd-perf-fft-pass1-table.md); method:
> [fast A/B boards](README.md#fast-ab-boards). Never compare these numbers with the i9-14900K boards; ratios only.

- **Machine:** the optimization track's benchmark machine, as in the
  [first board](2026-10-31-simd-perf-fft-f32-xeon.md): GCE spot `c4-standard-8`, Intel Xeon Platinum 8581C (Emerald
  Rapids), L1d 48 KiB and L2 2 MiB a core, Ubuntu 24.04, GCC 13.3, perf with the guest PMU. **Real AVX-512
  hardware.** Every process pinned to CPU 3.
- **Binaries:** `bench_fft_f32` from the linux-gcc-release lane tree (GCC 13.3, `-O3`, program LTO), saved as
  `~/ab/b16-head` (HEAD, 5280add1), `~/ab/b16-cand` (pass 1 loading the table at every level and size),
  `~/ab/b16-cand2` (the table where it wins: the change) and `~/ab/b16-cand3` (and the 8K plan). `CRD_CPU_LEVEL`
  selects the fft unit: avx512, avx2, sse2 (an sse4.2 selection runs the sse2 unit) and portable.
- **Peer:** Intel MKL 2020.0.4 in the same process. MKL keeps the host's ISA under a forced level (ADR-0134 §8), so
  at sse4.2, sse2 and portable it runs its AVX-512 library: its 12x to 30x lead there is the ISA, not a Cerid defect,
  and those columns are left out below.
- **Method:** `ab_fft_f32.sh` with `CRD_AB_ARMS` (one build's forwards a process, each timed transform warm, best of
  200, paired ratios over five to fifteen processes, percentile-bootstrap 95%, 2% band) and `CRD_AB_OFFSET` 0 (a
  cache-line-aligned buffer) and 16 (a malloc'd one); `ab-board.py` directly for two builds' in-place-3 rows under
  `--same` (their bits checked equal every repetition); `prof_fft_f32.sh` (top-down levels 1 and 2, store-buffer,
  L1d, RFO and port counts, `perf record`'s hottest symbols; counts per transform, the input copy included).

## The in-place plans below avx2 (HEAD)

The canonical plan runs under the deterministic modes at every level, and the 2K, 4K and 16K flips to in-place-3
were judged at avx512 and avx2 only. HEAD's in-place-3 against the other arms; above 1 the arm is faster:

| level, offset | arm | 2K | 4K | 8K | 16K |
|---|---|---|---|---|---|
| sse4.2, 0 | standalone-hier | 1.066 [1.064-1.069] | 1.039 [1.038-1.040] | 1.115 [1.115-1.116] | 1.118 [1.118-1.119] |
| sse4.2, 0 | ip4 | 1.065 [1.065-1.069] | 0.905 [0.904-0.905] | 0.958 [0.957-0.959] | 0.921 [0.921-0.923] |
| sse2, 0 | standalone-hier | 1.065 [1.063-1.065] | 1.039 [1.037-1.039] | 1.116 [1.115-1.117] | 1.115 [1.112-1.118] |
| sse2, 0 | ip4 | 1.065 [1.063-1.066] | 0.904 [0.902-0.905] | 0.958 [0.957-0.959] | 0.921 [0.920-0.921] |
| sse2, 16 | standalone-hier | 1.065 [1.053-1.068] | 1.038 [1.033-1.039] | 1.126 [1.125-1.127] | 1.120 [1.117-1.121] |
| sse2, 16 | ip4 | 1.066 [1.060-1.068] | 0.904 [0.902-0.905] | 0.972 [0.971-0.973] | 0.931 [0.930-0.931] |
| portable, 0 | standalone-hier | 0.982 [0.980-0.984] | 0.992 [0.991-0.994] | 0.888 [0.884-0.889] | 1.067 [1.064-1.070] |
| portable, 0 | ip4 | 1.071 [1.069-1.072] | 0.974 [0.974-0.976] | 0.955 [0.955-0.959] | 1.137 [1.133-1.142] |
| portable, 16 | standalone-hier | 0.983 [0.981-0.986] | 0.993 [0.991-0.996] | 0.890 [0.887-0.899] | 1.066 [1.060-1.066] |
| portable, 16 | ip4 | 1.072 [1.067-1.075] | 0.976 [0.974-0.978] | 0.958 [0.954-0.969] | 1.137 [1.132-1.137] |

Against the plans they replaced (2K IP4-AoS, 4K the standalone-hier, 16K IP4-AoS), the flips cost the
deterministic modes 6% at 2K and 4% at 4K on the sse2 unit and 7% at 2K and 12% at 16K at portable: a regression
the earlier boards did not see. (Under Default these levels run their own board rows, Stockham and the four-step.)

## Where the time goes

The sse2 unit, aligned, per transform:

| n | side | cycles | instructions | hottest |
|---|---|---|---|---|
| 2K | in-place-3 | 109,700 | 329,600 | pass 1 61% (its three parts), pass 2 24%, pass 3 10%, transpose 2% |
| 2K | ip4 | 103,400 | 332,700 | `execute_ip4aos` |
| 8K | in-place-3 | 553,000 | 1,531,800 | pass 1 47%, pass 2 35%, pass 3 9% |
| 8K | standalone-hier | 496,600 | 1,250,300 | stage 1 55%, the batched leaf 40% |

Pass 1 is the only pass that forms its twiddles: each output's W_n^{t·ka} from the factored roots (a broadcast of
hi, a load of lo for each part, two products and two fused operations), where pass 2 loads its own. The sse2 unit
computes every fused operation exactly from dozens of SSE2 instructions (`fma_exact.hpp`), so pass 1 pays four
emulated fused operations an output against pass 2's two. The plan can hold the products themselves: an entry
rounded as the lane rounds it (a rounded product, then one fused operation) is the same bits the lane computed.

avx512 at 8K (aligned, then 16 bytes off a line), for the 8K plan and the next lever:

| offset | side | cycles | instructions | top-down | store-buffer stalls | hottest |
|---|---|---|---|---|---|---|
| 0 | in-place-3 | 35,600 | 58,300 | memory 52.3%, core 16.0% | 19,100 | pass 1 37%, pass 2 18%, pass 3 15%, transpose 9%, copy 15% |
| 0 | standalone-hier | 37,900 | 60,200 | memory 37.0%, core 14.2% | 12,800 | stage 1 45%, batched leaf 39% |
| 0 | MKL | 30,900 | 45,200 | memory 58.8%, core 15.3% | 16,500 | `rowbatch_twidl_fwd_032` 36%, `colbatch_twidl_fwd_16` 29%, `colbatch_plain_fwd_16` 14% |
| 16 | in-place-3 | 46,600 | 57,800 | memory 61.6% | 24,400 | pass 1 (to the work buffer) 25%, transpose 22%, pass 2 18%, pass 3 14% |
| 16 | standalone-hier | 46,700 | 60,200 | frontend 14.5%, memory 45.9% | 15,600 | stage 1 48%, batched leaf 37% |
| 16 | MKL | 40,500 | 45,200 | memory 62.3% | 18,200 | the same three passes |

MKL runs 8K as 32·16·16 with its first pass over contiguous rows of 32 and no separate transposition; Cerid's
16·32·16 transposes each pass-3 group (9% aligned, 22% off a line) and runs 1.29x MKL's instructions.

## Pass 1 from one table

HEAD against `b16-cand` (pass 1 loading the table at every level and size), in-place-3 rows, the same bits every
repetition at every size and level:

| level | offset | 2K | 4K | 8K | 16K |
|---|---|---|---|---|---|
| avx512 | 0 | 1.046 [1.046-1.054] | 0.983 [0.943-1.000] | 1.008 [1.005-1.071] | 1.024 [1.004-1.049] |
| avx512 | 16 | 1.028 [0.993-1.035] | **0.956 [0.949-0.962]** | 1.021 [0.982-1.051] | 1.009 [1.005-1.016] |
| avx2 | 0 | 1.078 [1.073-1.092] | 1.037 [1.011-1.078] | 1.016 [1.002-1.030] | 0.978 [0.964-0.985] |
| avx2 | 16 | 1.065 [1.022-1.083] | 1.002 [0.970-1.034] | 1.032 [1.022-1.056] | 1.026 [1.011-1.044] |

On the levels with fused vector operations the table wins at 2K, where its 15 KiB and the data's 16 KiB share the
L1d, and loses at 4K off a line (30 KiB beside 32 KiB); from 8K the two are level. So the change keeps both: the
plan holds the factored roots and the table (15 KiB at 2K to 120 KiB at 16K), and pass 1 loads the table on the sse2
unit and the portable level at every size and at 2K (`kInPlace3TabledTo`) on avx2, avx512 and NEON, forming the
products from the roots above it (`b16-cand2`, HEAD the reference, the same bits every repetition):

| level | offset | 2K | 4K | 8K | 16K |
|---|---|---|---|---|---|
| avx512 | 0 | 1.046 [1.046-1.053] | 0.993 [0.976-1.028] | 1.014 [0.978-1.037] | 1.000 [0.989-1.030] |
| avx512 | 16 | 1.021 [1.014-1.021] | 1.000 [0.966-1.016] | 0.998 [0.950-1.007] | 1.007 [0.988-1.018] |
| avx2 | 0 | 1.073 [1.068-1.131] | 0.989 [0.977-0.998] | 0.995 [0.973-1.011] | 1.005 [0.990-1.014] |
| avx2 | 16 | 1.073 [1.065-1.100] | 0.985 [0.967-1.044] | 1.015 [1.011-1.020] | 0.999 [0.993-1.006] |
| sse2 | 0 | **1.379 [1.300-1.472]** | **1.350 [1.269-1.350]** | **1.285 [1.284-1.288]** | **1.258 [1.256-1.258]** |
| sse2 | 16 | **1.381 [1.302-1.472]** | **1.349 [1.348-1.351]** | **1.286 [1.284-1.288]** | **1.258 [1.257-1.259]** |
| portable | 0 | **1.282 [1.205-1.366]** | **1.255 [1.250-1.258]** | **1.232 [1.225-1.239]** | **1.189 [1.189-1.192]** |
| portable | 16 | **1.281 [1.268-1.364]** | **1.255 [1.251-1.264]** | **1.234 [1.229-1.243]** | **1.191 [1.190-1.194]** |

From 4K at avx2 and avx512 the change runs HEAD's code, so those cells are the board's noise (none outside 0.97 to
1.03 but avx2 4K aligned, 0.989 [0.977-0.998], inside the 2% band). The portable level wins too although the host's
glibc fuses its `fma`: it computes a step a column at a time, where the products cost more than two loads.

The in-place passes against the other arms with the change (`b16-cand2`), in-place-3 the reference:

| level, offset | arm | 2K | 4K | 8K | 16K |
|---|---|---|---|---|---|
| sse2, 0 | standalone-hier | 0.771 [0.771-0.772] | 0.770 [0.770-0.771] | 0.867 [0.866-0.867] | 0.889 [0.887-0.890] |
| sse2, 0 | ip4 | 0.769 [0.765-0.772] | 0.670 [0.669-0.672] | 0.745 [0.745-0.746] | 0.732 [0.732-0.733] |
| sse2, 16 | standalone-hier | 0.770 [0.769-0.772] | 0.769 [0.768-0.770] | 0.875 [0.875-0.877] | 0.892 [0.891-0.892] |
| sse2, 16 | ip4 | 0.770 [0.768-0.772] | 0.670 [0.669-0.670] | 0.756 [0.755-0.757] | 0.741 [0.739-0.742] |
| portable, 0 | standalone-hier | 0.766 [0.725-0.768] | 0.792 [0.790-0.797] | 0.716 [0.716-0.717] | 0.895 [0.892-0.896] |
| portable, 0 | ip4 | 0.836 [0.836-0.839] | 0.778 [0.776-0.782] | 0.775 [0.773-0.776] | 0.955 [0.953-0.956] |
| portable, 16 | standalone-hier | 0.766 [0.765-0.768] | 0.790 [0.790-0.795] | 0.717 [0.715-0.718] | 0.895 [0.893-0.896] |
| portable, 16 | ip4 | 0.835 [0.835-0.838] | 0.776 [0.774-0.781] | 0.776 [0.774-0.778] | 0.955 [0.953-0.956] |

In-place-3 now leads every other arm at every size and offset on the sse2 unit and the portable level, so the 2K,
4K and 16K flips no longer cost the deterministic modes there: against the plans they replaced, the sse2 unit gains
1.30x at 2K, 1.30x at 4K and 1.37x at 16K, the portable level 1.20x, 1.26x and 1.05x.

## The 8K plan

The dedicated board, HEAD's two arms (the standalone-hier, as planned, the reference) and MKL, nine to fifteen
processes:

| level | offset | in-place-3 | MKL |
|---|---|---|---|
| avx512 | 0 | 1.060 [1.033-1.074] faster | 1.186 [1.164-1.205] |
| avx512 | 16 | 1.008 [0.986-1.019] level | 1.103 [1.085-1.108] |
| avx2 | 0 | 1.083 [1.059-1.098] faster | 1.678 [1.666-1.765] |
| avx2 | 16 | 0.989 [0.962-1.010] unresolved | 1.494 [1.487-1.514] |

The 16K batch's avx2 cell off a line (0.96x) read level here. With pass 1 loading its table, in-place-3 runs 8K
1.15x the standalone-hier on the sse2 unit (1.12x at HEAD the other way) and 1.40x at portable (the table above), so
the 8K canonical plan is in-place-3: avx512 runs it under Default (0.894x MKL aligned and 0.914x off a line, from
0.843x and 0.907x), every level under the deterministic modes; avx2's Default row from the i9's board keeps the
standalone-hier.

## HEAD against the change where the plans agree

`CRD_AB_HEAD=b16-head`, `CRD_AB_CANDIDATE=b16-cand3`, the planned forwards (8K, whose plan changed, left out), the
same bits every repetition:

| level | offset | 1K | 2K | 4K | 16K | 32K | 64K |
|---|---|---|---|---|---|---|---|
| avx512 | 0 | 1.000 [0.942-1.062] | 1.053 [1.047-1.087] | 0.997 [0.960-1.035] | 0.987 [0.981-1.006] | 0.998 [0.993-1.002] | 0.995 [0.992-1.002] |
| avx512 | 16 | 1.027 [0.973-1.044] | 1.007 [0.993-1.049] | 1.000 [0.967-1.019] | 0.994 [0.964-1.002] | 1.003 [0.999-1.009] | 0.999 [0.994-1.005] |
| avx2 | 0 | 1.000 [0.966-1.011] | 1.007 [0.979-1.022] | 1.000 [0.975-1.049] | 1.002 [0.971-1.014] | 0.996 [0.987-1.007] | 0.999 [0.997-1.005] |
| avx2 | 16 | 1.000 [0.967-1.044] | 0.987 [0.971-1.027] | 0.980 [0.941-1.039] | 1.006 [0.994-1.012] | 1.001 [1.001-1.003] | 0.997 [0.994-1.001] |

No cell regresses beyond the band (avx2's Default 2K row is IP4-AoS, so its planned 2K is HEAD's code). avx512's
planned 2K is 1.05x faster aligned.

## The sse2 unit's own rows

Under Default the sse2 unit runs its board rows from the i9 (2K and 8K the four-step, 4K and 16K Stockham). With
pass 1's table, in-place-3 against those arms on this machine (`b16-cand3`, in-place-3 the reference):

| offset | arm | 2K | 4K | 8K | 16K |
|---|---|---|---|---|---|
| 0 | stockham | 1.032 [1.027-1.035] | 1.344 [1.320-1.351] | 1.126 [1.126-1.132] | 1.234 [1.225-1.237] |
| 0 | four-step | 0.897 [0.892-0.908] | 0.893 [0.892-0.894] | 0.916 [0.915-0.921] | 0.879 [0.878-0.881] |
| 16 | stockham | 1.034 [1.032-1.036] | 1.340 [1.334-1.348] | 1.143 [1.142-1.145] | 1.247 [1.239-1.253] |
| 16 | four-step | 0.901 [0.891-0.908] | 0.887 [0.881-0.889] | 0.929 [0.928-0.936] | 0.886 [0.878-0.889] |

Stockham leads at every size here, while the rows plan the four-step at 2K and 8K (in-place-3 1.11x and 1.09x it
now): the sse2 and portable rows are due a re-board on this machine, a next item.

## Checks

- The bits of every change against HEAD: equal at every size, level and offset, every repetition (`--same`).
- On the machine (linux-gcc-release): the fft suite, 146 of 146 CTest cases, every `crd-hesap-fft-kernels-at-<level>`
  arm among them, and a new test that the pass-1 table holds the factored roots' products bit for bit, within four
  f32 ulps of each root; the whole tree built (`-k 0`, no failure) and the 20 `crd-no-*`, `crd-check*` and diag
  registry guards passed, `crd-check-isa-isolation` included.
- The machine ran 1.53 h in one start and is stopped.
