# DIAG.10a unknown-origin CEIR and CKIR artifacts, 2026-10-11

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.10a](../ROADMAP.md#slice-diag.10a). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-10a); harness:
> [test instruments](../design/test-instruments.md). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.10a virtual-memory and streaming allocators](2026-10-11-diag-10a-vm-streaming-allocators.md).

## Goal

The DIAG.10a acceptance asks for the retained unknown-origin artifacts to be reinvestigated "under the strengthened
harness without converting a clean replay into an invented explanation". Six such inputs are in the committed
corpora: REPO.DEV.11 ([infrastructure audit](2026-09-14-infrastructure-audit.md)) adopted them after a long-lived
libFuzzer process reported them and they replayed clean. The row also carries the open corruption seen under
concurrent load. This slice strengthens the four loader targets, replays the six on them with every instrument this
host runs, and records what the corruption looks like now. Committed locally, not pushed.

## The strengthened harness

What changed in the shared harness since 2026-09-14 (`git log -- tests/support/fuzz cmake/CrdFuzz.cmake`): the
`BudgetAllocator` refuses a request whose sum would wrap and finds its payload by pointer arithmetic (DIAG.10a
allocator batch), the replay driver is headless and fails on a Debug CRT report instead of a dialog (allocator and
reload batches), and seed lists can be checked as they are written (`VerdictSink`). None of that touches what the four
loader targets check, so this slice adds the oracles their own headers state and nothing tested:

- `fuzz_ceir_text.cpp`: an accepted module also serializes, deserializes and prints back to the same text
  (`binary.hpp`: print . deserialize . serialize == print).
- `fuzz_ceir_binary.cpp`: an accepted module also prints text that `parse` accepts and prints identically, so every
  module the binary loader accepts has a text form. The text drops `Region::kind` and source positions, so text is
  compared, not blobs.
- `fuzz_ckir_binary.cpp`: an accepted graph also writes `.ckir` text that `ckir_read` reads back to the same blob
  (`ckir_asset.hpp`: serialize_graph(ckir_read(ckir_write(g))) == serialize_graph(g)); on a refusal it prints the
  text. The CKIR text target already compared the blobs of its two reads.

## The six artifacts

linux-clang-fuzz (clang 18.1.3, ASan and UBSan) rebuilt from this tree; repetitions use libFuzzer's `-runs=10000
<file>` (one process, the input executed 10,000 times) with `ASAN_OPTIONS=detect_stack_use_after_return=1:
strict_string_checks=1:check_initialization_order=1:strict_init_order=1:detect_leaks=1:halt_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. Valgrind 3.22 memcheck (`--track-origins=yes --leak-check=full
--error-exitcode=99`) ran the unsanitized linux-gcc-debug replay executables over the whole of each corpus, on the source after finding 1 below and before findings 2 to 5; those later fixes add refusals on paths none of the six inputs reaches, so the result stands for them.
Verdicts are the loader's own (a temporary print in the harness, removed before the commit).

| Artifact | Target | Round-four symptom (2026-09-14) | Loader verdict now | Alone | 10,000 repetitions | memcheck |
|---|---|---|---|---|---|---|
| `553e7ecf` | ceir-text | a jump to a stack address | accepted, round-trips through both forms | pass | pass, 73.7 s | 0 errors |
| `d85628dd` | ceir-text | an unknown-crash in `parse_type` | refused: unknown type keyword at byte 426 | pass | pass, 41.1 s | 0 errors |
| `183a3e2e` | ceir-binary | a stack-buffer-underflow through a `string_view` comparison | refused: the op name is not an identifier (`tes).sink`, the defect below) | pass | pass, 45.2 s | 0 errors |
| `e18eb14e` | ceir-binary | a misaligned `string_view` pointer | refused: unknown attribute kind at byte 1002 | pass | pass, 53.2 s | 0 errors |
| `34ce93c0` | ckir-binary | an unsigned offset subtraction overflow | refused | pass | pass, 39.0 s | 0 errors |
| `c7c59902` | ckir-binary | a stack-buffer-underflow in `Cursor::u8v` | refused | pass | pass, 36.7 s | 0 errors |

None reproduces its symptom on the strengthened harness, under memcheck (an instrument independent of the clang
sanitizers), or in 10,000 in-process repetitions. They stay adopted and labelled unknown. `183a3e2e`'s new refusal
comes from a property of its bytes (a corrupted op name) that the cross-format oracle exposed; it does not explain a
memory error in round four, and is not recorded as one.

## Findings of the new oracles (all fixed)

1. **Unprintable names in a CEIR blob** (committed `183a3e2e` and `316cdfb2` failed the binary oracle at once). The
   binary loader accepted any byte string where the printer writes a name bare: an op name `tes).sink`, a trait
   `2rd`. `binary.cpp` now requires `[A-Za-z_][A-Za-z0-9_.]*` (the lexer's `is_ident_start`/`is_ident_char`) for op
   names, struct, enum, param and trait names, field and case labels, symbolic dims and type- and attr-class names,
   and refuses an empty symbol ref, which the parser refuses as `@""`. An Extern type's name and labels print quoted
   and keep any bytes. Ten written seeds (one per rule), each checked refused as it is written.
2. **A type attribute naming no type** (libFuzzer, `16169aac`). Type ref 0 decoded to an invalid `TypeId`, which
   `attr_type` accepted; printing it asserted in `type_of` (an out-of-range read where asserts are off). Refused as
   "ATTR type value names no type".
3. **Zero-member tuples and variants** (libFuzzer, `fd58ba03`). `type_is_well_formed` allows any member count, the
   factories build `!tuple<>` and `!variant<>` and the printer writes them, but `parse` required one member. The parser
   now accepts the empty list as it does for `shape<>`; `crd-ceir-tests` gains a `[diag10a]` round-trip case.
4. **Non-canonical CKIR records** (libFuzzer, `b501bfff`: dim 7 of a rank-1 shape held `0xfe00000000000000`). The
   blob loader checked only references. `records_canonical` in `ckir_serialize.hpp` now refuses out-of-range op,
   dtype, type kind, tier, statement kind, barrier scope, stage, depth mode and interpolation bytes (the text writer
   indexes its name tables with them), a rank outside [0, 8], a nonzero dim past the rank and a stage-output slot past
   `n_out` that is not the default. Seven written seeds, each checked refused.
5. **INT64_MIN in `.ckir` text** (libFuzzer, `1e9bee27`). The writer emits `-9223372036854775808` for a shape dim and
   the reader's magnitude check refused it as out of range. The limit is now one larger for a negative value.

## Teeth

Twelve breaks on win-debug, each built and replayed, each failing, each restored and rebuilt (the runner script
restores the source in a `finally` block; the restored lanes replay green):

| Tooth | Break | Observed |
|---|---|---|
| T1 | op-name identifier check removed | ceir-binary replay: parse rejected print output (`183a3e2e`) |
| T2 | type name and label check skipped | ceir-binary: expected trait name (`316cdfb2`) |
| T3a, T3b | type-class, attr-class identifier checks removed | ceir-binary: type-class / attribute-class name must be 'dialect.x' |
| T4 | empty symbol check skipped | ceir-binary: empty symbol name |
| T5 | decoder drops each op's last attribute | ceir-text: the new cross-format oracle (printed text differs) |
| T6 | `.ckir` writer writes `kernel_body_count + 1` | ckir-binary: `ckir_read` rejected the writer's output |
| T7 | type-ref-0 check skipped | ceir-binary: assertion in `type_of` (`16169aac`) |
| T8 | parser requires a tuple member again | ceir-binary (`fd58ba03`) and the new `crd-ceir-tests` case |
| T9 | `records_canonical` not called | ckir-binary: blob mismatch (`b501bfff`) |
| T9b | dims past the rank not checked | ckir-binary: blob mismatch |
| T10 | reader magnitude limit back to INT64_MAX | ckir-binary: integer out of range (`1e9bee27`) |

The written seeds' refusals are also checked whenever the corpus is regenerated (`--seed` ends the run naming a seed
whose verdict changed). That check, not a tooth, is what covers each CKIR enum-range rule (op, dtype, tier, stage)
on its own: T9 removes the whole check and bites first on the dims seed, and an out-of-range enum without the check
reads past the writer's name tables, which only the ASan lanes observe.

## libFuzzer rounds (linux-clang-fuzz, one process each, working corpora under `~/b11`, committed corpora read-only)

| Target | Time | Executions | Outcome |
|---|---|---|---|
| ceir-binary | 540 s | 322,574 | finding 2 |
| ceir-binary | 540 s | 2,875,795 | finding 3 |
| ceir-binary | 540 s | 5,666,415 | clean |
| ceir-text | 540 s | 443,955 | an unexplained report (below) |
| ckir-binary | 540 s | 432 | finding 4 |
| ckir-binary | 540 s | 2,147,332 | finding 5 |
| ckir-binary | 420 s | 3,951,636 | an unexplained report (below) |

The new units were not merged into the committed corpora (the open decision for every target).

## The corruption of unknown origin

Three events this batch, all in single processes on targets that do not link crd-perf:

- ceir-text round: UBSan `member call on address ... with insufficient space for an object of type
  'const crd::containers::Array<unsigned char>'` at `array.hpp:618` (`annotate_size`). The written crash file has
  the same SHA-1 as its base unit (`cd301be8`), a unit the process had already run. It replays clean alone (3 times),
  in 3 x 10,000 and 2 x 3,000 in-process repetitions, and in 164 x 2,000 sequential repetitions (328,000 runs, 8
  minutes). Once, in a 10,000-repetition run right after the round, it failed with an ASan container-overflow: a
  write of the byte at index 109 of a 135-byte `Array<u8>` in `put_u32`, whose shadow showed the live boundary at
  109, the boundary the same `push_back` had just moved to 110. The shadow is otherwise exactly what the annotation
  of a 135-byte buffer produces, so the state contradicts the code that wrote it.
- ckir-binary round 3: ASan SEGV, a WRITE to address 4 at `asan_poison.hpp:126:1`, the closing brace of
  `asan_annotate_live_range`, again on a unit identical to its base (`ace35b97`). Three 10,000-repetition runs clean.
- Both inputs are adopted, with this repetition evidence, under the REPO.DEV.11 rule for a finding that replays clean.

What bounds it: no input reproduces it; it has now appeared on bundle, flight, ceir-differential, ceir-text and
ckir-binary targets, with and without crd-perf, with and without stack-use-after-return detection (the evidence-readers
session); the Windows Application log shows `cl.exe` access violations in `c1xx.dll`/`c2.dll` at ten different
offsets over 30 days (and Python crashes), processes with none of this code; WHEA logged nothing. A known-answer probe
(single-threaded integer hashing, a buffer shuffle, `memcpy` and double arithmetic over 12 MiB, the digest compared
with the first pass) found no wrong result: WSL g++ -O2, one process 240 s (48,059 passes) and 24 processes 240 s
(about 53,000 passes); Windows MSVC /O2, one process 120 s (13,426 passes) and 24 processes 240 s (about 51,000 passes).
The probe is scratch, not committed.

What it does not settle: all three of this batch's sites are in `crd::containers::Array`'s container-annotation path
(`annotate_size`, `asan_annotate_live_range`, a byte `push_back` just after its annotation). Array is the most common
code in every target, so this may be where corrupted state is most often noticed, or it may point at the
annotation interface. The next local step is a timed control: the same rounds with
`ASAN_OPTIONS=detect_container_overflow=0` against the default, long enough to compare rates (the events are about
one per few million executions, and ceir-binary ran 8.9 million clean). The host is an Intel Core i9-14900K
(microcode 0x12F, BIOS 2001); that part has a widely documented instability history, and a CPU already degraded is
not repaired by a microcode update, but nothing here shows that this CPU is the cause. A run of the same rounds on
another machine is the discriminating experiment and is the user's.

## Verification

- win-debug: the whole tree built (242 steps); the full CTest inside vcvars passed 7,549 of 7,550, the one failure
  `crd-master-plan` because this note did not exist yet (it passes now; one test is skipped by its own gate, as
  before). On the final source the four loader corpora replay (27, 30, 13 and 30 inputs), `crd-ceir-tests` passes
  576 cases, and the 8 `crd-no-*`/`crd-check*` guards, `crd-master-plan` and the loader corpus CTests pass.
- win-shipping and win-clang-cl-shipping (touched targets built; clang-cl linked first time): the four
  corpora, `crd-ceir-tests` 576 cases, `crd-kir-tests` 313, `crd-ceir-cook-tests` 108; `--seed` regenerates the CEIR
  and CKIR binary seeds byte for byte (20 files each).
- win-asan (inside vcvars): the four corpora and `crd-ceir-tests` 576 cases; `crd-kir-tests` (315 CTests) and
  `crd-ceir-cook-tests` (109) through CTest, each case in its own process; seeds byte for byte; no sanitizer report.
- WSL, one lane at a time: linux-gcc-debug (corpora 3 times each, ceir 576, kir 313, cook 108, seeds identical),
  linux-gcc-asan (the same; `crd-kir-tests` through CTest, 315 passed, after a single-process run hit the script's
  25-minute limit inside the hair render case), linux-clang-tsan with `setarch -R` (corpora 10 times each, the
  `[diag10a]`, `[roundtrip]` and `[binary]` CEIR cases and the `[asset]` CKIR cases, no TSan report),
  linux-clang-fuzz rebuilt on the final source (the four corpora replay).
- Strict tidy (LLVM 20.1.8) is clean on all 8 changed C++ files. clang-format differs only in the repository's hand
  alignment, include grouping and line breaks. The Allman check, `check-master-plan.py` and `check-repository.py`
  pass. No script or workflow changed.

## Not done here

- The container-overflow control above, and the other-machine run (user).
- Merging this batch's libFuzzer units into the committed corpora (the open decision).
