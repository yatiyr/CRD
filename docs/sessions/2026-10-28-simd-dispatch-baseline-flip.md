# 2026-10-28 — SIMD-DISPATCH: general code builds at the SSE2 baseline

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). HEAD before: `7f8440da`. ADR-0134 §4.

## CI first

Push run 40748658864 (`f7304724`) was still running; its finished jobs had failed for two causes, fixed in `fcb3bb42`:

- Every GCC and Clang lane, x64 and ARM64: `fft_kernels.cpp` used `std::memcpy` and `std::uintptr_t` but had
  reached `<cstring>` and `<cstdint>` only through the scalar math headers, which `f7304724` cut. It includes them
  itself. Ninja stops at the first failing unit, so the rest of the class was swept before the push: GCC
  `-fsyntax-only` over every first-party unit of the linux-gcc-debug tree that is new or depends on a file changed
  since the last compiled run (`3c1debc6`), 1,115 units, and the same over linux-clang-tsan, 1,093 units. Both were
  clean once the fix was in; with the include removed again the sweep failed fft's four units.
- win-arm64-debug, the lane's first build: `clocks.cpp` named `ARM64_CNTVCT`, which only the SDK's `winnt.h`
  defines. It reads CNTVCT_EL0 through `ARM64_SYSREG(3, 3, 14, 0, 2)` from `arm64intr.h`. The tick scale and
  hesap-stats' 64x64 multiply called `_umul128`, which ARM64 lacks; both take `__umulh` there.

No run completed green since the last note, so DIAG.11c and DIAG.12 stay Needs CI.

## What changed

- `cmake/CrdSimd.cmake`: crd-simd-flags carries no ISA flag. GCC and Clang general code is pinned to
  `-march=x86-64` (a distribution compiler may default to v2 or v3); cl and clang-cl take no `/arch`.
  `CRD_SIMD_TARGET` is 1 on x64, 3 on ARM64, 0 for `scalar`. `CRD_SIMD_LEVEL` keeps its meaning as the kernel cap:
  `auto|scalar|sse2|avx2|neon`; `native` is refused with the reason, and `CRD_SIMD_LEVEL_RESOLVED` names the backend
  of general code (`sse2`, `neon` or `scalar`). `CRD_SIMD_MSVC_ARCH_FLAG` and its tidy consumers are gone: the kernel
  units select on the dash-spelled `CRD_KERNEL_LEVEL` that clang-tidy reads.
- `backend.hpp` refuses `CRD_SIMD_TARGET=2`, so a stale package or ad hoc command fails loudly.
- Presets: `win-debug-sse2`, `linux-gcc-debug-sse2`, `win-vs-debug-sse2` and the native `DebugSSE2` configuration are
  gone. The test presets `win-debug-at-sse2` and `linux-gcc-debug-at-sse2` run the lane's binaries with
  `CRD_CPU_LEVEL=sse2`. The `Test at the sse2 level` preset step of the windows and linux-gcc jobs runs them after
  the lane's own pass, first the hidden `[cpu-level-applied]` case, and checks the JUnit against the register as
  lane `<preset>-at-sse2` ([CI tiers](../design/ci-tiers.md)). Tests that force their own level keep it. The
  evidence bundle reads only the lane's `ctest-junit.xml`; the arm keeps the lane pass's `LastTest.log` as
  `LastTest-lane.log`, since its own CTest rewrites the file.
- No general unit carries a higher ISA: the win-debug and linux-gcc-debug compile databases have none outside
  `/kernels/`, and no CMake file adds one.
- The emission check expects the baseline (no ymm in `test_simd.obj`) on every non-scalar x64 lane.
- `test_simd_transcendental.cpp`'s 500,000-sample moat ran only under an AVX2 general build; it now runs on general
  code's two-lane twins, and the four- and eight-lane twins stay held to portable's bits per level by the SIMD-type
  probe.
- The GEMM benches estimate their peak from the dense kernels' dispatched level instead of a compile-time macro and
  print that level. The tensor bench drivers build at `-march=x86-64` like the engine. `run_gather_probe.sh` and
  `run_pipeline_probe.sh` built sources that no longer exist and were deleted.
- The include budget's intrinsics bans now cover `<emmintrin.h>` and `<xmmintrin.h>` in every engine entry: general
  code on GCC and Clang now reaches `<emmintrin.h>`, not `<immintrin.h>`, through `backend.hpp`.

## Checks

win-debug reconfigured and built crd-math-tests, crd-time and the two GEMM benches; no general unit carries
`/arch:AVX2` (34 kernel units do). `[transcendental]` 68/68 and the guard set with the emission check 16/16. The
tooling tests (`test-dev-workflow.py`, `test-repository-tools.py`, `test-native-build-profiles.py`), `check-ci-tiers`
and the pinned actionlint pass. The linux-gcc-debug tree, reconfigured with the new flags (general units
`-march=x86-64`, none `-mavx2`; kernel units keep their `-march=x86-64-vN` last), passed `-fsyntax-only` over all
1,940 first-party units, and the tightened include budget there: 27 of 27 headers within. Hosted CI runs the rest.

## Left for the row

The isolation guard replaces the emission check and its scripts; the level in the doctor, the host tuple and the
board records; the boards per level. The row-close diag-overhead and jobs boards read what general code lost at the
baseline (AVX2 autovectorisation, `roundss`, `popcnt`); the hesap vs-reference benches compile Eigen inside the
bench unit, so its peer runs at the baseline there too, which the boards must account for.
