# Operating the Cerid CI platform: runbooks

<!-- doc-role: reference -->
> Operator runbooks. Live owner: [CI-1](../ROADMAP.md#slice-ci-1) to [CI-4](../ROADMAP.md#slice-ci-4); decision:
> [ADR-0135 §12 to §16](../decisions/0135-cerid-ci-platform.md#16-amendment-2026-10-31-company-level-operation-iap-tunnel-access-and-polling);
> setup: [setup recipe](ci-platform-setup.md); contract: [CI platform](../design/ci-platform.md).

Every runbook starts from `python ci/ops.py health` (on the launcher, or anywhere with `--store` and credentials for
the bucket) and ends with it green. `ops.py` acts on the same state as the running coordinator with
compare-and-swap, so it is safe to use while the coordinator runs; every manual action is written to the audit log
(`ops.py audit list`, the Operations page). On the launcher prefix the commands with the environment:
`cd /var/lib/cerid-ci/cerid && sudo -u cerid-ci env $(sudo cat /etc/cerid-ci/launcher.env | xargs -d "\n") ...`.

## Service-level objectives and how they are measured

| Objective | Target | Measured by | Where |
|---|---|---|---|
| Queue time, launcher | 95 % of assignments within 120 s of the step being ready | every assignment appends its wait to `orchestrator/metrics/queue/<date>.json` | `ops.py slo`, Operations page |
| Queue time, owned machines | 95 % within 300 s | the same | the same |
| Queue time, cloud | 95 % within 900 s (a VM boot included) | the same | the same |
| Coordinator availability | 99.5 % of minutes over 30 days | the watchdog marks each minute it ticks in `orchestrator/metrics/ticks/<date>.json` | the same |

Targets live in `slo` of `ci/coordinator.json`. `ops.py slo` exits 1 when an objective is missed. A missed queue
objective means too few agents or too small a quota for the load (see "a queue waits"); a missed availability
objective means coordinator restarts or a stopped launcher (see "the coordinator is not ticking").

## The coordinator is not ticking

Symptom: `health` prints `coordinator: NOT TICKING`, the UI does not open or shows stale builds.

1. `gcloud compute instances describe cerid-ci-launcher --zone us-central1-a --format='value(status)'` must be
   `RUNNING`; if not, `gcloud compute instances start cerid-ci-launcher --zone us-central1-a`.
2. `gcloud compute ssh cerid-ci-launcher --tunnel-through-iap --command 'systemctl status cerid-ci-coordinator;
   journalctl -u cerid-ci-coordinator -n 50'`. The log is JSON lines: look for `level: error` and the `event`.
3. A configuration problem prints `CONFIG:` lines: fix `/etc/cerid-ci/launcher.env`, run
   `python ci/coordinator/main.py check`, then `sudo systemctl restart cerid-ci-coordinator`.
4. "the store was written by schema N": a newer coordinator wrote the state; check out that revision again (see
   "upgrade the coordinator").

Nothing is lost while it is down: running agents keep running and send their results when it is back; a job whose
agent stopped heartbeating is retried by the watchdog (it treats the gap as an agent loss only after it is back).

## A stuck build

Symptom: a step stays running or waiting far longer than its history.

1. `python ci/ops.py build show N` names each step's state and reason (`waiting` with the dependency it waits for).
2. A step waiting on a queue: `python ci/ops.py queue list` shows the queue's agents; no agent means the launcher
   agent or the owned machine is down (restart its service) or the queue name is wrong.
3. A running step without progress: `python ci/ops.py job logs JOB --follow`. The watchdog cancels it at its
   timeout, then ends it 2 minutes later if the agent does not stop; to end it now, `python ci/ops.py job cancel JOB`.
4. A cloud step stuck in "starting" (assigned): its VM never started the agent; the watchdog replaces it after 15
   minutes. To look: `gcloud compute instances get-serial-port-output cerid-ci-<job>-a<attempt> --zone ...`.

## A lost or preempted VM

Nothing to do in the normal case: a spot VM that is preempted, deleted or never boots becomes an infrastructure
retry on a new machine (twice), recorded in the step's history and the reason column. When a step ends `failed`
with `infrastructure: preempted` after its retries, the zone is short of spot capacity: retry later
(`python ci/ops.py job retry JOB`) or set `executor_config.spot` to false for that cell (paid at on-demand prices).
`gcloud compute instances list --filter=labels.cerid-ci=job` lists job machines; the orphan sweep deletes any no
record knows within 15 minutes, and the reaper deletes every machine whose step ended.

## The budget is hit

Symptom: steps end as `budget`, a "CI spend at 100 %" notification, the Spend page red.

1. `python ci/ops.py cost` shows what spent it (by lane, device, build) and the forecast.
2. Budget steps are never passes; the required result reports the build incomplete. They run again next month, or
   after the cap is raised.
3. To raise the cap (a user decision): change `budget.monthly_cap_usd` in `ci/devices.json` and the GCP budget
   amount in the console; both bound spend. Then `python ci/ops.py job retry JOB` for the budget steps.
4. The GCP stop (`gcp_stop` true) stays in force until the month ends, even after raising the ledger cap, until the
   GCP budget amount is raised too.

## A flaky test is quarantined

The coordinator quarantines a test only on proof (its identical key both passed and failed), for 7 days, at most 10
at once, and opens an issue record (`CI-FLAKY-...`, the Test quality page) whose text is the ROADMAP finding to file.
The test still runs; its failure is printed and annotated as a quarantined failure and does not fail the lane.

1. File the finding row in `docs/ROADMAP.md` with the issue text (the repository's rule: every finding has an owning
   row).
2. Fix the cause; then `python ci/ops.py quarantine release --lane L --test T` (audited). An unreleased quarantine
   ends by itself and a notification says so; from then on the test's failures fail the lane again.

## A test newly fails: first bad commit

When a test passed in its lane's previous run on a branch and fails now (and is not flaky), the coordinator opens a
bisection: it narrows the range with every recorded run of that test, then re-runs only that test
(`pipeline.py lane --tests`) on the middle commit, at most 3 times, through the normal dispatcher and budget.
`python ci/ops.py bisect list` (or the Test quality page) shows the state; `found` names the first bad commit and
sends a notification; `stopped` means the re-runs were spent or one could not run the test (its build failed).

## A queue waits

`python ci/ops.py queue list` shows waiting steps and the oldest wait. Cloud steps wait for the concurrency group
(six machines) and the regional quota; owned steps for their machine; launcher steps for the launcher agent. The SLO
page shows whether this is a pattern. Raise the quota, add an owned machine, or accept the queue.

## An agent cannot connect

The agent prints the coordinator's answer. 401: its session or token is unknown or expired, or the registration was
revoked (`python ci/ops.py token create` a new one); 426: the agent's protocol differs from the coordinator's
(update the machine's checkout to the coordinator's revision); "plain http is allowed only to loopback or private
addresses": point it at the internal address, the tunnel or the tailnet. A cloud VM's agent refused with "no job is
reserved for instance" lost its reservation (the watchdog replaced it); the VM deletes itself.

## Notifications do not arrive

`python ci/ops.py notify test` sends one message to every channel and prints which accepted it; the coordinator's log
has `notify_failed` lines with the provider's answer. Each notification is sent once per key (a failure per build,
a budget threshold per month), so a restart never repeats one.

## Upgrade the coordinator

1. `python ci/ops.py backup create --out /var/backups/cerid-ci-$(date +%F).tar.gz` on the launcher.
2. `git fetch` and `git checkout <new reviewed revision>` in `/var/lib/cerid-ci/cerid` as `cerid-ci`.
3. `sudo systemctl restart cerid-ci-coordinator cerid-ci-launcher-agent`; `python ci/ops.py health`.
4. Cloud VMs fetch the new agent from the coordinator by themselves; update each owned machine's checkout and restart
   its agent. A coordinator refuses a store written by a newer schema and an agent of another protocol is refused
   with the coordinator's version, so a half-done upgrade fails loudly. Roll back by checking out the previous
   revision.

## Backups and disaster recovery

- **Backups:** a weekly cron job on the launcher runs `python ci/ops.py backup create --out ...` (state and results
  with a SHA-256 manifest; `--all` adds caches, logs and artifacts) and copies the file off the launcher
  (`gcloud storage cp <file> gs://cerid-ci-results-<project>/backups/`). Object versioning stays off; the backup is
  the history.
- **Launcher lost:** create it again with the setup recipe's B6 and B7 (the same commands); the state is in the
  bucket, so builds, agents and history are intact; owned agents re-register by themselves with their tokens.
- **Bucket lost or damaged:** create the bucket again (B1, B3), then `python ci/ops.py backup restore <file>` (every
  object is checked against the manifest; existing objects are kept unless `--overwrite`). Results newer than the
  backup are gone; the next complete run rebuilds the cache.
- **Project lost:** Part A and Part B of the setup recipe in a new project, then the restore above.

## Retention

Logs are kept 90 days and artifacts 30 days after a build finishes (`limits.logs_days`, `limits.artifacts_days` in
`ci/coordinator.json`); build and job records, test history and the ledger are kept. The coordinator applies it
hourly; `python ci/ops.py retention run` applies it now.
