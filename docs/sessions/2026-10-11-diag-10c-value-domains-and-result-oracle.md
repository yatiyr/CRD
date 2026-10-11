# DIAG.10c value-domain probes and the generation-attributed result oracle, 2026-10-11

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.10c](../ROADMAP.md#slice-diag.10c). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-10c). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.10c numerical mode](2026-10-11-diag-10c-numerical-mode.md).

## Goal

DIAG.10c's acceptance asks that "a wrong-but-noncrashing result is caught by an independent invariant/oracle and
attributed to a program generation" and that "valid special values allowed by a domain are not rejected
indiscriminately". This batch lands the two clauses that carry those sentences: value-domain probes with per-domain
allowances, and the result oracle at the CEIR boundary, attributed through the reload set. Committed locally, not
pushed.

## Value-domain probes

`engine/foundation/core/include/crd/core/value_domain.hpp` (crd-core, a leaf every module links):

| API | What it does |
| --- | --- |
| `classify_value(f32 / f64)` | Zero, NegativeZero, Subnormal, Normal, Infinity, NegativeInfinity or NaN, from the value's own bits |
| `FloatDomain` | which special classes are allowed (NaN, +Inf, -Inf, subnormal, -0) and a closed range; the default is "finite": no NaN or infinity, subnormals and -0 allowed, no bound |
| `any_float_domain`, `finite_float_domain`, `distance_domain`, `unit_interval_domain` | NaN as a missing sample; finite; [0, +Inf] with +Inf for "never hit"; [0, 1] |
| `IntDomain` | a closed i64 range |
| `domain_valid` | false for a NaN bound or minimum > maximum |
| `value_violations` | the `ValueViolation` bits of one value (NaN, Infinity, NegativeInfinity, Subnormal, NegativeZero, BelowMinimum, AboveMaximum) |
| `probe_values` | over a run: values checked and violating, the union of violations, and the first offender's index, bits and violations; nothing is checked against an invalid domain, and the report says so |

Decisions:
- The range is numeric. -0 equals 0; an infinite bound admits the infinity at that end when its class is allowed; a
  NaN has no place on the line, so only its class is judged. A value can carry several violations (a disallowed -Inf
  below a finite minimum is NegativeInfinity and BelowMinimum).
- The probe reads bit patterns and does no floating-point arithmetic or comparison, so the caller's mode cannot
  change a verdict. Under denormals-are-zero, SSE comparisons treat subnormal operands as zero: `x == 0` is true for a
  subnormal and a negative subnormal is not `< 0`. The "every verdict under FTZ, DAZ and round-down" case is the check.
- A binary32 value is classified from its own bits (its subnormals are normal binary64 values) and widened to
  binary64 exactly, normalizing a subnormal's fraction, before the range compare.

## The result oracle at the CEIR boundary

`engine/execution/ceir-cook/include/crd/ceir/cook/result_oracle.hpp` (crd-ceir-cook):
- `check_results(values, expectation)` judges a successful run's results against an `IntDomain` invariant and,
  when given, reference values computed another way. The verdict is the first offending result: CountMismatch,
  OutOfDomain (the domain is checked first at each index), DiffersFromReference; or Agrees, or InvalidDomain when
  the expectation cannot judge.
- `ReloadSet::locate_result(h, entry, index)` (new, `hot_reload.hpp`) attributes result `index` of `entry` to the
  generation `h` names, current or retiring, never to whatever is installed now; a drained generation is Gone with
  only its number. `where` is the op that defined that operand of the entry's closing `func.return` in that
  generation's own Context (`file:line:col op#id`), the return op when the result is a parameter or past its
  operands, and the NoOperation gap for an entry the generation does not define.
- `check_program_results` combines them; `render_result_finding` gives one line for a human or agent, for example
  `differs-from-reference result#1 = 3, expected 9 at asset#5200 gen#1 retiring <file>:<line>:<col> op#<id>`.
- The named op produced the value; it need not be the op an edit changed (an invariant on a wrapped square names the
  `arith.muli`, a wrong constant names the `arith.addi` that consumed it).
- A run that failed is not judged: its error already blames an op.
- The CEIR executors are integer-only (wrapping i64), so the oracle uses `IntDomain`, and a CEIR plan run has no
  floating-point mode to probe: that "other boundary" of the numerical-mode clause is settled by this argument. The
  float domains wait for the tensor and audio boundaries.

## Tests

crd-core-tests, `[diag10c]` (`tests/foundation/core/test_diag_value_domain.cpp`); every expected value is written
from IEEE-754 bit patterns:

| Case | Asserts |
| --- | --- |
| values are classified from their bits in both widths | all seven classes in binary32 and binary64, signalling and payload NaNs, the binary32 max subnormal is a normal binary64; distinct names |
| a domain rejects exactly the special values it does not allow | `any` reports no special value; `finite` names NaN and each infinity and keeps subnormals and -0; `distance` admits +Inf and 0 and -0 but names NaN, -Inf (with BelowMinimum) and -1; a strict domain names subnormals and -0 only; distinct names |
| the range is numeric and a binary32 value is compared exactly | `unit-interval` at 0, -0, 1, the next float after 1, a negative subnormal, +Inf, NaN; bounds at 2^-149, 2^-148 and the binary32 max subnormal written as binary64; invalid domains (NaN bound, inverted) check nothing |
| an integer domain bounds a count and reports the first value outside it | bounds inclusive, the first offender's index, bits and violation, the union, an inverted domain |
| a probe over a run names its first offender in both widths | first index, its binary32 bits zero-extended, the union; `any` is clean |
| no verdict depends on the caller's numerical mode | the same reports under FTZ, DAZ and round-down as under the declared mode, a negative subnormal still below 0, subnormals still Subnormal |

crd-ceir-cook-tests, `[diag10c]` (`tests/execution/ceir-cook/test_result_oracle.cpp`). The program is
`@main(x) -> (x + 1, x * x)` authored as text under a named file; the reference is the same formula in C++, with the
executors' wrapping. Each wrong run succeeds (`ExecError::None` / `RunError::None`):

| Case | Asserts |
| --- | --- |
| a hot-swapped generation's wrong result is caught by a reference and blamed on it | generation 1 agrees on five inputs and blames nothing; a constant edit hot-swaps to generation 2, which returns x + 2: DiffersFromReference at result 0, gen#2 current, the `arith.addi` line of file B; the plan built from generation 1 still agrees |
| a plan built from a wrong generation is blamed on that generation after the fix | generation 1 returns x * 1; after the fix installs, generation 2 agrees everywhere; the old plan's result 1 is blamed on gen#1 retiring at the `arith.muli` line of file A, never file B; x = 1, where the wrong program happens to agree, blames nothing; after a drain the finding is gen#1 gone with no position |
| an invariant catches a wrapped result without any reference | x * x >= 0 for x >= 0: 3037000499 agrees, 3037000500 wraps negative and is OutOfDomain BelowMinimum at the `arith.muli`, on the interpreter and the compiled plan; the domain wins over the reference at one index; AboveMaximum |
| count mismatches, invalid domains and results no op defines | three results against two reference values and two against three (blamed at the `func.return`), an index past the operands, an invalid domain blames nothing, an undefined entry names the generation and no file, a foreign handle is Gone, distinct verdict names |

## Teeth

Each break on win-debug (`[diag10c]` of the touched program), restored byte for byte by rewriting the file, then both
programs rebuilt and green again (core 10 cases, 268 assertions; cook 4 cases, 165 assertions):

| Break | Result |
| --- | --- |
| the range compares the binary64 value with a float `<` | the mode case failed (a negative binary64 subnormal not below 0 under DAZ) |
| the binary64 classifier tests a subnormal with `x != 0.0` | the mode case failed (a subnormal classified Zero under DAZ) |
| the binary32 widening skips normalizing a subnormal | the exact-bounds assertions at 2^-149 and the max subnormal failed |
| -0 keeps its sign in the range key | 3 cases failed (-0 below 0 in `distance` and `unit-interval`) |
| the +Inf allowance is ignored | 2 cases failed (6 assertions) |
| the first offender is overwritten by later ones | 2 cases failed (6 assertions) |
| `locate_result` reads the current generation, not the handle's | the retiring case failed |
| `locate_result` ignores the defining op | all 4 oracle cases failed |
| the oracle ignores the reference | 2 cases failed (22 assertions) |
| the oracle ignores the domain | the invariant case failed (12 assertions) |
| the oracle misses too few results (`>` for `!=`) | the count case failed (3 assertions) |
| the oracle ignores the count altogether | the reference is read past its end; the process did not exit and was killed at the runner's 120 s bound (a blocked debug check is the likely cause, not observed); recorded as a bite, not a clean failure |

## Verification

Whole programs on the final source (the test file and a header comment were reworded after the teeth; every lane
below was rebuilt from that source):

| Lane | crd-core-tests | crd-ceir-cook-tests |
| --- | --- | --- |
| win-debug (whole tree built, 212 steps) | 45 cases (10 `[diag10c]`) | 114 cases (4 `[diag10c]`) |
| win-shipping | 37 cases | 114 cases |
| win-clang-cl-shipping (linked first time) | 37 cases | 114 cases |
| win-asan (inside vcvars) | 45 cases | 114 cases |
| linux-gcc-debug | 37 cases | 114 cases |
| linux-gcc-asan | 37 cases; `[diag10c]` repeated 3 times | 114 cases; `[diag10c]` repeated 3 times |
| linux-clang-tsan (`setarch -R`) | 37 cases; `[diag10c]` repeated 10 times | 114 cases; `[diag10c]` repeated 10 times |

The only sanitizer reports are the crash-capture specimen's deliberate SEGVs (9 on win-asan, 12 on linux-gcc-asan,
all in the full core run, none in a `[diag10c]` run); no TSan report.

Coverage (`linux-clang-coverage`, WSL, clang 18.1.3): the scope registry gains the group `value-domain probes and
the result oracle` (`value_domain.cpp`, its header and `result_oracle.cpp`) and four outcome sets: FpDifference
(left unregistered by the numerical-mode batch), ValueClass, ValueViolation and ResultVerdict, whose Agrees is a
`default-value` exclusion (ResultCheck's initializer). The measured set was rebuilt and measured once with every test
passing: 68 scope files all instrumented, verdict PASS, each new set fully produced (9 of 9, 7 of 7, 7 of 7, 4 of 5
with the exclusion). The new group ran 235 of 239 regions and 188 of 194 branches. Totals: regions 9,535/10,680,
branches 6,234/7,908, functions 1,075/1,124, lines 17,758/20,298. `hot_reload.cpp`, which holds `locate_result`, is
not in the scope (it is the reload supervisor, not diagnostic code); the oracle cases run it. The profiles (1.5 GB)
are deleted.

Repository checks: strict tidy (LLVM 20.1.8) clean on the five changed C++ sources; clang-format differs only in the
repository's hand alignment, one-line case labels and include grouping (two comment lines and one test line over 120
columns were rewrapped); the Allman check, the eight `crd-no-*`/`crd-check*` CTest guards, `crd-diag-coverage`,
`crd-diag-seeded-faults`, `crd-build-profiles`, `crd-repository-tools`, `diag-coverage.py --validate`,
`test-diag-coverage.py` (29), check-master-plan and check-repository pass. No script or workflow changed. The full
win-debug CTest was not run; the two touched programs ran whole on every lane. git status shows only this batch's
files and nothing is left running.

## Remaining in DIAG.10c

Shape/stride and units/time-domain probes and the float domains at the tensor and audio boundaries; the mode probe at
the audio callback and third-party calls; lifetime accounting beyond heap bytes; queue/VRAM pressure and budgets;
warm/cold, soak, reload and shutdown workloads with seeded injection; slowdown measurement; device-reset and
host-pressure evidence; and the hosted run.
