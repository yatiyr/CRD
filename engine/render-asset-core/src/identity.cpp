#include <crd/renderasset/identity.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/hash.hpp>

#include <utility> // std::move

namespace crd::renderasset
{
namespace
{
// ASCII lowercase, byte-wise (schemes/segments are ASCII by contract).
char to_lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

// Case-insensitive equality of a token against a lowercase literal.
bool ieq(StringView tok, StringView lower_literal) noexcept
{
    if (tok.size() != lower_literal.size())
    {
        return false;
    }
    for (usize i = 0; i < tok.size(); ++i)
    {
        if (to_lower(tok[i]) != lower_literal[i])
        {
            return false;
        }
    }
    return true;
}
} // namespace

StringView scheme_prefix(AssetScheme scheme) noexcept
{
    switch (scheme)
    {
    case AssetScheme::Engine:
        return "engine://";
    case AssetScheme::App:
        return "app://";
    case AssetScheme::Plugin:
        return "plugin://";
    case AssetScheme::Test:
        return "test://";
    }
    return "engine://";
}

StringView scheme_token(AssetScheme scheme) noexcept
{
    switch (scheme)
    {
    case AssetScheme::Engine:
        return "engine";
    case AssetScheme::App:
        return "app";
    case AssetScheme::Plugin:
        return "plugin";
    case AssetScheme::Test:
        return "test";
    }
    return "engine";
}

bool parse_scheme(StringView token, AssetScheme& out) noexcept
{
    if (ieq(token, "engine") || ieq(token, "crd")) // crd:// folds to engine:// (deprecated alias)
    {
        out = AssetScheme::Engine;
        return true;
    }
    if (ieq(token, "app"))
    {
        out = AssetScheme::App;
        return true;
    }
    if (ieq(token, "plugin"))
    {
        out = AssetScheme::Plugin;
        return true;
    }
    if (ieq(token, "test"))
    {
        out = AssetScheme::Test;
        return true;
    }
    return false;
}

StringView asset_type_name(AssetType type) noexcept
{
    switch (type)
    {
    case AssetType::Unknown:
        return "unknown";
    case AssetType::Shader:
        return "shader";
    case AssetType::Program:
        return "program";
    case AssetType::Material:
        return "material";
    case AssetType::Technique:
        return "technique";
    case AssetType::RenderPhase:
        return "render-phase";
    case AssetType::FrameGraph:
        return "frame-graph";
    case AssetType::Vertex:
        return "vertex";
    case AssetType::Lod:
        return "lod";
    case AssetType::Light:
        return "light";
    case AssetType::Mesh:
        return "mesh";
    case AssetType::Texture:
        return "texture";
    case AssetType::Sampler:
        return "sampler";
    }
    return "unknown";
}

AssetType infer_type(StringView first_segment) noexcept
{
    if (first_segment == "shader")
    {
        return AssetType::Shader;
    }
    if (first_segment == "program")
    {
        return AssetType::Program;
    }
    if (first_segment == "material")
    {
        return AssetType::Material;
    }
    if (first_segment == "technique")
    {
        return AssetType::Technique;
    }
    if (first_segment == "phase")
    {
        return AssetType::RenderPhase;
    }
    if (first_segment == "frame")
    {
        return AssetType::FrameGraph;
    }
    if (first_segment == "vertex")
    {
        return AssetType::Vertex;
    }
    if (first_segment == "lod")
    {
        return AssetType::Lod;
    }
    if (first_segment == "light")
    {
        return AssetType::Light;
    }
    if (first_segment == "mesh")
    {
        return AssetType::Mesh;
    }
    if (first_segment == "texture")
    {
        return AssetType::Texture;
    }
    if (first_segment == "sampler")
    {
        return AssetType::Sampler;
    }
    return AssetType::Unknown;
}

StringView asset_extension(StringView folder) noexcept
{
    if (folder == "frame") { return ".frame.toml"; }
    if (folder == "vertex") { return ".crdv"; }
    if (folder == "material") { return ".crdm"; }
    if (folder == "post") { return ".crdp"; }
    if (folder == "lighting") { return ".crdl"; }
    if (folder == "technique") { return ".crdt"; }
    if (folder == "lod") { return ".crdlod"; }
    return {};
}

bool on_disk_relative(const AssetRef& ref, String& out)
{
    if (!ref.valid())
    {
        return false;
    }
    const StringView path = ref.path(); // normalized, no scheme — e.g. "frame/forward_csm"
    usize            slash = path.size();
    for (usize i = 0; i < path.size(); ++i)
    {
        if (path[i] == '/')
        {
            slash = i;
            break;
        }
    }
    const StringView ext = asset_extension(path.substr(0, slash)); // the folder is the first segment
    if (ext.empty())
    {
        return false; // an unknown folder has no on-disk shape — leave `out` untouched
    }
    out.append(path.data(), path.size());
    out.append(ext.data(), ext.size());
    return true;
}

AssetId asset_id_of(StringView canonical) noexcept { return AssetId{crd::containers::hash_string(canonical)}; }

AssetRef AssetRef::parse(StringView raw, DiagnosticList& diags, memory::IAllocator* alloc, AssetType type_hint)
{
    AssetRef ref(alloc);

    const usize sep = raw.find("://");
    if (sep == StringView::npos)
    {
        diags.error(DiagCode::MalformedPath, "reference has no scheme separator '://'", raw);
        return ref;
    }
    const StringView scheme_tok = raw.substr(0, sep);
    if (scheme_tok.empty())
    {
        diags.error(DiagCode::MalformedPath, "reference has an empty scheme", raw);
        return ref;
    }
    AssetScheme scheme = AssetScheme::Engine;
    if (!parse_scheme(scheme_tok, scheme))
    {
        diags.emit(Severity::Error, DiagCode::UnknownScheme, "unrecognized asset scheme", raw, scheme_tok,
                   "engine|app|plugin|test|crd", scheme_tok);
        return ref;
    }

    const StringView rest = raw.substr(sep + 3);

    // Split `rest` on '/' or '\', normalizing '.' (drop) and '..' (pop). Segments
    // are views into `raw` and are only read while building the canonical string.
    crd::containers::Array<StringView> segments(alloc);
    usize start = 0;
    const usize n = rest.size();
    for (usize i = 0; i <= n; ++i)
    {
        const bool at_end = (i == n);
        const char c = at_end ? '/' : rest[i];
        if (c != '/' && c != '\\')
        {
            continue;
        }
        const StringView seg = rest.substr(start, i - start);
        start = i + 1;
        if (seg.empty() || seg == ".") // collapses "//", leading/trailing '/', and "."
        {
            continue;
        }
        if (seg == "..")
        {
            if (segments.size() == 0)
            {
                diags.error(DiagCode::PathEscapesRoot, "reference escapes its mount root with '..'", raw);
                return ref;
            }
            segments.pop_back();
            continue;
        }
        segments.push_back(seg);
    }

    if (segments.size() == 0)
    {
        diags.error(DiagCode::EmptyPath, "reference has an empty path after the scheme", raw);
        return ref;
    }

    const StringView prefix = scheme_prefix(scheme);
    String canonical(alloc);
    canonical.append(prefix.data(), prefix.size());
    for (usize i = 0; i < segments.size(); ++i)
    {
        if (i > 0)
        {
            canonical.push_back('/');
        }
        canonical.append(segments[i].data(), segments[i].size());
    }

    ref.m_scheme = scheme;
    ref.m_path_offset = prefix.size();
    ref.m_type = (type_hint != AssetType::Unknown) ? type_hint : infer_type(segments[0]);
    ref.m_id = asset_id_of(StringView{canonical.data(), canonical.size()});
    ref.m_canonical = std::move(canonical);
    ref.m_valid = true;
    return ref;
}
} // namespace crd::renderasset
