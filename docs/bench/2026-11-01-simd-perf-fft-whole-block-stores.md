# 2026-11-01 — SIMD-PERF: the f32 and f64 forwards' AoSoA blocks stored whole at avx512 (real AVX-512)

<!-- doc-role: evidence -->
> Dated evidence; counts and results are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).
> Owner row: [SIMD-PERF](../ROADMAP.md#slice-simd-perf); session:
> [2026-11-01](../sessions/2026-11-01-simd-perf-fft-whole-block-stores.md); method:
> [fast A/B boards](README.md#fast-ab-boards). Never compare these numbers with the i9-14900K boards; ratios only.

- **Machine:** the optimization track's benchmark machine, as in the
  [first board](2026-10-31-simd-perf-fft-f32-xeon.md): GCE spot `c4-standard-8`, Intel Xeon Platinum 8581C (Emerald
  Rapids), L1d 48 KiB and L2 2 MiB a core, Ubuntu 24.04, GCC 13.3, perf with the guest PMU. **Real AVX-512
  hardware.** Every process pinned to CPU 3. One start.
- **Binaries:** `bench_fft_f32` and `bench_fft_vs_refs` from the linux-gcc-release lane tree (GCC 13.3, `-O3`,
  program LTO): HEAD (`8311a54e`, `~/ab/b19-head`, `~/ab/b19-head-f64`) and three candidates, one source change each:
  **c1** loads and stores a paired lane's two blocks whole (two whole registers, their halves joined by lane
  shuffles); **c2** stores them whole and keeps the half loads; **c3**, kept (the session's `perf(fft)` commit),
  stores them whole where the data leaves the L1d and keeps each half's own store in the 2K and 4K in-place
  codelets and the full-table stage 1, and stores stage 1's transposed groups whole in its factored edition and the
  tiled four-step's P1.
  `CRD_CPU_LEVEL=avx512` selects the fft unit.
- **Peers:** Intel MKL 2020.0.4 in the f32 program; FFTW 3 (`FFTW_MEASURE`) gates the f64 program.
- **Method:** `scripts/ab_fft_f32.sh` with `CRD_AB_HEAD` and `CRD_AB_CANDIDATE` (the planned forward of both builds
  and MKL from one process a build a repetition, `--same` digests: the two builds printed the same bits at every
  size of every repetition on every f32 board), at `CRD_AB_OFFSET` 0 (a 64-byte boundary) and 16 (a malloc'd
  buffer); each timed transform warm, best of 200 a process, one warm-up, median paired ratio [percentile-bootstrap
  95%], 2% band, at most 9 repetitions (21 on the 1K to 4K board). The f64 board runs `ab-board.py` on both builds'
  `bench_fft_vs_refs` (its planned forward, best of 200 to 8 a size), at most 7 repetitions. Counts through
  `scripts/prof_fft_f32.sh` (the profile loop, per transform, the input copy included), and the codelets' store
  instructions read from `objdump` of each build.

## What bounded the passes

At avx512 a Lane holds two tiles: a `Vec16f` real part is two blocks' real halves. HEAD loaded it as a half load and
an insert from memory and stored it as a half store and an extract to memory, so a step of the 32-point deep-split
pass 2 (`codelet32_fused_notr_ss`, 16 columns) issued 221 store instructions: 128 for its 64 output registers, 79
spills and 14 others. From 8K the store buffer was full for a large share of the cycles (HEAD, per transform):

| n | side | cycles | stores | `resource_stalls.sb` | port 5 | memory bound |
|---|---|---|---|---|---|---|
| 8K | planned (in-place-3) | 35,417 | 9,634 | 18,481 | 19,877 | 53.1% |
| 32K | deep-split-3 | 178,737 | 42,262 | 76,679 | 90,076 | 59.8% |
| 128K | planned (deep-split-4) | 1,209,871 | 182,184 | 634,648 | 435,712 | 68.5% |
| 128K | deep-split-3 | 1,237,198 | 204,399 | 760,261 | 403,631 | 69.6% |

A block of 8 real then 8 imaginary parts is one register's halves, so the two blocks of a step's output can be
stored as two whole registers: the parts' low halves joined by an insert (port 0 or 5) and their high halves by a
256-bit lane shuffle (port 5), half the stores. In the L1d (2K, 4K) port 5 bounds the passes instead: at 2K HEAD
dispatched 4,006 port-5 uops in 4,221 cycles a transform.

## c1: whole loads and whole stores (not kept)

| n | offset 0 | offset 16 |
|---|---|---|
| 1K | 0.983 [0.950-1.035] unresolved | 0.967 [0.934-1.017] unresolved |
| 2K | 0.891 [0.868-0.897] slower | 0.942 [0.902-0.967] slower |
| 4K | 0.927 [0.910-0.941] slower | 0.936 [0.913-0.949] slower |
| 8K | 1.200 [1.163-1.226] faster | 1.168 [1.111-1.228] faster |
| 16K | 1.132 [1.114-1.141] faster | 1.154 [1.144-1.173] faster |
| 32K, 64K (IP4-AoS) | level, unresolved | level, unresolved |
| 128K | 1.214 [1.189-1.245] faster | 1.188 [1.149-1.228] faster |
| 256K | 1.180 [1.170-1.228] faster | 1.145 [1.136-1.156] faster |
| 512K | 1.205 [1.172-1.238] faster | 1.146 [1.138-1.165] faster |
| 1M | 1.159 [1.121-1.173] faster | 1.097 [1.086-1.101] faster |
| 2M, 4M (four-step) | level | level |

At 2K the transform took 4,706 cycles and 4,545 port-5 uops (from 4,221 and 4,006): a whole load's lane shuffle
moves the inserts (port 0 or 5) onto port 5 alone, and the stores' joins add to it.

## c2: whole stores, half loads (not kept)

| n | offset 0 | offset 16 |
|---|---|---|
| 1K | 1.019 [0.982-1.056] unresolved | 1.000 [0.983-1.018] level |
| 2K | 0.942 [0.941-0.970] slower | 1.007 [0.966-1.020] unresolved |
| 4K | 0.948 [0.943-0.967] slower | 0.988 [0.946-0.994] unresolved |
| 8K | 1.217 [1.154-1.269] faster | 1.133 [1.096-1.213] faster |
| 16K | 1.174 [1.155-1.180] faster | 1.127 [1.119-1.160] faster |
| 128K | 1.212 [1.199-1.260] faster | 1.163 [1.149-1.192] faster |
| 256K | 1.175 [1.170-1.179] faster | 1.158 [1.147-1.171] faster |
| 512K | 1.193 [1.190-1.200] faster | 1.166 [1.158-1.171] faster |
| 1M | 1.134 [1.102-1.174] faster | 1.134 [1.122-1.151] faster |

Per symbol at 4K (100,000 transforms): pass 1 2,978 to 3,164 cycles a transform and the 16-point pass 2 2,233 to
2,485, the store joins on port 5; at 2K pass 1 (`_tw`) and the 8-point pass 2 the same way.

## c3: whole stores past the L1d (kept)

The 2K and 4K in-place codelets keep each half's own store (`store_block_halves`: the tabled pass 1, which runs in
place at 2K, a new `codelet16_inplace_first_halves` the 4K plan runs, and pass 2 at B = 8 and 16), so their code is
HEAD's; from 8K the blocks store whole. Stage 1's transposed store now takes a group's real and imaginary parts
together and stores each column's block whole in the factored edition (n2 of 1024 and up) and the tiled four-step's
P1; the full-table edition (the 1K standalone-hier, small n2) keeps the halves.

| n | offset 0 | offset 16 |
|---|---|---|
| 1K | 1.000 [0.982-1.000] level | 1.000 [0.966-1.018] unresolved |
| 2K | 1.000 [1.000-1.000] level | 1.000 [0.973-1.007] unresolved |
| 4K | 0.997 [0.990-1.000] level | 0.985 [0.966-1.009] unresolved |
| 8K | 1.206 [1.170-1.243] faster | 1.135 [1.100-1.185] faster |
| 16K | 1.173 [1.129-1.186] faster | 1.133 [1.129-1.166] faster |
| 32K (IP4-AoS) | 1.007 [0.999-1.036] unresolved | 1.002 [0.999-1.004] level |
| 64K (IP4-AoS) | 1.008 [0.998-1.020] unresolved | 1.009 [0.995-1.023] unresolved |
| 128K | 1.289 [1.266-1.314] faster | 1.172 [1.125-1.184] faster |
| 256K | 1.211 [1.189-1.216] faster | 1.178 [1.172-1.196] faster |
| 512K | 1.226 [1.208-1.231] faster | 1.183 [1.181-1.194] faster |
| 1M (tiled four-step) | 1.118 [1.108-1.134] faster | 1.135 [1.121-1.149] faster |
| 2M | 0.989 [0.961-1.007] unresolved | 1.003 [0.973-1.031] unresolved |
| 4M | 1.014 [0.996-1.023] unresolved | 1.006 [0.994-1.025] unresolved |

The 1K to 4K rows come from their own board (up to 21 repetitions); the program prints milliseconds to 10 ns, 1.8%
of a 1K transform, and those sizes run HEAD's code. The same loop's counts (per transform):

| n | side | cycles | stores | `resource_stalls.sb` | port 5 | memory bound |
|---|---|---|---|---|---|---|
| 8K | planned (in-place-3) | 31,229 (-11.8%) | 7,457 (-22.6%) | 12,141 (-34.3%) | 20,754 | 45.1% |
| 32K | deep-split-3 | 146,088 (-18.3%) | 34,057 (-19.4%) | 39,854 (-48.0%) | 93,151 | 37.8% |
| 128K | planned (deep-split-4) | 1,002,679 (-17.1%) | 132,963 (-27.0%) | 320,760 (-49.5%) | 460,107 | 53.1% |
| 128K | deep-split-3 | 969,815 (-21.6%) | 171,667 (-16.0%) | 392,521 (-48.4%) | 426,078 | 53.7% |

The instructions are unchanged within 0.5%; port 5 takes 3% to 6% more, which the halved store-buffer stalls repay.

## Against MKL (c3, planned)

c3's median against the same board's MKL median (MKL runs in HEAD's process), HEAD's in brackets:

| n | offset 0 | offset 16 |
|---|---|---|
| 8K | 1.063x MKL (0.888x) | 1.034x (0.909x) |
| 16K | 1.018x (0.867x) | 1.072x (0.947x) |
| 128K | 1.610x (1.253x) | 1.566x (1.346x) |
| 256K to 1M | 1.785x to 2.105x | 1.728x to 1.805x |

## The 32K and 64K arms on c3

IP4-AoS stays planned there; c3's deep-splits now lead it (`CRD_AB_ARMS="ip4 deep-split-3 deep-split-4"`,
`CRD_AB_REFERENCE=ip4`, at most 15 repetitions):

| n | offset | MKL | deep-split-3 | deep-split-4 |
|---|---|---|---|---|
| 32K | 0 | 1.281 [1.217-1.314] | 1.258 [1.198-1.268] | 1.257 [1.240-1.298] |
| 64K | 0 | 1.356 [1.340-1.363] | 1.283 [1.275-1.294] | 1.359 [1.334-1.368] |
| 32K | 16 | 1.141 [1.092-1.226] | 1.235 [1.219-1.241] | 1.262 [1.254-1.276] |
| 64K | 16 | 1.256 [1.234-1.270] | 1.272 [1.259-1.292] | 1.342 [1.322-1.362] |

Planned as the deep-split-4 at avx512, 32K would run 0.99x MKL aligned and 1.11x off a line, 64K 1.01x and 1.06x
(from 0.74x to 0.88x): the plan change is the next batch's (an avx512 plan row from this machine's arms board, since
the canonical plan serves avx2, whose code this change left as it was).

## f64 (`bench_fft_vs_refs`, c3 against HEAD, offset of the program's own buffers)

| n | ratio | n | ratio |
|---|---|---|---|
| 1K | 1.000 [1.000-1.000] level | 256K | 1.160 [1.147-1.180] faster |
| 2K | 1.000 [1.000-1.000] level | 512K | 1.193 [1.185-1.215] faster |
| 4K | 1.000 [1.000-1.083] unresolved | 1M (six-step) | 1.002 [0.993-1.006] level |
| 8K | 1.000 [0.964-1.000] unresolved | 2M | 0.989 [0.971-1.002] unresolved |
| 16K to 64K (IP4-AoS) | 0.982 to 0.992, unresolved | 4M | 0.996 [0.982-1.026] unresolved |
| 128K | 1.133 [1.115-1.201] faster | 8M | 0.977 [0.973-0.989] unresolved |

The program prints milliseconds to 1 us (3.6% of an 8K transform), so 4K to 64K cannot resolve; 16K to 64K
(IP4-AoS) and 2M to 8M (the four-step's hierarchical codelets) call none of the changed stores, so their cells are
the board's noise on unchanged code (the two-binary layout floor of the
[cnn board](2026-11-01-simd-perf-cnn-avx2-pairs.md)).
