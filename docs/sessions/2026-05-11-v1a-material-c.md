# Session — 2026-05-11 — v1a-material-c — per-collider Collider::material handle

## Goal

Ship the per-collider material attachment surface per ADR-0069 §3 + §11.
Sub-slice c in the v1a-material cluster (a → b → c → d → cluster close +
14-config sweep). Refactors `add_collider` so the canonical contract
takes a `Collider` whose `material` field is a `MaterialId` (the typical
scene-load path: cooker pre-allocates materials, scene loader streams
colliders), with a non-virtual convenience overload preserved for
runtime authoring code that wants to inline both in one call.

## What we built / changed

- **`engine/eylem/include/crd/eylem/collider.hpp`** — added `MaterialId
  material = MaterialId::default_material()` field to `Collider`. Default
  resolves on any freshly constructed collider (slot 1 in the scene's
  pool is always allocated). Doc explains the cooker → loader → scene
  pre-allocation pattern and the in-class slot ordering (`shape, flags,
  material, local_position, local_rotation, union`).

- **`engine/eylem/include/crd/eylem/physics_scene.hpp`** — refactored
  `add_collider` from one 3-arg pure virtual to:
  - **2-arg pure virtual** `add_collider(BodyId, const Collider&)` — the
    canonical contract; reads `Collider::material` directly. This is what
    the scene loader calls.
  - **3-arg non-virtual convenience** `add_collider(BodyId, const Collider&,
    const Material&)` — internally `create_material(material)` then forwards
    to the canonical 2-arg path with a copy whose `material` field has been
    overwritten. Every impl gets identical semantics for free; no override
    required.

- **`engine/eylem/src/null_physics_scene.cpp`** —
  - Added `using IPhysicsScene::add_collider;` to bring the convenience
    overload into the class scope (otherwise the override of the 2-arg
    hides it per C++ name-lookup rules).
  - Override of `add_collider` now takes `(BodyId, const Collider&)`.
  - `StoredCollider` no longer holds inline `Material` — the field lives
    in `m_materials` (the scene's MaterialPool) and is referenced by
    `Collider::material` (a 4-byte handle).

- **`tests/eylem/test_v1a_interface.cpp`** — 3 new TEST_CASEs / 13 new
  assertions:
  - Collider's default material resolves to `MaterialId::default_material()`
    (slot 1, generation 1, not null)
  - 2-arg `add_collider` reads pre-set `Collider::material` and the pool
    slot survives round-trip
  - 3-arg `add_collider` convenience allocates a new pool slot (index 2,
    generation 1) and the resulting material is queryable via the scene's
    `material(id)` API

## Plain-English explanation

Before this slice, `Collider` had no notion of material — every call to
`add_collider` had to pass a `Material` by value, and `NullPhysicsScene`
stored that material inline alongside the collider. That model doesn't
match the cooker pipeline: a cooker writes colliders + materials to disk
**separately** (because many colliders share the same material — think
all the metal pipes in a level reference one steel material). The loader
streams them in, references them by handle.

So now `Collider` has a 4-byte `MaterialId material` field. The canonical
`add_collider(body, collider)` overload reads it directly. The 3-arg
convenience overload is preserved as a non-virtual default that calls
`create_material(material)` then sets the resulting handle on a copy of
`collider` — so existing call sites and new authoring code that wants
to inline both in one call still works exactly as before.

The 3-arg overload becoming non-virtual is the standard "non-virtual
interface" pattern — every IPhysicsScene impl gets identical semantics
for the convenience overload for free; only the 2-arg canonical path
needs an override. NullPhysicsScene needs `using IPhysicsScene::
add_collider;` to expose the inherited convenience overload (otherwise
C++ hides all overloads of the same name in the base when the derived
declares any).

## Decisions made

- **Canonical contract is the 2-arg overload** (collider already has its
  MaterialId set). This matches the cooker → loader path which is the
  hot scene-load case. The 3-arg convenience is for runtime authoring.
- **Default-constructed Collider has `material = MaterialId::default_material()`**.
  Means a freshly authored Collider is immediately valid against any
  scene's MaterialPool (slot 1 always exists per v1a-material-b). Avoids
  the "NULL material" trap that catches developers who forget to
  initialise the field.
- **`StoredCollider` drops the inline `Material`**. Material lives in
  exactly one place: the scene's MaterialPool. Two colliders sharing the
  same material reference the same pool slot — what `MaterialId` is for.
- **`using IPhysicsScene::add_collider;` in NullPhysicsScene**. Required
  C++ idiom to expose the inherited 3-arg convenience overload alongside
  the 2-arg override. Not over-thinking — it's the documented pattern.
- **Did NOT bump or pin `sizeof(Collider)`**. Layout is implementation
  detail at v1a (only the union variants are pinned). v1b's ColliderPool
  AoSoA-8 storage will choose its own per-field columns; the public
  Collider POD is a transit type at v1a, not a storage type.

## Files touched

- `engine/eylem/include/crd/eylem/collider.hpp` — +14 LOC (MaterialId field + doc)
- `engine/eylem/include/crd/eylem/physics_scene.hpp` — net +20 LOC (canonical 2-arg + non-virtual 3-arg + doc)
- `engine/eylem/src/null_physics_scene.cpp` — net +3 LOC (using-declaration + 2-arg signature + StoredCollider trim)
- `tests/eylem/test_v1a_interface.cpp` — +75 LOC (3 cases / 13 assertions)

## Tests / verification

- Built: ✅ `crd-eylem` + `crd-eylem-rigid3d` + their tests clean
  (zero warnings under `/W4 /WX`). The Collider field rippled through
  the rigid3d ColliderPool obj rebuild without any signature change
  (pool stores its own AoSoA columns; doesn't read the public Collider
  layout).
- `crd-eylem-tests`: **206 assertions across 26 test cases pass**
  (was 193/23; +13 / +3).
- `ctest --preset win-debug`: **1036/1036 pass** (was 1033; +3 from new
  v1a-material-c cases).
- Sweep cadence: win-debug only this sub-slice; full 14-config sweep
  batched at v1a-material cluster close (after v1a-material-d).

## Decision deltas vs ADR-0069

- ADR-0069 §3 specified per-collider `MaterialId material` — shipped
  exactly as specified.
- ADR-0069 §11 specified "scene loader references pre-allocated
  materials by handle" — shipped via the 2-arg canonical add_collider
  overload as planned.
- Refinement (NOT in ADR): default field value = `MaterialId::default_material()`.
  Keeps freshly authored Colliders immediately valid; trivial constexpr
  default. Documented in the Collider doc block.
- Refinement (NOT in ADR): 3-arg convenience overload as non-virtual.
  Standard NVI pattern; ADR didn't pin virtual vs non-virtual either way.
  Reduces per-impl boilerplate (convenience semantics are identical
  across all IPhysicsScene impls).

## Next session starts with

- **v1a-material-d**: mass derivation per ADR-0069 §3. Implements
  `IPhysicsScene::derive_body_mass(BodyId)` that walks the body's
  colliders in **ColliderId-stable order** (deterministic by construction
  per ADR-0063), computes each collider's volume (analytic for sphere /
  box / capsule; bbox-volume × convex-fraction for hull; user-supplied
  for triangle mesh / heightfield / sdf), reads each collider's material
  density via `Collider::material → MaterialPool::get → density`, and
  produces the body's total mass + COM offset + inertia tensor diagonal.
  ~120 LOC + ~5 tests. Closes the v1a-material cluster; full 14-config
  sweep follows.
