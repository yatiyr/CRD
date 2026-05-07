#include <crd/scene/archetype_chunk.hpp>
#include <crd/scene/component.hpp>
#include <crd/scene/component_registry.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <initializer_list>
#include <utility>

using crd::scene::Chunk;
using crd::scene::ChunkAllocator;
using crd::scene::ChunkHeader;
using crd::scene::ChunkLayout;
using crd::scene::ComponentId;
using crd::scene::ComponentMask;
using crd::scene::ComponentRegistry;
using crd::scene::compute_chunk_layout;
using crd::scene::EntityId;
using crd::scene::kChunkAlignment;
using crd::scene::kChunkSize;
using crd::scene::kMaxComponentsPerArchetype;

namespace
{
struct Position
{
    float x{}, y{}, z{};
};
struct Velocity
{
    float dx{}, dy{}, dz{};
};
struct Renderable
{
    crd::u64 mesh_handle{};
    crd::u64 material_handle{};
    crd::u32 flags{};
};
} // namespace

TEST_CASE("Empty mask yields layout with only entity-id array", "[scene][chunk][layout]")
{
    ComponentRegistry r;
    ComponentMask mask{};

    ChunkLayout layout = compute_chunk_layout(mask, r);
    REQUIRE(layout.is_valid());
    CHECK(layout.component_count() == 0U);
    CHECK(layout.components_sorted.size() == 0U);
    CHECK(layout.offsets.size() == 0U);

    // EntityId array starts after a 64-byte aligned header.
    CHECK(layout.entity_id_offset >= sizeof(ChunkHeader));
    CHECK((layout.entity_id_offset % kChunkAlignment) == 0U);

    // Capacity is bounded only by the EntityId array.
    const crd::usize body = kChunkSize - layout.entity_id_offset;
    CHECK(layout.entity_capacity == static_cast<crd::u32>(body / sizeof(EntityId)));
}

TEST_CASE("Single component layout has 64-byte aligned offsets", "[scene][chunk][layout]")
{
    ComponentRegistry r;
    ComponentId pos = r.register_type<Position>();

    ComponentMask mask{};
    mask.set(pos);

    ChunkLayout layout = compute_chunk_layout(mask, r);
    REQUIRE(layout.is_valid());
    CHECK(layout.component_count() == 1U);
    CHECK(layout.components_sorted[0] == pos);
    CHECK(layout.sizes[0] == sizeof(Position));
    CHECK((layout.entity_id_offset % kChunkAlignment) == 0U);
    CHECK((layout.offsets[0] % kChunkAlignment) == 0U);
    CHECK(layout.entity_capacity > 0U);

    // Verify the layout actually fits inside one chunk.
    const crd::usize end =
        static_cast<crd::usize>(layout.offsets[0]) + static_cast<crd::usize>(layout.entity_capacity) * sizeof(Position);
    CHECK(end <= kChunkSize);
}

TEST_CASE("Two-component layout fits and arrays are aligned", "[scene][chunk][layout]")
{
    ComponentRegistry r;
    ComponentId pos = r.register_type<Position>();
    ComponentId rnd = r.register_type<Renderable>();

    ComponentMask mask{};
    mask.set(pos);
    mask.set(rnd);

    ChunkLayout layout = compute_chunk_layout(mask, r);
    REQUIRE(layout.is_valid());
    REQUIRE(layout.component_count() == 2U);
    CHECK((layout.entity_id_offset % kChunkAlignment) == 0U);
    CHECK((layout.offsets[0] % kChunkAlignment) == 0U);
    CHECK((layout.offsets[1] % kChunkAlignment) == 0U);

    // Total bytes used must fit in kChunkSize.
    const crd::usize end = static_cast<crd::usize>(layout.offsets[1]) +
                           static_cast<crd::usize>(layout.entity_capacity) * static_cast<crd::usize>(layout.sizes[1]);
    CHECK(end <= kChunkSize);

    // Per-archetype version-counter array is sized in advance — verify the
    // archetype's component count fits.
    CHECK(layout.component_count() <= kMaxComponentsPerArchetype);
}

TEST_CASE("Layout sorts components by ComponentId regardless of mask order", "[scene][chunk][layout]")
{
    ComponentRegistry r;
    ComponentId a = r.register_type<Position>();   // gets id 0
    ComponentId b = r.register_type<Velocity>();   // gets id 1
    ComponentId c = r.register_type<Renderable>(); // gets id 2

    ComponentMask mask{};
    mask.set(c);
    mask.set(a);
    mask.set(b);

    ChunkLayout layout = compute_chunk_layout(mask, r);
    REQUIRE(layout.is_valid());
    REQUIRE(layout.component_count() == 3U);
    CHECK(layout.components_sorted[0] == a);
    CHECK(layout.components_sorted[1] == b);
    CHECK(layout.components_sorted[2] == c);

    // Offsets are monotonically increasing (sorted SoA arrays).
    CHECK(layout.offsets[0] < layout.offsets[1]);
    CHECK(layout.offsets[1] < layout.offsets[2]);
}

// A non-type template parameter creates a distinct type per N, so each
// register_type<TestSlot<N>>() instance gets its own ComponentId.
namespace
{
template <crd::u32 N> struct TestSlot
{
    char b;
};

template <crd::u32... Is> void register_test_slots(ComponentRegistry& r, std::integer_sequence<crd::u32, Is...>)
{
    (void)std::initializer_list<int>{((void)r.register_type<TestSlot<Is>>(), 0)...};
}
} // namespace

TEST_CASE("Layout returns invalid when component count exceeds the per-archetype cap", "[scene][chunk][layout]")
{
    ComponentRegistry r;

    constexpr crd::u32 kOverflow = kMaxComponentsPerArchetype + 1;
    register_test_slots(r, std::make_integer_sequence<crd::u32, kOverflow>{});
    REQUIRE(r.size() == kOverflow);

    ComponentMask mask{};
    for (crd::u16 i = 0; i < static_cast<crd::u16>(kOverflow); ++i)
    {
        mask.set(ComponentId{i});
    }

    ChunkLayout layout = compute_chunk_layout(mask, r);
    CHECK_FALSE(layout.is_valid());
    CHECK(layout.entity_capacity == 0U);
}

TEST_CASE("ChunkAllocator returns 64-byte aligned memory and zeros header", "[scene][chunk][allocator]")
{
    ChunkAllocator alloc;
    Chunk chunk = alloc.allocate();

    REQUIRE(chunk.memory != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(chunk.memory) % kChunkAlignment == 0U);

    // Header zeroed
    const ChunkHeader* h = chunk.header();
    REQUIRE(h != nullptr);
    CHECK(h->entity_count == 0U);
    CHECK(h->entity_capacity == 0U);
    CHECK(h->archetype_id == 0U);
    for (crd::u64 v : h->version_counter)
    {
        CHECK(v == 0U);
    }

    CHECK(alloc.outstanding() == 1U);
}

TEST_CASE("Multiple chunks have distinct memory", "[scene][chunk][allocator]")
{
    ChunkAllocator alloc;
    Chunk a = alloc.allocate();
    Chunk b = alloc.allocate();
    Chunk c = alloc.allocate();

    CHECK(a.memory != b.memory);
    CHECK(b.memory != c.memory);
    CHECK(a.memory != c.memory);
    CHECK(alloc.outstanding() == 3U);
}

TEST_CASE("ChunkAllocator::free clears the chunk and decrements outstanding", "[scene][chunk][allocator]")
{
    ChunkAllocator alloc;
    Chunk a = alloc.allocate();
    Chunk b = alloc.allocate();
    REQUIRE(alloc.outstanding() == 2U);

    alloc.free(a);
    CHECK(a.memory == nullptr);
    CHECK(alloc.outstanding() == 1U);
    CHECK(b.memory != nullptr);

    alloc.free(b);
    CHECK(b.memory == nullptr);
    CHECK(alloc.outstanding() == 0U);
}

TEST_CASE("ChunkAllocator destructor releases outstanding chunks (ASan leak check)", "[scene][chunk][allocator]")
{
    // No explicit `free` — rely on the dtor. Under win-asan this case fails
    // loudly if any chunk leaks.
    {
        ChunkAllocator alloc;
        (void)alloc.allocate();
        (void)alloc.allocate();
        (void)alloc.allocate();
        REQUIRE(alloc.outstanding() == 3U);
    }
    SUCCEED();
}

TEST_CASE("Chunk::header / entity_id_array / component_array return correct pointers", "[scene][chunk][accessors]")
{
    ComponentRegistry r;
    ComponentId pos = r.register_type<Position>();
    ComponentMask mask{};
    mask.set(pos);

    ChunkLayout layout = compute_chunk_layout(mask, r);
    REQUIRE(layout.is_valid());

    ChunkAllocator alloc;
    Chunk chunk = alloc.allocate();

    auto* header_ptr = chunk.header();
    CHECK(reinterpret_cast<std::uintptr_t>(header_ptr) == reinterpret_cast<std::uintptr_t>(chunk.memory));

    auto* eid = chunk.entity_id_array(layout);
    CHECK(reinterpret_cast<std::uintptr_t>(eid) ==
          reinterpret_cast<std::uintptr_t>(chunk.memory) + layout.entity_id_offset);

    void* comp0 = chunk.component_array(layout, 0);
    CHECK(reinterpret_cast<std::uintptr_t>(comp0) ==
          reinterpret_cast<std::uintptr_t>(chunk.memory) + layout.offsets[0]);
}

TEST_CASE("ChunkHeader version_counter array is sized to kMaxComponentsPerArchetype", "[scene][chunk][header]")
{
    static_assert(sizeof(ChunkHeader::version_counter) == sizeof(crd::u64) * kMaxComponentsPerArchetype,
                  "ChunkHeader.version_counter must be sized by kMaxComponentsPerArchetype");
    SUCCEED();
}
