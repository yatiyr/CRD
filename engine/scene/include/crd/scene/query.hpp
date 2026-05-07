#pragma once

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/scene/component.hpp>
#include <crd/scene/entity.hpp>
#include <crd/scene/relation.hpp>
#include <crd/scene/storage_backend.hpp>

#include <tuple>
#include <utility>

namespace crd::scene
{
class World; // forward — query.hpp must NOT pull world.hpp (cyclic).

// Free filter types — pulled OUT of Query<Cs...> so the non-template
// pipeline in query.cpp can take them by ConstSpan without seeing any
// particular Cs... pack.
struct RelationFilter
{
    ComponentId relation_id; // ComponentId of Relation<Tag>
    EntityId    target;      // null → "any target" (relation present)
};

using FilterPredicateFn = bool (*)(EntityId entity, const World* world, void* user_data);

struct PredicateFilter
{
    FilterPredicateFn fn = nullptr;
    void*             user_data = nullptr;
};

// Non-template filter pipeline. Drives World::for_each_chunk(required) and
// applies forbidden / relations / predicates per chunk; yields filtered
// ChunkViews to `user_fn`. Defined in query.cpp; called from
// Query<Cs...>::drive_filtered_chunks (in world.hpp's inline section)
// so all Cs... packs share one implementation.
void run_query_pipeline(World& world,
                        const ComponentMask& required,
                        const ComponentMask& forbidden,
                        const RelationFilter* relations,
                        crd::u32 relation_count,
                        const PredicateFilter* predicates,
                        crd::u32 predicate_count,
                        ChunkVisitor user_fn,
                        void* user_data);

// Query<Cs...> — Phase 3.0 v1g (ADR-0052 §1).
//
// The expression-built range that lets gameplay code walk an arbitrary
// (component × relation × predicate) intersection over both L2 backends:
//
//   for (auto [e, t, r] : world.query<Transform, Renderable>()
//                              .with<Visible>()
//                              .without<Hidden>()
//                              .with_relation<relations::ChildOf>(parent)
//                              .filter(&is_in_frustum, &camera))
//   {
//       submit(e, t, r);
//   }
//
// The template parameter pack `Cs...` defines the per-entity tuple yielded
// by range-for iteration: each entity is dereferenced as
// `(EntityId, Cs&...)`. Filters added via `.with<T>()` add COMPONENT
// REQUIREMENTS without yielding T in the tuple — to yield, list T in
// `query<...>()`.
//
// Two iteration shapes:
//   - Range-for: yields per-entity tuples. Materialises a flat
//     Array<EntityId> on first iteration and caches it; subsequent loops
//     reuse the cache. Per-entity component access via
//     World::get_component_mut<T>(e).
//   - Chunk visitor: q.for_each_chunk(fn, ud) yields filtered ChunkViews.
//     Primary unit; v1h's par_each will dispatch one chunk per worker
//     fiber. Faster than range-for when the visitor body wants direct
//     SoA chunk access.
//
// Movability: Query is move-only. Copy is deleted because the matched-
// entity cache and filter Arrays would duplicate to no good purpose. The
// factory World::query<Cs...>() returns by value via guaranteed copy
// elision (C++17+); chained .with<>().without<>() return Query& by
// reference, so no copies happen along the chain.
//
// Template-method bodies that touch World live in this header — the
// header forward-declares World, and World's complete definition is
// pulled in by the consumer's transitive include of world.hpp before any
// instantiation of with<T> / Iterator::operator*() / etc. occurs.
template <typename... Cs> class Query
{
public:
    explicit Query(World& world);

    Query(const Query&) = delete;
    Query& operator=(const Query&) = delete;
    Query(Query&&) noexcept = default;
    Query& operator=(Query&&) noexcept = default;

    // ---- Filter chain --------------------------------------------------
    //
    // Each filter has two ref-qualified overloads:
    //   - lvalue: returns Query& for chaining on a stored variable.
    //   - rvalue: returns Query (by move-out) so factory-style chains
    //             `auto q = world.query<T>().with<U>().without<V>();` work.
    //
    // Without the rvalue overload, `auto q = ...` on a chained rvalue tries
    // to copy-construct from the chain's lvalue ref — and copy is deleted.

    template <typename T> Query&  with() &;
    template <typename T> Query   with() &&;
    template <typename T> Query&  without() &;
    template <typename T> Query   without() &&;
    template <typename Tag> Query& with_relation(EntityId target = EntityId::null()) &;
    template <typename Tag> Query  with_relation(EntityId target = EntityId::null()) &&;
    Query& filter(FilterPredicateFn fn, void* user_data = nullptr) &;
    Query  filter(FilterPredicateFn fn, void* user_data = nullptr) &&;

    // ---- Chunk-level visitor ------------------------------------------

    void for_each_chunk(ChunkVisitor fn, void* user_data);

    // ---- Range-for support --------------------------------------------

    class Iterator
    {
    public:
        Iterator(const Query* q, crd::usize idx) noexcept : m_query(q), m_index(idx) {}

        [[nodiscard]] auto operator*() const;

        Iterator& operator++() noexcept
        {
            ++m_index;
            return *this;
        }

        [[nodiscard]] bool operator==(const Iterator& o) const noexcept
        {
            return m_query == o.m_query && m_index == o.m_index;
        }
        [[nodiscard]] bool operator!=(const Iterator& o) const noexcept { return !(*this == o); }

    private:
        const Query* m_query;
        crd::usize   m_index;
    };

    [[nodiscard]] Iterator begin();
    [[nodiscard]] Iterator end();

    // ---- Diagnostics ---------------------------------------------------

    [[nodiscard]] crd::u32                                  count();
    [[nodiscard]] const crd::containers::Array<EntityId>&   matches();

    [[nodiscard]] const World& world() const noexcept { return *m_world; }
    [[nodiscard]] World&       world() noexcept { return *m_world; }

private:
    void invalidate_cache() noexcept { m_materialised = false; }
    void materialise();
    void drive_filtered_chunks(ChunkVisitor fn, void* user_data);

    World* m_world;

    ComponentMask m_required{};
    ComponentMask m_forbidden{};
    crd::containers::Array<RelationFilter>  m_relations;
    crd::containers::Array<PredicateFilter> m_predicates;

    crd::containers::Array<EntityId> m_match_cache;
    bool                             m_materialised = false;
};

} // namespace crd::scene
