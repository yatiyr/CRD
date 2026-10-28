# 2026-10-28 — SIMD-DISPATCH: NEON parity and the hosted ARM64 lanes

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). HEAD before: `1597b0a2`.

## CI first

No completed run has a failure left to fix. Complete run 40649898318 (`3e849d96`) finished with five
linux-clang-tsan failures from the out-of-line fiber-switch hooks
([eylem note](2026-10-28-simd-dispatch-avx512-eylem.md#ci-first)). Schedule run 40663338177 (`06ff67a5`, which holds
`38b982f2`) is the hooks fix's hosted proof: its linux-clang-tsan job (122755707795) fails only the nonlinear CG moat,
which `31f8c3d0` (newer than `06ff67a5`) fixed. DIAG.11c and DIAG.12 still wait on a finished complete-tier run.

## NEON backend (`e6e66e3a`)

The crd-math SIMD types' NEON branches serve general code on ARM64 and the level probe's neon unit. Every family
still builds portable there; per-family NEON kernel units and NEON exp/log twins are
[JOBS-PORT.2](../ROADMAP.md#slice-jobs-port.2)'s ("per-level kernel units beside NEON").

- **min and max** were the only bit change: `vminq`/`vmaxq` return NaN and order -0 below +0, where portable and x86
  return the second operand. They now select on the comparison (`vbslq(vcltq(a, b), a, b)`), as for f64.
- **fma** is FMLA (`vfmaq(c, a, b)`), one rounding, instead of the C library per lane.
- **Vec4d** has a NEON backend: two `float64x2_t`, every operation in its own NEON form (FMLA/FMLS, `vld2q`/`vst2q`
  for the complex loads, `vzip` transposes, the SSE2 lane forms for addsub and the partial loads).
- **Vec8f complex helpers** (swap_pairs, addsub, fmaddsub, the unit unpacks, blend, half stores, transpose8x8) run on
  NEON registers instead of scalar copies.
- **Shifts** assert a count in [0, 32), and a zero right shift returns its operand (NEON's immediates take 1 to 32).
- **convert_truncate** converts only the lanes the scalar range test accepts, so no lane relies on FCVTZS's
  out-of-range result.

The new `[simd]` case pins min and max on a NaN in either operand and on both signed-zero ties for Vec4f, Vec8f,
Vec16f, Vec4d and Vec8d (passes on win-debug). x64 compiles none of the NEON branches; Clang 22's
`--target=aarch64-pc-windows-msvc` compiled the headers, `test_simd.cpp` and the level probe at neon without a
warning. Runtime proof is the ARM64 lanes.

Residuals, each owned: FPCR (FZ, DN, rounding) is not read on ARM64 yet, so ADR-0063's no-FTZ rule goes unchecked
there (`fp_environment.hpp`, JOBS-PORT.2); ARM's default NaN is 0x7FC00000 against x86's 0xFFC00000, so no
bit-compared operation may return a NaN the hardware made (the probe compares arithmetic as values).

## ARM64 lanes

Hosted jobs `linux-arm64` (ubuntu-24.04-arm: `linux-gcc-arm64-debug`, `linux-gcc-arm64-asan`,
`linux-clang-arm64-debug`, `linux-clang-arm64-asan`) and `windows-arm64` (windows-11-arm, MSVC ARM64:
`win-arm64-debug`), change tier, in the required result. Toolchains are the images': GCC, Clang, Ubuntu's ninja-build
and gdb on Linux (the pinned Ninja and sccache are x86-64), the pinned x64 Ninja under emulation and
`msvc-dev-env.py --arch arm64` (vcvarsarm64.bat) on Windows. No compiler cache, no Vulkan, no SDE.

crd-jobs stops an ARM64 build (`fiber_init.cpp`: no ARM64 fiber context before JOBS-PORT.2), so the presets set
`CRD_MODULES_WITHOUT=jobs`. The new registry option keeps every module whose closure, with its test dependencies,
avoids the named modules: 31 modules with their tests (core, units, log, containers, toml, vm, time, math, hesap,
hesap-fft, the geometry primitives, convex, polygon and Delaunay, eylem, ceir, the render assets, kir and its
emitters, the cooks), 36 configured. The lanes widen with no workflow edit once crd-jobs builds there.

Making that set configure found a latent defect: `support/diag`, shared by core, perf, memory and jobs, linked
crd-jobs and crd-perf unconditionally, so any selection with core and without them (`CRD_MODULES=core` included)
failed to configure. The jobs and perf specimens now exist where their module is configured; the crash-capture
specimen links crd-jobs where it builds, and its three fiber modes (and core's tests of them) exist there
(`CRD_DIAG_FIBER_MODES`). On x64 nothing changes: win-debug `[crash-capture]` passes 11 cases, the fiber case
included. `CycleCounter` reads the generic timer on MSVC ARM64 (`_ReadStatusReg(ARM64_CNTVCT)`), not the
monotonic-clock fallback.

Verified locally: the registry fixture (13 cases, a new one for the option), a win-debug configure with
`CRD_MODULES_WITHOUT=jobs`, `check-ci-tiers.py`, the tooling tests and actionlint. The ARM64 lanes' first run is the
hosted proof still owed.
