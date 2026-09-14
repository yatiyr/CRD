# Repository layout and hygiene

<!-- doc-role: contract -->
> Contract. Live work and findings: [ROADMAP](../ROADMAP.md); rules: [AGENTS](../../AGENTS.md).

The user requested physical and Visual Studio target grouping on 2026-09-12. This is repository organization,
not a change to public module names, dependency ownership, renderer sequencing or geometry/physics algorithms.
The user subsequently authorized renaming nonconforming public symbols and migrating callers. Follow
[CODING](../CODING.md); preserve constant values, serialized IR names and algorithm semantics. Namespace/global
constants use kCamelCase. The [symbol migration map](../research/2026-09-12-public-symbol-migration.json)
records old/new spellings, including explicit F64 suffixes for double-precision math constants. Numeric bit/ULP gates and affected consumers must pass after spelling changes.

## Source and IDE layout

Modules live in `engine/<family>/<module>/`; corresponding tests live in `tests/<family>/<module>/`.
Families: foundation, numerics (hesap), geometry, execution (CEIR/CHIR), gpu (contexts/CKIR providers), rendering,
assets, world (scene/animation/timeline), physics, media and ui. Test-only support, tools and application families
remain explicit. The existing shared benchmark suite stays under `tests/bench/`.
Public `crd/...` includes and `crd-*` targets remain stable. Family membership describes navigation, not new
dependency permissions. `docs/systems/README.md` remains the module API index. Every module, tool and test
directory is registered once with its declared direct dependencies in the [module registry](module-registry.md),
which also owns `CRD_MODULES` selection and host-tool import.

CMake sets target folders from source ownership; engine and test hierarchies match the physical tree.
Source groups preserve paths beneath each module. External dependency and CMake utility targets have separate
folders. Visual Studio Folder View shows physical families; CMake Targets View shows target families.
Reconfigure existing presets after a checkout that moves sources. Native structural edits use the
[project synchronizer](project-structure-sync.md); CMake remains the build authority.

## Native Visual Studio solution

`win-vs` generates the native Visual Studio 2026 x64 solution with eight MSVC configurations (`build/win-vs-debug/CRD.slnx`
with the verified CMake 4.3.2 installation). It requires CMake 4.2+
([generator reference](https://cmake.org/cmake/help/latest/generator/Visual%20Studio%2018%202026.html)); other presets
retain their existing tool requirements. The full target tree remains visible; its build preset selects the sandbox
and dependencies with two MSBuild workers. [Configuration guide](visual-studio-configurations.md) covers the full
CMake preset selector and native Debug/Release/RelWithDebInfo/ASan/Shipping/ShippingProfile/DebugScalar/DebugSSE2 profiles. The sandbox is the startup project. Debugger working directory and cooked
assets follow the executable's configuration directory, including the selected native configuration suffix.
The debugger sets `CRD_ASSETS_DIR` to the repository's `assets/` tree for disk-first authoring and overrides.

```powershell
python scripts/project-sync.py open --preset win-vs
cmake --build --preset win-vs-debug
# For a selected additional target:
cmake --build build/win-vs-debug --config Debug --target crd-math-tests --parallel 2
```

The preset lets Visual Studio discover its compiler through the selected toolset. It does not inherit Ninja's
bare `cl` override. Open the generated solution to inspect Solution Explorer; opening the repository folder uses
Visual Studio's separate CMake/Folder View. Generated solution/project files stay under ignored `build/`.

### Adding files and folders from Visual Studio

Existing entries edit real source directly. With `project-sync.py open`, saved file/filter/solution edits also become
durable source/CMake structure. Added build-directory items relocate into their owning source directory; module moves
between families physically move source and migrate references. Remove retains disk bytes; Delete is an explicit
separate operation. Empty authored directories persist. Save All, wait for synchronization and accept any VS reload
prompt. The [synchronizer guide](project-structure-sync.md) specifies supported edits, commands, conflicts and recovery.

**File > Open > Folder** still opens Visual Studio's separate CMake workspace. Physical source changes and agent/CLI
operations project back into the native solution when its watcher runs. Compiler flags, dependencies and arbitrary
custom build semantics belong in CMake. New modules require real CMake target definitions; source selection works for
both `crd_collect_sources` and explicit lists through tracked membership overrides. Build affected consumers after moves.

References: [Microsoft CMake project manipulation](https://learn.microsoft.com/en-us/cpp/build/cmake-projects-in-visual-studio?view=msvc-170#cmake-project-manipulation)
and [logical project filters](https://learn.microsoft.com/en-us/cpp/build/reference/vcxproj-filters-files?view=msvc-170).

## Repository contents

Keep authored code/assets, reproducible generators and peer harnesses, documentation and portable shared settings.
Canonical CEIR/CKIR/CHIR source text uses UTF-8 without BOM and LF on every checkout; Git attributes and the
repository guard enforce that byte-stable authoring contract. Write scratch experiments, captures and cleanup recovery bundles beneath ignored `build/<task>/`; keep reusable
scripts in `scripts/` and measurement evidence in `docs/bench/`. Do not delete a harness because its name is old.
Local SDKs, licensed reference dependencies, compiler output and IDE/user caches remain ignored. Agent entry files
are portable project documentation and must remain visible to Git. Do not blanket-ignore source asset formats.

Deletion requires inspected provenance and preservation of unique content. Historical source excerpts and dated
measurements retain their original meaning; source links route to the current locations. A cleanup manifest records
removed artifacts and the recovery location. No Git history rewrite or automatic commit/push is part of cleanup.

## Verification and CI

Check all migrated references, CMake configure/generation, generated source consistency, architecture guards and
representative scoped CTest consumers. A source-only move must preserve file contents except reviewed path updates.
CI owns the full build/configuration sweep; local scope follows [BUILDING](../BUILDING.md). CI must fail on tidy
warnings using the selected pinned LLVM executable, fail on empty test selections, and retain failure output.
Repository/documentation guards run on Windows and Linux; [CI tiers](ci-tiers.md) own which presets a run executes
and the evidence each lane leaves; [pinned inputs](pinned-inputs.md) own every external source, tool, action and
runner the build and the workflow acquire. No remote green claim applies to unpublished changes.
Linux CI installs the SHA-256-pinned Khronos validation layer matching its Vulkan headers; runtime layer selection
is explicit. A passing CTest summary cannot conceal initialization diagnostics emitted before a capture object exists.

## Primary references

- [CMake target folders](https://cmake.org/cmake/help/v3.25/prop_tgt/FOLDER.html) and
  [USE_FOLDERS](https://cmake.org/cmake/help/v3.25/prop_gbl/USE_FOLDERS.html) define generated target organization.
- [CMake source groups](https://cmake.org/cmake/help/v3.25/command/source_group.html) describe source hierarchy.
- [Microsoft CMake Targets View](https://devblogs.microsoft.com/cppblog/cmake-support-in-visual-studio-targets-view-single-file-compilation-and-cache-generation-settings/)
  documents support for target folders and source groups, distinct from physical Folder View.
- [Git ignore semantics](https://git-scm.com/docs/gitignore): ignore rules apply to untracked files; adding a rule
  does not remove an already tracked artifact.

- [Ninja action inputs](https://github.com/seanmiddleditch/gha-setup-ninja/blob/v5/action.yml) expose `destination`;
  CI uses runner temporary storage so tools do not pollute the checked source root.
- [actionlint 1.7.12](https://github.com/rhysd/actionlint/releases/tag/v1.7.12) validates workflow syntax and
  expressions before a full build; the Linux CI tool archive is pinned by SHA-256.
- [LunarG Linux SDK setup](https://vulkan.lunarg.com/doc/view/latest/linux/getting_started.html) documents the
  separate layer/library search paths. The installer pins SDK 1.4.341.1 rather than following that page's latest version.
