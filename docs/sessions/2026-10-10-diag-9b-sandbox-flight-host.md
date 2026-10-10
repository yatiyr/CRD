# DIAG.9b sandbox flight host, 2026-10-10

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.9b](../ROADMAP.md#slice-diag.9b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-9b). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.9b external I/O replay](2026-10-10-diag-9b-external-io-replay.md).

## Goal

The last local clause of DIAG.9b: a real host (crd-sandbox) installing the flight recorder, its trigger sources, a
`FlightCommandHost` sink with its module snapshot, and the `flight.*` and `fault.*` commands. After this batch every
local clause of the row is shown and the row waits only for its hosted run. Committed locally, not pushed.

## What was there (checked before coding)

- No production host used the recorder. `FlightRecorder`, `FlightCommandHost`, the trigger sources and the
  `FaultInjector` were exercised only by crd-perf-tests and the fatal specimen (`flight_fatal_specimen.cpp`).
- The sandbox built its `DiagCommandService` only with `--diag`, registered the program, replay and `gpu.resources`
  commands, and left `DiagServiceConfig::flight_recorder` and `fault_injector` null, so its doctor reported both
  modes as not installed.
- The jobs pool ran with `hang_watchdog_period_ms = 0`, so a starvation or hang handler would never be called. The
  watchdog is a thread kept in a static `std::thread`: with it on, any return from `main` that skips
  `crd::jobs::shutdown` would terminate the process at exit. Two such paths exist after `jobs::init` (the
  `--gpu-cull` refusal and the smoke failure).
- The sandbox has no `StreamingAllocator`, so the resource-pressure trigger (`FlightPressurePolicy`) has nothing to
  decorate. The resource manager allocates from a TLSF arena through `try_allocate` (loads, reloads).
- `FlightCommandHost::modules()` waits on the snapshot's mutex. A forward sink can be called on a fatal path (an
  assertion on any thread); if that thread was inside `refresh_modules` it would lock a mutex it already holds.

## What changed

- **crd-perf**, `FlightCommandHost::try_modules(out, overflow)`: the module snapshot for a forward sink. The
  snapshot's lock now records its holder; a thread that already holds it gets false at once, any other thread tries
  2000 times (the recorder's fatal-path bound) and then gets false. On false the sink writes its bundle with the module
  list declared absent. `set_forward` documents what a forward sink may not do.
- **crd-sandbox**, new `sandbox/src/flight_host.{hpp,cpp}` (`crd::sandbox::FlightHost`), owning the recorder, the
  command host, the injector, the frame watch and the allocator decorators:
  - `start(options)`: reads the policy (`--flight-policy <asset>` through `read_diag_asset_file` and
    `parse_flight_policy_asset`, else `FlightPolicy{}`), checks the capture directory, initialises the recorder,
    installs the assertion and fatal-invariant triggers (a second host is refused `triggers-refused` and leaves no
    recorder), takes the module snapshot, installs the command host as the sink with the bundle writer as its forward
    sink when a directory is given, attaches the injector and sets the jobs starvation and hang handlers. A refusal
    leaves nothing installed.
  - `configure_jobs`: a 250 ms watchdog window for a pool that has none.
  - `configure_service` and `serve`: the doctor's live pointers; `flight.*` and `fault.*` registered; the I/O and GPU
    fault hooks installed (refused in a shipping build).
  - `watch_allocator(backing)`: `FaultAllocator` over `FlightAllocatorWatch` over the arena, for the resource manager.
  - `frame(begin, end)`: the frame-hitch watch, then `poll()`; true when a capture was delivered.
  - The bundle writer: `flight-<n>.cdb` under the directory, the first free n, never over an existing file, then
    `enforce_bundle_retention(dir, "flight-", ".cdb", 10)`. It takes its own lock with a bounded try that refuses at
    once on the thread already inside it, reads the snapshot with `try_modules`, logs nothing, and calls neither the
    recorder nor a service; a capture it cannot write is counted (`bundle_failures`).
  - `stop()` (and the destructor): hooks, plan, jobs handlers, triggers, then the recorder, which delivers and writes a
    pending capture.
- **crd-sandbox `main.cpp`**:
  - `--flight-policy <asset>` and `--flight-dir <dir>`; the host is started before the diagnostic service and declared
    before it, so it outlives the service, the resource manager and the pool.
  - With `--diag`, `configure_service` and `serve` run with the sandbox's own commands.
  - The module snapshot is refreshed once the GPU context exists (the driver's modules load then).
  - The resource manager takes the host's decorated allocator.
  - `configure_jobs` before `crd::jobs::init`, and a guard that shuts the pool down on a return path that skips the
    ordered teardown (disarmed by the teardown, which shuts it down in its own place).
  - Each frame-loop iteration starts by feeding the previous iteration (start to start) to `frame()` and logging a
    delivered capture's summary and bundle path. The smoke report adds a DIAG.9b line.
  - The teardown stops the host right after `crd::jobs::shutdown()`.
  - A smoke failure (`0 frames presented`) now sets exit code 2 and leaves through the ordered teardown instead of
    returning from inside the loop. Found while testing: with a device loss injected at submission 120, the sandbox
    presented nothing, took the old `return 2`, and the scope-exit destruction order destroyed the Vulkan device
    before its shader modules (`VUID-vkDestroyDevice-device-05137`, 10+ reports) and then crashed (bash exit 139).
    After the change the same run exits 2 with no validation error and no crash.

## Tests

`crd-sandbox-inspect-tests`, new `test_flight_host.cpp` (7 `[flight-host]` cases, no window or device; the host's
source is compiled into the test as the panel's sources are):

- *a frame hitch is a capture the host writes as a bundle naming this build and executable*: an authored policy asset
  (64 events, 16 before, 4 after, events only, 20 ms hitch) is applied; the start snapshot names the test executable;
  on a real service with read, record and inject the seven commands are listed, `diag.capabilities` reports
  `flight-recorder` compiled, enabled and usable and `fault-injection` as the build has it; twenty 16 ms frames, a
  45 ms frame and the late one after it fire exactly one hitch, and the capture is delivered on the fourth frame after
  it; the writer wrote `flight-0.cdb`, whose import is Ok with this build's identity and this executable's module
  (identity equal to the snapshot's) and whose flight record holds the hitch (interval 45 ms, threshold 20 ms), 16
  frames before and 4 after; `flight.status` and `flight.write` read the same capture.
- *the writer never overwrites and keeps the newest ten bundles*: a sentinel `flight-0.cdb` is skipped and kept byte
  for byte; after 12 captures the directory holds 10, the newest `flight-12.cdb` and not the sentinel.
- *allocation failure at the resource allocator, injected and real, is one capture each*: armed through `fault.arm`,
  the first `try_allocate` returns null with one injected-fault capture, no suppressed trigger and the recipe's
  one-hit plan first; disarmed, a real 1 MiB request from a 64 KiB arena is one allocation-failure capture naming the
  size. Shipping: the arm is Unavailable and the same request succeeds.
- *a read failed through the I/O hook the host installed is an injected-fault capture*: `read_file_binary` fails and
  the fault event names `target.bin`. Shipping: Unavailable, the read succeeds.
- *the watchdog window the host gives the pool reports a real hang into the recorder*: a pool with a window keeps it;
  a default pool gets 250 ms; a single-thread pool with a queued job and a main thread that does not pump is reported
  `ExecutorStarved` into the recorder.
- *flight.status keeps the replay recipe when the trigger message and recipe are at their bounds*: an 8-rule plan
  makes the injected fault's trigger message (191 bytes) and recipe (255 bytes) hit their bounds; `flight.status` has
  every field, `replay_available` true, and the full one-hit plan inside the replay field. The summary is marked
  clipped only because those two strings are cut at the 160-byte field bound; no field is dropped, so the follow-up
  that asked whether the summary's field order should change needed no change.
- *refused starts install nothing, and stop restores every seam and delivers a pending capture*: an unknown key is
  refused by name, a missing directory is refused, and neither installs a handler; a started host owns the assertion
  and fatal handlers and the I/O seam (another hook is refused), a second host is refused `triggers-refused`; a manual
  trigger still open at `stop()` is delivered as Shutdown, truncated and written; afterwards the previous handlers are
  back, another I/O hook installs and is consulted by a read, `stop()` twice is safe, and a new host starts.

Teeth, each built on win-debug and run against `[flight-host]`, then restored:

| Break | Cases failing |
|---|---|
| T1 the command host not installed as the sink | 7 of 7 |
| T2 `try_modules` always refuses | the hitch case (the bundle has no modules) |
| T3 the decorators in the other order | the allocation case and the status case |
| T4 the assertion trigger not uninstalled | 6 of 7 (every later host is refused) |
| T5 `frame()` does not feed the hitch watch | the hitch case |
| T6 the service not pointed at the host | the hitch case (`diag.capabilities`) |
| T7 the writer writes over an existing name | the retention case |
| T8 no watchdog window for the pool | the hang case |
| T9 the I/O hook not installed | the read case and the stop case |

T7 first hung rather than failed: the failing `CHECK` compared the sentinel with the overwritten file's bytes, and
Catch2's printer stalled on the binary text (the large-array lesson in MEMORY). The comparison is now a bool, and T7
fails cleanly. The whole win-debug tree was rebuilt after the teeth.

## Verification

- **win-debug:** the whole tree built. `crd-sandbox-inspect-tests` 31 cases (24 before, plus 7) and `crd-perf-tests`
  255 cases green.
- **win-shipping:** `crd-sandbox-inspect-tests` 31 cases and `crd-perf-tests` 122 cases green; the 7 `[flight-host]`
  cases ran their refusal branches (180 assertions).
- **win-clang-cl-shipping:** the same targets and `crd-sandbox` linked first time; 31 and 122 cases green.
- **win-asan (inside vcvars):** `crd-sandbox-inspect-tests` 31 cases and the perf `[diag9b]`, `[flight]`, `[fault]`,
  `[doctor]` and `[identity]` cases (43) green, no sanitizer report.
- **WSL, one lane at a time** (`crd-sandbox-inspect-tests`, `crd-perf-tests` and `crd-sandbox` built on each):
  - linux-gcc-debug: `crd-sandbox-inspect-tests` 30 cases (23 before, plus 7), `crd-perf-tests` 253 cases, the perf
    flight, fault, doctor and identity cases (42) green; `[flight-host]` repeated 3 more times, all passing.
  - linux-gcc-asan: 30 and 42 cases green, `[flight-host]` 3 more times, no sanitizer report.
  - linux-clang-tsan with `setarch -R`: 30 and 42 cases green, `[flight-host]` 10 more times, no TSan report.
- **The real binary (win-debug, this machine's GPU), from a scratch directory:**
  - `--smoke-test 4 --diag-grant read,record,inject --diag fault.status --flight-dir <dir>`: the recorder started,
    the commands served (20 in the panel), the hooks installed, `fault.status` answered `compiled:true`; the startup
    frame hitch (an 844 ms first interval) was delivered and written as `flight-0.cdb`; PASS, exit 0.
  - `--diag bundle.inspect` on that bundle: 4 sections (manifest, flight record, build identity, symbol index), 83
    modules (the Vulkan loader and driver among them, from the refresh after the device), `same_as_host:true`.
  - `--diag diag.capabilities`: `flight-recorder` enabled and usable, `fault-injection` compiled and usable, not
    enabled, `external-io-replay` never.
  - `--backend dx12 --diag flight.status`: PASS, exit 0.
  - `--flight-policy` with an unknown key: exit 1, "policy-refused (unknown key "prevents" in a flight-policy
    document)". `--headless`: unchanged, exit 0.
  - `--diag fault.arm --diag-param plan=seed=1;device-loss@120+1%1000000`: the device was lost at the planned
    submission, the injected-fault capture (recipe `fault-plan/1 fired=device-loss#120 minimal=...`) was delivered
    and written, and the smoke failed with exit 2 through the ordered teardown (see above for the crash before).
  - win-shipping, the same arm: Unavailable ("this build has no fault seams"), hooks unavailable, PASS, exit 0.
- **Repository checks:** strict tidy clean on all 6 changed C++ files (one raw-string finding fixed); clang-format
  differs only in hand alignment and include grouping (main.cpp has the same count of non-whitespace differences as
  before); the Allman check, the 8 ctest guards,
  `check-master-plan` and `check-repository` pass.

## Hosted evidence still needed

The first hosted run must show what the DIAG.9b row lists, now including `crd-sandbox-inspect-tests` with the 7
`[flight-host]` cases green on all six lanes (injection branches on every lane with fault seams, refusal branches on
the shipping lanes) and `crd-sandbox` building on every lane. The real-binary runs
above are local evidence only: no hosted job runs crd-sandbox, and the hosted Windows runners have no Vulkan device.

## Follow-ups

- The `--gpu-cull` refusal in `main.cpp` (`return 2` before the frame loop) still leaves through the scope without the
  ordered teardown. It cannot be reached by an injected fault and the pool guard now shuts the jobs pool down there,
  but the GPU objects are still destroyed in declaration order.
- A capture delivered on the panel's worker (`flight.write`'s poll) or on a fatal path is written but not logged; only
  the frame loop logs captures.
