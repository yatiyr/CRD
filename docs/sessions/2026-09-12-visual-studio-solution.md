# 2026-09-12 — native Visual Studio solution

<!-- doc-role: evidence -->
> Session evidence. Only live status: [ROADMAP / REPO.VS](../ROADMAP.md#slice-repo.vs).

The user requested a Visual Studio project build to inspect the grouped repository in the IDE. Scope: generate a
native VS 2026 x64 Debug solution, build the sandbox/dependencies and verify its generated folders and runtime assets.
The earlier `win-vs-ref` directory was a Ninja build, not a native Visual Studio solution.

Added `win-vs-debug` configure/build/test presets; the build preset selects `crd-sandbox` with two workers. Compiler
selection is owned by the VS generator. The sandbox is the startup project, and cooked assets follow its Debug
executable directory. [Layout contract](../design/repository-layout.md#native-visual-studio-solution) owns commands.

The first compiler probe exposed duplicate `PATH`/`Path` entries inherited from the agent host. Passing a normalized
child environment fixed MSBuild's dictionary exception and identified MSVC 19.51.36246.0. No global environment setting
was changed. The restricted first dependency download produced an empty CPM file; an authorized network configure
retried it. These were harness/configuration findings, not C++ compiler failures.

Temporary scripts, logs and a backup of this turn's starting files are in `build/visual-studio-20260912/`.
No commit or push was made. The advisor capability was unavailable; no advisor approval is claimed.

## Generated solution evidence

CMake 4.3.2 generated `build/win-vs-debug/CRD.slnx` with 270 real project files in 34 solution folders. All referenced
project paths exist. The actual XML assigns hesap to `/engine/numerics/`, geometry to `/engine/geometry/`, CEIR to
`/engine/execution/`, GPU providers to `/engine/gpu/`, renderer modules to `/engine/rendering/`, and matching test
families. `crd-sandbox` has `DefaultStartup="true"`; its debugger directory and cook output both use `sandbox/Debug`.
The solution preset offers Debug/x64 only, so its compile-time debugging options cannot be mistaken for Release.

## Build and runtime evidence

The native MSBuild Debug sandbox/dependency build exited 0 without compiler warnings or errors. The resulting
`sandbox/Debug/crd-sandbox.exe` and `sandbox/Debug/assets/cooked/demo_assets.crdr` both exist. The multi-configuration
cook path was necessary: the old flat output path would leave a native Visual Studio executable without its demo pack.
Single-configuration Ninja output retains its existing path.

Both Vulkan and DirectX 12 bounded `--smoke-test 2` runs exited 0 and reported presented frames with thousands of
drawn scene instances. Both installed the repository's authored asset root; neither log contained warning/error/VUID
diagnostics. These were launch/cook/presentation checks, not performance benchmarks or whole-renderer qualification.
Visual Studio's generated debugger environment now sets the same `CRD_ASSETS_DIR` automatically for F5.

Local evidence: `build-sandbox.log`, `solution-check.json`, `smoke-results.json` and both backend smoke logs beneath
`build/visual-studio-20260912/`. The full solution is available for inspection; only the sandbox and its dependencies
were compiled. The earlier cleanup's unresolved DX12 CI cases remain owned by REPO.3c; this build does not close them.

Final configure and incremental sandbox build also exited 0 after installing the debugger environment. CMake's
`--open build/win-vs-debug` opened the generated solution in Visual Studio for the user's inspection.
Scoped CTest under `win-vs-debug` passed all three selected guards: `crd-repository-hygiene`, `crd-master-plan` and
`crd-repository-tools` (nonempty selection, 60-second timeout; log `guards.log`). REPO.VS is closed; context returns to
REPO.3c. Existing principles and memory rules remain applicable without edits.

## Follow-up — adding items in the IDE

The user asked whether new Visual Studio folders/files map back to actual source. Inspected the generated
`crd-hesap-dense.vcxproj` and its filters: existing entries use absolute repository source paths, while filters are
logical labels. Inspected `crd_collect_sources` and explicit test/sandbox source lists; discovery is target-dependent.
Documented the distinction between generated-solution editing and native CMake Folder View in the layout contract,
with Microsoft primary references. No UI add/rename operation or automatic custom-helper edit was tested or claimed.
No build behavior changed and REPO.VS remains closed; the current CI repair pointer is unchanged.
