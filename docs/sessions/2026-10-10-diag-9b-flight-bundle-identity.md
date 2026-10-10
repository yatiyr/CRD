# DIAG.9b build and symbol identity in a flight bundle, 2026-10-10

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.9b](../ROADMAP.md#slice-diag.9b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-9b). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.9b assets, commands, doctor](2026-10-10-diag-9b-assets-commands-doctor.md).

## Goal

The fifth slice of DIAG.9b: a flight bundle names the build and the binaries that wrote it. Before this batch a
flight bundle held a Manifest and the FlightRecord only, so its replay recipe and events could not be tied to a build
or symbolized. Still open after this batch: injection during cook, install, execute and teardown, refusal of arbitrary
external I/O replay as a shipping check, a real host (crd-sandbox), and the hosted run. This batch is committed locally
and not pushed.

## What was there (checked before coding)

- The SymbolIndex section (tag 5, DIAG.5d) and `ModuleIdentity` already existed, but were only ever filled from a
  minidump (`parse_minidump_modules`). There was no live loaded-module enumeration. The Linux build-id capture in
  `crash.cpp` is private and constrained by the async-signal handler; it was left untouched.
- ceir-cook's `ReplayBuild`/`same_build` already name a record's build (version, platform, compiler, arch, config,
  asserts), but crd-perf cannot depend on ceir-cook, and it carries no sanitizer, profiling or fault-seam bits.
- The fatal path (`finish(Fatal)` from the assert or fatal-invariant trigger) calls the sink, which writes the bundle.
  Enumerating modules there would take the loader lock (Toolhelp, `dl_iterate_phdr`) on a dying process, possibly
  inside a loader callback, so it must not happen on that path.
- `import_bundle` set `has_symbols` for an explicitly absent SymbolIndex section (an empty payload), so "omitted on
  purpose" read as "present with no modules".

## What changed

- **Build identity** (`crd/perf/build_identity.hpp`, new; bundle section `BuildIdentity`, tag 7):
  `current_build_identity()` reads only compile-time constants (safe on a fatal path): the diagnostic schema,
  `CRD_VERSION_STRING`, platform, compiler and its version (`msvc <_MSC_FULL_VER>`, `clang X.Y.Z`, `gcc X.Y.Z`),
  arch, `debug`/`release`, and a closed flag set (asserts, profiling, fault seams, ASan, TSan). The payload ('CBI1',
  version 1) is six u16-length fields of at most 64 bytes; the decoder refuses a wrong magic or version, unknown
  flags, an over-long or short field and trailing bytes. `same_build_identity(a, b, differing)` names every differing
  field in a fixed order.
- **Live module enumeration** (`enumerate_loaded_modules` in `crd/perf/symbol_index.hpp`, new
  `src/module_enumeration.cpp`): on Windows a Toolhelp module snapshot, each module pinned with
  `GetModuleHandleExW(FROM_ADDRESS)` while its in-memory PE headers are read and bounds-checked against SizeOfImage:
  base, SizeOfImage, CheckSum, TimeDateStamp and the CodeView RSDS GUID and age (kind None without one). On Linux
  `dl_iterate_phdr` into preallocated slots (nothing allocates under the loader lock): base and PT_LOAD span and the
  NT_GNU_BUILD_ID note. Only basenames are kept (module and PDB), so no directory of the machine leaves in a bundle.
  At most 1024 modules (`kMaxLiveModules`), with an overflow flag.
- **Module snapshot** (`merge_module_snapshot`): a module no longer loaded stays, marked unloaded (the DIAG.5d
  "retain unloaded generations" rule for a flight bundle); a module loaded again is a new generation. Bounded at 1024.
- **Flight bundle**: `write_flight_bundle(capture, modules = {})` writes the Manifest, the FlightRecord, the
  BuildIdentity and a SymbolIndex of `modules`; with no modules the SymbolIndex is an explicit absent section the
  manifest declares. It never enumerates. `FlightCommandHost` keeps the snapshot (`refresh_modules`, `modules`,
  `modules_overflow`); `flight.write` refreshes it first (a request, never a fatal path) and reports `modules` and
  `modules_overflow`. The fatal specimen takes its snapshot at startup and writes it from the fatal path.
- **Import and inspect**: `import_bundle` decodes tag 7 into `build` (`has_build`, or `build_refused` for a payload its
  decoder refuses) and no longer counts an explicit absent SymbolIndex as `has_symbols`. `bundle.inspect` names the
  `build-identity` section and adds a `build` item with every field plus `same_as_host` and `differing`, comparing the
  bundle's build with the host's own. win-clang-cl-shipping's `-Wswitch` makes a missed tag a build error (observed).
- **Lint in touched headers**: strict tidy flagged two old in-memory status enums (`ImportStatus`, `ImportReject`) now
  `u8`, and the pinned on-disk `SymbolRecord::_pad_b`, which carries a NOLINT with its reason like `_pad_a`.
- **Fuzz**: the bundle importer's oracle now also requires a decoded build identity to stay inside its bounds, never be
  reported refused at the same time, and be deterministic. Two seeds were added (a fixed BuildIdentity bundle, and one
  with a trailing byte); `--seed` regenerated the ten existing seeds byte-identical, twice. The corpus is 12 inputs.

## Tests

New `tests/foundation/perf/test_diag_flight_identity.cpp` (8 cases on Windows and 7 on Linux, `[diag9b][identity]`) and the
test-local oracle `module_file_identity.hpp`, which reads a module's identity from its file on disk (PE through its
section table, ELF through its program headers' file offsets) and shares no code with the live walk.

- This build's identity matches its own macros; its fault-seam flag matches an independent probe (whether
  `install_io_fault_hook` installs), so a shipping lane shows `false`.
- The section round-trips; no strict prefix decodes; wrong magic, version, an unknown flag, an over-long field and a
  trailing byte are each refused with their reason; an over-long field is clipped on encode.
- Each of the 12 fields and flags, changed alone, is the one named by `same_build_identity`; two together are named in
  order.
- The live enumeration lists this executable first, with the identity its file carries (RSDS and PDB name on Windows,
  build-id on Linux), and every name is a basename.
- Windows: every module loaded before and after `crash::capture_dump` matches the OS-written minidump of the same
  process read by the DIAG.5d parser (size, stamp, checksum, GUID, age, names compared without case: the loader keeps
  the case a module was loaded by, `OLEAUT32.dll`, where the dump reads the mapped file, `oleaut32.dll`).
- Windows: a system DLL loaded through `FlightCommandHost::refresh_modules` appears, and once freed stays in the
  snapshot marked unloaded with the same identity; a bundle written then names it, and `identity_matches` accepts it.
- Linux: each module with a build-id whose file `/proc/self/maps` maps at its base matches that file (5 modules on
  WSL, the executable and libc among them).
- The merge keeps unloaded generations, appends a reload as a new generation, is stable, and stops at 1024 with the
  overflow flag.
- A flight bundle carries this build and the snapshot, symbol for symbol; without a snapshot the SymbolIndex is
  absent, declared, `has_symbols` false and no manifest mismatch; a corrupt BuildIdentity is reported refused.

Extended: the fatal-specimen case now requires the bundle written on the fatal path to carry this lane's build and the
specimen's own executable with the identity its file carries; the `bundle.inspect` case counts 4 sections plus the
build item and requires `same_as_host`; the `flight.write` case requires a build and a snapshot as large as the host's.

### Teeth

Each break was applied with a marked edit, the lane rebuilt and the identity, flight and command cases run:

| Break | Lane | Result |
| --- | --- | --- |
| The decoder accepts trailing bytes | win-debug | the refusal case and the corrupt-bundle check fail |
| `same_build_identity` ignores `config` | win-debug | the comparison case fails (`config` and `config,tsan`) |
| The live walk adds 1 to each RSDS age | win-debug | the file check, 27 minidump modules and the fatal specimen fail |
| An empty snapshot is not declared absent | win-debug | the absent-declaration check fails |
| `bundle.inspect` misses the tag | win-debug, win-clang-cl-shipping | the inspect case fails; clang-cl stops with `-Wswitch` |
| The merge never marks a module unloaded | win-debug | the merge case and the freed-DLL case fail |
| The importer counts an absent SymbolIndex | win-debug | the absent-snapshot check fails |
| The Linux walk flips a build-id byte | linux-gcc-debug | the executable case, all 5 mapped modules and the fatal specimen fail |

A first Linux break (the PT_LOAD span without subtracting the lowest address) could not bite: for PIE modules the
lowest PT_LOAD address is 0, so it is the same value. The build-id flip replaced it. After each restore the lane was
rebuilt and passed; win-debug was then rebuilt as a whole tree.

## Verification

- win-debug: the whole tree built; crd-perf-tests 254 cases, crd-perf-ui-tests 21, crd-sandbox-inspect-tests 24,
  crd-ceridc-tests 20 (from a scratch directory with `CRD_CERIDC_EXE`), crd-resources-tests `[diag9b]` 2, the DX12
  `[device-loss]` case and the Vulkan `[device-loss]` cases (6) on this machine's GPU, and the 12-input corpus replay.
- win-shipping and win-clang-cl-shipping (reconfigured; every executable including a changed header rebuilt): the same
  suites green, crd-perf-tests 121 cases, the identity cases with the fault-seam flag false.
- win-asan (inside vcvars, reconfigured): the same suites green, crd-perf-tests 254 cases.
- WSL, one lane at a time, each reconfigured: linux-gcc-debug crd-perf-tests 252 cases, ceridc 20, resources 2, the
  Vulkan `[device-loss]` cases on lavapipe, perf-ui 21, sandbox-inspect 23 and the corpus replay; linux-gcc-asan 252
  cases (the only sanitizer reports are the deliberate specimens'); linux-clang-tsan (`setarch -R`) the identity,
  `[diag9b]`, flight, command, symbol, import and bundle cases (79) with no TSan report.
- Strict tidy clean on all 18 changed C++ files. clang-format differs only on hand alignment and include grouping (the
  Windows headers must stay in their order). The Allman check, the 8 ctest guards, `check-master-plan` and
  `check-repository` pass.

## What remains in DIAG.9b

- Injection during cook, install, execute and teardown with the previous assets surviving.
- Refusal of arbitrary external I/O replay as a shipping capability check.
- A real host (crd-sandbox) installing the recorder, its triggers, a `FlightCommandHost` sink (with
  `refresh_modules` at start and after module loads) and both command sets.
- The hosted run: in addition to the earlier list, the `[identity]` cases green on all six lanes (the Windows minidump
  and freed-DLL cases on the hosted Windows runners, the `/proc/self/maps` case on the hosted Linux runners) and the
  12-input corpus replay.
