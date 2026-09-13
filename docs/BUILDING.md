# Build and verification

<!-- doc-role: rule -->
> Current rule. Current work: [ROADMAP](ROADMAP.md); conduct: [AGENTS](../AGENTS.md).

[Presets](../CMakePresets.json) owns configurations; [CI](../.github/workflows/ci.yml) owns scheduling.
C++20; CMake 3.25+ (VS 2026: 4.2+); Python 3.12+; MSVC/clang-cl/GCC.
Local overrides: ignored `CMakeUserPresets.json`.

## Fast local workflow — one primary configuration

**Build affected targets/consumers, run selected CTests/guards and tidy changed C++ on one primary configuration.**
Add local lanes only for a discriminating risk/failure. CI owns broader qualification. This user-approved
[policy](sessions/2026-09-12-large-cpp-research-and-loop-plan.md) supersedes local multi-platform rituals; REPO.DEV.5
owns tier scheduling. Preserve existing CI obligations until its replacement qualifies.

Check scope belongs to the [first unfinished slice](ROADMAP.md#strict-sequential-execution). Helpers initialize the
toolchain; `build-target.bat` takes one target. Rebuild affected executables; cap concurrency and serialize heavy jobs.

```powershell
$env:CMAKE_BUILD_PARALLEL_LEVEL = '2'
& ./scripts/configure-preset.bat win-debug
if ($LASTEXITCODE -ne 0) { throw 'Configure failed' }
& ./scripts/build-target.bat build/win-debug <target>
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
& ./scripts/run-ctest.ps1 --test-dir build/win-debug -R '<specific-test-regex>' --timeout 180 --no-tests=error --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'CTest failed' }
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/tidy-files.ps1 <changed.cpp> <changed.hpp>
if ($LASTEXITCODE -ne 0) { throw 'Tidy failed' }
```

`dev.py doctor` diagnoses tools; `plan` explains scope. `check` qualification continues; `--dry-run` executes nothing.
[Contract](design/developer-workflow.md).
Configure for changed inputs or stale builds. Require timeouts, nonzero CTest matches and guards.

| Change | Local check beyond the primary build/tests | CI obligation |
|---|---|---|
| Docs only | Documentation validator; hygiene guard for layout changes; no engine build | Documentation/tooling guards |
| Python/CMake/IDE | Changed-tool fixtures and representative generated target | Windows/Linux tooling; native profile fixtures when affected |
| Private C++ | Incremental LLVM-20 tidy; affected linked consumers | Affected primary Windows/Linux targets |
| Public/template headers, generated API, ABI | Reverse consumers and standalone header check where affected | Compiler diversity; relevant optimized/ISA/profile lanes |
| Memory/lifetime/concurrency | Focused adversarial tests; applicable sanitizer instrument | Sanitizer/stress lanes; verify custom fiber instrumentation |
| GPU/CEIR/CKIR/layout | Available affected provider, validation and declared CPU/image oracle | Affected backend/OS/device lanes; hardware truth retained |
| Intrinsics/OS API/build flags/LTCG | Actual failing/risky configuration when locally available | Explicit matching compiler/ISA/optimized/platform checks |

Analyze cross-platform risks now: sizes/alignment, endian/packing, path case, extensions/OS APIs, ISA and lifetime.
Use CI. Scoped WSL selects one target/CTest regex on native Linux storage; the
unfiltered [wsl-build](../scripts/wsl-build.ps1) is not scoped. WSL does not qualify native presentation hardware.

## Qualification

Keep warnings zero and hand formatting consistent; **never run `clang-format -i`**. Run
[LLVM-20 tidy](../scripts/tidy-files.ps1) on changed headers/TUs and confirm parsing. Unparsed files are ungated.
GPU checks require `ValidationCapture` or the DX12 debug-layer counterpart, validation silence, bit/ULP or declared
quality oracles, and three repetitions when claiming determinism. Asset changes need source/cook/execute,
replacement/deletion and failure/reload lifetime proof. Save measured performance boards at measurement time.

Record revision/tree identity, command, toolchain, selected/executed counts, exit and relevant adapter/driver.
Distinguish executed, failed, skipped and unsupported; a compile/emitter/missing-device return is not runtime proof.

CI failure: inspect the exact job/revision, reproduce narrowly, fix the mechanism and verify the next published
revision. Never weaken checks, ignore older failures or substitute a retry-pass for diagnosis.
**Agents never commit or push, directly or through automation/helpers.** Continue independent work while publication
waits; local passes cannot qualify unpublished changes. Missing evidence keeps its gate open.

Never run [per-slice-check](../scripts/per-slice-check.ps1)/[full-sweep](../scripts/full-sweep.ps1) or unfiltered local
build/test sweeps. CI tiers: preflight, affected Windows/Linux on changes, nightly/manual full matrix. Preserve all
supported presets and cluster/release gates; a fast pass cannot close a full qualification obligation.

## Visual Studio

`python scripts/project-sync.py open --preset win-vs` opens all eight native profiles and starts sync. Save All, wait
for watching/reload; inspect `project-sync.py status`. Open Folder exposes all CMake presets. See
[configurations](design/visual-studio-configurations.md) and [structure/recovery](design/project-structure-sync.md).
Native build `--config` and CTest `-C` must match; pass PowerShell arguments explicitly:

```powershell
./scripts/run-ctest.ps1 -CtestArguments @('--test-dir', 'build/win-vs-debug', '-C', 'Debug',
    '-R', '<affected-test>', '--timeout', '180', '--no-tests=error', '--output-on-failure')
```

Linux: export the [validation installer](../scripts/install-vulkan-validation.py)'s layer/library paths; inspect startup
diagnostics. `VULKAN_SDK` alone does not select runtime layers. Keep the system driver.
WARP pin (opt-in, CI unset): [recipe](recipes/2026-09-13-dx12-pinned-warp.md).

## Troubleshooting

- Stale PCH: inspect `CMAKE_COMMAND` and [check-deps](../scripts/check-deps.bat). VS-bundled CMake once produced
  `#deps 0` on this locale; use standalone helpers. Verify absolute paths before scoped regeneration/deletion.
- ASan `0xc0000135`: check runtime DLL paths via [msvc-env](../scripts/msvc-env.bat). Raw clang-tidy cannot consume
  MSVC PCH; the tidy helper uses real compile flags and strips incompatible PCH inputs.
- LTCG: diagnose the optimized artifact; ASan/non-LTCG cannot qualify it. Preserve ABI/noinline boundaries.
- PowerShell: capture native completion/exit before filtering; never pipe to `Select-Object -First`. Use UTF-8.
  Slow is not hung: check progress/duration before termination.
- Unload/free order, single jobs-init owner, String capacity, append-only vtables: [MEMORY](../MEMORY.md).

Docs: `python scripts/check-master-plan.py`. Tool/layout changes: also `python scripts/check-repository.py`,
`python scripts/test-repository-tools.py` and affected fixtures.
New modules need CMake/tests, a consumer and [systems](systems/README.md) route.
[Historical evidence](archive/2026-09-12-orientation-history.md#docs-building).
