// Phase 3.0 v1l — scene_cooker tests (ADR-0055).
//
// Coverage:
//   - Empty TOML cooks to a valid SCEN.
//   - Single-entity Transform round-trip via SceneLoader + instantiate_scene.
//   - Hierarchy with ChildOf relation round-trips.
//   - All 6 built-in relations cook + load + instantiate cleanly.
//   - Error paths: unknown component, missing relation target, type mismatch,
//     duplicate entity name, hierarchical name reference, acyclic relation
//     with array of targets.
//   - Non-acyclic relation (Targets) accepts an array of targets.
//   - Empty Transform table = default-constructed Transform.
//   - Determinism: same TOML cooked twice → bit-exact bytes.
//   - 100-entity stress.

#include <crd/cooker/scene_cooker.hpp>
#include <crd/math/vec.hpp>
#include <crd/resources/loader.hpp>
#include <crd/resources/resource_id.hpp>
#include <crd/scene/relation.hpp>
#include <crd/scene/scene_resource.hpp>
#include <crd/scene/serialize.hpp>
#include <crd/scene/system.hpp>
#include <crd/scene/transform.hpp>
#include <crd/scene/transform_propagation.hpp>
#include <crd/scene/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>

using crd::cooker::CookError;
using crd::cooker::SceneCookContext;
using crd::cooker::scene_cooker_inline;
using crd::math::Vec3f;
using crd::scene::EntityId;
using crd::scene::SceneLoader;
using crd::scene::SceneResource;
using crd::scene::Transform;
using crd::scene::TransformPropagation;
using crd::scene::World;
using crd::scene::relations::ChildOf;

namespace
{
constexpr crd::resources::ResourceId kTestId{0xCAFE'BABE'1234'5678ULL, 0xDEAD'BEEF'F00D'F00DULL};

[[nodiscard]] SceneResource* load_scene_bytes(const crd::containers::Array<crd::u8>& bytes)
{
    if (bytes.size() == 0)
    {
        return nullptr;
    }
    SceneLoader loader;
    crd::resources::LoadContext ctx{};
    ctx.id        = kTestId;
    ctx.bytes     = crd::containers::ConstSpan<crd::u8>{bytes.data(), bytes.size()};
    ctx.allocator = crd::memory::default_allocator();
    return static_cast<SceneResource*>(loader.load(ctx));
}

void unload_scene(SceneResource* res)
{
    if (res == nullptr)
    {
        return;
    }
    SceneLoader loader;
    loader.unload(res);
}

void setup_target_world(World& w)
{
    w.register_component<Transform>(crd::scene::transform_serialize_trait());
    w.register_component<crd::scene::TransformDirtyFlag>(crd::scene::StorageHint::SparseSet);
    w.register_builtin_relations();
    w.register_system(std::make_unique<TransformPropagation>());
}

[[nodiscard]] bool approx(crd::f32 a, crd::f32 b, crd::f32 tol = 1e-4F)
{
    return (a - b) < tol && (b - a) < tol;
}

[[nodiscard]] SceneCookContext make_ctx()
{
    SceneCookContext c{};
    c.id        = kTestId;
    c.allocator = crd::memory::default_allocator();
    return c;
}
} // namespace

// ---------------------------------------------------------------------------
// Happy path
// ---------------------------------------------------------------------------

TEST_CASE("Empty TOML cooks to valid empty SCEN", "[scene-cooker][empty]")
{
    auto bytes = scene_cooker_inline("", make_ctx());
    REQUIRE(bytes.size() > 0);

    auto* res = load_scene_bytes(bytes);
    REQUIRE(res != nullptr);
    CHECK(res->info.entity_count == 0U);
    unload_scene(res);
}

TEST_CASE("Single-entity Transform round-trip via cooker + loader",
          "[scene-cooker][round-trip]")
{
    constexpr const char* k_toml_text = R"(
[entity.player]
Transform = { translation = [1.5, 2.5, 3.5] }
)";

    crd::containers::Array<CookError> errors{crd::memory::default_allocator()};
    auto bytes = scene_cooker_inline(k_toml_text, make_ctx(), &errors);
    REQUIRE(errors.size() == 0);
    REQUIRE(bytes.size() > 0);

    auto* res = load_scene_bytes(bytes);
    REQUIRE(res != nullptr);
    CHECK(res->info.entity_count == 1U);

    World target;
    setup_target_world(target);
    auto inst = target.instantiate_scene(*res);
    REQUIRE(inst.entities.size() == 1U);
    const Transform* t = target.get_component<Transform>(inst.entities[0]);
    REQUIRE(t != nullptr);
    CHECK(approx(t->translation.x.value, 1.5F));
    CHECK(approx(t->translation.y.value, 2.5F));
    CHECK(approx(t->translation.z.value, 3.5F));

    unload_scene(res);
}

TEST_CASE("Hierarchy with ChildOf relation round-trips",
          "[scene-cooker][round-trip][relations]")
{
    constexpr const char* k_toml_text = R"(
[entity.parent]
Transform = { translation = [10.0, 0.0, 0.0] }

[entity.child]
Transform = { translation = [1.0, 0.0, 0.0] }
ChildOf = "parent"
)";

    crd::containers::Array<CookError> errors{crd::memory::default_allocator()};
    auto bytes = scene_cooker_inline(k_toml_text, make_ctx(), &errors);
    REQUIRE(errors.size() == 0);

    auto* res = load_scene_bytes(bytes);
    REQUIRE(res != nullptr);
    CHECK(res->info.entity_count == 2U);
    CHECK(res->info.relation_count == 1U);

    World target;
    setup_target_world(target);
    auto inst = target.instantiate_scene(*res);
    REQUIRE(inst.entities.size() == 2U);

    crd::u32 children_count = 0;
    for (EntityId e : inst.entities)
    {
        if (target.has_relation<ChildOf>(e))
        {
            ++children_count;
        }
    }
    CHECK(children_count == 1U);

    unload_scene(res);
}

TEST_CASE("All six built-in relations round-trip via cooker",
          "[scene-cooker][round-trip][relations]")
{
    constexpr const char* k_toml_text = R"(
[entity.a] # parent / socket / owner / target / dependency / controller
Transform = {}

[entity.b]
Transform = {}
ChildOf = "a"
AttachedTo = "a"
Owns = "a"
Targets = "a"
DependsOn = "a"
PossessedBy = "a"
)";

    crd::containers::Array<CookError> errors{crd::memory::default_allocator()};
    auto bytes = scene_cooker_inline(k_toml_text, make_ctx(), &errors);
    REQUIRE(errors.size() == 0);
    REQUIRE(bytes.size() > 0);

    auto* res = load_scene_bytes(bytes);
    REQUIRE(res != nullptr);
    // Six relations on entity 'b'.
    CHECK(res->info.relation_count == 6U);

    unload_scene(res);
}

TEST_CASE("Non-acyclic relation accepts array of targets (Targets)",
          "[scene-cooker][round-trip][relations][array]")
{
    constexpr const char* k_toml_text = R"(
[entity.tracked_a]
Transform = {}

[entity.tracked_b]
Transform = {}

[entity.tracker]
Transform = {}
Targets = ["tracked_a", "tracked_b"]
)";

    crd::containers::Array<CookError> errors{crd::memory::default_allocator()};
    auto bytes = scene_cooker_inline(k_toml_text, make_ctx(), &errors);
    REQUIRE(errors.size() == 0);

    auto* res = load_scene_bytes(bytes);
    REQUIRE(res != nullptr);
    // ONE relation in the SCEN: each Tag is a single component on the
    // source entity. The cooker iterates the array and calls
    // add_relation_via_id for each — the runtime model UPSERTs, so the
    // last entry wins. Final state = one Targets relation pointing at
    // the last array element.
    CHECK(res->info.relation_count == 1U);

    unload_scene(res);
}

TEST_CASE("Empty Transform table = default-constructed",
          "[scene-cooker][defaults]")
{
    constexpr const char* k_toml_text = R"(
[entity.thing]
Transform = {}
)";

    crd::containers::Array<CookError> errors{crd::memory::default_allocator()};
    auto bytes = scene_cooker_inline(k_toml_text, make_ctx(), &errors);
    REQUIRE(errors.size() == 0);

    auto* res = load_scene_bytes(bytes);
    REQUIRE(res != nullptr);

    World target;
    setup_target_world(target);
    auto inst = target.instantiate_scene(*res);
    const Transform* t = target.get_component<Transform>(inst.entities[0]);
    REQUIRE(t != nullptr);
    CHECK(approx(t->translation.x.value, 0.0F));
    CHECK(approx(t->scale.x, 1.0F));

    unload_scene(res);
}

// ---------------------------------------------------------------------------
// Determinism
// ---------------------------------------------------------------------------

TEST_CASE("Determinism: same TOML cooks to bit-exact bytes",
          "[scene-cooker][determinism]")
{
    constexpr const char* k_toml_text = R"(
[entity.p]
Transform = { translation = [1.0, 2.0, 3.0] }

[entity.c]
Transform = { translation = [4.0, 5.0, 6.0] }
ChildOf = "p"
)";

    auto a = scene_cooker_inline(k_toml_text, make_ctx());
    auto b = scene_cooker_inline(k_toml_text, make_ctx());
    REQUIRE(a.size() > 0);
    REQUIRE(a.size() == b.size());
    CHECK(std::memcmp(a.data(), b.data(), a.size()) == 0);
}

// ---------------------------------------------------------------------------
// Error paths
// ---------------------------------------------------------------------------

TEST_CASE("Unknown component name -> cooker fails with diagnostic",
          "[scene-cooker][error]")
{
    constexpr const char* k_toml_text = R"(
[entity.x]
Transform = {}
WeirdUnknownComponent = {}
)";

    crd::containers::Array<CookError> errors{crd::memory::default_allocator()};
    auto bytes = scene_cooker_inline(k_toml_text, make_ctx(), &errors);
    CHECK(bytes.size() == 0);
    CHECK(errors.size() > 0);
}

TEST_CASE("Missing relation target -> cooker fails", "[scene-cooker][error]")
{
    constexpr const char* k_toml_text = R"(
[entity.child]
Transform = {}
ChildOf = "no_such_parent"
)";

    crd::containers::Array<CookError> errors{crd::memory::default_allocator()};
    auto bytes = scene_cooker_inline(k_toml_text, make_ctx(), &errors);
    CHECK(bytes.size() == 0);
    CHECK(errors.size() > 0);
}

TEST_CASE("Type mismatch (Transform.translation as string) -> cooker fails",
          "[scene-cooker][error]")
{
    constexpr const char* k_toml_text = R"(
[entity.x]
Transform = { translation = "not_an_array" }
)";

    crd::containers::Array<CookError> errors{crd::memory::default_allocator()};
    auto bytes = scene_cooker_inline(k_toml_text, make_ctx(), &errors);
    CHECK(bytes.size() == 0);
    CHECK(errors.size() > 0);
}

TEST_CASE("Hierarchical entity name (with dot) -> cooker fails",
          "[scene-cooker][error]")
{
    constexpr const char* k_toml_text = R"(
[entity."player.right_hand"]
Transform = {}
)";

    crd::containers::Array<CookError> errors{crd::memory::default_allocator()};
    auto bytes = scene_cooker_inline(k_toml_text, make_ctx(), &errors);
    CHECK(bytes.size() == 0);
    CHECK(errors.size() > 0);
}

TEST_CASE("Acyclic relation (ChildOf) with array of targets -> cooker fails",
          "[scene-cooker][error][relations]")
{
    constexpr const char* k_toml_text = R"(
[entity.a]
Transform = {}

[entity.b]
Transform = {}

[entity.c]
Transform = {}
ChildOf = ["a", "b"]
)";

    crd::containers::Array<CookError> errors{crd::memory::default_allocator()};
    auto bytes = scene_cooker_inline(k_toml_text, make_ctx(), &errors);
    CHECK(bytes.size() == 0);
    CHECK(errors.size() > 0);
}

TEST_CASE("Multiple errors accumulate in one pass",
          "[scene-cooker][error][accumulate]")
{
    // Three errors: unknown component, missing target, hierarchical name.
    constexpr const char* k_toml_text = R"(
[entity.a]
WeirdComponent = {}

[entity.b]
ChildOf = "no_such_parent"

[entity."has.dot"]
Transform = {}
)";

    crd::containers::Array<CookError> errors{crd::memory::default_allocator()};
    auto bytes = scene_cooker_inline(k_toml_text, make_ctx(), &errors);
    CHECK(bytes.size() == 0);
    CHECK(errors.size() >= 3U);
}

TEST_CASE("SceneArtifactBuilder + instantiate_scene + step (no cooker)",
          "[scene-cooker][control][builder]")
{
    // Source: build directly, run propagation, serialise.
    World source;
    setup_target_world(source);
    EntityId sp = source.spawn();
    EntityId sc = source.spawn();
    source.add_component<Transform>(sp, Transform{});
    source.add_component<Transform>(sc, Transform{});
    source.set_translation(sp, Vec3f{10, 0, 0});
    source.set_translation(sc, Vec3f{1, 0, 0});
    source.add_relation<ChildOf>(sc, sp);
    source.step(1.0 / 60.0);
    REQUIRE(approx(source.get_component<Transform>(sc)->world.c3.x, 11.0F));

    // Serialise.
    crd::scene::SceneArtifactBuilder builder{crd::memory::default_allocator(), kTestId};
    auto bytes = builder.build(source);

    // Load + instantiate into target.
    auto* res = load_scene_bytes(bytes);
    REQUIRE(res != nullptr);
    World target;
    setup_target_world(target);
    auto inst = target.instantiate_scene(*res);
    for (EntityId e : inst.entities)
    {
        if (target.has_component<Transform>(e))
        {
            target.mark_transform_subtree_dirty(e);
        }
    }
    target.step(1.0 / 60.0);

    EntityId child_loaded = EntityId::null();
    for (EntityId e : inst.entities)
    {
        if (target.has_relation<ChildOf>(e))
        {
            child_loaded = e;
            break;
        }
    }
    REQUIRE(!child_loaded.is_null());
    const Transform* ct = target.get_component<Transform>(child_loaded);
    REQUIRE(ct != nullptr);
    INFO("builder-control: child.world.c3.x = " << ct->world.c3.x);
    CHECK(approx(ct->world.c3.x, 11.0F));
    unload_scene(res);
}

TEST_CASE("Direct-built hierarchy + step propagation works (control test)",
          "[scene-cooker][control]")
{
    World w;
    setup_target_world(w);
    EntityId parent = w.spawn();
    EntityId child  = w.spawn();
    w.add_component<Transform>(parent, Transform{});
    w.add_component<Transform>(child, Transform{});
    w.set_translation(parent, Vec3f{10.0F, 0.0F, 0.0F});
    w.set_translation(child, Vec3f{1.0F, 0.0F, 0.0F});
    w.add_relation<ChildOf>(child, parent);

    w.step(1.0 / 60.0);

    const Transform* ct = w.get_component<Transform>(child);
    REQUIRE(ct != nullptr);
    INFO("control: parent.world.c3.x = " << w.get_component<Transform>(parent)->world.c3.x);
    INFO("control: child.world.c3.x = " << ct->world.c3.x);
    CHECK(approx(ct->world.c3.x, 11.0F));
}

// ---------------------------------------------------------------------------
// Round-trip + propagation
// ---------------------------------------------------------------------------

TEST_CASE("Cooked hierarchy + step propagation: world matrices correct",
          "[scene-cooker][round-trip][propagation]")
{
    constexpr const char* k_toml_text = R"(
[entity.parent]
Transform = { translation = [10.0, 0.0, 0.0] }

[entity.child]
Transform = { translation = [1.0, 0.0, 0.0] }
ChildOf = "parent"
)";

    crd::containers::Array<CookError> errors{crd::memory::default_allocator()};
    auto bytes = scene_cooker_inline(k_toml_text, make_ctx(), &errors);
    REQUIRE(errors.size() == 0);

    auto* res = load_scene_bytes(bytes);
    REQUIRE(res != nullptr);

    World target;
    setup_target_world(target);
    auto inst = target.instantiate_scene(*res);
    // The cooker bakes world matrices into the SCEN; consumers can read
    // transform.world directly post-instantiate without needing a step().

    EntityId child_loaded = EntityId::null();
    for (EntityId e : inst.entities)
    {
        if (target.has_relation<ChildOf>(e))
        {
            child_loaded = e;
            break;
        }
    }
    REQUIRE(!child_loaded.is_null());
    const Transform* ct = target.get_component<Transform>(child_loaded);
    REQUIRE(ct != nullptr);
    CHECK(approx(ct->world.c3.x, 11.0F));

    unload_scene(res);
}

// ---------------------------------------------------------------------------
// Stress
// ---------------------------------------------------------------------------

TEST_CASE("100-entity TOML cooks + round-trips correctly",
          "[scene-cooker][stress]")
{
    std::string toml_text;
    for (int i = 0; i < 100; ++i)
    {
        toml_text += "[entity.e";
        toml_text += std::to_string(i);
        toml_text += "]\nTransform = { translation = [";
        toml_text += std::to_string(i);
        toml_text += ".0, 0.0, 0.0] }\n";
    }

    crd::containers::Array<CookError> errors{crd::memory::default_allocator()};
    auto bytes = scene_cooker_inline(crd::containers::StringView{toml_text.c_str(), toml_text.size()},
                                     make_ctx(), &errors);
    REQUIRE(errors.size() == 0);

    auto* res = load_scene_bytes(bytes);
    REQUIRE(res != nullptr);
    CHECK(res->info.entity_count == 100U);

    unload_scene(res);
}
