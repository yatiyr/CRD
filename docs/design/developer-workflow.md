# Developer workflow contract

<!-- doc-role: reference -->
> Contract for REPO.DEV.3. Status and children live only in [ROADMAP](../ROADMAP.md).

Purpose: fast, explainable local verification without losing affected consumers or platform obligations.
The [accepted research](../research/2026-09-12-large-cpp-development-and-ci.md#local-development-contract)
defines the full scope. This is a thin Python frontend over CMake, CTest and existing Cerid helpers.
It neither implements another build engine nor owns project-structure transactions.

## Selection

Read the current configuration from CMake File API codemodel v2 and cmakeFiles v1. Resolve opaque dependency IDs
through the configuration's explicit ID map. Include all target kinds and generated sources; propagated compiler
include directories account conservatively for header-only interfaces omitted from the codemodel. Follow reverse
edges to actual consumers. Do not assume a filename, target-name convention or one configuration's graph applies
to another. Associate discovered CTests with built artifacts; preserve fixture setup/cleanup and global guards.

Local changes include net tracked worktree changes against HEAD and nonignored untracked files. CI provides its
actual base/head revision pair. Record both identities; an explicit path list is a diagnostic scenario, not a claim
to cover the whole working tree. Never stage, commit, push or modify the Git index during discovery.

Unknown files, source removals/renames, common build/tool rules, runtime assets without proven consumption edges,
generated inputs, stale/missing models and unavailable test discovery broaden the plan with a reason. Broad
requirements belong to CI; the local frontend must not launch a whole-repository sweep automatically. An unavailable
configuration remains unqualified. Narrow target selection cannot replace the full CI comparison gate.

Use CTest JSON discovery, not human output. Cerid's `crd_discover_tests` uses Catch PRE_TEST with configuration-specific
inventories. Its pending annotations carry a validated target/artifact owner; object-consuming guards declare their
owner with `cerid.test.target`. Preserve those guards for their affected targets. Rediscover after building.
Zero expected tests, missing executable commands, disabled/skipped tests and fixture expansion require
explicit accounting. Keep existing resource locks, timeouts, validation, precision and performance contracts.
Direct CTest executable registrations use `crd_test_target` for CMake-owned artifact identity on fresh partial builds;
missing commands without that proof remain errors. SIMD disassembly must preserve decoder failures. Insufficient native code permits
an explicit CTest skip only for configured IPO; unsupported NEON checks remain unqualified, never green.

## Frontend and evidence

Callable interfaces: `doctor`, `plan`, `check` and sealed `evidence` inspection. Full qualification remains owned by
REPO.DEV.3b/3c; see [scoped execution evidence](../sessions/2026-09-12-scoped-check-and-native-discovery.md).

`doctor` is read-only: actual tools, preset eligibility, cache/toolchain identity, runtime paths, disk/RAM and sync
state, including invalid empty native compiler defaults. `plan` prints reasons, targets, guards, risk lanes and pending
discovery commands as text or JSON. It and `check --dry-run` never invoke PRE_TEST executables. `check` executes
the reviewed scope with bounded jobs, real exit codes, dry-run support and no synchronization bypass. `evidence`
records revision plus worktree content identity, model/configuration, commands, counts, durations and result files.
Existing direct CMake/CTest helpers remain valid. Incomplete execution never becomes a successful qualification.

Configure a new build with the canonical preset helper first. Check refreshes stale/missing File API replies through
guarded configuration, retaining the existing toolchain. Local jobs are one/two; more than 32 selected targets requires
a smaller explicitly diagnostic `--target` scope and CI. `--path`/`--target` never claim whole-tree coverage. It builds
required fixture owners and requires exact selected/JUnit names and counts, with zero skipped/disabled requirements.
Source/revision and model changes invalidate a run. Its OS-backed lock serializes checks; coordination with raw
external builds and other hosts still requires qualification. Windows uses the existing LLVM-20 helper; portable
strict-analysis execution retains REPO.DEV.3b.3. A Linux C++ check currently reports that gap rather than passing.

Execution owns its process tree before starting native tools. Preserve native exits separately from supervisor
cleanup; capture logs and stop descendants on explicit budget expiry/interruption. Unavailable containment is an
instrument failure, not permission to launch an uncontained build. Seal each result without overwriting previous
evidence; artifact integrity alone cannot establish runtime success or qualify a different revision/platform.

Transient permission denial reading the atomic native-status publication permits a bounded read retry (one second
inside the total command budget), never command re-execution. Record retries, preserve the native failure exit and
keep persistent denial as instrument failure; [regression evidence](../sessions/2026-09-12-atomic-abuffer-emitter-repair.md#supervisor-status-read-repair).

## Verification

Adversarial temporary fixtures cover opaque IDs, reverse closure, interface includes, generated sources, multiple
configurations, stale/foreign/corrupt replies, path safety, deleted/renamed/untracked changes, assets, fixture tests,
unknown ownership and zero matches. Exercise a real CMake/CTest fixture on Windows and Linux, then inspect the
real Cerid graph without building unrelated modules. Compare selected and full CI coverage before replacing gates.
The advisor capability was searched and is unavailable; no advisor review is claimed.

Primary interfaces: [CMake File API](https://cmake.org/cmake/help/v3.25/manual/cmake-file-api.7.html),
[CTest JSON and fixtures](https://cmake.org/cmake/help/v3.25/manual/ctest.1.html).
