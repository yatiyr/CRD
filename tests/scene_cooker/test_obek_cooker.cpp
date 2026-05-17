// Phase 3.0 v1m3a â€” obek_cooker tests (ADR-0058).
//
// v1m3a coverage (substrate; v1m3b/c/d extend):
//   - Empty .obek.toml cooks to a valid OBEK with zero entities.
//   - Single-entity Ã¶bek with Transform round-trips via ObekLoader +
//     instantiate_obek.
//   - Two-entity Ã¶bek with ChildOf relation round-trips and reparents
//     under the supplied parent at instantiate time.
//   - `extends` / per-entity `obek` / `overrides` are explicitly rejected
//     at v1m3a with the "reserved for v1m3X" error message.

#include <crd/cooker/obek_cooker.hpp>
#include <crd/math/vec.hpp>
#include <crd/resources/loader.hpp>
#include <crd/resources/resource_id.hpp>
#include <crd/scene/obek.hpp>
#include <crd/scene/relation.hpp>
#include <crd/scene/serialize.hpp>
#include <crd/scene/system.hpp>
#include <crd/scene/transform.hpp>
#include <crd/scene/transform_propagation.hpp>
#include <crd/scene/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

using crd::cooker::CookError;
using crd::cooker::obek_cooker_inline;
using crd::cooker::ObekCookContext;
using crd::math::Vec3f;
using crd::scene::EntityId;
using crd::scene::ObekLoader;
using crd::scene::ObekResource;
using crd::scene::Transform;
using crd::scene::TransformPropagation;
using crd::scene::World;
using crd::scene::relations::ChildOf;

namespace
{
constexpr crd::resources::ResourceId kTestId{0xCAFE'BABE'1234'5678ULL, 0xDEAD'BEEF'F00D'F00DULL};
constexpr crd::u64                   kTestRootId = 0xABCD'1234'5678'9000ULL;

[[nodiscard]] ObekResource* load_obek_bytes(const crd::containers::Array<crd::u8>& bytes)
{
    if (bytes.size() == 0)
    {
        return nullptr;
    }
    ObekLoader loader;
    crd::resources::LoadContext ctx{};
    ctx.id        = kTestId;
    ctx.bytes     = crd::containers::ConstSpan<crd::u8>{bytes.data(), bytes.size()};
    ctx.allocator = crd::memory::default_allocator();
    return static_cast<ObekResource*>(loader.load(ctx));
}

void unload_obek(ObekResource* res)
{
    if (res == nullptr)
    {
        return;
    }
    ObekLoader loader;
    loader.unload(res);
}

void setup_target_world(World& w)
{
    w.register_component<Transform>(crd::scene::transform_serialize_trait());
    w.register_component<crd::scene::TransformDirtyFlag>(crd::scene::StorageHint::SparseSet);
    w.register_builtin_relations();
    w.register_system(std::make_unique<TransformPropagation>());
}

[[nodiscard]] bool approx(crd::f32 a, crd::f32 b, crd::f32 tol = 1e-5F)
{
    const crd::f32 d = a - b;
    return (d < tol) && (-d < tol);
}

ObekCookContext make_ctx()
{
    ObekCookContext ctx{};
    ctx.id           = kTestId;
    ctx.allocator    = crd::memory::default_allocator();
    ctx.obek_root_id = kTestRootId;
    return ctx;
}

} // namespace

// -----------------------------------------------------------------------------
// Empty
// -----------------------------------------------------------------------------

TEST_CASE("Empty obek TOML cooks to a valid OBEK with zero entities", "[obek-cooker][empty]")
{
    constexpr const char* kTomlText = "";
    crd::containers::Array<CookError> errors{crd::memory::default_allocator()};
    // Empty-string StringView is the intent — the test exercises the
    // zero-entity TOML path.
    // NOLINTNEXTLINE(bugprone-string-constructor)
    auto bytes = obek_cooker_inline(crd::containers::StringView{kTomlText, 0}, make_ctx(), &errors);
    REQUIRE(errors.size() == 0);
    REQUIRE(bytes.size() > 0);

    auto* res = load_obek_bytes(bytes);
    REQUIRE(res != nullptr);
    CHECK(res->info.entity_count == 0U);
    CHECK(res->info.obek_root_id == kTestRootId);
    unload_obek(res);
}

// -----------------------------------------------------------------------------
// Single entity round-trip
// -----------------------------------------------------------------------------

TEST_CASE("Single-entity obek TOML round-trips Transform", "[obek-cooker][round-trip][single]")
{
    constexpr const char* kTomlText = R"TOML(
[entity.player]
Transform = { translation = [10.0, 20.0, 30.0] }
)TOML";

    crd::containers::Array<CookError> errors{crd::memory::default_allocator()};
    const auto text_view = crd::containers::StringView{kTomlText, std::strlen(kTomlText)};
    auto bytes = obek_cooker_inline(text_view, make_ctx(), &errors);
    REQUIRE(errors.size() == 0);
    REQUIRE(bytes.size() > 0);

    auto* res = load_obek_bytes(bytes);
    REQUIRE(res != nullptr);
    CHECK(res->info.entity_count == 1U);

    World target;
    setup_target_world(target);
    auto inst = target.instantiate_obek(*res);
    REQUIRE(inst.entities.size() == 1U);
    const Transform* t = target.get_component<Transform>(inst.entities[0]);
    REQUIRE(t != nullptr);
    CHECK(approx(t->translation.x.value, 10.0F));
    CHECK(approx(t->translation.y.value, 20.0F));
    CHECK(approx(t->translation.z.value, 30.0F));

    unload_obek(res);
}

// -----------------------------------------------------------------------------
// Hierarchy + reparent
// -----------------------------------------------------------------------------

TEST_CASE("ChildOf hierarchy round-trips and reparents under instantiate parent",
          "[obek-cooker][round-trip][hierarchy]")
{
    constexpr const char* kTomlText = R"TOML(
[entity.root]
Transform = { translation = [0.0, 0.0, 0.0] }

[entity.hand]
Transform = { translation = [1.0, 0.0, 0.0] }
ChildOf   = "root"
)TOML";

    crd::containers::Array<CookError> errors{crd::memory::default_allocator()};
    const auto text_view = crd::containers::StringView{kTomlText, std::strlen(kTomlText)};
    auto bytes = obek_cooker_inline(text_view, make_ctx(), &errors);
    REQUIRE(errors.size() == 0);
    REQUIRE(bytes.size() > 0);

    auto* res = load_obek_bytes(bytes);
    REQUIRE(res != nullptr);
    CHECK(res->info.entity_count == 2U);
    CHECK(res->info.relation_count == 1U);

    World target;
    setup_target_world(target);
    EntityId anchor = target.spawn();
    target.add_component<Transform>(anchor, Transform{});

    auto inst = target.instantiate_obek(*res, anchor);
    REQUIRE(inst.entities.size() == 2U);
    // The Ã¶bek's "root" entity has no ChildOf in source â†’ gets ChildOf(anchor).
    // The Ã¶bek's "hand" entity already has ChildOf("root") â†’ preserved.
    // We don't know which file_idx is which name (depends on TOML iteration
    // order in toml++'s table â€” typically insertion order). Inspect both.
    EntityId e0 = inst.entities[0];
    EntityId e1 = inst.entities[1];
    EntityId t0 = target.get_relation_target<ChildOf>(e0);
    EntityId t1 = target.get_relation_target<ChildOf>(e1);
    // Exactly one of e0/e1 is parented to anchor, the other to its sibling.
    const bool e0_parents_to_anchor = (t0 == anchor);
    const bool e1_parents_to_anchor = (t1 == anchor);
    CHECK((e0_parents_to_anchor ^ e1_parents_to_anchor));  // exactly one is the Ã¶bek root
    if (e0_parents_to_anchor)
    {
        CHECK(t1 == e0);
    }
    else
    {
        CHECK(t0 == e1);
    }

    unload_obek(res);
}

// -----------------------------------------------------------------------------
// Cook-time overrides (v1m3d)
// -----------------------------------------------------------------------------

TEST_CASE("Cook-time overrides bake into OOVR and apply at instantiate", "[obek-cooker][overrides][bake]")
{
    constexpr const char* kTomlText = R"TOML(
overrides = [
    { entity = "player", component = "Transform", value = { translation = [42.0, 0.0, 0.0] } },
]

[entity.player]
Transform = { translation = [0.0, 0.0, 0.0] }
)TOML";

    crd::containers::Array<CookError> errors{crd::memory::default_allocator()};
    const auto text_view = crd::containers::StringView{kTomlText, std::strlen(kTomlText)};
    auto bytes = obek_cooker_inline(text_view, make_ctx(), &errors);
    REQUIRE(errors.size() == 0);
    REQUIRE(bytes.size() > 0);

    auto* res = load_obek_bytes(bytes);
    REQUIRE(res != nullptr);
    CHECK(res->info.entity_count == 1U);
    REQUIRE(res->cook_override_records.size() == 1U);
    CHECK(res->cook_override_records[0].file_idx == 0U);
    CHECK(res->cook_override_records[0].component_fourcc == crd::scene::kFourCC_Transform);

    World target;
    setup_target_world(target);
    auto inst = target.instantiate_obek(*res);
    REQUIRE(inst.entities.size() == 1U);
    CHECK(inst.overrides_applied == 1U);
    CHECK(inst.overrides_skipped == 0U);
    const Transform* t = target.get_component<Transform>(inst.entities[0]);
    REQUIRE(t != nullptr);
    // Override wins over the entity's authored Transform (translation [0,0,0] â†’ [42,0,0]).
    CHECK(approx(t->translation.x.value, 42.0F));
    unload_obek(res);
}

TEST_CASE("Caller override at instantiate wins over cook-time override", "[obek-cooker][overrides][precedence]")
{
    constexpr const char* kTomlText = R"TOML(
overrides = [
    { entity = "player", component = "Transform", value = { translation = [42.0, 0.0, 0.0] } },
]

[entity.player]
Transform = { translation = [0.0, 0.0, 0.0] }
)TOML";

    crd::containers::Array<CookError> errors{crd::memory::default_allocator()};
    const auto text_view = crd::containers::StringView{kTomlText, std::strlen(kTomlText)};
    auto bytes = obek_cooker_inline(text_view, make_ctx(), &errors);
    REQUIRE(errors.size() == 0);

    auto* res = load_obek_bytes(bytes);
    REQUIRE(res != nullptr);

    World target;
    setup_target_world(target);

    // Caller-supplied override stomps over the cook-time override.
    Vec3f caller_value{77.0F, 0.0F, 0.0F};
    crd::scene::ObekOverride patch{};
    patch.file_idx         = 0U;
    patch.component_fourcc = crd::scene::kFourCC_Transform;
    patch.field_offset     = static_cast<crd::u32>(offsetof(Transform, translation));
    patch.payload          = crd::containers::ConstSpan<crd::u8>{
        reinterpret_cast<const crd::u8*>(&caller_value), sizeof(Vec3f)};

    crd::scene::ObekOverride patches[] = {patch};
    auto inst = target.instantiate_obek(*res, EntityId::null(),
                                        crd::containers::ConstSpan<crd::scene::ObekOverride>{patches, 1});
    REQUIRE(inst.entities.size() == 1U);
    CHECK(inst.overrides_applied == 2U);   // cook-time + caller
    const Transform* t = target.get_component<Transform>(inst.entities[0]);
    REQUIRE(t != nullptr);
    // Caller (deepest) wins: translation = [77, 0, 0].
    CHECK(approx(t->translation.x.value, 77.0F));
    unload_obek(res);
}

TEST_CASE("Override pointing at unknown entity name emits error", "[obek-cooker][overrides][not-found]")
{
    constexpr const char* kTomlText = R"TOML(
overrides = [
    { entity = "ghost", component = "Transform", value = { translation = [0, 0, 0] } },
]

[entity.player]
Transform = { translation = [0.0, 0.0, 0.0] }
)TOML";

    crd::containers::Array<CookError> errors{crd::memory::default_allocator()};
    const auto text_view = crd::containers::StringView{kTomlText, std::strlen(kTomlText)};
    auto bytes = obek_cooker_inline(text_view, make_ctx(), &errors);
    CHECK(bytes.size() == 0);
    REQUIRE(errors.size() >= 1);
    bool found = false;
    for (const auto& err : errors)
    {
        const crd::containers::StringView msg{err.message.c_str(), err.message.size()};
        if (msg.find("entity name not found") != crd::containers::StringView::npos)
        {
            found = true;
            break;
        }
    }
    CHECK(found);
}

// -----------------------------------------------------------------------------
// Per-entity overrides still reserved (v1m5+)
// -----------------------------------------------------------------------------

TEST_CASE("Per-entity overrides key rejected with v1m3d reservation message", "[obek-cooker][reserved]")
{
    constexpr const char* kTomlText = R"TOML(
[entity.player]
overrides = []
)TOML";

    crd::containers::Array<CookError> errors{crd::memory::default_allocator()};
    const auto text_view = crd::containers::StringView{kTomlText, std::strlen(kTomlText)};
    auto bytes = obek_cooker_inline(text_view, make_ctx(), &errors);
    CHECK(bytes.size() == 0);
    REQUIRE(errors.size() >= 1);
    bool found = false;
    for (const auto& err : errors)
    {
        const crd::containers::StringView msg{err.message.c_str(), err.message.size()};
        if (msg.find("v1m3d") != crd::containers::StringView::npos)
        {
            found = true;
            break;
        }
    }
    CHECK(found);
}

// -----------------------------------------------------------------------------
// extends chain (v1m3b)
// -----------------------------------------------------------------------------

namespace
{
// Tiny in-memory file resolver: pairs of (path, text). Tests pass a
// std::vector of these via user_data.
struct InMemoryFile
{
    crd::containers::StringView path;
    crd::containers::StringView text;
};

bool in_memory_resolver(crd::containers::StringView path,
                        crd::memory::IAllocator*    alloc,
                        crd::containers::String&    out_text,
                        void*                       user_data)
{
    auto* files = static_cast<const crd::containers::Array<InMemoryFile>*>(user_data);
    for (const InMemoryFile& f : *files)
    {
        if (f.path == path)
        {
            out_text = crd::containers::String{f.text, alloc};
            return true;
        }
    }
    return false;
}

ObekCookContext make_ctx_with_resolver(const crd::containers::Array<InMemoryFile>& files)
{
    ObekCookContext ctx{};
    ctx.id               = kTestId;
    ctx.allocator        = crd::memory::default_allocator();
    ctx.obek_root_id     = kTestRootId;
    ctx.file_resolver    = &in_memory_resolver;
    // const_cast: file_resolver_ud is type-erased void*; the in-memory
    // resolver only reads the array, but the engine API takes non-const ud.
    ctx.file_resolver_ud = const_cast<crd::containers::Array<InMemoryFile>*>(&files); // NOLINT(cppcoreguidelines-pro-type-const-cast)
    return ctx;
}
} // namespace

TEST_CASE("Single extends merges parent components into child", "[obek-cooker][extends]")
{
    constexpr const char* kBaseTomlText = R"TOML(
[entity.player]
Transform = { translation = [1.0, 2.0, 3.0] }
)TOML";

    constexpr const char* kChildTomlText = R"TOML(
extends = "obek/base.obek.toml"

[entity.player]
Transform = { translation = [10.0, 20.0, 30.0] }
)TOML";

    crd::containers::Array<InMemoryFile> files{crd::memory::default_allocator()};
    files.push_back({crd::containers::StringView{"obek/base.obek.toml"},
                     crd::containers::StringView{kBaseTomlText, std::strlen(kBaseTomlText)}});

    crd::containers::Array<CookError> errors{crd::memory::default_allocator()};
    const auto text_view = crd::containers::StringView{kChildTomlText, std::strlen(kChildTomlText)};
    auto bytes = obek_cooker_inline(text_view, make_ctx_with_resolver(files), &errors);
    REQUIRE(errors.size() == 0);
    REQUIRE(bytes.size() > 0);

    auto* res = load_obek_bytes(bytes);
    REQUIRE(res != nullptr);
    CHECK(res->info.entity_count == 1U);
    REQUIRE(res->chain_dependencies.size() == 1U);
    CHECK(res->chain_dependencies[0].kind == static_cast<crd::u8>(crd::scene::ObekChainKind::Extends));

    World target;
    setup_target_world(target);
    auto inst = target.instantiate_obek(*res);
    REQUIRE(inst.entities.size() == 1U);
    const Transform* t = target.get_component<Transform>(inst.entities[0]);
    REQUIRE(t != nullptr);
    // Child's translation wins (deepest = base, child applied last).
    CHECK(approx(t->translation.x.value, 10.0F));
    CHECK(approx(t->translation.y.value, 20.0F));
    CHECK(approx(t->translation.z.value, 30.0F));
    unload_obek(res);
}

TEST_CASE("Chain of 3 extends resolves deepest-first", "[obek-cooker][extends][chain]")
{
    // Grandparent defines translation = [1,1,1]
    // Parent extends grandparent, overrides translation = [2,2,2]
    // Child extends parent, overrides translation = [3,3,3]
    // Expected: child wins â†’ translation = [3,3,3]
    constexpr const char* kGrandparent = R"TOML(
[entity.thing]
Transform = { translation = [1.0, 1.0, 1.0] }
)TOML";
    constexpr const char* kParent = R"TOML(
extends = "obek/grandparent.obek.toml"

[entity.thing]
Transform = { translation = [2.0, 2.0, 2.0] }
)TOML";
    constexpr const char* kChild = R"TOML(
extends = "obek/parent.obek.toml"

[entity.thing]
Transform = { translation = [3.0, 3.0, 3.0] }
)TOML";

    crd::containers::Array<InMemoryFile> files{crd::memory::default_allocator()};
    files.push_back({crd::containers::StringView{"obek/grandparent.obek.toml"},
                     crd::containers::StringView{kGrandparent, std::strlen(kGrandparent)}});
    files.push_back({crd::containers::StringView{"obek/parent.obek.toml"},
                     crd::containers::StringView{kParent, std::strlen(kParent)}});

    crd::containers::Array<CookError> errors{crd::memory::default_allocator()};
    const auto text_view = crd::containers::StringView{kChild, std::strlen(kChild)};
    auto bytes = obek_cooker_inline(text_view, make_ctx_with_resolver(files), &errors);
    REQUIRE(errors.size() == 0);
    REQUIRE(bytes.size() > 0);

    auto* res = load_obek_bytes(bytes);
    REQUIRE(res != nullptr);
    CHECK(res->info.entity_count == 1U);
    // Two ancestors recorded in OCHN (parent + grandparent).
    REQUIRE(res->chain_dependencies.size() == 2U);

    World target;
    setup_target_world(target);
    auto inst = target.instantiate_obek(*res);
    REQUIRE(inst.entities.size() == 1U);
    const Transform* t = target.get_component<Transform>(inst.entities[0]);
    REQUIRE(t != nullptr);
    CHECK(approx(t->translation.x.value, 3.0F));
    CHECK(approx(t->translation.y.value, 3.0F));
    CHECK(approx(t->translation.z.value, 3.0F));
    unload_obek(res);
}

TEST_CASE("extends cycle detection emits error", "[obek-cooker][extends][cycle]")
{
    // a.extends = b
    // b.extends = a   â† cycle
    constexpr const char* kA = R"TOML(
extends = "obek/b.obek.toml"

[entity.thing]
Transform = { translation = [1, 1, 1] }
)TOML";
    constexpr const char* kB = R"TOML(
extends = "obek/a.obek.toml"

[entity.thing]
Transform = { translation = [2, 2, 2] }
)TOML";

    crd::containers::Array<InMemoryFile> files{crd::memory::default_allocator()};
    files.push_back({crd::containers::StringView{"obek/a.obek.toml"},
                     crd::containers::StringView{kA, std::strlen(kA)}});
    files.push_back({crd::containers::StringView{"obek/b.obek.toml"},
                     crd::containers::StringView{kB, std::strlen(kB)}});

    crd::containers::Array<CookError> errors{crd::memory::default_allocator()};
    // Cook starting from `a` (text supplied directly; b reached via resolver,
    // then b's extends back to a triggers cycle detection).
    const auto text_view = crd::containers::StringView{kA, std::strlen(kA)};
    auto bytes = obek_cooker_inline(text_view, make_ctx_with_resolver(files), &errors);
    CHECK(bytes.size() == 0);
    REQUIRE(errors.size() >= 1);
    bool found = false;
    for (const auto& err : errors)
    {
        const crd::containers::StringView msg{err.message.c_str(), err.message.size()};
        if (msg.find("cycle") != crd::containers::StringView::npos)
        {
            found = true;
            break;
        }
    }
    CHECK(found);
}

TEST_CASE("extends without resolver emits error", "[obek-cooker][extends][no-resolver]")
{
    constexpr const char* kTomlText = R"TOML(
extends = "obek/base.obek.toml"

[entity.player]
Transform = { translation = [0, 0, 0] }
)TOML";

    crd::containers::Array<CookError> errors{crd::memory::default_allocator()};
    const auto text_view = crd::containers::StringView{kTomlText, std::strlen(kTomlText)};
    // make_ctx() returns context with file_resolver = nullptr.
    auto bytes = obek_cooker_inline(text_view, make_ctx(), &errors);
    CHECK(bytes.size() == 0);
    REQUIRE(errors.size() >= 1);
    bool found = false;
    for (const auto& err : errors)
    {
        const crd::containers::StringView msg{err.message.c_str(), err.message.size()};
        if (msg.find("file_resolver") != crd::containers::StringView::npos)
        {
            found = true;
            break;
        }
    }
    CHECK(found);
}

// -----------------------------------------------------------------------------
// Nested Ã¶bek references (v1m3c)
// -----------------------------------------------------------------------------

TEST_CASE("Nested obek reference splices entities and parents under placeholder",
          "[obek-cooker][nested]")
{
    constexpr const char* kWheelToml = R"TOML(
[entity.hub]
Transform = { translation = [0.0, 0.0, 0.0] }

[entity.tire]
Transform = { translation = [0.5, 0.0, 0.0] }
ChildOf = "hub"
)TOML";

    constexpr const char* kVehicleToml = R"TOML(
[entity.body]
Transform = { translation = [0.0, 0.5, 0.0] }

[entity.wheel_fl]
obek = "obek/wheel.obek.toml"
Transform = { translation = [1.0, 0.0, 1.5] }
)TOML";

    crd::containers::Array<InMemoryFile> files{crd::memory::default_allocator()};
    files.push_back({crd::containers::StringView{"obek/wheel.obek.toml"},
                     crd::containers::StringView{kWheelToml, std::strlen(kWheelToml)}});

    crd::containers::Array<CookError> errors{crd::memory::default_allocator()};
    const auto text_view = crd::containers::StringView{kVehicleToml, std::strlen(kVehicleToml)};
    auto bytes = obek_cooker_inline(text_view, make_ctx_with_resolver(files), &errors);
    REQUIRE(errors.size() == 0);
    REQUIRE(bytes.size() > 0);

    auto* res = load_obek_bytes(bytes);
    REQUIRE(res != nullptr);
    // Entities: vehicle's body + wheel_fl placeholder + wheel's hub + wheel's tire = 4.
    CHECK(res->info.entity_count == 4U);
    // OCHN: one Nested entry for the wheel.obek.toml reference.
    REQUIRE(res->chain_dependencies.size() == 1U);
    CHECK(res->chain_dependencies[0].kind == static_cast<crd::u8>(crd::scene::ObekChainKind::Nested));

    World target;
    setup_target_world(target);
    auto inst = target.instantiate_obek(*res);
    REQUIRE(inst.entities.size() == 4U);
    // Some entity must be a child of some other (the nested splice via ChildOf).
    bool nested_parented = false;
    for (crd::usize i = 0; i < inst.entities.size(); ++i)
    {
        for (crd::usize j = 0; j < inst.entities.size(); ++j)
        {
            if (i == j) continue;
            if (target.get_relation_target<ChildOf>(inst.entities[i]) == inst.entities[j])
            {
                nested_parented = true;
            }
        }
    }
    CHECK(nested_parented);
    unload_obek(res);
}

TEST_CASE("Two-level nested obek vehicle wheel tire-pattern", "[obek-cooker][nested][deep]")
{
    constexpr const char* kInner = R"TOML(
[entity.tread]
Transform = { translation = [0.1, 0.0, 0.0] }
)TOML";

    constexpr const char* kMiddle = R"TOML(
[entity.tire]
obek = "obek/inner.obek.toml"
Transform = { translation = [0.5, 0.0, 0.0] }
)TOML";

    constexpr const char* kOuter = R"TOML(
[entity.body]
Transform = { translation = [0.0, 0.0, 0.0] }

[entity.wheel]
obek = "obek/middle.obek.toml"
Transform = { translation = [1.0, 0.0, 0.0] }
)TOML";

    crd::containers::Array<InMemoryFile> files{crd::memory::default_allocator()};
    files.push_back({crd::containers::StringView{"obek/inner.obek.toml"},
                     crd::containers::StringView{kInner, std::strlen(kInner)}});
    files.push_back({crd::containers::StringView{"obek/middle.obek.toml"},
                     crd::containers::StringView{kMiddle, std::strlen(kMiddle)}});

    crd::containers::Array<CookError> errors{crd::memory::default_allocator()};
    const auto text_view = crd::containers::StringView{kOuter, std::strlen(kOuter)};
    auto bytes = obek_cooker_inline(text_view, make_ctx_with_resolver(files), &errors);
    REQUIRE(errors.size() == 0);
    REQUIRE(bytes.size() > 0);

    auto* res = load_obek_bytes(bytes);
    REQUIRE(res != nullptr);
    // body + wheel + tire + tread = 4 entities total.
    CHECK(res->info.entity_count == 4U);
    // OCHN: 2 Nested (one for middle, one for inner).
    REQUIRE(res->chain_dependencies.size() == 2U);
    crd::u32 nested_count = 0;
    for (const auto& dep : res->chain_dependencies)
    {
        if (dep.kind == static_cast<crd::u8>(crd::scene::ObekChainKind::Nested))
        {
            ++nested_count;
        }
    }
    CHECK(nested_count == 2U);
    unload_obek(res);
}

TEST_CASE("Nested obek cycle detection emits error", "[obek-cooker][nested][cycle]")
{
    // a includes b, b includes a â†’ cycle.
    constexpr const char* kA = R"TOML(
[entity.thing_a]
obek = "obek/b.obek.toml"
)TOML";

    constexpr const char* kB = R"TOML(
[entity.thing_b]
obek = "obek/a.obek.toml"
)TOML";

    crd::containers::Array<InMemoryFile> files{crd::memory::default_allocator()};
    files.push_back({crd::containers::StringView{"obek/a.obek.toml"},
                     crd::containers::StringView{kA, std::strlen(kA)}});
    files.push_back({crd::containers::StringView{"obek/b.obek.toml"},
                     crd::containers::StringView{kB, std::strlen(kB)}});

    crd::containers::Array<CookError> errors{crd::memory::default_allocator()};
    const auto text_view = crd::containers::StringView{kA, std::strlen(kA)};
    auto bytes = obek_cooker_inline(text_view, make_ctx_with_resolver(files), &errors);
    CHECK(bytes.size() == 0);
    REQUIRE(errors.size() >= 1);
    bool found = false;
    for (const auto& err : errors)
    {
        const crd::containers::StringView msg{err.message.c_str(), err.message.size()};
        if (msg.find("cycle") != crd::containers::StringView::npos)
        {
            found = true;
            break;
        }
    }
    CHECK(found);
}

// -----------------------------------------------------------------------------
// Determinism
// -----------------------------------------------------------------------------

TEST_CASE("Determinism: identical obek TOML produces bit-equal bytes", "[obek-cooker][determinism]")
{
    constexpr const char* kTomlText = R"TOML(
[entity.alpha]
Transform = { translation = [1.0, 0.0, 0.0] }

[entity.beta]
Transform = { translation = [0.0, 1.0, 0.0] }
ChildOf = "alpha"
)TOML";

    auto cook = [&]() {
        crd::containers::Array<CookError> errors{crd::memory::default_allocator()};
        const auto text_view = crd::containers::StringView{kTomlText, std::strlen(kTomlText)};
        return obek_cooker_inline(text_view, make_ctx(), &errors);
    };
    auto a = cook();
    auto b = cook();
    REQUIRE(a.size() == b.size());
    REQUIRE(a.size() > 0);
    CHECK(std::memcmp(a.data(), b.data(), a.size()) == 0);
}
