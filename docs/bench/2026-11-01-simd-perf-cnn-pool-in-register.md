# 2026-11-01 — SIMD-PERF: the cnn image loop, the row pair pooled in its register (real AVX-512)

<!-- doc-role: evidence -->
> Dated evidence; counts and results are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).
> Owner row: [SIMD-PERF](../ROADMAP.md#slice-simd-perf); session:
> [2026-11-01](../sessions/2026-11-01-simd-perf-cnn-pool-in-register.md); method:
> [fast A/B boards](README.md#fast-ab-boards). Never compare these numbers with the i9-14900K boards; ratios only.

- **Machine:** the optimization track's benchmark machine, as in the
  [first board](2026-10-31-simd-perf-fft-f32-xeon.md): GCE spot `c4-standard-8`, Intel Xeon Platinum 8581C (Emerald
  Rapids), Ubuntu 24.04, GCC 13.3, perf with the guest PMU. **Real AVX-512 hardware.** Every process pinned to
  CPU 3. This start's absolute times differ from the [previous board](2026-10-31-simd-perf-cnn-row-pairs.md)'s
  (pre-row's cnn rows 3,600 to 3,850 ns a sample here against 3,030 to 3,130 there, another spot host); only the
  ratios of one board are compared.
- **Binaries:** the pre-row engine `2f2aed72`'s own `bench_nn` (`scripts/build-prerow.sh`, its linux-gcc-release
  preset, AVX2 code); HEAD `a019687a` (`~/ab/nn-head7`); the staging alone, the candidate with the in-register pool
  switched off (`~/ab/nn-c7s`); the kept change, staging and in-register pool (`~/ab/nn-c7`); all from the
  linux-gcc-release lane tree (GCC 13.3, `-O3`, program LTO). `CRD_CPU_LEVEL` selects the hesap-tensor kernels'
  level; pre-row runs its compiled AVX2.
- **Program:** each engine's `bench_nn` (frozen CNN: conv 1 to 8 channels on 16 x 16, relu, 2x2 pool, conv 8 to 16
  on 8 x 8, relu, pool, fc 256 to 10, softmax; f32, Q8_0 and i8 tiers; batch 8 and 4096; best of 50 whole-batch
  inferences, ns a sample). HEAD's and the candidates' processes run `bench_nn cnn`, pre-row's its whole program.
- **Method:** `scripts/ab-board.py`, one warm-up, 5 to 25 repetitions in rotated order (10 to 25 against pre-row,
  5 at sse2 and portable), median paired speed ratio [percentile-bootstrap 95%], 2% band; the arm faster above 1.
  Profiles: `perf stat` (cycles, instructions, branch misses, `-M TopdownL1`) and `perf record --call-graph dwarf`
  (cycles) of one `bench_nn cnn` process each, symbol shares, the callers of the C library's copies and
  `perf annotate` of `conv2d_images`; the unit's avx512 code read from a non-LTO compile of its object.

## Where the time went (one `bench_nn cnn` process)

| | HEAD avx512 | kept avx512 | HEAD avx2 | kept avx2 |
|---|---|---|---|---|
| cycles | 5.75 G | 5.23 G | 8.26 G | 7.83 G |
| instructions | 20.1 G | 18.0 G | 37.3 G | 36.7 G |
| branch misses | 2.36 M | 1.69 M | 2.38 M | 1.63 M |
| retiring / backend / frontend / bad speculation | 64.5 / 17.2 / 16.4 / 2.0% | 64.8 / 20.3 / 14.1 / 0.8% | 72.9 / 19.6 / 5.9 / 1.6% | 75.3 / 20.8 / 2.3 / 1.6% |
| the convolution's row blocks | 55.8% | 65.2% | 69.3% | 71.5% |
| `conv2d_images` (staging, pool, image loop) | 26.9% | 25.3% | 18.2% | 21.1% |
| the C library's copies and fills (`memmove`, `memset`, their PLT stubs) | 8.7% | not in the top 25 | 5.6% | not in the top 25 |

- **HEAD:** a third of the avx512 samples sat outside the fma loop. Every staged row was a `memcpy` of 8 or 16
  floats (`__memmove_avx512_unaligned_erms` 6.2% plus the PLT stub, every call from `conv2d_images`), and every
  image re-zeroed its padding rows and columns (`memset` 1.5%). The fused pool stored the second layer's row pair
  from its `Vec16f` into two row buffers and read them back through `max_pool2_row`'s general loops (its hottest
  lines the `vshufps`/`vpermps` pair split, the vertical `vmaxps` and the loop's counters).
- **The kept change:** no copy call and no `rep stos` in the avx512 object (one `memset` a call zeroes the staging);
  the row pair's pool is, per output channel, `vextractf32x8`, `vmaxps`, `vextractf128`, two `vshufps` and `vmaxps`
  on the accumulator register, then one `xmm` store of the four pooled columns. Cycles -8.9% at avx512 and -5.2% at
  avx2.
- **Left in `conv2d_images`:** the first layer's 16-column rows at avx512 and both layers at avx2 still pool through
  the row buffers (their two conv rows are computed one after the other, eight accumulators each).

## Boards (ns a sample, speed ratio over the reference)

**avx512, HEAD the reference (25 repetitions):**

| row | HEAD | staging alone | ratio | the kept change | ratio |
|---|---|---|---|---|---|
| f32 batch 8 | 2,564 | 2,497 | 1.027 [1.014-1.051] unresolved | 2,315 | **1.107 [1.102-1.130] faster** |
| q8 batch 8 | 2,614 | 2,561 | 1.021 [1.015-1.030] unresolved | 2,354 | **1.112 [1.106-1.122] faster** |
| i8 batch 8 | 2,579 | 2,492 | 1.033 [1.026-1.044] faster | 2,327 | **1.112 [1.103-1.117] faster** |
| f32 batch 4096 | 2,648 | 2,586 | 1.026 [1.018-1.031] unresolved | 2,425 | **1.093 [1.089-1.099] faster** |
| q8 batch 4096 | 2,556 | 2,507 | 1.018 [1.016-1.023] unresolved | 2,322 | **1.101 [1.099-1.105] faster** |
| i8 batch 4096 | 2,519 | 2,418 | 1.041 [1.035-1.051] faster | 2,276 | **1.107 [1.102-1.114] faster** |

The staging gives 1.8% to 4.1% (every interval above 1.014), the in-register pool the rest.

**avx512, pre-row the reference (10 repetitions, every cell decided):** the kept change 1.503 [1.483-1.517]
(f32 batch 4096) to 1.663 [1.640-1.671] (i8 batch 4096); f32 batch 8 1.603, q8 batch 8 1.619, i8 batch 8 1.649,
q8 batch 4096 1.641, all faster.

**avx2, HEAD the reference (25 repetitions):** 1.047 [1.041-1.076] to 1.068 [1.056-1.070], every cell faster
(f32 batch 8 1.047, q8 batch 8 1.057, i8 batch 8 1.068, f32 batch 4096 1.053, q8 batch 4096 1.065, i8 batch 4096
1.064). The row pair is not built at avx2; the gain is the staging and the zeros written once a call.

**avx2, pre-row the reference (25 repetitions):**

| row | pre-row | the kept change | ratio |
|---|---|---|---|
| f32 batch 8 | 3,609 | 3,552 | 1.027 [1.013-1.051] unresolved |
| q8 batch 8 | 3,699 | 3,509 | **1.058 [1.048-1.080] faster** |
| i8 batch 8 | 3,722 | 3,531 | **1.059 [1.051-1.086] faster** |
| f32 batch 4096 | 3,648 | 3,544 | **1.030 [1.028-1.032] faster** |
| q8 batch 4096 | 3,787 | 3,448 | **1.097 [1.096-1.101] faster** |
| i8 batch 4096 | 3,753 | 3,451 | **1.088 [1.084-1.089] faster** |

The f32 batch-4096 cell, 0.962 to 0.972 of pre-row on the
[pool-staging board](2026-10-31-simd-perf-cnn-pool-staging.md), is now 1.030 faster; the f32 batch-8 cell is
unresolved only because its interval reaches into the 2% band (its lower bound 1.013 is above 1). Nothing at avx2
trails the pre-row engine.

**sse2 and portable, HEAD the reference (5 repetitions):** every median 0.984 to 1.003 (sse2) and 1.001
(portable), every cell unresolved: the processes spread 4% to 6% on this start at 150 and 230 microseconds a
sample, where the change's part (the staging, no pool pair below avx512) is under 1% of the time, the convolution's
fmas running the exact emulation (sse2) or the scalar loop (portable). No median is below 0.984 and the one cell
under 1.000 (sse2 q8 batch 8) has an interval of [0.943-1.066].

## Correctness

The pooled pair runs `max_pool2_row`'s operations on the same operands in the same order (the two conv rows'
maximum, then each column pair's on the even and odd lanes), and the staged rows hold the same values; the bits are
HEAD's. On the machine (linux-gcc-release lane tree at the candidate): every `crd-hesap-tensor*` case matching nn,
kernel, level or simd and every `crd-math-kernels-at-<level>` case, 464, all passing at every forced level
(portable, sse2, sse4.2, avx2, avx512 and the VNNI variants); the SIMD probe compares `deinterleave`'s even and odd
lanes at every level with the portable level's bits. The convolution case adds a call over both images at once
(the second image staged into the first one's zero rows and columns, the scratch filled with guard words before);
its shapes pool rows of 2, 5, 7 and 8 columns (one to four pooled columns, both store paths) with and without the
relu. Teeth: skipping the once-a-call zeroing fails every level's arm; storing the even columns in place of the
pooled ones fails the avx512 arm only. Clang 18 (`-fsyntax-only`, the lane's flags, `-Wunused-function`) is clean on
the unit at all six levels. Every board process passed its tier bound against the f32 reference.
