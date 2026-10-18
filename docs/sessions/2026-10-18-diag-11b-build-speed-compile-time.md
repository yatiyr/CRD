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

The user's one command, from an elevated prompt in `D:\Dev\cerid` (the two harness builds are current as of this
batch):

```
scripts\elevated-captures.bat
```

It runs `python scripts/sample-overhead-wpr.py record --progress`, which writes `build/diag-overhead-wpr/<UTC stamp>/`
(the trace `overhead.etl`, `manifest.json`, `analysis.json`) and prints one verdict line per workload and arm, and then
the C++ Build Insights capture of task C (`build-profile.py run ... --vcperf CeridBuildInsights` around an uncached
win-debug build in the cached shape, about five minutes: `build/build-insights/win-debug.json` with the
`win-debug-vcperf.etl` and `.json` traces). Each step reports its own exit; nothing is installed or granted. The next
batch reads both unelevated.

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
- The four-header default precompiled header saves nothing on MSVC: the PCH-on profile's front end was 3,432 s
  against 3,382 s off (both contended), as the 2026-09-14 board found.

## The reductions

- **Catch2, two registered patches** (`cmake/pins.json` → `packages.Catch2.patches`, specs in `cmake/patches/`, the
  copies first on Catch2's include path as system directories, checked in `compile_commands.json`: on cl
  `/external:I<tree>/patched/Catch2/src` before `<tree>/_deps/catch2-src/src`). `catch2-chrono-stringmaker-opt-in`
  puts `catch_tostring.hpp`'s chrono printers behind `CATCH_CONFIG_ENABLE_CHRONO_STRINGMAKER`, as Catch2 v2 had them.
  With only that patch the next profile showed `<chrono>` still in all 1,155 units, now through
  `benchmark/catch_clock.hpp`: every assertion reaches `IResultCapture`, whose `benchmarkEnded(BenchmarkStats<>)`
  defaults its duration to the clock header's `FDuration`. That needs real chrono declarations in every unit (the
  class must be the same everywhere), but only durations and clocks, which both standard libraries keep in a header of
  their own that their `<chrono>`, `<thread>` and `<mutex>` include (`<__msvc_chrono.hpp>`, `<bits/chrono.h>`); the
  second patch, `catch2-clock-chrono-subset`, includes that one where the library has it and `<chrono>` otherwise.
  Every name Catch2 uses (`duration`, `time_point`, `duration_cast`, the three clocks, the ratio aliases) is in it.
  The one observable change: a failure message of a duration prints through C++20's `operator<<` when the test
  includes `<chrono>`, and `{?}` for a type with none (a `steady_clock` time point), where Catch2 printed
  "N ns since epoch"; assertions pass and fail exactly as before. One test used `std::chrono::steady_clock` without
  including `<chrono>` (`test_ceir_pipeline_dx12.cpp`) and now includes it; the static scan of every `std::chrono`
  user found no other.
- **Hot headers.** `crd/containers/array.hpp` drops `<iterator>` (its iterators are pointers; nothing used it).
  `crd/containers/hash.hpp` drops `<functional>`: its `std::hash` fallback is declared by `<string_view>` with the
  arithmetic, enumeration and pointer specializations. `hash_map.hpp` and `hash_set.hpp` took `<functional>` for
  `std::equal_to<>` as their default key equality; `hash.hpp` now defines `DefaultEqual` (a transparent `a == b` with
  the same `noexcept`), their default, so no container header includes `<functional>`. `crd/jobs/jobs.hpp` drops
  `<algorithm>` for its two `std::min` calls (the ternary the same header already used at its third site).
- **Math.** `crd/math/simd/transcendental.hpp` held the scalar f64 exp and log cores beside their AVX2 twins, and
  `crd/math/transcendental.hpp` (through `cmath.hpp`, the one-include math surface) took the whole of it, the SIMD
  backend and `<immintrin.h>` included. The scalar cores and their constants moved byte for byte (a verified script)
  into `crd/math/exp_log_core.hpp`, which the scalar header includes; `simd/transcendental.hpp` includes it and keeps
  the twins. Equivalence: a probe of `exp`, `log`, `expm1`, `log1p`, the `float` `exp`, `crd_exp4` and `crd_log4`
  compiled with HEAD's two headers first on the include path and with the tree's gives identical per-function
  disassembly on GCC 13.3 and Clang 18 (`-O2 -ffp-contract=off -mavx2 -mfma`; 523 and 745 lines) and MSVC 14.51
  (`/O2 /arch:AVX2`, 844 lines), each side's include listing showing its own headers. A static scan of every user of a
  `CRD_SIMD_*` macro or an x86 intrinsic found none left without its definer (an undefined pair would compare `0 == 0`
  in `#if` and take the AVX2 branch on the scalar presets); one user of `crd_exp4`, `hesap-tensor/nn.hpp`, includes
  its header now.
- **Missing includes the cuts exposed**, each fixed in the consumer, never by putting the header back:
  `geometry-primitives/transform.hpp` (`<algorithm>` for `std::max` of an initializer list, which libstdc++ had
  delivered through `<iterator>`), five `geometry-spatial` tests (`<memory>` for `std::make_unique`, which came with
  `<chrono>`). Found by whole-tree builds that keep going past failures: win-debug (1,998 edges), linux-clang-tsan and
  linux-gcc-debug (2,086 edges); afterwards each builds with no failure and no `warning:` line.
- **Not cut**: the FFT codelets (hot; their dispatchers inline them), `crd/log/log_record.hpp`'s and the resource
  manager's `<chrono>` (time points in their interfaces), `crd/containers/sort.hpp`'s `<functional>` and `<iterator>`
  (it uses `std::less<>` and `std::iterator_traits`).

## The include budget: a guard so the cuts stay

[check-include-budget.py](../../scripts/check-include-budget.py) with
[include-budget.json](../../scripts/include-budget.json): six hot headers, each with a test unit whose command it
borrows and the standard headers it must not reach; the `crd-check-include-budget` CTest. Within on win-debug,
linux-gcc-debug and linux-clang-tsan (69-282 headers per probe). Teeth, each restored byte for byte with its time:
`<iterator>` back in `array.hpp`, `<functional>` in `hash.hpp`, `<algorithm>` in `jobs.hpp` each fail it on MSVC and GCC
with the chain (`crd/containers/hash_map.hpp -> crd/containers/hash.hpp -> functional`); the scalar math header
including the SIMD one again fails it on GCC (`-> crd/math/simd/backend.hpp -> immintrin`); undoing either Catch2
patch in the tree's copy fails it on the compilers that patch serves (the MSVC branch on win-debug, the libstdc++
branch on GCC and Clang). One case in `test-repository-tools.py` (both listings, a Turkish `/showIncludes` prefix,
the chain, a launcher removed, a probe that does not compile never within); four mutations of the guard each fail it.
A fifth, the stop at GCC's closing "Multiple include guards" list, could not: those lines have no leading dots and are
ignored anyway, so the code was removed.

The profiles after the cuts (contended like the first ones; they rank):

| Profile | Build | Front end | Back end | `<chrono>` units | Catch2 macros |
|---|---|---|---|---|---|
| win-debug PCH off, before | 257 s, edge sum 4,098 s | 3,382 s | 135 s | 1,155 (575 s) | 723 s |
| win-debug PCH off, after | 215 s, edge sum 3,415 s | 2,731 s | 120 s | 240 (169 s) | 377 s |
| linux-clang-tsan (TSan off), before | 233 s, edge sum 3,724 s | 3,095 s | 405 s | 1,126 (633 s) | 678 s |
| linux-clang-tsan (TSan off), after | 168 s, edge sum 2,710 s | 2,110 s | 364 s | 224 (143 s) | 273 s |

On Clang the host carried the same load in both runs (8.2 and 8.3 Windows cores busy); on MSVC 8.6 before and
5.7-6.8 after, so its totals are not a timing. Per header (inclusive): `array.hpp` 315 → 166 s (MSVC), 381 → 108 s
(Clang); `hash.hpp` 161 → 36 s (Clang); `<functional>` 998 → 198 units (MSVC); `<iterator>` out of the MSVC top 40;
`<format>` out of the MSVC top 40, its `std::vformat_to` template sets 223 → 43 s on Clang; `simd/backend.hpp` 818 →
573 units (Clang), the rest through `crd/math/deterministic.hpp`'s `Vec4f` and the SIMD types.
