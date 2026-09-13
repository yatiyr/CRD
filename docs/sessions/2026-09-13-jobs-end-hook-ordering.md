# Jobs end-hook ordering: on_job_end before the counter release

<!-- doc-role: historical -->
> Dated evidence. Live owner: [REPO.3c](../ROADMAP.md#slice-repo.3c). Rules: [AGENTS](../../AGENTS.md).
> Same batch: [portable strict analysis](2026-09-13-portable-strict-analysis.md); register: [third-party defects](../third-party-defects.md).

## Hosted failure

[Run 34766787633](https://github.com/yatiyr/CRD/actions/runs/34766787633) at `a0419cf` (the register-gate revision)
failed `linux-gcc-debug` on one test of 6,508: `parallel_for captures one sample per job`
(`tests/foundation/perf/test_jobs_adapter.cpp:111`), `CHECK(stats.jobs_ended == num_jobs)` with `7 == 8`, while the
other four assertions passed, including `count_job_samples() == 8` evaluated a few instructions later. Debug-SSE2,
Release and both repository jobs passed on the same run; the same test passed on every Linux lane at `ae44264`.

## Root cause

Not the May 2026 tally race: the adapter's counters have been atomic since that repair, and the discriminator is the
same one recorded then (all eight samples present, one tally short), read the other way round. The job trampoline in
[worker_pool.cpp](../../engine/foundation/jobs/src/worker_pool.cpp) decremented the job's counter, which wakes waiters,
and switched back to the scheduler stack before `run_job_in_fiber` invoked the observer's `on_job_end`. A waiter
released by the eighth decrement could therefore read `jobs_adapter_stats()` while the eighth `on_job_end` was still
pending on the scheduler stack; by the time `count_job_samples()` ran it had completed. The window is one fiber switch
wide, which is why it surfaced once on a hosted Debug runner and not locally. Every observer that records per-job data
(crd-perf's Sample and tallies) had the same gap: `wait` returning did not mean the job's observation was complete.

## Repair

`on_job_end` now runs inside the trampoline after the callable returns and before `counter_decrement`, on the OS
thread that finished the callable and on the fiber's stack (64 KiB smallest tier); `run_job_in_fiber` no longer calls
it. Because the callable may have migrated the fiber to another OS thread, the thread index is read through
`tl_thread_index()`, which joins the `CRD_JOBS_TLS_OPAQUE` accessors (the optimizer may otherwise reuse the
pre-migration TLS base, the documented GCC `-O3` hazard in that file). [observer.hpp](../../engine/foundation/jobs/include/crd/jobs/observer.hpp)
states the ordering guarantee and the one semantic change it brings: the trampoline re-reads the current observer, so
an observer swapped while a job is in flight receives that job's end without a begin (crd-perf counts it as a missing
token and drains parked tokens on uninstall). No test changed: the assertion the hosted lane failed is the contract.

## Verification

- Strict gate: `worker_pool.cpp` (its database entry) and `observer.hpp` (an owning unit) clean under LLVM 20.1.8.
- Windows scoped check, `dev.py check --path <both files> --target crd-jobs-tests --target crd-perf-tests --jobs 2`
  (`20260913T194321-43739b32d5ca`): **passed**, 206 of 206 CTests of both executables selected, reported, executed and
  passed, zero skipped or disabled, both repository guards, tidy phase with both files clean; `diagnostic subset
  passed`. The three adapter cases then ran `--repeat until-fail:50` on the same build: all passed.
- Linux (WSL Ubuntu 24.04, `build/linux-gcc-debug`): `crd-perf-tests` rebuilt with the change; the three adapter cases
  `--repeat until-fail:50` all passed; the whole executable **86 test cases, 285 assertions, all passed**.
- Not measured: a local reproduction of the old order under repetition (the window is one fiber switch); the
  mechanism rests on the code order and the hosted discriminator (samples 8, tally 7).

## State

REPO.3c (Needs CI) owns this repair: its hosted wait now includes the next push, which carries the repair together with
the register gate. REPO.3c.10 is unaffected; the `win-asan` result of run 34766787633 was still pending at the time of
writing. No commit or push by the agent.
