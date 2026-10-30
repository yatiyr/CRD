# 2026-10-31 — The Cerid orchestrator at company level: test quality, operations, IAP-tunnel access, designed UI and CLI

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-ci-0).

Rows: [CI-0](../ROADMAP.md#slice-ci-0) to [CI-4](../ROADMAP.md#slice-ci-4). Decision:
[ADR-0135 §16](../decisions/0135-cerid-ci-platform.md#16-amendment-2026-10-31-company-level-operation-iap-tunnel-access-and-polling).
Contract: [CI platform](../design/ci-platform.md#the-cerid-orchestrator). Setup:
[recipe](../recipes/ci-platform-setup.md); operations: [runbooks](../recipes/ci-platform-runbooks.md). Earlier work:
[the orchestrator](2026-10-30-ci-orchestrator.md), [the checkout strategy](2026-10-30-ci-orchestrator-checkout-strategy.md).

## The user's order (verbatim, 2026-10-31)

<a id="user-order"></a>

> "yes switch the guide to polling and tailscale, do not miss anything we need proper elite company level CI system
> and also design of the tool and all the UI UX must be gold standard"

Relayed the same day, after the user asked "why do we need tailscale?": the default way to open the web UI is a
Google IAP TCP tunnel (`python ci/ops.py ui`), Tailscale becomes the optional add-on for phone access and Cloudflare
the documented alternative; polling stays the default trigger; the user's part of the setup is only (1) create the
`cerid-ci` project, (2) link billing, (3) set the 50 USD budget, (4) say go, and the rest is an operator runbook run
with the user's signed-in `gcloud`, idempotent, each step with a check. The earlier orders stand (CI independent of
GitHub's, cloud now and owned machines later, the 50 USD cap, every device, easy to manage, Buildkite quality).
Branch `ci-orchestrator-elite`; Python, HTML, CSS, JavaScript and documents only; no C++ build ran.

## What was built

- **Access.** UI mode `iap-tunnel` (loopback and 35.235.240.0/20 only; a runner VM in the network is refused),
  `tailscale` (tailscaled whois of the peer; allow-listed logins; tagged devices refused; no header trusted), each
  listener with its own surfaces and access mode (`CERID_CI_LISTEN=host:port=ui+agent@mode`), and `ops.py ui`
  (starts the IAP tunnel with the user's gcloud, waits for `/healthz`, opens the browser, explains a refused tunnel).
  Polling every 60 s is the default trigger; webhooks are an add-on.
- **Test quality** (`ci/coordinator/quality.py`): per-test history from `test-runs/` records every lane now writes
  (outcome, duration, key); flaky only on proof from one key; quarantine (10 at most, 7 days, issue record with the
  ROADMAP finding text, audited, notified; the lane's verdict reads a JUnit view without the held failures, the
  original stays the evidence); first-bad-commit bisection (recorded runs, then up to 3 targeted re-runs through
  `pipeline.py lane --tests`).
- **Operations.** Notifications (`notify.py`: email through the user's SMTP provider, self-hosted ntfy; failure,
  recovery, budget 80/100 % and the GCP stop, quarantine and expiry, bisection; once per key); the audit log of every
  manual and automatic decision; the reproducibility record per attempt (machine, image, toolchain versions,
  environment without secrets); cost reports per build, lane, device and month with a forecast (`reports.py`); SLOs
  for queue time and availability with their measurement; retention; backup and restore; runbooks.
- **Web UI** rebuilt on a design system (tokens, light and dark following the OS and switchable, icon-and-word
  status, AA contrast held by a test, focus rings, skip link, reduced motion, keyboard shortcuts, phone layouts,
  designed empty, loading and error states): Builds, the build page (status, what failed with the first error
  highlighted and jump-to-error, step graph), the live log over Server-Sent Events, the QUAL matrix (filters, sticky
  headers, drill-down, legend), Test quality, Spend (cap, forecast, breakdowns), Agents and queues, Operations (SLOs,
  audit). `ci/coordinator/demo.py serve` shows it with labelled fixture data.
- **CLI.** `ci/ops.py` noun-verb with `--json` everywhere, colour only on a terminal (NO_COLOR, `--no-color`), errors
  with a next step (exit 2), bash, zsh and PowerShell completion; the earlier verbs keep working.
- **Docs.** The setup recipe rewritten as Part A (the user's four steps) and Part B (the operator's idempotent gcloud
  runbook with checks), then the optional pieces; the runbooks; ADR-0135 §16; the design doc's orchestrator, test
  quality and web UI sections; rows CI-1 to CI-4 extended in place.

## Bugs found and fixed on the way (each with a test that fails without the fix)

- A notification's log record passed its event as a keyword colliding with the log's own event argument: every
  notification raised (`Notifications.test_once_per_key_filtered_and_never_fatal` failed first).
- A UI-only listener still served the agent program (`Surfaces.test_each_listener_serves_only_its_surfaces`).
- `allow_dependency_failure` did not cover the dependencies a `wait` step adds, so a required-result step after a
  wait was canceled when a lane failed (`Document.test_allow_dependency_failure_covers_the_wait_barrier_too`, found
  while seeding the demo).
- A job taken before the first watchdog tick had no ready time and was never measured for the queue-time objective
  (`Reports.test_a_job_taken_before_any_watchdog_tick_is_still_measured`).
- The live log viewer glued the first line of every chunk onto the previous line (seen in the browser; the extracted
  `appendLog` is tested under Node.js in `WebUi.test_pure_functions_of_the_web_ui`).
- The matrix drill-down and the quarantine release did nothing: their references carried a NUL separator, which HTML
  turns into U+FFFD (seen in the browser; they now carry JSON).
- The inline log of the first failed step did not scroll to its first error (offsets were not relative to the log
  box; seen at phone width).

## Verification

`python scripts/test-ci-orchestrator.py` (118 tests, about 40 s, including the Node.js checks of the UI's pure
functions and the WCAG contrast check), `python scripts/test-ci-orchestrator-e2e.py` (3), `python
scripts/test-ci-platform.py` (76), and the quick validators. In a real browser (the desktop app's browser pane)
against `ci/coordinator/demo.py serve` on the filesystem backend: desktop and 375 px phone widths, dark and light
themes, keyboard only (Tab focus rings, `/`, `n`, `j`/`k`, `g s`, `?`), the live log tail and the build page over
Server-Sent Events, the drill-down dialog, no console error after the fixes, no horizontal scroll at phone width.

Screenshots (kept outside the repository, in this session's tool-results directory
`C:\Users\abici\.claude\projects\D--Dev-cerid\25b5b653-fafe-48b3-9c70-24be8265c983\tool-results\`; some predate the
fixes listed above, which the later ones show):

| View | File |
|---|---|
| Builds, dark, desktop | `mcp-Claude_Browser-blob-1793397527793-7v38ej.jpg` |
| Failed build: status, what failed, first error highlighted | `mcp-Claude_Browser-blob-1793397535876-52k67h.jpg` |
| Step graph and steps table | `mcp-Claude_Browser-blob-1793397541299-vlawq1.jpg` |
| A failed job's log with error lines | `mcp-Claude_Browser-blob-1793397573973-2xknjb.jpg` |
| QUAL matrix with filters and legend | `mcp-Claude_Browser-blob-1793397613733-ltw1wm.jpg` |
| QUAL per-cell drill-down | `mcp-Claude_Browser-blob-1793397662380-amcw76.jpg` |
| Test quality | `mcp-Claude_Browser-blob-1793397670623-g7sb81.jpg` |
| Spend against the cap with the forecast | `mcp-Claude_Browser-blob-1793397681141-s8x79r.jpg` |
| Operations: SLOs and audit log | `mcp-Claude_Browser-blob-1793397681141-hugh48.jpg` |
| A running build, live | `mcp-Claude_Browser-blob-1793397801293-2qnr6i.jpg` |
| The live log tail | `mcp-Claude_Browser-blob-1793397825319-pax8iz.jpg` |
| Failed build, light theme | `mcp-Claude_Browser-blob-1793397915368-8blss0.jpg` |
| Failed build, phone width | `mcp-Claude_Browser-blob-1793397923452-3jc76v.jpg` |
| QUAL matrix, phone width, dark | `mcp-Claude_Browser-blob-1793397940458-h5i8g9.jpg` |
| Agents and queues | `mcp-Claude_Browser-blob-1793397976255-sboxpe.jpg` |
| Keyboard shortcuts dialog | `mcp-Claude_Browser-blob-1793397976255-y1akcn.jpg` |

## User actions now needed (in order)

The recipe's Part A: (A1) create the `cerid-ci` project and note its ID, (A2) link billing, (A3) create the budget
named `cerid-ci monthly`, 50 USD, (A4) say go with the project ID. Saying go covers the operator's Part B with the
user's gcloud and, for a private repository, one read-only deploy key added with the user's `gh`. Then daily:
`python ci/ops.py ui`. Optional later, each a user action where an account is involved: Tailscale (device approval),
a webhook with a public route, a commit-status token, an SMTP app password or an ntfy server.

## Open items

- Every live run waits on Part A: the first polled build, a launch per executor, both caps stopping a paid launch,
  the UI through the real IAP tunnel, and CI-4's parity proof.
- The IAP TCP tunnel carries no per-user identity; with one operator holding the tunnel role the audit log names
  "operator (IAP tunnel)". A second operator would want the Tailscale mode (per-login identity) or Cloudflare Access.
- `check-master-plan.py` still accepts only GitHub Actions runs as Needs CI evidence (shared with the serial
  workflow; CI-4).
- A quarantined test needs its ROADMAP finding filed by the owner (the issue record carries the text); the CI does
  not edit the ROADMAP.
