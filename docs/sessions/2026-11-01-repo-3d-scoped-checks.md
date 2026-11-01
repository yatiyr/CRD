# REPO.3d: the scoped configuration and consumer checks, TP-5's record and the installer's Qt line

<!-- doc-role: historical -->
> Dated evidence. Live owners: [REPO.3d](../ROADMAP.md#slice-repo.3d) and [REPO.3](../ROADMAP.md#slice-repo.3);
> register: [TP-5](../third-party-defects.md#tp-5). Rules: [AGENTS](../../AGENTS.md).

Track B, 2026-11-01, on `cerid-track-b` (c4-standard-8, GCC 13.3, Clang 18.1.3) at 408515f1 plus this batch's edits.

## CI first

- **No run covers b7794126 or f8b2b7d8 yet.** GitHub's `main` was still 43963cb2 at 10:06 UTC (the mirror lags the
  GitLab push); the push run at 43963cb2 (41295628482) was pending and the `workflow_dispatch` run 41295647725 and
  the scheduled 41295924613 at 43963cb2 were queued behind 41269105413 (03:25 UTC) and 41286750175 (07:56 UTC).
  The push runs at a883e502, 30a34cc3, 71423138, a936e7b7 and 4b5f7c17 were cancelled with no job started.
- **The dispatch run 41274380605 (de5e10b0)** had two jobs still running, linux-gcc-debug (124764040384) and
  win-relwithdebinfo (124764040641); every failure of its finished jobs was read in the
  [previous batch](2026-11-01-repo-3-checkout-paths.md#ci-first), none this track's. Both linux-gcc-asan shards and
  linux-gcc-public-checks (124764040408) passed; win-public-checks (124764040379) failed only the file-time stamp test
  track A's cc5ee9e9 fixed.

## Scoped configuration checks

Every x64 Linux configure preset configured into a fresh tree (`cmake --preset <p> -B ~/cfg/<p>` with the pinned
Ninja): `linux-gcc-debug`, `-release`, `-relwithdebinfo`, `-asan`, `-shipping`, `-debug-scalar`, `-public-checks`,
`linux-clang-fuzz`, `linux-clang-coverage` and `linux-clang-tsan` each exit 0 with 110 of 110 modules and no
`CMake Warning`, `CMake Deprecation` or `CMake Error` line. The ARM64 presets need the ARM64 runners (hosted
`linux-gcc-arm64-*`/`linux-clang-arm64-*`, all green in 41274380605); the Windows presets are the Windows lanes'.

## Consumer checks

`linux-gcc-public-checks` (`CRD_PUBLIC_CHECKS=ON`) on the track machine:

- **Public headers.** The configure reports 1,256 headers of 98 modules compiled standalone through their modules'
  consumer view, none excluded (the modules with no library target to view through: the DX12, CUDA, HIP, Metal and
  WebGPU backends). `crd-header-checks` builds all 1,274 steps with no warning (1 min 50 s).
- **The package.** `crd-package-consumer` passes (30 s): the installed tree is complete and relocatable, the engine's
  build tree is deleted, the downstream consumer and the diagnostic application build against the moved prefix with
  the engine's DWARF reaching their symbols, the deliberate failure leaves a bundle that `bundle.inspect` decodes at
  the full (40 of 40 messages) and basic (5 of 40) details, the allocator, jobs, reload and capture examples and the
  codecs route show what they must, and a Release consumer and `BUILD_SHARED_LIBS=ON` are refused.

The Windows side of the same checks is hosted: win-public-checks in 41274380605 passed everything but the file-time
test above. What REPO.3d still needs is the published revision's: a complete-tier hosted run green on every lane
after [DIAG](../ROADMAP.md#slice-diag) closes (track A), at or after cc5ee9e9, 4b9bc837, 7cfed26c and 76330344, and
with win-release's open `diagnostic host: the application's wild write ...` failure (`code image: the module is not
a Windows image`, HW-RESILIENCE.2's crash capture, track A) repaired.

## TP-5's record

[TP-5](../third-party-defects.md#tp-5) is REPO.3d's register entry. Read against the pin and upstream:

- **The commit.** The entry named the pin `633eb1ff`; `cmake/pins.json` pins `3e895b49` (upstream's last commit,
  2020-03-25). `633eb1ff31a4...` is the CPM source-cache directory in the lanes' report paths
  (`.cpm-cache/mikktspace/633eb1ff.../mikktspace.c`), not a commit. Corrected.
- **The licence.** The entry and the root `CMakeLists.txt` said MIT; the file's header is the zlib licence, as
  `pins.json` and the generated licence manifest say. Corrected in both.
- **Both sites.** `QuickSort` (`mikktspace.c:1460`) has the same `uSeed >> (32 - t)` as `QuickSortEdges` (`:1667`);
  the oracle reports the second because UBSan does not recover. The retirement condition now names both.
- **Upstream.** No commit after the pin; pull request 3 (2021-09-03, open) guards only `QuickSortEdges`' shift;
  issue 7 (2024-07-26, open) reports both. No repin is possible.
- **Lanes.** Still current: 41274380605's linux-gcc-asan shard 2 (124764040403) fails the registered test with the
  same report and the gate passes (`registered TP-5: failed`). The ARM64 ASan lanes enable UBSan too but configure
  37 of 110 modules under `CRD_MODULES_WITHOUT=jobs`, asset-io omitted; the entry says they join it by a documented
  decision when JOBS-PORT.2 brings asset-io to them.

## The LunarG installer's Qt line

REPO.3 recorded that no lane ran the installer because the Windows lanes restore the SDK from cache. That was wrong:
`win-debug-scalar` is a cold lane (`CRD_COLD_PRESETS`), skips the SDK cache and runs `install-vulkan-sdk.py` on every
run. In 41274380605 (job 124764040380) the pinned 1.4.341.1 installer, started with `--accept-licenses
--default-answer --confirm-command install`, printed through its own logger, between `Loading component scripts...`
and the component selection:

    [1837] Warning: QFont::setPixelSize: Pixel size <= 0 (-1)

So the line is current and the installer's. The same log shows a second finding: before installing the embedded
payload the installer fetched update metadata from LunarG's remote repository (`Fetching latest update
information...`, `Retrieving meta information from remote repository...`, one item into its cache under
`AppData/Local/cache/qt-installer-framework`), a network read outside the pin. The Qt Installer Framework's
documented options have no offline switch; `--set-temp-repository` makes the given repositories the only ones used
in the fetch and `--platform` selects the platform plugin. Which of them silences the warning and removes the
remote fetch without changing the installed tree needs a run of the pinned installer with each, on a hosted Windows
machine (this track builds nothing on Windows): REPO.3's next work.

## Evidence

- Track machine: the ten configures, `crd-header-checks` and `crd-package-consumer` above.
- Local validators: `check-master-plan.py`, `check-repository.py`, `check-allman-braces.py`, `check-generated.py`.
