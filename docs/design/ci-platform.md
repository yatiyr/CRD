# The Cerid CI platform

<!-- doc-role: reference -->
> Contract for CI-0 to CI-4. Status lives only in [ROADMAP](../ROADMAP.md#slice-ci-0); the decision is
> [ADR-0135](../decisions/0135-cerid-ci-platform.md); the operator's steps are the
> [setup recipe](../recipes/ci-platform-setup.md).

Purpose: one testing workflow that qualifies Cerid identically on every platform and device it targets, on rented
cloud machines now and owned machines later, under 50 USD a month, run by one program and one registry.

## The user-facing system

| File | What it is |
|---|---|
| [ci/devices.json](../../ci/devices.json) | The device registry: one cell per device (kind, OS, CPU and levels, GPU and APIs, screen, executor, tier, cost, accounts), the features (dashboard columns) with their roadmap rows, the console profiles, the accounts by name and the budget |
| [ci/pipeline.py](../../ci/pipeline.py) | Planner, lanes, console profile, conclusion, probes and the launcher's dispatcher |
| [ci/executors/](../../ci/executors/__init__.py) | `gce_vm`, `owned_agent`, `firebase_test_lab`, `ssh_host` |
| [ci/ops.py](../../ci/ops.py) | `status`, `add-device`, `budget`, `accounts`, `parity`, `bake` |
| [ci/dashboard/generate.py](../../ci/dashboard/generate.py) | The QUAL dashboard (render, publish) |

Everything else is reused: [ci-tiers.json](../../.github/ci-tiers.json) and [ci-tier.py](../../scripts/ci-tier.py)
(tiers, resolution, the required-result rule), the configure and test presets, the `crd_module()` registry
([module registry](module-registry.md)), the install helpers and `cmake/pins.json`, `ci-evidence.py` (inventory, watch,
bundle), `check-registered-failures.py`, `diag-seeded-faults.py`, the GenMC helpers and every `crd-check-*` guard
through CTest. The contract tests are [test-ci-platform.py](../../scripts/test-ci-platform.py), a step of the
hosted repository job and of the new repository checks.

## A build, end to end

1. Buildkite starts the build's single configured step on the launcher queue: `python ci/pipeline.py upload`.
2. `upload` runs the preflight checks (actionlint while GitHub workflows exist, `check-pins.py`, `check-ci-tiers.py`,
   `test-ci-platform.py`), maps the Buildkite source to a ci-tier.py event (`schedule`, a manual build with
   `CERID_CI_TIER` as `workflow_dispatch`, a pull request against its merge base, a push against the newest head the
   store recorded for the branch), plans, uploads `ci-plan.json` as an artifact and the pipeline to Buildkite, and
   records the head for the next push.
3. Every planned step runs `pipeline.py lane`, `pipeline.py run repository` or a device command on its cell's queue:
   `cerid-cloud` for launched machines, `cerid-owned` for owned agents.
4. The dispatcher on the launcher (`pipeline.py dispatch`) lists the cloud queue's scheduled jobs, routes each by its
   `device=` rule, applies the budget, reserves it, issues a one-job acquisition token and launches the cell's
   executor; it reaps finished machines and writes their cost to the ledger.
5. `Required result` (`pipeline.py conclude`) passes only when every planned step passed or was cached; a budget step
   makes it fail as incomplete.
6. `QUAL dashboard` renders the page from the store and publishes it when Cloudflare is configured.

## Steps and their keys

| Step | Key | Command | Runs |
|---|---|---|---|
| Repository checks | `repository-<host>` | `pipeline.py run repository` | every build, on the Linux and Windows hosts |
| Lane | `lane-<preset>` | `pipeline.py lane --preset P --device D [--modules M]` | the presets ci-tier.py selects, on the cell hosting the preset's ci-tiers job |
| Device lane | `lane-<preset>@<device>` | the same lane on another cell (real GPUs, other CPU vendors, owned machines) | complete tier |
| Console profile | `console-<device>` | `pipeline.py lane ... --console <profile>` | complete tier |
| Device command | `device-<device>-<feature>` | a registry `device_commands` entry (the macOS probe) | complete tier |

The input key of a step is the SHA-256 of its kind, its identity (preset or host), the device cell, the cell's baked
image and the Git blob ids it consumes. Documentation never enters a build key; the repository checks hash the whole
tree. A narrowed lane (`--modules`) hashes only its modules' dependency closure and every path no module owns. A key
whose cache entry passed is skipped as `cached: passed in build N`; `--fresh` (or `CERID_CI_FRESH=1`) ignores it.

## Lanes

A lane runs, phase for phase, the hosted job it replaces (table below), with the same helpers and arguments; GitHub's
`$GITHUB_ENV` and `$GITHUB_PATH` files become a lane-local environment file the helpers write and the lane merges.
Conditions come from the same sources as the workflow: `CACHED_PRESETS` and `COLD_PRESETS` (held equal to the
workflow's lists by a test) and the `preset_steps` of ci-tiers.json (the sse2 pass, the SDE arm, the seeded detector
failures, the GenMC model). After a failed phase only the cache statistics, the evidence bundle and the artifact upload
still run (GitHub's `always()`), and GenMC still runs unless cancelled. Spot preemption and a lost agent are retried
on a new machine twice; a failed test is never retried. The compiler cache uses the sccache GCS backend
(`CERID_CI_SCCACHE_BUCKET`, a key prefix per preset and image) and falls back to an empty local cache. The CPM source
cache and the GenMC build are restored from and saved to the result store under GitHub's key rules.

## Console profile

[ADR-0135 §9](../decisions/0135-cerid-ci-platform.md#9-the-console-profile-lane-runnable-now) defines the
constraints. On Linux the test commands run in a transient systemd service (`systemd-run --wait --pipe` with
`MemoryMax`, `MemorySwapMax=0`, `AllowedCPUs`, `MemoryDenyWriteExecute`, `NoNewPrivileges`, `ProtectSystem=strict`,
`ProtectHome=read-only`, `PrivateTmp` and the build tree writable); on Windows through `pipeline.py jail`, which joins
a new Job Object with a job-wide memory limit, an affinity mask and kill-on-close before starting the command, after
the lane turned on Arbitrary Code Guard and the image-load blocks for every test executable and labelled the build tree
Low integrity. Four probes run inside the same wrapper before the tests and fail the lane unless: an allocation just
under the cap succeeds and one past it fails, the usable CPUs are at most the profile's, a writable and executable
mapping is refused, and a write outside the build tree fails while one inside succeeds.

## Budget

`apply_budget` walks the paid steps cheapest priority first (repository 0, change lanes 1, complete lanes and device
lanes 2, tier C 3) and marks any that would pass the cap with the month's recorded spend as `budget`. The dispatcher
refuses a paid launch whose price times the cell's maximum run time would pass the cap, and every paid launch once the
GCP budget notification reports the budget reached. Estimates use the median of the last five recorded durations of
the step, else the registry's defaults. `ci/ops.py budget` shows the cap, the spend, the GCP state and the price of a
complete run.

## Result store layout

| Path | Written by | Content |
|---|---|---|
| `results/<build>/<step>.json` | each lane | outcome, device, preset, features, log link, minutes, input key |
| `cache/<input key>.json` | each passing step | the build and log that passed |
| `tests-passed/<preset>@<device>/<build>.json` | each lane | the binary keys of the tests that passed in that run; a lane reads its last five |
| `durations/<step>.json` | each step | the last 20 durations |
| `machines/<job>.json`, `ledger/<YYYY-MM>/<job>.json` | the dispatcher | launched machines; their real cost |
| `budget/gcp-<YYYY-MM>.json` | the dispatcher, `ops.py budget --pull` | the newest Cloud Billing notification |
| `images/<family>.json` | `ops.py bake` | the baked image of a family |
| `heads/<branch>.json` | `upload` | the last planned head (the next push's base) |
| `quota/ftl/<date>/<kind>/<job>.json` | the FTL adapter | the Spark plan's daily test quota |
| `blobs/cpm/`, `blobs/genmc/` | lanes | dependency caches |

## Adding a device

`python ci/ops.py add-device --id ... --kind ... --executor owned_agent ...` validates the whole registry with the new
cell (kinds, levels, APIs, vendors, features, accounts, presets, the hosted jobs' single owners and the required
coverage) and refuses an invalid one without writing. For an owned machine it prints the `buildkite-agent.cfg` and the
install steps (the pinned archive and its digest). A gce_vm cell needs its image family baked once:
`python ci/ops.py bake <device>`.

## Parity with GitHub Actions (CI-4)

Generated by `python ci/ops.py parity`; `scripts/test-ci-platform.py` fails when the workflow and this table differ,
when a step has no Buildkite step, or when a mapped phase is not in the lane. The first table is `ci.yml`; the second
lists the dispatch-only host-check experiments (`host-check.yml`), each open until a Buildkite experiment step carries
it: their inline timing loops must first move into scripts. The parity proof itself (the same revisions green on both
systems for every preset, test, guard, sanitizer, SDE arm and ARM64 lane) is CI-4's remaining evidence.

<!-- parity:begin -->
| GitHub job | GitHub step | Buildkite step | Carried by |
|---|---|---|---|
| `preflight` | uses actions/checkout | `plan` | agent checkout of the build commit |
| `preflight` | uses actions/setup-python | `plan` | the launcher image's Python 3.12 |
| `preflight` | Validate workflows (actionlint 1.7.12) | `plan` | run preflight: Validate workflows (actionlint) |
| `preflight` | Pinned inputs agree with the registry | `plan` | run preflight: Pinned inputs agree with the registry |
| `preflight` | Tier mapping owns every preset | `plan` | run preflight: Tier mapping owns every preset |
| `preflight` | Choose the comparison base | `plan` | push_base(): the newest planned head on the branch (result store) |
| `preflight` | Fetch the comparison base | `plan` | git fetch of the base before planning |
| `preflight` | Resolve the tier | `plan` | plan(): ci-tier.py resolve, then the module selection |
| `repository` | uses actions/checkout | `repository-<host>` | agent checkout of the build commit |
| `repository` | uses actions/setup-python | `repository-<host>` | the image's Python 3.12 |
| `repository` | Install pinned Ninja 1.11.1 | `repository-<host>` | run repository: install-ninja.py |
| `repository` | Repository hygiene and layout | `repository-<host>` | run repository: scripts/check-repository.py |
| `repository` | Documentation and roadmap | `repository-<host>` | run repository: scripts/check-master-plan.py |
| `repository` | CI tier mapping | `repository-<host>` | run repository: scripts/check-ci-tiers.py |
| `repository` | Pinned inputs | `repository-<host>` | run repository: scripts/check-pins.py |
| `repository` | Generated sources carry their provenance and bytes | `repository-<host>` | run repository: scripts/check-generated.py |
| `repository` | Allman braces on every control statement | `repository-<host>` | run repository: scripts/check-allman-braces.py |
| `repository` | License manifest follows the registry | `repository-<host>` | run repository: scripts/gen_license_manifest.py --check |
| `repository` | Build tooling regression checks | `repository-<host>` | run repository: scripts/test-repository-tools.py |
| `repository` | Module registry contracts | `repository-<host>` | run repository: scripts/test-module-selection.py |
| `repository` | Affected workflow selection contracts | `repository-<host>` | run repository: scripts/test-dev-workflow.py |
| `repository` | CPU sampling runbook verdicts | `repository-<host>` | run repository: scripts/test-sample-cpu-wpr.py |
| `repository` | Counter capability verdicts | `repository-<host>` | run repository: scripts/test-counters-capability.py |
| `repository` | Debugger recipe verdicts | `repository-<host>` | run repository: scripts/test-debugger-recipes.py |
| `repository` | Reverse debugging verdicts | `repository-<host>` | run repository: scripts/test-reverse-debug-recipes.py |
| `repository` | Seeded detector failure verdicts | `repository-<host>` | run repository: scripts/test-diag-seeded-faults.py |
| `repository` | Diagnostic coverage verdicts | `repository-<host>` | run repository: scripts/test-diag-coverage.py |
| `repository` | Overhead board verdicts | `repository-<host>` | run repository: scripts/test-diag-overhead.py |
| `repository` | Overhead external sampling verdicts | `repository-<host>` | run repository: scripts/test-sample-overhead-wpr.py |
| `repository` | Project structure synchronization | `repository-<host>` | run repository: scripts/test-project-sync.py |
| `repository` | Canonical build profile contracts | `repository-<host>` | run repository: scripts/test-native-build-profiles.py |
| `repository` | Native MSVC profile compile and execution | `repository-<host>` | run repository: scripts/test-native-build-profiles.py --generator Visual Studio 18 2026 |
| `repository` | Native Visual Studio structure round trips | `repository-<host>` | run repository: scripts/test-project-sync-native.py --generator Visual Studio 18 2026 |
| `repository` | Linux structure projection and compile | `repository-<host>` | run repository: scripts/test-project-sync-native.py --generator Ninja |
| `repository` | Generated CEIR artifacts | `repository-<host>` | run repository: tools/ceir_opgen/ceir_opgen.py --check |
| `repository` | Capability source references | `repository-<host>` | run repository: tools/ceir_capability_matrix/gen_matrix.py --check |
| `repository` | CI platform contracts | `repository-<host>` | run repository: scripts/test-ci-platform.py |
| `windows` | Checkout | `lane-<preset> (win-debug, win-release, win-asan, win-relwithdebinfo, win-debug-scalar, win-public-checks)` | Lane.checkout |
| `windows` | Setup Python 3.12 | `lane-<preset> (win-debug, win-release, win-asan, win-relwithdebinfo, win-debug-scalar, win-public-checks)` | Lane.python_version |
| `windows` | Install pinned Ninja 1.11.1 | `lane-<preset> (win-debug, win-release, win-asan, win-relwithdebinfo, win-debug-scalar, win-public-checks)` | Lane.ninja |
| `windows` | Setup MSVC | `lane-<preset> (win-debug, win-release, win-asan, win-relwithdebinfo, win-debug-scalar, win-public-checks)` | Lane.msvc |
| `windows` | Record graphics driver identity | `lane-<preset> (win-debug, win-release, win-asan, win-relwithdebinfo, win-debug-scalar, win-public-checks)` | Lane.adapters |
| `windows` | Cache CPM downloads | `lane-<preset> (win-debug, win-release, win-asan, win-relwithdebinfo, win-debug-scalar, win-public-checks)` | Lane.cpm_cache |
| `windows` | Cache Vulkan SDK | `lane-<preset> (win-debug, win-release, win-asan, win-relwithdebinfo, win-debug-scalar, win-public-checks)` | Lane.vulkan_sdk |
| `windows` | Install Vulkan SDK | `lane-<preset> (win-debug, win-release, win-asan, win-relwithdebinfo, win-debug-scalar, win-public-checks)` | Lane.vulkan_sdk |
| `windows` | Set Vulkan environment | `lane-<preset> (win-debug, win-release, win-asan, win-relwithdebinfo, win-debug-scalar, win-public-checks)` | Lane.vulkan_sdk |
| `windows` | Cold acquisition (empty package cache, pinned WARP) | `lane-<preset> (win-debug, win-release, win-asan, win-relwithdebinfo, win-debug-scalar, win-public-checks)` | Lane.cold_before |
| `windows` | Install pinned sccache | `lane-<preset> (win-debug, win-release, win-asan, win-relwithdebinfo, win-debug-scalar, win-public-checks)` | Lane.sccache |
| `windows` | Compiler cache service | `lane-<preset> (win-debug, win-release, win-asan, win-relwithdebinfo, win-debug-scalar, win-public-checks)` | Lane.sccache |
| `windows` | Install pinned Intel SDE | `lane-<preset> (win-debug, win-release, win-asan, win-relwithdebinfo, win-debug-scalar, win-public-checks)` | Lane.sde |
| `windows` | Configure | `lane-<preset> (win-debug, win-release, win-asan, win-relwithdebinfo, win-debug-scalar, win-public-checks)` | Lane.configure |
| `windows` | Cold acquisition (every pinned package acquired) | `lane-<preset> (win-debug, win-release, win-asan, win-relwithdebinfo, win-debug-scalar, win-public-checks)` | Lane.cold_after |
| `windows` | Configure with the compiler cache | `lane-<preset> (win-debug, win-release, win-asan, win-relwithdebinfo, win-debug-scalar, win-public-checks)` | Lane.configure |
| `windows` | Build | `lane-<preset> (win-debug, win-release, win-asan, win-relwithdebinfo, win-debug-scalar, win-public-checks)` | Lane.build |
| `windows` | Compiler cache statistics | `lane-<preset> (win-debug, win-release, win-asan, win-relwithdebinfo, win-debug-scalar, win-public-checks)` | Lane.sccache_stats |
| `windows` | Test inventory | `lane-<preset> (win-debug, win-release, win-asan, win-relwithdebinfo, win-debug-scalar, win-public-checks)` | Lane.inventory |
| `windows` | Test | `lane-<preset> (win-debug, win-release, win-asan, win-relwithdebinfo, win-debug-scalar, win-public-checks)` | Lane.test |
| `windows` | Test at the sse2 level | `lane-<preset> (win-debug, win-release, win-asan, win-relwithdebinfo, win-debug-scalar, win-public-checks)` | Lane.test_sse2 |
| `windows` | Seeded detector failures | `lane-<preset> (win-debug, win-release, win-asan, win-relwithdebinfo, win-debug-scalar, win-public-checks)` | Lane.seeded_faults |
| `windows` | Evidence bundle and summary | `lane-<preset> (win-debug, win-release, win-asan, win-relwithdebinfo, win-debug-scalar, win-public-checks)` | Lane.bundle |
| `windows` | Preserve evidence | `lane-<preset> (win-debug, win-release, win-asan, win-relwithdebinfo, win-debug-scalar, win-public-checks)` | Lane.upload |
| `clang-tidy` | Checkout | `lane-<preset> (win-tidy)` | Lane.checkout |
| `clang-tidy` | Setup Python 3.12 | `lane-<preset> (win-tidy)` | Lane.python_version |
| `clang-tidy` | Install pinned Ninja 1.11.1 | `lane-<preset> (win-tidy)` | Lane.ninja |
| `clang-tidy` | Setup MSVC | `lane-<preset> (win-tidy)` | Lane.msvc |
| `clang-tidy` | Record graphics driver identity | `lane-<preset> (win-tidy)` | Lane.adapters |
| `clang-tidy` | Install clang-tidy (LLVM 20.1.8, pinned) | `lane-<preset> (win-tidy)` | Lane.clang_tidy |
| `clang-tidy` | Verify clang-tidy version | `lane-<preset> (win-tidy)` | Lane.clang_tidy |
| `clang-tidy` | Cache CPM downloads | `lane-<preset> (win-tidy)` | Lane.cpm_cache |
| `clang-tidy` | Cache Vulkan SDK | `lane-<preset> (win-tidy)` | Lane.vulkan_sdk |
| `clang-tidy` | Install Vulkan SDK | `lane-<preset> (win-tidy)` | Lane.vulkan_sdk |
| `clang-tidy` | Set Vulkan environment | `lane-<preset> (win-tidy)` | Lane.vulkan_sdk |
| `clang-tidy` | Configure | `lane-<preset> (win-tidy)` | Lane.configure |
| `clang-tidy` | Build | `lane-<preset> (win-tidy)` | Lane.build |
| `clang-tidy` | Evidence bundle and summary | `lane-<preset> (win-tidy)` | Lane.bundle |
| `clang-tidy` | Preserve evidence | `lane-<preset> (win-tidy)` | Lane.upload |
| `clang-cl` | Checkout | `lane-<preset> (win-clang-cl)` | Lane.checkout |
| `clang-cl` | Setup Python 3.12 | `lane-<preset> (win-clang-cl)` | Lane.python_version |
| `clang-cl` | Install pinned Ninja 1.11.1 | `lane-<preset> (win-clang-cl)` | Lane.ninja |
| `clang-cl` | Setup MSVC | `lane-<preset> (win-clang-cl)` | Lane.msvc |
| `clang-cl` | Record graphics driver identity | `lane-<preset> (win-clang-cl)` | Lane.adapters |
| `clang-cl` | Cache CPM downloads | `lane-<preset> (win-clang-cl)` | Lane.cpm_cache |
| `clang-cl` | Add LLVM to PATH | `lane-<preset> (win-clang-cl)` | Lane.llvm_path |
| `clang-cl` | Cache Vulkan SDK | `lane-<preset> (win-clang-cl)` | Lane.vulkan_sdk |
| `clang-cl` | Install Vulkan SDK | `lane-<preset> (win-clang-cl)` | Lane.vulkan_sdk |
| `clang-cl` | Set Vulkan environment | `lane-<preset> (win-clang-cl)` | Lane.vulkan_sdk |
| `clang-cl` | Install pinned sccache | `lane-<preset> (win-clang-cl)` | Lane.sccache |
| `clang-cl` | Compiler cache service | `lane-<preset> (win-clang-cl)` | Lane.sccache |
| `clang-cl` | Configure | `lane-<preset> (win-clang-cl)` | Lane.configure |
| `clang-cl` | Configure with the compiler cache | `lane-<preset> (win-clang-cl)` | Lane.configure |
| `clang-cl` | Build | `lane-<preset> (win-clang-cl)` | Lane.build |
| `clang-cl` | Compiler cache statistics | `lane-<preset> (win-clang-cl)` | Lane.sccache_stats |
| `clang-cl` | Test inventory | `lane-<preset> (win-clang-cl)` | Lane.inventory |
| `clang-cl` | Test | `lane-<preset> (win-clang-cl)` | Lane.test |
| `clang-cl` | Evidence bundle and summary | `lane-<preset> (win-clang-cl)` | Lane.bundle |
| `clang-cl` | Preserve evidence | `lane-<preset> (win-clang-cl)` | Lane.upload |
| `linux-gcc` | Checkout | `lane-<preset> (linux-clang-tsan, linux-gcc-debug, linux-gcc-release, linux-gcc-relwithdebinfo, linux-gcc-asan, linux-gcc-debug-scalar, linux-gcc-public-checks)` | Lane.checkout |
| `linux-gcc` | Reclaim runner disk | `lane-<preset> (linux-clang-tsan, linux-gcc-debug, linux-gcc-release, linux-gcc-relwithdebinfo, linux-gcc-asan, linux-gcc-debug-scalar, linux-gcc-public-checks)` | Lane.reclaim_disk |
| `linux-gcc` | Setup Python 3.12 | `lane-<preset> (linux-clang-tsan, linux-gcc-debug, linux-gcc-release, linux-gcc-relwithdebinfo, linux-gcc-asan, linux-gcc-debug-scalar, linux-gcc-public-checks)` | Lane.python_version |
| `linux-gcc` | Install pinned Ninja 1.11.1 | `lane-<preset> (linux-clang-tsan, linux-gcc-debug, linux-gcc-release, linux-gcc-relwithdebinfo, linux-gcc-asan, linux-gcc-debug-scalar, linux-gcc-public-checks)` | Lane.ninja |
| `linux-gcc` | Install system dependencies | `lane-<preset> (linux-clang-tsan, linux-gcc-debug, linux-gcc-release, linux-gcc-relwithdebinfo, linux-gcc-asan, linux-gcc-debug-scalar, linux-gcc-public-checks)` | Lane.apt |
| `linux-gcc` | Record system package versions | `lane-<preset> (linux-clang-tsan, linux-gcc-debug, linux-gcc-release, linux-gcc-relwithdebinfo, linux-gcc-asan, linux-gcc-debug-scalar, linux-gcc-public-checks)` | Lane.apt_census |
| `linux-gcc` | Install pinned Vulkan headers and SPIRV-Reflect | `lane-<preset> (linux-clang-tsan, linux-gcc-debug, linux-gcc-release, linux-gcc-relwithdebinfo, linux-gcc-asan, linux-gcc-debug-scalar, linux-gcc-public-checks)` | Lane.vulkan_headers |
| `linux-gcc` | Install matching Vulkan validation layer | `lane-<preset> (linux-clang-tsan, linux-gcc-debug, linux-gcc-release, linux-gcc-relwithdebinfo, linux-gcc-asan, linux-gcc-debug-scalar, linux-gcc-public-checks)` | Lane.vulkan_validation |
| `linux-gcc` | Cache CPM downloads | `lane-<preset> (linux-clang-tsan, linux-gcc-debug, linux-gcc-release, linux-gcc-relwithdebinfo, linux-gcc-asan, linux-gcc-debug-scalar, linux-gcc-public-checks)` | Lane.cpm_cache |
| `linux-gcc` | Cold acquisition (empty package cache) | `lane-<preset> (linux-clang-tsan, linux-gcc-debug, linux-gcc-release, linux-gcc-relwithdebinfo, linux-gcc-asan, linux-gcc-debug-scalar, linux-gcc-public-checks)` | Lane.cold_before |
| `linux-gcc` | Install pinned sccache | `lane-<preset> (linux-clang-tsan, linux-gcc-debug, linux-gcc-release, linux-gcc-relwithdebinfo, linux-gcc-asan, linux-gcc-debug-scalar, linux-gcc-public-checks)` | Lane.sccache |
| `linux-gcc` | Compiler cache service | `lane-<preset> (linux-clang-tsan, linux-gcc-debug, linux-gcc-release, linux-gcc-relwithdebinfo, linux-gcc-asan, linux-gcc-debug-scalar, linux-gcc-public-checks)` | Lane.sccache |
| `linux-gcc` | Install pinned Intel SDE | `lane-<preset> (linux-clang-tsan, linux-gcc-debug, linux-gcc-release, linux-gcc-relwithdebinfo, linux-gcc-asan, linux-gcc-debug-scalar, linux-gcc-public-checks)` | Lane.sde |
| `linux-gcc` | Configure | `lane-<preset> (linux-clang-tsan, linux-gcc-debug, linux-gcc-release, linux-gcc-relwithdebinfo, linux-gcc-asan, linux-gcc-debug-scalar, linux-gcc-public-checks)` | Lane.configure |
| `linux-gcc` | Cold acquisition (every pinned package acquired) | `lane-<preset> (linux-clang-tsan, linux-gcc-debug, linux-gcc-release, linux-gcc-relwithdebinfo, linux-gcc-asan, linux-gcc-debug-scalar, linux-gcc-public-checks)` | Lane.cold_after |
| `linux-gcc` | Configure with the compiler cache | `lane-<preset> (linux-clang-tsan, linux-gcc-debug, linux-gcc-release, linux-gcc-relwithdebinfo, linux-gcc-asan, linux-gcc-debug-scalar, linux-gcc-public-checks)` | Lane.configure |
| `linux-gcc` | Build | `lane-<preset> (linux-clang-tsan, linux-gcc-debug, linux-gcc-release, linux-gcc-relwithdebinfo, linux-gcc-asan, linux-gcc-debug-scalar, linux-gcc-public-checks)` | Lane.build |
| `linux-gcc` | Compiler cache statistics | `lane-<preset> (linux-clang-tsan, linux-gcc-debug, linux-gcc-release, linux-gcc-relwithdebinfo, linux-gcc-asan, linux-gcc-debug-scalar, linux-gcc-public-checks)` | Lane.sccache_stats |
| `linux-gcc` | Test inventory | `lane-<preset> (linux-clang-tsan, linux-gcc-debug, linux-gcc-release, linux-gcc-relwithdebinfo, linux-gcc-asan, linux-gcc-debug-scalar, linux-gcc-public-checks)` | Lane.inventory |
| `linux-gcc` | Test | `lane-<preset> (linux-clang-tsan, linux-gcc-debug, linux-gcc-release, linux-gcc-relwithdebinfo, linux-gcc-asan, linux-gcc-debug-scalar, linux-gcc-public-checks)` | Lane.test |
| `linux-gcc` | Test at the sse2 level | `lane-<preset> (linux-clang-tsan, linux-gcc-debug, linux-gcc-release, linux-gcc-relwithdebinfo, linux-gcc-asan, linux-gcc-debug-scalar, linux-gcc-public-checks)` | Lane.test_sse2 |
| `linux-gcc` | Seeded detector failures | `lane-<preset> (linux-clang-tsan, linux-gcc-debug, linux-gcc-release, linux-gcc-relwithdebinfo, linux-gcc-asan, linux-gcc-debug-scalar, linux-gcc-public-checks)` | Lane.seeded_faults |
| `linux-gcc` | Install GenMC toolchain | `lane-<preset> (linux-clang-tsan, linux-gcc-debug, linux-gcc-release, linux-gcc-relwithdebinfo, linux-gcc-asan, linux-gcc-debug-scalar, linux-gcc-public-checks)` | Lane.genmc |
| `linux-gcc` | Restore GenMC build | `lane-<preset> (linux-clang-tsan, linux-gcc-debug, linux-gcc-release, linux-gcc-relwithdebinfo, linux-gcc-asan, linux-gcc-debug-scalar, linux-gcc-public-checks)` | Lane.genmc |
| `linux-gcc` | GenMC weak-memory model | `lane-<preset> (linux-clang-tsan, linux-gcc-debug, linux-gcc-release, linux-gcc-relwithdebinfo, linux-gcc-asan, linux-gcc-debug-scalar, linux-gcc-public-checks)` | Lane.genmc |
| `linux-gcc` | Evidence bundle and summary | `lane-<preset> (linux-clang-tsan, linux-gcc-debug, linux-gcc-release, linux-gcc-relwithdebinfo, linux-gcc-asan, linux-gcc-debug-scalar, linux-gcc-public-checks)` | Lane.bundle |
| `linux-gcc` | Preserve evidence | `lane-<preset> (linux-clang-tsan, linux-gcc-debug, linux-gcc-release, linux-gcc-relwithdebinfo, linux-gcc-asan, linux-gcc-debug-scalar, linux-gcc-public-checks)` | Lane.upload |
| `windows-shipping` | Checkout | `lane-<preset> (win-shipping, win-shipping-profile)` | Lane.checkout |
| `windows-shipping` | Setup Python 3.12 | `lane-<preset> (win-shipping, win-shipping-profile)` | Lane.python_version |
| `windows-shipping` | Install pinned Ninja 1.11.1 | `lane-<preset> (win-shipping, win-shipping-profile)` | Lane.ninja |
| `windows-shipping` | Setup MSVC | `lane-<preset> (win-shipping, win-shipping-profile)` | Lane.msvc |
| `windows-shipping` | Record graphics driver identity | `lane-<preset> (win-shipping, win-shipping-profile)` | Lane.adapters |
| `windows-shipping` | Cache CPM downloads | `lane-<preset> (win-shipping, win-shipping-profile)` | Lane.cpm_cache |
| `windows-shipping` | Cache Vulkan SDK | `lane-<preset> (win-shipping, win-shipping-profile)` | Lane.vulkan_sdk |
| `windows-shipping` | Install Vulkan SDK | `lane-<preset> (win-shipping, win-shipping-profile)` | Lane.vulkan_sdk |
| `windows-shipping` | Set Vulkan environment | `lane-<preset> (win-shipping, win-shipping-profile)` | Lane.vulkan_sdk |
| `windows-shipping` | Install pinned sccache | `lane-<preset> (win-shipping, win-shipping-profile)` | Lane.sccache |
| `windows-shipping` | Compiler cache service | `lane-<preset> (win-shipping, win-shipping-profile)` | Lane.sccache |
| `windows-shipping` | Configure | `lane-<preset> (win-shipping, win-shipping-profile)` | Lane.configure |
| `windows-shipping` | Configure with the compiler cache | `lane-<preset> (win-shipping, win-shipping-profile)` | Lane.configure |
| `windows-shipping` | Build | `lane-<preset> (win-shipping, win-shipping-profile)` | Lane.build |
| `windows-shipping` | Compiler cache statistics | `lane-<preset> (win-shipping, win-shipping-profile)` | Lane.sccache_stats |
| `windows-shipping` | Test inventory | `lane-<preset> (win-shipping, win-shipping-profile)` | Lane.inventory |
| `windows-shipping` | Test | `lane-<preset> (win-shipping, win-shipping-profile)` | Lane.test |
| `windows-shipping` | Evidence bundle and summary | `lane-<preset> (win-shipping, win-shipping-profile)` | Lane.bundle |
| `windows-shipping` | Preserve evidence | `lane-<preset> (win-shipping, win-shipping-profile)` | Lane.upload |
| `windows-clang-cl-shipping` | Checkout | `lane-<preset> (win-clang-cl-shipping)` | Lane.checkout |
| `windows-clang-cl-shipping` | Setup Python 3.12 | `lane-<preset> (win-clang-cl-shipping)` | Lane.python_version |
| `windows-clang-cl-shipping` | Install pinned Ninja 1.11.1 | `lane-<preset> (win-clang-cl-shipping)` | Lane.ninja |
| `windows-clang-cl-shipping` | Setup MSVC | `lane-<preset> (win-clang-cl-shipping)` | Lane.msvc |
| `windows-clang-cl-shipping` | Record graphics driver identity | `lane-<preset> (win-clang-cl-shipping)` | Lane.adapters |
| `windows-clang-cl-shipping` | Cache CPM downloads | `lane-<preset> (win-clang-cl-shipping)` | Lane.cpm_cache |
| `windows-clang-cl-shipping` | Add LLVM to PATH | `lane-<preset> (win-clang-cl-shipping)` | Lane.llvm_path |
| `windows-clang-cl-shipping` | Cache Vulkan SDK | `lane-<preset> (win-clang-cl-shipping)` | Lane.vulkan_sdk |
| `windows-clang-cl-shipping` | Install Vulkan SDK | `lane-<preset> (win-clang-cl-shipping)` | Lane.vulkan_sdk |
| `windows-clang-cl-shipping` | Set Vulkan environment | `lane-<preset> (win-clang-cl-shipping)` | Lane.vulkan_sdk |
| `windows-clang-cl-shipping` | Install pinned sccache | `lane-<preset> (win-clang-cl-shipping)` | Lane.sccache |
| `windows-clang-cl-shipping` | Compiler cache service | `lane-<preset> (win-clang-cl-shipping)` | Lane.sccache |
| `windows-clang-cl-shipping` | Configure | `lane-<preset> (win-clang-cl-shipping)` | Lane.configure |
| `windows-clang-cl-shipping` | Configure with the compiler cache | `lane-<preset> (win-clang-cl-shipping)` | Lane.configure |
| `windows-clang-cl-shipping` | Build | `lane-<preset> (win-clang-cl-shipping)` | Lane.build |
| `windows-clang-cl-shipping` | Compiler cache statistics | `lane-<preset> (win-clang-cl-shipping)` | Lane.sccache_stats |
| `windows-clang-cl-shipping` | Test inventory | `lane-<preset> (win-clang-cl-shipping)` | Lane.inventory |
| `windows-clang-cl-shipping` | Test | `lane-<preset> (win-clang-cl-shipping)` | Lane.test |
| `windows-clang-cl-shipping` | Evidence bundle and summary | `lane-<preset> (win-clang-cl-shipping)` | Lane.bundle |
| `windows-clang-cl-shipping` | Preserve evidence | `lane-<preset> (win-clang-cl-shipping)` | Lane.upload |
| `linux-gcc-shipping` | Checkout | `lane-<preset> (linux-gcc-shipping)` | Lane.checkout |
| `linux-gcc-shipping` | Reclaim runner disk | `lane-<preset> (linux-gcc-shipping)` | Lane.reclaim_disk |
| `linux-gcc-shipping` | Setup Python 3.12 | `lane-<preset> (linux-gcc-shipping)` | Lane.python_version |
| `linux-gcc-shipping` | Install pinned Ninja 1.11.1 | `lane-<preset> (linux-gcc-shipping)` | Lane.ninja |
| `linux-gcc-shipping` | Install system dependencies | `lane-<preset> (linux-gcc-shipping)` | Lane.apt |
| `linux-gcc-shipping` | Record system package versions | `lane-<preset> (linux-gcc-shipping)` | Lane.apt_census |
| `linux-gcc-shipping` | Install pinned Vulkan headers and SPIRV-Reflect | `lane-<preset> (linux-gcc-shipping)` | Lane.vulkan_headers |
| `linux-gcc-shipping` | Install matching Vulkan validation layer | `lane-<preset> (linux-gcc-shipping)` | Lane.vulkan_validation |
| `linux-gcc-shipping` | Cache CPM downloads | `lane-<preset> (linux-gcc-shipping)` | Lane.cpm_cache |
| `linux-gcc-shipping` | Install pinned sccache | `lane-<preset> (linux-gcc-shipping)` | Lane.sccache |
| `linux-gcc-shipping` | Compiler cache service | `lane-<preset> (linux-gcc-shipping)` | Lane.sccache |
| `linux-gcc-shipping` | Configure | `lane-<preset> (linux-gcc-shipping)` | Lane.configure |
| `linux-gcc-shipping` | Configure with the compiler cache | `lane-<preset> (linux-gcc-shipping)` | Lane.configure |
| `linux-gcc-shipping` | Build | `lane-<preset> (linux-gcc-shipping)` | Lane.build |
| `linux-gcc-shipping` | Compiler cache statistics | `lane-<preset> (linux-gcc-shipping)` | Lane.sccache_stats |
| `linux-gcc-shipping` | Test inventory | `lane-<preset> (linux-gcc-shipping)` | Lane.inventory |
| `linux-gcc-shipping` | Test | `lane-<preset> (linux-gcc-shipping)` | Lane.test |
| `linux-gcc-shipping` | Evidence bundle and summary | `lane-<preset> (linux-gcc-shipping)` | Lane.bundle |
| `linux-gcc-shipping` | Preserve evidence | `lane-<preset> (linux-gcc-shipping)` | Lane.upload |
| `windows-native` | Checkout | `lane-<preset> (win-vs)` | Lane.checkout |
| `windows-native` | Setup Python 3.12 | `lane-<preset> (win-vs)` | Lane.python_version |
| `windows-native` | Install pinned Ninja 1.11.1 | `lane-<preset> (win-vs)` | Lane.ninja |
| `windows-native` | Record graphics driver identity | `lane-<preset> (win-vs)` | Lane.adapters |
| `windows-native` | Cache CPM downloads | `lane-<preset> (win-vs)` | Lane.cpm_cache |
| `windows-native` | Cache Vulkan SDK | `lane-<preset> (win-vs)` | Lane.vulkan_sdk |
| `windows-native` | Install Vulkan SDK | `lane-<preset> (win-vs)` | Lane.vulkan_sdk |
| `windows-native` | Set Vulkan environment | `lane-<preset> (win-vs)` | Lane.vulkan_sdk |
| `windows-native` | Configure | `lane-<preset> (win-vs)` | Lane.configure |
| `windows-native` | Build the Debug sandbox profile | `lane-<preset> (win-vs)` | Lane.build |
| `windows-native` | Test inventory | `lane-<preset> (win-vs)` | Lane.inventory |
| `windows-native` | Test | `lane-<preset> (win-vs)` | Lane.test |
| `windows-native` | Evidence bundle and summary | `lane-<preset> (win-vs)` | Lane.bundle |
| `windows-native` | Preserve evidence | `lane-<preset> (win-vs)` | Lane.upload |
| `linux-arm64` | Checkout | `lane-<preset> (linux-gcc-arm64-debug, linux-gcc-arm64-asan, linux-clang-arm64-debug, linux-clang-arm64-asan)` | Lane.checkout |
| `linux-arm64` | Setup Python 3.12 | `lane-<preset> (linux-gcc-arm64-debug, linux-gcc-arm64-asan, linux-clang-arm64-debug, linux-clang-arm64-asan)` | Lane.python_version |
| `linux-arm64` | Install system dependencies | `lane-<preset> (linux-gcc-arm64-debug, linux-gcc-arm64-asan, linux-clang-arm64-debug, linux-clang-arm64-asan)` | Lane.apt |
| `linux-arm64` | Record system package versions | `lane-<preset> (linux-gcc-arm64-debug, linux-gcc-arm64-asan, linux-clang-arm64-debug, linux-clang-arm64-asan)` | Lane.apt_census |
| `linux-arm64` | Cache CPM downloads | `lane-<preset> (linux-gcc-arm64-debug, linux-gcc-arm64-asan, linux-clang-arm64-debug, linux-clang-arm64-asan)` | Lane.cpm_cache |
| `linux-arm64` | Configure | `lane-<preset> (linux-gcc-arm64-debug, linux-gcc-arm64-asan, linux-clang-arm64-debug, linux-clang-arm64-asan)` | Lane.configure |
| `linux-arm64` | Build | `lane-<preset> (linux-gcc-arm64-debug, linux-gcc-arm64-asan, linux-clang-arm64-debug, linux-clang-arm64-asan)` | Lane.build |
| `linux-arm64` | Test inventory | `lane-<preset> (linux-gcc-arm64-debug, linux-gcc-arm64-asan, linux-clang-arm64-debug, linux-clang-arm64-asan)` | Lane.inventory |
| `linux-arm64` | Test | `lane-<preset> (linux-gcc-arm64-debug, linux-gcc-arm64-asan, linux-clang-arm64-debug, linux-clang-arm64-asan)` | Lane.test |
| `linux-arm64` | Evidence bundle and summary | `lane-<preset> (linux-gcc-arm64-debug, linux-gcc-arm64-asan, linux-clang-arm64-debug, linux-clang-arm64-asan)` | Lane.bundle |
| `linux-arm64` | Preserve evidence | `lane-<preset> (linux-gcc-arm64-debug, linux-gcc-arm64-asan, linux-clang-arm64-debug, linux-clang-arm64-asan)` | Lane.upload |
| `windows-arm64` | Checkout | `lane-<preset> (win-arm64-debug)` | Lane.checkout |
| `windows-arm64` | Setup Python 3.12 | `lane-<preset> (win-arm64-debug)` | Lane.python_version |
| `windows-arm64` | Install pinned Ninja 1.11.1 | `lane-<preset> (win-arm64-debug)` | Lane.ninja |
| `windows-arm64` | Setup MSVC | `lane-<preset> (win-arm64-debug)` | Lane.msvc |
| `windows-arm64` | Cache CPM downloads | `lane-<preset> (win-arm64-debug)` | Lane.cpm_cache |
| `windows-arm64` | Configure | `lane-<preset> (win-arm64-debug)` | Lane.configure |
| `windows-arm64` | Build | `lane-<preset> (win-arm64-debug)` | Lane.build |
| `windows-arm64` | Test inventory | `lane-<preset> (win-arm64-debug)` | Lane.inventory |
| `windows-arm64` | Test | `lane-<preset> (win-arm64-debug)` | Lane.test |
| `windows-arm64` | Evidence bundle and summary | `lane-<preset> (win-arm64-debug)` | Lane.bundle |
| `windows-arm64` | Preserve evidence | `lane-<preset> (win-arm64-debug)` | Lane.upload |
| `required` | uses actions/checkout | `required` | agent checkout of the build commit |
| `required` | uses actions/setup-python | `required` | the launcher image's Python 3.12 |
| `required` | Every expected job ran and passed | `required` | pipeline.py conclude (ci-tier.py conclude rule) |

| GitHub job | GitHub step | Buildkite step | Carried by |
|---|---|---|---|
| `host-check/fuzz` | Checkout | `experiment-fuzz` | open (CI-4): not yet carried |
| `host-check/fuzz` | Setup Python 3.12 | `experiment-fuzz` | open (CI-4): not yet carried |
| `host-check/fuzz` | Install pinned Ninja 1.11.1 | `experiment-fuzz` | open (CI-4): not yet carried |
| `host-check/fuzz` | Install system dependencies | `experiment-fuzz` | open (CI-4): not yet carried |
| `host-check/fuzz` | Install pinned Vulkan headers and SPIRV-Reflect | `experiment-fuzz` | open (CI-4): not yet carried |
| `host-check/fuzz` | Cache CPM downloads | `experiment-fuzz` | open (CI-4): not yet carried |
| `host-check/fuzz` | Configure | `experiment-fuzz` | open (CI-4): not yet carried |
| `host-check/fuzz` | Build the libFuzzer target | `experiment-fuzz` | open (CI-4): not yet carried |
| `host-check/fuzz` | Rounds and repetitions | `experiment-fuzz` | open (CI-4): not yet carried |
| `host-check/fuzz` | Collect logs and artifacts | `experiment-fuzz` | open (CI-4): not yet carried |
| `host-check/fuzz` | Preserve logs and artifacts | `experiment-fuzz` | open (CI-4): not yet carried |
| `host-check/tsan` | Checkout | `experiment-tsan` | open (CI-4): not yet carried |
| `host-check/tsan` | Setup Python 3.12 | `experiment-tsan` | open (CI-4): not yet carried |
| `host-check/tsan` | Install pinned Ninja 1.11.1 | `experiment-tsan` | open (CI-4): not yet carried |
| `host-check/tsan` | Install system dependencies | `experiment-tsan` | open (CI-4): not yet carried |
| `host-check/tsan` | Install pinned Vulkan headers and SPIRV-Reflect | `experiment-tsan` | open (CI-4): not yet carried |
| `host-check/tsan` | Cache CPM downloads | `experiment-tsan` | open (CI-4): not yet carried |
| `host-check/tsan` | Configure | `experiment-tsan` | open (CI-4): not yet carried |
| `host-check/tsan` | Build the jobs tests | `experiment-tsan` | open (CI-4): not yet carried |
| `host-check/tsan` | Rounds | `experiment-tsan` | open (CI-4): not yet carried |
| `host-check/tsan` | Collect logs | `experiment-tsan` | open (CI-4): not yet carried |
| `host-check/tsan` | Preserve logs | `experiment-tsan` | open (CI-4): not yet carried |
| `host-check/arm64-sanitizer-cost` | Checkout | `experiment-arm64-sanitizer-cost` | open (CI-4): not yet carried |
| `host-check/arm64-sanitizer-cost` | Setup Python 3.12 | `experiment-arm64-sanitizer-cost` | open (CI-4): not yet carried |
| `host-check/arm64-sanitizer-cost` | Install system dependencies | `experiment-arm64-sanitizer-cost` | open (CI-4): not yet carried |
| `host-check/arm64-sanitizer-cost` | Cache CPM downloads | `experiment-arm64-sanitizer-cost` | open (CI-4): not yet carried |
| `host-check/arm64-sanitizer-cost` | Use-after-return control | `experiment-arm64-sanitizer-cost` | open (CI-4): not yet carried |
| `host-check/arm64-sanitizer-cost` | Configure and build both trees | `experiment-arm64-sanitizer-cost` | open (CI-4): not yet carried |
| `host-check/arm64-sanitizer-cost` | Interleaved timings | `experiment-arm64-sanitizer-cost` | open (CI-4): not yet carried |
| `host-check/arm64-sanitizer-cost` | Preserve timings | `experiment-arm64-sanitizer-cost` | open (CI-4): not yet carried |
| `host-check/x64-asan-stack-alignment` | Checkout | `experiment-x64-asan-stack-alignment` | open (CI-4): not yet carried |
| `host-check/x64-asan-stack-alignment` | Setup Python 3.12 | `experiment-x64-asan-stack-alignment` | open (CI-4): not yet carried |
| `host-check/x64-asan-stack-alignment` | Install pinned Intel SDE | `experiment-x64-asan-stack-alignment` | open (CI-4): not yet carried |
| `host-check/x64-asan-stack-alignment` | Stack-alignment control | `experiment-x64-asan-stack-alignment` | open (CI-4): not yet carried |
| `host-check/x64-asan-stack-alignment` | Preserve results | `experiment-x64-asan-stack-alignment` | open (CI-4): not yet carried |
<!-- parity:end -->
