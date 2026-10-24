# DIAG.12: the frame pacing route, 2026-10-24

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.12](../ROADMAP.md#slice-diag.12). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-12),
> [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md). Rules: [AGENTS](../../AGENTS.md).
> Previous batch: [one recorder](2026-10-24-diag-12-one-recorder.md).

## CI read

No completed run has a failed job that main has not fixed. 40128305793, 40128640993 and 40130140033 were cancelled
before any job started. 40122902896 (head `a4500838`) is still running; its four failed jobs are the two causes
`7bee95e8` and `17176125` fix.

## Finding

The previous batch classed DIAG.6c(i)'s `FramePacingAnalyzer` as unwired: only its own tests used it. The
slow-frame runbook already sends a user from a capture to `cprof_export`, so that is the route; no new command,
panel or file.

Wiring it exposed a second defect: the analyzer computed `begin - previous begin`, `end - begin` and
`present - input` as plain i64 differences and summed them unchecked. A capture file is untrusted, and the committed
fuzz seed `capture-frame-span-overflows` (a frame from INT64_MIN to INT64_MAX) would overflow both once the export
fed it.

## Change

- `frame_pacing.hpp`: `span_ns(begin, end)` (0 when reversed, INT64_MAX when the difference does not fit) and a
  saturating sum for every axis. It replaces the exporter's own `duration_ns`, which did the same for spans. A
  `bucket_width_ns()` accessor states the percentiles' resolution.
- `export_perfetto_json` feeds the view's frame records once, into `PerfettoExportStats::pacing`, and writes the
  metadata `pacing` block: `frames`, `intervals`, `mean_interval_ns` (throughput), `p50_interval_ns`,
  `p99_interval_ns`, `max_interval_ns` (pacing), `bucket_ns`, `mean_work_ns`, and `input_to_present: "not recorded"`.
  A capture has no input or present time; UX-QUAL.2's row now names recording them.
- `cprof_export` prints the same numbers from the stats.
- Runbook "A slow frame" (`Read`) and the DIAG.12 design section say where a hitch shows.

## Teeth (win-debug)

- `p99_interval_ns` written from the 50th percentile fails `perfetto export: the metadata carries the frames' pacing
  apart from their work`. The case puts one 30 ms hitch among six frames, so p50 and p99 differ.
- The interval computed as `begin - previous begin` again fails `frame pacing: an untrusted capture's extreme times
  saturate instead of overflowing` (maximum, mean and p99 interval).
- Restored from byte copies, touched, rebuilt: both cases pass.

## Verification

- win-debug: crd-perf-tests, cprof_export and crd-fuzz-perf-capture build; the `pacing`, `perfetto export:` and
  `CPROF ` cases (12) and `crd-fuzz-perf-capture-corpus` pass. The test's capture run through `cprof_export` writes a
  byte-identical document, and `json.load` reads its `pacing` block.
- linux-gcc-debug (`wsl-lane.sh`): the same three targets build with no `warning:` line; the same 13 tests pass.
- Guards: 15 of 15. tidy-files passes the six changed C++ files. clang-format finds no more issues than HEAD in any of the six changed files. Allman,
  check-master-plan and check-repository pass.

Hosted once pushed: the `perfetto export:` and `frame pacing:` cases and `crd-fuzz-perf-capture-corpus` pass on every
lane.
