# DIAG.12: the application's crash, faulty then repaired, 2026-10-24

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.12](../ROADMAP.md#slice-diag.12). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-12),
> [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md). Rules: [AGENTS](../../AGENTS.md).
> Previous batch: [one application, faulty then repaired](2026-10-24-diag-12-same-application.md).

## CI read at the start

`gh run list --branch main --limit 15`: no completed run has a failed job. Every completed push run since
40089047314 was cancelled by the next push with no failed job; 40103197370 (head `6b214076`) is in progress, its 18
finished jobs green, and 40108992548 (head `8948421c`) is pending. Neither was touched. DIAG.11c's portable-boundary
cases still need a completed run at or after `85fee881`.

## Change

`crd-diag-host-specimen` takes a sixth mode, `crash`: the faulty run writes an order through a lookup that found
nothing (a volatile null write, as `crash_capture_specimen` does); the repaired run does not. The crash handler the host
installed from the profile is the detector, so nothing else in the application changed. Under ThreadSanitizer the
specimen defaults `handle_segv=0`, as `crash_capture_specimen` does, so that handler receives the fault instead of
TSan's (exit 66).

The new `[diag12]` case in `test_diag_host.cpp` checks the faulty run:
- no bundle: a hard crash leaves only the crash capture (the design's "fatal path" rule);
- exactly one `crash_*` capture in the profile's crash directory;
- Windows: exit `0xC0000005`; the dump goes through the engine's import route (`build_symbol_index_from_minidump`,
  a `SymbolIndex` bundle section, `import_bundle`). The application's module comes back with the identity of its file
  (`same_identity`, and `identity_matches` when the build has a PDB), and the ExceptionStream's faulting instruction
  lies inside that module;
- Linux: exit 139; the record's `(exe)` module line carries the build id of the file, and its `rip` lies in the
  executable's image (base plus the file's PT_LOAD span; every Linux preset links a position-independent executable,
  the toolchains' default, so that span starts at 0). On a build without a PDB (win-release) both identities are
  `None` and `same_identity` still compares the image size, stamp and checksum.

Under ASan the sanitizer owns the fault: the case asserts a non-zero exit and, on Windows, no dump, the same branch
as crash capture's own wild write. `check_repaired_run` (exit 0, Clean, no bundle, empty crash directory) covers the
repaired arm. The probe header `tests/foundation/core/minidump_probe.hpp` is included by relative path, so there is no
second ExceptionStream reader. The case is not tagged `[diag-workflow]`, because the per-lane workflow already counts
a wild-write capture.

## Teeth

Each break below was built, run, restored, rebuilt and rerun (`build/diag12c/teeth.py` and `teeth_linux.py`, not
committed).

| Lane | Break | Result |
|---|---|---|
| win-debug | faulty arm loses the write | fails: not Crashed, no capture |
| win-debug | repaired arm keeps the write | fails: not Clean, a dump written |
| win-debug | the write goes through `std::memset` (faults in the C runtime) | fails: `in_module(ExceptionAddress)` |
| linux-gcc-debug | faulty arm loses the write | fails: not Crashed, no record |
| linux-gcc-debug | the write goes through `std::memset` (faults in libc) | fails: `rip - exe_base < size` |
| linux-clang-tsan | no `handle_segv=0` default | fails: `ThreadSanitizer: SEGV`, exit 66 |

## Verification

- win-debug: `crd-diag-host-specimen` and `crd-perf-tests` built; all 22 `diagnostic host` CTests pass (31
  assertions in the crash case).
- `wsl-lane.sh build`/`test`: the same 22 pass on linux-gcc-debug, linux-clang-tsan and linux-gcc-asan.
- `tidy-files.py` is clean on both files. That covers the Windows branch only: the Linux record reader is compiled
  `-Werror` by GCC and Clang but is not analysed by clang-tidy until clang-tidy 20 is on the WSL host (DIAG.11b (o),
  the user's). `clang-format --dry-run`: on the added lines the only findings are the file's own declaration
  alignment. No added line is over 120 columns. `check-allman-braces.py` passes, and so do the ctest guards,
  `check-master-plan.py` and `check-repository.py`.

To show once pushed: the crash case passes on every lane, with the dump branch on the Windows lanes, the record
branch on the Linux lanes and the sanitizer branch on win-asan and linux-gcc-asan.

## Remaining in DIAG.12

The application's authored-program error (CEIR) and GPU hazard, faulty and repaired. Then privacy across every class:
a dump or record holds the full executable path and raw memory, and the design never declares a raw dump redacted.
After that: removal of obsolete scaffolding and duplicated services, and the re-read of every child and DG01-DG18
against final source and published CI. The Linux record has no engine importer yet (`crash.cpp`: "a later composer");
the case reads its three lines itself.

Follow-up for an extra batch: give the Linux crash record an engine import route in crd-perf, as the dump has: the
`(exe)` and module lines become a `SymbolIndex` section, used by this case instead of the test's own reader.
