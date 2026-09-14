# CI tiers, required result and exact-revision evidence

<!-- doc-role: reference -->
> Contract for REPO.DEV.5. Status lives only in [ROADMAP](../ROADMAP.md#slice-repo.dev.5); the accepted research is
> [CI design](../research/2026-09-12-large-cpp-development-and-ci.md#ci-design); the workflow is
> [ci.yml](../../.github/workflows/ci.yml) and the mapping [ci-tiers.json](../../.github/ci-tiers.json).

Purpose: one coverage contract in three tiers, every supported preset owned by a lane, one stable required status
that cannot be satisfied by a skipped job, cancellation that cannot destroy qualification evidence, and structured
evidence for the exact revision every lane built.

## Tiers and the mapping

[ci-tiers.json](../../.github/ci-tiers.json) gives every visible configure preset a tier and a job:

| Tier | Runs | Presets |
|---|---|---|
| preflight | every run | none; the `preflight` and `repository` jobs (workflow syntax, mapping, hygiene, documentation, tooling fixtures, generated artifacts) |
| change | pushes and pull requests that touch sources | `win-debug`, `win-release`, `win-asan`, `win-debug-sse2`, `win-tidy`, `win-clang-cl`, `win-shipping`, `win-clang-cl-shipping`, `linux-gcc-debug`, `linux-gcc-release`, `linux-gcc-relwithdebinfo`, `linux-gcc-asan`, `linux-gcc-debug-sse2`, `linux-gcc-shipping` |
| complete | nightly, manual, build-system changes | the change presets plus `win-relwithdebinfo`, `win-debug-scalar`, `win-shipping-profile`, `linux-gcc-debug-scalar`, the native `win-vs` solution and the public-check presets `win-public-checks`, `linux-gcc-public-checks` ([public consumption](public-consumption.md)) |
| diagnostic | never hosted | `win-tidy-local`, `linux-clang-fuzz` ([test instruments](test-instruments.md)) |

The change tier is the lane set the repository ran on every push before this contract, so a source change keeps
every obligation it had; only a documentation-only push stops after preflight, by design. The complete tier adds the
five presets that had never run hosted and, since REPO.DEV.8, the two public-check presets. The first complete run is
their first hosted exercise and may surface real defects: that is the tier's purpose, not a regression of the push
signal. The diagnostic presets stay local: clang++ on Linux is a diagnostic compiler only, because the tree does not
build under its `-Werror` set (the [REPO.DEV.11 audit](../sessions/2026-09-14-infrastructure-audit.md) counted 40
unique sites in 13 files: 25 `-Wsign-conversion`, 13 `-Wdouble-promotion`, 2 `-Wnested-anon-types`); a qualified
clang++ lane is a matrix decision for the maintainer, not a gap in the contract.

[check-ci-tiers.py](../../scripts/check-ci-tiers.py) is the `crd-ci-tiers` CTest, a preflight step, a repository
step and a tooling test. It fails when a visible preset has no owner or an entry names no preset, when an owning job
does not need `preflight`, is not gated on its non-empty list, does not take its matrix from the resolver's output,
is missing from the required aggregate or runs on another runner than declared, when a diagnostic preset is
invoked, when the class table lacks a catch-all, when a trigger is missing, or when cancellation is not limited to
pull requests.

## Resolution

[ci-tier.py resolve](../../scripts/ci-tier.py) runs once in the non-matrix `preflight` job (a matrix job's outputs
are those of its last leg). Scheduled runs take the complete tier; a manual run takes its `tier` input; a push or
pull request classifies `git diff --name-only <base> HEAD` with the first matching class:

| Class | Paths | Tier |
|---|---|---|
| register | `docs/third-party-defects.md` (lane verdicts depend on it) | complete |
| documentation | `docs/**`, any `*.md`, editor, format and ignore files | preflight |
| build-system | `.github/**`, root `CMakeLists.txt`, `CMakePresets.json`, `cmake/**`, `scripts/**`, `tests/CMakeLists.txt`, `.clang-tidy` | complete |
| source | everything else (engine, tests, tools, assets, module `CMakeLists.txt`) | change |

The highest tier among the changed paths wins. A base that is all zeros (new branch, force push), unreachable, or
an empty diff resolves to the complete tier: a changed build system qualifies itself with conservative scope, which
is also why the push introducing this contract resolves to the full matrix. The resolver writes `tier`, `expected`
(the jobs that must succeed) and one JSON preset list per job to the job outputs and a table to the step summary.

Heavy jobs `needs: preflight` only, so the repository checks run beside them; each takes
`strategy.matrix.preset` from `fromJSON(needs.preflight.outputs.presets_<job>)` and is gated with
`if: ... != '[]'` because an empty matrix fails evaluation instead of skipping.

## Required result

`required` needs every job, runs with `if: always()` and evaluates `ci-tier.py conclude` on the `needs` context:
preflight must have succeeded, every expected job must report `success`, and a job the tier did not expect must be
`skipped` (or `success`). A skipped expected job, a failed or cancelled job of any kind, or a preflight that
published no expectation fails it. A docs-only push therefore ends with a meaningful pass after the repository
checks, and a failed preflight cannot be hidden by the jobs it prevented. Branch protection should require only
"Required result".

## Cancellation and scheduling

The concurrency group is `pr-<ref>` for pull requests with `cancel-in-progress`, so superseded feedback runs stop;
every other run is keyed by its run id and is never cancelled or serialized: a push run on `main` is the
qualification evidence for that revision and the nightly (03:00 UTC) or manual complete run must not be starved by
frequent pushes. `workflow_dispatch` exposes the `tier` input (`change` or `complete`).

## Evidence

Every lane records the adapter census, configures, builds, writes the CTest inventory
(`ci-evidence.py inventory`, `ctest --show-only=json-v1`), runs with `--output-junit`, and ends in the two-sided
[register gate](../third-party-defects.md) on every preset (lanes without entries require zero failures and
propagate CTest's exit). `ci-evidence.py bundle` then always runs: it writes `conclusion.json` (schema
`cerid-ci-evidence/1`: revision and tree hash, run identity, preset, generator, build type, compiler identity and
version, CMake version, the Cerid switches from the cache, adapters, inventory counts with labels and resource
locks, executed/passed/failed/skipped counts and duration, failing tests with their output tails, the slowest
tests, the build board of the lane from its Ninja log (edges, span, edge sum, effective parallelism, slowest edges)
and the sccache statistics of a cached lane ([build performance](build-performance.md)), the job status when the
bundle ran and what evidence was missing), copies the JUnit and inventory beside it, and appends a table to the job
summary. The artifact `evidence-<preset>` carries the bundle and the CTest logs. The
bundle never changes a lane's exit code.

The native lane (`windows-native`, VS 2026 image) configures `win-vs`, builds the Debug sandbox profile exactly as
the IDE build preset does and runs the repository guards under the multi-config CTest, including the two
native-only fixtures no Ninja lane can register; engine suites stay with the Ninja lanes.

## Parallelism and shards

CTest still runs serially. The research orders measured budgets before parallelism and no hosted durations existed
before this contract; every lane now records per-test durations in its JUnit. Bounded `ctest -j` for independent
CPU suites, labels and duration-derived shards are the declared follow-up on
[REPO.DEV.5](../ROADMAP.md#slice-repo.dev.5), gated on the first complete-tier durations; the `crd_gpu_device`
resource lock stays as it is.

## Not in scope

Immutable action hashes, cache trust and token policy belong to REPO.DEV.6; compiler caching to
[build performance](build-performance.md) (REPO.DEV.7); affected
target selection inside a lane to the developer-workflow rows.
