# 2026-09-12 — project structure synchronization

<!-- doc-role: evidence -->
> Session evidence. Only live work: [ROADMAP / REPO.SYNC](../ROADMAP.md#slice-repo.sync).

The user requested a robust two-way project structure synchronizer and documentation. Confirmed choices: both
exclude/keep and delete modes; module moves in solution folders also move the physical source and update references.
The [contract](../design/project-structure-sync.md) and [ADR-0131](../decisions/0131-project-structure-synchronization.md)
define the work. Advisor capability is unavailable; no advisor review is claimed. Work is direct, without subagents.

Reuse audit: `crd_collect_sources` discovers source/header files; many tests and sandbox explicitly enumerate them.
`CrdIdeFolders.cmake` projects physical ownership into target groups. At entry, generated `.vcxproj` paths pointed to
real source but structural edits only changed generated files. The adapter now distinguishes saved edits from CMake
generation and compiler-generated items. Existing repository tool tests supplied the cross-platform CI entry point.

This turn's starting files are preserved in `build/project-sync-20260912/before.zip`. Earlier cleanup and VS work
remain uncommitted and are not this turn's baseline. No algorithm changes, commit or push are authorized by this task.

## Delivered mechanism

The [CLI](../../scripts/project-sync.py) shares a portable operation/transaction engine with the saved Visual Studio
adapter. Tracked [membership](../../cmake/project-structure.json) is consumed by every CMake build. Local generation
snapshots and journals live under ignored build directories. Native `open` starts a hidden watcher; saved file/filter/
module edits become physical source/CMake changes and regenerate the solution. Compiler/dependency semantics remain
explicit CMake. Remove/keep and Delete are selectable, including directory operations. Family moves preserve disabled
modules; target renames and public include/path migrations have separate contracts. Empty directories and filters persist.

The adapter rejects unsafe paths, symlink/reparse traversal, collisions, partial XML, unsupported project semantics,
ambiguous ownership and conflicting edits. It coordinates saved IDE buffers and build state through a small COM helper.
Generation observers and the native pre-build guard prevent pending structure from silently compiling. Recovery restores
the tool's pre-sync writes, refuses later edits and pauses ingestion before an explicit regenerated projection. It does
not claim to recover bytes Visual Studio deleted before the tool observed them.

## Verification evidence

Local toolchain: Windows 11, Python 3.14.4, standalone CMake 4.3.2, VS 2026/MSVC 19.51.36246, native x64 Debug;
Linux tests and a real Ninja/GCC fixture ran through WSL. No full engine or configuration sweep was run.

| Check | Evidence |
|---|---|
| Portable adverse cases | `python scripts/test-project-sync.py`: 47 tests passed on Windows and Linux. Paths/case-only moves, concurrent writers, interruption/recovery, CMake projection, XML, modules, watcher retry and generation feedback |
| Actual Visual Studio actions | `python scripts/test-project-sync-native.py --ide`: separate hidden fixture; COM Add File + Add Filter + Save, watcher import, real compile, remove/delete/recover, header and family moves, empty folders, direct generation observer |
| Native saved-project/CLI round trips | `crd-project-structure-native` CTest: real CMake/MSVC compile, source membership, regeneration and no feedback loop |
| Linux compiler round trips | `python3 scripts/test-project-sync-native.py --generator Ninja`: real add/remove/delete/recover and module/header migration compile passed |
| Actual Cerid solution | Reconfigured with 271 projects / 254 source-owned targets; sandbox preset build succeeded with the sync guard and cooked assets. Engine algorithms unchanged |
| Repository/documentation/CI tooling | Scoped CTest guards, roadmap/link/budget validator, repository-tool tests and actionlint 1.7.12; final close-out results below |

Reproducible harnesses are tracked. Local captures: `build/project-sync-20260912/tests-windows.log`, `tests-linux.log`,
`native-ide-integration.log`, `linux-integration.log`, `cerid-configure-final.log`, `cerid-build.log`, `ctest.log`,
`docs-check.log` and `actionlint.log`. These are disposable evidence, not required inputs for another agent's build.
CI now runs portable tests plus Windows VS 2022 and Linux Ninja compile fixtures. Those remote runs remain unobserved
until the user publishes; this session does not close unrelated REPO.3c/3d runner/configuration qualification.

## Lessons verified during integration

- Assembly is a real native project item (`MASM`), not an unknown missing CMake source. Added MASM/NASM item support.
- Compiler-identification projects under CMakeFiles are generated probes, not existing user projects to enroll.
- A watcher must process already-pending edits on startup and retry after busy/unsaved IDE preconditions clear.
  A fresh external generation must not trigger another configure. Source edits arriving during generation stay pending.
- Case-only physical renames need exact-spelling checks even on a case-insensitive filesystem; include spelling matters
  on Linux. Moving a source also rebases relative includes to headers that did not move.
- Native fixture outputs under the OS temporary directory triggered MSBuild MSB8029. The harness now uses a bounded,
  disposable repository build subdirectory; the final native IDE run has no warnings.
- The initial scoped CTest caught BUILDING's orientation size budget after documentation additions. Compacted the guide;
  the validator passed. This was a documentation gate failure, not an engine failure.
- A restricted tool process could not see the user's active VS COM instance. The bridge remembers its attached process
  and rejects that access-context mismatch instead of treating the live IDE as absent.
- Hidden CMake subprocesses need explicit stdout/stderr forwarding on Windows. The normal open workflow was rerun
  after that fix; configuration diagnostics are retained in open.log and the live watcher was restarted.

Documentation: ADR-0131, the detailed operation/recovery guide, BUILDING, repository layout, scripts index, agent rules,
context and the sole ROADMAP table carry the affected facts. PRINCIPLES, SANITY, START_HERE and MEMORY were inspected;
their remaining contracts still apply. No second tracker, speculative renderer work or commit was introduced.

## Close-out

The normal `python scripts/project-sync.py open --preset win-vs-debug` workflow succeeded on the actual Cerid solution.
Its watcher reported `watching` with a live process, a baseline, no incomplete transactions and no pending generation.
The live native structure guard passed. Final scoped CTest: **5/5 passed**, covering repository hygiene, documentation,
repository tools, the 47 portable synchronization regressions and the real native round trip. Actionlint passed.
The single ROADMAP's REPO.SYNC children and parent close against this evidence;
context returns to the previously retained REPO.3c repair. No remote CI result or unrelated DX12 failure is claimed solved.
Use the operation guide for Save All/reload, both removal choices, physical module moves and journal recovery.
