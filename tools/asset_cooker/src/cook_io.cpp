// cook_io.cpp — GEO-6 (D-007 row 71): the declared-input seam. See cook_io.hpp for the contract.

#include <crd/cooker/cook_io.hpp>

#include <crd/platform/filesystem.hpp>

#include <cstring>

namespace fs = crd::platform::fs;

namespace crd::cooker
{

namespace
{

constexpr crd::u64 kFnvOffset64 = 14695981039346656037ULL;
constexpr crd::u64 kFnvPrime64  = 1099511628211ULL;

// a relative auxiliary path's FORM is acceptable: no absolute forms, no schemes / drive letters. ".." segments
// pass here — the ESCAPE check happens on the lexically normalized resolved path against the root boundary.
[[nodiscard]] bool aux_path_form_ok(crd::containers::StringView p) noexcept
{
    if (p.empty()) { return false; }
    if (p.front() == '/' || p.front() == '\\') { return false; }
    for (crd::usize i = 0; i < p.size(); ++i)
    {
        if (p[i] == ':') { return false; } // drive letters / uri schemes
    }
    return true;
}

// lexically normalize a '/'-separated path ("a/b/../c" → "a/c"; "." dropped). False when ".." pops past the
// start — the path escapes whatever it was resolved against.
[[nodiscard]] bool lexically_normalize(crd::containers::StringView in, crd::containers::String& out,
                                       crd::memory::IAllocator* alloc)
{
    crd::containers::Array<crd::containers::StringView> parts(alloc);
    crd::usize                                          seg_start = 0;
    for (crd::usize i = 0; i <= in.size(); ++i)
    {
        if (i == in.size() || in[i] == '/' || in[i] == '\\')
        {
            const crd::containers::StringView seg = in.substr(seg_start, i - seg_start);
            seg_start                             = i + 1U;
            if (seg.empty() || seg == ".") { continue; }
            if (seg == "..")
            {
                if (parts.size() == 0U) { return false; } // escapes the base
                parts.pop_back();
                continue;
            }
            parts.push_back(seg);
        }
    }
    out.clear();
    for (crd::usize i = 0; i < parts.size(); ++i)
    {
        if (i > 0U) { out.push_back('/'); }
        out.append(parts[i].data(), parts[i].size());
    }
    return true;
}

} // namespace

crd::u64 cook_hash64(crd::containers::ConstSpan<crd::u8> bytes) noexcept
{
    crd::u64 hash = kFnvOffset64;
    for (crd::usize i = 0; i < bytes.size(); ++i)
    {
        hash ^= static_cast<crd::u64>(bytes[i]);
        hash *= kFnvPrime64;
    }
    return hash;
}

CookIO::CookIO(crd::containers::StringView source_path, crd::containers::StringView meta_path,
               crd::memory::IAllocator* alloc, crd::containers::StringView root)
    : m_source_path(source_path.data(), source_path.size(), alloc)
    , m_meta_path(meta_path.data(), meta_path.size(), alloc)
    , m_root(alloc)
    , m_inputs(alloc)
    , m_alloc(alloc)
{
    if (!root.empty()) { m_root.append(root.data(), root.size()); }
    else
    {
        // no root given: the boundary is the source's own directory
        crd::usize dir_end = 0;
        for (crd::usize i = 0; i < source_path.size(); ++i)
        {
            if (source_path[i] == '/' || source_path[i] == '\\') { dir_end = i; }
        }
        m_root.append(source_path.data(), dir_end);
    }
}

void CookIO::record(crd::containers::StringView path, crd::containers::ConstSpan<crd::u8> bytes, bool existed)
{
    // one edge per path — a handler re-reading the same input records once (the first read's content governs;
    // inputs are immutable for the duration of a cook)
    for (crd::usize i = 0; i < m_inputs.size(); ++i)
    {
        const crd::containers::String& seen = m_inputs[i].path;
        if (crd::containers::StringView(seen.data(), seen.size()) == path) { return; }
    }
    CookInput edge(m_alloc);
    edge.path         = crd::containers::String(path.data(), path.size(), m_alloc);
    edge.content_hash = existed ? cook_hash64(bytes) : 0U;
    edge.existed      = existed;
    m_inputs.push_back(static_cast<CookInput&&>(edge));
}

bool CookIO::read_source(crd::containers::Array<crd::u8>& out)
{
    out.clear();
    if (!m_source_read)
    {
        m_source_read = true;
        m_source_ok   = fs::read_file_binary(
            fs::Path(crd::containers::StringView(m_source_path.data(), m_source_path.size())), m_source_cache);
        record(crd::containers::StringView(m_source_path.data(), m_source_path.size()),
               crd::containers::ConstSpan<crd::u8>(m_source_cache.data(), m_source_cache.size()), m_source_ok);
    }
    if (m_source_ok)
    {
        out.reserve(m_source_cache.size());
        for (crd::usize i = 0; i < m_source_cache.size(); ++i) { out.push_back(m_source_cache[i]); }
    }
    return m_source_ok;
}

bool CookIO::read_meta(crd::containers::String& out)
{
    out.clear();
    if (m_meta_path.size() == 0U) { return false; }
    if (!m_meta_read)
    {
        m_meta_read = true;
        m_meta_ok   = fs::read_file_text(
            fs::Path(crd::containers::StringView(m_meta_path.data(), m_meta_path.size())), m_meta_cache);
        record(crd::containers::StringView(m_meta_path.data(), m_meta_path.size()),
               crd::containers::ConstSpan<crd::u8>(reinterpret_cast<const crd::u8*>(m_meta_cache.data()),
                                                   m_meta_cache.size()),
               m_meta_ok);
    }
    if (m_meta_ok) { out.append(m_meta_cache.data(), m_meta_cache.size()); }
    return m_meta_ok;
}

bool CookIO::read_input(crd::containers::StringView rel_path, crd::containers::Array<crd::u8>& out)
{
    out.clear();
    if (!aux_path_form_ok(rel_path)) { return false; } // refused paths are NOT dependencies — they never resolve

    // resolve against the source's directory, then normalize and hold the ROOT boundary: express the resolved
    // path as root + "/" + tail, normalize the tail — a ".." run that pops past the tail's start escapes root
    crd::usize dir_end = 0;
    for (crd::usize i = 0; i < m_source_path.size(); ++i)
    {
        if (m_source_path.data()[i] == '/' || m_source_path.data()[i] == '\\') { dir_end = i + 1U; }
    }
    crd::containers::String tail(m_alloc); // the source-dir path RELATIVE to root, then the aux path
    {
        const crd::containers::StringView src_dir(m_source_path.data(), dir_end);
        crd::containers::StringView       rel_dir = src_dir;
        if (m_root.size() > 0U && src_dir.starts_with(crd::containers::StringView(m_root.data(), m_root.size())))
        {
            rel_dir = src_dir.substr(m_root.size());
            if (!rel_dir.empty() && rel_dir.front() == '/') { rel_dir = rel_dir.substr(1U); }
        }
        tail.append(rel_dir.data(), rel_dir.size());
        tail.append(rel_path.data(), rel_path.size());
    }
    crd::containers::String norm_tail(m_alloc);
    if (!lexically_normalize(crd::containers::StringView(tail.data(), tail.size()), norm_tail, m_alloc))
    {
        return false; // the reference escapes the source tree — refused, never a dependency
    }

    crd::containers::String resolved(m_alloc);
    if (m_root.size() > 0U)
    {
        resolved.append(m_root.data(), m_root.size());
        resolved.push_back('/');
    }
    resolved.append(norm_tail.data(), norm_tail.size());

    const bool ok =
        fs::read_file_binary(fs::Path(crd::containers::StringView(resolved.data(), resolved.size())), out);
    record(crd::containers::StringView(resolved.data(), resolved.size()),
           crd::containers::ConstSpan<crd::u8>(out.data(), out.size()), ok);
    return ok;
}

bool CookIO::stable_id(crd::containers::StringView suffix, crd::resources::ResourceId& out_id)
{
    out_id = crd::resources::ResourceId{};

    crd::containers::String sidecar(m_alloc);
    sidecar.append(m_source_path.data(), m_source_path.size());
    sidecar.append(suffix.data(), suffix.size());
    sidecar.append(".meta");
    const fs::Path sidecar_path(crd::containers::StringView(sidecar.data(), sidecar.size()));

    crd::containers::String text(m_alloc);
    if (fs::read_file_text(sidecar_path, text))
    {
        const std::string_view sv(text.data(), text.size());
        const std::string_view key = "uuid = \"";
        auto                   pos = sv.find(key);
        if (pos != std::string_view::npos)
        {
            pos += key.size();
            const auto end = sv.find('"', pos);
            if (end != std::string_view::npos)
            {
                out_id = crd::resources::ResourceId::parse(sv.substr(pos, end - pos));
            }
        }
    }

    if (out_id.is_null()) // absent or malformed: mint once, persist forever
    {
        out_id             = crd::resources::ResourceId::mint_random();
        const auto id_str  = out_id.to_string(m_alloc);
        text.clear();
        text.append("[id]\n");
        text.append("uuid = \"");
        text.append(id_str.c_str());
        text.append("\"\n");
        if (!fs::write_file_text(sidecar_path, crd::containers::StringView(text.data(), text.size())))
        {
            out_id = crd::resources::ResourceId{};
            return false;
        }
    }

    // the sidecar's (post-mint) content is an input: a hand-edited id must recook the job
    record(crd::containers::StringView(sidecar.data(), sidecar.size()),
           crd::containers::ConstSpan<crd::u8>(reinterpret_cast<const crd::u8*>(text.data()), text.size()), true);
    return true;
}

} // namespace crd::cooker
