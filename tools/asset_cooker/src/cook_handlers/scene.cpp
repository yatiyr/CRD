// Phase 3.0 v1l — `.scene.toml` → SCEN cooker (ADR-0055).
//
// Workflow:
//   1. Parse TOML text via toml++.
//   2. Walk [entity.NAME] tables in document order; allocate file-local
//      indices.
//   3. For each entity, walk component fields in alphabetical key order
//      (advisor pin #8 determinism).
//   4. For component keys: dispatch to the registered TOML reader,
//      stamp the component into a temp World.
//   5. For relation keys: defer until pass 2 (need all entity names
//      resolved first).
//   6. Pass 2: walk relations; resolve entity-name → file_idx; install
//      via World::add_relation_via_id.
//   7. Run SceneArtifactBuilder on the temp World; return CRDR bytes.
//
// All errors accumulate (advisor pin #3) — the cooker reports every
// problem found in one pass.

#include <crd/cooker/scene_cooker.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/hash_map.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/math/quat.hpp>
#include <crd/math/vec.hpp>
#include <crd/scene/relation.hpp>
#include <crd/scene/scene_resource.hpp>
#include <crd/scene/serialize.hpp>
#include <crd/scene/system.hpp>
#include <crd/scene/transform.hpp>
#include <crd/scene/transform_propagation.hpp>
#include <crd/scene/world.hpp>

#include <toml++/toml.hpp>

#include <cstring>
#include <memory>
#include <string>
#include <string_view>

namespace crd::cooker
{

namespace
{
struct ReaderEntry
{
    crd::containers::String     key;
    ComponentTomlReaderFn       reader;
    crd::scene::ComponentSerialize serialize;
    bool                        is_relation;

    explicit ReaderEntry(crd::memory::IAllocator* a) : key(a) {}
    ReaderEntry(ReaderEntry&&) = default;
    ReaderEntry& operator=(ReaderEntry&&) = default;
};

inline void emit_error(crd::containers::Array<CookError>& errors,
                       crd::containers::StringView         msg,
                       crd::u32 line = 0,
                       crd::u32 col  = 0)
{
    CookError err{errors.allocator()};
    err.message = crd::containers::String{msg, errors.allocator()};
    err.line    = line;
    err.column  = col;
    errors.push_back(std::move(err));
}

// Convert a toml::node source-position to a u32 line.
inline crd::u32 line_of(const toml::node& n) noexcept
{
    return static_cast<crd::u32>(n.source().begin.line);
}

[[nodiscard]] bool read_f32_field(const toml::node* node, crd::f32 fallback, crd::f32& out)
{
    if (node == nullptr)
    {
        out = fallback;
        return true;
    }
    if (auto v = node->value<double>(); v.has_value())
    {
        out = static_cast<crd::f32>(*v);
        return true;
    }
    if (auto v = node->value<int64_t>(); v.has_value())
    {
        out = static_cast<crd::f32>(*v);
        return true;
    }
    return false;
}

// Read a Vec3 from a TOML array of three floats. Returns false on type
// mismatch; out unchanged.
[[nodiscard]] bool read_vec3(const toml::node* node, crd::math::Vec3f fallback, crd::math::Vec3f& out)
{
    if (node == nullptr)
    {
        out = fallback;
        return true;
    }
    const toml::array* arr = node->as_array();
    if (arr == nullptr || arr->size() < 3)
    {
        return false;
    }
    crd::f32 xs[3] = {0, 0, 0};
    for (crd::usize i = 0; i < 3; ++i)
    {
        if (!read_f32_field(arr->get(i), 0.0F, xs[i]))
        {
            return false;
        }
    }
    out = crd::math::Vec3f{xs[0], xs[1], xs[2]};
    return true;
}

[[nodiscard]] bool read_quat(const toml::node* node, crd::math::Quatf fallback, crd::math::Quatf& out)
{
    if (node == nullptr)
    {
        out = fallback;
        return true;
    }
    const toml::array* arr = node->as_array();
    if (arr == nullptr || arr->size() < 4)
    {
        return false;
    }
    crd::f32 xs[4] = {0, 0, 0, 1};
    for (crd::usize i = 0; i < 4; ++i)
    {
        if (!read_f32_field(arr->get(i), 0.0F, xs[i]))
        {
            return false;
        }
    }
    out = crd::math::Quatf{xs[0], xs[1], xs[2], xs[3]};
    return true;
}

} // namespace

// ---- Built-in: Transform reader ---------------------------------------

bool read_transform_from_toml(const void* node_opaque, void* dst, crd::usize size, crd::u32 line,
                              crd::containers::Array<CookError>* errors)
{
    if (size != sizeof(crd::scene::Transform))
    {
        if (errors != nullptr)
        {
            emit_error(*errors, crd::containers::StringView{"Transform reader: size mismatch"}, line);
        }
        return false;
    }
    auto* node = static_cast<const toml::node*>(node_opaque);
    auto* table = node->as_table();
    if (table == nullptr)
    {
        if (errors != nullptr)
        {
            emit_error(*errors, crd::containers::StringView{"Transform: expected a table"}, line);
        }
        return false;
    }

    crd::math::Vec3f translation{0, 0, 0};
    crd::math::Quatf rotation = crd::math::Quatf::identity();
    crd::math::Vec3f scale{1, 1, 1};

    if (!read_vec3(table->get("translation"), translation, translation))
    {
        if (errors != nullptr)
        {
            emit_error(*errors, crd::containers::StringView{"Transform.translation: expected [f, f, f]"}, line);
        }
        return false;
    }
    if (!read_quat(table->get("rotation"), rotation, rotation))
    {
        if (errors != nullptr)
        {
            emit_error(*errors, crd::containers::StringView{"Transform.rotation: expected [x, y, z, w]"}, line);
        }
        return false;
    }
    if (!read_vec3(table->get("scale"), scale, scale))
    {
        if (errors != nullptr)
        {
            emit_error(*errors, crd::containers::StringView{"Transform.scale: expected [f, f, f]"}, line);
        }
        return false;
    }

    auto* t        = static_cast<crd::scene::Transform*>(dst);
    t->translation = translation;
    t->rotation    = rotation;
    (void)crd::math::try_normalize(t->rotation);
    t->scale = scale;
    // Leave world = identity. The cooker doesn't run TransformPropagation
    // on its temp world (no system registration), so baking world =
    // from_trs(local) would produce stale values for child entities (the
    // bake misses parent.world * local composition). Instead, world stays
    // identity in the cooked SCEN; consumers call mark_transform_subtree_dirty
    // + step() once after instantiate_scene to derive the correct world
    // matrices via propagation.
    t->world = crd::math::Mat4f::identity();
    return true;
}

// Run TransformPropagation on the cooker's temp world so the SCEN bytes
// carry the correct world matrices baked in. Without this, child entities'
// `world` field is identity (or local-only) — consumers must call
// mark_transform_subtree_dirty + step() after instantiate_scene to derive
// world matrices on first frame. v1l ships THE BAKED PATH: cooker runs
// propagation pre-serialise → SCEN bytes carry final world matrices →
// instantiate_scene + read transform.world directly works without
// requiring the consumer to step() first.

// ---- SceneCooker::Impl -------------------------------------------------

struct SceneCooker::Impl
{
    crd::memory::IAllocator*               alloc;
    crd::containers::Array<ReaderEntry>    readers;

    explicit Impl(crd::memory::IAllocator* a) : alloc(a), readers(a) {}

    [[nodiscard]] const ReaderEntry* find_reader(crd::containers::StringView key) const noexcept
    {
        for (const ReaderEntry& r : readers)
        {
            const crd::containers::StringView rk{r.key.c_str(), r.key.size()};
            if (rk == key)
            {
                return &r;
            }
        }
        return nullptr;
    }
};

SceneCooker::SceneCooker(crd::memory::IAllocator* alloc) : m_impl(new (alloc->allocate(sizeof(Impl), alignof(Impl))) Impl(alloc))
{
}

SceneCooker::~SceneCooker()
{
    if (m_impl != nullptr)
    {
        crd::memory::IAllocator* a = m_impl->alloc;
        m_impl->~Impl();
        a->deallocate(m_impl);
        m_impl = nullptr;
    }
}

void SceneCooker::register_reader(crd::containers::StringView key,
                                  ComponentTomlReaderFn       reader,
                                  crd::scene::ComponentSerialize serialize_trait,
                                  bool                        is_relation)
{
    ReaderEntry e{m_impl->alloc};
    e.key         = crd::containers::String{key, m_impl->alloc};
    e.reader      = reader;
    e.serialize   = serialize_trait;
    e.is_relation = is_relation;
    m_impl->readers.push_back(std::move(e));
}

void SceneCooker::register_builtin_readers()
{
    using namespace crd::scene;

    crd::scene::ComponentSerialize transform_cs{};
    transform_cs.fourcc  = kFourCC_Transform;
    transform_cs.version = 1U;
    register_reader(crd::containers::StringView{"Transform"}, &read_transform_from_toml, transform_cs,
                   /*is_relation=*/false);

    // The six built-in relations. Their TOML reader is null — the cooker
    // dispatches relations through a dedicated entity-name resolver pass
    // (relations don't need a per-component reader; their bytes are just
    // an EntityId resolved from a name string).
    auto rel_cs = [](crd::u32 fc) {
        crd::scene::ComponentSerialize c{};
        c.fourcc  = fc;
        c.version = 1U;
        return c;
    };

    register_reader(crd::containers::StringView{"ChildOf"},     nullptr, rel_cs(kFourCC_RelChildOf),     true);
    register_reader(crd::containers::StringView{"AttachedTo"},  nullptr, rel_cs(kFourCC_RelAttachedTo),  true);
    register_reader(crd::containers::StringView{"Owns"},        nullptr, rel_cs(kFourCC_RelOwns),        true);
    register_reader(crd::containers::StringView{"Targets"},     nullptr, rel_cs(kFourCC_RelTargets),     true);
    register_reader(crd::containers::StringView{"DependsOn"},   nullptr, rel_cs(kFourCC_RelDependsOn),   true);
    register_reader(crd::containers::StringView{"PossessedBy"}, nullptr, rel_cs(kFourCC_RelPossessedBy), true);
}

namespace
{
// Return true iff a relation TOML key's value is acceptable in the
// world's relation model. ChildOf / AttachedTo / Owns / DependsOn are
// Acyclic — at most one target. Targets / PossessedBy accept multi-
// target arrays.
[[nodiscard]] bool relation_allows_array(crd::u32 fourcc) noexcept
{
    using namespace crd::scene;
    if (fourcc == kFourCC_RelChildOf || fourcc == kFourCC_RelAttachedTo || fourcc == kFourCC_RelOwns ||
        fourcc == kFourCC_RelDependsOn)
    {
        return false;
    }
    return true;
}

// Setup the temp World used to materialise the cooked scene before
// SceneArtifactBuilder hands it bytes. Registers Transform + builtin
// relations + Transform's serialize trait — must match the runtime
// World's registrations bit-for-bit.
//
// TransformPropagation IS registered: the cooker runs step() once after
// pass 3 (relations installed) so the SCEN bytes carry final composed
// world matrices. Consumers can read `transform.world` directly post-
// instantiate without needing a separate mark+step pass.
void setup_temp_world(crd::scene::World& w)
{
    w.register_component<crd::scene::Transform>(crd::scene::transform_serialize_trait());
    w.register_component<crd::scene::TransformDirtyFlag>(crd::scene::StorageHint::SparseSet);
    w.register_builtin_relations();
    w.register_system(std::make_unique<crd::scene::TransformPropagation>());
}

// Look up a registered ComponentId by FourCC.
[[nodiscard]] crd::scene::ComponentId find_component_by_fourcc(const crd::scene::World& w, crd::u32 fourcc) noexcept
{
    const auto& reg = w.components();
    for (crd::u16 i = 0; i < reg.size(); ++i)
    {
        const crd::scene::ComponentId cid{i};
        const crd::scene::ComponentInfo* info = reg.info(cid);
        if (info != nullptr && info->serialize.fourcc == fourcc)
        {
            return cid;
        }
    }
    return crd::scene::ComponentId{};
}

// Apply a component reader's bytes to entity `e` via the registered
// component's storage backend.
void apply_component_bytes(crd::scene::World& w, crd::scene::EntityId e, crd::scene::ComponentId cid,
                           const crd::scene::ComponentInfo* info, const void* bytes)
{
    // The storage backend's insert path takes a void* (may move-from).
    // For the temporary World inside the cooker, we pass the bytes
    // through a stack-staging buffer to give the backend a mutable
    // pointer — same pattern as World::instantiate_scene's hot path.
    alignas(16) crd::u8 staging[1024];
    CRD_ASSERT(info->size <= sizeof(staging) && "scene_cooker: component size exceeds staging (1024)");
    std::memcpy(staging, bytes, info->size);
    w.backend_for_public(cid).insert(e, cid, staging);
}

} // namespace

crd::containers::Array<crd::u8> SceneCooker::cook_inline(
    crd::containers::StringView      toml_text,
    const SceneCookContext&          ctx,
    crd::containers::Array<CookError>* errors_out)
{
    crd::containers::Array<CookError> local_errors{ctx.allocator};
    auto& errors = (errors_out != nullptr) ? *errors_out : local_errors;

    // Parse TOML. toml++ is exception-free in compile mode CRD_TOML_NO_EXCEPTIONS;
    // its `parse` returns a result type with .err() / .table().
    const std::string_view text_sv{toml_text.data(), toml_text.size()};
    toml::parse_result parse_result = toml::parse(text_sv);
    if (!parse_result)
    {
        const auto& err = parse_result.error();
        emit_error(errors, crd::containers::StringView{"toml: parse failed"},
                   static_cast<crd::u32>(err.source().begin.line),
                   static_cast<crd::u32>(err.source().begin.column));
        return crd::containers::Array<crd::u8>{ctx.allocator};
    }
    const toml::table& root = parse_result.table();

    // Build the temp World. Each user-registered reader must have a
    // matching `register_component<T>(...serialize_trait...)` registration
    // in this World — handled by registering all known readers' types
    // upfront.
    crd::scene::World world{ctx.allocator};
    setup_temp_world(world);

    // The [entity.NAME] tables. Iterate in document order via the
    // top-level table's order — toml::table preserves insertion order.
    const toml::node* entity_root_node = root.get("entity");
    if (entity_root_node == nullptr)
    {
        // Empty scene — emit a valid SCEN with zero entities.
        crd::scene::SceneArtifactBuilder builder{ctx.allocator, ctx.id};
        return builder.build(world);
    }
    const toml::table* entity_root = entity_root_node->as_table();
    if (entity_root == nullptr)
    {
        emit_error(errors, crd::containers::StringView{"toml: 'entity' must be a table"},
                   line_of(*entity_root_node));
        return crd::containers::Array<crd::u8>{ctx.allocator};
    }

    // Pass 1: collect entity names + spawn EntityIds. Reject duplicates.
    crd::containers::HashMap<crd::u64, crd::scene::EntityId> name_to_entity{ctx.allocator};
    crd::containers::Array<crd::containers::String>          entity_names{ctx.allocator};
    crd::containers::Array<const toml::table*>               entity_tables{ctx.allocator};

    for (const auto& [key, value] : *entity_root)
    {
        const auto* entity_table = value.as_table();
        if (entity_table == nullptr)
        {
            emit_error(errors, crd::containers::StringView{"toml: 'entity.NAME' must be a table"},
                       line_of(value));
            continue;
        }
        const std::string_view name_sv{key.str()};
        // Reject hierarchical name references (advisor pin #2).
        if (name_sv.find('.') != std::string_view::npos)
        {
            emit_error(errors,
                       crd::containers::StringView{"hierarchical entity names (e.g. 'player.right_hand') are "
                                                   "reserved for v1l+1; use a flat name with explicit ChildOf"},
                       line_of(value));
            continue;
        }
        // Reject duplicate names.
        const crd::u64 hash = crd::containers::fnv1a_64(name_sv.data(), name_sv.size());
        if (name_to_entity.find(hash) != nullptr)
        {
            emit_error(errors, crd::containers::StringView{"duplicate entity name"}, line_of(value));
            continue;
        }
        // Spawn.
        crd::scene::EntityId e = world.spawn();
        name_to_entity.emplace(hash, e);
        entity_names.push_back(crd::containers::String{
            crd::containers::StringView{name_sv.data(), name_sv.size()}, ctx.allocator});
        entity_tables.push_back(entity_table);
    }

    // Pass 2: per-entity component fields. Walk in alphabetical key
    // order for determinism (advisor pin #8).
    for (crd::usize ei = 0; ei < entity_names.size(); ++ei)
    {
        const auto* entity_table = entity_tables[ei];
        // Resolve the entity's id from the name (we have it indexed
        // by hash; this is just a sanity check).
        const std::string_view name_sv{entity_names[ei].c_str(), entity_names[ei].size()};
        const crd::u64 hash = crd::containers::fnv1a_64(name_sv.data(), name_sv.size());
        crd::scene::EntityId* slot = name_to_entity.find(hash);
        if (slot == nullptr)
        {
            continue; // shouldn't happen — duplicate-or-invalid was caught above
        }
        const crd::scene::EntityId entity = *slot;

        // Sort field keys alphabetically (toml++ tables iterate in
        // insertion order; we need deterministic ordering for SCEN
        // determinism).
        crd::containers::Array<std::string_view> field_keys{ctx.allocator};
        for (const auto& [key, _] : *entity_table)
        {
            field_keys.push_back(std::string_view{key.str()});
        }
        // Simple insertion sort; alphabetic by underlying char order.
        for (crd::usize i = 1; i < field_keys.size(); ++i)
        {
            std::string_view k = field_keys[i];
            crd::usize j = i;
            while (j > 0 && field_keys[j - 1] > k)
            {
                field_keys[j] = field_keys[j - 1];
                --j;
            }
            field_keys[j] = k;
        }

        for (std::string_view fk : field_keys)
        {
            const crd::containers::StringView fk_view{fk.data(), fk.size()};
            const ReaderEntry* reader = m_impl->find_reader(fk_view);
            if (reader == nullptr)
            {
                emit_error(errors,
                           crd::containers::StringView{"unknown component or relation key"},
                           line_of(*entity_table->get(fk)));
                continue;
            }
            // Relations are deferred to pass 3 so we have the full
            // entity-name table.
            if (reader->is_relation)
            {
                continue;
            }
            // Component path — invoke the reader.
            const toml::node* field_node = entity_table->get(fk);
            const crd::scene::ComponentId cid = find_component_by_fourcc(world, reader->serialize.fourcc);
            if (cid.is_null())
            {
                emit_error(errors,
                           crd::containers::StringView{"component reader registered but not in World registry"},
                           line_of(*field_node));
                continue;
            }
            const crd::scene::ComponentInfo* info = world.components().info(cid);
            CRD_ASSERT(info != nullptr);

            alignas(16) crd::u8 staging[1024];
            CRD_ASSERT(info->size <= sizeof(staging) && "scene_cooker: component too large for staging");
            std::memset(staging, 0, info->size);
            if (!reader->reader(field_node, staging, info->size, line_of(*field_node), &errors))
            {
                continue; // reader emitted its own error
            }
            apply_component_bytes(world, entity, cid, info, staging);
        }
    }

    // Pass 3: relations. Now every entity name is resolved.
    for (crd::usize ei = 0; ei < entity_names.size(); ++ei)
    {
        const auto* entity_table = entity_tables[ei];
        const std::string_view name_sv{entity_names[ei].c_str(), entity_names[ei].size()};
        const crd::u64 hash = crd::containers::fnv1a_64(name_sv.data(), name_sv.size());
        crd::scene::EntityId* slot = name_to_entity.find(hash);
        if (slot == nullptr)
        {
            continue;
        }
        const crd::scene::EntityId src = *slot;

        for (const auto& [key, value] : *entity_table)
        {
            const std::string_view fk{key.str()};
            const crd::containers::StringView fk_view{fk.data(), fk.size()};
            const ReaderEntry* reader = m_impl->find_reader(fk_view);
            if (reader == nullptr || !reader->is_relation)
            {
                continue;
            }
            // Relation value: string OR array of strings.
            auto resolve_target = [&](const std::string_view& target_name) -> crd::scene::EntityId
            {
                const crd::u64 h = crd::containers::fnv1a_64(target_name.data(), target_name.size());
                if (auto* found = name_to_entity.find(h); found != nullptr)
                {
                    return *found;
                }
                return crd::scene::EntityId::null();
            };
            const crd::scene::ComponentId cid = find_component_by_fourcc(world, reader->serialize.fourcc);
            if (cid.is_null())
            {
                emit_error(errors, crd::containers::StringView{"relation registered but not in World"},
                           line_of(value));
                continue;
            }

            if (auto sv = value.value<std::string_view>(); sv.has_value())
            {
                // Single target.
                const crd::scene::EntityId tgt = resolve_target(*sv);
                if (tgt.is_null())
                {
                    emit_error(errors,
                               crd::containers::StringView{"relation target entity name not found"},
                               line_of(value));
                    continue;
                }
                world.add_relation_via_id(cid, src, tgt);
            }
            else if (const auto* arr = value.as_array(); arr != nullptr)
            {
                if (!relation_allows_array(reader->serialize.fourcc))
                {
                    emit_error(errors,
                               crd::containers::StringView{"acyclic relation cannot have multiple targets"},
                               line_of(value));
                    continue;
                }
                for (const auto& elt : *arr)
                {
                    if (auto elt_sv = elt.value<std::string_view>(); elt_sv.has_value())
                    {
                        const crd::scene::EntityId tgt = resolve_target(*elt_sv);
                        if (tgt.is_null())
                        {
                            emit_error(errors,
                                       crd::containers::StringView{"relation target entity name not found"},
                                       line_of(value));
                            continue;
                        }
                        world.add_relation_via_id(cid, src, tgt);
                    }
                    else
                    {
                        emit_error(errors,
                                   crd::containers::StringView{"relation array element must be a string"},
                                   line_of(value));
                    }
                }
            }
            else
            {
                emit_error(errors,
                           crd::containers::StringView{"relation value must be a string or array of strings"},
                           line_of(value));
            }
        }
    }

    // If any errors accumulated, fail the cook.
    if (errors.size() > 0)
    {
        return crd::containers::Array<crd::u8>{ctx.allocator};
    }

    // Bake final world matrices into the temp World by running the
    // PreRender phase's TransformPropagation. Mark every Transform-
    // bearing entity dirty so propagation walks them (set_translation
    // etc. weren't called — components were inserted directly via the
    // backend). After step() the SCEN bytes carry correct hierarchical
    // world matrices.
    for (crd::usize ei = 0; ei < entity_names.size(); ++ei)
    {
        const std::string_view name_sv{entity_names[ei].c_str(), entity_names[ei].size()};
        const crd::u64 hash = crd::containers::fnv1a_64(name_sv.data(), name_sv.size());
        const crd::scene::EntityId* slot = name_to_entity.find(hash);
        if (slot != nullptr && world.has_component<crd::scene::Transform>(*slot))
        {
            world.mark_transform_subtree_dirty(*slot);
        }
    }
    world.step(1.0 / 60.0);

    // Emit the SCEN bytes.
    crd::scene::SceneArtifactBuilder builder{ctx.allocator, ctx.id};
    return builder.build(world);
}

// ---- Free function ------------------------------------------------------

crd::containers::Array<crd::u8> scene_cooker_inline(
    crd::containers::StringView toml_text,
    const SceneCookContext&     ctx,
    crd::containers::Array<CookError>* errors_out)
{
    SceneCooker cooker{ctx.allocator};
    cooker.register_builtin_readers();
    return cooker.cook_inline(toml_text, ctx, errors_out);
}

} // namespace crd::cooker
