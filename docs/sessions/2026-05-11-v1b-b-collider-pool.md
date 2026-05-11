# Session — 2026-05-11 — v1b-b — eylem rigid-3D ColliderPool (3 kinds)

## Goal

Ship v1b-b: per-kind AoSoA-8 `ColliderPool` for Sphere / Box / Capsule
(the 3 shapes the v1b sandbox demo + v1b-d viz will use). Lock the
`ColliderId` encoding so the v1c+ broadphase can dispatch by kind in
O(1) without a global record table. Confirm: ConvexHull + Plane (and
later TriangleMesh / Heightfield / Sdf per ADR-0062 §4.5) **deferred to
v1d** when their consumers (GJK + plane raycast) ship — pools without
consumers are dead storage per the quality bar.

## What we built / changed

- **`engine/eylem-rigid3d/include/crd/eylem_rigid3d/collider_pool.hpp`** +
  **`src/collider_pool.cpp`**:
  - `ColliderPool` class — three per-kind AoSoA pools internally
    (`SphereColliderPool` / `BoxColliderPool` / `CapsuleColliderPool`),
    each its own `Soa<KindChunk, kLane>` over kind-specific column layouts
  - **`ColliderId` index field encodes routing**: `[kind:3 | per_kind_idx:21]`
    — top 3 bits select the shape kind; bottom 21 bits are the per-kind
    pool's local index. **Lookup is O(1) without any global record table** —
    the ID itself dispatches to the right pool. (Bumped to `[kind:4 |
    per_kind_idx:20]` later in the day during the five-category collider
    decision; see ADR-0062 §4.5 + Wave 1 ADR work.)
  - Per-kind chunks store: common columns (`local_pos.{xyz}`,
    `local_rot.{xyzw}`) + kind-specific columns (sphere = `radius`;
    box = `half_{xyz}`; capsule = `radius` + `half_height`) + per-lane
    integer side-bands (`body_idx[Lane]`, `generation[Lane]`,
    `live[Lane]`)
  - **Free-list reclaim per kind**, generation bump on reuse — same
    pattern as `BodyPool` (v1b-a)
  - `body_of(id)` returns the full `BodyId` (raw 32 bits including
    generation) — colliders track their owning body
  - `sphere_storage()` / `box_storage()` / `capsule_storage()` accessors
    expose the SoA chunks for v1c+ broadphase iteration
  - **ConvexHull + Plane explicitly unsupported in v1b-b** — `insert()`
    returns `ColliderId::null()` with a `CRD_LOG_ERROR`. They land in
    v1d alongside their consumers. Same loud-failure discipline:
    "stub targets are not integration".
- **`engine/eylem-rigid3d/include/crd/eylem_rigid3d/eylem_rigid3d.hpp`** —
  umbrella now includes `collider_pool.hpp`.
- **`engine/eylem-rigid3d/CMakeLists.txt`** — added `crd-log` PRIVATE dep
  for the unsupported-shape error logging.
- **`tests/eylem-rigid3d/test_collider_pool.cpp`** + CMakeLists update —
  11 new test cases / 75 new assertions: empty state, encoding round-trip,
  insert+read each kind, per-kind independence, generation bump on
  remove+reinsert, capacity exhaustion (per-kind independent),
  ConvexHull+Plane explicit-null, null-body rejection, deterministic
  handle sequence.

## Plain-English explanation

`ColliderPool` is the storage layer for collider shapes. Where
`BodyPool` had one big tile of body state, `ColliderPool` has THREE
per-kind tiles — one for spheres, one for boxes, one for capsules.
Each kind tile stores only the columns relevant to that shape (sphere
needs only `radius`; box needs `half_extents.{xyz}`; capsule needs
`radius + half_height`). Memory savings + cache-friendly per-kind
iteration for v1c+ broadphase.

Different shapes go into different pools. `ColliderId` encodes which
pool a collider lives in via the top bits of its index — no extra
lookup table needed. v1c+ broadphase iterates each pool independently
(per-kind code path is more SIMD-friendly than dispatching per element).

Each collider tracks the `BodyId` of its owning body — so when contact
generation finds a contact between two colliders, it can hand a
complete `BodyId` to the contact-cache without a second lookup.

ConvexHull / Plane / TriangleMesh / Heightfield / Sdf are NOT in this
slice. Per the quality bar, "stub targets are not integration" — the
unsupported kinds return `ColliderId::null()` from `insert()` with a
loud error log, and ship in their proper slices (v1d for ConvexHull +
Plane; v1d-mesh for TriangleMesh; v1d-hf for Heightfield; Phase 3.1.5
for Sdf).

## Decisions made

- **Per-kind storage (3 pools), not one union pool** — per-kind
  iteration from v1c+ broadphase reads only the columns it needs and
  avoids per-element type dispatch. PhysX `PxgShape*` family + Bullet's
  per-type hash-pool layout both validate.
- **`ColliderId` encoding `[kind:N | per_kind_idx]`** — handle
  dispatches; no global record table; O(1) lookup. Originally 3 bits
  for kind (8 kinds, exactly the 5 industry-standard + 3 reserved) but
  bumped to 4 bits / 20 per-kind during the same-day five-category
  collider lock so 16 shape kinds reservable + 1M colliders/kind.
- **`body_idx[lane]` stores full `BodyId.raw`** (index + generation)
  not just index — saves a lookup in the contact-cache hot path.
- **Loud failure for unsupported shapes** — explicit error log per
  shape category ("ConvexHull not supported until v1d (GJK+EPA)";
  "TriangleMesh not supported until v1d-mesh"; etc.) so demos relying
  on them surface immediately rather than degrading silently.
- **Per-kind capacity is independent** — sphere pool exhaustion does
  NOT block box inserts; verified by test #8.

## Files touched

- `engine/eylem-rigid3d/include/crd/eylem_rigid3d/collider_pool.hpp` — created (~135 LOC)
- `engine/eylem-rigid3d/src/collider_pool.cpp` — created (~225 LOC)
- `engine/eylem-rigid3d/include/crd/eylem_rigid3d/eylem_rigid3d.hpp` — added `collider_pool.hpp` include
- `engine/eylem-rigid3d/CMakeLists.txt` — added `crd-log` PRIVATE dep
- `tests/eylem-rigid3d/test_collider_pool.cpp` — created (11 cases, 75 assertions)
- `tests/eylem-rigid3d/CMakeLists.txt` — added `test_collider_pool.cpp` to executable

## Tests / verification

- Built? ✅ `crd-eylem-rigid3d` clean
- Tests pass? **19/19 cases, 215 assertions** (8 BodyPool + 11 ColliderPool, all in `crd-eylem-rigid3d-tests`)
- Sweep cadence: win-debug only; full sweep batched at end of v1b cluster.

## Next session starts with

- Architectural cluster: lock the multi-domain ADRs that emerged from
  user prompts ("what about complex mesh colliders?" → ADR-0062 §4.5
  five-category lock; "what about force fields?" → ADR-0067; "what
  about body types + collision filtering + callbacks?" → ADR-0068).
- After ADRs land: v1a-material-a (Material API surface) + v1b-c
  (`RigidBodyComponent` + `EylemSystem`).
