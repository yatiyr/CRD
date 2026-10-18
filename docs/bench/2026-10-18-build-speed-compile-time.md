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

## Not cut, with the numbers that rank them

- The FFT's generated codelets: 35 units parse `batched_codelets_gen.hpp` (263,577 lines) and `hier_codelets.hpp`
  (26,171), 178 s and 38 s of exclusive MSVC parse time; those units are the slowest of the build (7-11 s of front end
  each). The codelets are the FFT's hot kernels, force-inlined into `FftPlan<T>`'s dispatchers on optimized GCC and
  Clang, so a cut that keeps their code generation is an explicit instantiation of `FftPlan<f32/f64>` in one unit with
  its member definitions out of the class (the follow-up named in the session).
- `simd/backend.hpp` (with `<immintrin.h>`) still reaches 573 Clang units after the math split, through
  `crd/math/deterministic.hpp`'s `Vec4f` and the SIMD types (195 s); `cmath.hpp`'s `complex.hpp` brings `<complex>`
  and, on MSVC, its stream headers (`<sstream>` in 852 units after).
- Catch2's own library: 106 units, 179 s of edge time on MSVC (the only third-party compile above 10 s; GLFW and zstd
  are 8 s each).
