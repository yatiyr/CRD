# DIAG.10a compiled-versus-reference CEIR differential, 2026-10-10

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.10a](../ROADMAP.md#slice-diag.10a). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-10a); harness:
> [test instruments](../design/test-instruments.md). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.10a resource reload](2026-10-10-diag-10a-resource-reload.md).

## Goal

The sixth slice of DIAG.10a: "compiled/reference CEIR comparisons". The compiled execution tier (`plan::compile` and
`plan::run`) and the reference interpreter (`exec::Interpreter`) were compared only on hand-built programs in
`test_plan.cpp`. They now run every program a fuzz target generates, with oracles that are neither tier's own
verdict, and what the comparison found is fixed in both tiers. Committed locally, not pushed.

## What changed

- A fuzz target `crd-fuzz-ceir-differential` in `tests/execution/ceir/fuzz` (the replay executable and its `-corpus`
  CTest on every lane, the libFuzzer executable under `CRD_ENABLE_FUZZER`), registered in `scripts/fuzz.py`
  (`ceir-differential`), in the test-instruments table and in `test-repository-tools.py`'s target list.
- Every input decodes to a program, never to a parse failure, so mutations reach the executors: a header (one to four
  functions, each with 0 to 2 parameters and results; a step budget of 64 << (byte % 12); a host input script of up
  to 15 values or no input source at all; the entry arguments, small values in one byte and i64 edges or raw values
  otherwise) and each function's body as a stream of op choices: `arith` const, addi, muli and cmpi (all ten
  predicates); `core` if (0 to 2 results), for, while, switch, match and the three scope kinds (`core.scope`,
  `async.scope`, `task.group`); `core.state`, `delay` and `history` (depths 1 to 4, the feedback any later value of the
  same block); `func.call` to any function, recursion included; the launch kinds, await and fiber_wait (of a token
  or of a forged value), join, race, cancel and continuation (one in eight with the wrong argument count);
  `task.parallel_for` and `map_reduce` with isolated bodies; `input.random`, `clock`, `time_step` and `event`. The
  generator keeps a program inside the module-validity set a cook enforces (dominance and isolation, the core yield
  counts, each token consumed exactly once, the recursion policy); a program the verifiers refuse is not run. At most
  192 ops and four nested region levels per function.
- Oracles, none of them either tier's verdict:
  - the two tiers agree on the error class (the ExecError and RunError pairs `plan.hpp` names), the returned values,
    every cell (the plan's dense cells against the reference's `cell_value` of the same op by stable id), every map
    output on success and, on an error, the stable id of the op it blames;
  - a reference error with no compiled counterpart is a finding; the only accepted compile refusal is the documented
    tier difference, a parallel body that holds a cell or reads host input (refused whole at compile, by the
    reference when the op runs);
  - the plan run twice, a fresh reference session, the program printed and parsed into a new Context and the program
    serialized and loaded into a new Context (both compiled and run) give the same outcome;
  - a crash, a hang past the budget or an allocation past the harness budget is a finding.
- A failure prints the program's text with both outcomes, so a finding carries its own reproduction.
- `plan::run` takes the step budget (`max_steps`, default `plan::kDefaultMaxSteps`, the reference's default), so
  FuelExhausted compares at a small budget.
- 32 seeds, each checked as written to be a program both tiers run (`VerdictSink`), and each confirmed by hand to
  reach its intended outcome: arithmetic over i64 edges (signed and unsigned predicates disagreeing), a two-result if,
  a for loop feeding a depth-3 cell, for with a bad step, while (a cell counter in its condition region), switch and
  match, a selector out of range, the three scope kinds, delay and history cells across calls, a three-function call
  chain, the token program (launch, spawn, join, await of three values, worker, continuation, main thread, race,
  fiber wait: 13), await of a forged dead and a forged live handle, a continuation of the wrong arity, host inputs
  with a script, an exhausted script and no source, map_reduce in index order (33930), the stateful parallel body,
  and one per finding below. Three libFuzzer findings are adopted under their SHA-1 names (35 inputs in all).

## Findings

1. **The reference charged terminators a step.** `eval_block` spent a step on every op, the `core.yield` and
   `func.return` included; the compiled plan never dispatches a terminator. The tiers ran out of steps at different
   ops: inside a `core.while` the reference spent one more step per iteration (the condition's yield). The first
   random round found five such inputs. A step is now a dispatched op that is not a terminator, a loop iteration, a
   parallel-range index or a fold step, in both tiers (`exec.hpp` says so at FuelExhausted). The two existing
   crd-ceir-tests cases that use small budgets pass unchanged. Seed `while-cond-yield-fuel`; unit case "a step is a
   dispatched op or an iteration".
2. **Unbounded recursion overflowed the native stack** in both tiers (exit 0xC00000FD on win-debug): the step budget
   bounds work, not depth. Measured with the bound lifted: on win-debug recursion through an if survived 400 nesting
   levels and died by 500, through a `map_reduce` body 300 and 450; on win-asan (1 MiB main-thread stack, the
   tightest lane) 190 and 200, 180 and 195. `exec::kMaxNesting` = 64, a third of the worst, equal to the loaders'
   static region-nesting cap: an op with a region or a `func.call` counts one level while it runs, and an op that
   would nest past the bound fails `NestingTooDeep` before it runs, after its step is spent, blamed on that op in both
   tiers. A sub-interpreter running a parallel map or fold starts at its parent's level (`inherit_nesting`), so a body
   that calls back into the program cannot reset it. `NestingTooDeep` is appended to ExecError and RunError;
   `kLastExecError` and `kLastRunError` now bound the run-record decoder and its fuzz model, so no record layout
   changed. Seeds `recursion-unbounded`, `recursion-at-bound` (31 calls) and `recursion-past-bound` (32), and the
   same through a `map_reduce` body (21 and 22 calls); unit case "recursion stops at exec::kMaxNesting".
3. **The reference reserved a parallel range before running it.** `run_seq_map` pushed a slot per index first: a
   `parallel_for` over 2^31 indices reserved 16 GiB (the harness's budget allocator stopped it at 64 MiB). Nothing
   bounded such a range in either tier when its body dispatches nothing. Each index (and each fold step) is now a step
   of the body's budget in both tiers, and the reference's map grows per index. Seed `parallel-range-huge`; unit case
   "a parallel range is bounded by the step budget".
4. **A parallel range's count overflowed.** Both tiers computed `(hi - lo + step - 1) / step` in i64 (undefined past
   half the range) and cut it to 32 bits: `[INT64_MIN, INT64_MAX)` by 2^62 came out as an empty map in both, so the
   differential cannot see it; a unit case pins the four indices. `exec::parallel_range_count` and
   `parallel_range_index` use unsigned arithmetic. Seed `parallel-range-wide`.
5. **A `for` loop's increment overflowed.** `iv += step` past INT64_MAX is undefined; MSVC wraps it, so a loop from
   INT64_MAX - 1 to INT64_MAX by INT64_MAX ran on until the budget was spent, in both tiers alike. Both now stop at
   the last iteration within `step` of `hi`. Seed `for-step-near-max` (an UBSan finding on `linux-clang-fuzz` before
   the fix); unit case "a for loop whose step reaches past INT64_MAX runs once".
6. **The compiled map of a re-entered parallel op was mixed.** When a `map_reduce` (or `parallel_for`) body calls
   back into a function that runs the same op, the plan cleared and appended to the op's single map slot, so the
   inner activation's entries landed in the outer map (`[0, 1]` where the reference reports `[1]`). Each activation
   now builds its own map, taking the slot's storage, and stores it when it completes, as the reference does. Seed
   `map-reduce-reentered`; unit case "a map_reduce re-entered from its own body".
7. **The reference lost the map of a parallel op inside a parallel body** (libFuzzer, round 1, after six seconds). The
   outer body runs on a sub-interpreter, so the inner op's map was stored in the sub and discarded with it: the
   reference reported no map where the plan reported the last completed activation. The parent now adopts a sub's map
   outputs when the body's range or fold completes (`Interpreter::adopt_map_outputs`), which is the reference's own
   top-level rule (the last completed activation wins). Adopted input `525697be...`; unit case "a parallel op inside
   a parallel body keeps its map".
8. **The plan's token scope was quadratic** (libFuzzer, rounds 2 and 3, as timeouts). A parallel body gets a fresh
   token store; the plan built it by moving the whole run-wide store out and back for every activation, so a loop
   that launches a token and runs a parallel op each iteration cost O(n^2): 2,000 iterations took 0.58 s and 8,000
   took 7.7 s on win-debug. The body's store is now the tail of the run-wide one past `token_base`, dropped when the
   body ends: 0.04 s and 0.15 s. Adopted input `92c57885...` and seed `tokens-in-a-loop-with-a-parallel-op`.
9. **A parallel body was not charged to the run** (libFuzzer, round 4, a 31-byte timeout that ran for over 300 s
   under ASan and 48 s on linux-gcc-debug). Both tiers gave a parallel body a copy of the remaining budget and
   restored the parent's afterwards, so `f0(n)` mapping over `[-63, n)` and calling `f0` from the body did work that
   grows as the budget to the power of the nesting depth. The sequential tiers now spend one budget for the run: the
   reference charges the parent what a body's sub-interpreter spent (`Interpreter::charge_from`), the plan no longer
   restores its counter. The input now ends in FuelExhausted in 0.38 s on win-debug. Adopted input `adf630f4...`;
   unit case "a parallel body's steps are the run's". No test pinned the old rule. The parallel host provider keeps
   per-item budgets (recorded in the row).

## Random rounds and coverage (win-debug replay executable)

- Round 1, 300 random inputs before any fix: 280 clean, 14 stack overflows (finding 2), 5 fuel disagreements
  (finding 1), 1 allocation past the budget (finding 3).
- After the fixes, 2,500 random inputs of up to 600 bytes: all clean.
- A temporary instrumented build over 800 random inputs: none outside the validity set, 179 the documented compile
  refusal, 621 run by both tiers (231 ok, 131 bad-for-step, 84 bad-token, 65 selector-out-of-range, 52
  input-unavailable, 33 nesting-too-deep, 23 fuel-exhausted, 2 continuation-arity); 427 with cells, 228 with maps.
  `CondArity` is not reachable from a valid program (the yield count is verified).

## Teeth (win-debug, each observed failing, the exact file restored and the lane rebuilt)

| Break | Fails |
| --- | --- |
| The reference charges terminators again | corpus replay; unit case |
| No per-index step in the reference's parallel range | corpus replay (allocation past the budget) |
| No per-index step in the plan's parallel range | corpus replay (allocation past the budget) |
| No nesting bound in the plan | corpus replay (stack overflow); unit case |
| No nesting bound in the reference | corpus replay (stack overflow); unit case |
| A parallel sub-interpreter starting at level 0 | corpus replay (`map-reduce-recursion-past-bound`) |
| The plan's shared map slot | corpus replay; unit case |
| The `for` increment unguarded in both tiers | unit case only (both tiers wrap alike on MSVC) |
| The signed range count in both tiers | unit case only (both tiers agree on the empty map) |
| The plan's `ult` evaluated as `slt` | corpus replay (`arith-predicates-edges`) |
| The reference's `if` arm inverted | corpus replay; unit cases |
| The plan's fold step uncharged | corpus replay (`map-reduce-fold-fuel`) |
| The reference not adopting a sub's maps | corpus replay (adopted input); unit case |
| The plan's whole-store token scope | libFuzzer's 10 s limit on `linux-clang-fuzz` (the adopted input and the new seed) |
| The plan restoring the parent's budget after a parallel op | corpus replay (adopted input); unit case |

On `linux-clang-fuzz` (ASan and UBSan) the two overflow teeth are UBSan reports on the corpus replay: the unguarded
increment at `exec.cpp` (`9223372036854775806 + 9223372036854775807`) and the signed count in `exec.hpp`
(`9223372036854775807 - -9223372036854775808`).

The first teeth pass showed two seeds too weak (the predicate seed compared INT64_MIN against -1, where `ult` and
`slt` agree; the fold seed ran out at the same op either way); both were strengthened and the two teeth rerun. Every
restored file matched its pre-teeth copy byte for byte.

## libFuzzer on `linux-clang-fuzz` (single process, 600 s each, on working copies)

| Round | Source | Ended | Finding |
| --- | --- | --- | --- |
| 1 | findings 1 to 6 fixed | 6 s, 2,217 executions | finding 7 (a disagreement) |
| 2 | finding 7 fixed | 54 s, 6,109 executions | finding 8 (a timeout; its artifact was lost with the temporary directory) |
| 3 | the same, `--scratch` kept | 78 s, 1,631 executions | finding 8 again (`92c57885...`) |
| 4 | finding 8 fixed | 18 s, 104 executions | finding 9 (`adf630f4...`) |
| 5 | final source | 71 s, about 3,400 executions | an ASan SEGV with a zero frame pointer and no artifact |

Round 5 is not a finding of this target: the second fault is inside the unwinder walking a frame whose base pointer is
zero, no artifact was written, and the round's 588 working-corpus inputs replay clean. It is the DIAG.10a open item
"concurrent-load corruption" (two crd-perf targets under four concurrent processes), now seen single-process on a
target that does not link crd-perf, which points away from crd-perf and toward the machine or the toolchain. The kernel
log showed the WSL VM about three minutes old with a TSC adjustment of more than 11%. Not chased in this batch.

## Tool lesson

A scratchpad `cmd` or `bash` process older than 20 minutes is killed by the loop watchdog (the scheduled task
`CRD-Loop-Watchdog`); twice a whole-tree win-shipping build died silently mid-link, leaving orphaned `link.exe` processes
and half-written executables that broke CTest discovery (`LNK1104`, "Error running test executable"). Long Windows
steps are now split below 20 minutes or carry the `cmake --build` text the watchdog allows for up to 60 minutes, and
orphans are cleared before each build.

## Verification (final source)

- **win-debug:** the whole tree built. The full ctest inside vcvars passed 7,541 of 7,542 (one test skips itself, as
  before). The failure was `REN-40-G1 GATE (DX12)`, a segfault in the parallel run; alone it then passed 25 of 25, and
  no renderer or frame-cook code calls the CEIR plan or interpreter. The full run before findings 7 to 9 passed
  7,540 of 7,540. crd-ceir-tests 575 cases (8 `[diag10a]`), and the 8 `crd-no-*`/`crd-check*` guards.
- **win-shipping, win-clang-cl-shipping, win-asan (inside vcvars):** every touched target rebuilt on the final source
  (the win-shipping whole tree was built earlier in the batch). On each: crd-ceir-tests 575, crd-ceir-cook-tests 108,
  crd-ceir-host-tests 47, crd-ceir-gpu-tests 140, crd-ceir-gpu-vulkan-tests 60 and crd-ceir-gpu-dx12-tests 47 on this
  machine's GPU, crd-sandbox-inspect-tests 31, crd-ceridc-tests 21 with the rebuilt `ceridc`, the 21 fuzz-corpus and
  guard CTests, and the 32 seeds regenerated byte for byte. clang-cl linked first time; win-asan reported nothing.
- **WSL, one lane at a time:** linux-gcc-debug and linux-gcc-asan: the same suites (sandbox-inspect 30, no GPU
  suites but crd-ceir-gpu-tests), 13 fuzz-corpus and guard CTests, the corpus replayed three more times, seeds
  identical, no sanitizer report. linux-clang-tsan (`setarch -R`): crd-ceir-tests 575, crd-ceir-cook-tests 108, crd-ceir-host-tests 47,
  crd-ceridc-tests 21, crd-sandbox-inspect-tests 30, 12 corpus and guard CTests, the corpus replayed ten more times,
  seeds identical, no TSan report.
- **linux-clang-fuzz:** the corpus (35) replays clean under ASan and UBSan; libFuzzer as above.
- **Repository checks:** strict tidy clean on all eight changed C++ files; clang-format differs only in the repository's
  hand alignment, list packing, include grouping and one-line accessors; the Allman check, test-repository-tools (65,
  3 skipped), test-dev-workflow (54), check-master-plan and check-repository pass.

## Not done here

- The host provider (`crd-ceir-host`, the parallel tier) still computes its range count with signed overflow, cuts
  it to 32 bits and pre-sizes three per-index arrays from it, and its worker sub-interpreters start at nesting level
  0. It is outside this target (which compares the two sequential tiers); recorded in the row's Remaining list.
- `core.state` and its kinds accept any positive `depth`; both tiers allocate the ring when the cell is first read,
  so a program can ask for a ring of any size. The generator keeps depths at 1 to 4.
