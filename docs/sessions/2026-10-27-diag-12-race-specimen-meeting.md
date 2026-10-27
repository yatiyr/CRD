# 2026-10-27 — DIAG.12: the race specimen's jobs meet before they add

<!-- doc-role: session -->
Row: [DIAG.12](../ROADMAP.md#slice-diag.12).

## CI read

- Push run 40479322092 (`f91847f5`) failed one job: linux-clang-tsan, one test of 7,637, `diagnostic host:
  ThreadSanitizer catches the application's job race, and the repaired one runs clean`. The racing child printed
  `CRD_HOST_TALLY 400000`, exited clean and the verdict was `Unexpected`: no addition was lost and ThreadSanitizer
  reported nothing.
- Runs 40495096865, 40519993132 and the earlier ones were cancelled before any job ran; 40515770381 and 40520638197
  are queued. Needs CI audit: [DIAG.11c](../ROADMAP.md#slice-diag.11c) keeps its evidence; DIAG.12 needs the case
  below on a linux-clang-tsan run at or after this commit. Both rows follow SIMD-DISPATCH, so neither can flip.

## Cause and repair

`race_job` ran its 200,000 additions with nothing making the two jobs overlap. Main helps in `crd::jobs::wait`, so
when it took both jobs before a worker woke (or a worker finished one before the other started), one thread ran
both, the fiber switch between them is a ThreadSanitizer synchronization, and there was no race to catch.

Both jobs now meet on a relaxed counter before their loop (`diag_host_specimen.cpp`). A job that waits there keeps
its thread, so the second job must run on another thread at the same time; relaxed operations order none of the
additions after the meeting, so the plain additions stay a data race on every schedule. With three pool threads and
two jobs the meeting always completes. The repaired run meets the same way and still counts 400,000 (win-debug).
