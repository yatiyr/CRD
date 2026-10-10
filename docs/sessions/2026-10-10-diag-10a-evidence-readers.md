# DIAG.10a evidence-reader fuzzing, 2026-10-10

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.10a](../ROADMAP.md#slice-diag.10a). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-10a); harness:
> [test instruments](../design/test-instruments.md). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.10a allocator fuzzing](2026-10-10-diag-10a-allocator-fuzzing.md).

## Goal

The second slice of DIAG.10a: the contract's "capture/bundle/policy readers" on the bounded fuzz harness, with the
production entry points, oracles that state what the writer can produce, the acceptance's malformed lengths, overflow,
truncation and hostile counts, findings fixed and kept as corpus inputs, and every new ingress surface registered with
a fuzz owner. The run-record reader (`decode_record`, crd-ceir-cook) is a fourth grammar in another module and is the
next slice. Committed locally, not pushed.

## What changed

- Three fuzz targets in `tests/foundation/perf/fuzz` (`crd_fuzz_target()`: the replay executable and its `-corpus`
  CTest on every lane, the libFuzzer executable under `CRD_ENABLE_FUZZER`), each registered in `scripts/fuzz.py`
  (`perf-flight`, `perf-assets`, `perf-capture`) and in the test-instruments table:
  - `crd-fuzz-perf-flight` reads the input with `decode_flight_record`. An accepted record must be one a
    `FlightRecorder` can write, restated in the harness without calling the decoder or the policy validator (the
    trigger and every suppressed kind armed by the recorded policy, a suppression count exactly when there are
    suppressed kinds, one contiguous run of events across the trigger, post-events-full exactly when `post_events`
    landed, elapsed only under a time bound, shutdown and fatal ends exactly when `post_truncated` says so, a capture
    without the ring fatal and empty, a clipped message filling its buffer, Basic holding Warning and above, an
    unavailable replay naming why); it must re-encode to exactly its own bytes and produce its JSON summary.
  - `crd-fuzz-perf-assets` offers every input to `parse_flight_policy_asset`, `parse_fault_plan_asset` and
    `parse_fault_plan`. A refusal names its reason and leaves the output exactly as it was (each reader gets a
    distinctive value first); an accepted policy passes `validate_flight_policy` and its canonical document reads back
    to the same policy and writes the same document; an accepted plan passes `validate_fault_plan`, its text fits the
    800-byte bound and reads back to the same plan and text.
  - `crd-fuzz-perf-capture` reads the input with `validate_capture_buffer` and `CaptureView`. The harness computes the
    CPROF layout from the header itself; an accepted capture must have a consistent layout, and every accessor must
    answer from exactly where it says (names are their own fields or strings of the blob and end inside them, sample
    arrays lie after the frame records, frame records at the end of the blob), counter kinds and types name an
    enumerator, the Perfetto export completes, the gate and the view agree, and the same capture one byte off its
    alignment is refused.
- `tests/support/fuzz`: `VerdictSink` checks each seed's verdict while `--seed` writes it, so a regenerated corpus
  cannot carry an honest seed the reader now refuses or a forged one it now accepts. All three seed functions use it.
- The bundle corpus moved to `corpus/bundle/`; the replay walks a corpus directory recursively, so the perf corpora
  are siblings under `corpus/<reader>/`. `test-repository-tools.py` lists the new driver entries.

## Findings, each fixed and kept as a corpus input

Found by reading the readers against their writers, then held by seeds the harness generates (each seed is checked
to be refused by the fixed reader and, with the check removed, to fail the oracle):

1. `decode_flight_record` accepted records that contradict themselves: a trigger kind or suppressed kind the recorded
   policy does not arm, suppressed kinds without a count, an end reason the post window contradicts, a capture
   without the ring that is not fatal or holds events, events with gaps across the trigger, a clip flag on a short
   message, an Info event under a Basic policy, and an unavailable replay without its reason. bundle.inspect would
   present any of them as what happened. Ten forged seeds, one per contradiction.
2. `validate_capture_buffer` checked that the header's section sizes fit the buffer and nothing else, and
   `CaptureView` then read through every offset. A name-blob size near 2^64 wrapped the section sum, so the frame
   records sat outside the buffer; a thread's `sample_byte_offset` and `sample_count` were never checked;
   `resolve_name` trusted the blob's slot count, its offsets and its terminator; thread, counter and allocator names
   were handed out as C strings with no terminator check; counter kinds and types were cast unchecked; a frame
   record's counter count indexed its 256 values by the header's count in the Perfetto export; and an unaligned
   buffer was read through typed pointers. Each is now refused; the view keeps trusting a validated buffer. Nineteen
   seeds, written in the writer's exact layout with one defect each (more threads, counters or allocators than the
   profiler's tables are each laid out consistently, and a blob declared four bytes too large is the only defect of
   its seed). No on-disk layout or version changed; the real saves of crd-perf-tests still validate. The
   8-byte alignment rule refuses no real load: the profiler panel's buffer and `cprof_export`'s come from a
   `GrowableTlsfAllocator` (16-byte blocks), ceridc's diagnostic service uses one too, and the sandbox's uses the
   default (malloc) allocator; the writer already reads its own buffer through the same typed pointers.
3. crd-toml had no bound on nesting: a 20,000-deep array or inline table exhausted the stack (the assets seed ended
   win-debug with 0xC00000FD before the fix). It now refuses nesting deeper than 64 levels, counting header paths,
   array-of-tables elements, arrays and inline tables together, checked before a path's tables are created and before
   a container's elements are read. It also clamped silently: an integer past the 64-bit range read as `INT64_MAX`
   (`seed = 9223372036854775808` armed seed 9223372036854775807), a float overflow as infinity, and a number token
   longer than 63 characters as its first 63. These are refused now, with two `[toml][diag10a]` cases in
   crd-toml-tests and seeds in the assets corpus. No committed asset or configuration is affected (the whole win-debug
   ctest passed).

Found by libFuzzer (`linux-clang-fuzz`, clang 18 ASan+UBSan):

4. The Perfetto export computed a sample's and a frame's duration as `end - begin` in `i64`, which overflows for a
   hostile capture (UBSan: `144115188092655449 - INT64_MIN`). `duration_ns` now returns the difference without
   overflow, 0 for a reversed span (the writer already clamped a negative duration to 0) and `INT64_MAX` at most. The
   minimized input is adopted (`6ea98773...`, 720 bytes) and a written seed does the same for a frame span. MSVC has
   no UBSan, so the Windows lanes replay it clean either way; `linux-gcc-asan` runs UBSan without recovery.

The policy readers had no defect of their own: no refusal touched its output and every accepted document round-trips.

## libFuzzer rounds

Single-process rounds (`<target>-libfuzzer -seed=11`, the documented limits, a scratch working corpus with the
committed corpus as a read-only seed directory):

| Target | Wall | Executions | Coverage | Artifacts | Outcome |
|---|---|---|---|---|---|
| `crd-fuzz-perf-flight` | 421 s | 24,697,093 | 568 edges | 0 | clean |
| `crd-fuzz-perf-assets` | 421 s | 895,125 | 1,082 edges | 0 | clean |
| `crd-fuzz-perf-capture` | under 1 s | about 1,600 | 537 edges | 1 | finding 4 |
| `crd-fuzz-perf-capture` (fixed, seed 12) | 421 s | 2,899,433 | 609 edges | 0 | clean |

The working corpora's coverage units were not merged: the committed corpora stay the deterministic seeds and the
adopted finding.

### An unexplained corruption under concurrent load

The first round used `scripts/fuzz.py run` with four workers. Two flight workers died with a wild jump (the program
counter at a stack address, and at an unmapped address), and the artifact one of them wrote replays clean in both
the replay and the libFuzzer executables. The existing, unchanged bundle target did the same under the same load
(a SEGV, and an ASan report of impossible container-annotation arguments: an `old_mid` outside the range the code
clamps it to). Four independent single-process bundle runs side by side (240 s each) reproduced it in two of four
processes (a UBSan object-size report on a `std::span`, a SEGV with the program counter at the faulting address), and
again in two of four with `ASAN_OPTIONS=detect_stack_use_after_return=0` (a UBSan pointer overflow inside
`crc32_bytes`'s loop, and one in `Array::operator[]` on a stack address). One bundle run alone (40,291,401 executions
in 301 s) and every single-process round above were clean, and every artifact replays clean. The corrupted values are register and local values in
code whose inputs cannot produce them. This record does not explain it. It is not an input-dependent reader defect
(no artifact reproduces), and the allocator target ran too slowly under the same load to act as a control. One
candidate is the host under load (this box runs an Intel Core i9-14900K); that is not established. The DIAG.10a
rounds above are therefore single-process, and the observation stays open until a hosted or second-host run settles
it.

## Teeth

Every check added to the three readers was broken on its own, built on win-debug and observed failing, then restored
by rewriting the file and rebuilt (25 breaks):

- flight (6): the armed-trigger and suppression rule, the end-reason rule, the ring rule, the replay-reason rule,
  contiguity, and the clip and detail rule: each makes `crd-fuzz-perf-flight-corpus` fail
  `recorder_could_write`.
- capture (13): the blob-size bound (the wrap seed then passes the reader and fails the reference layout), the
  sample range, thread names, the blob's terminator, offsets and slot count, counter kinds and types, the table
  counts, frame counts, the buffer alignment, allocator names, counter names and the blob-size alignment: each fails
  `crd-fuzz-perf-capture-corpus` at the oracle that names it.
- crd-toml (5): without the value-depth check the assets replay and crd-toml-tests end in a stack overflow
  (0xC00000FD); without the header-depth check, the integer range, the float range or the token length,
  crd-toml-tests fails the case that names it.
- export (1, on `linux-clang-fuzz`): `end - begin` in place of `duration_ns` fails the adopted input under UBSan.

The blob's 8-byte minimum is the precondition of reading its header: without it a 4-byte blob at the end of the
buffer is read 4 bytes past the input, which only ASan with an exact-size buffer (libFuzzer) can observe, so it has
a seed (`blob-header-past-end`) but no tooth of its own.

## Verification

- win-debug: the whole tree built after the engine changes; the full ctest ran once: 7,524 of 7,526 passed. The two
  failures were `crd-master-plan` (this note did not exist yet; it passes now) and `crd-simd-emission-check`, which
  needs `dumpbin` on PATH and passes inside vcvars. On the final source: the whole tree rebuilt, the nine
  `crd-fuzz-*-corpus` CTests pass (bundle 12, flight 30, assets 41, capture 33, the allocator, CEIR and CKIR
  corpora), crd-toml-tests 9 cases, crd-perf-tests 265, crd-perf-ui-tests 21, crd-sandbox-inspect-tests 31, and the
  8 `crd-no-*` and `crd-check*` guards and `crd-master-plan` pass.
- win-shipping and win-clang-cl-shipping (each target rebuilt on the final source, clang-cl linked first time):
  the nine fuzz corpus CTests, crd-toml-tests 9, crd-perf-tests 132, crd-perf-ui-tests 14, crd-sandbox-inspect-tests
  31. `--seed` on both regenerates the flight, assets and capture corpora byte for byte, with every verdict as listed.
- win-asan (inside vcvars): the nine fuzz corpus CTests, crd-toml-tests 9, crd-perf-tests 265, crd-perf-ui-tests 21,
  crd-sandbox-inspect-tests 31; the only sanitizer reports are the deliberate specimens' (heap overflow,
  use after free, the crash-capture null write).
- WSL, one lane at a time: linux-gcc-debug (crd-toml-tests 9, crd-perf-tests 263, crd-perf-ui-tests 21,
  crd-sandbox-inspect-tests 30, the nine fuzz corpus CTests), linux-gcc-asan (the same counts; UBSan without
  recovery; the only reports are the three deliberate specimens') and linux-clang-tsan with `setarch -R`
  (crd-toml-tests 9, the touched crd-perf-tests tags 62 cases, crd-perf-ui-tests 21, crd-sandbox-inspect-tests 30,
  the nine fuzz corpus CTests; no TSan report). `cprof_export` built on every lane.
- `linux-clang-fuzz`: the four perf replay and libFuzzer executables; the rounds above.
- Strict tidy is clean on all 10 changed C++ files (a `memcpy` without its terminator and three local constant
  names were fixed). The crd-toml header is untouched: its toml++-compatible names fail the strict naming rule, so
  the limit lives in toml.cpp. clang-format differs on the new files only in the repository's hand alignment and
  include grouping. The Allman check passes.
- test-repository-tools 65 tests (3 skipped), test-dev-workflow 54, check-master-plan and check-repository pass.

## Remaining in DIAG.10a

- Run records (`decode_record` in crd-ceir-cook); command decoding (`DiagCommandService`); resource reload (pack
  manifests, torn packs, `do_reload_mount`); the compiled and reference CEIR comparison; the virtual-memory and
  streaming allocators; reinvestigating the unknown-origin CEIR and CKIR artifacts; the corruption under concurrent
  load above.
- The hosted run, as the row lists it.
