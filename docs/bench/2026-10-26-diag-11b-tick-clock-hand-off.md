# 2026-10-26 — DIAG.11b clause (8): TickClock's per-event cost after the hand-off fix

<!-- doc-role: evidence -->
> Dated evidence; counts and results are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).
> Owner row: [DIAG.11b](../ROADMAP.md#slice-diag.11b), clause (8) and follow-up (q); session:
> [2026-10-26](../sessions/2026-10-26-tick-clock-cross-thread.md); the board it re-measures:
> [tick clock inlining](2026-10-26-diag-11b-tick-clock-inlining.md).

- **Machine:** Intel i9-14900K (32 logical CPUs), 63.7 GiB, Windows 11 10.0.26200; `scripts/build-bench.py host`
  read the host idle (others 1.1 cores) before the run.
- **Build:** win-shipping-profile (MSVC 19.51, `/O2 /GL`, LTCG link), the working tree of the fix on `2463dc52`.
  cdb (`uf crd::time::TickClock::now_ns`) of this binary: the mapped read is the same 30-odd instructions with no call
  but the re-anchor's and, new, `wait_for_reanchor` on the odd-sequence branch, which a read takes only while a
  re-anchor writes.
- **Command:** `scripts/diag-overhead.py run --exe build/win-shipping-profile/tests/diag-overhead/crd-diag-overhead.exe
  --workloads jobs,events,zones --arms off,full,full-system-clock --processes 10 --threads 4 --affinity 5555`
  (interleaved processes, 200 timed frames after 20 warm-up, paired median ratios with bootstrap 95% intervals).

| Comparison | Frame | CPU | Verdict (0%) | Per event |
|---|---:|---:|---|---:|
| jobs: full vs full-system-clock | **−2.49% (−4.63 .. −1.76)** | −3.01% (−4.61 .. −1.18) | **within** | |
| events: full vs full-system-clock | −8.91% (−9.54 .. −8.51) | −8.91% | reported | −6.9 ns (−7.4 .. −6.5) |
| zones: full vs full-system-clock | −44.24% (−44.33 .. −44.03) | −43.47% | reported | −13.4 ns (−13.5 .. −13.3) |
| events: full vs off | | | reported | +70.2 ns (+69.9 .. +70.6) |
| zones: full vs off | | | reported | +16.5 ns (+16.5 .. +16.5) |

Jobs frames: full 312.1 µs, full-system-clock 321.7 µs, off 212.0 µs (p50).

## Verdict

The fix touched only the reads before the first map and the branch a read takes while a re-anchor writes; the mapped
read is unchanged and the board says so: the jobs frame −2.49% against the system clock (the inlining board's
−2.22% (−4.35 .. −1.29)), a kept site 70.2 ns over off (69.9 before) and 6.9 ns cheaper than on the system clock, a
zone 16.5 ns (16.5 before) and 13.4 ns cheaper.
