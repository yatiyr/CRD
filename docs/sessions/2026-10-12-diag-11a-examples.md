# DIAG.11a examples on the installed consumer: allocator selection, jobs, reload and capture, 2026-10-12

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11a](../ROADMAP.md#slice-diag.11a). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-11a) and
> [public consumption](../design/public-consumption.md#the-diagnostic-family-diag11a). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.11a the installed external consumer](2026-10-12-diag-11a-installed-consumer.md).

## Goal

DIAG.11a's last local clause: "Add examples for allocator selection, jobs, reload and capture" on the thin external
application that consumes the installed engine. Each example is a route of that application, built only against the
moved install prefix, run in a fresh process from a profile authored from the installed assets, and judged by the
package test on every binary it builds (diagnostic, profiling disabled, optimized profiling). No engine source changed.
Committed locally, not pushed.

## What landed

| Piece | Where | What it shows |
| --- | --- | --- |
| Shared support | `tests/support/package/diag-consumer/consumer_support.{hpp,cpp}` | the headless setup, the build line, the application's event codes, starting the host from a profile, one command through the host's own service, waiting for a written bundle |
| Allocator selection | `example_allocator.cpp` (`allocator <profile> <tlsf\|diagnostic>`) | the application chooses its order book's allocator: a TLSF heap over an arena it owns, or crd-memory's `DiagnosticAllocator` over that heap (redzones, quarantine, provenance, its records in a second independent arena); either way the owner gets `host.watch_allocator(chosen)`. Booking 1 KiB orders until the heap is exhausted captures an allocation-failure bundle whose trigger names the chosen allocator, with the bookings before it and the releases after it. The diagnostic choice also writes one byte past order 2: the redzone check reports `redzone-overrun` at byte 1024, the application records it, and the bundle holds it right after order 2's booking; the leak summary reads 0 |
| Jobs | `example_jobs.cpp` (`jobs <profile>`) | the host starts first and gives the pool the profile's watchdog window (`configure_jobs`); 64 jobs on four threads sum slices of 1..64000 to the known total and record each slice from its worker; then a job pinned to the main thread, which neither pumps nor waits, is reported by the watchdog as a hang (executor starved) and captured with the 64 slices before it; the application pumps, the job runs, the pool shuts down before the host stops |
| Reload | `example_reload.cpp` (`reload <profile> <limits.toml> <dir>`) | the diagnostic configuration: `apply()` of the unchanged profile is Unchanged, of the same profile after the policy file was edited is Applied with a new policy generation (the hitch threshold doubled), of a changed watchdog window is RestartRequired naming `watchdog_period_ms`, of a new capture directory is Applied. The application's own data: an order-limits table read with the packaged crd-toml, whose reader asks the engine's I/O fault seam first (`crd::consult_io_fault`); a seeded `failed-read` plan armed through the host's `fault.arm` fails the reload of the edited table, the last good generation is kept, and the injected-fault capture (naming `limits.toml`) is written to the new directory; disarmed, the next reload loads it. Without fault seams `fault.*` is not served and every reload loads |
| Capture | `example_capture.cpp` (`capture <profile>`) | a frame loop hands every frame to `host.frame()`; three captures are requested through the host's service (`flight.trigger`) while the application keeps running; each is written with the frames before and after the request; the directory keeps the newest `max_bundles` (2); `flight.status` reports the last |
| Proof | `scripts/test-package-consumer.py` (`Session.examples`, the judges `allocator_problems`, `jobs_problems`, `reload_problems`, `capture_problems`) | after the deliberate failure and its investigation, every example runs from the installed investigation profile with a 50 ms watchdog window and a policy of 16 post events, a 1 s post window and an unreachable hitch threshold (capture: `max_bundles = 2`); what each prints and leaves is judged, and every bundle is read back through the application's own `inspect` route |
| Unit tests | `scripts/test-repository-tools.py` (`test_package_consumer_judges_the_diagnostic_applications_examples`) | the script's event codes equal the application's header; each judge accepts the real shapes and names each deliberate mismatch |

`main.cpp` keeps the deliberate failure and the inspect route and dispatches the four example routes. The application
now links `Cerid::memory`, `Cerid::jobs` and `Cerid::toml` besides `Cerid::perf`, the modules the examples name.

## Design notes

- Reload means what the package can reload: no resource manager or platform file reader is packaged, so the examples
  reload the authored diagnostic configuration through `apply()` and the application's own authored data through its
  own reader on the engine's I/O fault seam. That seam is public in crd-core, so the host's fault injection reaches an
  application's reload without any engine edit.
- Every example is its own process with its own host: `watch_allocator` is one per host, `start()` is refused once the
  pool runs, and the deliberate failure ends its process.
- Lifetimes are part of the example: the host is stopped before the allocator it watches is destroyed, the pool's
  watchdog thread is joined before the host stops, and a command service is destroyed before its host.
- The allocator example's overrun is reported three times by design: by the check right after it, by the check its
  release runs, and when its block leaves the quarantine. The judge requires the first report and its position.

## Teeth

Seven breaks of the application (applied to the scratch copy, never the repository source, rebuilt, judged, then
restored from the repository file and rebuilt) and two breaks of the engine (a byte-exact backup of
`diagnostic_host.cpp`, a whole package run, the backup restored with its mtime bumped and compared byte for byte).
Every one failed.

| Break | Failed |
| --- | --- |
| the owner gets the chosen allocator, not the watched one | allocator-tlsf: no bundle (exit 8) |
| no byte written past order 2 | allocator-diagnostic: no check violation, no overrun event, first `none` |
| the pool not configured by the host | jobs: no hang within the bound (exit 8) |
| the late job not pinned to the main thread | jobs: no hang within the bound (exit 8) |
| the limits reader skips the fault seam | reload: no injected-fault bundle (exit 8) |
| the policy file not actually edited | reload: `unchanged` instead of `applied`, no new generation, threshold not doubled |
| two capture requests instead of three | capture: trigger count, written count, the kept bundles, the last trigger's frame |
| engine: `configure_jobs` leaves the window unset | the whole package test, at the jobs example |
| engine: `apply()` keeps the old capture directory | the whole package test, at the reload example (the bundle written to the replaced directory) |

The judges' own teeth are the unit tests: each mismatch (wrong choice, moved overrun, a leak, a missing slice, the
late job running early, a wrong hang kind, a policy without a new generation, a kept reload that loaded, a missing
kept event, an unserved `fault.arm` on a build with seams, an injection on a build without, a bundle too many in the
capture directory, a gap in the frames after the request, a missing request) is named.

## Verification

| Run | Result |
| --- | --- |
| `win-debug` package test (by preset, and in the CTest argument form) | PASS: the failure, the investigation and all five example runs (`allocator-tlsf`, `allocator-diagnostic`, `jobs`, `reload`, `capture`) |
| `win-shipping` (profiling disabled) | PASS; fault injection unavailable, `fault.arm` unknown-command, the edited table loads |
| `win-shipping-profile` (optimized profiling) | PASS; fault injection unavailable |
| `linux-gcc-debug` (WSL) | PASS (`dwarf`) |
| `linux-gcc-shipping` (WSL) | PASS |
| repeats | the five examples 20 of 20 on the win-shipping scratch, 5 of 5 more after the post window was widened to 1 s, 10 of 10 on the linux-gcc-debug scratch |

The examples' post window was widened from 300 ms to 1 s last (test data in the script, so a busy capture closes by
its events even on a slow runner). The `win-debug` CTest-form run and both Linux runs are on that final state; the two
Windows shipping runs predate it, and the examples were rerun on their kept scratch with it (5 of 5).

- Strict tidy (LLVM 20.1.8) is clean on all eight application files against the application's own compile database;
  MSVC `/W4 /permissive-` and gcc `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion` are clean on its
  six sources (the reader opens files with `fopen_s` on Windows). clang-format differs only in the repository's hand
  alignment and case-label style.
- `test-repository-tools.py` passes; the Allman check, the guard CTests, check-master-plan and check-repository pass.
- No engine, workflow or preset file changed, so no engine lane was rebuilt for this batch and the coverage lane was not
  rerun. The full win-debug CTest was not run.

## What the hosted run must show

- `crd-package-consumer` green on `win-public-checks` and `linux-gcc-public-checks` (complete tier) with, besides the
  deliberate failure and its investigation, the five example runs each printing "showed what it must": the
  allocation-failure bundles of both allocator choices (the diagnostic one with the overrun after order 2), the hang
  bundle with the 64 slices, the reload's injected-fault bundle in the second capture directory with the last good
  table kept, and the capture directory keeping `flight-1.cdb` and `flight-2.cdb`.

## Remaining in DIAG.11a

- No local clause. The row waits for its first hosted run.
