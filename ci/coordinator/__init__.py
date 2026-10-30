"""The Cerid CI orchestrator: Cerid's own coordinator, agent protocol, durable state and web UI (ADR-0135 §12).

One coordinator process (on the always-free launcher VM or any owned machine) turns triggers into builds, builds into
jobs and jobs into work for agents; the planner (`ci/pipeline.py`), the device registry, the four executors, the
result store and the QUAL dashboard are reused unchanged in role. Modules:

- state:     the job and build state machines (every transition is data and tested) and the records
- steps:     the pipeline document (dynamic uploads, wait steps, keys, queues, priorities, concurrency, retries)
- store:     durable state on the result store with compare-and-swap (Cloud Storage generations, or the local
             filesystem backend with the same interface)
- auth:      job and session tokens, registration tokens, RS256/ES256 JWT verification (Google identity, Cloudflare
             Access, Google IAP) in the standard library
- hosts:     git host adapters: signed webhooks, polling, commit status
- scheduler: the coordinator core (builds, assignment, heartbeats, timeouts, retries, cancellation, the watchdog)
- dispatch:  one-job cloud machines through the executors, the budget and the reaper
- logs:      chunked live logs, artifacts and retention
- service:   the HTTP service (webhooks, agent API, UI API, web UI)

Contract: docs/design/ci-platform.md#the-cerid-orchestrator.
"""
VERSION = '1.0.0'
# The agent protocol: an agent speaking another protocol is refused with HTTP 426 and the coordinator's version.
PROTOCOL = 1
# The state schema of every record; a coordinator refuses a store written by a newer schema.
SCHEMA_VERSION = 1
