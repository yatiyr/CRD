# 2026-10-31 — The CI launcher goes live; the CI, docs and workspace branches merge into main

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

## Merges at the SIMD-DISPATCH batch boundary

The four parallel branches were merged in a separate worktree while the SELL board ran on `main` (a CMake input edited
during a board re-runs CMake), then fast-forwarded into `main` at the batch boundary:
`ci-orchestrator-elite`, `docs-health`, `ci-diag-visual` and `workspace-overlay` (its tip commit message reworded
before the merge). Seven entry documents conflicted between the docs-health rewrite and the overlay branch; the
docs-health text was kept, the overlay's facts (agent files in the NAS overlay, GitLab as the code host) were added,
and the four orientation files were trimmed back under their byte budgets. The new documentation check then found
that batch 126's SELL board was missing from the [benchmark index](../bench/README.md), which was added. Validators
and suites run on the merge: check-master-plan (with check-docs), check-repository, allman braces, generated, pins,
ci-tiers, diagnostic runbooks (static); test-check-docs 22, test-workspace 31, test-ci-platform 76,
test-ci-orchestrator 148, test-ci-orchestrator-e2e 3, test-repository-tools 161 (one case needs Ninja on PATH: it
passed with it). Also merged: [PLG-GUEST](../ROADMAP.md#slice-plg-guest) ([note](2026-10-31-plugin-guest-row.md)).

## Code host

GitLab is now `origin` (fetch and push); GitHub is the `github` remote, read-only here and reached by GitLab's push
mirror, and `gh` defaults to `yatiyr/CRD`. The workflow's pushes therefore reach GitHub, and its Actions runs, through
the mirror.

## The launcher (ADR-0135 Part B)

Done in the `cerid-ci` project, following [the setup recipe](../recipes/ci-platform-setup.md):

- B4: firewall rules `cerid-ci-iap` (35.235.240.0/20 to 22 and 8780) and `cerid-ci-agents`; the default rules opening
  SSH and RDP to the Internet were deleted.
- B6: `cerid-ci-launcher`, e2-micro, Ubuntu 24.04, internal 10.128.0.2; SSH works through IAP.
- B7: the GitLab deploy token from Secret Manager stored for `cerid-ci` through `git credential approve` (mode 0600),
  the repository cloned, `launcher.env` written, `main.py check` reports
  `configuration complete: 1 repositories, UI iap-tunnel`, both systemd units active, `ops.py health` exits 0.
- Build #1 (polling, `main` at `3db4573f`, before these merges): plan passed, dashboard passed, required failed with
  "preflight published no expectation", since that commit's pipeline predates the orchestrator's expectation contract.
- B8: `gce-linux-x64-intel` baked as `cerid-ci-ubuntu-2404-x64-202610310321` in about 35 minutes (it also serves
  amd-zen4, amd-gpu-offline, browsers and the PlayStation profile); `gce-linux-arm64-ampere` and `gce-windows-x64` were
  baking when this note was written.
- Quotas: `GPUS-ALL-REGIONS` is 0, which blocks every GPU device, and `CPUS-ALL-REGIONS` is 32. Increase requests to 4
  and 64 were filed through the Cloud Quotas API (preferences `cerid-ci-gpus` and `cerid-ci-cpus`), billed to the
  project because the gcloud client project has no request allowance.

## Defects found and fixed

- **The recipe did not match the platform as built:** it assumed a GitHub SSH deploy key and bucket names carrying
  the project ID. It now uses the GitLab deploy token over HTTPS and names the buckets in variables.
- **The recipe's launcher commands could not run:** they `cd` into `/var/lib/cerid-ci`, which is the CI user's home,
  mode 0700. A wrapper, `/usr/local/bin/cerid-ci` (run with `sudo`), now carries the environment and the directory,
  and the runbooks use it.
- **`workspace.py sync` pushed a refused branch:** when every branch was refused and every tag excluded, the push ran
  with no refspec, and Git then pushed the current branch under `push.default`. This bypassed the agent-file and
  secret checks; it happened once here.
  - What it pushed: `main`, whose only finding was a false positive: the recipe's printf template that assembled a
    credential URL from placeholders. No secret left the machine.
  - The fix: an empty refspec list now pushes nothing. A new test fails without the fix and passes with it. The
    recipe now writes the credential with `git credential approve`, so it no longer has that shape.
- **`ops.py device bake` reported an image that was never made:** it deleted the bake instance, and with it the
  auto-deleted disk, right after requesting the image, so `cerid-ci-ubuntu-2404-x64-202610310321` failed with
  RESOURCE_NOT_FOUND (audit log) while the bake printed the image name and recorded it as baked. The ARM64 bake
  succeeded only by timing. A failed provisioning script would also have been imaged after maxRunDuration stopped the
  instance. The bake now lives in `GceVm.bake`, with three changes:
  - it images only a disk whose provisioning printed `cerid-ci-bake-complete` on the serial console (and on Windows,
    no `cerid-ci-bake-failed`);
  - it waits for the image operation and checks its error;
  - it deletes the instance only after that.

  Tests cover the order and both refusals, and the order test fails on the old order.
- **The fixed bake still could not finish:** Compute Engine answers 400 `resourceNotReady` for a stopped instance's
  serial console, and the bake read it after the power-off. It now reads the console on every poll while the
  instance runs; the script pauses 90 seconds after the marker, longer than a poll. A failure stops the instance at
  once: the Windows bake had idled three hours after a startup script that failed in five seconds.
- **The Windows bake used winget:** winget is a per-user package, so the SYSTEM account that runs a startup script
  does not have it. The bake now installs Python, Git, CMake and the Build Tools from installers pinned in
  `cmake/pins.json` (`bake-*`). Each is checked by SHA-256 inside the machine, and the image is generalized with
  `GCESysprep`. The Linux bake also stopped piping Kitware's unpinned apt script into bash: it unpacks the pinned
  CMake 4.4.5 archive for its architecture instead.
- **Builds #1 to #15 planned no lanes:** an agent runs every job in a clean environment, so the plan step saw no
  account and every device read `waiting: account`. The coordinator now passes the launcher's non-secret settings to
  the plan step. `ops.py build start` made its coordinator without the git mirror (build #15 failed its checkout)
  and now builds it like the service does. The plan also keeps a device waiting until its image family is baked,
  instead of starting a machine from an empty family.
- **Axion machines could not start:** c4a takes no Persistent Disk (HTTP 400 in build #16). The boot disk type is
  now a device setting, `hyperdisk-balanced` on Axion, and the registry refuses a Hyperdisk-only family without it.
- **No job machine ever took its job:** the dispatcher names a machine `cerid-ci-<job>-a<n>` and the executor added
  `cerid-ci-` again, so the instance name in each machine's identity token matched no reservation (409). The
  machine then deleted itself, and the reaper recorded every one as a spot preemption. The prefix is now added
  once. A vanished machine is now called preempted only when a `compute.instances.preempted` operation names it,
  and launch_failed, with Compute Engine's message, when its insert operation failed (the bake checks its own
  insert too).
- **Every Linux lane failed at configure (build #19):** the agent gave the job the mirror's bearer header as an
  unscoped `http.extraHeader`, so the Vulkan-Headers clone from GitHub carried the job token and got 401. The
  header is now scoped to the mirror's URL. The image also lacked `pkg-config`, which glfw's configure needs.
- **Quotas and identity:** the region's `SSD_TOTAL_GB` is 500 GB, which holds three job machines; an increase to
  2048 GB was filed (`cerid-ci-ssd`), and bake machines take pd-standard disks again. The runner account's IAM
  condition had been stored as `resource.name.extract("C:/Program Files/Git/instances/{name}")`: Git Bash rewrote
  the `/instances/{name}` argument when the binding was created. Job machines therefore cannot delete themselves;
  they power off and the reaper deletes them. Repairing the binding is an IAM change left to the user. The recipe
  now exports `MSYS_NO_PATHCONV=1` and checks the stored expression.
- **`workspace.py sync` reported a fast-forward push as "up to date":** Git's porcelain flag for a fast-forward is a
  space, which the report did not count. Every ref line is now parsed, and a test asserts the fast-forward is reported.
- **`workspace.py` pushed every tag:** the local tag `backup-before-pdf-strip` holds a 218 MiB blob, over GitLab's
  100 MiB limit, so GitLab would refuse the whole push. `code_remote_exclude_tags` now keeps chosen tags off the code
  hosts; the NAS bundle keeps every tag. A test covers it.

- **Build #30, the first with lanes on every Linux and Windows image:** Linux release passed 8523 of 8528 tests;
  four Windows steps failed, each for a reason outside the engine.
  - Both clang-cl lanes: the Build Tools install under Program Files (x86), and the lane looked only under Program
    Files. The lane now looks under both, and first in the Visual Studio its MSVC step set up.
  - win-release: Python refused the Intel SDE download (`CERTIFICATE_VERIFY_FAILED`). Python trusts only roots
    already in the machine store, which Windows otherwise fills on demand. The Windows bake now installs Microsoft's
    trusted root list from Windows Update first.
  - repository-windows: two tests compared a path under `TEMP` with CMake's real path. A SYSTEM job's `TEMP` is
    `C:\Windows\TEMP`, but the directory is spelled `Temp`. The tests now compare real paths, and the agent gives
    each job its own temporary directory inside the build path (`CERID_CI_TEMP`), removed with the job. A third test
    left the Python path unquoted (`C:\Program Files\...`), and cmd exited 9009.
  - The lane's host-record sampler died on a Windows sharing violation, raised while a reader held the record. Its
    rename now retries like the store's. `ops.py` also leaked an open `os.devnull` per command.
- **Launches went past the vCPU quota after a cancel:** a canceled job released its concurrency slot at once, while
  Compute Engine was still deleting its machine. A machine record now stays open, and counts against the cloud
  machine limit, until the instance is gone; a deletion still pending after ten minutes is requested again.

- **Every push started a full build, and they all ran at once:** builds #30 to #41 shared the six machines, so none
  finished. Builds now follow `ci.yml`'s concurrency groups: one push build per branch runs, a newer push cancels the
  one still waiting, a pull request's newer build cancels the one in progress, and manual and scheduled builds run on
  their own.

## Workspace

`.agent/workspace.json` was created with `code_remotes: ["origin"]` and that tag excluded. The first sync from this
checkout adopted the NAS overlay history, committed 552 overlay files and wrote verified code (494.9 MiB) and overlay
(1.4 MiB) bundles; the second sync found nothing to write.
