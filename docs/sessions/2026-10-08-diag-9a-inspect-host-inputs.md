# DIAG.9a live host inputs at the inspect host, 2026-10-08

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.9a](../ROADMAP.md#slice-diag.9a). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-9a). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.9a host input seam](2026-10-08-diag-9a-host-input-seam.md).

## Goal

The seventh DIAG.9a batch: give the inspect host, which every interactive consumer uses (`ceridc inspect`,
`program.inspect`, the crd-sandbox panel), a live host input source, so a seeded program can be debugged and its
inspected run recorded with every draw. DIAG.9a stays Partial.

## What was there (checked before coding)

- `inspect::Session::run` called `plan::run` without the `inputs` argument the input seam added, so a session-driven
  plan run had no way to read a host input.
- `InspectHost::start` installed no source and classified `random` as not held, so a drawing program failed
  `input-unavailable` there and its record stored `random` missing. Its `record` never copied `input_reads`; that was
  harmless only while nothing was read.
- `run_inspect_script` returns `Unfinished` with the run still attached (the host's destructor cancels and joins it),
  so a source owned inside the script would dangle; the caller must own it, declared before the host.
- `SeededInputs` is one run's source and not movable, while the sandbox panel starts many runs ("Run again").

## What changed

- **Session** (crd-ceir): the plan form of `inspect::Session::run` with an observer takes `const input::InputSource*
  inputs` (default none) and passes it to `plan::run`.
- **Seam**: `SeededInputs::reset(seed)` starts every stream over at draw 0 of `seed` (not while a run reads it).
- **Inspect host** (crd-ceir-cook): `InspectHost::start(args, recording, inputs)`. The source is the caller's, read on
  the executing thread, valid until the execution has ended. A recorded execution always reads through an
  `InputRecorder` over the source (or none), so the record keeps every read, delivered or not, exactly as
  `replay.record` does. `random` is classified held at `start`, and `record` stores it missing once the run has ended
  with more reads than a record keeps; `record` now copies `input_reads` and `input_reads_total`.
- **Script**: `InspectScript::inputs` passes the caller's source to `start`.
- **A source's allocator**: the source runs on the executing thread while the controller allocates its report, so it
  may not share the controller's allocator (the host's THREADS rule). `SeededInputs` grows a per-stream counter map on
  a stream's first draw, so every consumer below gives it its own small `GrowableTlsfAllocator` (64 KiB first chunk),
  declared with the source before the host. The first version built it on the caller's allocator; see the TSan
  finding under Evidence.
- **`program.inspect seed=`**: a u64 checked in the service's argument step (bad-argument before anything is read);
  the command owns a `SeededInputs` (and its allocator) declared before its host. The summary gains `random_source`
  and `seed` (between `max_stops` and `truncated`; three existing summary assertions were updated for the two new
  fields).
- **ceridc**: `inspect --seed <u64>` (a strict decimal u64, refused before anything runs). The report names
  `random_source` and `seed`, and its `record` object `input_reads`.
- **crd-sandbox**: `InspectPanel::set_inputs(PanelInputs)`; the panel owns a `SeededInputs` and its allocator,
  declared before its host, and resets it at every `start` (a start while a run is attached is refused `Busy` before
  the reset). `--inspect-seed <u64>` sets it for the first run and "Run again"; the record log line names the input
  reads.

## Tests

Which seed fails, and at which draw, is computed in each test from `SeededInputs::draw`; positions come from scanning
the program text (`assets/ceir/random_demo.ceir`).

- `tests/execution/ceir-cook/test_replay_inputs.cpp` (the old inspect-host case replaced by 3, now 8 cases): a
  seeded `main(4)` held at every switch, stepped over once and value-read records the same reads, input states, trace
  and outcome as `replay.record seed=` of the same file, and each held switch's watched draw is the seed's draw; the
  record replays in a fresh Context from its reads alone, `replay.run` in a fresh service reproduces it from the file,
  and against the edited file (stream 2) names the draw's line and column; a passing seed unrecorded returns the
  stream-1 draw, the same source continues its streams and `reset` makes the next run return the same value; with no
  source a recorded run's failed read is the record, equal to `replay.record` without a seed, and it reproduces;
  65537 draws on the inspect host store `random` missing with 65536 kept.
- `tests/execution/ceir-cook/test_inspect_diag.cpp` (1 new case, 3 new refusals): `program.inspect seed=` with a
  failing seed answers each held switch's seeded draw and fails at the switch; a passing seed returns the stream-1
  draw; no seed fails `input-unavailable` at the draw; `seed=x`, `-1` and 2^64 are bad-argument with nothing run.
- `tests/execution/ceir/test_input_seam.cpp`: `reset` gives what a new source of that seed gives.
- `tests/tools/ceridc/test_ceridc_inspect.cpp` (1 new case): the verb's held draws, its record's `input_reads`, no
  seed failing, five malformed seeds refused before running; the real binary records a seeded run, the program file
  is edited, a second `ceridc` process reproduces it from the record without the seed and a third names the input
  divergence at the draw's line.
- `tests/applications/sandbox-inspect/test_inspect_panel.cpp` (1 new case): without inputs the panel's run fails
  `input-unavailable`; seeded and recorded, each held switch shows the seeded draw, the run returns the stream-1 draw
  and its record (5 reads) replays from its reads; "Run again" twice reads the same draws from the first.

## Teeth (win-debug; the last one on linux-clang-tsan)

Each break was applied, the suites it touches rebuilt (and `ceridc` where the binary runs it), the relevant cases run
from a scratch folder under the lane, and the source restored by rewriting it. After the last tooth every touched
target was rebuilt, and `crd-ceir-tests [input]` (8 cases), `crd-ceir-cook-tests [diag]` (38), `crd-ceridc-tests` (15)
and `crd-sandbox-inspect-tests` (7) passed again.

| Break | Result |
| --- | --- |
| `Session::run` passes no inputs to `plan::run` | 4 of the 9 cook `[input]` cases fail |
| a recorded run reads the source without an `InputRecorder` | 3 cook cases fail |
| `record` copies no reads | 3 cook cases fail |
| no `random` downgrade past the bound | the 65537-draw case fails |
| an unrecorded run gets no source | 2 cook cases fail (the passing seed, `program.inspect`) |
| `run_inspect_script` drops `InspectScript::inputs` | 2 cook cases and the ceridc seed case fail |
| `program.inspect` drops the parsed seed | the `program.inspect` seed case fails |
| the ceridc verb drops the seed | the ceridc seed case fails |
| ceridc's `main` does not pass `--seed` (the binary rebuilt) | the ceridc seed case fails |
| the panel resets its source only when the seed changes | the panel case fails ("Run again") |
| the panel passes no source | the panel case fails |
| `SeededInputs::reset` keeps the streams | the seam case and the panel case fail |
| ceridc's seed parser accepts any text | the ceridc seed case fails |
| `program.inspect`'s seed check accepts any text | the refusal case fails |
| `program.inspect`'s source back on the controller's allocator (linux-clang-tsan) | TSan reports the race in `SeededInputs::next` in 3 of 3 runs of the seed case |

## Evidence

Final sources, every lane built from them. Suites: `crd-ceir-tests` 556 cases, `crd-ceir-cook-tests` 75,
`crd-ceir-host-tests` 45, `crd-ceridc-tests` 15 (run from a scratch folder under the lane with `CRD_CERIDC_EXE`
set), `crd-sandbox-inspect-tests` 7, `crd-ceir-gpu-tests` 140 and `crd-chir-tests` 30.

Assertions: 13,233, 3,915, 1,977, 604, 270, 2,661 and 5,382.

- **win-debug**: reconfigured; the whole tree builds (`input.hpp` and `inspect.hpp` are widely included in the CEIR
  modules); every suite passes with those counts. Through ctest the 8 repository guards and the 4 opgen and
  capability-matrix tests pass (rerun after the allocator fix).
- **win-shipping, win-clang-cl-shipping and win-asan**: each lane reconfigured and built the seven suites, `ceridc`
  and `crd-sandbox`, and passed every suite with the same counts. The clang-cl links were clean the first time;
  win-asan ran inside vcvars with no ASan report.
- **WSL** (linux-gcc-debug, linux-gcc-asan, linux-clang-tsan with the hosted lane's `TSAN_OPTIONS`): every lane
  built the same targets from the final sources and passed the seven suites with the same counts; no ASan, UBSan or
  TSan report.
- **The TSan finding**: the first full WSL pass ran an earlier version in which every consumer built its
  `SeededInputs` on the controller's allocator. linux-clang-tsan reported a data race in `crd-ceir-cook-tests`: the
  executing thread's first draw (`SeededInputs::next` growing its stream map, through `plan::run` under
  `InspectHost`) against the controller pushing the script's breakpoint reports into the same `TlsfAllocator`
  (`program.inspect` seed case). Both gcc lanes and all four Windows lanes had passed it. With each source on its own
  allocator, the three affected suites passed five TSan runs in a row with no report; putting `program.inspect`'s
  source back on the controller's allocator made TSan report the race in 3 of 3 runs; after the restore the TSan lane
  was rebuilt with the rest, and every lane above (Windows and WSL) was rerun from the final sources.
- **crd-sandbox on this machine's GPU** (win-debug, `CRD_ASSETS_DIR` set, run from a scratch folder):
  `--smoke-test 4 --inspect ceir/random_demo --inspect-arg 4 --inspect-seed 1 --inspect-break 10 --inspect-watch 9
  --inspect-step continue,continue,continue,continue --inspect-record sbx_seed.crpl` stopped at `10:13` with the
  watched draw 0, then at `10:13` with 3, failed `selector-out-of-range`, and wrote the record (7 events, 2 input
  reads); `ceridc diag --command replay.run` on it answered `reproduced` with 2 recorded reads. Seed 2 finished with
  36, as `ceridc inspect --seed 2` does; `--inspect-seed x` exited 1 before running. The scratch folder was deleted.
- **Checks**: strict tidy (LLVM 20) is clean on 20 of the 21 changed C++ files after one raw-string fix in the
  ceridc test (and clean again on the six files the allocator fix touched). The 21st, `sandbox/src/inspect_panel.hpp`,
  is reported ungated: it is outside the `/crd/` header filter, and the tool's standalone pass (and its `--plan`
  owner, `crd-gizmo-probe`) does not resolve its includes; its TU and the test TU are clean and it builds under
  `/W4 /WX` on every Windows lane. clang-format shows only the repository's hand
  alignment and namespace indentation; its line wraps and joins were applied by hand, and no added line is over 120
  columns. The Allman check, check-master-plan and check-repository pass.

## Hosted CI must show

- `crd-ceir-cook-tests` (75, with the 9 `[input]` cases), `crd-ceridc-tests` (15, with the real binary's
  `inspect --seed` record reproduced by another process) and `crd-sandbox-inspect-tests` (7, with the seeded panel
  case) green on all six lanes, with `crd-ceir-tests` (556), `crd-ceir-host-tests` (45), `crd-ceir-gpu-tests` (140) and
  `crd-chir-tests` (30) still green; `ceridc` and `crd-sandbox` building on every lane.

## Remaining for DIAG.9a

- State cells across invokes and a run spanning a reload: they need a host that keeps one interpreter across invokes.
- Clock and time-step inputs, input events and external I/O completions: their ops must be specified first (units
  and time domains for a clock).
- Backend-specific numeric replay of GPU dispatches with a declared tolerance or oracle.
- Network and physical effects stubbed only in an explicit test replay.
- Host records from crd-sandbox's panel: the request would have to run on a thread the pool enrolled.
