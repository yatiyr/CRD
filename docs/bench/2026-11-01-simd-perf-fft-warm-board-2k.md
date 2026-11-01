# 2026-11-01 — SIMD-PERF: the fft f32 board's warm transforms and the 2K plan (real AVX-512)

<!-- doc-role: evidence -->
> Dated evidence; counts and results are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).
> Owner row: [SIMD-PERF](../ROADMAP.md#slice-simd-perf); session:
> [2026-11-01](../sessions/2026-11-01-simd-perf-fft-warm-board-2k.md); method:
> [fast A/B boards](README.md#fast-ab-boards). Never compare these numbers with the i9-14900K boards; ratios only.

- **Machine:** the optimization track's benchmark machine, as in the
  [first board](2026-10-31-simd-perf-fft-f32-xeon.md): GCE spot `c4-standard-8`, Intel Xeon Platinum 8581C (Emerald
  Rapids), L1d 48 KiB and L2 2 MiB a core, Ubuntu 24.04, GCC 13.3, perf with the guest PMU. **Real AVX-512
  hardware.** Every process pinned to CPU 3; the host idle (`build-bench.py host`: 28.7 of 29.4 GiB available, no
  other process above 0.0 cores). One start, 0.46 h.
- **Binaries:** `bench_fft_f32` from the linux-gcc-release lane tree (GCC 13.3, `-O3`, program LTO): HEAD `8a243a34`
  (`~/ab/b11-head`, each timed transform cold), HEAD's engine with the warm driver (`~/ab/b11-warm`) and the kept
  change (`~/ab/b11-cand`). `CRD_CPU_LEVEL=avx512` and `avx2` select the fft units.
- **Peer:** Intel MKL 2020.0.4, timed in the same process every repetition; it runs its AVX-512 library
  (`libmkl_avx512.so`) at both levels (a peer runs at the host's best, `crd_peer_isa_flags`). Every arm passed the
  ADR-0092 §5 f32 class against the FFTW f64 truth of the same input.
- **Method:** `scripts/ab_fft_f32.sh` and `scripts/ab-board.py`, one warm-up, rotated order, median paired speed ratio
  [percentile-bootstrap 95%], 2% band, 7 to 15 repetitions. The profile loop (`bench_fft_f32 --loop`, three processes
  a side, 300,000 transforms at 1K and 2K and 100,000 at 4K and 8K, means net of the `copy` side's) is the steady-state
  reference; `scripts/prof_fft_f32.sh` adds `perf stat` top-down levels 1 and 2 and `perf record` (200,000
  transforms).

## The board's cold transforms

`bench_fft_f32`'s board timed each arm's transform right after the other arms' and MKL's (five other forwards at 2K
before an arm came round again), so the arm's tables and code came back from where the others had left them, while
the profile loop runs one side's transforms back to back. The two ranked the 2K arms in opposite orders. Each timed
transform now follows an untimed transform of the same arm (and MKL's of MKL), from a fresh copy of the input:

| level | 2K arm against in-place-3 | cold board (HEAD) | warm board | profile loop (net of the copy) |
|---|---|---|---|---|
| avx512 | standalone-hier | 1.096 [1.066-1.112] | 0.806 [0.791-0.823] | 0.783 (2.088 against 1.635 µs) |
| avx512 | ip4 (the plan) | 0.912 [0.881-0.929] | 0.714 [0.709-0.718] | 0.638 (2.562 µs) |
| avx512 | MKL | 1.466 [1.418-1.500] | 1.223 [1.214-1.244] | 1.175 (1.391 µs) |
| avx2 | standalone-hier | 1.119 [1.088-1.161] | 0.881 [0.876-0.919] | 0.955 (2.585 against 2.469 µs) |
| avx2 | ip4 (the plan) | 1.018 [0.989-1.033] | 0.770 [0.766-0.778] | 0.777 (3.179 µs) |
| avx2 | MKL | 1.977 [1.898-2.131] | 1.654 [1.643-1.685] | 1.741 (1.418 µs) |

Each cell is the arm's speed against in-place-3's (above 1 the arm is faster). The warm board agrees with the loop
in every ranking; the cold board put the standalone-hier first. The same effect had flattered Cerid at 1K: on the
cold full board MKL, timed after every Cerid arm, took 0.00099 ms against 0.00061 ms warm (the earlier boards' "1K
1.37x MKL" at avx512); the two-sided `--planned` board was barely affected (1K 0.847x cold, 0.818x warm).

The `--planned` board against MKL at avx512, cold and warm (HEAD's plans; the warm cells are the new reference):

| n | cold | warm |
|---|---|---|
| 1K | 0.847 [0.809-0.861] | 0.818 [0.806-0.831] |
| 2K | 0.518 [0.516-0.521] | 0.532 [0.525-0.540] |
| 4K | 1.003 [0.983-1.011] | 1.012 [0.974-1.032] |
| 8K | 0.831 [0.819-0.836] | 0.826 [0.824-0.836] |
| 16K | 0.779 [0.752-0.829] | 0.781 [0.755-0.798] |
| 32K | 0.883 [0.852-0.909] | 0.851 [0.836-0.871] |
| 64K | 0.886 [0.858-0.893] | 0.840 [0.819-0.854] |

At avx2 (HEAD's plans, warm): 1K 0.629, 2K 0.453, 4K 0.630, 8K 0.596, 16K 0.791, 32K 0.722, 64K 0.870 of MKL's
AVX-512 library.

## The 2K profile

`prof_fft_f32.sh 11`, cycles and instructions a transform with the copy (about 360 cycles of it), top-down level 2:

| side | avx512 cycles | instructions | memory bound | core bound | avx2 cycles | instructions |
|---|---|---|---|---|---|---|
| in-place-3 | 5,506 | 14,187 | 7.1% | 41.2% | 9,052 | 28,357 |
| standalone-hier | 7,577 | 13,252 | 38.4% | 27.8% | 8,890 | 24,621 |
| MKL (AVX-512) | 4,860 | 10,162 | 12.9% | 46.7% | 4,862 | 10,167 |
| MKL (`MKL_ENABLE_INSTRUCTIONS=AVX2`) | | | | | 6,813 | 18,896 |

In steady state the standalone-hier's input, AoSoA intermediate and table hold 38% of its slots on memory at 2K, as
at 4K; in-place-3 is core-bound at 1.40x MKL's instructions, its samples in pass 1 (28%), the engine's group
transposes (`Engine::execute`, 24%), pass 3 (19%) and pass 2 (17%). With MKL held to its AVX2 library the avx2
in-place-3 runs 0.75x it at 2K and 1.09x at 4K (21,311 against 19,636 cycles); 8K at 0.64x (the in-place-3 loop) and
0.76x (the standalone-hier).

## The kept change: the canonical f32 2K forward is in-place-3

HEAD's and the candidate's `--planned` forwards (both on the warm driver), avx512 runs the canonical planner:

| level | n | HEAD ms | candidate ms | speed ratio |
|---|---|---|---|---|
| avx512 | 1K | 0.00071 | 0.00070 | 1.000 [0.986-1.014] level |
| avx512 | 2K | 0.00246 | 0.00163 | **1.497 [1.488-1.541] faster** |
| avx2 | 1K | 0.00089 | 0.00089 | 0.989 [0.978-1.000] unresolved |
| avx2 | 2K | 0.00296 | 0.00297 | 0.993 [0.990-1.014] level (the per-level row, unchanged) |

The 2K forward is now 0.795x MKL at avx512 (0.789-0.851; it was 0.532x). At 4K to 64K HEAD and the candidate print
the same bits at both levels (the board's `--same` digests) and are level or unresolved (0.997 to 1.009). The avx2
per-level row keeps IP4-AoS at 2K (from the i9's board, the machine-dependent plan item), where in-place-3 runs 1.30x
it here.

## Open

- **2K and 4K at avx512, the rest to MKL:** the group transposes (24% and 21% of the samples) and 1.40x and 1.31x
  MKL's instructions.
- **8K:** the warm board's minimum has in-place-3 level with the standalone-hier at avx512, the loop's mean 1.24x
  slower with the copy (17.3 against 14.0 µs; 64 KiB leaves the L1d, the copy streams from the L2); the plan stays.
- **The avx2 per-level rows at 2K, 4K and 8K** wait on the i9 re-board with the in-place-3 arm.
