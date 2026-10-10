# DIAG.9c observer-effect experiments, 2026-10-10

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.9c](../ROADMAP.md#slice-diag.9c). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-9c). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.9c minimization](2026-10-10-diag-9c-minimization.md).

## Goal

The contract's "preserve natural uninstrumented failures alongside instrumented traces to detect observer effects",
on the reproduction harness the minimization slice used, as the row planned it: the same failing reproduction with
the flight recorder off, basic and full, keeping the natural failure beside the captures. The flight policy had no
detail levels, so the slice first defines them. Committed locally, not pushed.

## What changed

- `crd/perf/flight_recorder.hpp` and `src/flight_recorder.cpp`: `FlightDetail` (`Full = 0`, `Basic = 1`),
  `flight_detail_name`, `FlightPolicy::detail` (default full) and `FlightRecorder::total_filtered()`.
  `validate_flight_policy` refuses a level that names nothing. At basic, `record()` drops events below
  `Severity::Warning` under the ring mutex before they get a sequence number, counting them; triggers are untouched.
  The CFR1 encoder writes the level into the byte after the record's flags, which was a must-be-zero pad, and the
  decoder reads it there and refuses a value that names no level ("unknown detail level"); `kFlightRecordVersion`
  stays 1, every record written before reads as full, and the 12-seed bundle corpus is byte-identical (default
  policies are full, so their byte is still 0). `flight_capture_summary_json` names the level.
- `crd/perf/diag_assets.hpp` and `src/diag_assets.cpp`: the policy document's optional `detail` key ("full" or
  "basic"; a non-string or another name is refused with the key named); `flight_policy_to_asset` writes it.
  `assets/diag/flight_default.flight.toml` states `detail = "full"` (crd-perf-tests still holds it equal to
  `FlightPolicy{}`). `flight.status` reports `detail` and `filtered`.
- `crd/perf/observer_effect.hpp` and `src/observer_effect.cpp` (new): `ObserverLevel`, `ObservedRun` (failed and the
  caller's signature), `ObserverVerdict`, `ObserverLevelResult`, `ObserverExperiment`, `run_observer_experiment` and
  `observer_experiment_summary_json`. K natural runs with a null recorder come first and must agree and fail; then K
  runs at basic and K at full, each on a fresh recorder whose sink the experiment owns; after each run the recorder is
  polled and shut down, so a still-open window is delivered (marked shutdown). The first capture of each level's first
  run is kept beside `natural`. Run times are kept per level (min and max) and judged by nothing.
- `engine/foundation/toml/src/toml.cpp`: a key defined twice in one table, or in one inline table, is refused at its
  second definition ("duplicate key").

## Findings on the way

- crd-toml accepted a key defined twice and kept the last value: `capacity = 64` followed by `capacity = 1` in a
  flight-policy asset was read as capacity 1, and the asset parser (which reads the parsed table) could not see it.
  This was the queued "duplicate keys in the diagnostic asset parsers" item; the refusal belongs in the TOML reader,
  since TOML forbids a second definition. All 62 committed `.toml` files parse under Python's strict `tomllib`
  without a duplicate, and the whole win-debug ctest passes with the stricter reader.
- No real observer effect was found in the engine's instruments: the recorder allocates its ring and captures from
  the default allocator and never through the allocator a run injects faults into, records into a preallocated ring,
  and its fault evidence is written after the fault decision. An allocation-failure plan therefore counts the same
  hits at every level by construction, which the real-injector experiment shows. The observer-effect verdict is shown
  only on reproductions perturbed on purpose, each labelled synthetic in its test.
- At basic, a frame-hitch capture holds no events: the frames are informational, and the hitch is a trigger, not an
  event. Its trigger carries the interval and the threshold, and the post window is closed by time or shutdown, since
  no kept event arrives to fill it. This is stated in the header, not hidden.

## Tests

crd-perf-tests, new `tests/foundation/perf/test_diag_observer_effect.cpp`, 5 `[diag9c][observer]` cases:

- Detail: a basic recorder records the Warning and the RecoverableError and filters 5 Info events; informational
  events do not fill its post window; the capture's events are all Warning or above and its policy says basic; a
  hitch still triggers through `FlightFrameWatch` with no frame recorded; full records Info as before; a level that
  names nothing is refused by `validate_flight_policy`.
- Record: the level byte round-trips (1 for basic, 0 for full, the old pad), the summary says `"detail":"basic"`, and
  a byte of 2 is refused with "unknown detail level".
- A frame stream with a 60 ms gap at frame 17, judged by its own 33 ms budget, is consistent over 3 runs per level:
  the natural failure (signature 17) is kept, both captures are frame-hitch captures with the 60 ms interval, basic
  recorded 0 events and filtered 24, full kept the 18 frames up to the hitch and 4 after it, then counted 2 more as
  dropped while complete. No fault seam is involved, so this case is consistent on every lane, shipping included.
- An allocation-failure plan (`allocation-failure@7+1`) on a real `FaultInjector` behind a `FaultAllocator`: consistent
  (signature 7); both captures are injected-fault captures at hit 7 with one fault event and a replay recipe; basic
  holds no frame event and filtered 12, full holds 12 frames, and basic's event codes are a subset of full's. With
  `notrigger` the failure reproduces but no level captures it: `instrument-failure` at basic, 0 of 3 captured.
  Synthetic: an instrument that takes one attempt from the allocator under test moves the fault to frame 6 at both
  levels: `observer-effect` at basic. On the shipping lanes the arm is refused, the natural run passes and the
  experiment answers `not-reproduced` with nothing instrumented run.
- Verdicts: synthetic perturbation at full only (`observer-effect` at full, basic agreeing), synthetic instrumented
  runs that pass one time in three (`observer-effect` at basic, 1 of 3 runs differed), synthetic natural runs that
  disagree (`natural-unstable`, nothing instrumented run), a reproduction that passes (`not-reproduced`), a policy the
  recorder refuses (`instrument-failure`, nothing run) and a policy masking the hitch trigger (`instrument-failure`).

crd-perf-tests `[assets]`: the custom policy round-trips with `detail = "basic"`, a document with only `detail` keeps
every other default, and `detail = "verbose"`, `detail = 1`, `detail = "off"`, a policy with `capacity` given twice and
a fault plan with `seed` given twice are refused, each leaving the output untouched.

crd-resources-tests, 1 `[diag9c][observer][cook]` case: the first single failed read the cooker does not recover from
is found by running the natural cook, then the experiment (K = 2) is consistent; both captures are injected-fault
captures at that hit, ended by shutdown and truncated, holding the one fault event that names the file; the cooker
records nothing informational, so basic filtered nothing and the levels keep the same events. On the shipping lanes
the arm is refused and the answer is `not-reproduced`.

crd-toml-tests, 1 new case: a key defined twice at the root (refused, reported at line 3), in a dotted table and in an
inline table is refused; the same key in two tables and in successive array-of-tables elements is not.

## Teeth

Eight breaks on win-debug, each built and observed failing, then restored, rebuilt and green again (the whole win-debug
tree was rebuilt after the last one):

- basic not filtering (Info events recorded at basic): 3 of the 5 `[observer]` cases fail;
- the decoder accepting a level byte that names nothing: the record case fails (the record is still refused later, as
  an out-of-bounds policy, but not as an unknown level);
- the natural outcome not kept: 3 cases fail;
- a level that reproduced without a capture counted as passing: 2 cases fail (the `notrigger` plan and the masked
  hitch trigger);
- any-of-K agreement (a level counted as agreeing unless every run differed): the case whose instrumented runs pass
  one time in three fails;
- the asset parser reading `detail` but not storing it: the `[assets]` round-trip case fails;
- crd-toml accepting a duplicate key: crd-toml-tests' new case and both `[assets]` refusal cases fail;
- the injected-fault event recorded at Info: the real-injector case and the real-cooker case fail (basic drops the
  fault event).

## Verification

- win-debug: whole tree built twice (before the teeth and after the last restore). The whole ctest ran once on the
  stricter TOML reader: 7518 of 7520 passed; the two failures were this batch's own ROADMAP row (an unescaped `|` in
  the row, fixed, `crd-master-plan` passes) and `crd-simd-emission-check`, which needs `dumpbin` on PATH (a vcvars
  environment test, unrelated). After the final rebuild: crd-toml-tests 7 cases, crd-perf-tests 265, crd-resources-tests
  128, crd-sandbox-inspect-tests 31; the 13 `crd-no-*`, `crd-check*`, `crd-master-plan` and `crd-diag-*` CTests; the
  bundle corpus replay (`crd-fuzz-perf-bundle-corpus`), and `--seed` regenerates the 12 seeds byte-identical.
- win-shipping (whole tree built, then the touched targets rebuilt) and win-clang-cl-shipping (reconfigured; the
  touched targets and `crd-sandbox` linked first time): toml 7, perf 132, resources 128, sandbox-inspect 31; the
  `[observer]` cases run on both (the real-injector and cooker cases on their not-reproduced branch).
- win-asan (inside vcvars): toml 7, perf 265, resources 128, sandbox-inspect 31, no sanitizer report.
- WSL, one lane at a time, each reconfigured: linux-gcc-debug (toml 7, perf 263, resources 128, sandbox-inspect 30,
  `crd-sandbox` built; the `[observer]` cases 3 more runs each) and linux-gcc-asan (the same, the only reports the
  three deliberate specimens'). The first linux-gcc-debug build found a real portability error: `-Wformat=2` refuses
  the non-literal format string of a reason helper; the reasons are now built from literal formats.
- linux-clang-tsan (`setarch -R`): toml 7; crd-perf-tests `[diag9c],[diag9b],[assets],[flight]` 48 cases;
  crd-resources-tests `[diag9c],[diag9b]` 11; sandbox-inspect `[flight-host]` 7; the `[observer]` cases 10 more runs
  in each suite; no TSan report.
- Strict tidy is clean on all 12 changed C++ files (two raw-string findings fixed). clang-format differs only on the
  repository's hand alignment, one-line case labels and include grouping. The Allman check, check-master-plan,
  check-repository and check-generated pass.

## Remaining in DIAG.9c

- TTD (user): one elevated recording on this machine, as listed in the reverse-debugging note.
- rr: no eligible host here; an explicit capability limit until a Linux host with hardware counters exists.
- The hosted run, as the row lists it.
