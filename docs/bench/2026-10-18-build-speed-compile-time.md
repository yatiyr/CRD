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
unused); arms interleaved A B B A, so a steady load reaches both. Host: a digital audio workstation and a browser
used 3.1-4.4 cores on Windows throughout (recorded by each board; nothing else heavy), 24-33 GiB of 63.7 available.

win-debug, PCH off (the local and hosted cached trees' shape), MSVC 19.51, uncached:

| Board | Start (local) | configure | cold | teeth-break | teeth-restore |
|---|---|---|---|---|---|
| after, round 1 | 00:17 | 30.8 s | 157.1 s | 127.0 s | 125.2 s |
| before, round 1 | 00:24 | 29.9 s | 198.3 s | 161.8 s | failed at a link (below) |
| before, round 2 | 00:31 | 29.8 s | 208.1 s | 160.8 s | 165.1 s |
| after, round 2 | 00:41 | 53.1 s | 163.5 s | 131.5 s | 130.7 s |

The cold build is 21 % shorter (198-208 s → 157-164 s) and a header edit's rebuild 20-22 % (161-165 s → 125-132 s).
Loss: none measured; the second after configure took 53 s against 30 s (the cuts change nothing CMake does; a
configure-time outlier, reported). The before arm's first teeth-restore stopped at `LNK1103` ("debugging information
corrupt") in `crd-memory.lib(malloc_allocator.cpp.obj)` while linking `crd-ceir-fatal-specimen.exe`: a fault of the
linker or this host, not of a source (the same edit rebuilt and linked in round 2); per the host rule it was rerun,
not chased.

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
  are 8 s each).

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
