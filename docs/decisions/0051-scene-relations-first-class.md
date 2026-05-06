# ADR-0051 — Scene/ECS L3: Relations as first-class

**Status:** Accepted
**Date:** 2026-05-06
**Tags:** scene, ecs, arch, layer-3, relations

---

## Context

Most ECS implementations treat parent-child hierarchy as a special-case feature: a `HierarchyNode` component with `parent: EntityId` and `children: Array<EntityId>`, walked by bespoke traversal code. Other relationships (target locking, attachment sockets, ownership, "wants to follow" AI behaviour) end up as more bespoke components, each with its own traversal helpers.

Flecs demonstrated that **relationships are a primitive worth generalising**: a relation is `(tag, target_entity)` parameterised over both axes, the same machinery serves hierarchy / sockets / targets / ownership / AI links, and queries gain a vocabulary for traversing relationships without the engine writing per-relation code.

We adopt the Flecs relation model, simplified for our component grammar.

This ADR locks Layer 3 of the eight-layer architecture.

---

## Decision

### 1. Relations are components parameterised by a tag type and a target

```cpp
// Tag types: empty structs distinguish relation kinds.
struct ChildOf      {};
struct AttachedTo   {};
struct Targets      {};
struct Owns         {};
// Users define their own tags freely: struct Likes {}; struct FollowingPath {};

// Internally, a relation is stored as a component:
template <typename Tag>
struct Relation
{
    EntityId target = EntityId::null();
};

// Equivalent to a component of type Relation<ChildOf>.
world.add_relation<ChildOf>(child_entity, parent_entity);

// Lookup the parent:
EntityId parent = world.get_relation_target<ChildOf>(child_entity);

// Remove the relation:
world.remove_relation<ChildOf>(child_entity);
```

Under the hood, `add_relation<ChildOf>(c, p)` is `add_component<Relation<ChildOf>>(c, {p})`. Relations are just components with target-entity payload — they go through the same Storage layer (ADR-0050) and the same Index layer (ADR-0053) as any other component.

### 2. Storage hint policy for relations

Default: `StorageHint::Archetype` for stable relations (`ChildOf`, `AttachedTo`), `StorageHint::SparseSet` for transient ones (`Targets`, gameplay-event relations).

The defaults are advisory; users can override at registration:

```cpp
world.register_relation<ChildOf>   (StorageHint::Archetype);  // hierarchical, stable
world.register_relation<AttachedTo>(StorageHint::Archetype);  // stable per-frame
world.register_relation<Targets>   (StorageHint::SparseSet);  // changes often
world.register_relation<Likes>     (StorageHint::SparseSet);  // gameplay state
```

### 3. Relation queries

The query DSL (Layer 4, ADR-0052) gains relation-aware operators:

```cpp
// All entities that are children of a specific parent
auto children = world.query<Transform>().with_relation<ChildOf>(parent_id);

// All entities that have ANY ChildOf relation (i.e. all non-roots)
auto non_roots = world.query<Transform>().with_relation<ChildOf>();

// Inverse: find the parent of a given child
EntityId parent = world.get_relation_target<ChildOf>(child_id);

// All entities with a specific socket attached
auto attachments = world.query<Transform>().with_relation<AttachedTo>(weapon_socket_id);

// Combined relation + component query
auto turret_targets = world.query<Transform, Health>()
                          .with_relation<Targets>()
                          .changed<Transform>();
```

### 4. Hierarchical traversal

Tree walks are first-class:

```cpp
// Depth-first walk of the ChildOf tree rooted at root_entity
world.traverse_relation<ChildOf>(root_entity, [](EntityId e, crd::u32 depth) {
    // visit each descendant in DFS order
});

// All entities reachable via Owns from a given root
world.traverse_relation<Owns>(player_entity, [](EntityId e, crd::u32 depth) { ... });
```

The traversal is generic — any tag type, any tree-shaped relation (acyclic by contract — see §6).

### 5. Reverse-relation index (Layer 5 trait)

By default a relation only stores `child → parent`. Walking parent → children requires either iterating all entities and filtering, or maintaining an inverse index.

`crd-scene` provides an opt-in `ReverseIndex` trait at relation registration:

```cpp
world.register_relation<ChildOf>(StorageHint::Archetype, ReverseIndex{});
```

When set, the framework maintains a `HashMap<EntityId parent, Array<EntityId> children>` automatically (driven by relation insert/remove events). `traverse_relation<ChildOf>` uses the inverse index when available; falls back to query-and-filter when not.

ChildOf and AttachedTo default to `ReverseIndex{}`. Relations like `Targets`, where reverse lookups are rare, default to no inverse index.

### 6. Acyclicity contract for tree-shaped relations

Tree-shaped relations (`ChildOf`, `AttachedTo`, `Owns`) are required by their semantics to be acyclic. We enforce this:

```cpp
world.register_relation<ChildOf>(
    StorageHint::Archetype,
    ReverseIndex{},
    Acyclic{}
);
```

In debug builds, `add_relation` walks ancestors and asserts no cycle is introduced. In release, the walk is skipped — caller is trusted. The `Acyclic{}` trait is opt-in; non-tree relations (`Likes`, `Knows`) omit it.

### 7. Relation-aware destruction

When entity P is destroyed and entity C has `Relation<ChildOf>{P}`, the relation becomes a dangling reference. Two policies, opt-in per relation:

```cpp
struct OnTargetDestroyed
{
    enum class Policy : crd::u8
    {
        SetNull,    // child's ChildOf.target becomes EntityId::null()
        Cascade,    // child is also destroyed
        Detach,     // relation component removed from child
    };
    Policy policy = Policy::SetNull;
};

world.register_relation<ChildOf>(
    StorageHint::Archetype,
    ReverseIndex{},
    Acyclic{},
    OnTargetDestroyed{OnTargetDestroyed::Policy::Cascade}
);
```

ChildOf defaults to `Cascade` (destroy parent → destroy children, recursively). AttachedTo defaults to `Detach`. Targets defaults to `SetNull`. Users can override.

This is implemented at the destruction sync point (`SlotMap::flush_destroys`, ADR-0049) — the storage layer iterates relations and applies policy in the same frame, before any other system observes the destruction.

---

## Rationale

### Why generalise hierarchy

A bespoke `HierarchyNode + parent + children + traverse` machinery is ~400 LOC. Generalising to `Relation<Tag>` is ~250 LOC and serves five immediate needs (hierarchy, sockets, targets, ownership, generic gameplay links) plus N future ones (mounted vehicles, party membership, faction relations, AI threat targeting).

The cost is one extra indirection layer (`add_relation` → `add_component<Relation<Tag>>`). At ECS scale this disappears.

### Why match Flecs

Flecs is the most-evolved relation system in production ECS (used in Tempest Rising, Insurgency: Sandstorm, and others). Their model is battle-tested. Our simplification: we don't ship Flecs's pair-relation grammar (`(Likes, Apple)` as a generic "X has relation Y to Z" without parameterising on tag type) — that requires runtime relation IDs and adds significant complexity for marginal benefit on the workloads we target.

### Why opt-in inverse index, opt-in acyclic check, opt-in on-destroy policy

Relations have wildly different access patterns. Forcing ChildOf's invariants on a Likes relation (where cycles are normal and reverse-lookup is rare) wastes machinery. Per-relation traits keep each relation's runtime cost bounded by what it actually needs.

---

## Consequences

- `crd-scene` ships with `ChildOf` and `AttachedTo` as built-in relations in Phase 3.0 v1f. Other relations are user-defined.
- The hierarchical scene tree (ADR-0020 cornerstone) is implemented as `Relation<ChildOf>` plus the reverse index plus `traverse_relation<ChildOf>`. There is no separate "scene graph" code.
- The hierarchy update model (ADR-0054) walks the `ChildOf` reverse index in topological order.
- UI nodes live in the same hierarchy via the same `ChildOf` relation (ADR-0057).
- Animation socket attachment (Phase 3.2) uses `AttachedTo`. No new machinery needed.
- AI target tracking (Phase 8 robotics, gameplay) uses user-defined `Targets` / `Knows` / `ThreatLevel` relations.

---

## References

- ADR-0020 — Scene & ECS hybrid (cornerstone)
- ADR-0049 — Entity identity (Layer 1; relations target EntityIds)
- ADR-0050 — Storage backends (Layer 2; relations are components, stored via either backend)
- ADR-0052 — Query · System · Schedule (Layer 4; relation-aware query operators)
- ADR-0053 — Component index slot framework (Layer 5; ReverseIndex implemented as an Index)
- ADR-0054 — Transform hierarchy update model (consumes ChildOf reverse index)
- ADR-0057 — UI in scene tree (uses same ChildOf relation)
- Flecs Relations documentation (https://www.flecs.dev/flecs/md_docs_2Relationships.html) — primary reference
