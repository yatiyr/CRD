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
- Repository/docs: `check-repository.py`, `check-master-plan.py`, `test-repository-tools.py`; module registry
  contracts: `test-module-selection.py` ([design](../docs/design/module-registry.md)); CI tier mapping,
  resolution and lane evidence: `check-ci-tiers.py`, `ci-tier.py`, `ci-evidence.py`
  ([design](../docs/design/ci-tiers.md)).
- Native Visual Studio: `project-sync.py open --preset win-vs-debug` starts saved structure synchronization.
  `status`, `stop`, `sync` preview and `edit --operations <json> --apply --regenerate` share one portable transaction path.
  Read [structure/recovery](../docs/design/project-structure-sync.md) before module moves or Remove/Delete operations.
  Regressions: `test-project-sync.py`; isolated real compile: `test-project-sync-native.py` (optional Windows `--ide`).
- Pinned inputs ([registry](../cmake/pins.json), [design](../docs/design/pinned-inputs.md)): `check-pins.py` guards
  it; `pins.py fetch <section> <name>` downloads and verifies one entry; `install-vulkan-sdk.py` (Windows),
  `install-vulkan-headers.py` (Linux), `install-vulkan-validation.py` and `install-warp.py` acquire their inputs
  from it, accepting `--archive`/`--archive-dir` for verified offline copies; `install-sccache.py` takes `--archive`
  and `--download-dir`.
- Build performance ([design](../docs/design/build-performance.md)): `build-bench.py run` measures configure, cold,
  no-op, source-edit, header-edit, PCH-header-edit and link rows of one preset in a fresh scratch directory (explicit
  `--jobs`, optional `--sccache` statistics) and `render` merges boards for `docs/bench/`; the lane evidence bundle
  records every hosted build's Ninja log and sccache statistics.
- Public consumption ([design](../docs/design/public-consumption.md)): `check-headers.py --build <dir> --changed`
  compiles changed public headers standalone with a sibling unit's flags from the compile database (outputs
  redirected to a scratch directory); `test-package-consumer.py --preset <name> --scratch <dir>` installs a
  core-only package, moves it, builds and runs a downstream consumer against it and proves the profile header,
  relocatability and the two refusals (it is the `crd-package-consumer` CTest of the public-check presets).
- Test instruments ([design](../docs/design/test-instruments.md)): `fuzz.py run|merge|minimize|adopt|replay` drives
  the bounded libFuzzer targets of a `linux-clang-fuzz` build (time, length, RSS and allocation limits; artifacts
  in scratch; a finding enters a committed corpus only after its fix, `adopt`; an unfixed one is kept as bytes
  under `fuzz/pending/`) and replays the corpora any lane runs as the `crd-fuzz-*-corpus` CTests.
- Provenance and licenses ([routes](../docs/CONTRIBUTING.md#generated-sources-and-provenance)): `check-generated.py`
  holds every generated file to [generated-sources.json](generated-sources.json) (generator, status, SHA-256;
  `--scan` lists unlisted markers, `--regenerate` reruns the stdout generators on the current host and applies the
  formatter of a `formatted` entry when clang-format is available, `--refresh <path>`
  records a deliberate regeneration); `gen_license_manifest.py [--check]` renders
  [dependency-licenses.md](../docs/generated/dependency-licenses.md) from the pins registry. Both are lane steps.
- Linux Vulkan validation: `install-vulkan-validation.py --destination <ignored-directory>` installs the pinned,
  SHA-256-checked layer. Export the printed layer/library paths before GPU tests; it does not replace the driver.
- Generators, oracle scripts and `bench_*`/`run_bench_*` helpers are reproducibility assets. Their matching
  recipes and dated benchmark boards define inputs, peers and qualification scope.
- `per-slice-check.*`, `full-sweep.ps1` and the unfiltered WSL wrapper are broad CI recipes. Locally select targets.

Put one-shot scripts, logs and probes in ignored `build/<task>/`. Shared scripts must not encode a particular
developer's checkout, SDK version or home directory as their only working path. Preserve real exit codes.

Use [run-ctest.ps1](run-ctest.ps1) for scoped CTest: it supplies the detected MSVC runtime/disassembly tools and
forwards regex arguments directly. The old batch wrapper cannot safely receive a pipe-containing regex from PowerShell.
