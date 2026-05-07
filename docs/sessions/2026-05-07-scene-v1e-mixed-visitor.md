# 2026-05-07 — Phase 3.0 v1e: `World::for_each_chunk` (mixed-backend chunk visitor)

**Status at start:** Phase 3.0 v1d shipped earlier the same day. Both L2 backends (`ArchetypeChunkStorage`, `SparseSetStorage`) live behind `IStorageBackend`, World routes `add/has/get/remove` by `StorageHint`. But each backend's `for_each_chunk` only knows about its own data — multi-backend queries (the typical case for v1g's query DSL) had no primitive yet.

**Status at end:** v1e shipped. `World::for_each_chunk(required, fn, ud)` is the unified iteration primitive that v1g sits on. Splits `required` by hint, fast-paths the pure cases, and walks intersections for mixed and pure-sparse-multi-bit. Filtered chunks land in stack-local scratch — recursive queries are safe. Six-config green at 629/629 / 626 release / 17 smokes. Scene tests 113 / 34450.

---

## Goal of this session

Land the storage-side primitive that lets v1g's query DSL walk both backends transparently. Per phase doc:

> v1e — Mixed-backend queries + chunk visitor (~200 LOC + tests)
> Storage-side `for_each_chunk` interface. The plumbing that lets queries (next slice) walk both Archetype and SparseSet uniformly. Tests: query that touches both backends emits correct entity set.

The phase doc gave a 200 LOC band. Actual landed surface is ~470 LOC including tests + the multi-mode dispatch logic.

## What shipped

### Modified

- `engine/scene/include/crd/scene/world.hpp`
  - New method: `void for_each_chunk(ComponentMask required, ChunkVisitor fn, void* user_data)`.
  - Method-header doc-block pins:
    - `present_mask` is `≥ required` always (forwarded archetype chunks carry `arch.mask`; constructed chunks carry `required`).
    - filtered `entities` lifetime = visitor call only.
    - threading contract: not thread-safe; `par_each` over yielded chunks is the parallel path.

- `engine/scene/src/world.cpp`
  - Anonymous-namespace helpers: `for_each_set_bit(mask, fn)` (linear bit-scan over kMaxComponents — 256 iterations is invisible against visitor body cost), `mask_subset_of(subset, superset)`.
  - `for_each_chunk` implementation: split required, dispatch by populations, three filter paths.
  - Added `<utility>` include for `std::forward`.

### New

- `tests/scene/test_mixed_chunk_visitor.cpp` — 11 cases:
  1. Pure-archetype required forwards to ArchetypeChunkStorage (archetype superset semantics preserved).
  2. Pure-SparseSet single-bit required forwards to SparseSetStorage.
  3. Empty required yields every archetype chunk AND every non-empty sparse pool.
  4. Pure-SparseSet multi-bit yields the intersection (smallest pool anchor + sparse-check).
  5. Pure-SparseSet multi-bit with one pool empty/missing yields nothing.
  6. Mixed (1 arch + 1 sparse) yields filtered intersection.
  7. Mixed (2 arch + 2 sparse) full integration.
  8. Archetype bit never present yields nothing.
  9. Empty world yields nothing for any required.
  10. Visitor not invoked for empty filtered chunks (10 entities matching the archetype side, 0 matching sparse → 0 visits).
  11. Recursive call from inside visitor body is safe (proves stack-local scratch).

- `tests/scene/CMakeLists.txt` — added `test_mixed_chunk_visitor.cpp`.

## Algorithm

Splits `required` into `archetype_bits` and `sparse_bits` by walking `ComponentInfo::storage_hint` for every set bit. Then dispatches:

```
sparse_pop = sparse_bits.popcount()
arch_pop   = archetype_bits.popcount()

if sparse_pop == 0:
    archetype.for_each_chunk(required)
    if arch_pop == 0:
        sparse.for_each_chunk(required)        # symmetric: empty required hits both backends
    return

if arch_pop == 0 and sparse_pop == 1:
    sparse.for_each_chunk(required)
    return

# Stack-local scratch from here on — Array<EntityId> built from m_pending_destroy.allocator()

if arch_pop == 0:
    # pure-sparse multi-bit
    pick smallest pool as anchor; if any pool missing or empty → return
    walk anchor entities; sparse-check every other bit; collect into scratch
    yield ONE ChunkView{scratch, count, required}
    return

# mixed path: archetype-as-anchor, sparse-check per entity
for arch in archetypes:
    if arch.mask ⊉ archetype_bits: continue
    for chunk in arch.chunks:
        scratch.clear()
        for slot in chunk:
            entity = chunk.entities[slot]
            if all sparse_bits present on entity: scratch.push_back(entity)
        if scratch.size() > 0:
            yield ChunkView{scratch, count, required}
```

## Design decisions

### Why archetype-as-anchor for the mixed path

Walking archetypes is cache-coherent: each chunk's `entity_id_array` is contiguous, `arch.mask ⊇ archetype_bits` is one O(1) bitmask test, and the sparse-check per entity is a few `Array<u32>::operator[]` lookups. Sparse-as-anchor would mean iterating one sparse pool's dense entities then probing into the archetype side per entity — same logical work but no cache locality on the archetype side.

The case where sparse-as-anchor wins is when the smallest sparse pool is much smaller than the matching archetype's chunks. For v1e, archetype-as-anchor is the default; profile-driven anchor selection is reserved for v1g if it ever shows up in benchmarks.

### Why smallest-pool-as-anchor for pure-sparse multi-bit

There is no cache-coherent path for pure-sparse intersection: each pool is its own SoA, no shared chunk grain. So the only knob is "minimise probes per entity," which means iterate the smallest pool and probe into the others.

The implementation uses `SparseSetStorage::for_each_chunk` to walk the anchor pool's dense entities (single-bit `required`), then sparse-checks the others via `SparseSetStorage::has` per entity.

### Why stack-local scratch (advisor call)

A member `Array<EntityId> m_visit_scratch` would corrupt under recursive queries (debug overlays, editor tools, future v1g compositions that call `world.for_each_chunk` from inside a visitor). Stack-local scratch eliminates the footgun for ~one per-call allocate/free cost — invisible against visitor body cost.

The test `Recursive call from visitor body is safe` covers this property explicitly: an outer mixed visit calls `world.for_each_chunk` from inside the visitor body to drive an inner sparse-only iteration. With member scratch this would either crash (concurrent mutation) or silently corrupt (overwrite the outer's data); with stack-local it works.

### Why empty `required` visits both backends

Symmetric reading: empty `required` means "yield every chunk." Each backend's `for_each_chunk` already yields all of its chunks for empty `required`; World composes them. Test case `empty required yields every chunk in BOTH backends` verifies the symmetry.

### `present_mask` semantic — pinned, not normalised

Forwarded archetype chunks carry `arch.mask` (a superset of required). Constructed mixed/multi-sparse chunks carry exactly `required`. We considered post-processing the forward case to normalise `present_mask = required`, but that would kill the zero-overhead claim of the pure-archetype path. Instead we documented the contract: visitors must treat `present_mask` as ≥ `required`, never as `== required`. v1g's DSL will follow this rule.

### LOC band vs phase doc

Phase doc said "~200 LOC + tests." Actual landed:

- `world.hpp` delta: ~30 LOC (method decl + doc block)
- `world.cpp` delta: ~170 LOC (split + helpers + three filter paths)
- `test_mixed_chunk_visitor.cpp`: ~280 LOC, 11 cases
- Total: ~480 LOC

Implementation alone (without tests, doc-block, helpers) is ~145 LOC — under the 200 LOC band. Tests + dispatch logic + advisor-recommended doc block push the total higher.

## Bugs caught during integration

### CMake didn't pick up the new test file until cache wipe

After adding `test_mixed_chunk_visitor.cpp` to `tests/scene/CMakeLists.txt`, `cmake --build` reported "ninja: no work to do" three times. Fix: deleted `build/win-debug/CMakeCache.txt` and `build/win-debug/CMakeFiles/`, re-ran `cmake --preset win-debug`. After that the new file was picked up.

Suspected cause: VS 2026 + Ninja generator can leave stale `build.ninja` deps when source-list lines are added between existing entries. A clean reconfigure resolved it. No code change needed.

### win-tidy: three readability/style warnings

- `cppcoreguidelines-missing-std-forward` on `for_each_set_bit`'s `Fn&& fn` parameter — fixed by invoking via `std::forward<Fn>(fn)(...)`.
- `misc-unused-using-decls` on `using crd::scene::ComponentId` in test file — removed.
- `readability-use-anyofallof` on the `contains` test helper — replaced manual loop with `std::any_of`.

All three landed under five-line edits.

## Numbers

### Six-configuration green

| Config | Build | CTest | Headless smokes |
|---|---|---|---|
| win-debug          | clean | 629 / 629 | 17 / 17 |
| win-relwithdebinfo | clean | 629 / 629 | 17 / 17 |
| win-release        | clean | 626 / 626 | 17 / 17 |
| win-asan           | clean | 629 / 629 | 17 / 17 |
| win-clang-cl       | clean | 629 / 629 | 17 / 17 |
| win-tidy           | clean | — | — |

### Scene tests

- Pre-v1e: 102 cases / 34420 assertions.
- Post-v1e: 113 cases / 34450 assertions (+11 cases / +30 assertions).

The small assertion delta is because the v1e tests run small fixtures (≤10 entities each) — the v1d 10K-entity stress case still dominates the assertion count.

## What this unlocks

v1f (Relations) is now the natural next slice. Once relations are in, v1g (the query DSL) can sit on top of `World::for_each_chunk` to deliver the public `world.query<Cs...>().with<>().without<>().changed<>()` shape from ADR-0052.

Specifically v1e provides:
- The single entry point that v1g's `Query::par_each` will lower onto. v1g no longer has to teach the DSL about hint dispatch — the storage side already does it.
- The "filtered ChunkView with stack-local scratch" pattern that v1g's `.filter([](const T&){...})` operator will reuse.
- The contract that filtered `entities` are transient — v1g's iterators will copy what they need before yielding control back.

## Follow-ups

None opened. The "anchor archetype, accept the cache-coherent assumption" call is the only profile-driven choice that might revisit; tagged in code as a future optimisation if v1g benchmarks ever show it.

## Commit message proposal

```
feat(scene): World::for_each_chunk mixed-backend chunk visitor (v1e)

Phase 3.0 v1e ships the unified storage-side iteration primitive that
v1g (query DSL) sits on top of. Splits ComponentMask required by
ComponentInfo::storage_hint, then dispatches:
  - pure-archetype           -> ArchetypeChunkStorage (zero overhead)
  - pure-SparseSet 1 bit     -> SparseSetStorage
  - pure-SparseSet multi-bit -> smallest pool as anchor + sparse-check
  - mixed                    -> archetypes superset of archetype_bits +
                                sparse-check sparse_bits per entity
  - empty required           -> both backends contribute (symmetric)

Filtered chunks live in stack-local scratch (recursive queries safe).
Forwarded chunks keep the archetype's full present_mask (superset of
required); constructed chunks carry required exactly. Visitors must
treat present_mask as >= required.

Six-config DoD: 629/629 (was 618). 17/17 headless smokes per non-tidy
config. 113 scene tests / 34450 assertions (was 102 / 34420).
```
