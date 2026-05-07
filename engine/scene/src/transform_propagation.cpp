// Phase 3.0 v1j — TransformPropagation (ADR-0054).
//
// Algorithm:
//   1. Collect dirty entity set: query<Transform>().with<TransformDirtyFlag>().
//      Materialised as Array<EntityId>.
//   2. Find dirty roots: dirty entities whose ChildOf parent is either
//      (a) absent / null, or (b) NOT dirty.
//   3. For each dirty root, DFS pre-order via traverse_relation<ChildOf>.
//      At each visited entity:
//        - Recompute world = parent.world * local (or just local if no
//          parent or parent has no Transform).
//        - Write transform.world via get_component_mut<Transform>(e)
//          — this fires ChangeDetect on_update so downstream queries
//          (renderer's `query<Transform>().changed<Transform>()`) see
//          the post-propagation state correctly.
//        - Queue commands().remove_component<TransformDirtyFlag>(e).
//   4. World::step() flushes Commands at the PreRender phase boundary
//      → all dirty flags removed before any subsequent phase observes
//      them.
//
// Determinism guarantees:
//   - Dirty-root selection is deterministic: iteration of the dirty set
//     uses the materialised Array<EntityId>, which preserves insertion
//     order from query construction.
//   - DFS pre-order via traverse_relation is deterministic per ADR-0051's
//     reverse-index insertion-order contract (verified by v1f tests).
//   - parent.world * local floating-point order is fixed by code; no
//     reordering, no atomics, no thread-local state.

#include <crd/core/assert.hpp>
#include <crd/scene/relation.hpp>
#include <crd/scene/transform.hpp>
#include <crd/scene/transform_propagation.hpp>
#include <crd/scene/world.hpp>

namespace crd::scene
{

namespace
{
struct PropagationCtx
{
    World*                                       world = nullptr;
    const crd::containers::Array<EntityId>*      dirty_set_lookup = nullptr;
    ComponentId                                  childof_id{};
};

// True iff `e` carries TransformDirtyFlag.
[[nodiscard]] bool is_dirty(const World& w, EntityId e) noexcept
{
    return w.has_component<TransformDirtyFlag>(e);
}

// Get parent (ChildOf target) or null if the entity has no relation.
[[nodiscard]] EntityId parent_of(const World& w, EntityId e) noexcept
{
    return w.get_relation_target<crd::scene::relations::ChildOf>(e);
}

// Recompute one entity's world matrix from its parent's already-current
// world (or pure local if no parent / parent missing Transform).
void recompute_world_for(World& w, EntityId e)
{
    Transform* t = w.get_component_mut<Transform>(e);
    CRD_ASSERT(t != nullptr); // dirty set was filtered by query<Transform>
    const crd::math::Mat4f local = t->local();
    const EntityId parent = parent_of(w, e);
    if (parent.is_null())
    {
        t->world = local;
        return;
    }
    const Transform* parent_t = w.get_component<Transform>(parent);
    if (parent_t == nullptr)
    {
        // Parent has no Transform — propagate as if root. Common edge
        // case for hierarchical organisation entities (e.g. "scene root"
        // with no transform of its own that just owns ChildOf children).
        t->world = local;
        return;
    }
    t->world = parent_t->world * local;
}

// Iterative DFS pre-order via the index reverse-sources accessor. We
// can't use traverse_relation<Tag> because its visitor is templated; the
// non-template traverse_relation_impl IS available. But for clarity we
// use a stack-local frame buffer here — same pattern as v1f's iterative
// traverse_relation_impl.
void dfs_recompute(World& w, EntityId root)
{
    crd::containers::Array<EntityId> stack{w.allocator()};
    stack.push_back(root);
    while (stack.size() > 0)
    {
        const EntityId current = stack.back();
        stack.pop_back();

        recompute_world_for(w, current);
        w.commands().remove_component<TransformDirtyFlag>(current);

        // Children — reverse-index lookup. v1f exposes traverse_relation
        // as the canonical API; we re-use it here for the children-only
        // walk by capturing them into a local buffer.
        crd::containers::Array<EntityId> children{w.allocator()};
        w.traverse_relation<crd::scene::relations::ChildOf>(
            current,
            [&](EntityId visited, crd::u32 depth)
            {
                // traverse_relation visits root at depth 0, descendants
                // at depth 1+. We want only the current node's direct
                // children (depth == 1) — descendants further down get
                // walked in their own iterations of the outer DFS.
                if (depth == 1)
                {
                    children.push_back(visited);
                }
            });
        // Push in reverse so the stack pops them in insertion order
        // (matches v1f traverse_relation_impl semantics — deterministic).
        for (crd::usize i = children.size(); i > 0; --i)
        {
            stack.push_back(children[i - 1U]);
        }
    }
}

} // namespace

void TransformPropagation::run(World& world)
{
    // Find every entity carrying both Transform and TransformDirtyFlag.
    auto dirty_query = world.query<Transform>().with<TransformDirtyFlag>();
    if (dirty_query.count() == 0U)
    {
        return;
    }

    // Materialised list — stable order, used to test "is this entity in
    // the dirty set?" cheaply for parent checks below.
    const crd::containers::Array<EntityId>& dirty = dirty_query.matches();

    // Pass 1: identify dirty roots = dirty entities whose parent is NOT
    // dirty (or is absent). Walk the materialised list; for each dirty
    // entity, check its parent's dirty bit (O(1) sparse-set has).
    for (EntityId e : dirty)
    {
        if (!world.is_alive(e))
        {
            continue;
        }
        const EntityId parent = parent_of(world, e);
        if (!parent.is_null() && is_dirty(world, parent))
        {
            // Parent is dirty → it's a higher-up root or sibling-deep
            // chain. Skip; the DFS from the higher root will visit `e`.
            continue;
        }
        // Dirty root — DFS its subtree.
        dfs_recompute(world, e);
    }
}

} // namespace crd::scene
