# DIAG.9c seed, choice and input minimization, 2026-10-10

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.9c](../ROADMAP.md#slice-diag.9c). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-9c). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.9c reverse-debugging routes](2026-10-10-diag-9c-reverse-debug-routes.md).

## Goal

The contract's "seed/choice/input minimization around the Cerid reproduction harness", as the row planned it: the
DIAG.4b greedy remover promoted to a reusable helper and applied to fault-plan rules and hit ranges and to recorded
inputs, with an all-K-runs oracle. The harness is what the engine already reproduces: schedule scripts (DIAG.4b),
fault plans (DIAG.9b) and run records (DIAG.9a). Observer-effect experiments are the next slice. Committed locally,
not pushed.

## What changed

- `engine/foundation/core/include/crd/core/minimize.hpp` (header-only). It sits in crd-core because the schedule
  tests link the gate-ON twin `crd-jobs-schedcheck`, and crd-perf links plain crd-jobs: a helper in crd-perf would
  have put both job libraries in one executable. It holds `judge` (the all-K oracle), `minimize_sequence` (the DIAG.4b
  remover, with an optional admission predicate), `bisect_toward` and `minimize_toward_zero`, with `Budget`, `Stats`
  (`candidates`, `runs`, `reproduced`, `flaky`, `skipped`) and `Status` (`minimal`, `not-reproduced`, `budget`).
  The DIAG.4b header's argument for greedy removal over ddmin moved into it.
- `tests/foundation/jobs/sched_minimize.hpp` is gone; `test_sched_check.cpp` minimizes through the shared helper with
  K = 3 and a 512-run budget, and also checks `Status::Minimal` and that no candidate was a race.
- `engine/foundation/perf/include/crd/perf/fault_minimize.hpp` and `src/fault_minimize.cpp`: `minimize_fault_plan`,
  taking the caller's run as `FaultRunOnce` (arm, run, report each site's hits). Steps: judge the input; narrow every
  rule's range in rule order (bound an unbounded rule at the last reproducing run's hit count, then bisect its first
  hit up and its end down); pin the plan to the hits it fires on (at most 64, in at most 8 rules, seed 0, every rule
  always firing) when that reproduces; remove single hits of a pinned plan (a candidate needing more than 8 rules is
  skipped) or else whole rules.
- `engine/execution/ceir-cook/include/crd/ceir/cook/replay_minimize.hpp` and `src/replay_minimize.cpp`:
  `minimize_record_inputs` with `RecordMinimizeStatus` (`minimal`, `budget`, `not-reproduced`, `not-a-failure`,
  `unsupported`, `refused`, `unrunnable`, `cancelled`). It refuses before anything runs where `replay.run` refuses,
  including `check_external_replay`, then judges the record's own inputs, cuts the read stream to the reads the
  failing run made, shrinks arguments then delivered read values toward 0, and records the final run as a new record
  made by this build.

## Findings on the way

- Re-shrinking a value whose failure is not monotone drifts. The first version of the record minimizer repeated its
  passes while anything moved; a raw random draw that only has to reduce to 3 modulo 4 kept finding a smaller local
  boundary on every pass, and the seeded case ran out of its 4,096-run budget. A value is now shrunk again only when
  another value moved after it was last shrunk (a per-slot stamp against a move counter), which settles in a few
  passes and keeps the claim exact: every value has 0 not reproducing and its neighbour one step closer to 0 judged
  not to, given the final values of the others.
- Dropping a rule of a seeded plan changes what the later rules fire: `fault_plan_fires` mixes the rule index into
  the seed. Range narrowing therefore runs before any removal (it keeps every index), and removal runs on the pinned
  plan, whose rules always fire, so no removal shifts another fault.
- The real cook reproduction: with seed 1 a 40% failed-read plan fails all three reads the edited cook makes (0, 1
  and 2); the cook recovers from the first two and fails at the third, publishing nothing. The minimized plan is
  `seed=0;failed-read@2+1%1000000`: the `.meta` that holds the asset's id ("cannot read the .meta ...; its id is kept
  and nothing is published"), and the edited cook with no fault succeeds.

## Evidence

The four touched suites run whole on every lane, each Windows lane reconfigured (`configure-preset.bat`) and win-asan
run inside vcvars; WSL lanes one at a time, each reconfigured, TSan under `setarch -R`:

| Lane | crd-core-tests | crd-perf-tests | crd-ceir-cook-tests | crd-resources-tests | `[diag9c]` repeats |
| --- | --- | --- | --- | --- | --- |
| win-debug (whole tree built) | 35 cases | 260 | 108 | 127 | - |
| win-shipping | 27 | 127 | 108 | 127 | - |
| win-clang-cl-shipping | 27 | 127 | 108 | 127 | - |
| win-asan | 35 | 260 | 108 | 127 | - |
| linux-gcc-debug | 27 | 258 | 108 | 127 | 3 x 4 suites |
| linux-gcc-asan | 27 | 258 | 108 | 127 | 3 x 4 suites |
| linux-clang-tsan | 27 | 258 | 108 | 127 | 10 x 4 suites |

`[diag9c]`: core 5 cases (59 assertions), perf 5 (54; 50 on the shipping lanes, where the real-injector case answers
`not-reproduced` with the plan unchanged), ceir-cook 3 (193), resources 1 (234; 12 on the shipping lanes, the
refusal branch). `crd-jobs-schedcheck-tests` (built explicitly: it is `EXCLUDE_FROM_ALL` and in no hosted job) passes
6 cases, 47 assertions, on win-debug and on all three WSL lanes. The only sanitizer reports are the deliberate
specimens' (crash capture, heap overflow, use after free, use after return); none in the `[diag9c]` runs, and no TSan
report at all.

One full win-debug run of crd-perf-tests, the first after the whole-tree build, reported one failed assertion; its
output was cut by the run's own filter, so the case is not known. Twelve later full runs of the same executable all
passed, as did every later run on every lane, and the new cases are pure (no timing, threads or files). It is
recorded here rather than dismissed.

Repository checks: strict tidy is clean on all 10 changed C++ files (three findings fixed: a local constant's case,
a loop that is `std::ranges::any_of`, a conditional with identical branches); clang-format differs only in the
repository's hand alignment, case-label layout and include grouping; the Allman check, the 8 ctest guards,
check-master-plan and check-repository pass.

Teeth, each observed failing on win-debug, then restored and rebuilt:
- any-of-K instead of all-K: the race case keeps a candidate without 7 (core) and the racy plan is minimized (perf);
- no precondition in `minimize_sequence`: a non-reproducing input is "minimized" (core);
- the pass not restarted after a drop: core results are not 1-minimal, and the schedule script is not shrunk to its
  two tags (`crd-jobs-schedcheck-tests`);
- bisection stopping one step early: the toward-zero results are off and their neighbours still reproduce (core);
- the upper end of a range never narrowed: the seeded four-rule plan is not pinned and keeps its seed (perf);
- a pinned plan keeping a seed: three exact plan texts fail (perf);
- the failure compared without its op: the two-switch argument shrinks to 2 at the other switch (ceir-cook);
- the external-I/O refusal skipped: the file-reading program is not refused for that reason (ceir-cook).
After restoring, the whole win-debug tree was rebuilt, `crd-jobs-schedcheck-tests` was rebuilt on its own (the
whole-tree build does not build it, so the first rerun still held the pass-restart tooth), and every suite passed.

## Remaining in DIAG.9c

- Observer-effect experiments on that harness: the same failing seed or plan with the flight recorder off, basic and
  full, keeping the natural failure beside the instrumented capture. The flight policy has no detail levels today, so
  the slice first defines what basic and full record.
- TTD (user): one elevated recording on this machine, as listed in the previous note.
- rr: no eligible host here; an explicit capability limit until a Linux host with hardware counters exists.
- The hosted run, as the row lists it.
