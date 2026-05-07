// v1g pipeline
// Phase 3.0 v1g — Query DSL filter pipeline (ADR-0052 §1).
//
// `run_query_pipeline` is the single non-template entry point that does the
// heavy lifting: drives World::for_each_chunk(required) and applies
// forbidden / relation / predicate filters per chunk. All Query<Cs...>
// instantiations call into it, so the per-instantiation cost is one
// trampoline call — no template bloat in the filter loop.
//
// Per-entity dereference (yielding (EntityId, Cs&...) tuples for range-for)
// lives in Iterator::operator* in world.hpp; that part is necessarily
// template-on-Cs because the tuple type is.

#include <crd/core/assert.hpp>
#include <crd/scene/query.hpp>
#include <crd/scene/world.hpp>

#include <utility>

namespace crd::scene
{

namespace
{
// Walk every set bit of `mask`. Linear scan over kMaxComponents — this is
// query-construction territory, not the hot iteration loop.
template <typename Fn> void for_each_set_bit(const ComponentMask& mask, Fn&& fn)
{
    for (crd::u32 word = 0; word < 4; ++word)
    {
        const crd::u64 bits = mask.bits[word];
        if (bits == 0)
        {
            continue;
        }
        for (crd::u32 b = 0; b < 64; ++b)
        {
            if ((bits & (crd::u64{1} << b)) != 0)
            {
                std::forward<Fn>(fn)(ComponentId{static_cast<crd::u16>((word * 64U) + b)});
            }
        }
    }
}

struct VisitContext
{
    const PredicateFilter* predicates       = nullptr;
    crd::u32               predicate_count  = 0;
    const RelationFilter*  relations        = nullptr;
    crd::u32               relation_count   = 0;
    ComponentMask          archetype_forbidden{};
    ComponentMask          sparse_forbidden{};
    World*                 world            = nullptr;
    ChunkVisitor           user_visitor     = nullptr;
    void*                  user_data        = nullptr;
    crd::containers::Array<EntityId>* scratch = nullptr;
};

// Per-entity gate. Returns true iff `e` passes ALL post-required filters
// (forbidden + relations + predicates), in cheapest-first order so an
// early reject avoids the expensive paths.
[[nodiscard]] bool entity_passes(const VisitContext& ctx, EntityId e)
{
    // Forbidden — split by storage backend so each bit hits the right
    // probe (archetype-side O(1) mask test, sparse-side O(1) sparse table).
    bool has_forbidden = false;
    for_each_set_bit(ctx.archetype_forbidden,
                     [&](ComponentId fid)
                     {
                         if (ctx.world->storage().has(e, fid))
                         {
                             has_forbidden = true;
                         }
                     });
    if (has_forbidden)
    {
        return false;
    }
    for_each_set_bit(ctx.sparse_forbidden,
                     [&](ComponentId fid)
                     {
                         if (ctx.world->sparse_storage().has(e, fid))
                         {
                             has_forbidden = true;
                         }
                     });
    if (has_forbidden)
    {
        return false;
    }

    // Relations — read Relation<Tag>::target via the storage backend.
    for (crd::u32 i = 0; i < ctx.relation_count; ++i)
    {
        const auto& rf = ctx.relations[i];
        const ComponentInfo* info = ctx.world->components().info(rf.relation_id);
        if (info == nullptr)
        {
            return false;
        }
        const void* payload = (info->storage_hint == StorageHint::SparseSet)
                                  ? ctx.world->sparse_storage().get_const(e, rf.relation_id)
                                  : ctx.world->storage().get_const(e, rf.relation_id);
        if (payload == nullptr)
        {
            return false; // entity has no Relation<Tag> at all
        }
        const EntityId actual_target = *static_cast<const EntityId*>(payload);
        if (rf.target.is_null())
        {
            // "any target" — must be non-null
            if (actual_target.is_null())
            {
                return false;
            }
        }
        else
        {
            if (actual_target.raw != rf.target.raw)
            {
                return false;
            }
        }
    }

    // Predicates — last because they're caller-provided and may be heavy.
    for (crd::u32 i = 0; i < ctx.predicate_count; ++i)
    {
        const auto& pf = ctx.predicates[i];
        if (pf.fn != nullptr && !pf.fn(e, ctx.world, pf.user_data))
        {
            return false;
        }
    }

    return true;
}

void chunk_filter_visitor(const ChunkView& view, void* user_data)
{
    auto* ctx = static_cast<VisitContext*>(user_data);

    ctx->scratch->clear();
    for (crd::u32 i = 0; i < view.entity_count; ++i)
    {
        const EntityId e = view.entities[i];
        if (entity_passes(*ctx, e))
        {
            ctx->scratch->push_back(e);
        }
    }
    if (ctx->scratch->size() == 0)
    {
        return;
    }

    ChunkView filtered{};
    filtered.entities     = ctx->scratch->data();
    filtered.entity_count = static_cast<crd::u32>(ctx->scratch->size());
    filtered.present_mask = view.present_mask; // forwarded view's present_mask
                                                // is a SUPERSET of required (per
                                                // v1e contract); callers must
                                                // treat it as ≥ required.
    ctx->user_visitor(filtered, ctx->user_data);
}

} // namespace

void run_query_pipeline(World& world,
                        const ComponentMask& required,
                        const ComponentMask& forbidden,
                        const RelationFilter* relations,
                        crd::u32 relation_count,
                        const PredicateFilter* predicates,
                        crd::u32 predicate_count,
                        ChunkVisitor user_fn,
                        void* user_data)
{
    if (user_fn == nullptr)
    {
        return;
    }

    // Split forbidden by storage hint. Same trick v1e uses for required:
    // archetype-side bits get cheap mask tests; sparse-side bits get sparse
    // table probes. Pinned 2026-05-07 v1g planning per advisor #1.
    ComponentMask archetype_forbidden{};
    ComponentMask sparse_forbidden{};
    for_each_set_bit(forbidden,
                     [&](ComponentId c)
                     {
                         const ComponentInfo* info = world.components().info(c);
                         if (info != nullptr && info->storage_hint == StorageHint::SparseSet)
                         {
                             sparse_forbidden.set(c);
                         }
                         else
                         {
                             archetype_forbidden.set(c);
                         }
                     });

    crd::containers::Array<EntityId> scratch{world.allocator()};

    VisitContext ctx{};
    ctx.predicates          = predicates;
    ctx.predicate_count     = predicate_count;
    ctx.relations           = relations;
    ctx.relation_count      = relation_count;
    ctx.archetype_forbidden = archetype_forbidden;
    ctx.sparse_forbidden    = sparse_forbidden;
    ctx.world               = &world;
    ctx.user_visitor        = user_fn;
    ctx.user_data           = user_data;
    ctx.scratch             = &scratch;

    world.for_each_chunk(required, &chunk_filter_visitor, &ctx);
}

} // namespace crd::scene
