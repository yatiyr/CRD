# 2026-10-26 — DIAG.11b (q1): the flight recorder's per-thread rings under one global sequence (C2)

<!-- doc-role: evidence -->
> Dated evidence; counts and results are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).
> Session: [2026-10-26 C2](../sessions/2026-10-26-flight-recorder-c2.md); decision brief:
> [per-thread rings](../sessions/2026-10-26-flight-recorder-per-thread-rings-decision.md); owner row:
> [DIAG.11b](../ROADMAP.md#slice-diag.11b), follow-up (q1). Earlier boards: [Tracy peer](2026-10-26-diag-11b-tracy-peer.md),
> [tick clock hand-off](2026-10-26-diag-11b-tick-clock-hand-off.md).

- **Machine:** Intel i9-14900K (8 performance cores, 32 logical CPUs), 63.7 GiB, Windows 11 10.0.26200 (BIOS 2202,
  microcode 0x133); WSL2 on the same host for Linux. `scripts/build-bench.py host` read the host idle before every
  board (39.8 to 43.8 GiB free, other processes at most 0.4 cores).
- **Binaries:** win-shipping-profile (MSVC 19.51, `/O2 /GL`, profiling compiled in) with `-DCRD_DIAG_PEER_TRACY=ON`.
  *Before* is HEAD `41dd04b5` (the global ring, option A), built before any edit: `crd-diag-overhead` `5e1ae565…`,
  `crd-diag-overhead-tracy` `f86098d5…`. *After* is C2 at `f7083103` plus the harness's storage report (the only
  harness change; it reads `ring_stats()` instead of `capacity × 152`): `crd-diag-overhead` `08134774…`,
  `crd-diag-overhead-tracy` `f0c19672…`. Linux: linux-gcc-shipping (GCC 13.3, LTO) and its profiling-on scratch tree.
- **Method:** `scripts/diag-overhead.py run`, 200 timed frames after 20 warm-up, four jobs threads (the main thread
  included), affinity `5555`, interleaved processes, the after binary's arms and the before binary's (`--before-exe`)
  in one board, bootstrap 95% intervals; Tracy 0.14.1 `tracy-capture` on localhost for `tracy-connected` (every
  connected process's capture matched). The disabled budget: `diag-overhead.py instructions` (cachegrind, the
  difference of 100 and 300 frames, two repeats).

## The jobs frame against Tracy connected (the brief's board)

The brief's prototype numbers are A +7.63% (+0.50 .. +9.76) and C2 +1.71% (−2.79 .. +3.51) over Tracy connected
(record path only, 10 processes).

| Board | C2 full vs Tracy connected: frame | CPU | C2 full vs A full (before): frame | CPU |
|---|---|---|---|---|
| 1, 10 processes | +1.25% (−1.34 .. +3.24) | −4.35% (−6.02 .. −2.87) | −3.37% (−5.75 .. −0.41) | −3.99% (−5.72 .. −1.10) |
| 2, 10 processes | −1.38% (−3.26 .. −0.28) | −5.83% (−6.23 .. −2.66) | −8.74% (−10.62 .. −5.07) | −6.24% (−9.26 .. −2.97) |
| 3, 30 processes | **−4.20% (−5.12 .. −3.28)** | **−7.82% (−8.63 .. −6.55)** | **−5.79% (−7.31 .. −5.46)** | −5.31% (−7.24 .. −5.00) |

Board 3 frames (p50, µs): off 203.2, C2 full 290.6, A full 310.3, Tracy connected 302.9. The full implementation
reads at or under the prototype: the record path does slightly more (a ring mark before and after the claim, the
binding check) but the capture and the window closing are off the frame. On the 30-process board C2's kept event per
job costs less frame and less CPU than Tracy's message; the 10-process boards spread by 2.6 points between them.

## The modes against HEAD (frame time and CPU per frame; 10 processes, jobs basic also 96)

| Workload | basic vs off (budget 2%) | full vs off | off vs before-off | basic vs before-basic | full vs before-full |
|---|---|---|---|---|---|
| jobs, 10 processes | +0.06% (−3.46 .. +1.91), CPU +1.04% (−1.48 .. +2.29): unresolved | +40.28%, CPU +42.08% | +0.88% (−2.64 .. +2.96) | +0.33% (−1.23 .. +1.84) | **−5.33% (−7.07 .. −4.54)**, CPU −5.14% |
| jobs, 96 processes | **−0.63% (−1.56 .. −0.02), CPU +0.05% (−0.79 .. +0.71): within** | | +1.76% (+0.32 .. +2.59) | +0.77% (−0.45 .. +1.89) | |
| alloc | **−0.11% (−0.78 .. +0.70), CPU −0.06%: within** | +2.18% | +0.30% | +0.30% | +0.42% (−0.06 .. +1.10) |
| offline | **+0.63% (−0.58 .. +1.34), CPU −0.15%: within** | +233.33% | −0.66% | −0.79% | +0.65% (−0.69 .. +1.23) |

- Basic is within 2% on every workload (jobs at 96 processes, as the earlier jobs basic boards; at 10 the host's noise
  is wider than the budget). Basic records no jobs event, so C2 does not touch its path; the before/after rows of off
  and basic are the two binaries' layout, which the disabled budget is not judged on.
- Full: the four-thread jobs frame is 5.3% faster than the global ring; alloc and offline (one recording thread)
  are unchanged within their intervals.
- Storage (basic): the recorder's storage is 9,880 KiB (64 thread rings of 1024 events and the shared ring of
  1024, 152 bytes each; it was one 152 KiB ring); the resident growth over off is 0.47 to 0.56 MiB, unchanged,
  because only the rings a thread writes are touched. Within the 32 MiB storage budget.

## The disabled budget on instructions (linux-gcc-shipping, off against compiled out)

| Workload | Compiled out per frame | Off per frame | Off vs compiled out (1%) |
|---|---:|---:|---|
| offline | 10,158,693 .. 10,158,856 | 10,165,289 .. 10,165,316 | +0.06% .. +0.07%: within |
| jobs, four threads | 1,252,896 .. 1,254,890 | 1,253,983 .. 1,256,025 | −0.07% .. +0.25%: within |
| jobs, one thread | 1,050,653 .. 1,051,205 | 1,050,717 .. 1,050,852 | −0.05% .. +0.02%: within |

Off runs no recorder, so C2 cannot change it; the counts confirm it.

## What these boards do not show

- Every Windows number is still inflated by follow-up (r) (crd-perf's `std::atomic` operations left out of line by
  MSVC LTCG under the harness's AVX2 units); the comparisons here are within one build of one source each.
- No Linux timing board was taken for C2; the Linux lanes ran the recorder's tests (the session).
