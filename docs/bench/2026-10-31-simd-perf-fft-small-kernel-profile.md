# 2026-10-31 — SIMD-PERF: the f32 forward's 2K to 16K kernel against MKL, profiled (real AVX-512)

<!-- doc-role: evidence -->
> Dated evidence; counts and results are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).
> Owner row: [SIMD-PERF](../ROADMAP.md#slice-simd-perf); session:
> [2026-10-31](../sessions/2026-10-31-simd-perf-fft-small-kernel-profile.md); method:
> [fast A/B boards](README.md#fast-ab-boards). Never compare these numbers with the i9-14900K boards; ratios only.

- **Machine:** the optimization track's benchmark machine, as in the
  [first board](2026-10-31-simd-perf-fft-f32-xeon.md): GCE spot `c4-standard-8`, Intel Xeon Platinum 8581C (Emerald
  Rapids), L1d 48 KiB and L2 2 MiB a core, Ubuntu 24.04, kernel 7.0.0-1014-gcp, perf 7.0.14 with the guest PMU.
  **Real AVX-512 hardware.** Every process pinned to CPU 3, its sibling idle, no other job on the machine.
- **HEAD:** the engine of `d91c77f1`, the linux-gcc-release lane tree (GCC 13.3, `-O3`, program LTO),
  `bench_fft_f32` saved as `~/ab/b3-head`. `CRD_CPU_LEVEL=avx512` and `avx2` select the fft units; MKL runs its
  AVX-512 library at both (ADR-0134 §8).
- **Peer:** Intel MKL 2020.0.4 (`libmkl_avx512.so` on this CPU). Every side passed the ADR-0092 §5 f32 class against
  the FFTW f64 truth.
- **Method:** profiles by `scripts/prof_fft_f32.sh` (`bench_fft_f32 --loop`, one side a process; 1,000,000 transforms
  at 2K, 500,000 at 4K, 250,000 at 8K, 100,000 at 16K) with `CRD_PROF_EVENTS` for the decoder, the stores and the L1;
  counts are per transform **net of the copy side** (the input copy every loop transform starts from). Static counts
  from `objdump` of the avx512 and avx2 codelet bodies. Experiments as boards through `scripts/ab-board.py` (two
  builds of `bench_fft_f32`, one warm-up, 5 to 15 repetitions, median paired speed ratio [percentile-bootstrap 95%],
  2% band), each against HEAD's standalone-hier.

## Where the time goes (per transform, net of the copy)

`st` is retired store instructions, `RFO` L2 read-for-ownership requests (`l2_rqsts.all_rfo`), `SB` cycles stalled on
a full store buffer (`resource_stalls.sb`), `MITE` and `DSB` the uops from the legacy decoder and the uop cache, `mem`
top-down memory bound in percent. The planned forward is ip4 at 2K and 16K and standalone-hier at 4K and 8K (the
canonical planner at avx512; the i9's level table at avx2 holds the same arms).

| level | n | side | cycles | instructions | IPC | st | RFO | SB | DSB | MITE | mem |
|---|---|---|---|---|---|---|---|---|---|---|---|
| avx512 | 2048 | ip4 (planned) | 8,320 | 17,518 | 2.11 | 2,615 | 318 | 1,053 | 17,748 | 81 | 21.1 |
| avx512 | 2048 | standalone-hier | 5,713 | 13,004 | 2.28 | 2,039 | 532 | 474 | 4,340 | 8,953 | 25.9 |
| avx512 | 2048 | MKL | 4,280 | 9,895 | 2.31 | 954 | 241 | 0 | 5,610 | 4,438 | 12.9 |
| avx512 | 4096 | standalone-hier (planned) | 15,233 | 27,894 | 1.83 | 4,568 | 1,434 | 4,122 | 3,546 | 24,976 | 43.1 |
| avx512 | 4096 | ip4 | 17,941 | 37,467 | 2.09 | 6,417 | 870 | 795 | 38,003 | 178 | 24.3 |
| avx512 | 4096 | MKL | 9,613 | 21,828 | 2.27 | 1,869 | 1,014 | 11 | 21,123 | 521 | 30.6 |
| avx512 | 8192 | standalone-hier (planned) | 35,544 | 59,270 | 1.67 | 9,996 | 3,697 | 9,783 | 512 | 60,109 | 46.3 |
| avx512 | 8192 | MKL | 34,649 | 44,136 | 1.27 | 3,603 | 4,347 | 17,415 | 43,140 | 791 | 65.1 |
| avx512 | 16384 | ip4 (planned) | 99,933 | 166,301 | 1.66 | 27,430 | 4,427 | 11,453 | 167,348 | 1,222 | 31.1 |
| avx512 | 16384 | standalone-hier | 91,481 | 129,550 | 1.42 | 22,805 | 9,474 | 26,568 | 825 | 131,229 | 41.4 |
| avx512 | 16384 | MKL | 99,458 | 97,721 | 0.98 | 8,053 | 9,776 | 20,017 | 95,503 | 1,526 | 55.7 |
| avx2 | 2048 | ip4 (planned) | 9,551 | 29,332 | 3.07 | 4,288 | 241 | 899 | 28,883 | 220 | 20.3 |
| avx2 | 2048 | standalone-hier | 7,137 | 24,371 | 3.41 | 4,513 | 382 | 570 | 8,261 | 16,202 | 22.3 |
| avx2 | 4096 | standalone-hier (planned) | 15,633 | 51,726 | 3.31 | 9,668 | 1,044 | 1,511 | 8,263 | 43,571 | 28.4 |
| avx2 | 8192 | standalone-hier (planned) | 42,943 | 110,689 | 2.58 | 20,811 | 3,768 | 14,773 | 7,711 | 103,201 | 45.1 |
| avx2 | 16384 | ip4 (planned) | 98,605 | 276,618 | 2.81 | 40,227 | 4,339 | 4,440 | 270,640 | 1,504 | 23.9 |
| avx2 | 2048-16384 | MKL | as avx512 (its AVX-512 library) | | | | | | | | |

The copy side itself takes 2,608 cycles and 514 RFOs at 4K (0 RFOs at 2K: the input and its copy fit the L1 there).

At 4K, attributed by `perf record` on each event (standalone-hier, avx512): cycles stage 2 (`codelet64_batched_sc`)
46.1% and stage 1 (`codelet64_stage1_fused_sh_cs`) 41.8%; store-buffer stalls stage 2 54.5%, stage 1 9.7%, the copy
35.4%; RFOs stage 1 40.7%, stage 2 36.4%; L1 replacements stage 1 47.5%, stage 2 32.5%.

## The codelets (static counts of the f32 bodies)

| codelet | level | bytes | instructions | vector stores (to the stack) | vector loads (from the stack) |
|---|---|---|---|---|---|
| `codelet64_stage1_fused_sh_cs` | avx512 | 22,072-33,576 | 3,389-4,817 | 441-662 (313-407) | 566-660 (312-404) |
| `codelet64_batched_sc` | avx512 | 15,837-18,794 | 2,469-2,673 | 397-413 (269-285) | 376-414 (266-287) |
| `codelet32_batched_sc` | avx512 | 5,852-7,418 | 980-1,114 | 140-154 (76-90) | 121-156 (77-90) |
| `codelet16_stage1_fused_sh_cs` | avx512 | 6,012 | 954 | 94 (30) | 92 (28) |
| `codelet16_fused_notr_ss` | avx512 | 2,414-3,252 | 443-517 | 38-45 (6-13) | 38-44 (6-12) |
| `codelet8_fused_notr_ss` | avx512 | 935-1,341 | 191-228 | 16 (0) | 16 (0) |
| `codelet64_stage1_fused_sh_cs` | avx2 | 23,193-26,830 | 3,704-4,411 | 592-674 (464-546) | 702-761 (448-505) |
| `codelet64_batched_sc` | avx2 | 15,381-16,524 | 2,568-2,774 | 475-482 (347-354) | 436-477 (347-349) |

Ranges cover each codelet's template instantiations (the wide and the tile lane). A radix-64 step at avx512 holds 64
points of two planes in 32 registers, so it spills two stack stores for every data store; the bodies outgrow the uop
cache, and the 4K transform's uops come from the legacy decoder (MITE 24,976 of 28,522).

## The bound

- **4K is a working-set cliff, not a kernel the size of MKL's.** standalone-hier at 4K streams the 32 KiB input, a
  32 KiB AoSoA intermediate and the 32 KiB full stage-1 twiddle table (96 KiB) through a 48 KiB L1d. It issues 1.4
  RFOs a written line (1,434 for 1,024 line writes in its two passes), and stage 2's stores to the caller's lines,
  behind 285 spill stores a step, fill the store buffer: 4,122 of 15,233 cycles, against MKL's 11. MKL runs 4K as
  three in-place passes (`rowbatch_twidl_fwd_032`, `colbatch_plain_fwd_16`, `colbatch_twidl_fwd_08`) over the 32 KiB
  data: 0.66 RFOs a written line, 1,869 stores against 4,568. The size pattern agrees: at 2K the same structure's
  working set (48 KiB) fits the L1 and standalone-hier is level with MKL; at 8K both leave the L1.
- **2K is the plan.** The planned ip4 costs 1.46x standalone-hier's cycles at avx512 and 1.34x at avx2 (17,518
  against 13,004 instructions); MKL runs the same 64 x 32 split as standalone-hier (`rowbatch_twidl_fwd_064`,
  `colbatch_plain_fwd_32`).
- **16K:** the planned ip4 is level with MKL in cycles here (the planned board puts it at 0.78x to 0.90x); the
  standalone-hier is frontend bound (27%, its 128-point codelets from the legacy decoder).

## The levers measured, none kept

Each against HEAD's standalone-hier, interleaved in one board. MKL against the same arm in the same boards: avx512 2K
0.957 [0.951-0.975], 4K 1.348 [1.345-1.373], 8K 1.015 [1.010-1.025]; avx2 2K 1.182 [1.129-1.28], 4K 1.469
[1.448-1.481], 8K 1.144 [1.114-1.17] (above 1 MKL is faster).

| lever | level | 2K | 4K | 8K | bits |
|---|---|---|---|---|---|
| deep-split-3 offered at 1K to 4K (16·8·8, 16·16·8, 16·16·16: the radix-16 and radix-8 bodies that barely spill) | avx512 | 1.027 [1.013-1.033] | 0.995 [0.980-1.003] | (32·16·16) 0.855 [0.846-0.865] | own arm |
| | avx2 | 1.039 [1.038-1.044] | 0.963 [0.953-0.969] | 0.877 [0.870-0.902] | own arm |
| factored stage-1 twiddle from n2 = 64 (10 KiB in place of 32 KiB at 4K) | avx512 | 0.969 [0.951-0.981] | 1.067 [1.051-1.088] | 1.011 [0.980-1.045] | changed |
| | avx2 | 1.005 [0.990-1.021] | 0.993 [0.984-1.012] | 1.052 [1.026-1.075] | changed |
| stage-2 destinations prefetched before each step (4K only) | avx512 | | 1.066 [1.063-1.106] | | same |
| | avx2 | | 0.959 [0.942-0.978] | | same |

15 repetitions for the first two (the maximum; unresolved cells as shown), 5 (avx512) and 11 (avx2) for the
prefetch. The third pass of deep-split-3 costs what its smaller bodies save; the factored table changes the bits, wins
one cell and is what the i9 measured 3% to 6% slower at these n2 (`build_stage1_twiddles`); the prefetch (GCC's
`prefetcht0`, the level flags carry no PRFCHW) helps avx512 and costs avx2, and closes a sixth of the 4K gap. One pass
of every arm at 1K: standalone-hier 1.52x MKL at avx512 and 1.21x at avx2, deep-split-3 1.29x and 1.13x.

## Verdict

No engine change. The 4K gap (standalone-hier 0.68x to 0.74x of MKL) is the out-of-place structure's working set:
an n-sized intermediate and an n-sized twiddle table beside the data, and radix-64 bodies that spill two stores for
every data store. Closing it takes an in-place forward whose only tables are a few KiB: a row pass whose lanes are
rows (the register transpose `transpose_rows`), then column-batched passes, the working set the data and its tables.
At 2K the standalone-hier beats the planned ip4 by 1.3x at both levels, but the i9's July A/B chose ip4 there by 12%
and its October arms cannot rank 1K to 8K (`bench_fft_f32` printed three decimals of a millisecond then; it prints
five since `3e596ed5`), so the plan stays until an i9 board at that resolution exists (the machine-dependent plan
item).
