# DIAG.9a clock at the interactive hosts, 2026-10-08

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.9a](../ROADMAP.md#slice-diag.9a). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-9a). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.9a clock and time-step inputs](2026-10-08-diag-9a-clock-inputs.md).

## Goal

Give the interactive hosts (`program.inspect`, `ceridc inspect` and the crd-sandbox inspect panel) the host clock that
`replay.record` already takes, so a time-dependent program can be debugged and its inspected run recorded with every
time read; and give the sandbox panel the frame loop's own clock. DIAG.9a stays Partial.

## What was there (checked before coding)

- The inspect host already takes any input source (`InspectHost::start(args, recording, inputs)`) and records every
  read of a recorded run, `clock` under the same rule as `random` (the previous batch's inspect-host clock case).
- The three interactive consumers built only a `SeededInputs`, so a program that read the clock failed
  `input-unavailable` there.
- `replay.record` parsed `clock`, `sim_time` and `sim_step` itself, and both it and the host executor assembled the
  seeded source, the `HostClock` and the `InputRouter` by hand.
- `input::HostClock` holds plain fields and is "changed only while no run reads it". The sandbox panel's controller is
  the frame loop and the program runs on the host's own thread, so the frame loop cannot write the clock while a run
  is attached.

## What changed

- **Parser** (`crd/ceir/cook/replay_diag.hpp`): `parse_clock_argument(name, value, spec, requirement)` answers
  `NotClock`, `Ok` (set in a `HostClockSpec` when one is given) or `Refused` with what the value must be. `replay.record`
  now uses it; its refusal texts are unchanged. The built-in domain ordinals the hosts set are named
  (`kWallDomain`, `kSimDomain`, `kFrameDomain`).
- **Input bundle**: `cook::RunInputs` owns a `SeededInputs` on its own `GrowableTlsfAllocator`, a `HostClock` and an
  `InputRouter` (Random to the seeded source or none, Clock and TimeStep to the clock). `set(seeded, seed, spec)`
  starts the next run over (streams from draw 0, the clock from no reading); `clock()` lets a host set more domains
  between runs. Declared before the host, it outlives the run.
- **`program.inspect`**: `clock=`, `sim_time=` and `sim_step=`, checked in the service's argument step. The summary
  answers `wall_clock`, `sim_time`, `sim_time_ns`, `sim_step` and `sim_step_ns` after `seed` (the fields
  `replay.record` answers, through one private helper), so three existing summary assertions gained the fields.
- **ceridc**: `inspect --clock wall --sim-time <ns> --sim-step <ns>` (`InspectClockFlags`, refused before anything
  runs; the report names the same five fields).
- **crd-sandbox**: `PanelInputs::clock` (`--inspect-clock`, `--inspect-sim-time`, `--inspect-sim-step`), applied at
  every `start`, "Run again" included; `PanelInputs::frame_clock` (`--inspect-frame-clock`): the frame loop calls
  `set_frame_clock(time_ns, step_ns, frame)` every frame and `start` copies it into the run's clock (sim reads the
  loop's time and last frame step in nanoseconds, the frame counter's under `--fixed-dt`; frame reads the frame index
  with a step of 1). The copy happens only in `start`, on the controller thread before the run's thread exists, so a
  run held across frames keeps reading its start's clock; a live per-frame clock would need a per-domain reader that
  is safe on the executing thread. The first run starts before the first frame (time 0, frame 0, the fixed step or 0).
  The frame clock replaces the sim domain, so the sandbox refuses it together with `--inspect-sim-time` or
  `--inspect-sim-step`. A malformed clock flag exits 1 before anything runs.

## Tests

Positions come from scanning the program text (`assets/ceir/clock_demo.ceir`, or the test's own app program).

- `tests/execution/ceir-cook/test_replay_clock.cpp` (1 new case, 10 `[clock]` cases): the parser refuses eight
  malformed values with a requirement and touches no field, accepts the extremes, and validates without a spec;
  `RunInputs` with nothing set delivers nothing, with a seed and a sim spec delivers the seed's draws and the sim
  reading and step (twice, each `set` from draw 0), with a live wall only drops the earlier sim domain, and takes a
  frame domain set on its clock; the named ordinals match `time::builtin_domain_index`.
- `tests/execution/ceir-cook/test_inspect_diag.cpp` (1 new case, 4 new refusals): a step over budget held at the
  switch answers the given step and fails there; a step within budget with a sim reading and the live wall finishes
  on their sum (the watched sim reading is the given one); no wall fails `input-unavailable` at the wall read; no clock
  fails at the step read; `clock=frame`, `sim_time=x`, `sim_step=1.5` and `sim_time=2^63` are bad-argument with nothing
  run.
- `tests/tools/ceridc/test_ceridc_inspect.cpp` (1 new case): the verb's held step, its record of one read, no clock
  failing, six malformed flags refused before running; the real binary records the step failure and a live-wall run
  that finishes; the program file is edited (the step read asks for the frame domain); other `ceridc` processes
  reproduce all three records with no clock, and against the edited file the step failure is an `input` divergence at
  the step's line.
- `tests/applications/sandbox-inspect/test_inspect_panel.cpp` (1 new case): the panel's clock (a sim step over
  budget) is the held switch's watched step, the run fails, and its record (one read) replays from that read alone; a
  frame-clock app program reads the frame index, a frame step of 1, the loop's time and step as they were at its
  start although the loop moved on while the run was held, and "Run again" reads the loop's new clock.

## Teeth (win-debug)

Each break was applied, the suites it touches rebuilt (and `ceridc` where the binary runs it), the relevant cases run
from a scratch folder under the lane, and the source restored by rewriting it. After the last tooth every touched
target was rebuilt and every suite passed again.

| Break | Result |
| --- | --- |
| the parser accepts any `clock` value | 2 of 10 cook `[clock]` cases and the ceridc clock case fail |
| `RunInputs::set` keeps the streams going | the `RunInputs` case fails |
| `RunInputs::set` applies no clock | 2 cook cases and the panel case fail |
| no TimeStep route | 2 cook cases fail |
| `program.inspect` drops the parsed clock | the `program.inspect` clock case fails |
| `program.inspect` answers no clock fields | the `program.inspect` clock case fails |
| the ceridc verb drops the clock | the ceridc clock case fails |
| ceridc's `main` passes no clock flags (the binary rebuilt) | the ceridc clock case fails |
| the panel drops its clock | the panel case fails |
| the panel ignores the frame clock | the panel case fails |
| the frame domain steps by 0 | the panel case fails |
| `set_frame_clock` writes into a held run's clock | the panel case fails |

## Evidence

Final sources, every lane built from them. Suites: `crd-ceir-cook-tests` 85 cases (4,447 assertions),
`crd-ceir-host-tests` 46 (2,025), `crd-ceridc-tests` 17 (708; run from a scratch folder under the lane with
`CRD_CERIDC_EXE` set) and `crd-sandbox-inspect-tests` 8 (317); `ceridc` and `crd-sandbox` built on every lane.

- **win-debug**: every suite passes with those counts; through ctest the 8 repository guards pass.
- **win-shipping, win-clang-cl-shipping and win-asan**: each lane built the four suites, `ceridc` and `crd-sandbox`
  and passed every suite with the same counts. The clang-cl links were clean the first time; win-asan ran inside
  vcvars with no ASan report.
- **WSL** (linux-gcc-debug, linux-gcc-asan, linux-clang-tsan with the hosted lane's `TSAN_OPTIONS`): every lane built
  the same targets and passed the four suites with the same counts; no ASan, UBSan or TSan report. On the TSan lane
  the cook `[clock]` cases (10), the panel suite (8) and the ceridc `[inspect]` cases (6) then passed three more runs
  each with no report.
- **crd-sandbox on this machine's GPU** (win-debug, `CRD_ASSETS_DIR` set, run from a scratch folder that was then
  deleted): `--smoke-test 4 --inspect ceir/clock_demo --inspect-arg 7 --inspect-sim-step 50000000 --inspect-break 8
  --inspect-watch 5 --inspect-step continue --inspect-record sbx_clk.crpl` stopped at `8:9` with the watched step
  50000000, failed `selector-out-of-range` and wrote the record (4 events, 1 input read); `ceridc diag --command
  replay.run` on it answered `reproduced` with 1 recorded and 1 replayed read. `--fixed-dt 16.666667
  --inspect-frame-clock --inspect-clock wall` finished with 880107 (the live wall plus sim time 0 plus 7; the first
  run starts before the first frame with the fixed step 16666667, within budget) and its record (3 reads) answered
  `reproduced`. `--inspect-clock frame`, `--inspect-sim-time 1.5` and `--inspect-frame-clock --inspect-sim-step 5`
  each exited 1 before running. Both smoke runs passed (206 and 215 frames).
- **Checks**: strict tidy (LLVM 20) is clean on 14 of the 15 changed C++ files after three fixes (a nested
  conditional in the sandbox's flag parsing, and local constants renamed to lower case in two tests). The 15th,
  `sandbox/src/inspect_panel.hpp`, is reported ungated, as in the previous batch: the tool's standalone pass does not
  resolve its includes; its TU and the test TU are clean and it builds under `/W4 /WX` on every Windows lane.
  clang-format shows only the repository's hand alignment, operator-first wraps the surrounding sandbox code uses and
  the existing declaration style of `verb_inspect`; its other wraps were applied by hand, and no added line is over
  120 columns. The Allman check, check-master-plan and check-repository pass.

## Hosted CI must show

- `crd-ceir-cook-tests` (85, with the 10 `[clock]` cases), `crd-ceridc-tests` (17, with the real binary's
  `inspect` clock flags and its records reproduced by other processes) and `crd-sandbox-inspect-tests` (8, with the
  clocked panel case) green on all six lanes, with `crd-ceir-host-tests` (46) still green and `ceridc` and
  `crd-sandbox` building on every lane.

## Remaining for DIAG.9a

- State cells across invokes and a run spanning a reload: they need a host that keeps one interpreter across invokes.
- Input events and external I/O completions: their ops must be specified first.
- Backend-specific numeric replay of GPU dispatches with a declared tolerance or oracle.
- Network and physical effects stubbed only in an explicit test replay.
- Host records from crd-sandbox's panel: the request would have to run on a thread the pool enrolled.
