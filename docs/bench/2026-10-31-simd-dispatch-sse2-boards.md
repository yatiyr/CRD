# 2026-10-31 — SIMD-DISPATCH: the sse2 and sse4.2 bodies' boards against portable, the pre-row engine and the peers

<!-- doc-role: evidence -->
> Dated evidence; counts and results are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).
> Owner row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch); session:
> [2026-10-31](../sessions/2026-10-31-simd-dispatch-sse2-boards.md); before: the
> [v14-a dtype](2026-07-02-v14a-dtype-converts.md), [v14-c reduce](2026-07-02-v14c-reduce-vs-reproblas.md),
> [v14-m nn](2026-07-05-v14m-nn.md) and [v12 statistics and special](2026-06-27-v12-statistics-special.md) boards.

- **Machine:** Intel i9-14900K, WSL2 Ubuntu 24.04, linux-gcc-release lane tree (GCC 13.3), one thread on CPU 4.
  Busy host by the user's order of 2026-10-31: `scripts/build-bench.py host` before each bench and round read other
  Windows processes at 0.7 to 3.0 cores (dota2, devenv, opera, python) and 14.1 to 15.4 of 63.7 GiB available
  (flagged below the 25% mark); Linux 30.2 of 31.2 GiB available.
- **HEAD:** `7aeacb8e`. **Pre-row:** `2f2aed72`, its own tree: `bench_nn` and `bench_logpdf_grad` as that tree's
  retained benches, `bench_dtypes` and `bench_reduce` with that commit's own driver lines (`g++ -O3 -march=native`,
  the reduce bench with `-DCRD_SIMD_TARGET=2`). Pre-row has one x64 level (AVX2), so it is compared at avx2 only.
  `bench_special_batch` is new in the row and has no pre-row program.
- **Arms**, each `<kernel level>@<glibc level>`: `CRD_CPU_LEVEL` picks the kernel level and `GLIBC_TUNABLES`
  `glibc.cpu.hwcaps` hides the features above the glibc level, as `scripts/run-lane-bench.sh` does per level.
  `portable@sse2` is the portable body on an SSE2 machine (glibc's `fma` and `libm` in their non-FMA bodies), the arm
  an sse2 body must beat; the drivers' `portable` arm keeps the host's glibc (`portable@host`) and flatters any body
  that calls glibc: special's portable Lanczos runs 47 ns there and 241 ns at `portable@sse2`. `sse2@sse4.2` is the
  sse2 body on an SSE4.2 machine, the arm an sse4.2 unit must beat.
- **Method:** three rounds; each round runs every arm of a bench (the order reversed in even rounds), the pre-row
  binary first in odd rounds and last in even ones, then the bench's peers. The binaries run straight from the lane
  tree with `run-lane-bench.sh`'s environment, the gates first (49 CTest cases, all passed): another session was
  editing `ci/` in the checkout, so `wsl-lane.sh run`'s mirror check refused the drivers. Every bench's own gate held
  in every arm (bits against the portable level, the nn tiers' bounds). Values are medians [min-max] of the rounds;
  ratios are taken within a round.
- Peers at the host's ISA, one thread: numpy, ml_dtypes, torch (dtypes); ReproBLAS `dfb81505` (reduce); JAX and
  PyTorch (log-density gradients); scipy, MATLAB R2026a with `maxNumCompThreads(1)` on Windows and Boost 1.92.0
  (special); torch and onnxruntime (nn).

## Against the pre-row engine (avx2)

Pre-row ms over HEAD ms, above 1 HEAD faster:

| bench, row | ratio |
|---|---|
| dtypes: the eleven rows | 0.95 to 1.16; quantize Q8_0 7.51 [6.52-7.56] |
| reduce: naive, Tier D and Tier R at 1 Mi and 16 Mi | 0.95 to 1.09 (Tier R 1 Mi 0.966 [0.827-1.079]) |
| log-density gradient: Normal | 1.031 [1.015-1.052] |
| log-density gradient: Gamma (O(1)) | **0.122 [0.122-0.123]** (88.6 ns against 10.9) |
| log-density gradient: Student-t (O(N)) | **0.761 [0.754-0.767]** |
| nn: mlp f32, batch 16 and 4096 | **0.580 [0.580-0.628], 0.600 [0.576-0.657]** |
| nn: mlp Q8_0 and i8 | **0.668 to 0.704** |
| nn: cnn f32, Q8_0, i8, batch 8 | **0.269 to 0.281** |
| nn: cnn f32, Q8_0, i8, batch 4096 | **0.357 to 0.377** |

Accepted: dtypes and reduce. Three regressions hold in every round:

- **Gamma.** The O(1) gradient (`log(scale) + digamma(shape)`) is general code, built at the baseline ISA since
  this row, so every `libm::fma` of the scalar log and digamma cores is a call into glibc: HEAD's bench holds 88
  `call fma@plt` sites, the pre-row one none (its 178 `vfmadd` are inline). avx2 and `portable@host` time the same
  (88.6, 91.8 ns): no kernel is involved. Boost's scalar `digamma` (21.9 ns) now leads Cerid's (24.8 ns, 0.88x).
  The class: scalar transcendentals in baseline general code.
- **Student-t.** The O(N) sum's avx2 loop is the pre-row loop; `crd_log4` gained a second division in `e0f539cb`
  (the quotient's rounding carried so log stays within an ulp; it had reached 7 ulp next to sqrt(2)). The sum now
  divides three times per four elements instead of twice. It stays 2.23x ahead of JAX.
- **nn.** Every layer kind and tier, worst on the 3x3 convolutions (0.27 to 0.38). The input is now the corpus's
  frozen rows repeated (pre-row: Philox values in [-1, 1]); no input or output value is subnormal, and the ratio is
  the same at batch 8 and 4096. A `perf` cpu-clock profile of each binary (the 6.8 linux-tools build runs the
  software event in WSL) and callgrind's counts (HEAD 59.8 G instructions, pre-row 48.2 G) named two causes:
  - the convolution's fused 2x2 pool ran the scalar IEEE `maximum` (compares, `setnp`, `cmov`) three times per
    output: 54% of HEAD's samples, where pre-row's `vmaxss` ternaries were a small part of `conv_run`;
  - the mlp's layernorm stayed in general code, so its `fma` chain ran the exact f32 emulation (double products,
    rounding to odd) at the baseline ISA: `run_op` took 6.4x pre-row's samples.

**After the nn folds** (the pool's vertical and column-pair maxima on vector lanes through one row helper shared by
the fused and the stand-alone pool, zero-filled vector tails; layernorm as a kernel of the tensor family's nn unit),
pre-row over HEAD at avx2, three rounds:

| row | batch 16 (mlp), 8 (cnn) | batch 4096 |
|---|---|---|
| mlp f32 | 0.969 [0.966-1.020] | 0.961 [0.956-1.019] |
| mlp Q8_0 | 0.909 [0.868-0.941] | 0.985 [0.978-1.018] |
| mlp i8 | 0.989 [0.989-1.047] | 1.016 [1.008-1.078] |
| cnn f32 | 0.840 [0.807-0.859] | 0.835 [0.831-0.875] |
| cnn Q8_0 | 0.862 [0.861-0.914] | 0.890 [0.881-0.933] |
| cnn i8 | 0.858 [0.838-0.916] | 0.849 [0.838-0.859] |

The mlp rows are at pre-row within the spread but Q8_0 at batch 16 (0.91). The cnn rows still trail by 11% to 16%
in every round: in the profile the convolution kernel takes 18% more samples than pre-row's and the pool and
image loop 55% more (a call and two partial tails per pooled row). sse2 against `portable@sse2`: mlp 1.28x to 2.78x,
cnn 1.65x to 1.69x. Logs: `~/sse2-board/logs-nnfix2`.

## sse2 and sse4.2 bodies against the portable loop

`portable@sse2` ms over `sse2@sse2` ms, above 1 the sse2 body faster:

| family, row | ratio |
|---|---|
| conversions: f16 to f32, Q8_0 quantize, the SR batches | 1.78; 5.25; 1.11 to 1.71 |
| conversions: f32 to bf16 (one compiled loop on every level), Q8_0 dequantize (scalar) | 0.98, 1.03 |
| conversions: **f32 to f16, e4m3, e5m2 (round to nearest)** | **0.655, 0.718, 0.624** |
| reduce: Tier R at 1 Mi and 16 Mi | 5.48, 3.84 |
| log-density gradient: Student-t | 2.21 |
| special: lgamma, tgamma (4 Mi and 8 Ki) | 2.70 to 2.98 |
| nn: f32, Q8_0, i8 (mlp; cnn) | 1.59, 1.28, 2.76; 1.52 to 1.53 |

Every sse2 body beats the portable loop but the three round-to-nearest narrowings: their four-lane transcription
runs the subnormal path (two emulated variable shifts and three `pow2` conversions) on every step, which the scalar
loop branches past on normal inputs.

**After the fold** (`narrow_rne_lanes` runs the subnormal path only when a nonzero lane takes it; the same bits, the
conversion differentials and the v14-a gates passing), three more rounds of `bench_dtypes`, `portable@sse2` over
`sse2@sse2`: f32 to f16 1.497 [1.440-1.604], e4m3 1.725 [1.715-1.737], e5m2 1.427 [1.382-1.487]; the sse4.2 unit
over sse2 1.18 to 1.26. The avx2 fp8 narrowings, on the same transcription, now run 1.443 [1.403-1.505] times
pre-row (0.41 against 0.57 ns/elem); the other rows held (f32 to f16 at avx2 1.057, the SR batches 1.10 and 1.11).
At sse2 e4m3 now leads ml_dtypes (0.706 against 1.28 ns/elem).

`sse2@sse4.2` ms over `sse4.2@sse4.2` ms, above 1 the sse4.2 unit faster:

| family | ratio |
|---|---|
| conversions | 1.16 to 1.30 (f32 to bf16 1.30, f16 to f32 1.24) |
| hesap-tensor nn: mlp i8 | 0.917, 0.924 |
| hesap-tensor nn: mlp f32 and Q8_0, cnn | 0.97 to 1.01 |
| stats, special | the sse2 table at both levels |

The conversion family's sse4.2 unit stays. The hesap-tensor family's sse4.2 unit does not beat sse2 (its
`pmaddubsw` i8 dot loses 8%, the rest ties) and was removed: an sse4.2 selection runs the sse2 table, as stats,
special and fft already do.

## Against the peers

| row | Cerid avx2 | Cerid sse2 | peer |
|---|---|---|---|
| f32 to f16, ns/elem | 0.097 | 1.74 | torch 0.094, numpy 1.17 |
| f16 to f32 | 0.108 | 0.397 | numpy 0.599 |
| f32 to bf16 | 0.121 | 0.257 | torch 0.167, ml_dtypes 0.305 |
| f32 to e4m3 | 0.607 | 1.72 | ml_dtypes 1.28 |
| Tier R 1 Mi / 16 Mi, ns/elem | 0.223 / 0.487 | 0.418 / 0.617 | ReproBLAS 0.416 (both N) |
| Student-t gradient, ms/call | 1.18 | 51.4 | JAX 2.64, PyTorch 23.1 |
| Gamma gradient (O(1)), ns/call | 88.6 | 472 | JAX 238,300, PyTorch 13.6 M |
| lgamma 4 Mi batch, ns/elem | 4.85 | 82.1 | scipy 8.41, MATLAB-1T 7.49, Boost (scalar) 234 |
| tgamma 4 Mi batch | 5.92 | 142 | scipy 15.1, MATLAB-1T 14.1, Boost (scalar) 258 |
| mlp f32 batch 16 / 4096, ns/sample (after the nn folds) | 210 / 216 | 10,810 / 11,010 | onnxruntime 445 / 239, torch 1,247 / 401 |
| cnn f32 batch 8 / 4096, ns/sample (after the nn folds) | 2,135 / 2,270 | 77,580 / 79,320 | onnxruntime 2,296 / 2,180, torch 17,560 / 14,300 |

The peers run their AVX2 (and FMA) code at every Cerid level, so a sse2 column against them is a floor, not a
like-for-like row. Behind a peer at avx2: torch's f32 to f16 (0.97) and onnxruntime's cnn at batch 4096 (0.96);
before the nn folds onnxruntime led the mlp at batch 4096 (0.69) and the cnn by 2.4x to 2.8x.

Logs: `~/sse2-board/logs` in WSL (`round<N>-<bench>.log`; after the fold `~/sse2-board/logs-rne`; the pre-row
binaries in `~/sse2-board/prerow`). `scripts/run-lane-bench.sh` now takes `<level>@<glibc level>` list entries, so
`CRD_BENCH_LEVELS="avx2 sse4.2 sse2@sse4.2 sse2 portable@sse2"` runs these arms through the drivers.
