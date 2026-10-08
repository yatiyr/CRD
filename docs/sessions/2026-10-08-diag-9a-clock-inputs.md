# DIAG.9a clock and time-step inputs, 2026-10-08

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.9a](../ROADMAP.md#slice-diag.9a). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-9a). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.9a live inputs at the inspect host](2026-10-08-diag-9a-inspect-host-inputs.md).

## Goal

The eighth DIAG.9a batch: record the clock and time-step inputs the contract lists ("clock/time-step inputs") at
the host input seam, so a failure that depends on the host's time step reproduces in a later process from the record
alone, and a result that depends on the live wall clock replays to the recorded value. The previous batch had left
them open because no op read a clock and the units and time domains were unspecified. DIAG.9a stays Partial.

## What was there (checked before coding)

- The input seam (`crd/ceir/input.hpp`) already carried a kind and a channel per read, the run record stored the kind
  byte and refused one past `kLastInputKind`, and `InputRecorder` / `InputFeed` were kind-agnostic. Only
  `input.random` existed, and `reads_input` matched it by name.
- `replay_needs.cpp` classified `clock` from `TimeRead`, but `record_inputs` (and the inspect host's bound check)
  only ever stored `random` as recorded, so a clock read could only be missing.
- The time dialect (`crd/ceir/time.hpp`, CEIR-8f) already names six built-in domains: wall, sim, frame,
  audio_sample, sequencer, logical. That is the vocabulary the new ops use, not a new one.
- `TimeRead` is a pure read in `hazard.hpp` (unlike `RandomRead`, a write), so two time reads are not ordered by the
  hazard analysis. Checked whether that matters for a position-based feed: both executors run a block in list order
  (`exec.cpp` "list order", the plan's sequences), the cook runs no transform (`program_cook.cpp`), and no transform
  on the host executors' path consults the hazard analysis (`transform.cpp` does not); a replay runs the record's
  own blob. So the read order a record keeps is the order it feeds back. Recorded in the op's docs and the contract.
- crd-ceir links no clock (crd-time is not among its dependencies); crd-ceir-host links crd-time, crd-ceir-cook did
  not.
- The plan profile's per-op arrays (`plan_perf.cpp`, `kMaxOps` 32) are the only table indexed by `plan::Op`; its
  static assertion names the last member.

## What changed

- **Ops** (`input.ceirop.toml`, opgen): `input.clock {domain}` (a domain's current reading) and
  `input.time_step {domain}` (the length of its current step). `domain` is a built-in domain's name; its ordinal
  (`time::builtin_domain_index`, new with `builtin_domain_name` and `kBuiltinDomainCount`, append-only) is the seam
  channel. The value is the host's raw i64: nanoseconds for wall and sim, ticks otherwise; the wall is the host's
  monotonic clock from the clock's epoch, never calendar time. Effects `TimeRead`, determinism
  `ExternalNondeterminism`, native provider host. A bad domain is `UndefinedValue` at eval and `BadConst` at plan
  compile, like a bad `input.random` attribute.
- **Seam**: `InputKind::Clock` and `InputKind::TimeStep` (appended); `input_kind_name` answers the op names
  (`clock`, `time_step`); `reads_input` knows the three ops; `time_attrs` validates the domain.
  `input::HostClock` holds a reading and a step per built-in domain (`set_reading`, `set_step`, `advance` with
  wrapping, `clear`) and can make the wall live from a host-supplied monotonic reader (`use_live_wall`); unset
  domains, other channels and other kinds have no value. `input::InputRouter` sends each kind to its own source.
  Neither allocates.
- **Executors**: the reference interpreter installs `eval_clock` / `eval_time_step`; the plan compiles them to
  `Op::Clock` / `Op::TimeStep` with the domain in the immediate (the profile's assertion moved to `TimeStep`). The
  parallel-body pre-flight and the host provider's inline-launch rule already key on `reads_input`, so they cover the
  new ops unchanged.
- **Records**: schema 4 stands. No layout changed, and an older reader already refuses a kind past its last. The
  decoder now also refuses a time read whose channel is past the built-in domains. `is_seam_input` makes `clock`
  recorded under the same rule as `random` (every read held, every `TimeRead` op through the seam), in
  `record_inputs` and in the inspect host's bound check. A `Divergence` of kind `input` now carries the recorded and
  requested kinds, and `replay.run` answers them as `recorded_input` and `observed_input`: a clock read and a step
  read of one domain share a channel, so the channels alone could not tell them apart.
- **`replay.record`**: `clock=wall` (a live wall from crd-time's `MonotonicClock`, through
  `cook::monotonic_ns`; crd-ceir-cook now links crd-time, declared in its `crd_module` dependencies), `sim_time=`
  and `sim_step=` (i64 nanoseconds). The plan run and the host executor (`HostClockSpec` on `HostRecordRequest`) both
  build a `HostClock` with `apply_clock` and route random to the seeded source (or none) and both time kinds to the
  clock. The summary answers `wall_clock`, `sim_time`, `sim_time_ns`, `sim_step` and `sim_step_ns`. The reason text
  for a missing `clock` became "the time reads are not held; replay.record keeps every input.clock and
  input.time_step read" (the existing `replay.prepare` expectation was updated).
- **Demo program**: `assets/ceir/clock_demo.ceir`. `main(n)` reads the sim step, fails `selector-out-of-range` when
  it is over 33,333,333 ns (a one-case switch), and otherwise returns the wall reading plus the sim reading plus `n`.

`kReplayExecutor` stays 1: it versions the meaning of a trace for the existing ops, and appending two ops changes
none of them. A record of a program using the new ops cannot come from an older build in the first place (its cook
would not know the dialect's ops), and the build identity check already refuses another build's record.

The bound: `kReplayMaxInputReads` (65,536) is shared by every kind. A program that reads the clock in a loop reaches
it as quickly as one that draws; past it the record keeps the first 65,536 reads and stores the input missing.

## Tests

Positions come from scanning the program text; time values are chosen in the tests.

- `tests/execution/ceir/test_input_seam.cpp` (6 new cases): the built-in domain ordinals; `HostClock`'s readings,
  steps, `advance` (with wrapping), ignored channels and kinds, a live wall from a fake monotonic reader measured from
  its epoch, `clear`; `InputRouter` routing per kind; both executors reading four time reads through a logging
  source in the same order with the same raw values; a missing reading failing `InputUnavailable` at that op in both;
  five bad domains refused like a bad constant; a parallel body reading either op refused by the pre-flight and the
  plan compiler.
- `tests/execution/ceir-cook/test_replay_clock.cpp` (new, 8 cases): an over-budget step recorded with its single read,
  recorded twice to the same bytes, reproduced by a fresh service after the file was edited, and against the edited
  file (the step read asks for the frame domain) an `input` divergence naming both kinds at the read's line and
  column; a steady step with a live wall, whose result is the recorded wall reading plus the sim reading plus 7, which
  `replay.run` reproduces with no clock, and a record with a changed wall reading named as a value divergence at the
  wall read; no clock (and a sim reading without a step) failing at the step read, recorded and reproduced; 65,537
  clock reads storing `clock` missing and the replay refused; a `TimeRead` op outside the seam, and reads not held,
  storing it missing; six malformed clock arguments refused with nothing run or written; the decoder refusing a time
  read past the built-in domains; the inspect host given a `HostClock` recording the unobserved run's read and
  outcome, and past the bound storing `clock` missing.
- `tests/execution/ceir-cook/test_replay_inputs.cpp`: the "kind past the last" format check uses `kLastInputKind + 1`
  (kind 1 is now a valid kind).
- `tests/execution/ceir-host/test_host_inputs.cpp` (1 new case): a host record of the over-budget step, byte-equal to
  the host executor's record from the same clock arguments, reproduced on the recorded split and on 1 and 16 jobs, an
  edit to the frame domain named with both kinds at its line, and a steady step's changed wall reading named at the
  wall read.
- `tests/tools/ceridc/test_ceridc_diag.cpp` (1 new case, real binary): process 1 records the over-budget step (its
  file equal to this process's native record), the file is edited, process 2 reproduces it with no clock, process 3
  names the edit (`recorded 1, observed 2`, both `time_step`); process 4 records a steady step reading the live wall
  and process 5 reproduces that result; each replay answer equals the native call, and so does an MCP stdio process's
  tool text.

## Teeth (win-debug)

Each break was applied, the suites it touches rebuilt (and `ceridc` where the binary runs it), the `[clock]` cases (or
the ceridc clock case) run from a scratch folder under the lane, and the source restored by rewriting it. After the
last tooth every touched target was rebuilt and `crd-ceir-tests` (562 cases), `crd-ceir-cook-tests` (83),
`crd-ceir-host-tests` (46) and `crd-ceridc-tests` (16) passed again in full.

| Break | Result |
| --- | --- |
| `is_seam_input` answers random only | 5 of the 8 cook `[clock]` cases fail |
| the inspect host's bound downgrades random only | the inspected 65,537-read case fails |
| `reads_input` knows only `input.random` | 4 of 6 seam `[clock]` cases and 5 cook cases fail |
| a step read answers the domain's reading | 4 seam cases fail |
| the plan reads every time op as a clock | 2 seam cases fail |
| the reference interpreter reads domain 0 | 2 seam cases fail |
| the live wall ignores its epoch | the host-clock case fails |
| the router sends every kind to the random route | the router case fails |
| the decoder accepts any time domain | the format case fails |
| `replay.record` routes no time step | 4 cook cases and the ceridc clock case fail |
| the host executor ignores the clock arguments | the host-provider clock case fails |
| an `input` divergence names no recorded kind | 1 cook case and the host case fail |
| `sim_time` is ignored | 2 cook cases fail |
| `clock=wall` sets no live wall | 3 cook cases fail |
| any domain name is accepted | the bad-domain case fails |
| the plan compiles no time step | 3 seam cases fail |

## Evidence

Final sources, every lane built from them. Suites: `crd-ceir-tests` 562 cases, `crd-ceir-cook-tests` 83,
`crd-ceir-host-tests` 46, `crd-ceridc-tests` 16 (run from a scratch folder under the lane with `CRD_CERIDC_EXE`
set), `crd-sandbox-inspect-tests` 7, `crd-ceir-gpu-tests` 140 and `crd-chir-tests` 30. Assertions: 13,465, 4,281,
2,025, 655, 270, 2,661 and 5,382.

- **win-debug**: reconfigured; the whole tree builds (`input.hpp`, `plan.hpp` and `time.hpp` are included across the
  CEIR modules); every suite passes with those counts. Through ctest the 8 repository guards and the opgen tests pass
  (10 tests). The last source edit (the host test's clock constants moved to namespace scope for the naming check)
  came after the whole-tree build; `crd-ceir-host-tests` was rebuilt and passed again on win-debug, and every other
  lane built it after the move.
- **win-shipping, win-clang-cl-shipping and win-asan**: each lane reconfigured and built the seven suites, `ceridc`
  and `crd-sandbox`, and passed every suite with the same counts. The clang-cl links were clean the first time;
  win-asan ran inside vcvars with no ASan report.
- **WSL** (linux-gcc-debug, linux-gcc-asan, linux-clang-tsan with the hosted lane's `TSAN_OPTIONS`): every lane built
  the same targets and passed the seven suites with the same counts; no ASan, UBSan or TSan report. The WSL VM
  restarted once, right after the TSan build had finished and before its tests ran; the TSan lane was rerun on its own
  (the build was up to date) and passed.
- **Generated code**: opgen regenerated the `input` dialect (`--check` clean), and `check-generated.py --refresh`
  recorded the four tracked files' new bytes in `scripts/generated-sources.json`.
- **Checks**: strict tidy (LLVM 20) is clean on all 22 changed C++ files after one fix (the host test's local
  `k`-named constants moved to namespace scope). clang-format shows only the repository's hand alignment, include
  grouping and one-line `case` style on the added lines; its line wraps were applied by hand and no added line is
  over 120 columns. The Allman check, check-generated, check-master-plan, check-repository and
  test-repository-tools pass.

## Hosted CI must show

- `crd-ceir-cook-tests` (83, with the 8 `[clock]` cases), `crd-ceir-host-tests` (46, with the host-provider clock
  case), `crd-ceridc-tests` (16, with the real binary's five-process clock case) and `crd-ceir-tests` (562, with the
  6 new seam cases and the generated input smoke cases) green on all six lanes, with `crd-sandbox-inspect-tests` (7),
  `crd-ceir-gpu-tests` (140) and `crd-chir-tests` (30) still green; the `crd-ceir-opgen-drift` ctest green; `ceridc`
  and `crd-sandbox` building on every lane.

## Remaining for DIAG.9a

- State cells across invokes and a run spanning a reload: they need a host that keeps one interpreter across invokes.
- Input events and external I/O completions: their ops are not specified yet.
- A clock at the interactive hosts: `ceridc inspect`, `program.inspect` and the sandbox panel take a random source
  only. The inspect host already records whatever source it is given (the new inspect-host cases pass it a
  `HostClock`); the consumers need their flags, and the sandbox frame loop's real frame step is the natural live
  source.
- Backend-specific numeric replay of GPU dispatches with a declared tolerance.
- Network and physical effects stubbed only in explicit test replay.
- Host records from crd-sandbox's panel.
