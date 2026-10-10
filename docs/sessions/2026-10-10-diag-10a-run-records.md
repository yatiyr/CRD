# DIAG.10a run-record fuzzing, 2026-10-10

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.10a](../ROADMAP.md#slice-diag.10a). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-10a); harness:
> [test instruments](../design/test-instruments.md). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.10a evidence readers](2026-10-10-diag-10a-evidence-readers.md).

## Goal

The third slice of DIAG.10a: the run-record reader (`decode_record`, crd-ceir-cook), which `replay.run`,
`replay.prepare`, `replay.minimize` and the host and device replays start from, on the bounded fuzz harness with an
oracle that states what the recorders can write, the acceptance's malformed lengths, truncation and hostile counts,
and its findings fixed and kept as corpus inputs. Committed locally, not pushed.

## What changed

- A fuzz target `crd-fuzz-ceir-record` in `tests/execution/ceir-cook/fuzz` (`crd_fuzz_target()`: the replay
  executable and its `-corpus` CTest on every lane, the libFuzzer executable under `CRD_ENABLE_FUZZER`), registered in
  `scripts/fuzz.py` (`ceir-record`), in the test-instruments table and in `test-repository-tools.py`'s target list;
  the ceir-cook module now owns `tests/support/fuzz` as a test directory.
- Each input is decoded as given and, when it starts with a record's magic, again with its header's payload size and
  checksum recomputed. The checksum guards integrity, so without the reseal libFuzzer's mutations of the payload
  would almost all stop at `bad-checksum` and the payload reader would never be fuzzed.
- Oracles: the same verdict twice; at most eight times the input plus 64 KiB allocated by the decoder (a fresh
  `BudgetAllocator` per decode); an accepted record must be one a recorder writes, restated in the harness without
  calling the decoder (`recorder_could_write`: each executor's own schedule settings, errors and fields, never a
  cancelled run, the bounded trace and read stream, each input's state derived from its need as
  `detail::record_inputs` derives it, every kept read of an input the program needs, each device kernel's text with
  its FNV-1a hash, no device section in a plan or host record); it must re-encode to exactly its own bytes, report
  exactly the missing inputs its states name (external results unless not needed) through `record_missing_inputs`,
  and show no divergence from a trace that repeats it (`first_divergence`). The program blob is carried, not read:
  the CRDR reader beneath it is `crd-fuzz-ceir-binary`'s.
- Seeds are written by hand in the shapes the three recorders write, with a fixed build and a small stand-in blob,
  so the corpus is the same bytes on every lane (a real recording embeds `current_build()`): plan records (a passing
  run with random draws, a trace bounded at two of five events with clock and time-step reads and a fuel failure, a
  random read the host could not answer, a reloading host's asset and generation), host records (input.event reads
  with the schedule recorded, a failed run) and device records (an ulp envelope with a written f32 and an unwritten
  u32 buffer, an exact envelope with a device error). 30 records contradict a recorder in exactly one way; 19 damage
  the bytes (truncation as given and resealed, a trailing byte unsealed and sealed, a wrong checksum, an older schema,
  three counts past the payload, empty, garbage). `VerdictSink` checks each verdict as `--seed` writes it. 57 inputs.

## Findings, each fixed and kept as a corpus input

All in `engine/execution/ceir-cook/src/replay_record.cpp`; no record layout or schema changed, and every record a
recorder writes still reads.

1. Allocation sized by a count the input cannot hold. The event array was reserved from the kept-event count, the
   read array from the kept-read count and each device buffer from its word count, all before any byte was checked:
   a sealed record under 500 bytes made the decoder reserve 3 MB of events, 1.5 MB of reads or 16 MB of words before
   it reported the record truncated. A count is now checked against the bytes left (16 bytes per event, 14 per read,
   4 per word) first. Seeds `events-count-past-payload`, `reads-count-past-payload`, `words-count-past-payload`.
2. Input states no recorder writes. `decode_record` checked each need and state byte's range only.
   `detail::record_inputs` derives the state from the need: not-needed exactly when no op needs the input; program,
   build and entry arguments always needed and recorded; external results never recorded; the schedule recorded by
   the host executor (always, when needed) and by no other; the device tolerance likewise by the device executor; a
   seam input (random, clock, host-state) recorded only when surely needed and every read is held. `replay.prepare`
   answered a record's recorded states as available inputs, so a record saying `schedule` was recorded on a plan run,
   or `random` recorded while it had lost reads, was presented as evidence. Thirteen seeds, one per contradiction.
3. A kept read of an input the record says no op needs: only an input op reads through the seam, and its effects make
   its input needed. Seed `forged-read-of-unneeded-input`.
4. A cancelled run: both recorders return before writing one. Seeds `forged-plan-cancelled`,
   `forged-host-cancelled`.
5. A device kernel whose text does not hash to its recorded hash: the recorder stores the text's FNV-1a hash. The
   device replay already refused such a record when it ran (`ContentMismatch`); the decoder read it as valid. Seed
   `forged-kernel-hash`.
6. Device kernels or buffers in a plan or host record: the decoder refused the adapter, envelope and error fields
   there but not the arrays. Seeds `forged-plan-with-kernel`, `forged-plan-with-buffer`.

The existing rules got seeds too (a generation without an asset, a host record without jobs, schedule settings on a
plan record, each executor's error in the other's record, an entry on a device record, an exact envelope with a
bound, outputs of an unwritten buffer, an undelivered value, an unknown time domain, a trace longer than its total,
a trailing byte, truncation).

### What the tightening changed for existing tests

- DIAG.9b's external-results cases forged record files with `external-results` claims no recorder writes. Those
  files are now refused at `replay.run` and `replay.prepare` as not decoding (`failed`, "malformed"), still before
  anything runs. `record_missing_inputs` keeps its own oracle: the same four claims are asserted on the record in
  memory (the host and device replays take in-memory records), and the honest needed-and-missing claims (`yes` and
  `unknown`) still decode and are refused for the missing input. The `replay.prepare` check now reads a
  needed-and-missing record and reports it missing and unavailable, and a record claiming the results held is refused.
- The device case for an edited kernel text now checks that the file is refused as it is decoded and that
  `replay_device_record` refuses the same record held in memory (`ContentMismatch`, naming the kernel).
- Two ceir-host fixtures turned a host record into a plan record by changing its executor and schedule settings but
  kept `schedule` recorded; they now store it missing, as the plan recorder does.
- The unheld-stream rule cannot be reached by the corpus: a stream past 65,536 reads needs about 900 KB, past the
  harness's 64 KiB input bound. The existing incomplete-stream case in `test_replay_inputs.cpp` now encodes its real
  record (random stored missing), decodes it, and checks that the same record claiming random recorded is refused.

## Teeth

Each break was applied to `replay_record.cpp`, the lane rebuilt and the check observed failing, then restored:

| Break | Observed |
|---|---|
| no bound before reserving events | corpus replay: allocation oracle |
| no bound before reserving reads | corpus replay: allocation oracle |
| no bound before reserving device words | corpus replay: allocation oracle |
| kernel hash not compared | corpus replay: `recorder_could_write` |
| device kernels and buffers allowed off the device | corpus replay: `recorder_could_write` |
| cancelled runs accepted | corpus replay: `recorder_could_write` |
| the input-state check skipped | corpus replay: `recorder_could_write` |
| need and state not tied | corpus replay: `recorder_could_write` |
| a host schedule or device tolerance allowed missing | corpus replay: `recorder_could_write` |
| reads of unneeded inputs accepted | corpus replay: `recorder_could_write` |
| external results allowed recorded | corpus replay: `recorder_could_write` |
| a seam input recorded when only possibly needed | corpus replay: `recorder_could_write` |
| trailing payload bytes accepted | corpus replay: re-encode oracle |
| a seam input recorded with reads lost | `test_replay_inputs.cpp`'s incomplete-stream case |

## Verification

- win-debug: whole tree built; the full ctest (inside vcvars) passed 7,525 of 7,527 before the docs and registry list
  were finished (`crd-master-plan`: this note did not exist yet; `crd-repository-tools`: its fuzz target list lacked
  `ceir-record`); both pass now. On the final source: the 57-input corpus replay, `--seed` regenerating the corpus
  byte for byte, crd-ceir-cook-tests (108 cases), crd-ceir-host-tests (47), crd-sandbox-inspect-tests (31),
  crd-ceridc-tests (20, with `CRD_CERIDC_EXE`), crd-ceir-gpu-vulkan-tests (60) and crd-ceir-gpu-dx12-tests (47) on
  this machine's GPU, and 20 CTests (`crd-no-*`, `crd-check*`, `crd-master-plan`, `crd-repository-tools`, every
  `crd-fuzz-*-corpus`).
- win-shipping, win-clang-cl-shipping and win-asan (inside vcvars), each reconfigured: `crd-fuzz-ceir-record`,
  crd-ceir-cook-tests, crd-ceir-host-tests, crd-sandbox-inspect-tests, `ceridc`, crd-ceridc-tests and `crd-sandbox`
  built (clang-cl linked first time); the same suites green, the seeds byte-identical, no sanitizer report.
- WSL, one lane at a time: linux-gcc-debug, linux-gcc-asan and linux-clang-tsan (`setarch -R`) built the same targets;
  corpus replay (57), seeds byte-identical, crd-ceir-cook-tests 108, crd-ceir-host-tests 47, crd-sandbox-inspect-tests
  30 and crd-ceridc-tests 20 green, no ASan, UBSan or TSan report.
- linux-clang-fuzz: one single-process libFuzzer round (`-seed=1`, 600 s, the committed corpus read-only, a scratch
  working corpus): 7,761,763 executions, 755 new units, peak RSS 478 MB, no artifact. The new units were not merged.
- The last edit to `replay_record.cpp` reworded a comment after the win-shipping and linux-gcc-debug builds; it
  changes no code, and win-debug was rebuilt and rerun after it.
- Strict tidy is clean on the 7 changed C++ files (a nested conditional and a hand loop were replaced in the decoder).
  clang-format differs only in the repository's hand alignment, one-line case labels, include grouping and line
  breaks. The Allman check, `test-repository-tools.py` (65 tests), `check-master-plan.py` and `check-repository.py`
  pass.

## Remaining in DIAG.10a

Command decoding (`DiagCommandService`); resource reload (pack manifests, torn packs, `do_reload_mount`); the
compiled and reference CEIR comparison; the virtual-memory and streaming allocators; reinvestigating the
unknown-origin CEIR and CKIR artifacts on the strengthened harness; settling the concurrent-load corruption; the
hosted run, which must now also show `crd-fuzz-ceir-record-corpus` (57 inputs) on all six lanes and
`crd-ceir-cook-tests` and `crd-ceir-host-tests` green.
