# Module registry: dependency-closed selection and host tools

<!-- doc-role: historical -->
> Dated evidence. Live owner: [REPO.DEV.4](../ROADMAP.md#slice-repo.dev.4); contract:
> [module registry](../design/module-registry.md). Rules: [AGENTS](../../AGENTS.md).
> Preceding batch: [frontend qualification](2026-09-13-frontend-qualification.md).

## User direction

After pushing `18651d5` ("solidifying repository v3.") the user asked to carry the REPO.DEV slices on in a loop
"one by one until AUD-2". REPO.DEV.4 was the first Open row. AGENTS records "work directly; do not fork/delegate",
so the increment ran in this session with the advisor at the design gate; no Workflow or subagent was used.

## What changed

- [cmake/CrdModules.cmake](../../cmake/CrdModules.cmake): `crd_module()`/`crd_tests()` registrations,
  `crd_resolve_modules()` (selection, closure, tests, packages), `crd_add_modules()` (adds the selected closure in
  registration order; imports `HOST` executables from `CRD_HOST_TOOLS_DIR`), `crd_verify_modules()` (actual link graph
  against the declarations, every mode).
- Root `CMakeLists.txt`: the 101 literal `add_subdirectory` lines became 102 `crd_module()` registrations (96 engine
  modules, four host tools, `runtime`, `sandbox`) placed before the third-party acquisition, which is now gated on the
  selected packages; `tests/CMakeLists.txt`: 103 `crd_tests()` registrations. Declarations were generated from each
  directory's `target_link_libraries` blocks and corrected where the verifier disagreed (a `$<$<BOOL:${WIN32}>:...>`
  link in crd-imgui, seven `tomlplusplus` links inside `if(TARGET ...)` blocks, the sandbox's cook-time
  `asset_cooker` edge). The Linux full configure then showed that a conditional generator-expression link must be
  tolerated when its target is absent (`crd-gpu-context-dx12` on Linux); the verifier now checks such names only
  when the target exists. `tests/applications/sandbox` is owned by geometry-viz (what it links), so
  `-DCRD_BUILD_SANDBOX=OFF` still configures it (101 of 101, configure proven in a scratch directory).
- Synchronizer ([operations.py](../../scripts/project_sync/operations.py)): registration commands are
  `add_subdirectory`, `crd_module` and `crd_tests`; new modules register in the form the file uses, before
  `crd_resolve_modules()` or `crd_stage_warp_dll`, with `DEPENDS` derived from the requested dependencies; a new test
  directory is added to its owner's `TESTS`; unregistering removes the whole call. Hygiene guard accepts both forms.
- [scripts/test-module-selection.py](../../scripts/test-module-selection.py): twelve fixture cases, registered as the
  `crd-module-selection` CTest and a repository CI step. Three synchronizer cases and one hygiene case cover the
  registry forms.

## Verification

- **Full configuration unchanged.** `build/win-debug` regenerated: 292 File API targets with identical names, types,
  directories and dependencies; `compile_commands.json` 1,814 entries identical; `build.ninja` 5,786 build statements
  identical except the reconfigure-dependency statement, which now lists `cmake/CrdModules.cmake`. The first two
  reconfigures failed on the verifier's two findings above; the third passed.
- **Numerics without graphics or physics.** `-DCRD_MODULES=hesap-fft` on the `win-debug` preset in
  `build/win-debug-modules`: 9 of 102 modules (`core vm units memory math hesap containers log hesap-fft`), tests
  `numerics/hesap-fft`, no package (Catch2 only); the cache holds no `Vulkan_`, `CUDAToolkit`, `CRD_SHADERC` or GLFW
  entry (30 such entries in the full cache); the Ninja graph knows nine `crd-*` libraries and one executable.
  `crd-hesap-fft-tests` linked in 173 steps with zero diagnostics, two workers. WSL, `linux-gcc-debug` preset,
  `build/linux-gcc-debug-modules`: the same nine modules configured in 33 s over the 9p mount, linked in 172 steps
  with zero diagnostics and passed all 36 CTests.
- **Renderer selection pulls its real dependencies.** `-DCRD_MODULES=scene-render`: 40 of 102 modules including
  ceir, ceir-gpu, gpu-context with the Vulkan and D3D12 backends, kir, the five cook vocabularies, resources, scene,
  anim, lod and hesap-fft/interp; packages `glfw tomlplusplus zstd`; eylem, audio, imgui, ceir-host and the other
  hesap modules omitted.
- **Host tools.** `-DCRD_MODULES=sandbox -DCRD_HOST_TOOLS_DIR=build/win-debug/tools/asset_cooker`: 63 of 102 modules,
  `asset_cooker` imported from the host build's executable, generation succeeded with `$<TARGET_FILE:asset_cooker>`
  resolving to the import.
- **Cook step.** `cook-demo-assets` in `build/win-debug`: pack 100,272,336 bytes, SHA-256 `21452076…0a617`;
  touching `assets/source/BoomBox.glb` recooked in 3 s to identical bytes; a second output directory cooked from the
  same sources is byte-identical; the following helper run reports `ninja: no work to do.` A bare `ninja` from a
  shell without the MSVC environment failed the CMake re-run at GLFW's compiler identification (recorded lesson).
- **Guards.** `check-repository.py` PASS (96 modules); `test-module-selection.py` 12/12 on Windows and 11/11 under
  WSL before the last case was added; `test-project-sync.py` 55/55; `test-repository-tools.py` 22/22;
  `test-project-sync-native.py --generator "Visual Studio 18 2026"` PASS; `test-dev-workflow.py` 54/54.

| Host | Configuration | Result |
|---|---|---|
| Windows | `win-debug`, full | reconfigure passed; graph, compile database and Ninja statements identical to the pre-change snapshot |
| Linux | `linux-gcc-debug`, full | 102 of 102 configured over 9p in 4 min 24 s, verifier clean on the Linux link graph (first run caught the conditional D3D12 link, fixed above) |
| Windows | `win-debug` with `CRD_BUILD_SANDBOX=OFF` | 101 of 101 registrations configured, showcase tests kept, configure only |
| Windows | `hesap-fft` | 9 modules; `crd-hesap-fft-tests` linked in 173 steps, zero diagnostics; CTest **36/36** in 110 s (hesap-fft cases and the repository guards) |
| Linux | `hesap-fft` | 9 modules in 33 s; `crd-hesap-fft-tests` linked in 172 steps, zero diagnostics, two workers; CTest **36/36** in 151 s over 9p |
| Windows | `scene-render` | 40 modules, configure only |
| Windows | `sandbox` + host tools | 63 modules, cooker imported, generate passed |

## State

REPO.DEV.4 is Needs CI on this evidence: the hosted repository jobs of the next push run the new fixture guard, and the
full lanes prove the unchanged default on every preset. The pointer moves to REPO.DEV.5.
