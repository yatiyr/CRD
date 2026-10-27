# SIMD-DISPATCH: the tensor SR draws and sketches on a kernel Philox block loop

<!-- doc-role: historical -->
> Dated evidence. Live owner: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). Contract:
> [ADR-0134](../decisions/0134-runtime-cpu-dispatch.md). Previous:
> [AVX-VNNI variant](2026-10-27-simd-dispatch-avx-vnni-variant.md). Rules: [AGENTS](../../AGENTS.md).

## CI read

- Completed runs 40424055075 to 40445742611 were cancelled with no failed job. Run 40409214033 (`cf4b8a13`) failed
  only `simd convert_truncate gives INT32_MIN` on three optimising GCC lanes, the fold `e9828cc2` removed.
- Run 40434280766 (`d6528387`, after C2) resolved to the complete tier and is in progress: every Linux lane is green,
  among them the three lanes 40409214033 failed. Push run 40451010209 (`42787d00`) is pending; nothing was dispatched.
- Needs CI audit: [DIAG.12](../ROADMAP.md#slice-diag.12)'s last hosted clause holds in 40434280766: both
  `catch2 fault:` cases and the borrowed-stack contract case pass on all nine Linux lanes, the wild-read case on the
  nine finished Windows test lanes; win-shipping and win-relwithdebinfo were still running. DIAG.11c and DIAG.12 stay
  Needs CI: they require [DIAG.11b](../ROADMAP.md#slice-diag.11b), which waits on the user's re-record (8).

## Mechanism

- `TensorKernels` gains `philox4x32_blocks`: hesap-stats' block loop (`philox_blocks.hpp`, already in the unit's
  level namespace) compiled into each tensor kernel unit, so every level and the AVX-VNNI variant has its own copy.
  The substrate still has no hesap-stats link edge (ADR-0096 §1; `smoke_hesap_tensor` passes): it does not call
  `stats_kernels()`.
- `dtypes.hpp` `fill_sr_draws` and `decomp.hpp` `philox_gaussian_fill` take their whole blocks from the table; the
  unaligned and tail draws stay the constexpr `sr_draw`. Only kernel units include `philox_blocks.hpp` now, and an
  include-budget entry holds dtypes.hpp off `<immintrin.h>` on GCC and Clang. decomp.hpp gets no entry: it reaches
  the intrinsics through batched.hpp (the hesap-dense family, open in the row).
- Found in passing: the public `quantize_q8_0` ran the scalar reference although its kernel existed. It now runs the
  table's `quantize_q8_0`; the kernel differentials compare against `detail::quantize_q8_0_blocks`, the scalar
  reference, so no oracle became the code under test.
- Forced arms: the `[dtypes]` batch, StorageTensor and ggml cases join `[kernels]`, and
  `crd-hesap-tensor-decomp-kernels-at-<level>` runs the extracted sketch case and the randomized tier.

## Verification

`tensor kernels: every level the host runs gives the scalar Philox blocks`: 0 to 19 blocks (two vectors and partial
ones), five positions across the 2^32 word wrap and the 2^64 position wrap, the SR stream and a stream with both words
set, a guard word past the blocks. The sketch case compares the f64 and f32 fills, 700 samples (two chunk seams),
with the keyed `PhiloxRng` draws bit for bit.

| Tooth | win-debug | linux-gcc-debug |
|---|---|---|
| AVX2 stores blocks 2-3 and 4-5 swapped | Philox case, `[dtypes][kernels]` (1), `[sketch]` | Philox case, `[sketch]` |
| scalar loop swaps the stream words | the same three under `CRD_CPU_LEVEL=portable` | - |
| SR draws lose the domain tag | batch converts case | - |
| sketch fill starts one block late | `[sketch]` (`[rand]` passes: it bounds errors) | - |
| public Q8_0 quantizer skips its last block | `[dtypes][kernels]` (64), also at portable | - |
| dtypes.hpp includes `philox_blocks.hpp` again | - | `crd-check-include-budget` |

Each restore was rebuilt and passed. Both lanes pass crd-hesap-tensor-tests (44 cases), the decomp (9), nn (18) and
stats (141) suites, `smoke_hesap_tensor`, every hesap-tensor and stats forced arm and the include budget; the win-debug
guards pass. Strict tidy, clang-format (touched lines; no file above HEAD's count) and Allman are clean.

One guard run on win-debug failed `crd-check-allocation-results` in 0.23 s with no output (it takes about 2.5 s and
prints on every path); seven reruns of the whole guard set passed, and `scripts/tsan-host-check.py` (20 rounds, four
arms) gave 0 events. It is recorded, not chased ([host faults](../../MEMORY.md)).

## Board

linux-gcc-release archives; each probe compiled with the tree's general flags (`-O3 -mavx2 -mfma -msse4.2 -mf16c`,
LTO), once with HEAD's hesap-tensor and hesap-stats headers first on the path, both linked to the same archives;
`taskset -c 4`, interleaved, medians. Binaries: dtypes HEAD `f91d25ff`, new `1d4193f1`; decomp HEAD `2a7d2ead`, new
`dfe23b7f`. The HEAD portable arm still drew at the general code's AVX2 level (only the narrowers were forced), which
an SSE2 CPU cannot run; the new portable column is that CPU's path.

| ns per element (7 rounds) | HEAD | avx2 | HEAD portable arm | portable |
|---|---|---|---|---|
| f32 to f16 SR | 1.138 | 1.116 | 1.857 | 2.468 |
| f32 to bf16 SR | 0.787 | 0.777 | 1.251 | 1.865 |
| f32 to e4m3 SR | 1.121 | 1.126 | 2.087 | 2.698 |
| quantize Q8_0 | 1.739 | 0.204 | 1.678 | 1.424 |

| hooi_rand, ms (5 rounds of best-of-5) | HEAD | avx2 | portable | TensorLy 0.9.0 |
|---|---|---|---|---|
| 64^3 rank 16 | 10.36 | 10.20 | 15.28 | 15.66 |
| 32^4 rank 8 | 56.65 | 57.42 | 89.62 | 102.56 |
| 128^3 rank 32 | 97.67 | 98.48 | 128.85 | 203.29 |

The SR rows and hooi_rand sit at HEAD parity (within 1.4%, both signs); the fits are identical. The public Q8_0
quantizer is 8.5x HEAD. No peer ships deterministic SR ([2026-07-02](../bench/2026-07-02-v14a-dtype-converts.md)).
TensorLy ran matched single-threaded (medians of three). The portable level is 1.3x to 1.6x behind avx2 on hooi_rand
and only at TensorLy parity on 64^3: an SSE2 Philox body (four blocks per vector on `pmuludq`) is the next lever, for
the stats and tensor families alike. `scripts/run_dtype_bench.sh` links the tensor archive for the draws now (run
once as committed).

## Left in the row

The SSE2 and SSE4.2 Philox block bodies first (the board above), then the rest the row lists.
