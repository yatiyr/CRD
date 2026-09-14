# Build performance: measured boards, qualified compiler caching and bounded jobs

<!-- doc-role: reference -->
> Contract for REPO.DEV.7. Status lives only in [ROADMAP](../ROADMAP.md#slice-repo.dev.7); the accepted research is
> [compiler caching and parallelism](../research/2026-09-12-large-cpp-development-and-ci.md#measure-compiler-caching-and-parallelism);
> the measured board is [2026-09-14](../bench/2026-09-14-repo-dev7-build-board.md); the module is
> [CrdBuildPerformance.cmake](../../cmake/CrdBuildPerformance.cmake); the runner is
> [build-bench.py](../../scripts/build-bench.py).

Purpose: know what a Cerid build costs before changing how it is built, cache compiles only where a measurement
qualifies it, bound compile and link parallelism from measured memory pressure, keep every existing obligation
(debug symbols, `/WX`, sanitizers, ISA levels, Shipping/LTCG, the native solution) intact, and record every hosted
build as a board of the exact revision.

## What the probes decided

Two-compile probes on a real unit (`engine/foundation/core/src/assert.cpp`) with the pinned sccache 0.17.0 set the
design before any board ran:

| Toolchain | Configuration | Second compile | Reason |
|---|---|---|---|
| MSVC 14.51 | CMake precompiled header on (`/Yu … /Fp …`) | non-cacheable | sccache refuses `/Fp` and `/Yc` ("PCHs are not supported"); every consuming unit is counted under `Non-cacheable reasons: /Fp` |
| MSVC 14.51 | PCH off, `/Z7` | hit | embedded debug information; `/Zi` without a per-object PDB is refused as "shared pdb" |
| GCC 13.3 | CMake precompiled header on (`-include cmake_pch.hxx`) | hit | the include is a preprocessor argument; the preprocessed unit hashes the same |
| GCC 13.3 | PCH off | hit | |

So the MSVC question is never "cached versus uncached" but "PCH on and uncached" against "PCH off and cached", and
the board carries both; on GCC the precompiled header stays on and only the launcher is added.

## The opt-in module

[CrdBuildPerformance.cmake](../../cmake/CrdBuildPerformance.cmake) adds three cache entries, all empty by default,
so the default graph is byte-identical (proved on `build/win-debug`: 292 targets, 1,814 compile commands and 5,786
Ninja statements unchanged except the CMake re-run rule listing the new module):

- `CRD_COMPILER_LAUNCHER=<path>` sets `CMAKE_C/CXX_COMPILER_LAUNCHER` for Makefile and Ninja generators, which are
  the generators CMake applies launchers to. On MSVC-compatible compilers it also sets
  `CMAKE_MSVC_DEBUG_INFORMATION_FORMAT` to `Embedded` for Debug and RelWithDebInfo (policy CMP0141 is NEW under the
  3.25 minimum), and it warns when `CRD_ENABLE_PCH` is still on, because nothing would be cached; it never flips the
  PCH option itself. A Shipping configuration keeps its explicit `/Zi`, `/DEBUG:FULL` and LTCG uncached and says so.
  A Visual Studio generator prints that the solution runs uncached and touches nothing: native MSBuild caching
  needs its own proof, and the truthful IDE path is the uncached one.
- `CRD_COMPILE_JOBS=<n>` and `CRD_LINK_JOBS=<n>` declare Ninja job pools (`crd_compile`, `crd_link`) and route
  compiles and links through them; other generators report that pools are ignored. Non-integers fail the configure.

`scripts/install-sccache.py` acquires the pinned binary (registry entries `tools/sccache-windows` and
`tools/sccache-linux`: archive digest, member, member digest, version check) and prints the launcher path.

## The board runner

`scripts/build-bench.py run` configures one preset into a fresh scratch directory (it refuses an existing one),
builds with an explicit `--jobs`, and records per row: wall time, the Ninja log of that row (edges, span, edge sum,
effective parallelism, slowest edges), memory pressure sampled once a second (minimum available, peak committed,
their difference from the row's baseline), the build-tree size and, with `--sccache`, the statistics zeroed before
and read after the row (hits, misses, non-cacheable compiles and reasons). Rows: `configure`, `cold`, `noop`,
`source` (append a comment to `engine/foundation/log/src/logger.cpp`), `header` (`crd/log/log.hpp`, outside the
PCH), `pch-header` (`crd/core/types.hpp`, inside the PCH), `link` (delete `crd-ceir-tests` and relink). Edited files
are restored byte-for-byte with their original timestamps, so neither the working tree nor another build directory
sees the edit. `render` merges board documents into the markdown table that `docs/bench/` stores. The runner is a
measurement instrument, not a qualification sweep: one preset, a scratch tree, an explicit job count, no tests.

## Measured board and decisions

See the [board](../bench/2026-09-14-repo-dev7-build-board.md) for the full tables (machine, toolchain, every row,
memory, disk, cache size). The decisions it carries:

- **PCH off is free on MSVC.** The four-header precompiled payload does not change the cold build (136.5 s on,
  137.4 s off at 16 jobs; identical source, header and link rows), so the cached Windows configuration loses nothing
  by disabling it, and gains 1.4 GiB of tree.
- **Caching qualifies on `win-debug`.** A miss costs about a fifth (165.1 s against 137.4 s cold; the PCH-header
  edit 147.3 s against 123.4 s); a full hit rebuilds in 43.5 s, 3.1× faster than the default cold build, with every
  one of the 1,562 compiles served; reverting a header edit with hits takes 4.5 s against 8.8 s. The rule for a
  cached lane is therefore: a warm build must beat today's cold build (it does, by 3.1×), a cold miss may cost up to
  a quarter more than the uncached build (it costs a fifth), and the tests must be unchanged (they are the same
  executables from the same sources; the register gate stays in force).
- **The cache key includes the build directory.** The same sources in another scratch directory hit only the 534
  third-party units and missed the 1,028 Cerid units (the generated include directory and the object path differ);
  the same directory hit everything. Hosted lanes build in a fixed path; a local cache needs a stable directory.
- **Memory is not the bound at 16 jobs on a 64 GiB workstation.** Peak pressure 5.8 GiB with PCH on (about
  0.36 GiB per compile), 3.1 to 4.5 GiB without; the ASan link of `crd-ceir-tests` (20.7 MiB) stays under 0.2 GiB
  and the Debug relink is 0.4 s. Effective parallelism is 15.9 to 16.0 of 16 on every cold row (compile-bound, no
  scheduling gap), so `CRD_COMPILE_JOBS`/`CRD_LINK_JOBS` stay unset by default; the derived bound for a memory-limited
  host is `compile jobs = (available GiB - 4) / 0.4`, which on a 16 GiB, 4-vCPU hosted runner is far above its CPU
  count, so the runners run unbounded too.
- **Disk.** A Debug tree is 9.3 to 10.8 GiB; embedded debug information did not grow it; the whole preset caches in
  0.71 GiB (0.99 GiB with edited variants), which sets the hosted per-lane `SCCACHE_CACHE_SIZE` of 2 GiB.
- **Local defaults are unchanged.** The developer's `win-debug` keeps PCH on and no launcher; a developer who wants
  the cache configures the two entries once and keeps one build directory.

- **Caching qualifies on `linux-gcc-debug` with the precompiled header on.** On the WSL2 board (native ext4,
  GCC 13.3, 16 jobs) 1,773 compiles went through the launcher with no non-cacheable reason; a miss costs +12 %
  (160.5 s against 142.8 s cold); an edit that leaves the preprocessed unit unchanged hits (the PCH-header comment
  edit rebuilt in 21.3 s instead of 139.6 s); the same-directory warm board rebuilt the tree in 20.3 s (7.0×,
  1,773 of 1,773 hits, 2.0 GiB of pressure instead of 14.0 GiB).
  The build-directory dependence holds on Linux too (`-include <build>/…/cmake_pch.hxx`, `-ffile-prefix-map`).
- **Memory is the bound on GCC, not on MSVC.** 14.0 GiB peak pressure at 16 jobs (about 0.88 GiB per compile)
  against 5.8 GiB on MSVC; `CRD_COMPILE_JOBS = (available GiB - 4) / 0.9` is the derived bound for a Linux host
  (12 on a 16 GiB workstation at 16 threads); the hosted 4-vCPU runner needs none.

## Hosted lanes

Every lane's evidence bundle now carries the lane's Ninja log summary (a hosted checkout is a fresh directory, so
the log is the cold board of the exact revision) and, for a cached lane, the sccache statistics of the build step,
so the first cached push records the miss board and the second the hit board without any extra job.

The cached lanes are the presets listed in the workflow's `CRD_CACHED_PRESETS`; every other lane is unchanged. A
cached lane installs the pinned sccache, restores `SCCACHE_DIR` through `actions/cache` (key
`sccache-<os>-<preset>-<run id>` with the `sccache-<os>-<preset>-` restore prefix, so every run restores the newest
cache of that lane and a green run saves its own; `actions/cache` saves in its post step only when the job succeeds,
so a failed lane restores but does not save), configures with `-DCRD_COMPILER_LAUNCHER` (and `-DCRD_ENABLE_PCH=OFF` on
MSVC, explicitly, in the step), builds, prints and stores the statistics, and passes them to the bundle. The
per-lane `SCCACHE_CACHE_SIZE` is capped so the cached lanes cannot evict the CPM and Vulkan SDK caches from the
repository's 10 GB allowance. The GitHub Actions cache backend of sccache is not used: it needs
`actions/github-script`, which is outside the pinned registry, and the local-disk cache stays inside the pinned
`actions/cache`.

Extending the list is evidence-driven: read the cached lane's `conclusion.json` build section (hits, misses,
reasons, span) over a few pushes, then add a preset only with a board row for its compiler and configuration.

## Not in scope

Unity builds, C++20 modules, distributed compilation and a build-system replacement stay measured options for a
later profile; public-header checks and downstream consumers are REPO.DEV.8.
