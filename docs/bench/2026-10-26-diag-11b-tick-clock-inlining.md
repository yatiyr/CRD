# 2026-10-26 — DIAG.11b follow-up (q): why the cycle-counter clock was slower on the four-thread jobs frame

<!-- doc-role: evidence -->
> Dated evidence; counts and results are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).
> Owner row: [DIAG.11b](../ROADMAP.md#slice-diag.11b), follow-up (q); session:
> [2026-10-26](../sessions/2026-10-26-diag-11b-tick-clock-inlining.md); the board it explains:
> [Tracy peer](2026-10-26-diag-11b-tracy-peer.md) ("+2.9% from the cycle-counter clock, unexplained").

- **Machine:** Intel i9-14900K (8 performance cores, 32 logical CPUs), 63.7 GiB, Windows 11 10.0.26200 (Hyper-V
  present: `QueryPerformanceCounter` reads `rdtscp` and the hypervisor's reference page, `SharedUserData` QPC bypass
  flags 0x83); WSL2 (kernel 6.6.87.2) on the same host. Every board ran after `scripts/build-bench.py host` read the
  host idle.
- **Builds:** win-shipping-profile (MSVC 19.51, `/O2 /GL`, LTCG link), `CRD_DIAG_PEER_TRACY=OFF`; linux-gcc-shipping
  (GCC 13.3, LTO) through `scripts/wsl-lane.sh`.
- **Harness:** `crd-diag-overhead`, the jobs workload (2,048 jobs a frame on a four-thread pool, the main thread
  included), full mode, affinity `5555` on Windows (one logical processor on each of eight performance cores), 200
  timed frames after 20 warm-up, interleaved processes, paired median ratios with bootstrap 95% intervals. Every
  comparison below is two arms of **one binary**: the experiments switch by environment (an experiment-only patch,
  never committed), the final boards by the harness's new `--clock system`. Experiment binaries moved the same arm by
  up to 5% between rebuilds (tick, sites off: 245.5 to 258.4 µs), so no number is compared across binaries.

## The slowdown reproduced, with a production-identical arm

The switch: `counter_invariant()` returning false under an environment variable, so TickClock takes the path of a CPU
whose counter may not be mapped and every read is MonotonicClock's (`steady_clock`). The tick arm runs the
production code unchanged (binary `208cd07d`, 10 processes an arm). The frames were timed by the clock under test
(`diagnostic_now_ns`); the system arm's 100 ns resolution is 0.03% of a frame.

| Arm | Frame µs | Over tick | CPU over tick |
|---|---:|---:|---:|
| tick, full (production) | 340.8 | | |
| system clock, full | 329.7 | −2.90% (−4.35 .. −1.61) | −2.95% (−4.03 .. −1.50) |
| tick, full, sites off | 251.2 | | |
| system clock, full, sites off | 238.6 | −5.0% | |
| off, tick / off, system clock | 210.3 / 209.6 | (no clock read) | |

By thread count (6 processes an arm): one thread, the system clock is +4.16% (+2.58 .. +5.17) slower with sites off
and +5.1% with them (the cheaper read wins); two threads, within noise; four threads, slower as above.

## Where the time went

**Decomposition** (sites off, 12 processes an arm, one binary): the mapped read replaced by a constant, by `rdtsc`
alone, and by its loads and arithmetic without `rdtsc`.

| Read | Frame µs | Over no read |
|---|---:|---:|
| none (a constant) | 225.2 | |
| `rdtsc` alone | 241.7 | +16.5 µs |
| the map's loads and arithmetic, no `rdtsc` | 243.6 | +18.4 µs |
| TickClock (production) | 257.2 | +32.0 µs |
| system clock (`steady_clock`) | 248.3 | +23.1 µs |

The map's loads and arithmetic — seven loads of one read-only cache line and a multiply — cost the frame as much as
the counter read. **cdb on the shipping binary** (`uf crd::time::TickClock::now_ns`) showed why: the mapped path made
**six out-of-line calls** — `std::_Atomic_storage<unsigned int,4>::load` for the state, the sequence and its re-read,
and three calls to `std::basic_string_view<char>::data`, which `/OPT:ICF` had folded with the identical
`std::_Atomic_storage<unsigned __int64,8>::load` (`mov rax,[rcx]; ret`) — inside a function that saved six registers
for the calibration code it never ran. The same out-of-line atomic loads appear in `pop_region`,
`StripedCount::add`, the jobs adapter's job callback and `sum_slice`. At four threads each job's path is bounded by
locked operations on lines other cores write (the queue, the counter, the token table), so a read's latency, not its
throughput, reaches the frame; a four-thread loop of reads overlaps them and finds TickClock 5 ns cheaper.

**The mechanism**, reproduced in a two-file probe (`build/q2/inl`, MSVC 19.51): MSVC's link-time code generation keeps
one body of each inline function for the program, and a body compiled with `/arch:AVX2` is not inlined into a function
compiled for the baseline ISA. `f()` (baseline, `/GL`) reads two `std::atomic`s; another unit compiled
`/O2 /GL /arch:AVX2` reads them too. Linked with the AVX2 object first, `f()` calls both loads; linked with it last,
both are inlined. Without `/GL`, `clocks.cpp` alone inlines them. In the harness the executable's own units — every
consumer of crd-math, which carries crd-simd-flags' `/arch:AVX2` — come first in the link, while crd-time and
crd-perf (no crd-math) are baseline.

**The remedy proved before it was written** (an experiment build with crd-time and crd-perf at `/arch:AVX2`: the
loads inline, 12 processes): sites off, tick 225.6 against system clock 227.8 µs (system +1.70% (−0.69 .. +3.56));
full, 311.2 against 317.6 µs (+2.1%, the medians).

**Ruled out, with the earlier session's three:** frequency (each job's 512-iteration loop, timed by the TSC inside the
job, takes 330 TSC ticks in every arm, p10 328, p90 332); the serialising `rdtscp` the QPC path uses (an `lfence`
before or after the TickClock `rdtsc`: +1.97% and +1.14%, not faster). **Discarded measurements:** an injected delay
arm and a "TickClock plus a discarded `steady_clock` read" arm stored into one shared volatile word every read, whose
line contention made them +9% to +15%; VTune's user-mode sampling put the difference in the job's own loop, which the
TSC timing above disproves.

## The fix and its boards

`TickClock::now_ns` reads its map through functions of its own unit: with MSVC on x64 the compiler's volatile loads
(`__iso_volatile_load32/64`, what `std::atomic`'s acquire and relaxed loads are there) and a compiler barrier; with
GCC and Clang `std::atomic`'s own loads. The calibration path (`unmapped_now`) is out of line like the re-anchor.
cdb of the fixed binary: the mapped read is about 30 instructions with no call but the re-anchor's, the sequence re-read
after `rdtsc`. `TickClock::prefer_system()` and the harness's `--clock system` make the system clock a measured arm
of the same binary; `scripts/diag-overhead.py`'s `full-system-clock` arm and its `tick-clock` comparison judge
TickClock at a budget of 0% on a frame of work.

`scripts/diag-overhead.py run --workloads jobs,events,zones --arms off,full,full-system-clock --processes 10`
(fixed binary `ec9fe18a`; teeth binary `7a72c956`, the read put back on `std::atomic`'s loads and the calibration back
in line):

| Binary | Jobs: full vs full-system-clock, frame | CPU | Verdict (0%) | Site, ns | Zone, ns |
|---|---:|---:|---|---:|---:|
| fixed | **−2.22% (−4.35 .. −1.29)** | −2.47% (−3.91 .. −1.43) | **within** | 69.9 (−6.2 vs system) | 16.5 (−12.7) |
| teeth (`std::atomic` loads) | +4.16% (+2.86 .. +5.89) | +3.77% (+2.63 .. +5.33) | over | 73.7 (−3.2) | 19.6 (−10.6) |

Jobs frames: fixed 315.2 µs against 323.9 on the system clock; teeth 336.9 against 324.0. The site and zone costs are
the full mode over off per event (4,096 sites, 2,048 zones a frame) in the plain harness; the Tracy peer rows
(zone 18.6 ns, message 71.4 ns connected) were measured in the peer binary before this fix and are not re-measured
here. Two earlier boards of the fixed source (12 processes, the board script of the experiments): the system clock
+2.90% (+1.38 .. +3.94) and, rerun on the same binary, +2.16% (+0.02 .. +4.08) over TickClock; at one thread +5.57%.

## Linux (linux-gcc-shipping)

GCC's link-time optimisation inlined `std::atomic`'s loads before the fix (objdump of the mirror's build at
`82ed7559`: the mapped read is a leaf with no call but the re-anchor's) and after it. The profiler is compiled out
here, so the jobs frame's clock reads are the flight-recorder sites'.

| Board | Jobs: full vs full-system-clock | CPU | Verdict (0%) | Site |
|---|---:|---:|---|---:|
| 10 processes | −1.64% (−2.92 .. +0.64) | −1.16% (−2.90 .. +0.37) | unresolved | −6.7 ns (−7.2 .. −6.1) |
| 24 processes | **−1.22% (−2.14 .. −0.04)** | −1.50% (−1.98 .. −0.43) | **within** | |

## Verdict

The 2.9% was real, not an artifact: MSVC LTCG left `std::atomic`'s loads out of line in TickClock's mapped read (and
across crd-perf's hot paths) because the harness's `/arch:AVX2` units supplied those inline bodies to baseline-ISA
libraries, and the four-thread jobs frame exposes the read's latency. With the read on the compiler's own loads,
TickClock is 2.2% cheaper than the system clock on that frame on Windows and 1.2% on Linux, and the board judges it.
The engine-wide defect — every baseline-ISA library linked after an AVX2 unit under MSVC LTCG — is owned as DIAG.11b
follow-up (r), a design decision.
