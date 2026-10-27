# SIMD-DISPATCH: feature variants and AVX-VNNI on avx2

<!-- doc-role: historical -->
> Dated evidence. Live owner: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). Contract:
> [ADR-0134](../decisions/0134-runtime-cpu-dispatch.md). Previous:
> [nn kernels](2026-10-27-simd-dispatch-hesap-tensor-nn-kernels.md). Rules: [AGENTS](../../AGENTS.md).

## CI read

- Completed runs 40438126144 to 40443988971 were cancelled with no job run. Run 40409214033 (`cf4b8a13`) failed
  linux-gcc-release, -relwithdebinfo and -shipping on `simd convert_truncate gives INT32_MIN` only (the GCC fold
  `e9828cc2` removed); push run 40434280766 (`d6528387`, in progress) has those three lanes green, the hosted proof of
  that fix. 40445742611 (`8ded3813`) is pending; nothing was dispatched.
- Needs CI audit: DIAG.11c and DIAG.12 wait for a complete-tier run after C2 (the newest, 40307186916 at `ae7bdc23`,
  predates it); both stay Needs CI. This batch changes `cmake/`, so its push run resolves to the complete tier.

## Mechanism

ADR-0134 §1 makes AVX-VNNI a feature an avx2 kernel selects on, not a level. Until now the dpbusd bodies sat under the
compiler's `__AVXVNNI__`, which no preset defines and cl never defines, so no build ran them.

- `crd_add_cpu_kernels(... VARIANTS avx2+avxvnni ...)` (cmake/CrdSimd.cmake) builds the level's unit again as
  `<archive>-<family>-avx2-avxvnni` with `CRD_KERNEL_FEATURE_AVXVNNI=1` and `-mavxvnni` (GCC, Clang, clang-cl; cl
  compiles every intrinsic at `/arch:AVX2`), and gives the archive `CRD_KERNELS_<FAMILY>_AVX2_AVXVNNI`. A variant of
  a level the family lacks, or a feature that does not extend that level, stops the configure. The per-unit body moved
  into `_crd_add_kernel_unit`, shared by levels and variants.
- `kernel_level.hpp` gives the variant its own namespace (`avx2_avxvnni`) and table suffix (`Avx2AvxVnni`), so none of
  its inline copies merges with the plain avx2 unit's, and `CRD_KERNEL_HAS_AVXVNNI` replaces `__AVXVNNI__` in
  `tensor_kernels.cpp`.
- `KernelLevel` carries the variant's `CpuFeatureSet`; `select_kernel_level(levels, selected, features, cap)` takes the
  last table whose level both include and whose features `features` holds; `process_kernel_level` is the one call the
  three dispatchers make with `cpu_selection()` and the cap.
- `CpuSelection::kernel_features` is `features.usable`, or under an applied `CRD_CPU_LEVEL` only what that level is
  defined by (`cpu_level_features`, the psABI v1 to v4 sets `cpu_level_of` now reads too). A forced arm therefore runs
  as the smallest CPU of its level: `crd-hesap-tensor-*-kernels-at-avx2` runs the plain avx2 bodies, the unforced
  suites the variant. No new override or CTest arm was needed.

clang-tidy builds no code, so a target-feature mismatch of an intrinsic never reaches the analysis: the variant needs no
tidy restatement. A first draft restated each unit's ISA to the analysis (CMake and `cerid_dev/tidy.py`); it changed
no result and was removed.

## Verification

`test_kernel_dispatch.cpp`: a variant is taken only with all its features, outranked by a higher level, capped by the
build, never lifting the level. `test_cpu_features.cpp`: kernel features under no, unknown, other-architecture and
applied overrides, and each level's definition holding the one below. `test_tensor_kernels.cpp`: the archive holds the
variant exactly when it holds avx2, ascending order with a variant after its level's plain table, and the process runs
the dpbusd table exactly when its kernel features hold AVX-VNNI; every differential runs the variant where the host
has the feature (`runs_here`), and failures name it (`avx2+avx-vnni`).

| Tooth | Failed |
|---|---|
| Q8_0 dpbusd operands swapped | Q8_0 linear differential, variant only (win-debug and linux-gcc-debug) |
| row-major dpbusd drops the accumulator | int8 differential, variant row-major only |
| packed dpbusd drops the accumulator | int8 differential, variant packed only; the unforced nn `[i8]` gates fail, the `CRD_CPU_LEVEL=avx2` run passes |
| selection ignores the features | feature-variant selection cases; `kernels-at-avx2` (the process runs a table its features lack) |
| a forced level keeps every feature | the kernel-features case |
| the variant entry lists no feature | the archive case (order and the plain avx2 count) |

Each restore was rebuilt and passed. win-debug and linux-gcc-debug pass crd-core, crd-math, crd-hesap-stats and the
three hesap-tensor suites in full, every forced arm and the ctest guards; on both, the unforced process runs
`avx2+avx-vnni` and `CRD_CPU_LEVEL=avx2` runs `avx2`. Strict tidy (13 files), the clang-format touched-line oracle and
Allman are clean.

During the guard runs `crd-check-allocation-results` failed twice on win-debug in about ten parallel guard runs (once
after 2.1 s, once at its 120 s timeout, no output kept). It passed alone five times, 36 times as 12 parallel copies of
itself and in four more parallel guard runs; it only reads the engine, tools, runtime and sandbox sources, and this
batch adds no allocation call. `scripts/tsan-host-check.py` (30 rounds, pinned to CPUs 0 and 1 beside two busy loops)
then found one event on this host: the Cerid-free fiber control died with a TSan SEGV on a read (1 of 30; the jobs arm,
the plain and the unsanitized controls 0 of 30 each). The host fault is still present, so the two guard events are
recorded as local host events, not code defects; the hosted lanes run the guard on every push.

## Board

linux-gcc-release archives, bench_nn compiled with the tree's own flags (`-O3 -mavx2 -mfma -msse4.2 -mf16c`, LTO), one
binary (SHA prefix `728dc9a9`): unforced it runs the variant, `CRD_CPU_LEVEL=avx2` the plain avx2 tables (HEAD's
bodies, unchanged). Seven interleaved rounds on `taskset -c 4`, best-of-50 each; torch 2.12.1 and onnxruntime 1.27.0
(installed, single thread) in rounds 1, 4 and 7; medians in ns per sample.

| Case | avx2 + AVX-VNNI | avx2 | ort-int8 | torch-int8 |
|---|---|---|---|---|
| MLP i8, batch 16 | 168.6 | 352.4 | 448.1 | 2441.5 |
| MLP i8, batch 4096 | 164.5 | 349.7 | 181.3 | 411.4 |
| MLP Q8_0, batch 16 | 336.2 | 363.1 | | |
| MLP Q8_0, batch 4096 | 358.8 | 383.7 | | |
| CNN i8, batch 8 | 1905.9 | 1954.4 | 15236.5 | 18937.9 |
| CNN i8, batch 4096 | 2017.5 | 2026.8 | 17791.0 | 14308.9 |
| CNN Q8_0, batch 8 | 1928.8 | 1931.6 | | |

The variant doubles the int8 MLP and takes 7% off Q8_0's; the int8 tier beats ort-int8 (which dispatches to VNNI on
this CPU) by 1.10x at batch 4096 and 2.7x at 16, where the plain avx2 bodies lose to it at 4096 (1.93x). The CNN rows
are bound by the f32 convolutions. The f32 rows, whose code is the same in both arms, moved by up to 4% between them
(the layout floor of two table copies).

## Left in the row

The dtypes SR draws and the decomp sketches through a tensor-kernel Philox block fill, then the rest the row lists.
