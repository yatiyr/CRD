# 2026-05-07 — Phase 3.0 v1f: Relations (six built-ins)

**Status at start:** Phase 3.0 v1e shipped earlier the same day. `World::for_each_chunk` mixed-backend visitor in place, both L2 backends (Archetype + SparseSet) routed by `StorageHint`, sink fan-out consolidated through World. Scene tests 113 / 34450, six-config 629/629.

**Status at end:** v1f shipped. Relations are a first-class L3 layer with six built-in tag types (`ChildOf`, `AttachedTo`, `Owns`, `Targets`, `DependsOn`, `PossessedBy`), three opt-in traits (`ReverseIndex`, `Acyclic`, `OnTargetDestroyed`), and an iterative destruction worklist that handles 500-deep cascade trees without stack overflow. Six-config 651/651 / 648 release / 17 smokes. Scene tests 135 / 34520.

---

## Goal of this session

Land Layer 3 of the eight-layer architecture (ADR-0051): relations as components-with-target-payload, opt-in per-relation invariants, and the destruction-cascade machinery that makes "destroy parent → destroy subtree" a one-line API.

Per phase doc:
> v1f — Relations (~400 LOC + tests)
> `Relation<Tag>` template + `add_relation` / `remove_relation` / `get_relation_target` / `traverse_relation`. Built-in `ChildOf`, `AttachedTo`. Reverse-index trait + `Acyclic` debug assertion + `OnTargetDestroyed` policy.

Mid-session decision (during planning): expand the built-in set from two to **six**. The user asked for elite-engine breadth; the chosen set covers every (storage × acyclic × policy) combination that occurs in real engine work.

## What shipped

### New module files

```
engine/scene/include/crd/scene/relation.hpp   ~110 LOC — Relation<Tag> + traits + 6 built-in tag structs
tests/scene/test_relations.cpp                ~440 LOC — 22 cases
```

### Modified

- `engine/scene/include/crd/scene/component.hpp`
  - `ComponentInfo` extended with five relation fields:
    - `bool is_relation` (auto-stamped when T = Relation<Tag> via `is_relation_instance_v`)
    - `bool acyclic` (set by `Acyclic{}` trait)
    - `bool has_reverse_index` (set by `ReverseIndex{}` trait)
    - `bool has_on_target_destroyed` (set by `OnTargetDestroyed{...}` trait)
    - `crd::u8 on_target_destroyed_policy` (raw enum value)
  - All five default to false / 0 — non-relation components pay zero tax.

- `engine/scene/include/crd/scene/component_registry.hpp`
  - New trait dispatchers: `apply_trait(info, ReverseIndex)`, `apply_trait(info, Acyclic)`, `apply_trait(info, OnTargetDestroyed)`.
  - New `IsRelationInstance<T>` / `is_relation_instance_v<T>` template that detects `Relation<Tag>` instantiations.
  - `register_type<T>(traits...)` stamps `info.is_relation = true` when T is a relation instance.

- `engine/scene/include/crd/scene/entity.hpp`
  - Added `crd::containers::DefaultHash<crd::scene::EntityId>` specialisation so `HashMap<EntityId, V>` works without bespoke hash predicates at every call site. Mixes the 64-bit raw representation through `hash_u64`.

- `engine/scene/include/crd/scene/world.hpp`
  - New public template API: `register_relation<Tag>(traits...)`, `add_relation<Tag>(src, target)`, `remove_relation<Tag>(src)`, `get_relation_target<Tag>(src)`, `has_relation<Tag>(src)`, `would_form_cycle<Tag>(src, target)`, `traverse_relation<Tag>(root, visitor)`, `relation_id<Tag>()`, `register_builtin_relations()`.
  - New `RelationInfo` private struct with `HashMap<EntityId, Array<EntityId>> reverse_sources` + per-target add/remove/take helpers.
  - New `Array<RelationInfo*> m_relations` member, sized to `kMaxComponents` (256), lazy-allocated by `on_relation_registered()`.
  - Explicit `~World()` declaration (frees `RelationInfo` slots).

- `engine/scene/src/world.cpp`
  - Constructor initialises `m_relations` with kMaxComponents nullptr entries.
  - Destructor walks `m_relations`, runs `~RelationInfo()`, deallocates.
  - `RelationInfo::add_reverse / remove_reverse / take_sources` — bookkeeping helpers; emplace inner arrays with the World's allocator (operator[] would default-construct with the global default allocator).
  - `on_relation_registered(id)` — asserts `OnTargetDestroyed implies ReverseIndex`, lazy-allocates RelationInfo.
  - `add_relation_impl` — UPSERT short-circuit when `old_target == target`, debug-only cycle check via `would_form_cycle_impl`, reverse-index update before+after, storage UPSERT via `backend_for(id).insert(src, id, &target)`.
  - `remove_relation_impl`, `get_relation_payload_const`, `would_form_cycle_impl` (bounded chain walk), `traverse_relation_impl` (iterative DFS pre-order, stack-local frame buffer).
  - `cleanup_outgoing_relations(e)` — removes (target, e) from every reverse_sources where e has Relation<Tag>. Runs before backend drain.
  - `apply_on_target_destroyed(destroyed, worklist)` — walks every reverse-indexed relation, drains reverse_sources[destroyed], applies Cascade/Detach/SetNull policies. Cascade pushes affected sources onto the worklist.
  - `drain_destruction_worklist(worklist)` — iterative loop replacing the recursive destruction. Diamond dedup via the alive-check at the top of each iteration.
  - `destroy_immediate(e)` and `flush_destroys()` rewritten to seed the worklist and call `drain_destruction_worklist`.
  - `register_builtin_relations()` — six lines, registers the canonical built-ins.

- `tests/scene/CMakeLists.txt` — added `test_relations.cpp`.

## Six-built-in scope decision (pin)

ADR-0051 cited only `ChildOf` and `AttachedTo` as v1f built-ins. During v1f planning the user asked for elite-engine breadth ("very solid and elegant"). The set expanded to six, each demonstrating a distinct trait combination:

| Tag           | Storage    | Acyclic | OnTargetDestroyed | Use case |
|---------------|------------|---------|-------------------|----------|
| `ChildOf`     | Archetype  | yes     | Cascade           | Scene tree, UI tree, prefab, replication scope |
| `AttachedTo`  | Archetype  | yes     | Detach            | Sockets (weapons on hands, decals, audio sources) |
| `Owns`        | Archetype  | yes     | Cascade           | Lifetime ownership (effects own particles, vehicles own wheels) — semantically distinct from ChildOf |
| `Targets`     | SparseSet  | no      | SetNull           | AI lock-on, missile tracking, camera focus, follow-this |
| `DependsOn`   | SparseSet  | yes     | SetNull           | Asset dependencies, system order, animation graph |
| `PossessedBy` | SparseSet  | no      | Detach            | Input routing, AI/script control, replication priority |

Why these six and not more or fewer:
- Each combination is functionally distinct in real engine workloads.
- The grid is dense, not exhaustive — Archetype × no-Acyclic doesn't appear because if a relation is stable enough to be archetype-stored, it almost always wants the structural invariant.
- All six match canonical concepts in Unreal (Owner/Outer/Attached), Bevy (Parent/ChildOf), Flecs (ChildOf/IsA/Slot), and Unity DOTS (user-defined patterns).

The expansion is recorded as a v1f decision; ADR-0051 is not amended (its "ChildOf and AttachedTo" wording was illustrative, not exhaustive). Future relations remain user-defined.

## Built-in registration is opt-in

`register_builtin_relations()` is a single inline call that registers all six with their canonical defaults. Users that want non-default traits on a built-in must call `register_relation<Tag>(custom_traits...)` BEFORE `register_builtin_relations()` (registration is idempotent — the second call is a no-op).

**Pin for v1k SceneLoader**: ChildOf / AttachedTo / Owns / etc. are NOT auto-registered from `World()` ctor. Any code path that deserialises a SCEN file referencing these relations by hash must call `register_builtin_relations()` early in World setup. This is the only ordering constraint v1f imposes on later phases.

## Design decisions (advisor-driven)

### UPSERT short-circuit when old_target == target

Without it, re-targeting a relation to its current target would fire spurious `on_remove` + `on_insert` events through the storage path. v1i's `ChangeDetectIndex` would see a real change where there was none. One-line guard at the top of `add_relation_impl`. Pinned in code comment.

### Iterative destruction worklist

Cascade processing was the obvious recursive design (destroy parent → for each child, destroy_immediate(child) → ...). Stack depth = tree depth. UI hierarchies and particle attachment chains can exceed 100 levels, and the existing tests in v1f stress to 500. Iterative worklist via stack-local `Array<EntityId>` makes this safe regardless of depth. Diamond shapes (an entity reachable via two cascade paths) are handled by the alive-check at the top of each loop iteration — second visit to a dead entity is silently skipped.

### `apply_on_target_destroyed` then `cleanup_outgoing_relations` then sink + backends

Ordering matters. `apply_on_target_destroyed` reads `reverse_sources[destroyed]` (incoming relations) and applies policies. `cleanup_outgoing_relations` walks the dying entity's own relations and removes the dying entity from each target's `reverse_sources` (outgoing edges). Both must run BEFORE the backend drain — once the relation components are destroyed, the targets are unrecoverable.

### `OnTargetDestroyed` requires `ReverseIndex`

Without the reverse index, "find every source pointing at the dying target" is an O(N) scan of every entity in the world. v1f asserts the dependency at registration time (`on_relation_registered`). v1g+ can relax this if a use case appears.

### `would_form_cycle<Tag>` as the testability lever

Cycle detection is enforced via `CRD_ASSERT` inside `add_relation` (debug-only). Tests can't catch fatal asserts cleanly, so they verify the predicate directly: `world.would_form_cycle<ChildOf>(c, parent)` is a public bool returning function with bounded walk (4096 steps defensive). Tests check the predicate without ever calling `add_relation` with a cyclic input.

### Pool dtor ordering

`m_relations` is the LAST World member; its destruction runs first... wait, no — destruction is reverse-declaration-order, so `m_relations` is destroyed FIRST (because it was declared last). Members above (`m_storage`, `m_sparse_storage`, `m_components`) destruct after. The explicit `~World()` runs `~RelationInfo()` on each non-null slot before `m_relations`'s array dtor runs. This is the correct order: RelationInfo holds Arrays, but those Arrays use the World's allocator (which is a non-owning pointer carried by `m_pending_destroy.allocator()`). The allocator outlives all members, so order between m_relations and m_pending_destroy doesn't matter as long as the explicit destructor runs first.

## Bugs caught during integration

### `HashMap<EntityId, V>` needs `DefaultHash<EntityId>`

The first build attempt failed with `error C2512: 'std::hash<T>': no suitable default constructor available` from hash.hpp's fallback. EntityId had no specialisation. Added `DefaultHash<EntityId>` at the end of `entity.hpp` (post-EntityId definition) so the hash specialisation lives alongside the type and HashMap usage compiles transparently.

### CMake regen needed for new test source

Same recurring issue from v1d/v1e: adding a new .cpp to `tests/scene/CMakeLists.txt` doesn't trigger a build until a CMake cache wipe + reconfigure. Standard fix.

### No win-tidy warnings

Unlike v1d/v1e, v1f had zero tidy warnings on first compile. The discipline pays off.

## Numbers

### Six-configuration green

| Config | Build | CTest | Headless smokes |
|---|---|---|---|
| win-debug          | clean | 651 / 651 | 17 / 17 |
| win-relwithdebinfo | clean | 651 / 651 | 17 / 17 |
| win-release        | clean | 648 / 648 | 17 / 17 |
| win-asan           | clean | 651 / 651 | 17 / 17 |
| win-clang-cl       | clean | 651 / 651 | 17 / 17 |
| win-tidy           | clean | — | — |

### Scene tests

- Pre-v1f: 113 cases / 34450 assertions.
- Post-v1f: 135 cases / 34520 assertions (+22 cases / +70 assertions).

Test breakdown:
- 6 generic mechanics (register, round-trip, UPSERT, remove)
- 3 cycle-detection tests via `would_form_cycle`
- 2 traverse_relation tests
- 3 Cascade tests (ChildOf, Owns, 500-deep stress)
- 2 Detach tests (AttachedTo, PossessedBy)
- 2 SetNull tests (Targets, DependsOn)
- 2 multi-relation + flush_destroys tests
- 1 user-defined relation test
- 1 outgoing-cleanup test

## What this unlocks

v1g (the query DSL) is now the natural next slice. It can:
- Layer the `with_relation<Tag>(target)` operator on top of `RelationInfo::reverse_sources` lookups.
- Wire `traverse_relation` into a `world.descendants_of<ChildOf>(root)` range.
- Compose mixed queries: `world.query<Transform>().with_relation<ChildOf>(parent)` → archetype-walk filtered by reverse-index membership.

The v1j Transform propagation system consumes v1f directly: `traverse_relation<ChildOf>(dirty_root, fn)` is the topological walk over the hierarchy.

## Follow-ups

None opened. The OnTargetDestroyed-without-ReverseIndex configuration remains an open extension if a use case surfaces in v1g.

## Commit message proposal

```
feat(scene): Relations layer with six built-ins (v1f, ADR-0051)

Phase 3.0 v1f ships Layer 3 — relations as components-with-target-payload.
Six built-in tag types in crd::scene::relations:
  ChildOf      Archetype + Acyclic + ReverseIndex + Cascade
  AttachedTo   Archetype + Acyclic + ReverseIndex + Detach
  Owns         Archetype + Acyclic + ReverseIndex + Cascade
  Targets      SparseSet + ReverseIndex + SetNull
  DependsOn    SparseSet + Acyclic + ReverseIndex + SetNull
  PossessedBy  SparseSet + ReverseIndex + Detach

World API: register_relation, add_relation, remove_relation,
get_relation_target, has_relation, would_form_cycle, traverse_relation,
register_builtin_relations.

Iterative destruction worklist replaces the recursive destroy path —
500-deep cascade chains never overflow the stack. Diamond shapes deduped
via alive-check on each loop iteration.

UPSERT short-circuit when old_target == target prevents spurious
on_remove + on_insert events. OnTargetDestroyed asserts ReverseIndex
dependency at registration. would_form_cycle is the public testability
lever for cycle detection.

Built-in registration is opt-in via World::register_builtin_relations() —
v1k SceneLoader will call it before SCEN deserialisation.

Six-config DoD: 651/651 (was 629). 17/17 headless smokes per non-tidy
config. 135 scene tests / 34520 assertions (was 113 / 34450).
```
