# SIMD-DISPATCH: per-level kernel units, the dispatch table and the first dispatched family

<!-- doc-role: historical -->
> Dated evidence. Live owner: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). Contract:
> [ADR-0134](../decisions/0134-runtime-cpu-dispatch.md). Previous:
> [detection](2026-10-26-simd-dispatch-cpu-detection.md). Rules: [AGENTS](../../AGENTS.md).

## CI read

- The completed runs 40341016749 to 40389677682 are cancelled with no failed job. The one failed job, win-clang-cl on
  the in-progress run 40351178841 (head `e4c56fb8`, the post-window flight-ring case), is repaired by `13f8e651`, an
  ancestor of HEAD ([races](2026-10-26-ci-audit-and-flight-ring-races.md)).
- Needs CI audit: no complete-tier run has finished since 40307186916, so DIAG.11c and DIAG.12 stay as that audit left
  them. Nothing was dispatched: push run 40390536674 is pending.

## Mechanism

- `crd_add_cpu_kernels(<archive> NAME <family> LEVELS portable ... SOURCES ...)` (`cmake/CrdSimd.cmake`) compiles a
  family's kernel sources once per listed level the build compiles, as one object library per level with that level's
  flags (MSVC `/arch:SSE4.2|AVX2|AVX512`, clang-cl `/clang:-march=x86-64-v2` for SSE4.2, GCC and Clang
  `-march=x86-64`, `-v2`, `-v3`, `-v4`), `CRD_KERNEL_LEVEL` and the ADR-0063 options, never crd-simd-flags' global ISA.
  The objects join the archive, which gets `CRD_KERNELS_<FAMILY>`, the mask of built levels.
- `CRD_KERNEL_LEVELS` follows `CRD_SIMD_LEVEL` (scalar: portable; sse2: up to sse2; avx2: up to avx2; auto and native:
  up to avx512; ARM64: portable and neon). `CRD_KERNEL_LEVEL_CAP` on crd-simd-flags is the dispatcher's ceiling
  (`kernel_level_cap()`), per configuration under the native Visual Studio profiles.
- `crd/math/kernel_level.hpp` names the unit's level namespace, `CRD_MATH_LEVEL_NS`: a header shared by general code
  and kernel units puts its inline functions in that inline namespace, so a copy compiled for a higher ISA never
  shares a symbol, or a linker-merged body, with general code's. Such headers avoid std templates for the same
  reason (the compiler's bit cast builtin, not `std::bit_cast`).
- `crd/math/kernel_dispatch.hpp`: `KernelLevel<Table>`, `select_kernel_level` (the highest built level both the
  selection and the cap include) and `kernel_level_cap()`. A family's dispatcher lists its built tables and resolves
  one, once, from `cpu_selection().selected`; every built level stays callable.
- First family, the batch small-float conversions (nine routines): the scalar cores moved to
  `crd/math/float_convert_scalar.hpp`, the portable and AVX2 bodies to `kernels/float_convert_kernels.cpp`, the
  dispatcher to `src/float_convert_dispatch.cpp`, which refuses a built level without a table entry.
  `crd/math/float_convert.hpp` keeps its API and no longer includes `<immintrin.h>`; an include-budget entry holds it.
- `CRD_CPU_LEVEL=portable` now applies on every architecture (x64 refused it as another architecture's level): it runs
  the portable reference kernels in a SIMD binary.
- `scripts/run_dtype_bench.sh` runs on the mirror's linux-gcc-release tree with `CRD_CPU_LEVEL` arms; its paths had not
  existed since the engine tree moved.
- Found in passing: `scripts/check-headers.py` failed every header on GCC (`#pragma once in main file`, an error
  under `-Werror` that no option turns off). GNU drivers now compile the header through a one-line unit that includes
  it; a repository-tools case holds it (tooth: the unit skipped), and a header with its include removed still fails.

## Verification

Four `[kernels]` cases in crd-math-tests: the selection rule, the built levels against the configuration's top level,
each level the host executes against the scalar converts bit for bit (42 class patterns and 65,536 random ones, every
length 0 to 19 from an odd offset, all 65,536 f16 patterns widened, stochastic rounding with per-element draws), and
the process's resolved level. `crd-math-kernels-at-<level>` CTests run them once per compiled level with
`CRD_CPU_LEVEL` set (ADR-0134 §8 arms). The native Visual Studio profile probe asserts each configuration's
`CRD_KERNEL_LEVEL_CAP`.

| Tooth (win-debug) | Failed |
|---|---|
| f16 rounding immediate set to zero | the differential, every arm, the hesap-tensor batch case |
| SR deep-lane patch skipped | the differential, every arm |
| e4m3 slot given the e5m2 routine | the differential, every arm |
| table resolved from `detected` | the portable, sse2 and sse4.2 arms |
| avx2 body not built | the built-levels case, every arm |
| portable refused on x64 | the CRD_CPU_LEVEL override case |
| scalar configuration capped at sse2 (native profiles) | the DebugScalar probe of test-native-build-profiles.py |

The f16 tooth on linux-gcc-debug failed the differential, every arm and the hesap-tensor batch case. After the
restores the cases, the arms, the cpu and v14-a cases and the guards pass on win-debug and linux-gcc-debug; strict
tidy (both levels of the kernel unit are analysed: a probe in the AVX2 body was reported), clang-format, Allman
and the standalone header check on MSVC and GCC are clean.

## Board

`run_dtype_bench.sh`-style probe on linux-gcc-release, `taskset -c 4`, 1M elements, seven interleaved rounds (ns per
element, medians): HEAD's inline header compiled with the engine's general flags against the dispatched kernels;
the portable arm (`CRD_CPU_LEVEL=portable`) is one run of the board script.

| Routine | HEAD | Dispatched (avx2) | Portable arm |
|---|---|---|---|
| f32 to f16 | 0.097 | 0.097 | 1.209 |
| f16 to f32 | 0.103 | 0.103 | 0.736 |
| f32 to bf16 | 0.119 | 0.123 | 0.265 |
| f32 to e4m3 | 0.595 | 0.587 | 1.286 |
| f32 to e5m2 | 0.600 | 0.599 | 1.126 |
| f32 to f16 SR | 1.397 | 1.199 | 1.902 |
| f32 to bf16 SR | 1.069 | 0.847 | 1.318 |
| f32 to e4m3 SR | 1.342 | 1.182 | 2.111 |

Peers on the same run: numpy f32 to f16 1.202, torch 0.139, ml_dtypes bf16 0.335 and e4m3 1.322. The portable arm is
the scalar reference an SSE2-only CPU runs today: SSE2 bodies are in the row's remaining work.

## Left in the row

Level-aware SIMD types (backend.hpp keyed on the unit's level, the types in `CRD_MATH_LEVEL_NS`, an SSE4.2 backend,
a level-independent `k_native_lane_width`); the hesap and geometry kernels as per-level units; SSE2 and SSE4.2 bodies
where they beat the portable loop (these conversions included) and AVX-512 bodies under SDE; the baseline flip and
the `-sse2` lanes as forced arms; the isolation guard (tidy restating each unit's own `/arch` comes with the first
AVX-512 body); the level in the doctor, host tuple and board records; the boards per level.
