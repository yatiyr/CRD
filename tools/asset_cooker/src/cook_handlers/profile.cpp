// Phase 3.0 v1o3 — `.profile.toml` → PROF cooker handler (ADR-0060).
//
// Reads a TOML file declaring one or more profile rules (priority +
// optional predicates) plus a bundle of preset references; emits a
// FINF/FRLE/FBND CRDR blob via `ProfileArtifactBuilder`.
//
// Format (single-rule, the v1o3 minimum):
//
//   [[profile]]
//   priority = 0
//   bundle = ["quality_default.preset.toml", "camera_default.preset.toml"]
//
// Predicates are forward-compatible:
//
//   [[profile]]
//   priority = 100
//   [[profile.predicate]]
//   field = "OperatingSystem"   # canonical PredicateField name
//   op = "Equal"                # canonical PredicateOp name
//   value_int = 1               # raw enum value (Windows = 1)
//
// Bundle entries are filesystem paths relative to the .profile.toml's
// directory. The handler resolves each by reading the sibling `.meta`
// sidecar — that requires the referenced presets to be cooked FIRST,
// which the asset cooker's alphabetical-by-path ordering guarantees in
// practice (presets typically sit alongside the profile). If a referenced
// .meta sidecar is missing, the cook fails with a clear diagnostic.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/cooker/cook_handler.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/profile/profile_artifact_builder.hpp>
#include <crd/profile/profile_predicate.hpp>
#include <crd/profile/profile_resource.hpp>
#include <crd/resources/resource_id.hpp>

#include <toml++/toml.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace fs = crd::platform::fs;

namespace crd::cooker
{

namespace
{

constexpr crd::u32 kProfileHandlerVersion = 1U;

// Resolve `bundle_entry_path` (relative to the profile .toml's directory)
// to a ResourceId via the corresponding `.meta` sidecar. Returns null
// id on failure; the caller emits a diagnostic.
[[nodiscard]] crd::resources::ResourceId resolve_meta_uuid(
    const fs::Path& source_dir, std::string_view rel,
    crd::memory::IAllocator* alloc)
{
    crd::containers::String full(alloc);
    full.append(source_dir.generic().data(), source_dir.generic().size());
    full.append("/");
    full.append(rel.data(), rel.size());
    full.append(".meta");
    const fs::Path meta_path(crd::containers::StringView{full.data(), full.size()});

    crd::containers::String text(alloc);
    if (!fs::read_file_text(meta_path, text))
        return {};
    const std::string_view sv(text.data(), text.size());
    const std::string_view key = "uuid = \"";
    const auto pos = sv.find(key);
    if (pos == std::string_view::npos)
        return {};
    const auto start = pos + key.size();
    const auto end   = sv.find('"', start);
    if (end == std::string_view::npos)
        return {};
    return crd::resources::ResourceId::parse(sv.substr(start, end - start));
}

// Map a canonical PredicateField name to the enum value. Returns false
// if the name is unknown.
[[nodiscard]] bool parse_predicate_field(std::string_view name,
                                         crd::profile::PredicateField& out)
{
    using F = crd::profile::PredicateField;
    if (name == "Os")        { out = F::Os;        return true; }
    if (name == "GpuTier")   { out = F::GpuTier;   return true; }
    if (name == "Domain")    { out = F::Domain;    return true; }
    if (name == "Mode")      { out = F::Mode;      return true; }
    if (name == "TargetFps") { out = F::TargetFps; return true; }
    if (name == "CpuCores")  { out = F::CpuCores;  return true; }
    return false;
}

[[nodiscard]] bool parse_predicate_op(std::string_view name, crd::profile::PredicateOp& out)
{
    using O = crd::profile::PredicateOp;
    if (name == "Equal")     { out = O::Equal;     return true; }
    if (name == "GreaterEq") { out = O::GreaterEq; return true; }
    if (name == "LessEq")    { out = O::LessEq;    return true; }
    if (name == "InMask")    { out = O::InMask;    return true; }
    return false;
}

CookResult profile_handler(const CookContext& ctx)
{
    CookResult result(ctx.allocator);

    crd::containers::String text(ctx.allocator);
    if (!fs::read_file_text(fs::Path(ctx.source_path), text))
        return result;

    const auto parsed = toml::parse(std::string_view{text.data(), text.size()});
    if (!parsed)
    {
        std::fprintf(stderr, "profile cook: TOML parse error in %.*s\n",
                     static_cast<int>(ctx.source_path.size()), ctx.source_path.data());
        return result;
    }

    // Resolve the source directory (everything before the last '/').
    fs::Path source_dir;
    {
        const auto sp = ctx.source_path;
        const auto slash = sp.rfind('/');
        if (slash != crd::containers::StringView::npos)
        {
            source_dir = fs::Path(sp.substr(0U, slash));
        }
    }

    crd::profile::ProfileArtifactBuilder b{ctx.allocator, /*schema_version=*/1U, ctx.id};

    const toml::table& root = parsed.table();
    const toml::node*  profile_array_node = root.get("profile");
    if (profile_array_node == nullptr || !profile_array_node->is_array_of_tables())
    {
        std::fprintf(stderr, "profile cook: %.*s missing required `[[profile]]` array of tables\n",
                     static_cast<int>(ctx.source_path.size()), ctx.source_path.data());
        return result;
    }
    const toml::array& profile_array = *profile_array_node->as_array();

    for (const auto& node : profile_array)
    {
        const toml::table* profile = node.as_table();
        if (profile == nullptr)
        {
            std::fprintf(stderr, "profile cook: [[profile]] entry must be a table\n");
            return result;
        }

        // priority — defaults to 0 (lowest).
        crd::u32 priority = 0U;
        if (const toml::node* p = profile->get("priority"); p != nullptr)
        {
            if (auto v = p->value<int64_t>(); v.has_value() && *v >= 0)
                priority = static_cast<crd::u32>(*v);
        }

        // predicates — array of inline tables; v1o3 ships the parser even
        // though the default profile uses no predicates.
        crd::containers::Array<crd::profile::PredicateRecord> predicates(ctx.allocator);
        if (const toml::node* preds = profile->get("predicate");
            preds != nullptr && preds->is_array_of_tables())
        {
            for (const auto& pn : *preds->as_array())
            {
                const toml::table* pt = pn.as_table();
                if (pt == nullptr) continue;
                crd::profile::PredicateRecord rec{};
                if (auto fnode = pt->get("field"); fnode != nullptr)
                {
                    if (auto s = fnode->value<std::string>(); s.has_value())
                    {
                        if (!parse_predicate_field(*s, rec.field))
                        {
                            std::fprintf(stderr,
                                         "profile cook: unknown predicate field='%s'\n",
                                         s->c_str());
                            return result;
                        }
                    }
                }
                if (auto onode = pt->get("op"); onode != nullptr)
                {
                    if (auto s = onode->value<std::string>(); s.has_value())
                    {
                        if (!parse_predicate_op(*s, rec.op))
                        {
                            std::fprintf(stderr,
                                         "profile cook: unknown predicate op='%s'\n",
                                         s->c_str());
                            return result;
                        }
                    }
                }
                if (auto vnode = pt->get("value_int"); vnode != nullptr)
                {
                    if (auto v = vnode->value<int64_t>(); v.has_value())
                        rec.value = static_cast<crd::u32>(*v);
                }
                predicates.push_back(rec);
            }
        }

        // bundle — array of preset path strings (relative to source dir).
        crd::containers::Array<crd::resources::ResourceId> bundle(ctx.allocator);
        if (const toml::node* bn = profile->get("bundle"); bn != nullptr && bn->is_array())
        {
            for (const auto& en : *bn->as_array())
            {
                auto s = en.value<std::string>();
                if (!s.has_value())
                {
                    std::fprintf(stderr, "profile cook: bundle entries must be strings\n");
                    return result;
                }
                const crd::resources::ResourceId id =
                    resolve_meta_uuid(source_dir, *s, ctx.allocator);
                if (id.is_null())
                {
                    std::fprintf(stderr,
                                 "profile cook: failed to resolve bundle entry '%s' "
                                 "(missing/malformed .meta sidecar)\n",
                                 s->c_str());
                    return result;
                }
                bundle.push_back(id);
            }
        }

        b.add_rule(priority,
                   crd::containers::ConstSpan<crd::profile::PredicateRecord>{
                       predicates.data(), predicates.size()},
                   crd::containers::ConstSpan<crd::resources::ResourceId>{
                       bundle.data(), bundle.size()});
    }

    auto bytes = b.build();
    if (bytes.empty())
        return result;

    result.type_fourcc     = crd::profile::kFourCC_PROF;
    result.cooked_bytes    = std::move(bytes);
    result.handler_version = kProfileHandlerVersion;
    result.ok              = true;
    return result;
}

} // anonymous namespace

void register_profile_handler()
{
    register_cook_handler(".profile.toml", profile_handler);
}

} // namespace crd::cooker
