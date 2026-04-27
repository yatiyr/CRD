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
| `crd-memory`     | ✅      |
| `crd-containers` | ✅      |
| `crd-math`       | ✅      |
| `crd-platform`   | ✅      |
| `crd-app`        | ✅      |
| `crd-rhi`        | 🚧      |

Full status table and reasoning live in [`docs/ROADMAP.md`](docs/ROADMAP.md).

## Module Dependencies
- `crd-core`: no dependencies (types, platform, assert)
- `crd-containers`: depends on `crd-core`, `crd-memory`. Headers reference
  `crd-log`'s `CRD_DECLARE_LOG_CHANNEL` macro but link is one-way.
- `crd-memory`: depends on `crd-core`, `crd-log`
- `crd-log`: depends on `crd-core`, `crd-containers`. (Its
  `RingBufferSink` uses `crd::containers::Array`. First-party channel
  definitions like `g_log_containers` also live here to break the cycle.)
- `crd-math`: depends on `crd-core`
- `crd-platform`: depends on `crd-core`, `crd-log`, `crd-containers`. Backend
  is GLFW as a PRIVATE link dependency; no GLFW symbol leaks into public
  headers. Owns its own `g_log_platform` channel (no cycle, definition
  inside crd-platform itself).
- `crd-app`: depends on `crd-core`, `crd-containers`, `crd-platform`.
  Owns Application, LayerStack, propagated Event hierarchy, and a small
  typed sync EventBus. Deliberately does NOT depend on `crd-rhi`,
  `crd-rhi-vulkan`, or `crd-renderer`.
- `crd-rhi`: depends on `crd-core`, `crd-platform`, `crd-memory`. Ships the
  low-level API-agnostic GPU interface only; no backend types leak into the
  public surface.
- `crd-rhi-vulkan`: planned dependency target is `crd-rhi`, `crd-platform`, `crd-log`
- `crd-renderer`: planned dependency target is `crd-rhi`, `crd-math`, `crd-memory`, `crd-resources`

## Where to look

- [`docs/ROADMAP.md`](docs/ROADMAP.md) — phase plan, status, decision log
- [`docs/sessions/`](docs/sessions/) — one file per work session
- [`docs/systems/`](docs/systems/) — short overview per shipped subsystem
- [`docs/systems/math.md`](docs/systems/math.md) — current math module overview
- [`docs/systems/platform.md`](docs/systems/platform.md) — current platform module overview
- [`docs/systems/app.md`](docs/systems/app.md) — current app module overview
- [`docs/systems/rhi.md`](docs/systems/rhi.md) — current rhi module overview
- [`docs/log/LOG_FILE.md`](docs/log/LOG_FILE.md) — long deep-dive on `crd-log`
- [`docs/memory/MEMORY_FILE.md`](docs/memory/MEMORY_FILE.md) — long deep-dive on `crd-memory`
- [`docs/containers/CONTAINERS_FILE.md`](docs/containers/CONTAINERS_FILE.md) — long deep-dive on `crd-containers`
