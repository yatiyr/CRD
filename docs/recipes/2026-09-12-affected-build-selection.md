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
| Doctor `--inherit-env` | Diagnose the calling environment without vcvars capture | False | Read-only; missing tools remain reported |
| Evidence `--run` | Directory of a sealed local result | Required | Existing envelope; reading never executes its commands |
| Supervisor `timeout` | Explicit command execution budget, seconds | Required by caller | Finite and positive; fixture configure/build use 120 s, CTest uses 60 s |
| Check `--target` | Repeatable explicit CMake target | Inferred affected targets | Diagnostic subset; only configured libraries/executables, at most 32 |
| Check `--jobs` | Compile workers | 2 | 1 or 2; CTest uses one worker and retains resource locks |
| Check `--build-timeout` | Configure/build/tidy budget, seconds | 900 | Finite and positive; exhaustion is not a hang diagnosis |
| Check `--discovery-timeout` | Inventory-process budget, seconds | 120 | Finite and positive |
| Check `--test-timeout` | Default per-test budget, seconds | 180 | Existing CTest TIMEOUT properties retain precedence |
| Check `--dry-run` | Print plan without execution | False | No build, PRE_TEST discovery or evidence writes |
| SIMD guard `-Ipo` / `--ipo` | Actual target IPO setting | `0` | `0` or `1`; only explicit IPO permits insufficient-code skip 77 |

Paths/revisions/configurations are identifiers, not physical quantities. The planner has no timing or precision knob.
Plan is read-only. Check owns bounded discovery because PRE_TEST can execute programs and refresh generated files.

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

## Owning execution and its evidence

Killing only CMake can leave a compiler or test process alive. The [supervisor](../../scripts/cerid_dev/process.py)
starts an isolated waiting Python process (`-I -S`), establishes containment, then grants permission to launch the
native argument vector. The command runs without shell interpolation; spaces/metacharacters retain their literal
argument meaning. Native exit and supervisor cleanup are separate results.

Windows uses a [Job Object](https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects) with
`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, no breakaway flags and assignment before command startup. Cleanup queries the
job's active-process count before returning. A failed assignment cannot fall through into native execution. Linux
uses a new process group, keeps its leader unreaped until cleanup, and monitors the parent's pipe: owner death
terminates the group. The native child result is written atomically before the supervisor waits for final cleanup.
[Python subprocess](https://docs.python.org/3/library/subprocess.html) provides argument-vector execution and the
session/pipe primitives; this deliberately targets ordinary build-tool descendants, not hostile processes.

An explicit timeout records `budget_exhausted`, not an invented engine hang. Launch/containment/missing-status
failures record `instrument_failure`. Preserve the original native exit even when it is nonzero; cleanup failure
retains that outcome as context and prevents success. The tests exercise real descendants and parent death, plus
real CMake/MSVC/GCC/CTest processes. Linux process-liveness evidence distinguishes killed zombies from executing
children; the Windows-only assignment test remains explicitly not applicable on Linux.

A transient access denial while reopening the atomically published native-status file need not mean the command
failed. Retry only the read for at most one second within the overall command budget; retain retry/error metadata.
Never execute the command again to recover missing status. Persistent denial is instrument failure with containment
cleanup. Regressions use a command that writes once and exits 7, plus a persistent-denial case; the
[observed repair](../sessions/2026-09-12-atomic-abuffer-emitter-repair.md#supervisor-status-read-repair) records both hosts.

[Evidence envelopes](../../scripts/cerid_dev/evidence.py) hash process logs and metadata, reject path escapes and
cannot be resealed over an existing result. Inspection checks missing/changed/extra files and the envelope hash.
It preserves the recorded outcome: verified bytes can describe a failed run. This is local content integrity, not
a signature, proof of broad test coverage or published-revision qualification. A partial write or owner death leaves
incomplete evidence, which cannot pass inspection. The [check frontend](../../scripts/cerid_dev/check.py) binds inputs,
selected/executed tests, JUnit, skips, guards and tidy into that envelope. Qualification remains in ROADMAP.

Sort canonical relative POSIX names in both seal and inspection. Path-component ordering differs for directory
`ctest/` and sibling file `ctest.xml`, and Windows Path ordering also folds case. Comparing these different orders
can reject an unchanged artifact set. The regression uses shared prefixes and mixed case; it never rewrites outcomes.

[Doctor diagnostics](../../scripts/cerid_dev/environment.py) use CMake's cached version to choose the matching compiler
metadata, report absent tools as absent, and resolve DLLs as files rather than through executable PATHEXT rules.
Its process-local Windows environment capture is never dumped into logs. Read-only doctor status does not claim
that IDE buffers were inspected; execution must invoke the existing synchronization guard.

[Execution evidence](../sessions/2026-09-12-developer-execution-foundation.md) records the qualified host tuples and
tests. No performance improvement is claimed from these infrastructure changes.

## Configuration-specific discovery and complete checks

[Catch2's integration](https://github.com/catchorg/Catch2/blob/v3.7.1/extras/Catch.cmake) defaults to POST_BUILD,
which shares a test-list path across native configurations. A reproduced Debug-after-Release query invoked Release.
[The Cerid wrapper](../../cmake/CrdTestDiscovery.cmake) selects PRE_TEST, consumes Catch's actual include contribution,
and generates a failing pending placeholder only while the exact configured executable is absent. Target, artifact
and fixture properties are explicit metadata. The selector rejects unknown or contradictory annotations, builds
selected pending owners, then rediscoveries must contain real executable commands. Unrelated pending targets do not
force a repository build. List-valued properties are applied after Catch's TEST_LIST inclusion to avoid flattening.

Direct `add_test` executables use [crd_test_target](../../cmake/CrdTestOwnership.cmake) after registration. It appends
the target and generated executable path to labels without replacing commands or existing properties. Missing
commands are pending only when the current model proves that exact absent artifact; built commands must agree with
the declaration. Fixture/dependency closure still builds required owners. A real partial-build fixture leaves an
unrelated direct test executable absent while the selected consumer completes discovery, CTest and evidence sealing.

CTest's [index-file option](https://cmake.org/cmake/help/v3.25/manual/ctest.1.html#cmdoption-ctest-I) supports the CMake
3.25 baseline. A singleton start/end range followed by explicit additional indices avoids the default all-tests range.
The fresh inventory and selected names are saved; JUnit must report that exact set. A skipped/disabled requirement is
incomplete, not passed. Guard tests consuming object code declare their build owner; CMake's
[TARGET_OBJECTS](https://cmake.org/cmake/help/v3.25/manual/cmake-generator-expressions.7.html#genex:TARGET_OBJECTS)
supplies paths for the selected generator/configuration instead of guessing Ninja/Visual Studio object directories.

Doctor detects empty native compiler defaults before qualification: an existing cache with empty base C++ flags can
lose exception-unwind and optimization settings even when a fresh generator is correct. Preserve a cache backup and
reset only verified-invalid entries through guarded configuration; never erase arbitrary user toolchain settings.
Source identity is measured around execution and the generated model must remain unchanged. The check lock is
separate from the synchronizer writer lock so configure can acquire its own guard. A recorded lock-conflict snapshot
may require a fresh authoritative state inspection; bypassing that guard is not a repair.
For registered native builds, also ask the synchronization owner's `needs_generation` and pending-generation state.
A readable File API reply alone does not prove its baseline finalized; route changed/unfinished projections through
guarded configure before building, and refuse overlap with a live generation.

[Scoped session](../sessions/2026-09-12-scoped-check-and-native-discovery.md) records fixture and real-consumer outcomes,
including failures. Full portable analysis, external-build coordination and real-source qualification have explicit
ROADMAP owners. Fixtures alone cannot establish those gates.

Native generators may omit the compiler path from CMakeCache.txt while recording it in CMakeCXXCompiler.cmake.
Read the matching generated metadata, initialize the process-local MSVC runtime tools, and report MSBuild rather
than Ninja for a Visual Studio generator. MSBuild finding its compiler does not put dumpbin on CTest's PATH.

Disassembler failures must retain a nonzero result; empty output cannot prove an LTO object. The SIMD guard receives
the target's actual [configuration-specific IPO property](https://cmake.org/cmake/help/v3.25/prop_tgt/INTERPROCEDURAL_OPTIMIZATION_CONFIG.html).
Only explicit IPO with insufficient native code, or the unimplemented NEON check, returns
[CTest skip code 77](https://cmake.org/cmake/help/v3.25/prop_test/SKIP_RETURN_CODE.html). The scoped frontend counts that
as incomplete. The non-IPO sibling requires actual passing evidence. Controlled decoder fixtures test failure,
empty output, expected/missing YMM instructions and explicit skip reporting independently of compiler performance.
Windows PowerShell can throw on redirected native stderr before the decoder exit is captured. Temporarily use
Continue only around native invocation, capture its exit immediately, then restore the preference. The controlled
batch decoder writes stderr and exits 9; both IPO settings must retain that diagnostic and report instrument failure.
