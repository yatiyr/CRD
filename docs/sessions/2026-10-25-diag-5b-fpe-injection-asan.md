# DIAG.12: DIAG.5b's SIGFPE mode, record-write injection and ASan records, 2026-10-25

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.12](../ROADMAP.md#slice-diag.12). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-5b),
> [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md). Rules: [AGENTS](../../AGENTS.md).
> Previous batch: [UBSan and use-after-scope](2026-10-25-diag-3f-ubsan-use-after-scope.md).

## CI read

No completed run has a failed job. 40140261458 to 40126129075 were cancelled with no failed job; 40140275224 (head
`01562de5`) and 40142523714 (head `471a7f26`) are still running, with no failed job so far.

## Change

DIAG.12's re-check left three DIAG.5b items: no SIGFPE mode, a Linux writer that ignored the failure-injection seam,
and ASan records nobody had measured.

- **SIGFPE.** The crash specimen's `fpe` mode unmasks `FE_DIVBYZERO` and divides by zero in floating point. An
  integer division would be undefined behaviour, which UBSan reports before the instruction traps. The record holds
  signal 8 and `code 3` (`FPE_FLTDIV`). The specimen's TSan options add `handle_sigfpe=0`; without it TSan took the
  re-executed fault and exited 66.
- **Injection.** `s_inject_step` and `test_inject_write_failure` are now defined once for both backends. The Linux
  record writer honours `DumpFailed`: the write runs, then the seam fails it and the file is unlinked. `install()`
  clears the seam, as on Windows. The specimen's `report.marker` is written on Linux too, with
  `open`/`write`/`close` inside the handler. Its path helper `output_file` also serves `chain.marker`.
- **ASan records.** Measured on linux-gcc-asan before any assertion was written:
  - UBSan's null check stops every null-store mode before the store runs (`av`, `concurrent`, `denied`,
    `logger_lock`, `chain`, `hook_fault`, `fiber`, `inject_dump_fail`). They exit 1 with
    `runtime error: store to null pointer`, and crd's handler never runs.
  - SIGILL (`fastfail`) and SIGABRT (`assert_default`) behave as on the plain lanes: ASan's `handle_sigill` and
    `handle_abort` are off.
  - The overflow modes and `fpe`: crd writes one record, then ASan reports and exits 1. ASan gives every thread an
    alternate stack, and crd borrows it, so even the unguarded worker's overflow is recorded there.

Two defects turned up while measuring, and both are fixed:

- **The chained handler got a fabricated fault.** crd re-raised the signal after recording it, so ASan received a
  `tkill` siginfo. It reported `SEGV on unknown address 0x03e8000004d7` (uid and pid), with the `raise` site as pc.
  Now a fault the kernel raised (`si_code > 0`) returns from the handler instead. The instruction faults again under
  the restored handler, which sees the real siginfo and context. ASan then reports `stack-overflow on address` and
  `FPE`. A sent signal (abort's SIGABRT, `kill`) is still re-raised. The plain lanes' exit codes do not change.
- **A sent signal's code was printed unsigned.** `SI_TKILL` (-6) was written as `4294967290`, so the test parser
  stopped at that token. The record now writes `code -6`. The parser's `ok` requires the register dump, so it no
  longer rejects a negative code.

The Linux cases judge a sanitizer by `cd::lane_promise()`. The null-store modes check the `ubsan` promise, and the
chained modes the `asan` promise. The Windows block uses the `asan` promise too, so `built_with_asan` is deleted from
the crash-capture test. The build-id case now uses `fastfail`, which every Linux lane records. Its sanitizer fork
and its own `built_with_asan` are deleted.

The host application's crash case claimed "the sanitizer owns the fault" for the same null store. On a UBSan lane it
now asserts UBSan's report and no capture. Seed `linux-crash-record-truncated` follows the writer's `bool ok` line.
No file was added except this note.

## Teeth

All four teeth were applied together, then the lanes were rebuilt and rerun.

- linux-gcc-debug failed only the three cases each tooth targets:
  - injection check off: the injected-failure case (one record, marker 0);
  - `feenableexcept` removed: the `fpe` case (exit 61);
  - minus sign dropped: `assert_default` (`code 6`).
- linux-gcc-asan:
  - `raise` restored for every signal: the four overflow cases fail, because ASan's report no longer says
    `stack-overflow`.
  - `fpe` and `assert_default` fail, as they do on debug.
- After the restore, both lanes were rebuilt and rerun, and all 23 selected cases pass.

## Verification

- linux-gcc-debug and linux-gcc-asan (`wsl-lane.sh`): crd-core-tests, crd-perf-tests and both specimens build.
  CTest `^crash (capture|contract|record)` and the host crash case pass, 23 of 23 on each lane.
- linux-clang-tsan: the specimen's `fpe` exits 136 with one record (`code 3`), and `av` exits 139 with one record.
- win-debug: `[crash-capture],[crash]` 27 cases pass, and the host crash case passes. win-asan:
  `[crash-capture],[crash]` 27 cases pass.
- The ctest guards pass (15). Seed `linux-crash-record-truncated` is DETECTED on linux-gcc-debug, and the registry
  validates. tidy-files is clean on all 6 files. Allman, check-master-plan and check-repository pass.
- clang-format: the only findings on added lines are hand alignment of consecutive declarations, the file's style.

Hosted once pushed:
- the Linux `crash capture` cases, `fpe` and the injected failure included, pass on every Linux lane, with the
  sanitizer expectations on linux-gcc-asan;
- `crash record (linux)` and the host crash case pass on every Linux lane;
- the Windows `[crash-capture]` cases pass on win-debug and win-asan;
- seed `linux-crash-record-truncated` stays DETECTED on linux-gcc-debug.
