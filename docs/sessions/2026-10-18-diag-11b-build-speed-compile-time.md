# DIAG.11b: faster local builds, task C (compile time), and the loose ends of tasks A and B, 2026-10-18

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b) (its contract covers focused local routes, CI
> cost and independent external sampling). Contract: [build performance](../design/build-performance.md). Previous
> batches: [task A](2026-10-18-diag-11b-build-speed-cache-mirror.md), [task B](2026-10-18-diag-11b-build-speed-linking.md).

## Goal

Task C of the user's build-speed batch (2026-10-18): profile a full win-debug build (C++ Build Insights where it can
run) and a full Linux clang build (`-ftime-trace` with ClangBuildAnalyzer, pinned and built from source), name the
most expensive headers, templates and translation units, and cut compile time without changing behaviour. Before it,
the loose ends the operator found: five DX12 failures of a nightly run that no note explained, the elevated WPR
capture DIAG.11b clause (8) still names, and task B's boards measured while another application loaded the machine.
Changed-only local checks; the hosted lanes run the suites (the user's process of 2026-10-18).

<a id="tp-1-sightings"></a>
## The five DX12 failures of run 39013990584 are TP-1

The win-asan job of [39013990584](https://github.com/yatiyr/CRD/actions/runs/39013990584) (2026-10-17, `d08436b9`)
failed six tests. The [scheduled-runs note](2026-10-18-scheduled-runs-snapshot-lost-broadcast.md) explained the
allocator corpus; the other five were never unexpected. Read from the job log (`gh api .../jobs/117342225690/logs`):

| Test | AddressSanitizer report |
|---|---|
| DX12 cached DXR pipelines mint one Program identity each, retired with the context | READ of size 8 at `d3d10warp.dll+0x180132995`, 0 bytes after a 2,952-byte region |
| REN-38 RT GATE (DX12): the authored pipeline traces and the ANY-HIT can IGNORE every hit | the same pc, 2,924 bytes |
| REN-38-F13 GATE (DX12): authored INTERSECTION + CALLABLE stages trace a procedural sphere through the SBT | the same pc, 2,568 bytes |
| CEIR-19b GATE: the authored hybrid RT-shadow renderer casts a ray-traced shadow (DX12) | the same pc, 2,924 bytes |
| REN-38-F6 GATE (DX12): the authored RT PIPELINE graph traces the scene TLAS through the live host | the same pc, 3,788 bytes |

Every report is a `memmove` in the OS WARP (`d3d10warp.dll`, census `driver=10.0.26100.33438`) reading past a
`D3D12Core.dll` heap block, reached from `Dx12RasterContext::dxr_pipeline` (`dx12_raster_context.cpp:3379`) through
`CreateStateObject`: [TP-1](../third-party-defects.md#tp-1), whose register already lists these five tests, these
region sizes and an SDK-only reproduction with no Cerid code. The gate printed `registered TP-1: failed` for each and
`UNEXPECTED FAILURE` only for the corpus. The next two runs ([39108972918](https://github.com/yatiyr/CRD/actions/runs/39108972918)
and [39124869209](https://github.com/yatiyr/CRD/actions/runs/39124869209)) show the same five and
`gate: PASS (exactly the registered third-party failures, sanitizer unsuppressed)`. Nothing to repair in Cerid; the
register's hosted evidence names the three runs.

## DIAG.11b clause (8): the elevated capture is not done yet; one command prepared

Task A's summary said the elevated WPR run and the h2 hardware fault run were done. Both happened, for other rows:
the user's elevated `sample-cpu-wpr.py --stacks` run of 2026-10-07 recorded DIAG.6c's unscoped hotspot specimen
([note](2026-10-07-user-items-wpr-and-vulkan-loss.md)), and h2 is DIAG.7b's hardware fault, settled by the user's
decision of the same day. DIAG.11b clause (8) asks for independent external sampling of the overhead workloads
(`crd-diag-overhead` in its modes), which no trace holds; the 2026-10-07 trace is no longer on disk either (looked for
in the real `%TEMP%` through WSL, since this shell's AppData is virtualized, and under `build/`). So the clause is
open, and it is now one command.

[sample-overhead-wpr.py](../../scripts/sample-overhead-wpr.py):
- `record` (elevated): one WPR CPU session with stacks; inside it the workloads alloc, jobs and offline each run in
  the arms compiled-out (the win-shipping build), off, basic and full (the win-shipping-profile build), twice, the
  arms rotated per repetition, every process pinned to the P-cores like the 2026-10-16 board. Frames are sized to
  about 2.5 s per process (alloc 20,000, jobs and offline 6,000) because at the board's 200 frames the frames covered
  a fifth of an alloc process (28 ms of 160 ms, measured here), and a share over the whole process would be the
  setup's. Each process's PID, wall time and own JSON record go into `manifest.json` beside the trace; `wpr -cancel`
  runs on any interruption.
- `analyze <run dir>` (no elevation) streams `xperf -a dumper -stacktimeshifting -add_inline`, counts every CPU sample
  of each recorded process and the samples whose stack holds a diagnostic frame (classes, the frame nearest the leaf
  deciding: `MemoryStats::`, `FlightAllocatorWatch`, the flight recorder, the diagnostic host, the CEIR host bridge,
  the rest of `crd::perf::`), reports CPU milliseconds per frame (the CPU-time measurand clause (4) asks for on jobs)
  and compares each arm's measured cost against off with the cost its samples explain,
  (1 - share_off) / (1 - share_arm) - 1; off against compiled-out is the clause (1) cost. Verdicts AGREES,
  DISAGREES (a finding), UNRESOLVED (under 500 samples, or frames under 85 % of the process), UNREADABLE (no sample
  for a recorded process, never read as zero), PERMISSION_DENIED, TOOL_MISSING.
- The dumper's columns are found by name from its header lines, so a renamed column reads as UNREADABLE; the trace
  stays, so a parser repair re-runs `analyze` with no new capture (the 2026-10-07 run's export failed the same way and
  was re-analysed from its trace).
- [test-sample-overhead-wpr.py](../../scripts/test-sample-overhead-wpr.py) (12 cases, a CTest
  `crd-diag-overhead-sampling-runbook` and a repository CI step) covers every verdict with wpr, xperf and the harness
  faked; seven mutations of the script (no `-cancel`, a zero-sample process read as zero, no rotation, the stack's
  outer class overriding the leaf's, every comparison agreeing, foreign stack lines attributed, a run directory left
  after a denied start) each fail it. Run here unelevated, `record` prints `PERMISSION_DENIED: wpr -start failed
  (rc=3310899217) ... 0xc5585011` and leaves no run directory.

The user's one command, from an elevated PowerShell in `D:\Dev\cerid` (the two harness builds are current as of this
batch):

```
python scripts/sample-overhead-wpr.py record --progress
```

It writes `build/diag-overhead-wpr/<UTC stamp>/` (the trace `overhead.etl`, `manifest.json`, `analysis.json`) and
prints one verdict line per workload and arm; the next batch reads the directory unelevated.

## Benchmark hygiene: boards record the host

From 17:03 to about 19:00 local time on 2026-10-18 an image generator (24.8 GB, about four cores) shared the machine
while task B measured. The board already said so and voided one board; it also measured its decisive rows in that
window. So the boards now record the host themselves: [build-bench.py](../../scripts/build-bench.py) samples, before
the first row and after the last, the available memory and the processes using the most CPU (two snapshots 2 s apart)
and memory, on WSL the Windows host's as well through interop (a Windows process loads the CPUs and the memory behind
the WSL VM). A host where another process uses a core, all others two cores together, or less than a quarter of the
memory is available is refused unless `--allow-contended`, and the board then names the load in its record and its
rendered context; `build-bench.py host` prints the snapshot and exits 3 when it is contended. One new case in
`test-repository-tools.py`; four mutations (no refusal, no total-cores rule, the board's own process counted, the
sampler's own PowerShell counted) each fail it.

The first snapshot of this batch (21:40) already refused: a game (2.0 cores, 4.5 GB), a digital audio workstation
(1.4-1.7 cores) and a browser (1-3 cores) left 26.8-29.4 of 63.7 GB available and used 6.3 cores together.

## The compile-time profile route

- **C++ Build Insights.** `vcperf` 2.5 is in the VS 18 install (`VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64`), but
  `vcperf /start` answers "This operation requires administrator privileges", and `/start /noadmin` needs a one-time
  elevated `vcperf /grantusercontrol` first. So on this shell it is an elevated step.
- **MSVC's own switches instead.** `/Bt+` prints each unit's front-end (`c1xx.dll`) and back-end (`c2.dll`) seconds,
  and `/d1reportTime` the unit's include tree with every header's inclusive parse time and the time of every class
  and function definition; both run unelevated (probed on `engine/execution/ceir/src/print.cpp`: 146 headers, 0.45 s
  of its 0.54 s front end in includes). [build-profile.py](../../scripts/build-profile.py) seeds them through
  `CMAKE_CXX_FLAGS_INIT` into a fresh tree with no launcher (1,811 C++ commands carried them, none went through
  sccache) and parses Ninja's log as one stream.
- **Clang.** `-ftime-trace`, read by ClangBuildAnalyzer 1.6.0: registry entry `tools/clang-build-analyzer` (tag
  v1.6.0's commit `bae0cb48`, the commit-addressed archive's SHA-256, Unlicense with its bundled MIT, BSD, zlib, BSL
  and Apache sources named), built from source by
  [install-clang-build-analyzer.py](../../scripts/install-clang-build-analyzer.py) into `~/.cerid/tools/
  clang-build-analyzer` on WSL in 12 s (a second run reuses it: stamp and `--version` match). The license manifest
  lists it.
- Tests: three cases in `test-repository-tools.py` (the MSVC log and the ClangBuildAnalyzer report parsed into
  per-unit, inclusive and exclusive header, class, function and template tables, a busy host refused, the uncached
  fresh tree; the installer's verified source, version check and stamp); six mutations of the profiler each fail them.

The first profiles ran while the host was contended (recorded so; they rank, they do not time):

| Profile | Build | Front end | Back end | Include parsing |
|---|---|---|---|---|
| win-debug, PCH off (the cached local and hosted shape), 16 jobs | 257 s, edge sum 4,098 s | 3,382 s | 135 s | 2,440 s |
| linux-clang-tsan with TSan off, PCH on, 16 jobs | 233 s, edge sum 3,724 s | 3,095 s | 405 s | (in the front end) |

Where the time goes, on both compilers:
- **Catch2's `<chrono>`.** `catch_tostring.hpp` includes `<chrono>` unconditionally for its duration and time-point
  printers; MSVC: `<chrono>` 575 s inclusive in 1,155 units (1,059 through Catch2), 303 s of it its own text, plus
  `<format>` behind it; Clang: 633 s in 1,126 units, plus libstdc++'s eagerly instantiated `std::vformat_to` sets
  (222 s) and the formatter templates. Catch2's current development branch still includes it.
- **`crd/containers/array.hpp`** (1,365-1,414 units; 315 s MSVC, 381 s Clang): it includes `<iterator>` and uses
  nothing from it (its iterators are pointers); in libstdc++ `<iterator>` brings the stream-buffer iterators, so
  `<streambuf>`, `<ios_base>` and the locale classes enter every unit with an `Array`.
- **`crd/containers/hash.hpp`** (823-831 units; 130 s MSVC, 161 s Clang): `<functional>` for the `std::hash`
  fallback, which `<string_view>` (already included) declares with its arithmetic, enumeration and pointer
  specializations; on MSVC `<functional>` also brings `<unordered_map>`.
- **`crd/jobs/jobs.hpp`**: `<algorithm>` for two `std::min` calls (217 units, 36 s on MSVC).
- **`crd/math/cmath.hpp`**, the one-include math surface, includes `complex.hpp` (`<complex>` and, on MSVC, its stream
  headers: 171 s in 635 units) and through the transcendental functions `simd/backend.hpp` (`<immintrin.h>`: 275 s in
  818 units on Clang).
- **The FFT's generated codelets** (`batched_codelets_gen.hpp`, 263,577 lines; `hier_codelets.hpp`, 26,171) are
  parsed by each of the 35 units that include `fft.hpp` (5-9 s of front end each, the slowest units of the build);
  they are the FFT's hot kernels, force-inlined into the dispatchers on optimized GCC and Clang.
- `crd/log/log_record.hpp` and `crd/resources/resource_manager.hpp` include `<chrono>` for time points in their
  types (an interface, kept).
