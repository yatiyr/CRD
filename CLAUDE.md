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

