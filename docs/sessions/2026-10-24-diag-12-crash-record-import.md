# DIAG.12: an engine import route for the Linux crash record, 2026-10-24

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.12](../ROADMAP.md#slice-diag.12). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-12),
> [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md). Rules: [AGENTS](../../AGENTS.md).
> Previous batch: [the application's crash](2026-10-24-diag-12-application-crash.md).

## CI read at the start

`gh run list --branch main --limit 15`: no completed run has a failed job. Every completed push run since
40099751439 was cancelled by the next push with no failed job; 40103197370 (head `6b214076`) is in progress with 18
jobs green and 7 running, and 40110939655 (head `05a94f43`) is pending. Neither was touched.

## Change

The previous batch's follow-up: the Linux crash record had no engine importer, so the crash case read its lines
itself. Now:

- `crd::crash` (`crash.cpp`) records each module's base and size as the PT_LOAD span, the identity
  `enumerate_loaded_modules` reports: `module <base> <size> <build-id or -> <basename>`. Before, the base was the
  load bias and there was no size, so nothing could place an address in a module from the record alone.
- `crd::perf::parse_crash_record_modules` (declared in `symbol_index.hpp`, defined in `module_enumeration.cpp` beside
  the live walk so it reuses `basename_of_path`) reads those lines into `ModuleIdentity`s. It compiles on every lane
  and treats the record as untrusted: only newline-terminated lines count and a malformed module line ends the
  parse. `(exe)` takes the basename of the `exe` line, so no directory enters the index. There is no
  `build_symbol_index_from_crash_record`: the one caller passes the result to `serialize_symbol_index`.
- The crash case in `test_diag_host.cpp` now runs one import route on both platforms: the section (from the dump or
  the record) goes through `BundleWriter` and `import_bundle`, the application's module is found by name, compared
  with its file (`same_identity`, `identity_matches`), and the fault address (the dump's exception address, the
  record's `rip`) must lie in it. The test's own record reader and hex encoder are gone; it still reads `rip`
  itself, as the Windows branch reads the ExceptionStream through the test probe.
- `test_diag_build_id.cpp`'s module-line walker skips the new size column.

The record's `exe` line still holds the full path on disk; that is DIAG.12's privacy item, unchanged here.

## Teeth

Each break was built, run, restored, rebuilt and rerun (`build/diag12d/teeth.py`, not committed).

| Lane | Break | Result |
|---|---|---|
| win-debug, linux-gcc-debug | the parser sets every size to 0 | unit case fails `size == 0x2a000`; on Linux the crash case fails `same_identity` and `in_module` |
| win-debug | the parser keeps an unterminated last line | fails: 3 modules, not 2 |
| win-debug, linux-gcc-debug | `(exe)` keeps its placeholder name | unit case fails the name; the crash case fails `app != nullptr` |
| linux-gcc-debug | the record writes size 0 | the crash case fails `same_identity` and `in_module` |

## Verification

- win-debug: `crd-diag-host-specimen`, `crd-perf-tests`, `crd-core-tests` built; the parser, minidump, SymbolIndex and
  crash cases pass.
- `wsl-lane.sh build`/`test` on linux-gcc-debug and linux-clang-tsan: 40 selected CTests pass, among them the crash
  case, `crash record (linux): carries the exe ELF build-id`, `crash capture (linux)` and `live modules`.
- `tidy-files.py` clean on the five C++ files. The Linux branch of `crash.cpp` is compiled `-Werror` by GCC and
  Clang but is not analysed until clang-tidy 20 is on the WSL host (DIAG.11b (o), the user's).
- `clang-format --dry-run`: the added lines' findings are the surrounding hand alignment only; no added line is over
  120 columns. `check-allman-braces.py`, the ctest guards, `check-master-plan.py`, `check-repository.py`.

Hosted once pushed: the parser case passes on every lane; the crash case passes on every lane, the record branch on
the Linux lanes.

## Remaining in DIAG.12

The application's authored-program error and GPU hazard, faulty and repaired; privacy across every class; removal of
obsolete scaffolding and duplicated services; the re-read of every child and DG01-DG18 against final source and
published CI.

Follow-up for an extra batch: `crash.cpp`'s `extract_build_id` and `module_enumeration.cpp`'s `note_build_id` are the
same PT_NOTE walk in crd-core and crd-perf, and both TUs compute the PT_LOAD span; the first duplicated service for
DIAG.12 to fold into one.
