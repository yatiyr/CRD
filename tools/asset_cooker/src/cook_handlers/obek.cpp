// Phase 3.0 v1m3 — `.obek.toml` → OBEK cooker (ADR-0058).
//
// v1m3a (this file) — flat öbek substrate. The parse + entity-walk +
// component-apply + relation-install pipeline mirrors SceneCooker's
// (cook_handlers/scene.cpp) — they share the TOML reader registry and
// the same cook-time rules (no hierarchical names, alphabetical field
// order for determinism, multi-error accumulation, world-matrix bake
// via TransformPropagation).
//
// What's intentionally absent at v1m3a (each lights up in a later sub-slice):
//
//   v1m3b — `extends = "..."` chain resolution + cycle detection +
//           deepest-wins variant merge + OCHN emission.
//   v1m3c — `obek = "..."` nested-öbek references + recursive cook +
//           eager flatten + sub-instance tracking.
//   v1m3d — `overrides = [...]` cook-time override patches + OOVR chunk.

#include <crd/cooker/obek_cooker.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/hash_map.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/scene/obek.hpp>
#include <crd/scene/relation.hpp>
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
    crd::containers::String         key;
    ComponentTomlReaderFn           reader;
    crd::scene::ComponentSerialize  serialize;
    bool                            is_relation;

    explicit ReaderEntry(crd::memory::IAllocator* a) : key(a) {}
    ReaderEntry(ReaderEntry&&)            = default;
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

inline crd::u32 line_of(const toml::node& n) noexcept
{
    return static_cast<crd::u32>(n.source().begin.line);
}

[[nodiscard]] bool relation_allows_array(crd::u32 fourcc) noexcept
{
    using namespace crd::scene;
    if (fourcc == kFourCC_RelChildOf || fourcc == kFourCC_RelAttachedTo
        || fourcc == kFourCC_RelOwns || fourcc == kFourCC_RelDependsOn)
    {
        return false;
    }
    return true;
}

void setup_temp_world(crd::scene::World& w)
{
    w.register_component<crd::scene::Transform>(crd::scene::transform_serialize_trait());
    w.register_component<crd::scene::TransformDirtyFlag>(crd::scene::StorageHint::SparseSet);
    w.register_builtin_relations();
    w.register_system(std::make_unique<crd::scene::TransformPropagation>());
}

[[nodiscard]] crd::scene::ComponentId find_component_by_fourcc(const crd::scene::World& w,
                                                                crd::u32 fourcc) noexcept
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

void apply_component_bytes(crd::scene::World&             w,
                           crd::scene::EntityId           e,
                           crd::scene::ComponentId        cid,
                           const crd::scene::ComponentInfo* info,
                           const void*                    bytes)
{
    alignas(16) crd::u8 staging[1024];
    CRD_ASSERT(info->size <= sizeof(staging) && "obek_cooker: component size exceeds staging (1024)");
    std::memcpy(staging, bytes, info->size);
    w.backend_for_public(cid).insert(e, cid, staging);
}

} // namespace

// ---- ObekCooker::Impl ---------------------------------------------------

struct ObekCooker::Impl
{
    crd::memory::IAllocator*            alloc;
    crd::containers::Array<ReaderEntry> readers;

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

ObekCooker::ObekCooker(crd::memory::IAllocator* alloc)
    : m_impl(new (alloc->allocate(sizeof(Impl), alignof(Impl))) Impl(alloc))
{
}

ObekCooker::~ObekCooker()
{
    if (m_impl != nullptr)
    {
        crd::memory::IAllocator* a = m_impl->alloc;
        m_impl->~Impl();
        a->deallocate(m_impl);
        m_impl = nullptr;
    }
}

void ObekCooker::register_reader(crd::containers::StringView key,
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

void ObekCooker::register_builtin_readers()
{
    using namespace crd::scene;

    crd::scene::ComponentSerialize transform_cs{};
    transform_cs.fourcc  = kFourCC_Transform;
    transform_cs.version = 1U;
    register_reader(crd::containers::StringView{"Transform"}, &read_transform_from_toml, transform_cs,
                    /*is_relation=*/false);

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

// Apply one parsed öbek TOML table to the temp World. Idempotent across
// multiple calls — components UPSERT (later call wins per field), entity
// names dedupe by hash (re-mention an existing name → reuse its EntityId),
// relations install via add_relation_via_id (re-installation overrides
// the previous target). This is what makes the extends-chain "deepest
// wins" semantics work without explicit TOML table merging: apply the
// chain in deepest-first order; the latest call always wins.
//
// `name_to_entity` accumulates across calls; the caller drives lifetime.
// Errors append to `errors`; the function returns even on errors so the
// rest of the cook pipeline can continue accumulating.
namespace
{
struct EntityNamePair
{
    crd::containers::String name;
    crd::scene::EntityId    id;

    explicit EntityNamePair(crd::memory::IAllocator* a) : name(a), id(crd::scene::EntityId::null()) {}
    EntityNamePair(EntityNamePair&&)            = default;
    EntityNamePair& operator=(EntityNamePair&&) = default;
};

[[nodiscard]] const ReaderEntry* find_reader_in(const crd::containers::Array<ReaderEntry>& readers,
                                                crd::containers::StringView                key) noexcept
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

[[nodiscard]] crd::u64 fnv1a_64_bytes(const char* data, crd::usize size) noexcept
{
    constexpr crd::u64 prime  = 1099511628211ULL;
    constexpr crd::u64 offset = 14695981039346656037ULL;
    crd::u64 h = offset;
    for (crd::usize i = 0; i < size; ++i)
    {
        h ^= static_cast<crd::u8>(data[i]);
        h *= prime;
    }
    return h;
}

// One queued OCHN entry for the cooker. Path is owned to outlive the
// resolver-returned String.
struct PendingOchnEntry
{
    crd::containers::String   path;
    crd::u64                  content_hash{0U};
    crd::scene::ObekChainKind kind{crd::scene::ObekChainKind::Extends};

    explicit PendingOchnEntry(crd::memory::IAllocator* a) : path(a) {}
    PendingOchnEntry(PendingOchnEntry&&)            = default;
    PendingOchnEntry& operator=(PendingOchnEntry&&) = default;
};

// Recursion-state bundle threaded through the cooker. Reduces parameter
// noise and keeps the recursive descent (extends + nested öbek) clean.
struct RecCtx
{
    const ObekCookContext&                                    ctx;
    crd::scene::World&                                        world;
    crd::containers::HashMap<crd::u64, crd::scene::EntityId>& name_to_entity;
    crd::containers::Array<EntityNamePair>&                   accumulated_names;
    crd::containers::Array<PendingOchnEntry>&                 ochn;
    crd::containers::Array<crd::u64>&                         visited_path_hashes;
    const crd::containers::Array<ReaderEntry>&                readers;
    crd::containers::Array<CookError>&                        errors;
    crd::usize                                                depth;
};

constexpr crd::usize kMaxObekRecursionDepth = 64U;

// Forward declarations — the apply / chain-walk pair recurses.
void apply_table_to_world(const toml::table& root, RecCtx& rc);
[[nodiscard]] bool walk_and_apply_chain(crd::containers::StringView source_path,
                                         crd::containers::StringView source_text,
                                         RecCtx& rc);
} // namespace

crd::containers::Array<crd::u8> ObekCooker::cook_inline(
    crd::containers::StringView        toml_text,
    const ObekCookContext&             ctx,
    crd::containers::Array<CookError>* errors_out)
{
    crd::containers::Array<CookError> local_errors{ctx.allocator};
    auto& errors = (errors_out != nullptr) ? *errors_out : local_errors;

    crd::scene::World world{ctx.allocator};
    setup_temp_world(world);

    crd::containers::HashMap<crd::u64, crd::scene::EntityId> name_to_entity{ctx.allocator};
    crd::containers::Array<EntityNamePair>                   accumulated_names{ctx.allocator};
    crd::containers::Array<PendingOchnEntry>                 ochn{ctx.allocator};
    crd::containers::Array<crd::u64>                         visited_path_hashes{ctx.allocator};

    RecCtx rc{ctx, world, name_to_entity, accumulated_names, ochn, visited_path_hashes,
              m_impl->readers, errors, /*depth=*/0U};

    // Top-level cook — empty source path (root has no path; rec_ctx
    // visited_path_hashes only tracks resolved paths from extends + obek refs).
    (void)walk_and_apply_chain(crd::containers::StringView{}, toml_text, rc);

    if (errors.size() > 0)
    {
        return crd::containers::Array<crd::u8>{ctx.allocator};
    }

    crd::scene::ObekArtifactBuilder builder{ctx.allocator, ctx.id, ctx.obek_root_id};
    for (const PendingOchnEntry& entry : ochn)
    {
        builder.add_chain_dependency(crd::containers::StringView{entry.path.c_str(), entry.path.size()},
                                     entry.content_hash, entry.kind);
    }

    // ---- v1m3d: parse top-level `overrides = [...]` ----
    //
    // Each override entry: { entity = "name", component = "Component", value = {...} }.
    // - `entity` resolves to file_idx via accumulated_names (linear scan; öbek N
    //   typically << 1000).
    // - `component` resolves to FourCC via the reader registry.
    // - `value` is parsed by the registered TOML reader for that component
    //   into the full component-bytes payload (whole-component override; v1m5
    //   may extend with field-level overrides).
    //
    // Cook-time validated; the builder records each as a PendingOverride and
    // emits a single OOVR chunk in build().
    {
        toml::parse_result top_pr = toml::parse(std::string_view{toml_text.data(), toml_text.size()});
        if (!top_pr)
        {
            const auto& err = top_pr.error();
            emit_error(errors, crd::containers::StringView{"toml: parse failed re-parsing top-level"},
                       static_cast<crd::u32>(err.source().begin.line),
                       static_cast<crd::u32>(err.source().begin.column));
            return crd::containers::Array<crd::u8>{ctx.allocator};
        }
        const toml::table& root_tbl = top_pr.table();
        if (const auto* ovr_node = root_tbl.get("overrides"); ovr_node != nullptr)
        {
            const toml::array* ovr_arr = ovr_node->as_array();
            if (ovr_arr == nullptr)
            {
                emit_error(errors,
                           crd::containers::StringView{"`overrides` must be an array of inline tables"},
                           line_of(*ovr_node));
            }
            else
            {
                for (const auto& elt : *ovr_arr)
                {
                    const toml::table* entry = elt.as_table();
                    if (entry == nullptr)
                    {
                        emit_error(errors,
                                   crd::containers::StringView{"override entry must be an inline table"},
                                   line_of(elt));
                        continue;
                    }
                    const toml::node* entity_node = entry->get("entity");
                    const toml::node* comp_node   = entry->get("component");
                    const toml::node* value_node  = entry->get("value");
                    if (entity_node == nullptr || comp_node == nullptr || value_node == nullptr)
                    {
                        emit_error(errors,
                                   crd::containers::StringView{
                                       "override entry must have `entity`, `component`, and `value` fields"},
                                   line_of(elt));
                        continue;
                    }
                    auto entity_sv_opt = entity_node->value<std::string_view>();
                    auto comp_sv_opt   = comp_node->value<std::string_view>();
                    if (!entity_sv_opt.has_value() || !comp_sv_opt.has_value())
                    {
                        emit_error(errors,
                                   crd::containers::StringView{
                                       "override `entity` and `component` must be strings"},
                                   line_of(elt));
                        continue;
                    }

                    const std::string_view entity_name = *entity_sv_opt;
                    crd::u32 file_idx = static_cast<crd::u32>(accumulated_names.size());
                    for (crd::usize i = 0; i < accumulated_names.size(); ++i)
                    {
                        const auto& nm = accumulated_names[i].name;
                        if (nm.size() == entity_name.size()
                            && std::memcmp(nm.c_str(), entity_name.data(), nm.size()) == 0)
                        {
                            file_idx = static_cast<crd::u32>(i);
                            break;
                        }
                    }
                    if (file_idx >= accumulated_names.size())
                    {
                        emit_error(errors,
                                   crd::containers::StringView{"override entity name not found"},
                                   line_of(elt));
                        continue;
                    }

                    const std::string_view comp_name = *comp_sv_opt;
                    const crd::containers::StringView comp_view{comp_name.data(), comp_name.size()};
                    const ReaderEntry* reader = find_reader_in(m_impl->readers, comp_view);
                    if (reader == nullptr || reader->is_relation || reader->reader == nullptr)
                    {
                        emit_error(errors,
                                   crd::containers::StringView{
                                       "override `component` not registered as a component reader"},
                                   line_of(elt));
                        continue;
                    }

                    const crd::scene::ComponentId cid =
                        find_component_by_fourcc(world, reader->serialize.fourcc);
                    if (cid.is_null())
                    {
                        emit_error(errors,
                                   crd::containers::StringView{
                                       "override component reader not in World registry"},
                                   line_of(elt));
                        continue;
                    }
                    const crd::scene::ComponentInfo* info = world.components().info(cid);
                    CRD_ASSERT(info != nullptr);

                    alignas(16) crd::u8 staging[1024];
                    CRD_ASSERT(info->size <= sizeof(staging) && "obek_cooker: override too large");
                    std::memset(staging, 0, info->size);
                    if (!reader->reader(value_node, staging, info->size, line_of(*value_node), &errors))
                    {
                        continue;
                    }
                    builder.add_override(file_idx, reader->serialize.fourcc, /*field_offset=*/0U,
                                         crd::containers::ConstSpan<crd::u8>{staging, info->size});
                }
            }
        }
    }

    if (errors.size() > 0)
    {
        return crd::containers::Array<crd::u8>{ctx.allocator};
    }

    // Bake world matrices.
    for (const EntityNamePair& pair : accumulated_names)
    {
        if (world.has_component<crd::scene::Transform>(pair.id))
        {
            world.mark_transform_subtree_dirty(pair.id);
        }
    }
    world.step(1.0 / 60.0);

    return builder.build(world);
}

namespace
{
// Walk extends chain for `source_text` and apply each level (deepest
// first) plus the body itself into `rc.world`. Recurses into nested
// `obek = "..."` references via apply_table_to_world. Top-level call
// passes empty `source_path`.
//
// Returns true if cook succeeded (errors.size() unchanged); false if any
// error occurred during chain walk (errors appended). Cycle detection
// piggybacks on the shared visited_path_hashes set.
[[nodiscard]] bool walk_and_apply_chain(crd::containers::StringView source_path,
                                         crd::containers::StringView source_text,
                                         RecCtx& rc)
{
    if (rc.depth >= kMaxObekRecursionDepth)
    {
        emit_error(rc.errors,
                   crd::containers::StringView{"obek recursion exceeds max depth (64) — possible cycle"});
        return false;
    }

    struct LocalChainStep
    {
        crd::containers::String path;
        crd::containers::String text;
        crd::u64                content_hash{0U};

        explicit LocalChainStep(crd::memory::IAllocator* a) : path(a), text(a) {}
        LocalChainStep(LocalChainStep&&)            = default;
        LocalChainStep& operator=(LocalChainStep&&) = default;
    };

    crd::containers::Array<LocalChainStep> ancestors{rc.ctx.allocator};
    std::string_view iter_text{source_text.data(), source_text.size()};

    while (true)
    {
        if (ancestors.size() >= kMaxObekRecursionDepth)
        {
            emit_error(rc.errors,
                       crd::containers::StringView{"extends chain exceeds max depth (64) — possible cycle"});
            return false;
        }
        toml::parse_result pr = toml::parse(iter_text);
        if (!pr)
        {
            const auto& err = pr.error();
            emit_error(rc.errors, crd::containers::StringView{"toml: parse failed in extends chain"},
                       static_cast<crd::u32>(err.source().begin.line),
                       static_cast<crd::u32>(err.source().begin.column));
            return false;
        }
        const toml::table& tbl = pr.table();
        const toml::node*  ext = tbl.get("extends");
        if (ext == nullptr)
        {
            break;
        }
        auto ext_sv = ext->value<std::string_view>();
        if (!ext_sv.has_value())
        {
            emit_error(rc.errors, crd::containers::StringView{"`extends` must be a string path"},
                       line_of(*ext));
            return false;
        }
        const crd::u64 path_hash = fnv1a_64_bytes(ext_sv->data(), ext_sv->size());
        for (crd::u64 h : rc.visited_path_hashes)
        {
            if (h == path_hash)
            {
                emit_error(rc.errors, crd::containers::StringView{"cycle detected in extends/obek graph"},
                           line_of(*ext));
                return false;
            }
        }
        rc.visited_path_hashes.push_back(path_hash);

        if (rc.ctx.file_resolver == nullptr)
        {
            emit_error(rc.errors,
                       crd::containers::StringView{"extends used but ObekCookContext.file_resolver is null"},
                       line_of(*ext));
            return false;
        }
        LocalChainStep step{rc.ctx.allocator};
        step.path = crd::containers::String{
            crd::containers::StringView{ext_sv->data(), ext_sv->size()}, rc.ctx.allocator};
        if (!rc.ctx.file_resolver(crd::containers::StringView{step.path.c_str(), step.path.size()},
                                  rc.ctx.allocator, step.text, rc.ctx.file_resolver_ud))
        {
            emit_error(rc.errors, crd::containers::StringView{"extends path not found by resolver"},
                       line_of(*ext));
            return false;
        }
        step.content_hash = fnv1a_64_bytes(step.text.c_str(), step.text.size());
        iter_text = std::string_view{step.text.c_str(), step.text.size()};
        ancestors.push_back(std::move(step));
    }

    auto apply_one = [&](std::string_view text_sv) -> bool
    {
        toml::parse_result pr = toml::parse(text_sv);
        if (!pr)
        {
            const auto& err = pr.error();
            emit_error(rc.errors, crd::containers::StringView{"toml: parse failed"},
                       static_cast<crd::u32>(err.source().begin.line),
                       static_cast<crd::u32>(err.source().begin.column));
            return false;
        }
        apply_table_to_world(pr.table(), rc);
        return true;
    };

    for (crd::usize i = ancestors.size(); i > 0; --i)
    {
        if (!apply_one(std::string_view{ancestors[i - 1U].text.c_str(),
                                        ancestors[i - 1U].text.size()}))
        {
            return false;
        }
    }
    if (!apply_one(std::string_view{source_text.data(), source_text.size()}))
    {
        return false;
    }

    // Record OCHN entries — deepest extends first (matching apply order).
    // Each ancestor is recorded with `kind = Extends`. The current source
    // (source_path) is NOT recorded; it's "self".
    for (crd::usize i = ancestors.size(); i > 0; --i)
    {
        PendingOchnEntry entry{rc.ctx.allocator};
        entry.path         = std::move(ancestors[i - 1U].path);
        entry.content_hash = ancestors[i - 1U].content_hash;
        entry.kind         = crd::scene::ObekChainKind::Extends;
        rc.ochn.push_back(std::move(entry));
    }

    (void)source_path; // reserved for future error-reporting + kind=Nested attribution
    return true;
}

void apply_table_to_world(const toml::table& root, RecCtx& rc)
{
    // Reject `overrides` (top-level) — reserved for v1m3d.
    // Top-level `overrides = [...]` is handled in ObekCooker::cook_inline
    // AFTER walk_and_apply_chain returns. Silently skip here so ancestor
    // chain steps don't double-emit; the cook only honours overrides
    // declared in the current öbek (not those baked into ancestors).

    const toml::node* entity_root_node = root.get("entity");
    if (entity_root_node == nullptr)
    {
        return; // a chain step with no entities is fine
    }
    const toml::table* entity_root = entity_root_node->as_table();
    if (entity_root == nullptr)
    {
        emit_error(rc.errors, crd::containers::StringView{"toml: 'entity' must be a table"},
                   line_of(*entity_root_node));
        return;
    }

    // Pass 1: collect / dedupe entity names. Existing names reuse their
    // EntityId — that's how variant-chain "deepest wins" works without
    // table merging.
    crd::containers::Array<const toml::table*>      step_entity_tables{rc.ctx.allocator};
    crd::containers::Array<crd::scene::EntityId>    step_entity_ids{rc.ctx.allocator};

    for (const auto& [key, value] : *entity_root)
    {
        const auto* entity_table = value.as_table();
        if (entity_table == nullptr)
        {
            emit_error(rc.errors, crd::containers::StringView{"toml: 'entity.NAME' must be a table"},
                       line_of(value));
            continue;
        }
        const std::string_view name_sv{key.str()};
        if (name_sv.find('.') != std::string_view::npos)
        {
            emit_error(rc.errors,
                       crd::containers::StringView{"hierarchical entity names (e.g. 'player.right_hand') are "
                                                   "reserved; use a flat name with explicit ChildOf"},
                       line_of(value));
            continue;
        }
        const crd::u64 hash = crd::containers::fnv1a_64(name_sv.data(), name_sv.size());
        crd::scene::EntityId* existing = rc.name_to_entity.find(hash);
        crd::scene::EntityId  e        = (existing != nullptr) ? *existing : rc.world.spawn();
        if (existing == nullptr)
        {
            rc.name_to_entity.emplace(hash, e);
            EntityNamePair pair{rc.ctx.allocator};
            pair.name = crd::containers::String{
                crd::containers::StringView{name_sv.data(), name_sv.size()}, rc.ctx.allocator};
            pair.id = e;
            rc.accumulated_names.push_back(std::move(pair));
        }
        step_entity_tables.push_back(entity_table);
        step_entity_ids.push_back(e);
    }

    // Pass 2: per-entity component fields, alphabetical order. Per-entity
    // `obek = "..."` triggers nested-öbek recursion BEFORE the component
    // fields are applied — nested entities spawn first; then the parent's
    // own components apply on top of the placeholder entity.
    for (crd::usize ei = 0; ei < step_entity_tables.size(); ++ei)
    {
        const auto* entity_table = step_entity_tables[ei];
        const crd::scene::EntityId entity = step_entity_ids[ei];

        // ---- Per-entity `obek = "..."` nested reference (v1m3c) ----
        if (const toml::node* obek_node = entity_table->get("obek"); obek_node != nullptr)
        {
            auto obek_path_sv = obek_node->value<std::string_view>();
            if (!obek_path_sv.has_value())
            {
                emit_error(rc.errors,
                           crd::containers::StringView{"per-entity `obek` must be a string path"},
                           line_of(*obek_node));
            }
            else if (rc.ctx.file_resolver == nullptr)
            {
                emit_error(rc.errors,
                           crd::containers::StringView{
                               "per-entity `obek` used but ObekCookContext.file_resolver is null"},
                           line_of(*obek_node));
            }
            else
            {
                const crd::u64 path_hash = fnv1a_64_bytes(obek_path_sv->data(), obek_path_sv->size());
                bool already_visited = false;
                for (crd::u64 h : rc.visited_path_hashes)
                {
                    if (h == path_hash) { already_visited = true; break; }
                }
                if (already_visited)
                {
                    emit_error(rc.errors,
                               crd::containers::StringView{"cycle detected in extends/obek graph"},
                               line_of(*obek_node));
                }
                else
                {
                    rc.visited_path_hashes.push_back(path_hash);

                    crd::containers::String nested_path{
                        crd::containers::StringView{obek_path_sv->data(), obek_path_sv->size()},
                        rc.ctx.allocator};
                    crd::containers::String nested_text{rc.ctx.allocator};
                    if (!rc.ctx.file_resolver(
                            crd::containers::StringView{nested_path.c_str(), nested_path.size()},
                            rc.ctx.allocator, nested_text, rc.ctx.file_resolver_ud))
                    {
                        emit_error(rc.errors,
                                   crd::containers::StringView{"nested obek path not found by resolver"},
                                   line_of(*obek_node));
                    }
                    else
                    {
                        const crd::u64 nested_hash =
                            fnv1a_64_bytes(nested_text.c_str(), nested_text.size());

                        // Save + clear the parent's name table so nested entity
                        // names are scoped to the nested öbek (don't leak into
                        // the parent's name resolution). Captures into a local
                        // variable; restored after the nested apply.
                        crd::containers::HashMap<crd::u64, crd::scene::EntityId> saved_n2e{
                            std::move(rc.name_to_entity)};
                        rc.name_to_entity = crd::containers::HashMap<crd::u64,
                                                                     crd::scene::EntityId>{rc.ctx.allocator};
                        const crd::usize before_count = rc.accumulated_names.size();

                        ++rc.depth;
                        const bool ok = walk_and_apply_chain(
                            crd::containers::StringView{nested_path.c_str(), nested_path.size()},
                            crd::containers::StringView{nested_text.c_str(), nested_text.size()},
                            rc);
                        --rc.depth;

                        // Splice: any nested entity with no ChildOf yet gets
                        // ChildOf(parent_entity). Nested roots end up parented
                        // to the placeholder entity in the cooker temp World.
                        if (ok)
                        {
                            const crd::scene::ComponentId childof_id =
                                find_component_by_fourcc(rc.world, crd::scene::kFourCC_RelChildOf);
                            if (!childof_id.is_null())
                            {
                                for (crd::usize i = before_count; i < rc.accumulated_names.size(); ++i)
                                {
                                    const crd::scene::EntityId nested_e = rc.accumulated_names[i].id;
                                    // has_relation<ChildOf> via component_id check —
                                    // backend lookup avoids template instantiation here.
                                    const void* existing_payload =
                                        (rc.world.components().info(childof_id)->storage_hint
                                                == crd::scene::StorageHint::SparseSet)
                                            ? rc.world.sparse_storage().get_const(nested_e, childof_id)
                                            : rc.world.storage().get_const(nested_e, childof_id);
                                    if (existing_payload == nullptr)
                                    {
                                        rc.world.add_relation_via_id(childof_id, nested_e, entity);
                                    }
                                }
                            }

                            // Record OCHN entry for this nested reference.
                            PendingOchnEntry entry{rc.ctx.allocator};
                            entry.path         = std::move(nested_path);
                            entry.content_hash = nested_hash;
                            entry.kind         = crd::scene::ObekChainKind::Nested;
                            rc.ochn.push_back(std::move(entry));
                        }

                        // Restore parent's name table.
                        rc.name_to_entity = std::move(saved_n2e);
                    }
                }
            }
        }

        crd::containers::Array<std::string_view> field_keys{rc.ctx.allocator};
        for (const auto& [key, _] : *entity_table)
        {
            field_keys.push_back(std::string_view{key.str()});
        }
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
            if (fk == "obek")
            {
                continue; // handled above (nested-öbek recursion)
            }
            if (fk == "overrides")
            {
                emit_error(rc.errors,
                           crd::containers::StringView{"per-entity `overrides` reserved for v1m3d"},
                           line_of(*entity_table->get(fk)));
                continue;
            }
            const ReaderEntry* reader = find_reader_in(rc.readers, fk_view);
            if (reader == nullptr)
            {
                emit_error(rc.errors, crd::containers::StringView{"unknown component or relation key"},
                           line_of(*entity_table->get(fk)));
                continue;
            }
            if (reader->is_relation)
            {
                continue; // pass 3 below
            }
            const toml::node* field_node = entity_table->get(fk);
            const crd::scene::ComponentId cid = find_component_by_fourcc(rc.world, reader->serialize.fourcc);
            if (cid.is_null())
            {
                emit_error(rc.errors,
                           crd::containers::StringView{"component reader registered but not in World registry"},
                           line_of(*field_node));
                continue;
            }
            const crd::scene::ComponentInfo* info = rc.world.components().info(cid);
            CRD_ASSERT(info != nullptr);

            alignas(16) crd::u8 staging[1024];
            CRD_ASSERT(info->size <= sizeof(staging) && "obek_cooker: component too large for staging");
            std::memset(staging, 0, info->size);
            if (!reader->reader(field_node, staging, info->size, line_of(*field_node), &rc.errors))
            {
                continue;
            }
            apply_component_bytes(rc.world, entity, cid, info, staging);
        }
    }

    // Pass 3: relations.
    for (crd::usize ei = 0; ei < step_entity_tables.size(); ++ei)
    {
        const auto* entity_table = step_entity_tables[ei];
        const crd::scene::EntityId src = step_entity_ids[ei];

        for (const auto& [key, value] : *entity_table)
        {
            const std::string_view fk{key.str()};
            if (fk == "obek" || fk == "overrides")
            {
                continue;
            }
            const crd::containers::StringView fk_view{fk.data(), fk.size()};
            const ReaderEntry* reader = find_reader_in(rc.readers, fk_view);
            if (reader == nullptr || !reader->is_relation)
            {
                continue;
            }
            const crd::scene::ComponentId cid = find_component_by_fourcc(rc.world, reader->serialize.fourcc);
            if (cid.is_null())
            {
                emit_error(rc.errors,
                           crd::containers::StringView{"relation registered but not in World registry"},
                           line_of(value));
                continue;
            }
            auto resolve_target = [&](const std::string_view& target_name) -> crd::scene::EntityId
            {
                const crd::u64 h = crd::containers::fnv1a_64(target_name.data(), target_name.size());
                if (auto* found = rc.name_to_entity.find(h); found != nullptr)
                {
                    return *found;
                }
                return crd::scene::EntityId::null();
            };
            if (auto sv = value.value<std::string_view>(); sv.has_value())
            {
                const crd::scene::EntityId tgt = resolve_target(*sv);
                if (tgt.is_null())
                {
                    emit_error(rc.errors,
                               crd::containers::StringView{"relation target entity name not found"},
                               line_of(value));
                    continue;
                }
                rc.world.add_relation_via_id(cid, src, tgt);
            }
            else if (const auto* arr = value.as_array(); arr != nullptr)
            {
                if (!relation_allows_array(reader->serialize.fourcc))
                {
                    emit_error(rc.errors,
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
                            emit_error(rc.errors,
                                       crd::containers::StringView{"relation target entity name not found"},
                                       line_of(value));
                            continue;
                        }
                        rc.world.add_relation_via_id(cid, src, tgt);
                    }
                    else
                    {
                        emit_error(rc.errors,
                                   crd::containers::StringView{"relation array element must be a string"},
                                   line_of(value));
                    }
                }
            }
            else
            {
                emit_error(rc.errors,
                           crd::containers::StringView{"relation value must be a string or array of strings"},
                           line_of(value));
            }
        }
    }
}
} // namespace

// ---- Free function -----------------------------------------------------

crd::containers::Array<crd::u8> obek_cooker_inline(
    crd::containers::StringView        toml_text,
    const ObekCookContext&             ctx,
    crd::containers::Array<CookError>* errors_out)
{
    ObekCooker cooker{ctx.allocator};
    cooker.register_builtin_readers();
    return cooker.cook_inline(toml_text, ctx, errors_out);
}

} // namespace crd::cooker
