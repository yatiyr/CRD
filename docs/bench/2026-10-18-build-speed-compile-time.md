# 2026-10-18 — build speed, task C: where compile time goes, and the cuts

<!-- doc-role: evidence -->
> Dated evidence; counts and results are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).
> Contract: [build performance](../design/build-performance.md#compile-time-profiles); session:
> [2026-10-18](../sessions/2026-10-18-diag-11b-build-speed-compile-time.md); owner row:
> [DIAG.11b](../ROADMAP.md#slice-diag.11b) (focused local routes and CI cost). Earlier boards of the batch:
> [cache and mirror](2026-10-18-build-speed-cache-mirror.md), [linking](2026-10-18-build-speed-linking.md).

- **Machine/config:** Intel i9-14900K (32 logical CPUs), 63.7 GiB RAM, Windows 11 10.0.26200, MSVC 19.51 (VS 18
  Community, toolset 14.51.36231), CMake 4.3.2, Ninja 1.13.2; WSL2 Ubuntu 24.04 (31.2 GiB), Clang 18.1.3 with
  libstdc++ 13, GCC 13.3, ClangBuildAnalyzer 1.6.0 (the registry's `tools/clang-build-analyzer`, built from source).
  Revision: `d71be8d6` and this batch's working tree.
- **Machine state:** every profile below ran while the host was contended, and the profile recorded it (the
  `build-bench.py` host snapshot): a game, a digital audio workstation and a browser used 5.7-8.8 cores on Windows
  together, with 26.8-30.6 of 63.7 GiB available (14.1 GiB at the last Clang profile); WSL itself was idle. These
  profiles rank costs and count units; they do not time the build. The timing boards run on an idle host.
- **Method:** `scripts/build-profile.py run` into a fresh tree with no compiler launcher (a cache hit would hide the
  unit), 16 jobs, the verified local dependency archives. MSVC: `win-debug` with `CRD_ENABLE_PCH=OFF` (the shape of
  the local and hosted cached trees), `/Bt+ /d1reportTime` seeded through `CMAKE_CXX_FLAGS_INIT`; `win-debug` with PCH
  on once (the default graph). Clang: `linux-clang-tsan` with `CRD_ENABLE_TSAN=OFF` (the only Clang preset; TSan would
  inflate the back end), PCH on, `-ftime-trace` read by ClangBuildAnalyzer. Header times are inclusive (a header's
  own text and everything it includes), summed over the units that parse it; the first includer of a shared header
  carries its cost, so after a cut another header can show more (`cmath.hpp` on MSVC below).

## Totals

| Profile | Wall | Edge sum | Front end | Back end | Include parsing |
|---|---|---|---|---|---|
| win-debug, PCH off, before | 257.4 s | 4,097.7 s | 3,381.6 s | 134.8 s | 2,439.5 s |
| win-debug, PCH on, before | 264.5 s | 4,209.7 s | 3,432.2 s | 130.6 s | 2,495.2 s |
| win-debug, PCH off, after | 214.6 s | 3,415.3 s | 2,731.1 s | 119.8 s | 2,080.8 s |
| linux-clang-tsan (TSan off), before | 232.9 s | 3,724.4 s | 3,095.1 s | 405.4 s | (in the front end) |
| linux-clang-tsan (TSan off), after | 167.5 s | 2,709.9 s | 2,109.5 s | 364.0 s | (in the front end) |

Load on the Windows host: MSVC before 8.6 cores, after 5.7 (so the MSVC totals are not comparable as timings);
Clang before 8.3 cores, after 8.2. The PCH-on and PCH-off MSVC profiles are within that noise: the four-header
default precompiled header saves nothing, as the [2026-09-14 board](2026-09-14-repo-dev7-build-board.md) found.

## The costly headers, before and after

| Header | MSVC before | MSVC after | Clang before | Clang after |
|---|---|---|---|---|
| Catch2 `catch_test_macros.hpp` | 723 s, 1,000 units | 377 s, 1,000 units | 678 s, 974 units | 273 s, 974 units |
| `<chrono>` | 576 s, 1,155 units | 168 s, 240 units | 633 s, 1,126 units | 143 s, 224 units |
| `<format>` | 181 s, 1,157 units | not in the top 40 | not in the top 40 | not in the top 40 |
| `crd/containers/array.hpp` | 315 s, 1,414 units | 166 s, 1,414 units | 381 s, 1,365 units | 108 s, 1,364 units |
| `crd/containers/hash.hpp` | 141 s, 855 units | not in the top 40 | 161 s, 823 units | 36 s, 823 units |
| `<iterator>` | 171 s, 1,673 units | not in the top 40 | 49 s, 1,381 units | not in the top 40 |
| `<functional>` | 148 s, 998 units | not in the top 40 (198 units) | not in the top 40 | not in the top 40 |
| `crd/math/cmath.hpp` | 206 s, 642 units | 249 s, 642 units | 225 s, 617 units | 194 s, 617 units |
| `crd/math/simd/backend.hpp` | not in the top 40 | not in the top 40 | 275 s, 818 units | 195 s, 573 units |
| `crd/hesap/fft/fft.hpp` | 229 s, 35 units | 220 s, 35 units | 127 s, 35 units | 122 s, 35 units |

Templates on Clang: libstdc++'s `<chrono>` brings `<format>`, whose `std::vformat_to` sets were instantiated in every
unit that reached it: 222.6 s before, 43.5 s after; the formatter sets 119.3 → 23.3 s.

What changed, and why each is behaviour-preserving, is in the [session](../sessions/2026-10-18-diag-11b-build-speed-compile-time.md#the-reductions):
two registered Catch2 patches (the assertion path no longer reaches `<chrono>`), the unused standard headers of
`array.hpp`, `hash.hpp`, `hash_map.hpp` and `jobs.hpp`, and the scalar exp and log cores apart from their AVX2 twins
(identical disassembly on GCC, Clang and MSVC). `crd-check-include-budget` holds the cuts.

<a id="timing-boards"></a>
## Timing boards: before (`d71be8d6`) against after (the cuts)

`scripts/build-bench.py run` into a fresh uncached tree, 16 jobs, rows `configure`, `cold` (the whole tree), and the
teeth cycle (`teeth-break` edits `crd/memory/construct.hpp`, which about 1,750 edges depend on, and rebuilds;
`teeth-restore` writes it back and rebuilds: the edit of a widely included header, the loop's common case). The
before arm checks out `d71be8d6`'s version of every build input the cuts changed (the files the cuts added stay and are
unused); arms interleaved A B B A (win-debug: then B A), so a steady load reaches both. Host: a digital audio
workstation and a browser used 2.5-5.9 cores on Windows throughout (recorded by each board; nothing else heavy),
24-34 GiB of 63.7 available.

win-debug, PCH off (the local and hosted cached trees' shape), MSVC 19.51, uncached:

| Board | Start (local) | configure | cold | teeth-break | teeth-restore |
|---|---|---|---|---|---|
| after, round 1 | 00:17 | 30.8 s | 157.1 s | 127.0 s | 125.2 s |
| before, round 1 | 00:24 | 29.9 s | 198.3 s | 161.8 s | failed at a link (below) |
| before, round 2 | 00:31 | 29.8 s | 208.1 s | 160.8 s | 165.1 s |
| after, round 2 | 00:41 | 53.1 s | 163.5 s | 131.5 s | 130.7 s |
| before, round 3 | 04:41 | 36.0 s | 189.4 s | 156.3 s | 169.6 s |
| after, round 3 | 04:51 | 29.3 s | 163.3 s | 132.2 s | 133.1 s |

The cold build is a fifth shorter (189-208 s → 157-164 s, means 198.6 → 161.3 s, −19 %) and a header edit's rebuild
too (teeth-break 156-162 s → 127-132 s, −18 %; teeth-restore 165-170 s → 125-133 s, −23 %). Loss: none measured; the
second after configure took 53 s against 29-31 s (the cuts change nothing CMake does; a configure-time outlier,
reported). The before arm's first teeth-restore stopped at `LNK1103` ("debugging information corrupt") in
`crd-memory.lib(malloc_allocator.cpp.obj)` while linking `crd-ceir-fatal-specimen.exe`: a fault of the linker or
this host, not of a source (the same edit rebuilt and linked in rounds 2 and 3); per the host rule it was rerun, not
chased. Round 3 (2026-10-19, the same swap: before `d71be8d6`, after `3acdce76`, the state rounds 1 and 2 measured)
supplies the missing before restore; the first attempt at its after arm stopped before building, when restoring the
checkout met a file another process held open (`array.hpp`; retried and rerun).

linux-gcc-debug from the ext4 mirror (`~/cerid-src`, the same before/after swap in the mirror, HEAD's files restored
and checked afterwards), GCC 13.3, PCH on (the preset's default), the system linker, uncached, 16 jobs. WSL idle in
every snapshot; on Windows the audio workstation and a browser used 2.6-5.3 cores; the falling Windows memory
(16-31 GiB available) is the WSL VM's page cache (`vmmemWSL` 2 → 18 GiB), not another load:

| Board | Start (local) | configure | cold | teeth-break | teeth-restore |
|---|---|---|---|---|---|
| after, round 1 | 00:50 | 123.6 s | 151.4 s | 140.6 s | 172.4 s |
| before, round 1 | 01:00 | 104.3 s | 197.3 s | 182.4 s | 178.4 s |
| before, round 2 | 01:12 | 120.4 s | 193.5 s | 181.2 s | 180.6 s |
| after, round 2 | 01:23 | 109.0 s | 156.5 s | 138.8 s | 138.9 s |

The cold build is 21-23 % shorter (193-197 s → 151-157 s, edge sum 3,144-3,190 s → 2,428-2,545 s) and a header
edit's rebuild 22-24 % (180-182 s → 139-141 s). Loss: none measured; the first after teeth-restore took 172 s
against its own teeth-break's 141 s (edge sum 2,779 s against 2,271 s for the same 1,706 edges, so a slower window,
not more work; round 2 restored in 139 s). Configure (104-124 s, mostly the dependency sources' population) is not
changed by the cuts.

## Not cut, with the numbers that rank them

- The FFT's generated codelets: 35 units parse `batched_codelets_gen.hpp` (263,577 lines) and `hier_codelets.hpp`
  (26,171), 178 s and 38 s of exclusive MSVC parse time; those units are the slowest of the build (7-11 s of front end
  each). The codelets are the FFT's hot kernels, force-inlined into `FftPlan<T>`'s dispatchers on optimized GCC and
  Clang, so a cut that keeps their code generation is an explicit instantiation of `FftPlan<f32/f64>` in one unit with
  its member definitions out of the class (the follow-up named in the session).
- `simd/backend.hpp` (with `<immintrin.h>`) still reaches 573 Clang units after the math split, through
  `crd/math/deterministic.hpp`'s `Vec4f` and the SIMD types (195 s; the deterministic part cut on 2026-10-19,
  [below](#deterministichpp-declares-its-vector-types-2026-10-19)); `cmath.hpp`'s `complex.hpp` brings `<complex>`
  and, on MSVC, its stream headers (`<sstream>` in 852 units after).
- Catch2's own library: 106 units, 179 s of edge time on MSVC (the only third-party compile above 10 s; GLFW and zstd
  are 8 s each); cut on 2026-10-19 ([below](#catch2s-library-as-unity-batches-2026-10-19)).

<a id="deterministichpp-declares-its-vector-types-2026-10-19"></a>
## `deterministic.hpp` declares its vector types (2026-10-19)

`crd/math/deterministic.hpp` (328 units, most through `crd/hesap/complex.hpp`) included `simd/vec4f.hpp` and
`simd/vec8f.hpp` for eight batched overloads that only name the types; it declares `struct Vec4f;` and
`struct Vec8f;` now, and the one test that calls the overloads includes the two headers (the implementation unit
already did). Counted in linux-clang-tsan's Ninja dependency log (all 2,072 units, the same tree before and after):

| Header | Units before | Units after |
|---|---|---|
| `crd/math/simd/backend.hpp` | 584 | 418 |
| `<immintrin.h>` | 602 | 456 |
| `crd/math/simd/vec4f.hpp` | 576 | 406 |

Not timed (one header, 146 units at roughly the 0.34 s per unit the profile gave `<immintrin.h>`); on MSVC the C
runtime's `<wchar.h>` brings `<intrin.h>` into nearly every unit anyway, so the cut there is the SIMD headers' own
parse. The whole tree builds with no failure and no warning on win-debug, linux-gcc-debug and linux-clang-tsan, the
math suite passes on all three, and `include-budget.json` holds the header (and `cmath.hpp`) to no `<immintrin.h>`
on GCC and Clang.

<a id="catch2s-library-as-unity-batches-2026-10-19"></a>
## Catch2's library as unity batches (2026-10-19)

`build/task-c/catch2_unity_probe.py` compiles Catch2's 106 library units from a tree's compile database (each command
the first member's, source and object replaced, no launcher, 16 at a time, objects in a scratch directory)
separately and as unity sources that include N members, twice each; then the tree's own unity batches once
`CRD_CATCH2_UNITY` builds them. Host: the audio workstation, 1.3-2.0 Windows cores; WSL idle.

| Compiler (tree) | separate, 106 units | batches of 8 (14) | batches of 16 (7) | the tree's batches, patched headers (7) |
|---|---|---|---|---|
| MSVC 19.51 (win-debug) | 105.7 / 112.9 s, wall 7.1 / 7.5 s | 21.7 / 21.8 s, wall 1.9 s | 9.7 s, wall 1.8 s | 9.0 / 9.1 s, wall 1.6 s |
| GCC 13.3 (linux-gcc-debug) | 70.4 / 68.8 s, wall 5.1 / 4.8 s | 18.9 / 19.1 s, wall 2.0 s | 10.8 s, wall 2.1 s | 11.1 s, wall 2.0 s |
| Clang 18 (linux-clang-tsan) | 76.5 / 69.8 s, wall 5.1 / 4.7 s | 16.9 / 18.5 s, wall 1.7-1.9 s | 9.8 s, wall 1.9 s | 9.9 s, wall 1.7 s |

Summed compile seconds. Equivalence: the objects of the separate and the 16-member build define the same global
symbols on GCC (`nm --defined-only -g`: 8,644 against 8,645, the one more a weak `std::nextafter(float, float)`
instantiation, no strong symbol different); every batch compiled on all three compilers; with the unity library
crd-containers-tests (12,554 assertions in 121 cases), crd-math-tests (6,428 in 162), crd-jobs-tests and
crd-ceir-tests (18,784 in 580, the count task B recorded) pass on win-debug, the first three on linux-gcc-debug and
linux-clang-tsan, and the Catch2 benchmarks of `crd-bench` run on win-debug and linux-gcc-debug; the whole trees
build with no warning. The saving is a cold, uncached build's (about 95 s of MSVC and 60 s of GCC or Clang compile);
a cached tree hits either way.

Found on the way: Catch2's own units had compiled against the unpatched headers. Its target lists its source
directory with a plain `-I`, and the patched copies were a system directory of the target (`SYSTEM BEFORE PUBLIC`),
which the compilers search after the plain ones (on cl: `-I...catch2-src\src\catch2\..` before
`-external:I...patched\Catch2\src`), so the library's units saw the chrono printers that its consumers do not: two
definitions of `StringMaker` for chrono types in one program. The copies are now an interface system directory and
a plain private directory first (`-I...patched\Catch2\src` leads the library's command), and a ninth include-budget
entry (`catch2/catch_tostring.hpp` with a library unit's command) fails if the order is lost: without the private
include it reports `<chrono> through catch2/catch_tostring.hpp -> chrono` on win-debug.

<a id="a-test-precompiled-header-with-catch2s-macros-2026-10-19"></a>
## A test precompiled header with Catch2's macros (2026-10-19)

After the cuts, `catch2/catch_test_macros.hpp` was still the costliest header of the test units (377 s MSVC, 273 s
Clang, inclusive), parsed once in each. The default precompiled header (`crd/core/pch.hpp`) of a target that links
`Catch2::Catch2WithMain` or `Catch2::Catch2` now also holds it (`crd_enable_default_pch`, decided at the end of the
configuration from the target's link libraries; `CRD_NO_CATCH2_PCH` opts a target out). A probe first
(`build/task-c/test_pch_probe.py`: one target's units compiled with the tree's precompiled header and with a probe
header adding the macros, GCC 13.3, 16 at a time, twice): crd-containers-tests' 8 units 4.1-4.4 → 2.4 s of summed
compile, crd-ceir-tests' 107 units 83.8-84.8 → 52.6-53.2 s (wall 5.5 → 3.4-3.5 s); the probe header itself builds
in 0.6-0.7 s per target.

Boards, linux-gcc-debug from the mirror, uncached, 16 jobs, before (HEAD's `CMakeLists.txt`, every earlier cut in)
against after, interleaved A B B A and then A B. The host was quiet until about 06:00; then another project's test
programs loaded Windows in bursts (7-12 cores in the snapshots), and the rows marked * met a burst (their edge sums
rose 50 % for the same edges):

| Board | Start (local) | cold | teeth-break | teeth-restore | Windows cores busy (before / after) |
|---|---|---|---|---|---|
| after, round 1 | 05:38 | 124.0 s | 106.8 s | 104.4 s | 0.9 / 0.9 |
| before, round 1 | 05:46 | 127.8 s | 117.6 s | 118.7 s | 1.0 / 1.0 |
| before, round 2 | 05:54 | 127.7 s | 115.9 s | 117.6 s | 1.5 / 1.0 |
| after, round 2 | 06:02 | 118.8 s | 174.0 s * | 125.0 s * | 1.0 / 10.1 |
| after, round 3 | 06:12 | 120.1 s | 113.4 s | 176.1 s * | 12.3 / 7.5 |
| before, round 3 | 06:20 | 129.4 s | 116.7 s | 115.7 s | 7.1 / 0.8 |

The cold build is 5.7 % shorter (means 128.3 → 121.0 s; edge sums 2,063-2,118 → 1,931-2,007 s) and a header edit's
rebuild 3-9 % (teeth-break 115.9-117.6 → 106.8-113.4 s) to 11 % (teeth-restore 115.7-118.7 → 104.4 s, the one
undisturbed after row). Smaller than the probe's per-unit saving because the test units are under half of the
build and every test target now builds the larger precompiled header (111 of the 249 targets with one, 0.6-0.7 s
each). Loss: none
measured; the MSVC cached trees (precompiled headers off) are unchanged. These boards also show the whole batch's
cuts at a quiet load: linux-gcc-debug's cold build at 128 s before this change against 193-197 s before the cuts
(01:00-01:12, 2.6-5.3 Windows cores busy, so not a like-for-like timing). Equivalence: the trees build with no
warning on linux-gcc-debug, linux-clang-tsan and an MSVC tree with precompiled headers on (scratch, no launcher), and
crd-containers-tests (12,554 assertions in 121 cases), crd-math-tests (6,428 in 162), crd-jobs-tests, crd-memory-tests
and crd-ceir-tests (18,784 in 580) pass with the same counts; no test unit configures Catch2 before including it
(the two `CATCH_CONFIG_RUNNER` mains define a Catch2 v2 macro that v3 ignores).


<a id="c-build-insights-capture-2026-10-25"></a>
## C++ Build Insights capture (2026-10-25)

The user's elevated `scripts\elevated-captures.bat` (Visual Studio closed) ran `build-profile.py run --compiler msvc
--preset win-debug --jobs 16 --define CRD_ENABLE_PCH=OFF --vcperf CeridBuildInsights` into a fresh tree: the cached
shape (no precompiled header), no compiler launcher, `/Bt+ /d1reportTime`, and vcperf 2.5 around the build
(`/stopnoanalyze`, then `/analyze /templates` into `build/build-insights/win-debug-vcperf.etl` and `.json`). Host
idle: 38.1 of 63.7 GiB free, other processes 1.1 cores. Build exit 0. Read on 2026-10-25
([session](../sessions/2026-10-25-diag-11b-external-sampling.md#c-build-insights)).

| Profile | Wall | Edge sum | Front end | Back end | Include parsing | Units |
|---|---|---|---|---|---|---|
| win-debug, PCH off, 2026-10-25 | 176.5 s (configure 45.0 s) | 2,073.1 s (2,116 edges) | 1,705.5 s | 71.7 s | 1,297.8 s | 1,738 |

The 2026-10-18 rows above ran on a contended host (5.7 cores busy after), so the totals are not a like-for-like
timing; vcperf's own front-end and back-end passes give the same sums as `/Bt+` (1,705.5 s and 71.7 s).

| Unit (front + back end, summed over its compilations) | Seconds | Front / back |
|---|---:|---|
| `tests/support/fuzz/src/replay_main.cpp` (13 compilations, one per fuzz target) | 20.87 | 20.2 / 0.6 |
| `tests/numerics/hesap-fft/test_fft.cpp` | 7.85 | 5.5 / 2.3 |
| `tests/numerics/hesap-fft/test_nufft.cpp` | 7.30 | 4.8 / 2.5 |
| `tests/numerics/hesap-fft/test_dct.cpp` | 7.24 | 4.8 / 2.5 |
| `tests/numerics/hesap-dsp/test_spectral.cpp` | 6.94 | 6.3 / 0.6 |
| `tests/numerics/hesap-wavelet/test_cwt.cpp` | 6.92 | 6.4 / 0.6 |

Back end per unit, the largest: `test_nufft.cpp` 2.49 s, `test_dct.cpp` 2.48 s, `test_fft.cpp` 2.32 s,
`test_hilbert.cpp` 1.76 s, `test_fir.cpp` 1.57 s, `test_multirate.cpp` 1.53 s, `hesap-fft/src/fft.cpp` 1.20 s: every
unit above 1 s of back end includes the FFT.

| Header, inclusive | Seconds | Units |
|---|---:|---:|
| Catch2 `catch_test_macros.hpp` (`catch_assertion_handler.hpp` 229.6 s) | 243.5 | 1,002 |
| MSVC `__msvc_ostream.hpp` (`<ios>` 184.3 s, `xlocnum` 183.2 s, `<streambuf>` 142.9 s) | 191.1 | 1,273 |
| MSVC `xutility` | 184.9 | 1,642 |
| `crd/math/cmath.hpp` (through `crd/math/complex.hpp`, 172.1 s) | 172.7 | 644 |
| MSVC `<span>` (through `crd/containers/span.hpp`, 142.3 s) | 145.0 | 1,335 |
| MSVC `<complex>` | 141.4 | 657 |
| `crd/hesap/fft/fft.hpp` | 135.0 | 35 |

| Header, exclusive | Seconds | Units |
|---|---:|---:|
| `crd/hesap/fft/detail/batched_codelets_gen.hpp` | 101.9 | 35 |
| MSVC `xutility` | 72.2 | 1,642 |
| MSVC `xstring` | 49.2 | 1,339 |
| MSVC `zmmintrin.h` | 47.8 | 1,653 |
| MSVC `type_traits` | 43.2 | 1,675 |
| MSVC `<memory>` | 34.7 | 1,394 |
| MSVC `<chrono>` | 33.7 | 201 |

The stream headers' route, from the include trees: `<ostream>` reaches 601 units through Catch2's
`catch_tostring.hpp`, `<istream>` 547 units through `crd/math/complex.hpp` (MSVC's `<complex>` includes `<sstream>`)
and 43 through `crd/log/log_record.hpp`; no other project header brings them to more than 18 units.

Templates and classes (front end, `/d1reportTime`; MSVC's template instantiation time lands in the class and function
definitions): `std::basic_string` 5.7 s for the primary template and 4.4-4.5 s for each of its five specializations
over 1,339 units, `std::tuple` 3.8 s (1,437), Catch2's `ExprLhs` 3.2 s (1,006); functions: the FFT's
`codelet256_stage1_fused_sh_csf` 19.0 s, `_sh_cs` 14.1 s, `codelet256_batched` 11.7 s and `_batched_sc` 11.2 s (35
units each), Catch2's `CATCH2_INTERNAL_TEST_0` 10.3 s (906), `std::_Integral_to_string` 5.8 s (1,277). vcperf's own
template-instantiation table has no rows in this capture (`wpaexporter` with the add-in's Template Instantiations
table: "No data in table for preset Templates"), although `/analyze /templates` ran.

Function generation (vcperf, the functions above its threshold; code-generation thread time): 135.6 s in 1,302
generations of 258 functions. The FFT's codelets and twiddles are 126.8 s of it (735 generations of 85 functions:
each `double` codelet is generated in 21 units, each `float` one in 4); Catch2's test bodies 6.2 s; everything else
2.6 s. The largest: `codelet256_stage1_fused_sh_csf<double>` 14.3 s over 21 generations,
`codelet256_stage1_fused_sh_cs` 11.0 s, `codelet64_stage1_fused_64x64` 9.7 s, `codelet64_stage1_fused_64x32` 9.3 s,
`codelet256_batched_sc` 8.0 s.

Follow-up data (recorded, not started; each would be measured on its own board):

- The FFT codelets: header-inline functions generated again in every unit that uses them, 126.8 s of code generation
  of which 20 of every 21 `double` copies repeat, plus `batched_codelets_gen.hpp`'s 101.9 s of exclusive parse in 35
  units. The cut is the one named [above](#timing-boards) (the definitions out of line in one unit, or an explicit
  instantiation of `FftPlan<f32/f64>`), keeping the force-inlining into the dispatchers on optimized GCC and Clang.
- `replay_main.cpp` compiles once per fuzz target (`crd_fuzz_target`, 13 times, about 1.6 s each): one object library
  shared by the targets saves about 19 s of edge time.
- `crd/math/cmath.hpp` includes `complex.hpp`, whose `<complex>` brings MSVC's stream headers into 547 of its 644
  units: a `cmath.hpp` without the complex functions (their consumers include `complex.hpp`) is a cut of the kind the
  include budget holds.
