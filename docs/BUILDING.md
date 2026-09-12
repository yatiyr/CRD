# Build and verification guide

<!-- doc-role: rule -->
> Current rule. Current work: [ROADMAP](ROADMAP.md); current rules: [AGENTS](../AGENTS.md).

Use this guide before code work. [CMake presets](../CMakePresets.json), [CI](../.github/workflows/ci.yml) and the
linked helpers specify actual configuration/tool pins; do not infer them from an old session count.
C++20, CMake 3.25+, Ninja/Visual Studio, Windows MSVC/clang-cl and Linux GCC form the build substrate. Vulkan SDK and
CUDA requirements depend on the selected targets. Inspect the configured SDK/compiler paths before using them.
Tests/tooling require Python 3.12+. [Source/IDE layout](design/repository-layout.md): physical family folders and
CMake target folders agree; public include names, target names and existing binary directories remain stable.
[VS solution](design/repository-layout.md#native-visual-studio-solution) requires CMake 4.2+. Reconfigure after source moves. Put personal SDK/ISA choices in ignored `CMakeUserPresets.json` or user settings.

## Local work — affected targets only

On the Windows development host, use the standalone-CMake helpers. `build-target.bat` builds **one target per call**.
Rebuild every executable affected by a changed library; a sibling executable does not relink itself.

```powershell
& .\scripts\configure-preset.bat win-debug
& .\scripts\build-target.bat build/win-debug <target>
$buildExit = $LASTEXITCODE
if ($buildExit -ne 0) { throw "Build failed: $buildExit" }
# The runner discovers MSVC runtime/dumpbin tools and preserves regex arguments.
& .\scripts\run-ctest.ps1 --test-dir build/win-debug -R '<specific-test-regex>' --timeout 180 --no-tests=error --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'CTest failed' }
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/tidy-files.ps1 <changed.cpp> <changed.hpp>
```

Set the timeout to a justified workload bound; 180 seconds above is an example, not a universal budget.
Inspect CTest discovery/summary and require nonzero matches. Include the relevant registered guard tests in the
regex or run them as a separate scoped CTest invocation. Direct test binaries are diagnostic aids, never the
slice-completion gate. `ctest --timeout N` is an equivalent explicit bound in an initialized environment.

Windows/Linux GPU legs use the same changed-module/blast-radius scope. WSL builds live on native Linux storage
where configured; inspect [wsl-build.ps1](../scripts/wsl-build.ps1) for paths, but its unfiltered build/test flow is
**not** a scoped local command. Invoke the selected Linux target and CTest regex explicitly. Check each build exit
before testing; `ninja | tail && ctest` can test a stale binary after a failed build.

## Required evidence

- Zero compiler warnings; correct hand formatting; **LLVM-20 incremental tidy** for changed headers/TUs via
  [tidy-files.ps1](../scripts/tidy-files.ps1). Confirm files were parsed. No automatic format rewrite.
- Scoped debug and sanitizer checks plus appropriate shipping/release/LTCG evidence for affected paths. GPU work
  also needs Windows/Linux provider execution and applicable validation, reference and failure-path tests.
- `crd::gpu::ValidationCapture` (DX12 debug-layer counterpart) must show zero validation errors/warnings. Use CPU
  bit/ULP or other declared quality oracles, three determinism repetitions when claimed, and saved performance budgets.
- Source-to-cooked-to-executed asset proof; deletion proof for replaced builders; rollback and lifetime tests for reload.
- Linux: [install matching validation](../scripts/install-vulkan-validation.py) and export its two printed paths;
  `VULKAN_SDK` alone does not select runtime layers. Inspect startup diagnostics too.
- Document exact commands/configuration/selected test counts/results. An unavailable lane is unqualified, not green.
  No closure while a required lane or known verified failure remains unresolved.

## Whole-repository qualification — CI only

The CI recipes are [per-slice-check.ps1](../scripts/per-slice-check.ps1) and [full-sweep.ps1](../scripts/full-sweep.ps1).
They retain debug/ASan/shipping/tidy, release/LTCG and cluster-close configuration obligations. **Do not run their
whole-repo sweeps on this host.** CI owns broad coverage; local iteration stays affected targets plus consumers.
Do not run unfiltered `cmake --build`/`ctest` locally as a convenient substitute.

The development host has recorded instability under all-core load: cap build concurrency and run heavyweight jobs
sequentially. Verify the current hardware/toolchain configuration instead of presenting the old diagnosis as a
fresh hardware test. Never use `-Parallel` for these host-wide sweeps.

## Troubleshooting — diagnose before retrying

- **No header dependencies / stale PCH:** VS-bundled CMake on this locale previously produced `#deps 0`.
  Use standalone CMake through [configure](../scripts/configure-preset.bat)/[build](../scripts/build-target.bat).
  Check `CMAKE_COMMAND` and [check-deps.bat](../scripts/check-deps.bat). Reconfigure only the proven affected build
  directory after verifying its absolute path; do not wipe arbitrary computed paths.
- **ASan 0xc0000135:** inspect DLL search paths; `msvc-env.bat` discovers the installed toolset rather than pinning
  a developer-specific version. A missing runtime DLL is a harness problem, not an engine crash.
- **Tidy reports zero without parsing:** use the LLVM-20 helper. Missing includes or incompatible MSVC PCH input
  invalidate analysis. An empty diagnostic stream alone is not evidence either way.
- **LTCG-only failure:** reproduce with the actual failing compiler/configuration, inspect emitted/runtime values,
  and repair the mechanism. ASan without LTCG cannot validate it. Preserve required noinline/ABI boundaries.
- **PowerShell phantom exit:** never pipe a running native tool to `Select-Object -First`; it can terminate the
  producer. Capture completion/exit first, then filter output. Use UTF-8 file I/O, not legacy ANSI/BOM round trips.
- **Allocator/resource hazards:** preserve unload-then-free resource destruction, one jobs-init owner per test
  binary, String usable-capacity semantics and append-only virtual slots. Full scars are in MEMORY references.

Smoke executables supplement CTest. Discover the real current runtime targets; sandbox GPU smoke requires bounded
duration and validation evidence. Old retired rhi/renderer/shader smoke names are historical. Adding a module also
requires its CMake target, tests, appropriate consumer/smoke and a systems-index entry.

For documentation-only work run `python scripts/check-master-plan.py`; no engine build is implied.
Repository cleanup additionally runs `python scripts/check-repository.py` and `python scripts/test-repository-tools.py`.
These are CTest guards and Windows/Linux CI jobs. CI selects pinned LLVM 20.1.8 explicitly and treats warnings as errors.
[Historical host notes](archive/2026-09-12-orientation-history.md#docs-building).
