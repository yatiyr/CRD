# Session — 2026-05-11 — v1b-a — eylem rigid-3D BodyPool AoSoA-8

## Goal

Ship v1b-a per the locked Phase 3.1 v1 plan: `crd-eylem-rigid3d` peer
module + extended `PhysicsConfig` allocators + first real consumer of v0
SIMD substrate via `BodyPool` AoSoA-8 storage. First step toward "first
eylem entity visible in the debug renderer".

## What we built / changed

- **`engine/eylem-rigid3d/`** — new peer module (sibling of `crd-eylem`
  interface). Linked PUBLIC against `crd-core` + `crd-memory` +
  `crd-containers` + `crd-math` + `crd-eylem`.
- **`engine/eylem/include/crd/eylem/physics_config.hpp`** — extended
  `PhysicsConfig` with `persistent_alloc` (TLSF expected; scene-lifetime
  body / collider / joint pools + persistent contact cache + variable-
  size geometry) + `solver_scratch` (LinearAllocator expected; per-step
  bump arena reset every step). Both typed as `IAllocator*` for
  test/tool flexibility; doc explains the contract.
- **`engine/eylem-rigid3d/include/crd/eylem_rigid3d/body_pool.hpp`** +
  **`src/body_pool.cpp`** — `BodyPool` AoSoA-(8 on AVX2, 4 elsewhere)
  over `crd::math::simd::Soa<BodyChunk, kLane>`:
  - **19 SIMD float columns per tile** (`pos.{xyz}`, `rot.{xyzw}`,
    `linvel.{xyz}`, `angvel.{xyz}`, `inv_mass`, `inv_inertia.{xyz}`,
    `linear_damping`, `angular_damping`)
  - **Per-lane integer side-bands** (`flags[Lane]`, `generation[Lane]`,
    `live[Lane]`) in the same chunk for cache locality
  - **Free-list reclaim** (LIFO; slot 0 reserved as null sentinel)
  - **Generation-bumped `BodyId`** (`[gen:8|idx:24]`) — stale handles
    fail `contains()` after re-insert
  - Capacity-clamped at `min(max_bodies, 16M-1)`; insert returns
    `BodyId::null()` on exhaustion (no silent growth — would invalidate
    solver pointers mid-step)
  - Public AoS contract: `read(id) → RigidBody`, `write(id, body)`, plus
    chunk-direct `storage()` for the v1c+ broadphase / v1e+ solver hot
    path
  - `resolve(id) → {chunk_idx, lane_idx}` for SIMD lookups
- **`engine/eylem-rigid3d/include/crd/eylem_rigid3d/eylem_rigid3d.hpp`** —
  module umbrella header.
- **`engine/eylem-rigid3d/CMakeLists.txt`** — module build rules; added
  to root CMakeLists `add_subdirectory` list.
- **`tests/eylem-rigid3d/test_body_pool.cpp`** + CMakeLists — 8 test
  cases, 140 assertions: empty state, round-trip every public field,
  generation bump on remove, free-list reuse, write-in-place, AoSoA
  chunk-growth, deterministic handle sequence, capacity exhaustion,
  nullptr fallback.
- **`docs/systems/eylem-allocators.md`** — extended with v1b-a status
  block + corrected `solver_scratch` typing note (the doc previously
  said `LinearAllocator*`; impl uses `IAllocator*` for test flexibility,
  with `LinearAllocator` the expected concrete production type).

## Plain-English explanation

`BodyPool` is the storage layer for rigid bodies. Bodies live in
"tiles" of 8 (on AVX2) or 4 (elsewhere) — each tile is a struct of
SIMD-aligned columns (8 positions, 8 velocities, etc.) so the
broadphase / solver can iterate them with one SIMD load per column.
This is the AoSoA pattern — Array-of-Structs-of-Arrays — picked over
plain AoS (per-element gather) because v1c+ broadphase + v1e+ solver
both iterate dense packed columns.

Bodies are addressed by `BodyId` — a 32-bit handle packed as
generation:8 + index:24. The pool stores per-slot generation alongside
the live float columns; removed bodies bump generation, so a stale
`BodyId` fails `contains()` even if a new body recycles its slot.

The pool integrates with eylem's allocator strategy (TLSF for
persistent state, LinearAllocator for per-step scratch). Capacity is
fixed at construction — no reallocation mid-step would invalidate
solver pointers; if exhausted, `insert` returns null and the caller
must handle it.

This is the foundation for v1b-b (ColliderPool, same pattern), v1b-c
(EylemSystem integration into the ECS schedule), and v1c+ (broadphase
+ solver consume `storage()` directly).

## Decisions made

- **AoSoA-8 over AoS or AoSoA-N (other widths)** — matches v0 SIMD
  substrate's `Soa<TChunk, k_native_lane_width>` template; AVX2
  production builds get Lane=8, scalar fallback (`win-debug-scalar`)
  gets Lane=4. Bullet's btSoftBody-style SoA + PhysX's PxgPostSolveContact
  lane packing both validate the choice.
- **Free-list reclaim with generation bump (no compaction)** — the
  broadphase's persistent contact cache + solver's island bookkeeping
  both want stable slot indices across frames. Compaction (if ever
  needed) reserved for v1l defrag pass.
- **Capacity FIXED at construction** — `PhysicsConfig::max_bodies`
  caps. Silent growth would invalidate solver pointers mid-step.
- **`generation = 0` reserved as "never allocated"** — generation
  wraps `0xFF → 1`, never returns to 0. Default-init `BodyId` (raw=0)
  fails `contains()` cleanly because slot 0 is never live.

## Files touched

- `engine/eylem-rigid3d/CMakeLists.txt` — created
- `engine/eylem-rigid3d/include/crd/eylem_rigid3d/body_pool.hpp` — created (~165 LOC)
- `engine/eylem-rigid3d/include/crd/eylem_rigid3d/eylem_rigid3d.hpp` — created (umbrella)
- `engine/eylem-rigid3d/src/body_pool.cpp` — created (~245 LOC)
- `engine/eylem/include/crd/eylem/physics_config.hpp` — added `persistent_alloc` + `solver_scratch` fields
- `tests/eylem-rigid3d/CMakeLists.txt` — created
- `tests/eylem-rigid3d/test_body_pool.cpp` — created (8 cases, 140 assertions)
- `tests/CMakeLists.txt` — added `add_subdirectory(eylem-rigid3d)`
- `CMakeLists.txt` — added `add_subdirectory(engine/eylem-rigid3d)`
- `docs/systems/eylem-allocators.md` — v1b-a status block + IAllocator typing correction

## Tests / verification

- Built? ✅ `crd-eylem-rigid3d` + `crd-eylem-rigid3d-tests` clean
- Tests pass? **8/8 cases, 140 assertions** (`crd-eylem-rigid3d-tests`)
- Whole engine: 1014/1014 ctest pass (was 1006 + 8 new BodyPool tests)
- Sweep cadence: win-debug only this slice; full sweep batched at v1b
  cluster end per project policy.

## Next session starts with

- v1b-b: `ColliderPool` per shape kind (Sphere / Box / Capsule),
  same AoSoA pattern + `ColliderId` encoding `[kind:4 | per_kind_idx:20]`.
