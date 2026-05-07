#include <crd/memory/allocator.hpp>
#include <crd/scene/archetype.hpp>
#include <crd/scene/archetype_chunk.hpp>
#include <crd/scene/archetype_graph.hpp>
#include <crd/scene/component.hpp>
#include <crd/scene/component_registry.hpp>

#include <catch2/catch_test_macros.hpp>

using crd::scene::Archetype;
using crd::scene::ArchetypeGraph;
using crd::scene::ArchetypeId;
using crd::scene::ChunkAllocator;
using crd::scene::ComponentId;
using crd::scene::ComponentMask;
using crd::scene::ComponentRegistry;
using crd::scene::kInvalidArchetypeId;

namespace
{
struct A
{
    int v{};
};
struct B
{
    int v{};
};
struct C
{
    int v{};
};
} // namespace

TEST_CASE("Fresh ArchetypeGraph is empty", "[scene][graph]")
{
    ComponentRegistry r;
    ChunkAllocator alloc;
    ArchetypeGraph g{crd::memory::default_allocator(), alloc, r};

    CHECK(g.archetype_count() == 0U);
    CHECK(g.by_id(ArchetypeId{0}) == nullptr);
    CHECK(g.by_id(kInvalidArchetypeId) == nullptr);
}

TEST_CASE("archetype_for(empty mask) returns the empty archetype", "[scene][graph]")
{
    ComponentRegistry r;
    ChunkAllocator alloc;
    ArchetypeGraph g{crd::memory::default_allocator(), alloc, r};

    Archetype& empty = g.archetype_for(ComponentMask{});
    CHECK(empty.id.raw == 0U);
    CHECK(empty.mask == ComponentMask{});
    CHECK(empty.layout.is_valid());
    CHECK(g.archetype_count() == 1U);
}

TEST_CASE("archetype_for is memoised - same mask returns same archetype", "[scene][graph]")
{
    ComponentRegistry r;
    ComponentId a = r.register_type<A>();
    ChunkAllocator alloc;
    ArchetypeGraph g{crd::memory::default_allocator(), alloc, r};

    ComponentMask m1{};
    m1.set(a);

    Archetype& first = g.archetype_for(m1);
    Archetype& second = g.archetype_for(m1);
    CHECK(&first == &second);
    CHECK(first.id == second.id);
    CHECK(g.archetype_count() == 1U);
}

TEST_CASE("Different masks produce different archetypes", "[scene][graph]")
{
    ComponentRegistry r;
    ComponentId a = r.register_type<A>();
    ComponentId b = r.register_type<B>();
    ChunkAllocator alloc;
    ArchetypeGraph g{crd::memory::default_allocator(), alloc, r};

    ComponentMask m_a{};
    m_a.set(a);
    ComponentMask m_b{};
    m_b.set(b);

    Archetype& a_arch = g.archetype_for(m_a);
    Archetype& b_arch = g.archetype_for(m_b);
    CHECK(a_arch.id != b_arch.id);
    CHECK(g.archetype_count() == 2U);
}

TEST_CASE("after_add navigates from A to A union {C}", "[scene][graph]")
{
    ComponentRegistry r;
    ComponentId a = r.register_type<A>();
    ComponentId b = r.register_type<B>();
    ChunkAllocator alloc;
    ArchetypeGraph g{crd::memory::default_allocator(), alloc, r};

    ComponentMask m_a{};
    m_a.set(a);
    Archetype& a_arch = g.archetype_for(m_a);

    Archetype& ab_arch = g.after_add(a_arch, b);
    CHECK(ab_arch.id != a_arch.id);
    CHECK(ab_arch.mask.test(a));
    CHECK(ab_arch.mask.test(b));
    CHECK(ab_arch.mask.popcount() == 2U);
}

TEST_CASE("after_add of an existing component is idempotent", "[scene][graph]")
{
    ComponentRegistry r;
    ComponentId a = r.register_type<A>();
    ChunkAllocator alloc;
    ArchetypeGraph g{crd::memory::default_allocator(), alloc, r};

    ComponentMask m_a{};
    m_a.set(a);
    Archetype& a_arch = g.archetype_for(m_a);

    Archetype& same = g.after_add(a_arch, a);
    CHECK(&same == &a_arch);
    CHECK(g.archetype_count() == 1U); // no new archetype created
}

TEST_CASE("after_add edge is memoised", "[scene][graph]")
{
    ComponentRegistry r;
    ComponentId a = r.register_type<A>();
    ComponentId b = r.register_type<B>();
    ChunkAllocator alloc;
    ArchetypeGraph g{crd::memory::default_allocator(), alloc, r};

    ComponentMask m_a{};
    m_a.set(a);
    Archetype& a_arch = g.archetype_for(m_a);
    Archetype& ab1 = g.after_add(a_arch, b);
    Archetype& ab2 = g.after_add(a_arch, b);
    CHECK(&ab1 == &ab2);
    // Edge cached on A.add_edges[b]:
    CHECK(a_arch.add_edges[b.raw] == ab1.id);
    // Reverse edge primed simultaneously:
    CHECK(ab1.remove_edges[b.raw] == a_arch.id);
}

TEST_CASE("after_remove navigates from A to A\\{C}", "[scene][graph]")
{
    ComponentRegistry r;
    ComponentId a = r.register_type<A>();
    ComponentId b = r.register_type<B>();
    ChunkAllocator alloc;
    ArchetypeGraph g{crd::memory::default_allocator(), alloc, r};

    ComponentMask m_ab{};
    m_ab.set(a);
    m_ab.set(b);
    Archetype& ab_arch = g.archetype_for(m_ab);

    Archetype& a_only = g.after_remove(ab_arch, b);
    CHECK(a_only.mask.test(a));
    CHECK_FALSE(a_only.mask.test(b));
    CHECK(a_only.mask.popcount() == 1U);
}

TEST_CASE("after_remove of an absent component is idempotent", "[scene][graph]")
{
    ComponentRegistry r;
    ComponentId a = r.register_type<A>();
    ComponentId b = r.register_type<B>();
    ChunkAllocator alloc;
    ArchetypeGraph g{crd::memory::default_allocator(), alloc, r};

    ComponentMask m_a{};
    m_a.set(a);
    Archetype& a_arch = g.archetype_for(m_a);

    Archetype& same = g.after_remove(a_arch, b);
    CHECK(&same == &a_arch);
}

TEST_CASE("Archetype edge tables are sized to kMaxComponents", "[scene][graph][archetype]")
{
    ComponentRegistry r;
    ComponentId a = r.register_type<A>();
    ChunkAllocator alloc;
    ArchetypeGraph g{crd::memory::default_allocator(), alloc, r};

    ComponentMask m_a{};
    m_a.set(a);
    Archetype& a_arch = g.archetype_for(m_a);

    CHECK(a_arch.add_edges.size() == static_cast<std::size_t>(crd::scene::kMaxComponents));
    CHECK(a_arch.remove_edges.size() == static_cast<std::size_t>(crd::scene::kMaxComponents));

    // All entries default to invalid until lookups populate them.
    for (auto edge : a_arch.add_edges)
    {
        CHECK(edge.is_null());
    }
}
