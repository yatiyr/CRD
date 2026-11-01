# 2026-11-01 — OWN.1: the bundle's files on crd-platform, and the window backend split

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Rows: [OWN.1](../ROADMAP.md#slice-own.1)'s last item after the
[bundle's compressed sections](2026-11-01-own-bundle-compressed-sections.md), which closes
[OWN.2](../ROADMAP.md#slice-own.2) with it. Built, tested and measured on `cerid-track-b`. The contracts are
[capability ownership](../design/capability-ownership.md#the-bundles-files-on-crd-platform) (decision 9),
[ADR-0006's amendment](../decisions/0006-platform-v1.md#amendment-2026-11-01-the-window-backend-is-its-own-module) and
the [relocatable package](../design/public-consumption.md#the-diagnostic-family-diag11a).

## CI first

- Run 41254049879 (31404ec4, through batch 6) finished three jobs red. linux-gcc-shipping and linux-gcc-release stop
  at the link of crd-compression-tests: `-Wnull-dereference` at `test_deflate.cpp:755-756`, the zlib header case
  writing its two header bytes by index into a copy GCC's link-time pass sees could be empty (batch 5's test). This
  batch fixed it (the replacement header pushed, then the stream's tail), proved it on a linux-gcc-shipping tree on
  the machine (the test links; `[deflate]` and the bundle suite pass there), and found the same fix already on main as
  95ec6965 when it fetched; it kept main's commit and dropped its own.
- win-tidy fails on `tests/foundation/platform/test_arguments.cpp` (`modernize-raw-string-literal`,
  `bugprone-suspicious-missing-comma`): COMPLY.1's arguments test, track C's, fixed on main by a40af1bf.
- Every other finished job of that run passes. Batch 7 and 8's commits (the ZIP container, the bundle's compressed
  sections) have no complete hosted run yet; the shipping tree here linked crd-perf-tests and the hesap-tensor suites
  with them, and the resources, ceridc and cooker links were left to CI when the tree's avx512 FFT codelets ran past
  half an hour.

## Design

### Why the window backend had to move

crd-perf could not link crd-platform: crd-perf ships in the relocatable package, and crd-platform linked GLFW, which
the package does not export. Exporting crd-platform as it was would have failed the install (a packaged target linking
a target outside the export set); packaging GLFW behind it would have shipped a window library to every diagnostic
consumer. Only two of crd-platform's sources name GLFW: `context.cpp` (the GLFW runtime, `PlatformContext`) and
`window.cpp` (`Window`). They, their headers and the `platform.hpp` umbrella that includes them moved into a new
module, crd-platform-window (`engine/foundation/platform-window`), which links GLFW and crd-platform. The input
snapshot types (`input.hpp`) feed the window but name no backend, so they stay. Include paths and the `crd::platform`
namespace are unchanged, so no consumer's source changed: the app module, the runtime's `smoke_window`, the
geometry-bvh-gpu and shader-helpers tests (which ask `headless_requested()`) link crd-platform-window, and the window
tests moved with it into `crd-platform-window-tests`. crd-platform names `${CMAKE_DL_LIBS}` itself now: GLFW used to
bring `dlopen`'s library for `DynamicLibrary`. The registry gains `platform.windowing` (signature: the GLFW runtime
and window calls), so a second window backend cannot appear unnoticed.

### The bundle's files

All four of crd-perf's file operations were hand-written: the publish, the retention walk and the two existence
checks. Each is now one call into crd-platform:

| Function | Before | After |
|---|---|---|
| `write_bundle_atomic` | `fopen`, `fsync`/`FlushFileBuffers`, `rename`/`MoveFileExA` | `fs::write_file_atomic` |
| `enforce_bundle_retention` | `FindFirstFileA`/`opendir`, `stat`, `std::remove` | `fs::list_directory`, `is_file`, `last_modified_unix_ns`, `remove_file` |
| `bundle_directory_exists` | `GetFileAttributesA`/`stat` | `fs::is_directory` |
| `bundle_path_exists` | `GetFileAttributesA`/`stat` | `fs::exists` |

What the move changes, besides deleting the copies:

- Paths are UTF-8 on Windows. The `A` calls read the ANSI code page, so a capture directory under a Turkish user name
  could not be published to. crd-platform's calls go through the wide API.
- The publish is durable on POSIX too: crd-platform syncs the directory after the rename, which the hand-written
  publish never did, so a power loss after it returned could lose the rename.
- A null or empty path is refused before any file is touched; the old publish wrote, then removed, a file named
  `.tmp` for an empty one.

Retention orders by the last write time to the file system's resolution, as both old branches did (the Windows
`FILETIME`, the POSIX `st_mtim`). crd-platform's only reading was `last_modified_unix_seconds`, which ties every
bundle written within one second, so it gains `last_modified_unix_ns`. Writing it exposed a defect in the seconds
reading: it converted with `file_clock::now()` and `system_clock::now()`, two calls microseconds apart, so the same
file could read as two different seconds across a second boundary; a file watcher compares those readings. Both now
convert with `clock_cast`, which is exact. The nanosecond reading saturates outside 1677 to 2262: the Microsoft
library's file clock (100 ns ticks from 1601) reaches past it, and the earlier bundle test set its times near 1601.

### The registry

`io.durable-saves` has no pending file left, and `io.files` gains a signature for hand-written directory walks and
attribute reads (`FindFirstFile`, `GetFileAttributes`, `opendir`). Its two other matches are allowed with reasons:
the crash handler's install in crd-core (below crd-platform, and its crash-time writer uses only calls safe in a
crashed process) and crd-core's OS resource probe (it counts `/proc/self/fd`, a process probe). The registry now lists
76 capabilities with no file pending a move onto its owner.

### Found on the way

- `tests/geometry/geometry-bvh-gpu`: the morton sort's u64 oracle and determinism cases, and the parallel sort's
  u64 case, compared pair arrays with `memcmp`. `MortonPair<u64>` carries four bytes of padding after its index,
  which neither the sort nor the oracle writes, so the comparison read whatever the allocator's block held before.
  In the full suite run here the three cases failed; alone they passed. They now compare codes and indices.
- The cryptography inventory (COMPLY.1): five tools' closures no longer reach GLFW. The inventory was regenerated, and
  each tool's head classification was re-reviewed on 2026-11-01 (`gen-crypto-inventory.py --review`). Their findings
  hold unchanged: GLFW performs no cryptographic function, and nothing was added.

## Evidence

On `cerid-track-b` (Xeon with AVX-512, 8 vCPUs, Ubuntu 24.04, GCC 13.3):

- linux-gcc-debug: the whole tree built (`cmake --build -- -k 0`, every target, `CRD_BUILD_SANDBOX=ON`), so every
  consumer of crd-platform links without GLFW unless it names crd-platform-window (or GLFW itself, as imgui and the
  sandbox do).
- The guards (`ctest -R "^crd-check|^crd-no-|^crd-diag-coverage$|^crd-diag-seeded-faults$|^crd-comply"`, 22 tests)
  pass. The first run caught three things, each fixed: the new test directory was not registered
  (`crd-check-source-routes`), the UTF-8 bundle test held `std::u8string` copies (`crd-no-std-containers-check`), and
  the five tools' inventories (`crd-check-registers`, above).
- Suites, each from a scratch directory: crd-platform (64 cases, the exact-conversion case among them),
  crd-platform-window (9, under `CRD_PLATFORM_HEADLESS=1`), crd-perf (345, with the pinned validation layer on
  `VK_LAYER_PATH` and its library on `LD_LIBRARY_PATH`; without the latter the GPU diag-host case reports
  `no-vulkan-device`, an environment gap of this machine), crd-app, crd-compression, crd-resources,
  crd-geometry-shader-helpers, crd-geometry-bvh-gpu (75, after the padding fix) and crd-imgui: all pass.
- `scripts/test-package-consumer.py --preset linux-gcc-debug` passes: the installed layout is the family's 13
  archives (`libcrd-platform.a` new) and `libzstd.a`, no GLFW archive and no GLFW header; every example and the
  deliberate failure publish their bundles through `Cerid::platform` from the moved prefix and are inspected.
- linux-gcc-shipping (link-time `-Wnull-dereference` and the compile warning set at the link): crd-perf-tests,
  crd-platform-tests, crd-platform-window-tests and crd-compression-tests link, and crd-platform (64 cases),
  crd-platform-window (9), crd-compression (25) and the bundle suite (14) pass.
- The Python units of the changed scripts (`test-repository-tools.py`'s package consumer and ownership cases) pass;
  `check-capability-owners.py` reads 76 capabilities with none pending.

## Before/after board

`bench_compression` gains two cases: `bundle-publish` (the bundle of `bundle-write`, 52 803 bytes, published over
itself; its bits are the file read back) and `bundle-retain` (the retention walk over sixteen such bundles, all kept).
Two release builds ran through [ab-board.py](../../scripts/ab-board.py): the before arm is main at 11858569 with
this bench source, the after arm this batch. linux-gcc-release, GCC 13.3, CPU 3 pinned, one warm-up,
10 repetitions, band 2%, ns per KiB of the 256 KiB input. `--same` held on all 18 keys: both arms published the same
bytes and read the same results.

| case | before | after | speed ratio | verdict |
|---|---|---|---|---|
| bundle-publish | 13749 | 22120 | 0.6216 | slower |
| bundle-retain | 57.8 | 143.7 | 0.4021 | slower |
| bundle-write | 66113 | 68366 | 0.9666 | slower |
| deflate-raw, zlib-deflate, zip-write | | | 0.970 to 0.997 | slower or unresolved |
| every other codec case | | | 0.985 to 1.005 | level |

- **The publish is 2.1 ms dearer per bundle** (3.5 ms became 5.7 ms) because it is now durable on POSIX: the
  directory is synced after the rename, a second flush to the machine's persistent disk. The hand-written publish
  skipped it, so a power loss right after a capture could lose the bundle's name. A capture writes one bundle.
- **The retention walk costs 22 µs more** per walk of sixteen bundles (15 µs became 37 µs): crd-platform's listing
  converts each name to a `Path` and reads the file's type and time with two `stat` calls where the hand-written loop
  made one. The walk runs once per capture, after the publish.
- **The codec cases moved, not the codecs.** `deflate.cpp` is the same in both arms; the encoder's cases (and
  `bundle-write`, which runs it) moved 0.3% to 3.4%, the code-placement shift the earlier boards measured between
  +2.6% and -9% for the unchanged encoder from one build pair to the next
  ([one ZIP container](2026-11-01-own-one-zip-container.md#beforeafter-board)).

## Rows

OWN.1's last item is done, every audited duplicate is moved and the registry's pending list is empty, so OWN.1, OWN.2
(it closes when OWN.1 is Done with nothing pending) and the OWN parent (the guard runs in hosted CI on both repository
check hosts, run 41254049879) are Done. They sit after SIMD-PERF, the optimization track's unfinished row, so they
carry the user's order ("I want ... own rows to be done!",
[parallel tracks](2026-10-31-parallel-tracks.md#user-order)) as their `user-order` marker.
