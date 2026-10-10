# DIAG.9c reverse-debugging and watchpoint routes, 2026-10-10

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.9c](../ROADMAP.md#slice-diag.9c). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-9c). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.9b sandbox flight host](2026-10-10-diag-9b-sandbox-flight-host.md).

## Goal

The first slice of DIAG.9c: verified rr/TTD-class recipes with tool, version and permission checks, symbols,
original exit codes, storage bounds and offline replay; a small intentional corruption found by replay or backward
inspection on each claimed route; a missing tool or an incomplete recording reported as an instrument failure; the
natural uninstrumented failure kept beside each instrumented run; hardware watchpoints as a documented bounded option.
Seed, choice and input minimization and the wider observer-effect experiments are later slices. Committed locally,
not pushed.

## What the hosts can run (observed before coding)

| Tool | Host | State |
| --- | --- | --- |
| gdb 15.1-1ubuntu1~24.04.1 (with Python) | WSL Ubuntu 24.04 | runs; Yama `ptrace_scope` 1 |
| rr 5.7.0 | WSL | installed, cannot record: `perf_event_paranoid` is 2, and with `-n` rr reports "Unable to open performance counter with 'perf_event_open'; are hardware perf events available?"; `/sys/bus/event_source/devices` has no `cpu` entry (WSL2 exposes no PMU) |
| cdb 10.0.29547.1002 | Windows (WinDbg package 1.2603.20001.0) | runs unelevated from the package directory; not on PATH (only the `WinDbgX` alias is) |
| TTD.exe | Windows (same package, `amd64\ttd`) | launch refused unelevated (WinError 5); the shell is not elevated |

Two earlier records were stale: `scripts/debugger-recipes.py` and this box's tool notes said cdb was absent, because
they looked only on PATH.

## What changed

- `tests/support/diag/specimens/corruption_specimen.cpp`, target `crd-diag-corruption-specimen`. It is in the default
  build through `crd-diag-reverse-debug-specimens`, so every lane compiles and links it, and `-g` is requested on
  gcc and clang. `corrupt` writes one word past four cells into a sentinel through an off-by-one loop; `clean` runs the
  same code with the right bound. Exit 3 on detection, 0 clean, 64 on bad arguments. The writer and the store are
  marked in the source, and the markers are what the runbook checks a stop against.
- `scripts/reverse-debug-recipes.py`: `--probe` and the routes `gdb-record`, `gdb-watch`, `cdb-watch`, `rr`, `ttd`,
  with the verdicts and the verdict record described in the contract's Settled block. The gdb legs run as Python
  drivers inside gdb that print one JSON line; stops are classified from gdb's stop events, because stop notices are
  printed rather than returned by `gdb.execute`. The cdb legs give the data breakpoint and the window-end breakpoint
  fixed IDs, so each stop says which breakpoint it was.
- `scripts/test-reverse-debug-recipes.py`: 34 tests over every verdict without a debugger, including a cdb transcript
  as cdb printed it. It is the `crd-diag-reverse-debug-recipes` CTest and the "Reverse debugging verdicts" repository
  CI step on both runners.
- Linux CTests: `crd-diag-reverse-debug-gdb-watch` and `-gdb-watch-clean` on every Linux lane, and
  `-gdb-record`, `-gdb-record-clean` and `-gdb-record-incomplete` on Linux lanes without a sanitizer. A host
  without gdb fails them. The incomplete control must fail for its own reason: `--expect-reason "fell out of the
  bounded log"`.
- CI installs `gdb` on both Linux jobs and records its version with the other packages; `cmake/pins.json` lists it
  under `unpinned.apt`, and the license manifest and its generated-sources record were refreshed.
- `scripts/debugger-recipes.py`'s availability probe now finds cdb in the WinDbg package (through the new script's
  finder) and WinDbg through its `WinDbgX` alias. Its 6 tests are unchanged and pass.

## Findings on the way

- `record full save` is not a gdb command; the save is `record save`.
- A restored gdb log reports one instruction past its last navigable one as its highest; `record goto` that number
  answers "Target insn not found". The replay navigates `lowest + count - 1`.
- gdb 15.1 asserts (`./gdb/inferior.c:405: internal-error: find_inferior_pid: Assertion 'pid != 0' failed`) on
  `continue`, `reverse-continue` or with a watchpoint over a restored log, so the offline leg bisects instruction
  numbers with `record goto` and reads the sentinel; that alone names the writer.
- Under ASan and TSan, process record works (134,375 instructions in 5.8 s under ASan), but `record save` did not
  finish in 300 s: it writes the process image, which includes the sanitizer runtime's reserved address space. The
  first run of the incomplete control "passed" on those lanes only because a timeout is also incomplete, which is why
  the control now asserts its reason. The record route is declared unavailable on sanitizer builds at configure time.
- cdb on win-asan first stopped inside the ASan runtime: MSVC's ASan commits shadow memory through first-chance
  access violations, which cdb breaks on by default. The route sets `sxd av`; an unhandled violation still stops at
  its second chance.
- The cdb transcript echoes the initial command line, which names the stop markers; markers are matched as whole lines.
- A hardware breakpoint set at the initial process breakpoint is lost when the loader resets the thread context, so
  `cdb-watch` runs to `@$exentry` first.

## Evidence

Linux, WSL, one lane at a time, each reconfigured; the gdb CTests and `crd-diag-debugger-recipes` run three times:

| Lane | Tests | Record (instructions, trace bytes) | Writer line, record / watch |
| --- | --- | --- | --- |
| linux-gcc-debug | 7/7 x3 | 34,123, 2,066,232 | 78 / 76 |
| linux-gcc-shipping | 7/7 x3 | 14,538, 1,326,824 | 78 / 80 |
| linux-gcc-asan | 4/4 x3 | not a route (configure message) | - / found |
| linux-clang-tsan | 4/4 x3 | not a route (configure message) | - / found |

Line 78 is the marked store; the forward watchpoints stop after it. The clean controls report CLEAN live and offline;
the incomplete control (2,000-instruction log) reports "the write fell out of the bounded log (lowest instruction
32124)" on debug and 12539 on shipping.

Windows, `cdb-watch` corrupt and clean on each lane (win-asan inside vcvars): win-debug FOUND at line 79,
win-shipping 78, win-clang-cl-shipping 80, win-asan 79; CLEAN on all four. `ttd` reports `PERMISSION_DENIED`
("an elevated shell is required"); `rr` on WSL reports `HOST_INELIGIBLE` (no cpu PMU). `--probe` on each host lists
exactly these states.

Teeth, each observed failing, then restored and rebuilt:
- A: the off-by-one repaired (`i < last_index`). Every corrupt-mode route reports `SPECIMEN_INVALID` ("the
  uninstrumented corrupt run exited 0"): the three Linux CTests and `cdb-watch` on win-debug. The clean controls
  stay green.
- B: the gdb driver takes any reverse stop as the writer. `-gdb-record-clean` fails with FALSE_FIND (the stop at the
  window's start), and the incomplete control fails with NOT_FOUND.
- C: a truncated log no longer distinguished from a missing write. The mocked truncation case fails.
- D: every cdb stop but the window end counted as a writer. The mocked clean case fails, and the live clean run on
  win-debug reports FALSE_FIND at `NtTerminateProcess`.
After restoring, the specimen was rebuilt on win-debug and linux-gcc-debug and both suites passed again.

Repository checks: strict tidy clean on the specimen; clang-format clean after two hand-alignment fixes; the Allman
check, the ctest guards, check-pins, check-ci-tiers, check-generated, gen_license_manifest --check, the pinned
actionlint, test-repository-tools, check-master-plan and check-repository pass (see the commit).

## Remaining in DIAG.9c

- Seed, choice and input minimization around the reproduction harness: promote the DIAG.4b greedy 1-minimal
  remover (`tests/foundation/jobs/sched_minimize.hpp`) to a reusable helper and apply it to fault-plan rules and hit
  ranges and to recorded inputs, with an all-K-runs oracle.
- Observer-effect experiments on that harness: the same failing seed or plan with the flight recorder off, basic and
  full, keeping the natural failure beside the instrumented capture.
- TTD (user): one elevated recording on this machine (`python scripts/reverse-debug-recipes.py --route ttd --mode
  corrupt ...` from an elevated shell, and again with `--mode clean`); the replay leg needs no elevation.
- rr: no eligible host here. Qualification needs a Linux host with hardware performance counters where the user
  chooses `perf_event_paranoid <= 1`; until then the route is an explicit capability limit.
- The hosted run.
