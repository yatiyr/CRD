# DIAG.10c lifetime accounting beyond heap bytes, 2026-10-11

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.10c](../ROADMAP.md#slice-diag.10c). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-10c). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.10c probes at the tensor and audio boundaries](2026-10-11-diag-10c-tensor-and-audio-boundaries.md).

## Goal

DIAG.10c's clause "include file/OS-handle, mapped-memory, device-object and callback registrations in lifetime
accounting, not just heap bytes". Committed locally, not pushed.

## The ledger (crd-core)

`engine/foundation/core/include/crd/core/lifetime_ledger.hpp`:

| API | What it does |
| --- | --- |
| `LifetimeClass` | FileHandle, LibraryHandle, MappedRegion (bytes counted), DeviceObject, CallbackRegistration |
| `LifetimeLedger::acquired(c, bytes)` / `released(c, bytes)` | relaxed atomics; a release that matches nothing (or more bytes than are live) changes nothing, is counted as unmatched and returns false |
| `snapshot()` | per class: acquired, matched releases, live, high water, live bytes, unmatched releases |
| `lifetime_ledger()` | the process-wide, constant-initialized ledger every wired site records into |
| `lifetime_report(before, after)` | `LifetimeVerdict` (Underflowed, Outstanding, Drained, Balanced, worst first), per-class masks and signed deltas |
| `format_lifetime_report` | one line, e.g. `outstanding: file-handle +1, mapped-region +1 (+4096 bytes); drained: callback-registration -1` |

Decisions:
- Recording is wait-free and allocation-free and the table is trivially destructible, so a site may record from any
  thread and during static destruction (the identity registry is a function-local static destroyed at exit).
- A release the ledger cannot match is an instrument fault, not a count to clamp: it is counted and the window says
  Underflowed. When the count matches but the bytes do not, the count is released, the bytes stay and the release is
  still counted as unmatched.
- A window is meaningful once the workload it brackets has finished; a snapshot is a set of independent reads.
- Outstanding includes bytes: a region swapped for a bigger one leaves the count equal and the bytes higher.

## Wired sites

| Class | Acquired | Released |
| --- | --- | --- |
| MappedRegion | `vm::reserve_at` after the OS granted the range (page-rounded size) | `vm::release` after VirtualFree/munmap succeeded |
| MappedRegion | crd-jobs `platform_stack_alloc` (guard page included) | `platform_stack_free` after the OS took it back |
| LibraryHandle | `DynamicLibrary::open` on success | the destructor after FreeLibrary/dlclose succeeded |
| FileHandle | `log::FileSink::open_file` on success (also each reopen after a rotation) | `close_file` |
| DeviceObject | the process-wide `gpu::identity_registry()` minting a Resource or Program identity | its retire (a stale or invalid retire releases nothing) |
| CallbackRegistration | `EventBus::subscribe`, `FileWatcher::add`, `ResourceManager::subscribe_reload`, `log::add_sink`, `jobs::set_observer` over none | unsubscribe/remove, the owner's destruction or a move-assignment over it, `clear_sinks`/`shutdown`, clearing the observer |

A failed free leaves the entry live, as the OS leaves the resource. `EventBus` was implicitly copyable; a copy would
have counted its subscriptions twice, so it is now move-only (`Application` already never copied it). `FileWatcher`
had defaulted moves; its move-assignment now releases the entries it overwrites. The identity registry feeds the
ledger only when built with `feeds_lifetime_ledger` (the process-wide instance), and under its own mutex, so its
DeviceObject count moves with `live_count(Resource) + live_count(Program)`. Pass identities name recorded passes and
are not device objects.

Not wired, by rule (named in the design doc): handles opened and closed inside one call (the `platform::fs` RAII
streams, `ceir::cook`'s bounded file reads and writes, the perf bundle/capture/export/asset/command/doctor/flight
writes, crd-jobs topology reads, the module snapshot); `crash.cpp`'s fatal-path dump files and its process-lifetime
handler thread and events. Not wired yet: the window, the WASAPI device's COM objects and event, worker and logger
threads, and native device objects without a Cerid identity; they come with the shutdown workloads.

## The OS oracle (crd-core)

`engine/foundation/core/include/crd/core/os_resource_probe.hpp`:
- `os_file_open_in_process(path)`. Windows: open the path for attributes only (no sharing check), read this
  process's handle table with `NtQueryInformationProcess(ProcessHandleSnapshotInformation)` into pages from
  VirtualAlloc, take the probe handle's object type (File), and for every other handle of that type that is a disk
  file compare its normalized NT final path, case-insensitively, with the probe's. crd-core now links `ntdll`.
  Linux: compare the `(st_dev, st_ino)` of every descriptor in `/proc/self/fd` (except the directory's own) with
  `stat(path)`. Another process's handle never counts, so a scanner or indexer cannot make a released file look held.
  `GetFileType` runs on each File-type handle, including the pipes ctest hands the process; a synchronous pipe with a
  read pending on another thread could stall it, so the probe is for quiet test and investigation processes.
- `os_range_mapped(base, bytes)`: Windows sums VirtualQuery regions not MEM_FREE; Linux parses `/proc/self/maps` in
  4 KiB reads. Present, Absent or Partial.
- A path that cannot be identified, an empty or wrapping range or another OS is Unavailable with a reason.
- Libraries have no oracle: both OSes reference-count a module and expose no per-process reference count.

## Tests (`[diag10c]`, also tagged `[lifetime]`)

| Program | File | Cases |
| --- | --- | --- |
| crd-core-tests | `tests/foundation/core/test_diag_lifetime_ledger.cpp` | 5: counts, high water and bytes; unmatched releases (count and bytes) are Underflowed; Drained and the outstanding-over-drained order with the rendered line and a cut buffer; the file oracle (held for append and for read, another file not held, released, missing path Unavailable); the range oracle (Present, Partial after splitting, Absent, Unavailable) |
| crd-vm-tests | `tests/foundation/vm/test_diag_lifetime_vm.cpp` | 2: a reservation is one region of its rounded size, unchanged by commit/decommit, gone on release, with the address space agreeing; a failed reserve and empty releases record nothing |
| crd-jobs-tests | `tests/foundation/jobs/test_diag_lifetime_jobs.cpp` | 3: `init` with 4+2+1 fibers holds exactly seven regions of the stacks' sizes and `shutdown` returns to Balanced; the observer (install, replace, clear, clear again); 32 jobs on 4 threads recording 64,000 acquire/release pairs into one ledger balance exactly |
| crd-platform-tests | `tests/foundation/platform/test_diag_lifetime_platform.cpp` | 3: two library references, a move, a move-assignment over a held one; a failed open; watcher add/remove, repeated remove, move, move-assignment over two entries, destruction |
| crd-log-tests | `tests/foundation/log/test_diag_lifetime_log.cpp` | 3: a file sink holds one handle and the OS agrees, released and the OS agrees; a rotation reopens without leaving the rotated file held; installed sinks through `clear_sinks` and `shutdown`, a null sink refused |
| crd-app-tests | `tests/foundation/app/test_diag_lifetime_event_bus.cpp` | 1: subscribe, repeated and unknown unsubscribe, move, move-assignment over two, destruction; the bus is not copyable |
| crd-resources-tests | `tests/assets/resources/test_diag_lifetime_reload_subs.cpp` | 1: three subscriptions, a repeated and an unknown unsubscribe, two ended by the manager's destruction |
| crd-gpu-context-tests | `tests/gpu/gpu-context/test_diag_lifetime_device_objects.cpp` | 2: the ledger's DeviceObject change equals the registry's `live_count` change through Resource, Program and Pass mints and retires (stale and invalid retires included); a registry of one's own feeds nothing |

## Teeth

Ten deliberate breaks on linux-gcc-debug, each applied by script, the target rebuilt, its `[lifetime]` cases run and
seen failing, the file restored byte for byte (mtime bumped) and every touched program rebuilt and rerun afterwards:

| Break | Result |
| --- | --- |
| `released` without the underflow check (the live count wraps) | core: the unmatched-release case failed (3 assertions) |
| `FileSink` records no acquisition | log: the sink and rotation cases failed (the OS still saw the file held) |
| the registry feeds only Pass identities | gpu-context: the agreement case failed (ledger and `live_count` disagree) |
| a freed fiber stack is not recorded | jobs: shutdown left the window Outstanding |
| `EventBus::unsubscribe` records no release | app: the subscription case failed (3 assertions) |
| the `FileWatcher` destructor drops its entries unrecorded | platform: the watcher case failed |
| `vm::release` records no release | vm: the reservation case failed |
| the `ResourceManager` destructor releases nothing | resources: the subscription case failed |
| `clear_sinks` records no release | log: the sink-registration case failed (3 assertions) |
| the Linux descriptor scan never matches (inode 0) | core: the file-oracle case failed (Present expected twice) |

The first attempt at the last break compared a field with itself, which gcc's `-Werror` refused; the run then used the
previous tooth's stale binary. It was rewritten warning-free and rerun, and every lane was rebuilt afterwards.

One Windows-only break, in a separate checkout (below): the handle-table walk no longer skipping the probe's own
handle made the core file-oracle case fail (`Absent` expected after close, `Present` seen); restored byte for byte,
rebuilt, `[lifetime]` passed (82 assertions in 5 cases).

## Verification

Windows needed a second checkout. Every configure of `D:/Dev/cerid` runs the project-sync preconfigure hook, which asks
the attached Visual Studio (registered native build `build/win-vs-debug`) over COM whether buffers are saved; Visual
Studio had a "Keep Visual Studio updated" dialog open, its main window disabled, and rejected every call
(`RPC_E_CALL_REJECTED`, five attempts). The dialog was left alone and no synchronization state was touched. A sibling
worktree (`D:/Dev/cerid-lt`, the batch's diff applied, files compared identical) has no registered native build, so it
configured without contacting the IDE, offline from the pinned archives already present in `build/win-debug/_deps`
(`CRD_INPUT_ARCHIVES`, each SHA-256 verified against `cmake/pins.json`). It was removed after the runs.

Whole programs, final source (core, vm, jobs, platform, log, app, gpu-context, resources):

| Lane | Result |
| --- | --- |
| win-debug (worktree; whole tree built before the tidy fixes, then crd-core and the eight programs rebuilt) | 56 / 13 / 181 / 52 / 31 / 7 / 28 / 129 cases pass |
| win-shipping (worktree) | 48 / 13 / 173 / 52 / 29 / 7 / 28 / 129 pass |
| win-clang-cl-shipping (worktree; linked first time) | 48 / 13 / 173 / 52 / 29 / 7 / 28 / 129 pass |
| win-asan (worktree, inside vcvars) | 56 / 13 / 181 / 52 / 31 / 7 / 28 / 129 pass |
| linux-gcc-debug (whole tree built) | 48 / 13 / 180 / 52 / 31 / 7 / 28 / 129 pass |
| linux-gcc-asan, `[lifetime]` repeated 3 times | 48 / 13 / 180 / 52 / 31 / 7 / 28 / 129 pass |
| linux-clang-tsan (`setarch -R`), `[lifetime]` repeated 10 times, then 3 more on the final source | 48 / 13 / 180 / 52 / 31 / 7 / 28 / 129 pass |

- The only sanitizer reports are the specimens' deliberate ones: 12 from the crash-capture specimen in crd-core-tests
  on linux-gcc-asan, one data race from `data_race_specimen.cpp` in crd-jobs-tests on linux-clang-tsan. None in any
  `[lifetime]` run.
- Strict tidy (LLVM 20.1.8, the worktree's win-debug compile database) is clean on all 25 changed C++ files after these
  fixes: local-constant names in two files, an integer-to-pointer cast in the VirtualQuery walk (now a pointer cursor),
  and a pre-existing `use-default-member-init` finding on `IdentityRegistry::m_slots`, which a default member
  initializer cannot satisfy (it is built from the constructor's allocator); it carries a reasoned `NOLINTNEXTLINE`. The
  Linux branches of `os_resource_probe.cpp` were compiled by gcc and clang with `-Werror` but not tidied.
- clang-format differs only in the repository's hand alignment, case-label indentation and one-line function style.
- From the main tree's `build/win-debug` (no reconfigure needed): the 8 guards, `crd-build-profiles`,
  `crd-diag-seeded-faults`, `crd-diag-coverage`, `crd-master-plan`, `crd-repository-hygiene` and
  `crd-repository-tools` pass (14 of 14). The Allman check, `diag-coverage.py --validate`, `test-diag-coverage.py`,
  check-master-plan and check-repository pass. No script or workflow changed.
- Coverage (linux-clang-coverage on WSL, clang 18): the registry gains the scope group "lifetime ledger and its OS
  oracle" (`lifetime_ledger.cpp` and `.hpp`, `os_resource_probe.cpp`) and the outcome sets LifetimeVerdict 4 of 4 and
  OsPresence 4 of 4. The first measurement reported GAP: `os_resource_probe.hpp` declares only and is not
  instrumented, so it left the scope (its enum is still read from it). The same export then passes: 77 of 77 scope
  files instrumented, 933 CTests all passed, verdict PASS; regions 10,117/11,333, branches 6,634/8,414, functions
  1,140/1,192, lines 18,690/21,425 (the new group: regions 224/241, branches 154/186). Profiles deleted.
- The full win-debug CTest was not run.

## Follow-ups

- The owners not yet in the ledger (the window, the WASAPI device's COM objects and event, worker and logger threads,
  native device objects without a Cerid identity) come with the shutdown workloads that exercise them.
- The Windows file oracle calls `GetFileType` on every File-type handle of the process; a synchronous pipe with a read
  pending on another thread could stall it. Filtering by granted access, or querying with a bounded worker, would make
  it safe outside quiet processes.
- Tidy the Linux branches of `os_resource_probe.cpp` with a Linux compile database (WSL clang-tidy 20).
- Every configure of the main checkout stops while Visual Studio shows a modal dialog, because the preconfigure hook
  must reach the attached IDE. The refusal is by design; its message could name the likely cause (a modal dialog in
  the IDE) instead of the raw `RPC_E_CALL_REJECTED`.
