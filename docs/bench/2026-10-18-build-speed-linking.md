# 2026-10-18 — build speed, task B: the pinned mold on the WSL lanes, split DWARF, win-debug link modes

<!-- doc-role: evidence -->
> Dated evidence; counts and results are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).
> Contract: [build performance](../design/build-performance.md#the-linker); session:
> [2026-10-18](../sessions/2026-10-18-diag-11b-build-speed-linking.md); owner row:
> [DIAG.11b](../ROADMAP.md#slice-diag.11b) (focused local routes and CI cost). Task A's board:
> [cache and mirror](2026-10-18-build-speed-cache-mirror.md).

- **Machine/config:** Intel i9-14900K (32 logical CPUs), 63.7 GiB RAM, Windows 11 10.0.26200; WSL2 Ubuntu 24.04 on the
  same CPU (31.2 GiB, kernel 6.6.87.2), GCC 13.3.0, Clang 18.1.3, GNU ld 2.42, CMake 3.28.3, Ninja 1.11.1, gdb 15.1;
  mold 3.0.0 (the release build of tag v3.0.0, commit `8de38c35`, the first release of mold's Rust rewrite), installed
  by `scripts/install-mold.py` from the registry entry `tools/mold-linux`; sccache 0.17.0. Windows: MSVC 19.51
  (VS 18 Community, toolset 14.51.36231), CMake 4.3.2, Ninja 1.13.2. Revision: HEAD `9461800a` plus this batch's
  working tree.
- **Machine state:** the boards before 17:03 ran on an otherwise idle host. From 17:03 a desktop image generator the
  user started (ComfyUI, a Windows process holding 24.8-26.8 GB and averaging about four cores, in bursts) shared the
  machine, and WSL ran dockerd, containerd and `ollama serve` throughout; at 18:35 Windows had 0.7 GB free and one
  board (`gcc-asan-ld-b`) ran 5-7× slower in its cold and relink rows (its asset cook took 72 s instead of 11 s) while
  WSL itself reported no memory pressure and no swap. Every comparison below pairs arms measured in the same window,
  and the decisive whole-tree comparison is interleaved (arms rotated within each round) so a drift reaches every arm.
  Every reported row of that window was measured again on 2026-10-19 under a light load
  ([re-measured](#re-measured-2026-10-19)); the verdicts hold.
- **Harness:** `scripts/build-bench.py run --preset <p> --jobs 16 --build-dir ~/bench-1018/<p>-c --rows … --tooth
  engine/foundation/memory/include/crd/memory/construct.hpp "return nullptr; // never placement-new into a refused
  allocation" "return static_cast<T*>(nullptr); // never placement-new into a refused allocation" --define
  CRD_INPUT_ARCHIVES=<verified archives> --define CRD_COMPILER_LAUNCHER=<sccache>` from the mirror `~/cerid-src`, on
  task A's kept warm caches (`~/bench-1018/sccache-<preset>`), the tree deleted before each board, so every board is a
  fresh configure of the same path on a warm cache; `--define CRD_LINKER=~/.cerid/tools/mold/mold` for mold. Each
  board started detached through `wait-for.py start`, one heavy job at a time under the lane lock. The scripts are
  kept under `build/bench-1018b/` (the board wrappers, `relink-interleave.sh`, `gdb-proof.sh`, `tooth-lto.sh`,
  `link_interleave.py`).
- **Rows:** `cold` (a fresh tree built from the warm cache: every compile a hit, every program linked), `relink-all`
  (every linked program deleted and relinked), the teeth cycle (`teeth-break` edits `construct.hpp`, about 1,415
  compiles and every program; `teeth-restore` writes the bytes back; on the warm cache both are hits, so they measure
  hits plus links), and on the focused route `source` (a comment appended to `tests/execution/ceir/test_func.cpp`,
  building only `crd-ceir-tests`: the first a compile miss, then hits) and `link` (`crd-ceir-tests` deleted and
  relinked). "Link-edge sum" is the summed time of the row's link edges from the Ninja log; the asset cook
  (`demo_assets.crdr`), rerun because the cooker relinks, is listed apart because it bounds the row's wall time.

## Verdict

- **mold with a bounded thread pool, on the three local WSL lanes: kept.** mold's default pool is a thread per CPU for
  every link; at 16 jobs, 16 such links ran 512 threads on 32 CPUs, and the whole-tree rows were equal or slower than
  GNU ld (gcc-debug relink-all 9.2 s against 7.6 / 9.4 s; gcc-asan 16.0 against 15.0 / 15.1 s and a +9 % teeth cycle;
  clang-tsan teeth 37.6-41.5 s against 27.1-37.5 s). Two threads per link (16 jobs × 2 = the CPUs) removed the loss:
  interleaved in one tree, the summed link time of a whole-tree relink (medians) fell from 106.3 to 54.8 s on gcc-debug,
  179.2 to 81.9 s on gcc-asan and 125.5 to 25.6 s on clang-tsan, and its wall time from 7.3 to 3.8 s, 13.6 to
  10.0 s and 10.4 to 8.0 s, the last two now bounded by the asset cook they wait for (four threads: 4.0, 10.4,
  8.4 s; the full pool 8.8, 14.7, 11.8 s, slower than GNU ld on all three). On the focused route (one program,
  one source edit) mold relinks `crd-ceir-tests` in 0.7-0.8 s against 1.1-1.2 s (gcc-debug) and in 0.8-1.0 s
  against 1.4-1.5 s (gcc-asan); two threads keep that (0.8-0.9 s). The helpers (`compiler-cache.py
  configure-args`, through `wsl-lane.sh`) now configure linux-gcc-debug, linux-gcc-asan and linux-clang-tsan with
  `CRD_LINKER` and `CRD_LINKER_THREADS` = CPUs / lane jobs.
- **Equivalence:** `crd-ceir-tests` passes on gcc-asan linked by either linker, "All tests passed (18784 assertions in
  580 test cases)", with the same 931-line inventory; gdb resolves the same `test_func.cpp:23` breakpoint, frames,
  locals, a step and the sources on mold and ld programs (debug and asan), and the five gdb diag CTests
  (`crd-diag-reverse-debug-gdb-*`: the watchpoint routes everywhere, process record off the sanitizer lanes) pass on
  each; no build of either linker printed a `warning:` line or a linker diagnostic (the only warnings in every log are
  zstd's CMake deprecation, follow-up (l)). The link-time warning set still fires under mold: the 2026-10-17
  tooth fails the LTO link of `crd-perf-ui-tests` with the same `-Werror=array-bounds=` error under mold and GNU
  ld, and the restored sources link green with both (below).
- **At 4 jobs** the default pool already wins (relink-all 33.6 → 27.3 s on gcc-asan, 26.8 → 22.8 s on clang-tsan,
  21.9 → 21.1 s on gcc-debug): the loss at 16 jobs is the pools' oversubscription, not mold.
- **GCC LTO through the linker plugin (linux-gcc-shipping): no gain, so not added to the helpers.** Four LTO
  programs relink in 7.2-7.5 s with bounded mold against 7.3-8.0 s with GNU ld (the link-time compile is the
  link); the full pool was slower again (8.4-9.3 s).
- **`-gsplit-dwarf` on linux-gcc-debug: not kept.** GCC 13.3 refuses a precompiled header under it:
  `cmake_pch.hxx.gch: not used because '_gsplit_dwarf_path' defined as '...budget_pressure.cpp.dwo' not
  '...cmake_pch.hxx.dwo' [-Werror=invalid-pch]`, so split DWARF means `CRD_ENABLE_PCH=OFF`. Against the PCH-off tree
  it saves 1.2 s of a 9.1 s warm relink (link-edge sum 128 → 114 s) and leaves the teeth cycles equal (27.8-29.6 s
  against 28.5-29.2 s), while the PCH-on tree it would replace is as fast warm (teeth 25.0-44.6 / 29.7-32.9 s) and a
  cold miss without the PCH costs more (226.9 s on the PCH-off board against 202.9 s with it on task A's board). gdb
  works on the split tree: the breakpoint, frames, locals and sources resolve from 1,812 `.dwo` files (`info source`:
  "Producer is GNU C++20 13.3.0 ... -gsplit-dwarf"), and the five gdb diag CTests pass.
- **win-debug keeps the default `/INCREMENTAL`: nothing to change.** It is the fastest mode for the local loop:
  a one-file edit relinks `crd-ceir-tests` in 0.2 s of link time (0.8-0.9 s wall), a library edit relinks 203
  programs in 5.3-5.5 s, the full relink of all 212 takes 5.0 s. `/INCREMENTAL:NO` doubles the link time
  (9.8-10.0 s, link sums 136-139 s against 65-70 s); `/DEBUG:FASTLINK` ties the default on edits (5.4-5.8 s,
  0.8 s) and is twice as slow on the full relink (10.6-10.8 s), so it is not kept either.

## Losses

- mold with its default pool at 16 jobs: gcc-asan cold +10 % (17.5 against 15.8 / 16.0 s), relink-all +6 %, teeth
  +9 % (mean 35.0 against 32.1 s), peak pressure 7.1 against 4.1-4.3 GiB; clang-tsan teeth +29 % (mean 39.2 against
  30.4 s, both after 17:03). This is the configuration the helpers do not use.
- A smaller pool slows one large link: the largest ASan program links in 0.52-0.55 s with one thread against
  0.29-0.33 s with the full pool (raw linker timing below; two threads were not timed raw). On the focused route two
  threads measured 0.8-0.9 s against the full pool's 0.8-1.1 s, inside the rows' spread.
- Split DWARF: the PCH it forces off (above); the split board with mold at 16 jobs was slower still (cold 20.2 s).
- Converting a mirror tree to mold (or back) relinks every program once (about 10-15 s at 16 jobs).

## Whole tree at 16 jobs: GNU ld against mold's default pool

| Board | Start | Linker | cold | relink-all | relink link-edge sum | teeth break | teeth restore | peak GiB |
|---|---|---|---|---|---|---|---|---|
| gcc-debug-ld | 16:18 | GNU ld | 9.8 | 7.6 | 108 s (cook 1.2 s) | 25.0 / 40.9 | 44.6 / 31.1 | 2.0 |
| gcc-debug-mold | 16:22 | mold | 10.9 | 9.2 | 137 s (cook 1.7 s) | 27.6 / 27.6 | 27.6 / 27.4 | 2.3 |
| gcc-debug-ld-again | 16:26 | GNU ld | 11.2 | 9.4 | 136 s (cook 1.7 s) | 32.9 / 29.7 | 30.7 / 30.9 | 2.0 |
| gcc-asan-ld | 16:41 | GNU ld | 15.8 | 15.0 | 195 s (cook 11.3 s) | 31.1 / 31.1 | 31.4 / 32.1 | 4.1 |
| gcc-asan-mold | 16:45 | mold | 17.5 | 16.0 | 195 s (cook 13.4 s) | 35.9 / 39.8 | 32.1 / 32.3 | 7.1 |
| gcc-asan-ld-again | 16:50 | GNU ld | 16.0 | 15.1 | 198 s (cook 11.4 s) | 36.1 / 31.7 | 31.5 / 32.1 | 4.3 |
| clang-tsan-ld | 17:07 | GNU ld | 21.8 | 16.0 | 217 s (cook 12.9 s) | 30.1 / 27.1 | 27.3 / 37.5 | 2.6 |
| clang-tsan-mold | 17:11 | mold | 15.8 | 13.4 | 120 s (cook 11.7 s) | 38.8 / 37.6 | 39.0 / 41.5 | 4.7 |
| clang-tsan-ld-again | 17:16 | GNU ld | 13.2 | 11.8 | 150 s (cook 9.7 s) | 30.4 / 28.2 | 33.3 / 29.1 | 2.1 |
| gcc-asan-ld-b | 18:32 | GNU ld | 91.6 | 106.4 | 1545 s (cook 36.5 s) | 55.5 / 47.1 | 42.2 / 37.5 | 4.5 |
| gcc-asan-mold-t2 | 18:41 | mold, 2 threads | 14.3 | 13.9 | 118 s (cook 11.2 s) | 31.8 / 43.7 | 47.3 / 35.6 | 6.3 |
| gcc-asan-mold-b | 18:46 | mold | 25.3 | 20.2 | 260 s (cook 16.2 s) | 47.2 / 52.1 | 49.8 / 58.9 | 6.1 |
| clang-tsan-mold-t2 | 18:52 | mold, 2 threads | 13.7 | 9.6 | 46 s (cook 8.5 s) | 21.4 / 21.8 | 22.7 / 21.9 | 2.5 |

The clang-tsan boards (from 17:07) and the last four ran after 17:03, under the user's process; `gcc-asan-ld-b` is
the disturbed board described above.

## Interleaved whole-tree relink (the decisive comparison)

`relink-interleave.sh <tree> <cache> 3 ld mold t2 t4`: one built tree, `CRD_LINKER` reconfigured per step (`t2`/`t4`:
mold with `--threads=2`/`4` through a measurement wrapper, `mold-threads-wrapper.sh`, whose `ld.mold` adds the option;
the configuration the helpers write passes it as `CRD_LINKER_THREADS`), so each build relinks all 206 programs and
recompiles nothing; wall, link count, link-edge sum and the slowest edge per build.

| Lane | Arm | Wall per round (s) | Link-edge sum per round (s) | Median wall | Median link sum | Links |
|---|---|---|---|---|---|---|
| linux-clang-tsan | GNU ld | 12.8 / 10.3 / 10.4 | 170.7 / 124.9 / 125.5 | 10.4 | 125.5 | 206 |
| linux-clang-tsan | mold, full pool | 11.8 / 11.1 / 11.9 | 90.5 / 88.4 / 97.5 | 11.8 | 90.5 | 206 |
| linux-clang-tsan | mold, 2 threads | 7.7 / 8.0 / 15.6 | 25.4 / 25.6 / 25.8 | 8.0 | 25.6 | 206 |
| linux-clang-tsan | mold, 4 threads | 8.4 / 9.1 / 8.0 | 31.0 / 29.3 / 30.1 | 8.4 | 30.1 | 206 |
| linux-gcc-asan | GNU ld | 17.2 / 13.2 / 13.6 | 193.5 / 176.3 / 179.2 | 13.6 | 179.2 | 206 |
| linux-gcc-asan | mold, full pool | 14.7 / 15.0 / 14.5 | 180.9 / 183.4 / 172.1 | 14.7 | 180.9 | 206 |
| linux-gcc-asan | mold, 2 threads | 10.0 / 10.0 / 10.1 | 82.6 / 81.0 / 81.9 | 10.0 | 81.9 | 206 |
| linux-gcc-asan | mold, 4 threads | 10.5 / 10.2 / 10.4 | 86.2 / 86.3 / 91.0 | 10.4 | 86.3 | 206 |
| linux-gcc-debug | GNU ld | 8.0 / 7.3 / 7.2 | 117.3 / 106.3 / 105.5 | 7.3 | 106.3 | 206 |
| linux-gcc-debug | mold, full pool | 9.3 / 8.6 / 8.7 / 9.0 | 138.6 / 129.6 / 130.5 / 132.5 | 8.8 | 131.5 | 206 |
| linux-gcc-debug | mold, 2 threads | 3.8 / 3.8 / 3.7 | 54.7 / 55.7 / 54.8 | 3.8 | 54.8 | 206 |
| linux-gcc-debug | mold, 4 threads | 4.0 / 3.9 / 4.5 | 59.5 / 56.6 / 65.6 | 4.0 | 59.5 | 206 |

## Whole tree at 4 jobs

| Board | Start | Linker | cold | relink-all | relink link-edge sum | teeth break | teeth restore | peak GiB |
|---|---|---|---|---|---|---|---|---|
| gcc-debug-ld-j4 | 16:31 | GNU ld | 26.3 | 21.9 | 85 s (cook 1.1 s) | - | - | 0.6 |
| gcc-debug-mold-j4 | 16:33 | mold | 26.3 | 21.1 | 83 s (cook 1.2 s) | - | - | 1.0 |
| gcc-asan-ld-j4 | 16:55 | GNU ld | 40.7 | 33.6 | 128 s (cook 7.6 s) | - | - | 1.6 |
| gcc-asan-mold-j4 | 16:58 | mold | 32.9 | 27.3 | 105 s (cook 9.1 s) | - | - | 1.8 |
| clang-tsan-ld-j4 | 17:20 | GNU ld | 33.8 | 26.8 | 103 s (cook 7.7 s) | - | - | 1.1 |
| clang-tsan-mold-j4 | 17:23 | mold | 33.1 | 22.8 | 88 s (cook 14.1 s) | - | - | 2.0 |

## Focused route: one program

`CRD_BOARD_TARGETS=crd-ceir-tests,crd-diag-corruption-specimen` (the specimen is the gdb CTests' program),
`--source tests/execution/ceir/test_func.cpp`, `--link-target crd-ceir-tests`.

| Board | Start | Linker | cold | source (edit, compile, link) | link (relink only) |
|---|---|---|---|---|---|
| gcc-debug-one-ld | 18:07 | GNU ld | 2.5 | 1.2 / 1.1 / 1.2 | 1.0 / 1.1 / 1.1 |
| gcc-debug-one-mold | 18:09 | mold | 2.4 | 0.7 / 0.7 / 0.8 | 0.8 / 0.8 / 0.9 |
| gcc-debug-one-ld-again | 18:11 | GNU ld | 2.3 | 1.2 / 1.2 / 1.1 | 1.1 / 1.0 / 1.1 |
| gcc-asan-one-ld | 18:16 | GNU ld | 3.1 | 1.5 / 1.4 / 1.5 | 1.4 / 1.4 / 1.5 |
| gcc-asan-one-mold | 18:20 | mold | 2.4 | 0.8 / 0.8 / 1.0 | 1.1 / 1.0 / 1.1 |
| gcc-asan-one-mold-t2 | 18:25 | mold, 2 threads | 4.4 | 0.9 / 0.9 / 0.9 | 0.9 / 0.8 / 0.8 |
| gcc-asan-one-ld-again | 18:28 | GNU ld | 6.1 | 3.5 / 5.0 / 1.6 | 1.4 / 1.4 / 1.3 |

Single link commands, timed three times each on an idle host (16:36-16:39), from the tree's `ninja -t commands`:

| Program | Lane | Through the driver | GNU ld (`ld.bfd`, the collect2 command) | mold, full pool | mold, one thread |
|---|---|---|---|---|---|
| crd-ceir-tests (39.3 MB) | linux-gcc-debug | ld 0.70-0.96 s, mold 0.36-0.71 s | - | 0.155-0.178 s | 0.125-0.137 s |
| crd-gpu-context-vulkan-tests | linux-gcc-asan | - | 1.76-1.88 s | 0.29-0.33 s | 0.52-0.55 s |

## Split DWARF (linux-gcc-debug)

The first split board failed its cold row at the first unit of crd-core (`-Winvalid-pch`, above); every split board
below has `CRD_ENABLE_PCH=OFF`. Cache misses (the first board of a configuration on the kept cache):

| Board | Start | Configuration | cold (misses) | teeth break (misses) | teeth restore (hits) | peak GiB |
|---|---|---|---|---|---|---|
| task A `linux-gcc-debug c` | 10:12 | PCH on, GNU ld (the reference) | 202.9 (2,055) | 194.7 (1,412) | 25.2 | 13.5 |
| gcc-debug-nopch-prime | 17:52 | PCH off, GNU ld | 226.9 (1,566) | 279.9 (1,352) | 32.9 | 14.2 |
| gcc-debug-split-mold-prime | 17:29 | PCH off, `-gsplit-dwarf`, mold (full pool) | 262.2 (1,652) | 333.6 (1,415) | 55.3 | 13.7 |

Warm boards of the same configurations:

| Board | Start | Linker | cold | relink-all | relink link-edge sum | teeth break | teeth restore | peak GiB |
|---|---|---|---|---|---|---|---|---|
| gcc-debug-nopch | 18:02 | GNU ld | 12.7 | 9.1 | 128 s (cook 2.0 s) | 28.8 / 29.2 | 28.5 / 29.0 | 2.1 |
| gcc-debug-split-ld | 17:46 | GNU ld | 11.3 | 7.9 | 114 s (cook 1.8 s) | 28.1 / 29.5 | 27.8 / 29.6 | 2.3 |
| gcc-debug-split-mold | 17:41 | mold | 20.2 | 14.7 | 216 s (cook 3.6 s) | 40.8 / 64.1 | 37.4 / 35.3 | 2.3 |

## Link-time optimization (linux-gcc-shipping)

A focused route of four test programs (`crd-memory-tests`, `crd-scene-tests`, `crd-containers-tests`,
`crd-perf-ui-tests`, the shape of a local shipping check) on a new cache: the prime (GNU ld) missed all 432
compiles, cold 38.3 s; every later board hit all 432. Each link runs the link-time compile through the linker
plugin (`-flto=auto`), so the linker itself is a small part of it. The mold boards configured with the module's
`CRD_LINKER_THREADS=2` (the first real build of the option: `-Wl,--threads=2` on all 206 link lines of the tree).

| Board | Start | Linker | cold | relink-all | Link-edge sums |
|---|---|---|---|---|---|
| gcc-shipping-ld | 20:14 | GNU ld | 9.8 | 8.0 / 8.0 | 32.4 / 30.4 s |
| gcc-shipping-mold-t2 | 20:16 | mold, 2 threads | 9.0 | 7.2 / 7.5 | 27.6 / 29.3 s |
| gcc-shipping-mold | 20:19 | mold, full pool | 14.8 | 9.3 / 8.4 | 35.4 / 30.5 s |
| gcc-shipping-ld-again | 20:21 | GNU ld | 9.3 | 7.6 / 7.3 | 28.7 / 27.7 s |
| gcc-shipping-mold-final | 20:24 | mold, 2 threads | 15.3 | - | - |

Bounded mold is within the GNU ld boards' spread (relink 7.2-7.5 s against 7.3-8.0 s), so the GCC LTO presets
(linux-gcc-shipping, -release, -relwithdebinfo) keep GNU ld in the helpers; `CRD_LINKER` still works there when asked
(below).

## The link-time warning set under mold

`tooth-lto.sh` on the `gcc-shipping-mold-final` tree (mold, two threads): the 2026-10-17 tooth
([link-time warning set](../sessions/2026-10-17-diag-11b-link-time-warning-set.md#teeth)), a cross-file
out-of-bounds read (`crd_teeth_read_at` appended to `test_panel_helpers.cpp`, a `[teeth]` case reading index 6 of an
`int[4]` through it in `test_profiler_sources.cpp`), built into `crd-perf-ui-tests`, with byte-exact backups.

| Step | Linker | Result |
|---|---|---|
| break | mold | exit 1: `test_panel_helpers.cpp:155:24: error: array subscript 6 is outside array bounds of 'const int[4]' [-Werror=array-bounds=]`, `lto-wrapper: fatal error: make returned 2 exit status`, `mold: fatal: lto-wrapper failed` |
| break, the tree reconfigured with `-DCRD_LINKER= -DCRD_LINKER_THREADS=` | GNU ld | exit 1: the same error, `/usr/bin/ld: error: lto-wrapper failed` |
| restored (backups copied back, touched), relinked | GNU ld | exit 0, no warning line |
| reconfigured with mold and two threads, relinked | mold | exit 0, no warning line; the program's `.comment` names mold 3.0.0; the suite passes, "All tests passed (612 assertions in 14 test cases)" |

The sources compared byte-exact with their backups afterwards and `git diff` showed nothing under `tests/`.

## win-debug link modes

`link_interleave.py 2 inc noinc fast` in task A's warm tree `build/bench-1018-win-debug-c` (PCH off, the pinned
sccache): only the Debug linker flags are reconfigured per step (`CMAKE_{EXE,SHARED,MODULE}_LINKER_FLAGS_DEBUG`),
the arms rotate within each round, round 0 primed the edits and is left out (its first arm relinked nothing). Rows:
`relink-all` (the reconfigure's relink of all 212 programs), `source-all` and its restore (a comment appended to
`logger.cpp`: one compile, 203 programs relink), `source-one` and its restore (`test_func.cpp`, building
`crd-ceir-tests` only), `link-one` (`crd-ceir-tests.exe` deleted and relinked). Cells: wall seconds (link-edge
sum) for rounds 1 and 2; no step printed a warning line.

| Row | `/INCREMENTAL` (the default) | `/INCREMENTAL:NO` | `/DEBUG:FASTLINK /INCREMENTAL` |
|---|---|---|---|
| relink-all | 5.0 (65.1) / 5.0 (65.5) | 9.8 (138.4) / 9.8 (138.0) | 10.8 (152.9) / 10.6 (151.6) |
| source-all | 5.4 (68.3) / 5.3 (65.5) | 9.9 (136.2) / 10.0 (138.0) | 5.5 (69.9) / 5.4 (67.8) |
| source-all-restore | 5.5 (69.5) / 5.4 (68.6) | 10.0 (139.0) / 10.0 (137.3) | 5.8 (74.0) / 5.6 (70.2) |
| source-one | 0.9 (0.2) / 0.8 (0.2) | 1.1 (0.5) / 1.1 (0.4) | 0.8 (0.2) / 0.8 (0.2) |
| source-one-restore | 0.8 (0.2) / 0.8 (0.2) | 1.0 (0.4) / 1.1 (0.4) | 0.8 (0.2) / 0.8 (0.2) |
| link-one | 0.9 (0.5) / 0.9 (0.5) | 0.8 (0.4) / 0.8 (0.4) | 0.9 (0.5) / 1.2 (0.8) |

The compiles of these rows ran through an sccache server on sccache's default cache rather than task A's: the
helper's server start failed at the run's start (the fault below), so round 0 missed and later rounds hit; no row
compares a compile across arms.

## A fault the boards found: the WSL relay and the sccache port

At the start of the win-debug run, `compiler-cache.py server` failed (`sccache --start-server`: "Server startup
failed: Address in use", exit 2) and `build-target.bat` went on, its `for /f` over the command hiding the failure,
so the build ran on whatever server the first compile started. Cause, reproduced: WSL2 relays every localhost
port a Linux process listens on to Windows (`wslrelay.exe`). A Linux sccache server left by a lane build on the
default port 4226 then answered the Windows clients: `sccache --show-stats` on Windows reported the cache
`/home/yatiyr/.cache/cerid-sccache`, and a Windows server could not start until it exited. Two more findings on the
way: a statistics query with no server running starts one that exits with the query (a `--start-server` server
and one a compile starts stay), so the helper's check could pass on a server the build never saw; and a listener
that accepts and never answers holds a statistics query for its whole 60 s timeout.

Repaired in the helpers: the Linux server has its own port (4227; Windows keeps 4226; `CRD_SCCACHE_PORT` overrides,
`compiler-cache.py port` prints it, `wsl-lane.sh` exports it as `SCCACHE_SERVER_PORT` for every lane command and
the server step prints it); the cache is asked only when something listens, a server is started explicitly
otherwise, and the proof is a listening server on this cache; a server that cannot start fails the step, and
`build-target.bat` (an environment file and `errorlevel`) and `wsl-lane.sh` (an assignment under `set -e`) stop
the build on it. Afterwards a lane build leaves its server on 4227 with the Linux cache while the Windows helper's
server runs on 4226 with `build\sccache`, and a Windows client on the default port reaches the Windows server;
with a silent listener holding 4226, `build-target.bat` exits 1 with "the sccache server did not start on
D:\Dev\cerid\build\sccache (port 4226, exit 124)".

<a id="re-measured-2026-10-19"></a>
## Re-measured, 2026-10-19: the rows of the contended window

Every board and the clang-tsan interleave that ran from 17:03 to 19:17 on 2026-10-18, while the image generator
loaded the machine, ran again on 2026-10-19 between 02:05 and 04:36 local, with the same wrapper
(`build/task-c/phase_b.sh`, after task B's `board.sh` and `relink-interleave.sh`), the same trees, warm caches,
rows and tooth, from the mirror set to task B's sources (`591d4232`; `scripts/` at the then HEAD, so each board
recorded the host; the mirror was restored and checked in full afterwards). Every board recorded the host: WSL idle;
on Windows a digital audio workstation and a browser (2.6-5.5 cores) and, in bursts, another project's test
programs (up to 5.3 cores more; 10.9-13.9 cores busy in the snapshots of four boards after 04:11); the falling
Windows memory is the WSL VM's own page cache (`vmmemWSL` up to 27.8 GiB). Nothing at the scale of the
2026-10-18 load (about four cores and 25 GB held throughout). The tables above are kept as measured.

Whole tree at 16 jobs (teeth: break / restore, twice):

| Board | Start | Linker | cold | relink-all | relink link-edge sum | teeth break | teeth restore | peak GiB |
|---|---|---|---|---|---|---|---|---|
| clang-tsan-ld | 02:05 | GNU ld | 26.2 | 10.7 | 135 s (cook 8.9 s) | 25.6 / 25.9 | 25.6 / 26.1 | 2.1 |
| clang-tsan-mold | 02:09 | mold | 14.0 | 13.2 | 113 s (cook 11.7 s) | 26.9 / 26.3 | 35.2 / 26.2 | 3.8 |
| clang-tsan-ld-again | 02:13 | GNU ld | 12.2 | 10.7 | 142 s (cook 9.4 s) | 25.9 / 26.4 | 26.0 / 26.2 | 2.3 |
| clang-tsan-mold-t2 | 02:23 | mold, 2 threads | 10.6 | 8.5 | 37 s (cook 7.8 s) | 20.4 / 20.6 | 19.7 / 19.8 | 2.1 |
| gcc-asan-ld-b | 02:27 | GNU ld | 19.4 | 15.9 | 209 s (cook 11.7 s) | 35.6 / 35.7 | 35.0 / 35.8 | 4.5 |
| gcc-asan-mold-t2 | 02:32 | mold, 2 threads | 13.8 | 11.9 | 109 s (cook 10.3 s) | 29.1 / 29.9 | 31.2 / 28.2 | 7.0 |
| gcc-asan-mold-b | 02:36 | mold | 19.4 | 17.4 | 237 s (cook 15.1 s) | 36.6 / 36.8 | 47.0 / 46.1 | 6.7 |

Whole tree at 4 jobs:

| Board | Start | Linker | cold | relink-all | relink link-edge sum | peak GiB |
|---|---|---|---|---|---|---|
| clang-tsan-ld-j4 | 02:17 | GNU ld | 29.4 | 23.3 | 91 s (cook 7.1 s) | 1.0 |
| clang-tsan-mold-j4 | 02:20 | mold | 22.1 | 15.0 | 58 s (cook 10.2 s) | 1.5 |

Interleaved whole-tree relink, linux-clang-tsan (03:54-04:11; arms rotated within each round):

| Arm | Wall per round (s) | Link-edge sum per round (s) | Median wall | Median link sum | Links |
|---|---|---|---|---|---|
| GNU ld | 11.3 / 12.2 / 10.0 | 142.5 / 154.3 / 118.3 | 11.3 | 142.5 | 206 |
| mold, full pool | 13.7 / 13.0 / 28.0 | 109.6 / 116.8 / 390.5 | 13.7 | 116.8 | 206 |
| mold, 2 threads | 8.2 / 8.8 / 9.0 | 28.1 / 39.9 / 33.8 | 8.8 | 33.8 | 206 |
| mold, 4 threads | 8.5 / 7.7 / 9.7 | 33.3 / 28.7 / 37.1 | 8.5 | 33.3 | 206 |

Focused route, one program (`crd-ceir-tests`):

| Board | Start | Linker | cold | source (edit, compile, link) | link (relink only) |
|---|---|---|---|---|---|
| gcc-debug-one-ld | 02:42 | GNU ld | 1.8 | 0.9 / 0.8 / 0.8 | 0.9 / 0.8 / 0.8 |
| gcc-debug-one-mold | 02:44 | mold | 1.5 | 0.5 / 0.5 / 0.6 | 0.5 / 0.5 / 0.5 |
| gcc-debug-one-ld-again | 02:46 | GNU ld | 1.8 | 0.9 / 0.8 / 0.9 | 0.7 / 0.8 / 0.8 |
| gcc-asan-one-ld | 02:47 | GNU ld | 2.9 | 1.4 / 1.3 / 1.5 | 1.1 / 1.1 / 1.8 |
| gcc-asan-one-mold | 02:50 | mold | 1.5 | 0.5 / 0.7 / 0.9 | 0.6 / 1.1 / 0.5 |
| gcc-asan-one-mold-t2 | 02:52 | mold, 2 threads | 1.7 | 0.6 / 0.7 / 0.9 | 0.8 / 0.8 / 0.5 |
| gcc-asan-one-ld-again | 02:55 | GNU ld | 3.2 | 1.3 / 1.5 / 1.5 | 1.4 / 1.4 / 1.7 |

Split DWARF, linux-gcc-debug. Cache misses, each configuration on a fresh empty cache (task B's primes ran on the
kept cache, so some of their units hit):

| Board | Start | Configuration | cold (misses) | teeth break (misses) | teeth restore (hits) | peak GiB |
|---|---|---|---|---|---|---|
| gcc-debug-pchon-prime | 03:11 | PCH on, GNU ld | 215.2 (2,055) | 191.4 (1,412) | 25.2 | 13.7 |
| gcc-debug-nopch-prime | 03:20 | PCH off, GNU ld | 327.2 (1,807) | 230.3 (1,412) | 29.8 | 13.6 |
| gcc-debug-split-mold-prime | 03:33 | PCH off, `-gsplit-dwarf`, mold (full pool) | 258.4 (1,810) | 212.3 (1,415) | 29.8 | 13.4 |

Warm, GNU ld, first in sequence (02:58-03:11) and then rotated PCH on, split, PCH off, PCH off, split, PCH on
(04:11-04:36), because two PCH-on boards at 03:44-03:53 met a burst of the other load (teeth 30.2-48.2 s; kept in
`build/bench-1018c`, not used):

| Board | Start | Configuration | cold | relink-all | relink link-edge sum | teeth break | teeth restore |
|---|---|---|---|---|---|---|---|
| gcc-debug-split-mold | 02:58 | split, mold (full pool) | 10.8 | 7.9 | 117 s | 26.4 / 26.6 | 26.2 / 26.3 |
| gcc-debug-split-ld | 03:02 | split | 8.2 | 6.1 | 89 s | 23.6 / 23.8 | 23.7 / 24.0 |
| gcc-debug-nopch | 03:06 | PCH off | 9.3 | 7.1 | 103 s | 24.5 / 25.3 | 24.8 / 25.2 |
| gcc-debug-pchon-ld-r1 | 04:11 | PCH on | 10.1 | 7.7 | 125 s | 25.6 / 25.8 | 25.6 / 26.2 |
| gcc-debug-split-ld-r1 | 04:15 | split | 8.9 | 6.4 | 93 s | 25.4 / 43.6 | 25.1 / 29.8 |
| gcc-debug-nopch-r1 | 04:19 | PCH off | 10.8 | 8.8 | 127 s | 35.9 / 26.5 | 32.4 / 25.8 |
| gcc-debug-nopch-r2 | 04:23 | PCH off | 9.2 | 7.5 | 123 s | 25.9 / 27.5 | 26.2 / 28.2 |
| gcc-debug-split-ld-r2 | 04:28 | split | 8.7 | 6.5 | 94 s | 48.5 / 24.9 | 25.1 / 25.5 |
| gcc-debug-pchon-ld-r2 | 04:32 | PCH on | 9.6 | 7.4 | 106 s | 25.6 / 25.0 | 25.6 / 25.0 |

What holds and what moved:
- **Bounded mold: holds.** Teeth means at 16 jobs: clang-tsan 25.9 s on GNU ld, 20.1 s with two threads (−22 %),
  28.7 s with the full pool (+10 %, where the contended boards gave +29 %); gcc-asan 35.5, 29.6 (−17 %) and 41.6 s
  (+17 %, against +9 %). The interleaved clang-tsan relink: link sum 142.5 → 33.8 s (medians, a quarter; contended
  125.5 → 25.6 s), wall 11.3 → 8.8 s; four threads tie two. The full pool's loss at 16 jobs is real in both windows;
  its size moves with the load.
- **At 4 jobs the default pool wins: holds** (clang-tsan relink-all 23.3 → 15.0 s, against 26.8 → 22.8 s).
- **The focused route: holds.** mold relinks `crd-ceir-tests` in 0.5-0.6 s against 0.7-0.9 s on gcc-debug, and in
  0.5-1.1 s (two threads 0.5-0.9 s) against 1.1-1.8 s on gcc-asan; the contended `gcc-asan-one-ld-again` (3.5 /
  5.0 s source rows) measures 1.3-1.5 s now.
- **The disturbed board: explained.** `gcc-asan-ld-b` builds cold in 19.4 s and relinks in 15.9 s (contended:
  91.6 and 106.4 s), beside the uncontended gcc-asan-ld's 15.8 and 15.0 s.
- **Split DWARF not kept: holds, with the warm gap measured.** Warm, interleaved, PCH on and split tie on the teeth
  cycle (medians 25.6 and 25.5 s; split's two slow rows, 43.6 and 48.5 s, met the other load's bursts) and split
  relinks the tree about 1 s faster (6.1-6.5 against 7.4-7.7 s, link sums 89-94 against 106-125 s); on a cache miss
  the PCH it forces off costs more: a cold build of 258.4 s with split and 327.2 s with the PCH merely off, against
  215.2 s with it on. PCH off without split is the slowest warm arm (median 27.0 s).

