# CRD Engine

A modern C++20 game/simulation engine.

> **Roadmap, status, and per-session notes live under [`docs/`](docs/).**
> Read [`docs/ROADMAP.md`](docs/ROADMAP.md) first when picking the project
> back up. Each work session has its own file under `docs/sessions/`.
> Each shipped subsystem has a short overview under `docs/systems/`.

## Build System

- CMake 3.25+ with Ninja generator
- Use CMakePresets.json: win-debug, win-release, win-asan, win-tidy
- Dependencies managed via CPM.cmake
- MSVC: `/Zc:preprocessor` is required (for `__VA_OPT__` in log macros)

## Build & Test Commands
- Configure: `cmake --preset win-debug`
- Build: `cmake --build --preset win-debug`
- Test: `ctest --preset win-debug`
- clang-tidy: `clang-tidy -p build/win-debug <file>`
- Format: `clang-format -i <file>`

## Architecture
- Engine modules are static libraries under `engine/`
- Each module: `include/crd/<module>/` (public API) and `src/` (implementation)
- Modules: core, log, memory, math, containers, platform, graphics, scripting, …
- Runtime executable links all modules statically
- Scripting/plugins use dynamic libraries with C API boundary

## Conventions
- C++20 standard, no compiler extensions
- Namespace: `crd`
- Naming: CamelCase classes, lower_case functions/variables, `m_` member prefix,
  UPPER_CASE macros, `k`-prefixed global constants
- Allman brace style, 4-space indent, 120 column limit
- `.hpp` for C++ headers, `.h` for C-only headers
- Every module has tests using Catch2
- Use `CRD_ASSERT`/`CRD_VERIFY` for assertions

## Module Status

| Module           | Status |
| ---------------- | ------ |
| `crd-core`       | ✅      |
| `crd-log`        | ✅      |
| `crd-memory`     | ⏳ next |
| `crd-math`       | ⏳      |
| `crd-containers` | ⏳      |
| `crd-platform`   | ⏳      |

Full status table and reasoning live in [`docs/ROADMAP.md`](docs/ROADMAP.md).

## Module Dependencies
- `crd-core`: no dependencies (types, platform, assert)
- `crd-log`: depends on `crd-core`
- `crd-memory`: depends on `crd-core`
- `crd-math`: depends on `crd-core`
- `crd-containers`: depends on `crd-memory`, `crd-core`
- `crd-platform`: depends on `crd-core`, `crd-log`
- `crd-graphics`: depends on `crd-core`, `crd-math`, `crd-memory`, `crd-platform`

## Where to look

- [`docs/ROADMAP.md`](docs/ROADMAP.md) — phase plan, status, decision log
- [`docs/sessions/`](docs/sessions/) — one file per work session
- [`docs/systems/`](docs/systems/) — short overview per shipped subsystem
- [`docs/log/LOG_FILE.md`](docs/log/LOG_FILE.md) — long deep-dive on `crd-log`
