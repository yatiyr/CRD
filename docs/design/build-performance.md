# Build performance: measured boards, qualified compiler caching and bounded jobs

<!-- doc-role: reference -->
> Contract for REPO.DEV.7. Status lives only in [ROADMAP](../ROADMAP.md#slice-repo.dev.7); the accepted research is
> [compiler caching and parallelism](../research/2026-09-12-large-cpp-development-and-ci.md#measure-compiler-caching-and-parallelism);
> the measured boards are [2026-09-14](../bench/2026-09-14-repo-dev7-build-board.md),
> [2026-10-18](../bench/2026-10-18-build-speed-cache-mirror.md) (the local routes and the hosted cache) and
> [2026-10-18, linking](../bench/2026-10-18-build-speed-linking.md) (the linker, split DWARF, incremental linking),
> owned by [DIAG.11b](../ROADMAP.md#slice-diag.11b)'s focused-local-route and CI-cost contract; the module is
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

[CrdBuildPerformance.cmake](../../cmake/CrdBuildPerformance.cmake) adds five cache entries, all empty by default,
so the default graph is byte-identical (proved on `build/win-debug`: 292 targets, 1,814 compile commands and 5,786
Ninja statements unchanged except the CMake re-run rule listing the new module; `CRD_LINKER` and
`CRD_LINKER_THREADS` are described under [The linker](#the-linker)):

- `CRD_COMPILER_LAUNCHER=<path>` sets `CMAKE_C/CXX_COMPILER_LAUNCHER` for Makefile and Ninja generators, which are
  the generators CMake applies launchers to. On MSVC-compatible compilers it also sets
  `CMAKE_MSVC_DEBUG_INFORMATION_FORMAT` to `Embedded` for Debug and RelWithDebInfo (policy CMP0141 is NEW under the
  3.25 minimum), and it warns when `CRD_ENABLE_PCH` is still on, because nothing would be cached; it never flips the
  PCH option itself. Since 2026-10-18 Shipping is cached as well: the module sets `CRD_EMBEDDED_DEBUG_INFO`, the one
  switch that makes the root build file give Shipping `/Z7` in place of its explicit `/Zi` and makes
  [CrdPackage.cmake](../../cmake/CrdPackage.cmake) package the archives' debug information as `embedded` (no compile
  PDB exists to install). `/DEBUG:FULL` still writes each program's full PDB, LTCG still runs at the link, and the
  link itself is never cached. clang-cl writes byte-identical objects for `/Zi` and `/Z7` (probed on
  `assert.cpp`), so on clang-cl the switch changes nothing but cacheability. Without the launcher every
  configuration keeps `/Zi` and its compile PDBs. On Clang (not clang-cl) the launcher also adds
  `-Xclang -fno-pch-timestamp`: Clang stores every input's modification time in a precompiled header and sccache
  hashes the `.pch` a unit includes, so a header rebuilt in a fresh tree or checkout (same text, new times) missed
  for every unit that includes it (249 of 2,059 compiles on the warm linux-clang-tsan board, every one on a hosted
  checkout); without the times the `.pch` bytes follow the text (probed), the objects are byte-identical, and Ninja
  still rebuilds a precompiled header whenever one of its inputs changes. A Visual Studio generator prints that the solution runs uncached
  and touches nothing: native MSBuild caching needs its own proof, and the truthful IDE path is the uncached one.
- `CRD_COMPILE_JOBS=<n>` and `CRD_LINK_JOBS=<n>` declare Ninja job pools (`crd_compile`, `crd_link`) and route
  compiles and links through them; other generators report that pools are ignored. Non-integers fail the configure.

`scripts/install-sccache.py` acquires the pinned binary (registry entries `tools/sccache-windows` and
`tools/sccache-linux`: archive digest, member, member digest, version check) and prints the launcher path.

## Local cached trees

The presets and the default graph stay uncached; the local helpers turn the launcher on (2026-10-18, DIAG.11b
build-speed batch, [board](../bench/2026-10-18-build-speed-cache-mirror.md)).
[compiler-cache.py](../../scripts/compiler-cache.py) owns the rules:

- `configure-args <preset>` prints the entries a local configure adds: `CRD_COMPILER_LAUNCHER=<pinned sccache>` for
  a Ninja preset, plus `CRD_ENABLE_PCH=OFF` when the compiler is cl or clang-cl (their precompiled header is not
  cacheable). A Visual Studio preset and the clang-tidy presets get nothing (no launcher applies; the analysis runs
  on every unit), and neither do the MSVC LTCG presets (cl with interprocedural optimization: win-release,
  win-relwithdebinfo, win-shipping, win-shipping-profile): their compiles only parse and the link generates the code,
  so on the focused win-shipping route a cached restore saved 17 % while the cold miss cost 42 % and converting the
  tree rebuilds every LTCG program; such a tree is cached only when its configure names the launcher explicitly.
  clang-cl Shipping is cached (restore 40 % faster, warm rebuild 1.85×). `CRD_SCCACHE=off` prints the reverse
  entries, which return a converted tree to the default graph;
  `CRD_SCCACHE=<path>` names another binary. The binary defaults to the one
  `install-sccache.py --destination ~/.cerid/tools/sccache` installs (on Windows `%USERPROFILE%\.cerid\...`; a tool
  launched from a packaged application writes `%LOCALAPPDATA%` into the package's private copy, which other
  processes do not see). Nothing is added while no binary is installed.
- `server <build-dir>` reads the tree's `CRD_COMPILER_LAUNCHER`; for a cached tree it makes sure that binary's
  server runs on the local cache (`CRD_SCCACHE_DIR`, else `<checkout>/build/sccache` on Windows and
  `~/.cache/cerid-sccache` on Linux, outside the WSL mirror; `CRD_SCCACHE_CACHE_SIZE`, else 30G) and prints that
  environment. The server is started explicitly with its streams closed (measured: neither `--start-server` nor a
  statistics query holds the caller's pipes), and a server already running on another cache (a board's scratch
  cache) is restarted, because every client reaches the one server on its port whatever its own environment. The
  cache is asked only when something listens on the port: a statistics query with no server starts one that exits
  with the query (measured 2026-10-18; a `--start-server` server and one a compile starts stay). The port is
  `CRD_SCCACHE_PORT`, else 4226 on Windows and 4227 on Linux, exported as `SCCACHE_SERVER_PORT` (`port` prints it):
  WSL2 relays every localhost port a Linux process listens on to Windows (`wslrelay.exe`), so on the shared default
  port a Linux server left by a lane build answered the Windows clients (its cache a Linux path) and a Windows server
  could not start ("Address in use"). A server that cannot start fails the command, and both helpers stop the build
  on it; until 2026-10-18 they read its output through `for /f` and `eval "$(...)"`, which hid the failure, and the
  build ran on whatever server the first compile started.
- [configure-preset.bat](../../scripts/configure-preset.bat) puts the entries before `--preset`, so options given
  on its command line come later and win; [build-target.bat](../../scripts/build-target.bat) applies the server
  environment before it builds; [wsl-lane.sh](../../scripts/wsl-lane.sh) does both on WSL. The default job count of
  `build-target.bat` (four) is unchanged.

A cached compile is keyed by the compiler, its arguments and the preprocessed (or, in sccache's direct mode, the
source) text, and the arguments name the build directory, so a tree keeps its path: a cached tree is the same
`build/<preset>` directory reconfigured through the helper, not a new preset. The cache never changes what a compile
produces: a hit returns the object, standard output and diagnostics the compiler produced for those inputs, a
compile that fails (a `/WX` warning included) is not stored, and a changed precompiled header misses (probed on
GCC's `-include` and Clang's `-include-pch`).

## The linker

The module's `CRD_LINKER=<mold>` and `CRD_LINKER_THREADS=<n>` are empty by default: every preset, the default graph
and every hosted lane link with the toolchain's linker (2026-10-18, DIAG.11b build-speed task B,
[board](../bench/2026-10-18-build-speed-linking.md)).

- With `CRD_LINKER` on GCC or Clang, every C and C++ link line gets `-fuse-ld=mold -B<dir>`: GCC takes no path in
  `-fuse-ld`, so both drivers find the `ld.mold` that the installer links beside the binary in the `-B` directory
  (CMake 3.29's `CMAKE_LINKER_TYPE` does the same; the WSL reference host runs 3.28). `CRD_LINKER_THREADS` adds
  `-Wl,--threads=<n>`. No compile command changes, so a cached tree keeps every cache key, and CMake's compiler
  probes link with the toolchain's linker. A path that is not an installed `mold` with its `ld.mold`, or a thread
  count that is not a positive integer, fails the configure; MSVC-compatible compilers report the entry as ignored,
  and so does the module for a thread count without a linker.
- `scripts/install-mold.py --destination ~/.cerid/tools/mold` installs the pinned mold 3.0.0 (registry entry
  `tools/mold-linux`: the archive's digest, the `bin/mold` member and its digest, the reported version, the `ld.mold`
  link; Linux only).
- The local helpers link the measured lanes with it: on a Linux host
  [compiler-cache.py](../../scripts/compiler-cache.py) `configure-args` adds both entries for `MOLD_PRESETS`
  (linux-gcc-debug, linux-gcc-asan, linux-clang-tsan) when the pinned binary is installed, so
  [wsl-lane.sh](../../scripts/wsl-lane.sh) configures those trees with it. The thread count is the CPUs divided by
  the lane's job count (`CRD_LANE_JOBS`, 16): two on the 32-CPU host. mold's default pool is a thread per CPU for
  every link, and the 16 links of a parallel build then ran 512 threads on 32 CPUs and gained nothing over GNU ld
  (whole-tree relink and teeth rows equal or slower); bounded, the summed link time of a whole-tree relink fell to
  about half of GNU ld's on linux-gcc-debug and linux-gcc-asan and a fifth to a quarter on linux-clang-tsan, its wall
  time by 22-48 % (the contended rows re-measured on 2026-10-19:
  [board](../bench/2026-10-18-build-speed-linking.md#re-measured-2026-10-19)), and a one-program relink stayed as fast as with the full pool (0.7-0.9 s against GNU ld's 1.1-1.5 s for
  `crd-ceir-tests`). `CRD_MOLD=off` prints the reverse entries (the toolchain's linker again), `CRD_MOLD=<path>`
  names another binary and `CRD_MOLD_THREADS` another count. Converting a tree relinks every program once and
  recompiles nothing.
- Not kept, by measurement: `-gsplit-dwarf` on linux-gcc-debug. GCC refuses a precompiled header under it (the
  `.gch` records the `.dwo` name of the unit it was built for: `-Winvalid-pch`, an error under `-Werror`), so it
  needs `CRD_ENABLE_PCH=OFF`; against that PCH-off tree the warm whole-tree relink was 1.2 s shorter and the teeth
  cycles equal, while the PCH-on tree it would replace is as fast warm and cheaper on a cache miss. gdb resolved
  breakpoints, frames, locals and sources from the `.dwo` files, and the gdb diag CTests passed on it.
- Not kept either: mold on the GCC LTO presets (the plugin's link-time compile is the link: four linux-gcc-shipping
  programs relinked in 7.2-7.5 s with bounded mold against 7.3-8.0 s with GNU ld; `CRD_LINKER` still works there
  when asked, and the link-time warning set fires under it), and on win-debug anything but the default
  `/INCREMENTAL`: `/INCREMENTAL:NO` doubles the link time and `/DEBUG:FASTLINK` ties the default on edits while
  doubling a full relink.

<a id="compile-time-profiles"></a>
## Compile-time profiles

[build-profile.py](../../scripts/build-profile.py) answers which units, headers, classes, templates and functions a
build spends its compile time on (2026-10-18, DIAG.11b build-speed task C). `run --compiler msvc|clang --preset <p>
--build-dir <fresh dir> --jobs <n> --label <l> --output <profile.json>` configures the preset into a fresh scratch tree
with no compiler launcher (a cache hit never runs the compiler, so it would hide every unit it serves) and the
compiler's own profiling switches, builds it (`--target` scopes it, the way a module adopting the route profiles only
its targets), and writes a profile document beside the kept build log:

- MSVC: `/Bt+` (front-end and back-end seconds per unit) and `/d1reportTime` (per unit, the include tree with every
  header's inclusive parse time, and the time of every class and function definition), seeded through
  `CMAKE_CXX_FLAGS_INIT` so the project's own flags stay as they are. Both are compiler switches and need no
  elevation. C++ Build Insights (`vcperf`, present in the VS 18 install) needs an elevated shell for every capture, or
  a one-time elevated `vcperf /grantusercontrol` after which `vcperf /start /noadmin` works unelevated (measured
  2026-10-18: "This operation requires administrator privileges", and `/noadmin` refused without the grant).
- Clang: `-ftime-trace` (a JSON per object), read by the pinned ClangBuildAnalyzer 1.6.0 (registry entry
  `tools/clang-build-analyzer`, built from its commit-addressed source by
  [install-clang-build-analyzer.py](../../scripts/install-clang-build-analyzer.py) into `~/.cerid/tools`; no
  administrator rights).

The document keeps, per unit, the front-end and back-end seconds; per header the summed inclusive time (its own text
plus everything it includes, over every unit that parses it) and the summed exclusive time (its own text only, so the
exclusive times add up to the build's include time); per class, function or template the summed time and the units.
Inclusive time ranks what to cut from a unit's include list; exclusive time ranks the text that is expensive wherever
it is included. Like a board, a profile records the host before and after and refuses a contended host unless
`--allow-contended`. `parse-msvc <log>` and `parse-clang <report>` rebuild a document from a kept log or report,
`render` writes the tables docs/bench stores, and `compare <before> <after> --key headers` lines two documents up.
`--vcperf <session>` adds a C++ Build Insights capture around the build (an elevated shell, or `--vcperf-noadmin`
after the one-time grant): `vcperf /stopnoanalyze`, then `/analyze /templates` into a WPA trace and a Chrome
trace-event JSON beside the profile. [elevated-captures.bat](../../scripts/elevated-captures.bat) is the user's one
elevated step that runs it (with DIAG.11b's overhead sampling).

What the 2026-10-18 profiles changed ([board](../bench/2026-10-18-build-speed-compile-time.md)): `<chrono>`, which
C++20 extends with the calendar, the time-zone database and `<format>`, was the build's most expensive header, and
almost every test unit reached it through Catch2's assertion path; two registered patches
([pinned inputs](pinned-inputs.md#patches-on-a-build-owned-copy)) keep it off that path. Hot project headers lost the
standard headers they did not use: `crd/containers/array.hpp` its `<iterator>` (in libstdc++ it brings the stream
buffers and the locale classes), `crd/containers/hash.hpp` and `hash_map.hpp` their `<functional>` (the containers'
default key equality is now `DefaultEqual`, a transparent `a == b`), `crd/jobs/jobs.hpp` its `<algorithm>`. The
scalar exp and log cores moved from `crd/math/simd/transcendental.hpp` into `crd/math/exp_log_core.hpp`, so
`crd/math/cmath.hpp` no longer brings the SIMD backend and `<immintrin.h>` into every unit; the moved functions
compile to identical code (per-function disassembly of a probe at `-O2 -mavx2` and `/O2 /arch:AVX2` on GCC, Clang
and MSVC). The default precompiled header stays as it is: on MSVC its four headers save nothing (the PCH-on and
PCH-off profiles' front ends were within the host's noise), and what every unit paid was what it included.

Catch2's own library (106 units, the only third-party compile above 10 s) compiles as unity batches of 16 sources
(`CRD_CATCH2_UNITY`, on by default; 2026-10-19): its units each spent most of their time parsing the same headers,
and seven batches take 9-11 s of summed compile against 69-113 s for the separate units, with the same defined
global symbols (GCC) and the same suites' case and assertion counts. Catch2 ships an amalgamated single-file build of
these sources, so they are written to compile together; the engine's units stay one per source. Catch2's own units
reach the patched copies first as well (a plain `-I` ahead of its source directory: a directory a target lists as
system for itself is searched after its plain ones), so the program has one definition of every Catch2 entity; an
include-budget entry holds that.

<a id="include-budget"></a>
## Include budget

The cuts above are easy to undo: one include in a header a thousand units reach, and nothing fails.
[check-include-budget.py](../../scripts/check-include-budget.py) makes that a failure. Its registry,
[include-budget.json](../../scripts/include-budget.json), names each hot header, a test unit whose compile command
it borrows from the configuration's compile database (a list of units is tried in order, for a unit that has
another name in another configuration), and the standard headers it must not reach (`forbid` on every
compiler, `forbid_gnu` on GCC and Clang only, because MSVC's C runtime brings `<immintrin.h>` through `<wchar.h>` into
nearly every unit). For each entry a one-line probe is compiled with that command, its precompiled header removed (in
every spelling: MSVC's `/Yu /Fp /FI`, GCC's `-include`, Clang's `-Xclang -include-pch`; a probe that loads the
header lists none of the headers inside it) and its outputs in a scratch directory, with the compiler's own
include listing (`/showIncludes /Zs`, read with the localized prefix CMake recorded in `rules.ninja`, ahead of the
`--` that ends a clang-cl command; `-H -fsyntax-only`); a forbidden header anywhere in the listing fails, and the report names the chain of includes that reached it. An entry that cannot be checked (its unit missing,
the probe not compiling) fails too. It is the `crd-check-include-budget` CTest of every Ninja configuration that
builds the containers, jobs and math tests, so the `ctest -R "^crd-no-|^crd-check"` guards run it; on Windows it takes
the MSVC environment from `scripts/msvc-env.bat` when the shell has none. Adopting it for a new hot header is one
entry: the include, a test unit of its module, the forbidden headers and the measured reason.

## WSL source mirror

The Linux lanes on the WSL reference host build from `~/cerid-src`, a copy of the checkout on the native ext4 disk,
instead of `/mnt/d` (drvfs, where every file open costs milliseconds: 5,760 `lstat` calls take about 20 s and reading
every tracked file about 70 s). [source-mirror.py](../../scripts/source-mirror.py) keeps it exact and
[wsl-lane.sh](../../scripts/wsl-lane.sh) is the only route:

- `sync` copies the `.git` directory, then every path that `git status` reports as different on either side (the
  checkout's status comes from the Windows git through WSL interop, about 0.2 s; the mirror's from its own git, which
  compares content because the copied index carries the checkout's stat data), deletes mirror files git does not see
  (ignored ones included), and puts the checkout's index back. The first sync, and `--full`, compare and copy every
  file instead. Times are preserved, so Ninja sees what it would see on `/mnt/d` (a backup copied back keeps its old
  time there too, so it is touched before the rebuild).
- `check` refuses (exit 3) unless both sides have the same HEAD, the same `git status` entries and identical bytes in
  every reported path; `--full` compares every file's bytes.
- `wsl-lane.sh build|configure` sync, check, then build `~/cerid-mirror-build/<preset>` (configured from the mirror
  through the compiler-cache rules above); `test` and `run` only check, so a source edited after the build is refused
  rather than tested against binaries built from older sources. Configure, build, test and run run under
  `flock -o` on `~/.cerid/wsl-lane.lock` (one heavy WSL job at a time; the descriptor is not inherited, because the
  sccache server outlives a build and an inherited lock kept the next lane waiting until the server's idle
  timeout) and bound every heavy command (`CRD_LANE_TIMEOUT`, 3,500 s).

The trees configured from `/mnt/d` (`~/cerid-build/<preset>`) stay as they are; the board and the equivalence proof
(identical compile commands up to the source and tree paths, identical test inventories, the suites passing) are in
the [board](../bench/2026-10-18-build-speed-cache-mirror.md).

## Waiting for long commands

[wait-for.py](../../scripts/wait-for.py) replaces fixed sleeps with bounded, event-driven waits: `file` (a file
appears, or a log matches a pattern, or a failure pattern ends the wait), `exit-file` (an exit-code sentinel, whose
code it returns, with the log's tail), `pid` (processes exit) and `run` (a command killed with its process tree at the
deadline). `start` returns at once and leaves a detached supervisor running the command with a deadline of at most an
hour, its output in a log and its exit code (124 at the deadline) in a sentinel; a WSL command started from Windows
this way keeps its `wsl.exe` client attached, so the distribution is not stopped as idle under it. The supervisor
copies the command's output into the log through a pipe: handed the log file itself, `wsl.exe` wrote the Linux
standard output and error at two separate positions of the shared handle and the later stream overwrote the start
of the earlier one. A wait is refused
above 1,140 s: the local loop's watchdog reaps a shell command older than 20 minutes, so a longer wait is several
calls while the command keeps running.

## The board runner

`scripts/build-bench.py run` configures one preset into a fresh scratch directory (it refuses an existing one),
builds with an explicit `--jobs`, and records per row: wall time, the Ninja log of that row (edges, span, edge sum,
effective parallelism, slowest edges), memory pressure sampled once a second (minimum available, peak committed,
their difference from the row's baseline), the build-tree size and, with `--sccache`, the statistics zeroed before
and read after the row (hits, misses, non-cacheable compiles and reasons). Rows: `configure`, `cold`, `noop`,
`source` (append a comment to `engine/foundation/log/src/logger.cpp`), `header` (`crd/log/log.hpp`, outside the
PCH), `pch-header` (`crd/core/types.hpp`, inside the PCH), `link` (delete `crd-ceir-tests` and relink), and since
2026-10-18 `relink-all` (delete every output of a CMake executable, shared or module library link rule, or the
`--targets` programs, and rebuild: the link phase alone; its wall time is bounded by the slowest edge, which on the
sanitizer lanes is the demo assets' cook that a relinked cooker reruns, so linkers are compared by the row's edge sum
and on the focused route). Edited files
are restored byte-for-byte with their original timestamps, so neither the working tree nor another build directory
sees the edit. Since 2026-10-18 the teeth rows (`--tooth FILE OLD NEW`) replace one text, rebuild, write the
original bytes back with a new time (as a restored tooth is) and rebuild, then put the original times back;
`--targets` scopes every build row to named targets; each finished row is written at once, a termination request
unwinds through the restore of an applied tooth, and a row during which Ninja rewrote its log is marked instead of
reporting the lines after the old length. `render` merges board documents into the markdown table that `docs/bench/`
stores. The runner is a measurement instrument, not a qualification sweep: one preset, a scratch tree, an explicit
job count, no tests.

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
  0.71 GiB (0.99 GiB with edited variants), which set the 2 GiB per-lane `SCCACHE_CACHE_SIZE` of the hosted
  `actions/cache` design that the GitHub Actions backend replaced on 2026-10-18 (see Hosted lanes).
- **Local defaults (superseded 2026-10-18).** The presets and the default graph stay uncached, but the local helpers
  now configure the qualified trees through the cache (see Local cached trees and the
  [2026-10-18 board](../bench/2026-10-18-build-speed-cache-mirror.md)).

- **Caching qualifies on `linux-gcc-debug` with the precompiled header on.** On the WSL2 board (native ext4,
  GCC 13.3, 16 jobs) 1,773 compiles went through the launcher with no non-cacheable reason; a miss costs +12 %
  (160.5 s against 142.8 s cold); an edit that leaves the preprocessed unit unchanged hits (the PCH-header comment
  edit rebuilt in 21.3 s instead of 139.6 s); the same-directory warm board rebuilt the tree in 20.3 s (7.0×,
  1,773 of 1,773 hits, 2.0 GiB of pressure instead of 14.0 GiB).
  The build-directory dependence holds on Linux too (`-include <build>/…/cmake_pch.hxx`, `-ffile-prefix-map`).
- **Memory is the bound on GCC, not on MSVC.** 14.0 GiB peak pressure at 16 jobs (about 0.88 GiB per compile)
  against 5.8 GiB on MSVC; `CRD_COMPILE_JOBS = (available GiB - 4) / 0.9` is the derived bound for a Linux host
  (12 on a 16 GiB workstation at 16 threads); the hosted 4-vCPU runner needs none.
- **2026-10-18 ([board](../bench/2026-10-18-build-speed-cache-mirror.md)): the cache on every lane a batch builds,
  the WSL sources on ext4.** The mirror alone made linux-gcc-debug 6.3× faster cold and 6.1-6.3× on a header teeth
  cycle; a cached teeth restore is 3.6-4.2× faster on win-debug and win-asan and 6.6-15.5× on the WSL lanes; a warm
  rebuild of a whole tree 3.1-4.2× (MSVC) and 18-19× (GCC on the mirror). MSVC LTCG trees stay uncached (their link
  generates the code: -17 % on a focused restore against a +42 % miss), and Clang trees under the launcher drop the
  precompiled headers' input times (`-fno-pch-timestamp`), without which a fresh tree missed for every unit that
  includes one.

## Hosted lanes

Every lane's evidence bundle carries the lane's Ninja log summary (a hosted checkout is a fresh directory, so the log
is the cold board of the exact revision) and, for a cached lane, the sccache statistics of the build step with the
cache backend they came from, so the first cached push records the miss board and the second the hit board without
any extra job.

Since 2026-10-18 the cached lanes build through sccache's GitHub Actions cache backend. This supersedes the earlier
decision to persist a local-disk cache through `actions/cache`, which was taken because the backend needs
`actions/github-script` and that action was outside the pinned registry; it is pinned now (`actions/github-script`
v8, `cmake/pins.json`). The `actions/cache` design saved a full snapshot per lane per green run (2 GB each) and kept
the stale ones until eviction: on 2026-10-18 five of them held 9.5 of the repository's 11.96 GB against its 10 GB
allowance with only two lanes cached. The backend stores one entry per object instead, writes only misses, and keeps
what a run reads fresh, so the stale objects are what the allowance evicts first.

A cached lane installs the pinned, hash-verified sccache (`install-sccache.py`), then the `Compiler cache service` step
(a pinned `actions/github-script`, because the cache service's URL and token reach JavaScript actions only) starts the
sccache server with them, the token masked, and exports the same settings for the later steps:
`SCCACHE_GHA_VERSION` is the lane's namespace, `<preset>-<runner image>-<image version>`, so lanes never share an
entry and a new runner image (a new toolchain) starts a new namespace; `SCCACHE_IDLE_TIMEOUT=0` keeps the server
until the statistics step stops it; `SCCACHE_IGNORE_SERVER_IO_ERROR=1` makes a lost server reply a local compile. A
failed cache request is a miss, never a failed compile; when the runner offers no cache service or the server does
not start on it, the step says so and the lane builds through an empty local cache. The lane then configures with
`-DCRD_COMPILER_LAUNCHER` (and `-DCRD_ENABLE_PCH=OFF` on cl and clang-cl), builds, prints `sccache --show-stats` in
the job log and passes the JSON statistics to the evidence bundle. Every gate after the build is unchanged.

The cached lanes are the presets in the workflow's `CRD_CACHED_PRESETS`. The list is sized to the allowance from
the [board](../bench/2026-10-18-build-speed-cache-mirror.md#hosted-cache-budget): the per-lane cold footprints
measured locally add up to about 6.5 GiB (7.0 GB) for the cached lanes, beside about 1.6 GB of CPM, Vulkan SDK and
GenMC caches. Uncached, each for a stated reason:

- `win-tidy` (clang-tidy analyses every unit, so a cache would not shorten the job) and `win-vs` (CMake applies no
  launcher to a Visual Studio generator);
- `win-debug-scalar` and `linux-gcc-debug-scalar`, the cold-acquisition presets: they also stay the uncached
  reference build of each complete run, so every complete run still measures a cold build of the exact revision;
- the MSVC LTCG presets `win-release`, `win-relwithdebinfo`, `win-shipping` and `win-shipping-profile`: their
  compiles only parse (the link generates the code; locally a cached restore saved 17 % on a focused route), and
  their intermediate-language objects would take about 3.2 GiB of the allowance.

The hosted proof arrives with the first pushes: the first run of each cached lane shows misses with no cache errors,
the second hits, and `gh api repos/<owner>/<repo>/actions/cache/usage` stays under the allowance. Extending the
list needs a footprint row in the board and room in the allowance.

## Not in scope

Unity builds of the engine's own units (Catch2's library is the one third-party unity target), C++20 modules,
distributed compilation and a build-system replacement stay measured options for a later profile; public-header
checks and downstream consumers are REPO.DEV.8.
