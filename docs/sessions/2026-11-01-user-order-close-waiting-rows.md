# 2026-11-01 — User order: close the rows that wait only on CI or on further recordings

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-diag.11b).

Rows: [DIAG.11b](../ROADMAP.md#slice-diag.11b) now; the rows behind the open-ended
[SIMD-PERF](../ROADMAP.md#slice-simd-perf) track as their own clauses are proven.

## The user's order (verbatim, 2026-11-01)

<a id="user-order"></a>

- "let's close the parts waiting for CI let's use github CI for now. AND I HAVE DONE MULLIONS OF RECORDINGS WHAT THE
  HELL YOU NEED FROM ME AND YOU ARE NOT TELLING ME?"
- On the optimization track, earlier the same day: "so optimization branch made fft and other parts better already if
  we are better than our previous engine version that its good for me".

## What it means for the table

SIMD-PERF is the open-ended optimization track; it runs on its own machine and never finishes in table order, so it
must not hold every later row Partial. A row whose own clauses are all proven closes Done at its user-order override
(`check-master-plan.py`), with SIMD-PERF left Open as the background track it is.

## DIAG.11b clause (8)

The clause asks for a recording that judges jobs basic and full on the CPU per frame in the boards' shape. The user's
third elevated recording, `20261101T004352Z` at `d64f2ce8` ([session](2026-11-01-diag-11b-third-recording.md)), does:
jobs basic +0.0% measured and +0.2% external, jobs full +39.7% and +40.0%, both AGREE. The io and offline cells outside
the clause stay recorded with the machine's measured other load (2.6 to 5.3 logical processors); no further recording
is asked of the user. DIAG.11b closes Done; DIAG.11c and DIAG.12 close on the complete-tier GitHub Actions run
dispatched for them ([41319531955](https://github.com/yatiyr/CRD/actions/runs/41319531955)).
