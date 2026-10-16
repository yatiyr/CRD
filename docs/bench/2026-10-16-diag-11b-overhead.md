# DIAG.11b — diagnostic mode overhead: off, basic and full against the frozen budgets

<!-- doc-role: evidence -->
> Dated evidence; counts, results and Next paragraphs are historical. Current work: [ROADMAP](../ROADMAP.md#slice-diag.11b); current rules: [AGENTS](../../AGENTS.md).

**Date** 2026-10-16 · **Host** i9-14900K (8 P-cores + 16 E-cores, 32 logical processors), 64 GiB, Windows 11 Pro
10.0.26200, power scheme High performance; WSL2 kernel 6.6.87.2 on the same machine · **Compilers** MSVC 19.51.36246
(VS 18), GCC 13.3.0 · **Harness** `tests/support/diag-overhead` (`crd-diag-overhead`), driver
[`scripts/diag-overhead.py`](../../scripts/diag-overhead.py) · **Session**
[2026-10-16 DIAG.11b overhead harness](../sessions/2026-10-16-diag-11b-overhead-harness.md)

## What is measured

The budgets are ADR-0133 ID-5's ([decision](../decisions/0133-runtime-diagnostics-and-instrumentation.md#id-5-2026-09-14-diag0--numeric-budgets-bound-to-named-acceptance-workloads)):
the runtime-disabled mode no statistically resolved regression above 1% against profiling compiled out; the basic
recording mode at most 2% per frame and 32 MiB of process storage; full profiling has no numeric budget and its cost is
reported. Each arm is a binary and a mode, one process per run:

| Arm | Binary | What runs |
|---|---|---|
| compiled-out | built with profiling compiled out (`win-shipping`) | nothing: the true zero the disabled mode is held to |
| off | the measured binary | no diagnostic host, no flight recorder, profiler not started (the runtime-disabled mode) |
| basic | the measured binary | `DiagnosticHost` with the default profile and `assets/diag/overhead_basic.flight.toml` (Warning and above kept): crash handler, flight recorder and triggers, the jobs watchdog window, the allocator watch, one host frame per frame |
| full | the measured binary | the same host with `overhead_full.flight.toml` (every event kept), the profiler started, the jobs adapter installed, the CEIR plan compiled and run through the profiling bridge, the profiler's rings drained each frame |

The workloads are production paths driven as an application drives them; every frame produces a known answer:

| Workload | Frame | Application sites | ID-5 row |
|---|---|---|---|
| alloc | 4096 allocate/free operations on a TLSF heap over a static arena, 16..2048 bytes, 64 live; the owner gets `host.watch_allocator()` in basic and full | one per 256 operations | none (the allocation-heavy workload of the [DIAG.11b contract](../design/runtime-diagnostics.md#diag-11b)) |
| jobs | 2048 jobs of 512 squares each on an 8-thread crd-jobs pool | one per job, from its worker | basic: the jobs scheduler |
| offline | cook (parse, verify, serialize), load, compile and run the authored `assets/diag/overhead_offline.ceir` (`main(20000)`) | one per run | basic: a CEIR corpus replay (see the deviation below) |
| events | 4096 sites on one thread, nothing else | 4096 | none: a site microbenchmark, reported per site, never judged against a frame budget |

Every site is `CRD_FLIGHT_RECORD` with an `snprintf` payload. A frame is timed on the main thread from the start of the
work to the end of the host's frame call (and, in full, the profiler's frame mark and ring drain). After the frames
each process requests three manual captures and waits for each to be delivered and written; the bytes are counted and
the bundles removed.

**Method.** 16 processes per arm, 200 timed frames after 20 warmup frames each, every process pinned to logical
processors 0-15 (`--affinity FFFF`: the P-cores on Windows; on WSL2 vCPUs 0-15, whose mapping to host cores is not
known). The arms are interleaved: repetition *r* runs every arm, in an order rotated by *r*. Percentiles are nearest
rank over all 3,200 timed frames of an arm. A cost is the median over the 16 repetitions of the ratio of the arm's
median frame to the reference's median frame in the same repetition, with a percentile-bootstrap 95% interval (2,000
resamples, seed 0x11B). Verdict: *within* when the interval's upper end is at or below the budget, *over* when its lower
end is above it, *unresolved* otherwise. Unresolved is a measured loss, never a pass. Storage: the recorder's ring and
the median resident-set growth over the off arm, against 32 MiB.

Commands (Windows; the Linux boards ran the same driver in WSL2 with `--arms off,basic,full`):

```
python scripts/diag-overhead.py run --exe build/win-shipping-profile/tests/diag-overhead/crd-diag-overhead.exe \
  --baseline-exe build/win-shipping/tests/diag-overhead/crd-diag-overhead.exe --processes 16 --iterations 200 \
  --warmup 20 --captures 3 --threads 8 --affinity FFFF --scratch build/diag11b-overhead/scratch-win \
  --label "..." --output build/diag11b-overhead/win-shipping-profile.json
```

Binaries as measured (SHA-256 prefix): `win-shipping-profile` 26e15a9b7034, `win-shipping` 0d36a39ecda0,
`linux-gcc-shipping` 8b7003504a52, `linux-gcc-relwithdebinfo` dc6ea2d2a223. The Windows profile binary was rebuilt
afterwards for the attribution experiment and rebuilt again from the restored source; a 4-process check of the restored
binary reproduced the disabled cost on alloc (+36.10%, interval +34.40% .. +40.35%).

## Windows: win-shipping-profile against win-shipping

Windows 11 (AMD64), 32 logical processors; 16 processes per arm, 200 timed frames after 20 warmup each; affinity FFFF.

| Workload | Arm | p50 µs | p95 µs | p99 µs | p50 ns/site | Cost vs reference (95% interval) | Verdict | Payloads / recorded / filtered | Captures (bytes) | Resident growth |
|---|---|---:|---:|---:|---:|---|---|---|---|---:|
| alloc | compiled-out | 82.9 | 100.1 | 131.7 | 5181.2 | reference |  | 0 / 0 / 0 | 0 of 0 (0) | 0.1 MiB |
| alloc | off | 114.1 | 139.8 | 178.8 | 7131.2 | +37.71% vs compiled-out (+36.80% .. +38.88%) | over (1%) | 0 / 0 / 0 | 0 of 0 (0) | 0.1 MiB |
| alloc | basic | 120.6 | 140.8 | 164.4 | 7537.5 | +5.70% vs off (+5.28% .. +6.27%) | over (2%) | 0 / 0 / 59840 | 48 of 48 (112080) | 0.6 MiB |
| alloc | full | 122.8 | 150.3 | 192.8 | 7675.0 | +7.54% vs off (+6.75% .. +8.20%) | reported | 56320 / 59840 / 0 | 48 of 48 (2457600) | 1.7 MiB |
| alloc | basic storage | | | | | ring 152 KiB, resident growth over off 0.51 MiB | within (32 MiB) | | | |
| jobs | compiled-out | 397.8 | 816.2 | 2056.5 | 194.2 | reference |  | 0 / 0 / 0 | 0 of 0 (0) | 3.6 MiB |
| jobs | off | 409.1 | 677.4 | 1586.1 | 199.8 | +0.66% vs compiled-out (-3.64% .. +9.72%) | unresolved (1%) | 0 / 0 / 0 | 0 of 0 (0) | 3.6 MiB |
| jobs | basic | 403.6 | 481.9 | 733.4 | 197.1 | +1.83% vs off (-8.33% .. +3.53%) | unresolved (2%) | 0 / 0 / 7212480 | 48 of 48 (112080) | 4.1 MiB |
| jobs | full | 734.4 | 854.0 | 1231.1 | 358.6 | +77.52% vs off (+71.66% .. +85.66%) | reported | 7208960 / 7212480 / 0 | 48 of 48 (2580903) | 6.0 MiB |
| jobs | basic storage | | | | | ring 152 KiB, resident growth over off 0.49 MiB | within (32 MiB) | | | |
| offline | compiled-out | 338.8 | 400.0 | 475.9 | 338800.0 | reference |  | 0 / 0 / 0 | 0 of 0 (0) | 0.3 MiB |
| offline | off | 361.9 | 418.3 | 496.4 | 361900.0 | +6.63% vs compiled-out (+6.13% .. +7.23%) | over (1%) | 0 / 0 / 0 | 0 of 0 (0) | 0.3 MiB |
| offline | basic | 360.8 | 420.1 | 488.4 | 360800.0 | -0.24% vs off (-1.01% .. +0.49%) | within (2%) | 0 / 0 / 7040 | 48 of 48 (112080) | 0.7 MiB |
| offline | full | 2086.1 | 2226.8 | 2348.9 | 2086100.0 | +477.05% vs off (+471.98% .. +481.31%) | reported | 3520 / 7040 / 0 | 48 of 48 (1284240) | 1.8 MiB |
| offline | basic storage | | | | | ring 152 KiB, resident growth over off 0.41 MiB | within (32 MiB) | | | |
| events | compiled-out | 0.9 | 1.0 | 1.5 | 0.2 | reference |  | 0 / 0 / 0 | 0 of 0 (0) | 0.0 MiB |
| events | off | 0.9 | 1.0 | 1.3 | 0.2 | +0.00% vs compiled-out (+0.00% .. +0.00%) | reported | 0 / 0 / 0 | 0 of 0 (0) | 0.0 MiB |
| events | basic | 13.0 | 13.2 | 20.9 | 3.2 | +1350.00% vs off (+1344.44% .. +1355.56%) | reported | 0 / 0 / 14421440 | 48 of 48 (112080) | 0.5 MiB |
| events | full | 328.4 | 371.6 | 456.0 | 80.2 | +36419.44% vs off (+36244.44% .. +36500.00%) | reported | 14417920 / 14421440 / 0 | 48 of 48 (2212752) | 1.6 MiB |
| events | basic storage | | | | | ring 152 KiB, resident growth over off 0.50 MiB | reported (32 MiB) | | | |

## Linux (WSL2): linux-gcc-shipping, profiling compiled out

No compiled-out arm: this binary is compiled out, and no Linux preset differs from it only in profiling.

Linux 6.6.87.2-microsoft-standard-WSL2 (x86_64), 32 logical processors; 16 processes per arm, 200 timed frames after 20 warmup each; affinity FFFF.

| Workload | Arm | p50 µs | p95 µs | p99 µs | p50 ns/site | Cost vs reference (95% interval) | Verdict | Payloads / recorded / filtered | Captures (bytes) | Resident growth |
|---|---|---:|---:|---:|---:|---|---|---|---|---:|
| alloc | off | 77.8 | 141.9 | 201.7 | 4862.5 | reference |  | 0 / 0 / 0 | 0 of 0 (0) | 0.0 MiB |
| alloc | basic | 82.8 | 153.1 | 219.4 | 5174.7 | +5.55% vs off (+3.51% .. +7.72%) | over (2%) | 0 / 0 / 59840 | 48 of 48 (51696) | 0.6 MiB |
| alloc | full | 84.2 | 153.8 | 212.1 | 5260.8 | +8.35% vs off (+4.36% .. +9.74%) | reported | 56320 / 59840 / 0 | 48 of 48 (2397216) | 0.8 MiB |
| alloc | basic storage | | | | | ring 152 KiB, resident growth over off 0.60 MiB | within (32 MiB) | | | |
| jobs | off | 510.4 | 914.3 | 1043.3 | 249.2 | reference |  | 0 / 0 / 0 | 0 of 0 (0) | 3.5 MiB |
| jobs | basic | 528.4 | 890.4 | 1243.1 | 258.0 | +3.23% vs off (-0.30% .. +11.45%) | unresolved (2%) | 0 / 0 / 7212480 | 48 of 48 (51696) | 4.0 MiB |
| jobs | full | 666.4 | 829.6 | 986.3 | 325.4 | +30.04% vs off (+27.30% .. +33.35%) | reported | 7208960 / 7212480 / 0 | 48 of 48 (2520519) | 4.2 MiB |
| jobs | basic storage | | | | | ring 152 KiB, resident growth over off 0.50 MiB | within (32 MiB) | | | |
| offline | off | 377.7 | 680.0 | 779.6 | 377656.0 | reference |  | 0 / 0 / 0 | 0 of 0 (0) | 0.5 MiB |
| offline | basic | 385.7 | 705.0 | 834.8 | 385706.0 | +1.01% vs off (-0.40% .. +3.78%) | unresolved (2%) | 0 / 0 / 7040 | 48 of 48 (51696) | 0.8 MiB |
| offline | full | 2398.0 | 3417.4 | 3762.7 | 2398020.0 | +539.81% vs off (+527.36% .. +546.63%) | reported | 3520 / 7040 / 0 | 48 of 48 (1223856) | 1.0 MiB |
| offline | basic storage | | | | | ring 152 KiB, resident growth over off 0.25 MiB | within (32 MiB) | | | |
| events | off | 0.0 | 0.0 | 0.0 | 0.0 | reference |  | 0 / 0 / 0 | 0 of 0 (0) | 0.0 MiB |
| events | basic | 13.1 | 13.8 | 26.0 | 3.2 | +51023.00% vs off (+49833.76% .. +52151.92%) | reported | 0 / 0 / 14421440 | 48 of 48 (51696) | 0.8 MiB |
| events | full | 305.7 | 418.2 | 544.1 | 74.6 | +1191721.15% vs off (+1141510.36% .. +1212402.00%) | reported | 14417920 / 14421440 / 0 | 48 of 48 (2152368) | 0.8 MiB |
| events | basic storage | | | | | ring 152 KiB, resident growth over off 0.75 MiB | reported (32 MiB) | | | |

## Linux (WSL2): linux-gcc-relwithdebinfo, profiling and asserts compiled in

Linux 6.6.87.2-microsoft-standard-WSL2 (x86_64), 32 logical processors; 16 processes per arm, 200 timed frames after 20 warmup each; affinity FFFF.

| Workload | Arm | p50 µs | p95 µs | p99 µs | p50 ns/site | Cost vs reference (95% interval) | Verdict | Payloads / recorded / filtered | Captures (bytes) | Resident growth |
|---|---|---:|---:|---:|---:|---|---|---|---|---:|
| alloc | off | 114.8 | 188.6 | 268.8 | 7172.7 | reference |  | 0 / 0 / 0 | 0 of 0 (0) | 0.2 MiB |
| alloc | basic | 118.9 | 203.5 | 307.4 | 7431.4 | +3.96% vs off (+2.11% .. +4.46%) | over (2%) | 0 / 0 / 59840 | 48 of 48 (51600) | 0.7 MiB |
| alloc | full | 120.4 | 195.1 | 263.3 | 7523.0 | +4.71% vs off (+3.75% .. +6.19%) | reported | 56320 / 59840 / 0 | 48 of 48 (2397120) | 1.7 MiB |
| alloc | basic storage | | | | | ring 152 KiB, resident growth over off 0.46 MiB | within (32 MiB) | | | |
| jobs | off | 531.8 | 714.6 | 833.9 | 259.7 | reference |  | 0 / 0 / 0 | 0 of 0 (0) | 3.8 MiB |
| jobs | basic | 531.5 | 718.6 | 833.3 | 259.5 | +0.01% vs off (-4.51% .. +3.93%) | unresolved (2%) | 0 / 0 / 7212480 | 48 of 48 (51600) | 4.0 MiB |
| jobs | full | 793.2 | 966.2 | 1089.1 | 387.3 | +50.92% vs off (+45.45% .. +54.90%) | reported | 7208960 / 7212480 / 0 | 48 of 48 (2520432) | 5.2 MiB |
| jobs | basic storage | | | | | ring 152 KiB, resident growth over off 0.25 MiB | within (32 MiB) | | | |
| offline | off | 479.7 | 847.1 | 1023.7 | 479744.0 | reference |  | 0 / 0 / 0 | 0 of 0 (0) | 0.5 MiB |
| offline | basic | 482.7 | 865.8 | 1032.3 | 482740.0 | +0.03% vs off (-0.88% .. +0.80%) | within (2%) | 0 / 0 / 7040 | 48 of 48 (51600) | 0.8 MiB |
| offline | full | 2363.8 | 3260.8 | 3685.8 | 2363815.0 | +392.02% vs off (+381.89% .. +407.04%) | reported | 3520 / 7040 / 0 | 48 of 48 (1223760) | 1.7 MiB |
| offline | basic storage | | | | | ring 152 KiB, resident growth over off 0.25 MiB | within (32 MiB) | | | |
| events | off | 0.7 | 0.8 | 0.8 | 0.2 | reference |  | 0 / 0 / 0 | 0 of 0 (0) | 0.2 MiB |
| events | basic | 12.9 | 13.8 | 28.6 | 3.2 | +1640.35% vs off (+1637.33% .. +1644.13%) | reported | 0 / 0 / 14421440 | 48 of 48 (51600) | 0.5 MiB |
| events | full | 313.3 | 471.9 | 613.2 | 76.5 | +41919.36% vs off (+41491.16% .. +42503.71%) | reported | 14417920 / 14421440 / 0 | 48 of 48 (2152272) | 1.7 MiB |
| events | basic storage | | | | | ring 152 KiB, resident growth over off 0.25 MiB | reported (32 MiB) | | | |

## Attribution experiment: the allocator statistics

`crd/memory/memory_stats.hpp` tracks every allocator's statistics (five atomic read-modify-writes per allocation, two
per free) whenever `CRD_DEBUG` or `CRD_ENABLE_PROFILING` is set, whether or not anything reads them. Outside crd-perf
only `memory_stats.hpp`, `tlsf_allocator.cpp` and the perf-gpu-bridge reference the profiling switch. The experiment
rebuilt `win-shipping-profile` with the tracking condition reduced to `CRD_DEBUG` (binary a55d1bd25a05), measured the
two disabled arms with the same method, and restored the source:

Windows 11 (AMD64), 32 logical processors; 16 processes per arm, 200 timed frames after 20 warmup each; affinity FFFF.

| Workload | Arm | p50 µs | p95 µs | p99 µs | p50 ns/site | Cost vs reference (95% interval) | Verdict | Payloads / recorded / filtered | Captures (bytes) | Resident growth |
|---|---|---:|---:|---:|---:|---|---|---|---|---:|
| alloc | compiled-out | 82.9 | 110.9 | 217.8 | 5181.2 | reference |  | 0 / 0 / 0 | 0 of 0 (0) | 0.1 MiB |
| alloc | off | 82.6 | 107.5 | 213.9 | 5162.5 | -0.09% vs compiled-out (-0.84% .. +0.36%) | within (1%) | 0 / 0 / 0 | 0 of 0 (0) | 0.1 MiB |
| offline | compiled-out | 339.5 | 423.8 | 529.7 | 339500.0 | reference |  | 0 / 0 / 0 | 0 of 0 (0) | 0.3 MiB |
| offline | off | 356.0 | 425.0 | 515.4 | 356000.0 | +4.61% vs compiled-out (+4.10% .. +5.95%) | over (1%) | 0 / 0 / 0 | 0 of 0 (0) | 0.3 MiB |

The allocation-heavy disabled cost is the statistics: +37.71% with them, -0.09% without. The offline residual falls
from +6.63% to +4.61% and stays over; it is not attributed.

## Verdicts

| Budget | Windows (profile vs shipping) | Linux shipping | Linux relwithdebinfo |
|---|---|---|---|
| disabled ≤ 1% | **over** on alloc (+37.71%) and offline (+6.63%); jobs unresolved (+0.66%, -3.64% .. +9.72%) | not measured | not measured |
| basic ≤ 2% | **over** on alloc (+5.70%); within on offline (-0.24%); jobs unresolved (+1.83%, -8.33% .. +3.53%) | **over** on alloc (+5.55%); offline unresolved (+1.01%, -0.40% .. +3.78%); jobs unresolved (+3.23%, -0.30% .. +11.45%) | **over** on alloc (+3.96%); within on offline (+0.03%); jobs unresolved (+0.01%, -4.51% .. +3.93%) |
| basic storage ≤ 32 MiB | within everywhere: ring 152 KiB, resident growth over off ≤ 0.51 MiB | within: ≤ 0.60 MiB | within: ≤ 0.46 MiB |
| full (reported) | alloc +7.54%, jobs +77.52%, offline +477.05%; 80.2 ns per kept site | alloc +8.35%, jobs +30.04%, offline +539.81%; 74.6 ns per site | alloc +4.71%, jobs +50.92%, offline +392.02%; 76.5 ns per site |
| per site | off 0.2 ns, basic 3.2 ns | off 0.0 ns (the disabled site compiled to nothing), basic 3.2 ns | off 0.2 ns, basic 3.2 ns |

Every requested capture was delivered and written in every basic and full process (48 of 48 per arm); every frame
computed its known answer; the structural counts hold exactly (basic: no payload evaluated and every site and frame
event filtered; full: every payload evaluated once and every event recorded).

## Losses and what they belong to

All open on [DIAG.11b](../ROADMAP.md#slice-diag.11b); none is accepted.

1. **Disabled mode, allocation-heavy work: +37.7%** (Windows). The allocator statistics are tracked in every profiling
   build whether or not a reader runs; the experiment above shows it. Acceptance of the repair: the `disabled` verdict on
   alloc goes `within`, with the tests that assert `MemoryStats` on profiling lanes and the `CRD_DEBUG` semantics kept.
2. **Basic mode, allocation-heavy work: +3.96% to +5.70%** on all three tuples. Per frame the 16 filtered sites cost
   about 0.05 µs (3.2 ns each, from the events rows) of the 4.1 to 6.5 µs; the remaining per-operation instrument in the
   frame is the allocator watch (`FlightAllocatorWatch`), one forwarding virtual call on each of the 8192 operations.
   Its attribution by experiment, and a repair, are open.
3. **Disabled mode, offline: +4.6% unattributed** after the statistics. Next discriminating test: per-phase timing of
   cook, load, compile and run in both binaries.
4. **Jobs: unresolved on every tuple.** The p95 and p99 of the jobs frame are two to five times its median on this host.
   The measurand is the main thread's wall time per frame; the budget is CPU per frame, and an eight-worker pool with idle
   gaps is where they differ. A process CPU-time measurand (`QueryProcessCycleTime`, `getrusage`) is the next
   discriminating step.
5. **Linux disabled mode: not measured.** No Linux preset differs from a profiling build only in profiling (Windows has
   `win-shipping` and `win-shipping-profile`); a scratch configuration of `linux-gcc-shipping` with
   `CRD_ENABLE_PROFILING=ON`, or a `linux-gcc-shipping-profile` preset (a build-system and tier-mapping decision), is
   needed.
6. **Full mode mechanisms** (reported, no numeric budget): `FlightRecorder::record` takes the recorder-wide ring mutex
   for every kept event (75 to 80 ns per site; the jobs workload's eight workers contend on it), against the modes
   contract's "hot recording ... no blocking or global locking"; the CEIR profiling bridge's per-instruction hooks cost
   4 to 5.4 times the run. Owner: DIAG.11b, then the DIAG.12 audit.
7. `admit()` counts a filtered event with one shared atomic increment (3.2 ns single-threaded). In the jobs frame 2048
   such increments land on one cache line from eight workers; if basic must be resolved on jobs, a per-thread or sampled
   count is the candidate.

Not measured here (the row's remaining clauses): the mixed I/O and authored rendering workloads, independent external
sampling (WPR stack capture needs an elevated shell on this machine; WSL2 has no `perf`), and peer tools.
