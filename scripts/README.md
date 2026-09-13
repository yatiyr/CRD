# Development scripts

Run commands from the repository root. Build/test rules and exact quality requirements live in
[`docs/BUILDING.md`](../docs/BUILDING.md); the only work tracker is [`docs/ROADMAP.md`](../docs/ROADMAP.md).

- Windows configuration/build: `configure-preset.bat <preset>`, `build-target.bat <build-dir> <target>`.
  Both use `msvc-env.bat` to discover Visual Studio and select standalone CMake. The default build cap is four;
  `CMAKE_BUILD_PARALLEL_LEVEL` can set a smaller/larger justified cap.
- Tests: invoke CTest directly with a scoped regex, `--timeout` and `--no-tests=error` as BUILDING shows.
  Do not send regex alternation through a `.bat` invocation from PowerShell: cmd reparses metacharacters.
- Changed C++: `tidy-files.py` on any host (`tidy-files.ps1` wraps it on Windows); `tidy-rename-helper.ps1 -LogFile
  <capture> -DryRun` previews mechanical fixes.
- Repository/docs: `check-repository.py`, `check-master-plan.py`, `test-repository-tools.py`.
- Native Visual Studio: `project-sync.py open --preset win-vs-debug` starts saved structure synchronization.
  `status`, `stop`, `sync` preview and `edit --operations <json> --apply --regenerate` share one portable transaction path.
  Read [structure/recovery](../docs/design/project-structure-sync.md) before module moves or Remove/Delete operations.
  Regressions: `test-project-sync.py`; isolated real compile: `test-project-sync-native.py` (optional Windows `--ide`).
- Linux Vulkan validation: `install-vulkan-validation.py --destination <ignored-directory>` installs the pinned,
  SHA-256-checked layer. Export the printed layer/library paths before GPU tests; it does not replace the driver.
- Generators, oracle scripts and `bench_*`/`run_bench_*` helpers are reproducibility assets. Their matching
  recipes and dated benchmark boards define inputs, peers and qualification scope.
- `per-slice-check.*`, `full-sweep.ps1` and the unfiltered WSL wrapper are broad CI recipes. Locally select targets.

Put one-shot scripts, logs and probes in ignored `build/<task>/`. Shared scripts must not encode a particular
developer's checkout, SDK version or home directory as their only working path. Preserve real exit codes.

Use [run-ctest.ps1](run-ctest.ps1) for scoped CTest: it supplies the detected MSVC runtime/disassembly tools and
forwards regex arguments directly. The old batch wrapper cannot safely receive a pipe-containing regex from PowerShell.
