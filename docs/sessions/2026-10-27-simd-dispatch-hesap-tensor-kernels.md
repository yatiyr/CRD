# SIMD-DISPATCH: the hesap-tensor kernel family, part 1

<!-- doc-role: historical -->
> Dated evidence. Live owner: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). Contract:
> [ADR-0134](../decisions/0134-runtime-cpu-dispatch.md). Previous:
> [GCC fold](2026-10-27-simd-dispatch-gcc-cvtt-fold.md). Rules: [AGENTS](../../AGENTS.md).

## CI read

- The completed runs 40416850706 to 40424260124 are cancelled with no failed job. The three failed jobs of the
  in-progress run 40409214033 (head `cf4b8a13`: linux-gcc-relwithdebinfo 121876231527, -release 121876231573,
  -shipping 121876231690) are all `simd convert_truncate gives INT32_MIN`, the GCC fold `e9828cc2` fixed (an
  ancestor of HEAD). Push run 40425910553 is pending.
- Needs CI audit: no complete-tier run has finished since 40307186916, so DIAG.11c and DIAG.12 stay as audited.
  Nothing was dispatched while two runs are open.
- `docs/research/2026-10-27-networking-and-crash-pipeline-2026-comparison.md` is another writer's untracked research
  (its proposals wait for the user's approval); it was left untouched.

## Mechanism

- `crd_add_cpu_kernels(crd-hesap-tensor NAME tensor LEVELS portable avx2 INCLUDES crd-math ...)` builds
  `engine/numerics/hesap-tensor/kernels/tensor_kernels.cpp` per level; `src/tensor_kernel_dispatch.cpp` resolves the
  `TensorKernels` table (`tensor_kernels.hpp`, `tensor_dispatch.hpp`). Two routines, both bit-exact across levels:
  the f32 region of a permute (`PermuteRegionF32`: one call per plane on the serial path and per super-block task on
  the multithreaded one; the AVX2 body runs the 8x8 transpose, non-temporal stores and their fence) and the Tier R
  binned sum (the twelve-accumulator AVX2 body above 96 elements, the scalar deposits otherwise).
- `permute_tiles.hpp` (the scalar tile and the region's tile loop) and `binned_core.hpp` (the ReproBLAS fold-3
  operations as free functions on the plain `BinnedRungs`, bit casts through the builtin) live in
  `CRD_MATH_LEVEL_NS`; the kernel unit includes nothing else of the module. `reduce.hpp`'s `BinnedF64` (the public
  `BinnedAccumulatorF64`) wraps `BinnedRungs` with `add` and `convert`.
- permute.hpp and reduce.hpp no longer test `__AVX2__` or include intrinsics; a new include-budget entry holds
  permute.hpp off `<immintrin.h>` on GCC and Clang (a tooth re-adding the include failed it on linux-gcc-debug).
- `crd_add_kernel_level_tests` registers `crd-hesap-tensor-kernels-at-<level>` (`[kernels],[repro]`) and
  `crd-hesap-tensor-permute-kernels-at-<level>` (every `[permute]` gate).

## Changes beyond the move

| Finding | Now |
|---|---|
| A NaN's payload and sign in the binned read-out depended on the level and the partition (the AVX2 ladder sets the sticky bit in it, merges keep the first operand's) | `binned_convert` returns the one quiet NaN; infinities stay exact |
| `permute_tile_staged_f32` had no caller since the repository import (its 64 KB stack stage is the whole small fiber stack) | deleted; the 3D 8T row is re-measured below |
| The serial non-temporal path was unreachable (`kPermuteStreamBytes = ~0`; NT stores measured slower single-threaded on 2026-07-02) | removed with its gate |
| The outer odometer order was written twice | `permute_outer_order`, shared by both paths |
| Touched-file tidy findings: nested conditionals, the local constant `kW` | fixed |

## Verification

`test_tensor_kernels.cpp` (`[tensor][kernels]`): built levels; every level the host runs against a scalar oracle for
twelve region shapes (on and off the 8x8 grid, tiles that cut it, source strides 3, -1, -40 and 0), three alphas,
both store variants, NaN payloads and sentinel words around every row; the binned rungs (not only the read-out)
against portable for 11 lengths across the vector threshold and the endurance window, six inputs (cancellation, the
sticky-bit ties of powers of two from 2^30 to 2^-58, a late larger magnitude, a near-DBL_MAX window, an infinity,
two NaN payloads) and a non-empty accumulator, chunked merges; the resolved level.

| Tooth (win-debug) | Failed |
|---|---|
| 8x8 shuffle: one row takes the wrong halves | region differential |
| right edge strip dropped | region differential |
| stream variant ignores the scale | region differential |
| AVX2 deposit drops the sticky bit | rung differential (survived the read-out-only check until the tie input) |
| speculative widening keeps the unrolled lanes | binned differential |
| NaN read-out keeps the payload | NaN differential and the quiet-NaN check |
| scalar tile reads the a stride for b | region differential |
| table resolved from `detected` | resolved-level case at `CRD_CPU_LEVEL=sse2` |

linux-gcc-debug: the shuffle, sticky-bit and widening teeth failed the same cases; each restore was rebuilt and
passed. Both lanes pass the tensor, permute, einsum, nn, decomp and cli suites, `smoke_hesap_tensor` (the ADR-0096
isolation gate), the forced arms and the ctest guards; strict tidy (both levels of the kernel unit are analysed),
clang-format, Allman and the standalone header check (MSVC and GCC) are clean. `run_permute_bench.sh` and
`run_reduce_bench.sh` now build on the mirror's linux-gcc-release tree, link the tensor archive, run each level and
then the peers.

## Board

linux-gcc-release, nine interleaved rounds (medians, ns/element; host idle), 1T on `taskset -c 4`, 8T on the even
CPUs 0-14. HEAD binaries compile the same bench sources with HEAD's tensor headers first and link the same archives
(SHA prefixes: bench_permute `0387eaa9` HEAD, `ff3b430c` new; bench_reduce `ce9ee8c5`, `454f35c0`).

| Case | HEAD | avx2 | portable | Peer (same rounds) |
|---|---|---|---|---|
| 2D 4096^2 {1,0}, 1T | 0.517 | 0.508 | 0.727 | HPTT 0.507 |
| 4D 64^4 reversal, 1T | 0.467 | 0.462 | 0.559 | HPTT 0.393 |
| 3D 512^3 {1,2,0}, 1T | 0.519 | 0.523 | 0.634 | HPTT 0.543 |
| 2D, 8T | 0.166 | 0.168 | 0.235 | HPTT 0.175 |
| 4D, 8T | 0.145 | 0.152 | 0.203 | HPTT 0.212 |
| 3D, 8T | 0.162 | 0.163 | 0.220 | HPTT 0.158 |
| Tier R sum, 1M | 0.221 | 0.222 | 2.269 | ReproBLAS 0.349 |
| Tier R sum, 16M | 0.508 | 0.519 | 2.434 | ReproBLAS 0.551 |

The avx2 level is at HEAD parity on every row (the ranges overlap). Against the 2026-07-02 board HPTT moved on this
host (4D 1T 0.485 to 0.393) while Cerid did not, so the 4D single-thread row is now 1.18x behind HPTT and 3D 8T is
level within noise; both are open in the row. The portable column is what an SSE2-only CPU runs today: Tier R loses
6.5x to ReproBLAS there, the permute 1.4x.

## Left in the row

The tensor family's other sites (the `nn.hpp` Q8_0 and int8 dots and quantizers, softmax, the dtypes SR draws and the
decomp sketches), SSE2 bodies for both routines here (Tier R's two-lane ladder, a 4x4 transpose), the 4D 1T and 3D
8T permute levers against HPTT (a staged full-column strip needs its stage off the 64 KB small fiber stack), and the
rest the row lists.
