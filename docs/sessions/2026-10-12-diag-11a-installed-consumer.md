# DIAG.11a the installed external consumer: a packaged diagnostic family, its symbols and assets, 2026-10-12

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11a](../ROADMAP.md#slice-diag.11a). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-11a) and
> [public consumption](../design/public-consumption.md#the-diagnostic-family-diag11a). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.11a the reusable diagnostic host](2026-10-12-diag-11a-reusable-diagnostic-host.md).

## Goal

DIAG.11a's second slice: a thin external application consumes the installed engine, runs, fails on purpose, emits an
attributable bundle and is investigated without editing engine source; symbols and policies are packaged on purpose;
the consumption is tested on a moved prefix with nothing of the engine's build reachable; the profiling-disabled,
optimized-profiling and diagnostic binaries are shown for it. Only `Cerid::core` was packaged, and `bundle.inspect`
listed a flight record's size without decoding it. Committed locally, not pushed.

## What landed

| Piece | Where | What it does |
| --- | --- | --- |
| Packaged family | `cmake/CrdPackage.cmake`; the CMakeLists of core's ten neighbours | `Cerid::units`, `time`, `vm`, `log`, `containers`, `memory`, `jobs`, `toml`, `math`, `perf` and the interface target `simd-flags` join `Cerid::core`; every include directory is `$<BUILD_INTERFACE:...>` |
| Include-only edges | `crd_package_library(... INCLUDE_ONLY ...)` | units needs math's headers, containers needs log's; an install without the partner fails naming both |
| Symbols | `_crd_package_debug_information`, `Cerid_DEBUG_INFORMATION` | `pdb`: each archive's compile PDB installed beside it; `embedded`, `dwarf`, `none`, `not-installed` declared, never guessed |
| Assets | `crd_package_assets()`, `Cerid_ASSET_DIR`, `Cerid_ASSETS` | `assets/diag` installed under `share/cerid/diag`; the config file finds it relative to itself |
| Investigation route | `diag_commands.cpp` (`bundle.inspect`) | decodes the flight record: a trigger item, a flight item, one event item per retained event; `flight_record`, `flight_refused` and `flight_error` in the summary |
| Trigger path | `flight_recorder.cpp` (`copy_path_tail`) | a source path over 127 bytes keeps its tail after `...`, cut on a UTF-8 boundary |
| Invariant macro | `diagnostics.hpp` (`CRD_DIAG_INVARIANT`) | names `::crd::containers::StringView` and `::crd::u32`; a caller needs no `cont` alias |
| Thin application | `tests/support/package/diag-consumer/` (`CMakeLists.txt`, `main.cpp`) | `find_package(Cerid)`, `Cerid::perf` only; `fail <profile>` and `inspect <dir> <bundle>` |
| Proof | `scripts/test-package-consumer.py` (`crd-package-consumer` CTest) | packages the family, checks it, moves it, deletes the engine build tree, builds both consumers, runs the failure and the investigation |

Decisions:
- math is packaged because units' public `units_nonlinear.hpp` includes `crd/math/cmath.hpp`; a link edge would be a
  cycle (math links units), so the edge stays include-only and the package enforces it at install time. math brings
  `simd-flags`, which is correct for a consumer: the math headers must compile with the same ISA and the ADR-0063
  floating-point contract.
- The compile PDB is installed from CMake's default `/Fd` path for Ninja and Makefiles, so the build graph does not
  change. The linker looks for a missing compile PDB beside the archive, then in the output directory.
- The first symbol proof scanned the application's build output for `LNK4099`. Its tooth did not bite: the linker
  does print the warning (seen when linking by hand, in the machine's Turkish locale), but CMake's MSVC link step
  (`cmake -E vs_link_exe`) shows the linker's output only for a failing link. The proof now reads the program's own
  debug information for `trigger_replay_length`, a member of a type defined only in `flight_recorder.cpp`: the
  application's PDB names it only when the archives' PDBs were found (Windows), its DWARF only when the archives carry
  DWARF (Linux). A repository-tools case holds the name to that file and keeps it out of the public header.
- Deleting the engine's build tree before the consumers run is what makes a missing PDB visible: with the tree kept
  (and the layout check bypassed) the run passed, because the linker found every PDB at its build path. The hosted
  public-check lanes are the fresh machines; locally the deletion is the stand-in, recorded as such.
- The application's profile is authored by the test from the installed investigation profile (three keys replaced:
  the crash directory, the capture directory and the installed flight policy), so the installed assets are consumed,
  not only copied.
- The application keeps symbols in every configuration (`/DEBUG:FULL` and `/Zi`, or `-g`), as a shipped program does
  for its crash reports, so the Release consumers of the shipping profiles also prove the PDBs reach the link.

## Tests

| Program | Cases | What they hold the code to |
| --- | --- | --- |
| crd-perf-tests | 2 new `[diag11a][flight]`; 2 extended | the decoded trigger (file, line, message, scalars, sequence), flight item and six events on their sides of the trigger; a record the decoder refuses reported `flight_refused` with its reason and no trigger or event item; a 300-byte source path keeping `/consumer/src/orders.cpp` without splitting a two-byte letter; the specimen's bundle read through `bundle.inspect` (fatal-invariant trigger, `end` fatal, forty events before it, none after) |
| repository tools | 1 new, 1 extended (67 in all; 3 skips need CMake and Ninja on PATH and run in the `crd-repository-tools` CTest) | the layout check (one archive per module, PDBs as declared, assets byte for byte, headers of every module), the config record, the expected debug information per compiler and profile, the ELF section reader and the symbol probe on hand-built images, every mismatch the investigation names, and the probe names held to their sources |
| crd-package-consumer | the script | above, per profile |

## Teeth

Eleven breaks failed and one control passed by design. Each was applied by a binary replace from a byte-exact
backup and restored the same way with its mtime bumped; the touched lane was rebuilt and passed again.

| Break | Lane | Failed |
| --- | --- | --- |
| compile PDB not installed | win-debug | the layout check, naming eleven archives |
| the same, layout check bypassed | win-debug | the symbol probe (`trigger_replay_length` absent from the application's PDB) |
| the same, engine build tree kept | win-debug | nothing: the control that shows why the tree is deleted |
| archives' DWARF stripped after install | linux-gcc-debug | the symbol probe (no engine DWARF) |
| perf's include directory not wrapped | win-debug | the engine configure (CMake refuses to export a source path) |
| diagnostic assets not installed | win-debug | the config record (`Cerid_ASSETS` empty) |
| `CRD_DIAG_INVARIANT` naming `cont::StringView` again | win-debug | the application's compile (C2653) |
| units packaged without math (`CRD_MODULES=units`) | win-debug | the install, with the include-only refusal |
| `bundle.inspect` writing `value_a` as the line | win-debug | the decoded-trigger case |
| the trigger path clipped from the head again | win-debug | the long-path case |
| `flight_refused` always false | win-debug | the refused-record case |
| every event reported `before` | win-debug | the decoded-events case |

## Verification

| Lane | Result |
| --- | --- |
| win-debug | whole tree built; crd-perf-tests 287 cases, crd-ceridc-tests 22 (from a scratch folder with `CRD_CERIDC_EXE`), the perf bundle, perf flight and ceridc-diag fuzz corpora; package test PASS by preset and in the CTest argument form |
| win-shipping | crd-perf-tests 151, crd-ceridc-tests 22, the corpora; package test PASS (profiling not compiled, fault injection Unavailable) |
| win-shipping-profile | package test PASS (profiling compiled, fault injection Unavailable) |
| win-clang-cl-shipping | crd-perf-tests 151, crd-ceridc-tests 22, the corpora; linked first time |
| win-asan (inside vcvars) | crd-perf-tests 287, crd-ceridc-tests 22, the corpora; the only ASan reports are the three specimens that fault on purpose |
| linux-gcc-debug | crd-perf-tests 285, crd-ceridc-tests 22; `[diag11a],[flight]` repeated 3 times; package test PASS (`dwarf`) by preset and in the CTest argument form (the full `x86_64-linux-gnu-g++-13` path, the define list) |
| linux-gcc-shipping | package test PASS |
| linux-gcc-asan | crd-perf-tests 285, crd-ceridc-tests 22; repeated 3 times; the only reports are the four specimens |
| linux-clang-tsan (`setarch -R`, the preset's `TSAN_OPTIONS`) | crd-perf-tests 285, crd-ceridc-tests 22; repeated 10 times; no TSan report |

- Strict tidy (LLVM 20.1.8) is clean on the seven changed engine and test files; the application's `main.cpp`, which
  is in no engine compile database, is clean under the repository's `.clang-tidy` against its own build's database.
  clang-format differs only in the repository's hand alignment.
- The Allman check, `diag-coverage.py --validate`, and 23 CTests (the eight guards, repository hygiene and tools,
  developer workflow, project structure sync, build profiles, module selection, the DIAG script suites, ci-tiers,
  SIMD emission, CEIR invariants) pass; check-master-plan and check-repository pass.
- The coverage lane was not rerun: no outcome set changed, and the flight recorder's state-transition sites are the
  same lines. The full win-debug CTest was not run.

## What the hosted run must show

- `crd-package-consumer` green on `win-public-checks` and `linux-gcc-public-checks` (complete tier) with the
  diagnostic application: the family installed with `pdb` (Windows) or `dwarf` (Linux) symbols and the assets, the
  symbol probe passing, the deliberate failure leaving `flight-0.cdb`, and the investigation naming the trigger at
  `main.cpp`'s failing line and the forty events before it.
- crd-perf-tests with the two new `[diag11a][flight]` cases and the extended inspect cases, crd-ceridc-tests and the
  perf and ceridc fuzz corpora green on all six lanes.

## Remaining in DIAG.11a

- The examples for allocator selection, jobs, reload and capture on the consumer.
- Then the hosted run.
