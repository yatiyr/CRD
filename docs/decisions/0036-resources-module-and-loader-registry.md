---
id: ADR-0036
title: crd-resources module placement + loader-registry pattern
date: 2026-05-03
status: Accepted — PARTIALLY SUPERSEDED by ADR-0105 (2026-07-23, RET band). The loader-registry pattern and the
  crd-resources placement SURVIVE unchanged; every clause that names crd-rhi as the GPU boundary ("crd-resources must
  not depend on crd-rhi") re-reads with crd-gpu-context as the graphics layer — loaders stay GPU-free, uploads go
  through gpu-context (RET-3).
tags: [resources, arch]
---

# ADR-0036 — `crd-resources` module placement + loader-registry pattern

## Context

Phase 2.6 introduces `crd-resources`: the central runtime registry that holds every loaded asset
under intrusive refcount, dispatches loads through `crd-jobs`, and notifies consumers on
hot-reload. ADR-0013 already established that source assets never reach the runtime — only cooked
binary artifacts do. ADR-0014 already established the ref-counting split: generic primitives in
`crd-memory`, resource-facing concerns in `crd-resources`. ADR-0022 already established that the
open-world streaming pipeline needs all five of allocator, jobs, async I/O, resource manager, and
streaming allocator.

What was open: where does `crd-resources` sit in the dependency graph, and how do consumers
(shader, renderer, DAW, sim) plug their typed loaders into it without circular dependencies?

The naive approach — `crd-resources` knows about every resource type — would force `crd-resources`
to depend on `crd-rhi`, `crd-shader`, `crd-renderer`, `crd-audio`, every domain module. A DAW
build that wants `crd-resources` for sample loading would drag in every GPU module. A simulation
build that wants `crd-resources` for procedural data would drag in `crd-shader`. This violates
the module-isolation principle from `docs/PRINCIPLES.md`.

## Decision

**`crd-resources` sits LOW in the dependency graph and is type-erased at the registry boundary.
Consumers register typed loaders into a central `ResourceManager` at startup.**

### 1. Dependency placement — strictly downward

`crd-resources` depends only on:
- `crd-core`, `crd-log`, `crd-memory`, `crd-containers` — foundational substrate.
- `crd-jobs` — async load dispatch and fiber-cooperative `wait_ready`.
- `crd-platform` — file I/O (synchronous in v1a–v1c; async via `AsyncFile` from v1d, see
  ADR-0041).
- `crd-config` — for cook-profile and runtime budget configuration.

It does **NOT** depend on `crd-rhi`, `crd-shader`, `crd-renderer`, or any future
`crd-physics`/`crd-audio`/`crd-scene`. Those modules depend ON `crd-resources`, never the
other way.

### 2. Type-erased registry, typed handles

`ResourceManager` stores control blocks under a `ResourceId` key with a type FourCC tag. The
manager itself never names a concrete payload type. Consumers hold `ResourceHandle<T>`; the
template parameter is verified against the FourCC tag at handle construction (debug-asserted in
debug builds; release builds trust the cooked manifest).

### 3. Three extensibility hooks — the entire surface for plug-ins

The whole consumer-facing extension API is three functions:

- `ResourceManager::register_loader(unique_ptr<ILoader>)` — adds a typed loader for one type
  FourCC. Called once at startup by each consumer module's `register_*_loader(rm)` free function.
- `register_cook_handler(ext, fn)` — TOOLS-side equivalent for the cooker. Plug-ins register
  here to teach the cooker about new source extensions. Compile-time in v1b; DLL-based with
  Phase 4.
- `ResourceManager::mount_manifest(path)` — adds a binary pack to the search list. Last mount
  wins on UUID collision (logged at `Warn`). DAW projects always mount last.

Anything else (eviction policy, async vs sync, hot-reload subscription) is uniform across all
resource types and lives in `ResourceManager` itself.

### 4. Loaders use `IAllocator`

Every `ILoader::load()` receives a `crd::IAllocator*` in its `LoadContext`. Loaders allocate
their payload through this allocator. `ResourceManager` chooses the allocator (default = global
TLSF; per-loader override possible via registration). This keeps memory accounting consistent
across resource types and supports per-domain budgets (a robotics build can allocate sensor
buffers from a separate pool from rendering).

### 5. Loaders call `manager->load_sync<T>` for transitive dependencies

A `MaterialLoader` resolving its shader dependency does so via the `LoadContext::manager`
pointer. The manager handles cycle detection and reuses already-loaded handles. This pushes the
dependency graph entirely into the loader contract — `ResourceManager` itself doesn't need
type-specific knowledge of what depends on what; it just records the graph reported by the
cooker (the `DEPS` chunk) for eviction-impact analysis.

## Consequences

**Good:**
- DAW, simulation, headless cook, and game builds all share the exact same `crd-resources` core
  with zero unused code linked in.
- Adding a new resource type (e.g. `AnimationClip` in Phase 3.2) requires exactly one new file:
  the `AnimationClipLoader`. No edit to `crd-resources`, no rebuild of the registry.
- `crd-resources` can be unit-tested in isolation — its tests register a `BlobResource` test
  loader and never touch graphics or audio.
- The dependency direction is one-way and obvious: any future module that needs assets links
  `crd-resources`; `crd-resources` never grows upward dependencies.

**Constraints:**
- Loaders MUST allocate payloads through `LoadContext::allocator`. Private allocator use breaks
  the budget accounting that v1g eviction relies on.
- Loaders MUST NOT capture references to `LoadContext`-owned data (the `bytes` span, the
  `manager` pointer) past the end of `load()`. The control block stores only the payload pointer.
- Type tags (FourCC) must be globally unique across all loaders linked into a binary. A central
  reserved-FourCC list lives in `engine/resources/include/crd/resources/type_tags.hpp` to prevent
  collisions across modules.
- Consumers cannot rely on `ResourceManager` knowing about their type at compile time — they must
  call `register_loader` before any `load_sync<T>` of that type. Failure to register is a
  `CRD_ASSERT` in debug, `LoadState::Failed` in release.

## References

- `docs/phases/phase-2.6-resources.md`
- ADR-0013 — Asset pipeline (separate cooker exe)
- ADR-0014 — Reference counting split (RefCounted in `crd-memory`)
- ADR-0022 — Open-world streaming pipeline (5 prerequisites)
- ADR-0033 — `crd-jobs` implementation (async dispatch substrate)
- ADR-0037 — ResourceId hybrid UUID scheme
- ADR-0039 — ResourceHandle<T> semantics
