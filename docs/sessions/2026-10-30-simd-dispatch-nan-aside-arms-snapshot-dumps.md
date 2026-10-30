# 2026-10-30 — SIMD-DISPATCH: push run 40993784639's failures (NaN-aside arm values, async temp names, snapshot dumps)

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch) (CI first; Left: a green win-asan run).
Before: [the fft board](2026-10-30-simd-dispatch-fft-board-whole-codelets.md).

## CI first

Push run 40993784639 (87695de9) was still running, with win-debug in progress, but 15 of its jobs had already failed.
Nothing was cancelled or dispatched. Needs CI audit: no completed run is green, so DIAG.11c and DIAG.12 still wait on
a green complete-tier run, and nothing flips.

- **Seven linux-gcc jobs**: `-Werror=range-loop-construct` at test_svd.cpp:1636, already fixed at 2db8e32c.
- **win-tidy**: `bench_hesap_amg_parallel.cpp`'s phase enum had a `u32` base and `k`-prefixed constants
  (`performance-enum-size`, `readability-identifier-naming`). It now has a `crd::u8` base and CamelCase constants.
- **win-shipping, -shipping-profile, -release, -relwithdebinfo** (`spgemm_arm`, `batch_gradient picks`) and
  **win-clang-cl-shipping** (`ldlt_arms` and its four `-kernels-at-<level>` arms): the same cause, below.
- **win-asan** (the first run at three cases at a time): the async-file collision and the live-modules dump, below.

## Arm values compared NaN payloads aside

The failures were only `c32` and `c64` with specials (spgemm), `fold_jobs 1` with specials (batch_gradient), and
`special 2`, the NaN fixture, on one blocking arm (LDLT). `rows_same` (the per-sample gradients) never failed, and
real types with the same specials passed. So every finite value matched. Only NaN bits differed: where a parallel
body meets two NaNs in a commutative add, or a negated product, its compiled operand order differs from the serial
body's. ADR-0134 §7 leaves that payload open ("NaN payloads aside"). Three suites already apply that rule, each with
its own copy.

`tests/support/kernel-levels/nan_aside.hpp` holds one helper for it. It compares bit for bit except that two NaNs
agree, compares complex values by part, and attaches the first differing bit patterns to the failing CHECK. Its users:

- `same_csr`'s values (spgemm);
- `same_span` in test_front_walk_arms.cpp and test_lu_arms.cpp, for floating-point and complex spans only (indices,
  kinds and permutations stay byte-exact);
- batch_gradient's fold. Its guard words are NaNs too, so `same_fold` also refuses a guard word left in
  `grad[0, n)`. The rows and the guards stay byte-exact.

## Async file temp names under parallel ctest

Every crd-platform-tests process numbered its files from `test_async_0.bin` in the same directory, so one case
removed a file another case was reading. The name now carries the writing thread's id. The filesystem and
FileWatcher cases already did that (stamp plus thread id).

## Live dumps from a process snapshot

`live modules: every module matches a minidump of this process written by the OS` hit its 3,600 s timeout. It had
passed in 2.35 s in the run before. `write_dump` called `MiniDumpWriteDump` on the live process, which keeps every
other thread suspended through the write; its documentation names the loader deadlock as an in-process hazard. A
standalone probe (one thread looping on a DLL load and free, or on `HeapLock`/`HeapUnlock`, while the main thread
wrote 8 dumps) hung 6 of 6 times when it dumped the live process. Dumping from a `PssCaptureSnapshot` snapshot wrote
48 of 48. Even the probe's in-process watchdog hung, because the dump had suspended it too.

`write_dump`, shared by the fatal, hang and live paths, now writes from a PSS snapshot. The snapshot holds a
copy-on-write clone of the address space, the handles with names, types and traces, and every thread's full and
extended context; its bookkeeping takes VirtualAlloc memory. `IsProcessSnapshotCallback` answers `S_FALSE`. A
refused snapshot falls back to the live process. The design is in
[runtime-diagnostics.md](../design/runtime-diagnostics.md#diag-5a).

New crash-capture specimen modes: `live_churn` (four live dumps while a thread churns both locks) and `churn_av` (a
wild write under the same contender). Teeth: forcing the live path back made both new cases time out
(`verdict=Timeout`); with the restored code they pass. 202 dump, hang, crash and flight cases passed on win-debug.

## Checks

The touched test targets, crd-core-tests and crd-perf-tests built on win-debug. Their changed cases passed:
the arm cases (15 with the async-file cases), and the crash-capture, contract and module cases (39). Strict tidy is
clean on every changed source. The format findings on added lines are zero, apart from the crash-capture test's
aligned groups, which keep that file's own style. The ctest guards (19), Allman braces, the plan check and the
repository check passed.
