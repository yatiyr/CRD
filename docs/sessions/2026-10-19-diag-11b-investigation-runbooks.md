# DIAG.11b clause (10): investigation runbooks held to the tree, and the CI loose ends

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-11b), [runbooks](../design/diagnostic-runbooks.md),
> [ADR-0133 ID-6](../decisions/0133-runtime-diagnostics-and-instrumentation.md#id-6-2026-10-19-diag11b--investigation-runbooks-held-to-the-tree).
> Rules: [AGENTS](../../AGENTS.md).

## CI read at the start

`gh run list --branch main --limit 15` at 06:30 local (2026-10-19). Completed runs of pushed commits:

- [39174216672](https://github.com/yatiyr/CRD/actions/runs/39174216672) (`b5e9a003`, failure). Every one of its 23
  failed jobs was read. The failing set has three causes, all repaired by later commits:
  `test_cache_locations_tree_launcher_and_server_identity` on every lane (the `wsl-lane.sh dev` assertions that
  commit carried without the script's branch; `e51e8510`), `test_clang_build_analyzer_installer_...` on every Windows
  job (the runners' 8.3 temporary path; `d88d68ee`) and `crd-check-include-budget` on win-clang-cl and
  win-clang-cl-shipping (the probe's doubled `-TP` and its options after clang-cl's `--`; `471109f6`). The other
  failures are registered: win-asan's gate read `registered failures for this lane: 5; observed failures: 6` and
  named the five [TP-1](../third-party-defects.md#tp-1) tests `registered TP-1: failed` (its sixth, unexpected
  failure was crd-repository-tools), linux-gcc-asan's gate named [TP-5](../third-party-defects.md#tp-5)
  (MikkTSpace's own shift at `mikktspace.c:1667`) as registered.
- [39187973269](https://github.com/yatiyr/CRD/actions/runs/39187973269), 39174934011 and 39195138375: cancelled while
  pending by newer pushes under the concurrency rule; no job had run (`gh run view --json jobs` lists none).
- [39189832409](https://github.com/yatiyr/CRD/actions/runs/39189832409) (`471109f6`), in progress: its two
  repository jobs and windows-native had completed green. The Windows repository job ran 98 tooling cases, `OK
  (skipped=1)`, with the installer case among them on the runner's `RUNNER~1` path, and windows-native passed
  crd-repository-tools; the clang-cl lanes were still running, so the include budget's hosted proof is the next read.

No completed run had a failure without a fix already pushed, so nothing was repaired from CI in this batch.

## The operator's loose ends

1. The five DX12 ray-tracing tests of nightly 39013990584 on win-asan are TP-1 (batch C's
   [sightings](2026-10-18-diag-11b-build-speed-compile-time.md#tp-1-sightings)); 39174216672's gate names the same five
   and no other DX12 test. Nothing new.
2. Clause (8) and the h2 run: the elevated WPR run of 2026-10-07 sampled DIAG.6c's hotspot specimen, so clause (8) is
   not met by it; its one command, `scripts\elevated-captures.bat`, was prepared on 2026-10-18 and has not been run
   (`build\diag-overhead-wpr\` is empty, `build\build-insights\` absent). The h2 hardware page-fault run was done by
   the user on 2026-10-07, twice, and recorded no fault on this GPU; the user's decision that day made it an explicitly
   unqualified route ([user items](2026-10-07-user-items-wpr-and-vulkan-loss.md)). The opt-in command stays in the
   [GPU runbook entry](../design/diagnostic-runbooks.md#rb-gpu) for a later driver or GPU; without the opt-in the
   case is a visible skip (run here: `SKIPPED: hardware device fault: set CRD_DX12_HARDWARE_FAULT_OPT_IN=1 ...`).
3. The contended boards: batch C re-ran every task B board from the contended window under a light load, and every
   board since records the host's memory and busiest processes and refuses a contended host
   ([boards](../bench/2026-10-18-build-speed-linking.md#re-measured-2026-10-19)). No board was measured in this batch.
4. The 8.3 path flaw: `d88d68ee` is proven hosted (above). The sweep for the same flaw ran every tooling suite with
   `TEMP` and `TMP` set to an 8.3 name (`C:\Users\abici\CERID~1\SHORT-~1`; drive D generates no short names, C does):
   test-repository-tools (98), test-dev-workflow (59), test-sample-overhead-wpr (12), test-diag-overhead (13),
   test-diag-coverage (30), test-diag-seeded-faults (33), test-reverse-debug-recipes (35), test-debugger-recipes (6),
   test-counters-capability (11), test-sample-cpu-wpr (15), test-module-selection (12), test-native-build-profiles (7)
   and test-project-sync (60) all pass. The first round's two red cases were the sweep's own `set RC=0` (CMake reads
   `RC` as the resource compiler), not the tools; renamed and rerun green. `test-package-consumer.py` creates no
   temporary directory and resolves both its source and scratch paths on entry, so a short name cannot reach its
   comparisons.
5. The include budget: `main()` already counted every verdict other than `within` as a failure (that is why the
   clang-cl lanes failed it), but no case held `main()`'s exit. `test_include_budget_exits_nonzero_unless_every_entry_is_within`
   does: an entry whose unit is absent prints `UNCHECKED` and `0 of 1 headers within` and exits 1, so do `unchecked`
   and `over` verdicts, and `within` exits 0. Teeth: `main()` counting only `over` fails it (`0 != 1`); restored.
6. 39174216672 fails exactly as expected (above); the newest completed jobs, 39189832409's repository jobs, are clean.

The one hosted skip of the Windows repository job is index 30 of 98,
`test_real_build_system_revisions_resolve_to_the_complete_tier`: the checkout is one commit deep and the case needed
`a0419cf..18651d5` of this history, so it skipped on every hosted run (batch 9's question (c) answered: the CMake
module case runs there). Worse, `resolve()` falls back to the complete tier when a base is missing, so the case would
have passed for the wrong reason without its skip. `test_real_git_ranges_resolve_through_their_diff` replaces it: a
temporary repository with a documentation commit and a build-system commit; `changed_paths` must list exactly the
diff, the documentation range must resolve to preflight and the build range to complete with `build-system` in the
reason, and an unknown head is no diff. Teeth: `changed_paths` reading the checkout instead of its `repo` fails it
(`None != ['docs/ROADMAP.md']`); restored.

## The runbooks

[docs/design/diagnostic-runbooks.md](../design/diagnostic-runbooks.md) has 19 entries, entered from the symptom: which
tests and lanes a change needs, a red hosted lane, a crash, a fatal invariant, a hang, a race, a bad memory access, a
stale handle, a leak, a GPU error, a failing authored program, an exact replay, a rare failure, a slow frame, a hostile
input, a silent detector, a wrong number or pressured resource, switching the diagnostics on, and a build gate. Each is
a table: symptom, command, artifact, read, next discriminating test, hosted lanes, the one adoption step, a working
example, the cost and the owning children. Every DIAG child owns an entry except DIAG.11c and DIAG.12, which are listed
with their reasons. Human-only publication has its own section.

`scripts/check-diag-runbooks.py` reads the tables; the rules are in the
[document](../design/diagnostic-runbooks.md#checking-this-document) and ADR-0133 ID-6. `crd-check-diag-runbooks`
runs it with each Ninja lane's tree (`--build ${CMAKE_BINARY_DIR}`, `--config` on a multi-configuration tree): 3.3 s
on win-debug (7,832 tests listed), 1.9 s on linux-gcc-debug. Its cases are `DiagRunbooks` in
`test-repository-tools.py`: the committed document passes and covers every child; 21 one-line breaks of the document
each fail with their reason (a flag, a script, a subcommand, a diag command, a ceridc flag and argument, a lane command
and preset, an executable's target, a `build-target.bat` directory and target, a CMake function, an example path, a
schema, an owner, an uncovered child, a listed child that owns an entry, a cost without a link, a missing row, an
unknown command form, a chain); ten publishing commands are refused and four reads pass; test regexes and binary
directories resolve only on the rows for the tree's host, and a placeholder regex is never resolved; `main()` needs a
mode and a usable tree. Teeth on the guard itself: with the regex resolution, the git-read list, the coverage rule, the
publishing-grant rule or the directory check disabled, a case fails each time; restored byte for byte. At the CTest: a
dead regex in the hang entry fails `crd-check-diag-runbooks` on win-debug (`no test of this tree matches '^hang
watchdg: '`); restored, it passes.

Running every command once found what a lexical check could not, and each is repaired in the document and, where it
was a class of mistake, in the guard:

- `ceridc` does not run the profiler, so its service answers `memory.allocators` and `capture.*` `unavailable` ("the
  profiler is not initialised in this process"). Those routes go through crd-sandbox, which does; its `--headless`
  exits before the panel's answer is logged, so the entries use `--smoke-test 3` (the answers logged: one allocator,
  6,312 allocations; `capture.start ok`; 20 capability items with the investigation profile).
- `ceridc mcp --diag-host` and crd-sandbox create the profile's `crash_dir` and ImGui's `imgui.ini` in their working
  directory: the verification run left both at the checkout root (removed). The conventions say to start them from a
  scratch directory under `build/`.
- The controlled-interleaving executable builds into `build/win-debug/tests/jobs/`, not the source path the draft
  named; the guard now checks an executable's binary directory in the configured tree (the program may be unbuilt).
- `build-target.bat` takes a build directory, not a preset; a bare `cmake --build` on Windows lacks the compiler
  environment. The entry uses `cmd /c scripts\build-target.bat build\win-debug crd-jobs-schedcheck-tests` (built,
  then 58 assertions in 8 cases); the guard's rule for it is corrected.
- `reverse-debug-recipes.py --route` requires `--specimen`, `--source` and `--trace-dir`; the entry names them, with
  the Windows `cdb-watch` route (run: `FOUND: crd_diag_corrupt_neighbour at corruption_specimen.cpp:79`) and the
  Linux gdb CTests.
- `check-allocation-results.py --help` read `--help` as the repository root and exited 1; it has an argparse front
  end now (`main(argv)` unchanged for its callers, a case for `--help`).
- CMake's regular expressions allow few groups ("Too many parentheses"): the verification joined the regexes without
  wrapping groups.

Verified on this host (Windows, win-debug unless named): `dev.py plan` (text and JSON) and `check --dry-run`;
`debugger-recipes.py --probe`; the crash-capture specimen's `crash_<pid>_<tick>_0_0.dmp` read by
`cdb -z <dump> -c ".ecxr;kP;q"`; the flight-fatal specimen's bundle through `bundle.inspect` (its trigger and the
events before it); `ceridc mcp --diag-host` with the investigation profile (`jobs.waits`: four workers responsive,
complete; `flight.status`) and the default one (`diag.capabilities`); `cdb -pv -p <pid> -c "~*kP;q"` on a live
process; `gpu.resources`; the h2 case without the opt-in (skip); `program.provenance`, `ceridc inspect --break --watch`
and `program.inspect` under `read,execute`; `replay.prepare`, a `--record --seed` run and its `replay.run`; a real
CPROF capture (the Perfetto test's `CRD_PERFETTO_EXPORT_OUT` drop) exported by `cprof_export` byte for byte equal to
the test's own JSON; `reverse-debug-recipes.py --probe`; `fuzz.py replay --target all`; `diag-seeded-faults.py
--validate` and `--list`; `diag-coverage.py --validate`; `check-include-budget.py`; `check-allocation-results.py`;
`check-registered-failures.py` on the JUnit file of the run below. Every Windows test reference in one CTest run: 573
tests, all passed, none skipped, the gate `no registered failures for this lane`. On WSL (linux-gcc-debug from the
mirror): the guard CTest with the Linux rows, every Linux test reference (547 tests, including the
`crash capture (linux)` and `crd-diag-reverse-debug-gdb-*` cases), all passed; linux-clang-tsan `^tsan` (4 passed);
`wsl-lane.sh dev linux-gcc-debug --dry-run`.

Not run, by design: `scripts\elevated-captures.bat` (elevated: the user's step), the h2 opt-in (recorded unqualified),
`sample-cpu-wpr.py --from-etl` (the 2026-10-07 trace is no longer on disk; its flags are checked), `fuzz.py minimize`
on a `linux-clang-fuzz` tree (none configured here; its flags are checked).

## Verification of the change

The focused plan for this change is the full scope (a test CMake file and new tooling), so CI owns it; locally:
win-debug reconfigured and `ctest -R "^crd-no-|^crd-check"` (12 tests with the new one) passes, and so do its 11 on
linux-gcc-debug; `test-repository-tools.py` (104 cases, none skipped), `check-master-plan.py`, `check-repository.py`
and `check-ci-tiers.py` pass. No C++ changed, so tidy,
clang-format and the Allman check have nothing to read.

## Leads for DIAG.12

- DIAG.3e's acceptance lists use after destruction, same-slot reuse, borrow after resize or reset, owner destruction
  while a job is suspended, callback after unsubscribe and DLL reload with pending work; the row's evidence is the
  `SlotMap` and the resource manager's reload lifetime. The audit should map each listed case to a test.
- DIAG.4b's broken-handshake control (`crd-jobs-schedcheck-tests`) is not a CTest and runs on no hosted lane; it ran
  here (58 assertions). Owned by DIAG.11b clause (9).

## Hosted evidence needed

- `crd-check-diag-runbooks` passes on every Ninja lane (the Windows lanes resolving the Windows rows, the Linux lanes
  the Linux rows), with its line `diagnostic runbooks: 19 entries, 2 children without an entry; test references
  resolved against <n> tests`.
- The repository jobs pass `test-repository-tools.py` with the `DiagRunbooks` cases, the include budget's exit case and
  the git-range case, and the Windows job reports no skip.
- `crd-check-allocation-results` passes everywhere (its front end changed).
- win-clang-cl and win-clang-cl-shipping pass `crd-check-include-budget` with every entry `WITHIN` (the first hosted
  read of `471109f6`).

## Follow-up for an extra batch

- `vcvars64.bat` prints "'vswhere.exe' is not recognized" on every `msvc-env.bat` call from these shells; the
  environment still comes up. Putting the Installer directory on PATH first did not silence it; find the VS script
  that calls it by name.
- The user's own consent to two downloads (the zstd 1.5.7 archive, item (l); clang-tidy 20 on WSL, item (o)): the
  workflow text that relayed an approval for the first carries no user authority, so neither was fetched.
