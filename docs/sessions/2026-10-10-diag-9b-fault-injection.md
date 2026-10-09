# DIAG.9b seeded fault injection, 2026-10-10

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.9b](../ROADMAP.md#slice-diag.9b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-9b). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.9b flight recorder](2026-10-10-diag-9b-flight-recorder.md).

## Goal

The second slice of DIAG.9b: test-only, seeded fault injection through existing seams, with the shipping checks
(the `Inject` authority and a build gate), driven through the resource manager's load and reload paths so the
previous working asset is shown to survive. Device-loss simulation, the other lifecycle stages (cook, install,
execute, teardown), the authored policy, the `flight.*` commands, the doctor and a real host are later slices (see
the end). This batch is committed locally and not pushed.

## What was there (checked before coding)

- `DiagAuthority::Inject` existed as a distinct bit with no command declaring it.
- Allocation failure has a recoverable seam: `IAllocator::try_allocate`. The engine's payload loaders (MESH,
  TEXTURE, OpenPBR material, audio, anim, timeline) allocate their resource object with `try_allocate` and return
  null on failure; `allocate` is non-null or fatal by contract.
- All pack reads go through crd-platform: `read_file_binary` (mount, hot reload), `read_file_range` (sync and async
  loads) and `AsyncFile::read_async` (streamed loads).
- **A real defect.** `AsyncFile`'s completion job zero-filled `dst` when its read failed or came back short and told
  nobody: `read_async` had no status. `ResourceManager::run_stream_load_job` therefore handed a short or failed read
  to the loader as zero-filled artifact bytes, where the sync and async paths fail the load. A loader that accepts
  those bytes (or a placeholder loader) turned a partial read into a Ready or Placeholder resource.
- The hot reload (`do_reload_mount`) already keeps the last good payload when the pack read, the parse or the loader
  fails.
- crd-jobs has no cancellation. "Cancelled completion" is therefore defined at the one asynchronous completion the
  engine has, the `AsyncFile` job: it completes reporting Cancelled, delivering nothing.
- Shipping builds (`CRD_SHIPPING`) had no C++-visible flag; `_crd_shipping` is the top-level CMake condition (a
  generator expression, so native multi-config profiles keep working).

## What changed

- **The I/O fault seam** (`crd/core/fault_seam.hpp`, crd-core): one hook, `consult_io_fault(op, path, offset, size)`
  answering Proceed, Fail, Short(n), Delay(ns) or Cancel. With no hook it is one atomic load. The hook runs under the
  seam's mutex, so `uninstall_io_fault_hook` returns only after every call in progress finished. A shipping build
  compiles the seam out (`CRD_FAULT_SEAMS=$<IF:${_crd_shipping},0,1>`, private to crd-core): install is refused and
  the consult always answers Proceed. `fault_seams_compiled()` is the only way to ask.
- **crd-platform reads consult it.** `read_file_binary` and `read_file_range` fail on Fail, Short or Cancel (as a
  real failed or short read does) and sleep a Delay. `AsyncFile`'s completion job consults it as `AsyncRead`.
- **`AsyncReadStatus`.** `read_async(offset, dst, status)` now reports Complete, Short (the delivered prefix is in
  place, the rest zeroed), Failed (zeroed) or Cancelled (dst untouched); there is no status-less overload left. The
  job reads straight into `dst` (no temporary array) through an internal `read_range_into` that counts the bytes it
  got. To stay inside the 41-byte job closure the two sizes are 32-bit, so one read is at most 4 GiB - 1 bytes; a
  larger one is refused at submission.
- **The streamed load fails on any status but Complete** (`resource_manager.cpp`), logging the status, and the
  loader is never called; `LoadContext`'s comment tells loaders doing their own streamed reads to do the same.
- **`crd/perf/fault_injection.hpp`** (crd-perf):
  - `FaultPlan`: a seed and up to 8 rules, each a site, a first hit, a hit count (0 = unbounded), a probability in
    parts per million and a value. A plain-text form (`seed=7;notrigger;allocation-failure@2+1%1000000`, a value as
    `=n`) round-trips through `fault_plan_to_text` / `parse_fault_plan`; `validate_fault_plan` refuses an unknown
    version, no or too many rules, a zero or out-of-range probability, a value where none applies, a delay of zero or
    above 10 s, and a hit range that overflows.
  - Sites: `allocation-failure`, `failed-read`, `partial-read`, `delayed-completion`, `cancelled-completion`. Whether
    hit n of a site fires is a pure function (`fault_plan_fires`) of the seed, the site, the rule and n
    (a splitmix64 hash against the probability), so a plan injects the same faults over the same call sequence.
  - `FaultInjector::arm(plan, grant)`: Unauthorized without `Inject` in the grant (checked first, as the command
    service does), Unavailable in a build without fault seams, Rejected for an invalid plan (the armed plan stays).
    Arming restarts every site's hit count. A disarmed injector counts nothing.
  - A read consults every site it is eligible for (completion sites only for `AsyncRead`), so each site's ordinals
    count reads; the first that fires, in the order failed, partial, cancelled, delayed, applies. Only the applied
    one counts as fired.
  - A fired fault is recorded into the attached `FlightRecorder` (`kFlightCodeFaultInjected` 0x0908, a = site,
    b = hit) and, unless the plan says `notrigger`, its recipe becomes the recorder's replay recipe and an
    `InjectedFault` trigger fires (new `FlightTrigger` kind 9; `kFlightAllTriggers` is now 0x3FE). The recipe is
    `fault-plan/1 fired=<site>#<hit> minimal=<one-hit plan> plan=<the armed plan>`; the minimal plan comes first, so a
    clipped recipe (marked `...`) still reproduces the fault alone.
  - While armed, the plan owns the recorder's replay recipe: `arm` registers `fault-plan/1 plan=<text>` (a capture
    taken before anything fired is reproduced by the plan), each fired fault registers its own, and `disarm` marks
    replay unavailable ("the fault plan that owned the replay recipe was disarmed"), since neither recipe reproduces
    a later failure. A capture keeps the recipe registered when its trigger fired, so a pending one is unaffected.
  - Any plan's text fits `kFaultMaxPlanText` (800 bytes, checked by a `static_assert` against the widest rule), so
    `fault_plan_to_text` never clips.
  - `FaultAllocator` decorates an `IAllocator`: an injected `try_allocate` returns null; `allocate` is never failed.
  - `FaultInjector::io_hook` is the I/O seam's hook.
  - Commands (owner `perf`): `fault.arm` (Inject, one argument `plan`), `fault.disarm` (Inject) and `fault.status`
    (Read: whether the build has fault seams, the armed plan and each site's hits and faults).

## Tests

- `tests/foundation/perf/test_diag_fault_injection.cpp`, 7 cases tagged `[diag9b][fault]` in crd-perf-tests:
  - Plan text: a three-rule plan round-trips; 15 malformed or invalid texts are refused with a reason and leave the
    output untouched; 9 rules and an unknown version are refused by validation; the widest valid plan (8 rules, every
    number at its widest) is written whole and reads back the same.
  - Determinism: over 1200 hits the same seed fires the same hits, nothing outside the covered range fires, 30 % of
    the covered hits fire (a loose 200..400 band), another seed differs on over 100 hits, and the first firing rule is
    reported.
  - Arming: a Read+Record grant and a grant of everything but Inject are refused; with fault seams an invalid plan is
    Rejected with its error, an armed plan fails `try_allocate` but never `allocate`, and a disarmed injector counts
    nothing; on a shipping build the arm is Unavailable and the decorator passes every call through.
  - Commands: with Read+Record, `fault.arm` and `fault.disarm` are Unauthorized and no handler runs, while
    `fault.status` answers; with Inject+Read an invalid plan is BadArgument (`bad-probability`), and the arm is Ok with
    the plan in the status (or, on a shipping build, Unavailable "compiled out" and `"compiled":false`).
  - Flight recorder: a fault at allocation hit 2 fires an `InjectedFault` trigger (site, hit), the recipe starts with
    the fired fault and its one-hit plan, the injected event is the last event before the trigger, and the one-hit
    plan parsed back from the recipe fails the same allocation on a fresh injector. Re-armed, a manual capture
    carries the plan's recipe; after disarm it carries none, with the reason.
  - The I/O hook: a four-rule plan over four reads shows the per-site ordinals, the precedence, the shadowed delay
    and the counters; cancel and delay map to their actions; installed on the core seam a consult reaches the
    injector, a second hook is refused, and after uninstall a consult proceeds and counts nothing. A recipe longer
    than the recorder's 255 bytes ends in `...` after the fault and its one-hit plan.
  - Concurrency: 4 threads x 1000 hits at 20 % fire exactly the plan's set of ordinals (count and sum).
- `tests/foundation/platform/test_async_file.cpp`: the existing cases now check `Complete` and the refused-read
  `Failed`; two new `[diag9b]` cases with a test-local hook check each completion action (Failed and zeroed, Short
  with the real prefix, Short clamped one byte below the request, Cancelled and untouched, Delayed but complete and
  not early, one hook at a time) and the synchronous reads (Fail, Short and Cancel fail both reads, a Delay waits).
- `tests/assets/resources/test_fault_injection.cpp`, 2 `[diag9b]` cases in crd-resources-tests (which now links
  crd-perf):
  - A reload interrupted three ways keeps the last good MESH (same payload pointer, generation, bytes): an allocation
    failure in the real MESH loader through a `FaultAllocator` payload heap (the flight capture names the fault and
    its one-hit plan), a failed pack read and a pack read cut after 16 bytes. Disarmed, the same reload swaps the new
    content in with the next generation.
  - A streamed load whose completion is short, failed or cancelled ends Failed and the counting loader is never
    called; exactly one read (the streamed completion) is counted; a 20 ms delayed completion loads, not early; a
    control plan that covers a later read loads cleanly.
- On a shipping lane each case asserts the refusal branch instead (arm Unavailable, hook install refused, every
  read and load plain). On win-shipping the arming case's `Unavailable` assertion was seen passing.

### Teeth

Each break was built on win-debug and run, then restored by rewriting the file, rebuilt and rerun green:

| Break | Result |
| --- | --- |
| `arm` skips the Inject check | the arming case failed (5 assertions) |
| the seed left out of the hash | the determinism case failed (another seed no longer differs) |
| the streamed load ignores a non-Complete status | the streamed case failed: the loader was called for all three faults |
| a short completion reported Complete | the AsyncFile status case failed |
| a failed reload marks the resource Failed | the reload case failed (state Failed, payload null) |
| the I/O hook applies the last firing site | the I/O hook case failed (7 assertions) |
| a fired fault not recorded into the flight recorder | the flight case failed, and the reload case's capture check failed |
| `arm` and `disarm` leave the recorder's recipe alone | the flight case failed (the plan's recipe absent, the disarmed capture still "available") |
| a clipped recipe left unmarked | the I/O hook case failed (no `...`) |

The clip check also caught a slip in this batch's own code before it passed: the bounded text writer returned on
overflow without terminating the text (the earlier recipe code terminated it by hand; the shared clip marker did
not), so a clipped recipe read one byte past its end. The writer now terminates on overflow.

## Verification

| Lane | crd-perf-tests | crd-platform-tests | crd-resources-tests |
| --- | --- | --- | --- |
| win-debug (whole tree built) | 237 cases, 4,978 assertions | 49, 290 | 119, 14,559 |
| win-shipping (seams compiled out) | 104, 4,129 | 49, 212 | 119, 14,489 |
| win-clang-cl-shipping (seams compiled out) | 104, 4,129 | 49, 212 | 119, 14,489 |
| win-asan (inside vcvars) | 237, 4,903 | 49, 290 | 119, 14,559 |
| linux-gcc-debug (WSL) | 236, 4,902 | 49, 290 | 119, 14,559 |
| linux-gcc-asan (WSL, ASan + UBSan) | 236, 4,900 | 49, 290 | 119, 14,559 |
| linux-clang-tsan (WSL) | 236, 4,902 | 49, 290 | 119, 14,559 |

- The WSL lanes ran one at a time. The three fault suites (`[fault]` in crd-perf-tests, `[diag9b]` in the other two)
  were repeated 3 more times on each gcc lane and 10 more times under TSan, all passing; no TSan or UBSan report.
- The only sanitizer reports are the specimen children's intended ones: on win-asan heap overflow, use after free
  and the crash-capture null write; on linux-gcc-asan heap overflow, use after free, use after return and a leak.
- Strict tidy (LLVM 20.1.8) is clean on all 15 changed C++ files; it found an unused helper (which clang-cl's
  `-Wunused-function` would have failed the shipping lane on) and three local constants named in the global style.
- clang-format shows only the repository's hand alignment and single-line case labels. The Allman check passes.

## Hosted CI must later show

On all six lanes: crd-perf-tests green with the 7 `[fault]` cases, crd-platform-tests with its two `[diag9b]` cases,
crd-resources-tests with its two `[diag9b]` cases (each asserting the compiled-out branch on the shipping lanes), and
the 14 flight-recorder cases still green (the `[diag9b]` filter in crd-perf-tests now matches 21 cases).

## Remaining for DIAG.9b

- Device-loss simulation through the DX12 and Vulkan device-removal paths (DIAG.7b/7c), with the device's previous
  resources surviving where the provider promises recovery.
- Injection during cook, install, execute and teardown (load and reload are shown here), with the previous working
  assets surviving a recoverable failure.
- Refusal of arbitrary external I/O replay as a shipping capability check. Today a record that needs external
  results is refused before it runs (DIAG.9a); the check must also hold in a shipping build and be named by the
  doctor.
- The flight policy and a fault plan as authored assets (loaded through the existing config path, last known good
  kept), `flight.*` diag commands, and the doctor reporting the flight-recorder and fault-injection modes.
- A flight bundle carries only its manifest and the flight record; the host's sink must add the build and symbol
  identity sections (DIAG.5d) so the bundle can be symbolized offline.
- A real host (crd-sandbox) installing the recorder with its triggers, its frame watch, its sink and the fault
  commands.
