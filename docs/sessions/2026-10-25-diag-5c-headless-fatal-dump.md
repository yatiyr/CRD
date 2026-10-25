# DIAG.12: a Windows dump of a headless fatal, and no precompiled header under the cache, 2026-10-25

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.12](../ROADMAP.md#slice-diag.12). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-5c),
> [build performance](../design/build-performance.md#the-opt-in-module),
> [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md). Rules: [AGENTS](../../AGENTS.md).
> Previous batch: [DIAG.5b's SIGFPE, injection and ASan records](2026-10-25-diag-5b-fpe-injection-asan.md).

## CI read

No completed run has a failed job. 40142523714 and 40140261458 to 40131739936 were cancelled with no failed job;
40140275224 (head `01562de5`) is still running with no failed job so far; 40145072071 is pending.

## The headless fatal on Windows

DIAG.5c's last named limitation: a headless `CRD_FATAL` aborts, and on Windows `abort()` never reaches the
unhandled-exception filter, so the crash handler wrote no dump (Linux records the SIGABRT).

- UCRT's `abort()` calls a SIGABRT handler before it exits (`ucrt/startup/abort.cpp`). `install()` now registers one
  and `uninstall()` restores the previous disposition. The handler dumps the aborting thread through the fatal path
  with `kAbortExceptionCode` (`STATUS_FATAL_APP_EXIT`, the code Windows records for an abort), chains a previous
  handler, and returns; `abort()` then exits 3, or fails fast when `_CALL_REPORTFAULT` is kept.
- UCRT resets the disposition before it calls the handler (`ucrt/misc/signal.cpp`, `raise`), so the handler re-arms
  itself first: a concurrent abort then meets the single-shot gate and waits for the dump instead of exiting under
  it. The window between the reset and the re-arm is the bound.
- `fatal_on_this_thread` builds the record and this thread's context; the abort handler and `test_fatal_path` share it.
- Every `abort()` takes this route: `CRD_FATAL`, `default_fatal_invariant`, `fiber_abort`, `std::terminate`.
- Measured, not assumed: win-asan behaves as win-debug (exit 3, one dump, `ExceptionCode=0x40000015`); ASan's
  Windows runtime leaves `abort()` to the CRT. The crash-capture case's ASan branch, which claimed ASan owned the exit,
  is deleted.

Tests: `crash capture: a fatal assert with no handler under headless terminates promptly and dumps the aborting
thread` checks exit 3, one `crash_` dump, its exception code and that its exception thread is the one the specimen
printed (`CRD_ABORT_TID`), and that the module list names the specimen. The host application's `invariant`, `assert`
and `use-after-free` runs leave one `crash_` dump on Windows (`check_fatal_end`); the repaired runs leave none.

Teeth on win-debug: the `signal()` call removed fails the crash-capture case and both host cases (0 dumps); the code
`+1` and the thread id `+4` fail the code and thread checks. Restored, rebuilt, and all 55 selected cases pass.

## The cache served a stale precompiled header

The first linux-gcc-debug build of this batch failed in four crd-perf units: `assert.hpp` included a second time
after the precompiled header ("default argument given for parameter 4"). The `.gch` was a cache hit
(`sccache --show-stats`: 1 request, 1 hit); the same `.gch` and unit commands without the launcher compiled clean.

GCC records each `#pragma once` header's size and MD5 in a `.gch` and skips a later `#include` of it only when the
bytes match; sccache keys the `.gch` by the preprocessed text. The batch's `assert.hpp` comment edit kept the line
count, so the text and the key were unchanged and the old `.gch` came back; every unit that missed for another reason
(here `crash.hpp`) then included `assert.hpp` again. The working copy's `assert.hpp` was also CRLF under `eol=lf`
(the same mechanism); it is LF now. Hosted cached GCC lanes share the key, so this push would have failed them.

The fix removes the class instead of the trigger: a cached tree has no precompiled header on any compiler.
`compiler-cache.py configure-args` adds `-DCRD_ENABLE_PCH=OFF` for every compiler (the off route `=ON`), the two
Linux cache configure steps of `ci.yml` pass it, and `CrdBuildPerformance.cmake` stops a configure that names the
launcher with the precompiled header on (it was a warning, MSVC only). Clang's `-Xclang -fno-pch-timestamp` existed
only for a cached precompiled header and is deleted. Cost: the cached Linux lanes give up the Catch2 test header's
uncached gain (5.7 % cold, 3-11 % on a widely included header) and their first run misses every unit.

The three `assert.hpp` macro findings strict tidy reports once the header is named (`CRD_VERIFY`, `CRD_FATAL`,
`CRD_ASSERT_UNREACHABLE`) carry `NOLINTNEXTLINE(cppcoreguidelines-macro-usage)`: each reports the caller's file and
line.

## Verification

- win-debug: crd-core-tests, crd-perf-tests and the crash-capture, host and flight-fatal specimens build; CTest
  `crash capture|crash contract|^diagnostic host:|^flight trigger:` 55 of 55 pass.
- win-asan (inside vcvars): the same targets; `crash capture|^diagnostic host:|^flight trigger:` 39 of 39 pass.
- linux-gcc-debug, linux-gcc-asan and linux-clang-tsan reconfigured through `wsl-lane.sh configure` (each now
  `-DCRD_ENABLE_PCH=OFF`); linux-gcc-debug builds the four targets, and CTest
  `^crash (capture|contract|record)|^diagnostic host:|^flight trigger:` passes 50 of 50.
- Tooling: `test-repository-tools.py` (121, the module test with a new refusal case on both compiler families) and
  `test-dev-workflow.py` (59) pass; the pinned actionlint 1.7.12 is clean on `ci.yml`.
- Gates: the 15 ctest guards pass; strict tidy is clean on all 7 changed C++ files; `check-headers.py` passes both
  headers; Allman, check-master-plan and check-repository pass. clang-format's only findings on added lines are hand
  alignment of consecutive declarations, the files' style.

Hosted once pushed:
- every Windows lane, win-asan included: the crash-capture abort case passes with one dump;
- every Windows lane outside ASan: the host's `invariant`, `assert` and `use-after-free` cases pass with one dump;
- every cached Linux lane configures with `CRD_ENABLE_PCH=OFF` and builds; the Linux crash-capture and host cases
  pass unchanged.
