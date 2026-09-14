# Build performance: the measured board, qualified caching and bounded jobs

<!-- doc-role: historical -->
> Dated evidence. Live owner: [REPO.DEV.7](../ROADMAP.md#slice-repo.dev.7); contract:
> [build performance](../design/build-performance.md); board:
> [2026-09-14](../bench/2026-09-14-repo-dev7-build-board.md). Rules: [AGENTS](../../AGENTS.md).
> Preceding batch: [pinned inputs](2026-09-14-pinned-inputs.md).

## User direction

The standing direction is unchanged: carry the REPO.DEV slices on "one by one until AUD-2". REPO.DEV.7 followed
REPO.DEV.6 in the same session; the advisor ruled at the design gate (measure before adopting, PCH-on-uncached
against PCH-off-cached on MSVC, scratch trees with an explicit job count, no test sweep, adoption only where a
board qualifies it) and the increment ran directly. Hosted run 34780682504 (`18651d5`) concluded green on every lane
(`win-asan` last) during the slice; nothing was red at any poll.

## What the probes decided

Before any board, two compiles of `engine/foundation/core/src/assert.cpp` through the pinned sccache 0.17.0 (the
archives verified against fresh downloads, binaries verified by member digest) settled the shape of the slice:

- MSVC 14.51 with the CMake precompiled header (`/Yu … /Fp …`): the second compile is a "Non-cacheable call",
  reason `/Fp` (sccache's parser marks `/Fp` and `/Yc` as unsupported). With PCH off and `/Z7`: one miss, then one
  hit (cache read 4 ms against a 463 ms compile). `/Zi` without a per-object PDB is refused as "shared pdb".
- GCC 13.3 with the CMake precompiled header (`-include cmake_pch.hxx`): one miss, then one hit; the same with PCH
  off. Linux keeps its PCH and only adds the launcher.

## What changed

- [cmake/CrdBuildPerformance.cmake](../../cmake/CrdBuildPerformance.cmake), included by the root build file: the
  opt-in `CRD_COMPILER_LAUNCHER` (Makefile/Ninja only; Embedded debug information on MSVC for Debug and
  RelWithDebInfo; a warning when PCH is still on; Shipping and Visual Studio generators reported as uncached) and
  the `CRD_COMPILE_JOBS`/`CRD_LINK_JOBS` Ninja pools. The default `win-debug` graph is unchanged (292 targets,
  1,814 compile commands, 5,786 Ninja statements identical; only the CMake re-run rule lists the new module).
- [cmake/pins.json](../../cmake/pins.json): `tools/sccache-windows` and `tools/sccache-linux` (archive digest,
  member, member digest); [scripts/install-sccache.py](../../scripts/install-sccache.py) extracts only the member,
  verifies it and the reported version; `check-pins.py` lists the helper and forbids a direct release download.
- [scripts/build-bench.py](../../scripts/build-bench.py): the board runner (`run`, `render`) with the rows,
  memory sampling, Ninja-log and sccache accounting described in the design; edited files are restored
  byte-for-byte with their timestamps.
- [scripts/ci-evidence.py](../../scripts/ci-evidence.py): every bundle records the lane's Ninja log summary
  (edges, invocations, span, edge sum, parallelism, slowest edges) and, with `--sccache-stats`, the sccache
  statistics; the job summary shows both in a `Build` row.
- [ci.yml](../../.github/workflows/ci.yml): `CRD_CACHED_PRESETS` names the cached lanes; a cached lane installs
  the pinned sccache, restores and saves `SCCACHE_DIR` through the pinned `actions/cache` (run-id key, lane restore
  prefix, `SCCACHE_CACHE_SIZE` capped), configures with the launcher (and `-DCRD_ENABLE_PCH=OFF` on MSVC, in the
  step), stores the statistics and hands them to the bundle. `.sccache/` is ignored and a known root directory.
- Tests: four `BuildPerformance` cases in `test-repository-tools.py` (log and statistics summaries in the bundle
  and its markdown; edit rows restore bytes and timestamps, board rendering, the launcher's `SCCACHE_DIR`
  requirement; the installer extracts only the pinned member from zip and tar and fails on a wrong member digest or
  version; the CMake module leaves the default untouched, applies launcher and pools under Ninja, ignores the
  launcher for Shipping and rejects a non-integer pool depth).
- Docs: [design/build-performance.md](../design/build-performance.md); the board under `docs/bench`; BUILDING
  (6,996 bytes) names the switches; scripts README, pinned-inputs and CI-tiers designs updated; two memory records;
  ROADMAP row 053; the pointer moves to REPO.DEV.8.

## A defect the cold board found first

The first cold build of the scratch tree failed on MSVC 14.51.36231 in two test units that the incremental
`build/win-debug` tree and the hosted windows-2025 toolset still accepted: `HashMap<u32, u32>::find(7)`
instantiated `std::equal_to<>` with a signed/unsigned comparison (C4389, an error under `/WX`, reported at the
instantiation context in `xutility`), and `REQUIRE((owner_got ^ thief_got) == true)` compared `int` with `bool`
inside Catch's decomposer (C4805). Ninja rebuilds on timestamps, not on compiler identity, so the stale objects hid
the toolset upgrade. Both were test defects: the 23 heterogeneous key literals became `u32` literals and the deque
assertion became `REQUIRE(owner_got != thief_got)`; the suites passed unchanged (10,078 and 28,897 assertions).

## The board

Five `win-debug` boards at 16 jobs on the i9-14900K, every row in a fresh scratch tree
([board](../bench/2026-09-14-repo-dev7-build-board.md)):

| Board | Cold | Source edit | Header edit | PCH-header edit | Link | sccache |
|---|---|---|---|---|---|---|
| A: PCH on, uncached (today's default) | 136.5 s | 4.4 s | 8.8 s | 125.0 s | 1.0 s | none |
| B: PCH off, uncached | 137.4 s | 4.4 s | 8.7 s | 123.4 s | 0.9 s | none |
| C: PCH off, sccache, empty cache | 165.1 s | 4.5 s | 9.4 s | 147.3 s | 0.9 s | 0 hits / 1,562 misses on cold |
| D: PCH off, sccache, warm cache, other directory | 138.4 s | 3.4 s | | | 1.0 s | 534 hits / 1,028 misses |
| D2: PCH off, sccache, warm cache, same directory | 43.5 s | 3.4 s | 4.5 s | | 1.0 s | 1,562 hits / 0 misses |

The verdicts: the MSVC precompiled header buys nothing measurable, a miss costs about a fifth, a full hit rebuilds
3.1× faster than today's cold build, the cache key includes the build directory (hosted lanes build in a fixed
path), memory is not the bound (5.8 GiB peak pressure at 16 jobs; the ASan `crd-ceir-tests` link stays under
0.2 GiB), effective parallelism is 15.9 to 16.0 of 16, a Debug tree is 9.3 to 10.8 GiB and the whole preset caches
in 0.71 GiB. The default `win-debug` configuration is unchanged; hosted `win-debug` builds with PCH off through the
launcher; `CRD_COMPILE_JOBS`/`CRD_LINK_JOBS` exist for memory-bound hosts and stay unset.

Linux, on a native-ext4 copy under WSL2 (GCC 13.3, 16 jobs, PCH on throughout; configure rows network-bound):

| Board | Cold | Source edit | Header edit | PCH-header edit | Link | sccache |
|---|---|---|---|---|---|---|
| LA: uncached (today's default) | 142.8 s | 5.8 s | 10.6 s | 139.6 s | 0.6 s | none |
| LC: sccache, empty cache | 160.5 s | 5.4 s | 5.7 s | 21.3 s | 0.7 s | 1,773 misses on cold; the comment edits hit 1, 38 and 1,601 |
| LD: sccache, warm cache, other directory | 158.1 s | 5.0 s | | | 0.9 s | 0 hits / 1,773 misses |
| LD2: sccache, warm cache, same directory | 20.3 s | 4.7 s | 5.0 s | | 0.7 s | 1,773 hits / 0 misses |

GCC caches with its precompiled header (no non-cacheable reason), a miss costs +12 %, a full hit rebuilds in
20.3 s (7.0×, 2.0 GiB of pressure instead of 14.0 GiB), an edit that leaves the
preprocessed unit unchanged hits (MSVC misses the same edit), the build directory is part of the key on both
platforms, and memory is the bound on GCC (14.0 GiB at 16 jobs, about 0.88 GiB per compile) where it is not on
MSVC (5.8 GiB).


## Verification

- **Probes.** MSVC PCH on: `Non-cacheable calls 1`, reason `/Fp`; PCH off with `/Z7`: 1 miss then 1 hit
  (compiler 0.463 s, cache read 0.004 s). GCC PCH on: 1 miss then 1 hit; PCH off: the same.
- **Default graph unchanged.** `build/win-debug` reconfigured (15.4 s configure, 2.0 s generate): 292 targets,
  1,814 compile commands and 5,786 Ninja statements identical to the REPO.DEV.6 snapshot except the two CMake
  re-run statements that now list `cmake/CrdBuildPerformance.cmake`.
- **Runner smoke.** `CRD_MODULES=log`, PCH off, sccache: cold 146 edges (137 misses), no-op 1 edge, source edit
  1 miss, header edit 9 misses, link 0; the working tree was clean afterwards.
- **Boards.** Five Windows boards (2:35 to 3:08), every row exit 0; the tables are in the board document and the
  raw records in `build/bench/board-*.json`.
- **Test repairs.** `crd-containers-tests "[hash_map],[hash_set]"`: 10,078 assertions in 19 cases; `crd-jobs-tests
  "*deque*"`: 28,897 assertions in 13 cases; the keep-going rebuild of the scratch tree then reported zero failures.
- **Guards.** `check-pins.py` PASS (10 packages, 11 tools, 7 actions, 3 runners); `check-ci-tiers.py` PASS;
  actionlint 1.7.12 clean (the job-level `runner` context it rejected became the workspace path);
  `test-repository-tools.py` 40/40 with CMake and Ninja on PATH (the module fixture reads the pools from
  `CMakeFiles/rules.ninja`, where the Ninja generator declares them); `check-repository.py` PASS;
  `check-master-plan.py` PASS (863 rows, 1,044 documents, 9,043 links); `git diff --check` clean.

- **Linux boards.** LA, LC, LD (00:15 to 00:32 UTC) and LD2 (00:33 to 00:36), every row exit 0, on a native-ext4
  copy of the working tree under WSL2; the copy, the cache and the helper scripts were removed afterwards, as
  were the Windows scratch trees (the raw `build/bench/board-*.json` records stay, ignored).
