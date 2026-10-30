# 2026-10-30 — The Cerid CI platform: user order, plan and first implementation (CI-0 to CI-4)

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-ci-0).

Rows: [CI-0](../ROADMAP.md#slice-ci-0) to [CI-4](../ROADMAP.md#slice-ci-4). Decision:
[ADR-0135](../decisions/0135-cerid-ci-platform.md). Contract and parity table: [CI platform](../design/ci-platform.md).
Operator steps: [setup recipe](../recipes/ci-platform-setup.md).

## The user's order (verbatim, 2026-10-30)

<a id="user-order"></a>

> "FORGET ABOUT FUCKING GITHUB! ... WE ARE GOING FOR A GOLD STANDARD TESTING AND CI WORKFLOW WHERE WE ENSURE THAT
> QUALITY OF THE PRODUCT IS THE SAME AND GOLD STANDARD IN EVERY POSSIBLE HARDWARE AND PLATFORM! ... DON'T USE GITHUB'S
> CI! ... WE MUST HAVE TWO OPTIONS, FROM CLOUD OR OWNING THE MACHINE, FOR NOW EVERYTHING IS ON THE CLOUD BUT LATER I
> WILL BUY THE REQUIRED HARDWARE! BUT OUR SYSTEM MUST WORK EVERYWHERE"

> "I am ok with monthly 50 dollars if we can test every device"

> "I want fully easy to manage system not a nightmare of intertwined wires"

> "buildkite is looking good and we can do it the best we can. I want you to start on this in parallel ... a new and a
> better and easier and more independent CI workflow. Give me the full plan and implement it"

The same day, as an addition:

> "ok when we make profit from our product and be ready we will just plug in the console parts so everything must be
> future proof. DO EVERYTHING THE PROPER WAY"

with the console scope relayed with it: console cells for Xbox Series X|S (retail Dev Mode and the GDK dev kit),
PlayStation 5 (dev kit, test kit) and Nintendo's current console (dev kit), each an owned agent at tier C waiting on
its platform program; a console-profile lane runnable now on ordinary cloud VMs; and the plug-in contract for console
platform packs whose NDA code never enters this repository.

The rows run **in parallel with SIMD-DISPATCH by explicit user order**; the serial workflow keeps SIMD-DISPATCH as the
current row and these rows touch none of its files. The user also pre-approved, on 2026-10-30, downloading every tool,
SDK and emulator for local or CI testing ("I give you permission to download every tool and sdk and emulators where we
can test in our machine or in CI"); for this work that covered the buildkite-agent archives, Node.js and the wrangler
lockfile metadata. Accounts, billing and credentials stay user actions.

## What was built

- `ci/devices.json`: 42 device cells across every requested platform, CPU level, GPU vendor and API, including the
  planned Tier A/B/C routes (chipStar with PoCL and Radeon GPU Analyzer for AMD, the macOS probe and `xcrun metal`,
  the Android emulator on nested virtualization, Firebase Test Lab phones and tablets, AMD Developer Cloud, the rented
  Mac), five console cells and two console-profile cells.
- `ci/pipeline.py`: the planner (ci-tier.py tiers, the `crd_module()` graph, content-addressed input keys, the result
  store, the budget, the Buildkite document), the lanes phase for phase against `ci.yml`, the console profile, the
  conclusion, the macOS probe and the Stacks API dispatcher.
- `ci/executors/`: `gce_vm`, `owned_agent`, `firebase_test_lab`, `ssh_host`.
- `ci/ops.py`: `status`, `add-device`, `budget`, `accounts`, `parity`, `bake`.
- `ci/dashboard/`: the QUAL dashboard generator, its fixture and the locked Cloudflare Pages publisher.
- `scripts/test-ci-platform.py`: 74 contract tests, a step of the hosted repository job. Its CTest registration
  (`add_test(NAME crd-ci-platform ...)` in `tests/CMakeLists.txt`, beside `crd-ci-tiers`) is left to the serial
  workflow, which owns `tests/` while SIMD-DISPATCH runs.
- Pins: buildkite-agent 4.6.0 for Linux x64 and ARM64, Windows x64 and macOS ARM64, and Node.js 24.22.0 for the
  publisher, each downloaded, hashed and recorded in `cmake/pins.json`.

## Bugs found and fixed on the way (each with a test that failed first)

- `scripts/check-repository.py` rejected a linked Git worktree: there `.git` is a file, and the root-artifact scan
  allowed it only as a directory ("Unexpected root artifact: .git"). The scan is now `root_artifact_errors()`, `.git`
  is an allowed root file and `ci/` an allowed root directory; `RepositoryFixes` in the new suite failed before.
- `scripts/ci-evidence.py` recorded only GitHub run identity (`RUN_KEYS`), so a bundle written on any other CI had
  an empty `run` record. The Buildkite build, job, step and the registry cell are now recorded; the test failed first.

## Verification

Python only, by the parallel-work limits (no C++ build, configure, CTest or benchmark ran):
`python scripts/test-ci-platform.py`, `python scripts/check-master-plan.py`, `python scripts/check-repository.py`,
`python scripts/check-allman-braces.py`, `python scripts/check-ci-tiers.py`, `python scripts/check-pins.py`,
`python scripts/gen_license_manifest.py --check`, `python scripts/check-generated.py` and the CMake-free classes of
`scripts/test-repository-tools.py` (MasterPlanSequence, CiTiers, CiEvidence, PinnedInputs, Provenance,
CiPresetSteps, GenmcModelLane: 43 tests); its CMake fixture classes were not run under the no-configure limit and
are left to the hosted repository job. The Windows Job Object test ran for real on this host (a 512 MiB allocation
under a 256 MiB cap failed, a 32 MiB one passed). No lane, executor or
dispatcher call reached a cloud: they are tested against fakes and their first live run waits on the user actions in
the recipe.

## Next

The user actions of the [setup recipe](../recipes/ci-platform-setup.md), in its order; then the first live build,
which turns CI-1 to CI-3 from Blocked into evidence, and the CI-4 parity proof. Disabling GitHub Actions needs the
user's go-ahead after that proof.
