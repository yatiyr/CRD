# Public consumption: self-contained headers through the consumer view and a relocatable package

<!-- doc-role: historical -->
> Dated evidence. Live owner: [REPO.DEV.8](../ROADMAP.md#slice-repo.dev.8); contract:
> [public consumption](../design/public-consumption.md). Rules: [AGENTS](../../AGENTS.md).
> Preceding batch: [build board](2026-09-14-build-performance.md).

## User direction

The standing direction is unchanged: carry the REPO.DEV slices on "one by one until AUD-2". REPO.DEV.8 followed
REPO.DEV.7 in the same session; the advisor ruled at the design gate (one option and two complete-tier lanes;
header checks link the module target `PRIVATE`; exclusions need a reason; the package is per profile; the consumer
test qualifies the lane's own toolchain; the local check reuses the tidy synthesis with redirected outputs) and the
increment ran directly. No hosted run was in flight; the latest published run stays 34780682504 (`18651d5`).

## What changed

- [cmake/CrdPublicChecks.cmake](../../cmake/CrdPublicChecks.cmake), included by the root build file and called after
  `crd_add_modules()`: `CRD_PUBLIC_CHECKS` (default OFF, the default graph unchanged) generates one shim
  translation unit per public header (`include/**/*.hpp`, `*.h`; `*.in` and `*.inc` are not headers) into an
  OBJECT library per module that links only the module's own library targets (the consumer view), the aggregate
  `crd-header-checks`, a reasoned `crd_public_header_exclude()` that fails on a stale entry, a configure summary
  naming the modules without a library target on the host, and the `crd-package-consumer` CTest registration with
  the configuration's generator, make program, compiler, build type and switches.
- [cmake/CrdPackage.cmake](../../cmake/CrdPackage.cmake) and [CeridConfig.cmake.in](../../cmake/CeridConfig.cmake.in):
  `crd_package_library()` (export name, headers without templates, the generated profile header) and
  `crd_package_config()` (the `Cerid` config, version and targets files under `lib/cmake/Cerid`), install rules
  only. [core](../../engine/foundation/core/CMakeLists.txt) exports `Cerid::core` with `$<BUILD_INTERFACE:...>`
  around its source and generated include directories and around `crd-warnings`; the root refuses
  `BUILD_SHARED_LIBS=ON`.
- Header fixes (nine missing includes) and boundary declarations: `crd-jobs` `PUBLIC` on `crd-ceir-host`;
  `hesap-tensor` a `PUBLIC` dependency of `hesap-resources` and `hesap-autodiff`; `hesap-direct` a `PUBLIC`
  dependency of `hesap-opt` (formerly test-only); the `hesap-dense` and `hesap` include directories published as
  include-only `PUBLIC` edges of `crd-hesap-tensor` (ADR-0096); the ImGui include directories `SYSTEM PUBLIC` on
  `crd-imgui`. The registry rows carry the new `DEPENDS` entries; every edge is acyclic.
- Presets `win-public-checks` and `linux-gcc-public-checks` (Debug bases, tests on), owned by the `windows` and
  `linux-gcc` jobs at the complete tier in [ci-tiers.json](../../.github/ci-tiers.json); 22 visible presets.
- [scripts/check-headers.py](../../scripts/check-headers.py): the local changed-surface check (tidy synthesis,
  outputs redirected to scratch, depfile flags dropped); [scripts/test-package-consumer.py](../../scripts/test-package-consumer.py):
  the install, move, consume, run, profile, relocatability and refusal proof (`--preset` locally).
- Tests: three `PublicConsumption` cases in `test-repository-tools.py` (a fixture module whose non-self-contained
  header is named by the keep-going build while the self-contained one compiles, the excluded fragment has no
  shim and a stale exclusion fails the configure; layout, profile expectation, relocatability and inherited-flag
  checks of the consumer test; the header-check command rewrite proving every output lands in scratch and no PCH
  or depfile flag survives on MSVC and GNU commands).
- Docs: [design/public-consumption.md](../design/public-consumption.md); BUILDING (7,000 bytes) names the option
  and the local check; scripts README; CI-tiers and module-registry designs; ROADMAP row 054; the pointer moves to
  REPO.DEV.9; two memory records.

## What the first complete run found

MSVC 19.51 (`win-debug` base, 1,118 headers of 96 modules; `kir-hip` and `kir-metal` build no library) failed 21
shims on the original tree. GCC 13.3 (1,110 headers of 93 modules; the DX12, WebGPU, HIP and Metal modules build
no library on Linux) first ran on the tree after the first fifteen fixes and failed the six hesap-tensor shims
(`blas3.hpp` needs `hesap/complex.hpp`, so the include-only edge had to carry `hesap` too), then none. None of the
21 was a fragment; no exclusion was written:

| Class | Count | Headers |
|---|---|---|
| Missing include | 9 | `math/mat_simd_f32.hpp`, `geometry/convex/sat.hpp`, `geometry/curves/queries.hpp`, `hesap/dense/detail/gemm_pack.hpp` (+ `syrk_microkernel.hpp`), `hesap/quadrature/cubature.hpp`, `qng.hpp`, `hesap/opt/sqp_equality.hpp`, `hesap/sparse/structural.hpp` |
| Public header over a `PRIVATE` link edge | 1 | `ceir/host/host_provider.hpp` (`crd::jobs::Priority`) |
| Public header over an undeclared module | 4 | `hesap/resources/tensor_artifact.hpp`, `hesap/autodiff/einsum_reverse.hpp`, `hesap/opt/newton_sparse.hpp`, `levenberg_marquardt_sparse.hpp` |
| Public header over a `PRIVATE` include-only edge | 6 | `hesap/tensor/batched.hpp`, `decomp.hpp`, `einsum_exec.hpp`, `nn.hpp`, `sparse_cp.hpp`, `tt.hpp` |
| Public header over a `PRIVATE` third-party include | 1 | `imgui/unit_preferences_inspector.hpp` |

After the fixes both keep-going builds exit 0. The clean MSVC shim board: 1,118 shims in 43.6 s at 16 jobs
(edge sum 693 s, parallelism 15.9, median 0.54 s, slowest `hesap/interp/interp.hpp` at 4.8 s); the whole
`crd-header-checks` target including the generated headers took 65 s of wall time. The GCC board ran on the 9p
mount and is not a measurement; the hosted lane's evidence bundle records it.

## Verification

- **Header checks.** MSVC: 21 failures on the original tree, then 6 (the hesap-tensor surfaces before the
  `hesap` include-only edge), then 1,118 of 1,118 shims compile (keep-going build exit 0); GCC under WSL2: 6
  failures on the tree after the first fifteen fixes, then 1,110 of 1,110 (exit 0).
- **Default graph.** `build/win-debug` reconfigured: the same 47,323 Ninja lines and 1,814 compile commands;
  160 compile commands changed by added `-I` flags only (hesap-tensor, hesap-dense, hesap-stats, hesap-direct and
  its closure, time and platform for the modules with new edges and their consumers, the ImGui directories for
  ImGui consumers); nothing removed; the re-run rule lists the new modules. The `$<BUILD_INTERFACE:...>` wrapping
  changed nothing.
- **Consumer test.** `--preset win-debug` on MSVC 19.51 with Ninja: configure 14.5 s, core built, installed
  (`crd-core.lib`, every public header, no template), moved, no source/build/pre-move path in any installed
  `.cmake`, consumer configured and built without `/W4 /WX`, ran and printed version 0.1.0, asserts 1, profiling
  1, debug 1, release 0, log level 0, `windows`/`msvc`/`x64`; the Release consumer refused ("was installed from the
  Debug profile"); `BUILD_SHARED_LIBS=ON` refused. `--preset linux-gcc-debug` under WSL2 with GCC 13.3: PASS with
  the same checks. The first Windows run caught a defect of the test itself (the engine configure had not received
  the preset's switches: profiling 0 against expected 1), which is the check working.
- **Local check.** `check-headers.py --build build/win-debug --changed` on the eight fixed headers of this
  working tree: 8 passed (0.2 to 0.5 s each, module-view commands from the compile database), every object and
  PDB under the temporary scratch directory, nothing written into `build/win-debug`; two named headers
  (`crd/log/log.hpp`, `crd/core/pch.hpp`) likewise.
- **Guards.** `check-ci-tiers.py` PASS (22 visible presets, 21 owned by 8 jobs); `test-native-build-profiles.py`
  7/7; `test-repository-tools.py` 43/43 with CMake, Ninja and MSVC on PATH; the lane-wide
  `test-module-selection.py` 12/12, `test-project-sync.py` 55/55 and `test-dev-workflow.py` 54/54 under the
  same environment (the new registry edges and `cmake/` files break none); `check-repository.py` PASS;
  `check-master-plan.py` PASS (863 rows, 1,046 documents, 9,070 links); `git diff --check` clean.
- **Scratch.** The Windows shim tree (`build/public-checks-win`), the Windows consumer scratch
  (`build/package-consumer-win`) and the WSL trees (`~/public-checks`, `~/package-consumer`) were removed
  afterwards; the logs stay in the session scratchpad only.
