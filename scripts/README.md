# Development scripts

Run commands from the repository root. Build/test rules and exact quality requirements live in
[`docs/BUILDING.md`](../docs/BUILDING.md); the only work tracker is [`docs/ROADMAP.md`](../docs/ROADMAP.md).

- Windows configuration/build: `configure-preset.bat <preset>`, `build-target.bat <build-dir> <target>`.
  Both use `msvc-env.bat` to discover Visual Studio and select standalone CMake. The default build cap is four;
  `CMAKE_BUILD_PARALLEL_LEVEL` can set a smaller/larger justified cap. When the pinned sccache is installed
  (`install-sccache.py --destination %USERPROFILE%\.cerid\tools\sccache`), `configure-preset.bat` configures a Ninja
  preset through it (`compiler-cache.py configure-args <preset>` prints the entries; `CRD_SCCACHE=off` reverts) and
  `build-target.bat` starts its server on `build/sccache` (`compiler-cache.py server|stats`)
  ([local cached trees](../docs/design/build-performance.md#local-cached-trees)).
- WSL lanes: `bash /mnt/d/Dev/cerid/scripts/wsl-lane.sh sync|check|configure|build|test|run|dev|tree` builds and tests
  `~/cerid-mirror-build/<preset>` from the ext4 mirror `~/cerid-src`, kept exact by `source-mirror.py sync|check`
  (a stale mirror is refused) ([WSL source mirror](../docs/design/build-performance.md#wsl-source-mirror)); `dev`
  runs `dev.py check` in the lane's tree, the route `dev.py plan` prints for a lane
  ([risk routes](../docs/design/developer-workflow.md#risk-routes)).
- Waiting: `wait-for.py start` runs a long build or test detached under a deadline (at most an hour) with a log and
  an exit-code sentinel; `wait-for.py exit-file|file|pid|run` wait for it in bounded calls (at most 1,140 s, under the
  loop watchdog) instead of fixed sleeps ([waits](../docs/design/build-performance.md#waiting-for-long-commands)).
- Tests: invoke CTest directly with a scoped regex, `--timeout` and `--no-tests=error` as BUILDING shows.
  Do not send regex alternation through a `.bat` invocation from PowerShell: cmd reparses metacharacters.
- Changed C++: `tidy-files.py` on any host (`tidy-files.ps1` wraps it on Windows); `tidy-rename-helper.ps1 -LogFile
  <capture> -DryRun` previews mechanical fixes.
- Repository/docs: `check-repository.py`, `check-master-plan.py`, `test-repository-tools.py`; module registry
  contracts: `test-module-selection.py` ([design](../docs/design/module-registry.md)); CI tier mapping,
  resolution and lane evidence: `check-ci-tiers.py`, `ci-tier.py`, `ci-evidence.py`
  ([design](../docs/design/ci-tiers.md)).
- Workspace ([recipe](../docs/recipes/workspace-sync.md), [ADR-0136](../docs/decisions/0136-workspace-overlay.md)):
  `workspace.py sync|status|export|restore|verify|overlay` keeps the agent overlay (`.agent-overlay/`: CLAUDE.md,
  opencode.json, `.claude/`, `.codex/`, `.agent/`) beside the code repository, writes verified Git bundles, pinned
  tool archives and exports to the NAS folder under a lock, pushes code to the code hosts after refusing agent
  files, transcripts and secret shapes, exports a self-restoring archive (portable settings by default,
  `--include-local-settings` for an owner copy) and restores the whole workspace on any machine; `test-workspace.py`.
- Native Visual Studio: `project-sync.py open --preset win-vs-debug` starts saved structure synchronization.
  `status`, `stop`, `sync` preview and `edit --operations <json> --apply --regenerate` share one portable transaction path.
  Read [structure/recovery](../docs/design/project-structure-sync.md) before module moves or Remove/Delete operations.
  Regressions: `test-project-sync.py`; isolated real compile: `test-project-sync-native.py` (optional Windows `--ide`).
- Pinned inputs ([registry](../cmake/pins.json), [design](../docs/design/pinned-inputs.md)): `check-pins.py` guards
  it; `pins.py fetch <section> <name>` downloads and verifies one entry; `install-vulkan-sdk.py` (Windows),
  `install-vulkan-headers.py` (Linux), `install-vulkan-validation.py` and `install-warp.py` acquire their inputs
  from it, accepting `--archive`/`--archive-dir` for verified offline copies; `install-sccache.py` takes `--archive`
  and `--download-dir`. `install-genmc.py --destination <dir>` builds the pinned GenMC against the apt LLVM of its
  pinned major (Linux; stamped, so a cached install is reused only while pin and LLVM match) and
  `check-genmc-models.py --genmc <dir>/bin/genmc` runs the DIAG.4b model both ways and checks both verdicts.
- Build performance ([design](../docs/design/build-performance.md)): `build-bench.py run` measures configure, cold,
  no-op, source-edit, header-edit, PCH-header-edit, link and teeth (`--tooth FILE OLD NEW`: break, rebuild, restore,
  rebuild) rows of one preset in a fresh scratch directory (explicit `--jobs`, `--targets` to scope the build rows,
  optional `--sccache` statistics) and `render` merges boards for `docs/bench/`; the lane evidence bundle records
  every hosted build's Ninja log and sccache statistics with the cache backend. Every board records the host's
  available memory and busiest processes before and after (on WSL the Windows host's too) and refuses a contended
  host unless `--allow-contended`; `build-bench.py host` prints that snapshot and exits 3 when it is contended.
  Decision boards do not wait for a quiet host (user order, 2026-10-31): they pass `--allow-contended`, record the
  load and are judged by ratios of interleaved arms ([bench rules](../docs/bench/README.md)).
  `build-profile.py run --compiler msvc|clang --preset <p> --build-dir <fresh> --jobs <n> --label <l> --output <json>`
  profiles a preset's compile time in an uncached tree (MSVC `/Bt+ /d1reportTime`; Clang `-ftime-trace` read by the
  pinned ClangBuildAnalyzer that `install-clang-build-analyzer.py` builds into `~/.cerid/tools`; `--vcperf <session>`
  adds C++ Build Insights in an elevated shell) and `render`/`compare` print its tables;
  `check-include-budget.py --build <tree>` (the `crd-check-include-budget` CTest) fails a hot header of
  `include-budget.json` that reaches a forbidden standard header, with the include chain.
  `check-isa-isolation.py --manifest <tree>/isa-isolation/<config>.manifest` (the `crd-check-isa-isolation` CTest,
  ADR-0134 §6) fails general code above the CPU floor, a kernel unit above its level and a symbol shared between a
  level and general code, judging IL lanes on the linked images; `isa-isolation.json` holds its reviewed exceptions.
  `elevated-captures.bat` is the user's one elevated step (overhead sampling and Build Insights; `sampling` or `build-insights` runs one alone).
  `wsl-lane.sh build` takes `CRD_LANE_KEEP_GOING=1` to report every broken unit of a widely included header change in
  one round.
- Public consumption ([design](../docs/design/public-consumption.md)): `check-headers.py --build <dir> --changed`
  compiles changed public headers standalone with a sibling unit's flags from the compile database (outputs
  redirected to a scratch directory); `test-package-consumer.py --preset <name> --scratch <dir>` installs the
  diagnostic family's package (DIAG.11a: archives, headers, declared symbols, the diagnostic assets), moves it,
  deletes the engine build tree, builds and runs a core consumer and the thin diagnostic application
  (`tests/support/package/diag-consumer`) against it, and proves the profile header, relocatability, the symbols
  reaching the program, the application's deliberate failure and its `bundle.inspect` investigation, and the two
  refusals (it is the `crd-package-consumer` CTest of the public-check presets).
- Test instruments ([design](../docs/design/test-instruments.md)): `fuzz.py run|merge|minimize|adopt|replay` drives
  the bounded libFuzzer targets of a `linux-clang-fuzz` build (time, length, RSS and allocation limits; artifacts
  in scratch; a finding enters a committed corpus only after its fix, `adopt`; an unfixed one is kept as bytes
  under `fuzz/pending/`) and replays the corpora any lane runs as the `crd-fuzz-*-corpus` CTests.
  `fuzz-host-check.py --build <dir> --target <name> --seed <n> [--repeat <unit>]` runs the DIAG.10a host
  separation on one machine (a round under the default ASan options and the container-overflow control, the
  repetitions of an adopted unit; every event with its stack head); the dispatch-only `host-check.yml` runs it.
- Provenance and licenses ([routes](../docs/CONTRIBUTING.md#generated-sources-and-provenance)): `check-generated.py`
  holds every generated file to [generated-sources.json](generated-sources.json) (generator, status, SHA-256;
  `--scan` lists unlisted markers, `--regenerate` reruns the stdout generators on the current host and applies the
  formatter of a `formatted` entry when clang-format is available, `--refresh <path>`
  records a deliberate regeneration); `gen_license_manifest.py [--check]` renders
  [dependency-licenses.md](../docs/generated/dependency-licenses.md) from the pins registry. Both are lane steps.
- Linux Vulkan validation: `install-vulkan-validation.py --destination <ignored-directory>` installs the pinned,
  SHA-256-checked layer. Export the printed layer/library paths before GPU tests; it does not replace the driver.
- External diagnostic runbooks ([contract](../docs/design/runtime-diagnostics.md#diag-6c)): `sample-cpu-wpr.py`
  (WPR/xperf or perf sampling of the unscoped hotspot; `--from-etl` re-analyses a trace without elevation),
  `counters-capability.py` (counter classes with permission, multiplexing and missing-tool verdicts) and
  `debugger-recipes.py`. Their mocked verdict tests (`test-sample-cpu-wpr.py`, `test-counters-capability.py`,
  `test-debugger-recipes.py`) need no tools or elevation; they are `crd-diag-*` CTests and repository CI steps.
- Reverse debugging and watchpoints ([contract](../docs/design/runtime-diagnostics.md#diag-9c)):
  `reverse-debug-recipes.py --probe` reports which routes this host can run; `--route` runs one (`gdb-record`,
  `gdb-watch`, `cdb-watch`, `rr`, `ttd`) against the corruption specimen and names its writer or the instrument
  failure. The gdb routes are `crd-diag-reverse-debug-*` CTests on Linux; `test-reverse-debug-recipes.py` covers
  every verdict without a debugger and is a CTest and repository CI step.
- Seeded detector failures ([contract](../docs/design/runtime-diagnostics.md#diag-10b)):
  `diag-seeded-faults.py --build-dir <dir> [--preset <test-preset>] [--seed <id>]` applies each seed of
  `tests/support/diag/seeded-faults.json` that fits the lane (a source edit or extra sanitizer environment), rebuilds,
  reruns its detecting tests and reads their JUnit; it restores and rebuilds the tree after each seed. `--validate`
  checks the registry against the tree; `test-diag-seeded-faults.py` covers every verdict without a build and is a
  CTest and repository CI step. The `Seeded detector failures` CI step runs it on the five lanes the seeds were
  qualified on (win-debug, win-asan, linux-gcc-debug, linux-gcc-asan, linux-clang-tsan) and keeps the lane's own
  `Testing/Temporary/Last*.log` record as the lane's test run left it.
- Diagnostic coverage ([contract](../docs/design/runtime-diagnostics.md#diag-10b)):
  `diag-coverage.py --build-dir <linux-clang-coverage tree>` runs the test set of
  `tests/support/diag/coverage-scope.json` once under clang source-based coverage, reports regions, branches,
  functions and lines per scope file with the instrumented denominator, and fails on an error-path value or state
  transition that did not run unless an exclusion owns it (a stale exclusion also fails). `--validate` checks the
  registry against the tree; `--export <json>` re-evaluates an existing llvm-cov export. `test-diag-coverage.py`
  covers every verdict without a build, and holds `tests/support/diag/nonreproduced-failures.json` (flaky and
  nonreproduced failures, evidence only) complete and unread by any build or test step; it is a CTest and repository
  CI step.
- Diagnostic overhead ([contract](../docs/design/runtime-diagnostics.md#diag-11b)): `diag-overhead.py run --exe
  <crd-diag-overhead> [--baseline-exe <compiled-out build>] --scratch <dir> --label <lane> --output <board.json>`
  runs the harness (`tests/support/diag-overhead`) once per process for every workload and arm, interleaved, and
  judges ADR-0133 ID-5's budgets on the frame time and on the CPU per frame, the worse deciding (within, over, or
  unresolved when the host's noise is wider than the claim); `render` writes the markdown of a board for docs/bench.
  `--cpu-level` runs every process at that CRD_CPU_LEVEL; a record that did not run the asked level (ADR-0134 §8)
  fails its process and arms that ran different selections fail the workload. `test-diag-overhead.py` covers every statistic and verdict with fake processes and is a CTest and repository CI
  step. `sample-overhead-wpr.py record` (an elevated shell)
  samples the same workloads with WPR in the boards' shape, every arm a process of one trace, and refuses a harness
  tree not current with the checkout; `analyze <run dir>` (no elevation) counts each process's CPU samples, the
  diagnostic frames and their call sites, and judges whether the sampled CPU per frame agrees with the harness's and
  sits in the diagnostic code (two binaries: whether off runs any) (`test-sample-overhead-wpr.py`, a CTest and
  repository CI step, mocks wpr, xperf and the build tool).
- Investigation runbooks ([document](../docs/design/diagnostic-runbooks.md)): `check-diag-runbooks.py --build <tree>`
  (the `crd-check-diag-runbooks` CTest) holds every entry to the tree: each command's script, flags, lane command,
  `ceridc` verb, diag command and grant, test executable and CMake name exist; each test regex matches a test of the
  tree on the rows for its host; artifacts, examples and costs resolve; every DIAG child owns an entry or is listed;
  nothing publishes. `--static` checks all but the test references; its cases are in `test-repository-tools.py`.
- Source routes ([contract](../docs/design/test-instruments.md#source-routes)): `check-source-routes.py --build
  <tree>` (the `crd-check-source-routes` CTest on every full configuration with a compile database) judges every
  first-party C-family source against the tree's `compile_commands.json` and `source-routes.json`: compiled by a
  target, or named with the conditions under which the tree builds it (platform, option, target), the file that
  builds it outside the tree, its includer, or the open ROADMAP row that owns routing it. Its cases are in
  `test-repository-tools.py`.
- Standards and obligations registers ([contract](../docs/design/registers.md)): `check-registers.py [--today
  YYYY-MM-DD]` (the `crd-check-registers` CTest and a repository CI step) holds `cmake/standards-register.json` and
  `cmake/legal-register.json` to their snapshots and the tree: no expired pinned draft, no obsoleted pinned RFC, no
  overdue review, known owners, rows, sources and anchors, an entry for every governed row, evidence for finished
  work and a `review.next` that is the day the register's review calendar and recorded runs owe; in the legal domain
  also an active check for every binding obligation, official primary sources, the consolidated text read before an
  entry's rows close, counsel review before implementation, live scopes once their rows are Done and a Turkish entry
  in every topic. `register-review.py due|plan|fetch|compare|record` runs
  the scheduled re-research ([contract](../docs/design/registers.md#snapshot-and-review)): it prints the review owed,
  plans the RFC Editor, datatracker and IANA sources or the instruments' official texts (a download the user
  approves first), reads them with SHA-256 digests, judges them against the register into a report of register
  changes, instruments to read in their consolidated text and proposed ROADMAP amendments
  (`--apply` writes the register and records the run; no row is ever edited) and records runs made by hand. Their
  cases are in `test-repository-tools.py`.
- Generators, oracle scripts and `bench_*`/`run_bench_*` helpers are reproducibility assets. Their matching
  recipes and dated benchmark boards define inputs, peers and qualification scope. `run-lane-bench.sh <target>
  [-D<option>=ON ...]` builds a `runtime/examples` program in the WSL linux-gcc-release lane tree and runs it pinned
  at each CPU level the pinned CPUs run (`cpu_level_record --levels`); the boards' Cerid programs are CMake targets, so
  no script keeps a link line for one. Each level's run follows the line of `runtime/cpu_level_record` under the same
  `CRD_CPU_LEVEL` and CPU list (ADR-0134 §8), which stops the board when the level did not apply. On the
  optimization track's benchmark machine (`ci/bench_vm.py`) the checkout is native, so the same drivers build from it
  without a mirror; `bench-host-setup.sh` provisions that machine (perf, the pinned Ninja and Vulkan headers, the
  system peers) and prints its census.
- Fast A/B boards ([method](../docs/bench/README.md#fast-ab-boards)): `ab-board.py --cmd NAME=COMMAND --arm
  LABEL=NAME:REGEX --reference LABEL [--cpus LIST]` runs the arms' processes interleaved and pinned, pairs each arm's
  values with the reference's by repetition and reports the median speed ratio with its bootstrap 95% interval and a
  verdict against the noise band, stopping once every cell is decided; `--same NAME,NAME:REGEX` stops it when two
  builds print different result digests (`bits`). Its cases are in `test-repository-tools.py`. The fft drivers:
  `ab_fft_f32.sh` (the planned f32 forward against MKL, HEAD against a candidate), `ab_fft_prerow.sh`
  (`prof_fft_kernel` against the pre-row engine or a saved build, forward or inverse) and `prof_fft_f32.sh` (perf
  stat top-down and further events, perf record).
- `per-slice-check.*`, `full-sweep.ps1` and the unfiltered WSL wrapper are broad CI recipes. Locally select targets.

Put one-shot scripts, logs and probes in ignored `build/<task>/`. Shared scripts must not encode a particular
developer's checkout, SDK version or home directory as their only working path. Preserve real exit codes.

Use [run-ctest.ps1](run-ctest.ps1) for scoped CTest: it supplies the detected MSVC runtime/disassembly tools and
forwards regex arguments directly. The old batch wrapper cannot safely receive a pipe-containing regex from PowerShell.
