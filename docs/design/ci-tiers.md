# CI tiers, required result and exact-revision evidence

<!-- doc-role: reference -->
> Contract for REPO.DEV.5. Status lives only in [ROADMAP](../ROADMAP.md#slice-repo.dev.5); the accepted research is
> [CI design](../research/2026-09-12-large-cpp-development-and-ci.md#ci-design); the workflow is
> [ci.yml](../../.github/workflows/ci.yml) and the mapping [ci-tiers.json](../../.github/ci-tiers.json).

Purpose: one coverage contract in three tiers, every supported preset owned by a lane, one stable required status
that cannot be satisfied by a skipped job, cancellation that cannot destroy qualification evidence, and structured
evidence for the exact revision every lane built. The mapping and the resolver are the coverage contract of both the
hosted workflow and the Cerid orchestrator of the [CI platform](ci-platform.md) (ADR-0135), which reads them rather
than copying them.

## Tiers and the mapping

[ci-tiers.json](../../.github/ci-tiers.json) gives every visible configure preset a tier and a job:

| Tier | Runs | Presets |
|---|---|---|
| preflight | every run | none; the `preflight` and `repository` jobs (workflow syntax, mapping, hygiene, documentation, tooling fixtures, generated artifacts) |
| change | pushes and pull requests that touch sources | `win-debug`, `win-release`, `win-asan`, `win-tidy`, `win-clang-cl`, `win-shipping`, `win-clang-cl-shipping`, `linux-gcc-debug`, `linux-gcc-release`, `linux-gcc-relwithdebinfo`, `linux-gcc-asan`, `linux-gcc-shipping`, the [ARM64 lanes](#arm64-lanes) `linux-gcc-arm64-debug`, `linux-gcc-arm64-asan`, `linux-clang-arm64-debug`, `linux-clang-arm64-asan`, `win-arm64-debug` |
| complete | nightly, manual, build-system changes | the change presets plus `win-relwithdebinfo`, `win-debug-scalar`, `win-shipping-profile`, `linux-gcc-debug-scalar`, the native `win-vs` solution, the public-check presets `win-public-checks`, `linux-gcc-public-checks` ([public consumption](public-consumption.md)) and the clang ThreadSanitizer preset `linux-clang-tsan` ([test instruments](test-instruments.md)) |
| diagnostic | never in a tier | `win-tidy-local`, `linux-clang-fuzz` ([test instruments](test-instruments.md)), `linux-clang-coverage` ([DIAG.10b](runtime-diagnostics.md#diag-10b)) |

The change tier is the lane set the repository ran on every push before this contract, so a source change keeps
every obligation it had; only a documentation-only push stops after preflight, by design. The complete tier adds the
five presets that had never run hosted and, since REPO.DEV.8, the two public-check presets. The first complete run is
their first hosted exercise and may surface real defects: that is the tier's purpose, not a regression of the push
signal. The diagnostic presets stay out of every tier; one runs hosted only in a dispatch-only experiment (below).
clang++ on Linux became a hosted compiler with the `linux-clang-tsan` lane
(user decision 2026-10-05): the tree builds under its `-Werror` set once clang's `-Wconversion` drops sign conversions,
which GCC's never included ([inventory and triage](../sessions/2026-10-05-clang-werror-and-tsan-inventory.md)). The
lane runs the whole suite under ThreadSanitizer, four tests at a time, with
[third-party suppressions](../../tests/support/diag/tsan-suppressions.txt) for uninstrumented libraries only.
The same matrix entry runs the DIAG.4b GenMC weak-memory model (user decision 2026-10-10): `install-genmc.py` builds
the pinned GenMC against the runner's apt LLVM 20 (cached per pin and LLVM version) and `check-genmc-models.py`
requires the repaired model to verify and the seeded broken handshake to be found. It is a *preset step*, a
non-CMake check that rides on one preset and inherits its tier, declared under `preset_steps` in the mapping. A
preset step may instead list several presets (`presets`, no tier of its own): each leg then runs in its own preset's
tier. The DIAG.10b seeded detector failures are one (2026-10-11): the `Seeded detector failures` step of the
`windows` and `linux-gcc` jobs runs [diag-seeded-faults.py](../../scripts/diag-seeded-faults.py) on win-debug,
win-asan, linux-gcc-debug and linux-gcc-asan in the change tier and on linux-clang-tsan in the complete tier, after
the lane's build and tests pass ([test instruments](test-instruments.md#seeded-detector-failures-diag10b)). Each seed
rebuilds only the targets holding its detecting tests, so a lane adds a few incremental builds: 16 s on the local
win-debug tree and 92 s on win-asan. `Test at the sse2 level` is another (2026-10-28): general code builds at the SSE2
baseline on every lane (ADR-0134 §4), so the former `win-debug-sse2` and `linux-gcc-debug-sse2` builds became a
second CTest pass of the win-debug and linux-gcc-debug binaries under the `<preset>-at-sse2` test presets
(`CRD_CPU_LEVEL=sse2`, which on MSVC also lowers the CRT's own `__isa_available` dispatch and turns the UCRT's
FMA3 libm off), checked against the register as lane `<preset>-at-sse2`, after the hidden `[cpu-level-applied]`
case proves the level was applied. The pass leaves out what cannot change with the level: the
`<suite>-kernels-at-<level>: <case>` arms, which set their own level and ran in the lane's pass, and the `crd-check-*` and
`crd-no-*` source and build-tree guards.

### ARM64 lanes

The `linux-arm64` job (ubuntu-24.04-arm: GCC and Clang, debug and ASan with UBSan) and the `windows-arm64` job
(windows-11-arm, MSVC ARM64) run the suite on NEON hardware: every kernel family builds portable there and the level
probe's arms run portable and neon (SIMD-DISPATCH, [session](../sessions/2026-10-28-simd-dispatch-neon-parity.md)).
They use the images' toolchains (Ubuntu's ninja-build and gdb; the pinned x64 Ninja under emulation and
`msvc-dev-env.py --arch arm64` on Windows) and no compiler cache. crd-jobs has no ARM64 fiber context before
[JOBS-PORT.2](../ROADMAP.md#slice-jobs-port.2), so the presets set `CRD_MODULES_WITHOUT=jobs`
([module registry](module-registry.md#selection)); they widen by themselves once it builds there.
`linux-gcc-arm64-asan` compiles without use-after-return detection, which GCC 13 makes about 30 times dearer on
AArch64 than Clang ([TP-6](../third-party-defects.md#tp-6)); `linux-clang-arm64-asan` keeps it.

[check-ci-tiers.py](../../scripts/check-ci-tiers.py) is the `crd-ci-tiers` CTest, a preflight step, a repository
step and a tooling test. It fails when a visible preset has no owner or an entry names no preset, when an owning job
does not need `preflight`, is not gated on its non-empty list, does not take its matrix from the resolver's output,
is missing from the required aggregate or runs on another runner than declared, when a preset step is missing from
a listed preset's job, is not gated on that preset, is gated on a preset the mapping does not list, rides on a
diagnostic preset or declares another tier, when a diagnostic preset is invoked, when the class table lacks a
catch-all, when a trigger is missing, or when cancellation is not limited to pull requests.

Every other workflow file is an *experiment*: `check-ci-tiers.py` requires it to be triggered only by
`workflow_dispatch` and never to cancel a run in progress, so it cannot join a tier or the required result, and
`check-pins.py` holds it to the same pinned actions, runners and credentials as `ci.yml`. Only an experiment may run a
diagnostic preset on a hosted machine. The one experiment is the DIAG.10a
[host check](test-instruments.md#host-separation-diag10a) (`host-check.yml`, `linux-clang-fuzz`).

## Resolution

[ci-tier.py resolve](../../scripts/ci-tier.py) runs once in the non-matrix `preflight` job (a matrix job's outputs
are those of its last leg). Scheduled runs take the complete tier; a manual run takes its `tier` input; a push or
pull request classifies `git diff --name-only <base> HEAD` with the first matching class:

| Class | Paths | Tier |
|---|---|---|
| register | `docs/third-party-defects.md` (lane verdicts depend on it) | complete |
| documentation | `docs/**`, any `*.md`, editor, format and ignore files | preflight |
| build-system | `.github/**`, `ci/**` (the [CI platform](ci-platform.md), since 2026-10-30), root `CMakeLists.txt`, `CMakePresets.json`, `cmake/**`, `scripts/**`, `tests/CMakeLists.txt`, `.clang-tidy` | complete |
| source | everything else (engine, tests, tools, assets, module `CMakeLists.txt`) | change |

The highest tier among the changed paths wins. A base that is all zeros (new branch, force push), unreachable, or
an empty diff resolves to the complete tier: a changed build system qualifies itself with conservative scope, which
is also why the push introducing this contract resolves to the full matrix.

A push's base is not its own `before` (2026-10-19,
[session](../sessions/2026-10-19-diag-11b-malloc-allocator-bytes.md#the-tier-base-a-replaced-run-lost-its-changes)):
the push group replaces a pending run with the newer one, so the replaced push was never classified, and
`before..HEAD` left its changes out of the decision. `ci-tier.py base` (in the preflight, which reads the run list
with `actions: read`) takes the head of the newest earlier run of the workflow on the branch whose `Preflight (tier
resolution)` job succeeded, any event, ignoring runs created after this one; no such run, or no answer from the API,
leaves the base empty and the run takes the complete tier. A pull request still compares with its base branch tip. The resolver writes `tier`, `expected`
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

Every lane records the adapter census, configures, builds, writes the CTest inventory (`ci-evidence.py inventory`,
`ctest --show-only=json-v1`), runs with `--output-junit` under `ci-evidence.py watch` (which samples the host's
available and committed memory every 5 s and rewrites `Testing/Temporary/host-test.json` at each sample, so a job
stopped at its limit keeps it), and ends in the two-sided [register gate](../third-party-defects.md) on every preset
(lanes without entries require zero failures and propagate CTest's exit). `ci-evidence.py bundle` then always runs: it
writes `conclusion.json` (schema `cerid-ci-evidence/1`: revision and tree hash, run identity, preset, generator, build
type, compiler identity and version, CMake version, the Cerid switches from the cache, the diagnostic modes the
binaries are built with (`modes`: ASan, UBSan, TSan, libFuzzer, coverage, asserts, profiling, clang-tidy), adapters,
inventory counts with labels and resource locks, executed/passed/failed/skipped counts and duration, overall and per
label (`results.labels`: `diag-fatal` for the contract-fatal specimens, `diag-race` for the controlled-interleaving
cases, `diag-overhead` for the overhead modes and `diag-workflow` for the native diagnostic workflow, all four also
under `diag`, and `fuzz` for the corpus replays), failing tests with their output tails, the slowest tests, the build
board of the lane from its Ninja log (edges, span, edge sum, effective parallelism, slowest edges) and the sccache
statistics of a cached lane ([build performance](build-performance.md)), the host (`host`: CPU, logical CPUs and
memory, and from the Test step its wall time, least available memory and commit peak), the job status when the bundle
ran and what evidence was missing), copies the JUnit and inventory beside it, and appends a table to the job summary.
The artifact `evidence-<preset>` carries the bundle and the CTest logs. The bundle never changes a lane's exit code.

The native lane (`windows-native`, VS 2026 image) configures `win-vs`, builds the Debug sandbox profile exactly as
the IDE build preset does and runs the repository guards under the multi-config CTest, including the two
native-only fixtures no Ninja lane can register; engine suites stay with the Ninja lanes.

## Parallelism and shards

A lane runs its suite serially until its measured serial sum no longer fits GitHub's 6 h job limit with an hour
to spare after setup, build and its later steps; then its test preset gets `execution.jobs`, sized from the floor
max(longest case, the `crd_gpu_device` chain, the serial guard, serial sum / jobs) and at most the runner's logical
CPUs. Every case is its own process, the `crd_gpu_device` lock and `RUN_SERIAL` stay as they are, so the
pass, fail and skip set and the JUnit names do not depend on the bound. Shards would split one lane across runners;
no lane needs that yet.

| Lane | Bound | Why |
|---|---|---|
| `linux-clang-tsan` | 4 | TSan's per-case cost |
| `win-asan` | 3 of 4 CPUs | e496d16c: 8,106 cases, 20,031 s serial, 7 min under the cap; floor 6,677 s; a core for the pools ([session](../sessions/2026-10-29-simd-dispatch-win-asan-bound.md)) |
| `linux-gcc-asan` | 3 of 4 CPUs | c2645814: stopped at the cap after 6,811 of 8,766 cases in 20,331 s serial (about 26,000 s for all); a core for the pools ([session](../sessions/2026-11-01-hw-resilience-2-ci-hosted-identity.md)) |

The other lanes stay serial; the host record in each bundle shows when one comes close.

## Not in scope

Immutable action hashes, cache trust and token policy belong to REPO.DEV.6; compiler caching to
[build performance](build-performance.md) (REPO.DEV.7); affected
target selection inside a lane to the developer-workflow rows. SBOMs, the double-build reproducibility lane, SLSA
provenance and signing belong to [SUPPLY.1](../ROADMAP.md#slice-supply.1) to [SUPPLY.4](../ROADMAP.md#slice-supply.4).
