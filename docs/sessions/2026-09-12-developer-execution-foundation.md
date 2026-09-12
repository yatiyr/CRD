# Developer diagnostics and execution foundation

<!-- doc-role: evidence -->
> Dated evidence, 2026-09-12. Live work: [ROADMAP](../ROADMAP.md). Rules: [AGENTS](../../AGENTS.md).

## Scope

REPO.DEV.3b continues from [the selector session](2026-09-12-developer-selection.md). The deliverable remains the
complete doctor/check/evidence frontend and its verification contract. This increment implements doctor, contained
process execution and sealed evidence inspection; the complete scoped `check` orchestrator still needs integration.
No renderer, geometry/physics algorithm, Git commit/push or hardware-registration work was performed.

## Implemented and observed

`python scripts/dev.py doctor --build <directory> [--config <native-profile>] [--json]` reads the actual cache,
CMake-measured compiler metadata, tools, eligible CMake presets, runtime paths, RAM/disk availability and synchronization
records. Process liveness is queried before describing a generation as active. It does not compile/open the IDE bridge,
install anything or change process-global settings. IDE buffers remain explicitly uninspected until the execution guard.
The Windows helper captures the existing `msvc-env.bat` output into the child environment without displaying or storing
environment secrets. DLL lookup searches runtime paths as files, independently of executable PATHEXT rules.

The real Windows Debug cache resolves MSVC `19.51.36246.0`, x64, standalone CMake 4.3.2 and the installed Ninja.
The existing WSL Linux Debug cache resolves GNU `13.3.0`, `/usr/bin/cmake`, `/usr/bin/ctest` and `/usr/bin/ninja`.
The Linux host has no resolved `clang-tidy-20`; that absence is reported, not converted into a strict-analysis pass.
These probes are environment evidence, not fresh engine builds or backend qualification.

The process supervisor starts a waiting, isolated Python child. On Windows the child joins a Job Object before
native commands may start. On Linux it owns a new process group and monitors parent-pipe lifetime. The wrapper
retains its native exit separately from supervisor cleanup, bounds execution, captures complete output and records
duration/status. Timeout means budget exhaustion; it is not a diagnosis of a deadlock. A containment failure prevents
command startup. Native descendants are stopped before results return, including on parent death in the tested cases.

Local evidence envelopes retain process metadata/log hashes and the producer's scope/outcome. Sealing cannot overwrite
an existing envelope. `python scripts/dev.py evidence --run <directory> --json` verifies metadata and artifact hashes,
rejects missing/modified/extra/traversing artifacts, and preserves a recorded failure. Integrity is explicitly distinct
from success, signatures and remote qualification. Runtime errors in JSON mode also return structured failure data.

## Verification and limits

The portable suite grew to 31 cases. Windows executes all 31 successfully; Linux passes 30 with the Windows-only
Job Object assignment case explicitly not applicable. New cases cover
compiler-cache identity, runtime DLL lookup, read-only synchronization diagnostics, live versus abandoned generation,
structured errors, literal command arguments/nonzero exits, timeout of a real descendant, parent death, refused Job
assignment before startup, and evidence corruption/inventory/path boundaries. The supervisor disables Python site
customization before containment; the assignment-failure regression includes an ambient `sitecustomize.py` trap.

The real tiny VS 2026 and Linux/GCC fixtures now route configure, a two-worker build and the selected CTest through the
supervisor. This tests ordinary compiler/CMake descendants, not only sleeping Python fixtures. Process evidence is sealed
per stage. The fixture has a propagated header-only dependency, generated header, unrelated executable and one selected
consumer test. No unrelated Cerid modules are compiled. Results/raw logs are under ignored
`build/research-dev-workflow-20260912/`; this document retains the tracked interpretation.

The [workflow recipe](../recipes/2026-09-12-affected-build-selection.md) documents the new execution mechanism and primary
references. The advisor capability is unavailable; no consultation is claimed. Broad CI comparison, missing runtime
hardware, Linux strict-analysis setup and exact human-published qualification retain their existing owners.

## Remaining integration

Wire the complete `check` command to the existing environment, planner, synchronizer, supervisor and evidence APIs.
It must preserve preset/cache identity, perform scoped configure/build and post-build CTest rediscovery, keep fixtures
and mandatory guards/tidy, verify source identity around execution, record JUnit/executed/skipped/disabled counts,
reject empty/incomplete coverage and prevent overlapping builds. The native Catch2 unbuilt placeholder requires proven
ownership/discovery handling; do not build unrelated modules merely to remove it. CMake already makes the sandbox and
gizmo probe depend on `cook-demo-assets`, so normal selected consumer builds retain that generator dependency.

Final fixture results: VS 2026 Debug and WSL GCC/Ninja each configure, build and run **1/1 selected consumer CTest**
through the supervisor, with verified sealed evidence per stage. Six existing repository-tool cases, repository
hygiene, Actionlint and whitespace checks pass. Documentation validation passes **851 rows, 1,006 documents and
8,538 local links**, including orientation budgets. These results close no unavailable backend or published CI gate.

The published CI run was re-queried and remained active at revision `9045eebb5c072b6025343c67343a57a57c79bc85`.
Release, Shipping and ASan lanes had no new terminal results; no unchanged polling outcome was treated as completion.
Continue independent repository work; keep REPO.3c/REPO.3d and renderer review gates open.
