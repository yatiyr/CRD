# DIAG.10b seeded detector failures, 2026-10-11

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.10b](../ROADMAP.md#slice-diag.10b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-10b); runner:
> [test instruments](../design/test-instruments.md#seeded-detector-failures-diag10b). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.10a container-overflow control](2026-10-11-diag-10a-container-overflow-control.md).

## Goal

DIAG.10b's acceptance names five instrument failures that must make the diagnostic suite fail: broken sanitizer
activation, suppressed reports, a removed guard, a lost fatal record and incorrect symbol mapping, judged from
JUnit and the tests' own output, never from an exit status. This batch builds the runner that seeds them, seeds all
five classes, and fixes the two gaps the seeds found. Coverage measurement and the register of exclusions and
nonreproduced failures are the row's next slices. Committed locally, not pushed.

## The runner

`scripts/diag-seeded-faults.py` reads `tests/support/diag/seeded-faults.json`. A seed is exact source edits (each
seeded text must occur once) or extra environment, the lane facts it applies to (read from the build tree's
`CMakeCache.txt` and compiler identity: `os`, `asan`, `tsan`, `ubsan`, `msvc`, `asserts`, `shipping`), the targets
holding its detecting tests, and those tests' exact CTest names, each with the diagnostic text its failure must
print. Per seed: build and run the tests unseeded (all must run and pass), apply, rebuild, run with
`--output-junit`, restore the bytes with a newer timestamp, require git to report each edited file as before, rebuild,
run again. Verdicts: DETECTED, SURVIVED (a detecting test passed), WRONG_FAILURE (it failed without the named text),
CONTROL_FAILED, ZERO_RUN (a test absent from the JUnit, none selected, no JUnit), BUILD_FAILED (a mutant that does not
compile is never a detection), RESTORE_FAILED, REGISTRY_STALE, NOT_APPLICABLE, TOOL_MISSING, LANE_UNSUPPORTED. The
tests are selected by an anchored, escaped CMake regex because the WSL CMake (3.28) has no `--tests-from-file`.
Catch2 wraps long messages over indented lines, so the named text is matched with whitespace runs collapsed. The
report keeps the seeded run's own verdict beside the final one, so a failed restore never hides what the seed did.

`scripts/test-diag-seeded-faults.py` (31 cases, the `crd-diag-seeded-faults` CTest and a repository CI step) needs
no build. It holds the committed registry to the tree, requires a seed for every acceptance class and every seed to
apply to some CI lane, and drives every verdict through a fake lane, including a restore after a seeded build that
throws and an uncommitted change that must survive a seed. It also enforces the suppression policy that
[test instruments](../design/test-instruments.md) stated: a committed `*suppressions*.txt` entry must name a
third-party shared library (`lib*.so*`).

Two runner defects showed on the first real lanes and are fixed: an environment seed with no build targets ran the
stale test binary of an earlier build (every seed now names its targets, so the control builds the current tree),
and a refusal of uncommitted seeded files blocked verifying a change to those files before committing it (the runner
now restores the exact bytes and compares git's report before and after).

## The seeds

| Seed | Class | Lanes | Detecting tests |
|---|---|---|---|
| `specimens-lose-sanitizer` | sanitizer-activation | ASan, TSan | the standalone sanitizer specimens' cases |
| `lane-loses-sanitizer` | sanitizer-activation | gcc/clang ASan, TSan | the same, with the judging test TU also uninstrumented |
| `asan-report-to-log-file` | suppressed-report | ASan | heap-overflow and use-after-free cases |
| `tsan-report-to-log-file` | suppressed-report | TSan | the data-race case |
| `tsan-suppression-names-cerid-source` | suppressed-report | TSan | the data-race case |
| `lsan-suppression-names-cerid-source` | suppressed-report | Linux ASan | the leak case |
| `asan-continues-after-report` | suppressed-report | MSVC ASan | the heap-overflow case |
| `redzone-rear-check-removed` | removed-guard | all | the rear-overrun case and the allocator fuzz corpus |
| `windows-dump-emptied` | lost-fatal-record | Windows without ASan | the wild-write dump case |
| `linux-crash-record-truncated` | lost-fatal-record | Linux without a sanitizer | the wild-write record case |
| `symbol-identity-half-compared` | symbol-mapping | all | `identity_matches` rejects wrong symbols |

MSVC refuses to link an uninstrumented object with ASan-built engine libraries (`LNK2038` on the STL's
`annotate_string`/`annotate_vector`/`annotate_optional` mismatch records), so the activation seeds strip only the
standalone specimens (heap overflow, use after free, use after return, leak, data race); the first registry version
stripped every specimen and was BUILD_FAILED on win-asan. The whole-lane seed adds `-fno-sanitize=...` to the judging
test TU, which only gcc and clang accept.

MSVC ASan reads `:` as an option separator, so a Windows `log_path` must be quoted (`log_path='D:/...'`); the
registry quotes every seeded path.

## What the seeds found

Before the fix (the committed harness and tests), each run reading JUnit:

| Lane | Seed | Verdict |
|---|---|---|
| win-debug | redzone, dump, symbol | DETECTED, DETECTED, DETECTED (the corpus text was first WRONG_FAILURE: the registry named the wrong oracle text) |
| win-asan | `specimens-lose-sanitizer` | DETECTED (the tests judged by the test binary's own flags) |
| win-asan | `asan-report-to-log-file` | SURVIVED |
| win-asan | `asan-continues-after-report` | DETECTED |
| linux-gcc-asan | `specimens-lose-sanitizer` | SURVIVED: the use-after-return and leak cases passed |
| linux-gcc-asan | `lane-loses-sanitizer` | SURVIVED: all four cases passed |
| linux-gcc-asan | `asan-report-to-log-file` | SURVIVED |
| linux-gcc-asan | `lsan-suppression-names-cerid-source` | DETECTED |
| linux-clang-tsan | `specimens-lose-sanitizer`, `lane-loses-sanitizer`, `tsan-report-to-log-file` | SURVIVED |
| linux-clang-tsan | `tsan-suppression-names-cerid-source` | DETECTED |

Two gaps:

1. **Activation was judged from the binaries.** The use-after-return, leak and data-race cases accepted whatever
   route the specimen announced; the heap-overflow and use-after-free cases accepted whatever the test binary was
   compiled with. A specimen, or a lane, that lost `-fsanitize` therefore reported InstrumentAbsent and passed.
   `crd::diag::lane_promise()` now carries the lane's configuration into the harness (`CRD_ENABLE_ASAN` per
   configuration through `crd_profile_condition`, `CRD_ENABLE_TSAN`, gcc/clang and Linux for the use-after-return
   and leak routes), the cases judge the specimen against it, and each checks that its own binary carries it.
2. **Any death counted as a catch.** `run_specimen` returned SanitizerCaught for every non-zero exit of a
   sanitizer-tagged specimen, and the child's stderr was inherited, never read. It now reads stderr through the same
   pipe as stdout (bounded at 1 MiB, repeated to the test log), requires `ERROR: AddressSanitizer`,
   `ERROR: LeakSanitizer` or `WARNING: ThreadSanitizer`, and otherwise returns Unexpected with the exit code in the
   reason.

The other three classes were already detected by tests that read artifacts: the dump case parses the minidump
(`file_size` and the exception stream), the Linux record case parses the record (`rf.has_regs`), the symbol case
flips a build-id byte past the compared half, and both the rear-overrun case and the fuzz corpus oracle
(`check_all_redzones`) see the missing redzone report.

## After the fix

Every applicable seed is DETECTED on the final source:

| Lane | Seeds run | Verdict |
|---|---|---|
| win-debug | redzone, dump, symbol | 3 DETECTED |
| win-asan | `specimens-lose-sanitizer`, `asan-report-to-log-file`, `asan-continues-after-report`, redzone, symbol | 5 DETECTED |
| linux-gcc-debug | redzone, `linux-crash-record-truncated`, symbol | 3 DETECTED |
| linux-gcc-asan | `specimens-lose-sanitizer` (4 tests), `lane-loses-sanitizer` (4), `asan-report-to-log-file` (2), `lsan-suppression-names-cerid-source` (1) | 4 DETECTED |
| linux-clang-tsan (`setarch -R`) | `specimens-lose-sanitizer`, `lane-loses-sanitizer`, `tsan-report-to-log-file`, `tsan-suppression-names-cerid-source` | 4 DETECTED |

The lane-independent seeds (redzone, symbol) were not repeated on the Linux sanitizer lanes. The failing outputs
carry the reason the tests now print, for example `verdict=Unexpected sanitizer=asan reason=exit 1 without an
AddressSanitizer report in the child's output; Unexpected lane_asan=yes binary_asan=yes`, and for the whole-lane seed
`binary_asan=no`. Every seed's tree was restored byte for byte and rebuilt green; git reported the seeded files as
before each time.

One run of `redzone-rear-check-removed` on win-asan ended RESTORE_FAILED: after the restore and rebuild,
`crd-fuzz-memory-allocators-corpus` died 0.11 s into the process with an ASan `use-after-poison` inside
`RtlCreateHeap`, reached from `msvcrt.dll` during process start, before any corpus input. The immediate rerun of the
same executable passed, and the seed's next full run was DETECTED with a clean restore. It is recorded as a
nonreproduced event under the [host lesson](2026-10-11-diag-10a-container-overflow-control.md), not chased.

## Verification

- win-debug: crd-perf-tests (266 cases), crd-jobs-tests (174), crd-memory-tests (163) and crd-core-tests (35) whole,
  and their 638 CTests; the 8 `crd-no-*` guards, `crd-diag-seeded-faults`, `crd-repository-tools`,
  `crd-developer-workflow`, `crd-master-plan` and `crd-repository-hygiene` CTests.
- win-shipping and win-clang-cl-shipping (clang-cl linked first time): the four executables built and passed whole
  (perf 133, jobs 166, memory 162, core 27).
- win-asan inside vcvars: the four executables whole (perf 266, jobs 174, memory 163, core 35).
- WSL, one lane at a time: linux-gcc-debug, linux-gcc-asan and linux-clang-tsan (`setarch -R`, the preset's
  `TSAN_OPTIONS`) each ran the four executables whole (core 27, jobs 173, memory 163, perf 264) and the
  `crd-diag-seeded-faults` CTest (10 s through the 9p mount on linux-gcc-asan, hence its 120 s budget). The only
  sanitizer reports in their output are the deliberate specimens', now repeated from the children's stderr.
- Teeth: the pre-fix runs above are the teeth of both harness fixes (the old harness and tests, SURVIVED; the new,
  DETECTED). Adding `race:data_race_specimen.cpp` to the committed TSan suppressions failed the suppression policy
  test; restored, it passes.
- `test-diag-seeded-faults.py` 32 cases (Windows and WSL Python); strict tidy clean on the five changed C++ files;
  clang-format differs only in the repository's hand alignment, case-label indentation and declaration wrapping;
  Allman check; test-repository-tools (65), test-dev-workflow (54); the pinned actionlint 1.7.12 on `ci.yml`;
  check-master-plan and check-repository.

## Retry and quarantine

No CI step retries a test: `ci.yml` and the CTest wrappers carry no `--repeat` or `--rerun-failed`, and
`check-registered-failures.py` admits only failures registered against a reproduced third-party defect. The runner
itself never reruns.

## Remaining

- Coverage: branch, region, error-path and state-transition coverage of the DIAG scope with its instrumented
  denominator (clang source-based coverage on a Linux lane; MSVC has no equivalent route), and the exclusions as
  owned evidence.
- A register of flaky and nonreproduced failures as owned evidence (the host misexecution events of DIAG.10a and the
  one below are the first entries).
- The seeds on hosted lanes, and the hosted run of this batch.
