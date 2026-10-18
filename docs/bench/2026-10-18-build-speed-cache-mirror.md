# 2026-10-18 — build speed, task A: the pinned sccache on local and hosted lanes, the WSL source mirror

<!-- doc-role: evidence -->
> Dated evidence; counts and results are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).
> Contract: [build performance](../design/build-performance.md); session:
> [2026-10-18](../sessions/2026-10-18-diag-11b-build-speed-cache-mirror.md); owner row:
> [DIAG.11b](../ROADMAP.md#slice-diag.11b) (focused local routes and CI cost).

- **Machine/config:** Intel i9-14900K (32 logical CPUs), 63.7 GiB RAM, Windows 11 10.0.26200; MSVC 19.51.36246
  (VS 18 Community, toolset 14.51.36231), clang-cl 20.1.8, CMake 4.3.2, Ninja 1.13.2. WSL2 Ubuntu 24.04 on the same
  CPU (31.2 GiB, kernel 6.6.87.2), GCC 13.3.0, Clang 18.1.3, CMake 3.28.3, Ninja 1.11.1. sccache 0.17.0, the pinned
  binaries of `cmake/pins.json` (`install-sccache.py`). Revision: HEAD `b5f2446e` plus this batch's working tree.
- **Harness:** `scripts/build-bench.py run --preset <p> --jobs 16 --build-dir <fresh scratch> --label … --rows …
  --tooth engine/foundation/memory/include/crd/memory/construct.hpp "return nullptr; // never placement-new into a
  refused allocation" "return static_cast<T*>(nullptr); // never placement-new into a refused allocation"
  --define CRD_INPUT_ARCHIVES=<the verified archives a first configure downloaded>` (so no row downloads), with
  `--define CRD_ENABLE_PCH=OFF --define CRD_COMPILER_LAUNCHER=<sccache> --sccache <sccache>` and a fresh
  `SCCACHE_DIR` per lane for the cached boards. Windows boards run from `scripts/msvc-env.bat`; WSL boards from
  `/mnt/d/Dev/cerid` (drvfs) or the mirror `~/cerid-src`. One heavy job at a time, each board started detached
  through `wait-for.py start` and waited for with `wait-for.py exit-file`.
- **Rows:** `cold` (fresh tree), `noop`, `source` (a comment appended to `logger.cpp`), and the teeth cycle:
  `teeth-break` replaces one `return nullptr;` in `construct.hpp` (included by 43 files directly, about 1,450
  compiles of the tree) and rebuilds, `teeth-restore` writes the original bytes back (a new time, as a restored
  tooth is) and rebuilds; the cached boards run the cycle twice (the second break is the same edit again). Board
  letters: A the default (PCH on, uncached), B PCH off uncached (MSVC), C the cached configuration on an empty cache,
  D the same configuration rebuilt in C's directory on C's warm cache. On WSL: a from drvfs, b from the mirror
  uncached, c from the mirror cached on an empty cache, d warm in c's directory. Wall time is the measure; "Edges",
  "Ninja span", "Edge sum" and "Parallelism" come from the row's Ninja log lines (a row in which Ninja rewrote its
  log is marked; three earlier rows show 0 edges for that reason, before the runner learned to detect it).

## Verdict

- **WSL source mirror.** The same `linux-gcc-debug` board from `/mnt/d` against `~/cerid-src`, both uncached:
  cold 1,131.9 → 178.7 s (6.3×), no-op 50.4 → 0.1 s, the header teeth cycle 1,056.2 / 1,033.6 → 173.7 / 165.2 s
  (6.1-6.3×), configure 185.8 → 112.6 s. drvfs costs milliseconds per file open and stat; GCC reads hundreds of
  headers per unit.
- **The cache on the mirror.** A miss costs +12 to +18 % (cold 178.7 → 202.9 s on gcc-debug, 565.7 → 665.5 s on
  gcc-asan, 160.7 → 178.9 s on clang-tsan); the restore of a teeth cycle is a hit: 25.2 s against 165.2 s
  (gcc-debug, 6.6×), 35.2 s against about 550 s (gcc-asan), 23.2 s against about 160 s (clang-tsan); a warm full
  rebuild 9.8 s (gcc-debug, 18× the mirror cold, 115× the drvfs cold), 30.2 s (gcc-asan), 25.0 s (clang-tsan).
  Together: the loop's teeth restore on linux-gcc-debug went from 1,033.6 s to 25.2 s (41×).
- **The cache on Windows.** win-debug: a miss costs +34 % against the PCH-off build (230.0 against 171.8 s) and +24 %
  against today's default (185.0 s); the teeth restore is 37.0 s against 141.1 s (default) and 154.2 s (PCH off),
  3.8-4.2×, and the repeated cycle stays at 37-43 s; a warm full rebuild 59.2 s (3.1× the default cold). win-asan:
  restore 40.7 s against 147.5-161.5 s; warm rebuild 47.1 s against 197.0 s (4.2×). win-clang-cl-shipping (focused:
  the memory, scene and containers tests): restore 7.5 s against 12.5 s, warm 11.4 s against 21.1 s, miss +44-51 %.
- **Kept uncached by measurement:** MSVC LTCG. win-shipping on the same focused route: restore 20.3 s against 24.4 s
  (-17 %), warm 18.8 s against 31.7 s, miss +42 % (44.9 against 31.7 s cold); the link generates the code and is
  never cached, and converting a tree rebuilds every LTCG program, so the helpers leave win-release,
  win-relwithdebinfo, win-shipping and win-shipping-profile uncached (an explicit `-DCRD_COMPILER_LAUNCHER` still
  caches them, `/Z7`, proved below).
- **A fault found and repaired by the boards:** the first warm clang-tsan board hit 1,810 and missed 249 of 2,059
  compiles, because Clang writes every input's modification time into a precompiled header and sccache hashes the
  `.pch`; under the launcher the module now adds `-Xclang -fno-pch-timestamp` and the rerun hit 2,059 of 2,059.
- **Debugging:** cdb (WinDbg package) on `crd-memory-tests.exe` built entirely from cache hits (win-debug board D)
  breaks on `LinearAllocator::allocate`, prints the stack with `linear_allocator.cpp @ 56`,
  `test_allocator_asan.cpp @ 206` and the Catch2 frames, the source lines, the typed locals and `ln`, identical
  (frame offsets included) to the default uncached build's session; the cached `/Z7` win-shipping executable
  resolves the same frames with their inline Catch2 frames and source lines.

## Windows boards (16 jobs)

- **win-debug A: PCH on, uncached (default)**: preset `win-debug`, jobs 16, defines `CRD_INPUT_ARCHIVES=<archives>`; MSVC 19.51.36246.0; PCH ON; launcher `none`; tree 14.5 GiB.
- **win-debug B: PCH off, uncached**: preset `win-debug`, jobs 16, defines `CRD_INPUT_ARCHIVES=<archives> CRD_ENABLE_PCH=OFF`; MSVC 19.51.36246.0; PCH OFF; launcher `none`; tree 13.1 GiB.
- **win-debug C: PCH off, sccache, empty cache**: preset `win-debug`, jobs 16, defines `CRD_INPUT_ARCHIVES=<archives> CRD_ENABLE_PCH=OFF CRD_COMPILER_LAUNCHER=<sccache>`; MSVC 19.51.36246.0; PCH OFF; launcher `<sccache>`; tree 14.1 GiB; cache 0.8 GiB.
- **win-debug D: PCH off, sccache, warm cache, same directory**: preset `win-debug`, jobs 16, defines `CRD_INPUT_ARCHIVES=<archives> CRD_ENABLE_PCH=OFF CRD_COMPILER_LAUNCHER=<sccache>`; MSVC 19.51.36246.0; PCH OFF; launcher `<sccache>`; tree 10.6 GiB; cache 0.8 GiB.
- **win-asan A: PCH on, uncached (default)**: preset `win-asan`, jobs 16, defines `CRD_INPUT_ARCHIVES=<archives>`; MSVC 19.51.36246.0; PCH ON; launcher `none`; tree 8.2 GiB.
- **win-asan C: PCH off, sccache, empty cache**: preset `win-asan`, jobs 16, defines `CRD_INPUT_ARCHIVES=<archives> CRD_ENABLE_PCH=OFF CRD_COMPILER_LAUNCHER=<sccache>`; MSVC 19.51.36246.0; PCH OFF; launcher `<sccache>`; tree 7.3 GiB; cache 0.9 GiB.
- **win-asan D: PCH off, sccache, warm cache, same directory**: preset `win-asan`, jobs 16, defines `CRD_INPUT_ARCHIVES=<archives> CRD_ENABLE_PCH=OFF CRD_COMPILER_LAUNCHER=<sccache>`; MSVC 19.51.36246.0; PCH OFF; launcher `<sccache>`; tree 7.2 GiB; cache 0.9 GiB.
- **win-shipping A: PCH on, uncached (default)**: preset `win-shipping`, jobs 16, defines `CRD_INPUT_ARCHIVES=<archives>`; MSVC 19.51.36246.0; PCH ON; launcher `none`; tree 1.1 GiB.
- **win-shipping C: PCH off, sccache, empty cache**: preset `win-shipping`, jobs 16, defines `CRD_INPUT_ARCHIVES=<archives> CRD_ENABLE_PCH=OFF CRD_COMPILER_LAUNCHER=<sccache>`; MSVC 19.51.36246.0; PCH OFF; launcher `<sccache>`; tree 1.1 GiB; cache 0.2 GiB.
- **win-shipping D: PCH off, sccache, warm cache, same directory**: preset `win-shipping`, jobs 16, defines `CRD_INPUT_ARCHIVES=<archives> CRD_ENABLE_PCH=OFF CRD_COMPILER_LAUNCHER=<sccache>`; MSVC 19.51.36246.0; PCH OFF; launcher `<sccache>`; tree 1.1 GiB; cache 0.2 GiB.
- **win-clang-cl-shipping A: PCH on, uncached (default)**: preset `win-clang-cl-shipping`, jobs 16, defines `CRD_INPUT_ARCHIVES=<archives>`; Clang 20.1.8; PCH ON; launcher `none`; tree 0.2 GiB.
- **win-clang-cl-shipping C: PCH off, sccache, empty cache**: preset `win-clang-cl-shipping`, jobs 16, defines `CRD_INPUT_ARCHIVES=<archives> CRD_ENABLE_PCH=OFF CRD_COMPILER_LAUNCHER=<sccache>`; Clang 20.1.8; PCH OFF; launcher `<sccache>`; tree 0.2 GiB; cache 0.1 GiB.
- **win-clang-cl-shipping D: PCH off, sccache, warm cache, same directory**: preset `win-clang-cl-shipping`, jobs 16, defines `CRD_INPUT_ARCHIVES=<archives> CRD_ENABLE_PCH=OFF CRD_COMPILER_LAUNCHER=<sccache>`; Clang 20.1.8; PCH OFF; launcher `<sccache>`; tree 0.2 GiB; cache 0.1 GiB.

| Board | Row | Wall s | Edges | Ninja span s | Edge sum s | Parallelism | Pressure GiB | Min avail GiB | sccache hits / misses / non-cacheable |
|---|---|---|---|---|---|---|---|---|---|
| win-debug A: PCH on, uncached (default) | configure | 28.4 | 0 | 0.0 | 0 | 0.0 | 0.1 | 33.3 | uncached |
| win-debug A: PCH on, uncached (default) | cold | 185.0 | 2450 | 184.7 | 2947 | 16.0 | 4.1 | 26.1 | uncached |
| win-debug A: PCH on, uncached (default) | noop | 20.7 | 352 | 20.0 | 6297 | 315.6 | 0.1 | 29.2 | uncached |
| win-debug A: PCH on, uncached (default) | source | 5.6 | 208 | 5.3 | 63 | 11.9 | 1.6 | 30.0 | uncached |
| win-debug A: PCH on, uncached (default) | teeth-break | 140.5 | 1751 | 140.2 | 2234 | 15.9 | 2.3 | 27.7 | uncached |
| win-debug A: PCH on, uncached (default) | teeth-restore | 141.1 | 1751 | 140.8 | 2244 | 15.9 | 2.3 | 27.7 | uncached |
| win-debug A: PCH on, uncached (default) | teeth-break | 141.3 | 1751 | 141.0 | 2247 | 15.9 | 2.3 | 27.2 | uncached |
| win-debug A: PCH on, uncached (default) | teeth-restore | 142.0 | 1751 | 141.6 | 2257 | 15.9 | 2.2 | 27.1 | uncached |
| win-debug B: PCH off, uncached | configure | 28.6 | 0 | 0.0 | 0 | 0.0 | 0.0 | 30.7 | uncached |
| win-debug B: PCH off, uncached | cold | 171.8 | 2193 | 171.5 | 2736 | 15.9 | 5.1 | 25.8 | uncached |
| win-debug B: PCH off, uncached | noop | 0.6 | 1 | 0.1 | 0 | 1.0 | 0.0 | 28.0 | uncached |
| win-debug B: PCH off, uncached | source | 5.5 | 208 | 5.2 | 63 | 12.1 | 1.5 | 26.9 | uncached |
| win-debug B: PCH off, uncached | teeth-break | 142.8 | 1751 | 142.4 | 2271 | 15.9 | 2.0 | 26.0 | uncached |
| win-debug B: PCH off, uncached | teeth-restore | 154.2 | 1751 | 153.8 | 2453 | 15.9 | 3.1 | 27.6 | uncached |
| win-debug B: PCH off, uncached | teeth-break | 141.2 | 1751 | 140.9 | 2247 | 15.9 | 2.4 | 27.8 | uncached |
| win-debug B: PCH off, uncached | teeth-restore | 143.4 | 0 | 0.0 | 0 | 0.0 | 3.6 | 27.4 | uncached |
| win-debug C: PCH off, sccache, empty cache | configure | 35.9 | 0 | 0.0 | 0 | 0.0 | 0.5 | 29.5 | 0 / 0 / 0 |
| win-debug C: PCH off, sccache, empty cache | cold | 230.0 | 2193 | 229.5 | 3660 | 15.9 | 4.2 | 26.0 | 0 / 1850 / 0 |
| win-debug C: PCH off, sccache, empty cache | noop | 0.6 | 1 | 0.1 | 0 | 1.0 | 0.0 | 28.5 | 0 / 0 / 0 |
| win-debug C: PCH off, sccache, empty cache | source | 5.7 | 208 | 5.4 | 61 | 11.3 | 1.2 | 27.8 | 0 / 1 / 0 |
| win-debug C: PCH off, sccache, empty cache | teeth-break | 183.7 | 1751 | 183.4 | 2925 | 15.9 | 3.1 | 27.2 | 0 / 1451 / 0 |
| win-debug C: PCH off, sccache, empty cache | teeth-restore | 37.0 | 1751 | 36.6 | 578 | 15.8 | 1.7 | 27.9 | 1451 / 0 / 0 |
| win-debug C: PCH off, sccache, empty cache | teeth-break | 37.5 | 1751 | 37.1 | 584 | 15.8 | 2.4 | 26.2 | 1451 / 0 / 0 |
| win-debug C: PCH off, sccache, empty cache | teeth-restore | 42.5 | 0 | 0.0 | 0 | 0.0 | 2.6 | 25.8 | 1451 / 0 / 0 |
| win-debug D: PCH off, sccache, warm cache, same directory | configure | 28.8 | 0 | 0.0 | 0 | 0.0 | 0.1 | 32.4 | 0 / 0 / 0 |
| win-debug D: PCH off, sccache, warm cache, same directory | cold | 59.2 | 2193 | 58.9 | 930 | 15.8 | 4.4 | 24.3 | 1850 / 0 / 0 |
| win-debug D: PCH off, sccache, warm cache, same directory | noop | 1.0 | 1 | 0.1 | 0 | 1.0 | 0.3 | 26.9 | 0 / 0 / 0 |
| win-asan A: PCH on, uncached (default) | configure | 30.4 | 0 | 0.0 | 0 | 0.0 | 1.3 | 31.2 | uncached |
| win-asan A: PCH on, uncached (default) | cold | 197.0 | 2450 | 196.7 | 3148 | 16.0 | 3.5 | 26.4 | uncached |
| win-asan A: PCH on, uncached (default) | noop | 0.6 | 1 | 0.1 | 0 | 1.0 | 0.0 | 30.3 | uncached |
| win-asan A: PCH on, uncached (default) | source | 11.3 | 208 | 11.0 | 120 | 10.9 | 4.5 | 26.5 | uncached |
| win-asan A: PCH on, uncached (default) | teeth-break | 162.5 | 1751 | 162.1 | 2592 | 16.0 | 3.5 | 28.0 | uncached |
| win-asan A: PCH on, uncached (default) | teeth-restore | 147.5 | 1751 | 147.1 | 2352 | 16.0 | 3.2 | 27.6 | uncached |
| win-asan A: PCH on, uncached (default) | teeth-break | 152.1 | 1751 | 151.7 | 2426 | 16.0 | 3.1 | 27.9 | uncached |
| win-asan A: PCH on, uncached (default) | teeth-restore | 161.5 | 1751 (recompacted log) | n/a | 2576 | n/a | 3.7 | 28.0 | uncached |
| win-asan C: PCH off, sccache, empty cache | configure | 30.4 | 0 | 0.0 | 0 | 0.0 | 2.0 | 29.5 | 0 / 0 / 0 |
| win-asan C: PCH off, sccache, empty cache | cold | 224.8 | 2193 | 224.4 | 3595 | 16.0 | 4.3 | 26.7 | 0 / 1850 / 0 |
| win-asan C: PCH off, sccache, empty cache | noop | 0.6 | 1 | 0.1 | 0 | 1.0 | 0.0 | 30.5 | 0 / 0 / 0 |
| win-asan C: PCH off, sccache, empty cache | source | 12.5 | 208 | 12.2 | 136 | 11.2 | 4.7 | 26.4 | 0 / 1 / 0 |
| win-asan C: PCH off, sccache, empty cache | teeth-break | 181.5 | 1751 | 181.1 | 2899 | 16.0 | 3.2 | 28.2 | 0 / 1451 / 0 |
| win-asan C: PCH off, sccache, empty cache | teeth-restore | 40.7 | 1751 | 40.4 | 646 | 16.0 | 3.8 | 26.8 | 1451 / 0 / 0 |
| win-asan C: PCH off, sccache, empty cache | teeth-break | 40.8 | 1751 | 40.4 | 647 | 16.0 | 4.2 | 26.0 | 1451 / 0 / 0 |
| win-asan C: PCH off, sccache, empty cache | teeth-restore | 40.4 | 1751 (recompacted log) | n/a | 641 | n/a | 4.0 | 26.4 | 1451 / 0 / 0 |
| win-asan D: PCH off, sccache, warm cache, same directory | configure | 30.1 | 0 | 0.0 | 0 | 0.0 | 2.9 | 29.3 | 0 / 0 / 0 |
| win-asan D: PCH off, sccache, warm cache, same directory | cold | 47.1 | 2193 | 46.8 | 752 | 16.1 | 4.5 | 26.8 | 1850 / 0 / 0 |
| win-asan D: PCH off, sccache, warm cache, same directory | noop | 0.6 | 1 | 0.1 | 0 | 1.0 | 0.0 | 30.0 | 0 / 0 / 0 |
| win-shipping A: PCH on, uncached (default) | configure | 34.3 | 0 | 0.0 | 0 | 0.0 | 1.9 | 30.5 | uncached |
| win-shipping A: PCH on, uncached (default) | cold | 31.7 | 380 | 31.2 | 350 | 11.2 | 1.5 | 29.4 | uncached |
| win-shipping A: PCH on, uncached (default) | noop | 0.5 | 1 | 0.1 | 0 | 1.0 | 0.2 | 30.9 | uncached |
| win-shipping A: PCH on, uncached (default) | teeth-break | 28.4 | 141 | 28.0 | 203 | 7.2 | 2.4 | 28.6 | uncached |
| win-shipping A: PCH on, uncached (default) | teeth-restore | 24.4 | 141 | 24.0 | 190 | 7.9 | 1.3 | 29.3 | uncached |
| win-shipping C: PCH off, sccache, empty cache | configure | 30.2 | 0 | 0.0 | 0 | 0.0 | 4.1 | 28.3 | 0 / 0 / 0 |
| win-shipping C: PCH off, sccache, empty cache | cold | 44.9 | 361 | 44.4 | 483 | 10.9 | 2.4 | 26.7 | 0 / 331 / 0 |
| win-shipping C: PCH off, sccache, empty cache | noop | 0.4 | 1 | 0.1 | 0 | 1.0 | 0.0 | 28.8 | 0 / 0 / 0 |
| win-shipping C: PCH off, sccache, empty cache | teeth-break | 28.3 | 141 | 28.0 | 214 | 7.6 | 1.7 | 27.5 | 0 / 120 / 0 |
| win-shipping C: PCH off, sccache, empty cache | teeth-restore | 20.3 | 141 | 19.9 | 87 | 4.4 | 2.0 | 28.9 | 120 / 0 / 0 |
| win-shipping D: PCH off, sccache, warm cache, same directory | configure | 27.7 | 0 | 0.0 | 0 | 0.0 | 1.2 | 31.1 | 0 / 0 / 0 |
| win-shipping D: PCH off, sccache, warm cache, same directory | cold | 18.8 | 361 | 18.4 | 118 | 6.4 | 1.6 | 30.1 | 331 / 0 / 0 |
| win-shipping D: PCH off, sccache, warm cache, same directory | noop | 0.4 | 1 | 0.1 | 0 | 1.0 | 0.0 | 32.0 | 0 / 0 / 0 |
| win-clang-cl-shipping A: PCH on, uncached (default) | configure | 34.2 | 0 | 0.0 | 0 | 0.0 | 0.1 | 32.2 | uncached |
| win-clang-cl-shipping A: PCH on, uncached (default) | cold | 21.1 | 380 | 20.8 | 298 | 14.3 | 3.5 | 29.1 | uncached |
| win-clang-cl-shipping A: PCH on, uncached (default) | noop | 0.4 | 1 | 0.1 | 0 | 1.0 | 0.0 | 32.1 | uncached |
| win-clang-cl-shipping A: PCH on, uncached (default) | teeth-break | 12.5 | 141 | 12.1 | 147 | 12.1 | 3.0 | 29.5 | uncached |
| win-clang-cl-shipping A: PCH on, uncached (default) | teeth-restore | 12.5 | 141 | 12.2 | 147 | 12.0 | 2.9 | 29.7 | uncached |
| win-clang-cl-shipping C: PCH off, sccache, empty cache | configure | 48.8 | 0 | 0.0 | 0 | 0.0 | 3.8 | 25.3 | 0 / 0 / 0 |
| win-clang-cl-shipping C: PCH off, sccache, empty cache | cold | 31.9 | 361 | 31.6 | 460 | 14.6 | 3.4 | 28.2 | 0 / 331 / 0 |
| win-clang-cl-shipping C: PCH off, sccache, empty cache | noop | 0.4 | 1 | 0.1 | 0 | 1.0 | 0.0 | 31.2 | 0 / 0 / 0 |
| win-clang-cl-shipping C: PCH off, sccache, empty cache | teeth-break | 18.0 | 141 | 17.7 | 226 | 12.8 | 2.8 | 29.1 | 0 / 120 / 0 |
| win-clang-cl-shipping C: PCH off, sccache, empty cache | teeth-restore | 7.5 | 141 | 7.2 | 66 | 9.1 | 2.8 | 29.3 | 120 / 0 / 0 |
| win-clang-cl-shipping D: PCH off, sccache, warm cache, same directory | configure | 33.2 | 0 | 0.0 | 0 | 0.0 | 0.2 | 31.6 | 0 / 0 / 0 |
| win-clang-cl-shipping D: PCH off, sccache, warm cache, same directory | cold | 11.4 | 361 | 11.1 | 127 | 11.5 | 3.0 | 29.2 | 331 / 0 / 0 |
| win-clang-cl-shipping D: PCH off, sccache, warm cache, same directory | noop | 0.4 | 1 | 0.1 | 0 | 1.0 | 0.0 | 31.6 | 0 / 0 / 0 |

The shipping boards build only `crd-memory-tests`, `crd-scene-tests` and `crd-containers-tests` (`--targets`), the
shape of a focused local route, and run one teeth cycle.

## WSL boards (16 jobs)

- **linux-gcc-debug a: drvfs source, uncached**: preset `linux-gcc-debug`, jobs 16, defines `CRD_INPUT_ARCHIVES=<archives>`; GNU 13.3.0; PCH ON; launcher `none`; tree 5.1 GiB.
- **linux-gcc-debug b: mirror source, uncached**: preset `linux-gcc-debug`, jobs 16, defines `CRD_INPUT_ARCHIVES=<archives>`; GNU 13.3.0; PCH ON; launcher `none`; tree 5.1 GiB.
- **linux-gcc-debug c: mirror source, sccache**: preset `linux-gcc-debug`, jobs 16, defines `CRD_INPUT_ARCHIVES=<archives> CRD_COMPILER_LAUNCHER=<sccache>`; GNU 13.3.0; PCH ON; launcher `<sccache>`; tree 5.1 GiB; cache 0.7 GiB.
- **linux-gcc-debug d: mirror source, sccache**: preset `linux-gcc-debug`, jobs 16, defines `CRD_INPUT_ARCHIVES=<archives> CRD_COMPILER_LAUNCHER=<sccache>`; GNU 13.3.0; PCH ON; launcher `<sccache>`; tree 5.1 GiB; cache 0.7 GiB.
- **linux-gcc-asan b: mirror source, uncached**: preset `linux-gcc-asan`, jobs 16, defines `CRD_INPUT_ARCHIVES=<archives>`; GNU 13.3.0; PCH ON; launcher `none`; tree 15.5 GiB.
- **linux-gcc-asan c: mirror source, sccache**: preset `linux-gcc-asan`, jobs 16, defines `CRD_INPUT_ARCHIVES=<archives> CRD_COMPILER_LAUNCHER=<sccache>`; GNU 13.3.0; PCH ON; launcher `<sccache>`; tree 15.5 GiB; cache 1.1 GiB.
- **linux-gcc-asan d: mirror source, sccache**: preset `linux-gcc-asan`, jobs 16, defines `CRD_INPUT_ARCHIVES=<archives> CRD_COMPILER_LAUNCHER=<sccache>`; GNU 13.3.0; PCH ON; launcher `<sccache>`; tree 15.5 GiB; cache 1.1 GiB.
- **linux-clang-tsan b: mirror source, uncached**: preset `linux-clang-tsan`, jobs 16, defines `CRD_INPUT_ARCHIVES=<archives>`; Clang 18.1.3; PCH ON; launcher `none`; tree 4.4 GiB.
- **linux-clang-tsan c: mirror source, sccache (PCH times kept, before the fix)**: preset `linux-clang-tsan`, jobs 16, defines `CRD_INPUT_ARCHIVES=<archives> CRD_COMPILER_LAUNCHER=<sccache>`; Clang 18.1.3; PCH ON; launcher `<sccache>`; tree 4.4 GiB; cache 0.6 GiB.
- **linux-clang-tsan d: mirror source, sccache (PCH times kept, before the fix)**: preset `linux-clang-tsan`, jobs 16, defines `CRD_INPUT_ARCHIVES=<archives> CRD_COMPILER_LAUNCHER=<sccache>`; Clang 18.1.3; PCH ON; launcher `<sccache>`; tree 4.4 GiB; cache 0.6 GiB.
- **linux-clang-tsan c: mirror source, sccache**: preset `linux-clang-tsan`, jobs 16, defines `CRD_INPUT_ARCHIVES=<archives> CRD_COMPILER_LAUNCHER=<sccache>`; Clang 18.1.3; PCH ON; launcher `<sccache>`; tree 4.4 GiB; cache 0.6 GiB.
- **linux-clang-tsan d: mirror source, sccache**: preset `linux-clang-tsan`, jobs 16, defines `CRD_INPUT_ARCHIVES=<archives> CRD_COMPILER_LAUNCHER=<sccache>`; Clang 18.1.3; PCH ON; launcher `<sccache>`; tree 4.4 GiB; cache 0.6 GiB.

| Board | Row | Wall s | Edges | Ninja span s | Edge sum s | Parallelism | Pressure GiB | Min avail GiB | sccache hits / misses / non-cacheable |
|---|---|---|---|---|---|---|---|---|---|
| linux-gcc-debug a: drvfs source, uncached | configure | 185.8 | 0 | 0.0 | 0 | 0.0 | 0.7 | 29.9 | uncached |
| linux-gcc-debug a: drvfs source, uncached | cold | 1131.9 | 2429 | 1131.6 | 16887 | 14.9 | 7.5 | 23.6 | uncached |
| linux-gcc-debug a: drvfs source, uncached | noop | 50.4 | 1 | 43.6 | 44 | 1.0 | 0.0 | 30.2 | uncached |
| linux-gcc-debug a: drvfs source, uncached | source | 57.9 | 202 | 57.5 | 161 | 2.8 | 1.6 | 28.9 | uncached |
| linux-gcc-debug a: drvfs source, uncached | teeth-break | 1056.2 | 1706 | 1055.8 | 15923 | 15.1 | 7.6 | 23.4 | uncached |
| linux-gcc-debug a: drvfs source, uncached | teeth-restore | 1033.6 | 1706 | 1033.0 | 15363 | 14.9 | 6.5 | 24.7 | uncached |
| linux-gcc-debug b: mirror source, uncached | configure | 112.6 | 0 | 0.0 | 0 | 0.0 | 0.5 | 29.9 | uncached |
| linux-gcc-debug b: mirror source, uncached | cold | 178.7 | 2429 | 178.7 | 2844 | 15.9 | 13.4 | 18.4 | uncached |
| linux-gcc-debug b: mirror source, uncached | noop | 0.1 | 1 | 0.0 | 0 | 1.0 | 0.0 | 30.3 | uncached |
| linux-gcc-debug b: mirror source, uncached | source | 8.1 | 202 | 8.0 | 106 | 13.2 | 1.3 | 29.2 | uncached |
| linux-gcc-debug b: mirror source, uncached | teeth-break | 173.7 | 1706 | 173.7 | 2759 | 15.9 | 13.3 | 18.5 | uncached |
| linux-gcc-debug b: mirror source, uncached | teeth-restore | 165.2 | 1706 | 165.2 | 2620 | 15.9 | 13.7 | 18.1 | uncached |
| linux-gcc-debug c: mirror source, sccache | configure | 112.4 | 0 | 0.0 | 0 | 0.0 | 0.5 | 29.8 | 0 / 0 / 0 |
| linux-gcc-debug c: mirror source, sccache | cold | 202.9 | 2429 | 202.9 | 3227 | 15.9 | 13.2 | 18.6 | 4 / 2055 / 0 |
| linux-gcc-debug c: mirror source, sccache | noop | 0.1 | 1 | 0.0 | 0 | 1.0 | 0.0 | 30.2 | 0 / 0 / 0 |
| linux-gcc-debug c: mirror source, sccache | source | 7.2 | 202 | 7.2 | 106 | 14.8 | 1.9 | 28.9 | 1 / 0 / 0 |
| linux-gcc-debug c: mirror source, sccache | teeth-break | 194.7 | 1706 | 194.7 | 3091 | 15.9 | 13.5 | 18.3 | 3 / 1412 / 0 |
| linux-gcc-debug c: mirror source, sccache | teeth-restore | 25.2 | 1706 | 25.1 | 392 | 15.6 | 1.8 | 28.8 | 1415 / 0 / 0 |
| linux-gcc-debug c: mirror source, sccache | teeth-break | 24.8 | 1706 | 24.7 | 386 | 15.6 | 1.5 | 29.1 | 1415 / 0 / 0 |
| linux-gcc-debug c: mirror source, sccache | teeth-restore | 24.8 | 1706 (recompacted log) | n/a | 384 | n/a | 1.8 | 28.8 | 1415 / 0 / 0 |
| linux-gcc-debug d: mirror source, sccache | configure | 107.9 | 0 | 0.0 | 0 | 0.0 | 0.5 | 29.9 | 0 / 0 / 0 |
| linux-gcc-debug d: mirror source, sccache | cold | 9.8 | 2429 | 9.7 | 155 | 15.9 | 1.2 | 29.1 | 2059 / 0 / 0 |
| linux-gcc-debug d: mirror source, sccache | noop | 0.1 | 1 | 0.0 | 0 | 1.0 | 0.0 | 30.3 | 0 / 0 / 0 |
| linux-gcc-asan b: mirror source, uncached | configure | 105.8 | 0 | 0.0 | 0 | 0.0 | 0.6 | 29.9 | uncached |
| linux-gcc-asan b: mirror source, uncached | cold | 565.7 | 2429 | 565.7 | 7608 | 13.4 | 20.3 | 11.7 | uncached |
| linux-gcc-asan b: mirror source, uncached | noop | 0.1 | 1 | 0.0 | 0 | 1.0 | 0.0 | 30.2 | uncached |
| linux-gcc-asan c: mirror source, sccache | configure | 119.5 | 0 | 0.0 | 0 | 0.0 | 0.7 | 29.8 | 0 / 0 / 0 |
| linux-gcc-asan c: mirror source, sccache | cold | 665.5 | 2429 | 665.4 | 9218 | 13.8 | 20.7 | 11.3 | 8 / 2051 / 0 |
| linux-gcc-asan c: mirror source, sccache | noop | 0.1 | 1 | 0.0 | 0 | 1.0 | 0.0 | 30.2 | 0 / 0 / 0 |
| linux-gcc-asan c: mirror source, sccache | teeth-break | 547.2 | 1706 | 547.1 | 7194 | 13.2 | 20.5 | 11.6 | 3 / 1412 / 0 |
| linux-gcc-asan c: mirror source, sccache | teeth-restore | 35.2 | 1706 | 35.2 | 550 | 15.6 | 3.2 | 27.3 | 1415 / 0 / 0 |
| linux-gcc-asan d: mirror source, sccache | configure | 117.9 | 0 | 0.0 | 0 | 0.0 | 0.6 | 29.8 | 0 / 0 / 0 |
| linux-gcc-asan d: mirror source, sccache | cold | 30.2 | 2429 | 30.2 | 255 | 8.5 | 4.2 | 26.6 | 2059 / 0 / 0 |
| linux-gcc-asan d: mirror source, sccache | noop | 0.1 | 1 | 0.0 | 0 | 1.0 | 0.0 | 30.2 | 0 / 0 / 0 |
| linux-clang-tsan b: mirror source, uncached | configure | 112.4 | 0 | 0.0 | 0 | 0.0 | 0.4 | 29.9 | uncached |
| linux-clang-tsan b: mirror source, uncached | cold | 160.7 | 2429 | 160.6 | 2582 | 16.1 | 5.7 | 24.7 | uncached |
| linux-clang-tsan b: mirror source, uncached | noop | 0.1 | 1 | 0.0 | 0 | 1.0 | 0.0 | 30.2 | uncached |
| linux-clang-tsan c: mirror source, sccache (PCH times kept, before the fix) | configure | 120.1 | 0 | 0.0 | 0 | 0.0 | 0.5 | 29.9 | 0 / 0 / 0 |
| linux-clang-tsan c: mirror source, sccache (PCH times kept, before the fix) | cold | 180.0 | 2429 | 180.0 | 2886 | 16.0 | 5.7 | 24.8 | 6 / 2053 / 0 |
| linux-clang-tsan c: mirror source, sccache (PCH times kept, before the fix) | noop | 0.1 | 1 | 0.0 | 0 | 1.0 | 0.0 | 30.2 | 0 / 0 / 0 |
| linux-clang-tsan c: mirror source, sccache (PCH times kept, before the fix) | teeth-break | 160.7 | 1706 | 160.6 | 2556 | 15.9 | 6.1 | 24.4 | 3 / 1412 / 0 |
| linux-clang-tsan c: mirror source, sccache (PCH times kept, before the fix) | teeth-restore | 23.1 | 1706 | 23.0 | 366 | 15.9 | 1.4 | 28.8 | 1415 / 0 / 0 |
| linux-clang-tsan d: mirror source, sccache (PCH times kept, before the fix) | configure | 112.8 | 0 | 0.0 | 0 | 0.0 | 0.7 | 29.8 | 0 / 0 / 0 |
| linux-clang-tsan d: mirror source, sccache (PCH times kept, before the fix) | cold | 25.4 | 2429 | 25.4 | 204 | 8.0 | 2.1 | 28.3 | 1810 / 249 / 0 |
| linux-clang-tsan d: mirror source, sccache (PCH times kept, before the fix) | noop | 0.1 | 1 | 0.0 | 0 | 1.0 | 0.0 | 30.2 | 0 / 0 / 0 |
| linux-clang-tsan c: mirror source, sccache | configure | 108.1 | 0 | 0.0 | 0 | 0.0 | 0.7 | 29.9 | 0 / 0 / 0 |
| linux-clang-tsan c: mirror source, sccache | cold | 178.9 | 2429 | 178.9 | 2873 | 16.1 | 5.8 | 24.6 | 3 / 2056 / 0 |
| linux-clang-tsan c: mirror source, sccache | noop | 0.1 | 1 | 0.0 | 0 | 1.0 | 0.0 | 30.2 | 0 / 0 / 0 |
| linux-clang-tsan c: mirror source, sccache | teeth-break | 159.9 | 1706 | 159.8 | 2546 | 15.9 | 5.9 | 24.6 | 3 / 1412 / 0 |
| linux-clang-tsan c: mirror source, sccache | teeth-restore | 23.2 | 1706 | 23.1 | 368 | 15.9 | 1.6 | 28.7 | 1415 / 0 / 0 |
| linux-clang-tsan d: mirror source, sccache | configure | 104.6 | 0 | 0.0 | 0 | 0.0 | 0.7 | 29.8 | 0 / 0 / 0 |
| linux-clang-tsan d: mirror source, sccache | cold | 25.0 | 2429 | 24.9 | 195 | 7.8 | 1.9 | 28.7 | 2059 / 0 / 0 |
| linux-clang-tsan d: mirror source, sccache | noop | 0.1 | 1 | 0.0 | 0 | 1.0 | 0.0 | 30.2 | 0 / 0 / 0 |

The sanitizer boards skip the drvfs source (its cost is the same per-file cost as on gcc-debug) and run one teeth
cycle. The two clang-tsan boards marked "PCH times kept" ran before the module added `-Xclang -fno-pch-timestamp`:
the warm rebuild in c's directory hit 1,810 and missed 249 compiles, because the configure of the new tree rewrote
the generated headers and every precompiled header that includes one changed its recorded times; with the flag the
same rebuild hit 2,059 of 2,059.

## The loop's route: one teeth cycle across every lane a batch builds

`build/bench-1018/loop_route.py` (scratch) applies the same `construct.hpp` tooth and rebuilds `crd-memory-tests` on
every lane a DIAG batch builds, in turn, through the helpers the loop calls: `build-target.bat` at its default four
jobs on Windows, and on WSL the old route (`cmake --build ~/cerid-build/<preset> -j 16`, the tree configured from
`/mnt/d`) before and `wsl-lane.sh build <preset> crd-memory-tests` (sync, check, mirror tree, cache) after. Before: the
trees as they were (PCH on, uncached); after: win-debug, win-asan and win-clang-cl-shipping converted through
`configure-preset.bat`, win-shipping unchanged (an MSVC LTCG tree), the three WSL lanes on the mirror. "Break again"
and "restore again" repeat the same cycle: what a batch pays when a tooth is checked a second time, and the warm
rebuild of every lane after a change the caches have seen. Seconds:

| Lane | Before: no-op | break | restore | After: no-op | break (miss) | restore | break again | restore again |
|---|---|---|---|---|---|---|---|---|
| win-debug | 1.7 | 6.8 | 5.7 | 2.2 | 11.2 | 3.7 | 4.4 | 3.3 |
| win-asan | 1.9 | 6.7 | 5.8 | 4.2 | 6.9 | 4.1 | 4.8 | 3.5 |
| win-clang-cl-shipping | 2.7 | 9.1 | 7.1 | 4.0 | 9.5 | 5.2 | 6.2 | 5.2 |
| win-shipping | 2.0 | 10.6 | 10.4 | 2.1 | 11.6 | 10.8 | 12.3 | 10.6 |
| linux-gcc-debug | 72.4 | 89.2 | 83.6 | 3.4 | 11.2 | 6.0 | 4.7 | 4.7 |
| linux-gcc-asan | 79.8 | 81.8 | 81.8 | 3.7 | 9.9 | 5.9 | 4.5 | 5.0 |
| linux-clang-tsan | 80.7 | 74.1 | 76.4 | 3.8 | 7.2 | 5.4 | 4.6 | 5.5 |
| **all seven lanes** | **241.2** | **278.3** | **270.8** | **23.4** | **67.5** | **41.1** | **41.5** | **37.8** |

The cross-lane cycle went from 549.1 s (break 278.3 + restore 270.8) to 108.6 s on its first run after the
conversion (5.1×; the break is a miss) and 79.3 s when repeated (6.9×). The WSL lanes carry most of it: from `/mnt/d`
a no-op alone cost 72-81 s (the glob re-check and the Ninja stat walk over drvfs), from the mirror 3.4-3.8 s
including the sync and the check. On Windows a small target gains little: the restore 5.7 → 3.7 s on win-debug and
5.8 → 4.1 s on win-asan, and the miss after the conversion is slower than the uncached build was (11.2 against
6.8 s on win-debug: no precompiled header and the cache write).

## Converting the local trees

Each tree was reconfigured through `configure-preset.bat` (which printed the added entries) and rebuilt with
`build-target.bat <tree> all` at 16 jobs, on the empty local cache `build/sccache`:

| Tree | Configure s | Full rebuild s | Compiles (all misses) | Cache growth |
|---|---|---|---|---|
| build/win-debug | 22.4 | 206.5 | 1,850 | 442 MiB |
| build/win-asan | 22.0 | 208.8 | 1,850 | 510 MiB |
| build/win-clang-cl-shipping | 18.9 | 558.6 | 1,845 | 523 MiB |

The WSL lanes were configured fresh into `~/cerid-mirror-build/<preset>` by `wsl-lane.sh build <preset>` (sync,
check, configure, build) on the empty cache `~/.cache/cerid-sccache`: linux-gcc-debug 362 s, linux-gcc-asan 754 s
(its build completed; the helper then exited 2 because it had been edited under the running shell, and an immediate
rerun was a 6.0 s no-op), linux-clang-tsan 308 s; together 1.5 GiB of cache. win-shipping, win-release,
win-relwithdebinfo and win-shipping-profile were not converted.

## The mirror trees against the trees from `/mnt/d`

`build/bench-1018/wsl/equivalence.py` (scratch) compares an old tree (configured from `/mnt/d`, reconfigured by the
loop route's first build) with its mirror tree: the compile database with the source path, the tree path and a
leading launcher normalised, the CMake cache's typed entries, and the `ctest -N` inventory.

| Lane | Compile commands | Different beyond paths and launcher | Tests (old / new) |
|---|---|---|---|
| linux-gcc-debug | 2,043 / 2,043, the same units | 0 | 7,485 / 7,485, the same names |
| linux-gcc-asan | 2,043 / 2,043 | 0 | 7,482 / 7,482 |
| linux-clang-tsan | 2,043 / 2,043 | 2,042, by exactly two tokens: `-ferror-limit=0` in the old tree (a stale `CMAKE_CXX_FLAGS`/`CMAKE_C_FLAGS` cache entry of an earlier diagnostic configure; the mirror tree is the preset's) and this batch's `-Xclang -fno-pch-timestamp` in the new one | 7,482 / 7,482 |

The cache entries that differ are stale leftovers of older configurations in the old trees (toml++, zstd's legacy
level, SPIRV-Reflect paths, CTest's memory checker found earlier) and the compiler spelled as found (`g++` against
`/usr/bin/g++`); none reaches a compile command. Tests that read `CRD_REPO_DIR`, the assets and the scratch paths read
the mirror, whose bytes equal the checkout (`source-mirror.py check --full`, every file, 74 s).

The check's teeth on the real mirror (`build/bench-1018/staleness-teeth.txt`): after a sync `check` exits 0; an
edit of `engine/foundation/core/src/assert.cpp` on `/mnt/d` makes `check`, `test` and `run` exit 3 (naming the file);
restoring its bytes and time makes `check` pass; a new untracked file on `/mnt/d` and a file present only in the
mirror each make it exit 3. In passing, the first suite run of this batch was refused by it as well: nine checkout
files had been edited after the last sync.

## Suites

CTest on the converted trees as the hosted lanes run it (headless, soft performance budgets, JUnit), split into
ranges under the loop watchdog's limit; the user's process change of 2026-10-18 then made the hosted lanes the owners
of the full suites, so the incomplete range below is theirs:

| Tree | Tests | Result |
|---|---|---|
| build/win-debug (sccache, PCH off), range 1 | 3,914 | all pass but `crd-master-plan`: a missing anchor of this board (`#hosted-cache-budget`), written since |
| build/win-debug, range 2 | 3,914 | all pass |
| build/win-asan (sccache, PCH off), range 1 | 3,914 | all pass |
| build/win-asan, range 2 | about 3,340 of 3,914 | none failed; stopped by the one-hour supervisor deadline |
| ~/cerid-mirror-build/linux-gcc-debug (mirror, sccache) | 7,485 | all pass but `crd-master-plan` (the same anchor) and `DIAG.7a(f): Vulkan reports per-mode validation activation` |

The Vulkan case found a difference between the routes: `wsl-lane.sh` exported no `VK_LAYER_PATH`, so the loader took
the system validation layer, older than the engine's headers (VUID errors on the engine's `pNext` chains and
GPU-assisted validation inactive). The helper now exports the pinned layer (`~/.cerid/vulkan-validation`, linked to the
pinned install, or `CRD_VULKAN_VALIDATION`), and the case passes on the mirror tree with the other validation
activation cases.

## Debugging a program built from the cache

`build/bench-1018/cdb-proof.ps1` (scratch) runs cdb 10.0.29547 from the WinDbg package with `.lines -e`, a
breakpoint on `crd::memory::LinearAllocator::allocate`, `kp 4`, `lsa .`, `dv /t` and `ln .` on `crd-memory-tests.exe`:

| Executable | Its objects | Result |
|---|---|---|
| win-debug board D tree | 1,850 of 1,850 compiles from cache hits, `/Z7`, PCH off | breakpoint hit; frames `linear_allocator.cpp @ 56`, `test_allocator_asan.cpp @ 206` (`+0x11d`), `catch_test_registry.cpp @ 60`, `catch_test_case_info.hpp @ 124`; source lines 55-58; typed `this`, `size`, `alignment`; `ln` names the symbol and its neighbour |
| win-debug board A tree | the default (PCH on, `/Zi`, uncached) | the same frames, lines and offsets |
| win-shipping board D tree | cache hits, `/Z7` with LTCG and `/DEBUG:FULL` | breakpoint hit with the argument values (`size = 1`, `alignment = 0x10`); the test frame at line 207 and the inline Catch2 frames with their files and lines |

The engine's own symbolication runs in the suites above (the `[crash]` and `[crash-capture]` cases of crd-core-tests
and the DIAG.5a crash-dump tests read the linked PDB through dbghelp).

## Hosted cache budget

The hosted lanes cache through the GitHub Actions service, one entry per object under each lane's namespace; the
working set of one run must stay inside the repository's 10 GB allowance with the CPM (19 × 65 MB), Vulkan SDK
(405 MB) and GenMC (2 MB) caches. Footprints of one cold build (GiB), measured here or scaled from the nearest
measured lane:

| Lane | Tier | Hosted build step, run 38870170572 (min) | Cold footprint (GiB) | Source |
|---|---|---|---|---|
| win-debug | change | 4.6 (cached through `actions/cache`) | 0.43 | measured: the conversion of `build/win-debug` |
| win-asan | change | 26.3 | 0.50 | measured: the conversion |
| win-debug-sse2 | change | 25.2 | 0.43 | scaled: win-debug |
| win-public-checks | complete | 32.5 | 0.45 | scaled: win-debug plus the header checks |
| win-clang-cl | change | 23.2 | 0.45 | scaled: win-debug |
| win-clang-cl-shipping | change | 59.4 | 0.51 | measured: the conversion |
| linux-gcc-debug | change | 0.6 (cached) | 0.41 | measured: board c's cold share |
| linux-gcc-debug-sse2 | change | 13.6 | 0.41 | scaled: linux-gcc-debug |
| linux-gcc-public-checks | complete | 23.5 | 0.43 | scaled: linux-gcc-debug |
| linux-gcc-asan | change | 36.8 | 0.65 | measured: board c's cold share |
| linux-clang-tsan | complete | 28.5 | 0.36 | measured: board c's cold share |
| linux-gcc-release | change | 28.8 | 0.15 | scaled by object bytes (0.46 against 1.28 GiB) |
| linux-gcc-relwithdebinfo | change | 64.5 | 0.45 | scaled by object bytes (1.41 GiB) |
| linux-gcc-shipping | change | 54.8 | 0.45 | scaled by object bytes (1.40 GiB) |
| **the 14 cached lanes** | | | **6.48 GiB (6.96 GB)** | |
| win-release, win-relwithdebinfo, win-shipping, win-shipping-profile (uncached) | change, complete | 167-180 each | about 3.2 together | scaled: the focused win-shipping board (0.21 GiB for 451 compiles) |

With the other caches (about 1.7 GB) one run's working set is about 8.7 GB of the 10 GB; adding the four LTCG lanes
would make it about 12 GB, past the allowance, so the eviction would take entries one run needs again. The stale
entries of earlier runs (changed units, the seeded-fault variants) are what the allowance evicts first, since every
run reads its lanes' current entries. The hosted numbers replace the scaled rows after the first two cached runs
(`gh api repos/yatiyr/CRD/actions/cache/usage` and each lane's statistics).

## Losses

- A cold miss costs +12 to +18 % on the WSL lanes, +24 % (against today's default) to +34 % (against PCH off) on
  win-debug, +14 % on win-asan and +44-51 % on the focused clang-cl Shipping route; it is paid once per tree and per
  changed unit. The first break after a conversion is slower than the uncached build on a small target (11.2 against
  6.8 s on win-debug).
- MSVC LTCG gains little: -17 % on a focused restore, the link stays; those four trees stay uncached.
- The first configure on an empty cache took 35.9 s (win-debug) and 48.8 s (win-clang-cl-shipping) against
  28-34 s (the server starts and the compiler checks miss); later configures took the same time.
- win-debug board A's no-op row (20.7 s, 352 edges) is void: a line-ending pass rewrote three CMake inputs byte for
  byte while the board ran, and the next row re-ran CMake. win-debug board B's and C's last rows and clang-tsan's
  last cached row show no edges because Ninja rewrote its log during the row; their wall times stand.

## Host faults during the measurements

Five compiler crashes, each on a different unit and none reproducing on a rerun: MSVC C1001 in
`engine/ceir/src/work.cpp` under `/fsanitize=address` (win-asan board A, teeth restore; three clean recompiles of the
unit afterwards), MSVC C1001 in LTCG code generation of `crd-memory-tests.exe` at `<random>(1022)` (win-shipping
board A, cold), cc1plus `realloc(): invalid pointer` on `tests/numerics/hesap-fft/test_fft.cpp` and cc1plus aborting
in RTL expand on `test_spectral_rational.cpp` (linux-gcc-asan board b, two attempts), clang segmentation fault on
`tests/bench/test_bench.cpp` (linux-clang-tsan board c). The boards were rerun from fresh trees; the numbers above are
the clean reruns. The same class of input-independent faults on this host is recorded since 2026-10-11.
