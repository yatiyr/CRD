#include <crd/core/assert.hpp>
#include <crd/scene/archetype_graph.hpp>

#include <new>
#include <utility>

namespace crd::scene
{

namespace
{
// Per-archetype pool tuning. 32 slots per page balances:
//   - allocation amortisation (one parent allocate covers 32 archetypes)
//   - page-fragmentation cost (one tiny scene → one page = sizeof(Archetype) * 32)
// At 1000 archetypes (large scene), this is ~32 pages.
constexpr crd::usize kArchetypeSlotsPerPage = 32;
} // namespace

ArchetypeGraph::ArchetypeGraph(crd::memory::IAllocator* alloc, ChunkAllocator& chunks,
                               const ComponentRegistry& registry)
    : m_alloc(alloc), m_chunks(&chunks), m_registry(&registry),
      // Pool for Archetype structs. slot_size = sizeof(Archetype), slot_alignment =
      // alignof(Archetype). Parent allocator is m_alloc, so the entire allocation
      // chain stays inside the World's IAllocator.
      m_archetype_pool(sizeof(Archetype), alignof(Archetype), kArchetypeSlotsPerPage, alloc,
                       "crd-scene::ArchetypePool"),
      m_archetypes(alloc), m_by_mask(alloc)
{
}

ArchetypeGraph::ArchetypeGraph(ArchetypeGraph&& other) noexcept
    : m_alloc(other.m_alloc), m_chunks(other.m_chunks), m_registry(other.m_registry),
      m_archetype_pool(std::move(other.m_archetype_pool)), m_archetypes(std::move(other.m_archetypes)),
      m_by_mask(std::move(other.m_by_mask))
{
    other.m_alloc = nullptr;
    other.m_chunks = nullptr;
    other.m_registry = nullptr;
}

ArchetypeGraph& ArchetypeGraph::operator=(ArchetypeGraph&& other) noexcept
{
    if (this == &other)
    {
        return *this;
    }
    // Destroy our current archetypes before the pool's storage is replaced.
    destroy_all_archetypes();

    m_alloc = other.m_alloc;
    m_chunks = other.m_chunks;
    m_registry = other.m_registry;
    m_archetype_pool = std::move(other.m_archetype_pool);
    m_archetypes = std::move(other.m_archetypes);
    m_by_mask = std::move(other.m_by_mask);

    other.m_alloc = nullptr;
    other.m_chunks = nullptr;
    other.m_registry = nullptr;
    return *this;
}

ArchetypeGraph::~ArchetypeGraph()
{
    destroy_all_archetypes();
    // m_archetype_pool's destructor frees its pages — that's where the
    // Archetype struct memory actually goes back.
}

void ArchetypeGraph::destroy_all_archetypes() noexcept
{
    // Run ~Archetype() on every live struct. Their member Arrays release
    // their backing buffers through m_alloc here.
    //
    // The pool's slots themselves go back to the pool only via the pool's
    // dtor (which frees entire pages). We don't deallocate per-slot — that
    // would race with the pool dtor's bulk free.
    for (Archetype* arch : m_archetypes)
    {
        if (arch != nullptr)
        {
            arch->~Archetype();
        }
    }
    m_archetypes.clear();
}

Archetype& ArchetypeGraph::archetype_for(const ComponentMask& mask)
{
    if (auto* existing = m_by_mask.find(mask); existing != nullptr)
    {
        Archetype* a = m_archetypes[existing->raw];
        CRD_ASSERT(a != nullptr);
        return *a;
    }

    // Allocate the Archetype struct from the pool — flows through the World's
    // allocator chain (pool's parent = m_alloc).
    void* mem = m_archetype_pool.allocate(sizeof(Archetype), alignof(Archetype));
    CRD_ASSERT(mem != nullptr);
    Archetype* archetype = ::new (mem) Archetype(m_alloc);

    archetype->id = ArchetypeId{static_cast<crd::u32>(m_archetypes.size())};
    archetype->mask = mask;
    archetype->layout = compute_chunk_layout(mask, *m_registry, m_alloc);
    if (!archetype->layout.is_valid())
    {
        CRD_FATAL("ArchetypeGraph::archetype_for: chunk layout invalid (component count or "
                  "single-component size exceeds chunk budget)");
    }

    m_archetypes.push_back(archetype);
    m_by_mask.emplace(mask, archetype->id);
    return *archetype;
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
    return m_archetypes[id.raw];
}

const Archetype* ArchetypeGraph::by_id(ArchetypeId id) const noexcept
{
    if (id.is_null() || id.raw >= m_archetypes.size())
    {
        return nullptr;
    }
    return m_archetypes[id.raw];
}

crd::u32 ArchetypeGraph::archetype_count() const noexcept
{
    return static_cast<crd::u32>(m_archetypes.size());
}

crd::usize ArchetypeGraph::archetype_pool_pages() const noexcept
{
    return m_archetype_pool.page_count();
}

} // namespace crd::scene
