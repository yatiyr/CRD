# DIAG.11b: faster local builds, task A (compiler cache, WSL source mirror, event-driven waits), 2026-10-18

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b) (its contract covers focused local routes and CI
> cost). Contract: [build performance](../design/build-performance.md). Board:
> [2026-10-18 build speed](../bench/2026-10-18-build-speed-cache-mirror.md). Previous batch:
> [allocation refusals in the rest of the engine](2026-10-18-diag-11b-engine-allocation-refusals.md).

## Goal

An extra batch the user asked for on 2026-10-18: make the builds faster without regressing anything and without
disturbing the DIAG work. Task A: the pinned sccache on the local Windows and WSL builds and on the hosted build jobs
(the GitHub Actions cache backend), a WSL source mirror on the native ext4 disk with a check that refuses stale Linux
evidence, and a bounded wait helper in place of fixed sleeps. Measured before and after on the same lanes, one heavy
job at a time. Pushed to CI under the user's process of 2026-10-18 (changed-only local checks; the hosted lanes run
the suites).

## What changed

- **The launcher switch covers Shipping.** [CrdBuildPerformance.cmake](../../cmake/CrdBuildPerformance.cmake) sets
  `CRD_EMBEDDED_DEBUG_INFO` under the launcher on MSVC-compatible compilers; the root build file gives Shipping `/Z7`
  in place of its explicit `/Zi` from it, and [CrdPackage.cmake](../../cmake/CrdPackage.cmake) packages the archives'
  debug information as `embedded` from it (the consumer test's rule follows). Without the launcher nothing changes.
  On Clang (not clang-cl) the launcher adds `-Xclang -fno-pch-timestamp` (below).
- **Local cached trees.** [compiler-cache.py](../../scripts/compiler-cache.py) decides which presets the helpers cache
  (Ninja, not clang-tidy, not MSVC LTCG; cl and clang-cl without precompiled headers) and runs the server on the local
  cache; [configure-preset.bat](../../scripts/configure-preset.bat) and [build-target.bat](../../scripts/build-target.bat)
  use it. The pinned binaries are installed at `%USERPROFILE%\.cerid\tools\sccache` and `~/.cerid/tools/sccache`.
- **WSL source mirror.** [source-mirror.py](../../scripts/source-mirror.py) keeps `~/cerid-src` equal to the checkout
  (git-visible files, bytes and HEAD; the checkout's status through the Windows git, about 0.2 s) and refuses a stale
  mirror (exit 3); [wsl-lane.sh](../../scripts/wsl-lane.sh) is the route (sync, check, configure, build, test with the
  test preset's settings, run in a scratch directory, one heavy job at a time under `flock -o`) into
  `~/cerid-mirror-build/<preset>`; [wsl-build.ps1](../../scripts/wsl-build.ps1) now runs it.
- **Waits.** [wait-for.py](../../scripts/wait-for.py): `start` (a detached supervisor with a deadline of at most an
  hour, a log and an exit-code sentinel), `exit-file`, `file`, `pid` and `run`, every wait under 1,140 s.
- **The board runner** ([build-bench.py](../../scripts/build-bench.py)): teeth rows, `--targets`, a row checkpoint, a
  restore of an applied tooth on termination, and the detection of a Ninja log rewritten during a row.
- **Hosted lanes** ([ci.yml](../../.github/workflows/ci.yml)): the `actions/cache` snapshots of the two cached lanes
  are replaced by sccache's GitHub Actions backend on 14 lanes (`CRD_CACHED_PRESETS`), through a pinned
  `actions/github-script` v8 step (registry entry, license manifest) that starts the server on the cache service in
  the lane's namespace; every cached job prints `sccache --show-stats` and hands the JSON to the evidence bundle,
  which now names the cache backend ([ci-evidence.py](../../scripts/ci-evidence.py)).
  [check-ci-tiers.py](../../scripts/check-ci-tiers.py) holds the list to the local cache rule, keeps the
  cold-acquisition presets out and requires every cache step in each cached lane's job.
- Tests: `CompilerCache`, `SourceMirror`, `WaitHelper`, the teeth and checkpoint rows of `BuildPerformance` and the
  cached-lane rule of `CiTiers` in [test-repository-tools.py](../../scripts/test-repository-tools.py), each with teeth
  (mutations of the helpers fail them: listed below).

## Decisions

- **The hosted backend is the GitHub Actions cache, as asked.** The earlier design kept the local disk inside
  `actions/cache` because the backend needs `actions/github-script`; that action is now pinned like the others. The
  token reaches the server through one masked step, and every way the service can fail leaves the lane building
  (a request error is a miss, a missing service or a server that does not start falls back to an empty local cache).
- **MSVC LTCG stays uncached, locally and hosted.** Measured on a focused win-shipping route the restore saved 17 %
  for a +42 % miss; the link is the cost and is never cached, converting a tree rebuilds every LTCG program, and the
  four hosted LTCG lanes' intermediate-language objects (about 3.2 GiB) do not fit the 10 GB allowance beside the
  14 cached lanes (about 6.5 GiB) and the CPM, Vulkan SDK and GenMC caches (about 1.7 GB). An explicit
  `-DCRD_COMPILER_LAUNCHER` still caches such a tree (proved with cdb below).
- **The cold-acquisition presets stay uncached** (win-debug-scalar, linux-gcc-debug-scalar): they remain the cold
  reference build of every complete run. clang-tidy and the Visual Studio generator cannot use a launcher's gain.
- **Tree paths stay.** A cached compile's key includes the build directory, so the converted trees are the same
  `build/<preset>` directories; the cached configuration was proved on scratch boards of the same presets before
  the real trees were converted. On WSL the new trees are new directories, and the `/mnt/d` trees are kept.
- **The default `build-target.bat` job count (four) is unchanged**: a separate measured change, and this host's
  faults are load-correlated (five compiler crashes at 16 jobs today, below).

## Measurements

All in the [board](../bench/2026-10-18-build-speed-cache-mirror.md). The loop's own route (one `construct.hpp` tooth
rebuilt on all seven lanes a batch builds, through the helpers): 549.1 s before, 108.6 s after the conversion on its
first run, 79.3 s when repeated. The WSL mirror alone: linux-gcc-debug cold 1,131.9 → 178.7 s; with the cache the
teeth restore 1,033.6 → 25.2 s. win-debug teeth restore 141.1 → 37.0 s, warm rebuild 59.2 s against 185.0 s cold.

## Faults found by the measurements, repaired

- **Clang precompiled headers missed in a fresh tree.** The first warm linux-clang-tsan board missed 249 of 2,059
  compiles: Clang writes each input's modification time into a `.pch` and sccache hashes the `.pch`; a new tree's
  configure rewrote the generated headers, and a hosted checkout gives every header a new time, so every unit
  including a precompiled header would miss on every hosted run. Probed: the `.pch` bytes differ after `touch` and
  are identical with `-Xclang -fno-pch-timestamp`, the objects are byte-identical with and without the flag, and a
  changed header still misses. With the flag the warm board hit 2,059 of 2,059.
- **A daemon held the WSL lane lock.** The sccache server outlives a build and inherited the lock descriptor of the
  first `wsl-lane.sh` version, so the next lane waited for the server's idle timeout; the helper now runs itself under
  `flock -o` (tested: two runs serialize, a command that leaves a child behind leaves the lock free).
- **A supervised WSL log lost its start.** Handed the log file, `wsl.exe` wrote the Linux standard output and error
  at two positions of the shared handle; the supervisor now copies a pipe into the log (test: 400 interleaved lines
  arrive complete and in order).
- **`%LOCALAPPDATA%` is the package's private copy** for this agent's processes, invisible to other processes and
  WSL; tools and caches live under `%USERPROFILE%\.cerid` and `build/`.
- A first board left `CMakeLists.txt` rewritten byte for byte during a run (a line-ending pass), voiding one no-op
  row; the edited Python files had been written with CRLF by Windows Python and were restored to LF.

## Proofs

- **Suites on the converted trees** (CTest as the hosted lanes run it: headless, soft performance budgets, JUnit),
  run before the user's process change made the hosted lanes the suite owners:
  - win-debug (cached, PCH off), in two ranges of 3,914 tests: the first passed except `crd-master-plan`, which
    failed on an anchor of this batch's own board that was not written yet (`#hosted-cache-budget`, since written);
    the second passed with no failure.
  - win-asan (cached, PCH off): the first range passed (3,914 tests); of the second, about 3,340 tests ran and none
    failed before the one-hour supervisor deadline. The hosted win-asan lane, now cached, runs the whole suite.
  - linux-gcc-debug on the mirror tree (7,485 tests): two failed. `crd-master-plan`, the same anchor, and
    `DIAG.7a(f): Vulkan reports per-mode validation activation` (`GpuAssisted` inactive, with VUID errors for the
    engine's `pNext` chains): the lane helper exported no `VK_LAYER_PATH`, so the loader used the system validation
    layer, older than the engine's headers. `wsl-lane.sh` now exports the pinned layer (`~/.cerid/vulkan-validation`,
    a link to the pinned install `~/cerid-build/validation-repair-20260912`, or `CRD_VULKAN_VALIDATION`); after it the
    case passes on the mirror tree, with `DIAG.7c(b)` and the two `DIAG.7a(f)` unit cases.
- **The mirror trees equal the old trees** in compile commands and test inventory (board: "The mirror trees against
  the trees from `/mnt/d`"); the mirror's bytes equal the checkout (`source-mirror.py check --full`).
- **Debugging from the cache**: cdb resolves the same frames, source lines and typed locals in a win-debug program
  built entirely from cache hits as in the default tree, and in a cached win-shipping program (board).

## Teeth

Each helper mutation below was applied alone, its test class run, the bytes restored and their digest checked
(`build/snap-1018/teeth.py`, scratch); every one is caught:

| Mutation | Caught by |
|---|---|
| `compiler-cache.py` keeps the cl/clang-cl precompiled headers under the launcher | `CompilerCache.test_configure_args_follow_generator_compiler_and_tidy` |
| `source-mirror.py` compares sizes only, not bytes | `SourceMirror.test_sync_copies_exactly_the_visible_files_and_check_refuses_every_kind_of_staleness` |
| `source-mirror.py` ignores a different HEAD | the same |
| `wait-for.py` accepts a wait past the watchdog limit | `WaitHelper.test_timeouts_are_bounded_under_the_watchdog_and_polls_are_short` |
| `build-bench.py`'s teeth row never restores the tooth | `BuildPerformance.test_teeth_rows_break_restore_and_settle_and_targets_scope_every_build` |
| `check-ci-tiers.py` lets a cached lane skip the statistics hand-off | `CiTiers.test_cached_lanes_follow_the_cache_rule_and_carry_every_cache_step` |
| `check-ci-tiers.py` lets a cold-acquisition preset be cached | the same |
| `ci-evidence.py` stops naming the cache backend | `BuildPerformance.test_ninja_log_and_sccache_summaries_feed_the_evidence_bundle` |

Two tests were strengthened by this pass: the statistics hand-off mutation survived at first (no case removed
`--sccache-stats` from a cached job; one does now), and the bounded-wait case met a lost bound by waiting out the
1,141 s instead of failing (the out-of-range calls now run with the waiter replaced by one that fails at once). The
staleness check's teeth on the real mirror are in the board (an edit, a new untracked file and a mirror-only file each
make `check`, `test` and `run` exit 3).

## Verification

Changed-only (the user's process of 2026-10-18): `test-repository-tools.py` (90 tests, 3 skipped for missing tools;
the CMake module case, skipped without CMake on the path, passes inside `msvc-env.bat`), `test-dev-workflow.py`,
`test-diag-coverage.py`, `check-ci-tiers.py` and `check-pins.py` PASS, the pinned actionlint 1.7.12 (WSL,
`-shellcheck= -pyflakes=`) clean on every workflow, `check-master-plan.py`, `check-repository.py` and the ctest
repository guards on win-debug; the `eol=lf` files are LF in the index and the working tree. The workflow's MSVC LTCG
job carries the cache steps for a later listing; no preset of it is cached, as its comment now says.

## Hosted evidence this batch needs

Once pushed: the first run of each of the 14 cached lanes prints `sccache serves <preset>-<image>-<version> from the
cache service (v2: …)` and statistics with misses and no cache errors, the second run hits (the evidence bundle's
build line names the GHA backend), `gh api repos/yatiyr/CRD/actions/cache/usage` stays under 10 GB after both, and
every lane's gates pass unchanged (`check-ci-tiers.py` holds the list; actionlint 1.7.12 passed locally).

## Follow-up for an extra batch

- Read the first two cached hosted runs against the clauses above, replace the scaled rows of the board's hosted
  cache budget with the measured footprints, and repair any lane whose statistics show cache errors.
- Task B of the user's build-speed request (approved 2026-10-18): pin and measure the latest mold release on the Linux
  links and ClangBuildAnalyzer on a clang `-ftime-trace` build, both opt-in through the helpers like the cache.
