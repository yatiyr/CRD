# DIAG.11b: faster local builds, task B (the linker, split DWARF, win-debug link modes), 2026-10-18

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b) (its contract covers focused local routes and CI
> cost). Contract: [build performance](../design/build-performance.md#the-linker). Board:
> [2026-10-18 linking](../bench/2026-10-18-build-speed-linking.md). Previous batch:
> [task A](2026-10-18-diag-11b-build-speed-cache-mirror.md).

## Goal

Task B of the user's build-speed batch (2026-10-18): faster links and debug information without regressing anything.
Pin and fetch the latest mold and evaluate it on the Linux lanes (gcc debug and asan, clang tsan, and the GCC
link-time-optimized lane through the linker plugin), `-gsplit-dwarf` on the Linux debug lane with gdb proved, and
incremental linking on win-debug for the local loop; keep only what is measured faster and proven equivalent, with
the hosted lanes' checks unchanged. Changed-only local checks; the hosted lanes run the suites (the user's process of
2026-10-18).

The batch was stopped by mistake once while it re-ran the split-DWARF boards. This run resumed it from its transcript
and the logs under `build/bench-1018b/`: it kept the measured boards (the linker boards of gcc-debug, gcc-asan and
clang-tsan at 16 and 4 jobs, the split and PCH-off primes) and the uncommitted edits after reviewing them, and ran
what had not finished.

## What changed

- **Registry and installer.** `cmake/pins.json` pins mold 3.0.0 (`tools/mold-linux`: the release archive's SHA-256,
  the `bin/mold` member and its SHA-256, MIT, the tag's commit, which the binary's `--version` names); the license
  manifest lists it (its provenance digest refreshed). [install-mold.py](../../scripts/install-mold.py) installs it
  through `pins.install_member()` (archive and member verified, the reported version checked) with the `ld.mold` link
  the drivers look for (a hard link where a symbolic link needs a privilege). [pins.py](../../scripts/pins.py) now owns
  `extract_member()` and `install_member()`, which [install-sccache.py](../../scripts/install-sccache.py) uses too;
  [check-pins.py](../../scripts/check-pins.py) lists the new helper. Installed at `~/.cerid/tools/mold` (WSL).
- **The module.** [CrdBuildPerformance.cmake](../../cmake/CrdBuildPerformance.cmake): `CRD_LINKER` (an installed
  mold with its `ld.mold`, else the configure fails) adds `-fuse-ld=mold -B<dir>` to GCC and Clang link lines, and
  `CRD_LINKER_THREADS` (a positive integer) `-Wl,--threads=<n>`; MSVC-compatible compilers and a thread count without
  a linker report the entry as ignored. Both empty by default; no preset or workflow names them.
- **The helpers.** [compiler-cache.py](../../scripts/compiler-cache.py) `configure-args` adds both entries on a Linux
  host for linux-gcc-debug, linux-gcc-asan and linux-clang-tsan (`MOLD_PRESETS`, the lanes the board measured
  faster) when the pinned mold is installed, the thread count the CPUs divided by the lane's jobs (`CRD_LANE_JOBS`,
  which [wsl-lane.sh](../../scripts/wsl-lane.sh) now exports): two here. `CRD_MOLD=off|<path>` and
  `CRD_MOLD_THREADS` override. The GCC LTO presets are left out (no measured gain). The three mirror trees are
  converted (`wsl-lane.sh configure`).
- **The sccache server on two hosts** (a fault the boards found, below): the Linux server has its own port 4227
  (Windows keeps 4226; `CRD_SCCACHE_PORT`; `compiler-cache.py port`; `wsl-lane.sh` exports `SCCACHE_SERVER_PORT`);
  the server step asks a server only when something listens, starts one explicitly otherwise, proves a listening
  server on this cache, and fails when it cannot; [build-target.bat](../../scripts/build-target.bat) and
  `wsl-lane.sh` stop the build on that failure.
- **The board runner.** [build-bench.py](../../scripts/build-bench.py): a `relink-all` row (every program of the tree
  or of `--targets` deleted and relinked) and the linker and its threads in the board context.
- **Docs.** The design's new [The linker](../design/build-performance.md#the-linker) section, the module, server and
  runner paragraphs, the board, the bench index (task A's board was missing from it as well), DIAG.11b's row, the
  pointer, and four memory lessons.

## Measured

See the [board](../bench/2026-10-18-build-speed-linking.md). In short:

- mold's default pool (a thread per CPU for every link) gained nothing at 16 jobs: the whole-tree rows were equal or
  slower than GNU ld (clang-tsan teeth +29 %, gcc-asan +9 %); at 4 jobs it already won. Interleaved in one tree, two
  threads per link (16 jobs × 2 = the 32 CPUs) cut a whole-tree relink's summed link time from 106.3 to 54.8 s
  (gcc-debug), 179.2 to 81.9 s (gcc-asan) and 125.5 to 25.6 s (clang-tsan), its wall time from 7.3 to 3.8 s, 13.6 to
  10.0 s and 10.4 to 8.0 s (the last two bounded by the asset cook). On the focused route a one-source edit of
  `crd-ceir-tests` relinks in 0.7-0.9 s against 1.1-1.5 s.
- GCC LTO through the plugin: bounded mold 7.2-7.5 s against GNU ld's 7.3-8.0 s for four programs: no gain.
- `-gsplit-dwarf`: GCC refuses the precompiled header under it; against the PCH-off tree it saves 1.2 s of a 9.1 s
  warm relink, the teeth cycles are equal, and the PCH-on tree is as fast warm and cheaper on a miss: not kept.
- win-debug: the default `/INCREMENTAL` is the fastest; `/INCREMENTAL:NO` doubles the link time, `/DEBUG:FASTLINK`
  ties it on edits and doubles a full relink: nothing changes.

## Equivalence and gates

- Suites: `crd-ceir-tests` linked by GNU ld (twice) and by mold on gcc-asan, and by bounded mold on the converted
  linux-gcc-debug mirror tree through `wsl-lane.sh run`: "All tests passed (18784 assertions in 580 test cases)" each
  time, the same 931-line inventory; `crd-perf-ui-tests` mold-linked with LTO: 14 cases, 612 assertions.
- Debugger: gdb 15.1 (debuginfod off) on `crd-ceir-tests` of the mold and the ld trees of gcc-debug and gcc-asan and
  of the split-DWARF tree: the `test_func.cpp:23` breakpoint, three frames, the typed locals, a step and the sources
  resolve identically; the five `crd-diag-reverse-debug-gdb-*` CTests (two on the sanitizer lane) pass on every one,
  and through `wsl-lane.sh test` on the converted mirror tree.
- Warnings: no build of either linker printed a `warning:` line or a linker diagnostic; the logs' only warnings are
  zstd's CMake deprecation (follow-up (l)) and the split board's PCH refusal. The link-time warning set still fires
  under mold: the 2026-10-17 tooth (a cross-file out-of-bounds read in `crd-perf-ui-tests`) fails the LTO link with
  `test_panel_helpers.cpp:155:24: error: array subscript 6 is outside array bounds of 'const int[4]'
  [-Werror=array-bounds=]` under mold (`mold: fatal: lto-wrapper failed`) and under GNU ld, and the restored sources
  link green with both.
- Hosted lanes: unchanged. No preset, workflow or CI script names `CRD_LINKER`, `CRD_LINKER_THREADS` or mold.

## A fault found: the WSL relay and the sccache port

At the start of the win-debug board `compiler-cache.py server` failed (`--start-server`: "Address in use", exit 2) and
`build-target.bat` built on regardless. WSL2 relays every localhost port a Linux process listens on to Windows
(`wslrelay.exe`): a Linux sccache server left by a lane build on the default port answered the Windows clients (their
`--show-stats` named `/home/yatiyr/.cache/cerid-sccache`) and blocked a Windows server. Also measured: a statistics
query with no server starts one that exits with the query, so the helper's check could pass on a server the build
never saw; and a listener that never answers holds a query for its 60 s timeout. The repair is above; afterwards a
lane build's server sits on 4227 with the Linux cache while the Windows helper's server runs on 4226 with
`build\sccache`, and with a silent listener holding 4226 `build-target.bat` exits 1 with "the sccache server did not
start on D:\Dev\cerid\build\sccache (port 4226, exit 124)". Task A's measurements are unaffected: its boards started
their own servers, and a compile that finds no server starts one with the build's environment.

## Teeth

`build/bench-1018b/teeth.py` (scratch) applied each mutation alone, ran its test class of
`test-repository-tools.py` under `scripts/msvc-env.bat` (so the CMake module test runs), and wrote the bytes back,
checking their digest. All 18 are caught:

| Mutation | File |
|---|---|
| the helper drops the thread bound; links every Linux preset; links on Windows; ignores the lane jobs; `off` keeps the linker | `compiler-cache.py` |
| the Linux server shares the Windows port; the server step asks a port nothing listens on; a failed start passes; the proof asks an empty port (a transient server passes) | `compiler-cache.py` |
| `wsl-lane.sh` hides the server step again (`eval "$(...)"`) | `wsl-lane.sh` |
| the module drops the thread option; takes any thread count; puts the linker on compile lines; accepts a binary without `ld.mold` | `CrdBuildPerformance.cmake` |
| the installer skips the member digest; accepts another mold version | `pins.py`, `install-mold.py` |
| `relink-all` also deletes archives; the board loses the thread count | `build-bench.py` |

## Verification

- `python scripts/test-repository-tools.py`: 93 tests OK on Windows under `scripts/msvc-env.bat` (none skipped:
  the CMake module test ran) and on WSL from the synced mirror (one Windows-only WARP case skipped);
  `test-dev-workflow.py` 54 OK; `check-ci-tiers.py` PASS (its cache rule now reads `cache_args`, the linker
  entries apart).
- `check-pins.py`, `gen_license_manifest.py --check`, `check-generated.py`: pass.
- `check-master-plan.py`, `check-repository.py`: PASS; the ctest repository guards (`ctest -R "^crd-no-|^crd-check"`
  in `build/win-debug`): 10 of 10 pass.
- No C++ source changed (tidy, clang-format and the Allman check have nothing to run on); no workflow changed.
- Line endings: the `eol=lf` files are LF and `build-target.bat` (`eol=crlf`) CRLF in the working tree.

## Hosted evidence this batch needs

The repository job on Linux and Windows passes `test-repository-tools.py` with the new cases (the mold installer, the
module's linker and thread entries, the helpers' linker entries and server port and failure, `relink-all`, the
board's linker label) and `check-pins.py` with `tools/mold-linux`; every build lane is unchanged and passes as before.

## Follow-up for an extra batch

- Task C of the user's build-speed request (approved 2026-10-18): pin the ClangBuildAnalyzer source through the
  registry rules `check-pins.py` enforces, build it in a user directory, and profile a clang `-ftime-trace` build of
  linux-clang-tsan to find the headers and templates that dominate compile time; measure any change on the same lanes.
