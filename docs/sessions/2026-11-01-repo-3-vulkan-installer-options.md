# REPO.3: a hosted experiment for the Vulkan SDK installer's options

<!-- doc-role: historical -->
> Dated evidence. Live owner: [REPO.3](../ROADMAP.md#slice-repo.3). Contract:
> [test instruments](../design/test-instruments.md#host-separation-diag10a). Rules: [AGENTS](../../AGENTS.md).

Track B, 2026-11-01, on `cerid-track-b` at 7374f767 plus this batch's edits. The track machine stayed stopped: the
batch is a workflow, a Python driver and its unit test.

## CI first

- **No run covers 21ee3699 or 7374f767 yet.** GitHub's `main` was dd355099 at 10:30 UTC; the mirror then caught up and
  the push run 41297912276 at 7374f767 is pending. The `workflow_dispatch` run 41295647725 and the scheduled
  41295924613 (both 43963cb2, the first whose win-clang-cl can run MCP-ADAPT.0's tests) have only their preflight
  queued. The push runs at 43963cb2, 408515f1 and da6692d6 were replaced while pending.
- **The dispatch run 41274380605 (de5e10b0)** finished linux-gcc-debug (124764040384, passed); win-relwithdebinfo
  (124764040641) is still running. Its other failures were read in the
  [checkout-paths batch](2026-11-01-repo-3-checkout-paths.md#ci-first), none this track's.
- [MCP-ADAPT.0](../ROADMAP.md#slice-mcp-adapt.0) still waits for a clang-cl lane and [REPO.3d](../ROADMAP.md#slice-repo.3d)
  for [DIAG](../ROADMAP.md#slice-diag); neither row changes.

## The question

The [previous batch](2026-11-01-repo-3d-scoped-checks.md#the-lunarg-installers-qt-line) found that the cold
`win-debug-scalar` lane's run of the pinned LunarG 1.4.341.1 installer prints Qt's `QFont::setPixelSize: Pixel size
<= 0 (-1)` warning and fetches update metadata from LunarG's remote repository, a network read outside the pin. The
Qt Installer Framework's command line has no offline switch; its candidates are `--platform <plugin>` (the warning
comes from Qt's font code) and `--set-temp-repository <URI>`, which makes the given repositories the only ones the
fetch uses. Whether either removes a finding, and whether the installed tree stays the same, can only be measured
on Windows; this track builds nothing on Windows, so the measurement is a hosted experiment.

## The experiment

`host-check.yml` gains the `vulkan-sdk-installer` legs (dispatch only, like every leg of that workflow; also run by
`all`). Nine arms each take a fresh `windows-2025` machine and run
[vulkan-sdk-installer-check.py](../../scripts/vulkan-sdk-installer-check.py) `run`, which fetches the pinned
installer through `pins.fetch` (SHA-256 checked), runs it with `install-vulkan-sdk.py`'s own `INSTALL_ARGUMENTS`
plus the arm's options, and writes `log.txt`, `manifest.json` (path, size and SHA-256 of every file under the pinned
install root) and `result.json` (exit, every `QFont::setPixelSize` line, every remote-fetch line, the hosts the
output names, the framework's metadata cache under `%LOCALAPPDATA%\cache\qt-installer-framework` before and after,
the missing layout files and the tree's digest). The arms:

| Arm | Options added |
|---|---|
| `baseline`, `baseline-repeat` | none (production) |
| `verbose` | `--verbose` (names what the fetch reads) |
| `platform-minimal`, `platform-offscreen` | `--platform minimal` / `offscreen` |
| `temp-repository-empty` | `--set-temp-repository` naming an empty local directory |
| `temp-repository-updates` | `--set-temp-repository` naming a local repository whose `Updates.xml` lists no package |
| `minimal-updates`, `offscreen-updates` | each platform with the latter repository |

Every arm installs to the pinned root on its own machine, so a file that records the install path compares alike.
The judge job (`ubuntu-24.04`) collects the arms with `actions/download-artifact` v8.0.2 (pinned in
`cmake/pins.json` by commit, Node 24; the licence manifest regenerated) and runs `judge`: the paths on which the two
baseline runs disagree are left out of every comparison, and each arm is `removes both, same tree`,
`findings remain`, `tree differs` or `install failed`, with the added, removed and changed paths listed. It fails
only when a baseline left no complete install. The verdict decides the change to `install-vulkan-sdk.py`: an arm
that removes both findings with the same tree becomes its arguments; if none does, the record says which finding
each option touched and the next options to measure.

`test_vulkan_sdk_installer_arms_record_their_evidence_and_are_judged_against_two_baselines` drives `run` with a fake
installer through six arms (the exact command line, the local repositories, the findings, the cache, the manifest,
an unknown arm and a machine that already has the SDK refused) and `judge` through every verdict, the baseline
disagreement and a missing baseline; it also holds the workflow's matrix to the script's arms. Teeth: judging
without the baseline calibration fails it (`[] != ['InstallationLog.txt']`).

## Evidence

- Local: `test-repository-tools.py`, 295 of 296 passing; the one failure, `test_generated_target_folders`, needs
  Ninja on the path, which this host's agent shell does not have (it fails the same without this batch's edits; the
  hosted repository checks have Ninja). `test_registry_workflow_build_and_helpers_agree` now looks for every pinned
  action in all workflow files, since `download-artifact` serves only the host check. `check-pins.py`,
  `check-ci-tiers.py`, `check-master-plan.py`, `check-repository.py`, `check-allman-braces.py`, `check-generated.py`
  (the licence manifest refreshed) pass.
- Hosted: the dispatch is recorded on [REPO.3](../ROADMAP.md#slice-repo.3) once GitHub's mirror holds this commit.

## The measuring PC's CUDA run

REPO.3's other remaining item, `bash scripts/run_bench_gpu_cuda.sh` under WSL2 on the measuring PC's NVIDIA GPU,
needs the development host, where this track may not build or run; the track machine has no GPU. It is named in the
row for the track that builds on that PC (or the user) to run.
