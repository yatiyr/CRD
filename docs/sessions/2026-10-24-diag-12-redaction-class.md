# DIAG.12: the redaction class on every diagnostic record, 2026-10-24

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.12](../ROADMAP.md#slice-diag.12). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-12) (the event, capture and failure schema),
> [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md) ID-8. Rules: [AGENTS](../../AGENTS.md).
> Previous batch: [privacy across every class](2026-10-24-diag-12-privacy.md), whose two follow-ups this batch does.

## CI read at the start

`gh run list --branch main --limit 15`: 40120050759 and 40117742234 completed cancelled with no job. The only failed
job of a completed or running run is 40103197370's linux-clang-tsan `schedcheck: the failing script minimizes to its
essential tags` (head `6b214076`), fixed in `32adc973`; that run is still in progress (win-relwithdebinfo) and
40122902896 (`a4500838`) is pending. Neither was touched. DIAG.11c names 40103197370 as evidence, so its
linux-clang-tsan clauses now need a push run at or after `32adc973`.

## Follow-up 1: the manifest flag that described nothing

`kManifestFlagFieldsRedacted` said "the manifest's own string fields were redacted", but a manifest holds only
versions, flags and absent tags, and nothing set the bit. It is removed: version 1 defines no flag bit
(`kManifestKnownFlags = 0`), so every set bit is reported as unknown and never interpreted, which is the ID-8 guarantee
that no bit can declare the raw dump safe. The golden bundle writes flags 0 and is unchanged.

## Follow-up 2: the schema's redaction class

The schema paragraph lists a redaction class on every record; only DIAG.8b's inspected values had one. Implemented,
not amended:

- `crd::RedactionClass` moves to `crd/core/redaction.hpp` (new file: crd-ceir does not link crd-perf, and crd-core is
  the one library both reach), with `redaction_class_name`; `crd::ceir::inspect::RedactionClass` is an alias.
- `FlightEvent`, `FlightTriggerRecord`, `FlightTriggerInput` and `DiagnosticEvent` carry it. A Restricted one keeps
  code, severity, source and scalars and no text: `publish()` and `trigger()` never copy it, so no route out of the
  ring (bundle, `bundle.inspect`, `flight.status`, the summary JSON) can carry it; `make_event` keeps no message;
  `record(DiagnosticEvent)` and the fatal handler pass the class on. `FlightEvent` stays 152 bytes: its message length
  is a byte (at most 111), which pays for the class.
- The record format keeps version 1: the event's former clip byte is a flags byte (bit 0 clip, bit 1 restricted; older
  records hold 0 or 1), the trigger's class is record flag bit 3. The decoder refuses unknown event bits, a restricted
  event with text or a clip flag, and a restricted trigger with a message. The renderers print `redaction` next to each
  message; `to_log_line` marks `[restricted]`.
- CPROF captures hold instrument identifiers (thread, counter, allocator and zone names) that the program registers;
  they are Public by kind and carry no field.
- The same application: its last order names its customer and is recorded Restricted. `check_private` now also holds
  every bundle the application leaves free of the customer name; `check_orders` checks the order's class and number.
- `crd-fuzz-perf-flight`'s oracle requires a restricted event or trigger to hold no text; three seeds are added (a
  recorder capture with both, and each forged with text, refused). The other 30 seeds regenerate byte-identical.

## Teeth (win-debug)

Each break was built, run, restored, rebuilt and rerun (`build/diag12-redaction/teeth.py`, not committed).

| Break | Fails |
|---|---|
| `publish()` copies a restricted text | the restricted-record case; the application case (the customer name in the bundle, then the decoder refuses it) |
| decoder takes unknown event bits | the restricted-record case (bit 2 accepted) |
| decoder takes a restricted event with text | the restricted-record case; `crd-fuzz-perf-flight-corpus` (oracle at the forged seed) |
| `trigger()` copies a restricted text | the restricted-record case |
| decoder takes a restricted trigger with text | the restricted-record case |
| `make_event` keeps a restricted message | the restricted-record case |
| encoder drops the event's class | the restricted-record case |

## Verification

- win-debug: crd-perf-tests, crd-diag-host-specimen, crd-fuzz-perf-flight, crd-diag-flight-fatal-specimen,
  crd-ceir-tests and crd-diag-overhead built; 148 CTests (flight, diagnostic host, manifest and golden bundle, `diag:`,
  `diag 8b`, `doctor:`, `diag commands`, the flight corpus) pass after the JSON expectations gained `redaction`.
- linux-gcc-debug (`wsl-lane.sh build`/`test`): the five targets built `-Werror` with no compiler warning; the same
  set, 120 CTests, passes (the two `diag 11a` window cases skip headless, as before).
- `tidy-files.py` clean on all 17 files; `clang-format --dry-run` on the added lines reports only the house
  declaration alignment and the file's lambda style; `check-headers.py` on the five changed headers,
  `check-allman-braces.py`, the ctest guards, `check-master-plan.py` and `check-repository.py` pass.

Hosted once pushed: `flight record: a restricted event or trigger keeps its class and scalars, never its text`, the
application cases and `crd-fuzz-perf-flight-corpus` pass on every lane.

## Remaining in DIAG.12

Removal of the remaining obsolete scaffolding and duplicated services; the re-read of every child and DG01-DG18
against final source and published CI.
