# ADR-0135 — The Cerid CI platform: its own orchestrator, one agent everywhere, a device registry and a 50 USD cap

<!-- doc-role: decision -->
> Direction and acceptance contract. Live work: [ROADMAP](../ROADMAP.md#slice-ci-0) (CI-0 to CI-4).

**Status:** accepted direction. The user decided on 2026-10-30 to leave GitHub's CI for a gold-standard, platform-
independent testing workflow on Buildkite, cloud now and owned hardware later, at most 50 USD a month, easy to run
([order, verbatim](../sessions/2026-10-30-ci-platform.md#user-order)), and the same day that console platforms must
plug in later without redesign. The implementation contract and the CI-4 parity table are in
[the CI platform design](../design/ci-platform.md); the operator's steps in
[the setup recipe](../recipes/ci-platform-setup.md).

**Amended 2026-10-31 (§16):** company-level operation (notifications, flaky-test quarantine, first-bad-commit bisection, audit, reproducibility, costs, SLOs, runbooks), the web UI opened through an IAP TCP tunnel with polling as the trigger, and a designed web UI and CLI.

**Amended 2026-10-30 (§12 to §15):** by the user's order the same day, Cerid's own orchestrator (coordinator, agent,
durable state, web UI) replaces Buildkite; Buildkite is recorded as the rejected alternative (§13). Sections §2 and
the Buildkite wording of §4, §7, §10 and §11 are kept as the record of the first decision and are superseded where
§12 says so.

## §1 Context

The hosted GitHub Actions workflow ([ci.yml](../../.github/workflows/ci.yml)) qualifies ten jobs on six runner images
under REPO.DEV.5's tier contract ([CI tiers](../design/ci-tiers.md)). It cannot reach a real GPU, a phone, a Mac,
a console-class constraint or owned hardware, and its cost and queueing are outside Cerid's control. The user wants
the same quality on every platform and hardware the product targets, with two interchangeable ways to supply a
machine: rented from a cloud or owned.

## §2 Orchestration: Buildkite, one agent binary on every machine

*Superseded by §12 (2026-10-30): the Cerid orchestrator replaces Buildkite; the reasons are §13. Kept as the record of the first decision.*

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

## §12 Amendment (2026-10-30): Cerid's own orchestrator replaces Buildkite

The user ordered on 2026-10-30, the same day as the decision above: "can we make our own buildkite? I think we can
do it" and "start the paralel agent and make our own buildkite, our own orchestrator that will always work in the
quality of paid buildkite! I want this to be fully built as an orchestrator ci project!"
([session, verbatim](../sessions/2026-10-30-ci-orchestrator.md#user-order)). The earlier orders stand: CI independent
of GitHub's CI, cloud now and owned machines later with no redesign, a hard 50 USD monthly cap, every device and
platform, and a system that is easy to manage. This amendment replaces only Buildkite's role (§2, the agent half of
§4, the Pages hosting of §7 and the Buildkite wording of §10 and §11); the registry, the planner, the affected-only
plan, the content-addressed cache, the budget, the four executors, the console contract and `ci/ops.py` stay.

**The orchestrator.** One coordinator process (`ci/coordinator/`, Python standard library only) on the always-free
launcher VM or any owned machine:

1. *Triggers.* Signed webhooks from any git host (GitHub `X-Hub-Signature-256`, Gitea/Forgejo/Codeberg
   `X-Gitea-Signature`, GitLab's token, and a host-neutral `X-Cerid-Signature` over `<timestamp>.<body>` for any other
   host or relay), each verified on the raw body with a constant-time compare before parsing; remote polling
   (`git ls-remote`) as the fallback that needs no host configuration; manual builds (web UI, `ops.py build`) and
   cron schedules (the nightly complete run, the weekly fresh run) in the tracked `ci/coordinator.json`.
2. *Build model.* A build starts with one `plan` step on the launcher queue; `ci/pipeline.py upload` sends the
   planned steps (`cerid-ci-pipeline/1`), and any step may upload more. Steps carry keys, dependencies and `wait`
   barriers, agent queues and tags, priorities, concurrency groups with limits, per-step and per-build timeouts, soft
   failure, automatic retry on infrastructure loss (agent lost, VM preempted or lost, launch failure; bounded per
   step) and manual retry, `cached` and `budget` skips, artifacts, annotations, build meta-data, declared secrets.
   Cancellation reaches the running agent through its heartbeat and the VM through the reaper.
3. *Job lifecycle.* Exactly ten states: scheduled, assigned, running, passed, failed, canceled, timed_out, expired,
   budget, cached. Every allowed move is a row of one table; any other is refused, so a stale or racing request can
   never move a job backwards. Builds: scheduled, running, passed, failed, canceled.
4. *Durable state.* Everything lives in the result store under `orchestrator/` (one Cloud Storage bucket; a local
   directory on an owned machine), written with compare-and-swap: Cloud Storage object generations
   (`ifGenerationMatch`) and, for the filesystem backend with the same interface, a per-object generation under an
   operating-system lock. No database. A restarted coordinator resumes every build from the store; `ci/ops.py` acts
   on the same state beside it.
5. *Commit status* back to GitHub, Gitea or GitLab (optional, per repository), annotations and build/step meta-data.

**The agent** (`ci/agent/agent.py`, one standard-library file for Windows, Linux and macOS on x64 and ARM64): one-shot
on cloud VMs (take exactly one job, run it, upload, exit; the startup script deletes the VM) and long-running on
owned machines (poll its queue). It checks out the exact commit, runs the step in an allow-listed environment,
streams the log live in numbered chunks, uploads artifacts, heartbeats, stops the whole process tree on
cancellation, timeout or a preemption notice, and cleans up.

**Logs, artifacts, web UI.** Chunked live logs and per-step artifacts in the store with retention (90 and 30 days);
a plain HTML/JS UI with no build toolchain (`ci/web/`) served by the coordinator: builds, the build page with the
step graph, live log tail with ANSI colours, search and download, retry and cancel, agents and queues, spend against
the cap, and the QUAL dashboard (§7) as one page rendered live from the store. Cloudflare Pages is no longer needed
for it (an optional static copy remains possible).

**Operations.** `ci/ops.py` gains builds, build, retry, cancel, agents, queues, logs, agent-token, health, backup and
restore; the coordinator's watchdog reaps lost agents, stuck jobs, dead VMs and orphaned labelled instances; its own
log is structured JSON lines (journald); upgrades are a pinned checkout plus a restart, guarded by a state schema
version and an agent protocol version (§15).

## §13 Buildkite: the recorded, rejected alternative

Buildkite (§2) was the accepted direction for a few hours and is now rejected, for three reasons:

1. **The Stacks API was uncertain on the Free plan.** One-job cloud machines, the base of the zero-idle-cost design
   and of the 50 USD cap, depended on the Stacks API (register a stack, reserve, issue job acquisition tokens), and
   Buildkite's pricing page did not confirm it for the Free plan; the recipe could only check it after sign-up and
   the dispatcher had no fallback without it.
2. **A third-party account and its limits.** A Buildkite organization, its tokens and its Free plan's limits (5
   users, 10 concurrent self-hosted jobs, 30 days of build retention) would sit between every commit and its
   evidence, and the plan's terms can change.
3. **Independence.** The user's goal is CI that depends on no hosted CI service; the orchestrator keeps scheduling,
   logs and history on infrastructure Cerid controls (its own project's bucket, or an owned machine's disk) and moves
   to owned hardware without changing any interface.

The buildkite-agent pins (4.6.0, four platforms) are removed from `cmake/pins.json`; nothing else of Buildkite
remains. The quality bar the user set (Buildkite's) is held by the same features: dynamic pipelines, dependencies,
queues and tags, priorities, concurrency, timeouts, soft failure, automatic and manual retry, cancellation, live
logs, artifacts, annotations, meta-data, commit status and an operator UI.

## §14 Security review (2026-10-30)

| Threat | Mitigations implemented |
|---|---|
| **Malicious or forged webhook** | Signature or token verified on the raw body before parsing, constant-time compare; no secret configured means 503, never "accept"; the generic signature's timestamp must be within five minutes; every delivery id is recorded create-only (a replay is acknowledged and ignored); the trigger key (repository, event, branch, commit, pull request) is create-only, so a duplicate or a webhook plus a poll builds once; only `push` and open pull-request events build; a webhook can only start a build of a configured repository at a commit, never choose commands (the commands come from the repository's own tracked pipeline). |
| **Untrusted code in a pull request** | A build's first step runs the commit's own `ci/pipeline.py` on the launcher, so a pull request from a fork (or any pull request through the generic host, which cannot tell) is never built unless the repository sets `build_forks` (the webhook is acknowledged as ignored and logged as `build_refused_fork`). A pull request from the repository itself, and a push to a branch outside `trusted_branches`, build untrusted: a step that declares secrets is refused before any secret is read. The agent comes from the coordinator's own checkout, not the pull request. |
| **Compromised runner** | Cloud VMs hold no credential: the agent proves the VM's Google identity (RS256 token verified against Google's keys, audience = the coordinator, e-mail = the runner service account, project and instance name = the machine reserved for that job) and receives one job token, issued once per reservation (a replayed identity token is refused). Job tokens are 256-bit random, stored only as SHA-256, bound to one job and one attempt, expiring with the job's timeout; a retried attempt invalidates the previous token. A job token can only write its own job's log, artifacts, annotations, meta-data and uploads while that job runs. The runner service account can delete only `cerid-ci-*` instances and write the buckets; it cannot read Secret Manager. Owned machines hold a registration token (a 0600 file) whose queue and tags the agent cannot override, exchanged for 12-hour sessions; `ops.py agent-token revoke` and `agents --stop` cut a machine off. |
| **Secret leakage into logs** | Secrets live only in Secret Manager (or a directory on an owned machine), are read by the coordinator only for a trusted build's step that declares them, travel once in the assignment over TLS, and are given to that step's environment only. The agent replaces every secret value in the live log, holding back the longest secret's length minus one byte so a value split across two writes is caught (tested for every split point); secret values never reach the state store (tested). |
| **Web UI exposure** | No anonymous access: Cloudflare Access (RS256) or Google IAP (ES256) assertions are verified by the coordinator itself, so a request that bypasses the proxy is refused; `local` mode only on a loopback listener; optional e-mail allow-list; state-changing calls need the `X-Cerid-Request` header and a same-origin `Origin`; strict Content-Security-Policy (no inline script), `nosniff`, `no-referrer`; every API value is escaped before rendering. |
| **Repository credentials on runners** | `checkout: mirror`: the coordinator keeps a bare mirror fetched with its own read-only deploy key and serves upload-pack only (`git http-backend`; pushes refused) to agents holding that build's job token, passed to git through the environment (never a command line, never `.git/config`). |
| **Supply chain of the agent** | The agent is one standard-library Python file in this repository; a launched machine downloads it from the coordinator and refuses it unless its SHA-256 matches the digest the coordinator computed from its own checkout and passed in the launch (instance metadata or the SSH bootstrap); owned machines run it from a checkout pinned to a reviewed revision. The orchestrator adds **no third-party dependency**: no pins, no licences, no wheels; JWT verification (RSASSA-PKCS1-v1_5 and ECDSA P-256 verification, public data only) is implemented and tested with generated keys. |
| **State tampering or loss** | Only the coordinator's and the runners' service accounts write the bucket (uniform access, public access prevention); compare-and-swap means no lost update between the coordinator, `ops.py` and a restart; `ops.py backup` writes a tar.gz with a SHA-256 manifest and `restore` verifies every object and keeps existing objects unless told to overwrite. |
| **Runaway spend** | Unchanged from §6, plus: a running machine's worst case counts against the cap before its ledger entry, every cloud step shares one concurrency group sized to the regional quota, a VM that never boots is replaced after 15 minutes, and the orphan sweep deletes any labelled instance no record knows. |

## §15 Dependencies, versions and upgrades

The coordinator, the agent and the web UI use the Python standard library and plain HTML/JS; no package manager,
pin or licence enters for them. Every record carries `schema` (1); a coordinator refuses a store written by a newer
schema and migrates an older one forward on start. Agents send their protocol (1); a mismatch is refused with HTTP
426 naming the coordinator's version. Upgrade: `ops.py backup`, check out the new revision on the launcher, restart
the coordinator unit, `ops.py health`; cloud agents always fetch the coordinator's agent, owned agents are updated
with their checkout. Rollback is the previous revision (the schema only moves forward when a migration is added).

## §16 Amendment (2026-10-31): company-level operation, IAP-tunnel access and polling

The user ordered on 2026-10-31: "yes switch the guide to polling and tailscale, do not miss anything we need proper
elite company level CI system and also design of the tool and all the UI UX must be gold standard"
([session, verbatim](../sessions/2026-10-31-ci-orchestrator-elite.md#user-order)). The same day, after asking "why
do we need tailscale?", the user decided that the default way to open the web UI is a Google IAP TCP tunnel, with
Tailscale an optional add-on (for a phone) and Cloudflare the documented alternative; polling stays the default
trigger; the user's own part of the setup is only creating the project, linking billing, setting the 50 USD budget
and saying go, the rest being an operator runbook run with the user's signed-in `gcloud`.

**Access (replaces the Cloudflare default of §12).**

- *Triggers:* polling is the default (`git ls-remote` of the watched branches every 60 s through the launcher's
  read-only deploy key: a push builds within a minute on average, about 1,440 small requests a day, and nothing has
  to reach the coordinator from outside). Webhooks (§12) remain an add-on for when a public route exists.
- *Web UI:* the coordinator listens on loopback and the launcher's internal address only. The firewall admits TCP
  8780 only from Google's IAP TCP-forwarding range 35.235.240.0/20 (and from the project's subnet for the job VMs'
  agent API). `python ci/ops.py ui` starts `gcloud compute start-iap-tunnel` under the user's own login to a local
  port, waits until the coordinator answers and opens the browser. Only principals holding the IAP-secured Tunnel
  User role reach the port; the coordinator then accepts UI requests only from loopback and that range (UI mode
  `iap-tunnel`), so a runner VM inside the network is refused. The TCP tunnel carries no per-user identity: the audit
  log names such actions "operator (IAP tunnel)" and the tunnel role is held by the user alone.
- *Tailscale add-on:* a second listener on the tailnet address with UI mode `tailscale`: the peer must be a tailnet
  address and its login, asked of the local tailscaled (LocalAPI `whois`, or `tailscale whois`), must be on
  `ui.allowed_logins`; tagged devices are refused; no header is trusted. Each listener has its own surfaces (`ui`,
  `agent`, `webhook`) and its own access mode.
- *Alternatives:* Cloudflare Tunnel and Access (`cloudflare-access`, needed for webhooks) and Google IAP behind an
  HTTPS load balancer (`iap`, about 18 USD a month).

**Company-level operation.**

| Capability | Implementation |
|---|---|
| Notifications | failure, recovery (green after red on the branch), budget at 80 % and 100 % and the GCP stop, flaky quarantine and its expiry, first-bad-commit results; email through the user's own SMTP provider or a self-hosted ntfy on the tailnet; each sent once per key; nothing exposed |
| Per-test history and trends | every lane writes each test's outcome, duration and key per run (`test-runs/`); history, median and newest duration, sparklines |
| Flaky tests | flaky only on proof: the identical test key (binary, arguments, device, inputs) both passed and failed |
| Quarantine | at most 10 at once, 7 days, an issue record with the ROADMAP finding text, audited, notified; the test still runs and its failure is printed, annotated and recorded but does not fail the lane; it ends by itself |
| First bad commit | the tests that passed in the lane's previous run on the branch and fail now, bisected together (one regression usually breaks many): narrowed with every recorded run, then up to 3 targeted re-runs of those tests alone (`pipeline.py lane --tests`, one double-quoted expression) on the middle commit through the normal dispatcher and budget; at most 2 bisections run at once, the rest wait queued; the first bad commit is the first one where any of the tests fails |
| Audit log | every manual and automatic decision (build start and cancel, job retry and cancel, agent stop, token create and revoke, quarantine and release, backup and restore, bisection) with who, what, target and when |
| Retention | logs 90 days, artifacts 30 days after a build ends, applied hourly; records, test history and the ledger kept |
| Reproducibility | each attempt's commit, machine, image, platform, CPUs, toolchain versions and environment (secrets and tokens left out), shown as "Reproduce this step" |
| Cost reports | spend per build, lane, device and month against the cap with a linear forecast (`ops.py cost`, the Spend page) |
| Backup and recovery | `ops.py backup create|restore` with a SHA-256 manifest; runbooks for a lost launcher, bucket or project |
| Runbooks | stuck build, lost VM, budget hit, coordinator not ticking, agent cannot connect, upgrade, quarantine, bisection, queues ([runbooks](../recipes/ci-platform-runbooks.md)) |
| SLOs | queue time (95 % within 120 s launcher, 300 s owned, 900 s cloud) and coordinator availability (99.5 % of minutes over 30 days), measured from every assignment and every watchdog minute |

The quarantine is consistent with the repository's rule that a failure is fixed, never waived: only a proven-flaky
test qualifies, the quarantine is bounded in time and number, the evidence and the ROADMAP finding text are created
with it, and the original JUnit keeps the failure; the lane's verdict reads a view without the held failures, as it
already reads the third-party register.

**Design of the tool.** The web UI is plain HTML, CSS and JavaScript with no build step: a token-based design system
(colour, space, type, radius, motion) with light and dark themes following the OS and switchable; status always as
an icon and a word (never colour alone); WCAG 2.2 AA contrast held by a test; visible focus, a skip link, landmarks,
reduced motion and keyboard shortcuts on every page; layouts from phone width up. Each build page is ordered status
first, then what failed and why with the first error highlighted, then the step graph and the logs; logs, builds and
the build page update by Server-Sent Events. The QUAL matrix has filters, sticky headers, per-cell drill-down and a
legend; the Spend page the cap and the forecast. `ci/ops.py` is noun-verb with `--json` everywhere, colour only on a
terminal, errors that say what to do next, and bash, zsh and PowerShell completion; the earlier verbs keep working.
`ci/coordinator/demo.py` serves a labelled fixture locally to look at all of it without a cloud.

**Security review, additions to §14.**

| Threat | Mitigation |
|---|---|
| The UI reached from inside the network | UI mode `iap-tunnel` accepts only loopback and the IAP range; a runner VM's request is refused (tested) |
| A forged identity header (Tailscale, a proxy) | identity only from tailscaled's whois of the connecting peer or a verified signed assertion; no header is trusted (tested with a forged `Tailscale-User-Login`) |
| A listener serving more than it should | each listener serves only its surfaces; an agent-only listener has no UI and a UI-only one no agent API (tested) |
| Quarantine used to hide a regression | only proof of flakiness from one key qualifies, a regression on a changed key never does; bounded, expiring, audited, notified, the original evidence kept |
| A reproducibility record leaking secrets | secret values become `[secret]`, the job token and git's auth header are left out (tested) |
| Notification channels | outbound only, credentials in Secret Manager, once per key, failures never fail a build |
| The open SSH and RDP defaults of a new project | the runbook removes `default-allow-ssh` and `default-allow-rdp`; SSH to the launcher goes through IAP |

## §17 Amendment (2026-10-31): failure diagnostics and gold images

The user ordered on 2026-10-31: "do it I want the full CI system available!"
([session, verbatim](../sessions/2026-10-31-ci-diag-visual.md#user-order)), relayed with two scopes: the engine's
diagnostics hooked into every failed job, with a local reproduction command, and golden-image visual testing in the
spirit of Skia Gold. No engine code changes; what the engine must add is named by row in the session note.

**Failure diagnostics (CI-1).** A failed lane collects, before its upload (a phase that always runs), the files the
run left that the engine's diagnostics define (runtime-diagnostics.md, diagnostic-runbooks.md): cerid-diagnostics/1
bundles and flight records, crash records, raw dumps, CEIR run records, Perfetto and CPROF captures, plus the
sanitizer reports and validation-layer messages cut out of the test output per test, and a summary naming the failed
phase, the failed tests, the CPU level and the exact test command. Only files newer than the lane's start count, so
a long-lived machine never reports an earlier run's. Artifacts carry a kind (by an explicit `--kind` or by their
`diagnostics/<kind>/` path); the build, job and QUAL pages link them from the failed cell. `ops.py job reproduce`
downloads them with the job's reproducibility record (each checked against its SHA-256) and prints the exact local
replay: the commit in its own worktree, the lane as CI ran it with the failed tests only, the failed tests at the
job's CPU level, and each diagnostic's reader; `--run` runs the worktree and lane steps, never in the current
checkout.

*Privacy, following the engine's redaction contract (`crd/core/redaction.hpp`; ADR-0133 ID-8: a raw dump is never
declared redacted).* A raw memory dump of a test process holds its environment, and so the job's secrets: raw dumps
(minidumps, GPU dumps, cores) are withheld unless the device's cell opts in (`"diagnostics": {"raw_dumps": true}`,
for an owned machine whose jobs carry no secret), and a bundle's RawDump section becomes an explicit absent section
with the bundle's checksums rewritten. Every text item has the job's secret values replaced. The agent, which alone
holds the values, scrubs every artifact it uploads (UTF-8 and UTF-16 spellings): text is redacted, a bundle's
sections are redacted and rewritten, and any other binary that carries a value is withheld with a line in the log.
A withheld item stays in the summary by kind, size and SHA-256, never silently. Before this amendment artifacts were
uploaded unscrubbed; that defect is fixed with a test.

**Gold images (CI-3).**

| Part | Decision |
|---|---|
| Input | a `cerid-gold/1` manifest beside the PNGs in `<build>/gold/` (test name, file, API, backend, feature, CPU level), uploaded by the lane; Firebase Test Lab screenshots get the same manifest from the downloaded results tree |
| Identity | the digest of the pixels (SHA-256 of the size and RGBA8 data), so re-encoding is the same image; stored once |
| Keys | test, device, device class (GPU vendor and device kind), vendor, kind, API, backend, CPU level, feature, commit, build |
| Baselines | per grouping: the test plus the keys the policy names (`api` by default; `device_class` or `device` where a test declares that baselines differ, as app-launch screenshots do); any number of positive digests |
| Exact | the device's GPU is a software renderer (lavapipe, WARP, SwiftShader): only an approved digest passes |
| Perceptual | real GPUs and phones: LDR-FLIP at the device's pixels per degree (screen density at its kind's viewing distance; 67 otherwise) and SSIM against the newest positives; within all of the test's tolerances (mean FLIP, the share of pixels over a FLIP threshold, minimum SSIM) it passes, and says so; the pass lasts only while that baseline stays approved |
| Never a pass by default | an untriaged or not yet compared image marks its cell needs triage; a rejected or unreadable image, a bad manifest or a missing file fails it |
| Triage | approve or reject (or reset) by compare-and-swap with the label the person saw; audited; a reject writes a finding with the ROADMAP text its owner files and notifies; a new baseline re-compares the grouping's untriaged images |
| Policy | `ci/gold.json` in the repository, reviewed like code: defaults, per-test overrides by pattern, viewing distances |

*Comparison in the standard library.* FLIP (the reference algorithm, NVlabs/flip, BSD-3-Clause, written again) and
SSIM are pure Python with separable Gaussians: 512 x 512 takes about 7 s for both on the development host, so the
comparison runs on its own worker thread beside the coordinator's loop and never delays dispatch; an image over the
policy's pixel budget is compared box-downsampled by a declared integer factor with the pixels per degree divided by
it. A NumPy pin was considered and rejected: it would add a binary dependency to the launcher for a workload this
small, and the engine's own metric (IMG-ORACLE: HDR-FLIP and a ColorVideoVDP-class metric in crd-hesap) is the one
renderer rows qualify against; the CI cross-checks with it when it lands and with NVIDIA's reference tool when the
user approves its download (a test runs when `CERID_FLIP_REFERENCE` names its output).

**Security review, additions to §14.**

| Threat | Mitigation |
|---|---|
| A secret value in an uploaded artifact | the agent scrubs every upload (UTF-8 and UTF-16), rewrites bundles, withholds other binaries (tested against the previous agent, which leaked) |
| Raw memory leaving the machine | withheld unless the device opts in; RawDump bundle sections made absent; a dump holding a secret value is withheld even then |
| A crafted bundle or PNG | the readers bound every length, check every CRC, recover only complete prefixes, refuse palette, 16-bit and interlaced PNGs, and cap the pixel count |
| A crafted manifest | test names and file names are validated (no `..`, no absolute path); images are read only from the job's own artifacts |
| A stale or forged triage | triage needs a signed-in UI user and the request header; it applies only if the label is still the one the person saw |
| Untrusted text in summaries | everything an artifact supplies is escaped before display |

## Consequences

- One registry, one planner, one coordinator and one agent cover cloud and owned machines; buying hardware is adding
  a cell and a registration token.
- No hosted CI service sits between a commit and its evidence: scheduling, logs and history live in Cerid's own
  project or on an owned machine.
- The coverage contract, the presets, the module graph, the helpers and the pins are reused, not copied.
- Paid evidence is bounded by money, not by hosted runner limits; a cell the budget stops stays visibly open.
- Until the user completes the recipe, every cell waits on its account and GitHub Actions remains the evidence.
