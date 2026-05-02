# ADR-0034 — C++ hot-reload DLL scripting as primary scripting mechanism

**Status:** Accepted  
**Date:** 2026-05-02  
**Tags:** arch, scripting, extensibility

## Context

Cerid targets multiple verticals (games, simulation, DAWs). All of them
need user-authored logic — gameplay ticks, robot control loops, DSP
graphs, editor tool extensions. The classic options are:

- **Lua**: lightweight, but a C↔Lua boundary on hot paths and a foreign
  type system that cannot express the same zero-cost abstractions as C++.
- **C# (Mono/CLR)**: mature tooling, but managed-heap pauses and a 20 MB+
  runtime that conflicts with DAW/sim latency targets.
- **Python**: excellent for off-line scripting and tooling, unusable for
  real-time loops.
- **Native C++ in the main executable**: full performance, but requires a
  full process restart to iterate — unacceptable for creative tools.

The explicit requirement: no scripting overhead (no cross-language FFI on
hot paths), full C++ performance, but with iteration speed comparable to
interpreted languages.

## Decision

**C++ hot-reload via separate DLL is the primary extensibility mechanism.**

### Mechanism

1. All game / simulation / domain logic compiles into a dedicated DLL
   (e.g., `game.dll`), separate from the engine executable.
2. The DLL is loaded at startup via `LoadLibrary` (Windows) /
   `dlopen` (Linux). The engine resolves entry points through a
   **stable C ABI facade** — no C++ symbols cross the boundary.
   (C++ name mangling, vtable layout, and exception ABI are
   compiler-version dependent; C is not.)
3. On file-change notification or explicit trigger, the engine:
   a. Suspends the `crd-jobs` worker pool.
   b. Releases all DLL handles.
   c. Copies the freshly compiled DLL to a timestamped path
      (MSVC locks the original file while loaded).
   d. Loads the copy; re-resolves all C ABI entry points.
   e. Resumes the worker pool.
4. **All persistent state lives in engine-owned memory** — ECS
   components, resource handles, config values. The DLL owns only
   logic, not data. On reload, old logic is discarded cleanly and
   engine state continues without loss.

### ABI contract

- The DLL must export a `u32 abi_version()` symbol. If the engine and
  DLL versions disagree the engine refuses to load and logs an error.
- Public entry points are free C functions with primitive or
  engine-handle arguments. No C++ classes, templates, or exceptions
  cross the boundary.
- The cookbook (Phase 4.0c) documents the supported patterns:
  gameplay tick, custom `ILayer`, asset load/unload hooks, job lambdas
  bridged via the C boundary.

## Consequences

**Good:**
- Zero hot-path overhead: user code is native C++, inlined and optimized
  by the user's own compiler.
- Fast iteration: recompile + reload in seconds, no process restart.
- Full C++ type system, STL, and SIMD available inside the DLL.
- Works identically across all verticals (games, robotics, DAW, cinematic).

**Bad / costs:**
- C ABI boundary must be maintained. Users cannot export arbitrary C++
  classes across the boundary; they export factory functions that fill
  Cerid-owned structs or return opaque handles.
- No live state migration when a type layout changes (adding a field to
  a component requires a full reload, not a hot patch of the layout).
- Windows: MSVC locks the original DLL while it is loaded; the copy trick
  is required. Cross-platform discipline is mandatory.

## Alternatives rejected

- **Lua**: hot-path overhead, foreign type system, limited debugger support.
- **C# Mono**: managed GC pauses incompatible with DAW/sim latency targets.
- **Static link + relaunch**: compile + link + restart is too slow for
  creative tool iteration loops.
- **JIT-compiled scripts**: adds a JIT compiler dependency with no clear
  advantage over a well-structured C ABI reload.
