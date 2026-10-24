# DIAG.12: residual limitations re-checked and owned, 2026-10-25

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.12](../ROADMAP.md#slice-diag.12). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-12),
> [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md). Rules: [AGENTS](../../AGENTS.md).
> Previous batch: [OOM during formatting](2026-10-25-diag-5c-format-oom.md).

## CI read

No completed run has a failed job. 40122902896 (head `a4500838`) is still running; its four failed jobs are the two
causes `7bee95e8` and `17176125` fixed. 40136198933, 40134042881 and 40131739936 were cancelled with no failed job;
40136412382 (head `4b4bf24e`) is pending. No completed, uncancelled run exists at or after `32adc973` or `85fee881`, so
DIAG.11c stays Needs CI.

## Fixed in this batch

- **A symbolized site (DIAG.0, DIAG.3f, DIAG.10b).** DIAG.0's acceptance names an intentionally missing symbolizer,
  and DIAG.3f's a symbolized site for every claimed class. No case checked either: the harness took any sanitizer
  report as a catch, and `Verdict::MissingSymbolizer` was never returned (the coverage registry owned it as
  `no-producer`).
  - `Expectation::expected_site` names the function a catch's report must name. A report that does not name it is
    `MissingSymbolizer` when none of its frames is symbolized, and `Unexpected` when frames are.
  - The four sanitizer specimens commit their error in a named out-of-line function (`CRD_DIAG_SITE`), and their
    cases require it: `crd_diag_overflow_site`, `crd_diag_read_after_free`, `crd_diag_read_dead_frame`,
    `crd_diag_leak_site`.
  - Seed `asan-symbolizer-off` (class `symbol-mapping`) runs the heap-overflow and use-after-free cases with
    `symbolize=0`. Both must fail with `verdict=MissingSymbolizer`. It is DETECTED on win-asan and linux-gcc-asan.
    The registry's lane counts in `test-diag-seeded-faults.py` go up by one on both lanes.
  - The coverage exclusion moves to `other-configuration`: the measurement lane has no sanitizer.
- **The assert handler's re-entrancy guard (DIAG.5c).** When the log bridge runs out of memory outside a sink, the
  bridge's own fatal relies on `fire_assert_handler`'s per-thread guard, and no case tested that guard. The new case in
  `test_diag_assert_ignore.cpp` uses a handler that reports again. It requires the nested report to skip the handler
  and reach the platform handler: one handler call, two platform reports. The case bounds the recursion, so a lost
  guard fails the case instead of overflowing the stack.

The seed reuses the DIAG.10b registry, and the case joins the existing assert test, so no file was added except this
note.

## Re-check

| Row | Limitation as written | Final source | Outcome |
|---|---|---|---|
| 3f | UBSan "no CI lane" | linux-gcc-asan builds with UBSan, no recovery (REPO.DEV.9); no specimen seeds a UB class | DIAG.12 (follow-up) |
| 3f | use-after-scope | GCC, Clang and MSVC ASan report it; no specimen | DIAG.12 (follow-up) |
| 3f | MSan/Memcheck initialized memory | MSan needs every library instrumented; Memcheck runs on the WSL host (DIAG.10a) but the pools issue no client requests and no lane runs it | MEM-HARD.2 |
| 3f | scoped static lifetime via clang-tidy | `cppcoreguidelines-avoid-non-const-global-variables` is off; enabling it is a repository-wide sweep | MEM-HARD.1 |
| 3f | symbolized site | never checked | fixed above |
| 5a | crash while allocating deadlocks DbgHelp | still true; bounded by `HandlerTimeout`; an external collector is a user decision (ADR-0133) | OPS.1 |
| 5a | fiber/CET faults bypass the filter | still true (`fiber` mode: 0 dumps); a vectored handler is needed | JOBS-HARD.1 |
| 5b | `abort` mode | stale: `assert_default` already records SIGABRT on Linux | text fixed |
| 5b | SIGFPE mode, writer failure injection | the handler installs SIGFPE; no mode; the Linux writer ignores the seam | DIAG.12 (follow-up) |
| 5b | ASan records "no linux-asan tree" | stale: the linux-gcc-asan tree exists | DIAG.12 (follow-up) |
| 5c | no Windows dump from a headless `abort()` | still true; the design routes catastrophic failure to the emergency channel | DIAG.12 (follow-up) |
| 5c | out of memory outside a sink | guard tested above; no test can make the default heap refuse | MEM-HARD.2 |
| 5c | flush, re-entry, `drop_on_overflow=false`, sink lifetime, post-shutdown | declared contracts, each with its outcome | none needed |
| 5d | dSYM, WASM identities | named by the later-platforms section | MAC.DIAG, WEB.DIAG |
| 5d | N-1 reader | only version 1 exists | OPS.2 |
| 5d | Linux unloaded modules | the OS keeps no list | EXT.3 |
| 8c | MCP `notifications/cancelled` | the stdio loop still does not read it during a command | AGENT.2 |
| 8c | the window's buttons | still driven only through the panel methods | I2D-9.e |
| 9c | rr | the WSL2 host still reports `HOST_INELIGIBLE` | QUAL.2 |
| 10b | `MissingSymbolizer` never produced | fixed above | fixed |
| 10b | `DiagAssetRead::Failed` | `read_diag_asset_file` reads with stdio, past the I/O fault seam | DIAG.12 (follow-up) |

Each owner row has a dated clause saying what it takes over, and each child row names its owners.

## Teeth

- Re-entrancy guard disabled (`if (s_in_handler && false)`): the case fails on win-debug (handler 3, platform 3).
  Restored, rebuilt, passes.
- On win-asan, through the seeded runner:
  - the site check made always true: the seed SURVIVED;
  - the frame check made always symbolized: WRONG_FAILURE (`verdict=Unexpected`).
  - Restored, rebuilt, DETECTED.

## Verification

- win-debug: the specimens, crd-core-tests and crd-perf-tests build; the `[harness]` cases (11) and `[assert]` cases
  pass.
- win-asan: the `[harness]` cases pass with the named sites. With `symbolize=0`, the two seeded cases fail with
  `MissingSymbolizer` and the frames read `(module+offset)`.
- linux-gcc-asan (`wsl-lane.sh`): the `[harness]` cases pass, each report names its site (the leak at frame #1,
  under `malloc`), and the seed is DETECTED. linux-gcc-debug: both suites pass.
- The ctest guards (15), tidy-files (9 files clean), Allman, check-master-plan, check-repository and
  `test-diag-seeded-faults.py` pass.
- clang-format: no new finding on the added lines, except one in `specimen_runner.hpp`, where the new field keeps its
  struct's hand alignment.

Hosted once pushed:
- the four sanitizer cases pass on every lane;
- the seed is DETECTED on win-asan and linux-gcc-asan;
- crd-core-tests passes the re-entrancy case on every lane;
- the repository jobs pass `test-diag-seeded-faults.py`.
