# Needs CI audit against the first green complete-tier run, 2026-10-06

<!-- doc-role: historical -->
> Dated evidence. Live owners: [REPO.DEV](../ROADMAP.md#slice-repo.dev) and its children,
> [DIAG.3a](../ROADMAP.md#slice-diag.3a) through [DIAG.7a](../ROADMAP.md#slice-diag.7a).
> Rules: [AGENTS](../../AGENTS.md).

## Why this audit

The user asked why 32 rows still read Needs CI after days of CI work. The roadmap rule is "inspect changed CI results
first on re-entry". It was not followed: each CI read looked only for failures, and no row was rechecked once its
evidence existed.

That evidence came with run [37311671924](https://github.com/yatiyr/CRD/actions/runs/37311671924) at `4737877d`
(2026-10-05). It is the first complete-tier hosted run in which **all 25 jobs passed**:
- preflight, which resolved the run to the complete tier;
- the two repository jobs;
- the 21 test lanes, including the five presets that had never run hosted (`win-relwithdebinfo`, `win-debug-scalar`,
  `win-shipping-profile`, `linux-gcc-debug-scalar` and the native `win-vs`) and both public-check presets;
- `required`.

Each row below was checked against the list in the
[infrastructure audit](2026-09-14-infrastructure-audit.md) of what "the first hosted run must show".

## What the run shows

- **Repository jobs, on both runners with Python 3.12:** every guard passes (`check-repository`, `check-master-plan`,
  `check-ci-tiers`, `check-pins`, `check-generated` over 160 sources, `check-allman-braces`, the license manifest).
  So do the tooling suites:
  - `test-repository-tools` (55 tests);
  - `test-module-selection` (12);
  - `test-dev-workflow` (54);
  - `test-project-sync` (60);
  - `test-native-build-profiles` (7; on Windows also under Visual Studio 18 2026);
  - the native project sync, and the CEIR generator and capability-matrix checks.
- **Evidence bundles:** each of the 21 lanes uploaded one. The `win-debug` bundle's `conclusion.json` (schema
  `cerid-ci-evidence/1`) records revision `4737877d`, the tree, the toolchain, the adapter and the build's `ninja` and
  `sccache` sections. `win-debug` built through the pinned sccache: 1,607 hits, 97.3 %.
- **Fuzz corpora:** the four loader corpora and the perf-bundle corpus ran and passed on every lane checked.
  `linux-gcc-asan` ran its whole suite; its one failure is the registered third-party defect TP-5, accepted by the
  register gate.
- **Verified acquisition:** the Vulkan headers ("every digest verified"), the Khronos validation layer and sccache
  ("archive SHA-256 verified") were downloaded and verified on the lanes that install them.
- **Not shown: cold CPM acquisition.** Every lane restored its CPM archive cache (and Windows its Vulkan SDK cache),
  through the restore-key fallback. The first complete run, `34821419392` on 2026-09-14, restored them too. No hosted
  run has yet acquired the CPM archives, the Vulkan SDK and WARP from cold through the verifying helpers.

## Verdict per row

| Row | The hosted run had to show | Shown in 37311671924 | New state |
|---|---|---|---|
| REPO.3c | a green complete run over its closed children | yes | Done |
| REPO.DEV.3b.3 | the hosted repository jobs | yes, both runners | Done |
| REPO.DEV.3b, 3c, 3 | `test-dev-workflow.py` (54) and the frontend guards on both runners | yes | Done |
| REPO.DEV.4 | `test-module-selection.py` (12) and `check-repository.py` on both runners | yes | Done |
| REPO.DEV.5 | preflight resolving to complete; `required` green with every expected job; a bundle per lane; the five presets and the native solution | yes | Done |
| REPO.DEV.6 | cold runners acquiring every archive, the Vulkan SDK and WARP through the verifying helpers | **no**: every CPM and SDK cache was restored | Needs CI (named gap) |
| REPO.DEV.7 | each bundle with its Ninja log and sccache statistics; `win-debug` built through sccache | yes | Needs CI: waits on its prerequisite DEV.6 |
| REPO.DEV.8 | both public-check presets green | yes | Needs CI: waits on DEV.6 through DEV.7 |
| REPO.DEV.9 | the corpus CTests on every lane; `linux-gcc-asan` whole suites | yes | Needs CI: waits on DEV.6 through its prerequisites |
| REPO.DEV.10 | the two repository-job steps on both runners with Python 3.12 | yes | Needs CI: waits on DEV.6 through its prerequisites |
| REPO.DEV.11 | the first complete-tier run, and the selector comparison | the run, yes; the comparison needs a complete run of a change-tier revision (the first change-tier push is `215813f0`) | Needs CI |
| REPO.DEV (parent) | every child | DEV.6 to 11 remain | Open |

## DIAG.0 was left Open

DIAG.0's own text said "Local proof complete (flip to Needs CI on the maintainer push)", but the row was still Open.
Its one hosted clause is shown in 37311671924. On `linux-gcc-asan` and `win-asan`, "diag harness: the sanitizer
specimen is caught, or reported absent" passes; on an ASan build that case asserts SanitizerCaught. The five negative
controls pass on every lane. The row is now Needs CI and waits only on its prerequisite REPO.DEV.

## Rows that never belonged in Needs CI

Needs CI requires that all available local work is finished. Two rows say otherwise in their own text:
- **DIAG.3a:** "the remaining allocators' size paths + the full per-allocator zero-size/exhaustion/realloc-failure
  sweep".
- **DIAG.3b:** "the intentional use-after-reset negative-control specimen".

Both are now Partial, with that work named. The other DIAG Needs CI rows (DIAG.1a to DIAG.6b) cannot become Done yet:
they sit after the open REPO.DEV and DIAG.0, and the validator allows Done only in table order. They keep their CI
wait.

## Closing DEV.6

The gap needs one hosted run that cannot restore an archive cache. There are two ways to get it:
- a manual complete run after the repository's Actions caches are cleared, which is the user's decision; or
- a standing cold-acquisition step that configures with an empty CPM source cache in the complete tier.

Either one also closes DEV.7 to DEV.10, whose own hosted clauses are already shown.

**User decision (2026-10-06):** "go with option 2", the standing step.
- **Landed:** the complete-tier `win-debug-scalar` and `linux-gcc-debug-scalar` now skip every archive cache, and
  `scripts/check-cold-acquisition.py` checks before and after the configure.
- **Verified locally:** a real cold configure on the WSL reference host acquired all seven selected packages; the
  checker passed, and its `--before` mode rejected the warm cache afterwards. The tooling suite (56 tests) and the
  pins, CI-tier and repository guards pass.
- **Remaining:** the next complete-tier hosted run is the proof.

## CI reads after the audit

**v20, run 37389502998 at `a3f8c19b`.** The user approved cancelling the superseded queued and running runs (v14
`2dea4e6a`, and v16 to v19). Before the cancel, v16 had 12 jobs green and none failed.
- In v20, the Linux repository job failed in "Affected workflow selection contracts":
  `test_supervisor_parent_death_does_not_leave_a_running_tool` raised `ProcessLookupError`.
- **Cause: the fixture.** The supervised tool was reaped between `open()` and `read()` of `/proc/<pid>/stat`, and the
  kernel reports that as `ESRCH`, not `ENOENT`. The supervisor behaved correctly: the tool was already gone.
- **Fix:** `fixture_process_running` now treats both errors as "not running".
- **Checked:** `scripts/test-dev-workflow.py` passes 3 of 3 runs on the WSL reference host and passes on Windows.
- **Rows:** no Needs CI row flips on this read. The run is incomplete and is not a complete-tier run.
- Later in v20, `linux-gcc-debug` failed one of 6,838 tests: the `crd-no-malloc-allocator` guard. The new DIAG.3a
  boundary test constructs `MallocAllocator` to test it, and the guard only allows named allocator-test files.
  - **Fix:** add `tests/foundation/memory/test_allocator_boundaries.cpp` to both guard scripts' allowlists, next to
    `test_memory.cpp`.
  - **Checked:** both scripts pass, as do all 8 win-debug guard ctests.
  - **Cause of the miss:** the local checks ran the memory exe, not ctest's guards. That step is now part of every
    handover.

**v20 finished, and v21 ran twice.**
- v20 (change tier) failed only the `crd-no-malloc-allocator` guard on every lane (fixed in the previous batch). Its
  other failures on `win-asan` (four DX12 ray-tracing gates) and `linux-gcc-asan` (the mikktspace oracle) are the
  lanes' registered TP-1 and TP-5 entries.
- v21 `ba70bacf` (complete tier) failed the same guard everywhere, because it predates the allowlist fix.
- **v21 `3ae81c59`, run [37395083192](https://github.com/yatiyr/CRD/actions/runs/37395083192) (complete tier):**
  23 of 24 test jobs green. The one failure is `linux-clang-tsan`, a fiber shadow-stack underflow in the TSan switch
  annotations. It is fixed and recorded in the
  [TSan inventory](2026-10-05-clang-werror-and-tsan-inventory.md#second-complete-run-2026-10-06).

Rows, against this run:
- **REPO.DEV.6 stays Needs CI.**
  - Shown: both cold lanes started empty, acquired and verified the seven selected packages and passed their lanes.
    The Windows cold lane also installed the Vulkan SDK and WARP cold through their verifying helpers.
  - Not shown: the two gated bench pins (Eigen, OpenBLAS), which the checker named but no hosted preset acquires.
    Both cold lanes now also download and verify those archives (`--fetch-unselected`). Both verify on the reference
    host, so the next complete-tier run closes the row.
  - Corrected: the row's package count, from ten to nine.
- **REPO.DEV.7 to DEV.10** were green on their lanes again and still wait only on DEV.6.
- **REPO.DEV.11:** the selector comparison still needs a complete-tier run of a change-tier revision. Both v21 runs
  resolved to the complete tier because they changed build-system paths. v20 is a change-tier revision but ran only
  the change tier.
- **DIAG.1b** stays Needs CI: the lane did not qualify on this run (the crash above). The DIAG rows after REPO.DEV
  cannot be Done before it.

## Rows closed on the green complete tier (2026-10-06)

The evidence:
- **Nightly complete run [37445373995](https://github.com/yatiyr/CRD/actions/runs/37445373995) at `41f9962c`:** 26 of
  26 jobs green. Both cold lanes downloaded and verified the seven selected packages and the Eigen and OpenBLAS
  archives; the Windows cold lane also installed the Vulkan SDK and WARP cold. `linux-clang-tsan` was green.
- **`aa784559` (the loop's log-shutdown fix):** its push resolved to the change tier and passed 18 of 18 (run
  37456623136). The complete run dispatched on the same revision,
  [37456628683](https://github.com/yatiyr/CRD/actions/runs/37456628683), passed 26 of 26. That is REPO.DEV.11's
  selector comparison: the change tier missed no failing lane.

Flipped to Done in roadmap order: REPO.DEV.6 to DEV.11, the REPO.DEV parent, and DIAG.0, 1a, 1b, 2a, 2b. The user asked
for every Needs CI row to be ticked. DIAG.3c to DIAG.6b (13 rows) also have their hosted clauses green on these runs,
but the order rule allows Done only when every earlier row is Done, and DIAG.3a and DIAG.3b are Partial with local work
left. They close as soon as those two are finished, which is the next work in order. The first unfinished row is now
DIAG.3a.

## CI read before the DIAG.3a part 4 batch (2026-10-06)

`6ef7cb05` (the docs-only push closing the rows above) resolved to the preflight tier: run 37496382317 passed preflight
and both repository checks, and skipped the build and test lanes. It is no new hosted evidence for any row, so nothing
was flipped. DIAG.3a then finished its local work ([part 4](2026-10-06-diag-3a-allocator-boundaries.md)) and moved to
Needs CI; a complete-tier run of its revision closes it. The first unfinished row is now DIAG.3b.

## CI read, 2026-10-09: `a1e5301f`

The DIAG batches were pushed together as `5c6b359d`. Its complete run, 37776561255, failed four tests, which the next
commit, `a1e5301f`, repaired. The two runs on `a1e5301f`:
- **Push run 37814307870:** one unexpected failure, `crd-developer-workflow` on win-debug-sse2. In
  `test_supervisor_budget_stops_spawned_descendant` the test's own checks passed (budget exhausted, descendant not
  running), but removing its temporary directory hit WinError 32 on `output.log`. The same error appeared on
  `a0e56690`'s push run. On Windows a file can stay open for a moment after its process ends (a job-terminated
  process, or a scanner reading a new log).
- **Complete run 37814393863:** one failure, `crd-developer-workflow` on win-vs.
  Two cases of `test_simd_guard_preserves_decoder_failures_and_explicit_skips`, the ones where the simulated decoder
  exits 9, ran out of their 15 s budget (exit 124) with no output. The same test passed on win-vs in the three runs
  before, with totals of 13.7 s to 33.7 s, so the budget was too tight for that runner's variance. Nothing in the
  guard, the test or the supervisor changed since it last passed.

Fixes, both in `scripts/test-dev-workflow.py`. No oracle changes; every expected status and exit code is the same:
- the temporary directories are removed by `cleanup_temporary`, which retries a Windows sharing violation for at most
  10 s and then raises;
- the SIMD-guard cases get a 60 s budget.

Checked: the suite passes on Windows (54 tests) and on the WSL reference host.


## Rows closed on 6bd1ba59 (2026-10-09)

Both runs on `6bd1ba59` resolved to the complete tier and passed every job:
- push run [37852297628](https://github.com/yatiyr/CRD/actions/runs/37852297628);
- nightly run [37915366121](https://github.com/yatiyr/CRD/actions/runs/37915366121).

Each Needs CI row was checked in the job logs (the per-test status on all 20 test lanes of both runs) and in the
`win-debug`, `win-asan`, `linux-gcc-debug`, `linux-gcc-asan` and `linux-clang-tsan` evidence bundles (`LastTest.log`
of the push run). The only failures are the registered ones: TP-1 on `win-asan` (five DX12 ray-tracing gates) and TP-5
on `linux-gcc-asan` (the mikktspace oracle). Both lanes' register gates passed.

| Row | What the logs show | New state |
|---|---|---|
| DIAG.3a | the memory suite (162 cases) green on every lane, `linux-clang-tsan` included | Done |
| DIAG.3b | the allocator ASan case green everywhere; 25 `use-after-poison` and 6 `container-overflow` reports on both ASan lanes, `InstrumentAbsent` elsewhere | Done |
| DIAG.3c, 3d, 3e | their memory and container cases green on every lane | Done |
| DIAG.3f | use-after-free caught on both ASan lanes; use-after-return and the leak caught on `linux-gcc-asan` and reported absent on `win-asan`, as declared | Done |
| DIAG.4a | identity, observer and wait-contract cases and both negative controls green on every lane | Done |
| DIAG.4b | the jobs suites green, but the GenMC model has never been run | Partial |
| DIAG.4c to 6b | each row's cases green on every lane; the perf-bundle corpus replays on every lane | Needs CI, waits on DIAG.4b |
| DIAG.6c | its C++ cases green, but no hosted job runs its three script suites | Partial |
| DIAG.7a | DX12 cases with assertions on every Windows lane (WARP); Vulkan cases with assertions on lavapipe | Needs CI, waits on DIAG.4b and 6c |

### Gaps found

- **DIAG.4b.** The row said the weak-memory model was "qualified on a pinned Linux GenMC lane". No such lane exists,
  `cmake/pins.json` pins no GenMC, and no run is recorded. The model's own header calls its verdicts lane-pending. The
  design acceptance needs "the repaired bounded model passes with bounds stated", which nothing has shown, so the row
  returns to Partial. Local work: pin GenMC and run both invocations on the WSL reference host. User decision: a hosted
  GenMC lane in the nightly/complete tier, as for TSan, or the recorded WSL run accepted as declared evidence.
- **DIAG.6c.** Its hosted clause is "the tooling tests green", meaning `test-sample-cpu-wpr.py`,
  `test-counters-capability.py` and `test-debugger-recipes.py`. `ci.yml` runs five other tooling suites and none of
  these. Running them in the repository-checks job is local work, so the row is Partial.
- **Vulkan cases on hosted Windows.** The runners have no Vulkan device. DIAG.7a(f) reports skipped. The other five
  Vulkan 7a cases return early with a warning and pass with no assertions, which is a skip reported as a pass. Their
  hosted evidence is lavapipe on Linux, where all six run with assertions. Converting the early returns to `SKIP()` is
  separate work.

DIAG.7a had no local or user item left (the DX12 hardware pass-to-fault route is unqualified by user decision), so it
moved from Partial to Needs CI. The first unfinished row is now DIAG.4b.
