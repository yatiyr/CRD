# 2026-11-01 — SIMD-PERF: the f32 forward's buffer alignment, and the board and loop reconciled (real AVX-512)

<!-- doc-role: evidence -->
> Dated evidence; counts and results are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).
> Owner row: [SIMD-PERF](../ROADMAP.md#slice-simd-perf); session:
> [2026-11-01](../sessions/2026-11-01-simd-perf-fft-alignment.md); method:
> [fast A/B boards](README.md#fast-ab-boards). Never compare these numbers with the i9-14900K boards; ratios only.

- **Machine:** the optimization track's benchmark machine, as in the
  [first board](2026-10-31-simd-perf-fft-f32-xeon.md): GCE spot `c4-standard-8`, Intel Xeon Platinum 8581C (Emerald
  Rapids), L1d 48 KiB and L2 2 MiB a core, Ubuntu 24.04, GCC 13.3, perf with the guest PMU. **Real AVX-512
  hardware.** Every process pinned to CPU 3. One start, 0.47 h.
- **Binaries:** `bench_fft_f32` from the linux-gcc-release lane tree (GCC 13.3, `-O3`, program LTO) at `6b4cf519`
  plus this batch's bench change (`--offset`, `--distribution`; the engine unchanged), saved as `~/ab/b14-cand`.
  `CRD_CPU_LEVEL=avx512` and `avx2` select the fft units.
- **Peer:** Intel MKL 2020.0.4 in the same process, its AVX-512 library at both levels.
- **Method:** the profile stream (`bench_fft_f32 --loop <side> --distribution`: each forward timed alone after its
  untimed input copy, 400,000 transforms at 1K halving to 6,250 at 64K) for the per-transform distribution;
  `prof_fft_f32.sh` with `CRD_PROF_ROUNDS` for the sides' clocks (cycles over task-clock) in alternating rounds;
  `ab_fft_f32.sh` (each timed transform warm, best of 200 a process, paired ratios over five to nine processes,
  percentile-bootstrap 95%, 2% band) with `CRD_AB_OFFSET` for the decisive cells. `--offset B` puts the transformed
  buffer B bytes past a 64-byte boundary; 0 is a cache-line-aligned buffer, 16 a malloc'd one.

## The question: why did the board and the loop disagree?

[The last board](2026-11-01-simd-perf-fft-1k-8k-profile.md) ranked the 8K arms oppositely: the board's best
transform put in-place-3 first (0.91x MKL), the loop's mean the standalone-hier. Three candidates were measured in
turn:

1. **The statistic (best against mean).** Not it. Within one stream the minimum, the 10th percentile, the median and
   the mean order the arms the same way at every size from 1K to 8K, in three alternating rounds.
2. **The clock.** Not the 8K split. The three 8K sides ran at 3.60 to 3.69 GHz in the stream rounds, each within
   0.05 GHz of the others in its round. (At 2K and 4K in-place-3 and MKL ran 3.34 to 3.43 GHz against the
   standalone-hier's 3.61 to 3.70: cycles do not compare across those arms; time does.)
3. **Where the buffer sat.** This is it. The board's process and the loop's allocate in a different order, and the
   loop's `work` came out 16 bytes past a cache line (`0x...f050`); built in the board's order it came out on one
   (`0x...6600`), and the board's times are the aligned stream's. Reordering the loop's allocations alone moved its
   in-place-3 from 11.6 to 7.26 microseconds (minimum) and its standalone-hier from 9.8 to 7.8, while sweeping the buffer by whole cache lines (16 offsets, 0 to 3,840 bytes) left every side within its
   noise: the cache-line offset, not the page offset, is what changed.

## The alignment map at avx512

The stream's minimum and median per transform (microseconds, the copy untimed) at each buffer offset:

| n | side | 0 (aligned) | 8 | 16 | 32 | 16 against 0 (median) |
|---|---|---|---|---|---|---|
| 1K | planned (standalone-hier) | 0.527 / 0.575 | 0.542 / 0.592 | 0.542 / 0.589 | 0.547 / 0.596 | 1.02x |
| 1K | MKL | 0.407 / 0.447 | 0.422 / 0.467 | 0.423 / 0.470 | 0.420 / 0.510 | 1.05x |
| 2K | planned (in-place-3) | 1.043 / 1.145 | 1.103 / 1.214 | 1.103 / 1.218 | 1.082 / 1.193 | 1.06x |
| 2K | standalone-hier | 1.365 / 1.717 | 1.532 / 1.730 | 1.511 / 1.717 | 1.553 / 1.822 | 1.00x |
| 2K | MKL | 0.962 / 1.150 | 0.963 / 1.112 | 1.023 / 1.204 | 0.975 / 1.120 | 1.05x |
| 4K | planned (in-place-3) | 2.242 / 2.478 | 2.461 / 2.715 | 2.471 / 2.743 | 2.366 / 2.616 | 1.11x |
| 4K | standalone-hier | 3.187 / 3.447 | 3.773 / 4.113 | 4.044 / 4.438 | 3.761 / 4.113 | 1.29x |
| 4K | MKL | 2.170 / 2.484 | 2.605 / 3.065 | 2.634 / 3.044 | 2.757 / 3.226 | 1.23x |
| 8K | planned (standalone-hier) | 7.881 / 8.455 | 9.584 / 10.33 | 9.734 / 10.47 | 9.659 / 10.42 | 1.24x |
| 8K | in-place-3 | 7.220 / 8.203 | 11.54 / 12.36 | 11.35 / 12.34 | 10.07 / 10.98 | 1.50x |
| 8K | MKL | 6.554 / 7.429 | 8.756 / 10.92 | 8.687 / 9.508 | 8.727 / 9.527 | 1.28x |
| 16K | planned | 20.34 / 22.73 | 25.61 / 28.58 | 25.63 / 28.57 | 24.59 / 27.31 | 1.26x |
| 16K | MKL | 15.08 / 16.28 | 20.22 / 21.77 | 20.19 / 21.66 | 19.58 / 20.96 | 1.33x |
| 32K | planned | 39.23 / 43.21 | 49.14 / 53.99 | 48.82 / 54.85 | 49.28 / 55.18 | 1.27x |
| 32K | MKL | 31.55 / 35.21 | 43.84 / 46.70 | 42.36 / 47.58 | 42.61 / 47.60 | 1.35x |
| 64K | planned | 92.73 / 103.1 | 116.6 / 128.3 | 116.7 / 128.3 | 109.9 / 121.9 | 1.24x |
| 64K | MKL | 71.82 / 78.84 | 92.43 / 105.0 | 91.17 / 101.2 | 93.80 / 104.5 | 1.28x |

Once the data leaves the L1d (8K and up), every 64-byte vector access of a misaligned buffer touches two lines, and
both libraries lose a fifth to a third. In-place-3 loses most at 8K (1.50x): all three of its passes read and write
the caller's buffer, where the standalone-hier touches it only in its first read and its last write.

## The 8K plan, decided at both alignments

`ab_fft_f32.sh` with `CRD_AB_ARMS="standalone-hier in-place-3"`, the standalone-hier the reference:

| level | offset | standalone-hier ms | in-place-3 ms | in-place-3 speed ratio | MKL against standalone-hier |
|---|---|---|---|---|---|
| avx512 | 0 | 0.00807 | 0.00749 | 1.077 [1.068-1.136] faster | 1.226 [1.192-1.316] |
| avx512 | 16 | 0.00983 | 0.01145 | 0.858 [0.855-0.872] slower | 1.092 [1.081-1.158] |
| avx2 | 0 | 0.01108 | 0.01014 | 1.091 [1.044-1.100] faster | 1.668 [1.621-1.671] |
| avx2 | 16 | 0.01315 | 0.01605 | 0.816 [0.781-0.828] slower | 1.489 [1.471-1.518] |

Every cell is decided, and the two alignments disagree: in-place-3 is 8% to 9% faster on an aligned buffer and 14%
to 18% slower on a malloc'd one. The plan cannot follow the buffer (a forward whose bits changed with its caller's
address would break ADR-0063), so **the f32 8K forward stays the standalone-hier**: its worst case is the better
one. Running in-place-3 on a plan-owned aligned copy keeps its bits but pays two 64 KiB copies (about 1.6
microseconds each in the loop's copy side), more than its aligned lead.

The 2K and 4K plans hold at both alignments (offset 16, in-place-3 the reference): at 2K the standalone-hier runs
0.710x [0.670-0.716] and IP4-AoS 0.635x [0.587-0.640]; at 4K 0.603x [0.594-0.609] and 0.489x [0.482-0.496].

## The planned forward against MKL at both alignments (avx512)

`ab_fft_f32.sh` (the planned forward and MKL each repetition), Cerid's speed ratio against MKL:

| n | offset 0 | offset 16 |
|---|---|---|
| 1K | 0.788 [0.783-0.813] | 0.803 [0.779-0.825] |
| 2K | 0.887 [0.883-0.898] | 0.933 [0.911-0.936] |
| 4K | 0.979 [0.963-0.996] | 1.109 [1.099-1.221] |
| 8K | 0.829 [0.827-0.837] | 0.899 [0.883-0.914] |
| 16K | 0.686 [0.679-0.700] | 0.744 [0.719-0.799] |
| 32K | 0.832 [0.809-0.840] | 0.877 [0.865-0.907] |
| 64K | 0.760 [0.757-0.788] | 0.811 [0.804-0.843] |

MKL loses more to a misaligned buffer than Cerid's planned forwards do, so every gap is narrower at offset 16; the
widest at both alignments is 16K (0.69x aligned).

## Clock in short processes

A stream's clock depends on its length: 2,000 transforms of in-place-3 at 8K ran at 3.00 GHz where 20,000 to
50,000 ran at 3.6 to 3.7 (one board process of about 80 ms averages 3.21 GHz). Short arms are compared only inside
one process or in alternating processes of one length, as the board does.
