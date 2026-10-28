# 2026-10-28 — SIMD-DISPATCH: the forward jets move into the level namespace

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). HEAD before: `f7304724`.

## CI first

Push run 40701630725 (`3c1debc6`) completed. Its failed jobs are the six the
[batched note](2026-10-28-simd-dispatch-tensor-batched-kernels.md#ci-first) lists, fixed at HEAD, plus linux-gcc-asan
(4 h 20 min, inside its limit). That job had two failing tests: `assetio: OUR tangents vs the REFERENCE mikktspace.c
ORACLE`, which is registered third-party defect [TP-5](../third-party-defects.md#tp-5), and `Commands add_component and
set_component copy an lvalue once and move an rvalue without a copy` (`component_registry.hpp:123`, a store to a
misaligned 64-aligned component). `c98b31fe` fixed that one ([note](2026-10-28-simd-dispatch-ci-fs6-and-command-payloads.md)).
The other runs among the last 15 were cancelled before any job ran. No green run has completed, so DIAG.11c and DIAG.12
stay Needs CI.

## What changed

- `jet_simd.hpp` now lives in `crd::hesap::autodiff::forward::inline CRD_MATH_LEVEL_NS`. It includes the specific
  level-namespaced scalar headers, not `cmath.hpp`, which reaches `complex.hpp`.
  - The pack registers are a Vec8d while more than four partials remain at avx512, and a Vec4d otherwise. So `JetPackD<8>`
    is one zmm there, and `JetPackD<4>` stays one ymm.
  - `kRegs` and `kLanes` follow the level.
  - The unit seed is built from lane compares; the function-local static is gone.
  - The recursion is one `if constexpr` template per operation.
  - The helper namespace is `pack_detail`. A level-namespaced `detail` would make `forward::detail` (jvp_rules.hpp)
    ambiguous.
  - A new constructor seeds an arbitrary tangent from N values.
  - `Jet` and `JetPackD` spell their width `kDimension`. Ceres's `DIMENSION` fails the naming rule now that strict tidy
    reads the header.
- The gradient and Jacobian drivers moved from `drivers.hpp` into `jet_simd.hpp`. They take raw pointers, because a
  kernel unit cannot use `Span`, whose inline bodies are in the plain namespace. The tile width is deduced from the
  scratch. `drivers.hpp` keeps `jvp` and `directional`.
- `forward.hpp` no longer includes the jets. Units that name `JetPackD` include `jet_simd.hpp`. `include-budget.json`
  holds this down: `autodiff/forward.hpp` and `opt/forward_ad.hpp` must reach no intrinsics header on GCC and Clang.
- New `autodiff` kernel family (portable, sse2, avx2, avx512):
  - `kernels/autodiff_kernels.cpp`, the data-only `autodiff_kernels.hpp`, and `autodiff_dispatch.hpp` with its .cpp.
  - It runs `hesap.ad.gradient.f64`'s canned functions (`canned_functions.hpp`, level-namespaced). The CLI makes one
    table call per gradient. The Hessian and the value use the same functors in general code.
  - No sse4.2 level: it would run sse2's code. No neon: hesap-autodiff links crd-jobs, which the ARM64 lanes do not
    build yet ([JOBS-PORT.2](../ROADMAP.md#slice-jobs-port.2)).
- ADR-0134 gains two amendments: §5 covers level-following carriers and the functors over them, and §7 declares the
  jets bit-exact. The system doc notes the levels.

## Tests

- `test_jet_levels.cpp` reads a test-owned per-level family, `jet_probe` (`kernels/jet_level_probe.cpp` and
  `jet_probe_ops.hpp`).
  - Coverage: 29 operations and slopes at N in {1, 3, 4, 5, 7, 8, 9, 15, 16, 17, 32}, and the drivers on ragged and
    full tiles of 3, 4, 5, 8 and 16.
  - Inputs: 62 sets. Specials appear in values and partials: signed zeros, subnormals, extremes, infinities, NaN
    payloads, the exp edges and π/2.
  - Every level must give portable's bits, NaN as a class, and so must general code's jets.
  - Guard words follow every result.
  - Each level's layout is checked: registers, lanes, alignment and size.
  - On well-conditioned inputs, results must lie within 1e-12 of the scalar Jet oracle.
- The `[kernels]` cases cover the canned table (levels against the build mask and the selection, every level against
  general code's driver, and a guard word). The probe and the CLI gradient case also have the tag. They run as
  `crd-hesap-autodiff-kernels-at-<level>` and under SDE.
- The SIMD-type probe's cases (`test_simd_levels.cpp`) were tagged `[math][simd-levels]` only, so the hosted SDE arm
  never ran them. They now also carry `[kernels]`, as every other family's cross-level cases do.
- Local, win-debug:
  - The tree builds keep-going with no error.
  - The autodiff and opt suites pass.
  - `[jet-levels],[kernels]` passes natively, forced to sse2, and under SDE 10.13.1 `-skx` at avx512.
  - The include-budget guard passes.

## Board (row close)

At the row-close board, `external/crd_v15a_forward_bench.cpp` needs to link the family or be rebuilt per `-march`. It
names `JetPackD` directly, so it compiles unchanged. The board keeps the Vec8d pack only if it wins at N = 8 and 16.

## Next

The baseline flip with the `-sse2` forced arms.
