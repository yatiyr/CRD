# 2026-11-01 — DIAG.11b clause (8): the third recording, the compared quantity and the machine's other load

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [DIAG.11b](../ROADMAP.md#slice-diag.11b) clause (8). Before: [the user's two recordings](2026-11-01-diag-ci-asan-shards.md#diag11b-clause-8-the-users-two-recordings).
Also: the [Needs CI audit](#needs-ci-audit) of HW-RESILIENCE.2, DIAG.11c and DIAG.12.

## The recording

`20261101T004352Z`, recorded by the user from an elevated prompt in the clean worktree `D:\Dev\cerid-diag11b` at
`d64f2ce8` (unmodified; both harness trees built there, so `record`'s currency check held). The calibration sized the
frames to 3.2 s of off frames: alloc 37,896, jobs 14,989, offline 9,300, io 4,117. Every process's frames now cover
at least 85% of it.

At record time (the `d64f2ce8` analysis, kept as `analysis-at-record.json`): UNRESOLVED, 0 DISAGREES. The two rows the
re-recording was asked for agree: jobs basic over off measured +0.0% (CPU), external +0.2%; jobs full +39.7% and
+40.0%, the diagnostic code +20.0% and its call sites +21.9%. Unresolved on spread: io basic, full and compiled-out
(off's processes' median frames 3.0% apart), jobs compiled-out (2.2%), offline basic (4.1%).

## Finding 1: the spread gate read a quantity the row does not compare

A row compares the harness's CPU per frame when both cells recorded it; the gate that requires a cell's processes to
agree read their median frame, a wall time. io's frames wait on the file system: off's two processes read 655.4 and
675.0 us (3.0%) but 3,999,281 and 4,041,128 cycles per frame (1.0%). The gate now reads the compared quantity (CPU per
frame, else the median frame), at the same 2% (`sample-overhead-wpr.py`).

It is not a looser gate: in the two `b562ba6e` recordings io's CPU per frame spread more than its wall time (off 5.4%
against 3.8%; 7.8% against 4.0%), and offline full's 5.4% wall spread is 6.2% in CPU. The 2026-10-25 trace recorded no
CPU measurand, so its jobs basic case (393.1 against 470.5 us, 19.7%) is still judged on the median frame.

## Finding 2: the machine's other load, read from the trace

The analysis now counts, for every recorded process from its first to its last sample, the samples of every other
process but Idle: the share of the pinned processors' sampling slots they took, the logical processors they kept busy
overall and the busiest of them (by name, at 100 ms). An unresolved or disagreeing row names it. In this recording
every process ran beside 2.6 to 5.3 logical processors of other work and 3.4% to 22.0% of the pinned processors' slots
(the busiest WmiPrvSE.exe at up to 1.1 processors, claude.exe, clangd.exe).

## Re-analysis at the new gate

`analyze` of the same trace with the repaired script: DISAGREES, worst cell io full.

- io basic AGREES (+0.3% measured, +0.0% external), io compiled-out AGREES (+1.0%, +1.7%; no out-of-line diagnostic
  code).
- io full DISAGREES on location: measured -0.5%, external -1.3%, the diagnostic code +0.4% and its call sites +1.0%,
  elsewhere -2.7% (`fs::read_file_binary` -1.1%, `detail::read_range_into` -0.7%), 0.4 points past the tolerance.
  Off's second process is 1.0% dearer in CPU per frame than its first and ran beside the most other work of the io
  cells (11.8% of the pinned processors, 4.1 processors); full's two processes read 3,999,772 and 4,002,049 cycles,
  within 0.01% of off's first. The full mode is not cheaper; off's reference is lifted by one contended process.
- jobs compiled-out stays UNRESOLVED (CPU per frame 2.5% apart; its slower process ran beside 3.4 processors, the
  faster beside 3.6, so the other load does not explain it), offline basic UNRESOLVED (5.9%; its slower process
  beside 5.3 processors against 3.1).
- Every other row agrees as before: alloc basic, full and compiled-out, jobs basic and full, offline full and
  compiled-out.

The recording proves clause (8)'s jobs rows. The io full disagreement and the two unresolved rows were recorded on a
machine carrying three to five processors of other work; the next recording, on a machine without builds, WSL lanes
or other heavy programs running, decides them, and its lines now name any load that remains.

## Tests

`test-sample-overhead-wpr.py` (26 cases): the io pair agrees on CPU and stays unresolved when its CPU spreads 3.0%;
without a CPU measurand the median frame decides; foreign samples are counted by processor inside a process's window
(Idle excluded, a reused PID under another image counted); an unresolved and a disagreeing line name the load. Teeth:
the gate forced back to the median frame fails two cases; Idle counted as another process fails one.

## Needs CI audit

No completed run has a failed job. Complete-tier run 41269105413 (`b0b1b245`, running) failed its Windows builds on
`filesystem.cpp` C4003 and `checksum.cpp` C4324, fixed on main by 9723ac75 and 921e19cc.

- HW-RESILIENCE.2: in 41269105413, linux-gcc-arm64-debug and linux-clang-arm64-debug pass the wild-write case (2,616 of
  2,616 tests each); win-arm64-debug failed on C4324, so the row waits on queued run 41274380605 (`de5e10b0`).
- DIAG.11c and DIAG.12: 41274380605 is still the next candidate; nothing dispatched (one run queued, one running).
