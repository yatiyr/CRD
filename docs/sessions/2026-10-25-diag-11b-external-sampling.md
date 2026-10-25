# DIAG.11b clause (8): the 2026-10-25 sampling capture and C++ Build Insights, read

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-11b). Board:
> [external sampling](../bench/2026-10-25-diag-11b-external-sampling.md); build board:
> [Build Insights](../bench/2026-10-18-build-speed-compile-time.md#c-build-insights-capture-2026-10-25). Rules:
> [AGENTS](../../AGENTS.md). Earlier read: [2026-10-24](2026-10-24-diag-11b-sampling-attribution.md).

The user ran `scripts\elevated-captures.bat` elevated with Visual Studio closed; both steps exited 0. The sampling
trace is `build/diag-overhead-wpr/20261025T120843Z` (2.4 GB, both PDBs kept under `symbols/`), the Build Insights
capture `build/build-insights/`. As recorded, six sampling rows read DISAGREES. Each is explained below with evidence
from the trace, the binaries and two boards; the operator's starting hypotheses were tested, not assumed. The
complete-tier CI run 40195335794 on `6d2b2096` was running and was not touched.

## How the trace was read

`xperf -a dumper -stacktimeshifting` was streamed once and the recorded processes' samples and stacks kept (384,026
samples, the same per-process totals `analyze` counts); per-function counts per 1,000 frames, sampled addresses (the
image base `0x7ff6b9ee0000` from the trace's image-load events) and `cdb -z` disassembly of the kept PDB located each
cost. The analysis itself was repeated with the repository's `analyze` after the repairs (4 min 45 s, no elevation).

## The two binaries came from two sources

The profiling binary was linked 2026-10-24 06:20Z and the compiled-out one 12:30Z; neither tree was rebuilt after.
The baseline contains the rendering workload's device-validation option (`d7836fbb`, 10:26Z; 7 `validation` strings)
and the profiling binary none, so the pair also differed in source (the shared harness paths run no rendering, and
`d96eae30`'s jobs change only touches the pressure readings, not the job path). `record` now refuses, before the
sampler starts, any harness whose tree has work left for the target (`ninja -n`, from the tree's
`CMAKE_MAKE_PROGRAM`), and the manifest keeps the checkout's commit and whether tracked files differ.

## (a) Off over compiled-out (alloc, jobs, offline): method fixed

Hypothesis: the gap is the two-binary layout floor, no diagnostic code runs in off, so the frame-time verdict is
mis-specified. **Proven** for the verdict, **refuted** for "layout" on two of the three workloads:

- Off runs no out-of-line diagnostic code: 0.0% to 0.1% of compiled-out's CPU in every workload. So the row cannot be
  judged on frame time against the diagnostic share; it compared a layout-and-inlined-code gap with zero.
- alloc +10.0% frame (+8.4% sampled CPU): 91% of the added samples are in `TlsfAllocator::try_allocate` (39.8 → 46.1
  per 1,000 frames), the host of the force-inlined statistics gate. That is the gate's work, clause (1)'s reported
  worst case (+5.55% instructions on Linux), not layout.
- jobs +9.6% frame (+8.9% sampled CPU): `FiberPool::acquire` 1,538.6 → 1,930.9 per 1,000 frames, more than the whole
  gap; the jobs frame has no gate (exact count 0) and the instruction board reads +0.00%. It is contention on the
  fiber pool's free list (below), whose amount moves with timing.
- offline -6.1% frame (-4.9% CPU, off faster): `run_seq` 274.4 → 255.6; instructions +0.08%. Layout.

The fix (`sample-overhead-wpr.py`): a two-binary row agrees when off's added out-of-line diagnostic CPU over
compiled-out is within 2% of none, and its sampled CPU agrees with the measured cost where they are commensurate; the
gap's location is printed and the disabled budget stays with the instruction count. On this trace all three AGREE. With
this trace, counting `TlsfAllocator::try_allocate` as diagnostic code makes alloc DISAGREE (+7.7%), counting
`FiberPool::acquire` makes jobs DISAGREE (+11.8%). Tests `test_two_binaries_are_judged_on_the_diagnostic_code_off_runs`
(fails on the old method) and `test_two_binaries_disagree_when_off_runs_diagnostic_code`.

## (b) Jobs full 14% faster than off with 12.5% diagnostic samples: method fixed, a jobs finding recorded

Hypothesis: per-job work spaces the four pool threads' dequeues from the shared injection queue; frame time falls
while CPU rises. **The spacing is right; the rest is refuted:**

- The trace did not run four threads: `record` passed no `--threads` and pinned to `FFFF`, so jobs ran the harness's
  default eight-thread pool on 16 logical processors, the shape the boards abandoned on 2026-10-24.
- CPU falls with the frame time: sampled CPU per frame 3.619 → 3.347 ms (-7.5%); 8.2 to 8.9 threads busy per frame. The
  harness's own CPU at that shape (24-process board, same binaries): full -8.25% (-14.06 .. -5.13%), frame -5.82%.
- The contended structure is the fiber pool's free list, not the injection queue: in off, `FiberPool::acquire` holds
  1,930.9 of 3,619.0 samples per 1,000 frames (53%), 1,342.9 of them on the instruction after `lock cmpxchg qword ptr
  [rcx+18h]` (the Treiber head, `fiber_pool.cpp` `acquire_from`), with `acquired_count` and the peak on the same line.
  In full it falls to 851.9. `Scheduler::try_pop` (the injection queue) is 240.1 in off and rises to 340.9 in full.
- Full's cost is real: its diagnostic frames add 11.6% of off's CPU and their call sites 8.6% (the job body
  `sum_slice`, where each site's payload is formatted: `snprintf` decided 3,883 samples on its own before), but the
  pool's contention falls 27.7%.
- At the boards' shape (four threads, `5555`) the same binaries read full +37.12% (+35.77 .. +38.40%) in frame time and
  +37.04% in CPU. Full has no budget (reported), so nothing is hidden by a budget.

Fixes: `record` runs the boards' shape (`--threads 4 --affinity 5555`) and keeps each record's CPU measurand; a row is
measured on the harness's CPU per frame when both cells have it, on the frame time only when the frames run on one
thread, and otherwise not compared; a row also has to place its added CPU in the diagnostic frames and their call
sites; the C runtime's formatting and copies in the image (`snprintf`, `memcpy`) run for their caller. On this trace
jobs full stays DISAGREES, correctly: at eight threads its frame is the pool's contention. Jobs basic is UNRESOLVED:
its two processes read 393.1 and 470.5 µs (19.7% apart; a cell's processes must agree within 2%). Both need the new
recording. The fiber pool's contended head at eight workers is a jobs-substrate finding (it wastes about 37% of the
tiny-job workload's CPU at that shape), owned by [JOBS-HARD.3](../ROADMAP.md#slice-jobs-hard.3) (pool utilization and
oversubscription); it is not a diagnostic cost. Tests `test_a_multithreaded_frame_is_compared_on_its_cpu`,
`test_a_cost_the_samples_do_not_explain_disagrees`, `test_processes_that_disagree_are_unresolved`,
`test_the_c_runtime_runs_for_its_caller_and_a_diagnostic_sample_names_its_call_site` and the record cases.

## (c) Offline full: the estimator was wrong, the attribution right

The sampled totals give +252.6% (24,604 against 6,977 samples over the same 20,000 frames); the harness +258.2%. The
share estimate gave +134.5% because it divided the diagnostic samples by full's own non-diagnostic samples (10,491),
not off's (6,977): it assumed the mode adds no time outside its frames. The breakdown, of off's CPU per frame: the
bridge's `adapter_pre` +116.4% and `adapter_post` +85.5% (the inlined `rdtsc` reads are inside them; `CycleCounter::now`
has no frame of its own), `profiled_run`, the recorder and the profiler +0.3%; the executor `run_seq`, which calls both
hooks around every dispatched op, +43.9% (255.6 → 408.7 per 1,000 frames); everything else +6.5%. cdb places
`run_seq`'s added samples at its call sites: after `call rax` the loop index is reloaded from its stack slot
(`[rbp+0B0h]`) and the sequence from `[rbp+20h]` (+0xA0F, 18.9 per 1,000 frames), and the next instruction's dependent
loads at the loop head (+0x1EA, 4.5 → 41.0). No bridge function was missing from the class list: of the functions that decide samples only
in full, the two hooks hold 406.1 and 298.3 per 1,000 frames and every other is under 1 (`snprintf` 0.7,
`profiled_run` and `write_bundle_atomic` 0.2). The roadmap's +219% to +220% is the 200-frame board; this capture's 10,000-frame processes read
+258%.

Fix: the external cost is the sampled CPU per frame; the diagnostic frames plus the growth of their call sites must
explain it. Offline full now AGREES: diagnostic code +202.3%, call sites +44.1%, elsewhere +6.3%. Test
`test_a_cost_in_the_diagnostic_code_and_its_call_sites_agrees` (fails without the call sites and with the share
estimate).

## The verdicts after the repairs

`python scripts/sample-overhead-wpr.py analyze build/diag-overhead-wpr/20261025T120843Z` (exit 6): AGREES for alloc
basic, compiled-out and full, io basic, compiled-out and full, jobs compiled-out, offline basic, compiled-out and full;
UNRESOLVED jobs basic; DISAGREES jobs full. The per-row numbers are in the [board](../bench/2026-10-25-diag-11b-external-sampling.md).
Mutations, each restored: thirteen (the two-binary rule disabled or on frame time, no call sites, the share estimate,
frame time over a CPU measurand, frame time for a multi-threaded frame, the C runtime deciding, a diagnostic caller as
the call site, no spread check, no tree check, the old affinity, no `--threads`, the CPU measurand dropped) each fail
`test-sample-overhead-wpr.py` (now 18 cases, 12 before).

<a id="c-build-insights"></a>
## C++ Build Insights

Read the way `build-profile.py` intends ([board section](../bench/2026-10-18-build-speed-compile-time.md#c-build-insights-capture-2026-10-25)):
`build-profile.py render` of `win-debug.json`, the vcperf time-trace JSON for the passes and function generation, the
include trees of `win-debug.log` for who brings a header, and `wpaexporter` with the add-in's tables for the template
and function views. Top costs: front end 1,705.5 s against back end 71.7 s (include parsing 1,297.8 s); Catch2's test
macros 243.5 s over 1,002 units; MSVC's stream headers 183 to 191 s over 1,273 units (Catch2's `catch_tostring.hpp`
in 601, `crd/math/complex.hpp`'s `<complex>` in 547); `crd/math/cmath.hpp` 172.7 s over 644; the FFT's generated
codelets 101.9 s of exclusive parse in 35 units and 126.8 s of the 135.6 s of function generation (each `double`
codelet generated in 21 units). vcperf's template-instantiation table came out empty; template time is read from
`/d1reportTime`'s class and function definitions, where MSVC records it. Three safe cuts are recorded there as
follow-up data (the codelets in one unit, one shared fuzz replay driver, a `cmath.hpp` without `<complex>`); none was
started here.

## Not done here

- The new recording: `scripts\elevated-captures.bat sampling` from an elevated prompt with Visual Studio closed,
  after building `crd-diag-overhead` on win-shipping-profile and win-shipping at the same commit (`record` now refuses
  otherwise). It is the user's step; it decides jobs basic and full on CPU against CPU at the boards' shape.
- Peers on comparable features: no peer tool is defined or installed for the diagnostic modes; choosing and
  installing one is a download that waits for the user's approval.
- The fiber pool's contended free list: [JOBS-HARD.3](../ROADMAP.md#slice-jobs-hard.3).
