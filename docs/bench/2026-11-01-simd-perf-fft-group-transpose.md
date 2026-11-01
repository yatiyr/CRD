# 2026-11-01 — SIMD-PERF: the in-place forward's pass-3 group transpose at avx512 (real AVX-512)

<!-- doc-role: evidence -->
> Dated evidence; counts and results are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).
> Owner row: [SIMD-PERF](../ROADMAP.md#slice-simd-perf); session:
> [2026-11-01](../sessions/2026-11-01-simd-perf-fft-group-transpose.md); method:
> [fast A/B boards](README.md#fast-ab-boards). Never compare these numbers with the i9-14900K boards; ratios only.

- **Machine:** the optimization track's benchmark machine, as in the
  [first board](2026-10-31-simd-perf-fft-f32-xeon.md): GCE spot `c4-standard-8`, Intel Xeon Platinum 8581C (Emerald
  Rapids), L1d 48 KiB and L2 2 MiB a core, Ubuntu 24.04, GCC 13.3, perf with the guest PMU. **Real AVX-512
  hardware.** Every process pinned to CPU 3. Two starts, 0.48 h.
- **Binaries:** `bench_fft_f32` from the linux-gcc-release lane tree (GCC 13.3, `-O3`, program LTO): HEAD `11858569`
  (`~/ab/b11-cand`, the previous batch's kept build, byte-identical to a fresh HEAD build), candidate A (split
  scratch rows, `~/ab/b12-cand`) and the kept candidate B (`~/ab/b12-candB`). `CRD_CPU_LEVEL=avx512` and `avx2`
  select the fft units.
- **Peer:** Intel MKL 2020.0.4 in the same process every repetition, its AVX-512 library at both levels.
- **Method:** `scripts/ab_fft_f32.sh` (the level's planned forward, HEAD against the candidate and MKL) through
  `scripts/ab-board.py`, one warm-up, rotated order, median paired speed ratio [percentile-bootstrap 95%], 2% band,
  at most 15 repetitions; both builds printed the same bits at every size of every repetition (`--same`). The
  profile loop (`bench_fft_f32 --loop in-place-3`, 200,000 transforms at 2K and 100,000 at 4K, with the input copy)
  through `scripts/prof_fft_f32.sh` with `uops_dispatched.port_*` and `ld_blocks.store_forward` counted.

## The bound

HEAD's in-place-3 at avx512, per transform (with the copy):

| n | cycles | instructions | port 5 | port 0 | port 1 | loads | MKL cycles | MKL port 5 | MKL loads |
|---|---|---|---|---|---|---|---|---|---|
| 2K | 5,467 | 14,188 | 4,827 (88%) | 3,146 | 486 | 4,537 | 4,712 | 3,598 (76%) | 2,256 |
| 4K | 13,304 | 29,201 | 10,117 (76%) | 6,749 | 990 | 9,274 | 13,243 | 7,959 (60%) | 4,718 |

Port 5 is the bound: while 512-bit operations are in flight port 1 takes no vector operation (9% busy), so every
shuffle, 256-bit ones included, and half the 512-bit FMAs share port 5. The engine's group transpose (24.8% of the 2K
samples, 19.5% at 4K) ran eight `Vec8f` 8 x 8 transposes a group, 192 shuffles, all on port 5 there.

## The change and its two editions

`transpose8x8_halves` transposes the real and the imaginary 8 x 8 tile of an AoSoA block row in one `Vec16f` a row
(96 shuffles a group). Two ways to store the result were measured, each the same bits as HEAD:

| edition, 2K / 4K | cycles | port 5 | blocked store forwards | memory bound |
|---|---|---|---|---|
| HEAD | 5,467 / 13,304 | 4,827 / 10,117 | | 3.1% / 13.7% |
| A: split scratch rows (16 real, 16 imaginary), stored as halves, loaded whole by pass 3 | 4,961 / 12,220 | 4,222 / 8,892 | 289 / 559 | 9.8-10.2% / 16-21% |
| B (kept): AoSoA scratch rows stored whole, pass 3's half loads unchanged | 4,847 / 11,928 | 4,310 / 9,065 | 1.6 / 1.7 | 7.4-22% / 18% |

A's whole-register loads each spanned two half stores still in the store buffer, so they could not forward; B keeps
pass 3's two half loads a value (each inside one whole store) and pays their insert on port 0 or 5. In two
alternating profile rounds B ran 2.0% to 2.6% fewer cycles than A at each size (the table holds those rounds'
means); the A-against-B board was unresolved (2K 1.029 [0.993-1.065], 4K 1.036 [1.003-1.069]). A also needed a
generator change; B does not.

## The board: B against HEAD

| level | n | HEAD ms | candidate ms | speed ratio | verdict | MKL against HEAD |
|---|---|---|---|---|---|---|
| avx512 | 2K | 0.00159 | 0.00142 | 1.129 [1.113-1.151] | faster | 1.250 [1.241-1.254] |
| avx512 | 4K | 0.00347 | 0.00313 | 1.123 [1.103-1.134] | faster | 1.000 [0.984-1.015] |
| avx512 | 8K | 0.00963 | 0.00961 | 1.002 [0.999-1.008] | level | 1.206 [1.192-1.222] |
| avx2 | 2K | 0.00295 | 0.00297 | 1.000 [0.983-1.007] | level | 2.215 [2.207-2.233] |
| avx2 | 4K | 0.00588 | 0.00584 | 0.997 [0.982-1.007] | level | 1.667 [1.621-1.684] |

13 repetitions at each level, every cell decided. The 8K plan is the standalone-hier and the avx2 transpose is
unchanged: both are the board's controls. The planned f32 forward against MKL at avx512 is now 0.90x at 2K (was 0.80x)
and 1.12x at 4K (was 1.00x), the ratio of the two medians.

## What stays (MKL's shape)

MKL's 2K forward is two passes (`mkl_dft_avx512_mg_rowbatch_twidl_fwd_064_s` 57% of its samples, then
`colbatch_plain_fwd_32_s` 20%), 64 x 32; its 4K three (32, 16 and 8). In-place-3 is three passes of 16 x 8 x 16 at
2K. After the transpose change (edition A's record) it ran 1.28x MKL's instructions (12,991 against 10,162) and 1.77x
its loads, with the transpose 15% of the samples, pass 1 32%, pass 3 23% and pass 2 17%; the kept edition still
holds port 5 89% busy at 2K.
