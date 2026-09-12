# Visual Studio configuration and synchronization contract

<!-- doc-role: contract -->
> Contract. Live work: [ROADMAP](../ROADMAP.md#slice-repo.sync.5); rules: [AGENTS](../../AGENTS.md).

[ADR-0132](../decisions/0132-visual-studio-configuration-matrix.md) records the user's chosen workflow.
[CMakePresets.json](../../CMakePresets.json) owns configuration definitions; generated `.slnx`/`.vcxproj` files
are projections. Native Visual Studio 2026 requires CMake 4.2+; other presets retain CMake 3.25+ requirements.

## Choose the appropriate Visual Studio view

**Full compiler/platform matrix:** File > Open > Folder → repository root. Use Target System (local Windows,
Linux WSL or configured SSH host), Configure Preset, then Build Preset. The 20 visible configure presets retain
MSVC, clang-cl, strict clang-tidy, diagnostic tidy preview, sanitizer, Shipping, scalar/SSE2 and Linux GCC variants.
Host filtering shows the presets applicable to the selected target. Linux needs the C++ Linux/WSL tooling and
dependencies described in [BUILDING](../BUILDING.md); a selector entry alone does not qualify its platform.
Remote source-copy exclusions omit build products, local dependency/cache trees and IDE state.

**Native solution:** `python scripts/project-sync.py open --preset win-vs` generates and opens
`build/win-vs-debug/CRD.slnx`, starts its watcher and exposes the following MSVC configurations:

| Native configuration | Canonical preset | Assertions / profiling | Base / ISA |
|---|---|---|---|
| Debug | win-debug | on / on | Debug / auto |
| Release | win-release | off / off | Release + IPO / auto |
| RelWithDebInfo | win-relwithdebinfo | on / on | RelWithDebInfo + IPO / auto |
| ASan | win-asan | on / on | Debug + AddressSanitizer / auto |
| Shipping | win-shipping | off / off | Release + IPO + symbols + dead stripping / auto |
| ShippingProfile | win-shipping-profile | off / on | Shipping / auto |
| DebugScalar | win-debug-scalar | on / on | Debug / scalar |
| DebugSSE2 | win-debug-sse2 | on / on | Debug / SSE2 |

This table explains the projection, not a second configuration definition. Auto resolves to AVX2 for this x64
profile; explicit local ISA overrides remain possible. Existing target-specific IPO exemptions still apply.
RelWithDebInfo preserves the project's existing CRD_DEBUG/assertion contract despite optimization and NDEBUG.
Automatic compile-time logging is Trace in Debug-family/RelWithDebInfo and Info in Release/Shipping; explicit
`CRD_LOG_MIN_LEVEL` overrides both. ASan uses the debug CRT without incompatible RTC, edit-and-continue or incremental
linking. The selected toolset's AddressSanitizer component/runtime must be installed and discoverable when running tests.

Native build presets `win-vs-debug`, `win-vs-release`, `win-vs-relwithdebinfo`, `win-vs-asan`, `win-vs-shipping`,
`win-vs-shipping-profile`, `win-vs-debug-scalar`, `win-vs-debug-sse2` select the sandbox and dependencies with two
workers. Their matching CTest presets select the same configuration. For local work select affected targets/tests:

```powershell
cmake --build build/win-vs-debug --config Shipping --target crd-core --parallel 2
./scripts/run-ctest.ps1 -CtestArguments @('--test-dir', 'build/win-vs-debug', '-C', 'Shipping',
    '-R', '<affected-test>', '--timeout', '180', '--no-tests=error', '--output-on-failure')
```

Direct configuration uses `cmake --preset win-vs`. The synchronizer also accepts its old `--preset win-vs-debug`
spelling and redirects it. The hidden configure alias exists for preset inheritance; CMake cannot invoke it directly.

Native solutions contain a union of projects; the `tests/bench` suite stays visible but is excluded from the
default Shipping/ShippingProfile build. Ninja Shipping omits that suite at configure time. Runtime benchmark examples
retain their existing canonical preset behaviour. Analysis and Linux builds use their
canonical presets; the VS generator does not execute CMake's clang-tidy integration.

## Automatic synchronization has an explicit lifecycle

Opening through the command above starts automation. Ordinary text edits affect real source directly. Physical
source-structure changes and **saved** native project/filter/solution edits synchronize after two stable observations
at a two-second polling interval, followed by CMake generation. Save All, wait for `watching`, and accept Reload All
if Visual Studio requests it. Double-clicking the solution alone does not start the watcher; run `open` after reboot.
`python scripts/project-sync.py status` reports liveness, pending generation and conflicts.

[The structure guide](project-structure-sync.md) owns operation choices and recovery. Remove keeps disk bytes;
Delete removes them. Moving modules migrates physical directories and references. The watcher never manufactures
compiler settings from MSBuild edits. A file exclusion scoped to only one configuration is rejected with a remedy;
use All Configurations for a portable exclusion or author conditional membership in CMake. Changes to presets are
generation inputs. Concurrent CMake and saved IDE changes still require reconciliation, never last-writer overwrite.

## Implementation and regression gates

[native_build_profiles.py](../../scripts/native_build_profiles.py) resolves first-parent-wins preset inheritance,
validates the native subset and exports data to ignored build output. [CrdBuildProfiles](../../cmake/CrdBuildProfiles.cmake)
handles flags, per-configuration headers/CRT/symbols and IPO exclusions. [CrdSimd](../../cmake/CrdSimd.cmake) gates ISA
flags and macros. Core exposes configuration-specific generated includes; source algorithms are unchanged.
Global native profile behaviour is edited in the canonical mapped presets, not contradictory single cache switches.

[Profile tests](../../scripts/test-native-build-profiles.py) verify the mapping and compile/run all eight native
profiles using the production header, SIMD module and top-level sanitizer/Shipping flags. The probe checks assertion,
profiling, debug/release, logging, ISA and NDEBUG values, CRT/ISA project properties and retained IPO exclusions.
[Structure tests](../../scripts/test-project-sync.py) cover partial exclusion rejection; the
[compiler fixture](../../scripts/test-project-sync-native.py) performs source/IDE edits and compiles Debug and Release.
CTest registers these gates; [CI](../../.github/workflows/ci.yml) runs portable Windows/Linux checks and the native
VS 2026 fixture on the explicit `windows-2025-vs2026` image family. Actual host/commands/results live in the
[session](../sessions/2026-09-12-visual-studio-configurations.md); hosted generator-mismatch repair evidence is in
[the repository CI session](../sessions/2026-09-12-repository-ci-environment.md).
Whole-engine matrix qualification remains CI's responsibility; local fixtures are scoped build-tool evidence.
