# DIAG.9b authored policy and plans, flight commands and doctor modes, 2026-10-10

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.9b](../ROADMAP.md#slice-diag.9b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-9b). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.9b device loss](2026-10-10-diag-9b-device-loss.md).

## Goal

The fourth slice of DIAG.9b: the flight policy and fault plans as versioned authored assets, the `flight.*`
diagnostic commands, and the doctor reporting the flight-recorder and fault-injection modes. Everything is crd-perf,
so the next slice (a real host, crd-sandbox) can install the recorder, its sink and both command sets. Still open
after this batch: the build and symbol identity sections of a flight bundle, injection during cook, install, execute
and teardown, refusal of arbitrary external I/O replay as a shipping check, the host, and the hosted run. This batch is
committed locally and not pushed.

## What was there (checked before coding)

- The design's shared rule: diagnostic policies are versioned authorable assets on the existing config/resource/CEIR
  paths, validated while healthy, last-known-good on a bad reload. `FlightPolicy` and `FaultPlan` already had their
  own validators and a versioned in-code form; `FaultPlan` also a round-tripping text grammar.
- The existing config path is crd-toml (crd-core and crd-containers only; crd-config would also pull in crd-log,
  crd-math and crd-platform). Assets the cooker has no handler for are logged `no_handler` and left alone.
- The doctor (`run_doctor`) is census-sourced with three live probes and no access to any live instance;
  `diag.capabilities` calls it without arguments.
- Once a recorder has a sink, `take_capture` finds nothing: a capture is delivered to the sink and gone. Something
  has to keep the last capture for a command to read or write it.
- `kDiagMaxArgValueBytes` is 512, while a valid plan's text may reach 800 bytes (`kFaultMaxPlanText`): a long plan
  could not be armed through `fault.arm plan=` at all.

## What changed

- **Authored assets** (`crd/perf/diag_assets.hpp`, new; crd-perf now links crd-toml, declared in `crd_module(perf)`):
  - `parse_flight_policy_asset`: `kind = "flight-policy"`, `schema_version = 1`, then any of `capacity`,
    `pre_events`, `post_events`, `post_window_ns`, `hitch_threshold_ns` and `triggers` (an array of trigger names, or
    `["all"]`). Absent keys keep the in-code defaults.
  - `parse_fault_plan_asset`: `kind = "fault-plan"`, `schema_version = 1`, `seed`, `trigger_flight` and `rules` (rule
    strings in the plan-text grammar). The document is assembled into plan text and read by `parse_fault_plan`, so
    the asset and `fault.arm plan=` share one reader. A rule containing `;` (which would smuggle in another field) or
    no `@` (so a bare `notrigger` cannot become the plan's flag) is refused.
  - Every refusal names its key: an unknown key, a wrong type (a float or a table where an integer belongs), a negative
    or oversized integer, an unknown trigger, a missing or wrong `kind`, a missing or other `schema_version`, invalid
    TOML (with line and column), and a policy or plan that fails its own validation. A refusal leaves the output
    untouched. `flight_policy_to_asset` writes the canonical document, which reads back to the same policy.
  - `read_diag_asset_file` reads at most 16 KiB and refuses a larger file (`too-large`) or a missing one.
  - Committed assets: `assets/diag/flight_default.flight.toml` (held equal to `FlightPolicy{}` by a test) and
    `assets/diag/allocation_failure_once.fault.toml`.
- **Flight commands** (`crd/perf/flight_commands.hpp`, new). `FlightCommandHost` is the recorder's sink: it keeps a
  copy of the last delivered capture under its own mutex and passes each capture to an optional forward sink.
  `register_flight_commands` adds, owner `perf`:
  - `flight.status` (Read): the recorder's state and active policy (generation, bounds, armed triggers, recorded,
    pending, fatal, delivered) and the retained capture (trigger, message, end, pre and post windows, loss,
    suppressed triggers, replay) as the summary, with its events as items.
  - `flight.trigger` (Record): a manual trigger, optional `message=`. Suppressed (a capture pending) and Masked (the
    policy does not arm manual triggers) are Failed with the reason; an uninitialised recorder is Unavailable.
  - `flight.policy` (Record, request path): reads and parses the asset, then `reload_policy`. A refused document, a
    missing or oversized file and a refused reload (busy while a capture is pending) all keep the active policy, and
    the reason says which generation stays.
  - `flight.write` (Record, request path): collects a capture whose post window closed (`poll`), then writes the
    retained capture as a flight bundle through `write_bundle_atomic`, never over an existing file.
- **`fault.arm asset=<path>`**: the alternative to `plan=`, a fault-plan asset at a `diag_path_is_safe` path joined to
  the host's root (Unavailable without a root). It is also the route for a plan whose text exceeds the 512-byte
  argument bound.
- **Doctor**: `run_doctor(output_dir, const DoctorLive&)` with `DoctorLive{flight_recorder, fault_injector}`; the
  default argument keeps every existing call. Two modes are appended (existing modes are unchanged):
  - `flight-recorder`: compiled always; enabled when a recorder is installed and initialised; usable when enabled and
    no capture ended on a fatal path.
  - `fault-injection`: compiled when the build has fault seams (`crd::fault_seams_compiled()`); enabled when a plan is
    armed; usable when compiled and an injector is installed.
  `DiagServiceConfig` gains `flight_recorder` and `fault_injector`, which `diag.capabilities` hands to the doctor.

## Tests

New `tests/foundation/perf/test_diag_flight_commands.cpp`, 7 cases in crd-perf-tests (`[diag9b]` with `[assets]`,
`[flight][commands]`, `[fault]` or `[doctor]`):

- The committed default policy equals `FlightPolicy{}`; a custom policy round-trips through `flight_policy_to_asset`;
  a document with only the header gives the defaults.
- 16 refused policy documents, each checked for its reason and for an untouched output.
- The committed fault plan equals the plan its text names; a quiet two-rule document equals its `notrigger` text; 10
  refused plan documents (unknown key, negative seed, non-boolean flag, `;` smuggling, bare `notrigger`, invalid
  probability, no rules, a malformed rule, nine rules, wrong kind).
- The asset reader refuses a 16385-byte file and clears its output, and reports a missing file.
- The command service end to end with a Read+Record grant: status before any capture; `flight.write` with nothing
  delivered; `flight.trigger message=agent probe` starts a capture; a second trigger is suppressed; `flight.policy`
  is refused busy and keeps generation 1; the post window fills; `flight.write` polls and writes; the file read back
  through `read_bundle` and `decode_flight_record` holds the trigger message, 8 + 2 events (sequences 4 to 13) and one
  suppressed trigger; a second write is refused and the file's bytes are unchanged; `flight.status` lists the 10
  events; a document with `pre_event` is refused naming the key and keeps generation 1; a missing asset is Failed; the
  good document commits generation 2 (capacity 64, triggers assertion and injected-fault), after which a manual trigger
  is refused as masked; the service refuses `../escape.cdb` and a missing path before any handler runs.
- A Read grant: `flight.trigger`, `flight.policy` and `flight.write` are Unauthorized before any handler runs and
  nothing is triggered; `flight.status` answers. A host whose recorder never initialised answers status and refuses a
  trigger as Unavailable.
- `fault.arm asset=`: `../diag/...` refused before any handler; both arguments refused; a missing asset Failed; the
  committed asset armed with the plan `seed=1;allocation-failure@0+1%1000000` (Unavailable on a shipping lane); a
  service with no root refuses it as Unavailable.
- Doctor: with no live instances both modes are not enabled and not usable, and `fault-injection.compiled` equals an
  independent probe (whether `install_io_fault_hook` succeeds), not `fault_seams_compiled()`; installed but not
  initialised, the recorder is not enabled; initialised and armed, both are enabled and usable (fault injection only
  where the seam installs); `diag.capabilities` reports the same through the service config; after `finish(Fatal)` the
  recorder is enabled and not usable.

### Teeth

Each break was applied with a marked one-line edit, the lane rebuilt and the tests run:

| Break | Lane | Result |
| --- | --- | --- |
| An unknown flight-policy key is accepted | win-debug | the refused-document case (3 checks) and the command case's refused reload (7 checks) fail |
| `flight.trigger` declared Read | win-debug | the read-grant case fails (Unauthorized, handler runs, nothing triggered) |
| `fault.arm` asset path not checked | win-debug | the traversal refusal fails (`../diag/...` is read and armed) |
| A rule containing `;` accepted | win-debug | the smuggling refusal fails (4 checks) |
| `flight.write` overwrites | win-debug | the overwrite refusal and the byte comparison fail (3 checks) |
| `flight.write` does not poll | win-debug | the first write fails: nothing delivered |
| `fault-injection.compiled` always true | win-shipping | the doctor case fails (5 checks) |

The first four were applied together (each failure is in its own case or lines); the rest one at a time. After
restoring, win-debug was rebuilt (the whole tree) and win-shipping rebuilt, and both suites passed.

## Verification

- win-debug: the whole tree built; crd-perf-tests 246 cases green; crd-perf-ui-tests (21) and
  crd-sandbox-inspect-tests (24), which drive the command service, green.
- win-shipping and win-clang-cl-shipping (both reconfigured): crd-perf-tests 113 cases green, the compiled-out
  branches of `fault.arm asset=` and the doctor seen passing.
- win-asan (inside vcvars, reconfigured): crd-perf-tests 246 cases green.
- WSL, one lane at a time, each reconfigured: linux-gcc-debug 245 cases green; linux-gcc-asan 245 cases green (the
  only sanitizer reports are the deliberate specimens'); linux-clang-tsan (`setarch -R`) the `[diag9b]`, `[doctor]`
  and `[commands]` cases, 45, green with no TSan report.
- Strict tidy clean on all 11 changed C++ files (one finding fixed: a raw string literal; the first run was a
  clang-tidy access violation that did not recur). clang-format differs only on the repository's hand alignment and
  include grouping. The Allman check, the 8 ctest guards, `check-master-plan` and `check-repository` pass.

## What remains in DIAG.9b

- The build and symbol identity sections of a flight bundle (a live loaded-module enumeration does not exist yet:
  Windows identities come only from minidumps, and the Linux build-id capture is private to `crash.cpp`).
- Injection during cook, install, execute and teardown with the previous assets surviving.
- Refusal of arbitrary external I/O replay as a shipping capability check.
- A real host (crd-sandbox) installing the recorder, its triggers, a `FlightCommandHost` sink and both command sets
  (the 7 commands must fit the service's 32-command table beside those the sandbox registers today).
- The hosted run.
