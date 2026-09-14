# Module registry and dependency-closed selection

<!-- doc-role: reference -->
> Contract for REPO.DEV.4. Status lives only in [ROADMAP](../ROADMAP.md#slice-repo.dev.4); layout rules in
> [repository layout](repository-layout.md); the accepted research is
> [dependency closure and host tools](../research/2026-09-12-large-cpp-development-and-ci.md#dependency-closure-and-host-tools).

Purpose: configure a consumer of one module family without unrelated SDKs, links or third-party packages, keep the
default full configuration and every preset byte-for-byte unchanged, and make a missing or stale dependency edge a
configure error on every host. The mechanism is [cmake/CrdModules.cmake](../../cmake/CrdModules.cmake); the
registrations are literal lines in the root `CMakeLists.txt` and `tests/CMakeLists.txt`.

## Registration

Every directory the build adds is a node: 96 engine modules, four host tools, `runtime`, `sandbox` and 103 test
directories. Each node is one literal call, one per line, which the structure synchronizer and the hygiene guard parse:

```cmake
crd_module(engine/numerics/hesap-fft engine/hesap-fft DEPENDS containers core hesap math memory TESTS numerics/hesap-fft)
crd_module(tools/shader-cook NAME shader_cook HOST EXECUTABLES shader_cook DEPENDS ... shader-cook)
crd_tests(numerics/hesap-fft hesap-fft)          # tests/CMakeLists.txt
```

- `DEPENDS` lists the module's direct `crd-*` link edges by module name; `PACKAGES` its direct third-party links
  (`glfw`, `zstd`, `tomlplusplus`, `stb`, `cgltf`, `mikktspace`, `imgui`).
- `TESTS` names the test directories the module owns; `TEST_DEPENDS` and `TEST_PACKAGES` cover what those tests
  link beyond the module's own closure. A test directory may have several owners (`support/test_helpers` belongs to
  the four geometry modules whose tests link it); an unowned registration is a configure error in every mode.
- `HOST` marks cookers and generators that run on the build host; `EXECUTABLES` names what a target build imports
  from `CRD_HOST_TOOLS_DIR` instead of building. `NAME` resolves the one name collision (`tools/shader-cook` versus
  `engine/assets/shader-cook`).
- Registration order is the historical `add_subdirectory` order; `crd_resolve_modules()` closes the block and
  `crd_add_modules()` adds the selected nodes in that order after the third-party acquisition.

The synchronizer registers a new module as `crd_module(<dir> <binary> [DEPENDS ...])` before `crd_resolve_modules()`
when the root uses the registry, a new test directory as `crd_tests(...)` before `crd_stage_warp_dll`, and adds the
test directory to its owning module's `TESTS`; bare fixture roots keep the `add_subdirectory` form.

## Selection

`CRD_MODULES` (cache string, default empty) names modules, families (`foundation`, `numerics`, `geometry`,
`execution`, `gpu`, `rendering`, `assets`, `world`, `media`, `physics`, `ui`), `tools`/`host-tools`, `runtime`,
`sandbox` or `all`; separators are `;`, `,` or spaces. An unknown name fails the configure and prints the known
modules and families.

The selection configures the transitive `DEPENDS` closure of the named modules, the `TESTS` of the named modules
(not of modules pulled transitively) with their `TEST_DEPENDS` closure when `CRD_BUILD_TESTS` is on, and only the
packages those nodes declare. Everything else is absent: no directory is configured, no target exists, no stub
stands in. Modules that self-skip (`return()` without an SDK) stay tolerated because their consumers already guard
on `if(TARGET ...)`. The configure log prints the closure, tests, packages and omitted modules.

The full configuration (`CRD_MODULES` empty, every preset) adds every registration and acquires every package, so the
generated target set, compile database and Ninja graph are the ones the literal list produced.

## Verification

`crd_verify_modules()` runs at the end of every configure. It walks every build target, reads `LINK_LIBRARIES` and
`INTERFACE_LINK_LIBRARIES` (through `$<LINK_ONLY:...>` and generator expressions), maps each linked target to its
owning registration by source directory and requires: a module's direct `crd-*` links inside its own `DEPENDS`; a
test directory's links inside an owner's closure, `TESTS` or `TEST_DEPENDS`; a package link inside `PACKAGES` or
`TEST_PACKAGES`; and every linked `crd-*` name to be a configured target. A name inside a conditional generator
expression (`$<$<BOOL:${WIN32}>:crd-gpu-context-dx12>`) is checked when its target exists and tolerated when the
platform does not build it. Any miss lists the offending target, the declaration to change and the module to add,
then fails the configure. Root-level helper targets (`crd-warnings`,
`crd-simd-flags`) and SDK imports (`Vulkan::Vulkan`, `CUDA::*`) are outside the registry.

Build-time tool dependencies are not link edges: `sandbox` declares `asset_cooker` because its cook command runs
`$<TARGET_FILE:asset_cooker>`; declare such edges by hand. Ownership follows what a test links, not its name:
`tests/applications/sandbox` links geometry-viz, draw and imgui and never the sandbox executable, so geometry-viz
owns it and it builds with `CRD_BUILD_SANDBOX=OFF` as before.

[scripts/test-module-selection.py](../../scripts/test-module-selection.py) proves the contract on a fixture project
(full equals all, closure, test ownership, unknown names, undeclared edges and packages, unconfigured link targets,
self-skipping modules, conditional generator-expression links, host-tool import, late or duplicate registrations);
it is the `crd-module-selection` CTest and a repository CI step.

## Host and target

Cookers (`asset_cooker`, `shader_cook`, `kir_autotune`, `ceridc`) run on the host. The sandbox demo pack is one
`add_custom_command` whose inputs are the globbed source assets and the cooker executable, so a tool change or an
input edit recooks and an unchanged tree is a no-op. Cooked identity is FNV-64 content hashing; the pack carries no
timestamp and no host path, so the same sources cook to identical bytes in any output directory. The CEIR op and
capability generators (`tools/ceir_opgen`, `tools/ceir_capability_matrix`) are host Python with committed,
byte-compared outputs. `CRD_HOST_TOOLS_DIR` imports the declared executables so a target configuration can consume a
host build; cross-compiled runtime platforms are not claimed by this contract.

## Not in scope

`dev.py check` against a module-selected build directory and per-family CI lanes (REPO.DEV.5) are separate rows;
public-header checks, include-only edges and the relocatable package are [public consumption](public-consumption.md)
(REPO.DEV.8), which does not record include-only edges in the registry.
