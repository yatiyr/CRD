#include <crd/core/assert.hpp>
#include <crd/scene/archetype_graph.hpp>

#include <utility>

namespace crd::scene
{

ArchetypeGraph::ArchetypeGraph(crd::memory::IAllocator* alloc, ChunkAllocator& chunks,
                               const ComponentRegistry& registry)
    : m_alloc(alloc), m_chunks(&chunks), m_registry(&registry), m_archetypes(alloc), m_by_mask(alloc)
{
}

Archetype& ArchetypeGraph::archetype_for(const ComponentMask& mask)
{
    if (auto* existing = m_by_mask.find(mask); existing != nullptr)
    {
        Archetype* a = m_archetypes[existing->raw].get();
        CRD_ASSERT(a != nullptr);
        return *a;
    }

    auto archetype = std::make_unique<Archetype>(m_alloc);
    archetype->id = ArchetypeId{static_cast<crd::u32>(m_archetypes.size())};
    archetype->mask = mask;
    archetype->layout = compute_chunk_layout(mask, *m_registry, m_alloc);
    if (!archetype->layout.is_valid())
    {
        CRD_FATAL("ArchetypeGraph::archetype_for: chunk layout invalid (component count or single-component size "
                  "exceeds chunk budget)");
    }

    Archetype& ref = *archetype;
    m_archetypes.push_back(std::move(archetype));
    m_by_mask.emplace(mask, ref.id);
    return ref;
}

Archetype& ArchetypeGraph::after_add(Archetype& src, ComponentId added)
{
    if (src.mask.test(added))
    {
        return src; // adding a component the archetype already has — no move
    }

    ArchetypeId cached = src.add_edges[added.raw];
    if (!cached.is_null())
    {
        return *m_archetypes[cached.raw];
    }

    ComponentMask new_mask = src.mask;
    new_mask.set(added);
    Archetype& dst = archetype_for(new_mask);

    // Cache the edge in BOTH directions for free O(1) reverse navigation.
    src.add_edges[added.raw] = dst.id;
    dst.remove_edges[added.raw] = src.id;
    return dst;
}

Archetype& ArchetypeGraph::after_remove(Archetype& src, ComponentId removed)
{
    if (!src.mask.test(removed))
    {
        return src; // removing a component the archetype lacks — no move
    }

    ArchetypeId cached = src.remove_edges[removed.raw];
    if (!cached.is_null())
    {
        return *m_archetypes[cached.raw];
    }

    ComponentMask new_mask = src.mask;
    new_mask.clear(removed);
    Archetype& dst = archetype_for(new_mask);

    src.remove_edges[removed.raw] = dst.id;
    dst.add_edges[removed.raw] = src.id;
    return dst;
}

Archetype* ArchetypeGraph::by_id(ArchetypeId id) noexcept
{
    if (id.is_null() || id.raw >= m_archetypes.size())
    {
        return nullptr;
    }
    return m_archetypes[id.raw].get();
}

const Archetype* ArchetypeGraph::by_id(ArchetypeId id) const noexcept
{
    if (id.is_null() || id.raw >= m_archetypes.size())
    {
        return nullptr;
    }
    return m_archetypes[id.raw].get();
}

crd::u32 ArchetypeGraph::archetype_count() const noexcept
{
    return static_cast<crd::u32>(m_archetypes.size());
}

} // namespace crd::scene
