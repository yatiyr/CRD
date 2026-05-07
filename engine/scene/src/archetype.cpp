#include <crd/scene/archetype.hpp>

namespace crd::scene
{

Archetype::Archetype(crd::memory::IAllocator* alloc)
    : layout(alloc), chunks(alloc), add_edges(alloc), remove_edges(alloc)
{
    // Edge tables are dense: indexed by ComponentId.raw. Pre-fill with
    // kInvalidArchetypeId so an unset edge is a single-load test in the hot
    // navigation path.
    add_edges.resize(static_cast<crd::usize>(kMaxComponents), kInvalidArchetypeId);
    remove_edges.resize(static_cast<crd::usize>(kMaxComponents), kInvalidArchetypeId);
}

crd::u32 Archetype::entity_count() const noexcept
{
    crd::u32 sum = 0;
    for (const Chunk& c : chunks)
    {
        if (c.memory != nullptr)
        {
            sum += c.header()->entity_count;
        }
    }
    return sum;
}

} // namespace crd::scene
