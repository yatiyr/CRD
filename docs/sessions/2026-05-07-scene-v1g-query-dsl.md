# 2026-05-07 — Phase 3.0 v1g: Query DSL

**Status at start:** Phase 3.0 v1f shipped earlier the same day. Six relation built-ins + iterative cascade worklist. Scene tests 135 / 34520, six-config 651/651.

**Status at end:** v1g shipped. `world.query<Cs...>()` is the public composer entry point; `.with<>()` / `.without<>()` / `.with_relation<>()` / `.filter()` chain into `Query<Cs...>` ranges. Range-for yields `(EntityId, Cs&...)` tuples; `for_each_chunk` is the chunk-level primitive that v1h `par_each` will sit on. Six-config 673/673 / 670 release / 17 smokes. Scene tests 157 / 34559.

---

## Goal of this session

Land the Query DSL (ADR-0052 §1) — Layer 4's user-facing composer:

```cpp
auto q = world.query<Transform, Renderable>()
             .with<Visible>()
             .without<Hidden>()
             .with_relation<relations::ChildOf>(parent)
             .filter(&is_in_frustum, &camera);

for (auto [e, t, r] : q) { submit(e, t, r); }
```

Built on top of v1e's `World::for_each_chunk` (mixed-backend visitor) and v1f's relation reverse indexes.

## What shipped

### New module files

```
engine/scene/include/crd/scene/query.hpp   ~165 LOC
engine/scene/src/query.cpp                 ~205 LOC
tests/scene/test_query.cpp                 ~470 LOC, 22 cases
```

### Modified

- `engine/scene/include/crd/scene/world.hpp`
  - Added `<crd/scene/query.hpp>` include.
  - New public `World::query<Cs...>()` factory returning `Query<Cs...>` by value (guaranteed copy elision on rvalue chains).
  - New public `allocator()` accessor — Query and future System helpers extend the World's allocator chain through this.
  - **Query<Cs...> template method bodies** appended at the bottom of the file (after the World class is complete). This is where the templates that need `m_world->components()` / `get_component_mut<C>` etc. live; query.hpp only declares the class skeleton.

- `engine/scene/src/query.cpp` (new)
  - `run_query_pipeline(...)` — non-template entry point that drives `World::for_each_chunk(required)` and applies forbidden / relations / predicates per chunk via the `chunk_filter_visitor` trampoline.
  - Filter pipeline split forbidden into `archetype_forbidden` + `sparse_forbidden`, mirroring v1e's required split. Each forbidden bit hits the right backend probe.
  - `entity_passes()` helper applies forbidden → relations → predicates in cheapest-first order.

- `tests/scene/CMakeLists.txt` — added `test_query.cpp`.

### Public API

```cpp
template <typename... Cs>
class Query
{
public:
    explicit Query(World& world);

    // Move-only. Copy is deleted (would duplicate filter Arrays + match cache).
    Query(Query&&) = default;
    Query& operator=(Query&&) = default;
    Query(const Query&) = delete;
    Query& operator=(const Query&) = delete;

    // Filter chain — ref-qualified overloads so factory-style rvalue chains
    // return Query (by move) and lvalue chains return Query&.
    template <typename T> Query&  with()        &;
    template <typename T> Query   with()        &&;
    template <typename T> Query&  without()     &;
    template <typename T> Query   without()     &&;
    template <typename Tag> Query& with_relation(EntityId target = EntityId::null()) &;
    template <typename Tag> Query  with_relation(EntityId target = EntityId::null()) &&;
    Query& filter(FilterPredicateFn fn, void* user_data = nullptr) &;
    Query  filter(FilterPredicateFn fn, void* user_data = nullptr) &&;

    // Iteration.
    void for_each_chunk(ChunkVisitor fn, void* user_data);

    Iterator begin();   // lazy-materialises on first call
    Iterator end();

    // Diagnostics.
    crd::u32                                count();
    const crd::containers::Array<EntityId>& matches();
};

// Free filter types (NOT nested in Query<Cs...>) so the non-template
// pipeline can take them by pointer without seeing any Cs... pack.
struct RelationFilter { ComponentId relation_id; EntityId target; };
struct PredicateFilter { FilterPredicateFn fn; void* user_data; };
using FilterPredicateFn = bool (*)(EntityId entity, const World* world, void* user_data);
```

## Design decisions (advisor-driven)

### 1. Forbidden split into archetype_forbidden + sparse_forbidden

The v1e `present_mask` semantic differs between forwarded and constructed chunks: forwarded archetype chunks carry `arch.mask` (superset of required); constructed mixed/multi-sparse chunks carry exactly `required`. A naive chunk-level `present_mask & forbidden != 0` skip would produce false negatives on constructed chunks. Pinning the split solves it: archetype-side bits get archetype-storage probes, sparse-side bits get sparse-storage probes. Both are O(1); the chunk-level "fast path" simply doesn't exist for sparse forbidden — but archetype forbidden's chunk-level archetype-mask test is already handled by the archetype storage's own `for_each_chunk` (it never yields chunks where `arch.mask` lacks the required bits).

### 2. Single-relation-anchor optimisation deferred to v1h

When `with_relation<Tag>(target)` is the candidate-set anchor, walking `reverse_sources[target]` directly is faster than driving `World::for_each_chunk(required)` and per-entity sparse-checking. But that anchor path lives outside the chunk visitor framework, and v1g's range-for materialisation depends on the chunk visitor for both. Pinning a single anchor path in v1g would either fork the materialisation logic or require a unified abstraction we don't have shape evidence for yet. v1h's `par_each` will surface that shape; the optimisation lands then.

For v1g: relation filters are per-entity checks. Cost: O(N) probes vs O(matched) for the anchor path. For typical scenes (10K-1M entities, dozens of relations matching specific targets) this is acceptable. The optimisation slot is documented in code.

### 3. Move-only Query with ref-qualified filter chains

`auto q = world.query<T>().with<U>().without<V>();` is the canonical chain syntax. Without ref-qualified overloads, `.with<U>()` returns `Query&` and `auto q = ...` tries to copy from the lvalue ref → copy-deleted error.

Fix: each filter has TWO overloads:
```cpp
template <typename T> Query& with() &;     // lvalue: chain on stored variable
template <typename T> Query  with() &&;    // rvalue: factory-chain returns by move
```

Now:
- `auto q = world.query<T>().with<U>();` — `.with<U>()` on rvalue returns `Query` by move; `auto q` move-constructs. ✓
- `auto q = world.query<T>(); q.with<U>();` — `.with<U>()` on lvalue `q` returns `Query&`; result discarded. ✓

The pattern is well-established in modern C++ builders (Bevy `Commands`, ranges-v3 views).

### 4. Predicate signature: `bool (*)(EntityId, const World*, void*)`

Three-arg signature matches the existing `ChunkVisitor` pattern. The `World*` argument lets predicates read components via `world->get_component<T>(e)` without juggling captures. `user_data` carries arbitrary state.

A two-arg `(EntityId, void*)` would be lighter but force callers to put the World pointer inside their context struct, fragmenting the convention. Three-arg keeps things uniform.

### 5. Range-for via lazy materialisation

`begin()` walks `for_each_chunk` once and copies all surviving entities into `m_match_cache`; `end()` returns the past-end iterator. Subsequent `begin()`/`end()` calls return iterators into the cached list — no re-walk.

`*it` dereferences via `world.get_component_mut<C>(e)` per entity per component (slow-but-correct for both backends, mixed cases, and filtered chunks). The chunk-level fast path with direct SoA pointers is reserved for v1h+ when `par_each` performance work calls for it.

Cache invalidation: any mutating call (`.with<>()`, `.without<>()`, `.with_relation<>()`, `.filter()`) clears `m_materialised`. The next `begin()` re-walks. Tests pin both reuse and invalidation behaviour.

### 6. Free filter types out of Query<Cs...>

`RelationFilter` and `PredicateFilter` were originally nested inside `Query<Cs...>`. That made them template-dependent — `Query<int>::PredicateFilter*` and `Query<float>::PredicateFilter*` are different C++ types even though their layout is identical. The non-template `run_query_pipeline` couldn't take them generically.

Pulled out to free namespace types. Single non-template pipeline; one filter-loop instantiation rather than one per Cs... combination.

## Bugs caught during integration

### win-clang-cl: `unused function` for one helper

`greater_than_pred` was defined but never used (vestigial from an earlier draft). clang-cl with `-Werror -Wunused-function` rejected it. Removed.

### win-tidy: four unused using-decls

After cleaning up the test file's helpers, four `using crd::scene::X` declarations became unused. clang-tidy's `misc-unused-using-decls` flagged them. Removed.

### CTest: em-dash in test names again

Three test names had `→` (em-dash). CTest's name parser bombs out. Replaced with prose ("yields", "produces"). Same recurring fix from v1d/v1e/v1f sessions.

### Cache invalidation order in tests

The "Adding a filter after iteration invalidates the cache" test verifies that after calling `.with<Visible>()` on an already-iterated query, `count()` re-materialises. This is a real contract check — a stale cache would silently return wrong results.

## Numbers

### Six-configuration green

| Config | Build | CTest | Headless smokes |
|---|---|---|---|
| win-debug          | clean | 673 / 673 | 17 / 17 |
| win-relwithdebinfo | clean | 673 / 673 | 17 / 17 |
| win-release        | clean | 670 / 670 | 17 / 17 |
| win-asan           | clean | 673 / 673 | 17 / 17 |
| win-clang-cl       | clean | 673 / 673 | 17 / 17 |
| win-tidy           | clean | — | — |

### Scene tests

- Pre-v1g: 135 cases / 34520 assertions.
- Post-v1g: 157 cases / 34559 assertions (+22 cases / +39 assertions).

### LOC

- `query.hpp`         ~165
- `query.cpp`         ~205
- `world.hpp` delta   ~120 (factory + Query template bodies + `allocator()`)
- `test_query.cpp`    ~465
- Total              ~955

## What this unlocks

v1h (System + Schedule) is the natural next slice. It consumes v1g directly:
- `Query::for_each_chunk` becomes the unit dispatched to `crd-jobs` for `par_each`.
- The query's compile-time component pack `Cs...` flows into `ISystem::Reads` / `Writes` set-based scheduling.
- The chunk visitor's `present_mask` (subset-of `required`) is the auto-parallel scheduler's read-set source.

Beyond v1h:
- v1i (`ChangeDetectIndex`) adds `.changed<T>()` as a per-chunk version-counter filter — slots into the existing predicate path.
- v1i (`AsyncAwareIndex`) adds `.skip_pending<T>()` similarly.
- Phase 3.5 (`SpatialBVHIndex`) adds `.in_aabb()` — needs more scaffolding (BVH build+update), but the DSL operator is a one-liner once the index exists.
- Phase 4.0+ (`HistoryIndex`) adds `.at(frame, -N)` — also one-liner DSL.

The five reserved filter operators in ADR-0052 §1 are all "future predicate that filters entities." The current pipeline can absorb them as additional `PredicateFilter` entries; no DSL refactor needed.

## Follow-ups

None opened. The single-relation-anchor optimisation is documented in code as a v1h slot.

## Commit message proposal

```
feat(scene): Query DSL (v1g, ADR-0052 §1)

Phase 3.0 v1g ships the L4 user-facing composer. world.query<Cs...>()
returns Query<Cs...>; chain with .with<T>() / .without<T>() /
.with_relation<Tag>(target) / .filter(fn, ud); iterate via range-for
yielding (EntityId, Cs&...) tuples or via .for_each_chunk(visitor) for
chunk-level access.

Built on v1e's World::for_each_chunk (mixed-backend visitor) and v1f's
relation reverse indexes. Forbidden mask split into archetype/sparse
backends mirroring v1e's required split. Filter pipeline lives in a
non-template free function; the per-Cs template bodies are thin
trampolines.

Move-only Query with ref-qualified filter overloads — factory chains
(auto q = world.query<T>().with<U>().without<V>()) compile without
copying via guaranteed elision + move on rvalue chain returns.

Single-relation-anchor optimisation, par_each, ComponentRef-with-auto-
bump, .changed<>/.skip_pending<>/.in_aabb<>/.at(frame) are deferred to
v1h or v1i per their respective ADRs (0052, 0053).

Six-config DoD: 673/673 (was 651). 17/17 headless smokes per non-tidy
config. 157 scene tests / 34559 assertions (was 135 / 34520).
```
