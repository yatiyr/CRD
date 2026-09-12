# 2026-09-12 — Visual Studio configuration matrix

<!-- doc-role: historical -->
> Session evidence. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

The user found only Debug in the generated native solution and requested automatic two-way synchronization and
the complete configuration matrix. They selected CMake's preset selector for all toolchains/platforms plus complete
native MSVC configurations. Owner: REPO.SYNC.5. Contract: [ADR-0132](../decisions/0132-visual-studio-configuration-matrix.md)
and [configuration guide](../design/visual-studio-configurations.md).

The earlier preset explicitly set `CMAKE_CONFIGURATION_TYPES=Debug`. All 20 configure presets remained on disk.
Adding dropdown labels alone would have reused configure-time Debug macros/headers and wrong sanitizer/Shipping/ISA
settings. The implementation now derives native profiles from the canonical presets and makes those settings follow
the chosen configuration. The build directory remains stable. Source algorithms and geometry/physics scope are unchanged.

The watcher was checked alive, then stopped before editing its build inputs. Task-start files were preserved in
ignored `build/vs-configurations-20260912/before.zip`; it is a local recovery bundle, not a repository deliverable.
Advisor search found no callable capability. No advisor approval or remote CI result is claimed.

## Verification record

Host: Windows 11 x64, CMake 4.3.2, Visual Studio 2026 Community, MSVC 19.51.36246 / toolset 14.51.36231,
Python 3.14.4. Logs are under ignored `build/vs-configurations-20260912/`.

- Portable profile contracts: 7 passed on Windows and WSL Linux. Synchronizer suite: 52 passed on both
  (including inherited fixture tests). Repository tooling: 4 passed.
- Native profile probe: all 8 configurations compiled and passed scoped CTest; public macros, CRT/ISA and target
  IPO exemption checks passed. The first ASan run exposed a missing DLL search path in the fixture runner;
  the verified selected-compiler directory now supplies its runtime. No engine crash was inferred from 0xc0000135.
- Actual CMake solution generation succeeded with all 8 profiles and preserved structural ownership across them.
- Actual `crd-core` compiled cleanly in all 8 native configurations. Ninja `win-debug` core tests: 2/2 passed.
- Linux Ubuntu/WSL, GCC 13.3.0, Python 3.12.3: existing `linux-gcc-debug` configured, `crd-core-tests` built,
  2/2 core CTests passed. The portable native-structure fixture projected to Ninja and compiled successfully.
- Real isolated Visual Studio COM fixture: saved IDE additions/filters, filesystem/header/module moves,
  remove/delete/recovery, empty folders and watcher startup/no-feedback passed; affected targets compiled in
  both Debug and Release. Final registered native-profile CTest: 1/1 passed, covering its 8 compile/runtime legs
  and explicit MSBuild ASan, optimization, IPO, CRT/ISA and Shipping dead-stripping property assertions.
- Scoped native CTest tooling guards: 5/5 passed. Documentation and repository hygiene validators passed;
  actionlint 1.7.12 reported no workflow errors. CI now registers the native profile proof on VS 2022.
- The actual open IDE initially cached only Debug. After checking idle state and saved projects/documents,
  its solution was reloaded and saved documents reopened. COM inspection then returned all 8 configurations.
  Final `project-sync.py verify` passed; watcher status was alive/watching with no incomplete transaction or generation.
  A subsequent CMake diagnostic edit automatically regenerated through that live watcher, providing an additional
  real-checkout source-to-IDE lifecycle check. Status now summarizes generation metadata without dumping its large
  recovery inventory. Full journals remain on disk.

Reproduce the scoped gates (no whole-engine sweep):

```powershell
python scripts/project-sync.py open --preset win-vs
cmake --build build/win-vs-debug --config <configuration> --target crd-core --parallel 2
./scripts/build-target.bat build/win-debug crd-core-tests
ctest --test-dir build/win-debug/tests/core --timeout 60 --no-tests=error --output-on-failure
python scripts/test-project-sync.py
python scripts/test-project-sync-native.py --generator "Visual Studio 18 2026" --ide
./scripts/run-ctest.ps1 -CtestArguments @('--test-dir', 'build/win-vs-debug', '-C', 'Debug',
    '-R', '^crd-native-build-profiles$', '--timeout', '240', '--no-tests=error', '--output-on-failure')
```

WSL used `cmake --preset linux-gcc-debug -B /home/yatiyr/cerid-build/linux-gcc-debug`, then only
`--target crd-core-tests --parallel 2` and CTest under that build's `tests/core` directory. The Linux fixture command
was `python3 scripts/test-project-sync-native.py --generator Ninja`. `cmake --list-presets` listed all 7 Linux presets.
PowerShell native CTest calls use the explicit argument array so `-C` is not misbound as a script parameter.
No engine C++ changed; no format rewrite or numerical/physics work was performed.

The native preset still exposes the `tests/bench` suite, excluding it from default Shipping builds. Single-config
Ninja Shipping continues to omit that suite; runtime benchmark examples retain their existing rules. Conditional IDE-only exclusions cannot become all-configuration removals.
The full remote engine CI matrix has not run for these unpublished changes. No commit or push was performed.
