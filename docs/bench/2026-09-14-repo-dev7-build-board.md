# 2026-09-14 — REPO.DEV.7 build board: cold / no-op / edit / header / link, PCH and sccache on `win-debug`

<!-- doc-role: evidence -->
> Dated evidence; counts and results are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).
> Contract: [build performance](../design/build-performance.md); session: [2026-09-14](../sessions/2026-09-14-build-performance.md).

- **Machine/config:** Intel i9-14900K (32 logical CPUs), 63.7 GiB RAM, Windows 11 10.0.26200; MSVC 19.51.36246
  (VS 2026 Community toolset 14.51.36231), CMake 4.3.2, Ninja 1.13.2, sccache 0.17.0 (pinned,
  `cmake/pins.json`); preset `win-debug` (Debug, asserts and profiling on, `/W4 /WX`, `/arch:AVX2` auto); every
  build row `--parallel 16` on a workstation that also ran the session's tooling (baseline commit charge about
  82 GiB, about 24 GiB physical available before the first row). Every Windows board builds the same 2,086-edge
  graph (1,562 compiles through the launcher when cached, 1,838 to 2,099 edges dirty per row).
- **Harness:** `python scripts/build-bench.py run --preset win-debug --jobs 16 --build-dir build/bench-<x> --label …
  --rows configure,cold,noop,source,header,pch-header,link --output build/bench/board-<x>.json` from
  `scripts/msvc-env.bat`; boards C, D and D2 add `--define CRD_ENABLE_PCH=OFF --define CRD_COMPILER_LAUNCHER=<sccache>
  --sccache <sccache>` with `SCCACHE_DIR=build/bench-sccache` (`SCCACHE_CACHE_SIZE=30G`, emptied before C); board B
  adds `--define CRD_ENABLE_PCH=OFF`. The table is `scripts/build-bench.py render build/bench/board-*.json`.
- **Rows:** `configure` (`cmake --preset win-debug -B <scratch>`); `cold` (fresh tree); `noop` (rebuild, nothing
  dirty); `source` (append a comment to `engine/foundation/log/src/logger.cpp`, rebuild, restore); `header`
  (`crd/log/log.hpp`, outside the PCH); `pch-header` (`crd/core/types.hpp`, one of the four PCH headers); `link`
  (delete `crd-ceir-tests.exe`, rebuild). "Pressure" is the peak system commit charge above the row's baseline;
  "Min avail" the minimum available physical memory during the row; "Edges" the Ninja edges the row rebuilt.
- **Scope:** measurement of one preset in scratch trees, no tests run; a hosted runner has four vCPUs and its own
  numbers arrive in every lane's evidence bundle (`conclusion.json`, `build` section) from the next push on.

## The board

- **A: PCH on, uncached (default)**: `CRD_ENABLE_PCH=ON`, no launcher; tree 10.8 GiB after the board (9.3 GiB after cold).
- **B: PCH off, uncached**: `CRD_ENABLE_PCH=OFF`; tree 9.4 GiB (7.9 GiB after cold).
- **C: PCH off, sccache, empty cache**: the first cached build; cache 0.71 GiB after cold, 0.99 GiB after the edit rows.
- **D: PCH off, sccache, warm cache, different build directory** (`build/bench-d-cache-hit`).
- **D2: PCH off, sccache, warm cache, same build directory as C** (`build/bench-c-cache-miss`, emptied first).

| Board | Row | Wall s | Edges | Ninja span s | Edge sum s | Parallelism | Pressure GiB | Min avail GiB | sccache hits / misses / non-cacheable |
|---|---|---|---|---|---|---|---|---|---|
| A: PCH on, uncached (default) | configure | 34.3 | 0 | 0.0 | 0 | 0.0 | 0.1 | 23.6 | uncached |
| A: PCH on, uncached (default) | cold | 136.5 | 2099 | 136.3 | 2174 | 15.9 | 5.8 | 16.1 | uncached |
| A: PCH on, uncached (default) | noop | 0.5 | 1 | 0.1 | 0 | 1.0 | 0.0 | 18.3 | uncached |
| A: PCH on, uncached (default) | source | 4.4 | 162 | 4.1 | 47 | 11.3 | 1.1 | 17.7 | uncached |
| A: PCH on, uncached (default) | header | 8.8 | 209 | 8.6 | 118 | 13.8 | 1.9 | 16.7 | uncached |
| A: PCH on, uncached (default) | pch-header | 125.0 | 1899 | 124.8 | 1991 | 16.0 | 3.1 | 16.4 | uncached |
| A: PCH on, uncached (default) | link | 1.0 | 2 | 0.6 | 0 | 0.7 | 0.0 | 18.2 | uncached |
| B: PCH off, uncached | configure | 33.1 | 0 | 0.0 | 0 | 0.0 | 0.1 | 18.4 | uncached |
| B: PCH off, uncached | cold | 137.4 | 1848 | 137.1 | 2187 | 15.9 | 3.1 | 18.3 | uncached |
| B: PCH off, uncached | noop | 0.5 | 1 | 0.1 | 0 | 1.0 | 0.0 | 20.8 | uncached |
| B: PCH off, uncached | source | 4.4 | 162 | 4.1 | 46 | 11.4 | 1.7 | 19.6 | uncached |
| B: PCH off, uncached | header | 8.7 | 209 | 8.4 | 118 | 14.0 | 1.9 | 18.7 | uncached |
| B: PCH off, uncached | pch-header | 123.4 | 1639 | 123.1 | 1964 | 16.0 | 2.1 | 19.0 | uncached |
| B: PCH off, uncached | link | 0.9 | 2 | 0.6 | 0 | 0.8 | 0.0 | 20.5 | uncached |
| C: PCH off, sccache, empty cache | configure | 33.7 | 0 | 0.0 | 0 | 0.0 | 0.2 | 20.9 | 0 / 0 / 0 |
| C: PCH off, sccache, empty cache | cold | 165.1 | 1848 | 164.8 | 2631 | 16.0 | 3.7 | 19.0 | 0 / 1562 / 0 |
| C: PCH off, sccache, empty cache | noop | 0.5 | 1 | 0.1 | 0 | 1.0 | 0.0 | 21.3 | 0 / 0 / 0 |
| C: PCH off, sccache, empty cache | source | 4.5 | 162 | 4.3 | 45 | 10.6 | 1.7 | 20.3 | 0 / 1 / 0 |
| C: PCH off, sccache, empty cache | header | 9.4 | 209 | 9.1 | 127 | 13.9 | 2.3 | 19.2 | 0 / 38 / 0 |
| C: PCH off, sccache, empty cache | pch-header | 147.3 | 1639 | 147.0 | 2348 | 16.0 | 2.3 | 19.2 | 0 / 1383 / 0 |
| C: PCH off, sccache, empty cache | link | 0.9 | 2 | 0.6 | 0 | 0.8 | 0.1 | 20.8 | 0 / 0 / 0 |
| D: PCH off, sccache, warm cache, other directory | configure | 33.6 | 0 | 0.0 | 0 | 0.0 | 0.1 | 21.1 | 0 / 0 / 0 |
| D: PCH off, sccache, warm cache, other directory | cold | 138.4 | 1848 | 138.1 | 2205 | 16.0 | 3.3 | 19.5 | 534 / 1028 / 0 |
| D: PCH off, sccache, warm cache, other directory | noop | 0.5 | 1 | 0.1 | 0 | 1.0 | 0.0 | 21.6 | 0 / 0 / 0 |
| D: PCH off, sccache, warm cache, other directory | source | 3.4 | 162 | 3.2 | 43 | 13.4 | 1.2 | 20.8 | 1 / 0 / 0 |
| D: PCH off, sccache, warm cache, other directory | link | 1.0 | 2 | 0.7 | 1 | 0.8 | 0.0 | 21.1 | 0 / 0 / 0 |
| D2: PCH off, sccache, warm cache, same directory | configure | 41.0 | 0 | 0.0 | 0 | 0.0 | 0.6 | 23.9 | 0 / 0 / 0 |
| D2: PCH off, sccache, warm cache, same directory | cold | 43.5 | 1848 | 43.3 | 688 | 15.9 | 4.5 | 17.2 | 1562 / 0 / 0 |
| D2: PCH off, sccache, warm cache, same directory | noop | 0.5 | 1 | 0.1 | 0 | 1.0 | 0.0 | 21.8 | 0 / 0 / 0 |
| D2: PCH off, sccache, warm cache, same directory | source | 3.4 | 162 | 3.2 | 43 | 13.5 | 1.7 | 20.5 | 1 / 0 / 0 |
| D2: PCH off, sccache, warm cache, same directory | header | 4.5 | 209 | 4.3 | 62 | 14.6 | 1.9 | 19.3 | 38 / 0 / 0 |
| D2: PCH off, sccache, warm cache, same directory | link | 1.0 | 2 | 0.6 | 0 | 0.8 | 0.0 | 20.2 | 0 / 0 / 0 |

Slowest cold edges on every uncached board: the `crd-hesap-fft-tests` and `crd-hesap-wavelet-tests` units
(`test_dct`, `test_fft`, `test_nufft`, `test_cwt`, `test_hilbert`; 7.5 to 9.9 s each). On the full-hit board the
slowest edges are the asset cook (2.7 s) and the test executable links (1.9 to 2.1 s).

## Verdicts

1. **The MSVC precompiled header buys nothing measurable on this code base.** A against B: cold 136.5 s against
   137.4 s (edge sums 2,174 s against 2,187 s), identical source, header and link rows, and the PCH-header edit
   125.0 s against 123.4 s. The four-header payload (`assert`, `build_config`, `platform`, `types`) is too small to
   pay for the per-target PCH compiles, and it costs 1.4 GiB of tree. Turning PCH off for caching is free.
2. **A miss costs about a fifth.** C against B: cold 165.1 s against 137.4 s (+20 %; edge sum +444 s of cache
   writes), PCH-header edit 147.3 s against 123.4 s (+19 %), header edit 9.4 s against 8.7 s. The first cached
   hosted run pays this once.
3. **A full hit rebuilds the tree in 43.5 s, 3.1× faster than today's default cold build** (D2 against A), with
   1,562 of 1,562 compiles served from a 0.99 GiB cache; the remaining 43 s is the 286 non-compile edges (links,
   shader compiles, the asset cook: 688 s of edge time). Reverting a header edit with hits takes 4.5 s against 8.8 s.
4. **The cache key includes the build directory.** D, the same sources in a different scratch directory, hit only
   the 534 third-party units whose commands do not name the build tree and missed the 1,028 Cerid units (the
   generated `build_config.hpp` include directory and the object path differ); D2 in C's directory hit everything.
   Hosted lanes always build `build/<preset>` under the same workspace; a local cache needs a stable directory.
5. **Memory is not the bound at 16 jobs.** Peak pressure 5.8 GiB with PCH on (about 0.36 GiB per compile), 3.1 to
   4.5 GiB without; the Debug `crd-ceir-tests` relink adds nothing measurable (0.4 s, incremental linker). Effective
   parallelism is 15.9 to 16.0 of 16 on every cold row: the build is compile-bound with no scheduling gap, so job
   pools are a memory tool, not a throughput tool, on this machine.
6. **Disk.** A Debug tree is 9.3 to 10.8 GiB; embedded debug information (`/Z7`) did not grow the tree (B 9.4 GiB,
   D2 9.7 GiB) because the per-target PDBs went away with it; the sccache cache holds the whole preset in 0.71 GiB
   (zstd-compressed objects), 0.99 GiB with the edited variants.
7. **Configure** costs 33 to 41 s with every archive already in the CPM cache; it is the same on every board.

## Link pressure, ASan and Debug

Sampled at half-second intervals with `--parallel 1` in the existing trees (`build/bench/asan-link.json`): the
`win-asan` `crd-ceir-tests` row rebuilt 24 stale objects and linked the 20.7 MiB AddressSanitizer executable in
22.5 s with a peak commit charge 0.19 GiB above baseline; the Debug relink of the same target (10.5 MiB, incremental
linker) took 0.4 s at 0.22 GiB. Neither link approaches the compile-side pressure, so `CRD_LINK_JOBS` is a tool for
hosts that link larger optimized artifacts, not a default.


## Linux GCC board (WSL2, native storage)

- **Machine/config:** the same i9-14900K seen by a WSL2 virtual machine (kernel 6.6.87.2-microsoft-standard-WSL2,
  32 logical CPUs, 31.2 GiB assigned), sources copied to native ext4 (`~/cerid-bench`, 4,713 files) because the
  9p mount is not a build-speed instrument; GCC 13.3.0, CMake and Ninja of the Ubuntu 24.04 image, Python 3.12.3,
  the same pinned sccache 0.17.0 (Linux archive); preset `linux-gcc-debug`, `--parallel 16`. Not bare metal: the
  numbers compare Linux rows with each other, not with the Windows table. The `configure` rows downloaded the
  archives (the copy had no CPM source cache), so they are network-bound and not compared. On this VM the Ninja
  span exceeds the wall time by about 3 % (a clock-source difference), so the wall column is the one to read.
- **Harness:** the same runner and rows (`scripts/build-bench.py run --preset linux-gcc-debug --jobs 16 …`);
  boards LC, LD and LD2 add `--define CRD_COMPILER_LAUNCHER=<sccache> --sccache <sccache>` with
  `SCCACHE_DIR=~/bench-sccache` (emptied before LC); the precompiled header stays on in every Linux board.

| Board | Row | Wall s | Edges | Edge sum s | Parallelism | Pressure GiB | Min avail GiB | sccache hits / misses / non-cacheable |
|---|---|---|---|---|---|---|---|---|
| LA: PCH on, uncached (default) | configure (network) | 102.8 | 0 | 0 | 0.0 | 0.6 | 29.9 | uncached |
| LA: PCH on, uncached (default) | cold | 142.8 | 2086 | 2344 | 15.9 | 14.0 | 17.9 | uncached |
| LA: PCH on, uncached (default) | noop | 0.1 | 1 | 0 | 1.0 | 0.0 | 30.3 | uncached |
| LA: PCH on, uncached (default) | source | 5.8 | 156 | 74 | 12.9 | 1.0 | 29.5 | uncached |
| LA: PCH on, uncached (default) | header | 10.6 | 203 | 143 | 13.6 | 2.6 | 27.8 | uncached |
| LA: PCH on, uncached (default) | pch-header | 139.6 | 1849 | 2288 | 15.9 | 13.9 | 18.1 | uncached |
| LA: PCH on, uncached (default) | link | 0.6 | 2 | 1 | 1.0 | 0.0 | 30.3 | uncached |
| LC: PCH on, sccache, empty cache | configure (network) | 101.4 | 0 | 0 | 0.0 | 0.6 | 29.9 | 0 / 0 / 0 |
| LC: PCH on, sccache, empty cache | cold | 160.5 | 2086 | 2646 | 15.9 | 13.9 | 18.1 | 0 / 1773 / 0 |
| LC: PCH on, sccache, empty cache | noop | 0.1 | 1 | 0 | 1.0 | 0.0 | 30.2 | 0 / 0 / 0 |
| LC: PCH on, sccache, empty cache | source | 5.4 | 156 | 80 | 15.0 | 1.8 | 28.5 | 1 / 0 / 0 |
| LC: PCH on, sccache, empty cache | header | 5.7 | 203 | 85 | 15.0 | 1.7 | 28.6 | 38 / 0 / 0 |
| LC: PCH on, sccache, empty cache | pch-header | 21.3 | 1849 | 354 | 15.7 | 1.9 | 28.7 | 1601 / 0 / 0 |
| LC: PCH on, sccache, empty cache | link | 0.7 | 2 | 1 | 1.0 | 0.0 | 30.2 | 0 / 0 / 0 |
| LD: PCH on, sccache, warm cache, other directory | configure (network) | 103.4 | 0 | 0 | 0.0 | 0.6 | 29.9 | 0 / 0 / 0 |
| LD: PCH on, sccache, warm cache, other directory | cold | 158.1 | 2086 | 2624 | 15.9 | 14.0 | 17.9 | 0 / 1773 / 0 |
| LD: PCH on, sccache, warm cache, other directory | noop | 0.1 | 1 | 0 | 1.0 | 0.0 | 30.2 | 0 / 0 / 0 |
| LD: PCH on, sccache, warm cache, other directory | source | 5.0 | 156 | 72 | 14.6 | 1.9 | 28.8 | 1 / 0 / 0 |
| LD: PCH on, sccache, warm cache, other directory | link | 0.9 | 2 | 1 | 1.0 | 0.0 | 30.2 | 0 / 0 / 0 |
| LD2: PCH on, sccache, warm cache, same directory | configure (network) | 118.9 | 0 | 0 | 0.0 | 0.7 | 29.8 | 0 / 0 / 0 |
| LD2: PCH on, sccache, warm cache, same directory | cold | 20.3 | 2086 | 329 | 15.2 | 2.0 | 28.6 | 1773 / 0 / 0 |
| LD2: PCH on, sccache, warm cache, same directory | noop | 0.1 | 1 | 0 | 1.0 | 0.0 | 30.2 | 0 / 0 / 0 |
| LD2: PCH on, sccache, warm cache, same directory | source | 4.7 | 156 | 71 | 15.2 | 1.0 | 29.3 | 1 / 0 / 0 |
| LD2: PCH on, sccache, warm cache, same directory | header | 5.0 | 203 | 76 | 15.4 | 1.2 | 29.1 | 38 / 0 / 0 |
| LD2: PCH on, sccache, warm cache, same directory | link | 0.7 | 2 | 1 | 1.0 | 0.0 | 30.2 | 0 / 0 / 0 |

Slowest cold edges: the `crd-hesap-fft-tests` units again (`test_fft`, `test_dct`, `test_nufft` at 23 to 26 s) and
`fft.cpp` (20 to 22 s); a Debug tree is 4.5 GiB; the cache holds the preset in 0.38 GiB (0.76 GiB after the edit
rows and the second miss board).

Linux verdicts:

1. **The precompiled header stays on and is cacheable**: 1,773 compiles through the launcher, none refused, no
   non-cacheable reason on any row.
2. **A miss costs an eighth** (160.5 s against 142.8 s cold, +12 %).
3. **An edit that leaves the preprocessed unit unchanged hits on GCC.** The appended comment in the `source`,
   `header` and `pch-header` rows hit 1, 38 and 1,601 units on the empty-cache board (the cold build had stored
   them), so the PCH-header edit took 21.3 s instead of 139.6 s and the header edit 5.7 s instead of 10.6 s. On MSVC
   the same comment edit missed every touched unit (board C), so the preprocessed unit differs there; a real code
   edit misses the touched unit on both compilers and hits the rest.
4. **The cache key includes the build directory here too**: LD, in another directory, missed all 1,773 units
   (`-I<build>/…`, `-include <build>/…/cmake_pch.hxx` and the `-ffile-prefix-map` all name the tree); LD2, in LC's
   directory, hit all 1,773 and rebuilt the tree in 20.3 s, 7.0× faster than the uncached cold build, at 2.0 GiB
   of pressure instead of 14.0 GiB.
5. **Memory is the bound on Linux**: 14.0 GiB peak pressure at 16 jobs, about 0.88 GiB per compile, against
   5.8 GiB on MSVC. A 16 GiB host running 16 GCC jobs would swap; the derived bound is
   `CRD_COMPILE_JOBS = (available GiB - 4) / 0.9` (12 on such a host); the hosted 16 GiB, 4-vCPU runner uses
   4 jobs (about 3.5 GiB) and needs no pool.


## Decision carried to the contract

Hosted `win-debug` builds through the pinned sccache with `CRD_ENABLE_PCH=OFF` (qualified: no PCH penalty, +20 % on
the first miss, 3.1× on a full hit, identical test results on the fixed units); the local default configuration is
unchanged; `CRD_COMPILE_JOBS`/`CRD_LINK_JOBS` exist for memory-bound hosts and stay unset by default. See
[build performance](../design/build-performance.md) for the lane list and the extension rule.
