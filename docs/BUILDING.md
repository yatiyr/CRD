# Build and verification

<!-- doc-role: rule -->
> Current rule. Current work: [ROADMAP](ROADMAP.md); conduct: [AGENTS](../AGENTS.md).

[Presets](../CMakePresets.json) owns configurations; [CI](../.github/workflows/ci.yml) owns scheduling.
C++20; CMake 3.25+ (VS 2026: 4.2+); Python 3.12+; MSVC/clang-cl/GCC.
Overrides: ignored `CMakeUserPresets.json`.

## Fast local workflow — one primary configuration

**Build affected targets/consumers, run selected CTests/guards and tidy changed C++ on one primary configuration.**
Add local lanes only for a discriminating risk/failure; CI owns broader qualification under the approved
[policy](sessions/2026-09-12-large-cpp-research-and-loop-plan.md). Retain CI gates until replacements qualify.

Check scope belongs to the [first unfinished slice](ROADMAP.md#strict-sequential-execution). Helpers initialize the
toolchain; `build-target.bat` takes one target. Rebuild affected executables.

```powershell
$env:CMAKE_BUILD_PARALLEL_LEVEL = '2'
& ./scripts/configure-preset.bat win-debug
if ($LASTEXITCODE -ne 0) { throw 'Configure failed' }
& ./scripts/build-target.bat build/win-debug <target>
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
& ./scripts/run-ctest.ps1 --test-dir build/win-debug -R '<specific-test-regex>' --timeout 180 --no-tests=error --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'CTest failed' }
python scripts/tidy-files.py <changed.cpp> <changed.hpp>
if ($LASTEXITCODE -ne 0) { throw 'Tidy failed' }
```

`dev.py doctor`: tools; `plan`: scope; `check`: verification; `--dry-run`: no execution.
[Contract](design/developer-workflow.md).
Require timeouts, nonzero CTest matches, guards.
`-DCRD_MODULES=<module|family>` configures one dependency-closed [selection](design/module-registry.md);
`-DCRD_INPUT_ARCHIVES=<dir>` uses verified local [archives](design/pinned-inputs.md);
`-DCRD_COMPILER_LAUNCHER=<sccache>` (Ninja; PCH off on MSVC), `CRD_COMPILE_JOBS`/`CRD_LINK_JOBS`:
[build board](design/build-performance.md); `-DCRD_PUBLIC_CHECKS=ON` checks every public header and the package consumer:
[public consumption](design/public-consumption.md); fuzz corpora replay everywhere, `fuzz.py` fuzzes:
[instruments](design/test-instruments.md); planned coverage: [DIAG](design/runtime-diagnostics.md).

| Change | Local check beyond the primary build/tests | CI obligation |
|---|---|---|
| Docs only | Documentation validator; hygiene guard for layout changes | Documentation/tooling guards |
| Python/CMake/IDE | Changed-tool fixtures and a representative generated target | Windows/Linux tooling; native fixtures when affected |
| Private C++ | Incremental LLVM-20 tidy; affected linked consumers | Affected primary Windows/Linux targets |
| Public/template headers, generated API, ABI | `check-headers.py --changed`; reverse consumers | Compiler diversity; public-check presets; ISA/profile lanes |
| Memory/lifetime/concurrency | Scoped adversarial tests and detector controls | Sanitizer/stress; TSan qualification: DIAG.1b |
| GPU/CEIR/CKIR/layout | Available affected provider, validation and declared CPU/image oracle | Affected backend/OS/device lanes; hardware truth retained |
| Intrinsics/OS API/build flags/LTCG | Actual failing/risky configuration when locally available | Explicit matching compiler/ISA/optimized/platform checks |

Analyze cross-platform risks: sizes/alignment, endian/packing, path case, OS APIs, ISA, lifetime.
Scoped WSL selects one target/CTest regex on native Linux storage; the unfiltered
[wsl-build](../scripts/wsl-build.ps1) is not scoped, and WSL does not qualify native presentation hardware.

## Qualification

Keep warnings zero and hand formatting consistent; **never run `clang-format -i`**. Run
[LLVM-20 tidy](../scripts/tidy-files.py) on changed headers/TUs on any host; confirm parsing. Unparsed files and
hosts without clang-tidy 20 are ungated, never clean.
GPU checks require `ValidationCapture` or its DX12 counterpart, validation silence, bit/ULP or declared
quality oracles, and three repetitions to claim determinism. Asset changes need source/cook/execute,
replacement/deletion and failure/reload lifetime proof. Save boards when measured.

Record revision/tree identity, command, toolchain, selected/executed counts, exit, adapter/driver.
Distinguish executed, failed, skipped and unsupported; a compile or missing-device return is not runtime proof.

CI failure: inspect the exact job/revision, reproduce narrowly, fix the mechanism and verify the next published
revision. Never weaken checks, ignore older failures or substitute a retry-pass for diagnosis.
**Agents never commit or push, directly or through automation/helpers.** Continue independent work while publication
waits; local passes cannot qualify unpublished changes. Missing evidence keeps its gate open.

Never run [per-slice-check](../scripts/per-slice-check.ps1)/[full-sweep](../scripts/full-sweep.ps1) or unfiltered local
sweeps. CI [tiers](design/ci-tiers.md): preflight; change lanes on pushes; nightly/manual complete
matrix. Preserve every preset and cluster/release gate; a fast pass cannot close a full qualification obligation.

## Visual Studio

`python scripts/project-sync.py open --preset win-vs` opens all eight native profiles and starts sync. Save All, wait
for watching/reload; inspect `project-sync.py status`. See
[configurations](design/visual-studio-configurations.md) and [structure/recovery](design/project-structure-sync.md).
Native build `--config` and CTest `-C` must match; pass PowerShell arguments explicitly:

```powershell
./scripts/run-ctest.ps1 -CtestArguments @('--test-dir', 'build/win-vs-debug', '-C', 'Debug',
    '-R', '<affected-test>', '--timeout', '180', '--no-tests=error', '--output-on-failure')
```

Linux: export the [validation installer](../scripts/install-vulkan-validation.py)'s layer/library paths and inspect
startup diagnostics; `VULKAN_SDK` alone selects no runtime layer.
Third-party defects, ASan gate: [register](third-party-defects.md).

## Troubleshooting

- Stale PCH: inspect `CMAKE_COMMAND` and [check-deps](../scripts/check-deps.bat); use the standalone helpers and
  verify absolute paths before scoped regeneration/deletion.
- ASan `0xc0000135`: check runtime DLL paths via [msvc-env](../scripts/msvc-env.bat). Raw clang-tidy cannot consume
  MSVC PCH; the tidy helper uses real compile flags and strips the PCH inputs.
- LTCG: diagnose the optimized artifact; ASan/non-LTCG cannot qualify it.
- PowerShell: capture the native exit before filtering; never pipe to `Select-Object -First`.
- Unload/free order, single jobs-init owner, String capacity, append-only vtables: [MEMORY](../MEMORY.md).

Docs: `python scripts/check-master-plan.py`. Tool/layout changes: also `check-repository.py`,
`test-repository-tools.py` and affected fixtures.
New modules need CMake/tests, a consumer and [systems](systems/README.md) route.
[Historical evidence](archive/2026-09-12-orientation-history.md#docs-building).
