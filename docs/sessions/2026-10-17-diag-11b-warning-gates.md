# DIAG.11b: holding the warning-free lanes at zero, 2026-10-17

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b). Rules: [AGENTS](../../AGENTS.md) ("keep
> warnings zero"). Previous batch: [link-time warnings](2026-10-17-diag-11b-link-time-warnings.md), which queued
> follow-ups (c), (d) and (e) of this note.

## Goal

The previous batch brought the hosted linux-gcc-shipping lane to zero warnings, but nothing kept it there, and two
Windows lanes still printed warnings of their own. This batch takes the three queued follow-ups: (c) make GCC and
Clang link-time warnings fail the build; (d) remove win-shipping's and win-shipping-profile's `D9025 overriding '/O2'
with '/Od'` without touching the load-bearing `/Od` pin on crd-jobs; (e) look at win-clang-cl-shipping's two zstd
`-Wunused-function` warnings. Committed locally, not pushed.

## (c) `-Werror` on the link line

GCC 13.3.0 (the hosted ubuntu-24.04 compiler) under WSL, with a two-file program whose out-of-bounds store GCC sees
only once link-time optimization inlines one file into the other (`~/logs/ltoprobe`):

| Probe | Result |
| --- | --- |
| compile `-O2 -Wall -flto`, link `-O2 -flto` | `warning: writing 1 byte into a region of size 0 [-Wstringop-overflow=]`, exit 0 |
| the same objects, link adds `-Werror` (gcc or g++ driver) | `error: ... [-Werror=stringop-overflow=]`, `lto1: all warnings being treated as errors`, link fails |
| compile with `-Werror`, link without it | the warning again, exit 0: a compile line's `-Werror` does not reach link-time diagnostics |
| one object compiled with `-Wa,--noexecstack`, link with `-Werror` | `lto-wrapper: warning: Extra option to '-Xassembler' ...`, exit 0: lto-wrapper's notices ignore `-Werror` |

That is why the hosted lane could print 44 diagnostics and pass. The repair (root `CMakeLists.txt`, the non-MSVC
branch of `crd-warnings`): `target_link_options(crd-warnings INTERFACE
$<$<AND:$<LINK_LANGUAGE:CXX,C>,$<BOOL:${CRD_WARNINGS_AS_ERRORS}>>:-Werror>)`. It travels the same
`$<BUILD_INTERFACE:crd-warnings>` edge of crd-core as the compile options, so every engine, test and tool executable
gets it and the installed package does not; the diagnostic presets that turn warnings off (`linux-clang-fuzz`,
`linux-clang-coverage`) keep their link lines unchanged. It sits inside `if(MSVC) ... else()`, so clang-cl, whose
compiler ID is Clang but whose linker is lld-link, is not given it. The link-language gate keeps a bare `-Werror` off a
CUDA link (nvcc reads it with an argument). The lto-wrapper class has no flag; the previous batch's zstd repair is what
keeps it at zero.

After a reconfigure the link lines read `g++ -O3 -DNDEBUG -flto=auto -fno-fat-lto-objects -Werror ...`
(`ninja -t commands`). Full builds under it, on WSL (`~/logs/werror-*.log`), with `-k 0` so that one failing link
could not hide another:

| Configuration | Build | Result |
| --- | --- | --- |
| linux-gcc-release (the only IPO lane with the benchmarks; its tree dated from 2026-10-08) | 2,081 steps, 184 executables linked | exit 0, no `warning:` line, no `lto-wrapper` line |
| linux-gcc-relwithdebinfo (hosted: 45 warnings at 56748127, among them `eval_cpu.constprop` in `crd-technique-cook-tests`) | 2,055 steps, 183 executables linked | exit 0, no `warning:` line, no `lto-wrapper` line |
| linux-gcc-shipping | every executable relinked: 180 of the 189 link lines carry `-Werror` | exit 0, no `warning:` line, no `lto-wrapper` line |
| linux-gcc-debug, linux-gcc-asan, linux-clang-tsan (no link-time optimization: only the driver could warn at the link) | 1,074, 1,089 and 1,089 steps, 181 executables linked on each | exit 0, no `warning:` line; clang++ links as `clang++ -ferror-limit=0 -g -fsanitize=thread -Werror ...` |

The nine link lines without `-Werror` are the `tests/diag` specimens, which link no engine library and carry none of
the engine's warning flags when they compile either (they contain the defects the detectors must catch).

**Teeth.** `set_path` in `engine/ui/perf-ui/src/diag_panel.cpp` given back its pre-afe1742a indexed terminator store (a
known link-time false positive on this lane), then `crd-perf-ui-tests` built on linux-gcc-shipping:
`diag_panel.cpp:326:25: error: writing 1 byte into a region of size 0 [-Werror=stringop-overflow=]`,
`lto1: all warnings being treated as errors`, the link failed (exit 1). The byte-exact backup restored and touched,
the same target rebuilt: linked, and its 14 cases (603 assertions) pass.

## (d) D9025 without moving the pin

`engine/foundation/jobs/CMakeLists.txt` pinned `worker_pool.cpp` and `fiber_init.cpp` to `/Od` with a per-source
`COMPILE_FLAGS "/Od /Y-"`. On a Release-based configuration that option follows `CMAKE_CXX_FLAGS_RELEASE`'s
`/O2 /Ob2`, cl.exe discards the earlier `/O2` and says so: two D9025 lines per archive on win-shipping,
win-shipping-profile and win-release (measured here before the change: a clean rebuild of the two objects printed two
for crd-jobs and two for crd-jobs-schedcheck on each). `/WX` does not make a command-line warning an error.

The per-configuration flags are read per directory, so no target or source property can take `/O2` off one file's
command line. The two TUs now compile in a new directory, `engine/foundation/jobs/unoptimized/`, which removes the
optimization level (`/O1`, `/O2`, `/Ox`) from its own copies of `CMAKE_CXX_FLAGS_<CONFIG>` for every configuration
(single- and multi-configuration) and builds them as two object libraries, `crd-jobs-unoptimized` (gate as crd-jobs)
and `crd-jobs-schedcheck-unoptimized` (gate on, `EXCLUDE_FROM_ALL`), with `/Od`, interprocedural optimization off, and,
under Ninja, the archive's own compile PDB, which the package installs (`cmake/CrdPackage.cmake`). Each archive takes
its objects through `$<TARGET_OBJECTS:...>`, so the export set and the package's one archive per module are unchanged.
`/Ob<n>` is kept on purpose: it still selects inline expansion under `/Od`, and dropping it would change the code the
pin has always produced. On non-MSVC nothing changed.

The pin's code is the same code. The objects were saved before the change and rebuilt after it; `dumpbin /disasm`
(every instruction's bytes, relocated operands by symbol) and the symbol tables are equal once the per-TU string
literal labels (`$SG<n>`, numbered in parse order: the old command line force-included the PCH header, which `/Y-`
did not stop) are normalized:

| Lane | Archive | worker_pool.obj | fiber_init.obj |
| --- | --- | --- | --- |
| win-shipping | crd-jobs, crd-jobs-schedcheck | same code, 372 symbols | same code, 30 symbols |
| win-shipping-profile | crd-jobs | same code, 372 symbols | same code, 30 symbols |
| win-release | crd-jobs, crd-jobs-schedcheck | same code, 311 symbols | same code, 25 symbols |
| win-clang-cl-shipping | crd-jobs, crd-jobs-schedcheck | same code, 734 symbols | same code, 30 symbols |

After the change a clean rebuild prints no D9025 on any of these lanes, and the compile commands of both TUs read
`/Ob2 ... /Od` with no `/O2` and no `/GL` or `-flto`.

**The guard.** `scripts/check-compile-commands.py`, registered as the CTest `crd-check-compile-commands` on every MSVC
configuration whose generator writes `compile_commands.json` (Ninja, Makefiles), reports every cl.exe or clang-cl command
carrying two distinct levels among `/O1`, `/O2`, `/Ox` and `/Od` (either dash form, a launcher in front allowed), and
fails a file with no such command at all. `crd-repository-tools` covers it (`CompileCommands`, four cases). Teeth: with
the directory's strip turned into a no-op, a win-release rebuild printed the two D9025 again and the guard failed on all
four commands (both archives); restored, rebuilt: no D9025, the guard passes over 2,099 commands. With `/Od` dropped
from its level pattern, three of the four unit cases fail. A stale win-relwithdebinfo tree, configured before the
change, fails it on the same two files.

## (e) zstd's two `-Wunused-function` on win-clang-cl-shipping

`HUF_isSorted` (`huf_compress.c`) and `ZSTD_isAligned` (`zstd_lazy.c`) are `MEM_STATIC` helpers called only inside
`assert`. zstd 1.5.5's `mem.h` gives `MEM_STATIC` the `unused` attribute only under `__GNUC__`, which clang-cl does not
define, so there it is a plain `static inline`; with `NDEBUG` the asserts vanish and zstd's own `/clang:-Wall`
(`AddZstdCompilationFlags.cmake`) reports both; with the asserts compiled, as on the Debug clang-cl lane, both are
used. Nothing in
Cerid's code or flags is involved, and the only repair that is not a suppression is a zstd pin bump, which needs a new
archive fetched and registered (with no evidence here that upstream changed this; no other zstd copy exists on this
machine). That, or accepting the two lines as third-party diagnostics, is the user's decision; nothing was changed.

Later the same day (pointer, the text above is unchanged): no zstd release changes the `__GNUC__` gate (v1.5.7 and
the dev branch checked), and two registered patches on build-owned copies repaired both warnings with the objects'
code unchanged; see [zstd's clang-cl and configure warnings](2026-10-17-diag-11b-zstd-configure-and-pause-landing.md).

## Verification

| Lane | Built | Ran |
| --- | --- | --- |
| win-debug | `all` (133 steps left after the targeted builds): no warning | crd-jobs-tests (192 cases, 29,997 assertions) and crd-jobs-schedcheck-tests (6 cases); the `crd-no-*`/`crd-check*` guards, now 9 with `crd-check-compile-commands` (2,099 commands); `test-package-consumer.py --preset win-debug`: PASS |
| win-shipping | crd-jobs, crd-jobs-schedcheck and both test executables: no warning, no D9025 | both suites (184 cases, 29,974 assertions; 6 cases); the guard (2,093 commands); `test-package-consumer.py --preset win-shipping`: PASS (debug information `pdb`, one archive per module, the engine's symbols reach the application; the two TUs' types go to `crd-jobs.pdb`, their object library has no PDB of its own) |
| win-shipping-profile | the same | both suites; the guard |
| win-release | the same | both suites; the guard (2,099 commands); the teeth above |
| win-clang-cl-shipping | the same (thin LTO; every link on the first attempt) | both suites; the guard; the two zstd objects rebuilt to confirm their two warnings |
| win-asan | both test executables | both suites, under vcvars; the guard |
| Visual Studio 18 2026 (`win-vs`, a scratch tree configured from the archives the win-debug tree already held, `CRD_INPUT_ARCHIVES`, through `project-sync.py configure`) | `crd-project-sync-check`, crd-jobs and both test executables in Debug | both suites. The generated projects: `crd-jobs-unoptimized` has `Optimization` Disabled in all eight configurations, inline expansion as each configuration says, no whole-program optimization; crd-jobs keeps MaxSpeed in the Release-based ones. The scratch tree was deleted afterwards and the synchronizer's registration file restored byte for byte |
| linux-gcc-shipping, -release, -relwithdebinfo, -debug, -asan, linux-clang-tsan | the full builds above | shipping: crd-jobs, containers and hesap-dense suites; debug, asan and tsan: containers and perf-ui suites: all pass |

- `test-repository-tools.py` (75 tests, the four new `CompileCommands` cases among them), `test-module-selection.py`,
  `test-project-sync.py`, `test-native-build-profiles.py`, `check-master-plan.py`, `check-repository.py` and
  `check-allman-braces.py` pass. No C++ source changed, so tidy and clang-format had nothing to judge; no workflow
  changed. The root `CMakeLists.txt` edit resolves the eventual push to the complete tier.
- `docs/BUILDING.md` is at its 7,000-byte orientation budget, so the link-line rule is stated in
  [test instruments](../design/test-instruments.md) and the guard in [CONTRIBUTING](../CONTRIBUTING.md) (route 4).

## Hosted evidence this batch needs

Every GCC and Clang lane links with `-Werror` and stays green: linux-gcc-shipping, linux-gcc-release and
linux-gcc-relwithdebinfo print no `warning:` line. `crd-check-compile-commands` runs and passes on every Windows Ninja
lane that runs CTest. win-shipping, win-shipping-profile and win-release print no D9025 and pass crd-jobs' tests (the
pin's objects now come from the object libraries; the 2026-05 failure mode was exactly these suites on these lanes).
The windows-native job generates the two object-library projects and its project-sync check passes.
win-clang-cl-shipping still prints zstd's two `-Wunused-function` until the user decides (e).
