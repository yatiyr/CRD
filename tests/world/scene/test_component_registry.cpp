#include <crd/scene/component.hpp>
#include <crd/scene/component_registry.hpp>
#include <crd/scene/storage_backend.hpp>
#include <crd/scene/world.hpp>

#include <catch2/catch_test_macros.hpp>
#include <type_traits>

using crd::scene::AsyncAware;
using crd::scene::ComponentId;
using crd::scene::ComponentInfo;
using crd::scene::ComponentMask;
using crd::scene::ComponentRegistry;
using crd::scene::EntityId;
using crd::scene::GpuResident;
using crd::scene::History;
using crd::scene::IStorageBackend;
using crd::scene::Reflection;
using crd::scene::Replication;
using crd::scene::SpatialBVH;
using crd::scene::StorageHint;
using crd::scene::World;

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

struct Health
{
    int hp = 0;
};

struct DialogTrigger
{
    int dialog_id = 0;
};

struct EditorSelected
{
}; // tag-only component (empty)

struct NonDefaultCtor
{
    explicit NonDefaultCtor(int) {}
};
} // namespace

TEST_CASE("ComponentId default-constructs to null", "[scene][registry]")
{
    ComponentId id{};
    CHECK(id.is_null());
    CHECK(id == ComponentId{});
    CHECK_FALSE(id == ComponentId{0});
}

TEST_CASE("Fresh ComponentRegistry has no registrations", "[scene][registry]")
{
    ComponentRegistry r;
    CHECK(r.size() == 0U);
    CHECK(r.id_of<Position>().is_null());
    CHECK(r.info(ComponentId{}) == nullptr);
    CHECK(r.info(ComponentId{0}) == nullptr);
}

TEST_CASE("register_type returns valid ComponentId for first registration", "[scene][registry]")
{
    ComponentRegistry r;
    ComponentId id = r.register_type<Position>();
    CHECK_FALSE(id.is_null());
    CHECK(r.size() == 1U);
    CHECK(r.id_of<Position>() == id);
}

TEST_CASE("Multiple component registrations get distinct, monotonic ids", "[scene][registry]")
{
    ComponentRegistry r;
    ComponentId a = r.register_type<Position>();
    ComponentId b = r.register_type<Velocity>();
    ComponentId c = r.register_type<Health>();

    CHECK(a.raw == 0);
    CHECK(b.raw == 1);
    CHECK(c.raw == 2);
    CHECK(a != b);
    CHECK(b != c);
    CHECK(a != c);
    CHECK(r.size() == 3U);
}

TEST_CASE("Idempotent re-registration returns the same ComponentId", "[scene][registry]")
{
    ComponentRegistry r;
    ComponentId first = r.register_type<Position>();
    ComponentId second = r.register_type<Position>();
    CHECK(first == second);
    CHECK(r.size() == 1U);

    // Even if the second call has different traits, the first registration wins.
    ComponentId third = r.register_type<Position>(StorageHint::SparseSet, AsyncAware{});
    CHECK(third == first);
    CHECK(r.info(third)->storage_hint == StorageHint::Archetype); // first call had no hint -> default
    CHECK_FALSE(r.info(third)->async_aware);                      // second-call traits ignored
    CHECK(r.size() == 1U);
}

TEST_CASE("id_of returns null for unregistered types", "[scene][registry]")
{
    ComponentRegistry r;
    r.register_type<Position>();
    CHECK_FALSE(r.id_of<Position>().is_null());
    CHECK(r.id_of<Velocity>().is_null());
    CHECK(r.id_of<Health>().is_null());
}

TEST_CASE("info round-trips size, alignment, and identity", "[scene][registry]")
{
    ComponentRegistry r;
    ComponentId id = r.register_type<Position>();

    const ComponentInfo* info = r.info(id);
    REQUIRE(info != nullptr);
    CHECK(info->id == id);
    CHECK(info->size == sizeof(Position));
    CHECK(info->alignment == alignof(Position));
    CHECK_FALSE(info->name.empty());
}

TEST_CASE("Default StorageHint is Archetype when no traits passed", "[scene][registry]")
{
    ComponentRegistry r;
    ComponentId id = r.register_type<Position>();
    REQUIRE(r.info(id) != nullptr);
    CHECK(r.info(id)->storage_hint == StorageHint::Archetype);
}

TEST_CASE("Explicit StorageHint::SparseSet is stored", "[scene][registry]")
{
    ComponentRegistry r;
    ComponentId id = r.register_type<DialogTrigger>(StorageHint::SparseSet);
    REQUIRE(r.info(id) != nullptr);
    CHECK(r.info(id)->storage_hint == StorageHint::SparseSet);
}

TEST_CASE("Index trait flags are stored", "[scene][registry]")
{
    ComponentRegistry r;
    ComponentId id = r.register_type<Position>(StorageHint::Archetype, AsyncAware{}, SpatialBVH{}, GpuResident{});
    const auto* info = r.info(id);
    REQUIRE(info != nullptr);
    CHECK(info->async_aware);
    CHECK(info->spatial_bvh);
    CHECK(info->gpu_resident);
    CHECK(info->history_window == 0); // not specified
}

TEST_CASE("History trait stores window value", "[scene][registry]")
{
    ComponentRegistry r;
    ComponentId id = r.register_type<Position>(History{60});
    REQUIRE(r.info(id) != nullptr);
    CHECK(r.info(id)->history_window == 60);
}

TEST_CASE("Replication policy is stored", "[scene][registry]")
{
    ComponentRegistry r;
    CHECK(r.info(r.register_type<Position>(Replication::ServerAuthoritative))->replication ==
          Replication::ServerAuthoritative);
    CHECK(r.info(r.register_type<Velocity>(Replication::ClientPredicted))->replication == Replication::ClientPredicted);
    CHECK(r.info(r.register_type<Health>())->replication == Replication::Local); // default
}

TEST_CASE("Reserved trait records (Reflection) round-trip", "[scene][registry]")
{
    Reflection refl{};
    refl.display_name = "TestComponent";

    ComponentRegistry r;
    ComponentId id = r.register_type<Position>(refl);
    REQUIRE(r.info(id) != nullptr);
    CHECK(r.info(id)->reflection.display_name == refl.display_name);
}

TEST_CASE("Lifecycle ops captured for default-constructible type", "[scene][registry]")
{
    ComponentRegistry r;
    ComponentId id = r.register_type<Position>();
    const auto* info = r.info(id);
    REQUIRE(info != nullptr);
    CHECK(info->default_construct != nullptr);
    CHECK(info->destruct != nullptr);
    CHECK(info->move_construct != nullptr);

    // Exercise the captured callbacks against raw storage.
    alignas(Position) unsigned char buf[sizeof(Position)];
    info->default_construct(buf);
    auto* p = reinterpret_cast<Position*>(buf);
    CHECK(p->x == 0.0F);
    CHECK(p->y == 0.0F);
    CHECK(p->z == 0.0F);
    info->destruct(buf);
}

TEST_CASE("Tag-only component registers without lifecycle issues", "[scene][registry]")
{
    ComponentRegistry r;
    ComponentId id = r.register_type<EditorSelected>(StorageHint::SparseSet);
    REQUIRE(r.info(id) != nullptr);
    CHECK(r.info(id)->size == sizeof(EditorSelected));
    CHECK(r.info(id)->storage_hint == StorageHint::SparseSet);
    CHECK(r.info(id)->default_construct != nullptr); // empty struct is default-constructible
}

TEST_CASE("Non-default-constructible type leaves default_construct null", "[scene][registry]")
{
    ComponentRegistry r;
    ComponentId id = r.register_type<NonDefaultCtor>();
    REQUIRE(r.info(id) != nullptr);
    CHECK(r.info(id)->default_construct == nullptr);
    CHECK(r.info(id)->destruct != nullptr); // destructible
}

// ---- ComponentMask ------------------------------------------------------

TEST_CASE("ComponentMask: default-constructed is empty", "[scene][registry][mask]")
{
    ComponentMask m;
    CHECK_FALSE(m.any());
    CHECK(m.popcount() == 0U);
    for (crd::u16 i = 0; i < 32; ++i)
    {
        CHECK_FALSE(m.test(ComponentId{i}));
    }
}

TEST_CASE("ComponentMask: set / test / clear", "[scene][registry][mask]")
{
    ComponentMask m;
    m.set(ComponentId{0});
    m.set(ComponentId{63});
    m.set(ComponentId{64});  // crosses word boundary
    m.set(ComponentId{255}); // last representable bit

    CHECK(m.test(ComponentId{0}));
    CHECK(m.test(ComponentId{63}));
    CHECK(m.test(ComponentId{64}));
    CHECK(m.test(ComponentId{255}));
    CHECK_FALSE(m.test(ComponentId{1}));
    CHECK_FALSE(m.test(ComponentId{62}));
    CHECK_FALSE(m.test(ComponentId{65}));

    CHECK(m.popcount() == 4U);
    CHECK(m.any());

    m.clear(ComponentId{63});
    CHECK_FALSE(m.test(ComponentId{63}));
    CHECK(m.popcount() == 3U);
}

TEST_CASE("ComponentMask: AND / OR", "[scene][registry][mask]")
{
    ComponentMask a;
    a.set(ComponentId{1});
    a.set(ComponentId{5});
    a.set(ComponentId{200});

    ComponentMask b;
    b.set(ComponentId{5});
    b.set(ComponentId{200});
    b.set(ComponentId{201});

    ComponentMask intersect = a & b;
    CHECK_FALSE(intersect.test(ComponentId{1}));
    CHECK(intersect.test(ComponentId{5}));
    CHECK(intersect.test(ComponentId{200}));
    CHECK_FALSE(intersect.test(ComponentId{201}));
    CHECK(intersect.popcount() == 2U);

    ComponentMask combined = a | b;
    CHECK(combined.test(ComponentId{1}));
    CHECK(combined.test(ComponentId{5}));
    CHECK(combined.test(ComponentId{200}));
    CHECK(combined.test(ComponentId{201}));
    CHECK(combined.popcount() == 4U);
}

// ---- IStorageBackend interface compiles -----------------------------------

namespace
{
class StubBackend : public IStorageBackend
{
public:
    void insert(EntityId, ComponentId, void*) override { ++m_inserts; }
    void remove(EntityId, ComponentId) override { ++m_removes; }
    [[nodiscard]] bool has(EntityId, ComponentId) const override { return false; }
    [[nodiscard]] void* get_mut(EntityId, ComponentId) override { return nullptr; }
    void for_each_chunk(ComponentMask, crd::scene::ChunkVisitor, void*) override {}
    void on_entity_destroyed(EntityId) override { ++m_destroys; }

    int m_inserts = 0;
    int m_removes = 0;
    int m_destroys = 0;
};
} // namespace

TEST_CASE("IStorageBackend supports polymorphic dispatch", "[scene][registry][storage]")
{
    StubBackend stub;
    IStorageBackend& iface = stub;

    iface.insert(EntityId::make(1, 1), ComponentId{3}, nullptr);
    iface.remove(EntityId::make(1, 1), ComponentId{3});
    iface.on_entity_destroyed(EntityId::make(1, 1));

    CHECK(stub.m_inserts == 1);
    CHECK(stub.m_removes == 1);
    CHECK(stub.m_destroys == 1);

    static_assert(std::has_virtual_destructor_v<IStorageBackend>);
}

// ---- World integration -----------------------------------------------------

TEST_CASE("World::register_component proxies through to ComponentRegistry", "[scene][world][registry]")
{
    World w;
    CHECK(w.registered_component_count() == 0U);

    ComponentId pos = w.register_component<Position>();
    ComponentId vel = w.register_component<Velocity>(StorageHint::Archetype, AsyncAware{}, History{8});

    CHECK_FALSE(pos.is_null());
    CHECK_FALSE(vel.is_null());
    CHECK(pos != vel);
    CHECK(w.registered_component_count() == 2U);
    CHECK(w.component_id<Position>() == pos);
    CHECK(w.component_id<Velocity>() == vel);

    const auto* info = w.component_info(vel);
    REQUIRE(info != nullptr);
    CHECK(info->async_aware);
    CHECK(info->history_window == 8);
}

TEST_CASE("World::register_component is idempotent", "[scene][world][registry]")
{
    World w;
    ComponentId a = w.register_component<Position>();
    ComponentId b = w.register_component<Position>();
    CHECK(a == b);
    CHECK(w.registered_component_count() == 1U);
}
