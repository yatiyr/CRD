# Conservative affected-build selection

<!-- doc-role: reference -->
> Technique reference. Contract: [developer workflow](../design/developer-workflow.md). Status: [ROADMAP](../ROADMAP.md).

## Parameters

| Input | Meaning | Default | Valid range / consequence |
|---|---|---|---|
| `--root` | Repository source directory | Script's repository | Existing checkout; no source mutation |
| `--build` | Configured CMake build directory | Required | One actual build; missing model broadens requirements |
| `--config` | Configuration within that build | Sole configuration | Required for native multi-config builds; exact name |
| `--base`, `--head` | CI comparison revisions | Local worktree mode | Both supplied, clean checkout at resolved head |
| `--path` | Repeatable diagnostic change scenario | All net tracked/untracked changes | Repository-relative files; cannot combine with CI revisions |
| `--full` | Explain full qualification scope | False | Planning only; no automatic local sweep |
| `--json` | Structured plan output | Human-readable text | Includes reasons, identity, targets and CTest properties |

Paths/revisions/configurations are identifiers, not physical quantities. The planner has no timing or precision knob.
Its discovery subprocesses are bounded; it does not execute test bodies or build targets.

## Why a dependency graph is necessary

Changing a library can invalidate an executable whose own source did not change. Editing a header-only interface
can affect consumers even when the interface itself has no generated build target. Runtime assets and generators
can also affect behaviour without a link dependency. Selecting only the changed directory misses these cases.

Let `D(t)` be the configured dependencies of target `t`, and `S` the proven owners of changed source/header paths.
The affected set is the least fixed point `A = S union {t | D(t) intersects A}`. A queue over reverse edges computes
it without revisiting targets. This is a conservative dependency proof, not a prediction of which test will fail.

[CMake File API v2](https://cmake.org/cmake/help/v3.25/manual/cmake-file-api.7.html) supplies configuration-specific
targets, sources, artifacts, compiler include paths and opaque dependency IDs. An ID must be resolved through its
configuration's reference map. Splitting the printed ID is not part of the API contract. Propagated include paths
cover omitted header-only interfaces conservatively; they can select extra consumers, which is preferable to a miss.

## Assembly

1. Read Git's NUL-delimited net changes and nonignored untracked paths, or the actual clean CI base/head pair.
   Represent renames as old-path removal plus new-path addition. Hash revision and changed content/symlink/deletion
   metadata without writing the Git index. An explicit path scenario is clearly separate from whole-tree evidence.
2. Read the latest CMake reply index, then only its referenced documents. Verify versions, root/build/configuration,
   IDs, configure-input freshness and generation errors. Keep authored utility nodes in the graph; exclude CMake's
   explicitly generator-provided aggregates. Resolve all remaining edges and reverse-traverse proven owners.
3. Broaden unknown ownership, common build rules, generator/runtime assets, additions/removals or unavailable models.
   A broad plan describes a CI obligation; it cannot authorize an accidental local whole-repository build.
4. Discover [CTest JSON](https://cmake.org/cmake/help/v3.25/manual/ctest.1.html), match commands/arguments to artifacts,
   retain unbound guards and expand fixture setup/cleanup plus explicit test dependencies. Preserve properties,
   disabled state, timeouts and resource locks. Include fixture executable owners in the build requirements.
5. Reject unbuilt/incomplete inventories and empty or guard-only coverage for affected runtime targets. The executing
   frontend must build and rediscover before qualification; a plan is never a test result. Arbitrary utility targets
   such as cleanup, installation or dashboard tasks are not automatic build commands.

Example (diagnostic scenario; no compilation):

```powershell
python scripts/dev.py plan --build build/win-vs-debug --config Debug --path engine/execution/ceir-gpu/src/work_build.cpp --json
```

## Traps and limits

- Visual Studio emits several `ALL_BUILD` targets, one per project scope. Treating their names as unique breaks
  discovery; following their consumer edges turns a small edit into a full build. Use `isGeneratorProvided`.
- Catch2 can register an unbuilt placeholder without an executable command. It is incomplete discovery, not a
  passing test or proof that a module has no tests. Never rebuild the whole repository merely to silence it.
- Normalize arbitrary CTest argument text without probing it as a filesystem path: long filter strings are not paths.
- A graph from another configuration/platform cannot qualify missing platform sources. Unknown assets/generators
  require broader checks until proven consumption edges exist. A docs directory can contain executable code; its
  name alone is not a documentation-only exemption.
- A global hygiene guard cannot substitute for an affected consumer test. A disabled test stays visibly disabled.
  Plan identity does not prevent concurrent edits; the executing frontend must revalidate inputs around execution.

## Evidence and implementation

[Session evidence](../sessions/2026-09-12-developer-selection.md) records the focused Windows/Linux fixtures and
actual Cerid graph inspection. No build-speed improvement or benchmark victory is claimed, so there is no performance
board. Full selector-versus-CI comparison and the executing frontend retain their ROADMAP owners.

Code: [selection](../../scripts/cerid_dev/selection.py), [CLI](../../scripts/dev.py),
[adversarial/real fixtures](../../scripts/test-dev-workflow.py). The existing synchronizer query writer is reused;
the synchronizer remains the sole owner of structure transactions.
