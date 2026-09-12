# Project structure synchronization

<!-- doc-role: contract -->
> Implementation contract. Only live state: [ROADMAP / REPO.SYNC](../ROADMAP.md#slice-repo.sync).

Purpose: a saved Visual Studio structural edit and the equivalent human/agent operation produce the same durable
source/CMake result. [ADR-0131](../decisions/0131-project-structure-synchronization.md) pins ownership and the user's
remove/delete and physical-module-move choices. This is build tooling; it does not introduce an engine dependency.

## Start and inspect

From the repository root, with Python 3.12+ and the installed CMake/Visual Studio toolchain:

```powershell
python scripts/project-sync.py open --preset win-vs
python scripts/project-sync.py status
python scripts/project-sync.py sync                         # preview pending saved structure
python scripts/project-sync.py stop                         # stop this solution's watcher
```

`open` imports saved edits, configures, starts a hidden local watcher and opens the native solution. Default build:
`build/win-vs-debug`; `--build build/<name>` selects another native build inside this checkout. The watcher observes
two stable snapshots, then synchronizes saved structure and regenerates. **Save All, wait for `watching`, and accept
Visual Studio's Reload All prompt if shown.** Unsaved structural edits or affected buffers block mutation; a running
IDE build delays it. The watcher uses the same desktop/user context as Visual Studio. `status` exposes conflicts;
`build/<preset>/cerid-project-sync/watcher.log` holds the diagnostic. Run `open` again after restarting the computer.
It is not an installed extension or an OS startup service. Merely double-clicking a solution does not start it.

Ordinary source editing always edits the real file. File > Open > Folder remains Visual Studio's separate CMake
workspace. The native adapter watches saved `.vcxproj`, `.filters`, `.slnx`/`.sln` plus physical source structure;
it does not intercept unsaved UI gestures. CMake remains the owner of compiler flags, dependencies, platforms and
configuration-specific semantics. [The configuration guide](visual-studio-configurations.md) explains the full preset
selector, native MSVC profiles and their regression gates. Change those in CMake, not generated MSBuild properties.

## Operation contract

- Existing file edits already address the real source. Synchronization does not rewrite ordinary source content.
- Add source/header/item: resolve target ownership and physical filter path, relocate an item created under the
  generated project directory into source, and persist build membership. Do not overwrite an existing destination.
- Add/rename/move a directory or filter: persist the physical directory; move its contained source paths when renamed.
- Remove from target: exclude membership, retain file bytes. Delete: explicitly remove bytes with recovery evidence,
  then remove all affected build references. IDE actions which already delete bytes are distinguished from exclusion.
- Move module between solution families: move the physical module, migrate source/build/document references and
  regenerate its solution placement. Target identity remains stable unless explicitly renamed.
- CMake/source edits from an agent: regenerate the IDE projection without reinterpreting generator writes as IDE edits.
- Ambiguous edits, collisions, foreign paths, conflicting generations and unsupported foreign project semantics:
  stop with a structured conflict and exact remedy. Never silently ignore a requested structural change.

Project filters beneath a module map to physical relative directories. Conventional `Source Files`, `Header Files`
and `Resource Files` use existing `src`, `include` and `resources` directories. A filter rename moves its directory;
dragging an owned item to another filter moves the file. Shared sources retain their owning physical location.
Removing a filter while retaining its items dissolves the group and keeps their physical paths. Removing contained
items excludes them; it does not infer disk deletion. Empty authored filters and engine/test family folders survive
regeneration through the tracked manifest. Physical engine/test modules use `<area>/<family>/<module>`.

Whole-module moves between families preserve target names and public includes; literal build paths, relative includes,
script paths and documentation links migrate. Renaming a target is a separate operation. A whole family rename also
moves disabled modules. Filesystem renames use stable file/directory identity; ambiguous copies are not guessed to be
moves. New physical modules with a CMakeLists are registered; previously disabled modules are not implicitly enabled.
An unowned new file or multiple candidate targets requires an explicit selection. New native C++ projects import only
portable static/shared/executable source semantics. Custom build steps, CLR/MFC/ATL and foreign dependencies require
an explicit CMake definition. Removing a module with live dependents is rejected until dependencies are rerouted.

## Human and agent operations

Write a JSON operation array under ignored `build/<task>/operations.json`. Preview first when the operation needs
review; `--apply` is the explicit mutation choice. Add `--regenerate` for the complete source-to-IDE round trip.

```powershell
python scripts/project-sync.py edit --operations build/my-task/operations.json
python scripts/project-sync.py edit --operations build/my-task/operations.json --apply --regenerate
```

The two removal choices are `{"op":"remove","target":"crd-core","path":"engine/foundation/core/src/example.cpp"}`
(keep disk) and the same object with `"op":"delete"` (delete disk, remove every affected membership). Use real owned
paths; `example.cpp` is illustrative. Visual Studio Remove retains bytes; a saved IDE Delete that already removed the
file removes its build references. CLI Delete journals bytes before deletion. Native IDE deletion performed before
the synchronizer sees it needs IDE/OS recovery for those earlier deleted bytes; the tool cannot back up missing data.

| Operation | Fields after `op` | Meaning |
|---|---|---|
| `add` | `target`, `path`, optional `group`, UTF-8 `content` | Include an existing file, or create it without overwriting |
| `remove` / `delete` | `target`, `path` | Exclude/keep, or delete with journaled recovery |
| `mkdir` | `target`, `path` | Persist an empty directory inside the owning module |
| `remove-directory` / `delete-directory` | `target`, `path` | Exclude contained members/keep bytes, or enumerate and journal the entire directory before deletion |
| `move` | `source`, `destination`, optional boolean `already_moved` | Move a file/directory/module and migrate references; true reconciles a verified external move |
| `rename-target` | `source`, `destination` | Rename the CMake target and literal callers; physical module movement is separate |
| `add-module` | `target`, `directory`, `kind`, `files`, optional `dependencies` | `STATIC`, `SHARED` or `EXECUTABLE`; files map relative names to UTF-8 content; dependencies are explicit private links |
| `remove-module` | `directory` | Unregister the module, retaining its source files |

Paths are repository-relative and portable, with no traversal, protected roots or destination collisions. Do not
combine overlapping parent/child moves; select their common ancestor. Preview output lists file hashes, directory
effects and the resulting structure manifest. Inspect literal reference edits with Git diff and build affected consumers.
Computed CMake/shell paths cannot be evaluated safely by a structural tool: author those changes explicitly in CMake.

## Persistence, conflicts and recovery

[project-structure.json](../../cmake/project-structure.json) is tracked: schema version, empty directories, excluded
modules, per-target additions/removals and groups. [CrdProjectSync](../../cmake/CrdProjectSync.cmake) consumes membership
on Windows/Linux and other CMake builds, including explicit lists and discovered sources. Check in that manifest with
the source/CMake changes. Generated native projects, registrations, common-generation snapshots and journals stay under
ignored `build/`. A new checkout recreates its local baseline; it does not copy another machine's absolute state.

The native preset enables `CRD_PROJECT_SYNC`. The preconfigure hook imports saved edits before regeneration, and
`crd-project-sync-check` blocks compilation when structure is pending. Direct CMake regeneration has a completion
observer tied to its process handle; successful output alone becomes a new baseline. The configure wrapper handles
completion directly. Edits arriving during generation remain pending for the next pass. A CMake/manifest edit and
an IDE edit against the same old generation produce a conflict requiring reconciliation, not a last-writer winner.
Stop the watcher before deliberate large manual reorganizations; use one coordinated operation and re-open afterward.

First enrollment backs up existing native project files. If they are newer than the last CMake generation stamp,
enrollment refuses to overwrite them: preserve/reconcile their changes or enroll a fresh native build directory.
For a conflict, read `status`, save affected buffers, fix the named path/ownership/CMake ambiguity, and retry `open`.
Never delete synchronization state to force a conflict past the guard. A stopped/dead watcher is not evidence of sync.

```powershell
python scripts/project-sync.py recover <transaction-id>
python scripts/project-sync.py open --preset win-vs
```

Recovery restores the transaction's pre-sync bytes, pauses the watcher, and checks later file edits before overwriting.
Review the recovery before `open`: that command regenerates the recovered source and cancels the imported IDE gesture.
Further saved IDE edits after recovery conflict. Recovery covers writes performed by this tool, not a filesystem move
or native deletion completed before ingestion. Source directories and file modes are restored; multi-file transactions
are recoverable, not globally atomic. Keep the journal until the source changes are safely retained elsewhere.

## Reliability and qualification

The transaction journal retains original bytes and expected output hashes. Validate normalized portable paths and
resolved containment before writes, moves or deletes; reject symlink/reparse traversal. Use exclusive synchronization
and compare-and-swap preconditions. File updates are atomic; interrupted multi-file operations are recoverable and
reported as incomplete until recovery succeeds. Rollback refuses to overwrite a subsequent editor/agent edit.

Track only source ownership and structural metadata; compiler flags remain CMake's responsibility. Generated source,
external dependencies, shared files and multiple target ownership must not be accidentally relocated. Detect saved
IDE deltas before regeneration can replace project files. Keep disposable state and journals under ignored `build/`;
the canonical structure manifest is tracked and portable. Unsupported/hardware-unavailable checks are not green.

Required proof: pure transaction/path tests on Windows/Linux; add/remove/delete/rename/move round trips; empty folders;
explicit and discovered CMake source lists; two writers; malformed/partial XML; path escape/case collision; interruption
and rollback conflict; generation feedback suppression; module/reference migration; native Visual Studio generation
and affected target compile. Portable tests: [test-project-sync.py](../../scripts/test-project-sync.py). Real compiler
fixture: [test-project-sync-native.py](../../scripts/test-project-sync-native.py), `--generator "Visual Studio 18 2026"`
or `--generator Ninja`; optional Windows `--ide` exercises a separate hidden VS instance. CTest registers
`crd-project-structure-sync` and, in native VS builds, `crd-project-structure-native`. CI also runs Windows VS 2022
and Linux Ninja fixture legs. The [session](../sessions/2026-09-12-project-structure-sync.md) records actual evidence;
registration alone is not remote CI qualification.

## Implementation map

[storage](../../scripts/project_sync/storage.py) owns paths/locks/journals; [model](../../scripts/project_sync/model.py)
owns the manifest and CMake file-API model; [operations](../../scripts/project_sync/operations.py) owns shared edits and
reference migration; [visual_studio](../../scripts/project_sync/visual_studio.py) compares saved native structures;
[service](../../scripts/project_sync/service.py) owns generation/watch/guard lifecycle; [ide](../../scripts/project_sync/ide.py)
and [COM bridge](../../scripts/project_sync/vs_bridge.cs) coordinate saved buffers and builds. The [CLI](../../scripts/project-sync.py)
is the common entry. No native compiler/project XML is executed during import; explicit CMake generation is a separate step.

Operational bounds: 2-second polling and two stable observations; writer lock 10 seconds; IDE bridge timeout 20 seconds;
generation observer handshake 10 seconds; XML maximum 16 MiB and JSON maximum 32 MiB. DTD/entity input is rejected.
Native source ownership must agree across configurations; [the native preset offers eight](visual-studio-configurations.md).
Vendor/generated files are protected. Partial configuration exclusions require All Configurations or an explicit CMake rule.
Running the CLI in a restricted session that cannot reach an already attached IDE is a conflict, not detached permission.
Do not save another structural gesture while CMake is replacing its projection; wait for synchronization/reload first.
