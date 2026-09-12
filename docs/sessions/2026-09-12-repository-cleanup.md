# 2026-09-12 — repository cleanup and qualification

<!-- doc-role: evidence -->
> Dated session evidence. Live status: [ROADMAP](../ROADMAP.md#slice-repo).

## Authorization and scope

The user requested removal of stray repository files, a renewed ignore policy, grouping in both Visual Studio
Folder View and Targets View, and working tests/CI. They explicitly chose to rename nonconforming public names
and migrate callers. This authorizes source spelling changes, not geometry/physics algorithm redesign. Work was
direct; the advisor capability was unavailable. No commit or push was made.

## Preserved and reorganized

Baseline: `bc8fc1f51226166e539cb8c8f93b1dd829d343ef`. Before editing, all 4,596 tracked files were saved to
`build/repo-cleanup-20260912/before.zip` with path/size/SHA-256 manifest. The
[migration manifest](../research/2026-09-12-repository-migration.json) preserves old/new locations and recovery records.

All 96 engine modules and 103 test directories moved into named families. Public include paths, target names and
existing flat binary output directories remain stable. CMake registrations, source includes, scripts, generators
and current documentation links follow the new layout. Historical measurements and source excerpts retain their
meaning. CMake's generated model contains 291 targets with folders; the portable regression fixture checks family,
benchmark, application and dependency organization.

Three verified scratch fragments were deleted. Ambiguous diagnostics, dumps, cooked output and temporary helpers
were preserved intact under ignored `build/repo-cleanup-20260912/recovered/` (46 files with hashes). Automatic
approval review rejected the initial broad deletion; the narrower preservation/move operation was approved.
Optional local research books moved to `external/research-papers/docs-books/`; the reusable public-domain
Shewchuk reference moved to [bench/reference](../../bench/reference/README.md).

The ignore policy now keeps agent entry documents and source formats visible, while excluding generated output,
SDKs and personal settings. New repository/layout tests join the documentation and architecture guards in CTest
and Windows/Linux CI. LLVM 20.1.8 is explicitly selected and strict tidy warnings fail CI. Empty CTest selections
fail. Dependency revisions formerly following `master` are pinned to the exact prior local revisions. Windows
helpers discover the installed Visual Studio toolset and use the selected standalone CMake.

## Repairs and evidence

Measured local tuples: Windows/NVIDIA GeForce RTX 4070 Ti SUPER, driver `32.0.15.9579`; Linux is Ubuntu/WSL2
(kernel `6.6.87.2-microsoft-standard-WSL2`), GCC 13.3.0, Mesa 25.2.8 llvmpipe (LLVM 20.1.2, 256-bit CPU device).
The Linux Vulkan result qualifies that software-provider tuple, not a discrete Linux GPU. The corrected validation
layer is 1.4.341.1; the system loader remains 1.3.275.


Texture/mesh loader fixtures now use unique temporary files with RAII cleanup, including assertion-unwind paths.
Windows Debug built the affected resources target and passed all 116 registered tests; its three changed files
passed LLVM-20 tidy. Nine CEIR Vulkan 13z tests and 12 repository/architecture guards passed. Additional scoped
consumer targets built successfully. These are dated intermediate checks, not a claim that later edits passed.

The migration audit parsed 202 changed C++ files: 24 reported tidy issues; three optional peer benchmarks lacked
their Windows dependencies. Those three had only comment edits, now restored to baseline bytes. All actual changed
sources remain gated. The repairs rename public constants and their callers, split declarations, preserve repeated
lvalue callback semantics, and retain necessary preprocessor macros with specific explanations. An automatically
suggested local name collided with a loop index; review caught it and replaced it with `layer_count` before testing.

The CEIR generator now emits compliant keyword accessors, rejects collisions after sanitization and preserves
quoted authored text with safe raw-literal delimiters. All 62 generator validator tests and drift check passed.
Serialized attribute names remain unchanged. Public numeric constant initializers and numerical algorithms retain
their original meaning. [CODING](../CODING.md) now distinguishes constexpr/static constants from ordinary local consts.

## Current CI investigation

[Run 34681342652](https://github.com/yatiyr/crd/actions/runs/34681342652) tests the baseline commit, **not these
unpublished edits**. Its Linux SSE2 build was interrupted by runner shutdown (exit 143, no compiler diagnostic).
Linux Debug reported three DSP spectral segmentation faults. Windows Debug, SSE2 and clang-cl Shipping each reported the same 12 failures: authored-text
anti-drift checks, DX12 numeric/raster checks, an atomic A-buffer stack overflow, a tune-cache parse failure and an
impostor consumer failure. Their owning work is REPO.3 in the single roadmap. The repairs and local reproductions below distinguish resolved mechanisms from the remaining runner-specific
DX12 investigation; no test is waived or called green from source inspection. Full local sweeps remain prohibited; reproduce scoped targets.

Local logs and temporary instrumentation live under `build/repo-cleanup-20260912/`. Final post-repair evidence
and remote publication requirements are recorded below. No performance victory is claimed.

## Final local verification

- All 4,596 baseline paths are accounted for; all 46 preserved recovery files match their original SHA-256.
- LLVM-20: the repair/caller audit passed after fixing suggested-name collisions; both large CEIR GPU pipeline
  translation units then passed separately. The DSP spectral header also passed after its stack fix; all three final math declaration/implementation/test
  files passed after the explicit F64 spelling review.
- Windows Debug: geometry primitives 196, convex 207 and Delaunay 112 tests passed; selected CKIR hair/MSL
  coverage 18, CHIR source-model coverage 10, DX12 device coverage 6 and Vulkan/DX12 impostor coverage 2 passed.
- Resources: Windows Debug, ASan and Shipping plus Linux GCC Debug each rebuilt and passed all 116 tests with
  the temporary-file lifecycle changes. Windows Debug also passed both CEIR 27c/27d authored rewrite anti-drift tests.
- Final math spelling migration: Windows Debug and Shipping each rebuilt and passed all 172 registered tests,
  including the SIMD emission guard. All 21 math initializer spellings match the baseline exactly.
- DSP spectral repair: Linux GCC Debug and Windows Debug, ASan and Shipping each rebuilt and passed all four
  selected spectral cases, including scipy references and cross-thread bit identity. The Shipping link completed
  its active LTCG work; no optimization mode or test bound was weakened.
- Repository/architecture guards: all 12 passed on Windows and Linux. The final four repository-tool regression
  cases also passed on both platforms; documentation and workflow validation passed after the final edits.
- Windows clang-cl Shipping: all seven selected hair/fur, multiple-scattering, atomic A-buffer, inner-coverage and
  path-tracing tests passed locally. The atomic A-buffer case took 85 seconds; it was allowed to complete within
  its justified 180-second bound, not misreported as hung.
- Linux GCC Debug: all four spectral tests passed after the measured fiber-stack repair. GDB showed
  `FftPlan<double>::execute` probing a `0x69000 + 0x1c0`-byte frame on a 64 KiB fiber. The two FFT-bearing DSP
  parallel dispatches now request the existing 2 MiB tier; generated transforms and numerical reference values
  are unchanged. Existing scipy and cross-thread bit-identity gates remain in force.
- The committed tune cache contained the literal stray prefix `I D` before `module`, including in the baseline
  backup. Removing those three bytes restored the authored asset; its DX12 three-row anti-drift test passed.
- Canonical CEIR/CKIR/CHIR sources now have explicit LF checkout attributes and regression guards for invalid UTF-8,
  BOM, CRLF and bare CR. Nested benchmark target folders are covered by the generated CMake fixture.
  This addresses Windows byte-comparison drift without relaxing the parser or round-trip assertions.
- Workflow syntax/expression validation passed with official actionlint 1.7.12, verified against its release
  checksum. CI now performs that check, keeps downloaded Ninja/installers outside the source root, runs tests
  in the former build-only clang-cl lane, records Windows graphics driver identity and preserves CTest logs.

The remote Windows failures are not claimed fixed merely because local hardware passes. The next published
revision must be tested on those runners with the recorded diagnostics. Local scoped checks are complete;
REPO.3c/REPO.3d and the cleanup parent remain open for the unresolved runner investigation and published-revision
qualification. The user owns committing/pushing under AGENTS; no publication was performed. The baseline CI status
was read at 12:44 +03:00: several lanes were still running, so there is no final all-lanes result to report.

## Remaining runner-specific evidence

The seven unresolved DX12 cases in the baseline CI run remain owned by REPO.3c; local passes do not close them.
The current source locations are [compute hair/fur/MS and atomic A-buffer](../../tests/gpu/gpu-context-dx12/test_dx12_compute.cpp),
[inner coverage](../../tests/gpu/gpu-context-dx12/test_dx12_raster.cpp),
[NEE/MIS path tracing](../../tests/gpu/gpu-context-dx12/test_dx12_rt.cpp) and
[the impostor consumer](../../tests/rendering/scene-render/test_scene_render_gpu.cpp).
The next discriminating evidence is those exact cases on the failing runner/adapter with retained logs and driver
identity. The authored-text/tune-cache and Linux DSP repairs are separate verified mechanisms described above.
No capability skip, tolerance relaxation, software-adapter assumption or speculative runtime fix was added.

## Linux validation tooling repair

The first nine-case Vulkan run executed all assertions, but its full log exposed old system validation-layer errors
for newer extension structures during device creation. Those messages preceded `ValidationCapture`, so a green CTest
summary alone was insufficient. The local `VULKAN_SDK` directory held reflection sources, not a matching runtime layer.

[The installer](../../scripts/install-vulkan-validation.py) now selects the two exact validation files from the official
Linux x86_64 SDK 1.4.341.1 archive, verifies SHA-256
`3bf0f762afb6c79bc6a9d9fb5998745ccff928800a29619b501ed9de7fd9789b`, and leaves the driver/loader intact.
With its `VK_LAYER_PATH` and `LD_LIBRARY_PATH` selected, the rerun passed all nine cases, with assertion counts
6/72/9/74/10/24/23/23/2, zero VUID messages and zero device skips. The final Windows run passed the same assertions
without VUIDs/skips. Linux CI uses this installer before testing. A corrupt-download rejection joins the repository
tooling regressions (four tests); current actionlint validation passes. See the primary setup reference in
[the layout contract](../design/repository-layout.md).

## Reproduce the affected checks

Use the build/test helpers in [BUILDING](../BUILDING.md), with `CMAKE_BUILD_PARALLEL_LEVEL=2` on this host.
Each build command selects one target; each CTest command selects its module directory and a nonempty name regex.
The grouped **source** paths do not change the existing flat `build/<preset>/tests/<module>` directories.

```powershell
$env:CMAKE_BUILD_PARALLEL_LEVEL = 2
$env:CRD_PLATFORM_HEADLESS = '1'
# Substitute the configuration/module/regex from the evidence scope below.
& ./scripts/build-target.bat build/win-debug crd-hesap-dsp-tests
& ./scripts/run-ctest.ps1 --test-dir build/win-debug/tests/hesap-dsp -R '^dsp spectral:' --timeout 180 --no-tests=error --output-on-failure
python scripts/check-repository.py
python scripts/test-repository-tools.py
python scripts/check-master-plan.py
python tools/ceir_opgen/ceir_opgen.py --check
python tools/ceir_capability_matrix/gen_matrix.py --check
```

- Math: `crd-math-tests`, `tests/math`, all registered tests including the SIMD emission guard.
- Resources: `crd-resources-tests`, `tests/resources`, all registered tests.
- DSP repair: `crd-hesap-dsp-tests`, `tests/hesap-dsp`, `^dsp spectral:` (four cases).
- Vulkan authoring/dispatch: `crd-ceir-gpu-vulkan-tests`, `tests/ceir-gpu-vulkan`, `^ceir 13z` (nine cases).
  Inspect `Testing/Temporary/LastTest.log` for any no-device warning before counting device execution.
- CEIR authored rewrites: `crd-ceir-tests`, `tests/ceir`, `^ceir 27[cd]` (two cases).
- CHIR authored source: `crd-chir-tests`, `tests/chir`, `^chir 32[bc]` (ten cases).
- Remote reproduction: inspect the failed named cases and actual driver in the next published run. Baseline jobs
  `103520417125`, `103520417208` and `103520417241` hold clang-cl Shipping, Debug and SSE2 Windows diagnostics;
  Linux Debug job `103520417224` holds the reproduced DSP crash. Do not substitute another adapter's pass for them.

The Linux equivalent uses the configured native build directory `/home/yatiyr/cerid-build/linux-gcc-debug`, selected
`cmake --build <dir> --target <target>`, then the same scoped `ctest --test-dir <dir>/tests/<module> -R <regex>` gates.
Those paths are this session's measured environment, not a new required developer directory layout.
