# Session — 2026-05-11 — v1a-material-b — MaterialPool + IPhysicsScene material API

## Goal

Per ADR-0069 §3 + §11, land the scene-owned MaterialPool and the four
public material methods on IPhysicsScene that v1a-material-c (per-collider
`Collider::material` field) and v1a-material-d (mass derivation) consume.
Sub-slice b in the v1a-material cluster (a → b → c → d → cluster close +
14-config sweep). Pool exists as a free-standing class in `crd-eylem` so
every IPhysicsScene impl reuses it (NullPhysicsScene now, eylem-rigid3d
Scene at v1b-c, cooker handlers at v1k).

## What we built / changed

- **`engine/eylem/include/crd/eylem/material_pool.hpp`** — new class
  `MaterialPool`:
  - Slot 0 = null sentinel (never read), slot 1 = `default_material()`
    auto-allocated at construction, slot 2..N = user materials
  - `kIndexMax = (1<<24) - 1` — matches the 24-bit `MaterialId.index()` field
  - API: `insert(const Material&) → MaterialId`, `update(id, m) noexcept`,
    `get(id) → const Material&` (falls back to default for invalid id),
    `contains(id)`, `size()` (excludes slot 0; includes default), `capacity()`,
    `all() → ConstSpan<Material>` for cookers / debug viz
  - v1: every live slot has generation = 1 (no remove path; materials are
    scene-lifetime). Future remove uses the standard generation-bump pattern;
    read API stays stable.
  - Determinism: append-only; `MaterialId.index()` == slot position; identical
    insert sequences across runs produce identical IDs (the cooker's content-
    addressed FNV-1a-64 path per ADR-0067 §3 builds on this).

- **`engine/eylem/src/material_pool.cpp`** — implementation. Reserve(2),
  push slot-0 sentinel + push slot-1 default at construction.

- **`engine/eylem/include/crd/eylem/physics_scene.hpp`** — added 4
  pure-virtual methods to `IPhysicsScene`:
  - `[[nodiscard]] virtual MaterialId      create_material(const Material& m)`
  - `virtual void                          update_material(MaterialId id, const Material& m) noexcept`
  - `[[nodiscard]] virtual const Material& material(MaterialId id) const noexcept`
  - `[[nodiscard]] virtual bool            has_material(MaterialId id) const noexcept`

- **`engine/eylem/src/null_physics_scene.cpp`** — added `MaterialPool m_materials`
  member + 4 method impls that thin-wrap the pool.

- **`engine/eylem/include/crd/eylem/eylem.hpp`** — added `material_pool.hpp`
  to umbrella include list.

- **`tests/eylem/test_v1a_interface.cpp`** — 4 new TEST_CASEs / 32 new assertions:
  - MaterialPool default-allocates slot 1 + null id falls back to default
  - MaterialPool insert / update round-trips (with stable handle through update)
  - MaterialPool determinism: two pools given identical insert sequences produce
    identical MaterialIds
  - NullPhysicsScene exposes MaterialPool API (create_material / update_material /
    material(id) / has_material(id))

## Plain-English explanation

Every contact in the simulator needs a `Material` — friction, restitution,
density, surface velocity. v1a-material-a froze the 64-byte struct itself.
This sub-slice ships its **storage**: a per-scene `MaterialPool` that hands
out 4-byte `MaterialId` handles. Bodies / colliders reference materials by
handle (not by-value or by-pointer), which keeps the AoSoA-8 collider
storage compact and lets the material live at one address regardless of
how many colliders share it.

The pool is **append-only in v1**: we never remove a material because there
are usually only a handful per scene (the 8 shipped catalog materials per
ADR-0069 §6 plus a few project-specific entries). If a future need for
remove appears, it lands later via the standard generation-bump pattern;
the read API stays stable.

Slot 1 is **always** `default_material()` — a 0.5/0.5 friction, 0
restitution, 1000 kg/m³ water-density material. Calling
`scene->material(MaterialId::default_material())` works on any freshly
constructed scene. Invalid IDs (null / out-of-range / stale generation)
also resolve to the default via `material(id)` — callers do not need to
null-check on the read path. This is the same "graceful read fallback"
pattern as `World::get_component()` in crd-scene.

Determinism is guaranteed by construction: insert order is preserved,
indices are dense, no `std::sort`, no map lookups. Two pools given the
same `insert` sequence produce the same `MaterialId.index()` for each
input — the basis for the v1k cooker's content-addressed material
deduplication.

## Decisions made

- **Append-only in v1; no remove.** Materials are scene-lifetime (a
  handful per scene). Remove adds complexity (generation bumping, free
  list) for a workflow that doesn't exist today. Locked to fast-path
  semantics: `MaterialId.index()` == slot position, no sparse mapping.
- **`get(invalid_id)` falls back to slot 1 (default)**, mirroring the
  `World::get_component()` pattern. Avoids forcing every read site to
  branch on `has_material(id)` first.
- **Slot 1 lives at construction time**, so `MaterialId::default_material()`
  always resolves regardless of whether the scene author has called
  `create_material` for anything else.
- **`size()` excludes slot 0 but includes slot 1**: user-visible count =
  default + user materials. Matches what an editor "Materials" panel shows.
- **`MaterialPool` lives in `crd-eylem`**, not `crd-eylem-rigid3d`, because
  every IPhysicsScene impl reuses it. Same reasoning as `Collider` /
  `RigidBody` / `Joint` POD types.

## Files touched

- `engine/eylem/include/crd/eylem/material_pool.hpp` — new (82 LOC)
- `engine/eylem/src/material_pool.cpp` — new (74 LOC)
- `engine/eylem/include/crd/eylem/physics_scene.hpp` — +18 LOC (4 virtuals + doc)
- `engine/eylem/src/null_physics_scene.cpp` — +24 LOC (member + 4 impls + include)
- `engine/eylem/include/crd/eylem/eylem.hpp` — +1 line (umbrella include)
- `tests/eylem/test_v1a_interface.cpp` — +112 LOC (4 cases / 32 assertions)

## Tests / verification

- Built: ✅ `crd-eylem` + `crd-eylem-tests` clean (zero warnings under `/W4 /WX`).
- `crd-eylem-tests`: **193 assertions across 23 test cases pass** (was 161/19; +32/+4).
- `ctest --preset win-debug`: **1033/1033 pass** (was 1029; +4 from new
  material-pool + scene cases). `crd-simd-emission-check` requires `dumpbin`
  on PATH — passes when invoked from a vcvars shell, as expected per
  the project's existing CI guard topology.
- Sweep cadence: win-debug only this sub-slice; full 14-config sweep
  batched at v1a-material cluster close (after v1a-material-c + v1a-material-d).

## Decision deltas vs ADR-0069

- ADR-0069 §11 specified the four-method API (`create_material` /
  `update_material` / `material(id)` / `has_material(id)`); shipped exactly
  as specified. No deviation.
- Default-fallback semantic for `material(invalid_id)` is the v1
  refinement (ADR doesn't pin invalid-id behaviour either way); chose
  the safe-read-without-branch pattern for solver hot paths.

## Next session starts with

- **v1a-material-c**: per-collider `MaterialId material` field on
  `Collider` POD. Update `add_collider(BodyId, const Collider&, const
  Material&)` overload to internally `create_material(material)` then
  store the resulting MaterialId on the collider. Add a sister overload
  `add_collider(BodyId, const Collider&)` that reads `Collider::material`
  directly (the typical scene-load path: cooker pre-allocates materials
  via `create_material`, then references them by id when streaming
  colliders). ~50 LOC + ~3 tests.
