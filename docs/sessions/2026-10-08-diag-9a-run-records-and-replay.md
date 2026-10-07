# DIAG.9a run records and replay, 2026-10-08

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.9a](../ROADMAP.md#slice-diag.9a). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-9a). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.8c GUI consumer](2026-10-07-diag-8c-gui-consumer.md).

## Goal

The first DIAG.9a batch: record a run of an authored program as an immutable artifact, replay it in a later process
after the program file was edited, report the first divergent event or value, and refuse an incompatible replay
explicitly. DIAG.9a was Open; this batch moves it to Partial.

The DX12 hardware fault output pasted again at the start of this step (`submit=00000000 wait=00000000
reason=00000000`, an empty removal record, exit 30) is the second run already recorded in
[the user-items note](2026-10-07-user-items-wpr-and-vulkan-loss.md). It adds nothing new; the user's decision stands
and the case was not run again.

## What was there (checked before coding)

- `replay.prepare` (DIAG.8c) classified a program's replay inputs from its ops' effective effects but nothing recorded
  a run, so every input but the program's content hash was missing.
- No production CEIR op reads a random stream, a clock, host state or an external completion (the only effect
  families any dialect declares are Synchronization, GPUCommand, memory and allocation, SceneRead and HostStateRead
  on the declare-only scene resolvers). Adding a random or clock op just to have something to record was rejected.
- The compiled-plan executor (`plan::run`, used by `InspectHost`, `ceridc inspect`, `program.inspect` and the sandbox
  panel) already offers a safe point before every instr (`plan::RunControl`) and reads a result from the safe point's
  own frame (`plan::read_value`). Stable ids are pre-order, so an edit that changes only a value keeps every id.
- A cooked CRDR program blob carries its content hash and its authored positions, so it can be the stored artifact.
- A diagnostic command could declare only one authority class; writing a record is `record`, running a program is
  `execute`.

## What changed

- **Run records** (`crd/ceir/cook/replay_record.hpp`, `src/replay_record.cpp`, crd-ceir-cook): `ReplayRecord`, a
  versioned (`kReplayRecordSchema` 1), FNV-1a-checksummed little-endian file: the build (`CRD_VERSION_STRING`,
  platform, compiler, arch, debug or release, asserts, `kReplayExecutor` 1), the authored path, the cooked blob and its
  content hash, the entry, the arguments (at most 64), each of the nine inputs' need and state, `max_events`, the
  total event count, the kept events and the outcome (run error, faulting op's stable id, results, state cells).
  `encode_record` is deterministic; `decode_record` bounds every count and size before use (`not-a-record`,
  `unsupported-schema`, `truncated`, `bad-checksum`, `malformed`).
- **The traced run**: `load_replay_program` registers the host's dialects into a fresh Context, reads a blob and
  compiles the entry; `run_traced` runs it with a `RunControl` whose safe point appends one event per dispatched instr
  (op stable id, call depth) up to the bound and, at the next safe point of the same frame, reads up to four results
  of the previous event through `plan::read_value`. `first_divergence` compares in a fixed order: path, value (or the
  number of results read), length, outcome, results, cells.
- **Shared need analysis** (`src/replay_needs.{hpp,cpp}`, private): the effect walk and input table moved out of
  `replay.prepare` unchanged, so the recorder and the preparer classify inputs the same way. The two identity/argument
  reasons now point at `replay.record` instead of saying nothing records them.
- **Commands** (`crd/ceir/cook/replay_diag.hpp`, `src/replay_run_diag.cpp`):
  - `replay.record` (`execute`, also `record`; a program path; `out`, `entry`, `args`, `max_events`) loads the program
    (text and binary are cooked under the relative path; a cooked file is kept byte for byte), loads that blob exactly
    as a replay will, records the run and creates the record exclusively under the root. An existing `out` is refused
    before anything is read or run; a cancel stops the run and nothing is written.
  - `replay.run` (`execute`; a record path; `program`, `build`) refuses an undecodable record, another build (unless
    `build=any`), a record missing an input its program needs, and a blob whose recorded or recomputed content hash is
    not the recorded one, all before running; then replays the record's own blob (or, with `program=`, the given file)
    and answers a `divergence` item and both outcomes at their authored positions.
  - `replay.prepare` reads a run record as well as a program file: the record's build and arguments are available.
- **crd-perf**: `DiagCommandSpec::also`, a second authority class a command needs (the listing shows it; it must be a
  different single class); `DiagCall::root`; `perf::diag_path_is_safe`, the service's own path rule, for path
  arguments.
- **Shared helpers**: `src/diag_args.{hpp,cpp}` (the argument parsers `program.inspect` had privately);
  `load_program_file` and `load_program_bytes` split out of the program loader; `read_bounded_file` names what it
  reads; `write_new_file` creates exclusively (`_sopen_s` with `_O_EXCL`, `open` with `O_EXCL`).
- **Consumers**: ceridc's CLI and MCP services (`bind_diag_commands`) and crd-sandbox bind `replay.record` and
  `replay.run`.
- **Authored specimen**: `assets/ceir/replay_demo.ceir`. `main(seed)` loops three calls, then selects switch region
  `seed + bias` of two; `main(1)` fails `selector-out-of-range`, and editing the bias from 1 to -1 makes it finish.

## Tests

- `tests/execution/ceir-cook/test_replay_record.cpp` (7 cases, 541 assertions; positions scanned from the text):
  - `main(0)` finishes and every effect-derived input is not needed; `main(1)` faults at the switch's scanned line and
    column, the record holds the current build, the path, the entry, the argument and every event (the last is the
    faulting op), the addi's event carries 2, recording twice gives identical bytes, and re-encoding the decoded record
    gives the same bytes.
  - After the program file is edited, a fresh host replays the record from its blob: reproduced, both outcomes at the
    switch. Replaying against the edited file: `value` divergence at the edited constant's line and column, recorded
    1, observed -1, and the replayed run finishes. An unedited copy reproduces and says the program matches.
  - Refusals with nothing run: another build (`unavailable`, naming `config`; `build=any` reproduces and says
    `differs`), a missing input (naming `random`), a flipped byte (`bad-checksum`), another schema, a wrong content
    hash, a cut blob, a program file and a truncated record, and bad `program`/`build` arguments before any byte read.
  - The trace bound: 5 kept, the rest counted as lost; the replay verifies 5 and the whole count; the edited program,
    whose change lies after the kept events, is still caught as an `outcome` divergence.
  - `replay.record` needs both classes (each alone, and `read`, refused with no run and no file), refuses a bad `out`,
    `max_events`, `args` and a missing `out` in the argument check, a raised cancel, and an existing file (unchanged).
    The listing shows `"also":"record"`.
  - `replay.prepare` on a record: `form` record, `replay` prepared; on the program file: build and arguments missing.
  - `first_divergence` on synthetic traces: each kind alone at its index, earliest event first, an event before the
    outcome, and a truncated record compared on its kept events and whole count.
- `tests/foundation/perf/test_diag_commands.cpp`: a new case for the second class (either alone refused, naming the
  missing class, never run; both granted runs; malformed declarations refused; the listing), `DiagCall::root` and the
  public path rule (nine unsafe paths refused by both).
- `tests/tools/ceridc/test_ceridc_diag.cpp`: a new case through the real binary. One `ceridc` process records
  `main(1)`, the program file is edited, a second process replays the record (reproduced) and a third replays against
  the edited file (`value` divergence at the edited line); each answer is byte-equal to this process's native call, as
  is the MCP stdio tool text; a process without `record` refuses to record and writes nothing.
- `test_replay_diag.cpp`: the two reworded reasons.

## Teeth (win-debug)

Each break was applied, the target rebuilt and the suite run; each was reverted and rebuilt, and the suites passed.

| Break | Result |
| --- | --- |
| value comparison off | 8 assertions fail (the edit's divergence) |
| read-count comparison off | the synthetic case fails (it was not caught before that case was added) |
| replay loads the checkout's file instead of the blob | 8 fail (no longer reproduces after the edit) |
| build check off | the build case fails |
| trace bound ignored | the bound case fails (3) |
| checksum not verified | the checksum case fails |
| `also` not checked | 6 fail in the cook suite, 4 in the perf suite |
| missing-input check off | the missing-input case fails |
| content-hash check off | the artifact case fails (4) |
| results not read at the next safe point | 5 fail (no values to diverge on) |
| early existing-file check off | the overwrite case fails (the program ran) |
| ceridc not binding the commands | the real-binary case fails |
| `replay.prepare` ignoring a record's states | the prepare case fails (5) |

## Evidence

- **win-debug**: the whole tree builds. `crd-ceir-cook-tests` 62 cases (3,224 assertions), `crd-perf-tests` 216
  (1,882), `crd-ceridc-tests` 11 (463), `crd-chir-tests` 30 (5,382) and `crd-perf-ui-tests` 21 (627) pass; the 8
  ctest guards pass.
- **win-shipping and win-clang-cl-shipping** (profiling compiled out): the same targets and `crd-sandbox` build (the
  clang-cl links were clean); `crd-ceir-cook-tests` 62 (3,224), `crd-perf-tests` 83 (1,114), `crd-ceridc-tests` 11
  (463), `crd-chir-tests` 30 (5,382), `crd-perf-ui-tests` 14 (594) pass.
- **win-asan** (inside vcvars): the targets build; the full `crd-ceir-cook-tests` 62 (3,224) and `crd-perf-ui-tests`
  21 (627) pass, and the `[diag]` cases of `crd-ceridc-tests` (9), `crd-perf-tests` (129) and `crd-chir-tests` (9)
  pass. The only ASan reports are the three deliberate DIAG specimens' child processes.
- **WSL** (linux-gcc-debug, linux-gcc-asan, linux-clang-tsan with the hosted lane's `TSAN_OPTIONS`): every lane
  builds the targets and `crd-sandbox` and passes `crd-ceir-cook-tests` 62 (3,224), `crd-perf-tests` 215 (1,806),
  `crd-ceridc-tests` 11 (463, the real binary included), `crd-chir-tests` 30 (5,382) and `crd-perf-ui-tests` 21 (627).
  No TSan or UBSan report; the only ASan reports are the deliberate DIAG specimens' child processes.
- **The sandbox and ceridc on this machine**: a `crd-sandbox --smoke-test 2 --diag replay.record` run recorded
  `main(1)` (`selector-out-of-range`, `replayable`) and a second sandbox process replayed it (`reproduced`). The same
  record replays in the win-debug `ceridc` (`build` same, `reproduced`); the win-shipping `ceridc` refuses it
  (`unavailable`: differs in `config,asserts`) and reproduces it with `build=any`.
- **Caveat**: the last source change, made after the shipping, clang-cl, asan and WSL runs, was a comment in
  `crd/perf/diag_commands.hpp` (the second authority class); only win-debug was rebuilt after it, and its `[diag]`
  cases of `crd-ceir-cook-tests` (25), `crd-perf-tests` (129) and `crd-ceridc-tests` (9) passed again.
- **Checks**: strict tidy clean on every changed C++ file and the private headers (two test findings fixed:
  raw-string literals and local constant names). clang-format shows only the repository's hand alignment, include
  order and one-line `case` style; its line wraps were applied by hand. The Allman check, check-master-plan and
  check-repository pass.

## Hosted CI must show

- `crd-ceir-cook-tests` (62 cases), `crd-ceridc-tests` (11, including the real binary's cross-process replay),
  `crd-perf-tests` (the diag cases: 11 with profiling, 10 with it compiled out), `crd-chir-tests` (30) and
  `crd-perf-ui-tests` (21 with profiling, 14 without) green on all six lanes.
- `ceridc` and `crd-sandbox` building on every lane.

## Remaining for DIAG.9a

- Capture at the interactive hosts' production boundaries: the `InspectHost` run (ceridc inspect, the sandbox panel),
  the interpreter-based `HostProvider` (state cells carried across invokes, pooled jobs) and the sandbox frame loop.
- Random streams, clock and time-step inputs, input events and external I/O completions: no CEIR op reads them yet,
  so a host input seam (and the op that consumes it) must exist before they can be recorded; until then a program
  needing them records them missing and its replay is refused.
- Schedule choices (pooled and parallel async, task placement) for schedule replay.
- Asset generations: the ReloadSet generation of a hot-reloaded program in the record, and a run spanning a reload.
- Backend-specific numeric replay of GPU dispatches with a declared tolerance or oracle (no bit-identity claim).
- Network and physical effects stubbed only in an explicit test replay.
- Not tested: the snapshot item cap and the 64 MiB record limit (`max_record_bytes`). A difference in the number of
  results read for one event is pinned only by the synthetic `first_divergence` case; no authored program reaches it.
- The DIAG.8b and 8c rows' hosted clauses name earlier counts for `crd-ceir-cook-tests` (46, 55) and
  `crd-ceridc-tests` (5, 10); they are floors from those batches, and this row's counts (62, 11) supersede them.
