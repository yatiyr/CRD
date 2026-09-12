// Phase 3.0 v1m1 — ObekResource + ObekLoader + instantiate_obek tests (ADR-0058).
//
// Coverage (substrate + minimal round-trip; v1m2-5 extend):
//   - Empty obek loads with zero entities/components/relations.
//   - Single-entity obek with Transform round-trips bit-equal values.
//   - Multi-entity obek with several components round-trips.
//   - instantiate_obek(parent=null) leaves obek roots top-level.
//   - instantiate_obek(parent=alive) installs ChildOf(root → parent) on
//     each entity that lacked a ChildOf relation in the source obek.
//   - Two instantiations of the same ObekResource produce independent
//     entity sets that don't share state.
//   - Determinism: building the same World twice produces bit-equal bytes.
//   - ObekEntityGuid is stable across (root_id, file_idx).

#include <crd/math/vec.hpp>
#include <crd/resources/loader.hpp>
#include <crd/resources/resource_id.hpp>
#include <crd/scene/obek.hpp>
#include <crd/scene/relation.hpp>
#include <crd/scene/serialize.hpp>
#include <crd/scene/transform.hpp>
#include <crd/scene/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

using crd::math::Quatf;
using crd::math::Vec3f;
using crd::scene::EntityId;
using crd::scene::ObekArtifactBuilder;
using crd::scene::ObekLoader;
using crd::scene::ObekResource;
using crd::scene::Transform;
using crd::scene::World;
using crd::scene::relations::ChildOf;

namespace
{
constexpr crd::u32 kObekTestComponentFourCC = crd::scene::make_serialize_fourcc('O', 'T', 'S', 'T');

struct TestComponent
{
    crd::u32 a;
    crd::f32 b;
    crd::u32 c;
};

void setup_world(World& w)
{
    w.register_component<Transform>(crd::scene::transform_serialize_trait());
    w.register_component<crd::scene::TransformDirtyFlag>(crd::scene::StorageHint::SparseSet);
    w.register_component<TestComponent>(
        crd::scene::default_serialize_trait<TestComponent>(kObekTestComponentFourCC));
    w.register_builtin_relations();
}

[[nodiscard]] crd::containers::Array<crd::u8> build_obek(const World& w, crd::u64 root_id = 0xABCDU)
{
    ObekArtifactBuilder builder{crd::memory::default_allocator(),
                                crd::resources::ResourceId{0xCAFE'BABEULL, 0xDEAD'BEEFULL}, root_id};
    return builder.build(w);
}

[[nodiscard]] ObekResource* load_obek(const crd::containers::Array<crd::u8>& bytes)
{
    ObekLoader loader;
    crd::resources::LoadContext ctx{};
    ctx.id        = crd::resources::ResourceId{0xCAFE'BABEULL, 0xDEAD'BEEFULL};
    ctx.bytes     = crd::containers::ConstSpan<crd::u8>{bytes.data(), bytes.size()};
    ctx.allocator = crd::memory::default_allocator();
    return static_cast<ObekResource*>(loader.load(ctx));
}

void unload_obek(ObekResource* res)
{
    if (res != nullptr)
    {
        ObekLoader loader;
        loader.unload(res);
    }
}

[[nodiscard]] bool approx(crd::f32 a, crd::f32 b, crd::f32 tol = 1e-5F)
{
    const crd::f32 d = a - b;
    return (d < tol) && (-d < tol);
}

} // namespace

// -----------------------------------------------------------------------------
// Empty
// -----------------------------------------------------------------------------

TEST_CASE("Empty obek loads with zero entities", "[obek][empty]")
{
    World w;
    setup_world(w);
    auto bytes = build_obek(w);
    auto* res = load_obek(bytes);
    REQUIRE(res != nullptr);
    CHECK(res->info.schema_version == crd::scene::kObekSchemaVersion);
    CHECK(res->info.entity_count == 0U);
    CHECK(res->info.relation_count == 0U);
    unload_obek(res);
}

// -----------------------------------------------------------------------------
// Single entity round-trip
// -----------------------------------------------------------------------------

TEST_CASE("Single-entity obek round-trips Transform values", "[obek][round-trip][single]")
{
    World source;
    setup_world(source);
    EntityId e = source.spawn();
    source.add_component<Transform>(e, Transform{});
    source.set_translation(e, Vec3f{1.0F, 2.0F, 3.0F});

    auto bytes = build_obek(source);
    auto* res = load_obek(bytes);
    REQUIRE(res != nullptr);
    CHECK(res->info.entity_count == 1U);

    World target;
    setup_world(target);
    auto inst = target.instantiate_obek(*res);

    REQUIRE(inst.entities.size() == 1U);
    const Transform* t = target.get_component<Transform>(inst.entities[0]);
    REQUIRE(t != nullptr);
    CHECK(approx(t->translation.x.value, 1.0F));
    CHECK(approx(t->translation.y.value, 2.0F));
    CHECK(approx(t->translation.z.value, 3.0F));

    unload_obek(res);
}

// -----------------------------------------------------------------------------
// Multi-entity + custom component round-trip
// -----------------------------------------------------------------------------

TEST_CASE("Multi-entity obek with TestComponent round-trips", "[obek][round-trip][multi]")
{
    World source;
    setup_world(source);
    EntityId e0 = source.spawn();
    EntityId e1 = source.spawn();
    EntityId e2 = source.spawn();
    source.add_component<TestComponent>(e0, TestComponent{1U, 2.0F, 3U});
    source.add_component<TestComponent>(e1, TestComponent{4U, 5.0F, 6U});
    source.add_component<TestComponent>(e2, TestComponent{7U, 8.0F, 9U});

    auto bytes = build_obek(source);
    auto* res = load_obek(bytes);
    REQUIRE(res != nullptr);
    CHECK(res->info.entity_count == 3U);

    World target;
    setup_world(target);
    auto inst = target.instantiate_obek(*res);
    REQUIRE(inst.entities.size() == 3U);

    const TestComponent* c0 = target.get_component<TestComponent>(inst.entities[0]);
    const TestComponent* c1 = target.get_component<TestComponent>(inst.entities[1]);
    const TestComponent* c2 = target.get_component<TestComponent>(inst.entities[2]);
    REQUIRE(c0 != nullptr);
    REQUIRE(c1 != nullptr);
    REQUIRE(c2 != nullptr);
    CHECK(c0->a == 1U);
    CHECK(c1->a == 4U);
    CHECK(c2->a == 7U);
    CHECK(approx(c0->b, 2.0F));
    CHECK(approx(c1->b, 5.0F));
    CHECK(approx(c2->b, 8.0F));

    unload_obek(res);
}

// -----------------------------------------------------------------------------
// Reparent semantics
// -----------------------------------------------------------------------------

TEST_CASE("instantiate_obek(null parent) leaves roots top-level", "[obek][reparent]")
{
    World source;
    setup_world(source);
    EntityId root = source.spawn();
    EntityId child = source.spawn();
    source.add_component<Transform>(root, Transform{});
    source.add_component<Transform>(child, Transform{});
    source.add_relation<ChildOf>(child, root);

    auto bytes = build_obek(source);
    auto* res = load_obek(bytes);
    REQUIRE(res != nullptr);

    World target;
    setup_world(target);
    auto inst = target.instantiate_obek(*res, EntityId::null());
    REQUIRE(inst.entities.size() == 2U);

    // Root has no ChildOf in source → stays top-level (no ChildOf in target either).
    CHECK(!target.has_relation<ChildOf>(inst.entities[0]));
    // Child kept its ChildOf to root.
    CHECK(target.get_relation_target<ChildOf>(inst.entities[1]) == inst.entities[0]);

    unload_obek(res);
}

TEST_CASE("instantiate_obek(alive parent) installs ChildOf(root,parent)", "[obek][reparent]")
{
    World source;
    setup_world(source);
    EntityId root_a = source.spawn();
    EntityId root_b = source.spawn();
    EntityId child_a = source.spawn();
    source.add_component<Transform>(root_a, Transform{});
    source.add_component<Transform>(root_b, Transform{});
    source.add_component<Transform>(child_a, Transform{});
    source.add_relation<ChildOf>(child_a, root_a);

    auto bytes = build_obek(source);
    auto* res = load_obek(bytes);
    REQUIRE(res != nullptr);

    World target;
    setup_world(target);
    EntityId anchor = target.spawn();
    target.add_component<Transform>(anchor, Transform{});

    auto inst = target.instantiate_obek(*res, anchor);
    REQUIRE(inst.entities.size() == 3U);

    // Both roots now ChildOf(anchor); child_a's ChildOf(root_a) is preserved.
    CHECK(target.get_relation_target<ChildOf>(inst.entities[0]) == anchor);
    CHECK(target.get_relation_target<ChildOf>(inst.entities[1]) == anchor);
    CHECK(target.get_relation_target<ChildOf>(inst.entities[2]) == inst.entities[0]);

    unload_obek(res);
}

// -----------------------------------------------------------------------------
// Independent instantiations
// -----------------------------------------------------------------------------

TEST_CASE("Two instantiations of one obek don't share entities", "[obek][independence]")
{
    World source;
    setup_world(source);
    EntityId e = source.spawn();
    source.add_component<TestComponent>(e, TestComponent{42U, 1.0F, 100U});

    auto bytes = build_obek(source);
    auto* res = load_obek(bytes);
    REQUIRE(res != nullptr);

    World target;
    setup_world(target);
    auto a = target.instantiate_obek(*res);
    auto b = target.instantiate_obek(*res);

    REQUIRE(a.entities.size() == 1U);
    REQUIRE(b.entities.size() == 1U);
    CHECK(a.entities[0] != b.entities[0]);

    // Mutate one; other unchanged.
    if (auto* mut = target.get_component_mut<TestComponent>(a.entities[0]); mut != nullptr)
    {
        mut->a = 999U;
    }
    const TestComponent* ca = target.get_component<TestComponent>(a.entities[0]);
    const TestComponent* cb = target.get_component<TestComponent>(b.entities[0]);
    REQUIRE(ca != nullptr);
    REQUIRE(cb != nullptr);
    CHECK(ca->a == 999U);
    CHECK(cb->a == 42U);

    unload_obek(res);
}

// -----------------------------------------------------------------------------
// Determinism
// -----------------------------------------------------------------------------

TEST_CASE("Determinism: identical world produces bit-equal obek bytes", "[obek][determinism]")
{
    auto cook = [&]() {
        World w;
        setup_world(w);
        for (crd::u32 i = 0; i < 5U; ++i)
        {
            EntityId e = w.spawn();
            w.add_component<Transform>(e, Transform{});
            w.set_translation(e, Vec3f{static_cast<crd::f32>(i), 0.0F, 0.0F});
            w.add_component<TestComponent>(e, TestComponent{i, static_cast<crd::f32>(i), i * 10U});
        }
        return build_obek(w, /*root_id=*/0xDEAD0001ULL);
    };

    auto a = cook();
    auto b = cook();
    REQUIRE(a.size() == b.size());
    CHECK(std::memcmp(a.data(), b.data(), a.size()) == 0);
}

// -----------------------------------------------------------------------------
// ObekEntityGuid stability
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Override patches (v1m2 phase A)
// -----------------------------------------------------------------------------

TEST_CASE("Override patch by file_idx replaces a Transform field", "[obek][override][by-idx]")
{
    World source;
    setup_world(source);
    EntityId e = source.spawn();
    source.add_component<Transform>(e, Transform{});
    source.set_translation(e, Vec3f{1.0F, 2.0F, 3.0F});

    auto bytes = build_obek(source);
    auto* res = load_obek(bytes);
    REQUIRE(res != nullptr);

    World target;
    setup_world(target);

    // Patch translation = (10, 20, 30).
    Vec3f new_translation{10.0F, 20.0F, 30.0F};
    crd::scene::ObekOverride patch{};
    patch.file_idx         = 0U;
    patch.component_fourcc = crd::scene::kFourCC_Transform;
    patch.field_offset     = static_cast<crd::u32>(offsetof(Transform, translation));
    patch.payload          = crd::containers::ConstSpan<crd::u8>{
        reinterpret_cast<const crd::u8*>(&new_translation), sizeof(Vec3f)};

    crd::scene::ObekOverride patches[] = {patch};
    auto inst = target.instantiate_obek(*res, EntityId::null(),
                                        crd::containers::ConstSpan<crd::scene::ObekOverride>{patches, 1});

    CHECK(inst.overrides_applied == 1U);
    CHECK(inst.overrides_skipped == 0U);
    const Transform* t = target.get_component<Transform>(inst.entities[0]);
    REQUIRE(t != nullptr);
    CHECK(approx(t->translation.x.value, 10.0F));
    CHECK(approx(t->translation.y.value, 20.0F));
    CHECK(approx(t->translation.z.value, 30.0F));

    unload_obek(res);
}

TEST_CASE("Override skipped when file_idx out of range", "[obek][override][skip]")
{
    World source;
    setup_world(source);
    EntityId e = source.spawn();
    source.add_component<TestComponent>(e, TestComponent{1U, 1.0F, 1U});

    auto bytes = build_obek(source);
    auto* res = load_obek(bytes);
    REQUIRE(res != nullptr);

    World target;
    setup_world(target);

    crd::u32 bogus_value = 999U;
    crd::scene::ObekOverride patch{};
    patch.file_idx         = 42U;  // out of range — only 1 entity
    patch.component_fourcc = kObekTestComponentFourCC;
    patch.field_offset     = static_cast<crd::u32>(offsetof(TestComponent, a));
    patch.payload          = crd::containers::ConstSpan<crd::u8>{
        reinterpret_cast<const crd::u8*>(&bogus_value), sizeof(crd::u32)};

    crd::scene::ObekOverride patches[] = {patch};
    auto inst = target.instantiate_obek(*res, EntityId::null(),
                                        crd::containers::ConstSpan<crd::scene::ObekOverride>{patches, 1});

    CHECK(inst.overrides_applied == 0U);
    CHECK(inst.overrides_skipped == 1U);
    // Original value preserved.
    const TestComponent* c = target.get_component<TestComponent>(inst.entities[0]);
    REQUIRE(c != nullptr);
    CHECK(c->a == 1U);

    unload_obek(res);
}

TEST_CASE("Override skipped when component fourcc unregistered", "[obek][override][skip]")
{
    World source;
    setup_world(source);
    EntityId e = source.spawn();
    source.add_component<TestComponent>(e, TestComponent{1U, 1.0F, 1U});

    auto bytes = build_obek(source);
    auto* res = load_obek(bytes);
    REQUIRE(res != nullptr);

    World target;
    setup_world(target);

    crd::u32 some_value = 999U;
    crd::scene::ObekOverride patch{};
    patch.file_idx         = 0U;
    patch.component_fourcc = crd::scene::make_serialize_fourcc('X', 'X', 'X', 'X');  // not registered
    patch.field_offset     = 0U;
    patch.payload          = crd::containers::ConstSpan<crd::u8>{
        reinterpret_cast<const crd::u8*>(&some_value), sizeof(crd::u32)};

    crd::scene::ObekOverride patches[] = {patch};
    auto inst = target.instantiate_obek(*res, EntityId::null(),
                                        crd::containers::ConstSpan<crd::scene::ObekOverride>{patches, 1});

    CHECK(inst.overrides_applied == 0U);
    CHECK(inst.overrides_skipped == 1U);

    unload_obek(res);
}

TEST_CASE("Multiple overrides applied in order", "[obek][override][multi]")
{
    World source;
    setup_world(source);
    EntityId e0 = source.spawn();
    EntityId e1 = source.spawn();
    source.add_component<TestComponent>(e0, TestComponent{0U, 0.0F, 0U});
    source.add_component<TestComponent>(e1, TestComponent{0U, 0.0F, 0U});

    auto bytes = build_obek(source);
    auto* res = load_obek(bytes);
    REQUIRE(res != nullptr);

    World target;
    setup_world(target);

    crd::u32 v0 = 100U;
    crd::u32 v1 = 200U;
    crd::scene::ObekOverride patches[2]{};
    patches[0].file_idx         = 0U;
    patches[0].component_fourcc = kObekTestComponentFourCC;
    patches[0].field_offset     = static_cast<crd::u32>(offsetof(TestComponent, a));
    patches[0].payload          = crd::containers::ConstSpan<crd::u8>{
        reinterpret_cast<const crd::u8*>(&v0), sizeof(crd::u32)};
    patches[1].file_idx         = 1U;
    patches[1].component_fourcc = kObekTestComponentFourCC;
    patches[1].field_offset     = static_cast<crd::u32>(offsetof(TestComponent, c));
    patches[1].payload          = crd::containers::ConstSpan<crd::u8>{
        reinterpret_cast<const crd::u8*>(&v1), sizeof(crd::u32)};

    auto inst = target.instantiate_obek(*res, EntityId::null(),
                                        crd::containers::ConstSpan<crd::scene::ObekOverride>{patches, 2});

    CHECK(inst.overrides_applied == 2U);
    CHECK(inst.overrides_skipped == 0U);
    const TestComponent* c0 = target.get_component<TestComponent>(inst.entities[0]);
    const TestComponent* c1 = target.get_component<TestComponent>(inst.entities[1]);
    REQUIRE(c0 != nullptr);
    REQUIRE(c1 != nullptr);
    CHECK(c0->a == 100U);
    CHECK(c0->c == 0U);
    CHECK(c1->a == 0U);
    CHECK(c1->c == 200U);

    unload_obek(res);
}

TEST_CASE("ObekEntityGuid is stable across (root_id, file_idx)", "[obek][guid]")
{
    const auto g1 = crd::scene::make_obek_entity_guid(0xDEAD'BEEFULL, 7U);
    const auto g2 = crd::scene::make_obek_entity_guid(0xDEAD'BEEFULL, 7U);
    const auto g3 = crd::scene::make_obek_entity_guid(0xDEAD'BEEFULL, 8U);
    const auto g4 = crd::scene::make_obek_entity_guid(0xCAFE'BABEULL, 7U);
    CHECK(g1 == g2);
    CHECK(g1 != g3);
    CHECK(g1 != g4);
    CHECK(g3 != g4);
}

// -----------------------------------------------------------------------------
// InheritPolicy (v1m4)
// -----------------------------------------------------------------------------

namespace
{
constexpr crd::u32 kRuntimeStateFourCC = crd::scene::make_serialize_fourcc('R', 'T', 'S', 'T');
constexpr crd::u32 kSharedDataFourCC   = crd::scene::make_serialize_fourcc('S', 'H', 'R', 'D');

// Trivially-copyable test components.
struct RuntimeState  // for DontInherit: should never persist through öbek instantiation
{
    crd::u32 frame_counter;
    crd::u32 generation;
};

struct SharedData    // for Inherit: many instances might share this static data
{
    crd::u32 mesh_id;
    crd::u32 lod_bucket;
};
} // namespace

TEST_CASE("InheritPolicy enum default is Override on a freshly registered component", "[obek][inherit]")
{
    World w;
    w.register_component<RuntimeState>(
        crd::scene::default_serialize_trait<RuntimeState>(kRuntimeStateFourCC));
    const crd::scene::ComponentId id = w.component_id<RuntimeState>();
    REQUIRE(!id.is_null());
    const auto* info = w.component_info(id);
    REQUIRE(info != nullptr);
    CHECK(info->inherit_policy == crd::scene::InheritPolicy::Override);
}

TEST_CASE("Component registered with InheritPolicy::DontInherit stamps the trait", "[obek][inherit]")
{
    World w;
    w.register_component<RuntimeState>(
        crd::scene::default_serialize_trait<RuntimeState>(kRuntimeStateFourCC),
        crd::scene::InheritPolicy::DontInherit);
    const crd::scene::ComponentId id = w.component_id<RuntimeState>();
    const auto* info = w.component_info(id);
    REQUIRE(info != nullptr);
    CHECK(info->inherit_policy == crd::scene::InheritPolicy::DontInherit);
}

TEST_CASE("InheritPolicy::DontInherit skips component during instantiate_obek", "[obek][inherit][dont]")
{
    // Source: register RuntimeState as Override (default), populate value.
    World source;
    setup_world(source);
    source.register_component<RuntimeState>(
        crd::scene::default_serialize_trait<RuntimeState>(kRuntimeStateFourCC));
    EntityId e = source.spawn();
    source.add_component<Transform>(e, Transform{});
    source.add_component<RuntimeState>(e, RuntimeState{42U, 7U});

    auto bytes = build_obek(source);
    auto* res = load_obek(bytes);
    REQUIRE(res != nullptr);

    // Target: register the SAME component as DontInherit — it should be
    // skipped at instantiate even though the source öbek has it cooked in.
    World target;
    setup_world(target);
    target.register_component<RuntimeState>(
        crd::scene::default_serialize_trait<RuntimeState>(kRuntimeStateFourCC),
        crd::scene::InheritPolicy::DontInherit);

    auto inst = target.instantiate_obek(*res);
    REQUIRE(inst.entities.size() == 1U);
    CHECK(target.has_component<Transform>(inst.entities[0]));
    // RuntimeState was DontInherit → entity does NOT have it after instantiate.
    CHECK(!target.has_component<RuntimeState>(inst.entities[0]));
    CHECK(inst.components_skipped == 1U);
    unload_obek(res);
}

TEST_CASE("InheritPolicy::Inherit behaves as Override at v1m4 (CoW deferred to v1m4b)",
          "[obek][inherit][api-stub]")
{
    // Document v1m4 contract: Inherit observable behavior == Override.
    // The CoW backend optimization lands in v1m4b; consumers can safely
    // declare Inherit today and migrate transparently when CoW ships.
    World source;
    setup_world(source);
    source.register_component<SharedData>(
        crd::scene::default_serialize_trait<SharedData>(kSharedDataFourCC));
    EntityId e0 = source.spawn();
    EntityId e1 = source.spawn();
    source.add_component<SharedData>(e0, SharedData{100U, 0U});
    source.add_component<SharedData>(e1, SharedData{200U, 1U});

    auto bytes = build_obek(source);
    auto* res = load_obek(bytes);
    REQUIRE(res != nullptr);

    // Target registers SharedData as Inherit.
    World target;
    setup_world(target);
    target.register_component<SharedData>(
        crd::scene::default_serialize_trait<SharedData>(kSharedDataFourCC),
        crd::scene::InheritPolicy::Inherit);

    auto inst = target.instantiate_obek(*res);
    REQUIRE(inst.entities.size() == 2U);

    // Each instance has its own value (Override semantics).
    const SharedData* d0 = target.get_component<SharedData>(inst.entities[0]);
    const SharedData* d1 = target.get_component<SharedData>(inst.entities[1]);
    REQUIRE(d0 != nullptr);
    REQUIRE(d1 != nullptr);
    CHECK(d0->mesh_id == 100U);
    CHECK(d1->mesh_id == 200U);

    // Mutating one does not affect the other.
    if (auto* mut = target.get_component_mut<SharedData>(inst.entities[0]); mut != nullptr)
    {
        mut->mesh_id = 999U;
    }
    CHECK(target.get_component<SharedData>(inst.entities[0])->mesh_id == 999U);
    CHECK(target.get_component<SharedData>(inst.entities[1])->mesh_id == 200U);

    unload_obek(res);
}

TEST_CASE("Mixed InheritPolicy registration: Override + DontInherit on same World",
          "[obek][inherit][mixed]")
{
    World source;
    setup_world(source);
    source.register_component<RuntimeState>(
        crd::scene::default_serialize_trait<RuntimeState>(kRuntimeStateFourCC));
    source.register_component<SharedData>(
        crd::scene::default_serialize_trait<SharedData>(kSharedDataFourCC));
    EntityId e = source.spawn();
    source.add_component<Transform>(e, Transform{});
    source.add_component<RuntimeState>(e, RuntimeState{1U, 1U});
    source.add_component<SharedData>(e, SharedData{42U, 0U});

    auto bytes = build_obek(source);
    auto* res = load_obek(bytes);
    REQUIRE(res != nullptr);

    World target;
    setup_world(target);
    // Mixed registration: RuntimeState is DontInherit, SharedData is Override.
    target.register_component<RuntimeState>(
        crd::scene::default_serialize_trait<RuntimeState>(kRuntimeStateFourCC),
        crd::scene::InheritPolicy::DontInherit);
    target.register_component<SharedData>(
        crd::scene::default_serialize_trait<SharedData>(kSharedDataFourCC),
        crd::scene::InheritPolicy::Override);

    auto inst = target.instantiate_obek(*res);
    REQUIRE(inst.entities.size() == 1U);
    CHECK(target.has_component<Transform>(inst.entities[0]));
    CHECK(!target.has_component<RuntimeState>(inst.entities[0]));  // DontInherit skipped
    CHECK(target.has_component<SharedData>(inst.entities[0]));      // Override kept
    CHECK(target.get_component<SharedData>(inst.entities[0])->mesh_id == 42U);
    CHECK(inst.components_skipped == 1U);
    unload_obek(res);
}

// -----------------------------------------------------------------------------
// InheritPolicy::Inherit — shared-pool plumbing (v1m4b2)
// -----------------------------------------------------------------------------

TEST_CASE("InheritPolicy::Inherit forces SparseSet storage at registration", "[obek][inherit][cow]")
{
    World w;
    w.register_component<SharedData>(
        crd::scene::default_serialize_trait<SharedData>(kSharedDataFourCC),
        crd::scene::InheritPolicy::Inherit,
        crd::scene::StorageHint::Archetype);  // explicit Archetype request — should be overridden
    const crd::scene::ComponentId id = w.component_id<SharedData>();
    const auto* info = w.component_info(id);
    REQUIRE(info != nullptr);
    CHECK(info->inherit_policy == crd::scene::InheritPolicy::Inherit);
    CHECK(info->storage_hint == crd::scene::StorageHint::SparseSet);
}

TEST_CASE("Inherit-instantiated component reads through shared pool", "[obek][inherit][cow]")
{
    World source;
    setup_world(source);
    source.register_component<SharedData>(
        crd::scene::default_serialize_trait<SharedData>(kSharedDataFourCC),
        crd::scene::InheritPolicy::Inherit);
    EntityId src_e = source.spawn();
    source.add_component<SharedData>(src_e, SharedData{555U, 7U});

    auto bytes = build_obek(source);
    auto* res = load_obek(bytes);
    REQUIRE(res != nullptr);

    World target;
    setup_world(target);
    target.register_component<SharedData>(
        crd::scene::default_serialize_trait<SharedData>(kSharedDataFourCC),
        crd::scene::InheritPolicy::Inherit);

    auto inst = target.instantiate_obek(*res);
    REQUIRE(inst.entities.size() == 1U);
    const SharedData* d = target.get_component<SharedData>(inst.entities[0]);
    REQUIRE(d != nullptr);
    CHECK(d->mesh_id == 555U);
    CHECK(d->lod_bucket == 7U);
    unload_obek(res);
}

TEST_CASE("Multiple instantiate calls of same Inherit data dedupe to ONE pool entry",
          "[obek][inherit][cow][dedup]")
{
    World source;
    setup_world(source);
    source.register_component<SharedData>(
        crd::scene::default_serialize_trait<SharedData>(kSharedDataFourCC),
        crd::scene::InheritPolicy::Inherit);
    EntityId src_e = source.spawn();
    source.add_component<SharedData>(src_e, SharedData{42U, 0U});

    auto bytes = build_obek(source);
    auto* res = load_obek(bytes);
    REQUIRE(res != nullptr);

    World target;
    setup_world(target);
    target.register_component<SharedData>(
        crd::scene::default_serialize_trait<SharedData>(kSharedDataFourCC),
        crd::scene::InheritPolicy::Inherit);

    // Three independent instantiate_obek calls of the same source.
    auto i1 = target.instantiate_obek(*res);
    auto i2 = target.instantiate_obek(*res);
    auto i3 = target.instantiate_obek(*res);

    // Each call spawned 1 entity → 3 total entities.
    CHECK(i1.entities.size() == 1U);
    CHECK(i2.entities.size() == 1U);
    CHECK(i3.entities.size() == 1U);

    // All three share the SAME pool entry: shared-pool live_count == 1.
    const crd::scene::ComponentId cid = target.component_id<SharedData>();
    CHECK(target.sparse_storage().shared_pool_live_count(cid) == 1U);

    // Verify each entity reads the correct value.
    CHECK(target.get_component<SharedData>(i1.entities[0])->mesh_id == 42U);
    CHECK(target.get_component<SharedData>(i2.entities[0])->mesh_id == 42U);
    CHECK(target.get_component<SharedData>(i3.entities[0])->mesh_id == 42U);

    unload_obek(res);
}

TEST_CASE("Distinct Inherit values get distinct pool entries", "[obek][inherit][cow][dedup]")
{
    // Source has two entities with DIFFERENT SharedData values.
    World source;
    setup_world(source);
    source.register_component<SharedData>(
        crd::scene::default_serialize_trait<SharedData>(kSharedDataFourCC),
        crd::scene::InheritPolicy::Inherit);
    EntityId e0 = source.spawn();
    EntityId e1 = source.spawn();
    source.add_component<SharedData>(e0, SharedData{1U, 0U});
    source.add_component<SharedData>(e1, SharedData{2U, 0U});

    auto bytes = build_obek(source);
    auto* res = load_obek(bytes);
    REQUIRE(res != nullptr);

    World target;
    setup_world(target);
    target.register_component<SharedData>(
        crd::scene::default_serialize_trait<SharedData>(kSharedDataFourCC),
        crd::scene::InheritPolicy::Inherit);

    auto inst = target.instantiate_obek(*res);
    REQUIRE(inst.entities.size() == 2U);

    // Two distinct values → two pool entries.
    const crd::scene::ComponentId cid = target.component_id<SharedData>();
    CHECK(target.sparse_storage().shared_pool_live_count(cid) == 2U);
    unload_obek(res);
}

TEST_CASE("Refcount eviction: pool entry freed when last sharer destroyed",
          "[obek][inherit][cow][eviction]")
{
    World source;
    setup_world(source);
    source.register_component<SharedData>(
        crd::scene::default_serialize_trait<SharedData>(kSharedDataFourCC),
        crd::scene::InheritPolicy::Inherit);
    EntityId src_e = source.spawn();
    source.add_component<SharedData>(src_e, SharedData{99U, 0U});

    auto bytes = build_obek(source);
    auto* res = load_obek(bytes);
    REQUIRE(res != nullptr);

    World target;
    setup_world(target);
    target.register_component<SharedData>(
        crd::scene::default_serialize_trait<SharedData>(kSharedDataFourCC),
        crd::scene::InheritPolicy::Inherit);

    auto i1 = target.instantiate_obek(*res);
    auto i2 = target.instantiate_obek(*res);
    const crd::scene::ComponentId cid = target.component_id<SharedData>();
    CHECK(target.sparse_storage().shared_pool_live_count(cid) == 1U);

    // Destroy first instance — pool entry should still be live (one sharer left).
    target.destroy_immediate(i1.entities[0]);
    CHECK(target.sparse_storage().shared_pool_live_count(cid) == 1U);

    // Destroy second — pool entry refcount drops to 0; entry freed.
    target.destroy_immediate(i2.entities[0]);
    CHECK(target.sparse_storage().shared_pool_live_count(cid) == 0U);

    unload_obek(res);
}

TEST_CASE("get_mut on Inherit slot breaks the share (CoW)", "[obek][inherit][cow]")
{
    World source;
    setup_world(source);
    source.register_component<SharedData>(
        crd::scene::default_serialize_trait<SharedData>(kSharedDataFourCC),
        crd::scene::InheritPolicy::Inherit);
    EntityId src_e = source.spawn();
    source.add_component<SharedData>(src_e, SharedData{100U, 0U});

    auto bytes = build_obek(source);
    auto* res = load_obek(bytes);
    REQUIRE(res != nullptr);

    World target;
    setup_world(target);
    target.register_component<SharedData>(
        crd::scene::default_serialize_trait<SharedData>(kSharedDataFourCC),
        crd::scene::InheritPolicy::Inherit);

    auto inst = target.instantiate_obek(*res);
    REQUIRE(inst.entities.size() == 1U);

    // Initial read goes through shared pool.
    CHECK(target.get_component<SharedData>(inst.entities[0])->mesh_id == 100U);

    // Mutate via get_mut — CoW breaks the share, copies bytes inline.
    if (auto* mut = target.get_component_mut<SharedData>(inst.entities[0]); mut != nullptr)
    {
        mut->mesh_id = 999U;
    }
    // Subsequent reads see the mutated value.
    CHECK(target.get_component<SharedData>(inst.entities[0])->mesh_id == 999U);

    unload_obek(res);
}

// -----------------------------------------------------------------------------
// AAAA-tier batch instantiation reservations (v1m5b)
// -----------------------------------------------------------------------------

TEST_CASE("instantiate_obek_batch spawns count entities per slot", "[obek][batch]")
{
    World source;
    setup_world(source);
    EntityId src_e = source.spawn();
    source.add_component<Transform>(src_e, Transform{});

    auto bytes = build_obek(source);
    auto* res = load_obek(bytes);
    REQUIRE(res != nullptr);

    World target;
    setup_world(target);

    constexpr crd::u32 slot_count = 5U;
    auto batch = target.instantiate_obek_batch(*res, slot_count);
    CHECK(batch.value != 0U);
    // Each slot spawned 1 entity (source has 1 entity); batch total = 5.
    CHECK(target.entity_count() == slot_count);
    unload_obek(res);
}

TEST_CASE("BatchInstanceTag is added when registered on the target World", "[obek][batch][tag]")
{
    World source;
    setup_world(source);
    EntityId src_e = source.spawn();
    source.add_component<Transform>(src_e, Transform{});

    auto bytes = build_obek(source);
    auto* res = load_obek(bytes);
    REQUIRE(res != nullptr);

    World target;
    setup_world(target);
    target.register_component<crd::scene::BatchInstanceTag>(
        crd::scene::default_serialize_trait<crd::scene::BatchInstanceTag>(
            crd::scene::kFourCC_BatchInstanceTag));

    constexpr crd::u32 slot_count = 3U;
    auto batch = target.instantiate_obek_batch(*res, slot_count);

    // Each spawned entity should have BatchInstanceTag with matching batch handle.
    crd::u32 tagged_count = 0;
    for (EntityId e : target)
    {
        if (const auto* tag = target.get_component<crd::scene::BatchInstanceTag>(e); tag != nullptr)
        {
            CHECK(tag->batch.value == batch.value);
            ++tagged_count;
        }
    }
    CHECK(tagged_count == slot_count);
    unload_obek(res);
}

TEST_CASE("GEO-7: instantiate_obek_batch APPLIES per-slot transforms to root entities", "[obek][batch][geo7]")
{
    World source;
    setup_world(source);
    EntityId src_e = source.spawn();
    source.add_component<Transform>(src_e, Transform{});

    auto  bytes = build_obek(source);
    auto* res   = load_obek(bytes);
    REQUIRE(res != nullptr);

    World target;
    setup_world(target);

    // one asset -> N PLACED instances: the reserved transforms path delivered (D-007 row 72)
    Transform slots[3];
    for (crd::u32 i = 0; i < 3U; ++i)
    {
        slots[i].translation = crd::math::from_raw_vec<crd::units::dim::Length>(
            crd::math::Vec3f{static_cast<crd::f32>(i + 1U) * 10.0F, 0.0F, 0.0F});
    }
    auto batch = target.instantiate_obek_batch(*res, 3U, EntityId::null(), {},
                                               crd::containers::ConstSpan<Transform>(slots, 3U));
    CHECK(batch.value != 0U);
    REQUIRE(target.entity_count() == 3U);

    // every root landed at its slot's translation AND is queued for propagation (dirty-marked)
    bool saw[3] = {false, false, false};
    for (EntityId e : target)
    {
        const auto* t = target.get_component<Transform>(e);
        REQUIRE(t != nullptr);
        const crd::f32 x = crd::math::to_raw_vec(t->translation).x;
        for (crd::u32 i = 0; i < 3U; ++i)
        {
            if (x == static_cast<crd::f32>(i + 1U) * 10.0F) { saw[i] = true; }
        }
        CHECK(target.has_component<crd::scene::TransformDirtyFlag>(e));
    }
    CHECK(saw[0]);
    CHECK(saw[1]);
    CHECK(saw[2]);

    // fewer transforms than slots: the tail spawns unplaced (source-authored transform = identity)
    World target2;
    setup_world(target2);
    (void)target2.instantiate_obek_batch(*res, 3U, EntityId::null(), {},
                                         crd::containers::ConstSpan<Transform>(slots, 1U));
    crd::u32 at_ten  = 0;
    crd::u32 at_zero = 0;
    for (EntityId e : target2)
    {
        const crd::f32 x = crd::math::to_raw_vec(target2.get_component<Transform>(e)->translation).x;
        if (x == 10.0F) { ++at_ten; }
        if (x == 0.0F) { ++at_zero; }
    }
    CHECK(at_ten == 1U);
    CHECK(at_zero == 2U);

    unload_obek(res);
}

TEST_CASE("instantiate_obek_batch returns unique handles per call", "[obek][batch][handle]")
{
    World source;
    setup_world(source);
    EntityId src_e = source.spawn();
    source.add_component<Transform>(src_e, Transform{});

    auto bytes = build_obek(source);
    auto* res = load_obek(bytes);
    REQUIRE(res != nullptr);

    World target;
    setup_world(target);

    auto b1 = target.instantiate_obek_batch(*res, 1U);
    auto b2 = target.instantiate_obek_batch(*res, 1U);
    auto b3 = target.instantiate_obek_batch(*res, 1U);
    CHECK(b1.value != b2.value);
    CHECK(b2.value != b3.value);
    CHECK(b1.value != b3.value);
    unload_obek(res);
}

// -----------------------------------------------------------------------------
// revert / unpack / enumerate APIs (v1m5a)
// -----------------------------------------------------------------------------

TEST_CASE("revert_component restores Transform after runtime mutation", "[obek][revert]")
{
    World source;
    setup_world(source);
    EntityId src_e = source.spawn();
    source.add_component<Transform>(src_e, Transform{});
    source.set_translation(src_e, Vec3f{5.0F, 0.0F, 0.0F});

    auto bytes = build_obek(source);
    auto* res = load_obek(bytes);
    REQUIRE(res != nullptr);

    World target;
    setup_world(target);
    auto inst = target.instantiate_obek(*res);
    REQUIRE(inst.entities.size() == 1U);
    REQUIRE(inst.source == res);

    // Mutate at runtime.
    target.set_translation(inst.entities[0], Vec3f{99.0F, 99.0F, 99.0F});
    CHECK(approx(target.get_component<Transform>(inst.entities[0])->translation.x.value, 99.0F));

    // Revert the whole Transform — back to source bytes (5, 0, 0).
    target.revert_component(inst, 0U, crd::scene::kFourCC_Transform);
    CHECK(approx(target.get_component<Transform>(inst.entities[0])->translation.x.value, 5.0F));
    unload_obek(res);
}

TEST_CASE("revert_field restores only the targeted byte range", "[obek][revert]")
{
    World source;
    setup_world(source);
    EntityId src_e = source.spawn();
    source.add_component<Transform>(src_e, Transform{});
    source.set_translation(src_e, Vec3f{1.0F, 2.0F, 3.0F});

    auto bytes = build_obek(source);
    auto* res = load_obek(bytes);
    REQUIRE(res != nullptr);

    World target;
    setup_world(target);
    auto inst = target.instantiate_obek(*res);
    REQUIRE(inst.entities.size() == 1U);

    // Mutate translation entirely.
    target.set_translation(inst.entities[0], Vec3f{99.0F, 88.0F, 77.0F});

    // Revert only the translation field (12 bytes).
    target.revert_field(inst, 0U, crd::scene::kFourCC_Transform,
                        static_cast<crd::u32>(offsetof(Transform, translation)),
                        static_cast<crd::u32>(sizeof(Vec3f)));
    const Transform* t = target.get_component<Transform>(inst.entities[0]);
    CHECK(approx(t->translation.x.value, 1.0F));
    CHECK(approx(t->translation.y.value, 2.0F));
    CHECK(approx(t->translation.z.value, 3.0F));
    unload_obek(res);
}

TEST_CASE("revert_all restores every entity's components", "[obek][revert]")
{
    World source;
    setup_world(source);
    EntityId e0 = source.spawn();
    EntityId e1 = source.spawn();
    source.add_component<Transform>(e0, Transform{});
    source.add_component<Transform>(e1, Transform{});
    source.set_translation(e0, Vec3f{10.0F, 0.0F, 0.0F});
    source.set_translation(e1, Vec3f{20.0F, 0.0F, 0.0F});

    auto bytes = build_obek(source);
    auto* res = load_obek(bytes);
    REQUIRE(res != nullptr);

    World target;
    setup_world(target);
    auto inst = target.instantiate_obek(*res);
    REQUIRE(inst.entities.size() == 2U);

    target.set_translation(inst.entities[0], Vec3f{99.0F, 0.0F, 0.0F});
    target.set_translation(inst.entities[1], Vec3f{99.0F, 0.0F, 0.0F});

    target.revert_all(inst);
    CHECK(approx(target.get_component<Transform>(inst.entities[0])->translation.x.value, 10.0F));
    CHECK(approx(target.get_component<Transform>(inst.entities[1])->translation.x.value, 20.0F));
    unload_obek(res);
}

TEST_CASE("unpack_obek reverts and severs the source link", "[obek][revert][unpack]")
{
    World source;
    setup_world(source);
    EntityId src_e = source.spawn();
    source.add_component<Transform>(src_e, Transform{});
    source.set_translation(src_e, Vec3f{42.0F, 0.0F, 0.0F});

    auto bytes = build_obek(source);
    auto* res = load_obek(bytes);
    REQUIRE(res != nullptr);

    World target;
    setup_world(target);
    auto inst = target.instantiate_obek(*res);
    REQUIRE(inst.source == res);

    target.set_translation(inst.entities[0], Vec3f{99.0F, 0.0F, 0.0F});
    target.unpack_obek(inst);

    // Source link severed.
    CHECK(inst.source == nullptr);
    // Reverted to source value (99 → 42).
    CHECK(approx(target.get_component<Transform>(inst.entities[0])->translation.x.value, 42.0F));
    unload_obek(res);
}

TEST_CASE("unpack_obek_keep_overrides preserves current state", "[obek][revert][unpack]")
{
    World source;
    setup_world(source);
    EntityId src_e = source.spawn();
    source.add_component<Transform>(src_e, Transform{});
    source.set_translation(src_e, Vec3f{42.0F, 0.0F, 0.0F});

    auto bytes = build_obek(source);
    auto* res = load_obek(bytes);
    REQUIRE(res != nullptr);

    World target;
    setup_world(target);
    auto inst = target.instantiate_obek(*res);

    target.set_translation(inst.entities[0], Vec3f{99.0F, 0.0F, 0.0F});
    target.unpack_obek_keep_overrides(inst);

    CHECK(inst.source == nullptr);
    // Mutated value preserved (kept overrides).
    CHECK(approx(target.get_component<Transform>(inst.entities[0])->translation.x.value, 99.0F));
    unload_obek(res);
}

TEST_CASE("enumerate_overrides exposes cook-time records", "[obek][revert][enumerate]")
{
    World source;
    setup_world(source);
    EntityId e = source.spawn();
    source.add_component<Transform>(e, Transform{});

    crd::scene::ObekArtifactBuilder builder{
        crd::memory::default_allocator(),
        crd::resources::ResourceId{0xCAFE'BABEULL, 0xDEAD'BEEFULL},
        /*obek_root_id=*/0xABCDU};
    Vec3f override_value{42.0F, 0.0F, 0.0F};
    Transform override_transform{};
    override_transform.translation =
        crd::math::from_raw_vec<crd::units::dim::Length>(override_value);
    builder.add_override(/*file_idx=*/0U, crd::scene::kFourCC_Transform, /*field_offset=*/0U,
                         crd::containers::ConstSpan<crd::u8>{
                             reinterpret_cast<const crd::u8*>(&override_transform),
                             sizeof(Transform)});
    auto bytes = builder.build(source);

    auto* res = load_obek(bytes);
    REQUIRE(res != nullptr);
    REQUIRE(res->cook_override_records.size() == 1U);

    World target;
    setup_world(target);
    auto inst = target.instantiate_obek(*res);

    auto recs = target.enumerate_overrides(inst);
    CHECK(recs.size() == 1U);
    CHECK(recs[0].file_idx == 0U);
    CHECK(recs[0].component_fourcc == crd::scene::kFourCC_Transform);

    // After unpack, enumerate returns empty.
    target.unpack_obek_keep_overrides(inst);
    CHECK(target.enumerate_overrides(inst).size() == 0U);
    unload_obek(res);
}

// -----------------------------------------------------------------------------
// OCHN — chain dependency tracking (v1m2 phase B)
// -----------------------------------------------------------------------------

TEST_CASE("OCHN chunk round-trips chain dependencies", "[obek][ochn][chain]")
{
    World source;
    setup_world(source);
    EntityId e = source.spawn();
    source.add_component<Transform>(e, Transform{});

    crd::scene::ObekArtifactBuilder builder{
        crd::memory::default_allocator(),
        crd::resources::ResourceId{0x1111ULL, 0x2222ULL},
        /*obek_root_id=*/0xABCDU};
    builder.add_chain_dependency(crd::containers::StringView{"obek/base.obek.toml"},
                                 0x1234'5678'9ABC'DEF0ULL,
                                 crd::scene::ObekChainKind::Extends);
    builder.add_chain_dependency(crd::containers::StringView{"obek/wheel.obek.toml"},
                                 0xCAFE'BABE'DEAD'BEEFULL,
                                 crd::scene::ObekChainKind::Nested);
    auto bytes = builder.build(source);

    auto* res = load_obek(bytes);
    REQUIRE(res != nullptr);
    REQUIRE(res->chain_dependencies.size() == 2U);

    CHECK(res->chain_dependencies[0].kind == static_cast<crd::u8>(crd::scene::ObekChainKind::Extends));
    CHECK(res->chain_dependencies[0].content_hash == 0x1234'5678'9ABC'DEF0ULL);
    CHECK(res->chain_dependencies[1].kind == static_cast<crd::u8>(crd::scene::ObekChainKind::Nested));
    CHECK(res->chain_dependencies[1].content_hash == 0xCAFE'BABE'DEAD'BEEFULL);

    unload_obek(res);
}

TEST_CASE("OCHN absent when no chain dependencies recorded", "[obek][ochn][empty]")
{
    World source;
    setup_world(source);
    auto bytes = build_obek(source);
    auto* res = load_obek(bytes);
    REQUIRE(res != nullptr);
    CHECK(res->chain_dependencies.size() == 0U);
    unload_obek(res);
}
