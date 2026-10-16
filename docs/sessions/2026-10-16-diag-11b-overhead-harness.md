# DIAG.11b overhead harness, the event-site form and the first boards, 2026-10-16

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-11b) and
> [ADR-0133 ID-5](../decisions/0133-runtime-diagnostics-and-instrumentation.md#implementation-decisions).
> Rules: [AGENTS](../../AGENTS.md). Board: [2026-10-16 DIAG.11b overhead](../bench/2026-10-16-diag-11b-overhead.md).

## Goal

DIAG.11b's first clause: measure the declared budgets of the diagnostic modes (off, basic, full) on real workloads,
with tails, memory, drops, I/O and capture completion, and store the boards with their losses. This batch builds the
measurement (a harness, a driver, structural checks that run on every lane) and the first boards for the CPU workloads;
it repairs one structural gap the modes contract names and records every budget the boards show violated. Committed
locally, not pushed.

## What landed

| Piece | Where | What it shows |
| --- | --- | --- |
| Event-site form | `CRD_FLIGHT_RECORD` and `FlightRecorder::admit` (`crd/perf/flight_recorder.hpp`) | a site's message and scalars are evaluated only when a recorder runs and its detail keeps the event; the recorder and severity expressions once. `admit` is one atomic load; when the detail leaves the event out it counts it in `total_filtered()` as `record()` does |
| Lock-free filtered path | `FlightRecorder::record` (`flight_recorder.cpp`) | an event the detail leaves out is counted and dropped before the clock is read or the ring mutex taken (before: under the mutex, so a Basic recorder serialized every informational site on its ring) |
| Site contract cases | `tests/foundation/perf/test_diag_flight_sites.cpp` (`[diag11b]`) | side-effect controls: no recorder, an uninitialised one and a Basic detail evaluate no payload and change nothing; Basic keeps a warning and evaluates it once; Full evaluates each argument once; a reload to Basic changes what is admitted; after shutdown the site is off. And the no-wait case: while the ring is held (the asserts-only test seam), a Basic recorder's informational `record()` and `admit()` return, and a Warning, which the recorder keeps, does not complete until the ring is released (the positive control that the ring is held) |
| Harness | `tests/support/diag-overhead` (`crd-diag-overhead`), owned by crd-ceir-host, whose closure covers every workload | one workload in one mode per process, one JSON record (`cerid-diag-overhead/1`): build, host modes, every timed frame, sites and payload evaluations, recorded and filtered events, profiler frames and drops, captures requested and delivered with bytes written, resident set, the lifetime ledger's windows |
| Authored assets | `assets/diag/overhead_basic.flight.toml`, `overhead_full.flight.toml`, `overhead_offline.ceir` | the two modes' flight policies (the default policy at each detail, with an unreachable hitch threshold and a 100 ms post window so a measurement sees the steady state) and the offline program |
| Structural CTests | `crd-diag-overhead-<alloc\|jobs\|offline\|events>-<off\|basic\|full>` (12) | every workload in every mode, 8 frames, `--check`: every frame ran with its known answer; the lifetime ledger balances; the workload's setup started exactly its own threads plus the mode's (the watchdog in basic and full, nothing in off); off evaluates no payload, acquires nothing setting up, runs no recorder or profiler and asks for no capture; basic evaluates no payload, records nothing and filters every site and frame event; full evaluates every payload once and records every event; every requested capture is delivered and written |
| Driver | `scripts/diag-overhead.py` (`run`, `render`) | interleaved processes (each repetition rotates the arms), pooled nearest-rank percentiles, paired per-repetition ratios with a seeded percentile bootstrap, verdicts within / over / unresolved against ID-5, storage against 32 MiB, any failed process an instrument failure that removes the workload's verdicts; a site microbenchmark is reported per site, never judged |
| Verdict tests | `scripts/test-diag-overhead.py` (`crd-diag-overhead-verdicts` CTest, repository CI step "Overhead board verdicts") | statistics, every verdict boundary, interleaving, record parsing, every instrument failure (nonzero exit, no record, a wrong answer, a lost capture), the reported site microbenchmark, a stable render |
| Boards | [2026-10-16 DIAG.11b overhead](../bench/2026-10-16-diag-11b-overhead.md) | Windows (win-shipping-profile against win-shipping) and Linux (linux-gcc-shipping, linux-gcc-relwithdebinfo) boards, plus the attribution experiment |

## The workloads against ADR-0133 ID-5

ID-5 binds the budgets to named workloads. The harness drives the production paths those name, as an application
drives them, with these deviations, recorded in the amended ID-5 as well:

- allocation-heavy has no ID-5 row; it is the DIAG.11b contract's own workload (`alloc`, a TLSF heap behind the host's
  allocator watch).
- the jobs scheduler row is the `jobs` workload: the crd-jobs pool, configured by the host, with one site per job.
- "a CEIR corpus replay" became `offline`: the fuzz corpora are mostly refused inputs, so replaying them measures the
  rejection path; the offline workload cooks, loads, compiles and runs an authored program instead, the path ceridc and
  the cook take.
- the disabled row's zero-overhead gate and the hot-recording row's KIR eval and asset cook are not harness workloads;
  the site microbenchmark (`events`) reports the per-site cost of each mode instead.
- mixed I/O and authored rendering are not measured yet (the row's remaining work).

## Findings

The boards show two budgets violated and three unresolved; all are open on DIAG.11b, none is accepted:

1. **The runtime-disabled mode costs +37.7% on allocation-heavy frames** (Windows, interval +36.8% .. +38.9%).
   `crd/memory/memory_stats.hpp` tracks every allocator's statistics (five atomic read-modify-writes per allocation,
   two per free) in any build with `CRD_ENABLE_PROFILING`, whether or not anything reads them. Shown by experiment:
   rebuilt with the tracking condition reduced to `CRD_DEBUG`, the same arm measured -0.09% (-0.84% .. +0.36%), within;
   restored, it measured +36.1% again. Searched first: outside crd-perf only `memory_stats.hpp`, `tlsf_allocator.cpp`
   and the perf-gpu-bridge reference the profiling switch, and CMake changes no compile flag with it.
2. **The basic mode costs +3.96% to +5.70% on allocation-heavy frames** (all three tuples). The 16 filtered sites of a
   frame cost about 0.05 µs of the 4.1 to 6.5 µs; the other per-operation instrument is the allocator watch, a
   forwarding virtual call on each of the frame's 8192 operations. Its attribution by experiment is open.
3. **The offline disabled residual, +4.6%** after the statistics experiment (+6.6% with them), is not attributed. Next
   discriminating test: per-phase timing (cook, load, compile, run) in both binaries.
4. **Jobs is unresolved on every tuple**: its p95 and p99 are two to five times its median on this host. The measurand
   is the main thread's wall time per frame; the budget is CPU per frame, and an eight-worker pool with idle gaps is
   where the two differ. A process CPU-time measurand is the next step.
5. **The Linux disabled arm is not measured**: no Linux preset differs from a profiling build only in profiling.

Reported, no numeric budget: full mode costs +4.7% to +8.4% on alloc, +30% to +78% on jobs and 4 to 5.4 times on
offline; a kept event costs 75 to 80 ns at a site. Two mechanisms stand out and are open findings against the modes
contract's "hot recording ... no blocking or global locking", owned by DIAG.11b and then the DIAG.12 audit:
`FlightRecorder::record` takes the recorder-wide ring mutex for every kept event (the jobs workload's eight workers
contend on it), and the CEIR profiling bridge times every dispatched instruction. A design observation: `admit()`
counts a filtered event with one shared atomic increment (3.2 ns on one thread); 2048 of them per jobs frame land on one
cache line from eight workers.

Storage is within everywhere: the ring is 152 KiB and the basic mode's resident growth over off at most 0.60 MiB.

## Verification

- Teeth: the macro made to evaluate its message eagerly fails the side-effect case (`none.message == 0` read 2, and
  every disabled count after it) and the harness's off and basic checks ("a payload was evaluated"); the filter moved
  back under the ring mutex fails the no-wait case (`filtered_returned`); a watchdog window given to the off mode's
  pool fails the jobs-off thread check. Each was restored byte for byte (copy and touch), the lane rebuilt and rerun
  green. The driver's within rule changed to read the interval's lower end fails two verdict tests.
- Windows, `crd-perf-tests` whole: win-debug 289 cases, win-shipping-profile 285, win-shipping 152, win-clang-cl-shipping
  152 (the no-wait case runs only where asserts compile its seam); win-asan the `[diag11b]`, `[diag9b]` and `[diag9c]`
  cases (60). The 13 `crd-diag-overhead*` CTests pass on all five lanes (win-asan under the vcvars environment).
- WSL: `linux-gcc-debug` `crd-perf-tests` whole (287 cases), `linux-gcc-asan` and `linux-clang-tsan` the flight cases
  (59) with `[diag11b]` repeated 3 and 10 times, no sanitizer report; the 13 overhead CTests pass on linux-gcc-debug,
  linux-gcc-asan, linux-clang-tsan, linux-gcc-shipping and linux-gcc-relwithdebinfo.
- Strict tidy (LLVM 20.1.8) clean on the six changed C++ files; the Allman check, the `crd-no-*`/`crd-check*` guards,
  `test-repository-tools.py`, `check-ci-tiers.py`, `check-pins.py` and the pinned actionlint pass. clang-format: the
  edited engine files keep their earlier dry-run counts (109 and 210, hand alignment); the new files differ from it
  only in hand alignment and include grouping, as their neighbours do.
- Seen in passing, not from this batch: the hosted linux-gcc-shipping lane (run 38640651640) prints GCC LTO
  `-Wstringop-overflow` warnings at `array.hpp:276`, `diag_panel.cpp:323` and `test_diag_host.cpp:888`; the local build
  shows the last one too.

## Hosted evidence this batch needs

Every lane builds `crd-diag-overhead` and passes the 12 `crd-diag-overhead-*` CTests with nonzero execution, and the
repository job runs "Overhead board verdicts". The workflow edit makes the next push resolve to the complete tier.
