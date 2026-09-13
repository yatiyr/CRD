# Frontend qualification on Windows and Linux against actual Cerid consumers

<!-- doc-role: historical -->
> Dated evidence. Live owners: [REPO.DEV.3c](../ROADMAP.md#slice-repo.dev.3c), [REPO.DEV.3b](../ROADMAP.md#slice-repo.dev.3b)
> and [REPO.DEV.3](../ROADMAP.md#slice-repo.dev.3). Rules: [AGENTS](../../AGENTS.md). Same day:
> [portable strict analysis](2026-09-13-portable-strict-analysis.md), [jobs end-hook ordering](2026-09-13-jobs-end-hook-ordering.md),
> [strict-gate header repairs](2026-09-13-strict-gate-header-repairs.md), [REPO.DEV audit](2026-09-13-repo-dev-audit.md).

Contract: qualify `doctor`, `plan`, `check` and `evidence` on Windows and Linux against actual Cerid consumers,
including unbuilt Catch2 discovery, with no stale or empty passes and no automatic local sweep. Everything below ran
on the real repository, not on fixtures: the workstation's Windows MSVC Debug build (`build/win-debug`) and the WSL
Ubuntu 24.04 GCC Debug build (`build/linux-gcc-debug`) read through the 9p mount. The user owns every commit and push.

## Runs

| Host | Command | Outcome |
|---|---|---|
| Windows | `doctor --build build/win-debug` | CMake, MSVC, Ninja, Python and the pinned clang-tidy 20.1.8 resolved with versions; eleven eligible presets; no issues |
| Linux | `doctor --build build/linux-gcc-debug` | g++, Ninja and Python resolved; `clang_tidy_20: null` with the searched candidates named and no issue raised, so a documentation check stays valid; seven eligible presets |
| Windows | `plan --build build/win-debug` | affected scope of the working tree with owners, reverse consumers, tidy set, both guards and four risk gates; `local_sweep_allowed: false` |
| Linux | `plan --build build/linux-gcc-debug --git-timeout 600` | 2 min 57 s over 9p; scope **full** because the working tree carries a root `CMakeLists.txt` change and an unowned JSON (reasons named), 283 targets in the closure, 250 buildable, six tidy files, both guards, `local_sweep_allowed: false`; `check` on that scope without `--target` refuses |
| Windows | `check --path tests/execution/ceir-gpu-dx12/test_work_smoke_dx12.cpp` | guarded configure of a stale model, `crd-ceir-gpu-dx12-tests` built, fresh discovery, **65/65** on the NVIDIA adapter, both guards, tidy; passed, integrity verified. A first run passed every phase and then refused its verdict because tracked documents changed during it |
| Windows | `check --path worker_pool.cpp --path observer.hpp --target crd-jobs-tests --target crd-perf-tests` | **206/206**, both guards, tidy of both files with the header through its owning unit; passed. An earlier attempt refused to start while a registered native generation held the structure lock |
| Windows | `check --path <four headers> --target <four test executables>` ([header repairs](2026-09-13-strict-gate-header-repairs.md)) | 61 build steps, **539/539** including the nineteen global guards, tidy of the four headers through their owning targets; passed |
| Linux | `check --path tests/foundation/jobs/test_counter.cpp --target crd-jobs-tests --jobs 2 --git-timeout 600` | the jobs test executable had never been built on Linux: built in 70 s, discovered, **120/120**, both guards (the validator guard 57 s over 9p), tidy with a major-20 clang-tidy; passed, integrity verified over 25 artifacts |
| Linux | same command with the default Git budget | `instrument_failure`: `git diff HEAD` exceeded 60 s on the 9p mount (98 s cold, a few seconds afterwards); no evidence claimed |
| Linux | same command with the pinned 20.1.8, twice (once during two concurrent gate passes, once alone) | `failed` both times: `crd-master-plan` exceeded its 60 s CTest budget at 60.06 s (the validator alone measures 57 to 64 s over 9p), **119/120**, nothing skipped, exit 8 retained (`20260913T222233-268b13ebcb0c`, `20260913T223757-a63777d1bcc5`) |
| Linux | same command with the pinned 20.1.8 after the guard budget was raised to 300 s and the build reconfigured | **passed** (`20260913T225337-c86cb200c2f7`): build 42 s, discovery, **120/120**, validator guard 60 s, hygiene guard 12 s, tidy phase 20 s with `LLVM version 20.1.8` (one file clean from its database entry in 6.9 s), integrity verified |

## What the runs prove

- **Unbuilt Catch2 discovery.** The Linux `crd-jobs-tests` and the Windows `crd-ceir-gpu-dx12-tests` runs built the
  executable first and discovered from the fresh binary; the selected, reported and executed counts reconcile with
  the JUnit names.
- **No stale or empty pass.** A stale File API model triggers a guarded configure with `configure_reason` recorded;
  zero CTest results raise; skipped or disabled tests end `incomplete`; a changed checkout or model during execution
  invalidates the run (observed once on Windows); an unavailable or wrong-major strict tool ends `incomplete`; an
  exhausted Git or CTest budget is an instrument failure or a failure, never a pass (observed twice on Linux).
- **No automatic sweep.** The affected closure of `observer.hpp` is 130 targets; `check` built only the two explicit
  test executables and recorded the full scope for CI. `plan` reports `local_sweep_allowed: false`, and `check`
  without `--target` on a full scope refuses with "Full qualification belongs to CI".
- **Bounded execution and identity.** Every run used two compile workers, explicit build, discovery, test and Git
  budgets, serial CTest, and sealed its record with source identity before and after.

## State

REPO.DEV.3c, REPO.DEV.3b and REPO.DEV.3 are Needs CI on this evidence, waiting for the hosted repository jobs of the
next push; REPO.DEV.4 is the next Open row. A hosted Linux strict lane and CI throughput belong to REPO.DEV.5.
