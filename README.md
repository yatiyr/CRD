# Cerid Engine

<!-- doc-role: navigation -->
> Navigation; no independent live queue. Current work: [ROADMAP](docs/ROADMAP.md); current rules: [AGENTS](AGENTS.md).

[![CI](https://github.com/yatiyr/CRD/actions/workflows/ci.yml/badge.svg)](https://github.com/yatiyr/CRD/actions/workflows/ci.yml)

Cerid is a modular C++20 engine and application substrate. Games, real-time and offline rendering, simulation,
engineering and scientific tools, and creative applications are consumers of the same public modules. Algorithms that
ship with the engine are authored assets (CHIR programs, CEIR execution graphs, CKIR device kernels) that can be edited,
replaced and reloaded, not code hidden in engine C++. The product editor is CR-D007; the same commands serve the
editor, the command line, programs and agents.

New contributors and agents start at [START_HERE](START_HERE.md). A module existing in the tree is not a claim that
it is qualified: every row's state, evidence and remaining work is in the [master table](docs/ROADMAP.md#master-table).

## Architecture in brief

- **Modules.** <!-- fact:module-count -->109<!-- /fact --> modules are registered with `crd_module()` in the root
  [CMakeLists](CMakeLists.txt): <!-- fact:engine-module-count -->102<!-- /fact --> engine modules in the families
  <!-- fact:engine-families -->`assets`, `execution`, `foundation`, `geometry`, `gpu`, `media`, `numerics`, `physics`, `rendering`, `ui` and `world`<!-- /fact --> (`engine/<family>/<module>`), and
  <!-- fact:other-module-count -->7<!-- /fact --> tools and hosts (`tools/`, `runtime/`, `sandbox/`). The
  [systems map](docs/systems/README.md) gives each module's source, CMake contract and overview.
- **Execution.** CHIR is the high-level authoring language, CEIR the canonical execution IR and CKIR the device-kernel
  form ([ADR-0109](docs/decisions/0109-ceir-chir-ckir-ownership-and-module-placement.md)). `gpu-context` owns devices,
  programs and pipelines; CKIR backends exist for Vulkan, Direct3D 12, CUDA, HIP, Metal and WebGPU (`kir-*` modules).
- **CPU dispatch.** x64 builds keep SSE2 as the floor and select kernels at run time per CPU level:
  <!-- fact:cpu-levels -->`portable`, `sse2`, `sse4.2`, `avx2`, `avx512` and `neon`<!-- /fact -->
  ([ADR-0134](docs/decisions/0134-runtime-cpu-dispatch.md)). `CRD_CPU_LEVEL` forces a lower level for testing.
- **Determinism.** Modes <!-- fact:determinism-modes -->`Default`, `CrossPlatform` and `BackwardCompat`<!-- /fact -->:
  `Default` replays bit-exactly on one machine and build; `CrossPlatform` is bit-exact across MSVC, Clang and GCC, x64
  and ARM64 and every CPU level; `BackwardCompat` is reserved.
- **Numerics.** `crd-hesap` is the computational base for every solver and matrix need. Performance claims are
  measured against named peer libraries and recorded with their losses in [docs/bench](docs/bench/README.md).
- **Own primitives.** Fiber job system, allocators, containers, math and the text stack are Cerid-owned; vendor types
  do not cross public interfaces ([PRINCIPLES](docs/PRINCIPLES.md)).

## Platforms

Windows and Linux on x64 are the primary qualification targets; ARM64 builds run on hosted Linux and Windows ARM64
runners. macOS, web, mobile and consoles are planned releases with their own rows in the master table; a backend or
emitter that exists in source is not a platform release.

## Building

Requirements: CMake 3.25 or newer (4.2 or newer with Visual Studio 2026), Python 3.12 or newer, and MSVC, clang-cl,
GCC or Clang. Configurations are the presets in [CMakePresets.json](CMakePresets.json); third-party inputs are pinned
by version and SHA-256 in [cmake/pins.json](cmake/pins.json).

```powershell
./scripts/configure-preset.bat win-debug
./scripts/build-target.bat build/win-debug <target>
```

```bash
bash scripts/setup-wsl-deps.sh          # once: the hosted Linux lanes' system packages and SPIRV-Reflect
cmake --preset linux-gcc-debug
cmake --build --preset linux-gcc-debug --target <target>
```

The hosted Linux lanes configure and build with the same two preset commands; from Windows, BUILDING's WSL route
(`scripts/wsl-lane.sh`) runs them in a mirror of the checkout.

[BUILDING](docs/BUILDING.md) gives the scoped local workflow, tidy, Visual Studio profiles and the local-versus-CI
decision table; [CODING](docs/CODING.md) gives the C++ style contract.

## Testing and continuous integration

Each module declares its CTest suites in its `crd_module()` row; repository guards (`crd-check-*`, `crd-no-*`) and the
Python validators hold source rules, pinned inputs, generated files and this documentation to the tree. Locally,
only the changed targets and their consumers are built and tested; continuous integration runs the full matrix.

The hosted workflow is GitHub Actions ([ci.yml](.github/workflows/ci.yml)), with change, nightly and complete tiers
([CI tiers](docs/design/ci-tiers.md)). The project is moving to its own CI orchestrator on cloud and owned machines
([ADR-0135](docs/decisions/0135-cerid-ci-platform.md), [CI platform](docs/design/ci-platform.md), rows CI-0 to CI-4);
GitHub Actions remains the qualifying CI until CI-4 shows parity.

## Workspace

This repository holds the code and every document; GitLab is its code host and mirrors it to GitHub. AI-agent files
(CLAUDE.md, opencode.json, `.claude/`, `.codex/` and the Claude Code memory) are not part of it: they live in a
separate agent overlay kept only on the maintainer's NAS, and `python scripts/workspace.py` syncs, exports and
restores the whole workspace on any machine ([workspace sync](docs/recipes/workspace-sync.md),
[ADR-0136](docs/decisions/0136-workspace-overlay.md)).

## Where work is tracked

- [ROADMAP](docs/ROADMAP.md) is the only live tracker: one ordered master table of slices, bugs and findings. Its
  order is the accepted [delivery plan](docs/design/delivery-plan.md)
  ([ADR-0129](docs/decisions/0129-renderer-ui-editor-delivery-order.md) and its amendments).
- [context](context.md) names the current row; [AGENTS](AGENTS.md) is the working contract for people and agents.
- Decisions are [ADRs](docs/decisions/README.md); dated evidence is in [sessions](docs/sessions/) and
  [benchmarks](docs/bench/README.md); the full documentation map is [docs/README](docs/README.md).

## License

<!-- fact:copyright -->Copyright (c) 2026 Burak Eren Dere. All rights reserved.<!-- /fact -->

Cerid is proprietary software, not open source. The repository is public for viewing only; using, building,
copying, modifying, distributing, or using it for machine-learning training or text and data mining requires
the owner's prior written permission. The full terms, in English and Turkish, are in [`LICENSE`](LICENSE);
permission requests go to dereeren@yahoo.com. Third-party components keep their own licenses
([`cmake/pins.json`](cmake/pins.json), [generated manifest](docs/generated/dependency-licenses.md),
[`assets/source/LICENSES.md`](assets/source/LICENSES.md)).
