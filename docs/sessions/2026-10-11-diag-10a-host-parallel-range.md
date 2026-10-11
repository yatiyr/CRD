# DIAG.10a host provider parallel range, 2026-10-11

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.10a](../ROADMAP.md#slice-diag.10a). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-10a); harness:
> [test instruments](../design/test-instruments.md). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.10a compiled-versus-reference CEIR differential](2026-10-10-diag-10a-ceir-differential.md).

## Goal

The differential slice bounded the two sequential CEIR tiers (the reference interpreter and the compiled plan) and left
the parallel tier, the crd-jobs host provider in `crd-ceir-host`, on the old rules: a signed range count cut to 32
bits, three per-index arrays sized from that count, sub-interpreters starting at nesting level 0 and per-item budgets
nobody had stated. This slice brings the provider to the same answers, decides its budget rule and tests it against the
reference on the same modules. Committed locally, not pushed.

## What changed

- `engine/execution/ceir-host/src/host_provider.cpp`:
  - `run_parallel_map` counts with `exec::parallel_range_count` and indexes with `parallel_range_index`, spends one
    step of the run's budget per index before anything is sized from the count (a count past the budget, or past the
    u32 the jobs split takes, is FuelExhausted on the parallel op), and only then sizes the map and the error slots.
  - Each range index's sub-interpreter, the fold's sub-interpreter and each pooled launch body's sub-interpreter start
    at the level of the op that runs them. The level is read on the submitting thread (`in.nesting()`) while the op
    runs and carried in `RangeJob`, in `PooledToken` and as a new `pool_launch` argument, because a pooled launch's
    body runs after the submitting thread has moved on.
  - The map_reduce fold runs on the run's budget like the reference's fold: one step per fold step, its ops, and the
    total charged back with `charge_from`. It ran on one `sub_fuel` budget, uncharged.
  - Bodies on pool workers (range indices, pooled launches) run on Medium fibers (`kBodyStack`).
  - Each body's scratch allocator (range index, pooled launch, fold) starts with a 64 KiB chunk (`kBodyScratchChunk`)
    and grows by chunks; it started with the allocator's default 256 MiB.
- `engine/execution/ceir/include/crd/ceir/exec.hpp`: `Interpreter::set_nesting(level)` (the value form of
  `inherit_nesting`) and `Interpreter::spend_fuel(steps)` (spend a count at once, or nothing).
- `host_provider.hpp`: the budget rule at the constructor, and the `nesting` argument of `pool_launch`.
- `tests/execution/ceir-host/test_host_parallel_bounds.cpp`: six `[ceir][host][diag10a]` cases, each comparing the
  provider with the reference interpreter on the same module.

## The budget rule (decided here)

Everything the submitting thread runs spends the run's budget (2^24, the provider's submitting interpreter), exactly
as in the reference: the program's own ops, one step per parallel index and the fold. Each body a pool worker runs (a
range index's body, a pooled launch body) keeps its own `sub_fuel` budget, the per-item rule of this tier, so a body's
budget does not depend on how the range is split across jobs. The alternative, one shared budget across concurrent
bodies, would make which body runs out depend on worker timing. A test pins the rule: with bodies that dispatch
nothing, the submitting interpreter ends with exactly the reference's remaining budget (observed through the existing
`HostObserver::detach`, which sees the submitting interpreter after the run).

## Findings

1. **The range count overflowed and was cut to 32 bits.** `(hi - lo + step - 1) / step` in i64 is undefined past half
   the line; MSVC wraps `[INT64_MIN, INT64_MAX)` by 2^62 to an empty map where the reference maps four indices.
   `static_cast<u32>` ran 2^32 + 3 indices as 3 and 2^33 as none, successfully. Cases "a host parallel range across the
   whole i64 line" and "a host parallel range is charged to the run's budget".
2. **The provider sized three per-index arrays from the count and never charged the indices.** A range's map, error
   and blame slots were pushed for the whole count before any body ran (2^31 indices would have been about 40 GiB; no
   test runs such a range, so a tooth cannot take the machine down). The count is now spent from the run's budget
   first; the refused ranges allocate nothing from the provider's allocator (a counting allocator checks it).
3. **Sub-interpreters started at nesting level 0.** A range body, a fold step and a pooled launch body each had a
   fresh `kMaxNesting` of their own, so a body called from deep in the program could nest far past the reference's
   bound, and the provider succeeded where the reference fails `NestingTooDeep`. Case "a host body runs inside the
   nesting level of the op that runs it" (ten levels of descent, a body recursing 20 calls fits and 21 does not, same
   blamed op as the reference, for all three body kinds).
4. **Worker bodies ran on 64 KiB fibers.** With finding 3 a body could not be stopped by the bound before its fiber's
   guard page: on win-asan the unchanged provider ended the process with 0xC0000005 and no report on a parallel body
   recursing 30 calls deep. A temporary sweep (removed before commit) on the unchanged win-asan provider: a body
   nesting 34 levels completed on a Small fiber, 36 faulted; about 1.9 KB of fiber stack per level, so 64 levels need
   about 120 KiB. win-debug fits all 62 levels in 64 KiB, which is why only the sanitizer lane shows it. Medium (512
   KiB, 64 fibers) holds 64 levels four times over; a body never waits, so it holds a fiber only while it runs, at most
   one per worker; Large has 16 fibers, fewer than a pool may have workers. Every caller that runs the provider keeps
   the default 64 Medium fibers (crd-sandbox and the application use the default `jobs::Config`, ceridc and the tests
   set only threads and the frame arena); only the fiber-exhaustion specimen lowers the count. Case "a host body can use every nesting
   level on a pool worker's stack" (the deepest body the bound allows, one past it, and a hostile depth of a million
   calls, which ends at the bound on the reference's op).
5. **The fold ran on its own uncharged budget.** The map_reduce fold runs in index order on the submitting thread but
   took one `sub_fuel` budget: with `sub_fuel` 64 a fold of 8 steps of about 55 steps each failed FuelExhausted where
   the reference completes, and a fold's work was never charged to the run. Case "a host map_reduce fold runs on the
   run's budget".
6. **Every body allocated a 256 MiB first chunk.** Each range index, pooled launch and fold built a
   `GrowableTlsfAllocator` with its default first chunk of 256 MiB; the debug heap fills it, so a 4,096-index range
   ran past a 30 s limit on win-debug and 256 indices took 16.8 s (0.27 s with a 64 KiB first chunk), and in release each busy
   worker held 256 MiB. Found while writing the budget case; no test pins it (it is a cost, not an answer), so it has no
   tooth.

Unchanged and recorded in the design: a parallel op inside a body the provider runs on a sub-interpreter fails
NoSemantics (sub-interpreters carry no parallel context) where the reference runs it, and a pooled launch's error
reaches the run at its await where the reference raises it at the launch.

## Teeth

Each break applied to `host_provider.cpp` alone, the lane rebuilt, `[diag10a]` run, the exact bytes restored, the lane
rebuilt and green again (a script compared the restored file with its original).

| Break | win-debug | win-asan |
| --- | --- | --- |
| T1 the signed, 32-bit range count | 2 cases fail | |
| T2 no index charge (the u32 guard kept) | 1 case fails (remaining budget) | |
| T3 a range body at level 0 | 2 cases fail | 2 cases fail |
| T4 the fold at level 0 | 2 cases fail | |
| T5 a pooled launch body at level 0 | 2 cases fail | |
| T6 the fold on `sub_fuel` | 2 cases fail | |
| T7 a fold step that costs nothing | 1 case fails (remaining budget) | |
| T8 worker bodies on Small fibers | passes (62 levels fit in 64 KiB) | the run crashes, 0xC0000005 |

The index arithmetic (`lo + idx * step` in i64) has no tooth here: MSVC wraps it to the right values; it is the same
signed overflow the differential slice showed as a UBSan report on `linux-clang-fuzz`.

## A parallel-CTest race in a DIAG.9a test, fixed in passing

The full win-debug CTest run (below) failed one case once: "diag 9a: a device run on the CPU reference is recorded
and reproduces exactly from its file" (crd-ceir-cook-tests). Run alone it passed five times; its output was
overwritten by the rerun, so the failure itself is not kept. Two cases of `test_replay_device.cpp` wrote and removed
the same relative file, `diag9a_device_record.crpl`, and CTest runs each case as its own process in one working
directory, in parallel. The second case now writes `diag9a_device_record_unbound.crpl`. The fourteen `diag 9a` device
cases then passed ten repeats at `-j 16`. This is the likely cause, not a reproduced one.

## Verification

- **win-debug:** the whole tree built (121 steps after the `exec.hpp` change). The full CTest inside vcvars passed
  7,547 of 7,548; the failure is the race above. crd-ceir-host-tests 53 cases (6 `[diag10a]`), crd-ceir-tests 575,
  crd-ceir-cook-tests 108, crd-sandbox-inspect-tests 31, crd-ceridc-tests 21 with the rebuilt `ceridc`, the
  differential (35) and run-record (57) corpora, and the 8 `crd-no-*`/`crd-check*` guards with master-plan and the
  four CEIR fuzz corpora (13 CTests).
- **win-shipping:** the whole tree built; the same suites pass.
- **win-clang-cl-shipping:** every touched target built and linked first time; the same suites pass.
- **win-asan (the ASan runtime on PATH):** the same suites pass with no sanitizer report.
- **WSL, one lane at a time:** linux-gcc-debug and linux-gcc-asan (the `[diag10a]` cases repeated 3 more times) and
  linux-clang-tsan with `setarch -R` (10 more times): crd-ceir-host-tests 53, crd-ceir-tests 575,
  crd-ceir-cook-tests 108, crd-sandbox-inspect-tests 30, crd-ceridc-tests 21 and the two corpora; no ASan, UBSan or TSan
  report. The Medium-fiber nesting cases pass under both sanitizers.
- After the device-test fix, crd-ceir-cook-tests was rebuilt and passed 108 cases on all seven lanes.
- **Repository checks:** strict tidy is clean on the five changed C++ files (one local-constant name fixed);
  clang-format differs only in the repository's hand alignment, one-line case labels and include grouping; the Allman
  check, check-master-plan and check-repository pass.

## Not done here

- The provider is not in a fuzz target. A third tier in `crd-fuzz-ceir-differential` would need a filtered oracle (the
  reference completes, no body needs more than `sub_fuel`, no parallel op runs inside a body) and a jobs pool in the
  fuzz process.
- The remaining DIAG.10a items are unchanged: the virtual-memory and streaming allocators, the unknown-origin CEIR and
  CKIR artifacts, the concurrent-load corruption and the hosted run.
