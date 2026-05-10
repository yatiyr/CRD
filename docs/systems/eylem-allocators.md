# Eylem allocator strategy

> Phase 3.1 v1+ allocation contract. Establishes which `crd::memory`
> allocator each eylem subsystem uses + why. Ships as part of
> `PhysicsConfig` so consumers can swap per scene (game vs robotics
> sim vs medical visualizer have different lifetime requirements).
>
> No new allocators were created. Cerid already ships the full
> physics-allocator toolkit (linear, stack, pool, growable_pool,
> tlsf, malloc) in `engine/memory/include/crd/memory/allocators/` —
> on parity with PhysX, Jolt, Box2D, Havok. Eylem just picks the right
> one for each role.

## The five allocator roles in eylem

| Role | Lifetime | Sizing | Allocator | Owner |
|---|---|---|---|---|
| **Persistent state** (RigidBody / Collider / Joint pools, contact cache, AoSoA chunks) | scene lifetime | known max from `PhysicsConfig::max_bodies` etc. | `TlsfAllocator` (general) or `GrowablePoolAllocator` (per-type) | `PhysicsConfig::persistent_alloc` |
| **Per-frame solver scratch** (Jacobian rows, manifold buffers, broadphase pair lists, island arrays) | one `step()` call | bounded per body count | `LinearAllocator` (bump + reset) | `PhysicsConfig::solver_scratch` |
| **Per-fiber broadphase + narrow-phase scratch** (per-thread temp buffers during fan-out) | one parallel-task lifetime | small | `crd::jobs::frame_alloc()` (already shipped, lock-free, 1 MB/thread) | implicit via crd-jobs |
| **Variable-size geometry** (convex hull vertex buffers, mesh collider data, SDF slices) | scene or asset lifetime | unbounded | `TlsfAllocator` (bounded-time, fragmentation-resistant) | `PhysicsConfig::persistent_alloc` |
| **Constraint pool** (Manifold, ContactPoint, JointConstraint structs — fixed-size) | scene lifetime, recycled per frame | known struct size | `PoolAllocator` or `GrowablePoolAllocator` | scene-internal, fed by `persistent_alloc` |

## `PhysicsConfig` allocator slots

v1b adds two slots to `PhysicsConfig`:

```cpp
struct PhysicsConfig {
    // ... existing fields (gravity, fixed_dt, etc.) ...

    // Persistent state (bodies, colliders, joints, contact cache, geometry).
    // Lifetime = scene. nullptr falls back to crd::memory::default_allocator()
    // — fine for tests + tools, but production should pass a TlsfAllocator
    // sized to the worst-case scene budget.
    crd::memory::IAllocator* persistent_alloc = nullptr;

    // Per-frame solver scratch. Wiped every step() via reset(). nullptr
    // falls back to a scene-internal LinearAllocator sized from the
    // max_bodies hint. Production can pass an externally-managed one
    // (e.g. shared across the scene + the renderer's per-frame arena).
    crd::memory::LinearAllocator* solver_scratch = nullptr;
};
```

Per-fiber scratch uses `crd::jobs::frame_alloc(size, alignment)` which is
already wired to crd-jobs's per-thread arena (1 MB/thread default,
lock-free, O(1) per ADR-0033 + the v0e session log). Eylem fan-out tasks
just call it; no PhysicsConfig field needed.

## Why these specific allocators

### `TlsfAllocator` for persistent state

Two-Level Segregated Fit (Masmano et al. 2008) — **the canonical
real-time allocator**. Used by Sony, Naughty Dog (TressFX), Bullet
optionally, anything that needs sub-frame budget guarantees with
unbounded allocation patterns.

- O(1) allocate, deallocate, coalesce
- Bounded internal fragmentation
- Same-pool allocations (RigidBody + Collider + ConvexHull verts) keep
  related data spatially close, helping cache
- Dealloc is cheap → safe to use for body removal mid-frame

Eylem persistent allocations are unpredictable in size (Material is 20
B, RigidBody is 80 B, ConvexHull verts is N×12 B for arbitrary N) — TLSF
handles all cases at one allocator.

### `LinearAllocator` for solver scratch

Bump pointer, no per-allocation overhead, `reset()` wipes everything in
O(1). The standard pattern for per-frame allocations across the entire
games industry: Frostbite "frame arena," PhysX `PxScratchBuffer`, Jolt
`TempAllocator`, Unity DOTS `Allocator.TempJob`.

Solver scratch is the right fit because:
- Lifetime is exactly one `step()` call
- Allocation pattern is monotone-grow within the step
- All allocations die together at end of step
- No need for individual frees → bump is optimal

`LinearAllocator` ships with two flavours:
- Fixed pre-allocated block (predictable; production)
- Growable pages (convenient; tests)

Eylem v1b picks fixed pre-allocated for production paths, growable for
tests.

### `crd::jobs::frame_alloc()` for per-fiber scratch

Already exists per ADR-0033 + crd-jobs `frame_alloc_bytes` config.
Lock-free, O(1), per-thread arena. Eylem's broadphase fan-out (v1c+
parallel pair queries) and narrow-phase fan-out (v1d+ parallel GJK)
just call `crd::jobs::frame_alloc(N, 32)` for thread-local temporaries.
`crd::jobs::frame_reset()` is called once per top-level frame (by the
application, not eylem) and wipes all per-thread arenas.

This is exactly the pattern Jolt's `JobSystem` allocator uses internally.

### `PoolAllocator` for constraint cache

Per-pair contact manifolds and per-joint constraint records are
sizeof-stable POD structs allocated/freed together. `PoolAllocator`
gives O(1) intrusive-free-list alloc + free, with no fragmentation
because all slots are the same size.

`GrowablePoolAllocator` is the variant when the upper bound isn't
known a priori (sandbox / dynamic scene).

## Industry parity check

| Engine | Persistent | Solver scratch | Per-fiber | Constraints |
|---|---|---|---|---|
| **PhysX 5** | `PxAllocatorCallback` (heap) | `PxScratchBuffer` passed to `simulate()` | per-task scratch | per-type pools |
| **Jolt** | heap allocator | `TempAllocator` (ring) | n/a (single threaded inside step) | block alloc |
| **Box2D v3** | heap | `b2StackAllocator` | n/a | `b2BlockAllocator` |
| **Havok** | `hkMemoryAllocator` (TLSF-class) | per-task scratch | per-task scratch | type pools |
| **Bullet** | `btAlignedAllocator` (default malloc + 16B align) | n/a (no fan-out) | n/a | per-type pools |
| **Cerid eylem** | `TlsfAllocator` | `LinearAllocator` | `crd::jobs::frame_alloc` | `(Growable)PoolAllocator` |

We have direct parity with the elite tier. PhysX `PxScratchBuffer` is
exactly our `LinearAllocator`; Jolt `TempAllocator` is the same idea;
Box2D `b2StackAllocator` is `LinearAllocator` (or `StackAllocator` if
nested LIFO is needed); `b2BlockAllocator` is our `PoolAllocator`. The
only thing we have that they don't is a **fully-jobified per-fiber
arena that's lock-free** — that's a Cerid-side advantage from `crd-jobs`.

## What v1a does (this slice)

Nothing. The interface module ships with no allocator slots — bodies /
colliders / joints in the v1a `NullPhysicsScene` go through
`default_allocator()` (malloc) for simplicity. The slots ship in v1b
when storage actually matters.

## What v1b adds

1. `PhysicsConfig::persistent_alloc` + `solver_scratch` fields.
2. `crd-eylem-rigid3d` impl uses them: TLSF for persistent storage,
   LinearAllocator for solver scratch, PoolAllocator for the
   contact-manifold cache, frame_alloc for per-fiber scratch in
   broadphase + narrow phase fan-out.
3. Tests verify allocator-injection works (`make_rigid3d_scene(cfg)`
   round-trips a custom allocator through to body storage).
4. Sandbox demo (v1k) shows wiring a real TLSF pool sized to the demo's
   1k-body scene.

## What v1+ adds

- **v1c broadphase**: AABB tree nodes go through `persistent_alloc`
  (TLSF or PoolAllocator); per-step pair list uses `solver_scratch`.
- **v1d narrow phase**: GJK simplex stack uses per-fiber `frame_alloc`;
  manifold cache hashed by feature pair uses `PoolAllocator`.
- **v1e SI solver**: Jacobian rows, position-correction buffer, island
  scratch all use `solver_scratch` (LinearAllocator wins on bump-only
  pattern).
- **v1g islands**: union-find scratch + island arrays use
  `solver_scratch`.
- **v1j snapshot**: serialised buffer uses `persistent_alloc` (lifetime
  outlives the step).

## References

- `engine/memory/include/crd/memory/allocators/` — the substrate
- ADR-0003 — Memory v1 (allocator interface contract)
- ADR-0033 — `crd-jobs` (per-fiber arena via `frame_alloc`)
- ADR-0062 — Eylem architecture (this doc realises §10 "memory model")
- ADR-0063 — Eylem determinism contract (allocator choice doesn't
  affect determinism — bit-exact replay holds across allocator swaps,
  because pool indices are not exposed in the public API)
- v0e session log (mentions `frame_alloc` lock-free guarantee)
- Industry: PhysX `PxScratchBuffer`, Jolt `TempAllocator`,
  Box2D `b2BlockAllocator`, Havok `hkMemoryAllocator`,
  Masmano et al. 2008 (TLSF paper)
