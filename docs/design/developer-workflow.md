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

Use CTest JSON discovery, not human output. Rediscover after building because Catch2 inventories are generated at
build time. Zero expected tests, missing executable commands, disabled/skipped tests and fixture expansion require
explicit accounting. Keep existing resource locks, timeouts, validation, precision and performance contracts.

## Frontend and evidence

`doctor` is read-only: actual tools, preset eligibility, cache/toolchain identity, runtime paths, disk/RAM and sync
state. `plan` prints reasons, targets, tests/guards, risk lanes and exact commands as text or JSON. `check` executes
the reviewed scope with bounded jobs, real exit codes, dry-run support and no synchronization bypass. `evidence`
records revision plus worktree content identity, model/configuration, commands, counts, durations and result files.
Existing direct CMake/CTest helpers remain valid. Incomplete execution never becomes a successful qualification.

## Verification

Adversarial temporary fixtures cover opaque IDs, reverse closure, interface includes, generated sources, multiple
configurations, stale/foreign/corrupt replies, path safety, deleted/renamed/untracked changes, assets, fixture tests,
unknown ownership and zero matches. Exercise a real CMake/CTest fixture on Windows and Linux, then inspect the
real Cerid graph without building unrelated modules. Compare selected and full CI coverage before replacing gates.
The advisor capability was searched and is unavailable; no advisor review is claimed.

Primary interfaces: [CMake File API](https://cmake.org/cmake/help/v3.25/manual/cmake-file-api.7.html),
[CTest JSON and fixtures](https://cmake.org/cmake/help/v3.25/manual/ctest.1.html).
