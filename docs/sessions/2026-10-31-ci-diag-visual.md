# 2026-10-31 — The Cerid orchestrator: failure diagnostics, local reproduction and gold images

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-ci-1).

Rows: [CI-1](../ROADMAP.md#slice-ci-1) (diagnostics, reproduction) and [CI-3](../ROADMAP.md#slice-ci-3) (gold images,
triage). Decision: [ADR-0135 §17](../decisions/0135-cerid-ci-platform.md#17-amendment-2026-10-31-failure-diagnostics-and-gold-images).
Contract: [CI platform](../design/ci-platform.md#failure-diagnostics). Operations:
[runbooks](../recipes/ci-platform-runbooks.md#a-failed-job-reproduce-it-locally). Earlier work:
[company-level operation](2026-10-31-ci-orchestrator-elite.md), [the orchestrator](2026-10-30-ci-orchestrator.md).

## The user's order (verbatim, 2026-10-31)

<a id="user-order"></a>

> "do it I want the full CI system available!"

Relayed with it: hook the engine's diagnostics into the CI (every failed or crashed job collects and uploads, as
typed artifacts linked from the failed cell, the crash and failure bundles, flight-recorder and replay records,
Perfetto traces, sanitizer reports, GPU validation output and the job's reproducibility record; no engine change;
`ops.py job reproduce <id>`; the engine's privacy and redaction contracts respected), and build golden-image visual
testing in the spirit of Skia Gold (content-addressed images keyed by test, device, API, CPU level and commit;
approved baselines; exact comparison for deterministic software renderers, perceptual for real GPUs and phones with
per-test tolerances; untriaged differences never pass; a triage page with side-by-side views, blink and zoom,
approve and reject audited, reject filing a finding; per-device-class baselines where declared; Firebase screenshots
through the same path). Branch `ci-diag-visual`, based on `origin/ci-orchestrator-elite` because that branch is not
merged yet: merge `ci-orchestrator-elite` first, then this one. Python, HTML, CSS, JavaScript and documents only; no
C++ build, configure, engine CTest or WSL lane ran.

## What was built

- **Diagnostics** (`ci/coordinator/diagnostics.py`). A failed lane's `upload` phase (it always runs) collects the
  files the run left (newer than the lane's start, in the build tree and the temporary directory): `*.cdb` and
  `flight-<n>.cdb` bundles, `crash_*.log` records, `crash_*.dmp`, `gpu_*.dmp` and core dumps, `*.crpl` run records,
  `*.perfetto.json` and `*.cprof` captures; it cuts ASan, LSan, TSan, UBSan and MSan reports and Vulkan, D3D12 and
  Metal validation messages out of the CTest log per test; it writes `<build>/diagnostics/<kind>/...` and
  `summary.json` (failed phase, failed tests, CPU level, the exact CTest command, every item or why it was
  withheld). The coordinator types each artifact by that path (or an explicit `--kind`); the build page shows a chip
  per kind on the failed step, the job page a Diagnostics card with the reproduce command, the QUAL drawer the kinds
  with a link to the job. A step that never reaches its `upload` phase (a timeout, a crashed or killed lane: the
  jobs most likely to leave a crash dump) is collected by the agent: after a failed step it runs the checkout's
  collector (`diagnostics.py collect --checkout DIR --since T`) for every build tree the lane did not summarise, and
  the plan's steps now list `build/*/diagnostics/**/*` and `build/*/gold/**/*` among their artifact paths, so the
  agent's end-of-job upload sends them either way.
- **Privacy.** Raw memory (minidumps, GPU dumps, cores) is withheld unless the device opts in
  (`"diagnostics": {"raw_dumps": true}`), because a dump of a test process holds its environment and so the job's
  secrets; a bundle keeps its other sections with the RawDump section an explicit absent one (its checksums
  rewritten); every text item has the job's secret values replaced. The agent now scrubs every uploaded artifact of
  the job's secret values (UTF-8 and UTF-16), rewrites a bundle's sections, and withholds any other binary that
  carries one, saying so in the log.
- **Reproduction.** `python ci/ops.py job reproduce <id> [--attempt N] [--dest DIR] [--run]` downloads the newest
  attempt's diagnostics (checked against their SHA-256) and the reproducibility record, and prints: the commit in its
  own worktree, the lane exactly as CI ran it with the failed tests only (`pipeline.py lane --tests "^(a|b)$"`), the
  failed tests in the built tree at the job's CPU level (`--preset <p>-at-sse2` for the SSE2 phase), and each
  diagnostic's reader (`ceridc diag --command bundle.inspect`, `replay.run`, Perfetto UI, `cdb`, `gdb`). `--run`
  makes the worktree and runs the lane in it; it never touches the current checkout.
- **Gold images** (`ci/coordinator/gold.py`, `ci/coordinator/image.py`, `ci/gold.json`). A lane uploads
  `<build>/gold/manifest.json` (`cerid-gold/1`) with its PNGs; a Firebase Test Lab job builds the same manifest from
  its downloaded results (`gold.py ftl-manifest`, `FirebaseTestLab.results_command`). After the job, a worker thread
  beside the coordinator's loop decodes each PNG, stores it by pixel digest, keys it (test, device, device class,
  vendor, kind, API, backend, CPU level, feature, commit, build) and compares it: a positive digest passes, a
  negative one fails, otherwise a software GPU (lavapipe, WARP, SwiftShader) needs triage at any difference and a
  real GPU or phone passes within the test's tolerances (mean LDR-FLIP, the share of pixels over a FLIP threshold,
  SSIM) at its pixels per degree (screen density at its kind's viewing distance) or needs triage. Untriaged and
  not-yet-compared images turn a passing QUAL cell into **needs triage**; rejected or unreadable ones fail it.
  Approve and reject are compare-and-swap with the state the person saw (a stale click is refused), audited, and a
  reject writes a finding with the text of the ROADMAP row its owner files and notifies. A newly approved baseline
  re-compares the grouping's untriaged images. The verdicts are applied to the matrix before anything is counted or
  drawn (`generate.render(cells_hook=...)`), so the web UI's QUAL page, the served static page and the per-build
  dashboard artifact agree.
- **Web UI.** A Gold images page: the queue (filter, `j`/`k`), the approved, new and FLIP-difference images side by
  side with synchronised scrolling, blink (`b`; a manual flip under reduced motion), zoom (`+`, `-`, `0`) with
  pixelated scaling, the metrics against the tolerances, where it was rendered, a note, Approve (`a`) and Reject
  (`r`). A new status, needs triage (an eye icon, its own colour, AA contrast held by the test), in the matrix, the
  legend and the filters. `demo.py serve` shows all of it with fixture data.

## Bugs found and fixed on the way (each with a test that fails without the fix)

- The agent uploaded artifact files as they were: a log or report holding a secret value left the machine
  (`Diagnostics.test_uploads_are_scrubbed_typed_and_withheld_binaries_reported`; run against the previous
  `ci/agent/agent.py` it uploads the secret).
- A binary file holding a UTF-16 secret decodes as UTF-8, so a decode test called it text and would have "scrubbed"
  it by replacing only the UTF-8 spelling (found by the same test; NUL bytes now mark binary data).
- The fixture's gold results sat on builds the coordinator's recent list does not hold, so the triage page read
  nothing: the page and the re-comparison now read an index of the newest results the coordinator keeps
  (`Demo.test_the_fixture_fills_every_page`).
- From the advisor review: a step killed by its timeout, or a lane that crashed, never reached its own upload phase
  and so uploaded no diagnostics; the agent now collects after any failed step
  (`Diagnostics.test_a_step_killed_before_its_upload_still_leaves_its_diagnostics`). And the served static QUAL
  page did not carry the gold verdicts, so it showed pass where the web UI showed needs triage
  (`Demo.test_the_fixture_fills_every_page` counts the needs-triage cells on the served page).
- A file found under both the build tree and the temporary directory (a one-shot agent's checkout lives in the
  temporary directory) would have been collected twice; the two searches share one set of seen paths.

## The perceptual comparison in the standard library

FLIP and SSIM are written in pure Python (no NumPy pin): every Gaussian is separable, so a pass is one dimension at
a time, each window's dot product one `math.sumprod` call. Measured on this host (Python 3.14.4): 512 x 512 takes
3.9 s for FLIP and 0.9 s for SSIM. `max_pixels`
(262,144 by default) bounds the work: a larger image (a phone screenshot is about 2.6 megapixels) is compared box-
downsampled by a declared integer factor with the pixels per degree divided by it, never skipped. Properties are
tested (identical images score 0, inverted ones about 0.92, error grows with noise, white against black gives the
paper's 0.9674 from its own constants); one sRGB level of noise everywhere scores about 0.018 mean FLIP at 67 pixels
per degree, which set the default tolerance of 0.02. The comparison with NVIDIA's reference implementation runs
when `CERID_FLIP_REFERENCE` names a directory with its output for a pair (`Images.test_flip_agrees_with_the_reference_implementation`);
that tool is a download the user approves first. The engine's own metric is IMG-ORACLE's; the CI defers to it when
it lands.

## Engine work this needs (reported, not done here)

- **[CRASH-FIX.1](../ROADMAP.md#slice-crash-fix.1)**: reproduction packs and `ceridc repro <pack>`; `job reproduce`
  then downloads the pack and prints `ceridc repro`, replacing the lane rerun for crashes.
- **[DIAG.11b](../ROADMAP.md#slice-diag.11b)** (or CRASH-FIX.1): one documented diagnostics output directory the
  crash handler, the diagnostic host's `capture_dir`/`crash_dir`, the Perfetto export and the GPU-loss writer honour
  from the environment (for example `CRD_DIAG_OUTPUT_DIR`). Today tests choose their own directories, often temporary
  ones they delete, so the collector searches the build tree and the temporary directory by file name.
- **[DIAG.6c](../ROADMAP.md#slice-diag.6c)**: a fixed file name for an exported Perfetto trace (`*.perfetto.json`)
  so the collector does not depend on each test's naming.
- **[DIAG.5b](../ROADMAP.md#slice-diag.5b) / [DIAG.7c](../ROADMAP.md#slice-diag.7c)**: an on-disk GPU-loss record on
  Linux beside the Windows `gpu_*.dmp`.
- **[IMG-ORACLE](../ROADMAP.md#slice-img-oracle)** and the renderer rows that need goldens: write the rendered images
  of golden tests with a `cerid-gold/1` manifest into `<build>/gold/` (test name, file, API, backend), and own the
  perceptual metric the gold service cross-checks against.

## Verification

`python scripts/test-ci-orchestrator.py` (148 tests, one skipped without `CERID_FLIP_REFERENCE`), `python
scripts/test-ci-orchestrator-e2e.py` (3), `python scripts/test-ci-platform.py` (76), and the quick validators. The
CDB1 reader and writer read the engine's own fuzz-corpus bundles (`tests/foundation/perf/fuzz/corpus/bundle`) and
write them back byte for byte, and recover the truncated seed's complete prefix. In the desktop app's browser pane
against `ci/coordinator/demo.py serve`: the Gold images page at desktop width in the dark theme (queue, three panes,
metrics; `j`/`k`, `+` to 2x with pixelated scaling and scrollbars, `b` blinking between approved and new, `a`
approving with a note, the QUAL cell turning to pass), the QUAL matrix with needs-triage cells and the failed cell's
diagnostics, the failed build's diagnostic chips, the job's Diagnostics card with the withheld crash dump, and the
Gold and job pages at 375 px in the light theme. Screenshots (outside the repository, in this session's tool-results
directory `C:\Users\abici\.claude\projects\D--Dev-cerid\25b5b653-fafe-48b3-9c70-24be8265c983\tool-results\`):

| View | File |
|---|---|
| Gold images: queue, first image without a baseline | `mcp-Claude_Browser-blob-1793406265099-7lwwpt.jpg` |
| Approved, new, FLIP difference and metrics | `mcp-Claude_Browser-blob-1793406273309-2s1th4.jpg` |
| Zoom 2x, pixelated, synchronised scrollbars | `mcp-Claude_Browser-blob-1793406318709-say0wq.jpg` |
| QUAL matrix with needs-triage cells | `mcp-Claude_Browser-blob-1793406346855-14mvbd.jpg` |
| A failed cell's diagnostics in the drawer | `mcp-Claude_Browser-blob-1793406353469-p42cp6.jpg` |
| A failed build's diagnostic chips | `mcp-Claude_Browser-blob-1793406370144-pk4d92.jpg` |
| A job's Diagnostics card and reproduce command | `mcp-Claude_Browser-blob-1793406378122-9ggvsx.jpg` |
| Job diagnostics, phone width, light | `mcp-Claude_Browser-blob-1793406402312-6vny0i.jpg` |
| Gold images, phone width, light | `mcp-Claude_Browser-blob-1793406409077-v7bnqo.jpg` |

Three local runs failed in ways pure Python cannot: one segmentation fault, and twice `operator.mul` reporting
one argument from `map(mul, kernel, window)` at the same line; each passed when rerun unchanged. That matches this
host's recorded misexecution under load (MEMORY: "This host misexecutes") and is recorded here, not chased; the
convolution now calls `math.sumprod` instead (twice as fast, and no longer that call path). One later full run of the
orchestrator suite reported one error that two unchanged reruns did not repeat; recorded the same way.

## User actions now needed (in order)

1. Merge `ci-orchestrator-elite` into `main`, then `ci-diag-visual` (this branch is based on it).
2. The setup recipe's Part A as before (create the `cerid-ci` project, link billing, set the 50 USD budget, say go).
3. For phone screenshots: name the Firebase Test Lab results bucket the launcher reads
   (`CERID_FTL_RESULTS_BUCKET`, recipe step "Firebase Test Lab"); it is in the existing Firebase project.
4. Optional: approve the download of NVIDIA's FLIP reference tool (BSD-3-Clause) to run the cross-check.
5. On an owned machine whose jobs carry no secrets, optionally allow raw dumps (`"diagnostics": {"raw_dumps": true}`
   on its cell in `ci/devices.json`).

## Open items

- No engine test writes gold images yet; the first renderer golden makes the path live (IMG-ORACLE).
- The perceptual comparison runs on the coordinator's small VM; heavy image suites will want the worker on an owned
  machine or the engine's metric.
- Live runs of both features wait on Part A like everything else.
