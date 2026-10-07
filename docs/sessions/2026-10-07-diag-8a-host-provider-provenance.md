# DIAG.8a host-provider provenance, 2026-10-07

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.8a](../ROADMAP.md#slice-diag.8a). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-8a). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [CHIR lowering](2026-10-07-diag-8a-chir-lowering-provenance.md).

## Goal

Make a runtime error from the crd-jobs host provider name the authored op that raised it, as the compiled plan already
does, and prove it after CSE, serialization and a load into a fresh Context.

## What was there (checked before coding)

- `HostProvider::execute` returns `exec::ExecResult`, whose `op` already resolves through `resolve_provenance`. The
  pre-flight refusals (`ParallelBodyStateful`, `ParallelYieldArity`, `UnresolvedCall`) already named the precise op.
- Every body that runs on a separate sub-interpreter lost its error op. The sub recorded it (`Interpreter::fail`,
  first wins), but nothing could read it, and the caller re-blamed its own op:
  - provider: each `parallel_for`/`map_reduce` map index (`errs[idx]`, then `in.fail(errs[idx], &op)`), the
    `map_reduce` fold step, and a pooled launch body (the token kept only the error kind, so the `await` was blamed);
  - sequential reference: `run_seq_map` and the `map_reduce` fold, the same way.
- The compiled plan runs those bodies in its own frames and latches the innermost instr (`RunResult::fault`), so the
  plan named the inner op while the reference and the provider named the owner. The existing provider test pinned the
  old behaviour (`CHECK(r.op == pf)` for a fuel-exhausted body).
- The sequential in-frame launch (the reference's `async.launch`, the provider's non-pooled fallback) already named
  the inner op, because the body runs in the caller's interpreter.

## What landed

- **`Interpreter::failed_op()`** (`crd/ceir/exec.hpp`): an additive reader for the op the current run's error was
  recorded on. The op lives in the shared Module, so the pointer outlives the sub-interpreter that recorded it.
- **Sequential reference** (`exec.cpp`): the parallel map and the `map_reduce` fold blame `sub.failed_op()`, falling
  back to the owner only when the body named no op (`invoke_region`'s `NoEntry`/`BadArity`).
- **Provider** (`host_provider.cpp`, `host_provider.hpp`):
  - each map index stores the sub's op in its own pre-sized `err_ops[idx]` slot next to `errs[idx]` (disjoint slots,
    read after the counter wait, no atomics); the first failing index in index order is blamed on its own op;
  - the fold blames `fold.failed_op()`;
  - a pooled token keeps `err_op` beside `err`; `resolve_pooled` gains an `out_op` parameter, and `await` and `join`
    blame the body op, falling back to themselves for a forged or bad token.
- The existing fuel test now asserts the body's `core.for` (the op that ran out of fuel) and that it is not the
  `parallel_for`. This tightens the check.

## Test

`tests/execution/ceir-host/test_host_provenance.cpp` (new, 4 cases, `[ceir][host][diag]`). A module with four entries
is printed, parsed under a registered file, optimized with CSE through the PassManager, serialized and loaded into a
fresh Context. Expected positions come from scanning the printed text for the n-th `core.for` and `task.parallel_for`.
Each case compares the provider, the sequential reference and the compiled plan:
- **Parallel body:** `parallel_for(0, 8)` whose body has loop A (faults for iv >= 4) before loop B (faults for
  iv <= 1). Index 0 is the first offender and faults at B, although A comes first in program order and indices 4 to 7
  fault at A concurrently. The reference, the plan and the provider with job splits 1, 2, 3, 4 and 8 all name B's line
  and column in the authored file, and the provider's op is the reference's op.
- **Pooled launch:** the launch body's loop faults; `pooled_count() == 1` shows the body ran on a worker, and the error
  reported at the `await` names the body loop, as the reference and the plan do.
- **Fold step:** the `map_reduce` combine's loop faults; all three name it.
- **Owner control:** a `parallel_for` with a zero step of its own is still blamed on the `parallel_for` line.

Teeth (win-debug; each restored, the lane rebuilt and the suite rerun green three times, 28 cases):
- provider map index blamed on the owner again: the parallel case fails (15 assertions, every job split);
- pooled `await` blamed on itself again: the launch case fails (3 assertions);
- provider fold blamed on `map_reduce` again: the fold case fails (3 assertions);
- reference parallel map blamed on the owner again: the parallel case fails (7 assertions, the reference line plus the
  provider's op equality on every split).

linux-gcc-asan caught a bug in the first version of the test: a helper returned the plan's `instr_provenance` view
after the `CompileResult` that owns it was destroyed (use-after-poison in `Provenance::primary`, found by the DIAG.3b
allocator poisoning). The helper now checks the view while the plan is alive. No engine code was involved.

## Evidence

- **win-debug:** the whole tree builds (`exec.hpp` is included by every CEIR consumer). `crd-ceir-host-tests` passes
  28 cases (898 assertions) three times in a row; `crd-ceir-tests` 540 cases (12,649), `crd-chir-tests` 26 (5,069) and
  `crd-ceir-cook-tests` 40 (1,864) pass.
- **win-shipping, win-clang-cl-shipping, win-asan:** `crd-ceir-host-tests` (28 cases, 898 assertions),
  `crd-ceir-tests [diag]` (6 cases, 131) and `crd-chir-tests` (26 cases, 5,069) build and pass on each, on the final
  sources; win-debug was rebuilt and rerun with them too. No ASan report; the clang-cl link was clean first time.
- **WSL:** linux-gcc-debug, linux-gcc-asan and linux-clang-tsan each build and pass `crd-ceir-host-tests` (28 cases,
  898 assertions; rerun after the test fix), `crd-ceir-tests [diag]` (6 cases) and `crd-chir-tests` (26 cases) with no
  sanitizer report.
- **Checks:** strict tidy is clean on the four changed C++ sources. `clang-format --dry-run` on the new test reports
  only the repository's hand-aligned declarations; its four line-wrap findings were taken by hand (the test was then
  rebuilt and its 4 cases rerun on win-debug). The Allman check, the 8 ctest guards, check-master-plan and
  check-repository pass.

## Not covered here

- A pooled launch whose body fails and is never awaited reports nothing (the token is drained at exit), while the
  sequential reference fails at the launch. This is a pre-existing difference in when the error surfaces, not in which
  op is named; it is noted for the CEIR async owner and not changed here.

## Remaining on DIAG.8a

- CKIR node identity and a GPU validation error navigate to the CEIR dispatch origin.
- An error at an intrinsic (`[op.native]`) op names its authored op and its native provider.
- An entry-point refusal (`NoEntry`, today an op-less `NoOperation` gap) names the requested entry.
- A fault in an installed generation names that generation.
- A failed reload of a CHIR-lowered blob keeps its ChirNode origins. It uses the same `ORIG` path as the text-cook
  reload proof but has no test of its own yet.
