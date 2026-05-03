---
id: ADR-0039
title: ResourceHandle<T> semantics
date: 2026-05-03
status: Accepted
tags: [resources, arch]
---

# ADR-0039 — `ResourceHandle<T>` semantics

## Context

Phase 2.6 needs a runtime handle type for loaded resources. The handle is the only thing
consumers (game code, render passes, simulation modules, DAW plugins) hold. It must be:

- Cheap to copy (asset references appear in many structs and component arrays).
- Compatible with async loading (`load_async` returns a handle before the payload exists).
- Compatible with hot-reload (the underlying payload pointer changes; consumer references must
  remain valid).
- Safe across multi-domain consumers — a simulation node can't tolerate accidentally rendering
  a "default magenta" placeholder for a missing terrain heightmap, but a game scene editor
  prefers placeholders over crashes.
- Fiber-aware — when a job needs to wait for a resource it should suspend the fiber, not block
  the OS thread.

Traditional approaches:

- **`std::shared_ptr<T>`** — two pointers (object + control block), 16 bytes; allocated
  separately from the payload; no async load state, no generation counter, no engine-friendly
  hooks.
- **Indexed handle into a slot table (Bitsquid-style `Handle{ id: u32, generation: u8 }`)** —
  4 bytes, dereference is one indirect plus a generation check. Fast, but no atomic refcount
  built in; eviction is more awkward.
- **One pointer to an intrusive control block holding both refcount and payload pointer** —
  one pointer per handle (8 bytes); copy = one atomic increment; payload pointer can swap
  atomically for hot-reload while existing handles stay valid.

The third matches the engine pattern (intrusive `RefCounted<T>` already established in ADR-0014)
and is what shipping engines with similar requirements use (Frostbite's `AssetRef`, Unreal's
`TObjectPtr` semantics for soft references).

## Decision

**`ResourceHandle<T>` is a one-pointer wrapper over an intrusive ref-counted control block.
Copy = one atomic increment. `get()` is non-blocking. `wait_ready()` is fiber-cooperative.
A `generation` counter increments on every successful hot-reload swap. Soft-failure
placeholders are per-loader opt-in.**

### 1. One-pointer layout

```cpp
template<typename T>
class ResourceHandle
{
private:
    struct ControlBlock;          // crd::memory::RefCounted<ControlBlock>
    ControlBlock* m_block = nullptr;
public:
    // ... default, copy, move, destruct, comparison
};
static_assert(sizeof(ResourceHandle<int>) == sizeof(void*));
```

The control block holds: atomic refcount, atomic payload pointer, atomic generation counter,
atomic `LoadState`, the `ResourceId`, the load `crd::jobs::Counter*` (null when not loading),
and the back-pointer to `ResourceManager` for unload-at-zero-refcount.

### 2. `LoadState` — six explicit states

```cpp
enum class LoadState : u8
{
    Unloaded,    // never requested (or evicted; can be re-requested)
    Queued,      // submitted, not started
    Loading,     // job in flight
    Ready,       // payload valid; get() returns non-null
    Placeholder, // soft fallback active; get() returns the placeholder payload
    Failed,      // hard failure; get() returns nullptr
};
```

Six states, not three. The split between `Queued` and `Loading` exists for diagnostics (long
queue times indicate scheduler starvation; long load times indicate slow I/O or large payloads).
The split between `Placeholder` and `Failed` exists because the consumer behaviour differs
sharply: a renderer renders a placeholder fine; a simulation must hard-fail on a missing
terrain heightmap.

### 3. `get()` is non-blocking

```cpp
[[nodiscard]] const T* get() const noexcept;
```

Returns:
- non-null when `state() ∈ {Ready, Placeholder}`.
- `nullptr` otherwise (`Unloaded`, `Queued`, `Loading`, `Failed`).

The pointer is stable for the lifetime of one `(id, generation)` pair. After a hot-reload swap
the consumer may continue to dereference the old pointer briefly (the old payload is kept alive
until the next `get()` re-reads the atomic), but values become stale. Idiomatic usage:

```cpp
if (const auto* mat = m_material.get(); mat != nullptr) { ... }
```

`get()` performs one acquire-load on the payload pointer. Hot-reload's swap is a release-store.
This is the standard Acquire/Release publish pattern.

### 4. `wait_ready()` is fiber-cooperative

```cpp
LoadState wait_ready();
```

Implementation:
1. Read `state()`. If terminal (`Ready`, `Placeholder`, `Failed`), return.
2. Read the load `Counter*` from the control block.
3. If `crd::jobs::is_worker_fiber()`: `crd::jobs::wait(counter, 0)` — suspends the current
   fiber, releases the OS thread back to the scheduler.
4. Otherwise: spin + `std::this_thread::yield()` until the counter hits 0.
5. Re-read and return `state()`.

The fiber path means a render-prep job that needs ten resources can issue ten `load_async` calls
and call `wait_ready` on each in turn without parking ten OS threads. The non-fiber path is
provided so test code and one-off CLI tooling work without a live `WorkerPool`.

### 5. `generation` increments on every successful hot-reload swap

```cpp
[[nodiscard]] u32 generation() const noexcept;
```

The control block holds an atomic `u32 m_generation` that starts at 0. Every successful reload
(v1f) increments it before the payload-pointer swap, then notifies subscribers. Consumers that
need to invalidate cached derived state (a renderer that pre-built a descriptor set referencing
a texture) compare `last_seen_gen` against `handle.generation()` each frame and rebuild on
change. Consumers that just dereference `get()` each access don't have to care.

### 6. `load_placeholder()` is per-loader opt-in

```cpp
class ILoader
{
public:
    [[nodiscard]] virtual void* load(const LoadContext& ctx) = 0;
    [[nodiscard]] virtual void* load_placeholder(const LoadContext& /*ctx*/) { return nullptr; }
    // ...
};
```

Default `load_placeholder()` returns `nullptr` → control block enters `LoadState::Failed`.
Loaders that want a soft fallback override the method and return a valid payload (e.g. a
texture loader returns a magenta-checker texture; a material loader returns the engine error
material). The control block then enters `LoadState::Placeholder`.

This is a per-loader policy, not an engine-wide one. Simulation-style loaders (terrain
heightmap, robot URDF) deliberately do not override and get the hard-fail behaviour. Game-style
loaders (texture, material) override and get the soft-fail behaviour. The same `ResourceManager`
hosts both.

### 7. Refcount is the eviction signal, not the eviction trigger

When refcount drops to 0 the control block is NOT immediately destroyed. The entry remains in
the handle table, marked as evictable. The 2Q eviction policy (v1g) chooses when to actually
release the payload based on memory budget. A handle re-acquired before the policy evicts
returns the same payload at the same generation; an entry re-loaded after eviction gets a fresh
control block with `generation = 0` again.

## Consequences

**Good:**
- One pointer per handle. Copies are one atomic increment. Asset-heavy structs (renderables,
  prefab instances, scene nodes) pay the same per-reference cost as raw pointers.
- `get()` is `noexcept`, branchless apart from the null check, and produces stable pointers
  between hot-reloads — most consumer code never deals with reload at all.
- `wait_ready()` from a fiber doesn't waste the OS thread; the scheduler keeps draining
  unrelated work.
- Hot-reload is transparent unless the consumer cares (in which case `generation()` and
  `subscribe_reload()` are available).
- Simulation gets hard-fail by default — a missing critical resource is not papered over with
  a placeholder.
- Game / authoring tools get soft-fail by opting in — a missing texture renders magenta, the
  app keeps running.

**Constraints:**
- Consumers MUST NOT cache the raw pointer returned by `get()` past a frame boundary if they
  care about hot-reload — they must re-call `get()` or guard with `generation()` comparison.
- The `wait_ready()` non-fiber path requires `crd::jobs::Config::num_threads >= 2` to avoid
  deadlock (if there's only one thread and it's the one waiting, no other thread can complete
  the load). Documented in the API; tests enforce.
- The control block is heap-allocated through `crd::memory::RefCounted<T>`. We pay one
  allocation per loaded resource. Acceptable: the resource itself is typically far larger than
  the control block (hundreds of bytes minimum). If profiling shows this matters we'd add a
  control-block pool.
- A handle in `LoadState::Failed` doesn't auto-retry. Consumers that want retry semantics issue
  a fresh `load_*` call.

## References

- `docs/phases/phase-2.6-resources.md`
- ADR-0014 — Reference counting split (RefCounted in `crd-memory`, RAII for resources)
- ADR-0029 — Shader hot reload (atomic-swap contract this ADR generalizes)
- ADR-0033 — `crd-jobs` (counter / wait substrate the fiber path uses)
- ADR-0036 — `crd-resources` module placement (`ILoader` registry)
- ADR-0041 — `crd-platform` async filesystem I/O (the `load_async` underlying primitive)
