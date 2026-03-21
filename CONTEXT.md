# CRD Engine

A modern C++20 game/simulation engine.

## Build System

- CMake 3.25+ with Ninja generator
- Use CMakePresets.json: win-debug, win-release, win-asan, win-tidy
- Dependencies managed via CPM.cmake

## Build & Test Commands
- Configure: `cmake --preset win-debug`
- Build: `cmake --build --preset win-debug`
- Test: `ctest --preset win-debug`
- clang-tidy: `clang-tidy -p build/win-debug <file>`
- Format: `clang-format -i <file>`

## Architecture
- Engine modules are static libraries under engine/
- Each module: include/crd/<module>/ (public API) and src/ (implementation)
- Modules: core, memory, math, containers, log, platform, graphics, scripting, etc.
- Runtime executable links all modules statically
- Scripting/plugins use dynamic libraries with C API boundary

## Conventions
- C++20 standard, no compiler extensions
- Namespace: crd
- Naming: CamelCase classes, lower_case functions/variables, m_ member prefix, UPPER_CASE macros, k-prefixed global constants
- Allman brace style, 4-space indent, 120 column limit
- .hpp for C++ headers, .h for C-only headers
- Every module has tests using Catch2
- Use CRD_ASSERT/CRD_VERIFY for assertions

## Module Dependencies
- crd-core: no dependencies (types, platform, assert)
- crd-memory: depends on crd-core
- crd-math: depends on crd-core
- crd-containers: depends on crd-memory, crd-core
- crd-log: depends on crd-core
- crd-platform: depends on crd-core, crd-log
- crd-graphics: depends on crd-core, crd-math, crd-memory, crd-platform
```
## Roadmap

### Phase 1: Core Modules
- core (types, platform, assert, build config)
- memory (allocators: linear, stack, pool)
- math (vec, mat, quat, transform)
- containers (array, hashmap, string)
- log (logger, sinks)
- platform (window, input, filesystem, timer)

### Phase 2: Graphics
- API-agnostic graphics abstraction
- Vulkan backend (first), DirectX 12 (future)
- ImGui (docking branch) for dev tooling

### Phase 3: Scripting
- Hot-reload C++ scripting via DLLs
- C API boundary between engine and script modules
- Plugin system for third-party extensions

### Phase 4: Game UI System
- crd-ui module on top of crd-graphics
- Retained mode for game-facing UI (HUD, menus, dialogs)
- Custom rendering, layout, styling, animation

### Phase 5: Editor
- Built on crd-ui (replaces ImGui for editor panels)
- ImGui demoted to debug overlay only

## Architectural Decisions
- Static libraries for engine modules, dynamic only for scripting/plugins
- GLOB_RECURSE with CONFIGURE_DEPENDS for automatic source detection
- Allman brace style, .hpp for C++, .h for C-only
- ImGui (docking) is temporary dev tooling, not long-term UI solution
- AI/ML features are future optional modules, not baked into core
- Engine should stay general enough for both games and simulation (robotics etc.)