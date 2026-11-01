# 2026-11-01 — SIMD-PERF: the f32 4K forward as three in-place passes (real AVX-512)

<!-- doc-role: evidence -->
> Dated evidence; counts and results are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).
> Owner row: [SIMD-PERF](../ROADMAP.md#slice-simd-perf); session:
> [2026-11-01](../sessions/2026-11-01-simd-perf-fft-in-place-4k.md); method:
> [fast A/B boards](README.md#fast-ab-boards). Never compare these numbers with the i9-14900K boards; ratios only.

- **Machine:** the optimization track's benchmark machine, as in the
  [first board](2026-10-31-simd-perf-fft-f32-xeon.md): GCE spot `c4-standard-8`, Intel Xeon Platinum 8581C (Emerald
  Rapids, 2.3 GHz), L1d 48 KiB and L2 2 MiB a core, Ubuntu 24.04, kernel 7.0.0-1014-gcp, GCC 13.3, perf with the
  guest PMU. **Real AVX-512 hardware.** Every process pinned to CPU 3; the host idle (`build-bench.py host`: 28.7 of
  29.4 GiB available, no other process above 0.0 cores). One start, 0.66 h.
- **Binaries:** HEAD `590071f1`'s `bench_fft_f32` (`~/ab/b10-head`) and the kept change's (`~/ab/b10-cand`), both
  from the linux-gcc-release lane tree (GCC 13.3, `-O3`, program LTO). `CRD_CPU_LEVEL=avx512` and `avx2` select the
  fft units; MKL runs its AVX-512 library at both (ADR-0134 §8).
- **Peer:** Intel MKL 2020.0.4 (`libmkl_avx512.so`), timed in the same process every repetition. Every arm, Cerid's
  and MKL's, passed the ADR-0092 §5 f32 class against the FFTW f64 truth of the same input (in-place-3's error
  1.2e-7 at 2K and 4K, 1.3e-7 at 8K, the same as standalone-hier's).
- **Method:** `scripts/ab_fft_f32.sh` through `scripts/ab-board.py`, one warm-up, rotated order, median paired speed
  ratio [percentile-bootstrap 95%], 2% band. Board 1: the candidate's full board (`bench_fft_f32 --sizes 11-13`,
  every forward the planner offers timed in one process a repetition), the new `CRD_AB_ARMS` mode naming the arms, 21
  repetitions a level. Board 2: HEAD's and the candidate's planned forwards with `--same` on the output digests, 7 to
  9 repetitions. Profiles: `scripts/prof_fft_f32.sh` (500,000 transforms a side, `perf stat` top-down levels 1 and 2,
  the store and store-buffer events, `perf record`), counts per transform net of the `copy` side's.

## Board 1: in-place-3 against the out-of-place forwards and MKL

Each cell is the named arm's speed against in-place-3's (above 1 the arm is faster; the in-place speed-up is the
inverse).

| level | n | in-place-3 ms | standalone-hier | ip4 | MKL |
|---|---|---|---|---|---|
| avx512 | 2048 | 0.00222 | 1.095 [1.073-1.13] | 0.929 [0.912-0.946] | 1.514 [1.48-1.544] |
| avx512 | 4096 | 0.00345 | **0.726 [0.719-0.804]** | 0.582 [0.575-0.637] | **1.015 [0.980-1.052]** |
| avx512 | 8192 | 0.01008 | 0.995 [0.988-1.021] | 0.886 [0.882-0.892] | 1.206 [1.203-1.226] |
| avx2 | 2048 | 0.00284 | 1.164 [1.12-1.195] | 1.04 [1.018-1.058] | 1.953 [1.928-2] |
| avx2 | 4096 | 0.00471 | **0.856 [0.851-0.863]** | 0.789 [0.782-0.791] | 1.334 [1.322-1.349] |
| avx2 | 8192 | 0.01285 | 0.912 [0.900-0.934] | 0.964 [0.960-0.968] | 1.532 [1.519-1.553] |

- **4K:** in-place-3 runs 1.38x the standalone-hier (HEAD's plan) at avx512 and is level with MKL (0.985x, the
  interval 0.95x to 1.02x); at avx2 1.17x the standalone-hier and 0.75x MKL. The planner now takes it at 4K.
- **8K:** level with the standalone-hier at avx512, 1.10x it at avx2 (the i9's avx2 row keeps ip4 or the
  standalone-hier; see the plan below). The radix-32 middle pass spills, and 8K's 64 KiB leaves the L1d.
- **2K:** the standalone-hier (its 48 KiB working set fits the L1d) beats in-place-3's three passes by 1.10x and
  1.16x; the planned ip4 stays behind both at avx512 (the machine-dependent 2K plan item, unchanged).

## Board 2: HEAD's and the candidate's planned forwards

The plan changes only at 4K under the canonical planner, which avx512 runs (no avx512 row in the per-level table);
avx2's table keeps the standalone-hier there. Every other size printed the same digests in both builds at every
repetition, at both levels (`--same`), and ran level: avx2 1K to 1M 0.9996 to 1.011 (every cell level or
unresolved within [0.991, 1.064]), avx512 1K, 2K and 8K to 1M 0.996 to 1.008. The factored stage-1 twiddle the
plan builder now shares (`build_factored_stage1`) gives the standalone-hier and deep-split sizes from 32K the same
bits.

## Where the time goes at 4K (per transform, net of the copy)

| level | side | cycles | instructions | stores | top-down |
|---|---|---|---|---|---|
| avx512 | standalone-hier (HEAD's plan) | 16,988 | 27,900 | 4,584 | memory bound 45.9%, frontend 10.6% |
| avx512 | in-place-3, first cut (engine deinterleave pass) | 11,528 | 34,045 | 5,331 | core bound 36.3%, memory 18.0% |
| avx512 | **in-place-3, kept** | **10,328** | **28,738** | **4,818** | core bound 39.6%, memory 16.5%, retiring 42.0% |
| avx512 | MKL | 10,114 | 21,832 | 1,877 | core 34.4%, memory 34.4% |
| avx2 | standalone-hier | 20,019 | 51,740 | 9,696 | |
| avx2 | in-place-3, kept | 15,574 | 59,937 | 10,904 | retiring 60.7%, core 21.6%, memory 12.9% |

- **The bound removed:** the standalone-hier's input, AoSoA intermediate and full stage-1 twiddle table (96 KiB)
  through a 48 KiB L1d, memory bound 45.9% of the slots with the store buffer full on its stores to the caller's
  lines. in-place-3 keeps the 32 KiB data in the caller's buffer through all three passes, every table a few KiB:
  memory bound 16.5%.
- **The first cut's own cost:** the engine deinterleaved every tile in a pass of its own before pass 1 (its loop
  reloaded the plan's size after each store), 30% of the samples with the group transposes. A step of whole tiles
  deinterleaves as it loads, so at avx2 and avx512 pass 1 now reads the interleaved input itself (the same bits):
  -1,200 cycles and -5,300 instructions a transform.
- **What is left at avx512:** 1.32x MKL's instructions and 2.6x its stores. The group transposes (eight `Vec8f`
  8 x 8 transposes a 16 x 16 group) hold 22% of the samples; the radix-16 bodies spill (4,818 stores against the
  three passes' 1,536 vector data stores). At avx2 every radix-16 body spills on 16 `ymm` registers (pass 1 34% of
  the samples).

## Checks

On the machine, linux-gcc-release: `crd-hesap-fft-tests` (every level's forced forwards bit-identical to portable's
and within the class, the planner's and the per-level tables' cases) with the fft, dsp and wavelet fatal cases (94
CTest cases), `crd-hesap-dsp-tests` (104 cases), `crd-hesap-comms-tests` (27) and `crd-hesap-wavelet-tests` (25),
all passing.
