# REPO.3: the Vulkan SDK installer's verdict, the Shewchuk oracle's reads and MCP-ADAPT.0's clang-cl evidence

<!-- doc-role: historical -->
> Dated evidence. Live owners: [REPO.3](../ROADMAP.md#slice-repo.3), [MCP-ADAPT.0](../ROADMAP.md#slice-mcp-adapt.0).
> Contract: [test instruments](../design/test-instruments.md#host-separation-diag10a). Rules: [AGENTS](../../AGENTS.md).

Track B, 2026-11-01, on `cerid-track-b` at 91f2b44e plus this batch's commits. The track machine ran about 0.1 h for
the `linux-gcc-asan` reproduction below.

## CI first

- **MCP-ADAPT.0's clang-cl evidence.** The `workflow_dispatch` run 41295647725 at 43963cb2 is the first whose
  win-clang-cl could run the tests (after track A's 7cfed26c). Its win-clang-cl (Clang 22.1.3 in MSVC mode, job
  124824306727) built in six minutes and passed 9,472 of 9,472 tests (5,222 s), among them the 31 cases named for
  `ceridc`, MCP or the protocol stream: `crd-ceridc-mcp-conformance`, `MCP 2026-07-28: discovery, the per-request _meta
  and the version error`, `MCP 2025-11-25: the initialize handshake selects the legacy revision for the connection`,
  the envelope, `THE AGENT GATE: the full scenario through MCP alone`, `platform: the protocol owns its output stream
  and stray writes go to stderr`, the diag tool's native, verb, MCP and real-binary cases and
  `crd-fuzz-ceridc-diag-corpus`. Since 43963cb2 the row's paths changed only by track A's bed8406b (the
  `hardware.check` diag command, its cases and a fuzz seed); the MCP server and its schemas are untouched. With the
  MSVC, SSE2-floor and headless Linux evidence of run 41274380605, every clause is proven: the row is Done.
- **The Shewchuk oracle reads past an expansion (this track's 02c0e664).** The same run's `linux-gcc-arm64-asan`
  (job 124824306871) failed 3 of 2,629 tests and `win-asan` shard 1 (124824306955) 2 of 4,737: `orient2d`, `orient3d`
  and `incircle agrees with Shewchuk's adaptive and exact ...`, each `stack-buffer-overflow`, a read of 8 bytes in
  `fast_expansion_sum_zeroelim` (`shewchuk-predicates.c:1064`) just past `bterms` in `orient2dexact`'s frame. No
  complete run had covered 02c0e664 before. Shewchuk's four expansion sums read the next component after every step
  (`enow = e[++eindex]`), so after the last one they read one past the end; the value is never used (each use is
  behind `eindex < elen` or `findex < flen`). The 28 reads are now `(++eindex < elen) ? e[eindex] : 0.0` and the `f`
  form, which computes the same results without the read; the reference's README and `CMakeLists.txt` name the
  repair. The engine's own port (`linear_expansion_sum` in `predicates.cpp`) guarded every read but its first one,
  which read `e[1]` or `f[1]` of a one-component expansion (inside the caller's buffer, never used); it is guarded too.
- Track machine, `linux-gcc-asan` (GCC 13.3, x64): with the unpatched file, `orient2d agrees with Shewchuk's adaptive
  and exact orient2d` aborts with the same report (`:1064`, frame `orient2dexact`), so x64 GCC ASan reproduces it;
  with the repair the `[shewchuk]` cases pass (73 assertions in 4 cases) and so does all of
  `crd-geometry-primitives-tests` (118,529 assertions in 211 cases).
- The rest of run 41295647725 was still running; the dispatch run 41298463221 at 13453acb (track C's) is the first
  to cover the checkout-path and text-encoding guards on both repository-checks jobs, and is queued.

## The installer experiment's verdict

The host-check run 41298875777 at 72f9696f ([experiment](2026-11-01-repo-3-vulkan-installer-options.md)) ran seven
of its nine arms by 12:30 UTC; `minimal-updates` (job 124825474678) and `temp-repository-empty` (124825474705) were
still queued behind the CI runs when this batch ended, and its judge job waits for them. Each finished arm installed
385 files (2.26 GB) with exit 0 and no layout file missing. Judged locally from the seven arms' artifacts with the
corrected classifier (below):

| Arm | Options | QFont line | Remote fetch | Metadata cache | Tree |
|---|---|---|---|---|---|
| baseline, baseline-repeat | (production) | 1 | 1 | 5 files, LunarG's `Updates.xml` (3,945 bytes) | same |
| verbose | `--verbose` | 1 | 1 | as baseline | same |
| platform-minimal | `--platform minimal` | 1 | 1 | as baseline | same |
| platform-offscreen | `--platform offscreen` | 1 | 1 | as baseline | same |
| temp-repository-updates | `--set-temp-repository` local, no package | 1 | 0 | 3 files, the local `Updates.xml` (154 bytes) | same |
| offscreen-updates | `--platform offscreen` and the local repository | 1 | 0 | as temp-repository-updates | same |

The two baselines disagree on `InstallationLog.txt`, `maintenancetool.dat` and `maintenancetool.ini` (same sizes,
different digests), which every comparison leaves out. Findings:

- **The remote fetch.** The production run spends about a second in `Retrieving meta information from remote
  repository...` and caches LunarG's update metadata, which also describes a `com.lunarg.vulkan.debug` component the
  run never installs. With `--set-temp-repository` naming a local repository whose `Updates.xml` lists no package, the
  line is gone, the cache holds only that repository's entry, and the installer selects and installs the same two
  components (`com.lunarg.vulkan`, `com.lunarg.vulkan.core`) into the same tree. That option is now production:
  `install-vulkan-sdk.py` writes the repository under its download directory and passes it (`arguments()`), and the
  experiment script imports it from there (one owner). The CI cache key (`vulkan-sdk-1.4.341.1-windows`) names only
  the version, so warm lanes keep their cache; the cold `win-debug-scalar` lane installs on every run and is the
  proof on the next complete run.
- **The QFont line** is printed under every platform plugin measured (the default `windows`, `minimal`,
  `offscreen`) and with either repository, right after the second `Loading component scripts...`; no measured option
  touches it. Which code emits it is the next question.
- `--verbose` adds lines but no host: the installer never prints the repository's URL, so the `hosts` column is empty
  on every arm. It also prints `Warning: Component com.lunarg.vulkan.core specifies "Default" property while being
  not checkable`, which the production (non-verbose) output does not show.

**The classifier was wrong.** `vulkan-sdk-installer-check.py` counted the framework's `Fetching latest update
information...` line as a remote fetch, but the framework prints it for a local repository too
(temp-repository-updates and offscreen-updates print it with nothing remote in their cache). The remote finding is
now only the `Retrieving meta information from remote repository` line; `judge` reads both findings again from each
arm's `log.txt`, so recorded evidence is judged by the current patterns; and `run` records the text of each cached
`repository.txt` (`cache_repositories`), the one place a run names the repository it read. The hosted judge job runs
72f9696f's script and will count one remote line for every arm; the table above, from the downloaded artifacts and
the corrected script, is the record. `test_vulkan_sdk_installer_arms_record_their_evidence_and_are_judged_against_two_baselines`
now gives every fake arm the generic line, as the real installer does; teeth: the old pattern fails it (`2 != 1`),
and so does judging from `result.json` instead of the log (`'findings remain' != 'removes both, same tree'`).
`test_vulkan_sdk_installer_verifies_runs_and_checks_the_layout` holds the production command line and the written
repository.

## Evidence

- Local: `test-repository-tools.py -k vulkan` (3 tests) passes; the validators below pass.
- Track machine: the `linux-gcc-asan` reproduction and repair above, in a tree configured fresh from the preset.

## Remaining

- REPO.3: the next complete run's cold `win-debug-scalar` install with the local repository (no `Retrieving meta
  information from remote repository` line, the layout check passing); the QFont line's emitter (the installer's
  control and component scripts are in the pinned installer's resources) and its repair; the CUDA run on the measuring
  PC's NVIDIA GPU; then the close after REPO.3d.
- Read run 41298875777's last two arms and its judge summary.
- The next complete run's ASan lanes (`linux-gcc-asan`, both ARM64 ASan lanes, `win-asan`) for the Shewchuk cases.
