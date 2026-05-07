# ADR-0050 — Scene/ECS L2: Storage backends (Archetype + SparseSet hybrid)

**Status:** Accepted
**Date:** 2026-05-06
**Tags:** scene, ecs, arch, layer-2, performance

---

## Context

`crd-scene` targets million-entity scenes at 60 Hz. At that scale the storage backend choice is not a preference — it is a physical constraint on cache behaviour and iteration cost. Sparse-set iteration is observably 5–10× slower than archetype-chunked iteration past ~100K entities for multi-component queries; archetype add/remove costs O(component-bytes) and produces archetype-explosion when component combinations are highly varied.

Modern engines that pushed past this trade-off (Bevy, Flecs) converged on **two backends behind a unified abstraction** with a per-component-type hint at registration. We adopt the same shape.

This ADR locks Layer 2 of the eight-layer architecture: storage backends.

---

## Decision

### 1. Two storage backends behind `IStorageBackend`

```cpp
class IStorageBackend
{
public:
    virtual ~IStorageBackend() = default;

    // Component lifecycle
    virtual void  insert(EntityId e, ComponentId c, const void* data) = 0;
    virtual void  remove(EntityId e, ComponentId c)                   = 0;
    virtual bool  has   (EntityId e, ComponentId c) const             = 0;
    virtual void* get_mut(EntityId e, ComponentId c)                  = 0;

    // Bulk iteration over (chunked) storage. Visitor sees one storage chunk
    // at a time with packed SoA pointers — the API the query layer hands to
    // par_each, vectorised loops, and the GPU-resident extract path.
    //
    // ChunkVisitor is a C-callback + opaque user-data pointer (rather than a
    // std::function) so the dispatch path stays heap-free. Implemented in
    // v1b alongside the interface declaration.
    virtual void  for_each_chunk(ComponentMask required, ChunkVisitor fn, void* user_data) = 0;

    // Destruction notification (driven by SlotMap::flush_destroys).
    virtual void  on_entity_destroyed(EntityId e) = 0;
};
```

Both backends implement the same interface. The query layer (Layer 4) walks both transparently — `view<Transform, DialogTrigger>` works whether `Transform` is Archetype-stored and `DialogTrigger` is SparseSet-stored.

### 2. `ArchetypeChunkStorage` is the primary backend

- **Archetype** = a sorted set of `ComponentId`s. All entities with the same exact component set share an archetype.
- **Chunk** = 16 KB block of memory containing a header + SoA arrays for every component in the archetype.
- Components are stored in declaration-sorted order within the chunk; layout is computed once at archetype creation.
- Each chunk holds N entities where N is bounded by `(16 KB - header) / sum(sizeof(component))`. Typical N for `(Transform, Renderable)` ≈ 200 entities/chunk.
- Cache-line alignment: every per-component SoA array within a chunk starts at a 64-byte boundary. Padding bytes between arrays accepted; iteration of a single component is cache-coherent.
- Per-chunk version counter per component: incremented when any entity in the chunk has that component written. Free `ChangeDetect` foundation (Layer 5, ADR-0053).

```cpp
struct ChunkHeader
{
    crd::u16 entity_count;        // 0..N_max for this archetype
    crd::u16 entity_capacity;     // N_max
    crd::u32 archetype_id;        // back-reference
    crd::u64 version_counter[kMaxComponentsPerArchetype]; // change detection (Layer 5)
    // followed by SoA arrays, each 64-byte aligned
};
```

Adding/removing a component on an entity moves the entity from one archetype to another (`old_archetype.move_entity_out(e)` → `new_archetype.move_entity_in(e, copied_components)`). Cost is O(component-bytes-of-old + component-bytes-of-new) — high for entities with many components, but rare in practice for Archetype-stored types (they are the static, structural components).

### 3. `SparseSetStorage` is the escape hatch

- Per-component-type pool: `sparse[entity_index] -> dense_index`, `dense[dense_index] -> T` + `dense[dense_index] -> entity_id`.
- O(1) insert, O(1) remove (swap-with-last), O(1) lookup.
- Iteration walks the dense array — cache-coherent for single-component queries, slower for multi-component queries (must check membership in the other components' sparse arrays per entity).
- Used for components with one of these access patterns:
  - **High churn:** added/removed many times per second on the same entity (gameplay tags, transient effect markers, frame-scoped flags).
  - **Sparse:** present on <5% of entities (dialog triggers, spawn points, cinematic markers, editor-only metadata).
  - **Lookup-dominated:** queries are mostly "does entity X have this?" rather than "iterate all entities with this."

### 4. Storage hint at registration

```cpp
enum class StorageHint : crd::u8
{
    Archetype,  // default
    SparseSet,
};

world.register_component<Transform>     ( StorageHint::Archetype );
world.register_component<Renderable>    ( StorageHint::Archetype );
world.register_component<Velocity>      ( StorageHint::Archetype );
world.register_component<Light>         ( StorageHint::Archetype );
world.register_component<HierarchyNode> ( StorageHint::Archetype );

world.register_component<DialogTrigger> ( StorageHint::SparseSet );
world.register_component<RecentlyHit>   ( StorageHint::SparseSet );  // frame-scoped
world.register_component<EditorSelected>( StorageHint::SparseSet );  // editor-only
```

The hint is part of the public registration API and is non-breaking to extend (future hints like `GpuResident`, `Streamed` slot in as additional traits — see ADR-0053).

### 5. Mixed-backend queries

When a query touches both Archetype and SparseSet components (e.g. `view<Transform, DialogTrigger>`):

1. Walk archetypes that contain `Transform`.
2. For each entity in those archetype chunks, sparse-check `DialogTrigger`.
3. Yield only entities present in both.

This is slower than pure-archetype iteration (~3–5×) but only kicks in for queries that mix tiers. The common case — all-Archetype or all-SparseSet — runs the pure path with no penalty.

### 6. Chunk-parallel iteration is the primary perf model

Archetype chunks are the unit of work for `par_each`. The query layer hands one chunk to one fiber (Layer 4, ADR-0052). At ~200 entities/chunk and 1 M entities, that is ~5000 work units — well-sized for the `crd-jobs` pool.

For SparseSet pools, par_each ranges across the dense array in fixed-size strides (e.g. 1024 entries per work unit); cache effects are weaker but the API stays uniform.

---

## Rationale

### Why both, not pick one

We considered:
- **Pure SparseSet** (EnTT default): simpler, ~150 LOC. Loses 5–10× iteration perf at million scale; that wall arrives long before million-agent simulation does.
- **Pure Archetype** (Unity DOTS, Bevy v1): million-entity perf champion. Pays archetype-explosion cost on highly-varied component compositions; pays move-entity cost on every add/remove, which UI mutations and gameplay tag toggles do constantly.
- **Hybrid (Bevy v2+, Flecs)**: ~600 LOC, costs an extra `IStorageBackend` indirection on the dispatch path. Buys both perf characteristics where each shines.

We pick hybrid. The dispatch indirection is a single virtual call per chunk-batch (not per entity), invisible against the work the visitor does. The 5–10× perf delta on the hot iteration path far outweighs the dispatch cost.

### Why 16 KB chunks

- Fits comfortably in L1 cache (typical 32–48 KB) — iteration touches header + currently-iterated component arrays, both hot.
- ~200 entities per chunk for `(Transform, Renderable)` — large enough that per-chunk overhead amortises, small enough that load-balancing across fibers stays even.
- Mirrors Unity DOTS (16 KB) and Bevy (≈8 KB tables); proven in production.

### Why version counter per component per chunk

ChangeDetect (ADR-0053, Layer 5) needs to know which entities had which component written this frame. Per-entity bit tracking is expensive; per-component-per-chunk version counter is cheap and matches how iteration works (a chunk visitor that wrote any entity in that chunk bumps the counter). False positives at chunk granularity are acceptable (the consumer gets at most ~200 extra entities to process, and it filters them downstream).

---

## Consequences

- `crd-scene` ships with both backends in Phase 3.0 v1c (Archetype) and v1d (SparseSet).
- `IStorageBackend` is a virtual interface. The cost is one indirect call per chunk-batch (not per entity). Verified via benchmarks — hot iteration loops achieve full memory bandwidth despite the virtual.
- Adding a new storage backend later (`GpuResidentStorage`, `StreamedStorage`) means implementing `IStorageBackend`, registering it, and routing components via `StorageHint`. No core-code changes.
- Archetype graph: a directed acyclic graph of archetypes connected by add/remove edges. Built lazily as new archetypes are encountered. Memoised so repeated add-component operations are O(1) edge lookup.
- Memory overhead per archetype: ~1 KB for graph metadata + chunk list. Empty archetypes are cleaned up periodically (frame-coalesced, never on the hot path).
- Default for new component registrations is `StorageHint::Archetype`. Users explicitly opt into SparseSet.

---

## References

- ADR-0020 — Scene & ECS hybrid (cornerstone)
- ADR-0049 — Entity identity & SlotMap (Layer 1; provides EntityId)
- ADR-0052 — Query · System · Schedule (Layer 4; iterates chunks)
- ADR-0053 — Component index slot framework (Layer 5; ChangeDetect uses chunk version counters)
- Bevy ECS architecture (Table + Sparse hybrid evolution, 2022–2024)
- Unity DOTS chunk layout (16 KB, SoA, archetype-keyed)
- Flecs query model (archetype + sparse hybrid)
