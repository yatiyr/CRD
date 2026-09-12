# ADR-0132 — CMake presets and native Visual Studio configurations

<!-- doc-role: contract -->
> Contract. Live work: [ROADMAP](../ROADMAP.md#slice-repo.sync.5); rules: [AGENTS](../../AGENTS.md).

**Accepted direction, 2026-09-12:** the user selected the CMake preset selector for the complete compiler/platform
matrix, plus complete native MSVC solution configurations. This amends the Debug-only projection of
[ADR-0131](0131-project-structure-synchronization.md); CMake remains the sole build authority.

The native `win-vs` preset generates Debug, Release, RelWithDebInfo, ASan, Shipping, ShippingProfile, DebugScalar
and DebugSSE2 in one MSVC x64 build tree. The mapping in [CMakePresets.json](../../CMakePresets.json) names the
canonical Ninja preset for each profile. Resolve inherited semantics from those presets; never maintain an
independent set of native assertion/profiling/optimization switches. Generated headers and compiler/link settings
must follow the selected configuration. Preserve explicit target IPO exclusions and deterministic FP/security flags.

Visual Studio File > Open > Folder exposes the complete preset workflow. Target System selects Windows or Linux
(WSL/SSH); compiler, sanitizer and analysis environments retain separate build trees. A native MSVC configuration
cannot pretend to be clang-cl, clang-tidy or Linux. Native Shipping keeps the `tests/bench` suite discoverable but excludes
it from the default build; the canonical Ninja Shipping preset omits that suite. Explicit benchmark builds remain possible.

The existing `build/win-vs-debug` location remains stable for open solutions and synchronization journals.
`win-vs-debug` is a hidden inheritance compatibility alias. The synchronizer redirects its old `--preset win-vs-debug`
command to `win-vs`; direct CMake configuration uses `--preset win-vs` because CMake cannot invoke hidden presets.
Native build/test presets select individual configurations and retain `win-vs-debug` for Debug.
Structural edits remain global portable membership operations. Conditional build semantics belong in CMake.
Saved IDE edits, configuration changes and physical edits use the same guarded generation lifecycle.

Implementation, limits, commands and evidence: [configuration contract](../design/visual-studio-configurations.md).
Advisor discovery returned no callable capability; no external plan review is claimed.

Primary references: [CMake configuration types](https://cmake.org/cmake/help/latest/variable/CMAKE_CONFIGURATION_TYPES.html),
[Microsoft preset workflow](https://learn.microsoft.com/en-us/cpp/build/cmake-presets-vs?view=msvc-170),
[vendor maps](https://learn.microsoft.com/en-us/cpp/build/cmake-presets-json-reference?view=msvc-170) and
[CMake clang-tidy generator support](https://cmake.org/cmake/help/latest/prop_tgt/LANG_CLANG_TIDY.html).
