# DIAG.10c numerical-mode expectation and the jobs fiber boundary, 2026-10-11

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.10c](../ROADMAP.md#slice-diag.10c). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-10c). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.10b seeds on hosted lanes](2026-10-11-diag-10b-seeds-on-hosted-lanes.md).

## Goal

DIAG.10c was Open with no slice landed. DIAG.9a to 10b have no local work left (only hosted runs or user items), so
this is the first row with local work. Its contract asks, among much else, to "define precision/FTZ/DAZ/rounding
expectations without changing mathematical algorithms" and to probe "unexpected numerical mode" at existing
execution boundaries. This batch is that clause at the boundary where a probe found the mode broken: the crd-jobs
fiber switch. Committed locally, not pushed.

## The declared mode

The thread-start mode on each platform, written down (and matching the runtime FPU state
[ADR-0063](../decisions/0063-eylem-determinism-contract.md) requires of eylem threads, which no test checked):

| Field | Declared | Where it applies |
| --- | --- | --- |
| MXCSR | `0x1F80`: every exception masked, round to nearest even, no FTZ, no DAZ | all SSE/AVX float and double arithmetic, both platforms |
| x87 control word | `0x027F` on Windows (CRT, 53-bit precision), `0x037F` on Linux (glibc, 64-bit) | x87 arithmetic, i.e. Linux `long double`; MSVC emits none on x64 |

Only control state is compared. The six MXCSR status flags and the x87 status word record what earlier operations
did, and the psABI treats them as caller-saved.

## The probe

`engine/foundation/core/include/crd/core/fp_environment.hpp` (crd-core, a leaf every module links):

| API | What it does |
| --- | --- |
| `read_fp_environment()` | the calling thread's control state now |
| `declared_fp_environment()` | the declared mode above, with the read flags this build sets |
| `write_fp_environment(e)` | sets the read fields of `e`, keeping the status flags |
| `ScopedFpEnvironment` | sets a mode for a scope and puts the previous one back, also on an early exit |
| `fp_differences(a, b)` | one bit per differing field: FTZ, DAZ, rounding, exception masks, x87 precision, x87 rounding, x87 masks, other x87 bits, and `Unread` when a field was read on one side only |

MXCSR is read with `_mm_getcsr` on every x86-64 compiler. The x87 word needs inline assembly, so it is read on gcc,
clang and clang-cl and left unread on MSVC (no inline assembly on x64; no x87 arithmetic either). An unread field is
reported, never counted as a match. AArch64 reads nothing yet; its FPCR is JOBS-PORT's.

## The defect at the jobs boundary

Declared contract: every job starts in the declared mode whatever mode the thread that submitted or runs it is in; a
mode a job sets is its own, kept across its waits and thread migrations and gone when it ends; no job changes the
mode of the thread that ran it.

Windows already holds it: `fiber_switch_win64.asm` saves and restores MXCSR and the x87 word, and
`run_job_in_fiber` rebuilds a finished fiber's frame with the defaults. Linux did not. `fiber_switch_lin64.S` said
"No MXCSR / x87 FCW save (they are caller-saved in SysV)", repeated by ADR-0033. The x86-64 psABI (register usage)
makes the MXCSR control bits and the x87 control word callee-saved. So on Linux:
- a job that set flush-to-zero or a rounding mode passed it to the worker's scheduler stack, and from there to every
  later job on that thread, including thread 0 when the main thread pumped or waited;
- a fresh fiber started in whatever mode its thread had;
- a job that waited resumed in the mode of whichever job ran last on the resuming thread.

Every result is plausible and none crashes, so nothing had noticed. Every job runs on a fiber (the worker loop and the
main thread's pump and wait both use `WorkerPool::run_job_in_fiber`; the synchronous `Scheduler::execute_one` is only
called by the white-box scheduler tests), so the fiber switch is the one boundary to hold.

Fix: `fiber_switch_lin64.S` reserves 8 bytes below the pushed registers and saves MXCSR (`stmxcsr`) and the x87 word
(`fnstcw`), and restores both (`ldmxcsr`, `fldcw`) from the stack it switches to, as the Windows switch does. The
whole MXCSR is kept, so a job's sticky flags are its own too. `fiber_init_stack` writes the declared values into a
fresh fiber's frame, which grows from 64 to 72 bytes: `saved_RSP` is now `p - 72` (8 mod 16), the return slot moves
to +56, and `crd_fiber_start` is still entered at RSP 8 mod 16. Both platforms take the values from the probe's
constants (`kMxcsrDefault`, `kX87ControlDefault`). ADR-0033's Linux line carries a dated amendment.

## Tests

crd-core-tests, `[diag10c]` (`tests/foundation/core/test_diag_fp_environment.cpp`):

| Case | Asserts |
| --- | --- |
| the test thread starts in the declared mode | read equals declared; no FTZ or DAZ; round to nearest |
| a scoped mode reads back field by field and is put back | FTZ, DAZ, toward-zero rounding and (where read) x87 24-bit precision with upward rounding read back; `fp_differences` names exactly those fields both ways; the old mode is back after the scope |
| every compared MXCSR field changes a float result | 1/3 is `0x3EAAAAAB` to nearest and up, `0x3EAAAAAA` toward zero and down; a subnormal product is zero under FTZ; a subnormal operand reads as zero under DAZ |
| the comparison names each field, ignores status flags and never matches an unread field | status flags are ignored; each mask, precision, rounding and other-bit change is named alone; an unread side is `Unread`; every difference has its own name |

crd-jobs-tests, `[diag10c]` (`tests/foundation/jobs/test_diag_fp_mode.cpp`). Each case restores the test thread's
mode on exit. A single-thread pool makes the interleaving deterministic (thread 0 runs every job while it waits):

| Case | Asserts |
| --- | --- |
| a job starts in the declared mode, not its submitting thread's | thread 0 in FTZ, DAZ and toward-zero submits a job; the job starts declared; thread 0 keeps its own mode |
| a mode a job leaves set ends with the job | a job sets FTZ and upward rounding and returns; thread 0 and the next job are declared |
| a job keeps its mode across its own wait and the job run meanwhile starts clean | a parent sets a mode and waits on a child, which starts declared and leaves another mode; the parent resumes in its own mode; thread 0 is declared |
| on four threads each job's mode follows the job across waits and workers | 16 rounds of 8 parents with distinct modes, each waiting on a child that leaves a mode behind: no child starts dirty, no parent resumes wrong, thread 0 ends declared |

## Teeth

| Break | Lane | Result |
| --- | --- | --- |
| the old `fiber_switch_lin64.S` and `fiber_init.cpp` (HEAD) | linux-gcc-debug | all 4 jobs cases failed: the first job saw thread 0's mode (55 = FTZ, DAZ, rounding, x87 precision and rounding), thread 0 ended in a job's mode (53), the child saw its parent's mode, the parent resumed in its child's mode (37), and in the four-thread case 127 of 128 children started dirty and 108 parents resumed wrong |
| the Windows fresh-fiber frame holds FTZ | win-debug | all 4 jobs cases failed (`1` = flush-to-zero; 128 of 128 children dirty) |
| `fp_differences` ignores rounding | win-debug | the scoped-mode case failed (both directions) |
| `write_fp_environment` writes nothing | win-debug | the scoped-mode and float-result cases failed (10 assertions, including the toward-zero and downward 1/3) |

Each break was restored (the Linux files from a backup with a fresh timestamp, the others by rewriting the line),
byte-compared with the fixed file, rebuilt and rerun green on the lane that observed it.

## Verification

| Lane | crd-core-tests | crd-jobs-tests |
| --- | --- | --- |
| win-debug | whole tree built (191 steps); 39 cases | 178 cases |
| win-shipping | 31 cases | 170 cases |
| win-clang-cl-shipping (linked first time) | 31 cases; reads the x87 word, `0x027F` as declared | 170 cases |
| win-asan (inside vcvars) | 39 cases | 178 cases |
| linux-gcc-debug | 31 cases | 177 cases; crd-jobs-schedcheck-tests 6 cases |
| linux-gcc-asan | 31 cases | 177 cases; `[diag10c]` repeated 3 times |
| linux-clang-tsan (`setarch -R`) | 31 cases | 177 cases; `[diag10c]` repeated 10 times, no TSan report |

The only sanitizer reports are the deliberate specimens' (the crash-capture specimen's null store and SEGVs on
linux-gcc-asan; the data-race specimen on linux-clang-tsan), repeated into the test logs by the harness. The jobs
test was edited once more after these runs (its two `std::array` locals became `StaticArray`, see below); it was
rebuilt and rerun whole on all seven lanes.

Coverage (`linux-clang-coverage`, WSL, clang 18.1.3): the scope registry gains a group `numerical-mode probe`
(DIAG.10c: `fp_environment.cpp` and its header). The measured set was rebuilt and measured once: 861 CTests passed
(853 before, plus the eight new cases), the schedcheck program passed, 962 raw profiles, 65 scope files all
instrumented, verdict PASS. The new group ran 65 of 69 regions and 61 of 70 branches. Totals: regions 9,299/10,441,
branches 6,044/7,714, functions 1,040/1,089, lines 17,312/19,846. llvm-cov warned of 1,319 functions with
mismatched data (the known jobs/schedcheck twin).

The full win-debug CTest inside vcvars (`-j 8`) ran 7,567 tests: 7,563 passed, one skipped by its own gate, and
three failed:
- `crd-no-std-containers-check`: the new jobs test used `std::array`. Now `crd::containers::StaticArray`.
- `crd-build-profiles`: `test-native-build-profiles.py` pins the number of visible configure presets at 24, and
  DIAG.10b's coverage batch (c8a46664, local, not pushed) added `linux-clang-coverage` as the 25th. That batch ran
  only the CTests it touched, so the stale pin was not seen; the hosted run would have failed on it. The pin is now
  25 (the last hosted-green commit, 6bd1ba59, has 24).
- `v9a-c GPU N=10000 random: CPU vs GPU topology byte-identical + bounds within 1 ULP`: six node bounds differed by
  0.003 to 0.049. Run alone it passed, then 10 of 10. Recorded as `lbvh-gpu-bounds-parallel` in the
  [nonreproduced-failure register](../../tests/support/diag/nonreproduced-failures.json), not chased (the batch
  touches nothing the BVH builders run).

After the fixes, the ctest guards (`^crd-no-|^crd-check`) and `crd-build-profiles` pass.

Repository checks: strict tidy (LLVM 20.1.8) clean on the five changed C++ files; clang-format differs only in the
repository's hand alignment, case-label indentation and include grouping (two other findings, a 121-column line
and a splittable test name, were fixed); the Allman check, `test-native-build-profiles.py`, `test-diag-coverage.py`
(29), `diag-coverage.py --validate`, `test-repository-tools.py` (66; its 3 skips need CMake and Ninja on PATH), `test-dev-workflow.py` (54), check-master-plan and check-repository pass. git status shows only this batch's files; nothing is left running, and the WSL coverage profiles (`~/d10c-cov`) are deleted. After the commit's final edits, `crd-diag-seeded-faults`, `crd-repository-tools`, `crd-repository-hygiene`, `crd-developer-workflow` and `crd-ci-tiers` also pass on win-debug.

## Remaining in DIAG.10c

Value-domain probes with per-domain allowances; the generation-attributed wrong-result oracle at the CEIR boundary;
shape/stride and units/time-domain probes; the mode probe at other boundaries; lifetime accounting beyond heap bytes;
queue/VRAM pressure and budgets; warm/cold, soak, reload and shutdown workloads with seeded injection; slowdown
measurement; device-reset and host-pressure evidence; and the hosted run.
