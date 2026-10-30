# ADR-0135 — The Cerid CI platform: Buildkite, one agent everywhere, a device registry and a 50 USD cap

<!-- doc-role: decision -->
> Direction and acceptance contract. Live work: [ROADMAP](../ROADMAP.md#slice-ci-0) (CI-0 to CI-4).

**Status:** accepted direction. The user decided on 2026-10-30 to leave GitHub's CI for a gold-standard, platform-
independent testing workflow on Buildkite, cloud now and owned hardware later, at most 50 USD a month, easy to run
([order, verbatim](../sessions/2026-10-30-ci-platform.md#user-order)), and the same day that console platforms must
plug in later without redesign. The implementation contract and the CI-4 parity table are in
[the CI platform design](../design/ci-platform.md); the operator's steps in
[the setup recipe](../recipes/ci-platform-setup.md).

## §1 Context

The hosted GitHub Actions workflow ([ci.yml](../../.github/workflows/ci.yml)) qualifies ten jobs on six runner images
under REPO.DEV.5's tier contract ([CI tiers](../design/ci-tiers.md)). It cannot reach a real GPU, a phone, a Mac,
a console-class constraint or owned hardware, and its cost and queueing are outside Cerid's control. The user wants
the same quality on every platform and hardware the product targets, with two interchangeable ways to supply a
machine: rented from a cloud or owned.

## §2 Orchestration: Buildkite, one agent binary on every machine

Buildkite's hosted control plane schedules, logs and annotates builds; every machine runs the same pinned
`buildkite-agent` (cmake/pins.json `buildkite-agent-*`, 4.6.0 of 2026-10-16: the newest release at least two weeks
old on 2026-10-30, when 4.9.0 appeared). The agent is v4: deprecated flags fail loudly, `pipeline upload` rejects
secrets by default and Git commit verification defaults to strict branch membership (Buildkite's v3-to-v4 guide, read
2026-10-30). The Free plan (read 2026-10-30) allows 5 users, 10 concurrent self-hosted jobs and 30 days of build
retention; Cerid keeps its own longer history in the result store (§5). GitHub becomes an optional Git remote.

Cloud machines are one-job machines: the launcher uses Buildkite's **Stacks API** (register a custom stack, list
scheduled jobs, batch-reserve, issue a job acquisition token, finish a job it cannot run) and the machine starts the
agent with `BUILDKITE_AGENT_ACQUIRE_JOB` and that short-lived token, so no long-lived token ever reaches a runner.
Acquired jobs ignore agent tags, so the launcher routes each job by its `device=` rule to its registry cell.
Availability of the Stacks API on the organization's plan is verified at setup (recipe step 2); `ci/ops.py status`
reports a failed stack registration.

## §3 Compute: a separate Google Cloud project with its own billing

A new project (suggested id `cerid-ci`) with billing, a budget of 50 USD a month and the automated stop of §6. The
existing Firebase project (the `FTL_GCP_PROJECT` repository variable, Spark plan, Workload Identity Federation for
GitHub) stays free and separate; the CI project's runner service account gets only Firebase Test Lab roles there.
Compute Engine supplies Linux x86-64 (Intel Ice Lake, AMD Zen 4, AMD Zen 2 for the console profile), Linux ARM64
(T2A Ampere, C4A Axion), Windows Server 2025 (licence billed per second), NVIDIA L4 and T4 spot GPUs and nested
virtualization for the Android emulator. Spot VMs exist only while a job runs: idle cost is zero. AMD Instinct
hardware (AMD Developer Cloud credit), an hourly Mac, an Intel GPU host and a Windows ARM64 host are SSH hosts used
only when their secrets exist; Google Cloud has no Windows on ARM.

## §4 Exactly four executors

`ci/executors/`: `gce_vm` (a spot VM per job, deleted after it: self-delete, the launcher's reaper and
`maxRunDuration` with the DELETE action bound it three times), `owned_agent` (an owned machine on the owned queue; no
launch), `firebase_test_lab` (a one-job agent process on the launcher that drives FTL's devices) and `ssh_host` (the
agent started on an existing host over SSH with strict host-key checking, the token on standard input). Adding owned
hardware is a registry entry plus the agent configuration `ci/ops.py add-device` prints; no pipeline change.

## §5 Registry, plan, affected work and the content-addressed cache

`ci/devices.json` is the single registry: one cell per device with kind, OS, CPU model and levels, GPU and APIs,
screen where relevant, executor, tier (A hosted or emulated, B free real devices, C paid or credit hardware), cost
class and the accounts it needs. `.github/ci-tiers.json` stays the coverage contract and `scripts/ci-tier.py` its
resolver; the registry hosts every ci-tiers job on exactly one cell (validated). `ci/pipeline.py` plans a build:

1. the tier from ci-tier.py (unchanged rules: documentation, change, complete);
2. on the change tier, the affected modules: the changed paths' owners in the `crd_module()` registry of the root
   CMakeLists.txt and their reverse closure over DEPENDS and TEST_DEPENDS; any build-system, asset, root or unowned
   path builds everything. A narrowed lane configures `CRD_MODULES` (REPO.DEV.4's own selection, whose configure-time
   verification refuses an undeclared link). Lanes carrying the seeded detector failures or the GenMC model are never
   narrowed, so a source push keeps every hosted obligation;
3. an input key per step: SHA-256 over the Git blob ids the step consumes (never documentation for a build step; for a
   narrowed lane only its module closure plus every path no module owns), the device cell and its baked image. A key
   that passed before is not rerun: the step is emitted as skipped `cached: passed in build N`, linked to that run;
4. inside a lane, a per-test key (the test executable's bytes, its arguments and properties, the device and the
   runtime inputs no binary carries) excludes unchanged passed tests on the change tier; the complete tier reruns
   every test of a changed lane, and `CERID_CI_FRESH` or `--fresh` ignores both caches;
5. the budget (§6), then the Buildkite pipeline (JSON, which `pipeline upload` accepts), the required result
   (ci-tier.py's conclusion rule: a cached step passed, a budget step is incomplete) and the dashboard.

The result store is a Cloud Storage bucket in the CI project (a local directory on an owned machine): results, cache
entries, durations, the spend ledger, baked image names and quota counters.

## §6 Budget: 50 USD a month, enforced twice

The pipeline keeps its own ledger (one entry per launched machine, from the dispatcher's real start and end times
and the cell's hourly price) and marks paid steps that would pass the cap as `budget` before they are uploaded,
lowest priority first (repository checks, then change-tier lanes, complete-tier lanes, then tier C device cells). The
dispatcher refuses a paid launch whose worst case (price times the cell's maximum run time) would pass the cap, and
finishes the job with exit status 75 and a `budget:` detail. The Cloud Billing budget of the project publishes its
notifications to a Pub/Sub topic; the dispatcher pulls them and, once the reported cost reaches the budget, stops
every paid launch. A budget cell is never shown as a pass. Regional CPU and GPU quotas bound concurrency.

## §7 The QUAL dashboard

`ci/dashboard/generate.py` renders devices x features from the result store, the plan and the registry: pass, fail,
cached, waiting (planned cell, missing account, engine option or roadmap row not delivered) and budget, with logs,
per-device trends, regressions, the month's spend and the roadmap clauses without evidence. It is a static page; the
recommended host is Cloudflare Pages behind Cloudflare Access (free for a small team, private, no server to run),
deployed by the wrangler version locked with every transitive integrity hash in `ci/dashboard/package-lock.json` on
the pinned Node.js 24.22.0.

## §8 Console platform packs: the plug-in contract

Consoles plug in later without redesign. A **platform pack** (Xbox, PlayStation, Nintendo) is a separate private
repository, or a private submodule if the operator adds one, under the platform holder's agreement and with its own
access control. Its NDA SDK code, tools, headers, certification checklists and device commands are **never committed
to this repository**. A pack plugs in through the same interfaces as everything else:

- a `devices.json` with the registry's schema, loaded from `CERID_PLATFORM_PACKS` or `external/platform-packs/<name>`
  (an ignored directory); a pack adds cells and features, never replaces a public entry, and a pack inside the tracked
  tree is refused;
- one of the four executors, normally `owned_agent` (the dev kit on an owned host) and never a fifth kind;
- its own lane commands, run by the pack's scripts on the pack's machine.

Rows [6.0](../ROADMAP.md#slice-6.0), [6.0-HW](../ROADMAP.md#slice-6.0-hw) and
[SHIP-CONSOLE](../ROADMAP.md#slice-ship-console) own the packs. The public registry carries the console cells now —
Xbox Series X|S retail Developer Mode and GDK development kit, PlayStation 5 development kit and test kit, Nintendo
Switch 2 development kit — as owned agents at tier C with the status "waiting on platform program" (ID@Xbox / Partner
Center, PlayStation Partners, the Nintendo Developer Portal). An active console cell without its pack is refused.

## §9 The console-profile lane, runnable now

Two cells on ordinary spot VMs (AMD EPYC Rome, Zen 2, 8 cores and 16 threads, the console CPU generation) run the
shipping build and its tests the way a console build behaves, with positive controls that fail the lane if a
constraint is not in force:

| Constraint | Windows (Xbox class, `win-shipping`) | Linux (PlayStation class, `linux-gcc-shipping`) | Today |
|---|---|---|---|
| Hard memory cap | Job Object job-wide limit, 13.5 GiB (8 GiB for Series S) | transient systemd service `MemoryMax`, 12.5 GiB, no swap | enforced |
| Fixed core and thread layout | Job Object affinity: 14 of 16 logical CPUs, the OS share reserved | `AllowedCPUs=0-13` | enforced |
| No writable executable memory | Arbitrary Code Guard (`Set-ProcessMitigation ... DynamicCode`) on every test executable | `MemoryDenyWriteExecute=yes` | enforced |
| Restricted filesystem view | test binaries and the build tree at Low integrity (no write elsewhere) | `ProtectSystem=strict`, `ProtectHome=read-only`, `PrivateTmp` | enforced |
| AOT only, no JIT | build option `CRD_CEIR_JIT=OFF` | same | **waiting: [CEIR-JIT.3](../ROADMAP.md#slice-ceir-jit.3) must provide it** |
| No dynamic code loading | build option `CRD_DYNAMIC_CODE=OFF` (no plugin or shader-DLL loading paths) | same | **waiting: [6.0](../ROADMAP.md#slice-6.0) must provide it** |
| Xbox DX12 feature set | runtime requirement `CRD_DX12_REQUIRED_FEATURES=xbox-series` (feature level 12_2 class: mesh shaders, DXR 1.1, VRS tier 2, sampler feedback) | not applicable (Vulkan stand-in) | **waiting: [GPU-TIER.0](../ROADMAP.md#slice-gpu-tier.0) and [DX12-AGILITY](../ROADMAP.md#slice-dx12-agility) must provide it** |

The engine options do not exist yet and this work does not modify engine code: the dashboard shows those cells as
waiting on the named rows, and the lane adds the options to its configure the day the rows deliver them. The fixed
memory and thread budgets themselves (an engine-side budget, not an OS limit) belong to row 6.0's console-class
profile.

## §10 Migration without a quality gap (CI-4)

GitHub Actions keeps running unchanged until CI-4 proves parity: every preset, test, guard, sanitizer, SDE arm,
ARM64 lane and the host-check experiments green on Buildkite for the same revisions. Every GitHub job and step maps to
a Buildkite step and lane phase in the generated parity table, which a test holds equal to the workflow. Two
GitHub-specific hooks need Buildkite equivalents before the switch: `check-master-plan.py` accepts only a GitHub
Actions run URL as Needs CI evidence, and `ci-tier.py base` reads GitHub's run list (`pipeline.py` already takes the
base from the result store). Disabling the GitHub workflow is the user's decision after the proof.

## §11 Accounts and secrets

Accounts, billing, tokens and credentials are user actions ([recipe](../recipes/ci-platform-setup.md)). Secrets live
in the CI project's Secret Manager or in files named by environment variables; code reads them by name and never
prints, logs or commits a value. `ci/ops.py accounts` lists which account-backed inputs exist, by name only.

## Consequences

- One registry, one planner and one agent cover cloud and owned machines; buying hardware is adding a cell.
- The coverage contract, the presets, the module graph, the helpers and the pins are reused, not copied.
- Paid evidence is bounded by money, not by hosted runner limits; a cell the budget stops stays visibly open.
- Until the user completes the recipe, every cell waits on its account and GitHub Actions remains the evidence.
