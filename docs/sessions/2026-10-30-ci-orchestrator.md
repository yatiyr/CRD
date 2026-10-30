# 2026-10-30 — The Cerid orchestrator: Cerid's own CI coordinator, agent and web UI replace Buildkite (CI-1 to CI-4)

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-ci-0).

Rows: [CI-0](../ROADMAP.md#slice-ci-0) to [CI-4](../ROADMAP.md#slice-ci-4). Decision:
[ADR-0135 §12 to §15](../decisions/0135-cerid-ci-platform.md#12-amendment-2026-10-30-cerids-own-orchestrator-replaces-buildkite).
Contract: [CI platform, the Cerid orchestrator](../design/ci-platform.md#the-cerid-orchestrator). Operator steps:
[setup recipe](../recipes/ci-platform-setup.md). The first decision and the earlier orders:
[the CI platform session](2026-10-30-ci-platform.md#user-order).

## The user's order (verbatim, 2026-10-30)

<a id="user-order"></a>

> "can we make our own buildkite? I think we can do it"

> "start the paralel agent and make our own buildkite, our own orchestrator that will always work in the quality of
> paid buildkite! I want this to be fully built as an orchestrator ci project!"

The earlier orders of the same day still stand ([verbatim](2026-10-30-ci-platform.md#user-order)): CI independent of
GitHub's CI, cloud now and owned machines later with no redesign, a hard 50 USD monthly cap, every device and
platform, and "fully easy to manage system not a nightmare of intertwined wires". The work ran in parallel with the
serial SIMD-DISPATCH workflow, on its own branch (`ci-orchestrator`) and worktree, touching none of its files
(no engine, tests, runtime or cmake file other than `cmake/pins.json`; no C++ build, configure, CTest or benchmark ran).

## What was built

Buildkite's role is replaced; the registry, the planner, the affected-only plan, the content-addressed cache, the
budget, the four executors, `ci/ops.py` and the QUAL dashboard are reused.

- `ci/coordinator/`: the job state machine (exactly ten states, every move a table row) and the build state machine;
  the `cerid-ci-pipeline/1` step document (dynamic uploads, `wait`, keys, queues and tags, priorities, concurrency
  groups, step and build timeouts, soft failure, automatic and manual retries, `cached`/`budget` skips, artifacts,
  declared secrets; unknown fields refused); the coordinator core (builds, assignment, agents and tokens, logs,
  artifacts, annotations, meta-data, commit status, the watchdog for lost agents, stuck and timed-out jobs, expiry
  and retention); the cloud dispatcher over the existing executors (budget, reservation, launch, reaper, preemption
  as a retry, orphan sweep); signed webhooks for GitHub, Gitea/Forgejo, GitLab and a host-neutral signature with replay
  protection, polling, cron schedules; the git mirror served through `git http-backend` (fetch only) on job tokens;
  standard-library RS256 and ES256 JWT verification for Google identity tokens, Cloudflare Access and Google IAP; the
  HTTP service and the background loop; `ci/coordinator.json` with no secret value.
- Durable state with compare-and-swap on the existing result store: Cloud Storage object generations, and the
  filesystem backend with the same interface (a per-object generation under an operating-system lock).
- `ci/agent/agent.py`: one standard-library file, one-shot (cloud VM identity, or one job token on standard input)
  and long-running (registration token file, 12-hour sessions) modes, exact-commit checkout, allow-listed
  environment, live chunked logs with secret redaction across chunk boundaries, heartbeats, process-tree cancellation,
  timeouts, the GCE preemption notice, artifacts, and the in-job tools (`pipeline upload`, `annotate`, `artifact`,
  `meta-data`, `step outcome`).
- `ci/web/`: the web UI (builds, the build page with its step graph, live log tail with ANSI colours, search and
  download, retry and cancel, agents and queues, spend, the QUAL page), plain HTML/JS, strict CSP.
- `ci/pipeline.py` emits the orchestrator's document (`orchestrator_pipeline`, `cerid_event`, the outcome through the
  agent); the executors launch the Cerid agent (no token at all for a GCE VM; the agent's SHA-256 checked by every
  bootstrap); `ci/ops.py` gains builds, build, retry, cancel, agents, queues, logs, agent-token, health, backup and
  restore; the four buildkite-agent pins are removed and the licence manifest regenerated.
- Tests: `scripts/test-ci-orchestrator.py` (75: state machines, document, compare-and-swap on both backends including
  a real-HTTP Cloud Storage fake, threads and processes, authentication with generated RSA and P-256 keys, host
  adapters and cron, the coordinator, the dispatcher, the agent and the service; fault injection: lost agent, bounded
  retries, an agent that vanishes after taking a job, preempted VM, a VM that never boots, launch failure, duplicate
  webhook deliveries and webhook plus poll, coordinator restart mid-build, newer store schema, concurrent polls and
  uploads) and `scripts/test-ci-orchestrator-e2e.py` (the coordinator in-process on 127.0.0.1 with the filesystem
  backend and one real agent process: a signed webhook and its replay, the bootstrap upload, dependencies and `wait`,
  a dynamic upload, a soft failure, a declared secret redacted from the live log including across writes, ANSI
  output, an artifact, an annotation, meta-data, a step outcome, a checkout through the coordinator's mirror with the
  job token, and a cancellation that stops the running command). `scripts/test-ci-platform.py` (76) follows the new
  document, events and executors. Both new suites are steps of the hosted repository job and of the orchestrator's
  repository checks; their CTest registration lives in `tests/`, owned by the serial workflow (lines in the
  hand-over).
- CI-4: the parity table maps every GitHub Actions job and step to an orchestrator step (`plan`,
  `repository-<host>`, `lane-<preset>`, `required`) and a lane phase; the host-check experiments stay listed open.

## Bugs found and fixed on the way (each with a test that fails without the fix)

- `LocalStore` writes failed on Windows with "access denied" when three processes updated one object: a reader or the
  indexer holding the target open makes `os.replace` fail for milliseconds. `replace_file` now retries the rename for
  up to about two seconds; `Cas.test_concurrent_processes_lose_no_update` failed first, and a deterministic test holds
  the retry.
- The lane's evidence annotation had no body: `buildkite-agent annotate` read the summary from standard input, which
  the lane runner never provides. The annotate tool now takes `--body-file`; the lane test asserts the summary file.
- The baked Ubuntu images had no `python` (only `python3`) although every lane command starts with `python`; the
  provisioning now installs `python-is-python3`. The Windows image installed Python per user, invisible to the
  startup script running as SYSTEM; it now installs machine-wide. The provisioning test asserts both.
- `gce_vm.http_request` left HTTP error responses open (ResourceWarning on every 404 and 412); it closes them now.

## Verification

Python only, by the parallel-work limits: `python scripts/test-ci-orchestrator.py` (75 tests),
`python scripts/test-ci-orchestrator-e2e.py` (3 tests, about 18 s, a real agent process on this Windows host),
`python scripts/test-ci-platform.py` (76), and the quick validators `check-master-plan.py`, `check-repository.py`,
`check-allman-braces.py`, `check-pins.py`, `check-generated.py`, `check-ci-tiers.py` and
`gen_license_manifest.py --check`. The coordinator was also started from `ci/coordinator/main.py serve` with the
tracked configuration (health, builds, spend, queues, the QUAL page and the UI answered), and the web UI was looked at
in a browser with one finished and one running build (no console error). The Linux side of the agent (bash scripts,
process groups, `fcntl` locks) runs for the first time in the hosted repository job on `ubuntu-24.04`. No cloud call
was made: Cloud Storage, Compute Engine, Secret Manager and the git hosts are fakes in the tests, and the first live
build waits on the user actions below.

## User actions now needed (in this order; the recipe has every click)

1. Section 1 to 3 of the [setup recipe](../recipes/ci-platform-setup.md): the `cerid-ci` project with billing, the
   two buckets (versioning off), quotas, the 50 USD budget with its Pub/Sub stop, the two service accounts, the three
   Secret Manager secrets.
2. Section 4: the launcher VM, the firewall rule for port 8780 inside the network, the read-only deploy key, the
   environment file, the coordinator and launcher-agent units, the launcher's registration token, the image bakes.
3. Section 5: a domain on Cloudflare, the tunnel, the Access application with its bypass paths.
4. Section 6: the repository webhook and, optionally, the commit-status token.
5. Then sections 7 to 11 as devices are wanted (Test Lab, SSH hosts, owned machines, console packs, a static QUAL
   copy).

## Open items and consequences

- The first live build on the launcher (CI-1), a live launch per executor and both caps observed stopping a paid
  launch (CI-2), the live private UI (CI-3), and the parity proof on the same revisions (CI-4) wait on the user
  actions above; no agent may perform them.
- `check-master-plan.py` still accepts only a GitHub Actions run URL as Needs CI evidence; widening it to an
  orchestrator build page is CI-4 work, left until the orchestrator has a public URL (the script is shared with the
  serial workflow).
- Google Cloud has no Windows on ARM: the `windows-arm64` lane still needs an SSH host or an owned Snapdragon machine.
- The host-check experiments stay open in the parity table until their inline timing loops move into scripts.
