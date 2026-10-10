# DIAG.9b refusal of external I/O replay, 2026-10-10

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.9b](../ROADMAP.md#slice-diag.9b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-9b). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.9b install and teardown faults](2026-10-10-diag-9b-install-teardown-faults.md).

## Goal

The eighth slice of DIAG.9b: the acceptance clause "shipping capability checks reject injection and arbitrary
external I/O replay". The injection half landed with the [fault injection](2026-10-10-diag-9b-fault-injection.md)
slice (a shipping build compiles the seams out and every arm is Unavailable). This batch is the external I/O half.
Still open after it: a real host (crd-sandbox) and the hosted run. This batch is committed locally and not pushed.

## What was there (checked before coding)

- No build can feed external results to a replay. A run record (`crd/ceir/cook/replay_record.hpp`) has no section
  for external I/O completions or opaque call results; `input::InputKind` has no external kind and `InputFeed` only
  answers random, clock, time-step and event reads; `record_inputs` never stores `external-results` as recorded.
- The refusal was claim-based. `replay.run` refused a record only when one of the record's own input states said
  `missing` (`record_missing_inputs`); the needs were computed from the program at record time and stored in the file
  (`analyze_needs` ran in `replay.record` and the inspect host, never in `replay.run`). The checksum is integrity, not
  authenticity. So two edited records replayed unchecked:
  - a record whose `external-results` said `recorded` (with any need): it passed the check and ran;
  - a record whose own program needs external results (an op whose effective effects include FileIO, NetworkIO,
    DeviceIO, ExternalCall or AgentAction, or an opaque op) but whose `external-results` said `not-needed`.
  `replay.run program=<path>` replayed a record's inputs against any program under the root without asking what that
  program needs, and `replay.prepare` on a record answered `external-results` "available" when the record said so.
- The same held on all three executors: the plan path in `run_replay_run`, the host path (`replay_on_host` hands the
  record to the host executor, which crd-ceir-host or the sandbox supplies) and the device path
  (`replay_device_record`).
- No engine op declares an external-results effect yet (the DIAG.9a row records this), so today such a program also
  fails the plan compiler as `unsupported-op` and a device program refuses it as not a device op. That refusal is an
  accident of the op having no kernel; it is not a capability decision, and it disappears the day an op that reads a
  file is registered with a kernel.

## What changed

- **crd-ceir-cook**, `check_external_replay` (`replay_record.hpp`, in `replay_needs.cpp`): from the program's own ops
  (the same effect walk as `replay.prepare` and `replay.record`), Allowed when it needs no external results, otherwise
  Refused with the program, the number of ops and the first op at its authored file:line:col, or Cancelled. Its need
  is never taken from what a record says.
- `replay.run` asks it before anything runs, on every executor and for every program a replay would run:
  - plan: the record's own program once its content hash is checked, and the `program=` program before its plan is
    compiled (so the refusal names external I/O, not a missing kernel);
  - host: the record's blob and the `program=` program, loaded with the host's dialects, before the host executor is
    called;
  - device: `replay_device_record` checks the record's program and the `against` program after the content checks
    (`MissingInputs`).
  A refusal is Unavailable ("incompatible replay: the record's program needs external results (1 op, first xio.read
  at diag9b_ext_read.ceir:18:9); no build replays external I/O: a run record holds no external I/O completions or
  opaque call results").
- `record_missing_inputs` counts `external-results` missing unless the record says it is not needed and not held, so
  a record claiming them recorded is refused on every executor, and `replay.prepare` never answers them available
  from a record.
- **crd-perf**, the doctor reports a new mode `external-io-replay`: never compiled, enabled or usable, in any build,
  with the disposition saying why; `diag.capabilities` carries it. This is the capability check a reader of a
  shipping build sees beside `fault-injection` (compiled out there).
- Contracts updated: `replay_diag.hpp` (replay.run's refusals), `replay_record.hpp`, `device_replay.hpp`
  (`MissingInputs`), `doctor.hpp`.

## Tests

- **crd-ceir-cook-tests**, `test_replay_record.cpp`, 1 new case (`[diag9b]`): *no build replays external I/O; a
  record or program that needs external results is refused before anything runs*. The host registers a test op
  `xio.read` declaring FileIO (no engine op does); the committed `replay_demo.ceir` is recorded honestly, and the
  edited program reads its bias with `xio.read` instead of a constant. A stub host executor counts what reaches it.
  - the honest record says `external-results` not needed and replays (`reproduced`): the valid control;
  - four claims (`yes/recorded`, `no/recorded`, `unknown/recorded`, `yes/not-needed`) are each refused Unavailable as
    missing `external-results`, with nothing run;
  - a record whose blob is the file-reading program, claiming `no/not-needed`, is refused naming the record's program
    and `xio.read` at the line and column scanned from the text; `replay.prepare` on it (claiming `yes/recorded`)
    answers `external-results` missing and `replay: unavailable`;
  - the honest record replayed with `program=` the file-reading program is refused naming the given program;
  - a host record of the demo reaches the stub executor (control); the same with `program=` the file-reading
    program, and a host record whose blob is that program, are refused with the stub not called again.
- **crd-ceir-cook-tests**, `test_replay_device.cpp`, 1 new case (`[diag9b]`): the device fixture's exact record
  replays on a counting CPU reference executor (control, 1 run); a record whose blob is the device program with an
  `xio.read` added, the honest record replayed against that program, and a record claiming `external-results`
  recorded each return `MissingInputs` with the program and op named, and the executor runs no more.
- **crd-perf-tests**, `test_diag_flight_commands.cpp`, the doctor case now also checks `external-io-replay` is not
  compiled, enabled or usable, in the report and in `diag.capabilities`, on every lane.

Teeth on win-debug, each observed failing, then restored by copying the saved source back, touching it and
rebuilding (the restored suites then passed again: crd-ceir-cook-tests 105 cases, crd-perf-tests 255):
- the `external-results` rule removed from `record_missing_inputs`: the four claims replay (status Ok, executions
  grow) and the device claim runs;
- the host path's two checks removed: the stub host executor is reached for both refused host replays;
- the device path's two checks removed: the device cases fail (the forged program is refused only as not a device
  program, and the claimed record runs);
- `replay.prepare` answering a record's `external-results` available again: the prepare checks fail;
- the plan path's two checks removed: the record's and the given program are refused only as
  `entry did not compile: unsupported-op` (Failed), not as external I/O;
- the doctor mode reported compiled: the doctor case fails in the report and in `diag.capabilities`.

## Verification

- win-debug: the whole tree built; crd-ceir-cook-tests 105 cases, crd-perf-tests 255, crd-ceir-host-tests 47,
  crd-sandbox-inspect-tests 24, crd-ceir-gpu-tests 140, crd-ceir-gpu-vulkan-tests 60 and crd-ceir-gpu-dx12-tests 47
  (both on this machine's GPU) green, and crd-ceridc-tests 20 with the rebuilt `ceridc` from a scratch folder.
- win-shipping: the whole tree built; crd-ceir-cook-tests 105 (the two `[diag9b]` cases with the same 152 assertions
  as win-debug: nothing is compiled out), crd-perf-tests 122, crd-ceir-host-tests 47, crd-sandbox-inspect-tests 24
  and crd-ceridc-tests 20 green.
- win-clang-cl-shipping (the same consumers, `ceridc` and `crd-sandbox` rebuilt; every link succeeded first time):
  the same five suites green with the same counts.
- win-asan (inside vcvars): crd-ceir-cook-tests 105, crd-perf-tests 255 and crd-ceir-host-tests 47 green; the only
  AddressSanitizer reports are the three deliberate specimens' (heap overflow, use after free, crash capture).
- WSL, one lane at a time, from a scratch folder under the home directory: linux-gcc-debug (crd-ceir-cook-tests
  105, crd-perf-tests 253, crd-ceir-host-tests 47, crd-ceridc-tests 20, crd-sandbox-inspect-tests 23),
  linux-gcc-asan (the same but sandbox-inspect; the only reports are the heap-overflow, use-after-free,
  use-after-return and leak specimens', no UBSan report) and linux-clang-tsan under `setarch -R` (the same four
  suites, no TSan report). The crd-ceir-cook `[diag9b]` cases and the crd-perf `[doctor]` cases then passed 3 more
  runs on each gcc lane and 10 under TSan.
- Strict tidy clean on all 13 changed C++ files (one local-constant naming finding fixed). clang-format differs only
  on the repository's hand alignment, line-break choices and include grouping. The Allman check, the 8 ctest guards,
  `check-master-plan` and `check-repository` pass.

## What remains in DIAG.9b

- A real host (crd-sandbox) installing the recorder, its triggers, a `FlightCommandHost` sink (with
  `refresh_modules` at start and after module loads) and both command sets.
- The hosted run: in addition to the earlier list, crd-ceir-cook-tests with the two `[diag9b]` cases and
  crd-perf-tests' doctor case (with `external-io-replay`) green on all six lanes, the refusals passing unchanged on the
  shipping lanes.
