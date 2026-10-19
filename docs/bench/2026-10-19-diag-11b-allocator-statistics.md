# DIAG.11b — allocator statistics on request: the disabled mode's allocation cost, re-measured

<!-- doc-role: evidence -->
> Dated evidence; counts, results and Next paragraphs are historical. Current work: [ROADMAP](../ROADMAP.md#slice-diag.11b); current rules: [AGENTS](../../AGENTS.md).

**Date** 2026-10-19 · **Host** i9-14900K (8 P-cores + 16 E-cores, 32 logical processors), 64 GiB, Windows 11 Pro
10.0.26200 · **Compiler** MSVC 19.51.36246 (VS 18) · **Harness** `tests/support/diag-overhead` (`crd-diag-overhead`),
driver [`scripts/diag-overhead.py`](../../scripts/diag-overhead.py) · **Session**
[2026-10-19 allocator statistics on request](../sessions/2026-10-19-diag-11b-allocator-statistics-on-request.md) ·
**Earlier board** [2026-10-16 DIAG.11b overhead](2026-10-16-diag-11b-overhead.md) (its loss 1 is what this board
re-measures)

## What changed and what is measured

The 2026-10-16 board found the runtime-disabled mode +37.7% over profiling compiled out on allocation-heavy frames,
because `crd/memory/memory_stats.hpp` updated every allocator's counters (three atomic read-modify-writes and a
peak-bytes exchange per allocation, two per free) in every profiling build. Since this change a profiling build
without `CRD_DEBUG` (win-shipping-profile) counts an allocator only once a reader requests it before its first
allocation; an unrequested operation passes the allocator's statistics gate (one byte load and two compares).

Method as on 2026-10-16: 16 processes per arm, 200 timed frames after 20 warmup each, every process pinned to logical
processors 0-15 (`--affinity FFFF`), arms interleaved and rotated per repetition, cost the median over repetitions of
the paired median-frame ratio with a seeded bootstrap 95% interval; *within* when the interval's upper end is at or
below the budget (1% for the disabled mode, 2% for basic), *over* when its lower end is above it. Command (Windows,
from the checkout):

```
python scripts/diag-overhead.py run --exe build/win-shipping-profile/tests/diag-overhead/crd-diag-overhead.exe \
  --baseline-exe build/win-shipping/tests/diag-overhead/crd-diag-overhead.exe --processes 16 --iterations 200 \
  --warmup 20 --captures 3 --threads 8 --affinity FFFF --scratch build/diag11b-overhead/<scratch> \
  --label "..." --output build/diag11b-overhead/<record>.json --progress
```

Host before and after every run (`python scripts/build-bench.py host`): 25.8 to 29.7 of 63.7 GiB available, other
processes 0.8 to 1.6 cores together, the busiest a digital audio workstation (Studio One, 0.3 to 0.4 cores) or a
browser (0.5 cores); not contended by the board rules. Binaries are named by their SHA-256 prefix; MSVC links are not
bit-reproducible, so a rebuild of the same source has another prefix.

## Run 1 — the gate behind a call (first form)

`win-shipping-profile` f4e5761ec8c9 against `win-shipping` 46019287aa97. `MemoryStats::on_allocate` was not inlined
into `TlsfAllocator::try_allocate` (cdb disassembly), so every allocation paid a call to reach the gate.

| Workload | Arm | p50 µs | p95 µs | p99 µs | Cost vs reference (95% interval) | Verdict |
|---|---|---:|---:|---:|---|---|
| alloc | compiled-out | 81.2 | 95.3 | 150.1 | reference | |
| alloc | off | 85.8 | 107.2 | 161.0 | +5.79% vs compiled-out (+5.26% .. +6.47%) | over (1%) |
| alloc | basic | 91.1 | 108.9 | 142.7 | +6.74% vs off (+5.08% .. +7.06%) | over (2%) |
| alloc | full | 92.6 | 108.9 | 142.1 | +8.17% vs off (+7.23% .. +8.74%) | reported |
| jobs | off | 395.8 | 471.0 | 786.8 | -0.26% vs compiled-out (-5.07% .. +2.60%) | unresolved (1%) |
| jobs | basic | 398.3 | 451.1 | 578.1 | +0.64% vs off (-2.10% .. +4.72%) | unresolved (2%) |
| jobs | full | 737.3 | 832.8 | 1277.6 | +87.19% vs off (+83.49% .. +89.63%) | reported |
| offline | off | 370.7 | 406.3 | 510.9 | +0.17% vs compiled-out (-0.41% .. +0.38%) | within (1%) |
| offline | basic | 371.7 | 403.0 | 434.9 | +0.26% vs off (-0.26% .. +0.47%) | within (2%) |
| offline | full | 2047.9 | 2140.3 | 2420.9 | +452.15% vs off (+449.01% .. +455.27%) | reported |

Storage within (ring 152 KiB, resident growth over off at most 0.53 MiB); every capture delivered (48 of 48 per arm);
every frame computed its known answer; the site microbenchmark unchanged (off 0.2 ns, basic 3.2 ns, full 79.3 ns per
site).

## Run 2 — the gate inlined (the committed form)

`win-shipping-profile` 8c99f3e158c3 against `win-shipping` e039f4f03d34. The gate's load and compares are inlined into
the allocator; the first-allocation exchange and the counting are out of line.

| Workload | Arm | p50 µs | p95 µs | p99 µs | Cost vs reference (95% interval) | Verdict |
|---|---|---:|---:|---:|---|---|
| alloc | compiled-out | 82.2 | 88.2 | 126.0 | reference | |
| alloc | off | 85.6 | 92.1 | 110.6 | +4.70% vs compiled-out (+3.34% .. +5.22%) | over (1%) |
| alloc | basic | 90.2 | 109.4 | 135.0 | +5.42% vs off (+4.49% .. +6.18%) | over (2%) |
| alloc | full | 91.5 | 98.7 | 138.3 | +6.85% vs off (+6.01% .. +7.60%) | reported |
| jobs | off | 400.3 | 475.7 | 615.2 | +2.16% vs compiled-out (-2.53% .. +10.08%) | unresolved (1%) |
| jobs | basic | 401.9 | 463.9 | 554.2 | -0.16% vs off (-3.33% .. +4.00%) | unresolved (2%) |
| jobs | full | 739.9 | 831.5 | 1019.7 | +82.63% vs off (+71.42% .. +90.87%) | reported |
| offline | compiled-out | 372.4 | 441.8 | 591.2 | reference | |
| offline | off | 330.3 | 412.2 | 557.0 | -11.00% vs compiled-out (-11.73% .. -10.59%) | within (1%) |
| offline | basic | 330.3 | 379.3 | 524.6 | +0.11% vs off (-0.33% .. +0.44%) | within (2%) |
| offline | full | 2040.7 | 2422.7 | 2550.9 | +518.42% vs off (+515.50% .. +522.09%) | reported |

Storage within; every capture delivered; every known answer computed; sites off 0.2 ns, basic 3.2 ns, full 79.6 ns.

Repeated with the same two binaries (alloc and offline, compiled-out and off only): alloc off +4.86% (+4.17% ..
+5.85%), offline off -10.73% (-11.20% .. -10.41%). The numbers belong to the binaries, not to the run.

## Attribution: the gate against no gate

A third binary, `nogate` 371ee81057b4: the same source with the gate's two call sites in `on_allocate` and
`on_deallocate` replaced by nothing (an experiment, restored byte for byte afterwards), so it differs from the measured
binary only by the gate.

| Pair (measured against reference) | alloc off | offline off |
|---|---|---|
| gate 8c99f3e158c3 against nogate 371ee81057b4 | +4.98% (+3.53% .. +5.76%), over | -3.15% (-3.47% .. -2.61%) |
| nogate 371ee81057b4 against win-shipping e039f4f03d34 | +0.12% (-0.25% .. +0.55%), within | -7.98% (-8.56% .. -7.47%) |

Without the gate the profiling build's allocation-heavy frame is within 1% of profiling compiled out, as the
2026-10-16 experiment found (-0.09%). With it, +4.7% to +5.0%: the gate itself, about 0.5 ns per allocator operation
on a TLSF operation of about 10 ns, several times what its three instructions cost in isolation.

## Verdicts

| Budget | Before (2026-10-16) | Now (run 2) |
|---|---|---|
| disabled ≤ 1%, alloc | over, +37.71% | **over, +4.70%** (the gate, attributed above) |
| disabled ≤ 1%, offline | over, +6.63% | within on the board (-11.00%), but see the method floor |
| disabled ≤ 1%, jobs | unresolved | unresolved (+2.16%, -2.53% .. +10.08%) |
| basic ≤ 2%, alloc | over, +5.70% | over, +5.42% (the allocator watch's forwarding call, loss 2 of the earlier board) |
| basic ≤ 2%, offline | within | within (+0.11%) |
| basic storage ≤ 32 MiB | within | within (152 KiB ring, ≤ 0.53 MiB growth) |

## Losses and what they belong to

All open on [DIAG.11b](../ROADMAP.md#slice-diag.11b); none is accepted.

1. **Disabled mode, allocation-heavy work: +4.7% to +5.0%** (was +37.7%). The remaining cost is the per-operation
   statistics gate, shown by the gate-against-nogate pair. A gate any cheaper than a load and a branch is still an
   instruction per operation; meeting 1% on a 10 ns allocator needs the counting choice made where it costs nothing
   per operation (when the allocator is constructed, by its type or a start-up setting), which changes how allocators
   are counted and is the next batch's design.
2. **The two-binary method has a floor of several percent.** Offline moved -11.0% against compiled-out between two
   rebuilds of sources whose change does not touch the offline workload's hot path, and the gate and nogate binaries
   differ by -3.2% on offline although they differ only in the allocator gate. Comparing separately compiled binaries
   measures their code layout as well as the diagnostics, so a 1% verdict read from one pair is not trustworthy either
   way. Next discriminating step: several builds of each arm with varied layout (function order, padding), or retired
   instruction and cycle counts per frame (hardware counters; Windows needs an elevated ETW session, WSL2 has no
   `perf`). This floor also covers the earlier board's unattributed offline residual (+4.6%, its loss 3).
3. Basic on allocation-heavy work (+5.4%), jobs unresolved, the Linux disabled arm not measured, the full mode's
   mechanisms: unchanged, as listed on the earlier board.
